/*
snd_vis.c -- FTESurf: real-time analysis of the software mix for QC music visualisers
(snd_getvis, snd_visimage, snd_visinfo) and a mixer low-pass (snd_fx_lowpass).

The mixer side (SNDVIS_Tap, SNDVIS_Lowpass, SNDVIS_NoteMixTime) runs on whichever thread
mixes -- the dsoundmixer/wasapimixer thread or the main thread -- always inside mixermutex,
and never reads a cvar_t: SNDVIS_Latch copies them under the same lock. The rest is
main-thread only. The analysis core (Vis_*) has no engine dependency: #define
SNDVIS_STANDALONE and #include this file to drive it from a test program.
*/

#ifdef SNDVIS_STANDALONE
#include <math.h>
#include <string.h>
typedef unsigned char qbyte;
#else
#include "quakedef.h"
#include "pr_common.h"
#include "snd_vis.h"
#endif

#define VIS_PI			3.14159265358979323846
#define VIS_N			1024		//analysis window and real-FFT size
#define VIS_M			(VIS_N/2)	//complex FFT that computes it, fed pairs of real samples
#define VIS_MBITS		9
#define VIS_SPECBANDS	512			//internal log bands; snd_visimage's maximum
#define VIS_OUTS		16

#define VIS_SILENCE_RMS	1e-4f		//quieter than this for VIS_GATE_TIME: outputs fade to 0
#define VIS_GATE_TIME	0.5f
#define VIS_GATE_FADE	0.15f
#define VIS_BINFLOOR	2e-6f		//per-bin magnitude floor under every band division: ~3x what 16-bit quantisation noise leaves in a bin (0.6e-6)
#define VIS_RELFLOOR	0.03f		//a band's long average counts as >= 3% of vol's: a band holding only leakage reads ~0, not noise/noise
#define VIS_MAXRATIO	50.f
#define VIS_PULSE_TAU	0.15f
#define VIS_SPEC_LO		30.0
#define VIS_SPEC_HI		16000.0
#define VIS_SPEC_DB		60.f		//dB shown below the running peak
#define VIS_SPECREF_TAU	4.f
#define VIS_SPECREF_MIN	1e-4f		//~ -80 dBFS
#define VIS_SPEC_ATTACK	0.02f
#define VIS_SPEC_RELEASE 0.15f
#define VIS_PEAK_FALL	1.f			//seconds for peak-hold to fall full scale

enum {VB_BASS, VB_MID, VB_TREB, VB_VOL, VB_COUNT};

typedef struct
{
	float rate;
	int bandlo[3], bandhi[3];				//bins [lo,hi) of bass 20-320, mid 320-2800, treb 2800-11025 Hz
	float bandfloor[VB_COUNT];				//VIS_BINFLOOR * bins
	float window[VIS_N];					//periodic Hann
	float fftc[VIS_M/2], ffts[VIS_M/2];		//e^(-2pi i k/M)
	float splitc[VIS_M+1], splits[VIS_M+1];	//e^(-2pi i k/N), for unpacking the real spectrum
	unsigned short bitrev[VIS_M];
	unsigned short spk0[VIS_SPECBANDS], spk1[VIS_SPECBANDS];	//log band: bins [k0,k1) power-averaged,
	float spkf[VIS_SPECBANDS], sptilt[VIS_SPECBANDS];			//or (k1==k0) interpolated at bin spkf; <0 = above Nyquist

	int seeded, wasgated;
	float imm[VB_COUNT], avg[VB_COUNT], lng[VB_COUNT];
	float prevratio[2];						//bass, treb
	double lastonset[2];
	float pulse[2];
	unsigned int onsets[2];
	float silent, sincestart, gate;
	double clock;							//seconds of audio analysed; survives Vis_Init
	float out[VIS_OUTS];

	int wantspec, hadspec;					//the spectrum below costs about as much as the rest: only while an image is wanted
	float specref;
	float spraw[VIS_SPECBANDS], spsmooth[VIS_SPECBANDS], sppeak[VIS_SPECBANDS];
	float wave[VIS_N];
	float re[VIS_M], im[VIS_M], mag[VIS_M+1];	//mag: |X[k]|*4/N, so a bin-centred sine of amplitude A reads A
} viscore_t;

//first bin whose centre frequency is >= f
static int Vis_Bin(double f, float rate)
{
	double k = ceil(f * VIS_N / rate - 1e-9);
	if (k < 0)
		return 0;
	if (k > VIS_M+1)
		return VIS_M+1;
	return (int)k;
}

