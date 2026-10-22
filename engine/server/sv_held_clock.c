#if defined(__GNUC__)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#endif
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "sv_held_clock.h"

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
	FLT_MAX_EXP == 128 && FLT_EVAL_METHOD == 0, "held clock requires binary32 evaluation");

static float Held_Min(float a, float b) { return a < b ? a : b; }
static float Held_Max(float a, float b) { return a > b ? a : b; }

static int Held_ClockFinite(const held_clock_t *clock)
{
	float words[36];
	size_t i;
	typedef char held_clock_has_36_words[(sizeof(*clock) == sizeof(words)) ? 1 : -1];
	(void)sizeof(held_clock_has_36_words);
	memcpy(words, clock, sizeof(words));
	for (i = 0; i < sizeof(words)/sizeof(words[0]); ++i)
		if (!isfinite(words[i]))
			return 0;
	return clock->flags >= 0 && clock->flags <= 1048575 &&
		clock->flags == floorf(clock->flags);
}

static int Held_ClockFail(held_clock_replay_t *replay)
{
	replay->failed = 1;
	return 0;
}

int SV_HeldClockBegin(held_clock_replay_t *replay, const held_clock_t *initial,
	const held_clock_context_t *context)
{
	/* This is the only state assignment from recorded physical-time state. */
	if (replay->initialized || replay->failed)
		return Held_ClockFail(replay);
	if (!Held_ClockFinite(initial) || initial->counted != 1 ||
		initial->state != context->runningstate || initial->starttick < 0 ||
		initial->freeze || initial->frzon || initial->tickcarry || initial->resumesec ||
		!isfinite(context->excuse) || !isfinite(context->slow) ||
		!isfinite(context->hyst) || !isfinite(context->allow) ||
		!isfinite(context->freecredits) || !isfinite(context->gap) ||
		context->hyst <= 0 || context->allow < 0 || context->freecredits < 0 ||
		context->gap <= 0 || context->frozenflag != 16 || context->slowflag != 524288 ||
		context->runningstate != 2)
		return Held_ClockFail(replay);
	replay->clock = *initial;
	replay->context = *context;
	replay->initialized = 1;
	replay->released = 0;
	replay->post_phase = 0;
	replay->view_policy = replay->view_owner = 0;
	replay->view_seconds = replay->view_wall = replay->view_rate = 0;
	return 1;
}

float SV_HeldClockViewSeconds(const held_clock_replay_t *replay)
{
	float seconds;
	if (!replay || !replay->initialized || replay->failed) return NAN;
	if (!replay->view_policy) return 0;
	seconds = replay->view_seconds;
	if (replay->view_owner)
		seconds = seconds + Held_Max(0,replay->clock.frame - replay->view_wall);
	return isfinite(seconds) && seconds >= 0 ? seconds:NAN;
}

float SV_HeldClockSeconds(const held_clock_replay_t *replay)
{
	float seconds = SV_HeldClockViewSeconds(replay);
	return replay && isfinite(seconds) ? replay->clock.elapsed + seconds:NAN;
}

int SV_HeldClockViewTicks(const held_clock_replay_t *replay, float *ticks)
{
	float seconds = SV_HeldClockViewSeconds(replay), value;
	if (!ticks || !isfinite(seconds)) return 0;
	if (!replay->view_policy) {*ticks = 0; return 1;}
	if (!isfinite(replay->view_rate) || replay->view_rate < .001f || replay->view_rate > .05f) return 0;
	value = seconds / replay->view_rate;
	value = floorf(value + .5f);
	if (!isfinite(value) || value < 0 || value > 16777216) return 0;
	*ticks = value;
	return 1;
}

int SV_HeldClockFrame(held_clock_replay_t *replay, float time, float frametime)
{
	float end = time + frametime;
	if (!replay->initialized || replay->failed || replay->post_phase || !isfinite(time) || !isfinite(frametime) ||
		!isfinite(end) || frametime < 0)
		return Held_ClockFail(replay);
	replay->clock.frame = end;
	return 1;
}

