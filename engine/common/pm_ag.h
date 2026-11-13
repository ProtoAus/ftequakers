/* Copyright (C) 2026 FTESurf contributors. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FTE_PM_AG_H
#define FTE_PM_AG_H

/* Independently specified, experimental/unranked AG-derived movement. */
#define PMAG_VERSION 1
int PMAg_ProfileRate(const char *identity, int version, float tick);
qboolean PMAg_ContextValid(const movevars_t *vars);
// Zero denotes an incompatible advertised ceiling; Source callers retain their slot logic.
double PMAg_CommandSlot(float tick, double wantedfps, double serverfps);
void PMAg_PlayerMove(float gamespeed);
void PMAg_Accelerate(playermove_t *move, const movevars_t *vars,
	const vec3_t wishdir, float wishspeed, float surfacefriction);
qboolean PMAg_JumpEdge(playermove_t *move);

/* Dry ordinary-jump command phases, with no carrier/longjump entitlement.
 * Start runs initial gravity, jump and grounded friction; accelerate follows.
 * The caller must then move/collide/categorize before Finish runs final gravity.
 * edge is the result of the current-hull ledge probe, not a new friction mode. */
void PMAg_Friction(playermove_t *move, const movevars_t *vars,
	float surfacefriction, qboolean edge);
qboolean PMAg_DryStart(playermove_t *move, const movevars_t *vars,
	float surfacefriction, qboolean edge);
void PMAg_DryFinish(playermove_t *move, const movevars_t *vars);

#endif
