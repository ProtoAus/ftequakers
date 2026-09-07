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

/*
================================================================================
  pm_source.c -- Counter-Strike: Source player movement.  (FTESurf)

  A self-contained port of Valve's CGameMovement, selected by
  `pm_physicsmode 1` and dispatched from the top of PM_PlayerMove().
  QuakeWorld's movement in pmove.c is NOT touched -- this engine tree is
  shared with other games, and pm_physicsmode defaults to 0, so nothing
  changes for them.

  WHY A SEPARATE MODULE RATHER THAN PATCHING pmove.c
  --------------------------------------------------
  The two are the same shape but differ in about a dozen load-bearing places,
  and each difference is the sort that silently ruins surfing:

    * PM_ClipVelocity has no "adjust" pass, so float error leaves a residual
      into-plane component at overbounce 1.0 and the player sticks to ramps.
    * PM_SlideMove zeroes velocity when DotProduct(vel, primal_velocity) <= 0.
      Source has no such bail -- it uses allFraction -- and that difference
      kills momentum in ramp corners.
    * pmove.c:854 passes movevars.accelerate to PM_AirAccelerate, so
      sv_airaccelerate is dead code in the QW path.
    * Gravity is applied in one step, not split either side of the move.
    * There is no surfaceFriction, and no duck at all.

  REFERENCES (authority order)
    1. gamemovement_momentummod.cpp -- the real CGameMovement.  Line numbers in
       the comments below refer to that file, in Momentum Mod's public archive
       at github.com/momentum-mod/game (Source 1 SDK Licence; their development
       went closed-source in 2020 and the open tree was left up deliberately).
       No code from it is reproduced here -- this is a clean-room reimplement-
       ation against the behaviour it documents.
    2. Values measured in-engine against the above and recorded, per patch, in
       ENGINE_PATCHES.md.
    3. A separate reference set of numeric test vectors, used for cross-checking
       arithmetic only -- its *tunings* are CS:GO-flavoured and are NOT used.

  COORDINATE CONVENTION
  Source puts the player origin at the FEET (hull 0..72), unlike Quake's
  centred origin.  This module does not care -- it uses whatever hull the
  entity supplies -- but the mod sets a Source-shaped hull so that VBSP
  entity placement lines up.

  GRAVITY DIRECTION
  Source has no equivalent of FTE's arbitrary gravitydir, so this module
  assumes -Z and forces it on entry.  Wallwalk/6dof are not reachable here.
================================================================================
*/

#include "quakedef.h"

#ifdef HAVE_CLIENT
#include "cl_master.h"
#endif

/* ---------------------------------------------------------------- constants */

/* Surfaces with normal.z >= this are ground; anything steeper leaves you
   airborne and sliding.  This single number is the whole of surfing.
   Overridable via pm_standablenormal for experimentation. */
#define PMSRC_STANDABLE_DEFAULT     0.7f

/* Source's trace epsilon, 1/32 unit. */
#define PMSRC_DIST_EPSILON          0.03125f

/* TryPlayerMove: 4 bumps against at most 5 accumulated planes.

   FTESurf Patch 172: the bump count is now pm_bumpcount and this is only its
   fallback.  Momentum ships sv_ramp_bumpcount 8 (min 4, max 16,
   mom_gamemovement.cpp:42) because its ramp recovery spends bumps re-tracing,
   and four is not enough budget to both recover and still finish the move.
   The clip-plane ceiling stays a constant: it sizes an array, and Source's
   crease logic is written around exactly five. */
#define PMSRC_MAX_BUMPS             4
#define PMSRC_MAX_BUMPS_LIMIT       16
#define PMSRC_MAX_CLIP_PLANES       5

/* FTESurf Patch 177, the ramp fix.  How far to nudge the origin out along a
   recovered plane before re-tracing -- Momentum's sv_ramp_initial_retrace_length,
   which ships 0.2 and this is its fallback.  The same number also scales the
   27-direction search's hull expansion, at (bumpcount * 2) * this.

   The ceiling is OURS, not Momentum's: the value arrives over serverinfo and is
   therefore whatever the other end sent, and it is a distance the player is
   teleported by, once per bump.  At 4 with a 16-bump budget that is already 64
   units of free movement; past that a hostile or broken server could walk you
   through a wall using nothing but a movevar. */
#define PMSRC_RAMP_RETRACE          0.2f
#define PMSRC_RAMP_RETRACE_LIMIT    4.0f

/* The push applied when the crease turns out to be one plane twice over.  See
   the degenerate-crease block in PMSrc_TryPlayerMove; Momentum's own comment
   says 2.0 works and jitters, so 20 it is (cpp:2865). */
#define PMSRC_RAMP_UNSTICK_PUSH     20.0f

/* CategorizePosition: "shooting up this fast is definitely not on ground".
   gamemovement_momentummod.cpp:3822.  NOT movementmath's 180 -- that value is
   from a CS:GO-flavoured sandbox. */
#define PMSRC_NON_JUMP_VELOCITY     140.0f

/* CategorizePosition traces this far down looking for ground.  cpp:3810.

   FTESurf Patch 172: now pm_groundtracedist, and this is the fallback.  It is
   worth knowing what this number IS before tuning it: it is the width of the
   band in which you count as standing on a surface without touching it, so it
   is also the amount by which your takeoff height -- and therefore your jump
   apex -- can vary from one jump to the next.  That is the variance
   pm_jumpzoffset exists to remove.  Momentum's own default has moved (1.0 in
   the 0.8.7 source, 2.0 in today's build); 2.0 is stock Source and ours. */
#define PMSRC_GROUND_TRACE_DIST     2.0f

/* Duck transition times, seconds.  Source TIME_TO_DUCK / TIME_TO_UNDUCK. */
#define PMSRC_TIME_TO_DUCK          0.4f
#define PMSRC_TIME_TO_UNDUCK        0.2f
/* GAMEMOVEMENT_DUCK_TIME, milliseconds. */
#define PMSRC_DUCK_TIMER            1000.0f

/* CS:S speed modifiers.  Run 250 -> walk 130, duck 85. */
#define PMSRC_DUCK_SPEED_MODIFIER   0.34f

/* Eye heights above the feet.  Momentum g_ViewVectorsMom, mom_gamerules.cpp:34-40.
   Note the standing eye at 64 is deliberately ABOVE the 62-unit hull crown. */
#define PMSRC_VIEWHEIGHT_DEFAULT    64.0f
#define PMSRC_DUCKVIEWHEIGHT_DEFAULT 47.0f

/* Stamina, CS:S's cs_gamemovement.cpp STAMINA_* defines.  The pool is stored
   in MILLISECONDS and counts down at real time; the recovery rate is not a
   rate in the usual sense but the scale factor that turns the remaining
   milliseconds back into a fraction of STAMINA_MAX.  See PMSrc_StaminaRatio. */
#define PMSRC_STAMINA_MAX           100.0f
#define PMSRC_STAMINA_JUMP_COST     25.0f
#define PMSRC_STAMINA_LAND_COST     20.0f
#define PMSRC_STAMINA_RECOVERY      19.0f

/* Noclip flies at this multiple of maxspeed; +speed doubles it again. */
#define PMSRC_NOCLIP_SPEED          4.0f

/* Guard the fixed-tick loop.  A 50ms usercmd at 0.015 is 3 ticks; anything
   past 8 means the config is wrong, and running hundreds of ticks inside one
   command would hitch far worse than dropping the excess. */
#define PMSRC_MAX_TICKS             8

/* Slack so 15.0000001ms still counts as one whole 15ms tick. */
#define PMSRC_TICK_EPSILON          1e-5f

/* Board telemetry (Patch 131).  A ramp is the band between "steep enough that
   you are airborne on it" and "steep enough that it is a wall": the top is
   standable (0.7) and the bottom 0.1, about 84 degrees.  The 0.1 is a
   judgement rather than an engine constant -- past it the surface takes your
   speed head-on and grading it as a landing is meaningless.

   BOARD_AIRGATE is how long you must have been OFF a ramp for the next
   contact to count as a new one.  Without it a sustained ride re-boards every
   tick, because gravity pushes into the face every tick and so the clip fires
   every tick.  0.08s is two-thirds of a jump's worth of air at 66Hz -- long
   enough to reject a graze, short enough to catch a real ramp-to-ramp
   transfer. */
#define PMSRC_MIN_RAMP_NZ           0.1f
#define PMSRC_BOARD_AIRGATE         0.08f

/* ------------------------------------------------------------------- state */

/* The frametime of the tick currently being simulated.  Always
   movevars.ticrate -- named for symmetry with pmove.c's `frametime`. */
static float pms_frametime;

static vec3_t pms_forward, pms_right, pms_up;

/* The STANDING hull, captured from the entity on entry.  The duck hull is
   derived from it, so a mod can change player size without the engine
   needing to know about it. */
static vec3_t pms_standmins, pms_standmaxs;

/* One-shot latch for the "mins[2] is not 0" warning below.  Per session, not
   per map: this is a mod bug, it is loud once, and PMSrc_PlayerMove runs every
   usercmd -- an unlatched Con_Printf here would be 66 lines a second. */
static qboolean pms_warned_hullfloor;

/* Move values for this tick, after CheckParameters/duck cropping. */
static float pms_forwardmove, pms_sidemove, pms_upmove;
static float pms_maxspeed;

/* Set when this tick clipped against a non-standable plane -- i.e. you are
   surfing.  Reported to the mod so the HUD can say so. */
static qboolean pms_surfed;

extern cvar_t pm_noround;
extern cvar_t pm_dispprobe;	//FTESurf Patch 256, temporary -- defined in common.c
extern cvar_t pm_ladderprobe;	//FTESurf Patch 260, temporary -- defined in common.c

/*FTESurf Patch 258: com_bih.c's record of WHICH plane of BIH_ClipToTriangle's
  set stopped the last triangle clip, and which triangle it belonged to.  See the
  essay above BIH_ClipToTriangle; PMSrc_DispProbeWhy is the reader. */
extern int    bih_probe_plane;
extern vec3_t bih_probe_norm;
extern vec3_t bih_probe_tri[3];

/*Print the second line of the probe: the identity of the plane that stopped the
  trace, and the triangle's own face normal so the two candidate explanations can
  be told apart at a glance.

  plane 0 with a steep face normal  -> the terrain there really is that steep and
                                       the bevels are innocent.
  plane 2/3/4 with a FLAT face normal -> an in-plane edge plane is stopping the
                                       player over ground the triangle itself
                                       says is walkable.  That is Bug B.
  plane 1                           -> the +4 back slab, i.e. still a winding or
                                       thickness problem, not a bevel one.

  The record is only believed when its normal matches the trace being reported;
  a pmove trace is merged across models and the last triangle to win an inner
  trace need not be the one that won this one. */
static void PMSrc_DispProbeWhy (trace_t *t)
{
	/*Index space is com_bih.c's: 0..4 are the original five, 5..13 are Patch
	  258's edge-cross-axis bevels in build order, and 100..105 are the axial
	  bevels the `if (tr->shape)` block adds. */
	static const char *planename[5] = {
		"FACE", "BACK-SLAB(+4)", "edge p1p2", "edge p2p3", "edge p3p1"};
	static const char *axialname[6] = {
		"axial +x", "axial +y", "axial +z", "axial -x", "axial -y", "axial -z"};
	const char *what;
	char bevelbuf[32];
	vec3_t e1, e2, n;
	float len;

	if (bih_probe_plane < 0)
	{
		Con_Printf ("[dispprobe]   via: not a trisoup triangle (brush/patch, or nothing hit)\n");
		return;
	}
	if (bih_probe_plane < 5)
		what = planename[bih_probe_plane];
	else if (bih_probe_plane < 14)
	{	/*Patch 258's bevels, in build order (edge-major).  Deliberately NOT
		  labelled "edge N x axis M": a bevel whose cross product degenerates is
		  skipped, so the index is a position in the list, not a fixed pairing.
		  The normal on the line above identifies it exactly. */
		Q_snprintfz (bevelbuf, sizeof(bevelbuf), "bevel #%i", bih_probe_plane-5);
		what = bevelbuf;
	}
	else if (bih_probe_plane >= 100 && bih_probe_plane < 106)
		what = axialname[bih_probe_plane-100];
	else
		what = "?";
	if (fabs(bih_probe_norm[0] - t->plane.normal[0]) > 0.002 ||
	    fabs(bih_probe_norm[1] - t->plane.normal[1]) > 0.002 ||
	    fabs(bih_probe_norm[2] - t->plane.normal[2]) > 0.002)
	{
		Con_Printf ("[dispprobe]   via: unknown -- last triangle record (%.3f %.3f %.3f) "
		            "is not this trace's plane\n",
		            bih_probe_norm[0], bih_probe_norm[1], bih_probe_norm[2]);
		return;
	}

	VectorSubtract (bih_probe_tri[0], bih_probe_tri[1], e1);
	VectorSubtract (bih_probe_tri[2], bih_probe_tri[1], e2);
	CrossProduct (e1, e2, n);
	len = VectorLength (n);
	if (len > 0)
		VectorScale (n, 1/len, n);

	Con_Printf ("[dispprobe]   via plane %i %s of tri "
	            "(%.1f %.1f %.1f)(%.1f %.1f %.1f)(%.1f %.1f %.1f)  face norm %.3f %.3f %.3f\n",
	            bih_probe_plane, what,
	            bih_probe_tri[0][0], bih_probe_tri[0][1], bih_probe_tri[0][2],
	            bih_probe_tri[1][0], bih_probe_tri[1][1], bih_probe_tri[1][2],
	            bih_probe_tri[2][0], bih_probe_tri[2][1], bih_probe_tri[2][2],
	            n[0], n[1], n[2]);
}

/* pmove.c owns these; we reuse its touch list so trigger_push / teleports
   fire exactly as they do in the QuakeWorld path. */
void PM_AddTouchedEnt (int num);

/* --------------------------------------------------------------- utilities */

static float PMSrc_Standable (void)
{
	if (movevars.standablenormal > 0)
		return movevars.standablenormal;
	return PMSRC_STANDABLE_DEFAULT;
}

/* FTESurf Patch 172.  Both of these are read every tick and both are clamped
   rather than merely defaulted, because a movevar arriving from serverinfo is
   whatever the other end sent: 0 would mean "never find ground" and a large
   bump count would mean an unbounded trace budget inside one command. */
static float PMSrc_GroundTraceDist (void)
{
	if (movevars.groundtracedist > 0)
		return movevars.groundtracedist;
	return PMSRC_GROUND_TRACE_DIST;
}

static int PMSrc_BumpCount (void)
{
	int n = (int)movevars.bumpcount;
	if (n < PMSRC_MAX_BUMPS)
		n = PMSRC_MAX_BUMPS;
	if (n > PMSRC_MAX_BUMPS_LIMIT)
		n = PMSRC_MAX_BUMPS_LIMIT;
	return n;
}

/* FTESurf Patch 177.  Clamped for the same reason the two above are, and see
   PMSRC_RAMP_RETRACE_LIMIT for why the ceiling matters more here than usual. */
static float PMSrc_RampRetrace (void)
{
	float n = movevars.rampretrace;
	if (!(n > 0))
		return PMSRC_RAMP_RETRACE;
	if (n > PMSRC_RAMP_RETRACE_LIMIT)
		return PMSRC_RAMP_RETRACE_LIMIT;
	return n;
}

static trace_t PMSrc_TraceHull (vec3_t start, vec3_t end)
{
	return PM_PlayerTrace (start, end, MASK_PLAYERSOLID);
}

/* ---- FTESurf Patch 203: portals in Source movement --------------------------

   PMSrc_TraceHull above is the ONLY trace entry point in this file and it has
   about eighteen call sites: ground probes, the step-up and step-down tests,
   duck clearance, stuck recovery, the ramp fix's 27-direction search.
   PM_PlayerTracePortals cannot simply be substituted for it, because taking a
   portal is not a query -- it rewrites pmove.angles and pmove.velocity as a
   side effect.  A "is there floor two units below me" probe that silently
   teleported the player and spun their view would be a worse bug than the one
   this patch is fixing.

   So exactly one call site gets the portal-aware trace: the top-level bump in
   PMSrc_TryPlayerMove, which is the direct analogue of QuakeWorld's
   PM_SlideMove (pmove.c:240) and the only place in this file that owns the
   player's whole move for the tick.

   pms_portalcrossed is how that fact escapes the function.  PMSrc_StepMove runs
   the move TWICE -- flat, then stepped up 18 and back down -- restoring origin
   and velocity between the attempts but NOT angles, so without a flag a
   crossing in the flat attempt would be taken a second time in the stepped one:
   two rotations for one doorway, and a final position derived from the far side
   of a portal the player passed through once. */
static qboolean pms_portalcrossed;

/* FTESurf Patch 206: does this map have portals at all?  Cached once per tick
   in PMSrc_PlayerMove.

   P203 asked the narrower question -- "did THIS trace stop on a portal?" -- via
   trace.entnum, and that question cannot be answered.  PM_PortalCSG stamps
   entnum only when the move leaves through the portal plane itself; a move that
   enters the carved region and exits through a SIDE plane has been elongated
   through solid wall and still reports the world.  Anything that trusted entnum
   would reuse that trace and walk the player into the brushwork with no
   transform run.  So the gate is the coarse one, and on the 1290 maps with no
   portals it is one early-out and nothing changes. */
static qboolean pms_haveportals;

/* Momentum's CloseEnough(Vector, Vector, FLT_EPSILON), which the ramp fix uses
   everywhere it asks "is this the same plane" or "is this plane empty".  An
   exact VectorCompare would do for the empty test -- a cleared vector really is
   all zeroes -- but not for the same-plane test, where both sides came out of a
   trace and one of them has been through a normalise. */
static qboolean PMSrc_VecCloseEnough (const vec3_t a, const vec3_t b)
{
	return (fabs(a[0] - b[0]) <= FLT_EPSILON &&
	        fabs(a[1] - b[1]) <= FLT_EPSILON &&
	        fabs(a[2] - b[2]) <= FLT_EPSILON);
}

/* A plane normal a trace could plausibly have produced.  Momentum spells this
   out inline in three places (cpp:2452, :2519 and IsValidMovementTrace's
   :2132); a normal with a component past 1 is not a unit vector and did not
   come from geometry -- it is the uninitialised or garbage plane that a
   start-solid trace leaves behind, and clipping against it is how a surfer gets
   launched sideways through a ramp. */
static qboolean PMSrc_PlaneIsSane (const vec3_t n)
{
	return (fabs(n[0]) <= 1.0f && fabs(n[1]) <= 1.0f && fabs(n[2]) <= 1.0f);
}

/* Source's SimpleSpline -- smoothstep, 3t^2 - 2t^3.  Zero derivative at both
   ends, which is why the CS:S crouch has no visible snap at either end. */
static float PMSrc_SimpleSpline (float value)
{
	float sqr;
	if (value <= 0)
		return 0;
	if (value >= 1)
		return 1;
	sqr = value * value;
	return (3 * sqr) - (2 * sqr * value);
}

/* ----------------------------------------------------------------- stamina */

/*
  CS:S's stamina, cs_gamemovement.cpp.

  The pool is stored in milliseconds and drains at real time (ReduceTimers).
  Jumping and landing REFILL it -- a full pool is the penalised state, empty
  is fresh.  The ratio below is what a full pool costs you:

      jump:  (25/19)*1000 ms  ->  ratio 1 - 25/100 = 0.75
      land:  (20/19)*1000 ms  ->  ratio 1 - 20/100 = 0.80

  and it recovers linearly over that many milliseconds.  The ratio scales two
  things and nothing else: the jump impulse, and mv->m_flMaxSpeed.

  Scaling maxspeed rather than velocity is why this reads as "friction feels
  wrong" when it is missing.  It does not slow you down directly -- it lowers
  the speed ground friction is pulling you toward, so a landing bleeds off to
  200 instead of 250 and then climbs back over the next second.

  Note it does NOT hurt air-strafing: AirAccelerate's addspeed is measured
  against the 30 u/s air cap, and accelspeed is `airaccel * wishspeed * dt`
  which stays far above 30 even at a 0.75 ratio.  Surfing is untouched, which
  is exactly why CS:S can afford this.
*/
static qboolean PMSrc_StaminaEnabled (void)
{
	return movevars.stamina != 0;
}

static float PMSrc_StaminaRecovery (void)
{
	return movevars.staminarecovery > 0 ? movevars.staminarecovery : PMSRC_STAMINA_RECOVERY;
}

static float PMSrc_StaminaRatio (void)
{
	float ratio;

	if (!PMSrc_StaminaEnabled() || pmove.stamina <= 0)
		return 1.0f;

	ratio = (PMSRC_STAMINA_MAX - ((pmove.stamina / 1000.0f) * PMSrc_StaminaRecovery()))
	        / PMSRC_STAMINA_MAX;

	if (ratio < 0)
		ratio = 0;
	else if (ratio > 1)
		ratio = 1;
	return ratio;
}

/* cost is in STAMINA_MAX units; the pool is in milliseconds. */
static void PMSrc_StaminaSpend (float cost)
{
	if (!PMSrc_StaminaEnabled() || cost <= 0)
		return;
	pmove.stamina = (cost / PMSrc_StaminaRecovery()) * 1000.0f;
}

/*
  Carry the cross-command state in and out of the global pmove struct.
  See pmsourcestate_t in pmove.h for why every one of these fields matters
  to prediction.
*/
void PMSrc_SaveState (pmsourcestate_t *out)
{
	out->surfacefriction = pmove.surfacefriction;
	out->ducktime        = pmove.ducktime;
	out->ducking         = pmove.ducking;
	out->ducked          = pmove.ducked;
	out->msec_carry      = pmove.msec_carry;
	out->oldbuttons      = pmove.oldbuttons;
	out->stamina         = pmove.stamina;
	out->rampoff         = pmove.rampoff;
	out->boardcount      = pmove.boardcount;
	out->rampcontact     = pmove.rampcontact;
	out->srcladder       = pmove.srcladder;		//Patch 260
	VectorCopy (pmove.srcladdernormal, out->srcladdernormal);
}

void PMSrc_LoadState (const pmsourcestate_t *in)
{
	pmove.surfacefriction = in->surfacefriction;
	pmove.ducktime        = in->ducktime;
	pmove.ducking         = in->ducking;
	pmove.ducked          = in->ducked;
	pmove.msec_carry      = in->msec_carry;
	pmove.oldbuttons      = in->oldbuttons;
	pmove.stamina         = in->stamina;
	pmove.rampoff         = in->rampoff;
	pmove.boardcount      = in->boardcount;
	pmove.rampcontact     = in->rampcontact;
	pmove.srcladder       = in->srcladder;		//Patch 260
	VectorCopy (in->srcladdernormal, pmove.srcladdernormal);
}

/*
==================
PMSrc_ClipVelocity   (cpp:3159)

The single most important function for surfing.

The difference from FTE's PM_ClipVelocity is the "adjust" pass at the end.
Without it, floating-point error leaves a small residual component pointing
INTO the plane at overbounce 1.0; on a ramp that residual is re-clipped every
tick and the player sticks instead of sliding.  Note also that Source does NOT
snap small components to zero the way PM_ClipVelocity's STOP_EPSILON does --
that snapping is another source of ramp stutter.
==================
*/
static int PMSrc_ClipVelocity (vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
	float	backoff;
	float	change;
	float	adjust;
	int		blocked = 0;
	int		i;

	if (normal[2] > 0)
		blocked |= 1;		/* floor */
	/* FTESurf Patch 172: an epsilon compare, not `!normal[2]`.  A plane whose
	   normal is 1e-9 off vertical is a wall, and exact-float equality says it
	   is not.  Momentum tests CloseEnough(normal.z, 0, FLT_EPSILON) at
	   mom_gamemovement.cpp:2757 for the same reason.  Cosmetic today, since
	   every caller in this file discards the return -- fixed because the next
	   person to start using it should not have to discover this. */
	if (fabs (normal[2]) <= FLT_EPSILON)
		blocked |= 2;		/* wall / step */

	backoff = DotProduct (in, normal) * overbounce;

	for (i = 0; i < 3; i++)
	{
		change = normal[i] * backoff;
		out[i] = in[i] - change;
	}

	/* iterate once to make sure we aren't still moving through the plane */
	adjust = DotProduct (out, normal);
	if (adjust < 0.0f)
		VectorMA (out, -adjust, normal, out);

	return blocked;
}

/*
==================
PMSrc_CheckVelocity   (cpp:3060)

Note the clamp is PER AXIS, not on the magnitude.  A player moving diagonally
can therefore exceed sv_maxvelocity in total speed, which is real CS:S
behaviour and matters at surf speeds.
==================
*/
static void PMSrc_CheckVelocity (void)
{
	float maxvel = movevars.maxvelocity;
	int i;

	if (maxvel <= 0)
		maxvel = 3500;

	for (i = 0; i < 3; i++)
	{
		if (pmove.velocity[i] != pmove.velocity[i])	/* NaN */
			pmove.velocity[i] = 0;
		if (pmove.origin[i] != pmove.origin[i])
			pmove.origin[i] = 0;

		if (pmove.velocity[i] > maxvel)
			pmove.velocity[i] = maxvel;
		else if (pmove.velocity[i] < -maxvel)
			pmove.velocity[i] = -maxvel;
	}
}

/* ----------------------------------------------------------------- gravity */

/*
  Gravity is applied in two halves, one either side of the move
  (cpp:1261 / cpp:1702).  This is leapfrog/symplectic integration: it makes
  the trajectory independent of tick rate to second order, which is why a
  Source jump reaches the same height at 66 and 128 tick.  Applying it in one
  step -- what pmove.c:828 does -- does not.
*/
static void PMSrc_StartGravity (void)
{
	float ent_gravity = movevars.entgravity;
	if (!ent_gravity)
		ent_gravity = 1.0;

	pmove.velocity[2] -= (ent_gravity * movevars.gravity * 0.5f * pms_frametime);
	pmove.velocity[2] += pmove.basevelocity[2] * pms_frametime;
	pmove.basevelocity[2] = 0;

	PMSrc_CheckVelocity ();
}

