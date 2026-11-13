/* Independently authored from behavioral specifications, not SDK implementation.
 * Copyright (C) 2026 FTESurf contributors. SPDX-License-Identifier: GPL-2.0-or-later */
#include "quakedef.h"
#include "pm_thread.h"
#include "pm_ag.h"

static float PMAg_Duration(void) { return pmove.cmd.msec * 0.001; }

static void PMAg_Hull(void)
{
	VectorSet(pmove.player_mins, -16, -16, pmove.ducked ? -18 : -36);
	VectorSet(pmove.player_maxs, 16, 16, pmove.ducked ? 18 : 36);
	pmove.capsule = false;
}

static void PMAg_Posture(void)
{
	float fraction;
	PMAg_Hull();
	if (pmove.ducked) pmove.viewheight = 12;
	else if (pmove.ducking)
	{
		fraction = bound(0, (1000 - pmove.ducktime) / 400, 1);
		fraction = fraction * fraction * (3 - 2 * fraction);
		pmove.viewheight = 28 - 34 * fraction;
	}
	else pmove.viewheight = 28;
}

static trace_t PMAg_Trace(vec3_t start, vec3_t end)
{
	return PM_PlayerTrace(start, end, MASK_PLAYERSOLID);
}

static trace_t PMAg_PointTrace(vec3_t start, vec3_t end)
{
	vec3_t mins, maxs;
	trace_t trace;
	VectorCopy(pmove.player_mins, mins); VectorCopy(pmove.player_maxs, maxs);
	VectorClear(pmove.player_mins); VectorClear(pmove.player_maxs);
	trace = PMAg_Trace(start, end);
	VectorCopy(mins, pmove.player_mins); VectorCopy(maxs, pmove.player_maxs);
	return trace;
}

static void PMAg_Water(void)
{
	vec3_t sample;
	unsigned int contents;
	pmove.waterlevel = 0; pmove.watertype = FTECONTENTS_EMPTY;
	VectorCopy(pmove.origin, sample);
	sample[2] += pmove.player_mins[2] + 1;
	contents = PM_PointContents(sample);
	if (!(contents & (FTECONTENTS_WATER|FTECONTENTS_SLIME|FTECONTENTS_LAVA))) return;
	pmove.watertype = contents; pmove.waterlevel = 1;
	sample[2] = pmove.origin[2];
	if (!(PM_PointContents(sample) & (FTECONTENTS_WATER|FTECONTENTS_SLIME|FTECONTENTS_LAVA))) return;
	pmove.waterlevel = 2; sample[2] = pmove.origin[2] + pmove.viewheight;
	if (PM_PointContents(sample) & (FTECONTENTS_WATER|FTECONTENTS_SLIME|FTECONTENTS_LAVA)) pmove.waterlevel = 3;
}

static void PMAg_Categorize(void)
{
	vec3_t down;
	trace_t trace;
	PMAg_Water(); pmove.onground = false; pmove.groundent = -1;
	if (pmove.velocity[2] > 180) return;
	VectorCopy(pmove.origin, down); down[2] -= 2;
	trace = PMAg_Trace(pmove.origin, down);
	if (trace.fraction == 1 || trace.plane.normal[2] < 0.7f) return;
	pmove.onground = true; pmove.groundent = trace.entnum;
	pmove.waterjumpremaining = pmove.waterjumptime = 0;
	PM_AddTouchedEnt(trace.entnum);
	if (pmove.waterlevel < 2 && !trace.startsolid && !trace.allsolid)
		VectorCopy(trace.endpos, pmove.origin);
}

static qboolean PMAg_Clear(vec3_t origin)
{
	trace_t trace = PMAg_Trace(origin, origin);
	return !trace.startsolid && !trace.allsolid;
}

static void PMAg_Stand(void)
{
	vec3_t candidate;
	qboolean wasducked = pmove.ducked;
	VectorCopy(pmove.origin, candidate);
	if (pmove.onground) candidate[2] += 18;
	if (!PMAg_Clear(candidate)) return;
	pmove.ducked = false; PMAg_Hull();
	if (!PMAg_Clear(candidate)) { pmove.ducked = wasducked; PMAg_Hull(); return; }
	VectorCopy(candidate, pmove.origin);
	pmove.ducking = false; pmove.ducktime = 0; pmove.viewheight = 28;
	PMAg_Categorize();
}

