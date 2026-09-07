/*
	snd_minimp3.c - MP3 decoding for FTE, via lieff/minimp3.

	WHY THIS FILE EXISTS

	FTE has two other mp3 paths and neither one works on a 64-bit Windows build:

	  * snd_mp3.c wraps libmad, which is GPL and patent-encumbered and so is not
	    distributed with the engine at all -- it will not even compile without a
	    libmad/ tree that nobody has.  AVAIL_MP3 is therefore off.
	  * The ACM path (snd_win.c) asks Windows for the "MPEG Layer-3" codec.
	    There has never been a 64-bit ACM MP3 driver on Windows, and on Windows
	    11 there is not a 32-bit one either.  It fails with
	        Couldn't init decoder
	        Format not recognised: <file>.mp3
	    which is exactly what quakers' qconsole.log was full of.

	The practical effect for the quakers mod was that EVERY .mp3 in the game was
	silent: 153 ambient_generic sources across the Sven Co-op map corpus, the
	target_cdaudio music path (same plugin list), and the mod's own HUD stings.
	A .mp3 LOADS fine -- only the decode fails -- so S_LoadSoundWorker's
	.wav/.opus/.ogg extension fallback never even ran, and dropping a converted
	copy beside the original did nothing.

	minimp3 is a single public-domain (CC0) header with no dependencies, so it
	can just live in the tree.  Registering it here means every existing caller
	-- ambient_generic, the music/cd path, S_LocalSound -- starts working with
	no changes anywhere else.

	Decoding is done up-front into one PCM block rather than streamed.  That
	matches how S_LoadWavSound and S_LoadOVSound already behave for
	non-forcedecode sounds, and keeps this file to one entry point.  Cost is
	memory: a 3-minute stereo track is ~30 MB of S16 at 44.1kHz before
	ResampleSfx trims it to the mixer rate.
*/

#include "quakedef.h"

#ifndef SERVERONLY

#include "sound.h"

/* Only MPEG1/2/2.5 Layer 3 -- drop the Layer 1/2 tables we will never use.
   Keeps roughly 15KB out of the binary. */
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static qboolean QDECL S_LoadMP3Sound (sfx_t *s, qbyte *data, size_t datalen, int sndspeed, qboolean forcedecode);

/* How far into the file we will hunt for the first frame sync before deciding
   this is not an mp3 at all.  ID3v2 tags are self-describing and skipped
   exactly, so this only has to cover junk-prefixed or slightly-corrupt files. */
#define MP3_SYNC_SEARCH_LIMIT 65536

/*
	Is this an mp3?

	The plugin list is a chain of sniffers -- returning false hands the buffer to
	the next loader -- so this has to be picky.  A bare "first two bytes look like
	0xFFEx" test aliases against far too much (it matches plenty of raw PCM and
	the odd compressed stream), and a wrong yes here would swallow files the wav
	or vorbis loader should have got.

	Accepted:
	  "ID3"  - an ID3v2 tag, which nothing else in the sound path uses.
	  "\xFF\xFB"-style sync where the version/layer/bitrate/samplerate nibbles are
	           all individually legal AND a second frame header lands exactly
	           frame_bytes later.  Two consecutive valid headers is the standard
	           way to tell a real stream from a coincidence.
*/
static qboolean MP3_ValidFrameHeaderAt (const qbyte *h, const qbyte *end)
{
	int ver, layer, bitrate_idx, samplerate_idx;

	if (end - h < 4)
		return false;
	if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0)
		return false;

	ver            = (h[1] >> 3) & 3;
	layer          = (h[1] >> 1) & 3;
	bitrate_idx    = (h[2] >> 4) & 15;
	samplerate_idx = (h[2] >> 2) & 3;

	if (ver == 1)                   return false;   /* reserved MPEG version */
	if (layer != 1)                 return false;   /* 1 == Layer III */
	if (bitrate_idx == 0 ||
	    bitrate_idx == 15)          return false;   /* "free"/"bad" */
	if (samplerate_idx == 3)        return false;   /* reserved */
	return true;
}

