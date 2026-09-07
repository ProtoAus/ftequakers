/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#define BUTTON_ATTACK 1
#define BUTTON_JUMP 2
#define BUTTON_DUCK 8		//FTESurf: Source IN_DUCK. Only read by pm_source.c.
#define BUTTON_SPEED 128	//FTESurf: Source IN_SPEED. Bit 7, and ONLY set when the
							//client's in_speedbutton is on, so no other game sees it.
							//Only read by pm_source.c, and only in noclip.

typedef enum {
	PM_NORMAL,			// normal ground movement
	PM_OLD_SPECTATOR,	// fly, no clip to world (QW bug)
	PM_SPECTATOR,		// fly, no clip to world
	PM_DEAD,			// no acceleration
	PM_FLY,				// fly, bump into walls
	PM_NONE,			// can't move
	PM_FREEZE,			// can't move or look around (TODO)
	PM_WALLWALK,		// sticks to walls. on ground while near one
	PM_6DOF				// spaceship mode
} pmtype_t;

#define PMF_JUMP_HELD			1
#define PMF_LADDER				2	//pmove flags. seperate from flags
#define PMF_DUCKED				4	//FTESurf: pm_source.c is holding the ducked hull

#define	MAX_PHYSENTS	2048
typedef struct
{
	vec3_t	origin;
	vec3_t	angles;
	model_t	*model;		// only for bsp models
	vec3_t	mins, maxs;	// only for non-bsp models
	unsigned int	info;		// for client or server to identify
	qbyte		nonsolid;		//contributes to contents, but does not block. FIXME: why not just use the contentsmask directly?
	qbyte		notouch;		//don't trigger touch events. FIXME: why are these entities even in the list?
	qbyte		isportal;		//special portal traversion required
	unsigned int forcecontentsmask;
	float		scale;		//nettest Patch 57: prop scale for convex-hull collision (<=0 treated as 1)
//	framestate_t framestate;
#define PE_FRAMESTATE NULLFRAMESTATE	//remove this once we start wanting players to interact with ents in different frames.
} physent_t;

typedef struct
{
	// player state
	vec3_t		origin;
	vec3_t		safeorigin;	//valid when safeorigin_known. needed for extrasr4's ladders otherwise they bug out.
	vec3_t		angles;
	vec3_t		velocity;
	vec3_t		basevelocity;
	vec3_t		gravitydir;
	qboolean		jump_held;
	int			jump_msec;	// msec since last jump
	float		waterjumptime;
	int			pm_type;
	vec3_t		player_mins;
	vec3_t		player_maxs;
	qboolean	capsule;

	// world state
	int			numphysent;
	physent_t	physents[MAX_PHYSENTS];	// 0 should be the world

	// input
	usercmd_t	cmd;

	qboolean onladder;
	qboolean safeorigin_known;

	// results
	int			skipent;
	int			numtouch;
	int			touchindex[MAX_PHYSENTS];
	vec3_t		touchvel[MAX_PHYSENTS];
	qboolean		onground;
	int			groundent;		// index in physents array, only valid
								// when onground is true
	int			waterlevel;
	int			watertype;

	struct world_s		*world;

	//FTESurf: Counter-Strike: Source movement state (pm_source.c).
	//APPEND ONLY -- plugins bake struct strides, so inserting a field
	//mid-struct silently corrupts a stale plugin at map load.
	//All of these are part of the PREDICTED state: anything here that the
	//client cannot reproduce exactly will show up as rubber-banding.
	float		surfacefriction;	//Source m_surfaceFriction. 1, or 0.25 airborne+rising.
	float		ducktime;			//m_flDucktime, MILLISECONDS, counts down from 1000
	qboolean	ducking;			//m_bDucking -- mid-transition
	qboolean	ducked;				//m_bDucked  -- hull is the small one
	float		msec_carry;			//fixed-tick accumulator remainder, seconds
	int			oldbuttons;			//m_nOldButtons, for the jump/duck edge tests
	vec3_t		groundnormal;		//plane underfoot, or the last one clipped against
	qboolean	surfing;			//this move clipped a too-steep-to-stand face
	float		stamina;			//m_flStamina, MILLISECONDS. CS:S jump/land penalty.
									//PREDICTED -- also lives in pmsourcestate_t.
	float		viewheight;			//OUTPUT ONLY: eye above the origin this tick, from
									//Source's SetDuckedEyeOffset.  Derived from ducktime
									//every tick, so it is NOT part of the carried state.

	//FTESurf board telemetry (Patch 131).  The HUD grades the moment you land
	//on a ramp, and doing that honestly needs the plane we actually clipped
	//against and the velocity we actually had going in -- both of which exist
	//only inside PMSrc_TryPlayerMove's bump loop and are gone by the time any
	//QC runs.  These publish them.  OUTPUT ONLY: nothing here feeds back into
	//the movement, so a stale value can never change where the player goes.
	vec3_t		boardnormal;		//the ramp plane of the most recent board
	vec3_t		boardvelocity;		//velocity at that instant, BEFORE the clip
	float		boardcount;			//increments once per NEW ramp contact.
									//PREDICTED -- also in pmsourcestate_t, so a
									//counter can never be missed between frames.
	float		rampcontact;		//1 while clipping a ramp; the falling edge is
									//"left the ramp", which is what leaving-speed
									//is measured on.
	float		rampoff;			//seconds off a ramp, the arrival gate.
									//PREDICTED -- also in pmsourcestate_t.
	vec3_t		rampnormal;			//FTESurf Patch 137: the plane THIS tick clipped.
									//boardnormal above is latched on ENTRY, which it
									//has to be or the board grade would re-fire every
									//tick of a ride -- but the strafe bar wants the
									//opposite, the plane under you right now, updated
									//as a curved ramp turns.  Zeroed when not riding.
									//OUTPUT ONLY, like the rest of this block.

	/*FTESurf Patch 260: the Source mover's ladder state.  Deliberately NOT the
	  pmove.onladder field far above, and deliberately APPENDED here rather than
	  placed beside it -- this struct's stride is baked into already-built plugins,
	  so a field inserted mid-struct silently corrupts a stale one at map load.

	  onladder cannot carry this.  Four callers hard-zero it immediately before
	  PM_PlayerMove (cl_pred.c, pr_csqc.c, pr_cmds.c, svhl_game.c) and a fifth
	  rebuilds it from a trigger touch (sv_user.c), so a value stored there is
	  destroyed on every replayed command -- which, on the client, is every
	  command.  These two ride in pmsourcestate_t, which is saved and restored
	  around prediction by design.  PREDICTED, not output: the probe DISTANCE and
	  the probe DIRECTION both depend on srcladder, so losing it does not merely
	  drop you off the ladder, it changes where the next trace looks.
	  pmove.onladder is still WRITTEN every tick as an output so QC and CSQC keep
	  seeing PMF_LADDER. */
	qboolean	srcladder;
	vec3_t		srcladdernormal;
} playermove_t;

