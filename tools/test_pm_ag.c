/* Copyright (C) 2026 FTESurf contributors. SPDX-License-Identifier: GPL-2.0-or-later
 * Actual AG primitives with the real engine types; no world or oracle emulation. */
#include "quakedef.h"
#include "pm_ag.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define NEAR(a,b) CHECK(fabsf((a)-(b)) <= 0.0001f)

static playermove_t state;
static movevars_t vars;

static void Reset(void)
{
	memset(&state, 0, sizeof(state));
	memset(&vars, 0, sizeof(vars));
	state.pm_type = PM_NORMAL;
	vars.maxspeed = 320;
	vars.accelerate = vars.airaccelerate = 10;
	vars.gravity = 800; vars.entgravity = 1;
	vars.friction = 4; vars.stopspeed = 100; vars.edgefriction = 2;
}

static void Acceleration(void)
{
	const vec3_t forward = {1, 0, 0};
	const vec3_t side = {0, 1, 0};
	const int msec[] = {1, 4, 10};
	const float expected[] = {3.2f, 12.8f, 30.0f};
	int i;

	for (i = 0; i < 3; i++)
	{
		Reset();
		state.cmd.msec = msec[i];
		PMAg_Accelerate(&state, &vars, forward, 320, 1);
		NEAR(state.velocity[0], expected[i]);
		NEAR(state.velocity[1], 0);
		NEAR(state.velocity[2], 0);
	}
	Reset(); state.cmd.msec = 10;
	state.velocity[0] = 600;
	PMAg_Accelerate(&state, &vars, side, 320, 1);
	NEAR(state.velocity[0], 600); NEAR(state.velocity[1], 30);
	PMAg_Accelerate(&state, &vars, forward, 320, 1);
	NEAR(state.velocity[0], 600); /* no turn-speed or stock takeoff crop */

	Reset(); state.cmd.msec = 4;
	{
		const vec3_t diagonal = {0.6f, 0.8f, 0};
		state.velocity[2] = -100;
		PMAg_Accelerate(&state, &vars, diagonal, 320, 1);
		NEAR(state.velocity[0], 7.68f); NEAR(state.velocity[1], 10.24f);
		NEAR(state.velocity[2], -100);
	}
	Reset(); state.cmd.msec = 4;
	PMAg_Accelerate(&state, &vars, forward, 500, 0.5f);
	NEAR(state.velocity[0], 6.4f); /* maxspeed clamp, then surface friction */
	state.velocity[0] = 29;
	PMAg_Accelerate(&state, &vars, forward, 320, 1);
	NEAR(state.velocity[0], 30); /* remaining projection, not full budget */

	Reset(); state.onground = true; state.cmd.msec = 10;
	vars.accelerate = 5; vars.airaccelerate = 100;
	PMAg_Accelerate(&state, &vars, forward, 320, 0.5f);
	NEAR(state.velocity[0], 8);
	state.velocity[0] = 319;
	PMAg_Accelerate(&state, &vars, forward, 320, 1);
	NEAR(state.velocity[0], 320);

	Reset(); state.cmd.msec = 10;
	PMAg_Accelerate(&state, &vars, forward, 0, 1);
	NEAR(state.velocity[0], 0);
	state.cmd.msec = 0;
	PMAg_Accelerate(&state, &vars, forward, 320, 1);
	NEAR(state.velocity[0], 0);
	state.cmd.msec = 10; state.pm_type = PM_DEAD;
	PMAg_Accelerate(&state, &vars, forward, 320, 1);
	NEAR(state.velocity[0], 0);
	state.pm_type = PM_NORMAL; state.waterjumptime = 0.1f;
	PMAg_Accelerate(&state, &vars, forward, 320, 1);
	NEAR(state.velocity[0], 0);
	puts("PASS air/ground acceleration, 30-target vs uncapped product, maxspeed, projection, friction and dry guards");
}