static int MP3_FrameLength (const qbyte *h)
{	/* bits/sec table for Layer III, indexed [mpeg1?][bitrate_idx] */
	static const int br_v1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
	static const int br_v2[16] = {0, 8,16,24,32,40,48,56, 64, 80, 96,112,128,144,160,0};
	static const int sr_tab[4] = {44100,48000,32000,0};

	int ver            = (h[1] >> 3) & 3;   /* 3=MPEG1, 2=MPEG2, 0=MPEG2.5 */
	int bitrate_idx    = (h[2] >> 4) & 15;
	int samplerate_idx = (h[2] >> 2) & 3;
	int padding        = (h[2] >> 1) & 1;
	int bitrate, samplerate, spf;

	if (sr_tab[samplerate_idx] == 0)
		return 0;
	samplerate = sr_tab[samplerate_idx];
	if (ver == 2)      samplerate /= 2;     /* MPEG2   */
	else if (ver == 0) samplerate /= 4;     /* MPEG2.5 */

	bitrate = ((ver == 3) ? br_v1[bitrate_idx] : br_v2[bitrate_idx]) * 1000;
	if (!bitrate)
		return 0;

	spf = (ver == 3) ? 1152 : 576;          /* samples per Layer III frame */
	return (spf / 8) * bitrate / samplerate + padding;
}

