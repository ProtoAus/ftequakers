/*
	snd_flac.c - native FLAC decoding for FTE.

	WHY THIS FILE EXISTS

	FTE had no FLAC decoder at all.  fs.c:1667 lists "flac" among the extensions
	the filesystem will hand to the sound system, and snd_minimp3.c:137 goes out of
	its way to REJECT a "fLaC" magic so the mp3 sniffer cannot steal the file --
	but nothing downstream ever claimed it, so every .flac ended at
	S_LoadSoundWorker's "Format not recognised" and played silently.

	That is not a hypothetical gap.  they-hunger's th_escape ships ten of them:

		sound/th_escape/repairsnd01..06.flac      the vehicle repair loop
		sound/th_escape/diesel_idle_loop.flac     the truck engine
		sound/th_escape/diesel_rev.flac
		sound/th_escape/diesel_ignition-rev.flac
		sound/th_escape/diesel_ignition-rev-idle.flac

	i.e. the entire audio track of the map's central set piece, and the console
	filled up with ten "Format not recognised" lines on every load.

	WHY A DECODER RATHER THAN libFLAC

	libFLAC is not in the tree and is not installed in the MSYS2 root this engine
	builds in, so linking it would add a build dependency and a shipped DLL for one
	container.  FLAC's format is small enough not to need either: this is the whole
	of the subset a game asset can use -- CONSTANT / VERBATIM / FIXED / LPC
	subframes, Rice and escaped residuals, and the three stereo decorrelations.
	Same shape as snd_minimp3.c beside it: one sniffer, one entry point, decode the
	whole thing up front into one PCM block and hand it to ResampleSfx.

	NOT IMPLEMENTED, ON PURPOSE
	  - Ogg FLAC (an "OggS" container).  A different demuxer for a form nothing in
	    the corpus uses; the sniffer requires the native "fLaC" magic so such a file
	    falls through to the next loader instead of being mis-decoded.
	  - CRC-8 / CRC-16 verification.  A truncated or corrupt frame is caught by the
	    bit reader running off the end of the buffer, which is the failure that
	    actually happens; a wrong-but-well-formed frame is not.
	  - Seeking.  Everything here decodes up front, exactly as the wav/ogg/mp3
	    loaders do for non-forcedecode sounds.
*/

#include "quakedef.h"

#ifndef SERVERONLY

#include "sound.h"

#define FLAC_MAX_CHANNELS	8
#define FLAC_MAX_BLOCKSIZE	65535
#define FLAC_MAX_LPCORDER	32

/*
	Bit reader.

	MSB-first, one byte at a time.  Deliberately not the usual 64-bit accumulator:
	this runs ONCE per file at load time and the simple form has no shift-count
	edge cases to get wrong (a 32-bit read is legal in a FLAC escaped residual, and
	`1u << 32` is undefined behaviour).

	Running off the end of the buffer latches `error` and keeps returning; every
	caller checks it before trusting what it decoded, so a truncated file produces
	a clean failure rather than a wild read.
*/
typedef struct
{
	const qbyte	*data;
	size_t		len;
	size_t		pos;
	unsigned int	accbyte;
	int			accbits;
	qboolean	error;
} flacbits_t;

static unsigned int FLAC_Bits(flacbits_t *b, int n)
{
	unsigned int v = 0;
	int take;
	while (n > 0)
	{
		if (!b->accbits)
		{
			if (b->pos >= b->len)
			{
				b->error = true;
				return v;
			}
			b->accbyte = b->data[b->pos++];
			b->accbits = 8;
		}
		take = (n < b->accbits) ? n : b->accbits;
		v = (v << take) | ((b->accbyte >> (b->accbits - take)) & ((1u << take) - 1u));
		b->accbits -= take;
		n -= take;
	}
	return v;
}

/* Two's-complement read.  n == 32 is left to the cast: there is no bit above it
   to sign-extend from, and `1u << 32` would be undefined. */
static int FLAC_SBits(flacbits_t *b, int n)
{
	unsigned int v;
	if (n <= 0)
		return 0;
	v = FLAC_Bits(b, n);
	if (n < 32 && (v & (1u << (n - 1))))
		v |= ~((1u << n) - 1u);
	return (int)v;
}

/* Number of 0 bits before the next 1.  The cap is a runaway guard, not a format
   limit: a corrupt file can present a very long run of zeroes and this must not
   spin for the length of the file. */
