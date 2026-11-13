/*
 * Independently implemented AG dry-motion primitives.
 * Copyright (C) 2026 FTESurf contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "quakedef.h"
#include "pm_ag.h"

int PMAg_ProfileRate(const char *identity, int version, float tick)
{
	if (version != PMAG_VERSION || !identity) return 0;
	if (!strcmp(identity, "ag-100-v1") && tick == 0.01f) return 100;
	if (!strcmp(identity, "ag-250-v1") && tick == 0.004f) return 250;
	return 0;
}
qboolean PMAg_ContextValid(const movevars_t *vars)
{
	return vars->physicsmode == PHYSMODE_AG && vars->sourceversion == PMAG_VERSION &&
		((vars->agprofile == 100 && vars->ticrate == 0.01f) ||
		 (vars->agprofile == 250 && vars->ticrate == 0.004f));
}
double PMAg_CommandSlot(float tick, double wantedfps, double serverfps)
{
	double tickms = floor(tick * 1000.0 + 0.5), minimum;
	int n;
	if ((tick != 0.01f && tick != 0.004f) || !(wantedfps >= 0)) return 0;
	n = wantedfps > 0 ? (int)floor(1000.0 / wantedfps / tickms + 0.5) : 1;
	n = bound(1, n, 8);
	minimum = serverfps > 0 ? 1000.0 / serverfps : 0;
	while (n * tickms < minimum && n <= 8) n++;
	return n <= 8 ? n * tickms : 0;
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

/* Even an airborne press consumes the edge. Uncapped speed is not autobhop.
 * This is only the button decision; the dry jump/gravity phases own impulse. */
qboolean PMAg_JumpEdge(playermove_t *move)
{
	qboolean pressed = (move->cmd.buttons & BUTTON_JUMP) != 0;
	qboolean fresh = pressed && !move->jump_held;

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
	jumped = PMAg_JumpEdge(move) && !move->waterjumptime;
	if (jumped)
	{
		move->onground = false;
		move->velocity[2] = sqrtf(2.0f * 800.0f * 45.0f);
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
