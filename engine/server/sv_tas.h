/* FTESurf Patch 1055: movement-only TAS command admission. No human provenance.
 * Self-contained so tests compile the exact admission code used by sv_user.c. */
#ifndef SV_TAS_H
#define SV_TAS_H
#include <math.h>
#include <stdint.h>

#define SV_TAS_MAX_PENDING 128
#define SV_TAS_MAX_DONE 16000000
#define SV_TAS_TICK 0.015f
#define SV_TAS_EPSILON 1e-5f /* PMSRC_TICK_EPSILON; test guards the shared value. */

enum sv_tas_action {
	SV_TAS_NORMAL, SV_TAS_BLOCK, SV_TAS_STEP, SV_TAS_CANCEL
};

static int SV_TASClockSupported(int source, float tick, float gamespeed, float carry)
{
	return source && tick == SV_TAS_TICK &&
		gamespeed == 1.0f && isfinite(carry) && carry >= 0 &&
		carry < SV_TAS_TICK - SV_TAS_EPSILON;
}

/* QC's timeline arithmetic must still resolve each native tick. */
static int SV_TASRewindCount(double ticks)
{
	return ticks >= 0 && ticks < SV_TAS_MAX_DONE && ticks == floor(ticks);
}

/* P1061: only an explicitly owned rewind pin keeps the pause. Other holds
 * retain the original cancellation/escape policy. */
static int SV_TASRewindHold(int paused, int marked, int ownpin, int alive, int watch, int impulse)
{
	return paused && marked && ownpin && alive && watch && !impulse;
}

/* Natural command sequences are uint32, not decimal float counters. Only the
 * two exact 16-bit words cross QC. Ordinary play never uses this filter. */
static int SV_TASFresh(uint32_t sequence, uint32_t previous, int valid)
{
	uint32_t delta = sequence - previous;
	return !valid || (delta && delta < UINT32_C(0x80000000));
}

static enum sv_tas_action SV_TASPlan(int paused, int marked, int supported,
	int bodyfree, int decoded_qw, int fresh, int impulse, float pending, float done)
{
	if (!paused)
		return SV_TAS_NORMAL;
	if (!marked)
		return SV_TAS_BLOCK;
	if (!bodyfree)
		return SV_TAS_CANCEL;
	if (!decoded_qw || !fresh)
		return SV_TAS_BLOCK;
	/* Fresh decoded impulses still escape after a live clock change. Never
	 * reinterpret the transactional warp channel as a synthetic step. */
	if (impulse)
		return SV_TAS_CANCEL;
	if (!supported)
		return SV_TAS_BLOCK;
	if (!isfinite(pending) || pending < 0 || pending > SV_TAS_MAX_PENDING ||
		pending != floorf(pending) || !isfinite(done) || done < 0 ||
		done > SV_TAS_MAX_DONE || done != floorf(done) ||
		done + pending > SV_TAS_MAX_DONE)
		return SV_TAS_BLOCK;
	if (!pending || done >= SV_TAS_MAX_DONE)
		return SV_TAS_BLOCK;
	return SV_TAS_STEP;
}
#endif