static unsigned int FLAC_Unary(flacbits_t *b)
{
	unsigned int n = 0;
	while (!b->error)
	{
		if (FLAC_Bits(b, 1))
			break;
		if (++n > (1u << 24))
		{
			b->error = true;
			break;
		}
	}
	return n;
}

static void FLAC_Align(flacbits_t *b)
{
	b->accbits = 0;	/* discard the rest of the current byte */
}

/*
	Residual: the prediction error for samples [predorder, blocksize).

	Split into 2^partorder partitions, each with its own Rice parameter.  The FIRST
	partition is short by the predictor order, because those samples were sent
	verbatim in the subframe header and have no residual.
*/
static qboolean FLAC_Residual(flacbits_t *b, int blocksize, int predorder, int *out)
{
	int method, partorder, partitions, p, i, plen, param, escbits;
	int paramlen, escape;

	method = FLAC_Bits(b, 2);
	if (method > 1)
		return false;			/* 2 and 3 are reserved */
	paramlen = (method == 0) ? 4 : 5;
	escape   = (method == 0) ? 15 : 31;

	partorder  = FLAC_Bits(b, 4);
	partitions = 1 << partorder;
	if (blocksize % partitions)
		return false;
	plen = blocksize >> partorder;
	if (plen < predorder)
		return false;			/* the first partition cannot be negative-length */

	i = predorder;
	for (p = 0; p < partitions; p++)
	{
		int count = plen - ((p == 0) ? predorder : 0);
		param = FLAC_Bits(b, paramlen);
		if (param == escape)
		{
			/* Escape: the partition is raw n-bit samples, not Rice coded.  n == 0
			   is legal and means every residual in the partition is zero. */
			escbits = FLAC_Bits(b, 5);
			while (count-- > 0)
				out[i++] = escbits ? FLAC_SBits(b, escbits) : 0;
		}
		else
		{
			while (count-- > 0)
			{
				unsigned int q = FLAC_Unary(b);
				unsigned int r = param ? FLAC_Bits(b, param) : 0;
				unsigned int val = (q << param) | r;
				/* zig-zag: even -> +v/2, odd -> -(v+1)/2 */
				out[i++] = (val & 1) ? -(int)((val >> 1) + 1) : (int)(val >> 1);
			}
		}
		if (b->error)
			return false;
	}
	return true;
}

/*
	One subframe = one channel of one frame.

	`bps` arrives already adjusted for the stereo side channel's extra bit; the
	wasted-bits count is subtracted here and shifted back in at the end.
*/
static qboolean FLAC_Subframe(flacbits_t *b, int blocksize, int bps, int *out)
{
	int type, wasted = 0, order, precision, shift, i, j;
	int coefs[FLAC_MAX_LPCORDER];

	if (FLAC_Bits(b, 1))
		return false;			/* mandatory zero padding bit */
	type = FLAC_Bits(b, 6);
	if (FLAC_Bits(b, 1))
		wasted = (int)FLAC_Unary(b) + 1;
	if (wasted >= bps)
		return false;
	bps -= wasted;

	if (type == 0)
	{	/* CONSTANT */
		int v = FLAC_SBits(b, bps);
		for (i = 0; i < blocksize; i++)
			out[i] = v;
	}
	else if (type == 1)
	{	/* VERBATIM */
		for (i = 0; i < blocksize; i++)
			out[i] = FLAC_SBits(b, bps);
	}
	else if (type >= 8 && type <= 12)
	{	/* FIXED: a hardcoded polynomial predictor of order 0..4 */
		order = type - 8;
		if (order > blocksize)
			return false;
		for (i = 0; i < order; i++)
			out[i] = FLAC_SBits(b, bps);
		if (!FLAC_Residual(b, blocksize, order, out))
			return false;
		switch (order)
		{
		case 0:
			break;
		case 1:
			for (i = 1; i < blocksize; i++)
				out[i] += out[i-1];
			break;
		case 2:
			for (i = 2; i < blocksize; i++)
				out[i] += 2*out[i-1] - out[i-2];
			break;
		case 3:
			for (i = 3; i < blocksize; i++)
				out[i] += 3*out[i-1] - 3*out[i-2] + out[i-3];
			break;
		default:
			for (i = 4; i < blocksize; i++)
				out[i] += 4*out[i-1] - 6*out[i-2] + 4*out[i-3] - out[i-4];
			break;
		}
	}
	else if (type >= 32)
	{	/* LPC: coefficients carried in the stream */
		order = type - 31;
		if (order > blocksize || order > FLAC_MAX_LPCORDER)
			return false;
		for (i = 0; i < order; i++)
			out[i] = FLAC_SBits(b, bps);
		precision = FLAC_Bits(b, 4);
		if (precision == 15)
			return false;		/* all-ones is the reserved/invalid marker */
		precision++;
		shift = FLAC_SBits(b, 5);
		if (shift < 0)
			return false;		/* negative shift is not a thing any encoder emits */
		for (i = 0; i < order; i++)
			coefs[i] = FLAC_SBits(b, precision);
		if (!FLAC_Residual(b, blocksize, order, out))
			return false;
		/* 64-bit accumulator is required, not defensive: 32 taps of a 15-bit
		   coefficient against a 32-bit sample overflows int comfortably. */
		for (i = order; i < blocksize; i++)
		{
			qint64_t sum = 0;
			for (j = 0; j < order; j++)
				sum += (qint64_t)coefs[j] * out[i-1-j];
			out[i] += (int)(sum >> shift);
		}
	}
	else
		return false;			/* 2..7 and 13..31 are reserved */

	if (wasted)
		for (i = 0; i < blocksize; i++)
			out[i] = (int)((unsigned int)out[i] << wasted);

	return !b->error;
}