static void PMSrc_FinishGravity (void)
{
	float ent_gravity = movevars.entgravity;

	if (pmove.waterjumptime)
		return;

	if (!ent_gravity)
		ent_gravity = 1.0;

	pmove.velocity[2] -= (ent_gravity * movevars.gravity * pms_frametime * 0.5f);

	PMSrc_CheckVelocity ();
}

/* ------------------------------------------------------- accel / friction */

/*
==================
PMSrc_Friction   (cpp:1644)

Note: speed is the 3D magnitude and the whole 3D velocity is scaled -- but
FullWalkMove zeroes velocity[2] immediately before calling this, so in
practice it is a 2D operation.  Kept 3D to match Source exactly.
==================
*/
static void PMSrc_Friction (void)
{
	float	speed, newspeed, control;
	float	friction;
	float	drop = 0;

	if (pmove.waterjumptime)
		return;

	speed = VectorLength (pmove.velocity);
	if (speed < 0.1f)
		return;

	if (pmove.onground)
	{
		friction = movevars.friction * pmove.surfacefriction;

		/* Bleed off some speed, but if we have less than the bleed
		   threshold, bleed the threshold amount. */
		control = (speed < movevars.stopspeed) ? movevars.stopspeed : speed;
		drop += control * friction * pms_frametime;
	}

	newspeed = speed - drop;
	if (newspeed < 0)
		newspeed = 0;

	if (newspeed != speed)
	{
		newspeed /= speed;
		VectorScale (pmove.velocity, newspeed, pmove.velocity);
	}
}

/*
==================
PMSrc_Accelerate   (cpp:1833)
==================
*/
static void PMSrc_Accelerate (vec3_t wishdir, float wishspeed, float accel)
{
	float	addspeed, accelspeed, currentspeed;
	int		i;

	if (pmove.pm_type == PM_DEAD)
		return;
	if (pmove.waterjumptime)
		return;

	currentspeed = DotProduct (pmove.velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
		return;

	accelspeed = accel * pms_frametime * wishspeed * pmove.surfacefriction;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i = 0; i < 3; i++)
		pmove.velocity[i] += accelspeed * wishdir[i];
}

/*
==================
PMSrc_AirAccelerate   (cpp:1718)

THE surf function.  The asymmetry is deliberate and is what makes air-strafing
work: `addspeed` is computed against wishspeed CLAMPED to 30 u/s, but
`accelspeed` is computed from the UNCLAMPED wishspeed.  So the amount you may
add along wishdir is tiny, while the rate at which you may add it is large --
which means that as long as your velocity's projection onto wishdir stays
below 30, you keep gaining, with no cap on the resulting speed.
==================
*/
static void PMSrc_AirAccelerate (vec3_t wishdir, float wishspeed, float accel)
{
	float	addspeed, accelspeed, currentspeed;
	float	wishspd = wishspeed;
	float	cap;
	int		i;

	if (pmove.pm_type == PM_DEAD)
		return;
	if (pmove.waterjumptime)
		return;

	cap = movevars.maxairspeed;
	if (cap <= 0)
		cap = 30;

	if (wishspd > cap)
		wishspd = cap;

	currentspeed = DotProduct (pmove.velocity, wishdir);
	addspeed = wishspd - currentspeed;
	if (addspeed <= 0)
		return;

	/* NOTE: full wishspeed here, not the clamped wishspd. */
	accelspeed = accel * wishspeed * pms_frametime * pmove.surfacefriction;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i = 0; i < 3; i++)
		pmove.velocity[i] += accelspeed * wishdir[i];
}

/* ------------------------------------------------------------ the move loop */

/*
==================
PMSrc_IsValidMovementTrace   (cpp:109)

FTESurf Patch 177.  "Did this trace actually tell us anything?"

A swept hull trace against a surf ramp does not always come back with a usable
answer.  The two failure modes this catches are the whole reason ramp bugs
exist:

  * the trace STARTED inside the brush.  The player is a hull, the ramp is a
    plane, and one tick's worth of clipping leaves you a fraction of a unit
    inside it often enough to matter.  Source's answer is startsolid with no
    plane and fraction 0, and the stock move loop reads that as "you may not
    move", which is the dead stop on a seam.
  * the trace reports a clean sweep to an endpos that is itself solid.  That is
    a precision artefact of triangle-soup (displacement) tracing, and taking the
    move would tunnel the player into the world.  So the last check re-tests the
    destination with an UNSWEPT hull, which cannot lie about it.

Momentum's version has a third test (cpp:125-129) that is dead: it is the
fraction-is-zero test above ANDed with an extra condition, so it can only be
reached when the earlier one has already returned.  Dropped rather than copied,
because a check that cannot fire reads like a check that can.
==================
*/
static qboolean PMSrc_IsValidMovementTrace (trace_t *tr)
{
	trace_t	stuck;

	/* You can be stuck without any plane information at all, so this has to
	   come first -- cpp:113 says the same in fewer words. */
	if (tr->allsolid || tr->startsolid)
		return false;

	if (fabs(tr->fraction) <= FLT_EPSILON)
		return false;

	if (!PMSrc_PlaneIsSane (tr->plane.normal))
		return false;

	stuck = PMSrc_TraceHull (tr->endpos, tr->endpos);
	if (stuck.startsolid || fabs(stuck.fraction - 1.0f) > FLT_EPSILON)
		return false;

	return true;
}

/*
==================
PMSrc_FindRecoveryPlane   (cpp:2486)

The last resort, when the move loop knows the player is stuck on a ramp and has
no plane to push away from -- not from this trace, not from any plane
accumulated earlier this tick.

Trace the intended move 27 times, once per combination of {-o, 0, +o} on each
axis, with the hull grown to match.  Sum every normal that comes back sane and
normalise the sum.  That average is deliberately not "the nearest surface": in
the case this exists for the player is wedged, several faces are touching at
once, and the direction that gets them out is the one they all agree on.

o is (bumpcount * 2) * pm_rampretrace, so each failed recovery searches wider
than the last.  bumpcount is at least 1 here -- the caller can only reach this
after a previous pass set stuck_on_ramp -- which matters, because at bumpcount 0
every offset would be zero and this would be the same trace 27 times.

The hull growth is Momentum's, asymmetric and odd-looking, and copied as-is
(cpp:2494-2509): half the offset on the trailing face and a quarter on the
leading one.  The point is only that the probe box is bigger than the player, so
it can find a face the player's own hull is already inside of.
==================
*/
static qboolean PMSrc_FindRecoveryPlane (vec3_t start, vec3_t end, int bumpcount, vec3_t out)
{
	vec3_t	savemins, savemaxs;
	vec3_t	offset, offmins, offmaxs;
	vec3_t	tstart, tend;
	float	offsets[3];
	float	retrace = PMSrc_RampRetrace ();
	int		i, j, h, k;
	int		found = 0;

	offsets[0] = (bumpcount * 2) * -retrace;
	offsets[1] = 0.0f;
	offsets[2] = (bumpcount * 2) *  retrace;

	VectorCopy (pmove.player_mins, savemins);
	VectorCopy (pmove.player_maxs, savemaxs);
	VectorClear (out);

	for (i = 0; i < 3; i++)
	for (j = 0; j < 3; j++)
	for (h = 0; h < 3; h++)
	{
		trace_t tr;

		offset[0] = offsets[i];
		offset[1] = offsets[j];
		offset[2] = offsets[h];

		for (k = 0; k < 3; k++)
		{
			offmins[k] = offset[k] * 0.5f;
			offmaxs[k] = offset[k] * 0.5f;
			if (offset[k] > 0)	offmins[k] *= 0.5f;
			if (offset[k] < 0)	offmaxs[k] *= 0.5f;

			pmove.player_mins[k] = savemins[k] - offmins[k];
			pmove.player_maxs[k] = savemaxs[k] + offmaxs[k];
		}

		VectorAdd (start, offset, tstart);
		VectorSubtract (end, offset, tend);

		tr = PMSrc_TraceHull (tstart, tend);

		/* Sane plane, and a real impact from a start that was not itself
		   inside a brush -- otherwise we would be averaging in the same
		   garbage normal we are here to escape.  cpp:2519. */
		if (PMSrc_PlaneIsSane (tr.plane.normal) &&
			tr.fraction > 0.0f && tr.fraction < 1.0f && !tr.startsolid)
		{
			found++;
			VectorAdd (out, tr.plane.normal, out);
		}
	}

	VectorCopy (savemins, pmove.player_mins);
	VectorCopy (savemaxs, pmove.player_maxs);

	if (!found || VectorLength (out) <= FLT_EPSILON)
	{
		VectorClear (out);
		return false;
	}

	VectorNormalize (out);
	return true;
}

/*
==================
PMSrc_TryPlayerMove   (cpp:2582)

Differences from FTE's PM_SlideMove that matter:

  * The airborne first-impact branch (numplanes == 1 && !onground) clips
    against a floor/ramp with overbounce 1.0 and against a wall with
    1.0 + sv_bounce*(1 - surfaceFriction).  PM_SlideMove always uses 1.
  * There is no "DotProduct(velocity, primal_velocity) <= 0 => stop dead"
    bail on the first-impact branch -- only on the multi-plane branch.  QW
    applies it unconditionally, which zeroes your speed in ramp corners.
  * allFraction == 0 (never moved at all) zeroes velocity, which is the
    correct anti-jitter guard and is not the same thing as the QW bail.

FTESurf Patch 177 -- the ramp fix (pm_fixrampbugs), cpp:2393.

Stock Source has exactly one answer when a movement trace comes back useless:
stop.  On a surf ramp a useless trace is not an error, it is Tuesday -- you are
a box riding a plane at a few hundred units a second, and a fraction of a unit
of overlap at a tick boundary is the normal case rather than the exceptional
one.  So "stop" becomes the dead stop on a seam, and the variant where the
recovery push happens to point the wrong way becomes the launch.

Momentum's loop carries three things stock Source does not:

  * fixed_origin, a working origin SEPARATE from pmove.origin.  Recovery nudges
    fixed_origin and re-traces from it; pmove.origin is only ever assigned from
    a trace that actually succeeded.  A failed recovery therefore costs a bump
    and nothing else -- it cannot move the player.
  * valid_plane / has_valid_plane, the plane to push away from, looked for in
    three places in order of how much we trust it: this trace's own normal, then
    the planes already accumulated this tick walked BACKWARDS (most recent
    first), then the 27-direction search.
  * stuck_on_ramp, which turns the next pass of the loop into a recovery pass.

Two of the gates below read backwards until you notice what they are for:

  * the unswept re-test now only runs on the FIRST bump (or on the ground, or
    with the fix off).  On later bumps PMSrc_IsValidMovementTrace already ran
    and already did that exact test, so the old code's version would be the
    second of two identical traces.
  * pm.allsolid no longer returns 4 while the fix is on.  Being in a solid is
    the state the recovery path exists to get out of; returning early is
    conceding it.

Everything here is bound to pm_fixrampbugs, INCLUDING the degenerate-crease
push, which Momentum applies unconditionally.  Same reason as Patch 176: 0 has
to be exactly the previous build or the cvar is not a bisection.
==================
*/
static int PMSrc_TryPlayerMove (vec3_t firstdest, trace_t *firsttrace)
{
	int			bumpcount;
	vec3_t		dir;
	float		d;
	int			numplanes;
	vec3_t		planes[PMSRC_MAX_CLIP_PLANES];
	vec3_t		primal_velocity, original_velocity;
	vec3_t		new_velocity;
	vec3_t		fixed_origin;
	vec3_t		valid_plane;
	int			i, j;
	trace_t		pm;
	vec3_t		end;
	float		time_left, allFraction;
	float		tookportal;		/*P203: fraction of this bump spent reaching a portal plane, 0 if none*/
	int			blocked = 0;
	float		standable = PMSrc_Standable();
	int			numbumps = PMSrc_BumpCount ();
	qboolean	fixramps = (movevars.fixrampbugs != 0);
	qboolean	stuck_on_ramp = false;		/* assume we are not stuck yet */
	qboolean	has_valid_plane = false;	/* no plane information gathered yet */

	numplanes = 0;
	VectorCopy (pmove.velocity, original_velocity);
	VectorCopy (pmove.velocity, primal_velocity);
	VectorCopy (pmove.origin, fixed_origin);
	VectorClear (new_velocity);
	VectorClear (valid_plane);

	/* Both of these are read by the recovery block from the PREVIOUS pass of
	   the loop, which cannot happen before a pass has run -- stuck_on_ramp
	   starts false.  Cleared anyway so the read is defined rather than merely
	   unreachable. */
	memset (&pm, 0, sizeof(pm));
	VectorClear (end);

	allFraction = 0;
	time_left = pms_frametime;
	tookportal = 0;
	pms_portalcrossed = false;	/*P203: per-CALL, so StepMove's second attempt starts clean*/

	for (bumpcount = 0; bumpcount < numbumps; bumpcount++)
	{
		if (!pmove.velocity[0] && !pmove.velocity[1] && !pmove.velocity[2])
			break;

		if (stuck_on_ramp && fixramps)
		{
			/* ---- FTESurf Patch 177: recovery  (cpp:2437) ---------------- */
			if (!has_valid_plane)
			{
				/* First choice: the normal the failed trace did give us.  A
				   startsolid trace usually has none, but when it has one it is
				   the face we are inside of, which is exactly the face to push
				   away from.  Refuse the plane we already tried, or we would
				   nudge along it forever. */
				if (!PMSrc_VecCloseEnough (pm.plane.normal, vec3_origin) &&
					!PMSrc_VecCloseEnough (valid_plane, pm.plane.normal))
				{
					VectorCopy (pm.plane.normal, valid_plane);
					has_valid_plane = true;
				}
				else
				{
					/* Second choice: a plane already clipped this tick, newest
					   first.  On a ramp seam the plane that stopped us is
					   usually the one we were riding a moment ago. */
					for (i = numplanes; i-- > 0; )
					{
						if (!PMSrc_VecCloseEnough (planes[i], vec3_origin) &&
							PMSrc_PlaneIsSane (planes[i]) &&
							!PMSrc_VecCloseEnough (valid_plane, planes[i]))
						{
							VectorCopy (planes[i], valid_plane);
							has_valid_plane = true;
							break;
						}
					}
				}
			}

			if (has_valid_plane)
			{
				/* Clip against it as if the collision had happened normally --
				   floor rules above the standable cut, wall rules below, the
				   same split the airborne first-impact branch uses. */
				if (valid_plane[2] >= standable && valid_plane[2] <= 1.0f)
					PMSrc_ClipVelocity (pmove.velocity, valid_plane, pmove.velocity, 1);
				else
					PMSrc_ClipVelocity (pmove.velocity, valid_plane, pmove.velocity,
						1.0f + movevars.bounce * (1 - pmove.surfacefriction));
				VectorCopy (pmove.velocity, original_velocity);
			}
			else
			{
				/* Third choice, and there is no fourth: search for one.  Note
				   the `continue` -- finding a plane costs this bump, and the
				   nudge below happens on the NEXT pass.  That is deliberate:
				   the search only tells us which way is out, and we want the
				   clip above applied before we move. */
				if (PMSrc_FindRecoveryPlane (fixed_origin, end, bumpcount, valid_plane))
				{
					has_valid_plane = true;
					continue;
				}
			}

			if (has_valid_plane)
			{	/* Out along the plane, and try again from there. */
				VectorMA (fixed_origin, PMSrc_RampRetrace (), valid_plane, fixed_origin);
			}
			else
			{	/* 27 traces found nothing.  Whatever this is, it is not a ramp
				   we can push off; give up on recovery and let the ordinary
				   path have the bump. */
				stuck_on_ramp = false;
				continue;
			}
		}

		VectorMA (fixed_origin, time_left, pmove.velocity, end);

		tookportal = 0;

		/* P203/P206: on a map with portals the cached first trace is never
		   reused.  It was taken with a plain hull sweep, so at best it knows the
		   aperture is there without knowing how to go through it -- and at worst
		   PM_PortalCSG already elongated it through the wall while leaving
		   entnum reading "world", in which case reusing it moves the player into
		   solid with no transform run.  Neither case is distinguishable after
		   the fact, so re-trace through the branch below that can traverse. */
		if (firstdest && VectorCompare (end, firstdest) && !pms_haveportals)
			pm = *firsttrace;
		else if (stuck_on_ramp && has_valid_plane && fixramps)
		{
			/* Re-trace from the nudged origin, and keep the plane we recovered
			   rather than whatever this trace reports -- the trace is here to
			   find out how far we get, not what we are standing against.
			   cpp:2569.  Deliberately NOT portal-aware: this is a recovery
			   probe from an origin we invented, and a traversal launched from an
			   invented position would be one the player never actually made. */
			pm = PMSrc_TraceHull (fixed_origin, end);
			VectorCopy (valid_plane, pm.plane.normal);
		}
		else
			pm = PM_PlayerTracePortals (pmove.origin, end, MASK_PLAYERSOLID, &tookportal);

		if (tookportal)
		{
			/* ---- FTESurf Patch 203: we went through ----------------------

			   PM_PlayerTracePortals has already transformed pmove.origin's
			   destination, rewritten pmove.angles and pmove.velocity into the
			   far side's frame, re-traced from there and validated the landing.
			   `pm` is that far-side trace.  Everything this loop was carrying
			   describes the near side and is now meaningless.

			   The origin is moved HERE rather than being left to the
			   `pm.fraction > 0` block below, because a crossing that emerges
			   flush against something has fraction 0 -- and then the block
			   never runs, the origin stays on the near side, and the player is
			   left behind holding somebody else's angles and velocity.

			   allFraction is credited with the portal fraction for the same
			   family of reasons: without it, a bump spent entirely on reaching
			   the portal plane looks like "never moved at all" to the guard at
			   the bottom of this function, which answers that by deleting the
			   player's velocity.  A surfer entering a portal at 1500 u/s would
			   arrive stopped.  PM_SlideMove has no analogue of this because it
			   has no allFraction. */
			VectorCopy (pm.endpos, pmove.origin);
			VectorCopy (pmove.origin, fixed_origin);

			time_left -= time_left * tookportal;
			allFraction += tookportal;

			VectorCopy (pmove.velocity, primal_velocity);
			VectorCopy (pmove.velocity, original_velocity);
			numplanes = 0;

			/* Patch 177's ramp state is all near-side too: a plane we were
			   riding a moment ago is not a plane we can push off now. */
			has_valid_plane = false;
			stuck_on_ramp = false;
			VectorClear (valid_plane);

			/* Cached once per tick at the top of PMSrc_PlayerMove, from angles
			   that have just changed. */
			AngleVectors (pmove.angles, pms_forward, pms_right, pms_up);

			pms_portalcrossed = true;
		}

		/* ---- FTESurf Patch 256: the displacement-seam probe -----------------

		   TEMPORARY, and gated at 0.  It exists to decide ONE question: when the
		   player snags walking across a displacement seam, is the trace stopping
		   on a phantom (a triangle's face plane applied outside the region where
		   it is valid, because BIH_ClipToTriangle builds 11 of the 16 separating
		   planes an AABB sweep needs), or on the four-unit slab that Patch 256's
		   winding fix removes?

		   Read it like this, walking a seam with pm_dispprobe 1:
		     - a clean 4-6 unit lip, normal (0,0,1), and it disappears with
		       hl2_dispwinding 0/1 -> the winding was the whole bug.
		     - fraction<1 with a normal whose z is between roughly 0.2 and 0.7
		       while the surface under you is much flatter than that -> the
		       missing bevels, and com_bih.c needs the other five planes.
		     - startsolid at all -> neither; say so, because the analysis says
		       it cannot happen from normal locomotion.

		   Only grounded and only when the sweep is actually stopped, or it
		   prints every frame of every jump. */
		if (pm_dispprobe.ival && pmove.onground && pm.fraction < 1)
		{
			Con_Printf ("[dispprobe] frac %.4f norm %.3f %.3f %.3f  ss %i as %i  "
			            "org %.1f %.1f %.1f -> %.1f %.1f %.1f\n",
			            pm.fraction, pm.plane.normal[0], pm.plane.normal[1],
			            pm.plane.normal[2], pm.startsolid, pm.allsolid,
			            pmove.origin[0], pmove.origin[1], pmove.origin[2],
			            pm.endpos[0], pm.endpos[1], pm.endpos[2]);
			PMSrc_DispProbeWhy (&pm);	//FTESurf Patch 258
		}

		/* Only from the second bump on, and only in the air.  On the first bump
		   the unswept re-test below covers it, and on the ground a bad trace is
		   a wall, not a ramp.  cpp:2616.
		   P203: and never on the bump that crossed a portal.  That trace can
		   legitimately have fraction 0 (emerging flush) and a plane belonging to
		   the far side's geometry; IsValidMovementTrace would read it as a ramp
		   failure and "recover" the player along a plane from the other room. */
		if (bumpcount && fixramps && !pmove.onground && !tookportal && !PMSrc_IsValidMovementTrace (&pm))
		{
			has_valid_plane = false;
			stuck_on_ramp = true;
			continue;
		}

		if (pm.allsolid && !fixramps)
		{	/* trapped in a solid */
			VectorClear (pmove.velocity);
			return 4;
		}

		if (pm.fraction > 0)
		{
			if ((!bumpcount || pmove.onground || !fixramps) && pm.fraction == 1)
			{
				/* A swept box can report a clean pass while the END position
				   is inside geometry (a precision issue in triangle-soup
				   tracing).  Re-test unswept; if we would end up stuck,
				   refuse the move rather than tunnel into the world. */
				trace_t stuck = PMSrc_TraceHull (pm.endpos, pm.endpos);
				if (stuck.startsolid || stuck.fraction != 1.0f)
				{
					/* Patch 177: on the first bump this is where a ramp bug
					   announces itself, and it is recoverable.  Everywhere else
					   it still means stop.  cpp:2654. */
					if (!bumpcount && fixramps)
					{
						has_valid_plane = false;
						stuck_on_ramp = true;
						continue;
					}
					VectorClear (pmove.velocity);
					break;
				}
			}

			if (fixramps)
			{	/* We moved, so nothing is stuck and last tick's plane is stale. */
				has_valid_plane = false;
				stuck_on_ramp = false;
			}

			VectorCopy (pmove.velocity, original_velocity);
			VectorCopy (pm.endpos, pmove.origin);
			VectorCopy (pmove.origin, fixed_origin);

			/* Patch 177: inside the fraction > 0 block, where Momentum has it
			   (cpp:2688).  It used to be counted at the trace, which meant a
			   bump spent entirely on a refused move still looked like progress
			   to the allFraction == 0 guard at the bottom -- so a player who
			   never moved at all kept their velocity, and on a ramp that
			   velocity accumulates without bound. */
			allFraction += pm.fraction;
			numplanes = 0;
		}

		if (pm.fraction == 1)
			break;		/* moved the entire distance */

		if (pm.entnum > 0)
			PM_AddTouchedEnt (pm.entnum);

		if (pm.plane.normal[2] > standable)
			blocked |= 1;		/* floor */
		if (fabs (pm.plane.normal[2]) <= FLT_EPSILON)
			blocked |= 2;		/* step / wall -- epsilon, see PMSrc_ClipVelocity */

		/* Report surfing: we clipped against something too steep to stand
		   on but not a vertical wall. */
		if (pm.plane.normal[2] > 0.05f && pm.plane.normal[2] < standable)
			pms_surfed = true;

		/* FTESurf board telemetry (Patch 131).

		   A "ramp" is the band a surfer can actually ride: standable is the
		   top (0.7, Source's own cut -- above it you are simply standing) and
		   PMSRC_MIN_RAMP_NZ the bottom (0.1, ~84 deg, above which it is a
		   wall you hit rather than a face you board).  fabs because a
		   displacement triangle's winding can face down while being the
		   surface you ride.

		   Recording the plane and the PRE-CLIP velocity here is the whole
		   point: one line further down PMSrc_ClipVelocity destroys exactly
		   the component we want to report, and no amount of after-the-fact
		   tracing recovers which plane took it. */
		{
			float nz = fabs(pm.plane.normal[2]);
			if (nz >= PMSRC_MIN_RAMP_NZ && nz <= standable)
			{
				pmove.rampcontact = 1;

				/* Patch 137: the plane THIS tick clipped, unlatched.  The
				   board fields below only update on a new arrival, so on a
				   long ride down a curving ramp they go stale -- which is
				   fine for grading the landing and wrong for telling the
				   strafe bar which plane it is on now. */
				VectorCopy (pm.plane.normal, pmove.rampnormal);

				/* A NEW board, not another tick of the same ride.  Sustained
				   surfing clips every tick -- gravity keeps pushing into the
				   face -- so an un-gated edge test would grade a ride as
				   thousands of perfect landings. */
				if (pmove.rampoff >= PMSRC_BOARD_AIRGATE)
				{
					VectorCopy (pm.plane.normal, pmove.boardnormal);
					VectorCopy (original_velocity, pmove.boardvelocity);
					pmove.boardcount += 1;
				}
				pmove.rampoff = 0;
			}
		}

		time_left -= time_left * pm.fraction;

		if (numplanes >= PMSRC_MAX_CLIP_PLANES)
		{	/* shouldn't happen */
			VectorClear (pmove.velocity);
			break;
		}

		VectorCopy (pm.plane.normal, planes[numplanes]);
		numplanes++;

		if (numplanes == 1 && pmove.pm_type == PM_NORMAL && !pmove.onground)
		{
			/* Airborne first impact.  This is the surf path. */
			for (i = 0; i < numplanes; i++)
			{
				if (planes[i][2] > standable)
				{	/* floor or shallow slope */
					PMSrc_ClipVelocity (original_velocity, planes[i], new_velocity, 1);
					VectorCopy (new_velocity, original_velocity);
				}
				else
				{
					PMSrc_ClipVelocity (original_velocity, planes[i], new_velocity,
						1.0f + movevars.bounce * (1 - pmove.surfacefriction));
				}
			}
			VectorCopy (new_velocity, pmove.velocity);
			VectorCopy (new_velocity, original_velocity);
		}
		else
		{
			for (i = 0; i < numplanes; i++)
			{
				PMSrc_ClipVelocity (original_velocity, planes[i], pmove.velocity, 1);

				for (j = 0; j < numplanes; j++)
					if (j != i)
					{
						if (DotProduct (pmove.velocity, planes[j]) < 0)
							break;	/* not ok */
					}
				if (j == numplanes)
					break;
			}

			if (i != numplanes)
			{	/* go along this plane -- velocity already set by the clip */
				;
			}
			else
			{	/* go along the crease */
				if (numplanes != 2)
				{
					VectorClear (pmove.velocity);
					break;
				}

				/* ---- FTESurf Patch 177: vertical rampbug  (cpp:2856) ----

				   Two clip planes and they are the SAME plane.  That is not a
				   crease, it is a surfer who has sunk into the ramp: the face
				   did not push them out far enough, so the next trace hits it
				   again and the loop believes it is wedged between two
				   surfaces.  The cross product of a vector with itself is zero,
				   so the crease code below would set velocity to nothing and
				   the ride would end in a dead stop, mid-ramp, for no reason
				   the player can see.

				   Push away from the face instead, and only in x/y -- z belongs
				   to gravity, and adding to it here would be a free boost that
				   scales with how badly the player happened to clip. */
				if (fixramps && PMSrc_VecCloseEnough (planes[0], planes[1]))
				{
					VectorMA (original_velocity, PMSRC_RAMP_UNSTICK_PUSH, planes[0], new_velocity);
					pmove.velocity[0] = new_velocity[0];
					pmove.velocity[1] = new_velocity[1];
					break;
				}

				CrossProduct (planes[0], planes[1], dir);
				VectorNormalize (dir);
				d = DotProduct (dir, pmove.velocity);
				VectorScale (dir, d, pmove.velocity);
			}

			/* If the new velocity opposes where we started, stop dead --
			   this avoids tiny oscillations in sloping corners.  Note this
			   is inside the multi-plane branch ONLY. */
			d = DotProduct (pmove.velocity, primal_velocity);
			if (d <= 0)
			{
				VectorClear (pmove.velocity);
				break;
			}
		}
	}

	if (allFraction == 0)
		VectorClear (pmove.velocity);

	return blocked;
}