int SV_HeldClockRequest(held_clock_replay_t *replay, int freeze)
{
	if (!replay->initialized || replay->failed || replay->post_phase || replay->released ||
		(freeze != 0 && freeze != 1) || replay->clock.freeze == freeze)
		return Held_ClockFail(replay);
	if (replay->view_policy)
	{
		if (freeze)
		{
			if (replay->view_owner) return Held_ClockFail(replay);
			replay->view_wall = replay->clock.frame;
			replay->view_owner = 1;
		}
		else
		{
			if (!replay->view_owner) return Held_ClockFail(replay);
			replay->view_seconds = SV_HeldClockViewSeconds(replay);
			if (!isfinite(replay->view_seconds)) return Held_ClockFail(replay);
			replay->view_owner = 0;
		}
	}
	replay->clock.freeze = (float)freeze;
	if (freeze)
		replay->clock.flags = (float)((uint32_t)replay->clock.flags | 1u); /* SV_TimerPractice */
	if (!freeze)
		replay->released = 1;
	return 1;
}

static void Held_ClockMonitor(held_clock_replay_t *replay, float time, float dg)
{
	held_clock_t *c = &replay->clock;
	const held_clock_context_t *p = &replay->context;
	float dw, s, credit, framegap;
	dw = time - c->ckwall;
	c->ckwall = time;
	if (dw < 0)
		dw = 0;
	framegap = time - c->frame;
	if (p->excuse > 0)
		if (framegap >= p->excuse)
			dw = dw - Held_Min(dw, framegap);
	if (c->state != p->runningstate || c->frzon || !c->counted)
		return;
	/* aircell1 excludes run_pmhold, spectate and multi-session before admission. */
	if (dg <= 0)
		if (time < c->ckskip)
			return;
	c->ckskip = 0;
	s = dw - dg;
	c->ckwt = c->ckwt + dw;
	c->ckcb = Held_Min(p->freecredits, c->ckcb + dw / 60);
	if (dw >= p->gap)
	{
		c->ckgaps = c->ckgaps + 1;
		if (s >= p->hyst)
			if (c->ckcb >= 1)
			{
				credit = Held_Min(s, p->allow);
				s = s - credit;
				c->ckcr = c->ckcr + credit;
				c->ckncr = c->ckncr + 1;
				c->ckcb = c->ckcb - 1;
			}
	}
	c->ckd = c->ckd + s;
	if (c->ckup)
	{
		if (c->ckd > c->ckpk)
			c->ckpk = c->ckd;
		else if (c->ckd < c->ckpk - p->hyst)
		{
			c->cku = c->cku + Held_Max(0, c->ckpk - c->cktr - p->hyst);
			c->cktr = c->ckd;
			c->ckup = 0;
		}
	}
	else if (c->ckd < c->cktr)
		c->cktr = c->ckd;
	else if (c->ckd > c->cktr + p->hyst)
	{
		c->ckup = 1;
		c->ckpk = c->ckd;
	}
}

int SV_HeldClockNonce(held_clock_replay_t *replay)
{
	uint32_t flags;
	if (!replay->initialized || replay->failed || replay->post_phase ||
		replay->clock.state != replay->context.runningstate)
		return Held_ClockFail(replay);
	flags = (uint32_t)replay->clock.flags | 131072u;
	if ((double)(float)flags != (double)flags) return Held_ClockFail(replay);
	replay->clock.flags = (float)flags;
	return 1;
}