static void Vis_Init(viscore_t *v, float rate)
{
	static const double edges[4] = {20, 320, 2800, 11025};
	double clock = v->clock;
	int i, j, r;

	memset(v, 0, sizeof(*v));
	v->clock = clock;
	v->rate = rate;
	v->lastonset[0] = v->lastonset[1] = -1e9;

	for (i = 0; i < VIS_N; i++)
		v->window[i] = 0.5 - 0.5*cos(2*VIS_PI*i/VIS_N);
	for (i = 0; i < VIS_M/2; i++)
	{
		v->fftc[i] = cos(2*VIS_PI*i/VIS_M);
		v->ffts[i] = -sin(2*VIS_PI*i/VIS_M);
	}
	for (i = 0; i <= VIS_M; i++)
	{
		v->splitc[i] = cos(2*VIS_PI*i/VIS_N);
		v->splits[i] = -sin(2*VIS_PI*i/VIS_N);
	}
	for (i = 0; i < VIS_M; i++)
	{
		for (r = 0, j = 0; j < VIS_MBITS; j++)
			if (i & (1<<j))
				r |= 1<<(VIS_MBITS-1-j);
		v->bitrev[i] = r;
	}
	for (i = 0; i < 3; i++)
	{
		v->bandlo[i] = Vis_Bin(edges[i], rate);
		v->bandhi[i] = Vis_Bin(edges[i+1], rate);
		if (v->bandlo[i] < 1)
			v->bandlo[i] = 1;	//never DC
		v->bandfloor[i] = VIS_BINFLOOR * (v->bandhi[i] > v->bandlo[i] ? v->bandhi[i] - v->bandlo[i] : 1);
		v->bandfloor[VB_VOL] += v->bandfloor[i];
	}
	for (i = 0; i < VIS_SPECBANDS; i++)
	{
		double f0 = VIS_SPEC_LO * pow(VIS_SPEC_HI/VIS_SPEC_LO, (double)i/VIS_SPECBANDS);
		double f1 = VIS_SPEC_LO * pow(VIS_SPEC_HI/VIS_SPEC_LO, (double)(i+1)/VIS_SPECBANDS);
		double fc = sqrt(f0*f1);
		int k0 = Vis_Bin(f0, rate), k1 = Vis_Bin(f1, rate);
		v->sptilt[i] = sqrt(fc/1000);	//+3 dB/octave: pink-ish music draws level
		v->spk0[i] = v->spk1[i] = k0;
		v->spkf[i] = -1;
		if (f0 >= rate*0.5)
			v->spk0[i] = v->spk1[i] = 0;
		else if (k1 > k0)
			v->spk1[i] = k1;
		else
			v->spkf[i] = fc * VIS_N / rate;
	}
}

