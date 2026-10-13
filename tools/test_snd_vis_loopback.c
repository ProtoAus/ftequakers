// gcc -std=c99 tools/test_snd_vis_loopback.c -lm -lole32 -o test_snd_vis_loopback.exe
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#ifdef SNDVIS_TEST_FALLBACK
#undef _WIN32
#endif
#include "../engine/client/snd_vis_loopback.h"
#define SNDVIS_STANDALONE
#include "../engine/client/snd_vis.c"
static void PCM_Tests(void)
{
	unsigned char u8[] = {0,255}, s16[] = {0,0x80,0xff,0x7f};
	unsigned char s24[] = {0,0,0x80}, s32[] = {0,0,0,0x80};
	float f[] = {0.5f, -0.25f}, bad[] = {NAN, INFINITY};
	double d = -0.75;
	assert(fabs(SNDVIS_PCMMono(u8, 2, 1, 0)+1.0/256) < 1e-7);
	assert(fabs(SNDVIS_PCMMono(s16, 2, 2, 0)+1.0/65536) < 1e-7);
	assert(SNDVIS_PCMMono(s24, 1, 3, 0) == -1);
	assert(SNDVIS_PCMMono(s32, 1, 4, 0) == -1);
	assert(SNDVIS_PCMMono((unsigned char*)f, 2, 4, 1) == 0.125f);
	assert(SNDVIS_PCMMono((unsigned char*)bad, 2, 4, 1) == 0);
	assert(SNDVIS_PCMMono((unsigned char*)&d, 1, 8, 1) == -0.75f);
	VISLB_Clear(); assert(!vislb.total && vislb.generation);
	VISLB_Append((unsigned char*)f, 1, 2, 4, 1, 0);
	assert(vislb.total == 1 && vislb.ring[0] == 0.125f);
	// WASAPI permits a null data pointer for silent packets, including oversized packets.
	assert(VISLB_Append(NULL, VISLB_RING+100, 2, 4, 1, 1) == VISLB_RING);
	assert(vislb.total == VISLB_RING+101);
	for (int i = 0; i < VISLB_RING; i++) assert(vislb.ring[i] == 0);
	VISLB_Close(); assert(!vislb.total && !vislb.active);
	puts("PCM conversion/downmix/finite/reset/silent packet/bounded ring: passed");
}
static void Analyzer_Tests(void)
{
	viscore_t v = {0};
	lpfilter_t lp = {0};
	float samples[VIS_N];
	unsigned char image[16*2*4];
	int i, paint[2] = {0};
	Vis_Init(&v, 48000); v.wantspec = 1;
	for (i = 0; i < VIS_N; i++) samples[i] = 0.25f*sin(2*VIS_PI*1000*i/48000);
	Vis_Process(&v, samples, 0.05f);
	assert(v.out[11] > 0.17f);
	memset(samples, 0, sizeof(samples));
	for (i = 0; i < 40; i++) Vis_Process(&v, samples, 0.05f);
	assert(v.out[11] == 0 && v.out[6] < 1e-5f);
	Vis_Init(&v, 48000);
	Vis_FillImage(&v, 16, image);
	for (i = 0; i < 16; i++) assert(image[i*4] == 0 && image[(16+i)*4] == 128);
	Vis_BlankImage(16, image);
	assert(!LP_Process(&lp, 48000, 2, 0, 0.25f, paint, 2, 1));
	puts("analyzer tone/silence/source-reset spectrum: passed");
}
int main(int argc, char **argv)
{
	PCM_Tests(); Analyzer_Tests();
#if defined(_WIN32)
	if (argc > 1)
	{
		viscore_t core = {0};
		float samples[VIS_N];
		unsigned int i, generation = 0;
		DWORD start = GetTickCount();
		double now;
		int duration = atoi(argv[1]);
		while (GetTickCount()-start < (unsigned int)duration*1000)
		{
			now = GetTickCount64()/1000.0;
			if (VISLB_Poll(now) && vislb.total >= VIS_N)
			{
				if (generation != vislb.generation) { Vis_Init(&core, vislb.rate); generation = vislb.generation; }
				for (i = 0; i < VIS_N; i++) samples[i] = vislb.ring[(vislb.total-VIS_N+i)&(VISLB_RING-1)];
				Vis_Process(&core, samples, 0.05f);
				printf("loopback active=%i packets=%u rms=%.6f frames=%llu\n", vislb.active, vislb.packets, core.out[11], vislb.total);
			}
			else printf("loopback unavailable\n");
			Sleep(50);
		}
		VISLB_Close();
		assert(!vislb.active && !vislb.total && !vislb.client && !vislb.enumerator);
		puts("close/reset: passed");
	}
#else
	(void)argc; (void)argv;
	assert(!VISLB_Poll(0)); VISLB_Close();
	puts("non-Windows fallback: passed");
#endif
	return 0;
}