static void PMAg_Duck(void)
{
	qboolean held = (pmove.cmd.buttons & BUTTON_DUCK) != 0;
	qboolean previous = (pmove.oldbuttons & BUTTON_DUCK) != 0;
	float fraction;
	int lift;
	vec3_t shifted;

	if (pmove.pm_type == PM_DEAD) { if (pmove.ducked) PMAg_Stand(); return; }
	if (!held) { if (pmove.ducked || pmove.ducking) PMAg_Stand(); return; }
	if (!previous && !pmove.ducked) { pmove.ducktime = 1000; pmove.ducking = true; }
	if (!pmove.ducking) return;
	if (pmove.ducktime <= 600 || !pmove.onground)
	{
		pmove.ducked = true; pmove.ducking = false; pmove.viewheight = 12;
		if (pmove.onground) pmove.origin[2] -= 18;
		PMAg_Hull();
		if (pmove.onground)
		{
			VectorCopy(pmove.origin, shifted);
			for (lift = 0; lift <= 36; lift++)
			{
				pmove.origin[2] = shifted[2] + lift;
				if (PMAg_Clear(pmove.origin)) break;
			}
			if (lift > 36) VectorCopy(shifted, pmove.origin);
			PMAg_Categorize();
		}
	}
	else
	{
		fraction = (1000 - pmove.ducktime) / 400;
		fraction = fraction * fraction * (3 - 2 * fraction);
		pmove.viewheight = 28 - 34 * fraction;
	}
}

static void PMAg_Clip(const vec3_t velocity, const vec3_t normal, vec3_t out)
{
	float into = DotProduct(velocity, normal);
	int axis;
	for (axis = 0; axis < 3; axis++)
	{
		out[axis] = velocity[axis] - normal[axis] * into;
		if (out[axis] > -0.1f && out[axis] < 0.1f) out[axis] = 0;
	}
}

static qboolean PMAg_Slide(void)
{
	vec3_t initial, original, end, planes[5], crease;
	float remaining = PMAg_Duration();
	int bump, count = 0, i, j;
	qboolean blocked = false;
	trace_t trace;
	VectorCopy(pmove.velocity, initial); VectorCopy(initial, original);
	for (bump = 0; bump < 4; bump++)
	{
		VectorMA(pmove.origin, remaining, pmove.velocity, end);
		trace = PMAg_Trace(pmove.origin, end);
		if (trace.allsolid || trace.startsolid) { VectorClear(pmove.velocity); return true; }
		if (trace.fraction > 0) { VectorCopy(trace.endpos, pmove.origin); VectorCopy(pmove.velocity, original); count = 0; }
		if (trace.fraction == 1) break;
		blocked = true; PM_AddTouchedEnt(trace.entnum);
		remaining *= 1 - trace.fraction;
		if (count == 5) { VectorClear(pmove.velocity); break; }
		VectorCopy(trace.plane.normal, planes[count]); count++;
		for (i = 0; i < count; i++)
		{
			PMAg_Clip(original, planes[i], pmove.velocity);
			for (j = 0; j < count; j++) if (j != i && DotProduct(pmove.velocity, planes[j]) < 0) break;
			if (j == count) break;
		}
		if (i == count)
		{
			if (count != 2) { VectorClear(pmove.velocity); break; }
			CrossProduct(planes[0], planes[1], crease);
			VectorScale(crease, DotProduct(crease, pmove.velocity), pmove.velocity);
		}
		if (DotProduct(pmove.velocity, initial) <= 0) { VectorClear(pmove.velocity); break; }
	}
	return blocked;
}

static void PMAg_Step(void)
{
	vec3_t start, initial, low, lowvelocity, raised;
	float lowdistance, highdistance;
	trace_t trace;
	VectorCopy(pmove.origin, start); VectorCopy(pmove.velocity, initial);
	if (!PMAg_Slide()) return;
	VectorCopy(pmove.origin, low); VectorCopy(pmove.velocity, lowvelocity);
	VectorCopy(start, raised); raised[2] += movevars.stepheight;
	trace = PMAg_Trace(start, raised);
	if (!trace.startsolid && !trace.allsolid)
	{
		VectorCopy(trace.endpos, pmove.origin); VectorCopy(initial, pmove.velocity);
		PMAg_Slide(); VectorCopy(pmove.origin, raised); raised[2] -= movevars.stepheight;
		trace = PMAg_Trace(pmove.origin, raised);
		if (!trace.startsolid && !trace.allsolid && trace.plane.normal[2] >= 0.7f)
		{
			VectorCopy(trace.endpos, pmove.origin);
			lowdistance = (low[0]-start[0])*(low[0]-start[0]) + (low[1]-start[1])*(low[1]-start[1]);
			highdistance = (pmove.origin[0]-start[0])*(pmove.origin[0]-start[0]) + (pmove.origin[1]-start[1])*(pmove.origin[1]-start[1]);
			if (highdistance >= lowdistance) { pmove.velocity[2] = lowvelocity[2]; return; }
		}
	}
	VectorCopy(low, pmove.origin); VectorCopy(lowvelocity, pmove.velocity);
}