static void JumpEdges(void)
{
	static playermove_t expected;

	Reset(); state.onground = true;
	state.origin[2] = 100; state.velocity[0] = 600;
	state.oldbuttons = BUTTON_DUCK | BUTTON_ATTACK;
	state.ducktime = 123; state.ducked = true;
	state.cmd.buttons = BUTTON_JUMP | BUTTON_ATTACK;
	expected = state; expected.jump_held = true;
	CHECK(PMAg_JumpEdge(&state));
	CHECK(!memcmp(&state, &expected, sizeof(state))); /* only jump history changes */

	Reset(); state.onground = true;
	state.cmd.buttons = BUTTON_JUMP | BUTTON_ATTACK;
	CHECK(PMAg_JumpEdge(&state));
	CHECK(state.jump_held);
	CHECK(!PMAg_JumpEdge(&state)); /* held on ground is not autobhop */
	state.cmd.buttons = BUTTON_ATTACK;
	CHECK(!PMAg_JumpEdge(&state)); CHECK(!state.jump_held);
	state.cmd.buttons |= BUTTON_JUMP;
	CHECK(PMAg_JumpEdge(&state));

	Reset(); state.cmd.buttons = BUTTON_JUMP;
	CHECK(!PMAg_JumpEdge(&state)); CHECK(state.jump_held);
	state.onground = true;
	CHECK(!PMAg_JumpEdge(&state)); /* airborne attempt latched */
	state.cmd.buttons = 0;
	CHECK(!PMAg_JumpEdge(&state));
	state.cmd.buttons = BUTTON_JUMP;
	CHECK(PMAg_JumpEdge(&state));
	state.jump_held = false; state.pm_type = PM_DEAD;
	CHECK(!PMAg_JumpEdge(&state)); CHECK(state.jump_held);
	CHECK(state.cmd.buttons == BUTTON_JUMP); /* history only, no command mutation */
	puts("PASS fresh/held/release, airborne press then landing, dead press and unrelated buttons");
}

/* Clear-space fixture integration, not a collision/categorization surrogate.
 * The tested command starts supported, then remains airborne without obstacles. */
static void ClearMove(void)
{
	float dt = state.cmd.msec * 0.001;
	int axis;
	for (axis = 0; axis < 3; axis++)
		state.origin[axis] += state.velocity[axis] * dt;
}

