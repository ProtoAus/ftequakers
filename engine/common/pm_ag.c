/*
 * Independently implemented AG dry-motion primitives.
 * Copyright (C) 2026 FTESurf contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "quakedef.h"
#include "pm_ag.h"

static const pmprofile_t pm_profiles[] =
{
	{"ag-100",		"ag-100-v1",	PMPROFILE_AG,		100,	0.01f,	1542},
	{"ag-250",		"ag-250-v1",	PMPROFILE_AG,		250,	0.004f,	1542},
	{"ag-1000",		"ag-1000-v1",	PMPROFILE_AG,		1000,	0.001f,	1589},
	{"cs16",		"cs16-v1",		PMPROFILE_CS16,		100,	0.01f,	1589},
	{"hl2-pre",		"hl2-pre-v1",	PMPROFILE_HL2PRE,	0,		0.015f,	1589},
	{"hl2-post",	"hl2-post-v1",	PMPROFILE_HL2POST,	0,		0.015f,	1589},
};
const pmprofile_t *PM_ProfileByIdentity(const char *identity)
{
	size_t i;
	if (identity)
		for (i = 0; i < countof(pm_profiles); i++)
			if (!strcmp(identity, pm_profiles[i].identity)) return &pm_profiles[i];
	return NULL;
}
const pmprofile_t *PM_ProfileBySelector(const char *selector)
{
	size_t i;
	if (selector)
		for (i = 0; i < countof(pm_profiles); i++)
			if (!strcmp(selector, pm_profiles[i].selector)) return &pm_profiles[i];
	return NULL;
}
int PMAg_TickBudget(float tick)
{
	return tick == 0.001f ? 32 : 8;
}

int PMAg_ProfileRate(const char *identity, int version, float tick)
{
	const pmprofile_t *p = PM_ProfileByIdentity(identity);
	if (version != PMAG_VERSION || !p || !PM_ProfileGoldSrc(p->kind) || tick != p->tick) return 0;
	return p->rate;
}
// profilekind 0 is a bare harness context (tools/test_pm_ag_world.c): plain AG rules.
qboolean PMAg_ContextValid(const movevars_t *vars)
{
	size_t i;
	if (vars->physicsmode != PHYSMODE_AG || vars->sourceversion != PMAG_VERSION) return false;
	if (vars->profilekind && !PM_ProfileGoldSrc(vars->profilekind)) return false;
	for (i = 0; i < countof(pm_profiles); i++)
		if (PM_ProfileGoldSrc(pm_profiles[i].kind) && vars->agprofile == pm_profiles[i].rate &&
			vars->ticrate == pm_profiles[i].tick &&
			(!vars->profilekind || vars->profilekind == pm_profiles[i].kind))
			return true;
	return false;
}
double PMAg_CommandSlot(float tick, double wantedfps, double serverfps)
{
	double tickms = floor(tick * 1000.0 + 0.5), minimum;
	int n, budget = PMAg_TickBudget(tick);
	if ((tick != 0.01f && tick != 0.004f && tick != 0.001f) || !(wantedfps >= 0)) return 0;
	n = wantedfps > 0 ? (int)floor(1000.0 / wantedfps / tickms + 0.5) : 1;
	n = bound(1, n, budget);
	minimum = serverfps > 0 ? 1000.0 / serverfps : 0;
	while (n * tickms < minimum && n <= budget) n++;
	return n <= budget ? n * tickms : 0;
}

/* The caller supplies a normalized horizontal direction. The cap applies to
 * the target projection, NOT to the wishspeed in the acceleration product. */