/*
==================
PMSrc_StayOnGround   (cpp:1870)

Keeps you glued to the floor going downhill instead of taking a series of
tiny hops.  Traces up 2 first so a slight overlap doesn't defeat the
downward trace.
==================
*/
static void PMSrc_StayOnGround (void)
{
	trace_t trace;
	vec3_t start, end;

	/* Momentum's sv_snap_to_ground / mom_mv_snap_to_ground, "move the player to
	   exactly ground level when considered standing on the ground".  Ships at
	   1 there and here; off, you take a series of tiny hops downhill instead of
	   staying glued, which also means CategorizePosition sees you leave the
	   ground and come back. */
	if (!movevars.snaptoground)
		return;

	VectorCopy (pmove.origin, start);
	VectorCopy (pmove.origin, end);
	start[2] += 2;
	end[2] -= movevars.stepheight;

	/* See how far up we can go without getting stuck. */
	trace = PMSrc_TraceHull (pmove.origin, start);
	VectorCopy (trace.endpos, start);

	/* Now trace down from a known safe position. */
	trace = PMSrc_TraceHull (start, end);

	if (trace.fraction > 0.0f &&			/* must go somewhere */
		trace.fraction < 1.0f &&			/* must hit something */
		!trace.startsolid &&				/* can't be embedded */
		trace.plane.normal[2] >= PMSrc_Standable())	/* not a steep face */
	{
		/* FTESurf Patch 172: 1/64, not half of it.

		   Source's test is `delta > 0.5f * COORD_RESOLUTION` and
		   COORD_RESOLUTION is 1/32, so the threshold is 1/64.  We had written
		   0.5f * (1/64), which is 1/128 -- half the distance, so we snapped in
		   roughly twice as many cases as Source does.  Every one of those
		   extra snaps is an origin move that nothing asked for, on ground you
		   were already close enough to. */
		float delta = fabs (pmove.origin[2] - trace.endpos[2]);
		if (delta > (1.0f / 64.0f))
			VectorCopy (trace.endpos, pmove.origin);
	}
}

/*
==================
PMSrc_StepMove   (cpp:1549)

Try the move flat, then try it stepped up 18 units and back down, and keep
whichever travelled further horizontally.  When the stepped result wins, the
vertical velocity is taken from the FLAT attempt -- otherwise stepping would
inject upward velocity you never asked for.
==================
*/
static void PMSrc_StepMove (vec3_t vecDestination, trace_t *trace)
{
	vec3_t vecEndPos;
	vec3_t vecPos, vecVel;
	vec3_t vecDownPos, vecDownVel;
	vec3_t vecUpPos;
	trace_t t;
	float flDownDist, flUpDist;

	VectorCopy (vecDestination, vecEndPos);
	VectorCopy (pmove.origin, vecPos);
	VectorCopy (pmove.velocity, vecVel);

	/* Slide move down (the flat attempt). */
	PMSrc_TryPlayerMove (vecEndPos, trace);

	/* FTESurf Patch 203.  A portal was traversed, so pmove.origin is in the
	   other room and pmove.angles has been rotated.  Everything below this line
	   would undo that: the stepped attempt restores origin and velocity from
	   vecPos/vecVel and runs the move a SECOND time -- through the same portal,
	   rotating the view again -- and then picks a winner by comparing horizontal
	   distance travelled, which across a portal is not a comparison of anything.
	   Take the crossing and stop. */
	if (pms_portalcrossed)
		return;

	VectorCopy (pmove.origin, vecDownPos);
	VectorCopy (pmove.velocity, vecDownVel);

	/* Reset and try again, stepped up. */
	VectorCopy (vecPos, pmove.origin);
	VectorCopy (vecVel, pmove.velocity);

	VectorCopy (pmove.origin, vecEndPos);
	vecEndPos[2] += movevars.stepheight + PMSRC_DIST_EPSILON;

	t = PMSrc_TraceHull (pmove.origin, vecEndPos);
	if (!t.startsolid && !t.allsolid)
		VectorCopy (t.endpos, pmove.origin);

	PMSrc_TryPlayerMove (NULL, NULL);

	/* P203: same as above, for the stepped attempt.  The step-down trace that
	   follows would drag the player 18 units toward whatever is under them on
	   the far side of the portal, and the flat attempt's saved position is on
	   the near side, so neither branch of the comparison below is meaningful. */
	if (pms_portalcrossed)
		return;

	/* Move back down a stair. */
	VectorCopy (pmove.origin, vecEndPos);
	vecEndPos[2] -= movevars.stepheight + PMSRC_DIST_EPSILON;

	t = PMSrc_TraceHull (pmove.origin, vecEndPos);

	/* Patch 256's probe, second half.  This is where a phantom actually costs
	   the step: the analysis says the plane here is NOT zeroed by a startsolid,
	   it is a real face normal belonging to a triangle whose footprint the
	   player is not over, and it reads as too steep to stand on.  If the probe
	   shows normal.z sitting between ~0.2 and 0.7 while the terrain underfoot is
	   nearly flat, that is the missing bevels and not the winding. */
	if (pm_dispprobe.ival && t.plane.normal[2] < PMSrc_Standable())
	{
		Con_Printf ("[dispprobe] step-down REJECTED: norm %.3f %.3f %.3f "
		            "(need z >= %.3f)  ss %i\n",
		            t.plane.normal[0], t.plane.normal[1], t.plane.normal[2],
		            PMSrc_Standable(), t.startsolid);
		PMSrc_DispProbeWhy (&t);	//FTESurf Patch 258
	}

	/* If we didn't land on something standable, the step was pointless. */
	if (t.plane.normal[2] < PMSrc_Standable())
	{
		VectorCopy (vecDownPos, pmove.origin);
		VectorCopy (vecDownVel, pmove.velocity);
		return;
	}

	if (!t.startsolid && !t.allsolid)
		VectorCopy (t.endpos, pmove.origin);

	VectorCopy (pmove.origin, vecUpPos);

	/* Decide which attempt went further, horizontally. */
	flDownDist = (vecDownPos[0] - vecPos[0]) * (vecDownPos[0] - vecPos[0]) +
	             (vecDownPos[1] - vecPos[1]) * (vecDownPos[1] - vecPos[1]);
	flUpDist   = (vecUpPos[0] - vecPos[0]) * (vecUpPos[0] - vecPos[0]) +
	             (vecUpPos[1] - vecPos[1]) * (vecUpPos[1] - vecPos[1]);

	if (flDownDist > flUpDist)
	{
		VectorCopy (vecDownPos, pmove.origin);
		VectorCopy (vecDownVel, pmove.velocity);
	}
	else
	{	/* keep the stepped position, but the flat move's Z velocity */
		pmove.velocity[2] = vecDownVel[2];
	}
}

/* ------------------------------------------------------------------ hulls */

/*
  Hull ownership.

  FTE normally takes pmove.player_mins/maxs straight from the entity
  (sv_user.c:7752-7758), but duck lives in here now, so the engine has to own
  the height.  Taking the standing height from the entity would be circular:
  the server resizes the edict to match our duck state, so next tick the
  entity would report the DUCKED hull as if it were the standing one.

  So: width and floor come from the entity (a mod may legitimately change
  those), height comes from movevars.  Stateless, and correct no matter what
  the entity currently says.
*/
static float PMSrc_StandHeight (void)
{
	return movevars.standheight > 0 ? movevars.standheight : 62;
}

static float PMSrc_DuckHeight (void)
{
	return movevars.duckheight > 0 ? movevars.duckheight : 45;
}

/*
==================
PMSrc_ApplyStandHull / PMSrc_ApplyHull

WIDTH AND FLOOR COME FROM THE ENTITY, HEIGHT FROM MOVEVARS -- AND THE HEIGHT MUST
BE RECOMPUTED EVERY TIME, NEVER COPIED.

pms_standmaxs is captured raw from the entity at the top of PMSrc_PlayerMove, and
the entity is whatever the LAST move left it as: sv_user.c:8134-8137 writes the
ducked hull back onto the edict, and sv_user.c:7926-7932 reads it in again next
command.  So while you are ducked, pms_standmaxs[2] is the DUCKED height wearing
the standing name.

PMSrc_ApplyHull always survived that because it overwrites [2] itself.
PMSrc_CanUnduck did not -- it copied pms_standmaxs whole, so the "can I fit if I
stand up?" trace ran with the box you were already standing in, always fitted, and
you stood straight into the ceiling.  The forced-crouch branch in PMSrc_Duck is a
correct port of Source's and had simply never been reachable.

Hence one function that both callers use.  Momentum sidesteps the whole problem by
passing the literal VEC_HULL_MIN/VEC_HULL_MAX constants to UTIL_TraceHull
(mom_gamemovement.cpp:736); this is the same idea with the size still owned by the
mod rather than the engine.
==================
*/
static void PMSrc_ApplyStandHull (void)
{
	VectorCopy (pms_standmins, pmove.player_mins);
	VectorCopy (pms_standmaxs, pmove.player_maxs);

	/* The WIDTH is the mod's.  The vertical extent is not: in Source the origin
	   is the feet, so the box runs 0..standheight and neither end is inherited.
	   Deriving the top from an inherited bottom is what let a stray mins.z=-16
	   move the whole box down 16 and put the eye at 80 -- see the pin and the
	   warning in PMSrc_PlayerMove for how that happened and how it is caught. */
	pmove.player_mins[2] = 0;
	pmove.player_maxs[2] = PMSrc_StandHeight();
}

/* Apply the hull matching the current duck state to pmove. */
static void PMSrc_ApplyHull (void)
{
	PMSrc_ApplyStandHull ();
	if (pmove.ducked)
		pmove.player_maxs[2] = pmove.player_mins[2] + PMSrc_DuckHeight();
}

/* How much SHORTER the ducked box is: 62 - 45 = 17. */
static float PMSrc_HullDelta (void)
{
	return PMSrc_StandHeight() - PMSrc_DuckHeight();
}

/*
  How far the ORIGIN moves when the duck state flips in MID-AIR.

  Momentum's IGameMode::GetViewScale() is 0.5 for every CS-based mode
  (mom_system_gamemode.h:57 -- CGameMode_Surf does not override it).

  Momentum: viewDelta = GetViewScale() * (hullSizeNormal - hullSizeCrouch), applied
  in FinishDuck (mom_gamemovement.cpp:1114) and negated in FinishUnDuck (:1076) and
  CanUnduck (:733).  At 0.5 and a 17-unit delta that is 8.5, so the box loses 8.5
  off the bottom and 8.5 off the top and the head comes DOWN as the feet come up.

  Build 40 and earlier used the whole delta here, which kept the head still and
  lifted the feet by 18.  That was read off CGameMovement, the base class, which
  has no view scale -- and it is why the report "when you're in the air and duck,
  you lose hull from the top and bottom" was answered wrongly in Patch 241.  The
  report was right.
*/
static float PMSrc_AirDuckShift (void)
{
	float scale = movevars.viewscale;
	if (scale <= 0 || scale > 1)
		scale = 0.5f;
	return scale * PMSrc_HullDelta ();
}

/* -------------------------------------------------------------- eye height */

static float PMSrc_ViewHeight (void)
{
	return movevars.viewheight > 0 ? movevars.viewheight : PMSRC_VIEWHEIGHT_DEFAULT;
}

static float PMSrc_DuckViewHeight (void)
{
	return movevars.duckviewheight > 0 ? movevars.duckviewheight : PMSRC_DUCKVIEWHEIGHT_DEFAULT;
}

/*
==================
PMSrc_DuckFraction / PMSrc_SetDuckedEyeOffset
  (cpp:4258, CGameMovement::SetDuckedEyeOffset)

fraction 0 == standing, 1 == fully ducked.  The eye is the ONLY thing that
moves smoothly during a crouch: the hull snaps at the end of the transition,
the eye slides for the whole of it.  That slide is the camera smoothing you
feel in CS:S when you tap crouch.

WHY THIS IS DERIVED RATHER THAN WRITTEN AT EACH TRANSITION
----------------------------------------------------------
Source calls SetDuckedEyeOffset from four places inside Duck().  Doing the
same here would break at high framerates: this module runs a FIXED 66 Hz tick
and carries the remainder, so at 300 fps most usercmds run ZERO ticks -- and
on those the eye would keep whatever value the last command left, or worse,
whatever the entry seed wrote.  The result is visible jitter for the whole
0.4 s of the crouch.

So the fraction is computed as a pure function of the carried duck state
(ducktime + ducking + ducked + oldbuttons) once per move, after the ticks.
Same numbers, same spline, but the answer no longer depends on how many ticks
happened to fit in this command.

The three-state read needs oldbuttons, not just `ducked`: releasing crouch
PART WAY DOWN leaves ducking=true and ducked=false while running the UNDUCK
timeline (Duck() inverts the remaining time for exactly this case), so `ducked`
alone cannot tell a duck-in from a duck-out.

Source also subtracts `fMore = duckHullMin.z - standHullMin.z` from the ducked
offset, because in HL2 the ducked hull's floor is not at the same height as the
standing one.  In the Source player convention both mins.z are 0 (the origin is
at the feet), so fMore is 0 here and the term is dropped rather than carried as
a constant zero.
==================
*/
static float PMSrc_DuckFraction (void)
{
	float secs;

	if (!pmove.ducking)
		return pmove.ducked ? 1.0f : 0.0f;

	secs = (PMSRC_DUCK_TIMER - pmove.ducktime) * 0.001f;
	if (secs < 0)
		secs = 0;

	if (pmove.oldbuttons & BUTTON_DUCK)		/* going down */
		return PMSrc_SimpleSpline (secs / PMSRC_TIME_TO_DUCK);

	return PMSrc_SimpleSpline (1.0f - (secs / PMSRC_TIME_TO_UNDUCK));
}

static void PMSrc_SetDuckedEyeOffset (float fraction)
{
	pmove.viewheight = (PMSrc_DuckViewHeight() * fraction) +
	                   (PMSrc_ViewHeight() * (1.0f - fraction));
}

/* -------------------------------------------------------------- categorize */

/*
  FTESurf Patch 176.  Source's trace_t::DidHit() is
  `fraction < 1 || allsolid || startsolid`, NOT `fraction < 1`.  The two
  disagree in exactly the case the edge fix below cares about -- a trace that
  begins inside a brush reports fraction 1 and has certainly hit something --
  so the distinction is load-bearing here even though it is invisible
  elsewhere in this file.
*/
static qboolean PMSrc_TraceDidHit (trace_t *tr)
{
	return (tr->fraction < 1.0f || tr->allsolid || tr->startsolid);
}

/*
  The velocity the NEXT tick will start from, before it collides with
  anything: PMSrc_StartGravity's half step.  Pure -- no globals, no traces --
  so pm_selftest can pin it with no map loaded.
*/
static void PMSrc_NextTickVelocity (vec3_t vel, float dt, float gravity,
                                    float entgravity, vec3_t out)
{
	if (!entgravity)
		entgravity = 1.0f;
	VectorCopy (vel, out);
	out[2] -= entgravity * gravity * 0.5f * dt;
}

/*
==================
PMSrc_SlopeLandingGains   (cpp:1857)

The slope rule, factored out of PMSrc_CategorizePosition so that the one part
of Patch 176 that is pure arithmetic can be tested without a map.

`nextvel` is next tick's velocity (PMSrc_NextTickVelocity, possibly already
clipped once by the edge fix -- see the note there).  This clips it against
the candidate landing plane into `out`, and returns whether that collision
GAINS horizontal speed over `curvel`.

That single comparison is both of Momentum's slope cvars at once:

  * downhill, the collision converts fall into forward motion, |out.xy| rises,
    and we adopt it.  That is mom_mv_fix_downhill_slopes, "always collide and
    gain horizontal speed".
  * uphill, the collision is a tax -- stock Source robs you of speed you
    already had -- so we decline it and keep what we came in with.  That is
    mom_mv_fix_uphill_slopes, "land instead of colliding if beneficial".

The plan wrote this as (vel, normal, dt, gravity); it did not survive contact
with the reference, because when the edge fix runs and its fall trace hits,
cpp:1792 clips vecNextVelocity a FIRST time against the fall plane and the
half step of gravity has already been spent.  Recomputing it here would apply
gravity twice on exactly the frames the edge fix exists for.  So the caller
owns the gravity step and this owns the clip.
==================
*/
static qboolean PMSrc_SlopeLandingGains (vec3_t nextvel, vec3_t curvel,
                                         vec3_t normal, vec3_t out)
{
	float after, before;

	PMSrc_ClipVelocity (nextvel, normal, out, 1.0f);

	after  = out[0]    * out[0]    + out[1]    * out[1];
	before = curvel[0] * curvel[0] + curvel[1] * curvel[1];

	return after > before;
}

/*
==================
PMSrc_WouldEdgebug   (cpp:1761)

Momentum's sv_edge_fix.  Answers one question: if we DON'T ground the player
now, would they still be on the ground one tick from now?

An edgebug is landing on the very lip of a ramp and sliding off it in the same
tick, so the game grounds you, zeroes your fall, and then drops you again --
free speed, and stock Source hands it out or withholds it depending on where
in the tick you happened to arrive.  That is the RNG.  Simulating the tick
forward removes it in both directions: a genuine edgebug becomes reliable
rather than lucky, and an accidental one stops happening at all.

Three traces, and only on the frame a landing is being considered:

  1. the fall -- where the next tick's velocity would carry us.  Skipped when
     the ground trace already reported fraction 0, i.e. we are flush against
     the surface already;
  2. the slide -- clip against what we fell into, then run the FULL tick along
     the surface.  cpp:1794 is explicit that this uses the whole frametime
     rather than the remainder, so a max-distance edgebug is consistent;
  3. the ground test under wherever that ended up.

`nextvel` is updated by step 2's clip, exactly as cpp:1792 does, because the
caller then clips it a second time against the landing plane.

Dropped from the reference deliberately: cpp:1774's GetInteraction(0) branch,
which reuses the collision this player already recorded this tick when the
ground trace came back flush.  We keep no per-tick collision record and adding
one for this would be a large change for one fallback; the reference's own
third branch (cpp:1783, `pmFall = pm`) is what it uses when that record is
absent, and that is what we use always.
==================
*/
static qboolean PMSrc_WouldEdgebug (trace_t *ground, vec3_t nextvel)
{
	trace_t	fall, slide, under;
	vec3_t	end, gpoint;

	if (ground->fraction != 0.0f)
	{
		VectorMA (pmove.origin, pms_frametime, nextvel, end);
		fall = PMSrc_TraceHull (pmove.origin, end);
	}
	else
		fall = *ground;

	if (!PMSrc_TraceDidHit (&fall))
		return true;			/* misses the ground entirely next tick */

	PMSrc_ClipVelocity (nextvel, fall.plane.normal, nextvel, 1.0f);

	VectorMA (fall.endpos, pms_frametime, nextvel, end);
	slide = PMSrc_TraceHull (pmove.origin, end);

	VectorCopy (slide.endpos, gpoint);
	gpoint[2] -= PMSrc_GroundTraceDist ();
	under = PMSrc_TraceHull (slide.endpos, gpoint);

	return !PMSrc_TraceDidHit (&under);
}

/*
==================
PMSrc_CategorizePosition   (cpp:3786)

Ground detection, and therefore surf detection.  Four things differ from
PM_CategorizePosition:

  * The "moving up too fast to be on ground" threshold is 140, not 180.
  * A failed ground trace retries in four quadrants, so standing on the
    corner of a ledge still counts as ground.
  * surfaceFriction drops to 0.25 while airborne and moving upward, which
    feeds back into Accelerate/AirAccelerate.
  * (Patch 176) finding a standable plane is not the same as landing on it.
    See the landing decision below.
==================
*/
static void PMSrc_CategorizePosition (void)
{
	vec3_t		point;
	trace_t		pm;
	float		standable = PMSrc_Standable();
	int			cont;

	/* Reset each time we recategorize, otherwise we carry bogus friction. */
	pmove.surfacefriction = 1.0f;

	VectorCopy (pmove.origin, point);
	point[2] -= PMSrc_GroundTraceDist ();

	if (pmove.velocity[2] > PMSRC_NON_JUMP_VELOCITY)
	{
		pmove.onground = false;
		VectorClear (pmove.groundnormal);
	}
	else
	{
		pm = PMSrc_TraceHull (pmove.origin, point);

		if (movevars.groundquadrants && (pm.fraction == 1.0f || pm.plane.normal[2] < standable))
		{
			/* Try four sub-boxes, in case a shallower slope we COULD stand
			   on is under one corner of the hull.  cpp:3859.

			   This is Momentum's TryTouchGroundInQuadrants
			   (mom_gamemovement.cpp:1737) and today's
			   mom_mv_check_ground_quadrants -- "when on uneven ground, check
			   if any sub-quadrant of the player's bounding box is on ground
			   that can be stood on".  We have always done it; Patch 172 only
			   makes it switchable, so it can be taken out of the picture when
			   something else is being bisected. */
			trace_t best = pm;
			vec3_t savemins, savemaxs;
			vec3_t qmins, qmaxs;
			int q;
			float halfx, halfy;

			VectorCopy (pmove.player_mins, savemins);
			VectorCopy (pmove.player_maxs, savemaxs);
			halfx = (savemaxs[0] - savemins[0]) * 0.5f;
			halfy = (savemaxs[1] - savemins[1]) * 0.5f;

			for (q = 0; q < 4; q++)
			{
				VectorCopy (savemins, qmins);
				VectorCopy (savemaxs, qmaxs);
				if (q & 1)	qmins[0] += halfx; else qmaxs[0] -= halfx;
				if (q & 2)	qmins[1] += halfy; else qmaxs[1] -= halfy;

				VectorCopy (qmins, pmove.player_mins);
				VectorCopy (qmaxs, pmove.player_maxs);

				pm = PMSrc_TraceHull (pmove.origin, point);
				if (pm.fraction < 1.0f && pm.plane.normal[2] >= standable)
				{
					best = pm;
					break;
				}
			}

			VectorCopy (savemins, pmove.player_mins);
			VectorCopy (savemaxs, pmove.player_maxs);
			pm = best;
		}

		if (pm.fraction == 1.0f || pm.plane.normal[2] < standable)
		{
			pmove.onground = false;
			VectorCopy (pm.plane.normal, pmove.groundnormal);

			/* Moving up while airborne: quarter friction.  cpp:3868. */
			if (pmove.velocity[2] > 0.0f)
				pmove.surfacefriction = 0.25f;
		}
		else
		{
			/* ---- FTESurf Patch 176: the landing decision  (cpp:1750) ----

			   A standable plane is under us.  Stock Source grounds on it, full
			   stop, and that is the single largest source of surf RNG: whether
			   you land on a ramp or keep riding it is decided by where inside
			   a 1.5-unit band the tick boundary happened to fall.

			   Momentum decides it by looking one tick ahead instead.  Both
			   halves below run ONLY on the airborne -> grounded transition;
			   once you are standing, nothing here is reachable, which is why
			   walking and prestrafe are untouched by this patch.

			   This is cpp:1847-1864, the sv_rngfix_enable == 0 arm.  The other
			   arm (cpp:1812) skips the 140 test entirely on modes that allow
			   bhop, which for us would mean pm_autobunny silently disabling
			   half the fix; rngfix ships off in this tree and that arm is the
			   less tested of the two, so this is the one to port. */
			qboolean wasonground = pmove.onground;
			qboolean grounded = true;

			if (!wasonground && (movevars.fixslopes || movevars.fixedges))
			{
				vec3_t	nextvel, clipped;
				qboolean gains;

				PMSrc_NextTickVelocity (pmove.velocity, pms_frametime,
					movevars.gravity, movevars.entgravity, nextvel);

				/* Holding jump with autobunny means the player has already
				   asked not to be on the ground next tick, so simulating a
				   landing to decide whether to allow one is meaningless.
				   cpp:1761's HasAutoBhop() && IN_JUMP. */
				if (movevars.fixedges &&
					!(movevars.autobunny && (pmove.cmd.buttons & BUTTON_JUMP)))
				{
					if (PMSrc_WouldEdgebug (&pm, nextvel))
						grounded = false;
				}

				if (movevars.fixslopes)
				{
					gains = PMSrc_SlopeLandingGains (nextvel, pmove.velocity,
					                                 pm.plane.normal, clipped);

					/* A plane that would still be throwing us upward at more
					   than 140 u/s next tick is a ramp we are riding, not a
					   floor we have landed on.  cpp:1853.

					   Momentum leaves this test OUTSIDE sv_slope_fix, so it
					   applies even with every fix off.  We bind it to
					   pm_fixslopes on purpose: these cvars exist so a
					   regression can be bisected against the build before
					   them, and `pm_fixslopes 0 pm_fixedges 0` has to be
					   exactly that build -- stock Source, ground on the first
					   standable plane -- or it is not a bisection.  The
					   Momentum-faithful setting is the default, 1. */
					if (clipped[2] > PMSRC_NON_JUMP_VELOCITY)
						grounded = false;

					/* Adopt only if we are actually landing.  Declining the
					   landing and keeping the collision's velocity would be
					   the worst of both: cpp:1857 is inside the accept. */
					else if (grounded && gains)
						VectorCopy (clipped, pmove.velocity);
				}
			}

			if (!grounded)
			{
				/* Declined.  Stay airborne against the plane we are touching.
				   Note the reference does NOT drop surfacefriction to 0.25
				   here -- that is the no-standable-plane case above, and there
				   is a real surface under us. */
				pmove.onground = false;
				VectorCopy (pm.plane.normal, pmove.groundnormal);
			}
			else
			{
				pmove.onground = !pm.startsolid;
				pmove.groundent = pm.entnum;
				VectorCopy (pm.plane.normal, pmove.groundnormal);
				pmove.waterjumptime = 0;

				if (pm.entnum > 0)
					PM_AddTouchedEnt (pm.entnum);
			}
		}
	}

	/* Water level, same scheme as PM_CategorizePosition. */
	pmove.waterlevel = 0;
	pmove.watertype = FTECONTENTS_EMPTY;

	VectorCopy (pmove.origin, point);
	point[2] = pmove.origin[2] + pmove.player_mins[2] + 1;
	cont = PM_PointContents (point);
	if (cont & FTECONTENTS_FLUID)
	{
		pmove.watertype = cont;
		pmove.waterlevel = 1;
		point[2] = pmove.origin[2] + (pmove.player_mins[2] + pmove.player_maxs[2]) * 0.5f;
		cont = PM_PointContents (point);
		if (cont & FTECONTENTS_FLUID)
		{
			pmove.waterlevel = 2;
			point[2] = pmove.origin[2] + pmove.player_maxs[2] - 2;
			cont = PM_PointContents (point);
			if (cont & FTECONTENTS_FLUID)
				pmove.waterlevel = 3;
		}
	}
}