static qboolean PMAg_Edge(void)
{
	vec3_t start, end;
	float speed = sqrtf(DotProduct(pmove.velocity, pmove.velocity));
	trace_t trace;
	if (!speed) return false;
	VectorMA(pmove.origin, 16 / speed, pmove.velocity, start);
	start[2] += pmove.player_mins[2]; VectorCopy(start, end); end[2] -= 34;
	// The selected historical AG profile probes with its current player hull.
	trace = PMAg_Trace(start, end);
	return trace.fraction == 1;
}

static float PMAg_Wish(vec3_t direction, qboolean swimming, float forwardmove, float sidemove, float upmove)
{
	vec3_t forward, right, up;
	float speed;
	int axis;
	AngleVectors(pmove.angles, forward, right, up);
	if (!swimming) { forward[2] = right[2] = 0; VectorNormalize(forward); VectorNormalize(right); }
	for (axis = 0; axis < 3; axis++) direction[axis] = forward[axis]*forwardmove + right[axis]*sidemove;
	if (swimming)
	{
		direction[2] += upmove;
		if (!forwardmove && !sidemove && !upmove) direction[2] -= 60;
	}
	speed = VectorNormalize(direction);
	if (speed > movevars.maxspeed) speed = movevars.maxspeed;
	return swimming ? speed * 0.8f : speed;
}

static void PMAg_CheckWaterJump(void)
{
	vec3_t forward, start, end, horizontal;
	trace_t lower, upper;
	if (pmove.waterlevel != 2 || pmove.waterjumpremaining || pmove.velocity[2] < -180) return;
	AngleVectors(pmove.angles, forward, NULL, NULL); forward[2] = 0; VectorNormalize(forward);
	VectorCopy(pmove.velocity, horizontal); horizontal[2] = 0;
	if (DotProduct(horizontal, forward) < 0) return;
	VectorCopy(pmove.origin, start); start[2] += 8; VectorMA(start, 24, forward, end);
	lower = PMAg_PointTrace(start, end);
	if (lower.fraction == 1 || fabsf(lower.plane.normal[2]) >= 0.1f) return;
	start[2] = pmove.origin[2] + pmove.player_maxs[2]; VectorMA(start, 24, forward, end);
	upper = PMAg_PointTrace(start, end);
	if (upper.startsolid || upper.allsolid || upper.fraction != 1) return;
	pmove.waterjumpremaining = pmove.waterjumptime = 2; pmove.agwaterjump = true;
	pmove.waterjumpdir[0] = -50 * lower.plane.normal[0]; pmove.waterjumpdir[1] = -50 * lower.plane.normal[1];
	pmove.velocity[2] = 225; pmove.jump_held = true;
}

static void PMAg_Swim(float forwardmove, float sidemove, float upmove)
{
	vec3_t direction, destination, above;
	float wishspeed, speed, remaining, budget, dt = PMAg_Duration();
	trace_t trace;
	if (pmove.cmd.buttons & BUTTON_JUMP)
	{
		if (pmove.waterjumpremaining > 0) pmove.waterjumpremaining -= dt;
		else pmove.velocity[2] = (pmove.watertype & FTECONTENTS_LAVA) ? 50 : (pmove.watertype & FTECONTENTS_SLIME) ? 80 : 100;
		pmove.jump_held = true;
	}
	else pmove.jump_held = false;
	wishspeed = PMAg_Wish(direction, true, forwardmove, sidemove, upmove);
	speed = sqrtf(DotProduct(pmove.velocity, pmove.velocity));
	remaining = max(0, speed - speed * movevars.friction * pmove.surfacefriction * dt);
	if (speed) VectorScale(pmove.velocity, remaining / speed, pmove.velocity);
	budget = movevars.accelerate * wishspeed * dt * pmove.surfacefriction;
	if (budget > wishspeed - remaining) budget = wishspeed - remaining;
	if (budget > 0) VectorMA(pmove.velocity, budget, direction, pmove.velocity);
	VectorAdd(pmove.velocity, pmove.basevelocity, pmove.velocity);
	VectorMA(pmove.origin, dt, pmove.velocity, destination); VectorCopy(destination, above); above[2] += movevars.stepheight + 1;
	trace = PMAg_Trace(above, destination);
	if (!trace.startsolid && !trace.allsolid) VectorCopy(trace.endpos, pmove.origin);
	else PMAg_Slide();
	VectorSubtract(pmove.velocity, pmove.basevelocity, pmove.velocity);
}