typedef struct {
	//standard quakeworld
	float gravity;
	float stopspeed;
	float maxspeed;
	float spectatormaxspeed;
	float accelerate;
	float airaccelerate;
	float wateraccelerate;
	float friction;
	float waterfriction;
	float flyfriction;
	float entgravity;

	//extended stuff, sent via serverinfo
	float bunnyspeedcap;
	float watersinkspeed;
	float ktjump;
	float edgefriction; //default 2
	int	walljump;
	qboolean slidefix;
	qboolean airstep;
	qboolean pground;
	qboolean stepdown;
	qboolean slidyslopes;
	qboolean autobunny;
	qboolean bunnyfriction;	//force at least one frame of friction when bunnying.
	int stepheight;

	qbyte coordtype;	//FIXME: EZPEXT1_FLOATENTCOORDS should mean 4, but the result does not match ezquake/mvdsv's round-towards-origin which would result in inconsistencies. so player coords are rounded inconsistently.

	unsigned int	flags;

	//FTESurf: Counter-Strike: Source movement parameters (pm_source.c).
	//APPEND ONLY.  All are pushed through serverinfo like the other pm_*
	//extended movevars, so client prediction and the server agree.
	int		physicsmode;		//PHYSMODE_*. 0 keeps QuakeWorld's pmove.c untouched.
	float	ticrate;			//fixed physics step, seconds. 0.015 == 66.666.. Hz
	float	maxairspeed;		//GetAirSpeedCap(), 30. The surf budget.
	float	jumpvelocity;		//301.9933774 == sqrt(2*800*57), Momentum CGameModeBase::GetJumpFactor()
	float	standablenormal;	//0.7 -- normal.z below this is a surf ramp
	float	bounce;				//sv_bounce, for the airborne wall-clip overbounce
	float	maxvelocity;		//3500, clamped PER AXIS. Follows sv_maxvelocity unless pm_maxvelocity overrides.
	float	standheight;		//62 -- Momentum g_ViewVectorsMom hull max, NOT CS:S's 72
	float	duckheight;			//45 -- Momentum duck hull max. 62-45 = 17, but see viewscale below
	float	duckspeed;			//0.34 speed modifier while ducked
	float	viewheight;			//64 -- eye above the feet standing (Source VEC_VIEW)
	float	duckviewheight;		//47 -- eye above the feet ducked (VEC_DUCK_VIEW)
	float	noclipspeed;		//4 -- noclip flies at this * maxspeed
	float	stamina;			//0/1: master switch for the CS:S stamina penalty
	float	staminajumpcost;	//25 -- STAMINA_COST_JUMP
	float	staminalandcost;	//20 -- STAMINA_COST_FALL
	float	staminarecovery;	//19 -- STAMINA_RECOVERY_RATE
	float	normalizejump;		//Momentum mom_mv_normalize_jump_height. 0 == stock Source's variable jump.
	float	jumpaddrise;		//FTESurf Patch 241. 0 == the jump discards upward speed it was handed. 1 == build-38, adds to it.
	float	jumpzoffset;		//Momentum sv_jump_z_offset, 1.5. Takeoff is PINNED this far above the ground, not raised by it.
	float	walkspeed;			//CS_PLAYER_SPEED_WALK_MODIFIER. Scales maxspeed while the speed button is held.
	float	groundtracedist;	//Momentum sv_considered_on_ground. How far down CategorizePosition looks. 2 == Source.
	float	bumpcount;			//Momentum sv_ramp_bumpcount, 8. TryPlayerMove's trace budget per tick; Source ships 4.
	float	snaptoground;		//Momentum sv_snap_to_ground. 0 disables StayOnGround entirely.
	float	groundquadrants;	//Momentum mom_mv_check_ground_quadrants. The four sub-box ground retest.
	float	fixslopes;			//Momentum sv_slope_fix. Adopt a landing collision only when it GAINS 2d speed.
	float	fixedges;			//Momentum sv_edge_fix. Do not ground on a plane you would edgebug off next tick.
	float	fixrampbugs;		//Momentum sv_ramp_fix. Recover from a movement trace that starts solid instead of stopping dead.
	float	rampretrace;		//Momentum sv_ramp_initial_retrace_length, 0.2. How far to nudge out along a recovered plane.
	float	viewscale;			//Momentum IGameMode::GetViewScale(), 0.5 for every CS-based mode.
								//The mid-air duck moves the ORIGIN by viewscale*(standheight-duckheight),
								//not by the whole difference: the box shrinks from the top AND the bottom,
								//8.5 each at 62/45.  CMomentumGameMovement::FinishDuck, mom_gamemovement.cpp:1114.
								//Patch 241 read the BASE class instead and concluded "the FULL delta, not
								//half", which is why a crouch-jump used to lift the feet by 18.
	float	ladders;			//FTESurf Patch 260. 0 disables the ladder mover entirely.
	float	ladderdampen;		//Momentum sv_ladder_dampen, 0.2 -- how much sideways motion is
								//damped when you approach the rungs at a glancing angle.
	float	ladderangle;		//Momentum sv_ladder_angle, -0.707 (cos 135). The incidence
								//cosine below which ladderdampen starts applying.
} movevars_t;