/* ------------------------------------------------------------------- duck */

static qboolean PMSrc_CanUnduck (void)
{
	trace_t trace;
	vec3_t newOrigin;
	vec3_t savemins, savemaxs;
	qboolean ok;

	VectorCopy (pmove.origin, newOrigin);

	if (!pmove.onground)
	{
		/* Standing up in mid-air drops the origin back down by the same amount
		   ducking raised it, so test the move that would actually happen. */
		newOrigin[2] -= PMSrc_AirDuckShift ();
	}

	/* Test with the STANDING hull -- that is what we would become.  This MUST go
	   through PMSrc_ApplyStandHull: pms_standmaxs[2] is whatever the entity last
	   reported, which is the DUCKED height for the whole time this function is
	   worth calling.  See the comment on PMSrc_ApplyStandHull. */
	VectorCopy (pmove.player_mins, savemins);
	VectorCopy (pmove.player_maxs, savemaxs);
	PMSrc_ApplyStandHull ();

	trace = PMSrc_TraceHull (pmove.origin, newOrigin);
	ok = !(trace.startsolid || trace.fraction != 1.0f);

	VectorCopy (savemins, pmove.player_mins);
	VectorCopy (savemaxs, pmove.player_maxs);

	return ok;
}

static void PMSrc_FinishDuck (void)
{
	if (pmove.ducked)
		return;

	pmove.ducked = true;
	pmove.ducking = false;

	/* On the ground the feet stay put (both hulls have mins.z == 0 in the
	   Source convention, so the ground adjustment is a no-op).  In mid-air
	   the origin comes UP by the view-scaled half of the hull difference --
	   this is the crouch-jump, and at 0.5 the box also loses the other half
	   off the TOP. */
	if (!pmove.onground)
		pmove.origin[2] += PMSrc_AirDuckShift ();

	/* In mid-air the eye SNAPS here.  Unlike build 40, that IS visible: the
	   origin rises 8.5 while the eye offset drops the full 17, so the absolute
	   eye falls by 8.5 -- exactly as the head does.  Momentum behaves the same
	   way and for the same reason.  PMSrc_DuckFraction picks the value up from
	   the state at the end of the move. */
	PMSrc_ApplyHull ();
	PMSrc_CategorizePosition ();
}

static void PMSrc_FinishUnDuck (void)
{
	if (!pmove.onground)
		pmove.origin[2] -= PMSrc_AirDuckShift ();

	pmove.ducked = false;
	pmove.ducking = false;
	pmove.ducktime = 0;

	PMSrc_ApplyHull ();
	PMSrc_CategorizePosition ();
}

/*
  The maxspeed this tick actually runs at, after every modifier Source applies
  before CheckParameters.  Both of them scale m_flMaxSpeed rather than the move
  values, so both are in force for CheckParameters' clamp as well as for
  Accelerate -- which is the whole reason they belong here and not later.

  Stamina: cs_gamemovement.cpp does this in CCSGameMovement::PlayerMove, ahead
  of BaseClass::PlayerMove.

  Walk (FTESurf Patch 171): CS:S walks by REDUCING m_flMaxSpeed by
  CS_PLAYER_SPEED_WALK_MODIFIER in CCSPlayer::HandleSpeedChanges.  Quake
  instead scales the move VALUES clientside with cl_movespeedkey, and FTESurf
  used to borrow that -- which only gives the right answer while
  cl_forwardspeed happens to equal maxspeed, because once the values exceed it
  the clamp is what decides everything.  That coupling is why raising the move
  values to Momentum's 450 would otherwise have turned the walk key into a 0.9x
  amble.  Here it is 0.52 of whatever maxspeed currently is, for free, and it
  compounds with the duck crop exactly as Source's does (Duck() runs later).

  No per-command state: Source tracks m_bIsWalking across the button edge, but
  what it derives is a pure function of the button, so reading the bit is
  identical and needs nothing added to pmsourcestate_t.
*/
static float PMSrc_EffectiveMaxSpeed (void)
{
	float ms = movevars.maxspeed > 0 ? movevars.maxspeed : 250;
	qboolean spec = (pmove.pm_type == PM_SPECTATOR ||
					 pmove.pm_type == PM_OLD_SPECTATOR);

	if (!spec && PMSrc_StaminaEnabled () && pmove.stamina > 0)
		ms *= PMSrc_StaminaRatio ();

	if (!spec && movevars.walkspeed > 0 && (pmove.cmd.buttons & BUTTON_SPEED))
		ms *= movevars.walkspeed;

	return ms;
}

/*
  Crop the move values while ducked and on the ground.
  Base Source scales by 1/3; CS:S uses its own 0.34 speed modifier, which is
  what produces the familiar 85 u/s crouch speed from a 250 u/s run.
*/
static void PMSrc_HandleDuckingSpeedCrop (void)
{
	if (pmove.ducked && pmove.onground)
	{
		float frac = movevars.duckspeed > 0 ? movevars.duckspeed : PMSRC_DUCK_SPEED_MODIFIER;
		pms_forwardmove *= frac;
		pms_sidemove    *= frac;
		pms_upmove      *= frac;
	}
}

/*
==================
PMSrc_Duck   (cpp:4316)

The CS:S subset.  Source's duck-jump machinery (m_flJumpTime / bInDuckJump)
is gated on `gpGlobals->maxClients == 1` at cpp:2530 -- it only ever runs in
single-player HL2 -- so in a CS:S build bDuckJump is permanently false and
that whole branch is dead.  This implements what is left, which is the duck
and unduck transitions.
==================
*/
static void PMSrc_Duck (void)
{
	int buttonsChanged  = (pmove.oldbuttons ^ pmove.cmd.buttons);
	int buttonsPressed  = buttonsChanged & pmove.cmd.buttons;
	int buttonsReleased = buttonsChanged & pmove.oldbuttons;
	qboolean bInAir  = !pmove.onground;
	qboolean bInDuck = pmove.ducked;
	qboolean wantduck = (pmove.cmd.buttons & BUTTON_DUCK) ? true : false;

	if (wantduck)
		pmove.oldbuttons |= BUTTON_DUCK;
	else
		pmove.oldbuttons &= ~BUTTON_DUCK;

	if (pmove.pm_type == PM_DEAD)
		return;

	PMSrc_HandleDuckingSpeedCrop ();

	if (!wantduck && !pmove.ducking && !bInDuck)
		return;

	if (wantduck)
	{
		if ((buttonsPressed & BUTTON_DUCK) && !bInDuck)
		{
			pmove.ducktime = PMSRC_DUCK_TIMER;
			pmove.ducking = true;
		}

		if (pmove.ducking)
		{
			float flDuckMilliseconds = PMSRC_DUCK_TIMER - pmove.ducktime;
			float flDuckSeconds;
			if (flDuckMilliseconds < 0)
				flDuckMilliseconds = 0;
			flDuckSeconds = flDuckMilliseconds * 0.001f;

			/* Ducking in the air is INSTANT -- that is the crouch-jump and
			   the mid-air hull shrink surf maps are built around. */
			if (flDuckSeconds > PMSRC_TIME_TO_DUCK || bInDuck || bInAir)
				PMSrc_FinishDuck ();
			/* else: mid-transition on the ground.  The hull stays the full 72
			   units; only the eye slides, and PMSrc_DuckFraction derives that
			   from ducktime after the tick loop.  cpp:4405. */
		}
	}
	else
	{
		/* Released duck part-way through ducking: invert the remaining time
		   so the unduck takes proportionally as long.  cpp:4437-4448. */
		if ((buttonsReleased & BUTTON_DUCK))
		{
			if (bInDuck)
				pmove.ducktime = PMSRC_DUCK_TIMER;
			else if (pmove.ducking && !pmove.ducked)
			{
				float unduckMs   = 1000.0f * PMSRC_TIME_TO_UNDUCK;
				float duckMs     = 1000.0f * PMSRC_TIME_TO_DUCK;
				float elapsedMs  = PMSRC_DUCK_TIMER - pmove.ducktime;
				float fracDucked = elapsedMs / duckMs;
				pmove.ducktime = PMSRC_DUCK_TIMER - unduckMs + fracDucked * unduckMs;
			}
		}

		if (PMSrc_CanUnduck ())
		{
			if (pmove.ducking || pmove.ducked)
			{
				float flDuckMilliseconds = PMSRC_DUCK_TIMER - pmove.ducktime;
				float flDuckSeconds;
				if (flDuckMilliseconds < 0)
					flDuckMilliseconds = 0;
				flDuckSeconds = flDuckMilliseconds * 0.001f;

				if (flDuckSeconds > PMSRC_TIME_TO_UNDUCK || bInAir)
					PMSrc_FinishUnDuck ();
				else
					pmove.ducking = true;	/* standing back up; cpp:4470 */
			}
		}
		else
		{
			/* Stuck under something.  Stay ducked and reset the timer so we
			   pop up the moment we clear it. */
			if (pmove.ducktime != PMSRC_DUCK_TIMER)
			{
				pmove.ducktime = PMSRC_DUCK_TIMER;
				pmove.ducked = true;
				pmove.ducking = false;
				PMSrc_ApplyHull ();
			}
		}
	}
}

/* ------------------------------------------------------------------- jump */

/*
==================
PMSrc_CheckJumpButton   (cpp:2376)

Two details that are easy to get wrong and both matter:

  * The impulse is ADDED to velocity[2] when standing, but ASSIGNED when
    ducked (cpp:2474-2487).  Adding is what lets you keep upward velocity
    from a ramp.
  * FinishGravity() is called HERE, and again at the end of FullWalkMove --
    so on the jump tick, gravity's second half is applied twice.  That is
    real Source behaviour (cpp:2520 and cpp:2133) and it is why the measured
    apex is slightly under the 45 units the continuous maths predicts.
    Do not "fix" this.
==================
*/
static qboolean PMSrc_CheckJumpButton (void)
{
	float flMul;

	if (pmove.pm_type == PM_DEAD)
	{
		pmove.oldbuttons |= BUTTON_JUMP;
		return false;
	}

	if (pmove.waterjumptime)
		return false;

	if (pmove.waterlevel >= 2)
	{	/* swim up */
		if (pmove.watertype == FTECONTENTS_WATER)
			pmove.velocity[2] = 100;
		else if (pmove.watertype == FTECONTENTS_SLIME)
			pmove.velocity[2] = 80;
		else
			pmove.velocity[2] = 50;
		pmove.oldbuttons |= BUTTON_JUMP;
		return false;
	}

	if (!pmove.onground)
	{
		pmove.oldbuttons |= BUTTON_JUMP;
		return false;	/* in air, so no effect */
	}

	/* Must release and re-press to jump again -- unless autobunny is on,
	   which is how every surf server is configured. */
	if (!movevars.autobunny && (pmove.oldbuttons & BUTTON_JUMP))
		return false;

	pmove.onground = false;
	pmove.jump_held = true;

	flMul = movevars.jumpvelocity;
	if (flMul <= 0)
		flMul = 268.3281572999747f;

	/* ASSIGN while ducking, ADD while standing -- and "ducking" means Source's
	   `m_bDucking || FL_DUCKING`, i.e. the TRANSITION counts, not just the
	   finished duck (cpp:2472).  We tested only pmove.ducked, which is
	   FL_DUCKING alone, and that is worth almost exactly two units of height.

	   The tick applies gravity in three half-steps -- StartGravity before this,
	   FinishGravity at the end of this function, and FinishGravity again at the
	   end of FullWalkMove -- and at 800/66.67Hz a half-step is 6.0 u/s.  So
	   velocity[2] is -6 when we get here.

	   MEASURE THE HEIGHT AGAINST THE VELOCITY THE MOVE IS PERFORMED WITH.  This
	   is leapfrog integration: the drift uses the mid-step velocity, so the
	   apex is (v_drift + g*dt/2)^2 / 2g and NOT v_end^2 / 2g -- taking v_end
	   loses the 3.8 units the jump tick itself travels.

	     ADD    -> 262.33 here, drift 256.33, apex (256.33+6)^2/1600 = 43.01
	     ASSIGN -> 268.33 here, drift 262.33, apex (262.33+6)^2/1600 = 45.00

	   -- the assign throws away that first -6, which is the whole difference.
	   (Patch 168's block below explains why the 43.01 is the anomaly and 45.00
	   is the honest number, and how pm_normalizejump gets both to 45.00.)

	   The ground duck takes 0.4s, and that window is where most crouch-jumps
	   are actually pressed -- so the case Source gives the HIGHER jump to was
	   the one case we gave the lower one.  pm_selftest pins both. */
	/*
	  FTESurf Patch 168: Momentum's mom_mv_normalize_jump_height.

	  Everything above this comment describes stock Source, and stock Source's
	  jump height is NOT constant.  Work the tick through, at 800/66.67Hz where
	  a half-step of gravity is 6.0 u/s.  The height a leapfrog integrator
	  actually reaches is (v_drift + g*dt/2)^2 / 2g, where v_drift is the
	  velocity the move is performed with -- NOT the velocity left at the end of
	  the tick, which is half a step later:

	    ADD     StartGravity -6, +268.33, FinishGravity -6  -> drift 256.33 -> 43.01 units
	    ASSIGN  StartGravity -6, =268.33, FinishGravity -6  -> drift 262.33 -> 45.00 units

	  Two units, on every jump you take without crouching, and it is the whole
	  of "I can't make jumps here that I can make in Momentum".

	  Look at where the two units GO and the fix writes itself.  The ADD case
	  loses exactly one half-step because FinishGravity is charged twice on a
	  jump tick -- once here, once at the end of FullWalkMove -- and the ASSIGN
	  case only looks correct because throwing away StartGravity's half-step
	  happens to cancel the duplicate.  Two bugs cancelling is not the same as
	  no bug: the cancellation holds only from rest, and ASSIGN additionally
	  discards any real upward velocity you had, which CategorizePosition
	  permits up to NON_JUMP_VELOCITY (140 u/s) of.

	  So normalising is not a fudge factor.  Add unconditionally (which is what
	  keeps ramp-given rise) and drop the duplicate FinishGravity, and the
	  height becomes

	      (v0 - g*dt/2 + I + g*dt/2)^2 / 2g  ==  (v0 + I)^2 / 2g

	  -- exactly the ideal apex, free of dt, free of duck state, and still
	  additive over whatever vertical speed you brought with you.  At the
	  defaults that is 268.3281572999747^2 / 1600 = 45.0 units flat.

	  pm_normalizejump 0 restores stock Source verbatim, and pm_selftest pins
	  both sets of numbers so neither can drift.

	  NOT IMPLEMENTED: Momentum's mode 2 ("normalize landing height only unless
	  jumping from ladders") is a separate landing-side correction.  Any
	  non-zero value here means mode 1.
	*/
	/*
	  FTESurf Patch 241 -- AND THE OTHER HALF OF WHAT PATCH 168 REMOVED.

	  Patch 168's argument above is right about the ADD and wrong about what it
	  cost.  Stock Source ASSIGNS while ducked or mid-duck, and that assign was
	  doing two jobs: buying back the duplicate FinishGravity (which normalising
	  handles properly), and CAPPING the jump when you arrive already rising.
	  Normalising kept the first job and quietly dropped the second, so after
	  Patch 168 there is no state in which the impulse is capped at all -- where
	  stock Source capped it in exactly the state a surfer spends most of their
	  time in.

	  What that opens.  CategorizePosition (:1685) will call you grounded with
	  up to NON_JUMP_VELOCITY -- 140 u/s -- of rise still on the clock, and
	  pm_groundtracedist means you count as standing on a floor you are not
	  touching.  Put those together with a trigger_teleport, which drops you one
	  unit above its destination with your velocity intact (sv_entities.qc,
	  trigger_teleport_touch, and Source's own tmp.z++ before it), and with
	  pm_autobunny re-firing the jump on the first grounded tick without a
	  button release, and the two jumps ADD:

	      apex = (inbound + 268.328)^2 / 1600
	          inbound   0  ->   45.0   (the whole point of normalising)
	          inbound 100  ->   84.8
	          inbound 140  ->  104.2   -- 2.3x a jump, from a teleport

	  Reported as "quake 2 double bounce ... I got teleported, and the next frame
	  I guess I was on the floor and got bounced even higher, combining 2 jumps".
	  It is exactly that, and the arithmetic above is where the height comes from.

	  THE FIX IS NOT AN ASSIGN.  An unconditional assign is what Patch 168 was
	  right to refuse: it would also delete rise you EARNED.  Instead, undo the
	  half-step StartGravity just took to recover the velocity we entered the
	  tick with, and discard only an upward one.  From rest that subtracts zero,
	  so every pinned number in pm_selftest is untouched; from a handed rise it
	  collapses to the from-rest case exactly.

	  ONLY the gravity half-step is undone, not StartGravity's other term, and
	  the difference is smaller than it looks.  Source applies a carrier's
	  VERTICAL base velocity as an acceleration, not as a launch:

	      velocity[2] += basevelocity[2] * frametime;   basevelocity[2] = 0;

	  (PMSrc_StartGravity, faithful to cpp.)  PMSrc_WalkMove's
	  VectorAdd(velocity, basevelocity) at :2424 cannot put it back either --
	  velocity[2] has just been zeroed at :2420-2422 and basevelocity[2] is
	  already 0 by then, so that path carries the horizontal component only.

	  So `inbound` above picks up exactly one tick of a carrier's vertical push:
	  1.5 u/s off a lift rising at 100, or about 11 u/s off a Source pad
	  authored as `basevelocity 0 0 1140` at 100 Hz -- see Patch 240, which gave
	  basevelocity a writer and found that the port tool appears to have written
	  a velocity where Source wants an acceleration.  Discarding one tick of it
	  is both negligible and the right side to be on: that push is the world
	  moving this tick, not speed the player brought to the jump.

	  A downward inbound is left alone.  It cannot normally occur -- the
	  previous tick's `if (onground) velocity[2] = 0` at the foot of
	  FullWalkMove zeroes it -- and discarding it would be a buff, not a fix.

	  pm_jumpaddrise 1 restores build 38 byte for byte, and pm_selftest pins
	  both sides.  The case that used to read "normalized: adds to inbound rise"
	  is now that cvar's case: see the jump block in PMSrc_SelfTest_f, which
	  says so where it changed.
	*/
	if (movevars.normalizejump)
	{
		if (!movevars.jumpaddrise)
		{
			float ent_gravity = movevars.entgravity;
			float inbound;

			if (!ent_gravity)
				ent_gravity = 1.0f;

			inbound = pmove.velocity[2] +
				(ent_gravity * movevars.gravity * 0.5f * pms_frametime);

			if (inbound > 0)
				pmove.velocity[2] -= inbound;
		}
		pmove.velocity[2] += flMul;
	}
	else if (pmove.ducked || pmove.ducking)
		pmove.velocity[2]  = flMul;
	else
		pmove.velocity[2] += flMul;

	/* Stamina scales the whole resulting Z, not just the impulse, and the
	   ratio is read BEFORE this jump's own cost is charged -- so a first jump
	   from rest is always full height and only the follow-ups are clipped.
	   cs_gamemovement.cpp, CCSGameMovement::CheckJumpButton. */
	if (PMSrc_StaminaEnabled ())
	{
		pmove.velocity[2] *= PMSrc_StaminaRatio ();
		PMSrc_StaminaSpend (movevars.staminajumpcost > 0 ?
			movevars.staminajumpcost : PMSRC_STAMINA_JUMP_COST);
	}

	if (movevars.normalizejump)
	{
		/*
		  FTESurf Patch 172: Momentum's sv_jump_z_offset, and this is a REWRITE
		  of what Patch 168 guessed at.

		  Patch 168 read the help string -- "instantaneous height increase when
		  the player jumps" -- and implemented an increase: origin[2] += offset.
		  The real thing (mom_gamemovement.cpp:1596-1616) is not an increase at
		  all.  It traces DOWN to the ground, then UP by the offset from the
		  contact point, and puts you there.  It PINS the takeoff height.

		  Which matters because of what pm_groundtracedist is.  You count as
		  standing on a surface anywhere within that band without touching it,
		  so stock Source's takeoff height -- and therefore its apex -- varies
		  by the full width of the band, up to two units, depending on nothing
		  you did.  Adding a constant to that keeps every bit of the variance
		  and just moves it up.  Tracing to the surface first deletes it.

		  So the jump is 45.0 + jumpzoffset above the SURFACE, every time,
		  rather than 45.0 + jumpzoffset above wherever in the band you
		  happened to be floating.  At Momentum's 1.5 that is 46.5.

		  Every trace is checked and the whole thing is skipped if any of them
		  says no: this runs at the top of a jump, and refusing to move is
		  always safe, while an unchecked move under a low ceiling is not.
		*/
		if (movevars.jumpzoffset > 0)
		{
			vec3_t down, up;
			trace_t tr;

			VectorCopy (pmove.origin, down);
			down[2] -= PMSrc_GroundTraceDist () + 0.1f;

			tr = PMSrc_TraceHull (pmove.origin, down);
			if (tr.fraction != 1.0f && !tr.startsolid && !tr.allsolid)
			{
				VectorCopy (tr.endpos, up);
				up[2] += movevars.jumpzoffset;

				tr = PMSrc_TraceHull (tr.endpos, up);
				if (tr.fraction == 1.0f && !tr.startsolid && !tr.allsolid)
				{
					/* Momentum prints exactly this, and for the same reason:
					   it is the only way to see the correction actually being
					   applied, and the (from => to) pair is what tells you
					   whether it moved you UP or DOWN -- which is the whole
					   difference between pinning and adding.
					   mom_gamemovement.cpp, "Jump height normalization". */
					Con_DPrintf ("jump z: moving player %s %.4f (%.4f => %.4f)\n",
						(tr.endpos[2] >= pmove.origin[2]) ? "up" : "down",
						fabs (tr.endpos[2] - pmove.origin[2]),
						pmove.origin[2], tr.endpos[2]);

					VectorCopy (tr.endpos, pmove.origin);
				}
			}
		}
	}
	else
		PMSrc_FinishGravity ();

	pmove.oldbuttons |= BUTTON_JUMP;
	return true;
}

/* --------------------------------------------------------------- the moves */

/* Build wishdir/wishspeed from the move values.  Shared by WalkMove and
   AirMove; both flatten forward/right onto the horizontal plane first. */
static float PMSrc_WishDir (vec3_t wishdir)
{
	vec3_t forward, right;
	vec3_t wishvel;
	float wishspeed;
	int i;

	VectorCopy (pms_forward, forward);
	VectorCopy (pms_right, right);

	forward[2] = 0;
	right[2] = 0;
	VectorNormalize (forward);
	VectorNormalize (right);

	for (i = 0; i < 2; i++)
		wishvel[i] = forward[i] * pms_forwardmove + right[i] * pms_sidemove;
	wishvel[2] = 0;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize (wishdir);

	if (wishspeed != 0.0f && wishspeed > pms_maxspeed)
		wishspeed = pms_maxspeed;

	return wishspeed;
}