/* Ladder membership comes from intersected ladder contents, never a plain wall. */
static qboolean PMAg_Ladder(void)
{
	vec3_t forward, right, up, normal, side, climb, intent, end, below;
	trace_t contact;
	float speed, into;
	qboolean floor;
	if (!(pmove.onladder || ((PM_PointContents(pmove.origin)|PM_ExtraBoxContents(pmove.origin)) & FTECONTENTS_LADDER)) || pmove.pm_type != PM_NORMAL) { pmove.srcladder = false; return false; }
	AngleVectors(pmove.angles, forward, right, up);
	VectorMA(pmove.origin, 24, forward, end);
	contact = PM_PlayerTrace(pmove.origin, end, MASK_PLAYERSOLID|FTECONTENTS_LADDER);
	if (contact.fraction == 1 || fabsf(contact.plane.normal[2]) >= 0.999f) return false;
	VectorCopy(contact.plane.normal, normal);
	pmove.srcladder = true; VectorCopy(normal, pmove.srcladdernormal);
	if (pmove.cmd.buttons & BUTTON_JUMP) { VectorScale(normal, 270, pmove.velocity); pmove.srcladder = pmove.onladder = false; pmove.onground = false; pmove.jump_held = true; PMAg_Slide(); return true; }
	speed = min(200, movevars.maxspeed); VectorClear(intent);
	if (pmove.cmd.forwardmove > 0) VectorMA(intent, speed, forward, intent);
	if (pmove.cmd.forwardmove < 0) VectorMA(intent, -speed, forward, intent);
	if (pmove.cmd.sidemove < 0) VectorMA(intent, -speed, right, intent);
	if (pmove.cmd.sidemove > 0) VectorMA(intent, speed, right, intent);
	VectorSet(up, 0, 0, 1); CrossProduct(up, normal, side); VectorNormalize(side); CrossProduct(normal, side, climb);
	into = DotProduct(intent, normal);
	VectorMA(intent, -into, normal, pmove.velocity);
	VectorMA(pmove.velocity, -into, climb, pmove.velocity);
	VectorCopy(pmove.origin, below); below[2] += pmove.player_mins[2] - 1;
	floor = (PM_PointContents(below) & FTECONTENTS_SOLID) != 0;
	if (floor && into > 0) VectorMA(pmove.velocity, 200, normal, pmove.velocity);
	pmove.onground = false; PMAg_Slide(); return true;
}