/* Frame/sample number: the same variable-length encoding UTF-8 uses, up to 7
   bytes.  The value is not needed (nothing here seeks), only its length. */
static qboolean FLAC_SkipUTF8(flacbits_t *b)
{
	unsigned int c = FLAC_Bits(b, 8);
	int extra;
	if (c < 0x80)
		return !b->error;
	if ((c & 0xe0) == 0xc0)		extra = 1;
	else if ((c & 0xf0) == 0xe0)	extra = 2;
	else if ((c & 0xf8) == 0xf0)	extra = 3;
	else if ((c & 0xfc) == 0xf8)	extra = 4;
	else if ((c & 0xfe) == 0xfc)	extra = 5;
	else if (c == 0xfe)			extra = 6;
	else						return false;
	while (extra-- > 0)
		FLAC_Bits(b, 8);
	return !b->error;
}

/* The frame header's 4-bit sample-rate field indexes a table of common rates, but
   a valid stream's frames always agree with STREAMINFO and only STREAMINFO's rate
   is used here -- so the field is skipped, not decoded, and the table is not
   carried.  The bits-per-sample field DOES have to be read: 0 there means "as
   STREAMINFO" rather than a rate. */
static const int flac_bpstab[8] =
{	0, 8, 12, 0, 16, 20, 24, 32	};