static void DryCommands(void)
{
	const vec3_t direction = {1, 0, 0};
	const int msec[] = {10, 4, 1};
	int rate, tick;

	Reset(); state.onground = true; state.cmd.msec = 10;
	state.cmd.buttons = BUTTON_JUMP; state.velocity[0] = 600;
	CHECK(PMAg_DryStart(&state, &vars, 1, false));
	CHECK(!state.onground); NEAR(state.velocity[0], 600);
	NEAR(state.velocity[2], 264.328157f);
	PMAg_Accelerate(&state, &vars, direction, 0, 1);
	ClearMove();
	NEAR(state.origin[0], 6); NEAR(state.origin[2], 2.643282f);
	PMAg_DryFinish(&state, &vars);
	NEAR(state.velocity[2], 260.328157f);

	Reset(); state.onground = true; state.cmd.msec = 10;
	state.jump_held = true; state.cmd.buttons = BUTTON_JUMP;
	state.velocity[0] = 200;
	CHECK(!PMAg_DryStart(&state, &vars, 1, false));
	NEAR(state.velocity[0], 192); NEAR(state.velocity[2], 0);
	PMAg_DryFinish(&state, &vars); NEAR(state.velocity[2], 0);
	state.velocity[0] = 50;
	CHECK(!PMAg_DryStart(&state, &vars, 1, false)); NEAR(state.velocity[0], 46);
	state.velocity[0] = 200;
	CHECK(!PMAg_DryStart(&state, &vars, 1, true)); NEAR(state.velocity[0], 184);
	state.velocity[0] = 200; state.cmd.buttons = 0;
	CHECK(!PMAg_DryStart(&state, &vars, 1, false));
	state.velocity[0] = 200; state.cmd.buttons = BUTTON_JUMP;
	CHECK(PMAg_DryStart(&state, &vars, 1, false)); NEAR(state.velocity[0], 200);

	Reset(); state.cmd.msec = 10;
	state.velocity[0] = 30; state.velocity[2] = 40;
	PMAg_Friction(&state, &vars, 1, false);
	NEAR(state.velocity[0], 27.6f); NEAR(state.velocity[2], 36.8f); /* 3D helper speed50 */
	Reset(); state.cmd.msec = 10; vars.entgravity = 0;
	PMAg_DryStart(&state, &vars, 1, false); NEAR(state.velocity[2], -4);
	PMAg_DryFinish(&state, &vars); NEAR(state.velocity[2], -8);
	state.waterjumptime = 1;
	PMAg_DryStart(&state, &vars, 1, false); PMAg_DryFinish(&state, &vars);
	NEAR(state.velocity[2], -8); /* both gravity helpers veto nonzero waterjump timer */
	puts("PASS clear-space dry jump full command, uncapped600, two jump half-fixups, held/release friction, 3D friction and gravity veto");

	for (rate = 0; rate < 3; rate++)
	{
		Reset(); state.onground = true; state.cmd.msec = msec[rate];
		state.cmd.buttons = BUTTON_JUMP; state.velocity[0] = 600;
		for (tick = 0; tick < 1000 / msec[rate]; tick++)
		{
			CHECK(PMAg_DryStart(&state, &vars, 1, false) == (tick == 0));
			PMAg_Accelerate(&state, &vars, direction, 0, 1);
			ClearMove(); PMAg_DryFinish(&state, &vars);
		}
		CHECK(fabsf(state.origin[0] - 600) < 0.01f);
		CHECK(fabsf(state.origin[2] - (268.328157f - 400)) < 0.02f);
		CHECK(fabsf(state.velocity[2] - (268.328157f - 800)) < 0.03f);
		printf("PASS %d Hz clear-space dry command stream: z %.6f vz %.6f x %.6f\n",
			1000/msec[rate], state.origin[2], state.velocity[2], state.origin[0]);
	}
}

static void RateStreams(void)
{
	const int msec[] = {10, 4, 1};
	const float increment[] = {30, 12.8f, 3.2f};
	int rate, tick, duration;

	for (rate = 0; rate < 3; rate++)
	{
		Reset(); state.cmd.msec = msec[rate]; duration = 0;
		for (tick = 0; tick < 1000 / msec[rate]; tick++)
		{
			float speed = sqrtf(state.velocity[0]*state.velocity[0] + state.velocity[1]*state.velocity[1]);
			vec3_t perpendicular = {1, 0, 0};
			if (speed > 0)
			{
				perpendicular[0] = -state.velocity[1] / speed;
				perpendicular[1] = state.velocity[0] / speed;
			}
			PMAg_Accelerate(&state, &vars, perpendicular, 320, 1);
			duration += state.cmd.msec;
		}
		CHECK(duration == 1000);
		CHECK(fabsf(sqrtf(state.velocity[0]*state.velocity[0] + state.velocity[1]*state.velocity[1])
			- sqrtf((float)tick)*increment[rate]) < 0.005f);
		printf("PASS %d Hz velocity stream: %d commands, %d ms, speed %.6f\n",
			1000/msec[rate], tick, duration,
			sqrtf(state.velocity[0]*state.velocity[0] + state.velocity[1]*state.velocity[1]));
	}
}

int main(void)
{
	Acceleration();
	JumpEdges();
	DryCommands();
	RateStreams();
	puts("PASS AG dry primitives only; no collision, prediction, oracle or installed-server parity claim");
	return 0;
}