/*
==================
PMSrc_WalkMove   (cpp:1906)
==================
*/
static void PMSrc_WalkMove (void)
{
	vec3_t wishdir;
	float wishspeed;
	float spd;
	vec3_t dest;
	trace_t pm;
	qboolean oldground = pmove.onground;

	wishspeed = PMSrc_WishDir (wishdir);

	pmove.velocity[2] = 0;
	PMSrc_Accelerate (wishdir, wishspeed, movevars.accelerate);
	pmove.velocity[2] = 0;

	VectorAdd (pmove.velocity, pmove.basevelocity, pmove.velocity);

	spd = VectorLength (pmove.velocity);
	if (spd < 1.0f)
	{
		VectorClear (pmove.velocity);
		VectorSubtract (pmove.velocity, pmove.basevelocity, pmove.velocity);
		return;
	}

	/* First try moving straight to the destination, staying at this height. */
	dest[0] = pmove.origin[0] + pmove.velocity[0] * pms_frametime;
	dest[1] = pmove.origin[1] + pmove.velocity[1] * pms_frametime;
	dest[2] = pmove.origin[2];

	/* ---- FTESurf Patch 206: the grounded walk-through ------------------------

	   This trace IS the whole move for a player walking on the floor, and when
	   it comes back clean the function returns right here without ever reaching
	   PMSrc_TryPlayerMove -- the one site P203 made portal-aware.  That was
	   harmless only while the aperture was an unpassable box.  Now that P206's
	   window accepts a standing player, PM_PortalCSG ELONGATES this trace
	   through the wall, so a clean fraction of 1 is exactly what a successful
	   carve looks like: the player would be walked bodily into the far side of
	   the entry wall with no transform, no rotation and no velocity change.

	   So on a map with portals this trace is the portal-aware one, and a
	   crossing here is committed the same way PMSrc_TryPlayerMove commits it. */
	if (pms_haveportals)
	{
		float tookportal = 0;
		pm = PM_PlayerTracePortals (pmove.origin, dest, MASK_PLAYERSOLID, &tookportal);
		if (tookportal)
		{
			VectorCopy (pm.endpos, pmove.origin);
			VectorSubtract (pmove.velocity, pmove.basevelocity, pmove.velocity);
			AngleVectors (pmove.angles, pms_forward, pms_right, pms_up);
			pms_portalcrossed = true;
			/* No StayOnGround: it traces 2 up then stepheight+2 down and snaps
			   the player to whatever it finds, which one tick after emerging
			   from a doorway is a floor they have no relationship to yet. */
			return;
		}
	}
	else
		pm = PMSrc_TraceHull (pmove.origin, dest);

	if (pm.fraction == 1)
	{
		VectorCopy (pm.endpos, pmove.origin);
		VectorSubtract (pmove.velocity, pmove.basevelocity, pmove.velocity);
		PMSrc_StayOnGround ();
		return;
	}

	/* Don't walk up stairs if we weren't on the ground to begin with. */
	if (!oldground && pmove.waterlevel == 0)
	{
		VectorSubtract (pmove.velocity, pmove.basevelocity, pmove.velocity);
		return;
	}

	if (pmove.waterjumptime)
	{
		VectorSubtract (pmove.velocity, pmove.basevelocity, pmove.velocity);
		return;
	}

	PMSrc_StepMove (dest, &pm);

	VectorSubtract (pmove.velocity, pmove.basevelocity, pmove.velocity);

	/* FTESurf Patch 203: not after a portal.  StayOnGround traces 2 up then
	   stepheight+2 down and snaps the player to whatever it lands on -- which,
	   one tick after emerging from a doorway, is a floor the player has no
	   relationship to yet.  Worse for a portal in a floor or ceiling, where it
	   would pull them straight back through.  CategorizePosition runs on the
	   next tick and re-establishes ground honestly. */
	if (!pms_portalcrossed)
		PMSrc_StayOnGround ();
}

/*
==================
PMSrc_AirMove   (cpp:1764)
==================
*/
static void PMSrc_AirMove (void)
{
	vec3_t wishdir;
	float wishspeed;

	wishspeed = PMSrc_WishDir (wishdir);

	PMSrc_AirAccelerate (wishdir, wishspeed, movevars.airaccelerate);

	VectorAdd (pmove.velocity, pmove.basevelocity, pmove.velocity);
	PMSrc_TryPlayerMove (NULL, NULL);
	VectorSubtract (pmove.velocity, pmove.basevelocity, pmove.velocity);
}

/*
==================
PMSrc_FullWalkMove   (cpp:2036)

The order here IS the physics.  Do not reorder.
==================
*/
static void PMSrc_FullWalkMove (void)
{
	qboolean wasonground = pmove.onground;

	if (pmove.waterlevel < 2)
		PMSrc_StartGravity ();

	if (pmove.waterlevel >= 2)
	{
		/* Water is not what this tool is for; keep it simple and
		   QuakeWorld-ish rather than pretending to be exact. */
		if (pmove.cmd.buttons & BUTTON_JUMP)
			PMSrc_CheckJumpButton ();
		else
			pmove.oldbuttons &= ~BUTTON_JUMP;

		PMSrc_TryPlayerMove (NULL, NULL);
		PMSrc_CategorizePosition ();
		if (pmove.onground)
			pmove.velocity[2] = 0;
		return;
	}

	if (pmove.cmd.buttons & BUTTON_JUMP)
		PMSrc_CheckJumpButton ();
	else
		pmove.oldbuttons &= ~BUTTON_JUMP;

	/* Friction runs before base velocity is added, so standing still on a
	   conveyor doesn't bleed the conveyor's speed. */
	if (pmove.onground)
	{
		pmove.velocity[2] = 0.0;
		PMSrc_Friction ();
	}

	PMSrc_CheckVelocity ();

	if (pmove.onground)
		PMSrc_WalkMove ();
	else
		PMSrc_AirMove ();

	PMSrc_CategorizePosition ();
	PMSrc_CheckVelocity ();

	/* Landing.  CheckJumpButton clears onground for the jump tick, so testing
	   the transition here rather than inside CategorizePosition (which runs
	   several times per tick, once per FinishDuck) charges this exactly once.
	   The penalty bites from the NEXT tick, since maxspeed for this one was
	   read before the move -- which is also the order Source reads it in. */
	if (!wasonground && pmove.onground)
		PMSrc_StaminaSpend (movevars.staminalandcost > 0 ?
			movevars.staminalandcost : PMSRC_STAMINA_LAND_COST);

	if (pmove.waterlevel < 2)
		PMSrc_FinishGravity ();

	if (pmove.onground)
		pmove.velocity[2] = 0;
}

/* ------------------------------------------------------------ stuck check */

/*
  A compact stand-in for Source's CheckStuck offset table: probe outward in
  the six axial directions plus up, at increasing distances, and take the
  first free spot.  Deliberately NOT PM_NudgePosition -- that snaps the
  origin to 1/8 unit, and Source runs on unquantized float origins.
*/
static qboolean PMSrc_CheckStuck (void)
{
	static const float dists[] = { 0.25f, 0.5f, 1, 2, 4, 8, 16 };
	static const vec3_t dirs[] =
	{
		{ 0, 0, 1}, { 0, 0,-1},
		{ 1, 0, 0}, {-1, 0, 0},
		{ 0, 1, 0}, { 0,-1, 0},
	};
	vec3_t base, test;
	size_t d, i;

	if (PM_TestPlayerPosition (pmove.origin, false))
		return false;	/* not stuck */

	VectorCopy (pmove.origin, base);

	for (d = 0; d < countof(dists); d++)
	{
		for (i = 0; i < countof(dirs); i++)
		{
			VectorMA (base, dists[d], dirs[i], test);
			if (PM_TestPlayerPosition (test, false))
			{
				VectorCopy (test, pmove.origin);
				return false;
			}
		}
	}

	VectorCopy (base, pmove.origin);
	return true;	/* hopelessly stuck */
}

/* ------------------------------------------------------------- noclip/spec */

/*
  Noclip / spectator flight.

  The DIRECTION comes from the move values; the SPEED does not.  That matters
  because CS:S's walk key is `cl_movespeedkey 0.52` -- Quake's run key inverted
  -- so a magnitude-driven noclip would get *slower* when you asked it to go
  faster.  Normalising and applying our own speed makes noclip independent of
  cl_forwardspeed, cl_movespeedkey and cl_run entirely.

  pm_noclipspeed * sv_maxspeed by default (4 * 250 = 1000 u/s), doubled while
  +speed is held (2000 u/s).  BUTTON_SPEED only exists when the client's
  in_speedbutton is set, so on a client that has not opted in this degrades to
  the plain 4x and nothing else changes.
*/
static void PMSrc_NoClipMove (void)
{
	vec3_t wishvel;
	float wishspeed, factor;
	int i;

	for (i = 0; i < 3; i++)
		wishvel[i] = pms_forward[i] * pms_forwardmove + pms_right[i] * pms_sidemove;

	/*
	  FTESurf Patch 154: +jump does not fly you upward in noclip.

	  The client synthesises `upmove = 200` from a held jump key whenever no
	  explicit +moveup is active (cl_input.c:2667) -- Quake's swimming rule,
	  which predates noclip having its own controls.  In noclip it means the
	  key you hold constantly while playing quietly climbs you out of the room
	  you were trying to look at.

	  Dropped only here, in the noclip mover: swimming, and the jump itself,
	  are untouched.  With the button doing nothing for movement it is free to
	  mean something else, and the gamecode uses it to fly through
	  trigger_teleports (SV_TeleportBypassed).

	  FTESurf Patch 162: this was inert until now, and the reason was upstream.
	  cl_smartjump diverts +jump to in_up whenever pmovetype is PM_SPECTATOR --
	  which is what noclip is -- so BUTTON_JUMP was never set and this test
	  could never be true.  IN_JumpDown now presses in_jump as well, so the bit
	  arrives and the check below does what it always said it did.

	  An explicit +moveup on its OWN key still works, and that is the point of
	  testing the button rather than the sign of upmove: +moveup presses in_up
	  without in_jump, so it carries no BUTTON_JUMP and is not swallowed here.
	*/
	if ((pmove.cmd.buttons & BUTTON_JUMP) && pms_upmove > 0)
		pms_upmove = 0;

	wishvel[2] += pms_upmove;

	factor = movevars.noclipspeed > 0 ? movevars.noclipspeed : PMSRC_NOCLIP_SPEED;
	if (pmove.cmd.buttons & BUTTON_SPEED)
		factor *= 2;

	/* FTESurf Patch 171: scale off the BASE maxspeed, not pms_maxspeed.
	   pms_maxspeed carries the stamina throttle and, as of this patch, the
	   walk-key modifier -- and the speed button is the same button that is
	   supposed to make noclip go FASTER here.  Reading it would have made
	   +speed multiply by 0.52 and by 2 at once, which is a 4% speedup dressed
	   up as a sprint.  This is the same independence the comment above claims;
	   it just has one more modifier to stay independent of now. */
	{
		float base = movevars.maxspeed > 0 ? movevars.maxspeed : 250;
		wishspeed = VectorNormalize (wishvel);
		if (wishspeed > 0)
			VectorScale (wishvel, base * factor, pmove.velocity);
		else
			VectorClear (pmove.velocity);
	}

	VectorMA (pmove.origin, pms_frametime, pmove.velocity, pmove.origin);

	pmove.onground = false;
	VectorClear (pmove.groundnormal);
}

/* ----------------------------------------------------------------- a tick */

/*
==================
PMSrc_Tick   (cpp:4515 PlayerMove + ProcessMovement)

One 0.015s Source tick.
==================
*/
/*
==================
PMSrc_LadderMove   (mom_gamemovement.cpp:494-681, LadderMove)

Returns true when the player is ON a ladder and this function has already set
their velocity for the tick; the caller then runs the ladder's own move instead
of the walk move.  False means "not on a ladder", and the walk move runs as
before -- including on the tick you jump OFF, which is what Source does (its
LadderMove sets MOVETYPE_WALK and PlayerMove's switch then routes to
FullWalkMove).

WHY THE CONTENTS TEST IS SAFE HERE, WHICH IS NOT OBVIOUS.
MASK_PLAYERSOLID is SOLID|PLAYERCLIP|WINDOW|BODY and does NOT contain
FTECONTENTS_LADDER (bspfile.h).  A solidmask in this engine does not filter what
a trace REPORTS, it decides which brushes the trace is allowed to visit at all
(com_bih.c:990, `if (node->data.contents & tr->hitcontents)`); a brush that IS
visited reports its whole contents word unfiltered (com_bih.c:783,
`tr->trace.contents = brush->contents;`).  So a ladder-only brush would be
invisible to this trace -- and the reason this works anyway is map data, not
code: a census of all 1310 library maps found 1750 ladder brushes and ZERO
ladder-only ones (73.0% also GRATE, 13.6% WINDOW, 12.9% SOLID), so every one of
them is eligible for some other reason and the ladder bit rides along.  Measured
in-engine before this was written (pm_ladderprobe): at a real ladder face the
unmodified mask returns 0x10034000 LADDER|PLAYERCLIP|MONSTERCLIP, and across 359
probe samples the widened masks never once saw a ladder this one missed.

If a map ever ships a ladder-only brush, it will silently not work, and the fix
is to widen the mask HERE -- not to port Momentum's LadderMask().  That is
`MASK_PLAYERSOLID & ~CONTENTS_PLAYERCLIP`, which is safe in Source only because
Source's own MASK_PLAYERSOLID carries CONTENTS_GRATE as a separate bit.  FTE has
no GRATE bit at all: mod_vbsp.c remaps Source GRATE to PLAYERCLIP|MONSTERCLIP,
and MONSTERCLIP is not in MASK_BOXSOLID -- so PLAYERCLIP is the ONLY thing making
73% of the library's ladder brushes eligible, and masking it out would blind
1281 of 1750 of them.

KNOWN GAP, stated because it will otherwise be hunted in this file.  About 9.6%
of library ladder brushes (164 of 1702 that a model-ownership census could
validate) are owned by a brush-entity submodel rather than the world.  Those on
SOLID brush entities are still found -- pmove does trace brush entities, unlike
CSQC's traceline.  Those the QC forces SOLID_NOT (func_illusionary,
func_simpleladder, func_brush with Solidity "Never Solid") are in no BIH this
trace walks and in no physent, so no change in this file can reach them; that is
a QC fix.  Eight maps have no world-reachable ladder brush at all, among them
surf_fungus and bhop_collective.
==================
*/
static qboolean PMSrc_LadderMove (trace_t *out)
{
	trace_t	pm;
	vec3_t	wishdir, end, floorpt;
	vec3_t	velocity, perp, cross, lateral, tmp, anglevec;
	float	dist, climbspeed, forwardspeed, rightspeed;
	float	normal, tmpdist, perpdist, angledot, dampen, anglecos;
	qboolean onfloor;
	int		i;

	if (!movevars.ladders)
		return false;

	/*Source's `if (player->GetMoveType() == MOVETYPE_NOCLIP) return false;`.
	  PMSrc_Tick has already returned for the flying pm_types before reaching us,
	  so this is belt and braces rather than the live gate. */
	if (pmove.pm_type == PM_SPECTATOR || pmove.pm_type == PM_OLD_SPECTATOR ||
		pmove.pm_type == PM_FLY || pmove.pm_type == PM_6DOF || pmove.pm_type == PM_DEAD)
	{
		pmove.srcladder = false;
		return false;
	}

	/*Which way to look for rungs.  Already attached: back along the remembered
	  normal, so letting go of the keys does not let go of the ladder.  Not
	  attached: along the direction you are asking to move, and if you are asking
	  for nothing then there is no ladder behaviour at all -- you cannot be
	  captured by a ladder you merely walked past. */
	if (pmove.srcladder)
		VectorNegate (pmove.srcladdernormal, wishdir);
	else if (pms_forwardmove || pms_sidemove)
	{
		/*m_vecForward, not a flattened copy: Source probes along the full 3D view
		  vector here, so looking up at the rungs tilts the probe upward. */
		for (i = 0; i < 3; i++)
			wishdir[i] = pms_forward[i]*pms_forwardmove + pms_right[i]*pms_sidemove;
		if (VectorNormalize (wishdir) < 0.001)
			return false;
	}
	else
		return false;

	/*LadderDistance(): 10 while attached, 2 while not (mom_gamemovement.cpp:71).
	  Valve's base is a flat 2; the 10 is Momentum's, and it is what stops a
	  climb from dropping you every time the hull drifts a unit off the wall. */
	dist = pmove.srcladder ? 10.0f : 2.0f;
	VectorMA (pmove.origin, dist, wishdir, end);

	pm = PMSrc_TraceHull (pmove.origin, end);
	if (out)
		*out = pm;

	/*The three rejects, in Momentum's order.  The normal.z == 1 test is a
	  Momentum addition (Valve has only the other two) and is an exact float
	  compare there, so it is an exact compare here: a dead-flat floor is not a
	  ladder, but a floor off by one ulp still is.  Reproducing that faithfully
	  matters more than tidying it, because the whole point of this mover is to
	  behave like the game it is imitating. */
	if (pm.fraction == 1.0f || pm.plane.normal[2] == 1.0f ||
		!(pm.contents & FTECONTENTS_LADDER))
	{
		/*One exception, and it is a real hazard rather than a hypothetical.
		  PM_PlayerTrace merges per-physent traces with
		      if (trace.fraction < total.fraction || (trace.startsolid && !total.startsolid))
		  (pmovetst.c) -- so a LATER physent the player is stuck inside replaces
		  the whole trace_t, contents included, and BIH_ClipBoxToBrush's startsolid
		  path returns BEFORE assigning contents (com_bih.c:763-769), leaving zero.
		  A correct ladder read on the world can therefore be erased by clipping a
		  door frame.  Dropping the player mid-climb for that would read as "the
		  ladder works except when I hug the corner", so while already attached a
		  startsolid trace holds the state rather than clearing it. */
		if (pmove.srcladder && pm.startsolid)
			return true;
		pmove.srcladder = false;
		return false;
	}

	pmove.srcladder = true;
	VectorCopy (pm.plane.normal, pmove.srcladdernormal);

	/*Are we standing at the foot of it?  Source asks
	      GetPointContents(floor) == CONTENTS_SOLID
	  and an EQUALITY test is wrong in this engine: FTE's remapped words carry
	  DETAIL and TRANSLUCENT alongside SOLID (the probe read 0x08000001
	  SOLID|DETAIL off ordinary brushwork), so `== FTECONTENTS_SOLID` would answer
	  "not floor" on most real floors.  Bit test. */
	VectorCopy (pmove.origin, floorpt);
	floorpt[2] += pmove.player_mins[2] - 1;
	onfloor = ((PM_PointContents (floorpt) & FTECONTENTS_SOLID) != 0) || pmove.onground;

	/*Jump off.  Velocity is REPLACED by normal*270, not added to -- Momentum and
	  Valve agree on both the operation and the number.  Returning false hands the
	  tick to the walk move, which is Source's own control flow. */
	if (pmove.cmd.buttons & BUTTON_JUMP)
	{
		pmove.srcladder = false;
		VectorScale (pm.plane.normal, 270, pmove.velocity);
		return false;
	}

	/*ClimbSpeed(): MAX_CLIMB_SPEED 200, times DUCK_SPEED_MULTIPLIER 0.34 while
	  the duck BUTTON is held -- the raw button bit, not the ducked state. */
	climbspeed = (pmove.cmd.buttons & BUTTON_DUCK) ? 200.0f*0.34f : 200.0f;

	/*Momentum reads four discrete button bits here (IN_FORWARD/IN_BACK/
	  IN_MOVELEFT/IN_MOVERIGHT) even though the wishdir above came from the analog
	  values.  FTE's usercmd has no such bits -- QuakeWorld sends analog
	  forwardmove/sidemove and nothing else -- so the faithful equivalent is the
	  SIGN of the analog value.  This is a deliberate adaptation, not a
	  transcription: it matches on keyboard (the only way Source could produce
	  those bits) and it degrades sensibly on a stick instead of ignoring it. */
	forwardspeed = (pms_forwardmove > 0) ? climbspeed : (pms_forwardmove < 0 ? -climbspeed : 0);
	rightspeed   = (pms_sidemove    > 0) ? climbspeed : (pms_sidemove    < 0 ? -climbspeed : 0);

	if (forwardspeed == 0 && rightspeed == 0)
	{	/*No input: you hang. Velocity is zeroed outright, not decayed -- there is
		  no friction or acceleration anywhere in this path. */
		VectorClear (pmove.velocity);
		return true;
	}

	VectorScale (pms_forward, forwardspeed, velocity);
	VectorMA (velocity, rightspeed, pms_right, velocity);

	/*perp = up x laddernormal: the horizontal axis lying in the ladder's plane. */
	VectorClear (tmp);
	tmp[2] = 1;
	CrossProduct (tmp, pm.plane.normal, perp);
	VectorNormalize (perp);

	normal = DotProduct (velocity, pm.plane.normal);	/*into the rungs*/
	VectorScale (pm.plane.normal, normal, cross);
	VectorSubtract (velocity, cross, lateral);			/*along the rungs*/

	CrossProduct (pm.plane.normal, perp, tmp);			/*up the ladder*/

	tmpdist  = DotProduct (tmp, lateral);
	perpdist = DotProduct (perp, lateral);

	VectorScale (perp, perpdist, anglevec);
	VectorAdd (anglevec, cross, anglevec);
	VectorNormalize (anglevec);
	angledot = DotProduct (anglevec, pm.plane.normal);

	anglecos = movevars.ladderangle ? movevars.ladderangle : -0.707f;
	dampen   = movevars.ladderdampen > 0 ? movevars.ladderdampen : 0.2f;
	if (angledot < anglecos)
	{	/*Approaching the rungs edge-on: damp the sideways component so you climb
		  rather than skating along the ladder's face. */
		VectorScale (tmp, tmpdist, lateral);
		VectorMA (lateral, dampen*perpdist, perp, lateral);
	}

	VectorMA (lateral, -normal, tmp, pmove.velocity);

	if (onfloor && normal > 0)
	{	/*Standing on the ground and pushing away from the ladder: shove off it.
		  Always the full 200, never the ducked value -- Momentum uses the literal
		  MAX_CLIMB_SPEED here, not ClimbSpeed(). */
		VectorMA (pmove.velocity, 200.0f, pm.plane.normal, pmove.velocity);
	}

	return true;
}

/*
==================
PMSrc_FullLadderMove   (gamemovement.cpp:2558, FullLadderMove)

Momentum does not override this, so Valve's runs.  What matters is what is NOT
here: no StartGravity/FinishGravity and no Friction/Accelerate.  Gravity does not
act on a ladder for that structural reason rather than by being zeroed -- and the
distinction is not academic.  Momentum sets the gravity MULTIPLIER to 1.0 on
attach, so the instant you leave the ladder normal gravity resumes; a port that
zeroed gravity instead would have to remember to restore it, and would strand
anyone a trigger_gravity had modified.
==================
*/
static void PMSrc_FullLadderMove (void)
{
	PMSrc_CheckVelocity ();
	PMSrc_TryPlayerMove (NULL, NULL);
	PMSrc_CategorizePosition ();
}

/* ---- FTESurf Patch 260: the ladder precondition, measured ------------------

   Build 3 is a mover port ONLY IF the ladder is visible to the mover's traces.
   The QC's ladder essay (sv_entities.qc, "Ladders -- and why there is no spawn
   function here any more") states the case for that: VBSP consumes func_ladder
   at compile time and bakes its brushes into the WORLD with Source's
   CONTENTS_LADDER set, the hl2 plugin maps that bit straight to
   FTECONTENTS_LADDER, and therefore "those brushes already report
   FTECONTENTS_LADDER to every trace ... the contents are already in the trace
   results waiting to be read".

   Two of those three steps are verifiable by reading and both hold.  The third
   does not follow, and the reason is one line in bspfile.h:

       MASK_PLAYERSOLID == MASK_BOXSOLID
                        == SOLID | PLAYERCLIP | WINDOW | BODY

   FTECONTENTS_LADDER is 0x4000 and is not in that set.  A solidmask does not
   merely filter what a trace REPORTS, it decides which brushes are solid to it
   at all -- so a brush whose only contents is LADDER is not hit, returns
   fraction 1, and reports nothing.  That is exactly why WPhys_CheckWater
   (sv_phys.c) has to set hitcontentsmaski to ~0 around its ladder trace and
   put it back afterwards.

   So whether the bit reaches a Source-mode trace depends on what ELSE the
   mapper put on the brush -- if it is also GRATE (which FTE remaps to
   PLAYERCLIP|MONSTERCLIP) the trace hits it for that reason and the LADDER bit
   may ride along in trace.contents; if the brush is ladder-only it is
   invisible.  That is map data, not code, and it differs per map.  Reading
   cannot settle it.  This probe does.

   It runs where the real hook will run -- after Duck, before the walk move --
   and fires the same trace three ways so the three failure modes separate
   cleanly in one line each:

     mask=PLAYERSOLID    what the mover sees today, unmodified.
     mask=PLAYERSOLID|LADDER  whether simply widening the mask is the whole fix.
     mask=~0             whether the bit is in the map at all.

   plus PM_ExtraBoxContents, which is how the QuakeWorld mover finds ladders --
   note it only walks NONSOLID physents carrying a forcecontentsmask and never
   looks at world brush contents, so on a Source map baked into the world it is
   expected to read 0.  If it does not, the ladder is an entity after all and
   the QC census was wrong.

   Deliberately prints on a negative too.  "Nothing here" and "the probe never
   ran" are different answers and must not look alike. */
static void PMSrc_ContentsName (int c, char *out, size_t outsize)
{
	static const struct { int bit; const char *name; } bits[] = {
		{FTECONTENTS_SOLID,			"SOLID"},
		{FTECONTENTS_WINDOW,		"WINDOW"},
		{FTECONTENTS_LAVA,			"LAVA"},
		{FTECONTENTS_SLIME,			"SLIME"},
		{FTECONTENTS_WATER,			"WATER"},
		{FTECONTENTS_LADDER,		"LADDER"},
		{FTECONTENTS_PLAYERCLIP,	"PLAYERCLIP"},
		{FTECONTENTS_MONSTERCLIP,	"MONSTERCLIP"},
		{FTECONTENTS_BODY,			"BODY"},
		{FTECONTENTS_CORPSE,		"CORPSE"},
		{FTECONTENTS_DETAIL,		"DETAIL"},
		{FTECONTENTS_SKY,			"SKY"},
	};
	size_t i;
	*out = 0;
	if (!c)
	{
		Q_strncpyz (out, "empty", outsize);
		return;
	}
	for (i = 0; i < countof(bits); i++)
		if (c & bits[i].bit)
		{
			if (*out)
				Q_strncatz (out, "|", outsize);
			Q_strncatz (out, bits[i].name, outsize);
		}
	if (!*out)
		Q_strncpyz (out, "(none named)", outsize);
}

static void PMSrc_LadderProbeLine (const char *what, vec3_t start, vec3_t end, unsigned int mask)
{
	trace_t t;
	char names[128];

	t = PM_PlayerTrace (start, end, mask);
	PMSrc_ContentsName (t.contents, names, sizeof(names));
	Con_Printf ("[ladderprobe]   %-22s frac %.4f ss %i  contents 0x%08x %s  norm %.3f %.3f %.3f\n",
	            what, t.fraction, t.startsolid?1:0, (unsigned int)t.contents, names,
	            t.plane.normal[0], t.plane.normal[1], t.plane.normal[2]);
}