#define PHYSMODE_QUAKEWORLD	0
#define PHYSMODE_SOURCE		1

//pmsourcestate_t is declared in protocol.h -- client.h needs it and is
//included before this header (client/quakedef.h:186 vs :196).
void PMSrc_SaveState(pmsourcestate_t *out);
void PMSrc_LoadState(const pmsourcestate_t *in);

#define MOVEFLAG_VALID							0x80000000	//to signal that these are actually known. otherwise reserved.
//#define MOVEFLAG_Q2AIRACCELERATE				0x00000001
#define MOVEFLAG_NOGRAVITYONGROUND				0x00000002	//no slope sliding
//#define MOVEFLAG_GRAVITYUNAFFECTEDBYTICRATE	0x00000004	//apply half-gravity both before AND after the move, which better matches the curve
#define MOVEFLAG_QWEDGEBOX						0x00010000	//calculate edgefriction using tracebox and a buggy start pos
#define MOVEFLAG_QWCOMPAT						(MOVEFLAG_NOGRAVITYONGROUND|MOVEFLAG_QWEDGEBOX)

extern	movevars_t		movevars;
extern	playermove_t	pmove;

void PM_PlayerMove (float gamespeed);
void PM_Init (void);
void PM_InitBoxHull (void);
void PM_AddTouchedEnt (int num);

//FTESurf: Counter-Strike: Source movement, engine/common/pm_source.c.
//Dispatched from the top of PM_PlayerMove when movevars.physicsmode is
//PHYSMODE_SOURCE; QuakeWorld's path is otherwise untouched.
void PMSrc_PlayerMove (float gamespeed);
void PMSrc_Init (void);		//registers the pm_selftest command

void PM_CategorizePosition (void);
int PM_HullPointContents (hull_t *hull, int num, vec3_t p);

int PM_ExtraBoxContents (vec3_t p);	//Peeks for HL-style water.
int PM_PointContents (vec3_t point);
qboolean PM_TestPlayerPosition (vec3_t point, qboolean ignoreportals);
qboolean PM_AnyPortals (void);	//FTESurf Patch 206: pm_source.c's plain trace sites need to know
#ifndef __cplusplus
struct trace_s PM_PlayerTrace (vec3_t start, vec3_t stop, unsigned int solidmask);
//FTESurf Patch 203: PM_PlayerTrace, plus "and if that hit a portal, go through it
//and re-trace from the far side".  Was static to pmove.c because QuakeWorld's
//PM_SlideMove was its only caller; pm_source.c is now a second one.  *tookportal
//comes back as the fraction of the ORIGINAL segment that was spent reaching the
//portal plane -- 0 when nothing was traversed -- and on a traversal pmove.angles
//and pmove.velocity have already been rewritten into the far side's frame.
struct trace_s PM_PlayerTracePortals (vec3_t start, vec3_t end, unsigned int solidmask, float *tookportal);
#endif