/*
	Is this a native FLAC stream, and where does it start?

	The plugin table is walked backwards (snd_mem.c:113) so this sniffer is tried
	ahead of the older loaders; requiring the exact magic keeps it from claiming
	anything that is not ours.  An ID3v2 tag in front of a FLAC file is not legal
	but does exist in the wild, and skipping it costs four lines.
*/
static qboolean FLAC_Sniff(const qbyte *data, size_t datalen, size_t *startofs)
{
	size_t ofs = 0;

	if (datalen > 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3')
	{
		ofs = 10 + (((size_t)(data[6] & 0x7f) << 21) |
		            ((size_t)(data[7] & 0x7f) << 14) |
		            ((size_t)(data[8] & 0x7f) <<  7) |
		            ((size_t)(data[9] & 0x7f)      ));
		if (ofs + 4 > datalen)
			return false;
	}

	if (datalen < ofs + 4 || strncmp((const char*)data + ofs, "fLaC", 4))
		return false;

	*startofs = ofs;
	return true;
}

static qboolean QDECL S_LoadFLACSound (sfx_t *s, qbyte *data, size_t datalen, int sndspeed, qboolean forcedecode)
{
	flacbits_t	br;
	size_t		ofs = 0;
	int			sirate = 0, sichannels = 0, sibps = 0, simaxblock = 0;
	quint64_t	sitotal = 0;
	int			*chan[FLAC_MAX_CHANNELS];
	int			i, c;
	short		*pcm = NULL;
	size_t		pcmframes = 0, pcmcapacity = 0;
	qboolean	gotinfo = false, ok = true;

	if (!FLAC_Sniff(data, datalen, &ofs))
		return false;

	memset(chan, 0, sizeof(chan));

	br.data = data;
	br.len = datalen;
	br.pos = ofs + 4;			/* past "fLaC" */
	br.accbyte = 0;
	br.accbits = 0;
	br.error = false;

	/* --- metadata blocks; only STREAMINFO is wanted, the rest are skipped --- */
	for (;;)
	{
		unsigned int last, type, blen;
		last = FLAC_Bits(&br, 1);
		type = FLAC_Bits(&br, 7);
		blen = FLAC_Bits(&br, 24);
		if (br.error)
			break;

		if (type == 0 && blen >= 34 && !gotinfo)
		{
			FLAC_Bits(&br, 16);					/* min blocksize */
			simaxblock = (int)FLAC_Bits(&br, 16);
			FLAC_Bits(&br, 24);					/* min framesize */
			FLAC_Bits(&br, 24);					/* max framesize */
			sirate     = (int)FLAC_Bits(&br, 20);
			sichannels = (int)FLAC_Bits(&br, 3) + 1;
			sibps      = (int)FLAC_Bits(&br, 5) + 1;
			/* total samples is 36 bits; taken in two reads because FLAC_Bits
			   returns an unsigned int. */
			sitotal    = ((quint64_t)FLAC_Bits(&br, 4) << 32) | FLAC_Bits(&br, 32);
			for (i = 0; i < 16; i++)
				FLAC_Bits(&br, 8);				/* md5 of the decoded audio */
			blen -= 34;
			gotinfo = true;
		}
		while (blen-- > 0)
			FLAC_Bits(&br, 8);
		if (br.error || last)
			break;
	}

	if (!gotinfo || br.error)
		return false;
	if (sirate <= 0 || sichannels < 1 || sichannels > FLAC_MAX_CHANNELS)
		return false;
	if (sibps < 4 || sibps > 32)
		return false;
	if (simaxblock < 1 || simaxblock > FLAC_MAX_BLOCKSIZE)
		return false;

	for (i = 0; i < sichannels; i++)
	{
		chan[i] = BZ_Malloc(simaxblock * sizeof(int));
		if (!chan[i])
			goto done;
	}

	/* STREAMINFO's total is advisory (0 = unknown), but when it is there it sizes
	   the output buffer exactly and saves every realloc. */
	if (sitotal > 0 && sitotal < 0x40000000)
	{
		pcmcapacity = (size_t)sitotal;
		pcm = BZ_Malloc(pcmcapacity * sichannels * sizeof(short));
		if (!pcm)
		{
			pcmcapacity = 0;
			goto done;
		}
	}

	/* --- frames --- */
	for (;;)
	{
		unsigned int sync, bsbits, srbits, chanasgn, ssbits;
		int blocksize, framebps, framechans;

		FLAC_Align(&br);
		if (br.pos >= br.len)
			break;
		if (sitotal > 0 && pcmframes >= (size_t)sitotal)
			break;

		sync = FLAC_Bits(&br, 14);
		if (br.error)
			break;
		if (sync != 0x3ffe)
		{	/* Not a frame header.  Either the stream ended on padding or the file
			   is damaged; either way there is nothing more to decode.  Everything
			   already decoded is kept -- a truncated asset should play as far as it
			   goes rather than vanish. */
			break;
		}
		FLAC_Bits(&br, 1);						/* reserved */
		FLAC_Bits(&br, 1);						/* blocking strategy */
		bsbits   = FLAC_Bits(&br, 4);
		srbits   = FLAC_Bits(&br, 4);
		chanasgn = FLAC_Bits(&br, 4);
		ssbits   = FLAC_Bits(&br, 3);
		FLAC_Bits(&br, 1);						/* reserved */

		if (!FLAC_SkipUTF8(&br))
			break;

		if (bsbits == 0)			break;					/* reserved */
		else if (bsbits == 1)		blocksize = 192;
		else if (bsbits <= 5)		blocksize = 576 << (bsbits - 2);
		else if (bsbits == 6)		blocksize = (int)FLAC_Bits(&br, 8) + 1;
		else if (bsbits == 7)		blocksize = (int)FLAC_Bits(&br, 16) + 1;
		else						blocksize = 256 << (bsbits - 8);

		if (srbits >= 1 && srbits <= 11)	{ /* table value; rate is fixed by STREAMINFO anyway */ }
		else if (srbits == 12)		FLAC_Bits(&br, 8);
		else if (srbits == 13)		FLAC_Bits(&br, 16);
		else if (srbits == 14)		FLAC_Bits(&br, 16);
		else if (srbits == 15)		break;					/* invalid */

		framebps = flac_bpstab[ssbits];
		if (!framebps)
			framebps = sibps;						/* 0 = "as STREAMINFO", 3 = reserved */

		if (chanasgn < 8)			framechans = (int)chanasgn + 1;
		else if (chanasgn <= 10)	framechans = 2;
		else						break;					/* reserved */

		FLAC_Bits(&br, 8);							/* CRC-8 of the header */

		if (blocksize > simaxblock || framechans != sichannels || br.error)
			break;

		for (c = 0; c < framechans; c++)
		{
			/* The side channel of a decorrelated pair carries one extra bit,
			   because a difference of two n-bit values needs n+1. */
			int bps = framebps;
			if ((chanasgn == 8 && c == 1) || (chanasgn == 9 && c == 0) || (chanasgn == 10 && c == 1))
				bps++;
			if (!FLAC_Subframe(&br, blocksize, bps, chan[c]))
			{
				ok = false;
				break;
			}
		}
		if (!ok || br.error)
			break;

		FLAC_Align(&br);
		FLAC_Bits(&br, 16);							/* CRC-16 of the frame */

		/* Undo the stereo decorrelation, in place. */
		if (chanasgn == 8)
		{	/* left / side  ->  right = left - side */
			for (i = 0; i < blocksize; i++)
				chan[1][i] = chan[0][i] - chan[1][i];
		}
		else if (chanasgn == 9)
		{	/* side / right ->  left = side + right */
			for (i = 0; i < blocksize; i++)
				chan[0][i] = chan[0][i] + chan[1][i];
		}
		else if (chanasgn == 10)
		{	/* mid / side.  The mid channel dropped its low bit on encode; the
			   side channel's low bit is where it went. */
			for (i = 0; i < blocksize; i++)
			{
				int side = chan[1][i];
				int mid  = (chan[0][i] << 1) | (side & 1);
				chan[0][i] = (mid + side) >> 1;
				chan[1][i] = (mid - side) >> 1;
			}
		}

		if (pcmframes + blocksize > pcmcapacity)
		{
			size_t want = (pcmframes + blocksize) * 2;
			/* BZ_Realloc Sys_Errors rather than returning NULL, so there is no
			   failure branch to write; the NULL case is only about not handing
			   realloc a pointer this function has not allocated yet. */
			if (pcm)
				pcm = BZ_Realloc(pcm, want * sichannels * sizeof(short));
			else
				pcm = BZ_Malloc(want * sichannels * sizeof(short));
			pcmcapacity = want;
		}

		/* To S16, which is what every other loader here hands ResampleSfx.  A
		   24-bit source loses its bottom 8 bits; the mixer is 16-bit, so they
		   would be dropped a step later regardless. */
		for (i = 0; i < blocksize; i++)
		{
			for (c = 0; c < sichannels; c++)
			{
				int v = chan[c][i];
				if (framebps > 16)		v >>= (framebps - 16);
				else if (framebps < 16)	v <<= (16 - framebps);
				if (v > 32767)			v = 32767;
				else if (v < -32768)	v = -32768;
				pcm[(pcmframes + i) * sichannels + c] = (short)v;
			}
		}
		pcmframes += blocksize;
	}

	/* STREAMINFO's total is the authority when it is present: the last frame is
	   padded out to a whole block and those extra samples are not part of the
	   audio. */
	if (sitotal > 0 && pcmframes > (size_t)sitotal)
		pcmframes = (size_t)sitotal;

done:
	for (i = 0; i < FLAC_MAX_CHANNELS; i++)
		if (chan[i])
			BZ_Free(chan[i]);

	if (!pcm || !pcmframes)
	{
		if (pcm)
			BZ_Free(pcm);
		/* Deliberately NOT setting SLS_FAILED, for the same reason snd_minimp3.c
		   does not: returning false hands the buffer to the next loader, and
		   latching the failure would stop that. */
		Con_DPrintf("%s: sniffed as flac but decoded no audio\n", s->name);
		return false;
	}

	ok = ResampleSfx(s, sirate, sichannels, QAF_S16, pcmframes, -1, (qbyte*)pcm);
	BZ_Free(pcm);
	return ok;
}

/*
	Called from S_Init, beside S_RegisterMP3Plugin.  Order does not matter between
	the two: each requires its own container magic, so neither can claim the
	other's files.
*/
void S_RegisterFLACPlugin(void)
{
	S_RegisterSoundInputPlugin(NULL, S_LoadFLACSound);
}

#endif /* !SERVERONLY */