//in-place radix-2 DIT FFT of VIS_M complex points
static void Vis_FFT(viscore_t *v)
{
	float *re = v->re, *im = v->im, t;
	int i, j, k, len, half, step, p, q;

	for (i = 0; i < VIS_M; i++)
	{
		j = v->bitrev[i];
		if (j > i)
		{
			t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (len = 2; len <= VIS_M; len <<= 1)
	{
		half = len>>1;
		step = VIS_M/len;
		for (i = 0; i < VIS_M; i += len)
		{
			for (j = 0, k = 0; j < half; j++, k += step)
			{
				float wr = v->fftc[k], wi = v->ffts[k], tr, ti;
				p = i+j;
				q = p+half;
				tr = wr*re[q] - wi*im[q];
				ti = wr*im[q] + wi*re[q];
				re[q] = re[p] - tr;
				im[q] = im[p] - ti;
				re[p] += tr;
				im[p] += ti;
			}
		}
	}
}

//one analysis of the VIS_N newest-audible samples (-1..1); dt = seconds of audio since the previous one
static void Vis_Process(viscore_t *v, const float *in, float dt)
{
	static const float thresh[2] = {1.35f, 1.5f}, refractory[2] = {0.18f, 0.12f};
	double sumsq = 0;
	float rms, ratio[VB_COUNT], ratioavg[VB_COUNT], fl, mlong, pulsedecay, att, rel, peakfall, specmax, logref;
	int i, b, k, silent, gated;

	if (!(dt >= 0))
		dt = 0;
	v->clock += dt;

	for (i = 0; i < VIS_M; i++)
	{
		float a = in[2*i], c = in[2*i+1];
		sumsq += a*a + c*c;
		v->re[i] = a * v->window[2*i];
		v->im[i] = c * v->window[2*i+1];
	}
	memcpy(v->wave, in, sizeof(v->wave));
	rms = sqrt(sumsq / VIS_N);
	Vis_FFT(v);
	for (k = 0; k <= VIS_M; k++)
	{	//X[k] = E[k] + W^k O[k], E/O being the spectra of the even/odd samples
		int p = k & (VIS_M-1), q = (VIS_M-k) & (VIS_M-1);
		float er = 0.5f*(v->re[p] + v->re[q]), ei = 0.5f*(v->im[p] - v->im[q]);
		float odr = 0.5f*(v->im[p] + v->im[q]), odi = -0.5f*(v->re[p] - v->re[q]);
		float xr = er + v->splitc[k]*odr - v->splits[k]*odi;
		float xi = ei + v->splitc[k]*odi + v->splits[k]*odr;
		v->mag[k] = sqrtf(xr*xr + xi*xi) * (4.0f/VIS_N);
	}

	for (b = 0; b < 3; b++)
	{
		float s = 0;
		for (k = v->bandlo[b]; k < v->bandhi[b]; k++)
			s += v->mag[k];
		v->imm[b] = s;
	}
	v->imm[VB_VOL] = v->imm[VB_BASS] + v->imm[VB_MID] + v->imm[VB_TREB];

	silent = !(rms >= VIS_SILENCE_RMS);
	v->silent = silent ? v->silent + dt : 0;
	gated = v->silent > VIS_GATE_TIME;
	if (!silent && (!v->seeded || v->wasgated))
	{	//audio (re)started: learn its level quickly for the next second
		if (!v->seeded)
			for (b = 0; b < VB_COUNT; b++)
			{	//at half, so a start after silence reads 2 and is an onset: a short hat puts
				//nearly all its energy in this first window, and seeding at 1 hid it
				v->lng[b] = 0.5f*v->imm[b];
				v->avg[b] = v->imm[b];
			}
		v->seeded = 1;
		v->sincestart = 0;
	}
	v->wasgated = gated;

	//MilkDrop's attack/release (0.2 / 0.5 per frame at 30 fps), rescaled to dt
	mlong = exp(-dt / (v->sincestart < 1 ? 0.5f : 4.f));
	for (b = 0; b < VB_COUNT; b++)
	{
		float mix = pow(v->imm[b] > v->avg[b] ? 0.2 : 0.5, 30*dt);
		v->avg[b] = v->avg[b]*mix + v->imm[b]*(1-mix);
		if (v->seeded && !gated)	//frozen through silence, so the ratio resumes where it was
		{
			v->lng[b] = v->lng[b]*mlong + v->imm[b]*(1-mlong);
			if (v->sincestart < 1 && v->lng[b] < 0.5f*v->imm[b])
				v->lng[b] = 0.5f*v->imm[b];	//a start seeded from a partial window reads <= 2, not 50
		}
	}
	if (!gated)
		v->sincestart += dt;

	for (b = 0; b < VB_COUNT; b++)
	{
		fl = (b == VB_VOL) ? 0 : VIS_RELFLOOR * v->lng[VB_VOL];
		if (fl < v->bandfloor[b])
			fl = v->bandfloor[b];
		if (fl < v->lng[b])
			fl = v->lng[b];
		ratio[b] = v->seeded ? v->imm[b] / fl : 0;
		ratioavg[b] = v->seeded ? v->avg[b] / fl : 0;
	}

	if (gated)
		v->gate *= exp(-dt / VIS_GATE_FADE);
	else
		v->gate = v->seeded ? 1 : 0;

	pulsedecay = exp(-dt / VIS_PULSE_TAU);
	for (i = 0; i < 2; i++)
	{
		float r = ratio[i ? VB_TREB : VB_BASS];
		v->pulse[i] *= pulsedecay;
		//an onset's time is only known to +-dt/2: without that slack, 8/s hats analysed at 60/s
		//measure 7 steps (0.117 s) apart every other time and half of them fall in the 0.12 s refractory
		if (!silent && v->seeded && r >= thresh[i] && v->prevratio[i] < thresh[i] && r > v->prevratio[i] &&
			v->clock - v->lastonset[i] >= refractory[i] - 0.5f*dt)
		{
			v->pulse[i] = 1;
			v->lastonset[i] = v->clock;
			v->onsets[i]++;
		}
		v->prevratio[i] = r;
	}

	v->out[0] = ratio[VB_BASS] * v->gate;
	v->out[1] = ratio[VB_MID] * v->gate;
	v->out[2] = ratio[VB_TREB] * v->gate;
	v->out[3] = ratioavg[VB_BASS] * v->gate;
	v->out[4] = ratioavg[VB_MID] * v->gate;
	v->out[5] = ratioavg[VB_TREB] * v->gate;
	v->out[6] = ratio[VB_VOL] * v->gate;
	v->out[7] = ratioavg[VB_VOL] * v->gate;
	v->out[8] = v->pulse[0] > v->pulse[1] ? v->pulse[0] : v->pulse[1];
	v->out[9] = v->pulse[0];
	v->out[10] = v->pulse[1];
	v->out[11] = rms;
	v->out[12] = v->clock;
	v->out[13] = 1;
	v->out[14] = v->out[15] = 0;
	for (i = 0; i <= 11; i++)
	{	//nothing non-finite may reach QC; !(x >= 0) is also true for NaN
		if (!(v->out[i] >= 0))
			v->out[i] = 0;
		else if (v->out[i] > VIS_MAXRATIO)
			v->out[i] = VIS_MAXRATIO;
	}

	//snd_visimage's spectrum: linear levels first, then dB against a slowly decaying running peak
	if (!v->wantspec)
	{
		v->hadspec = 0;
		return;
	}
	if (!v->hadspec)
	{	//resuming: stale levels and a stale reference would show a dim or frozen image for seconds
		v->hadspec = 1;
		v->specref = 0;
		memset(v->spsmooth, 0, sizeof(v->spsmooth));
		memset(v->sppeak, 0, sizeof(v->sppeak));
	}
	specmax = 0;
	for (i = 0; i < VIS_SPECBANDS; i++)
	{
		int k0 = v->spk0[i], k1 = v->spk1[i];
		float l = 0;
		if (k1 > k0)
		{
			for (k = k0; k < k1; k++)
				l += v->mag[k]*v->mag[k];
			l = sqrtf(l / (k1-k0));
		}
		else if (v->spkf[i] >= 0)
		{
			float f = v->spkf[i];
			k = (int)f;
			if (k > VIS_M-1)
				k = VIS_M-1;
			f -= k;
			if (f > 1)
				f = 1;
			l = v->mag[k]*(1-f) + v->mag[k+1]*f;
		}
		l *= v->sptilt[i];
		v->spraw[i] = l;
		if (l > specmax)
			specmax = l;
	}
	if (!gated)
	{
		v->specref *= exp(-dt / VIS_SPECREF_TAU);
		if (v->specref < specmax)
			v->specref = specmax;
	}
	if (!(v->specref >= VIS_SPECREF_MIN))
		v->specref = VIS_SPECREF_MIN;
	logref = log10(v->specref);
	att = 1 - exp(-dt / VIS_SPEC_ATTACK);
	rel = 1 - exp(-dt / VIS_SPEC_RELEASE);
	peakfall = dt / VIS_PEAK_FALL;
	for (i = 0; i < VIS_SPECBANDS; i++)
	{
		float l = v->spraw[i], s;
		if (l > v->specref * 0.001f)	//below that is 0 after mapping anyway: skip the log
		{
			l = 1 + (20/VIS_SPEC_DB) * (log10f(l) - logref);
			if (l > 1)
				l = 1;
			else if (!(l > 0))
				l = 0;
		}
		else
			l = 0;
		v->spraw[i] = l;
		s = v->spsmooth[i];
		s += (l - s) * (l > s ? att : rel);
		v->spsmooth[i] = s;
		v->sppeak[i] -= peakfall;
		if (v->sppeak[i] < s)
			v->sppeak[i] = s;
	}
}

static qbyte Vis_Byte(float f)
{
	if (!(f > 0))
		return 0;
	if (f >= 1)
		return 255;
	return (qbyte)(f*255 + 0.5f);
}

//bands x 2 RGBA8, row 0 first in memory: spectrum (R smoothed, G peak-hold, B raw), row 1: waveform
static void Vis_FillImage(const viscore_t *v, int bands, qbyte *rgba)
{
	int i, j, i0, i1;
	for (j = 0; j < bands; j++)
	{
		float r = 0, g = 0, bl = 0;
		i0 = j*VIS_SPECBANDS/bands;
		i1 = (j+1)*VIS_SPECBANDS/bands;
		for (i = i0; i < i1; i++)
		{	//max over the internal bands this texel covers
			if (r < v->spsmooth[i])
				r = v->spsmooth[i];
			if (g < v->sppeak[i])
				g = v->sppeak[i];
			if (bl < v->spraw[i])
				bl = v->spraw[i];
		}
		rgba[j*4+0] = Vis_Byte(r);
		rgba[j*4+1] = Vis_Byte(g);
		rgba[j*4+2] = Vis_Byte(bl);
		rgba[j*4+3] = 255;
	}
	for (j = 0; j < bands; j++)
	{	//the whole window decimated, oldest at u=0, the newest-audible sample in the last texel
		qbyte c = Vis_Byte(v->wave[VIS_N-1 - ((bands-1-j)*VIS_N)/bands]*0.5f + 0.5f);
		qbyte *px = rgba + (bands+j)*4;
		px[0] = px[1] = px[2] = c;
		px[3] = 255;
	}
}

static void Vis_BlankImage(int bands, qbyte *rgba)
{
	int j;
	for (j = 0; j < bands; j++)
	{
		rgba[j*4+0] = rgba[j*4+1] = rgba[j*4+2] = 0;
		rgba[j*4+3] = 255;
		rgba[(bands+j)*4+0] = rgba[(bands+j)*4+1] = rgba[(bands+j)*4+2] = 128;
		rgba[(bands+j)*4+3] = 255;
	}
}

/* ---- low-pass core: RBJ biquad on the int paint buffer, cutoff gliding in log-frequency ---- */
#define LP_MAXCH	8
#define LP_BLOCK	32					//frames per coefficient update while gliding
#define LP_FADE		256					//frames of wet/dry crossfade when engaging/bypassing
#define LP_SETTLED	(0.05*0.69314718)	//within 0.05 octave of the top counts as open
typedef struct
{
	int running;
	double fc, wet;
	double x1[LP_MAXCH], x2[LP_MAXCH], y1[LP_MAXCH], y2[LP_MAXCH];
	double insq, outsq;					//energy in and out while running, for snd_visinfo
	unsigned int sqframes;
} lpfilter_t;

//target 0 = off: glide open, crossfade to dry, then bypass. Returns whether it still runs.
static int LP_Process(lpfilter_t *lp, int rate, int nc, float target, float tau, int *pb, int stride, int frames)
{
	int i, j, c, n;
	double fcmax = 0.45*rate;	//at Nyquist the biquad degenerates; this high it is inaudible
	double lgoal, ltop, lf, glide, wetstep, wet0, wet1, wet, w0, cs, alpha, a0, b0, b1, a1, a2;
	int *p;

	if (nc > LP_MAXCH)
		nc = LP_MAXCH;
	if (!lp->running)
	{
		if (!target || frames <= 0)
			return 0;
		//engage wide open with the history primed to the current sample: no step, then glide down
		lp->running = 1;
		lp->fc = fcmax;
		lp->wet = 0;
		for (c = 0; c < nc; c++)
			lp->x1[c] = lp->x2[c] = lp->y1[c] = lp->y2[c] = pb[c];
	}
	ltop = log(fcmax);
	lgoal = (target && target < fcmax) ? log(target) : ltop;
	for (i = 0; i < frames; i += LP_BLOCK)
	{
		n = frames - i;
		if (n > LP_BLOCK)
			n = LP_BLOCK;
		glide = 1 - exp(-n / (rate * (double)tau));
		lf = log(lp->fc);
		lf += (lgoal - lf) * glide;
		lp->fc = exp(lf);

		wetstep = (double)n / LP_FADE;
		wet0 = lp->wet;
		if (target || lf < ltop - LP_SETTLED)
			wet1 = (wet0 + wetstep < 1) ? wet0 + wetstep : 1;
		else
			wet1 = (wet0 - wetstep > 0) ? wet0 - wetstep : 0;	//open again: crossfade to dry, then bypass

		//Q = 1/sqrt(2): Butterworth
		w0 = 2*VIS_PI * lp->fc / rate;
		cs = cos(w0);
		alpha = sin(w0) * 0.70710678118654752;
		a0 = 1 + alpha;
		b1 = (1 - cs) / a0;
		b0 = b1 * 0.5;
		a1 = -2*cs / a0;
		a2 = (1 - alpha) / a0;

		p = pb + i*stride;
		for (j = 0; j < n; j++, p += stride)
		{
			wet = wet0 + (wet1 - wet0) * (j+1) / n;
			for (c = 0; c < nc; c++)
			{
				double x = p[c];
				double y = b0*x + b1*lp->x1[c] + b0*lp->x2[c] - a1*lp->y1[c] - a2*lp->y2[c];
				lp->x2[c] = lp->x1[c];
				lp->x1[c] = x;
				lp->y2[c] = lp->y1[c];
				lp->y1[c] = y;
				y = x + (y - x) * wet;
				p[c] = (int)(y < 0 ? y - 0.5 : y + 0.5);
				lp->insq += x*x;
				lp->outsq += (double)p[c]*p[c];
			}
		}
		lp->sqframes += n;
		lp->wet = wet1;
		for (c = 0; c < nc; c++)
			if (fabs(lp->y1[c]) < 1e-12 && fabs(lp->y2[c]) < 1e-12)
				lp->y1[c] = lp->y2[c] = 0;	//no denormals after a long silence

		if (!target && wet1 <= 0)
		{	//dry from here on: the rest of this buffer is left untouched
			lp->running = 0;
			break;
		}
	}
	return lp->running;
}

#ifndef SNDVIS_STANDALONE
/* ======================== engine glue ======================== */

extern cvar_t _snd_mixahead;

static cvar_t snd_vis				= CVARD("snd_vis", "1", "Analyse the software mix for snd_getvis/snd_visimage. 0 removes the mixer tap entirely.");
static cvar_t snd_vis_rate			= CVARD("snd_vis_rate", "120", "Analyses per second of audio (10..1000, 0 = 1000). They run on a fixed audio-time grid, lazily when QC asks, catching up on any grid points that became audible since the last call, so the frame rate and frame hitches do not change the results; between grid points the builtins return the cached result.");
static cvar_t snd_vis_latency		= CVARD("snd_vis_latency", "-1", "Seconds between the newest mixed sample and the end of the analysis window, so visuals match what is audible. -1: measured from the device's play cursor (DirectSound), its queue length (WASAPI etc), else s_mixahead.");
static cvar_t snd_vis_trace			= CVARD("snd_vis_trace", "0", "Debug: 1 analyses every frame as QC would and prints each onset; 2 also prints every analysis.");
static cvar_t snd_fx_lowpass		= CVARD("snd_fx_lowpass", "0", "Low-pass (2-pole Butterworth) cutoff in Hz applied to the whole mix, eg 900 for a muffled pause menu. 0 or >= 20000 is off. snd_getvis still sees the unfiltered mix.");
static cvar_t snd_fx_lowpass_time	= CVARD("snd_fx_lowpass_time", "0.25", "Time constant in seconds of snd_fx_lowpass's glide, in both directions.");

#define VIS_RING		32768	//frames of mono history, a power of two: 683 ms at 48 kHz, so a hitch can be caught up
#define VIS_MAXCATCHUP	64		//analyses per call at most (533 ms at 120/s); older pending grid points are skipped

enum {VISLAT_NONE, VISLAT_CURSOR, VISLAT_QUEUE, VISLAT_MIXAHEAD};

static struct
{
	//latched on the main thread under mixermutex
	int tapon;
	float lptarget, lptau;
	//written by the mixer
	soundcardinfo_t *card;
	int rate, channels;
	unsigned int resets;
	quint64_t total;					//frames appended since the last reset
	int latmode, latframes;				//the play point was latframes behind lattotal at latstamp
	quint64_t lattotal;
	double latstamp;
	float ring[VIS_RING];
} vistap;

#define LP_CARDS	4
typedef struct
{
	soundcardinfo_t *card;
	int rate, channels;
	unsigned int lastpaint;
	lpfilter_t f;
} lpstate_t;
static lpstate_t lpstates[LP_CARDS];	//mixer-owned; snd_visinfo reads them under the lock
static int lp_running;					//filters not bypassed; 0 with no target = no work at all
static unsigned int lp_paints;

enum {VISNA_OK, VISNA_DISABLED, VISNA_NOCARD, VISNA_EXTERNAL, VISNA_NOSAMPLES};
static const char *vis_reasons[] = {"", "snd_vis is 0", "no sound card", "the output device mixes externally (OpenAL)", "no samples mixed yet"};
enum {VISSRC_CVAR, VISSRC_CURSOR, VISSRC_QUEUE, VISSRC_MIXAHEAD};
static const char *vis_latsrcs[] = {"snd_vis_latency", "measured: the device's play cursor, extrapolated from the last mix", "estimated: the device's queue length", "fallback: s_mixahead"};

static struct
{
	viscore_t core;
	int coreinit, rate;
	unsigned int resets;
	quint64_t lastend;					//ring frame the previous window ended at
	int haveend;
	double nextend;						//ring frame of the next grid point (fractional: 44100/120 is not whole)
	int reason;
	unsigned int serial;				//bumps per analysis; 0 = never analysed
	float latency;
	int latsrc;
	unsigned int ringfill;
	double costsum, costmax;
	unsigned int costn, maxpercall;
	int alwarned;
	double imagetime;					//realtime of the last snd_visimage: the spectrum is computed for 1 s after
	float span[VIS_RING];				//the pending windows, copied out of the ring under the lock
} vis;

#define VISIMG_MAX	8
static struct
{
	char name[MAX_QPATH];
	image_t *tid;
	int bands;
	unsigned int serial;				//vis.serial it holds; 0 = the blank image
	double lastuse;
} visimg[VISIMG_MAX];
static qbyte visimg_pixels[VIS_SPECBANDS*2*4];

/* ---- mixer side ---- */

void SNDVIS_Tap(soundcardinfo_t *sc, const int *pb, int stride, int frames)
{
	int i, c, nc;
	unsigned int w;
	float scale;

	if (!vistap.tapon || sc != sndcardinfo || frames <= 0)
		return;
	nc = sc->sn.numchannels;
	if (vistap.card != sc || vistap.rate != sc->sn.speed || vistap.channels != nc)
	{
		vistap.card = sc;
		vistap.rate = sc->sn.speed;
		vistap.channels = nc;
		vistap.total = 0;
		vistap.latmode = VISLAT_NONE;
		vistap.resets++;
	}
	scale = 1.0f / (32768.0f * nc);	//paint buffer is 16-bit scale, unclamped, before any device conversion
	w = (unsigned int)vistap.total;
	for (i = 0; i < frames; i++, pb += stride)
	{
		int s = 0;
		for (c = 0; c < nc; c++)
			s += pb[c];
		vistap.ring[(w+i) & (VIS_RING-1)] = s * scale;
	}
	vistap.total += frames;
}

void SNDVIS_NoteMixTime(soundcardinfo_t *sc, int soundtime)
{
	if (!vistap.tapon || sc != vistap.card)
		return;
	if (sc->samplequeue == 0)
	{	//memory-mapped (DirectSound): soundtime is the play cursor, paintedtime what was just mixed
		vistap.latmode = VISLAT_CURSOR;
		vistap.latframes = sc->paintedtime - soundtime;
	}
	else if (sc->samplequeue > 0)
	{	//write-once queue (WASAPI, SDL...): the play point is unknown; the queue it asked us to fill bounds it
		vistap.latmode = VISLAT_QUEUE;
		vistap.latframes = sc->samplequeue / sc->sn.numchannels;
	}
	else
		vistap.latmode = VISLAT_MIXAHEAD;
	if (vistap.latframes < 0)
		vistap.latframes = 0;
	vistap.lattotal = vistap.total;
	vistap.latstamp = Sys_DoubleTime();
}

static lpstate_t *LP_State(soundcardinfo_t *sc)
{
	lpstate_t *lp, *reuse = NULL;
	int i;
	lp_paints++;
	for (i = 0; i < LP_CARDS; i++)
	{
		lp = &lpstates[i];
		if (lp->card == sc && lp->rate == sc->sn.speed && lp->channels == sc->sn.numchannels)
		{
			lp->lastpaint = lp_paints;
			return lp;
		}
		if (!reuse || lp_paints - lp->lastpaint > lp_paints - reuse->lastpaint)
			reuse = lp;	//idle longest: a card that went away stops being painted
	}
	if (reuse->f.running)
		lp_running--;
	memset(reuse, 0, sizeof(*reuse));
	reuse->card = sc;
	reuse->rate = sc->sn.speed;
	reuse->channels = sc->sn.numchannels;
	reuse->lastpaint = lp_paints;
	return reuse;
}

void SNDVIS_Lowpass(soundcardinfo_t *sc, int *pb, int stride, int frames)
{
	lpstate_t *lp;
	int was;
	if (frames <= 0 || (!vistap.lptarget && !lp_running))
		return;
	lp = LP_State(sc);
	was = lp->f.running;
	LP_Process(&lp->f, sc->sn.speed, sc->sn.numchannels, vistap.lptarget, vistap.lptau, pb, stride, frames);
	lp_running += lp->f.running - was;
}

/* ---- main thread ---- */

void SNDVIS_Latch(void)
{
	float f;
	vistap.tapon = !!snd_vis.ival;
	if (!vistap.tapon)
		vistap.card = NULL;	//re-enabling starts a fresh ring
	f = snd_fx_lowpass.value;
	vistap.lptarget = (f > 0 && f < 20000) ? (f < 10 ? 10 : f) : 0;
	f = snd_fx_lowpass_time.value;
	vistap.lptau = (f > 0.005f) ? (f < 60 ? f : 60) : 0.005f;
}

//console output is batched: log.c opens, appends and closes the log file per Con_Printf, and
//120 separate trace lines a second took a minimised client from 60 fps to 2. Its buffer is 2048 bytes.
static char visprintbuf[1400];
static size_t visprintlen;
static void Vis_PrintFlush(void)
{
	if (visprintlen)
		Con_Printf("%s", visprintbuf);
	visprintlen = 0;
}
static void Vis_Print(const char *fmt, ...)
{
	va_list argptr;
	char line[256];
	size_t l;
	va_start(argptr, fmt);
	vsnprintf(line, sizeof(line), fmt, argptr);
	va_end(argptr);
	line[sizeof(line)-1] = 0;
	l = strlen(line);
	if (visprintlen + l >= sizeof(visprintbuf))
		Vis_PrintFlush();
	memcpy(visprintbuf+visprintlen, line, l+1);
	visprintlen += l;
}
static void SNDVIS_Trace(unsigned int bassonsets, unsigned int trebonsets)
{
	const viscore_t *v = &vis.core;
	const float *o = v->out;
	if (v->onsets[0] != bassonsets)
		Vis_Print("snd_vis onset bass t=%.4f r=%.2f\n", v->clock, v->prevratio[0]);
	if (v->onsets[1] != trebonsets)
		Vis_Print("snd_vis onset treb t=%.4f r=%.2f\n", v->clock, v->prevratio[1]);
	if (snd_vis_trace.ival >= 2)
		Vis_Print("snd_vis t=%.4f b %.3f m %.3f t %.3f ba %.3f ma %.3f ta %.3f v %.3f va %.3f beat %.3f bb %.3f bt %.3f rms %.6f lat %.1f\n",
			o[12], o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], o[9], o[10], o[11], vis.latency*1000);
}

//analyses every grid point that has become audible since the last call; returns whether values are live
static int SNDVIS_Update(void)
{
	soundcardinfo_t *sc;
	double now, behind = 0, step = 1, g, t0;
	quint64_t end = 0, total, first = 0, oldest, we;
	int rate = 0, reason = VISNA_OK, latsrc = VISSRC_CVAR, reinit = 0, n;
	unsigned int resets = 0, i, len, on0, on1;
	float visrate = snd_vis_rate.value, dt;

	if (!snd_vis.ival)
	{
		vis.reason = VISNA_DISABLED;
		return 0;
	}
	if (!(visrate > 0) || visrate > 1000)
		visrate = 1000;
	else if (visrate < 10)
		visrate = 10;
	now = Sys_DoubleTime();

	S_LockMixer();
	sc = sndcardinfo;
	if (!S_HaveOutput() || !sc)
		reason = VISNA_NOCARD;
	else if (sc->sn.sampleformat == QSF_EXTERNALMIXER)
		reason = VISNA_EXTERNAL;
	else if (vistap.card != sc || vistap.total < VIS_N)
		reason = VISNA_NOSAMPLES;
	else
	{
		rate = vistap.rate;
		total = vistap.total;
		resets = vistap.resets;
		if (snd_vis_latency.value >= 0)
			behind = snd_vis_latency.value * rate;
		else if (vistap.latmode == VISLAT_CURSOR || vistap.latmode == VISLAT_QUEUE)
		{
			latsrc = (vistap.latmode == VISLAT_CURSOR) ? VISSRC_CURSOR : VISSRC_QUEUE;
			behind = (double)(total - vistap.lattotal) + vistap.latframes - (now - vistap.latstamp) * rate;
		}
		else
		{
			latsrc = VISSRC_MIXAHEAD;
			behind = _snd_mixahead.value * rate;
		}
		if (!(behind > 0))
			behind = 0;
		if (behind > VIS_RING - VIS_N)
			behind = VIS_RING - VIS_N;
		if (behind > (double)(total - VIS_N))
			behind = (double)(total - VIS_N);
		end = total - (quint64_t)behind;
		oldest = (total > VIS_RING) ? total - VIS_RING + VIS_N : VIS_N;	//earliest window end still wholly in the ring
		vis.ringfill = (total < VIS_RING) ? (unsigned int)total : VIS_RING;

		step = rate / visrate;
		reinit = !vis.coreinit || resets != vis.resets || rate != vis.rate;
		if (reinit || !vis.haveend)
			vis.nextend = end;	//the grid starts at the newest audible window
		if ((quint64_t)vis.nextend <= end)
		{
			double pending = floor((end - vis.nextend) / step) + 1;
			if (pending > VIS_MAXCATCHUP)
				vis.nextend += (pending - VIS_MAXCATCHUP) * step;	//too far behind: drop the oldest
			if (vis.nextend < oldest)
				vis.nextend += ceil((oldest - vis.nextend) / step) * step;
			first = (quint64_t)vis.nextend;
			if (first <= end)
			{
				len = (unsigned int)(end - first) + VIS_N;
				for (i = 0; i < len; i++)
					vis.span[i] = vistap.ring[(unsigned int)(first - VIS_N + i) & (VIS_RING-1)];
			}
			else
				first = 0;
		}
	}
	S_UnlockMixer();

	if (reason != VISNA_OK)
	{
		if (reason == VISNA_EXTERNAL && !vis.alwarned)
		{
			Con_Printf(CON_WARNING "snd_vis: the OpenAL output bypasses the software mixer, so there is nothing to analyse; set s_al_disable 1 (or choose a non-OpenAL s_device) and snd_restart\n");
			vis.alwarned = 1;
		}
		vis.reason = reason;
		return 0;
	}
	vis.reason = VISNA_OK;
	vis.alwarned = 0;
	vis.latency = behind / rate;
	vis.latsrc = latsrc;

	if (reinit)
	{
		Vis_Init(&vis.core, rate);
		vis.coreinit = 1;
		vis.resets = resets;
		vis.rate = rate;
		vis.haveend = 0;
	}
	if (!first)
		return 1;	//no grid point has become audible since the last call: keep the cached analysis

	vis.core.wantspec = (realtime - vis.imagetime < 1);
	for (g = vis.nextend, n = 0; n < VIS_MAXCATCHUP && (we = (quint64_t)g) <= end; g += step, n++)
	{
		dt = vis.haveend ? (float)(we - vis.lastend) / rate : (float)VIS_N / rate;
		on0 = vis.core.onsets[0];
		on1 = vis.core.onsets[1];
		t0 = Sys_DoubleTime();
		Vis_Process(&vis.core, vis.span + (we - first), dt);
		t0 = Sys_DoubleTime() - t0;
		vis.costsum += t0;
		vis.costn++;
		if (vis.costmax < t0)
			vis.costmax = t0;
		vis.lastend = we;
		vis.haveend = 1;
		vis.serial++;
		if (snd_vis_trace.ival)
			SNDVIS_Trace(on0, on1);
	}
	Vis_PrintFlush();
	vis.nextend = g;
	if (vis.maxpercall < (unsigned int)n)
		vis.maxpercall = n;
	return 1;
}

static void SNDVIS_Info_f(void)
{
	static const char *names[VIS_OUTS] = {"bass", "mid", "treb", "bass_att", "mid_att", "treb_att", "vol", "vol_att",
		"beat", "beat_bass", "beat_treb", "rms", "time", "available", "reserved", "reserved"};
	static const float zeros[VIS_OUTS];
	int live = SNDVIS_Update(), i, havecard = 0, rate = 0, channels = 0, bits = 0, threaded = 0, queue = 0;
	const float *o = live ? vis.core.out : zeros;
	char name[64], gain[96];
	lpfilter_t f;

	memset(&f, 0, sizeof(f));
	S_LockMixer();
	if (S_HaveOutput() && sndcardinfo)
	{
		soundcardinfo_t *sc = sndcardinfo;
		havecard = 1;
		Q_strncpyz(name, sc->name, sizeof(name));
		rate = sc->sn.speed;
		channels = sc->sn.numchannels;
		bits = sc->sn.samplebytes*8;
		threaded = sc->selfpainting;
		queue = sc->samplequeue;
		for (i = 0; i < LP_CARDS; i++)
			if (lpstates[i].card == sc)
			{	//its energy counters restart at every snd_visinfo
				f = lpstates[i].f;
				lpstates[i].f.insq = lpstates[i].f.outsq = 0;
				lpstates[i].f.sqframes = 0;
			}
	}
	S_UnlockMixer();

	Vis_Print("snd_vis: available %i%s%s\n", live, live?"":" -- ", live?"":vis_reasons[vis.reason]);
	if (havecard)
		Vis_Print(" card \"%s\": %i Hz, %i ch, %i-bit, %s mixer, %s\n", *name?name:"default device", rate, channels, bits,
			threaded?"threaded":"main-thread", queue>0?"queue output":queue<0?"device-paced output":"memory-mapped output");
	if (live)
	{
		Vis_Print(" latency %.1f ms (%s)\n", vis.latency*1000, vis_latsrcs[vis.latsrc]);
		Vis_Print(" ring %u/%u frames (%.0f ms)\n", vis.ringfill, (unsigned int)VIS_RING, vis.ringfill*1000.0/vis.rate);
	}
	for (i = 0; i < VIS_OUTS; i += 4)
		Vis_Print(" %-9s %8.3f  %-9s %8.3f  %-9s %8.3f  %-9s %8.3f\n", names[i], o[i], names[i+1], o[i+1], names[i+2], o[i+2], names[i+3], o[i+3]);
	if (live)
		Vis_Print(" onsets: bass %u, treb %u\n", vis.core.onsets[0], vis.core.onsets[1]);
	if (vis.costn)
		Vis_Print(" cost %.1f us avg, %.1f us max over %u analyses; at most %u in one call\n", vis.costsum*1e6/vis.costn, vis.costmax*1e6, vis.costn, vis.maxpercall);
	if (f.sqframes && f.insq > 0 && f.outsq > 0)	//measured on the paint buffer as it goes to the device
		Q_snprintfz(gain, sizeof(gain), ", output/input %+.1f dB over the last %u filtered frames", 10*log10(f.outsq/f.insq), f.sqframes);
	else
		*gain = 0;
	if (f.running)
		Vis_Print(" lowpass target %g Hz, effective %.0f Hz%s\n", vistap.lptarget, f.fc, gain);
	else
		Vis_Print(" lowpass target %g Hz, bypassed%s\n", vistap.lptarget, gain);
	Vis_PrintFlush();
}

void SNDVIS_Frame(void)
{
	if (snd_vis_trace.ival)
		SNDVIS_Update();	//once per frame, as QC would; the trace lines come from the analyses themselves
}

void SNDVIS_Init(void)
{
	Cvar_Register(&snd_vis, "Sound visualiser");
	Cvar_Register(&snd_vis_rate, "Sound visualiser");
	Cvar_Register(&snd_vis_latency, "Sound visualiser");
	Cvar_Register(&snd_vis_trace, "Sound visualiser");
	Cvar_Register(&snd_fx_lowpass, "Sound controls");
	Cvar_Register(&snd_fx_lowpass_time, "Sound controls");
	Cmd_AddCommandD("snd_visinfo", SNDVIS_Info_f, "Reports the state of the snd_getvis analysis of the software mix, and snd_fx_lowpass.");
	vis.reason = VISNA_NOSAMPLES;
	vis.imagetime = -1e9;
}

/* ---- QC ---- */
#if defined(CSQC_DAT) || defined(MENU_DAT)
//float(float *out, float count) snd_getvis
void QCBUILTIN PF_snd_getvis(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	int qcptr = G_INT(OFS_PARM0);
	float fcount = G_FLOAT(OFS_PARM1);
	int count, i, live;
	float *out;

	G_FLOAT(OFS_RETURN) = 0;
	if (!(fcount >= 1))
		return;
	count = (fcount > VIS_OUTS) ? VIS_OUTS : (int)fcount;
	if (qcptr <= 0 || qcptr+count*(int)sizeof(float) >= prinst->stringtablesize)
	{
		PR_BIError(prinst, "PF_snd_getvis: invalid pointer\n");
		return;
	}
	out = (float*)(prinst->stringtable + qcptr);
	live = SNDVIS_Update();
	for (i = 0; i < count; i++)
		out[i] = live ? vis.core.out[i] : 0;
	G_FLOAT(OFS_RETURN) = count;
}

//float(string imagename, float bands) snd_visimage
void QCBUILTIN PF_snd_visimage(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	const char *name = PR_GetStringOfs(prinst, OFS_PARM0);
	float fb = G_FLOAT(OFS_PARM1);
	int bands, live, i, slot = -1;
	unsigned int serial;
	image_t *tid;

	G_FLOAT(OFS_RETURN) = 0;
	if (!name || !*name || qrenderer == QR_NONE)
		return;
	if (fb != fb || fb == 0)
		bands = 256;
	else if (fb < 16)
		bands = 16;
	else if (fb > VIS_SPECBANDS)
		bands = VIS_SPECBANDS;
	else
		bands = (int)fb;

	vis.imagetime = realtime;
	live = SNDVIS_Update();
	serial = live ? vis.serial : 0;

	tid = Image_FindTexture(name, NULL, RT_IMAGEFLAGS);
	if (!TEXVALID(tid))
		tid = Image_CreateTexture(name, NULL, RT_IMAGEFLAGS);
	if (!TEXVALID(tid))
		return;

	for (i = 0; i < VISIMG_MAX; i++)
		if (!strcmp(visimg[i].name, name))
			slot = i;
	if (slot < 0)
	{	//least recently used
		for (slot = 0, i = 1; i < VISIMG_MAX; i++)
			if (visimg[i].lastuse < visimg[slot].lastuse)
				slot = i;
		Q_strncpyz(visimg[slot].name, name, sizeof(visimg[slot].name));
		visimg[slot].tid = NULL;
	}
	visimg[slot].lastuse = realtime;

	//upload only when the analysis moved on (or the texture is not ours as we left it)
	if (visimg[slot].tid != tid || visimg[slot].bands != bands || visimg[slot].serial != serial ||
		tid->status != TEX_LOADED || tid->width != bands || tid->height != 2)
	{
		if (live)
			Vis_FillImage(&vis.core, bands, visimg_pixels);
		else
			Vis_BlankImage(bands, visimg_pixels);
		//IF_NOSRGB: this is data; an sRGB upload would bend 0.5 to 0.21 in the shader
		Image_Upload(tid, TF_RGBA32, visimg_pixels, NULL, bands, 2, 1, RT_IMAGEFLAGS|IF_NOSRGB);
		tid->width = bands;
		tid->height = 2;
		visimg[slot].tid = tid;
		visimg[slot].bands = bands;
		visimg[slot].serial = serial;
	}
	G_FLOAT(OFS_RETURN) = live;
}
#endif
#endif