static qboolean MP3_Sniff (const qbyte *data, size_t datalen, size_t *startofs)
{
	size_t ofs = 0;

	if (datalen < 4)
		return false;

	/* Reject known containers by magic BEFORE the sync scan.
	   S_LoadSoundWorker walks the plugin table backwards (snd_mem.c:113) and
	   S_RegisterSoundInputPlugin appends, so this loader is tried ahead of the
	   wav one -- a false positive here does not merely waste time, it steals
	   the file.  And the sync scan does produce them: over 1200 Sven Co-op wavs
	   two of them (shocktrooper/puh.wav, shocktrooper/kur.wav) contain raw PCM
	   that passes the two-consecutive-valid-headers test by chance.  Both start
	   "RIFF", so checking the container magic removes the whole class. */
	if (!strncmp((const char*)data, "RIFF", 4))	return false;	/* wav  */
	if (!strncmp((const char*)data, "OggS", 4))	return false;	/* ogg/opus */
	if (!strncmp((const char*)data, "fLaC", 4))	return false;	/* flac */
	if (!strncmp((const char*)data, "FORM", 4))	return false;	/* aiff */
	if (!strncmp((const char*)data, "RIFX", 4))	return false;	/* big-endian wav */
	if (!strncmp((const char*)data, ".snd", 4))	return false;	/* au   */

	if (datalen > 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3')
	{	/* ID3v2: 6-byte header then a syncsafe (7 bits per byte) size. */
		size_t tagsize = ((size_t)(data[6] & 0x7f) << 21) |
		                 ((size_t)(data[7] & 0x7f) << 14) |
		                 ((size_t)(data[8] & 0x7f) <<  7) |
		                 ((size_t)(data[9] & 0x7f)      );
		*startofs = 10 + tagsize;
		return (*startofs < datalen);   /* the tag alone is not a sound */
	}

	while (ofs + 4 <= datalen && ofs < MP3_SYNC_SEARCH_LIMIT)
	{
		if (MP3_ValidFrameHeaderAt(data + ofs, data + datalen))
		{
			int len = MP3_FrameLength(data + ofs);
			if (len >= 4)
			{
				if (ofs + len + 4 > datalen)
				{	/* single-frame file: accept, nothing to cross-check against */
					*startofs = ofs;
					return true;
				}
				if (MP3_ValidFrameHeaderAt(data + ofs + len, data + datalen))
				{
					*startofs = ofs;
					return true;
				}
			}
		}
		ofs++;
	}
	return false;
}

static qboolean QDECL S_LoadMP3Sound (sfx_t *s, qbyte *data, size_t datalen, int sndspeed, qboolean forcedecode)
{
	mp3dec_t         *dec;
	mp3dec_frame_info_t info;
	size_t            ofs = 0;
	short            *pcm = NULL;
	size_t            pcmsamples = 0;    /* frames written, per channel */
	size_t            pcmcapacity = 0;   /* frames the buffer can hold, per channel */
	int               channels = 0;
	int               hz = 0;
	short             framebuf[MINIMP3_MAX_SAMPLES_PER_FRAME];
	qboolean          ok;

	if (!MP3_Sniff(data, datalen, &ofs))
		return false;

	/* ~7KB of tables and history; too big for the stack on some of the worker
	   threads this runs on. */
	dec = Z_Malloc(sizeof(*dec));
	mp3dec_init(dec);

	while (ofs < datalen)
	{
		int samples = mp3dec_decode_frame(dec, data + ofs, (int)(datalen - ofs),
		                                  framebuf, &info);

		if (!info.frame_bytes)
			break;              /* out of data, or nothing decodable left */
		ofs += info.frame_bytes;

		if (!samples)
			continue;           /* header-only frame (Xing/Info tag, or resync) */

		if (!channels)
		{	/* first real frame decides the format for the whole file */
			channels = info.channels;
			hz       = info.hz;
			if (channels < 1 || channels > 2 || hz < 1)
			{
				Con_Printf(CON_WARNING"%s: unsupported mp3 format (%i channels, %ihz)\n",
				           s->name, info.channels, info.hz);
				break;
			}
		}
		else if (info.channels != channels || info.hz != hz)
		{	/* Mid-stream format changes are legal MP3 and a nightmare to mix.
			   Stop cleanly and keep what we have rather than interleaving
			   mismatched data into the same buffer. */
			Con_DPrintf("%s: mp3 format changes mid-stream, truncating\n", s->name);
			break;
		}

		if (pcmsamples + samples > pcmcapacity)
		{	/* Grow geometrically; seeded from a bitrate-free estimate so the
			   common case is one or two reallocs, not dozens. */
			size_t want = pcmcapacity ? pcmcapacity * 2 : (pcmsamples + samples) * 64;
			if (want < pcmsamples + samples)
				want = pcmsamples + samples;
			pcm = BZ_Realloc(pcm, want * channels * sizeof(short));
			if (!pcm)
			{
				Z_Free(dec);
				return false;
			}
			pcmcapacity = want;
		}

		memcpy(pcm + pcmsamples * channels, framebuf,
		       samples * channels * sizeof(short));
		pcmsamples += samples;
	}

	Z_Free(dec);

	if (!pcm || !pcmsamples)
	{
		if (pcm)
			BZ_Free(pcm);
		/* Deliberately NOT setting SLS_FAILED.  Returning false hands the
		   buffer to the next loader in the chain, and latching the failure
		   would stop that -- so a file we sniffed wrong would be dead instead
		   of merely mis-guessed.  Only a warning, and only at dprint level,
		   since the honest reading of "sniffed as mp3, decoded to nothing" is
		   that the sniff was wrong. */
		Con_DPrintf("%s: sniffed as mp3 but decoded no audio; trying other loaders\n", s->name);
		return false;
	}

	/* ResampleSfx copies into its own sfxcache_t (it is handed a pointer into
	   the caller's file buffer everywhere else), so our block is ours to free. */
	ok = ResampleSfx(s, hz, channels, QAF_S16, pcmsamples, -1, (qbyte*)pcm);
	BZ_Free(pcm);
	return ok;
}

/*
	Called from S_Init.  Late enough that the static AudioInputPlugins entries
	are in place, so mp3 lands in a free slot above them -- which is what we
	want, since "highest priority is last" and every other loader sniffs a
	format mp3 cannot be confused with anyway.
*/
void S_RegisterMP3Plugin(void)
{
	S_RegisterSoundInputPlugin(NULL, S_LoadMP3Sound);
}

#endif /* !SERVERONLY */
