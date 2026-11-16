/* Copyright (C) 2026 FTESurf contributors. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FTE_PM_AG_H
#define FTE_PM_AG_H

/* Independently specified, experimental/unranked AG-derived movement. */
#define PMAG_VERSION 1

/* Patch 1589: every optional profile, one table. GoldSrc kinds run PMAg at a
 * fixed rate; HL2 kinds run PMSrc with HL2 settings and its jump boost. */
#define PMPROFILE_NONE		0
#define PMPROFILE_AG		1
#define PMPROFILE_CS16		2
#define PMPROFILE_HL2PRE	3	//HL2's jump boost before its limiter (accelerated hopping)
#define PMPROFILE_HL2POST	4	//after the limiter, whose backward sign flip is ABH
typedef struct
{
	const char *selector;	//sv_movementprofile value
	const char *identity;	//*movementprofile, saves/recordings namespace
	int kind;
	int rate;				//GoldSrc steps per second; 0 for HL2
	float tick;
	int since;				//engine patch whose clients know it (server gate on *pmprofile)
} pmprofile_t;
#define PMPROFILE_CLIENTVERSION 1591	//this client's *pmprofile
#define PMAG_AUTOHOPVERSION 1591		//first *pmprofile whose AG mover honours pm_autobunny
const pmprofile_t *PM_ProfileByIdentity(const char *identity);
const pmprofile_t *PM_ProfileBySelector(const char *selector);
#define PM_ProfileGoldSrc(kind) ((kind) == PMPROFILE_AG || (kind) == PMPROFILE_CS16)
#define PM_ProfileHL2(kind) ((kind) == PMPROFILE_HL2PRE || (kind) == PMPROFILE_HL2POST)
// Most fixed ticks one command may run: 8 at 100/250, 32 (32 ms) at 1000.
int PMAg_TickBudget(float tick);
float PMAg_CS16StaminaRatio(const playermove_t *move);

int PMAg_ProfileRate(const char *identity, int version, float tick);
qboolean PMAg_ContextValid(const movevars_t *vars);
// Zero denotes an incompatible advertised ceiling; Source callers retain their slot logic.
double PMAg_CommandSlot(float tick, double wantedfps, double serverfps);
void PMAg_PlayerMove(float gamespeed);
void PMAg_Accelerate(playermove_t *move, const movevars_t *vars,
	const vec3_t wishdir, float wishspeed, float surfacefriction);
qboolean PMAg_JumpEdge(playermove_t *move, const movevars_t *vars);

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
