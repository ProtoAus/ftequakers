// snd_vis.h -- FTESurf: analysis of the software mix for QC music visualisers, and the
// mixer low-pass. Included only by snd_vis.c, snd_mix.c, snd_dma.c, pr_csqc.c, pr_menu.c:
// the makefile does not track headers, so touch those .c files if this changes.
#ifndef SND_VIS_H
#define SND_VIS_H

void SNDVIS_Init(void);		//S_Init: cvars + snd_visinfo
void SNDVIS_Latch(void);	//S_Update, main thread, mixer locked: copies cvars for the mixer
void SNDVIS_Frame(void);	//S_Update, main thread, mixer unlocked: snd_vis_trace

//mixer side (any thread, always under mixermutex). pb is the paint buffer, stride ints per frame.
void SNDVIS_Tap(soundcardinfo_t *sc, const int *pb, int stride, int frames);	//before the filter
void SNDVIS_Lowpass(soundcardinfo_t *sc, int *pb, int stride, int frames);
void SNDVIS_NoteMixTime(soundcardinfo_t *sc, int soundtime);	//end of S_Update_

void QCBUILTIN PF_snd_getvis(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals);
void QCBUILTIN PF_snd_visimage(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals);

#endif