static void PMSrc_LadderProbe (void)
{
	static float acc;
	vec3_t flatforward, fwd;
	char names[128];
	float len;
	int extra;

	acc += pms_frametime;
	if (acc < 0.25)
		return;
	acc = 0;

	flatforward[0] = pms_forward[0];
	flatforward[1] = pms_forward[1];
	flatforward[2] = 0;
	len = VectorNormalize (flatforward);
	if (len < 0.001)
	{	/*looking straight up or down: there is no forward to trace along, and a
		  zero-length forward trace would silently read as "nothing there". */
		Con_Printf ("[ladderprobe] view is vertical, no flat forward -- look at the ladder\n");
		return;
	}
	VectorMA (pmove.origin, 24, flatforward, fwd);

	extra = PM_ExtraBoxContents (pmove.origin);
	PMSrc_ContentsName (extra, names, sizeof(names));
	Con_Printf ("[ladderprobe] org %.1f %.1f %.1f  yaw %.1f  onground %i  extrabox 0x%08x %s\n",
	            pmove.origin[0], pmove.origin[1], pmove.origin[2], pmove.angles[1],
	            pmove.onground?1:0, (unsigned int)extra, names);

	/*Standing IN the volume, which is how a ladder region is usually authored. */
	PMSrc_LadderProbeLine ("box@org PLAYERSOLID",  pmove.origin, pmove.origin, MASK_PLAYERSOLID);
	PMSrc_LadderProbeLine ("box@org +LADDER",      pmove.origin, pmove.origin, MASK_PLAYERSOLID|FTECONTENTS_LADDER);
	PMSrc_LadderProbeLine ("box@org allbits",      pmove.origin, pmove.origin, ~0u);

	/*Facing the rungs, which is how Source confirms it. */
	PMSrc_LadderProbeLine ("fwd24 PLAYERSOLID",    pmove.origin, fwd, MASK_PLAYERSOLID);
	PMSrc_LadderProbeLine ("fwd24 +LADDER",        pmove.origin, fwd, MASK_PLAYERSOLID|FTECONTENTS_LADDER);
	PMSrc_LadderProbeLine ("fwd24 allbits",        pmove.origin, fwd, ~0u);
}

static void PMSrc_Tick (void)
{
	/* Board telemetry, per tick (Patch 131).  rampcontact is re-derived by
	   TryPlayerMove below, so clear it first; rampoff accumulates here and is
	   zeroed there, which makes it "seconds since we last touched a ramp"
	   without needing a timestamp anywhere. */
	pmove.rampcontact = 0;
	VectorClear (pmove.rampnormal);	/* Patch 137: re-derived below, like rampcontact */
	pmove.rampoff += pms_frametime;

	/* --- CheckParameters (cpp:965): clamp the move values to maxspeed --- */
	pms_forwardmove = pmove.cmd.forwardmove;
	pms_sidemove    = pmove.cmd.sidemove;
	pms_upmove      = pmove.cmd.upmove;

	pms_maxspeed = PMSrc_EffectiveMaxSpeed ();

	if (pmove.pm_type != PM_SPECTATOR && pmove.pm_type != PM_OLD_SPECTATOR)
	{
		float spd = (pms_forwardmove * pms_forwardmove) + (pms_sidemove * pms_sidemove);
		if (spd != 0.0 && spd > pms_maxspeed * pms_maxspeed)
		{
			float ratio = pms_maxspeed / sqrt (spd);
			pms_forwardmove *= ratio;
			pms_sidemove    *= ratio;
			pms_upmove      *= ratio;
		}
	}

	if (pmove.pm_type == PM_DEAD || pmove.pm_type == PM_FREEZE || pmove.pm_type == PM_NONE)
	{
		pms_forwardmove = pms_sidemove = pms_upmove = 0;
	}

	/* --- ReduceTimers (cpp:1071) --- */
	if (pmove.ducktime > 0)
	{
		pmove.ducktime -= 1000.0f * pms_frametime;
		if (pmove.ducktime < 0)
			pmove.ducktime = 0;
	}
	if (pmove.stamina > 0)
	{
		pmove.stamina -= 1000.0f * pms_frametime;
		if (pmove.stamina < 0)
			pmove.stamina = 0;
	}

	AngleVectors (pmove.angles, pms_forward, pms_right, pms_up);

	/* P203: PMSrc_TryPlayerMove clears this on entry too, but not every tick
	   reaches it -- PMSrc_WalkMove returns early when the flat trace is clean.
	   Cleared here as well so nothing can read last tick's answer. */
	pms_portalcrossed = false;

	/* P206: once per tick, not once per trace.  Every plain trace site in this
	   file consults it, and the physent set does not change inside a move. */
	pms_haveportals = PM_AnyPortals ();

	if (pmove.pm_type == PM_SPECTATOR || pmove.pm_type == PM_OLD_SPECTATOR ||
		pmove.pm_type == PM_FLY || pmove.pm_type == PM_6DOF)
	{
		PMSrc_NoClipMove ();
		return;
	}

	if (pmove.pm_type == PM_NONE || pmove.pm_type == PM_FREEZE)
	{
		PMSrc_CategorizePosition ();
		return;
	}

	PMSrc_CheckStuck ();
	PMSrc_CategorizePosition ();

	PMSrc_Duck ();

	/*Patch 260, temporary.  Deliberately HERE: Source calls Duck() then its
	  ladder block (gamemovement.cpp:4577 then :4591), so this is the line the
	  real hook will occupy and the probe measures the trace the mover would
	  actually make, not an approximation of it from somewhere else. */
	if (pm_ladderprobe.ival)
		PMSrc_LadderProbe ();

	/*Patch 260.  Source calls Duck() at gamemovement.cpp:4577 and the ladder
	  block at :4591, in that order, on every movetype -- so the hook goes AFTER
	  the duck, never before.  A true return means the ladder move owns the tick.
	  pmove.onladder is written unconditionally rather than only on the true path,
	  so the frame you step off is reported as off; it is an OUTPUT for QC/CSQC
	  (PMF_LADDER) and is not what carries the state -- see pmove.srcladder. */
	if (PMSrc_LadderMove (NULL))
	{
		pmove.onladder = true;
		PMSrc_FullLadderMove ();
		return;
	}
	pmove.onladder = false;

	PMSrc_FullWalkMove ();
}

/* --------------------------------------------------------------- entry pt */

/*
==================
PMSrc_PlayerMove

Runs whole fixed ticks and carries the remainder.

WHY A FIXED TICK AT ALL
-----------------------
FTE drives player physics per usercmd with frametime = cmd.msec/1000
(sv_user.c:7512), so the simulation step is whatever the client's framerate
happened to produce.  Source instead runs exactly one CGameMovement pass per
tick interval.  That difference is not cosmetic for surfing: air acceleration
adds `airaccel * wishspeed * dt` per step against a fixed 30 u/s budget, so
the speed you gain from a ramp depends on the step size.  A trainer that
rewards a particular framerate is worse than useless.

66.666... Hz is 15 ms EXACTLY, which is the reason it works cleanly here:
usercmd_t.msec is an integer number of milliseconds, so a 15 ms command
survives the round trip losslessly.  64-tick's 15.625 ms would not.

The carry (pmove.msec_carry) is part of the predicted state and is propagated
through the client's replay, so a dropped or doubled packet cannot desync the
client from the server.
==================
*/
void PMSrc_PlayerMove (float gamespeed)
{
	float tick = movevars.ticrate;
	float avail;
	int iters = 0;

	if (tick < 0.001f)
		tick = 0.015f;

	/* Source has no arbitrary gravity direction. */
	pmove.gravitydir[0] = 0;
	pmove.gravitydir[1] = 0;
	pmove.gravitydir[2] = -1;

	pms_frametime = tick;
	pms_surfed = false;

	if (pmove.surfacefriction <= 0)
		pmove.surfacefriction = 1.0f;

	/* Width from the entity, height from movevars -- see PMSrc_ApplyHull for
	   why the height cannot come from the entity. */
	VectorCopy (pmove.player_mins, pms_standmins);
	VectorCopy (pmove.player_maxs, pms_standmaxs);

	/*
	  ...and the FLOOR from neither: in Source the origin IS the feet, so the
	  box bottom is 0 by definition.  Momentum never derives this at all -- it
	  passes the literal VEC_HULL_MIN/VEC_HULL_MAX to UTIL_TraceHull
	  (mom_gamemovement.cpp:736).  Taking it from the entity is what turned one
	  bad setmodel into a movement bug:

	  FTESurf's PutClientInServer used to setsize() the CS:S hull and THEN
	  setmodel(self, "").  An empty model string does not load, so PF_setmodel
	  took its "nq pretended that its models were all +/- 16" fallback
	  (pr_cmds.c:3181-3186, reachable because the game sets
	  sv_gameplayfix_setmodelsize_qw 1 for prop bounds) and stamped mins.z=-16
	  over the hull.  This function then read it, ApplyHull derived the top from
	  it, and the box became -16..46 -- the correct height, 62, in the wrong
	  place.  The feet rested on the floor with the ORIGIN 16 units above it, so
	  the eye sat at 80 rather than 64, while every diagnostic that measured the
	  eye against the ORIGIN kept reporting a perfectly correct 64.  sv_user.c
	  writes the hull back to the entity each move, so it never healed.

	  Pinning it here fixes the movement for any mod that gets this wrong, and
	  the warning makes sure it cannot hide silently a second time.
	*/
	if (pms_standmins[2] != 0)
	{
		if (!pms_warned_hullfloor)
		{
			pms_warned_hullfloor = true;
			Con_Printf (CON_WARNING "Source physics: player mins[2] is %g, not 0. "
						"The origin must be at the feet; using 0. "
						"Check that the mod's setsize runs AFTER its setmodel.\n",
						pms_standmins[2]);
		}
		pms_standmins[2] = 0;
	}

	PMSrc_ApplyHull ();

	pmove.numtouch = 0;

	avail = pmove.msec_carry + (pmove.cmd.msec * 0.001f * gamespeed);

	while (avail + PMSRC_TICK_EPSILON >= tick)
	{
		if (iters >= PMSRC_MAX_TICKS)
		{
			/* Config is wrong (cl_netfps far below the tick rate, or a huge
			   stall).  Drop the excess rather than hitching. */
			avail = 0;
			break;
		}
		PMSrc_Tick ();
		avail -= tick;
		iters++;
	}

	if (avail < 0)
		avail = 0;
	pmove.msec_carry = avail;

	pmove.surfing = pms_surfed;

	/* Hand the mod the hull we settled on, so the server can resize the
	   edict and the client can predict with the same box. */
	PMSrc_ApplyHull ();

	/* ...and the eye height that goes with it.  Derived from the carried duck
	   state rather than written during the ticks -- see PMSrc_DuckFraction. */
	PMSrc_SetDuckedEyeOffset (PMSrc_DuckFraction ());
}

/*
================================================================================
  pm_selftest -- numeric verification.

  Drives the REAL functions above (not a reimplementation) with known inputs
  and checks them against values derived from the reference sources, so a
  regression shows up as a number rather than as "surfing feels off".

  Expected values come from movementmath's test suite, re-derived for
  dt = 0.015 instead of its 1/128, plus the CS:S tunings from
  gamemovement_momentummod.cpp.  These functions are all trace-free, so this
  runs with no map loaded.
================================================================================
*/
static int pms_test_fails;

static void PMSrc_Check (const char *what, float got, float expect, float tol)
{
	float d = got - expect;
	if (d < 0) d = -d;
	if (d <= tol)
		Con_Printf ("  ^2ok^7   %-34s %12.5f\n", what, got);
	else
	{
		Con_Printf ("  ^1FAIL^7 %-34s %12.5f  (expected %.5f)\n", what, got, expect);
		pms_test_fails++;
	}
}

/*
  Drive one jump tick exactly as FullWalkMove sequences it, and return the
  velocity the MOVE is performed with -- the value AirMove would multiply by
  frametime.  velocity[2] is left holding the end-of-tick value, so a caller
  can check both.

  pmove.stamina is cleared every time: the previous case's jump charged 25, and
  the ratio scales the whole resulting Z, so without this every case after the
  first would silently measure 0.75x and fail for the wrong reason.
*/
static float PMSrc_TestJumpDrift (float vz0, qboolean ducking)
{
	float drift;

	VectorClear (pmove.velocity);
	pmove.velocity[2] = vz0;
	pmove.onground    = true;
	pmove.ducked      = false;
	pmove.ducking     = ducking;
	pmove.oldbuttons  = 0;
	pmove.cmd.buttons = BUTTON_JUMP;
	pmove.waterlevel  = 0;
	pmove.stamina     = 0;

	PMSrc_StartGravity ();
	PMSrc_CheckJumpButton ();
	drift = pmove.velocity[2];
	PMSrc_FinishGravity ();

	pmove.ducking = false;
	return drift;
}

/* The apex a leapfrog integrator reaches from a given drift velocity.  The
   half-step is what Build 14's numbers were missing. */
static float PMSrc_TestJumpHeight (float drift)
{
	float v = drift + movevars.gravity * 0.5f * pms_frametime;
	return (v * v) / (2 * movevars.gravity);
}