void PMAg_Accelerate(playermove_t *move, const movevars_t *vars,
	const vec3_t wishdir, float wishspeed, float surfacefriction)
{
	float target, projected, remaining, budget;
	float dt = move->cmd.msec * 0.001;
	int axis;

	if (move->pm_type == PM_DEAD || move->waterjumptime)
		return;
	if (wishspeed > vars->maxspeed)
		wishspeed = vars->maxspeed;
	target = wishspeed;
	if (!move->onground && target > 30.0f)
		target = 30.0f;
	projected = DotProduct(move->velocity, wishdir);
	remaining = target - projected;
	if (remaining <= 0)
		return;
	budget = (move->onground ? vars->accelerate : vars->airaccelerate)
		* wishspeed * dt * surfacefriction;
	if (budget > remaining)
		budget = remaining;
	for (axis = 0; axis < 3; axis++)
		move->velocity[axis] += budget * wishdir[axis];
}

/* Even an airborne press consumes the edge; pm_autobunny lets a held button jump on landing (Patch 1591).
 * This is only the button decision; the dry jump/gravity phases own impulse. */
qboolean PMAg_JumpEdge(playermove_t *move, const movevars_t *vars)
{
	qboolean pressed = (move->cmd.buttons & BUTTON_JUMP) != 0;
	qboolean fresh = pressed && (!move->jump_held || vars->autobunny);

	move->jump_held = pressed;
	return fresh && move->onground && move->pm_type != PM_DEAD;
}

void PMAg_Friction(playermove_t *move, const movevars_t *vars,
	float surfacefriction, qboolean edge)
{
	float speed = sqrtf(DotProduct(move->velocity, move->velocity));
	float control, remaining, scale;
	float dt = move->cmd.msec * 0.001;
	int axis;

	if (speed == 0 || move->waterjumptime)
		return;
	control = speed < vars->stopspeed ? vars->stopspeed : speed;
	remaining = speed - control * vars->friction * surfacefriction
		* (edge ? vars->edgefriction : 1.0f) * dt;
	if (remaining < 0)
		remaining = 0;
	scale = remaining / speed;
	for (axis = 0; axis < 3; axis++)
		move->velocity[axis] *= scale;
}

// CS 1.6 fuser2 ratio, (100 - fuser2 * 0.001 * 19) / 100.
float PMAg_CS16StaminaRatio(const playermove_t *move)
{
	return (100.0f - move->stamina * 0.001f * 19.0f) * 0.01f;
}

static void PMAg_HalfGravity(playermove_t *move, const movevars_t *vars)
{
	float dt = move->cmd.msec * 0.001;
	float multiplier = vars->entgravity ? vars->entgravity : 1.0f;

	if (!move->waterjumptime)
		move->velocity[2] -= vars->gravity * multiplier * dt * 0.5f;
}

qboolean PMAg_DryStart(playermove_t *move, const movevars_t *vars,
	float surfacefriction, qboolean edge)
{
	qboolean jumped;

	PMAg_HalfGravity(move, vars);
	jumped = PMAg_JumpEdge(move, vars) && !move->waterjumptime;
	if (jumped)
	{
		move->onground = false;
		if (vars->profilekind == PMPROFILE_CS16)
		{	// CS 1.6 PM_PreventMegaBunnyJumping: over 1.2x maxspeed drops to 0.8 of that.
			float cap = 1.2f * vars->maxspeed, speed = sqrtf(DotProduct(move->velocity, move->velocity));
			if (cap > 0 && speed > cap)
				VectorScale(move->velocity, cap / speed * 0.8f, move->velocity);
		}
		move->velocity[2] = sqrtf(2.0f * 800.0f * 45.0f);
		if (vars->profilekind == PMPROFILE_CS16)
		{	// fuser2: the last jump's stamina scales this one, then recharges to 1315.789429 ms.
			if (move->stamina > 0)
				move->velocity[2] *= PMAg_CS16StaminaRatio(move);
			move->stamina = 1315.789429f;
		}
		PMAg_HalfGravity(move, vars);
	}
	if (move->onground)
	{
		move->velocity[2] = 0;
		PMAg_Friction(move, vars, surfacefriction, edge);
	}
	return jumped;
}

void PMAg_DryFinish(playermove_t *move, const movevars_t *vars)
{
	PMAg_HalfGravity(move, vars);
	if (move->onground)
		move->velocity[2] = 0;
}