int SV_HeldClockAccounting(held_clock_replay_t *replay, float time, float movetick,
	float movetickrate, float movecarry)
{
	held_clock_t *c = &replay->clock;
	float now;
	uint32_t flags;
	if (!replay->initialized || replay->failed || replay->post_phase || !isfinite(time) ||
		!isfinite(movetick) || !isfinite(movetickrate) || !isfinite(movecarry) ||
		movetick < c->lastmt || movetick != floorf(movetick) ||
		movetick > 16777216 || movetickrate < .001f || movetickrate > .05f ||
		movecarry < 0 || movecarry >= movetickrate || c->state != replay->context.runningstate)
		return Held_ClockFail(replay);
	if (replay->view_rate && replay->view_rate != movetickrate) return Held_ClockFail(replay);
	replay->view_rate = movetickrate; /* Native accounting input, never a sampled origin. */
	/* SV_ClockSec, then SV_TimerFreezeFrame: do not reassociate these operations. */
	now = movetick * movetickrate;
	now = now + movecarry;
	Held_ClockMonitor(replay, time, now - c->lastsec);
	if (c->frzon && c->starttick >= 0)
	{
		c->frzticks = c->frzticks + (movetick - c->lastmt);
		c->frzsec = c->frzsec + (now - c->lastsec);
	}
	c->lastmt = movetick;
	c->lastsec = now;
	flags = (uint32_t)c->flags;
	if (c->freeze)
	{
		if (!c->frzon)
		{
			c->frzon = 1;
			c->frzat = now - c->startsec;
			c->frzat = c->frzat - c->frzsec;
			c->frzmt = movetick;
			c->frzcarry = movecarry;
		}
		flags |= replay->context.frozenflag;
	}
	else
	{
		c->frzon = 0;
		c->frzat = 0;
		flags &= ~replay->context.frozenflag;
	}
	c->flags = (float)flags;
	c->elapsed = now - c->startsec;
	c->elapsed = c->elapsed - c->frzsec;
	if (c->frzon)
		c->elapsed = c->frzat;
	if (!Held_ClockFinite(c))
		return Held_ClockFail(replay);
	replay->post_phase = 1;
	return 1;
}

int SV_HeldClockPacket(held_clock_replay_t *replay)
{
	if (!replay->initialized || replay->failed || replay->post_phase != 1)
		return Held_ClockFail(replay);
	replay->clock.packet = replay->clock.packet + 1;
	replay->post_phase = 2;
	return 1;
}

static int Held_ClockTicks(held_clock_replay_t *replay)
{
	held_clock_t *c = &replay->clock;
	float viewing;
	if (!SV_HeldClockViewTicks(replay,&viewing)) return Held_ClockFail(replay);
	c->ticks = c->lastmt - c->starttick;
	c->ticks = c->ticks + c->tickcarry;
	c->ticks = c->ticks - c->frzticks;
	c->ticks = c->ticks + viewing;
	if (!isfinite(c->ticks) || c->ticks < 0)
		return Held_ClockFail(replay);
	return 1;
}

int SV_HeldClockRecordFrame(held_clock_replay_t *replay)
{
	if (!replay->initialized || replay->failed || replay->post_phase != 2 ||
		replay->clock.state != replay->context.runningstate)
		return Held_ClockFail(replay);
	if (!Held_ClockTicks(replay))
		return 0;
	replay->post_phase = 3;
	return 1;
}

int SV_HeldClockLastOrigin(held_clock_replay_t *replay, const float origin[3])
{
	if (!replay->initialized || replay->failed ||
		(replay->post_phase != 3 && replay->post_phase != 4) ||
		!isfinite(origin[0]) || !isfinite(origin[1]) || !isfinite(origin[2]))
		return Held_ClockFail(replay);
	memcpy(replay->clock.lastorg, origin, sizeof(replay->clock.lastorg));
	replay->post_phase = 0;
	return 1;
}

int SV_HeldClockFinish(held_clock_replay_t *replay)
{
	held_clock_t *c = &replay->clock;
	float shortfall;
	if (!replay->initialized || replay->failed || !replay->released || replay->post_phase != 2 ||
		c->freeze || c->frzon || c->state != replay->context.runningstate)
		return Held_ClockFail(replay);
	/* SV_TimerFinish sets state/ticks before Journal/Ruleset/Profile/SlowClock. */
	c->state = 3;
	if (!Held_ClockTicks(replay))
		return 0;
	shortfall = c->cku;
	if (c->ckup)
		shortfall = shortfall + Held_Max(0, c->ckpk - c->cktr - replay->context.hyst);
	if (replay->context.slow > 0)
		if (shortfall >= replay->context.slow)
			c->flags = (float)((uint32_t)c->flags | replay->context.slowflag);
	replay->post_phase = 4;
	return 1;
}

int SV_HeldClockCompare(const held_clock_replay_t *replay, const held_clock_t *observation)
{
	return replay->initialized && !replay->failed && Held_ClockFinite(observation) &&
		!memcmp(&replay->clock, observation, sizeof(*observation));
}