static void PMAg_Tick(void)
{
	vec3_t direction, current, originalbase;
	float wishspeed, factor, forwardmove, sidemove, upmove;
	unsigned int contents;
	int axis;
	qboolean waswaterjump;
	usercmd_t command = pmove.cmd;
	/* GoldSrc v1 has no Source surface-material friction state. A fresh
	 * client carries zeroed Source storage, which must not disable acceleration. */
	pmove.surfacefriction = 1;
	VectorSet(pmove.gravitydir, 0, 0, -1); PMAg_Posture();
	pmove.ducktime = max(0, pmove.ducktime - pmove.cmd.msec);
	factor = pmove.ducked ? 0.333f : 1;
	forwardmove = command.forwardmove * factor; sidemove = command.sidemove * factor; upmove = command.upmove * factor;
	PMAg_Categorize(); PMAg_Duck();
	VectorCopy(pmove.basevelocity, originalbase); VectorClear(current);
	contents = pmove.watertype;
	if (contents & Q2CONTENTS_CURRENT_0) current[0] += 1;
	if (contents & Q2CONTENTS_CURRENT_90) current[1] += 1;
	if (contents & Q2CONTENTS_CURRENT_180) current[0] -= 1;
	if (contents & Q2CONTENTS_CURRENT_270) current[1] -= 1;
	if (contents & Q2CONTENTS_CURRENT_UP) current[2] += 1;
	if (contents & Q2CONTENTS_CURRENT_DOWN) current[2] -= 1;
	VectorMA(pmove.basevelocity, 50 * pmove.waterlevel, current, pmove.basevelocity);
	pmove.waterjumptime = pmove.waterjumpremaining;
	if (pmove.pm_type == PM_SPECTATOR || pmove.pm_type == PM_OLD_SPECTATOR)
	{
		wishspeed = PMAg_Wish(direction, true, forwardmove, sidemove, upmove) / 0.8f;
		VectorScale(direction, wishspeed, pmove.velocity); VectorMA(pmove.origin, PMAg_Duration(), pmove.velocity, pmove.origin);
	}
	else if (pmove.pm_type != PM_NONE && pmove.pm_type != PM_FREEZE && !PMAg_Ladder())
	{
		waswaterjump = pmove.agwaterjump;
		if (waswaterjump)
		{
			pmove.waterjumpremaining -= PMAg_Duration();
			if (pmove.waterjumpremaining < 0 || !pmove.waterlevel) { pmove.waterjumpremaining = 0; pmove.agwaterjump = false; }
			pmove.velocity[0] = pmove.waterjumpdir[0]; pmove.velocity[1] = pmove.waterjumpdir[1]; PMAg_Slide();
		}
		else if (pmove.waterlevel >= 2) { PMAg_CheckWaterJump(); PMAg_Swim(forwardmove, sidemove, upmove); }
		else
		{
			pmove.velocity[2] += pmove.basevelocity[2] * PMAg_Duration();
			pmove.basevelocity[2] = originalbase[2] = 0;
			PMAg_DryStart(&pmove, &movevars, pmove.surfacefriction, PMAg_Edge());
			wishspeed = PMAg_Wish(direction, false, forwardmove, sidemove, upmove);
			PMAg_Accelerate(&pmove, &movevars, direction, wishspeed, pmove.surfacefriction);
			/* Vertical carrier is command-local and is never accumulated in state. */
			VectorAdd(pmove.velocity, pmove.basevelocity, pmove.velocity);
			if (pmove.onground && sqrtf(DotProduct(pmove.velocity, pmove.velocity)) < 1) VectorClear(pmove.velocity);
			if (pmove.onground) PMAg_Step(); else PMAg_Slide();
			VectorSubtract(pmove.velocity, pmove.basevelocity, pmove.velocity);
			PMAg_Categorize(); PMAg_DryFinish(&pmove, &movevars);
		}
		PMAg_Categorize();
	}
	for (axis = 0; axis < 3; axis++)
	{
		if (!isfinite(pmove.velocity[axis])) pmove.velocity[axis] = 0;
		if (movevars.maxvelocity > 0) pmove.velocity[axis] = bound(-movevars.maxvelocity, pmove.velocity[axis], movevars.maxvelocity);
	}
	pmove.waterjumptime = pmove.waterjumpremaining;
	pmove.oldbuttons = command.buttons;
	VectorCopy(originalbase, pmove.basevelocity); pmove.cmd = command;
}

void PMAg_PlayerMove(float gamespeed)
{
	usercmd_t command = pmove.cmd;
	float tick = movevars.ticrate;
	float available = pmove.msec_carry + command.msec * 0.001f * gamespeed;
	unsigned int count = 0;
	pmove.ticksrun = 0; pmove.tickused = 0;
	if (!PMAg_ContextValid(&movevars)) return; // explicit refusal, never Source or a rate fallback
	PMAg_Posture(); // loaded state owns outputs even when this command executes no tick
	if (available + 0.0000001f < tick) PMAg_Water(); // point outputs only: no contact motion or timers
	/* Profile rates have integral command milliseconds. Retain only sub-tick
	 * carry; a stalled command may never credit more than eight executed ticks. */
	pmove.cmd.msec = (int)(tick * 1000 + 0.5f);
	while (available + 0.0000001f >= tick)
	{
		if (count == 8) { available = 0; break; }
		PMAg_Tick();
		available -= tick;
		count++;
	}
	pmove.msec_carry = max(0, available);
	pmove.ticksrun = count;
	pmove.tickused = tick;
	pmove.cmd = command;
}