static void PMSrc_SelfTest_f (void)
{
	vec3_t save_vel, save_org, wishdir, out, normal;
	float dt = 0.015f;
	float saved_ft = pms_frametime;
	float sf = pmove.surfacefriction;
	int savetype = pmove.pm_type;
	float savewjt = pmove.waterjumptime;
	float savestam = pmove.stamina;
	movevars_t savemv = movevars;
	playermove_t *pm = &pmove;

	VectorCopy (pmove.velocity, save_vel);
	VectorCopy (pmove.origin, save_org);

	pms_test_fails = 0;
	pms_frametime = dt;
	pmove.surfacefriction = 1.0f;
	pmove.pm_type = PM_NORMAL;
	pmove.waterjumptime = 0;
	(void)pm;

	/* Report what the RUNNING config is, then test against a fixed CS:S
	   parameter set rather than against it.  movevars are only populated
	   once a server is running or a connection is made, so a config-driven
	   test would silently pass against all-zero parameters when run from
	   the menu -- which is exactly the false pass this avoids.  Compare the
	   two lines below to see whether your config matches CS:S. */
	Con_Printf ("^5pm_selftest^7  dt=%g (%.4f Hz)\n", dt, 1.0/dt);
	Con_Printf ("^7running config: mode=%i accel=%g airaccel=%g fric=%g stop=%g grav=%g jump=%g aircap=%g\n",
		movevars.physicsmode, movevars.accelerate, movevars.airaccelerate,
		movevars.friction, movevars.stopspeed, movevars.gravity,
		movevars.jumpvelocity, movevars.maxairspeed);

	/* The Momentum-surf reference set the checks below are written against.
	   Deliberately NOT the running config: movevars are zero until a server
	   starts, so a config-driven test passes against all-zero parameters when
	   run from the menu.  Build 41 moved these off CS:S's numbers and onto
	   Momentum's -- see CGameModeBase::SetGameModeVars / g_ViewVectorsMom. */
	movevars.gravity			= 800;
	movevars.friction			= 4;
	movevars.stopspeed			= 75;
	movevars.accelerate			= 5;
	movevars.airaccelerate		= 150;
	movevars.maxairspeed		= 30;
	movevars.jumpvelocity		= 301.9933774;
	movevars.maxvelocity		= 3500;
	movevars.standheight		= 62;
	movevars.duckheight			= 45;
	movevars.viewscale			= 0.5;
	movevars.standablenormal	= 0.7;
	movevars.duckspeed			= 0.34;
	movevars.entgravity			= 1;
	movevars.bounce				= 0;
	movevars.viewheight			= 64;
	movevars.duckviewheight		= 47;
	movevars.noclipspeed		= 4;
	movevars.stamina			= 1;
	movevars.staminajumpcost	= 25;
	movevars.staminalandcost	= 20;
	movevars.staminarecovery	= 19;
	/* The jump block below drives BOTH modes and sets this itself; seed it to
	   stock so anything before it reads the Source behaviour. */
	movevars.normalizejump		= 0;
	movevars.jumpaddrise		= 0;	/*Patch 241; the jump block drives both sides of this too*/
	movevars.jumpzoffset		= 0;
	movevars.walkspeed			= 0.52;
	movevars.groundtracedist	= 2;
	movevars.bumpcount			= 8;
	movevars.snaptoground		= 1;
	movevars.groundquadrants	= 1;
	movevars.fixslopes			= 1;
	movevars.fixedges			= 1;
	movevars.fixrampbugs		= 1;
	movevars.rampretrace		= 0.2;
	pmove.stamina				= 0;
	Con_Printf ("^7testing against:  accel=5 airaccel=150 fric=4 stop=75 grav=800 jump=301.993 aircap=30\n");
	Con_Printf ("^7                  stamina jump=25 land=20 recovery=19, hull 62/45, view 64/47,\n");
	Con_Printf ("^7                  viewscale 0.5 (air duck shift 8.5), noclip 4x\n");

	/* --- Patch 172: the parameters that used to be constants -------------

	   Both accessors CLAMP rather than merely default, because these arrive
	   over serverinfo and "whatever the other end sent" includes 0 and
	   1000000.  A zero trace distance means never finding ground, and an
	   unbounded bump count means an unbounded number of traces inside one
	   usercmd -- neither is a setting, both are a hang. */
	{
		float savegtd = movevars.groundtracedist;
		float savebc  = movevars.bumpcount;

		movevars.bumpcount = 8;
		PMSrc_Check ("bumpcount: Momentum's 8 passes through", PMSrc_BumpCount(), 8, 0.5f);
		movevars.bumpcount = 0;
		PMSrc_Check ("bumpcount: 0 clamps up to Source's 4", PMSrc_BumpCount(), 4, 0.5f);
		movevars.bumpcount = 2;
		PMSrc_Check ("bumpcount: below 4 clamps up", PMSrc_BumpCount(), 4, 0.5f);
		movevars.bumpcount = 99999;
		PMSrc_Check ("bumpcount: clamps down to 16", PMSrc_BumpCount(), 16, 0.5f);

		movevars.groundtracedist = 2;
		PMSrc_Check ("groundtracedist: 2 passes through", PMSrc_GroundTraceDist(), 2, 0.0001f);
		movevars.groundtracedist = 0;
		PMSrc_Check ("groundtracedist: 0 falls back to 2", PMSrc_GroundTraceDist(), 2, 0.0001f);

		movevars.groundtracedist = savegtd;
		movevars.bumpcount = savebc;
	}

	/* ClipVelocity's wall bit is an epsilon test now, not `!normal[2]`.  The
	   near-vertical case is the one exact-float equality gets wrong. */
	{
		vec3_t n, in, out;
		int b;

		VectorSet (in, 100, 0, 0);
		VectorSet (n, 1, 0, 1e-9f);
		b = PMSrc_ClipVelocity (in, n, out, 1);
		PMSrc_Check ("ClipVelocity: 1e-9 off vertical is a wall", (b & 2) ? 1 : 0, 1, 0.5f);

		VectorSet (n, 0.5f, 0, 0.5f);
		b = PMSrc_ClipVelocity (in, n, out, 1);
		PMSrc_Check ("ClipVelocity: a real slope is not a wall", (b & 2) ? 1 : 0, 0, 0.5f);
	}

	/* --- Patch 176: the landing decision ---------------------------------

	   PMSrc_CategorizePosition itself cannot run here -- it traces, and
	   pm_selftest runs with no map.  So the rule was factored into two pure
	   functions and it is those that get pinned; what is left in
	   CategorizePosition is three trace calls and an if.

	   The numbers below are hand-derived rather than recorded from a run, so
	   a change in ClipVelocity shows up here as a failure rather than as a
	   quietly updated expectation. */
	{
		vec3_t	n, v, nv, out;
		trace_t	tr;
		qboolean gains;

		/* The half step of gravity the next tick will apply: 800*0.5*0.015. */
		VectorSet (v, 100, 0, 0);
		PMSrc_NextTickVelocity (v, dt, 800, 1, nv);
		PMSrc_Check ("NextTickVelocity: half a tick of gravity", nv[2], -6.0f, 0.0001f);
		PMSrc_Check ("NextTickVelocity: leaves x alone", nv[0], 100.0f, 0.0001f);
		PMSrc_NextTickVelocity (v, dt, 800, 0, nv);
		PMSrc_Check ("NextTickVelocity: entgravity 0 means 1", nv[2], -6.0f, 0.0001f);
		PMSrc_NextTickVelocity (v, dt, 800, 0.5f, nv);
		PMSrc_Check ("NextTickVelocity: entgravity scales", nv[2], -3.0f, 0.0001f);

		/* DOWNHILL.  Dropping straight onto a slope that falls away in +x
		   (normal tilts the same way).  The collision converts fall into
		   forward speed -- 0 u/s of 2d becomes 240 -- so it is worth having,
		   and at -180 it is not throwing us up, so we land.  This is
		   mom_mv_fix_downhill_slopes. */
		VectorSet (n, 0.6f, 0, 0.8f);
		VectorSet (v, 0, 0, -500);
		gains = PMSrc_SlopeLandingGains (v, v, n, out);
		PMSrc_Check ("Slope downhill: gains 2d speed", gains ? 1 : 0, 1, 0.5f);
		PMSrc_Check ("Slope downhill: converts fall to forward", out[0], 240.0f, 0.001f);
		PMSrc_Check ("Slope downhill: lands (z <= 140)", out[2], -180.0f, 0.001f);

		/* UPHILL, at speed.  The mirrored normal.  The collision costs 408
		   u/s of the 1000 we had, and it would throw us up at 444 -- over the
		   140 cut, so we decline the landing entirely and keep riding.  Both
		   halves of the rule fire on the same vector, which is the point:
		   the collision you would lose speed to is the one that was never a
		   landing.  This is mom_mv_fix_uphill_slopes. */
		VectorSet (n, -0.6f, 0, 0.8f);
		VectorSet (v, 1000, 0, -100);
		gains = PMSrc_SlopeLandingGains (v, v, n, out);
		PMSrc_Check ("Slope uphill: loses 2d speed", gains ? 1 : 0, 0, 0.5f);
		PMSrc_Check ("Slope uphill: would be robbed of", 1000.0f - out[0], 408.0f, 0.001f);
		PMSrc_Check ("Slope uphill: declines (z > 140)",
			(out[2] > PMSRC_NON_JUMP_VELOCITY) ? 1 : 0, 1, 0.5f);

		/* FLAT GROUND.  The non-regression that matters more than either of
		   the above: an ordinary landing must be exactly what it was.  The
		   clip removes the fall and nothing else, 2d speed is unchanged --
		   NOT greater -- so nothing is adopted and no free speed exists. */
		VectorSet (n, 0, 0, 1);
		VectorSet (v, 300, 0, -200);
		gains = PMSrc_SlopeLandingGains (v, v, n, out);
		PMSrc_Check ("Slope flat: no gain, so nothing adopted", gains ? 1 : 0, 0, 0.5f);
		PMSrc_Check ("Slope flat: keeps its 2d speed", out[0], 300.0f, 0.0001f);
		PMSrc_Check ("Slope flat: lands", out[2], 0.0f, 0.0001f);

		/* DidHit is not `fraction < 1`.  A trace that starts inside a brush
		   reports fraction 1 and has certainly hit; the edge fix reads this
		   to decide whether the next tick finds ground at all. */
		memset (&tr, 0, sizeof(tr));
		tr.fraction = 1.0f;
		PMSrc_Check ("DidHit: clean miss", PMSrc_TraceDidHit(&tr) ? 1 : 0, 0, 0.5f);
		tr.startsolid = true;
		PMSrc_Check ("DidHit: startsolid at fraction 1", PMSrc_TraceDidHit(&tr) ? 1 : 0, 1, 0.5f);
		memset (&tr, 0, sizeof(tr));
		tr.fraction = 0.5f;
		PMSrc_Check ("DidHit: an ordinary impact", PMSrc_TraceDidHit(&tr) ? 1 : 0, 1, 0.5f);
	}

	/* --- Patch 177: the ramp fix's pure parts ---------------------------

	   The move loop itself traces, so it cannot run here -- pm_selftest has no
	   world.  What CAN be pinned is every decision the loop makes ABOUT a
	   trace, and that is where the bugs live: a plane sanity test that lets a
	   deformed normal through is how a surfer gets launched sideways, and a
	   same-plane test that is too loose is how the recovery loop nudges along
	   one face forever.

	   PMSrc_IsValidMovementTrace's last check traces, so only its four
	   early refusals are reachable here.  They are also the four that matter:
	   each one is a distinct way a ramp trace comes back useless. */
	{
		trace_t	tr;
		vec3_t	a, b;
		float	saveret = movevars.rampretrace;

		movevars.rampretrace = 0.2f;
		PMSrc_Check ("rampretrace: Momentum's 0.2 passes through", PMSrc_RampRetrace(), 0.2f, 0.0001f);
		movevars.rampretrace = 0;
		PMSrc_Check ("rampretrace: 0 falls back to 0.2", PMSrc_RampRetrace(), 0.2f, 0.0001f);
		movevars.rampretrace = -1;
		PMSrc_Check ("rampretrace: negative falls back too", PMSrc_RampRetrace(), 0.2f, 0.0001f);
		movevars.rampretrace = 99999;
		PMSrc_Check ("rampretrace: clamps down to 4", PMSrc_RampRetrace(), 4.0f, 0.0001f);
		movevars.rampretrace = saveret;

		VectorSet (a, 0, 0, 1);
		PMSrc_Check ("PlaneIsSane: a floor", PMSrc_PlaneIsSane(a) ? 1 : 0, 1, 0.5f);
		VectorSet (a, 0.6f, 0, 0.8f);
		PMSrc_Check ("PlaneIsSane: a ramp", PMSrc_PlaneIsSane(a) ? 1 : 0, 1, 0.5f);
		VectorSet (a, 0, 0, 0);
		PMSrc_Check ("PlaneIsSane: empty is SANE (zero is not deformed)",
			PMSrc_PlaneIsSane(a) ? 1 : 0, 1, 0.5f);
		VectorSet (a, 1.5f, 0, 0);
		PMSrc_Check ("PlaneIsSane: past 1 is deformed", PMSrc_PlaneIsSane(a) ? 1 : 0, 0, 0.5f);
		VectorSet (a, 0, -1.0001f, 0);
		PMSrc_Check ("PlaneIsSane: past -1 too", PMSrc_PlaneIsSane(a) ? 1 : 0, 0, 0.5f);

		VectorSet (a, 0.6f, 0, 0.8f);
		VectorCopy (a, b);
		PMSrc_Check ("VecCloseEnough: identical", PMSrc_VecCloseEnough(a, b) ? 1 : 0, 1, 0.5f);
		b[1] += 1e-9f;
		PMSrc_Check ("VecCloseEnough: 1e-9 is the same plane",
			PMSrc_VecCloseEnough(a, b) ? 1 : 0, 1, 0.5f);
		b[1] += 0.001f;
		PMSrc_Check ("VecCloseEnough: 0.001 is a different plane",
			PMSrc_VecCloseEnough(a, b) ? 1 : 0, 0, 0.5f);
		VectorClear (b);
		PMSrc_Check ("VecCloseEnough: a cleared plane IS empty",
			PMSrc_VecCloseEnough(b, vec3_origin) ? 1 : 0, 1, 0.5f);
		PMSrc_Check ("VecCloseEnough: a real plane is not",
			PMSrc_VecCloseEnough(a, vec3_origin) ? 1 : 0, 0, 0.5f);

		/* allsolid and startsolid come first because a stuck trace can have no
		   plane at all -- testing the normal first would read uninitialised
		   memory and call it a floor. */
		memset (&tr, 0, sizeof(tr));
		tr.fraction = 0.5f;
		tr.plane.normal[2] = 1;
		tr.allsolid = true;
		PMSrc_Check ("ValidTrace: allsolid is not valid",
			PMSrc_IsValidMovementTrace(&tr) ? 1 : 0, 0, 0.5f);
		tr.allsolid = false;
		tr.startsolid = true;
		PMSrc_Check ("ValidTrace: startsolid is not valid",
			PMSrc_IsValidMovementTrace(&tr) ? 1 : 0, 0, 0.5f);
		tr.startsolid = false;
		tr.fraction = 0.0f;
		PMSrc_Check ("ValidTrace: fraction 0 moved nowhere",
			PMSrc_IsValidMovementTrace(&tr) ? 1 : 0, 0, 0.5f);
		tr.fraction = 0.5f;
		tr.plane.normal[0] = 2.5f;
		PMSrc_Check ("ValidTrace: a deformed plane is not valid",
			PMSrc_IsValidMovementTrace(&tr) ? 1 : 0, 0, 0.5f);
	}

	/* --- Accelerate: from rest, accelspeed = accel*dt*wishspeed --------- */
	VectorSet (wishdir, 1, 0, 0);
	VectorClear (pmove.velocity);
	PMSrc_Accelerate (wishdir, 250, 5);
	PMSrc_Check ("Accelerate 250@5 from rest", pmove.velocity[0], 5 * dt * 250, 0.0001f);

	/* Clamped by addspeed once we are near wishspeed. */
	VectorSet (pmove.velocity, 249, 0, 0);
	PMSrc_Accelerate (wishdir, 250, 5);
	PMSrc_Check ("Accelerate clamps at wishspeed", pmove.velocity[0], 250, 0.0001f);

	/* --- Friction: drop = max(speed,stopspeed) * friction * dt ---------- */
	VectorSet (pmove.velocity, 250, 0, 0);
	pmove.onground = true;
	PMSrc_Friction ();
	PMSrc_Check ("Friction from 250", pmove.velocity[0],
		250 - (250 * movevars.friction * dt), 0.001f);

	/* Below stopspeed the drop uses stopspeed, not speed. */
	VectorSet (pmove.velocity, 50, 0, 0);
	PMSrc_Friction ();
	PMSrc_Check ("Friction from 50 (uses stopspeed)", pmove.velocity[0],
		50 - (movevars.stopspeed * movevars.friction * dt), 0.001f);

	/* --- AirAccelerate: the 30 u/s budget ------------------------------- */
	/* accelspeed = 150*250*0.015 = 562.5, way over addspeed, so we get
	   exactly the air cap and not one unit more. */
	VectorClear (pmove.velocity);
	PMSrc_AirAccelerate (wishdir, 250, 150);
	PMSrc_Check ("AirAccel from rest -> air cap", pmove.velocity[0],
		movevars.maxairspeed, 0.0001f);

	/* Already at the cap along wishdir: addspeed <= 0, nothing added.
	   This is why you must keep turning to keep gaining. */
	PMSrc_AirAccelerate (wishdir, 250, 150);
	PMSrc_Check ("AirAccel at cap adds nothing", pmove.velocity[0],
		movevars.maxairspeed, 0.0001f);

	/* Moving fast PERPENDICULAR to wishdir: the projection onto wishdir is
	   0, so the full cap is available no matter how fast you already are.
	   That is the whole trick -- speed is unbounded. */
	VectorSet (pmove.velocity, 0, 2000, 0);
	PMSrc_AirAccelerate (wishdir, 250, 150);
	PMSrc_Check ("AirAccel gains at 2000 u/s", pmove.velocity[0],
		movevars.maxairspeed, 0.0001f);

	/* Low airaccelerate is rate-limited instead: 10*250*0.015 = 37.5,
	   still over the 30 cap, so drop to something that is not. */
	VectorClear (pmove.velocity);
	PMSrc_AirAccelerate (wishdir, 250, 5);
	PMSrc_Check ("AirAccel rate-limited (accel 5)", pmove.velocity[0],
		5 * 250 * dt, 0.0001f);

	/* --- ClipVelocity: a 45 degree ramp loses no speed ------------------ */
	VectorSet (normal, 0.70710678f, 0, 0.70710678f);
	VectorSet (out, 0, 0, -500);
	PMSrc_ClipVelocity (out, normal, out, 1.0f);
	PMSrc_Check ("Clip 500 down onto 45deg: x", out[0], 250, 0.01f);
	PMSrc_Check ("Clip 500 down onto 45deg: z", out[2], -250, 0.01f);
	PMSrc_Check ("Clip 500 down onto 45deg: |v|", VectorLength(out),
		500 * 0.70710678f, 0.01f);

	/* The adjust pass must leave nothing pointing into the plane. */
	PMSrc_Check ("Clip leaves no into-plane part", DotProduct(out, normal), 0, 0.0005f);

	/* --- Jump ------------------------------------------------------------

	   Both modes, both duck states, and the HEIGHT rather than a velocity,
	   because the height is the thing a player runs into.

	   Build 14 and earlier pinned "standing jump apex 39.165", derived as
	   v_end^2 / 2g from the velocity left at the END of the jump tick.  That
	   is a reproducible number and it is 3.8 units wrong: leapfrog integration
	   performs the move with the MID-step velocity, so the apex is
	   (v_drift + g*dt/2)^2 / 2g and the jump tick's own travel counts.  The
	   real figures are 43.01 standing and 45.00 crouching.  The physics was
	   always right; the test named the wrong quantity, which is exactly how a
	   later "fix" gets talked into breaking it.

	   jumpzoffset stays 0 throughout -- it is the one path in CheckJumpButton
	   that traces, and this whole file is supposed to run with no map loaded. */
	{
		float halfg = movevars.gravity * 0.5f * dt;
		float drift, stand, duck, ideal, savedft;

		ideal = (movevars.jumpvelocity * movevars.jumpvelocity) / (2 * movevars.gravity);
		Con_Printf ("  ^7info^7 %-34s %12.5f\n", "ideal apex from impulse", ideal);
		PMSrc_Check ("ideal apex is Momentum's 57 units", ideal, 57, 0.01f);

		/* ---- stock Source, pm_normalizejump 0 ---------------------------
		   Three half-steps of gravity on a jump tick -- StartGravity,
		   CheckJumpButton's own FinishGravity, and FullWalkMove's.  The
		   duplicate is real Source behaviour and is why the standing jump
		   lands 2 units under its own impulse. */
		movevars.normalizejump = 0;

		drift = PMSrc_TestJumpDrift (0, false);
		PMSrc_Check ("stock: standing takeoff velocity", pmove.velocity[2],
			movevars.jumpvelocity - 3 * halfg, 0.001f);
		stand = PMSrc_TestJumpHeight (drift);
		PMSrc_Check ("stock: standing jump height", stand, 54.758f, 0.01f);

		/* Source assigns rather than adds whenever you are ducked OR mid-duck,
		   which discards StartGravity's half-step and buys back the duplicate.
		   This case fails on the code before Patch 149, which tested
		   pmove.ducked alone. */
		drift = PMSrc_TestJumpDrift (0, true);
		PMSrc_Check ("stock: crouch-jump takeoff velocity", pmove.velocity[2],
			movevars.jumpvelocity - 2 * halfg, 0.001f);
		duck = PMSrc_TestJumpHeight (drift);
		PMSrc_Check ("stock: crouch-jump height", duck, 57.000f, 0.01f);
		PMSrc_Check ("stock: crouch-jump gains 2.24 units", duck - stand, 2.242f, 0.01f);

		/* ---- Patch 168, pm_normalizejump 1 ------------------------------
		   Add unconditionally, drop the duplicate FinishGravity: the height
		   collapses to (v0 + impulse)^2 / 2g in every case. */
		movevars.normalizejump = 1;

		drift = PMSrc_TestJumpDrift (0, false);
		PMSrc_Check ("normalized: takeoff velocity", pmove.velocity[2],
			movevars.jumpvelocity - 2 * halfg, 0.001f);
		stand = PMSrc_TestJumpHeight (drift);
		PMSrc_Check ("normalized: standing jump height", stand, 57.000f, 0.01f);

		drift = PMSrc_TestJumpDrift (0, true);
		duck = PMSrc_TestJumpHeight (drift);
		PMSrc_Check ("normalized: crouch-jump height", duck, 57.000f, 0.01f);
		PMSrc_Check ("normalized: duck state is worth nothing", duck - stand, 0, 0.0005f);

		/* Free of the tick rate, which is half the reason to do it this way
		   rather than with a constant.  100Hz is a 4.0 half-step against 6.0,
		   and the height must not notice. */
		savedft = pms_frametime;
		pms_frametime = 0.01f;
		drift = PMSrc_TestJumpDrift (0, false);
		PMSrc_Check ("normalized: same height at 100Hz",
			PMSrc_TestJumpHeight (drift), 57.000f, 0.01f);
		pms_frametime = savedft;

		/* ---- Patch 241, pm_jumpaddrise ----------------------------------
		   THIS BLOCK USED TO PIN THE OPPOSITE ANSWER, deliberately, and the
		   change is the patch.  The old case was:

		       "normalized: adds to inbound rise"  ->  (100 + I)^2 / 2g

		   with a comment saying an unconditional assign would have destroyed
		   rise you brought with you.  That is still true of an ASSIGN, and it
		   is why Patch 241 does not use one -- but "additive over ANY inbound
		   rise" was never the property worth keeping.  CategorizePosition hands
		   this function up to NON_JUMP_VELOCITY of rise and still calls it
		   grounded, and a trigger_teleport or a save-loc restore can produce
		   that rise out of nothing.  See the essay in PMSrc_CheckJumpButton.

		   So the default is now that the rise is discarded and the height is
		   the SAME 57 whatever you arrived with, and the old behaviour lives on
		   under pm_jumpaddrise 1, pinned below so both sides stay honest.
		   (57, not 45, since Patch 245 moved the impulse onto Momentum's.) */
		drift = PMSrc_TestJumpDrift (100, false);
		PMSrc_Check ("clamped: inbound 100 still reaches 57",
			PMSrc_TestJumpHeight (drift), 57.000f, 0.01f);

		/* The ceiling CategorizePosition permits.  If this one ever fails the
		   double-bounce is back. */
		drift = PMSrc_TestJumpDrift (140, false);
		PMSrc_Check ("clamped: inbound 140 still reaches 57",
			PMSrc_TestJumpHeight (drift), 57.000f, 0.01f);

		/* Nothing about the from-rest jump moved: the term subtracted is
		   exactly StartGravity's half-step, which is what a from-rest jump
		   arrives holding, so the subtraction is zero there. */
		drift = PMSrc_TestJumpDrift (0, false);
		PMSrc_Check ("clamped: from rest is unchanged",
			PMSrc_TestJumpHeight (drift), 57.000f, 0.01f);

		/* A downward inbound is deliberately NOT discarded -- see the essay.
		   FullWalkMove zeroes it on landing so this cannot normally occur, and
		   the number is here to say which way it falls if it ever does. */
		drift = PMSrc_TestJumpDrift (-100, false);
		PMSrc_Check ("clamped: a FALL is left alone",
			PMSrc_TestJumpHeight (drift),
			((movevars.jumpvelocity - 100) * (movevars.jumpvelocity - 100))
				/ (2 * movevars.gravity), 0.01f);

		/* ...and build 38 comes back whole under the cvar. */
		movevars.jumpaddrise = 1;
		drift = PMSrc_TestJumpDrift (100, false);
		PMSrc_Check ("addrise 1: build 38 adds to the rise",
			PMSrc_TestJumpHeight (drift),
			((100 + movevars.jumpvelocity) * (100 + movevars.jumpvelocity))
				/ (2 * movevars.gravity), 0.01f);
		drift = PMSrc_TestJumpDrift (0, false);
		PMSrc_Check ("addrise 1: from rest is still 57",
			PMSrc_TestJumpHeight (drift), 57.000f, 0.01f);
		movevars.jumpaddrise = 0;

		movevars.normalizejump = 0;
	}

	/* --- Hulls and eyes (Patch 241, CORRECTED in build 41) ----------------
	   Reported as "ensure the player is crouching correctly like in source, I
	   believe when you're in the air and duck, you lose hull from the top and
	   bottom".  Patch 241 answered NO, citing
	   gamemovement_momentummod.cpp:4174-4203 and its
	   `viewDelta = hullSizeNormal - hullSizeCrouch` -- "the FULL delta, not half".

	   THAT ANSWER WAS WRONG AND THE REPORT WAS RIGHT.  That file is Momentum's
	   BASE class, CGameMovement.  CMomentumGameMovement::FinishDuck overrides it
	   (mom_gamemovement.cpp:1109-1132) and multiplies by GetViewScale(), which is
	   0.5 for every CS-based mode (mom_system_gamemode.h:57; CGameMode_Surf does
	   not override it).  So at 62/45 the origin rises 8.5 and the crown drops
	   8.5: the box loses hull from the top AND the bottom, exactly as reported.

	   The lesson, recorded because it cost two wrong answers: check for a
	   CMomentumGameMovement:: override before quoting gamemovement.cpp.

	   The origin STEP itself is not pinned here: PMSrc_FinishDuck calls
	   CategorizePosition, which traces, and this whole function is supposed to
	   run with no map loaded -- the same reason jumpzoffset is held at 0 in the
	   block above.  It is an in-game check against pmove_org instead. */
	{
		float delta = PMSrc_HullDelta ();
		float shift = PMSrc_AirDuckShift ();

		PMSrc_Check ("hull: standing height", PMSrc_StandHeight(), 62.0f, 0.0001f);
		PMSrc_Check ("hull: ducked height", PMSrc_DuckHeight(), 45.0f, 0.0001f);
		PMSrc_Check ("hull: shrink when ducking", delta, 17.0f, 0.0001f);

		/* The origin only moves by the view-scaled HALF of that.  This is the
		   number the crouch-jump is made of, and the one build 40 had wrong. */
		PMSrc_Check ("hull: mid-air origin shift", shift, 8.5f, 0.0001f);
		PMSrc_Check ("hull: crown drops by the other half", delta - shift, 8.5f, 0.0001f);

		/* Reach above the takeoff SURFACE, with the shipped jump: apex 57 from
		   pm_normalizejump, + pm_jumpzoffset 1.5, + the mid-air shift.  Both
		   addends are config, so this asserts the arithmetic, not the cfg. */
		PMSrc_Check ("hull: crouch-jump reach", 57.0f + 1.5f + shift, 67.0f, 0.0001f);

		PMSrc_Check ("eye: standing", PMSrc_ViewHeight(), 64.0f, 0.0001f);
		PMSrc_Check ("eye: ducked", PMSrc_DuckViewHeight(), 47.0f, 0.0001f);

		/* The eye tracks the CROWN, not the feet: it drops by the whole hull
		   delta while the origin rises by half of it, so a mid-air crouch moves
		   the absolute eye DOWN by exactly the shift.  Build 40 asserted the eye
		   did not move at all, which was the same error in a different place. */
		PMSrc_Check ("eye: drops by exactly the hull delta",
			(PMSrc_ViewHeight() - PMSrc_DuckViewHeight()) - delta, 0.0f, 0.0001f);
		PMSrc_Check ("eye: mid-air crouch lowers the eye by the shift",
			(PMSrc_ViewHeight() - PMSrc_DuckViewHeight()) - shift, 8.5f, 0.0001f);

		/* The ends of the eye slide, which SetDuckedEyeOffset interpolates
		   between.  Pure arithmetic, no trace. */
		PMSrc_SetDuckedEyeOffset (0.0f);
		PMSrc_Check ("eye: fraction 0 is standing", pmove.viewheight, 64.0f, 0.0001f);
		PMSrc_SetDuckedEyeOffset (1.0f);
		PMSrc_Check ("eye: fraction 1 is ducked", pmove.viewheight, 47.0f, 0.0001f);
		PMSrc_SetDuckedEyeOffset (0.5f);
		PMSrc_Check ("eye: half way is half way", pmove.viewheight, 55.5f, 0.0001f);
	}

	/* --- CanUnduck (build 41) ---------------------------------------------
	   The coverage gap that let the stand-into-the-ceiling bug ship.  There was
	   no case here at all, so nothing noticed that PMSrc_CanUnduck was tracing
	   pms_standmaxs -- which carries the DUCKED height for the whole time the
	   function is worth calling, because the entity was resized by the previous
	   move's writeback (sv_user.c:8134).

	   No map is loaded, so this cannot assert on a real trace.  What it CAN do,
	   and what would have caught it, is assert that the hull the test installs is
	   the standing one even when pmove is ducked and pms_standmaxs disagrees. */
	{
		vec3_t savemins, savemaxs, savestandmins, savestandmaxs;
		qboolean saveducked = pmove.ducked;

		VectorCopy (pmove.player_mins, savemins);
		VectorCopy (pmove.player_maxs, savemaxs);
		VectorCopy (pms_standmins, savestandmins);
		VectorCopy (pms_standmaxs, savestandmaxs);

		/* Stand: pms_* are consistent, so both paths agree. */
		VectorSet (pms_standmins, -16, -16, 0);
		VectorSet (pms_standmaxs, 16, 16, 62);
		pmove.ducked = false;
		PMSrc_ApplyHull ();
		PMSrc_Check ("canunduck: standing hull is the standing height",
			pmove.player_maxs[2] - pmove.player_mins[2], 62.0f, 0.0001f);

		/* Duck, then poison pms_standmaxs exactly the way the writeback does. */
		pmove.ducked = true;
		PMSrc_ApplyHull ();
		PMSrc_Check ("canunduck: ducked hull is the ducked height",
			pmove.player_maxs[2] - pmove.player_mins[2], 45.0f, 0.0001f);
		VectorCopy (pmove.player_maxs, pms_standmaxs);	/*the entity now reports 45*/

		PMSrc_ApplyStandHull ();
		PMSrc_Check ("canunduck: stand test ignores the stale entity hull",
			pmove.player_maxs[2] - pmove.player_mins[2], 62.0f, 0.0001f);

		/*
		  And the floor, which is the bug the eye-height report actually was.

		  Poison mins[2] with -16 exactly as PF_setmodel's +/-16 fallback did
		  (pr_cmds.c:3181-3186, reached because FTESurf sets
		  sv_gameplayfix_setmodelsize_qw 1 for prop bounds and the player's
		  setmodel(self,"") ran AFTER its setsize).  Before the fix the box came
		  out -16..46: the right HEIGHT, 62, in the wrong PLACE, which put the
		  feet on the floor and the origin -- and so the eye -- 16 units above
		  it.  Height alone cannot catch that, so check both ends.
		*/
		VectorSet (pms_standmins, -16, -16, -16);
		VectorSet (pms_standmaxs, 16, 16, 16);
		pmove.ducked = false;
		PMSrc_ApplyHull ();
		PMSrc_Check ("hull floor: poisoned mins[2] is pinned to the feet",
			pmove.player_mins[2], 0.0f, 0.0001f);
		PMSrc_Check ("hull floor: standing top is 62 above the FEET",
			pmove.player_maxs[2], 62.0f, 0.0001f);
		PMSrc_Check ("hull floor: width still comes from the mod",
			pmove.player_mins[0], -16.0f, 0.0001f);

		pmove.ducked = true;
		PMSrc_ApplyHull ();
		PMSrc_Check ("hull floor: ducked top is 45 above the FEET",
			pmove.player_maxs[2], 45.0f, 0.0001f);
		PMSrc_Check ("hull floor: ducked bottom is still the feet",
			pmove.player_mins[2], 0.0f, 0.0001f);

		VectorCopy (savestandmins, pms_standmins);
		VectorCopy (savestandmaxs, pms_standmaxs);
		VectorCopy (savemins, pmove.player_mins);
		VectorCopy (savemaxs, pmove.player_maxs);
		pmove.ducked = saveducked;
	}

	/* --- Stamina -------------------------------------------------------- */
	/* The pool is milliseconds and the ratio reads it back as a fraction of
	   STAMINA_MAX: a full jump charge is (25/19)*1000 = 1315.8 ms, which is
	   exactly a 25% penalty, recovering linearly over that same 1.32 s. */
	{
		float jumpms = (25.0f / 19.0f) * 1000.0f;
		float landms = (20.0f / 19.0f) * 1000.0f;

		pmove.stamina = 0;
		PMSrc_Check ("Stamina fresh -> no penalty", PMSrc_StaminaRatio(), 1.0f, 0.0001f);

		PMSrc_StaminaSpend (25);
		PMSrc_Check ("Stamina jump charge (ms)", pmove.stamina, jumpms, 0.01f);
		PMSrc_Check ("Stamina after jump -> 0.75", PMSrc_StaminaRatio(), 0.75f, 0.0001f);

		PMSrc_StaminaSpend (20);
		PMSrc_Check ("Stamina land charge (ms)", pmove.stamina, landms, 0.01f);
		PMSrc_Check ("Stamina after land -> 0.80", PMSrc_StaminaRatio(), 0.80f, 0.0001f);

		/* Half the pool drained is half the penalty -- it is linear in ms. */
		pmove.stamina = jumpms * 0.5f;
		PMSrc_Check ("Stamina half-recovered -> 0.875", PMSrc_StaminaRatio(), 0.875f, 0.0001f);

		/* One tick of ReduceTimers is 15 ms flat, so a jump charge is gone
		   after 1315.8/15 = 87.7 ticks, i.e. 1.32 s. */
		PMSrc_Check ("Stamina recovery time (s)", jumpms * 0.001f, 25.0f/19.0f, 0.0001f);

		/* Off by cvar must be a true no-op, not a small penalty. */
		movevars.stamina = 0;
		pmove.stamina = jumpms;
		PMSrc_Check ("Stamina disabled -> no penalty", PMSrc_StaminaRatio(), 1.0f, 0.0001f);
		movevars.stamina = 1;
		pmove.stamina = 0;
	}

	/* --- Duck eye height ------------------------------------------------ */
	/* SimpleSpline is smoothstep: flat at both ends, exactly half way at the
	   midpoint.  That flatness is what makes the crouch read as smooth
	   rather than as a linear slide with two corners in it. */
	PMSrc_Check ("SimpleSpline(0)", PMSrc_SimpleSpline(0), 0, 0.0001f);
	PMSrc_Check ("SimpleSpline(0.5)", PMSrc_SimpleSpline(0.5f), 0.5f, 0.0001f);
	PMSrc_Check ("SimpleSpline(1)", PMSrc_SimpleSpline(1), 1, 0.0001f);
	PMSrc_Check ("SimpleSpline(0.25) eases in", PMSrc_SimpleSpline(0.25f), 0.15625f, 0.0001f);

	{
		float saveducktime = pmove.ducktime;
		qboolean saveducking = pmove.ducking, saveducked = pmove.ducked;
		int saveold = pmove.oldbuttons;

		pmove.ducking = false;	pmove.ducked = false;
		PMSrc_SetDuckedEyeOffset (PMSrc_DuckFraction());
		PMSrc_Check ("Eye standing", pmove.viewheight, 64, 0.001f);

		pmove.ducked = true;
		PMSrc_SetDuckedEyeOffset (PMSrc_DuckFraction());
		PMSrc_Check ("Eye ducked", pmove.viewheight, 47, 0.001f);

		/* Half way through a 0.4 s duck: ducktime has counted 200 ms down
		   from 1000, the spline is at 0.5, so the eye is midway. */
		pmove.ducking = true; pmove.ducked = false;
		pmove.oldbuttons = BUTTON_DUCK;
		pmove.ducktime = PMSRC_DUCK_TIMER - 200;
		PMSrc_SetDuckedEyeOffset (PMSrc_DuckFraction());
		PMSrc_Check ("Eye mid-duck (0.2s of 0.4s)", pmove.viewheight, 55.5f, 0.001f);

		/* Half way through a 0.2 s unduck, from fully ducked. */
		pmove.ducked = true;
		pmove.oldbuttons = 0;
		pmove.ducktime = PMSRC_DUCK_TIMER - 100;
		PMSrc_SetDuckedEyeOffset (PMSrc_DuckFraction());
		PMSrc_Check ("Eye mid-unduck (0.1s of 0.2s)", pmove.viewheight, 55.5f, 0.001f);

		pmove.ducktime = saveducktime;
		pmove.ducking = saveducking;
		pmove.ducked = saveducked;
		pmove.oldbuttons = saveold;
	}

	/* --- pmsourcestate_t round trip (guards Patch 132) -------------------

	   None of this struct is networked.  The client has to re-seed it from
	   its own propagation copy at the head of every prediction replay, and
	   for a long time only ONE of the two paths that needed to do so did --
	   which is what made the camera shake when you ducked in mid-air, because
	   a replay seeded with ducked==false runs FinishDuck a second time and
	   adds the +18 origin bump twice (see cl_pred.c Patch 132).

	   A save/load round trip cannot catch a missing CALL, but it does pin the
	   two halves together: if a field is ever added to the struct and to only
	   one of Save/Load, this fails immediately instead of becoming another
	   silent prediction divergence. */
	{
		pmsourcestate_t st;
		float saveducktime = pmove.ducktime;
		qboolean saveducking = pmove.ducking, saveducked = pmove.ducked;
		int saveold = pmove.oldbuttons;
		float savecarry = pmove.msec_carry;

		pmove.ducktime   = 812.5f;
		pmove.ducking    = true;
		pmove.ducked     = true;
		pmove.oldbuttons = BUTTON_DUCK;
		pmove.msec_carry = 0.004f;
		PMSrc_SaveState (&st);

		/* Scribble over every one of them, the way a replay's stale ring slot
		   does, then restore and check nothing was lost on the way. */
		pmove.ducktime   = 0;
		pmove.ducking    = false;
		pmove.ducked     = false;
		pmove.oldbuttons = 0;
		pmove.msec_carry = 0;
		PMSrc_LoadState (&st);

		PMSrc_Check ("State round trip: ducktime", pmove.ducktime, 812.5f, 0.0001f);
		PMSrc_Check ("State round trip: ducking", pmove.ducking ? 1 : 0, 1, 0.0001f);
		PMSrc_Check ("State round trip: ducked", pmove.ducked ? 1 : 0, 1, 0.0001f);
		PMSrc_Check ("State round trip: oldbuttons", (float)(pmove.oldbuttons & BUTTON_DUCK),
			(float)BUTTON_DUCK, 0.0001f);
		PMSrc_Check ("State round trip: msec_carry", pmove.msec_carry, 0.004f, 0.000001f);

		pmove.ducktime   = saveducktime;
		pmove.ducking    = saveducking;
		pmove.ducked     = saveducked;
		pmove.oldbuttons = saveold;
		pmove.msec_carry = savecarry;
	}

	/* --- The GROUND strafe optimum ---------------------------------------

	   The HUD's strafe bar needs a target turn rate on the ground as well as
	   in the air, and the two are NOT the same formula.  In the air,
	   AirAccelerate measures addspeed against a wishspeed CLAMPED to the
	   30 u/s cap, so a = 30 - dot(v,w) and d|v|^2 = 900 - dot^2, maximised at
	   dot = 0: wishdir exactly perpendicular.  On the ground there is no such
	   cap -- Accelerate applies a = min(A, wishspeed - dot) with A a CONSTANT
	   (accel * dt * wishspeed * surfacefriction, 18.75 here) -- so

	       d|v|^2 = 2*A*dot + A^2     while dot < wishspeed - A   (rising)
	              = wishspeed^2 - dot^2   beyond it               (falling)

	   and the maximum sits on the boundary, at dot = wishspeed - A, i.e.

	       cos(theta) = (wishspeed - A) / |v_after_friction|

	   Below wishspeed - A that angle does not exist and the answer is simply
	   "run forwards".  The numbers below come from a brute-force search over
	   this exact code path at 0.001-degree resolution, and are checked here
	   rather than in QuakeC because it is THIS Friction/Accelerate pair that
	   defines them.

	   The perpendicular rows are the point of the whole block: reusing the
	   air answer on the ground is not a small inaccuracy.  At 250 u/s it
	   turns a gain of +3.47 into a loss of -14.25. */
	{
		float A = movevars.accelerate * dt * 250.0f;
		float sp, spf, ct, th, gain, turn;
		int i;
		static const float speeds[3] = {150, 250, 400};
		static const float expgain[3] = {9.7500f, 3.4728f, -12.1863f};
		static const float expturn[3] = {0.0f, 50.277f, 145.653f};

		PMSrc_Check ("Ground accelspeed A", A, 18.75f, 0.0001f);

		for (i = 0; i < 3; i++)
		{
			sp = speeds[i];
			VectorSet (pmove.velocity, sp, 0, 0);
			pmove.onground = true;
			PMSrc_Friction ();
			spf = pmove.velocity[0];

			ct = (250.0f - A) / spf;
			if (ct > 1)			/* too slow for a perpendicular component to pay */
				ct = 1;
			th = acos (ct);

			VectorSet (wishdir, cos(th), sin(th), 0);
			PMSrc_Accelerate (wishdir, 250, movevars.accelerate);

			gain = sqrt(pmove.velocity[0]*pmove.velocity[0] +
			            pmove.velocity[1]*pmove.velocity[1]) - sp;
			turn = (atan2(pmove.velocity[1], pmove.velocity[0]) * 57.29577951308232f) / dt;

			PMSrc_Check (va("Ground optimum gain @%g", sp), gain, expgain[i], 0.002f);
			PMSrc_Check (va("Ground ideal turn @%g", sp), turn, expturn[i], 0.02f);
		}

		/* And the air answer, applied on the ground, at the speed where it
		   hurts most. */
		VectorSet (pmove.velocity, 250, 0, 0);
		pmove.onground = true;
		PMSrc_Friction ();
		VectorSet (wishdir, 0, 1, 0);
		PMSrc_Accelerate (wishdir, 250, movevars.accelerate);
		gain = sqrt(pmove.velocity[0]*pmove.velocity[0] +
		            pmove.velocity[1]*pmove.velocity[1]) - 250;
		PMSrc_Check ("Ground perpendicular is WORSE @250", gain, -14.2532f, 0.002f);
	}

	/* --- Prestrafe: the ground fixed point at cl_yawspeed 120 -------------
	   Hold one strafe key and turn at a constant rate and the ground move
	   settles at a speed above maxspeed, because Accelerate limits the
	   PROJECTION along wishdir while the magnitude is free to grow.

	   Two things worth pinning.  The first is the number itself: Momentum
	   reads 289 here and we used to read 278, and the entire difference is
	   that its ground speed is 260 where CS:S's knife is 250 -- 1.112862x
	   either side.  The second is that sv_accelerate DOES NOT ENTER IT.  At
	   this turn rate the tick needs 11.5 u/s and even accel 5 offers 18.75,
	   so accelspeed saturates at addspeed every tick, every tick ends with
	   exactly maxspeed along wishdir, and the whole fixed point is the
	   perpendicular remainder f*M*sin(t)/(1 - f*cos(t)).  Checking 5, 150 and
	   10000 all land on the same value is what makes that an elimination
	   rather than an assertion -- it is the reason an air-accel setting could
	   never have explained the gap.
	   --------------------------------------------------------------------- */
	{
		static const float pms[2]   = {250, 260};
		static const float pexp[2]  = {278.2156f, 289.3443f};
		static const float paccel[3]= {5, 150, 10000};
		const float dyaw = 120.0f * 0.017453292519943295f * dt;
		float yaw, spd;
		int i, k, n;

		for (i = 0; i < 2; i++)
		for (k = 0; k < 3; k++)
		{
			VectorSet (pmove.velocity, pms[i], 0, 0);
			yaw = 0;
			for (n = 0; n < 4000; n++)
			{
				pmove.onground = true;
				PMSrc_Friction ();
				VectorSet (wishdir, cos(yaw), sin(yaw), 0);
				PMSrc_Accelerate (wishdir, pms[i], paccel[k]);
				yaw += dyaw;
			}
			spd = sqrt(pmove.velocity[0]*pmove.velocity[0] +
			           pmove.velocity[1]*pmove.velocity[1]);
			PMSrc_Check (va("Prestrafe @maxspeed %g, accel %g", pms[i], paccel[k]),
						 spd, pexp[i], 0.01f);
		}
	}

	/* --- Board telemetry (Patch 131) -------------------------------------

	   The HUD's board panel does its arithmetic in QuakeC, but it rests
	   entirely on one property of the function below: at overbounce 1,
	   ClipVelocity is an ORTHOGONAL PROJECTION, so it removes exactly
	   (v.n)n and leaves |v| * sin(approach).  That identity is what lets
	   the panel report a cost without measuring a difference -- and
	   measuring the difference is what does not work, because a surfer's
	   per-tick background motion is larger than the cost being measured.

	   So test the identity, in the real function, rather than the QC. */
	{
		vec3_t bn, bv, bout;
		float s2 = 0.7071067811865476f;   /* sin/cos 45 */

		/* Straight down into a 45-degree ramp.  Approach is 45 degrees off
		   the normal, so sinA = cos45 and exactly that share survives. */
		VectorSet (bn, s2, 0, s2);
		VectorSet (bv, 0, 0, -1000);
		PMSrc_ClipVelocity (bv, bn, bout, 1);
		PMSrc_Check ("Board 45deg head-on keeps", VectorLength(bout), 707.10678f, 0.01f);

		/* The same ramp, ridden exactly along its face: v.n is zero, the
		   projection removes nothing, and a perfect board costs nothing.
		   This is the 100%-kept end of the scale. */
		VectorSet (bv, 1000 * s2, 0, -1000 * s2);
		PMSrc_ClipVelocity (bv, bn, bout, 1);
		PMSrc_Check ("Board parallel keeps all", VectorLength(bout), 1000, 0.01f);

		/* Dead-on into a wall-steep face: everything into the plane goes. */
		VectorSet (bn, 1, 0, 0);
		VectorSet (bv, -1000, 0, 0);
		PMSrc_ClipVelocity (bv, bn, bout, 1);
		PMSrc_Check ("Board head-on keeps none", VectorLength(bout), 0, 0.01f);

		/* The ramp band the telemetry fires in.  0.7 is Source's own
		   standable cut and is INCLUDED (TryPlayerMove's test is a strict
		   >), 0.1 is the wall cut. */
		PMSrc_Check ("Board ramp band top", PMSrc_Standable(), 0.7f, 0.0001f);
		PMSrc_Check ("Board ramp band bottom", PMSRC_MIN_RAMP_NZ, 0.1f, 0.0001f);
		PMSrc_Check ("Board air gate (s)", PMSRC_BOARD_AIRGATE, 0.08f, 0.0001f);
	}

	/* --- What a perfect turn is worth: air, and on a ramp (Build 7) ------

	   The HUD's `best` line used to be drawn only while standing, which is the
	   one place a surfer never is.  Two more forms back it now (sh_strafe.qc);
	   this pins both to the real functions rather than to a second reading of
	   them, which is the only thing this file can do that the QC cannot.

	   AIR is exact and closed-form.  With dot(v, wishdir) = 0 the whole air cap
	   is applied perpendicular to the velocity, so the new speed is just the
	   hypotenuse and Strafe_AirBest is sqrt(v^2 + cap^2) - v. */
	{
		vec3_t pn, pv, pw, pu, pz, pin, pout;
		float a, c, wn, G, rate, model, real, span, mid, lo, s2, best2;
		int i, k;

		VectorSet (pmove.velocity, 1000, 0, 0);
		VectorSet (wishdir, 0, 1, 0);
		PMSrc_AirAccelerate (wishdir, 250, movevars.airaccelerate);
		PMSrc_Check ("Air best @1000 perpendicular",
			sqrt(pmove.velocity[0]*pmove.velocity[0] +
			     pmove.velocity[1]*pmove.velocity[1]) - 1000,
			sqrt(1000.0f*1000.0f + 30.0f*30.0f) - 1000.0f, 0.0001f);

		/* RAMP has no closed form, because wishdir is HORIZONTAL: the wn = 0
		   heading that free air optimises to is generally not reachable, so
		   Strafe_PlaneBest searches the heading instead.  I had the closed form
		   in first and it overstates by up to 44% across the slope -- it
		   evaluates the objective somewhere the player cannot point.

		   What can be pinned here is the identity the search evaluates:

		       |clip(v + a*w - G*z)|^2 == |u|^2 + 2a(u.w) + a^2(1 - wn^2)

		   with u = v - G*P(z).  That single step is everything between the real
		   tick and the objective function, and it is checked against the real
		   ClipVelocity below on an nz = 0.6 face at 1000 u/s across the slope. */
		G    = movevars.gravity * dt;
		rate = movevars.airaccelerate * 250.0f * dt;

		VectorSet (pn, 0.8f, 0, 0.6f);
		VectorSet (pv, 0, 1000, 0);            /* in the plane: dot(pv,pn) == 0 */
		VectorSet (pw, 1, 0, 0);               /* horizontal, as AirMove builds it */

		c = DotProduct (pv, pw);
		a = movevars.maxairspeed - c;          /* AirAccelerate's addspeed */
		if (a > rate)
			a = rate;
		wn = DotProduct (pw, pn);

		/* P(z) = z - nz*n */
		pz[0] = -pn[0]*pn[2];
		pz[1] = -pn[1]*pn[2];
		pz[2] = 1.0f - pn[2]*pn[2];
		VectorMA (pv, -G, pz, pu);

		model = DotProduct(pu,pu) + 2*a*DotProduct(pu,pw) + a*a*(1 - wn*wn);

		VectorMA (pv, a, pw, pin);
		pin[2] -= G;
		PMSrc_ClipVelocity (pin, pn, pout, 1);
		real = DotProduct (pout, pout);

		/* Compared as SPEEDS, not as the squares.  |v|^2 here is 1.0e6, and
		   vec3_t is float32, so DotProduct rounding alone moves the square by
		   ~0.06 -- 6e-8 relative, and nothing at all in the quantity anyone
		   cares about.  Checking the squares needed a tolerance so loose it
		   would not have caught a real error; checking the speeds is both
		   tighter in the units that matter and immune to that. */
		PMSrc_Check ("Plane objective == real clip", sqrt(model), sqrt(real), 0.001f);
		PMSrc_Check ("Plane gain @nz0.6 cross, heading 0",
			sqrt(model) - 1000.0f, 0.380807f, 0.0005f);

		/* And the SEARCH SCHEDULE itself, which is the part most likely to be
		   quietly wrong: 72 coarse steps then two refinement rounds of 12 must
		   land within a rounding error of the true maximum.  Brute force here
		   at 0.01 degrees; the true answer on this face is +0.435862 u/s at a
		   heading of 0.4696 degrees, which is nowhere near the wn = 0 direction
		   the closed form assumed. */
		best2 = DotProduct (pu, pu);
		for (i = 0; i < 36000; i++)
		{
			float ph = (2*M_PI) * i / 36000.0f;
			pw[0] = cos(ph); pw[1] = sin(ph); pw[2] = 0;
			c = DotProduct(pv, pw);
			a = movevars.maxairspeed - c;
			if (a > rate) a = rate;
			if (a <= 0) continue;
			wn = DotProduct(pw, pn);
			s2 = DotProduct(pu,pu) + 2*a*DotProduct(pu,pw) + a*a*(1 - wn*wn);
			if (s2 > best2) best2 = s2;
		}
		PMSrc_Check ("Plane best @nz0.6 cross 1000 (brute)",
			sqrt(best2) - 1000.0f, 0.435862f, 0.0005f);

		/* The QC's schedule, replicated exactly, against that. */
		{
			float qbest = DotProduct (pu, pu);
			mid = 0;
			span = (2*M_PI) / 72.0f;
			for (i = 0; i < 72; i++)
			{
				float ph = i * span;
				pw[0] = cos(ph); pw[1] = sin(ph); pw[2] = 0;
				c = DotProduct(pv, pw);
				a = movevars.maxairspeed - c;
				if (a > rate) a = rate;
				if (a <= 0) continue;
				wn = DotProduct(pw, pn);
				s2 = DotProduct(pu,pu) + 2*a*DotProduct(pu,pw) + a*a*(1 - wn*wn);
				if (s2 > qbest) { qbest = s2; mid = ph; }
			}
			for (k = 0; k < 2; k++)
			{
				lo = mid - span;
				span = (2*span) / 12.0f;
				for (i = 0; i <= 12; i++)
				{
					float ph = lo + i * span;
					pw[0] = cos(ph); pw[1] = sin(ph); pw[2] = 0;
					c = DotProduct(pv, pw);
					a = movevars.maxairspeed - c;
					if (a > rate) a = rate;
					if (a <= 0) continue;
					wn = DotProduct(pw, pn);
					s2 = DotProduct(pu,pu) + 2*a*DotProduct(pu,pw) + a*a*(1 - wn*wn);
					if (s2 > qbest) { qbest = s2; mid = ph; }
				}
			}
			/* 0.002 u/s is the worst shortfall this schedule showed anywhere in
			   a sweep over nz 0.15-0.69, both tangent directions, 300-3000 u/s.
			   If this ever trips, the schedule was "improved" without redoing
			   that sweep -- see the comment on PLANEBEST_COARSE. */
			PMSrc_Check ("Plane search schedule finds the max",
				sqrt(best2) - sqrt(qbest), 0, 0.002f);
		}
	}

	/*
	  --- surfacefriction, and the duck crop -------------------------------

	  FTESurf Build 8.  The HUD's strafe model had surfacefriction HARDCODED to
	  1 and applied the duck speed crop unconditionally.  Both are wrong in the
	  air, and the air is where surfing happens:

	    * CategorizePosition quarters surfacefriction while airborne AND rising,
	      but only below NON_JUMP_VELOCITY -- above 140 it takes the first
	      branch, never traces for ground, and friction stays 1.  So the
	      quartered band is strictly 0 < vz <= 140.  It multiplies straight into
	      AirAccelerate's accelspeed, so the gain there is a QUARTER.

	    * HandleDuckingSpeedCrop is `ducked && onground`.  Crouched in the air --
	      which is every crouched surfer, since surfing is never onground -- the
	      move values are NOT cropped and the full 250 wishspeed applies.

	  These drive the real functions with the real state rather than restating
	  the rule, which is the only way this file can check the QC's copy of it.
	  All three regimes, because the two boundaries are the whole point.
	*/
	{
		float g1, gq, edge;
		qboolean saveduck = pmove.ducked, saveground = pmove.onground;
		float savefwd = pms_forwardmove, savesid = pms_sidemove, saveup = pms_upmove;

		/* Reference: perpendicular AirAccelerate at friction 1. */
		pmove.surfacefriction = 1.0f;
		VectorSet (pmove.velocity, 1000, 0, 0);
		VectorSet (wishdir, 0, 1, 0);
		PMSrc_AirAccelerate (wishdir, 250, movevars.airaccelerate);
		g1 = pmove.velocity[1];
		PMSrc_Check ("air gain, friction 1", g1, 30, 0.0001f);

		/* THE ANSWER, and it is not the one I expected: at CS:S settings the
		   quarter DOES NOT BIND.  accelspeed is clamped to addspeed, addspeed
		   can never exceed the 30 u/s cap, and

		       150 * 250 * 0.015 * 0.25 = 140.6   >>   30

		   so the gain is 30 at either friction.  This check is here to say that
		   out loud: if it ever starts failing, either the cap or the tuning
		   moved and the HUD's model needs revisiting with it. */
		pmove.surfacefriction = 0.25f;
		VectorSet (pmove.velocity, 1000, 0, 0);
		PMSrc_AirAccelerate (wishdir, 250, movevars.airaccelerate);
		gq = pmove.velocity[1];
		PMSrc_Check ("deadstrafe: no effect at aa=150", gq, g1, 0.0001f);

		/* ...and that it DOES bind once the rate is the binding constraint,
		   which is what makes carrying surfacefriction in the model worth
		   anything at all.  At sv_airaccelerate 10: 10*250*0.015 = 37.5 at
		   friction 1 (still capped to 30), 9.375 at 0.25 (below the cap, so
		   the rate wins).  Crossover is airaccelerate 32. */
		pmove.surfacefriction = 1.0f;
		VectorSet (pmove.velocity, 1000, 0, 0);
		PMSrc_AirAccelerate (wishdir, 250, 10);
		PMSrc_Check ("deadstrafe: aa=10 friction 1", pmove.velocity[1], 30, 0.0001f);

		pmove.surfacefriction = 0.25f;
		VectorSet (pmove.velocity, 1000, 0, 0);
		PMSrc_AirAccelerate (wishdir, 250, 10);
		PMSrc_Check ("deadstrafe: aa=10 friction 0.25", pmove.velocity[1], 9.375f, 0.0001f);

		/* The vz boundary the HUD reads off.  Stated as the predicate rather
		   than traced, because a real CategorizePosition needs a map -- but the
		   predicate IS what cl_hud.qc evaluates, so pinning it here is what
		   keeps the two copies of the rule from drifting. */
		edge = (140.0f > 0 && 140.0f <= PMSRC_NON_JUMP_VELOCITY) ? 0.25f : 1.0f;
		PMSrc_Check ("friction rule at vz=140", edge, 0.25f, 0.0001f);
		edge = (141.0f > 0 && 141.0f <= PMSRC_NON_JUMP_VELOCITY) ? 0.25f : 1.0f;
		PMSrc_Check ("friction rule at vz=141", edge, 1.0f, 0.0001f);
		edge = (-1.0f > 0 && -1.0f <= PMSRC_NON_JUMP_VELOCITY) ? 0.25f : 1.0f;
		PMSrc_Check ("friction rule while falling", edge, 1.0f, 0.0001f);

		/* The duck crop, straight through the real function.  Ground: cropped.
		   Air: untouched -- which is the half the HUD had wrong.  Like the
		   friction above it moves no displayed number at aa=150 (the air gain
		   is capped at 30 for ws=85 and ws=250 alike); what it protects is the
		   GROUND gain, 5*0.015*ws, where 85 and 250 are 6.4 against 18.8. */
		pmove.surfacefriction = 1.0f;
		pmove.ducked = true;
		pmove.onground = true;
		pms_forwardmove = 250; pms_sidemove = 0; pms_upmove = 0;
		PMSrc_HandleDuckingSpeedCrop ();
		PMSrc_Check ("duck crop on the ground", pms_forwardmove, 250*0.34f, 0.001f);

		pmove.onground = false;
		pms_forwardmove = 250; pms_sidemove = 0; pms_upmove = 0;
		PMSrc_HandleDuckingSpeedCrop ();
		PMSrc_Check ("duck crop in the air (none)", pms_forwardmove, 250, 0.001f);

		pmove.ducked = saveduck;
		pmove.onground = saveground;
		pms_forwardmove = savefwd; pms_sidemove = savesid; pms_upmove = saveup;
	}

	/* --- The walk key, and the move-value cap it used to be tangled with --
	   The second block is the one that explains "I set sv_maxspeed 260 and
	   nothing moved": the client sends forwardmove in UNITS, and a move value
	   BELOW maxspeed is the binding cap, silently.  Prestrafe is
	   min(cl_sidespeed, sv_maxspeed) * 1.112862, not sv_maxspeed * 1.112862.
	   ---------------------------------------------------------------------- */
	{
		float savems = movevars.maxspeed, savews = movevars.walkspeed;
		float savefwd = pms_forwardmove, savesid = pms_sidemove;
		float saveburt = pmove.cmd.buttons;
		vec3_t savefw, saversg;
		float ws;

		VectorCopy (pms_forward, savefw);
		VectorCopy (pms_right, saversg);

		movevars.maxspeed = 260;
		movevars.walkspeed = 0.52;

		pmove.cmd.buttons = 0;
		PMSrc_Check ("maxspeed, running", PMSrc_EffectiveMaxSpeed(), 260, 0.001f);
		pmove.cmd.buttons = BUTTON_SPEED;
		PMSrc_Check ("maxspeed, walking", PMSrc_EffectiveMaxSpeed(), 135.2f, 0.001f);
		movevars.walkspeed = 0;
		PMSrc_Check ("maxspeed, walk disabled", PMSrc_EffectiveMaxSpeed(), 260, 0.001f);
		movevars.walkspeed = 0.52;
		pmove.pm_type = PM_SPECTATOR;
		PMSrc_Check ("maxspeed, walk ignored in noclip", PMSrc_EffectiveMaxSpeed(), 260, 0.001f);
		pmove.pm_type = PM_NORMAL;
		pmove.cmd.buttons = 0;

		/* wishspeed straight out of the move values, at maxspeed 260 */
		pms_maxspeed = 260;
		VectorSet (pms_forward, 1, 0, 0);
		VectorSet (pms_right, 0, -1, 0);
		pms_sidemove = 0;

		pms_forwardmove = 250;
		ws = PMSrc_WishDir (wishdir);
		PMSrc_Check ("cl_forwardspeed 250 CAPS wishspeed", ws, 250, 0.001f);

		pms_forwardmove = 450;
		ws = PMSrc_WishDir (wishdir);
		PMSrc_Check ("cl_forwardspeed 450 reaches maxspeed", ws, 260, 0.001f);

		movevars.maxspeed = savems;
		movevars.walkspeed = savews;
		pms_forwardmove = savefwd;
		pms_sidemove = savesid;
		pmove.cmd.buttons = saveburt;
		VectorCopy (savefw, pms_forward);
		VectorCopy (saversg, pms_right);
	}

	/* --- Hulls ---------------------------------------------------------- */
	PMSrc_Check ("stand height", PMSrc_StandHeight(), 62, 0.001f);
	PMSrc_Check ("duck height", PMSrc_DuckHeight(), 45, 0.001f);
	PMSrc_Check ("hull shrink when ducking", PMSrc_HullDelta(), 17, 0.001f);
	PMSrc_Check ("crouch-jump feet lift", PMSrc_AirDuckShift(), 8.5f, 0.001f);
	PMSrc_Check ("standable normal", PMSrc_Standable(), 0.7f, 0.0001f);
	PMSrc_Check ("crouch-jump reach", 57 + 1.5f + PMSrc_AirDuckShift(), 67, 0.001f);

	/* --- restore -------------------------------------------------------- */
	movevars = savemv;
	VectorCopy (save_vel, pmove.velocity);
	VectorCopy (save_org, pmove.origin);
	pmove.surfacefriction = sf;
	pmove.pm_type = savetype;
	pmove.waterjumptime = savewjt;
	pmove.stamina = savestam;
	pms_frametime = saved_ft;
	pmove.cmd.buttons = 0;

	if (pms_test_fails)
		Con_Printf ("^1%i check(s) FAILED^7\n", pms_test_fails);
	else
		Con_Printf ("^2all checks passed^7\n");
}

void PMSrc_Init (void)
{
	Cmd_AddCommandD ("pm_selftest", PMSrc_SelfTest_f,
		"FTESurf: verify the Counter-Strike: Source movement maths against known values. Runs without a map loaded.");
}
