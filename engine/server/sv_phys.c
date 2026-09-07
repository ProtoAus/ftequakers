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
// sv_phys.c

#include "quakedef.h"
#if !defined(CLIENTONLY) || defined(CSQC_DAT)

#include "pr_common.h"

/*


pushmove objects do not obey gravity, and do not interact with each other or trigger fields, but block normal movement and push normal objects when they move.

onground is set for toss objects when they come to a complete rest.  it is set for steping or walking objects

doors, plats, etc are SOLID_BSP, and MOVETYPE_PUSH
bonus items are SOLID_TRIGGER touch, and MOVETYPE_TOSS
corpses are SOLID_NOT and MOVETYPE_TOSS
crates are SOLID_BBOX and MOVETYPE_TOSS
walking monsters are SOLID_SLIDEBOX and MOVETYPE_STEP
flying/floating monsters are SOLID_SLIDEBOX and MOVETYPE_FLY

solid_edge items only clip against bsp models.

*/

//FTESurf: CVAR_SERVERINFO so client prediction can see it.  Momentum has exactly
//ONE velocity cvar and this is it (CGameModeBase::SetGameModeVars sets it to 3500);
//pm_maxvelocity now merely overrides it when non-zero.  Without the serverinfo flag
//the client could not read this number at all, so any map raising it desynced by
//construction.
cvar_t	sv_maxvelocity = CVARFD("sv_maxvelocity","10000", CVAR_SERVERINFO, "Maximum speed an entity may reach. Under pm_physicsmode 1 this is clamped PER AXIS, as Source's CheckVelocity does, so diagonal speed can exceed it; otherwise the whole vector is scaled down.");

cvar_t	sv_gravity			 = CVAR( "sv_gravity", "800");
cvar_t	sv_stopspeed		 = CVAR( "sv_stopspeed", "100");
cvar_t	sv_maxspeed			 = CVAR( "sv_maxspeed", "320");
cvar_t	sv_spectatormaxspeed = CVAR( "sv_spectatormaxspeed", "500");
cvar_t	sv_accelerate		 = CVAR( "sv_accelerate", "10");
cvar_t	sv_airaccelerate	 = CVAR( "sv_airaccelerate", "0.7");
cvar_t	sv_wateraccelerate	 = CVAR( "sv_wateraccelerate", "10");
cvar_t	sv_friction			 = CVAR( "sv_friction", "4");
cvar_t	sv_waterfriction	 = CVAR( "sv_waterfriction", "4");
cvar_t	sv_wallfriction		 = CVARD( "sv_wallfriction", "1", "Additional friction when running into walls");
cvar_t	sv_gameplayfix_noairborncorpse		= CVAR(  "sv_gameplayfix_noairborncorpse", "0");
cvar_t	sv_gameplayfix_multiplethinks		= CVARAD("sv_gameplayfix_multiplethinks", "1", /*dp*/"sv_gameplayfix_multiplethinksperframe", "Enables multiple thinks per entity per frame so small nextthink times are accurate. QuakeWorld mods expect a value of 1, while NQ expects 0.");
cvar_t	sv_gameplayfix_stepdown				= CVARD( "sv_gameplayfix_stepdown", "0", "Attempt to step down steps, instead of only up them. Affects non-predicted movetype_walk.");
cvar_t	sv_gameplayfix_bouncedownslopes		= CVARD( "sv_gameplayfix_grenadebouncedownslopes", "0", "MOVETYPE_BOUNCE speeds are calculated relative to the impacted surface, instead of the vertical, reducing the chance of grenades just sitting there on slopes.");
cvar_t	sv_gameplayfix_trappedwithin		= CVARD( "sv_gameplayfix_trappedwithin", "0", "Blocks further entity movement when an entity is already inside another entity. This ensures that bsp precision issues cannot allow the entity to completely pass through eg the world.");
//cvar_t	sv_gameplayfix_radialmaxvelocity	= CVARD( "sv_gameplayfix_radialmaxvelocity", "0", "Applies maxvelocity radially instead of axially.");
#if !defined(CLIENTONLY) && defined(NQPROT) && defined(HAVE_LEGACY)
cvar_t	sv_gameplayfix_spawnbeforethinks	= CVARD( "sv_gameplayfix_spawnbeforethinks", "0", "Fixes an issue where player thinks (including Pre+Post) can be called before PutClientInServer. Unfortunately at least one mod depends upon PreThink being called first in order to correctly determine spawn positions.");
#endif
cvar_t	dpcompat_noretouchground	= CVARD( "dpcompat_noretouchground", "0", "Prevents entities that are already standing on an entity from touching the same entity again.");
cvar_t	sv_sound_watersplash = CVAR( "sv_sound_watersplash", "misc/h2ohit1.wav");
cvar_t	sv_sound_land		 = CVAR( "sv_sound_land", "demon/dland2.wav");
cvar_t	sv_stepheight		 = CVARAFD("pm_stepheight", "",	/*dp*/"sv_stepheight", CVAR_SERVERINFO, "If empty, the value "STRINGIFY(PM_DEFAULTSTEPHEIGHT)" will be used instead. This is the size of the step you can step up or down.");
extern cvar_t sv_nqplayerphysics;

cvar_t	pm_ktjump			 = CVARF("pm_ktjump", "", CVAR_SERVERINFO);
cvar_t	pm_bunnyspeedcap	 = CVARFD("pm_bunnyspeedcap", "", CVAR_SERVERINFO, "0 or 1, ish. If the player is traveling faster than this speed while turning, their velocity will be gracefully reduced to match their current maxspeed. You can still rocket-jump to gain high velocity, but turning will reduce your speed back to the max. This can be used to disable bunny hopping.");
cvar_t	pm_watersinkspeed	 = CVARFD("pm_watersinkspeed", "", CVAR_SERVERINFO, "This is the speed that players will sink at while inactive in water. Empty means 60.");
cvar_t	pm_flyfriction		= CVARFD("pm_flyfriction", "", CVAR_SERVERINFO, "Amount of friction that applies in fly or 6dof mode. Empty means 4.");
cvar_t	pm_slidefix			 = CVARFD("pm_slidefix", "", CVAR_SERVERINFO, "Fixes an issue when walking down slopes (ie: so they act more like slopes and not a series of steps)");
cvar_t	pm_slidyslopes		 = CVARFD("pm_slidyslopes", "", CVAR_SERVERINFO, "Replicates NQ behaviour, where players will slowly slide down ramps. Generally requires 'pm_noround 1' too, otherwise the effect rounds to nothing.");
cvar_t	pm_bunnyfriction	= CVARFD("pm_bunnyfriction", "", CVAR_SERVERINFO, "Replicates NQ behaviour, ensuring that there's at least a frame of friction while jumping - friction is proportional to tick rate.");
cvar_t	pm_autobunny		= CVARFD("pm_autobunny", "", CVAR_SERVERINFO, "Players will continue jumping without needing to release the jump button.");
cvar_t	pm_airstep			 = CVARAFD("pm_airstep", "", /*dp*/"sv_jumpstep", CVAR_SERVERINFO, "Allows players to step up while jumping. This makes stairs more graceful but also increases potential jump heights.");
cvar_t	pm_pground			 = CVARFD("pm_pground", "", CVAR_SERVERINFO, "Use persisten onground state instead of recalculating every frame."CON_WARNING"Do NOT use with nq mods, as most nq mods will interfere with onground state, resulting in glitches.");
cvar_t	pm_stepdown			 = CVARFD("pm_stepdown", "", CVAR_SERVERINFO, "Causes physics to stick to the ground, instead of constantly losing traction while going down steps.");
cvar_t	pm_walljump			 = CVARFD("pm_walljump", "", CVAR_SERVERINFO, "Allows the player to bounce off walls while arborne.");
cvar_t	pm_edgefriction		 = CVARAFD("pm_edgefriction", "", /*nq*/"edgefriction", CVAR_SERVERINFO, "Increases friction when about to walk over a cliff, so you're less likely to plummet by mistake. When empty defaults to 2, but uses a tracebox instead of a traceline to detect the drop.");

//FTESurf: Counter-Strike: Source movement (engine/common/pm_source.c).
//pm_physicsmode 0 keeps QuakeWorld's pmove.c and none of the rest is read,
//so this whole block is inert for every other game on this engine.
cvar_t	pm_physicsmode		 = CVARFD("pm_physicsmode", "0", CVAR_SERVERINFO, "0: QuakeWorld movement (pmove.c).\n1: Counter-Strike: Source movement (pm_source.c) - fixed tick, Source air acceleration, ClipVelocity with the adjust pass, Source ducking.");
cvar_t	pm_ticrate			 = CVARFD("pm_ticrate", "0.015", CVAR_SERVERINFO, "Fixed physics step for pm_physicsmode 1, in seconds. 0.015 is 66.666.. Hz, which is 15ms exactly and therefore survives usercmd_t's integer msec field losslessly. Set cl_netfps to match.");
cvar_t	pm_maxairspeed		 = CVARFD("pm_maxairspeed", "30", CVAR_SERVERINFO, "Source's GetAirSpeedCap: the most air acceleration may add along wishdir per tick. 30 in every Source game. This is the budget air-strafing spends.");
cvar_t	pm_jumpvelocity		 = CVARFD("pm_jumpvelocity", "301.9933774", CVAR_SERVERINFO, "Upward impulse on jump. Momentum's CGameModeBase::GetJumpFactor() is sqrt(57*2*800) = 301.9933774, which every CS-based mode (surf, bhop, KZ) inherits. CS:S's own 268.3281572999747 (a 45 unit apex) is what FTESurf shipped up to build 40, and it made every plain jump 9.76 units lower than Momentum's.");
cvar_t	pm_standablenormal	 = CVARFD("pm_standablenormal", "0.7", CVAR_SERVERINFO, "Surfaces whose normal.z is below this leave you airborne and sliding. 0.7 (about 45 degrees) in every Source game. This one number is the whole of surfing.");
//FTESurf Patch 260.  Ladders, ON by default: they are intended map content, and
//242 library maps ship them.  0 bypasses the mover entirely and restores the old
//behaviour exactly, which is the bisection handle if a ladder ever grabs someone
//who did not want it.
cvar_t	pm_ladders			 = CVARFD("pm_ladders", "1", CVAR_SERVERINFO, "Climb brushes carrying Source's CONTENTS_LADDER, as Momentum does. 0 bypasses the ladder mover entirely and restores the pre-Patch-260 behaviour exactly. Note that roughly 10% of library ladder brushes belong to brush entities the QC forces non-solid (func_illusionary, func_simpleladder) and cannot be reached by any pmove trace at any setting -- that is a QC gap, not something this cvar controls.");
cvar_t	pm_ladderdampen		 = CVARFD("pm_ladderdampen", "0.2", CVAR_SERVERINFO, "Momentum sv_ladder_dampen. How much sideways movement is damped when you meet the rungs at a glancing angle, so you climb instead of skating along the ladder's face.");
cvar_t	pm_ladderangle		 = CVARFD("pm_ladderangle", "-0.707", CVAR_SERVERINFO, "Momentum sv_ladder_angle. Cosine of the incidence angle below which pm_ladderdampen applies; -0.707 is 135 degrees.");
cvar_t	pm_sourcebounce		 = CVARFD("pm_sourcebounce", "0", CVAR_SERVERINFO, "sv_bounce equivalent: extra overbounce when an airborne player clips a wall, scaled by (1 - surfaceFriction).");
cvar_t	pm_maxvelocity		 = CVARFD("pm_maxvelocity", "0", CVAR_SERVERINFO, "DEPRECATED override for the per-axis velocity clamp. 0 (the default) means follow sv_maxvelocity, which is the only such cvar Momentum has. Set non-zero only to make the player's clamp differ from every other entity's.");
cvar_t	pm_standheight		 = CVARFD("pm_standheight", "62", CVAR_SERVERINFO, "Standing hull height. Momentum's g_ViewVectorsMom is 62 (mom_gamerules.cpp:36) - NOT CS:S's 72, which is what FTESurf shipped up to build 40. Note the eye at pm_viewheight 64 is deliberately ABOVE the crown.");
cvar_t	pm_duckheight		 = CVARFD("pm_duckheight", "45", CVAR_SERVERINFO, "Ducked hull height. Momentum is 45 (mom_gamerules.cpp:39). The hull shrinks by 17, but the mid-air origin only moves by pm_viewscale times that - see pm_viewscale.");
cvar_t	pm_duckspeed		 = CVARFD("pm_duckspeed", "0.34", CVAR_SERVERINFO, "Move-value scale while ducked and grounded. CS:S uses 0.34, giving 85 u/s from a 250 u/s run.");
cvar_t	pm_viewheight		 = CVARFD("pm_viewheight", "64", CVAR_SERVERINFO, "Eye height above the feet while standing (Source VEC_VIEW). The eye slides between this and pm_duckviewheight over the crouch, on a SimpleSpline.");
cvar_t	pm_duckviewheight	 = CVARFD("pm_duckviewheight", "47", CVAR_SERVERINFO, "Eye height above the feet while ducked (Source VEC_DUCK_VIEW). Momentum is 47 (mom_gamerules.cpp:40).");
cvar_t	pm_viewscale		 = CVARFD("pm_viewscale", "0.5", CVAR_SERVERINFO, "Momentum's IGameMode::GetViewScale(), 0.5 for every CS-based mode. Ducking in MID-AIR moves the origin by this fraction of (pm_standheight - pm_duckheight), so the hull shrinks from the top AND the bottom - 8.5 units each at 62/45 - rather than lifting the feet by the whole 17. 1 is the pre-build-41 behaviour, which lifted the feet by the full difference and left the head where it was. 0 is treated as unset and falls back to 0.5.");
cvar_t	pm_noclipspeed		 = CVARFD("pm_noclipspeed", "4", CVAR_SERVERINFO, "Noclip flies at this multiple of sv_maxspeed, doubled while +speed is held (which needs the client's in_speedbutton set). Direction comes from the move keys but speed does not, so cl_movespeedkey cannot slow it down.");
cvar_t	pm_stamina			 = CVARFD("pm_stamina", "0", CVAR_SERVERINFO, "Enable CS:S's stamina penalty: jumping and landing throttle maxspeed and jump height for about a second afterwards. This is what makes a landing bleed speed instead of holding it. Air acceleration is NOT affected, so surfing is unchanged.");
cvar_t	pm_staminajumpcost	 = CVARFD("pm_staminajumpcost", "25", CVAR_SERVERINFO, "Percentage penalty applied on jump (CS:S STAMINA_COST_JUMP). Recovers linearly over cost/pm_staminarecovery seconds. Set to 0 to keep the landing penalty but let repeated jumps reach full height.");
cvar_t	pm_staminalandcost	 = CVARFD("pm_staminalandcost", "20", CVAR_SERVERINFO, "Percentage penalty applied on landing (CS:S STAMINA_COST_FALL).");
cvar_t	pm_staminarecovery	 = CVARFD("pm_staminarecovery", "19", CVAR_SERVERINFO, "CS:S STAMINA_RECOVERY_RATE. A cost of N recovers over N/this seconds, so the default 25 and 19 give a 1.32s jump penalty.");
cvar_t	pm_normalizejump	 = CVARFD("pm_normalizejump", "1", CVAR_SERVERINFO, "Momentum Mod's mom_mv_normalize_jump_height. Every jump reaches exactly pm_jumpvelocity^2/(2*sv_gravity), whatever your duck state and whatever pm_ticrate is. 0 is stock Source, where a standing jump reaches 43.0 units and a crouch-jump 45.0 - the two units you cannot make.");
cvar_t	pm_jumpaddrise		 = CVARFD("pm_jumpaddrise", "0", CVAR_SERVERINFO, "FTESurf Patch 241. Whether a jump ADDS its impulse to upward speed you were already carrying. 0 (default) discards that rise first, so every jump reaches exactly its own height however you arrived - a trigger_teleport or a save-loc restore can hand you up to pm_groundtracedist worth of a floor while still rising, and CategorizePosition calls that grounded up to 140 u/s. 1 is the build-38 behaviour, where the two combine into a jump up to 2.3x as high. Only consulted when pm_normalizejump is set; stock Source's own duck-state ASSIGN capped this case and Patch 168 removed that branch.");
cvar_t	pm_jumpzoffset		 = CVARFD("pm_jumpzoffset", "0", CVAR_SERVERINFO, "Momentum Mod's sv_jump_z_offset, 1.5 there. PINS the takeoff height to this many units above the surface: the origin is traced down to the ground and then back up by this amount. It is not a bonus - it removes the up-to-pm_groundtracedist of jump-height variance you get from counting as grounded while not touching anything. Only used when pm_normalizejump is set; 0 disables it and leaves the variance in.");
cvar_t	pm_groundtracedist	 = CVARFD("pm_groundtracedist", "2", CVAR_SERVERINFO, "Momentum Mod's mom_mv_considered_on_ground: how far below you the ground trace looks, in units. 2 is stock Source. This is also the width of the band in which you count as standing on a surface without touching it, so it is how much your jump height can vary before pm_jumpzoffset pins it.");
cvar_t	pm_bumpcount		 = CVARFD("pm_bumpcount", "8", CVAR_SERVERINFO, "Momentum Mod's sv_ramp_bumpcount: how many collide-and-slide iterations one movement tick may spend. Stock Source ships 4; Momentum ships 8 because its ramp recovery spends bumps re-tracing. Clamped to 4..16.");
cvar_t	pm_snaptoground		 = CVARFD("pm_snaptoground", "1", CVAR_SERVERINFO, "Momentum Mod's mom_mv_snap_to_ground: move the player to exactly ground level when considered standing on the ground. Off, you take a series of tiny hops going downhill instead of staying glued to it.");
cvar_t	pm_groundquadrants	 = CVARFD("pm_groundquadrants", "1", CVAR_SERVERINFO, "Momentum Mod's mom_mv_check_ground_quadrants: when the ground trace finds nothing standable, retest with four sub-boxes in case a shallower slope is under one corner of the hull. This is Source's own behaviour; the switch exists so it can be taken out of the picture while bisecting something else.");
cvar_t	pm_fixslopes		 = CVARFD("pm_fixslopes", "1", CVAR_SERVERINFO, "Momentum Mod's sv_slope_fix, which is both of its mom_mv_fix_uphill_slopes and _downhill_slopes. When you would land on a slope, look one tick ahead: adopt the collision only if it GAINS horizontal speed (downhill, where it converts your fall into forward motion), and do not land at all if it would still be throwing you upward at over 140 u/s (uphill, where stock Source robs you of speed you already had). 0 is stock Source - land on the first standable plane, whatever it does to you.");
cvar_t	pm_fixedges			 = CVARFD("pm_fixedges", "1", CVAR_SERVERINFO, "Momentum Mod's sv_edge_fix. Before landing, simulate the next tick: fall, clip, slide, then look for ground under where that ended up. If there would be none, you were about to edgebug - land and immediately drop off the lip - so stay airborne instead. Removes the RNG in both directions: an intentional edgebug becomes reliable, and an accidental one stops happening. Costs up to three extra traces, but only on the frame a landing is considered.");
cvar_t	pm_fixrampbugs		 = CVARFD("pm_fixrampbugs", "1", CVAR_SERVERINFO, "Momentum Mod's sv_ramp_fix. When a movement trace comes back useless - it started inside the ramp, or it swept cleanly to a spot that is itself solid - stock Source stops the player dead. On a surf ramp that is not an error condition, it is the normal case, and the dead stop on a seam is the result. This recovers instead: find a plane to push away from (this trace's, then the planes already clipped this tick, then a 27-direction search with a grown hull), nudge out along it by pm_rampretrace, and re-trace. Also fixes the vertical rampbug, where a surfer sinks into the face and the loop mistakes one plane for a crease between two.");
cvar_t	pm_rampretrace		 = CVARFD("pm_rampretrace", "0.2", CVAR_SERVERINFO, "Momentum Mod's sv_ramp_initial_retrace_length. How far pm_fixrampbugs nudges the player out along a recovered plane before re-tracing, and the step by which its 27-direction search grows on each failed attempt. Clamped to 0.2 if unset and to 4 at the top - it is a distance the player is teleported by, once per bump, so an unbounded value is a way through a wall rather than a setting.");
cvar_t	pm_walkspeed		 = CVARFD("pm_walkspeed", "0", CVAR_SERVERINFO, "CS:S's CS_PLAYER_SPEED_WALK_MODIFIER: while the speed button is held, maxspeed is scaled by this. 0.52 is CS:S. 0 disables it, which is what every non-Source game on this engine wants - they use Quake's clientside cl_movespeedkey instead. Needs the client's in_speedbutton set, or the button never reaches the server.");
cvar_t	pm_lockmovement		 = CVARFD("pm_lockmovement", "0", CVAR_NOTFROMSERVER, "Lock the movement ruleset. Every cvar that feeds the mover is restored to this game's default at each map load, and refused thereafter unless sv_cheats is 1. The engine default is 0, so this does nothing until a game asks for it in its default.cfg.");

#define cvargroup_serverphysics  "server physics variables"
void WPhys_Init(void)
{
	Cvar_Register (&sv_maxvelocity,						cvargroup_serverphysics);
	Cvar_Register (&sv_gravity,							cvargroup_serverphysics);
	Cvar_Register (&sv_stopspeed,						cvargroup_serverphysics);
	Cvar_Register (&sv_maxspeed,						cvargroup_serverphysics);
	Cvar_Register (&sv_spectatormaxspeed,				cvargroup_serverphysics);
	Cvar_Register (&sv_accelerate,						cvargroup_serverphysics);
	Cvar_Register (&sv_airaccelerate,					cvargroup_serverphysics);
	Cvar_Register (&sv_wateraccelerate,					cvargroup_serverphysics);
	Cvar_Register (&sv_friction,						cvargroup_serverphysics);
	Cvar_Register (&sv_waterfriction,					cvargroup_serverphysics);
	Cvar_Register (&sv_wallfriction,					cvargroup_serverphysics);
	Cvar_Register (&sv_sound_watersplash,				cvargroup_serverphysics);
	Cvar_Register (&sv_sound_land,						cvargroup_serverphysics);
	Cvar_Register (&sv_stepheight,						cvargroup_serverphysics);

	Cvar_Register (&sv_gameplayfix_noairborncorpse,		cvargroup_serverphysics);
	Cvar_Register (&sv_gameplayfix_multiplethinks,		cvargroup_serverphysics);
	Cvar_Register (&sv_gameplayfix_stepdown,			cvargroup_serverphysics);
	Cvar_Register (&sv_gameplayfix_bouncedownslopes,	cvargroup_serverphysics);
	Cvar_Register (&sv_gameplayfix_trappedwithin,		cvargroup_serverphysics);
	Cvar_Register (&dpcompat_noretouchground,			cvargroup_serverphysics);

#if !defined(CLIENTONLY) && defined(NQPROT) && defined(HAVE_LEGACY)
	Cvar_Register (&sv_gameplayfix_spawnbeforethinks,	cvargroup_serverphysics);
#endif
}

#define	MOVE_EPSILON	0.01

static void WPhys_Physics_Toss (world_t *w, wedict_t *ent);

/*
================
SV_CheckAllEnts
================

static void SV_CheckAllEnts (void)
{
	int			e;
	edict_t		*check;

// see if any solid entities are inside the final position
	for (e=1 ; e<sv.world.num_edicts ; e++)
	{
		check = EDICT_NUM(svprogfuncs, e);
		if (check->isfree)
			continue;
		if (check->v->movetype == MOVETYPE_PUSH
		|| check->v->movetype == MOVETYPE_NONE
		|| check->v->movetype == MOVETYPE_FOLLOW
		|| check->v->movetype == MOVETYPE_NOCLIP
		|| check->v->movetype == MOVETYPE_ANGLENOCLIP)
			continue;

		if (World_TestEntityPosition (&sv.world, (wedict_t*)check))
			Con_Printf ("entity in invalid position\n");
	}
}
*/

/*
================
SV_CheckVelocity
================
*/
void WPhys_CheckVelocity (world_t *w, wedict_t *ent)
{
	int		i;
#ifdef HAVE_SERVER
	//FTESurf: Source clamps each axis independently (CGameMovement::CheckVelocity,
	//gamemovement.cpp:3060), so diagonal speed may exceed the cap.  FTE's default
	//is the RADIAL clamp below, which scales the whole vector -- and because
	//SV_RunCmd calls this on the player every usercmd (sv_user.c:7812) BEFORE the
	//velocity reaches the mover, gaining downward speed while at the cap silently
	//deleted horizontal speed: at 3500 and falling 2000, 628 u/s of it, 66 times a
	//second.  The client has no equivalent call, so it was also a permanent
	//prediction miss.  pm_source.c's own per-axis clamp was already correct; this
	//one ran first and undid it.
	//
	//Gated on the mode, not changed outright: quakers/nettest share this tree and
	//run pm_physicsmode 0, where the radial clamp is the long-standing behaviour.
	if (sv_nqplayerphysics.ival || movevars.physicsmode == PHYSMODE_SOURCE)
	{	//bound axially (like vanilla, and like Source)
		for (i=0 ; i<3 ; i++)
		{
			if (IS_NAN(ent->v->velocity[i]))
			{
				Con_DPrintf ("Got a NaN velocity on entity %i (%s)\n", ent->entnum, PR_GetString(w->progs, ent->v->classname));
				ent->v->velocity[i] = 0;
			}
			if (IS_NAN(ent->v->origin[i]))
			{
				Con_Printf ("Got a NaN origin on entity %i (%s)\n", ent->entnum, PR_GetString(w->progs, ent->v->classname));
				ent->v->origin[i] = 0;
			}

			if (ent->v->velocity[i] > sv_maxvelocity.value)
				ent->v->velocity[i] = sv_maxvelocity.value;
			else if (ent->v->velocity[i] < -sv_maxvelocity.value)
				ent->v->velocity[i] = -sv_maxvelocity.value;
		}
	}
	else
#endif
	{	//bound radially (for sanity)
		for (i=0 ; i<3 ; i++)
		{
			if (IS_NAN(ent->v->velocity[i]))
			{
				Con_DPrintf ("Got a NaN velocity on entity %i (%s)\n", ent->entnum, PR_GetString(w->progs, ent->v->classname));
				ent->v->velocity[i] = 0;
			}
			if (IS_NAN(ent->v->origin[i]))
			{
				Con_Printf ("Got a NaN origin on entity %i (%s)\n", ent->entnum, PR_GetString(w->progs, ent->v->classname));
				ent->v->origin[i] = 0;
			}
		}

		if (Length(ent->v->velocity) > sv_maxvelocity.value)
		{
//			Con_DPrintf("Slowing %s\n", PR_GetString(w->progs, ent->v->classname));
			VectorScale (ent->v->velocity, sv_maxvelocity.value/Length(ent->v->velocity), ent->v->velocity);
		}
	}
}

/*
=============
SV_RunThink

Runs thinking code if time.  There is some play in the exact time the think
function will be called, because it is called before any movement is done
in a frame.  Not used for pushmove objects, because they must be exact.
Returns false if the entity removed itself.
=============
*/
qboolean WPhys_RunThink (world_t *w, wedict_t *ent)
{
	float	thinktime;

	if (!sv_gameplayfix_multiplethinks.ival)	//try and imitate nq as closeley as possible
	{
		thinktime = ent->v->nextthink;
		if (thinktime <= 0 || thinktime > w->physicstime + host_frametime)
			return true;

		if (thinktime < w->physicstime)
			thinktime = w->physicstime;	// don't let things stay in the past.
									// it is possible to start that way
									// by a trigger with a local time.
		ent->v->nextthink = 0;
		*w->g.time = thinktime;
		w->Event_Think(w, ent);
		return !ED_ISFREE(ent);
	}

	do
	{
		thinktime = ent->v->nextthink;
		if (thinktime <= 0)
			return true;
		if (thinktime > w->physicstime + host_frametime)
			return true;

		if (thinktime < w->physicstime)
			thinktime = w->physicstime;	// don't let things stay in the past.
									// it is possible to start that way
									// by a trigger with a local time.
		ent->v->nextthink = 0;

		*w->g.time = thinktime;
		w->Event_Think(w, ent);

		if (ED_ISFREE(ent))
			return false;

		if (ent->v->nextthink <= thinktime)	//hmm... infinate loop was possible here.. Quite a few non-QW mods do this.
			return true;
	} while (1);

	return true;
}

/*
==================
SV_Impact

Two entities have touched, so run their touch functions
==================
*/
static void WPhys_Impact (world_t *w, wedict_t *e1, trace_t *trace)
{
	wedict_t *e2 = trace->ent;

	*w->g.time = w->physicstime;
	if (e1->v->touch && e1->v->solid != SOLID_NOT)
	{
		w->Event_Touch(w, e1, e2, trace);
	}

	if (e2->v->touch && e2->v->solid != SOLID_NOT)
	{
		w->Event_Touch(w, e2, e1, trace);
	}
}


/*
==================
ClipVelocity

Slide off of the impacting object
==================
*/
#define	STOP_EPSILON	0.1
//courtesy of darkplaces, it's just more efficient.
static void ClipVelocity (vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
	int i;
	float backoff;

	backoff = -DotProduct (in, normal) * overbounce;
	VectorMA(in, backoff, normal, out);

	for (i = 0;i < 3;i++)
		if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
			out[i] = 0;
}

static void WPhys_PortalTransform(world_t *w, wedict_t *ent, wedict_t *portal, vec3_t org, vec3_t move)
{
	int oself = *w->g.self;
	void *pr_globals = PR_globals(w->progs, PR_CURRENT);

	*w->g.self = EDICT_TO_PROG(w->progs, portal);
	//transform origin+velocity etc
	VectorCopy(org, G_VECTOR(OFS_PARM0));
	VectorCopy(ent->v->angles, G_VECTOR(OFS_PARM1));
	VectorCopy(ent->v->velocity, w->g.v_forward);
	VectorCopy(move, w->g.v_right);
	VectorCopy(ent->xv->gravitydir, w->g.v_up);
	if (!DotProduct(w->g.v_up, w->g.v_up))
		w->g.v_up[2] = -1;

	PR_ExecuteProgram (w->progs, portal->xv->camera_transform);

	VectorCopy(G_VECTOR(OFS_RETURN), org);
	VectorCopy(w->g.v_forward, ent->v->velocity);
	VectorCopy(w->g.v_right, move);
//	VectorCopy(w->g.v_up, ent->xv->gravitydir);

	//monsters get their gravitydir set if it isn't already, to ensure that they still work (angle issues).
	if ((int)ent->v->flags & FL_MONSTER)
		if (!ent->xv->gravitydir[0] && !ent->xv->gravitydir[1] && !ent->xv->gravitydir[2])
			ent->xv->gravitydir[2] = -1;


	//transform the angles too
	VectorCopy(org, G_VECTOR(OFS_PARM0));
#ifndef CLIENTONLY
	if (w == &sv.world && ent->entnum <= svs.allocated_client_slots)
	{
		VectorCopy(ent->v->v_angle, ent->v->angles);
	}
	else
#endif
		ent->v->angles[0] *= r_meshpitch.value;
	VectorCopy(ent->v->angles, G_VECTOR(OFS_PARM1));
	AngleVectors(ent->v->angles, w->g.v_forward, w->g.v_right, w->g.v_up);
	PR_ExecuteProgram (w->progs, portal->xv->camera_transform);
	VectorAngles(w->g.v_forward, w->g.v_up, ent->v->angles, true);
#ifndef CLIENTONLY
	if (ent->entnum > 0 && ent->entnum <= svs.allocated_client_slots)
	{
		client_t *cl = &svs.clients[ent->entnum-1];
		ent->v->angles[0] *= r_meshpitch.value;
		VectorCopy(ent->v->angles, ent->v->v_angle);
		ent->v->angles[0] *= r_meshpitch.value;
		SV_SendFixAngle(cl, NULL, FIXANGLE_AUTO, true);
	}
#endif

	/*
	avelocity is horribly dependant upon eular angles. trying to treat it as a matrix is folly.
	if (DotProduct(ent->v->avelocity, ent->v->avelocity))
	{
		ent->v->avelocity[0] *= r_meshpitch.value;
		AngleVectors(ent->v->avelocity, w->g.v_forward, w->g.v_right, w->g.v_up);
		PR_ExecuteProgram (w->progs, portal->xv->camera_transform);
		VectorAngles(w->g.v_forward, w->g.v_up, ent->v->avelocity);
		ent->v->avelocity[0] *= r_meshpitch.value;
	}
	*/

	*w->g.self = oself;
}



/*
============
SV_FlyMove

The basic solid body movement clip that slides along multiple planes
Returns the clipflags if the velocity was modified (hit something solid)
1 = floor
2 = wall / step
4 = dead stop
If steptrace is not NULL, the trace of any vertical wall hit will be stored
============
*/
#define	MAX_CLIP_PLANES	5
static int WPhys_FlyMove (world_t *w, wedict_t *ent, const vec3_t gravitydir, float time, trace_t *steptrace)
{
	int			bumpcount, numbumps;
	vec3_t		dir;
	float		d;
	int			numplanes;
	vec3_t		planes[MAX_CLIP_PLANES];
	vec3_t		primal_velocity, original_velocity, new_velocity;
	int			i, j;
	trace_t		trace;
	vec3_t		end;
	float		time_left;
	int			blocked;
	wedict_t	*impact;
	vec3_t diff;

	numbumps = 4;

	blocked = 0;
	VectorCopy (ent->v->velocity, original_velocity);
	VectorCopy (ent->v->velocity, primal_velocity);
	numplanes = 0;

	time_left = time;

	for (bumpcount=0 ; bumpcount<numbumps ; bumpcount++)
	{
		for (i=0 ; i<3 ; i++)
			end[i] = ent->v->origin[i] + time_left * ent->v->velocity[i];

		trace = World_Move (w, ent->v->origin, ent->v->mins, ent->v->maxs, end, MOVE_NORMAL, (wedict_t*)ent);

		impact = trace.ent;
		if (impact && impact->v->solid == SOLID_PORTAL)
		{
			vec3_t move;
			vec3_t from;

			VectorCopy(trace.endpos, from);	//just in case
			VectorSubtract(end, trace.endpos, move);
			WPhys_PortalTransform(w, ent, impact, from, move);
			VectorAdd(from, move, end);

			//if we follow the portal, then we basically need to restart from the other side.
			time_left -= time_left * trace.fraction;
			VectorCopy (ent->v->velocity, primal_velocity);
			VectorCopy (ent->v->velocity, original_velocity);
			numplanes = 0;

			trace = World_Move (w, from, ent->v->mins, ent->v->maxs, end, MOVE_NORMAL, (wedict_t*)ent);
			impact = trace.ent;
		}

		if (trace.allsolid)//should be (trace.startsolid), but that breaks compat. *sigh*
		{	// entity is trapped in another solid
			VectorClear (ent->v->velocity);
			return 3;
		}

		if (trace.fraction > 0)
		{	// actually covered some distance
			VectorCopy (trace.endpos, ent->v->origin);
			VectorCopy (ent->v->velocity, original_velocity);
			numplanes = 0;
		}

		if (trace.fraction == 1)
			 break;		// moved the entire distance

		if (!trace.ent)
			Host_Error ("SV_FlyMove: !trace.ent");

		if (dpcompat_noretouchground.ival)
		{	//note: also sets onground AFTER the touch event.
			if (!((int)ent->v->flags&FL_ONGROUND) || ent->v->groundentity!=EDICT_TO_PROG(w->progs, trace.ent))
				WPhys_Impact (w, ent, &trace);
		}

		if (-DotProduct(gravitydir, trace.plane.normal) > 0.7)
		{
			blocked |= 1;		// floor
			if (((wedict_t *)trace.ent)->v->solid == SOLID_BSP || dpcompat_noretouchground.ival)
			{
				ent->v->flags =	(int)ent->v->flags | FL_ONGROUND;
				ent->v->groundentity = EDICT_TO_PROG(w->progs, trace.ent);
			}
		}
		if (!DotProduct(gravitydir, trace.plane.normal))
		{
			blocked |= 2;		// step
			if (steptrace)
				*steptrace = trace;	// save for player extrafriction
		}

//
// run the impact function
//
		if (!dpcompat_noretouchground.ival)
			WPhys_Impact (w, ent, &trace);
		if (ED_ISFREE(ent))
			break;		// removed by the impact function


		time_left -= time_left * trace.fraction;

	// cliped to another plane
		if (numplanes >= MAX_CLIP_PLANES)
		{	// this shouldn't really happen
			VectorClear (ent->v->velocity);
			if (steptrace)
				*steptrace = trace;	// save for player extrafriction
			return 3;
		}

		if (0)
		{
			ClipVelocity(ent->v->velocity, trace.plane.normal, ent->v->velocity, 1);
			break;
		}
		else
		{
			if (numplanes)
			{
				VectorSubtract(planes[0], trace.plane.normal, diff);
				if (Length(diff) < 0.01)
					continue;	//hit this plane already
			}

			VectorCopy (trace.plane.normal, planes[numplanes]);
			numplanes++;

	//
	// modify original_velocity so it parallels all of the clip planes
	//
			for (i=0 ; i<numplanes ; i++)
			{
				ClipVelocity (original_velocity, planes[i], new_velocity, 1);
				for (j=0 ; j<numplanes ; j++)
					if (j != i)
					{
						if (DotProduct (new_velocity, planes[j]) < 0)
							break;	// not ok
					}
				if (j == numplanes)
					break;
			}

			if (i != numplanes)
			{	// go along this plane
//				Con_Printf ("%5.1f %5.1f %5.1f   ",ent->v->velocity[0], ent->v->velocity[1], ent->v->velocity[2]);
				VectorCopy (new_velocity, ent->v->velocity);
//				Con_Printf ("%5.1f %5.1f %5.1f\n",ent->v->velocity[0], ent->v->velocity[1], ent->v->velocity[2]);
			}
			else
			{	// go along the crease
				if (numplanes != 2)
				{
//					Con_Printf ("clip velocity, numplanes == %i\n",numplanes);
//					Con_Printf ("%5.1f %5.1f %5.1f   ",ent->v->velocity[0], ent->v->velocity[1], ent->v->velocity[2]);
					VectorClear (ent->v->velocity);
//					Con_Printf ("%5.1f %5.1f %5.1f\n",ent->v->velocity[0], ent->v->velocity[1], ent->v->velocity[2]);
					return 7;
				}
//				Con_Printf ("%5.1f %5.1f %5.1f   ",ent->v->velocity[0], ent->v->velocity[1], ent->v->velocity[2]);
				CrossProduct (planes[0], planes[1], dir);
				VectorNormalize(dir);	//fixes slow falling in corners
				d = DotProduct (dir, ent->v->velocity);
				VectorScale (dir, d, ent->v->velocity);
//				Con_Printf ("%5.1f %5.1f %5.1f\n",ent->v->velocity[0], ent->v->velocity[1], ent->v->velocity[2]);
			}
		}

//
// if original velocity is against the original velocity, stop dead
// to avoid tiny occilations in sloping corners
//
		if (DotProduct (ent->v->velocity, primal_velocity) <= 0)
		{
			VectorClear (ent->v->velocity);
			return blocked;
		}
	}

	return blocked;
}


/*
============
SV_AddGravity

============
*/
static void WPhys_AddGravity (world_t *w, wedict_t *ent, const float *gravitydir)
{
	float scale = ent->xv->gravity;
	if (!scale)
		scale = (ent->v->movetype == MOVETYPE_BOUNCEMISSILE)?0.5:1.0;

	VectorMA(ent->v->velocity, scale * movevars.gravity * host_frametime, gravitydir, ent->v->velocity);
}

/*
===============================================================================

PUSHMOVE

===============================================================================
*/

/*
============
SV_PushEntity

Does not change the entities velocity at all
============
*/
static trace_t WPhys_PushEntity (world_t *w, wedict_t *ent, vec3_t push, unsigned int traceflags)
{
	trace_t	trace;
	vec3_t	end;
	wedict_t *impact;

	VectorAdd (ent->v->origin, push, end);

	if ((int)ent->v->flags&FLQW_LAGGEDMOVE)
		traceflags |= MOVE_LAGGED;

	if (ent->v->movetype == MOVETYPE_FLYMISSILE)
		traceflags |= MOVE_MISSILE;
	else if (ent->v->solid == SOLID_TRIGGER || ent->v->solid == SOLID_NOT)
	// only clip against bmodels
		traceflags |= MOVE_NOMONSTERS;
	else
		traceflags |= MOVE_NORMAL;

	trace = World_Move (w, ent->v->origin, ent->v->mins, ent->v->maxs, end, traceflags, (wedict_t*)ent);

	impact = trace.ent;
	if (impact && impact->v->solid == SOLID_PORTAL)
	{
		vec3_t move;
		vec3_t from;
		float firstfrac = trace.fraction;
		VectorCopy(trace.endpos, from);	//just in case
		VectorSubtract(end, trace.endpos, move);
		WPhys_PortalTransform(w, ent, impact, from, move);
		VectorAdd(from, move, end);
		trace = World_Move (w, from, ent->v->mins, ent->v->maxs, end, traceflags, (wedict_t*)ent);
		trace.fraction = firstfrac + (1-firstfrac)*trace.fraction;
	}

	/*hexen2's movetype_swim does not allow swimming entities to move out of water. this implementation is quite hacky, but matches hexen2 well enough*/
	if (ent->v->movetype == MOVETYPE_H2SWIM)
	{
		if (!(w->worldmodel->funcs.PointContents(w->worldmodel, NULL, trace.endpos) & (FTECONTENTS_WATER|FTECONTENTS_SLIME|FTECONTENTS_LAVA)))
		{
			VectorCopy(ent->v->origin, trace.endpos);
			trace.fraction = 0;
			trace.ent = w->edicts;
		}
	}
#if defined(HAVE_SERVER) && defined(HEXEN2)
	else if (ent->v->solid == SOLID_PHASEH2 && progstype == PROG_H2 && w == &sv.world && trace.fraction != 1 && trace.ent &&
		(((int)((wedict_t*)trace.ent)->v->flags & FL_MONSTER) || (int)((wedict_t*)trace.ent)->v->movetype == MOVETYPE_WALK))
	{	//hexen2's SOLID_PHASEH2 ents should pass through players+monsters, yet still trigger impacts. I would use MOVE_ENTCHAIN but that would corrupt .chain, perhaps that's okay though?

		//continue the trace on to where we wold be if there had been no impact
		trace_t trace2 = World_Move (w, trace.endpos, ent->v->mins, ent->v->maxs, end, traceflags|MOVE_NOMONSTERS|MOVE_MISSILE|MOVE_RESERVED/*Don't fuck up in the face of dp's MOVE_WORLDONLY*/, (wedict_t*)ent);

		//do the first non-world impact
	//	if (trace.ent)
	//		VectorMA(trace.endpos, sv_impactpush.value, trace.plane.normal, ent->v->origin);
	//	else
			VectorCopy (trace.endpos, ent->v->origin);
		World_LinkEdict (w, ent, true);

		if (trace.ent)
		{
			WPhys_Impact (w, ent, &trace);
			if (ent->ereftype != ER_ENTITY)
				return trace;	//someone remove()d it. don't do weird stuff.
		}

		//and use our regular impact logic for the rest of it.
		trace = trace2;
	}
#endif

//	if (trace.ent)
//		VectorMA(trace.endpos, sv_impactpush.value, trace.plane.normal, ent->v->origin);
//	else
		VectorCopy (trace.endpos, ent->v->origin);
	World_LinkEdict (w, ent, true);

	if (trace.ent)
		WPhys_Impact (w, ent, &trace);

	return trace;
}




typedef struct
{
	wedict_t	*ent;
	vec3_t	origin;
	vec3_t	angles;
} pushed_t;
static pushed_t	pushed[1024], *pushed_p;

/*
============
SV_Push

Objects need to be moved back on a failed push,
otherwise riders would continue to slide.
============
*/
static qboolean WPhys_PushAngles (world_t *w, wedict_t *pusher, vec3_t move, vec3_t amove)
{
	int			i, e;
	wedict_t	*check, *block;
	vec3_t		mins, maxs;
	//float oldsolid;
	pushed_t	*p;
	vec3_t		org, org2, move2, forward, right, up;
#ifdef HAVE_SERVER
	short yawchange = (amove[PITCH]||amove[ROLL])?0:ANGLE2SHORT(amove[YAW]);
#endif

	pushed_p = pushed;

	// find the bounding box
	for (i=0 ; i<3 ; i++)
	{
		mins[i] = pusher->v->absmin[i] + move[i];
		maxs[i] = pusher->v->absmax[i] + move[i];
	}

// we need this for pushing things later
	VectorNegate (amove, org);
	AngleVectors (org, forward, right, up);

// save the pusher's original position
	pushed_p->ent = pusher;
	VectorCopy (pusher->v->origin, pushed_p->origin);
	VectorCopy (pusher->v->angles, pushed_p->angles);
	pushed_p++;

// move the pusher to it's final position
	VectorAdd (pusher->v->origin, move, pusher->v->origin);
	VectorAdd (pusher->v->angles, amove, pusher->v->angles);
	World_LinkEdict (w, pusher, false);

// see if any solid entities are inside the final position
	if (pusher->v->movetype != MOVETYPE_H2PUSHPULL)
	for (e = 1; e < w->num_edicts; e++)
	{
		check = WEDICT_NUM_PB(w->progs, e);
		if (ED_ISFREE(check))
			continue;

		if (check->v->movetype == MOVETYPE_PUSH
		|| check->v->movetype == MOVETYPE_NONE
		|| check->v->movetype == MOVETYPE_NOCLIP
		|| check->v->movetype == MOVETYPE_ANGLENOCLIP)
			continue;
/*
		oldsolid = pusher->v->solid;
		pusher->v->solid = SOLID_NOT;
		block = World_TestEntityPosition (w, check);
		pusher->v->solid = oldsolid;
		if (block)
			continue;
*/
	// if the entity is standing on the pusher, it will definitely be moved
		if ( ! ( ((int)check->v->flags & FL_ONGROUND)
			&& PROG_TO_WEDICT(w->progs, check->v->groundentity) == pusher) )
		{
			// see if the ent needs to be tested
			if ( check->v->absmin[0] >= maxs[0]
			|| check->v->absmin[1] >= maxs[1]
			|| check->v->absmin[2] >= maxs[2]
			|| check->v->absmax[0] <= mins[0]
			|| check->v->absmax[1] <= mins[1]
			|| check->v->absmax[2] <= mins[2] )
				continue;


			// see if the ent's bbox is inside the pusher's final position
			if (!World_TestEntityPosition (w, (wedict_t*)check))
				continue;
		}

		if ((pusher->v->movetype == MOVETYPE_PUSH) || (PROG_TO_WEDICT(w->progs, check->v->groundentity) == pusher))
		{
			if (pushed_p == (pushed+(sizeof(pushed)/sizeof(pushed[0]))))
				continue;
			// move this entity
			pushed_p->ent = check;
			VectorCopy (check->v->origin, pushed_p->origin);
			VectorCopy (check->v->angles, pushed_p->angles);
			pushed_p++;

			// try moving the contacted entity
			VectorAdd (check->v->origin, move, check->v->origin);
			VectorAdd (check->v->angles, amove, check->v->angles);
#ifdef HAVE_SERVER
			if (w == &sv.world && check->entnum>0&&(check->entnum)<=sv.allocated_client_slots)
				svs.clients[check->entnum-1].baseangles[YAW] += yawchange;
#endif

			// figure movement due to the pusher's amove
			VectorSubtract (check->v->origin, pusher->v->origin, org);
			org2[0] = DotProduct (org, forward);
			org2[1] = -DotProduct (org, right);
			org2[2] = DotProduct (org, up);
			VectorSubtract (org2, org, move2);
			VectorAdd (check->v->origin, move2, check->v->origin);

			if (check->v->movetype != MOVETYPE_WALK)
				check->v->flags = (int)check->v->flags & ~FL_ONGROUND;

			// may have pushed them off an edge
			if (PROG_TO_WEDICT(w->progs, check->v->groundentity) != pusher)
				check->v->groundentity = 0;

			block = World_TestEntityPosition (w, check);
			if (!block)
			{	// pushed ok
				World_LinkEdict (w, check, false);
				// impact?
				continue;
			}



			// if it is ok to leave in the old position, do it
			// this is only relevent for riding entities, not pushed
			// FIXME: this doesn't acount for rotation
			VectorCopy (pushed_p[-1].origin, check->v->origin);
			block = World_TestEntityPosition (w, check);
			if (!block)
			{
				pushed_p--;
				continue;
			}

			//okay, that didn't work, try pushing the against stuff
			WPhys_PushEntity(w, check, move, 0);
			block = World_TestEntityPosition (w, check);
			if (!block)
				continue;

			VectorCopy(check->v->origin, move);
			for (i = 0; i < 8 && block; i++)
			{
				//precision errors can strike when you least expect it. lets try and reduce them.
				check->v->origin[0] = move[0] + ((i&1)?-1:1)/8.0;
				check->v->origin[1] = move[1] + ((i&2)?-1:1)/8.0;
				check->v->origin[2] = move[2] + ((i&4)?-1:1)/8.0;
				block = World_TestEntityPosition (w, check);
			}
			if (!block)
			{
				World_LinkEdict (w, check, false);
				continue;
			}
		}

		// if it is sitting on top. Do not block.
		if (check->v->mins[0] == check->v->maxs[0])
		{
			World_LinkEdict (w, check, false);
			continue;
		}

		//some pushers are contents brushes, and are not solid. water cannot crush. the player just enters the water.
		//but, the player will be moved along with the water if possible.
		if (pusher->v->skin < 0)
			continue;

		if (check->v->solid == SOLID_NOT || check->v->solid == SOLID_TRIGGER)
		{	// corpse
			check->v->mins[0] = check->v->mins[1] = 0;
			VectorCopy (check->v->mins, check->v->maxs);
			World_LinkEdict (w, check, false);
			continue;
		}

//		Con_Printf("Pusher hit %s\n", PR_GetString(w->progs, check->v->classname));
		if (pusher->v->blocked)
		{
			*w->g.self = EDICT_TO_PROG(w->progs, pusher);
			*w->g.other = EDICT_TO_PROG(w->progs, check);
#ifdef VM_Q1
			if (w==&sv.world && svs.gametype == GT_Q1QVM)
				Q1QVM_Blocked();
			else
#endif
				PR_ExecuteProgram (w->progs, pusher->v->blocked);
		}

		// move back any entities we already moved
		// go backwards, so if the same entity was pushed
		// twice, it goes back to the original position
		for (p=pushed_p-1 ; p>=pushed ; p--)
		{
			VectorCopy (p->origin, p->ent->v->origin);
			VectorCopy (p->angles, p->ent->v->angles);
			World_LinkEdict (w, p->ent, false);

#ifdef HAVE_SERVER
			if (w==&sv.world && p->ent->entnum>0&&(p->ent->entnum)<=sv.allocated_client_slots)
				svs.clients[p->ent->entnum-1].baseangles[YAW] -= yawchange;
#endif
		}
		return false;
	}

//FIXME: is there a better way to handle this?
	// see if anything we moved has touched a trigger
	for (p=pushed_p-1 ; p>=pushed ; p--)
		World_TouchAllLinks (w, p->ent);

	return true;
}

/*
============
SV_Push

============
*/
qboolean WPhys_Push (world_t *w, wedict_t *pusher, vec3_t move, vec3_t amove)
{
#define PUSHABLE_LIMIT 8192
	int			i, e;
	wedict_t	*check, *block;
	vec3_t		mins, maxs;
	vec3_t		pushorig;
	int			num_moved;
	wedict_t	*moved_edict[PUSHABLE_LIMIT];
	vec3_t		moved_from[PUSHABLE_LIMIT];
	float oldsolid;

	if ((amove[0] || amove[1] || amove[2]) && !w->remasterlogic)
	{
		return WPhys_PushAngles(w, pusher, move, amove);
	}

	for (i=0 ; i<3 ; i++)
	{
		mins[i] = pusher->v->absmin[i] + move[i]-(1/32.0);
		maxs[i] = pusher->v->absmax[i] + move[i]+(1/32.0);
	}

	VectorCopy (pusher->v->origin, pushorig);

// move the pusher to it's final position

	VectorAdd (pusher->v->origin, move, pusher->v->origin);
	World_LinkEdict (w, pusher, false);

// see if any solid entities are inside the final position
	num_moved = 0;
	for (e=1 ; e<w->num_edicts ; e++)
	{
		check = WEDICT_NUM_PB(w->progs, e);
		if (ED_ISFREE(check))
			continue;
		if (check->v->movetype == MOVETYPE_PUSH
		|| check->v->movetype == MOVETYPE_NONE
		|| check->v->movetype == MOVETYPE_FOLLOW
		|| check->v->movetype == MOVETYPE_NOCLIP
		|| check->v->movetype == MOVETYPE_ANGLENOCLIP)
			continue;

	// if the entity is standing on the pusher, it will definately be moved
		if ( ! ( ((int)check->v->flags & FL_ONGROUND)
		&&
			PROG_TO_WEDICT(w->progs, check->v->groundentity) == pusher) )
		{
			if ( check->v->absmin[0] >= maxs[0]
			|| check->v->absmin[1] >= maxs[1]
			|| check->v->absmin[2] >= maxs[2]
			|| check->v->absmax[0] <= mins[0]
			|| check->v->absmax[1] <= mins[1]
			|| check->v->absmax[2] <= mins[2] )
				continue;

		// see if the ent's bbox is inside the pusher's final position
			if (!World_TestEntityPosition (w, check))
				continue;
		}

		oldsolid = pusher->v->solid;
		pusher->v->solid = SOLID_NOT;
		block = World_TestEntityPosition (w, check);
		pusher->v->solid = oldsolid;
		if (block)
			continue;

		if (num_moved == PUSHABLE_LIMIT)
			break;

		VectorCopy (check->v->origin, moved_from[num_moved]);
		moved_edict[num_moved] = check;
		num_moved++;

		if (check->v->groundentity != pusher->entnum)
			check->v->flags = (int)check->v->flags & ~FL_ONGROUND;

		// try moving the contacted entity
		VectorAdd (check->v->origin, move, check->v->origin);
		if (pusher->v->skin < 0)
		{
			pusher->v->solid = SOLID_NOT;
			block = World_TestEntityPosition (w, check);
			pusher->v->solid = oldsolid;
		}
		else
			block = World_TestEntityPosition (w, check);
		if (!block)
		{	// pushed ok
			World_LinkEdict (w, check, false);
			continue;
		}

		if (block)
		{
			//try to nudge it forward by an epsilon to avoid precision issues
			float movelen = VectorLength(move);
			VectorMA(check->v->origin, (1/8.0)/movelen, move, check->v->origin);
			block = World_TestEntityPosition (w, check);
			if (!block)
			{	//okay, that got it. we're all good.
				World_LinkEdict (w, check, false);
				continue;
			}
		}

		// if it is ok to leave in the old position, do it
		VectorCopy (moved_from[num_moved-1], check->v->origin);
		block = World_TestEntityPosition (w, check);
		if (!block)
		{
			//if leaving it where it was, allow it to drop to the floor again (useful for plats that move downward)
			if (check->v->movetype != MOVETYPE_WALK)
				check->v->flags = (int)check->v->flags & ~FL_ONGROUND;

			num_moved--;
			continue;
		}

	// its blocking us. this is probably a problem.

		//corpses 
		if (check->v->mins[0] == check->v->maxs[0])
		{
			World_LinkEdict (w, check, false);
			continue;
		}
		if (check->v->solid == SOLID_NOT || check->v->solid == SOLID_TRIGGER)
		{	// corpse
			check->v->mins[0] = check->v->mins[1] = 0;
			VectorCopy (check->v->mins, check->v->maxs);
			World_LinkEdict (w, check, false);
			continue;
		}

		//these pushers are contents brushes, and are not solid. water cannot crush. the player just enters the water.
		//but, the player will be moved along with the water.
		if (pusher->v->skin < 0)
			continue;

		VectorCopy (pushorig, pusher->v->origin);
		World_LinkEdict (w, pusher, false);

		// if the pusher has a "blocked" function, call it
		// otherwise, just stay in place until the obstacle is gone
		if (pusher->v->blocked)
		{
			*w->g.self = EDICT_TO_PROG(w->progs, pusher);
			*w->g.other = EDICT_TO_PROG(w->progs, check);
#ifdef VM_Q1
			if (w==&sv.world && svs.gametype == GT_Q1QVM)
				Q1QVM_Blocked();
			else
#endif
				PR_ExecuteProgram (w->progs, pusher->v->blocked);
		} else {
			*w->g.other = 0;
		}

	// move back any entities we already moved
		for (i=0 ; i<num_moved ; i++)
		{
			VectorCopy (moved_from[i], moved_edict[i]->v->origin);
			World_LinkEdict (w, moved_edict[i], false);
		}
		return false;
	}

	return true;
}


/*
============
SV_PushMove

============
*/
static void WPhys_PushMove (world_t *w, wedict_t *pusher, float movetime)
{
	int			i;
	vec3_t		move;
	vec3_t		amove;

	if (!pusher->v->velocity[0] && !pusher->v->velocity[1] && !pusher->v->velocity[2]
		&& !pusher->v->avelocity[0] && !pusher->v->avelocity[1] && !pusher->v->avelocity[2])
	{
		pusher->v->ltime += movetime;
		return;
	}

	for (i=0 ; i<3 ; i++)
	{
		move[i] = pusher->v->velocity[i] * movetime;
		amove[i] = pusher->v->avelocity[i] * movetime;
	}

	if (WPhys_Push (w, pusher, move, amove))
		pusher->v->ltime += movetime;
}


/*
================
SV_Physics_Pusher

================
*/
static void WPhys_Physics_Pusher (world_t *w, wedict_t *ent)
{
	float	thinktime;
	float	oldltime;
	float	movetime;
vec3_t oldorg, move;
vec3_t oldang, amove;
float	l;

	oldltime = ent->v->ltime;

	thinktime = ent->v->nextthink;
	if (thinktime < ent->v->ltime + host_frametime)
	{
		movetime = thinktime - ent->v->ltime;
		if (movetime < 0)
			movetime = 0;
	}
	else
		movetime = host_frametime;

	if (movetime)
	{
		WPhys_PushMove (w, ent, movetime);	// advances ent->v->ltime if not blocked
	}

	if (thinktime > oldltime && thinktime <= ent->v->ltime)
	{
VectorCopy (ent->v->origin, oldorg);
VectorCopy (ent->v->angles, oldang);
		ent->v->nextthink = 0;
#if 1
		*w->g.time = w->physicstime;
		w->Event_Think(w, ent);
#else
		pr_global_struct->time = sv.world.physicstime;
		pr_global_struct->self = EDICT_TO_PROG(w->progs, ent);
		pr_global_struct->other = EDICT_TO_PROG(w->progs, w->edicts);
#ifdef VM_Q1
		if (svs.gametype == GT_Q1QVM)
			Q1QVM_Think();
		else
#endif
			PR_ExecuteProgram (svprogfuncs, ent->v->think);
#endif
		if (ED_ISFREE(ent))
			return;
VectorSubtract (ent->v->origin, oldorg, move);
VectorSubtract (ent->v->angles, oldang, amove);

l = Length(move)+Length(amove);
if (l > 1.0/64)
{
//	Con_Printf ("**** snap: %f\n", Length (l));
	VectorCopy (oldorg, ent->v->origin);
	VectorCopy (oldang, ent->v->angles);
	WPhys_Push (w, ent, move, amove);
}

	}

}


/*
=============
SV_Physics_Follow

Entities that are "stuck" to another entity
=============
*/
static void WPhys_Physics_Follow (world_t *w, wedict_t *ent)
{
	vec3_t vf, vr, vu, angles, v;
	wedict_t *e;

	// regular thinking
	if (!WPhys_RunThink (w, ent))
		return;

	// LordHavoc: implemented rotation on MOVETYPE_FOLLOW objects
	e = PROG_TO_WEDICT(w->progs, ent->v->aiment);
	if (e->v->angles[0] == ent->xv->punchangle[0] && e->v->angles[1] == ent->xv->punchangle[1] && e->v->angles[2] == ent->xv->punchangle[2])
	{
		// quick case for no rotation
		VectorAdd(e->v->origin, ent->v->view_ofs, ent->v->origin);
	}
	else
	{
		angles[0] = ent->xv->punchangle[0] * r_meshpitch.value;
		angles[1] = ent->xv->punchangle[1];
		angles[2] = ent->xv->punchangle[2] * r_meshroll.value;
		AngleVectors (angles, vf, vr, vu);
		v[0] = ent->v->view_ofs[0] * vf[0] + ent->v->view_ofs[1] * vr[0] + ent->v->view_ofs[2] * vu[0];
		v[1] = ent->v->view_ofs[0] * vf[1] + ent->v->view_ofs[1] * vr[1] + ent->v->view_ofs[2] * vu[1];
		v[2] = ent->v->view_ofs[0] * vf[2] + ent->v->view_ofs[1] * vr[2] + ent->v->view_ofs[2] * vu[2];
		angles[0] = e->v->angles[0] * r_meshpitch.value;
		angles[1] = e->v->angles[1];
		angles[2] = e->v->angles[2] * r_meshroll.value;
		AngleVectors (angles, vf, vr, vu);
		ent->v->origin[0] = v[0] * vf[0] + v[1] * vf[1] + v[2] * vf[2] + e->v->origin[0];
		ent->v->origin[1] = v[0] * vr[0] + v[1] * vr[1] + v[2] * vr[2] + e->v->origin[1];
		ent->v->origin[2] = v[0] * vu[0] + v[1] * vu[1] + v[2] * vu[2] + e->v->origin[2];
	}
	VectorAdd (e->v->angles, ent->v->v_angle, ent->v->angles);
	World_LinkEdict (w, ent, true);
}

/*
=============
SV_Physics_Noclip

A moving object that doesn't obey physics
=============
*/
static void WPhys_Physics_Noclip (world_t *w, wedict_t *ent)
{
	vec3_t end;
#ifdef HAVE_SERVER
	trace_t trace;
	wedict_t *impact;
#endif

// regular thinking
	if (!WPhys_RunThink (w, ent))
		return;

	VectorMA (ent->v->angles, host_frametime, ent->v->avelocity, ent->v->angles);
	VectorMA (ent->v->origin, host_frametime, ent->v->velocity, end);

#ifdef HAVE_SERVER
	//allow spectators to no-clip through portals without bogging down sock's mods.
	if (ent->entnum > 0 && ent->entnum <= sv.allocated_client_slots && w == &sv.world)
	{
		trace = World_Move (w, ent->v->origin, ent->v->mins, ent->v->maxs, end, MOVE_NOMONSTERS, (wedict_t*)ent);
		impact = trace.ent;
		if (impact && impact->v->solid == SOLID_PORTAL)
		{
			vec3_t move;
			vec3_t from;
			VectorCopy(trace.endpos, from);	//just in case
			VectorSubtract(end, trace.endpos, move);
			WPhys_PortalTransform(w, ent, impact, from, move);
			VectorAdd(from, move, end);
		}
	}
#endif

	VectorCopy(end, ent->v->origin);

	World_LinkEdict (w, (wedict_t*)ent, false);
}

/*
==============================================================================

TOSS / BOUNCE

==============================================================================
*/

/*
=============
SV_CheckWaterTransition

=============
*/
static void WPhys_CheckWaterTransition (world_t *w, wedict_t *ent)
{
	int		cont;

	cont = World_PointContentsWorldOnly (w, ent->v->origin);

	//needs to be q1 progs compatible
	if (cont & FTECONTENTS_LAVA)
		cont = Q1CONTENTS_LAVA;
	else if (cont & FTECONTENTS_SLIME)
		cont = Q1CONTENTS_SLIME;
	else if (cont & FTECONTENTS_WATER)
		cont = Q1CONTENTS_WATER;
	else
		cont = Q1CONTENTS_EMPTY;

	if (!ent->v->watertype)
	{	// just spawned here
		ent->v->watertype = cont;
		ent->v->waterlevel = 1;
		return;
	}

	if (ent->v->watertype != cont && w->Event_ContentsTransition(w, ent, ent->v->watertype, cont))
	{
		ent->v->watertype = cont;
		ent->v->waterlevel = 1;
	}

	else if (cont <= Q1CONTENTS_WATER)
	{
		if (ent->v->watertype == Q1CONTENTS_EMPTY && *sv_sound_watersplash.string)
		{	// just crossed into water
			w->Event_Sound(NULL, ent, 0, sv_sound_watersplash.string, 255, 1, 0, 0, 0);
		}
		ent->v->watertype = cont;
		ent->v->waterlevel = 1;
	}
	else
	{
		if (ent->v->watertype != Q1CONTENTS_EMPTY && *sv_sound_watersplash.string)
		{	// just crossed into open
			w->Event_Sound(NULL, ent, 0, sv_sound_watersplash.string, 255, 1, 0, 0, 0);
		}
		ent->v->watertype = Q1CONTENTS_EMPTY;
		ent->v->waterlevel = cont;
	}
}

/*
=============
SV_Physics_Toss

Toss, bounce, and fly movement.  When onground, do nothing.
=============
*/
static void WPhys_Physics_Toss (world_t *w, wedict_t *ent)
{
	trace_t	trace;
	vec3_t	move;
	float	backoff;

	int fl;
	const float *gravitydir;
	int movetype;

	WPhys_CheckVelocity (w, ent);

// regular thinking
	if (!WPhys_RunThink (w, ent))
		return;

	if (ent->xv->gravitydir[2] || ent->xv->gravitydir[1] || ent->xv->gravitydir[0])
		gravitydir = ent->xv->gravitydir;
	else
		gravitydir = w->g.defaultgravitydir;

// if onground, return without moving
	if ( ((int)ent->v->flags & FL_ONGROUND) )
	{
		if (-DotProduct(gravitydir, ent->v->velocity) >= (1.0f/32.0f))
			ent->v->flags = (int)ent->v->flags & ~FL_ONGROUND;
		else
		{
			if (sv_gameplayfix_noairborncorpse.value)
			{
				wedict_t *onent;
				onent = PROG_TO_WEDICT(w->progs, ent->v->groundentity);
				if (!ED_ISFREE(onent))
					return;	//don't drop if our fround is still valid
			}
			else
				return;	//don't drop, even if the item we were on was removed (certain dm maps do this for q3 style stuff).
		}
	}

// add gravity
	movetype = ent->v->movetype;
	if (movetype != MOVETYPE_FLY
		&& movetype != MOVETYPE_FLY_WORLDONLY
		&& movetype != MOVETYPE_FLYMISSILE
		&& (movetype != MOVETYPE_BOUNCEMISSILE || w->remasterlogic/*gib*/)
		&& movetype != MOVETYPE_H2SWIM)
		WPhys_AddGravity (w, ent, gravitydir);

// move angles
	VectorMA (ent->v->angles, host_frametime, ent->v->avelocity, ent->v->angles);

// move origin
	VectorScale (ent->v->velocity, host_frametime, move);
	if (!DotProduct(move, move))
	{
		//rogue buzzsaws are vile and jerkily move via setorigin, and need to be relinked so that they can touch path corners.
		if (ent->v->solid && ent->v->nextthink)
			World_LinkEdict (w, ent, true);
		return;
	}

	fl = 0;
#ifndef CLIENTONLY
	/*doesn't affect csqc, as it has no lagged ents registered anywhere*/
	if (sv_antilag.ival==2)
		fl |= MOVE_LAGGED;
#endif

	trace = WPhys_PushEntity (w, ent, move, fl);

	if (trace.allsolid && sv_gameplayfix_trappedwithin.ival && ent->v->solid != SOLID_NOT && ent->v->solid != SOLID_TRIGGER)
	{
		trace.fraction = 0;	//traces that start in solid report a fraction of 0. this is to prevent things from dropping out of the world completely. at least this way they ought to still be shootable etc

#pragma warningmsg("The following line might help boost framerates a lot in rmq, not sure if they violate expected behaviour in other mods though - check that they're safe.")
		VectorNegate(gravitydir, trace.plane.normal);
	}
	if (trace.fraction == 1 || !trace.ent)
		return;
	if (ED_ISFREE(ent))
		return;

	VectorCopy(trace.endpos, move);

	movetype = ent->v->movetype;
	if (movetype == MOVETYPE_BOUNCEMISSILE && w->remasterlogic)
		movetype = MOVETYPE_BOUNCE;	//'gib'...
	if (movetype == MOVETYPE_BOUNCE)
	{
		if (ent->xv->bouncefactor)
			backoff = 1 + ent->xv->bouncefactor;
		else
			backoff = 1.5;
	}
	else if (movetype == MOVETYPE_BOUNCEMISSILE)
	{
		if (ent->xv->bouncefactor)
			backoff = 1 + ent->xv->bouncefactor;
//		else if (progstype == PROG_H2 && ent->v->solid == SOLID_PHASEH2 && ((int)((wedict_t*)trace.ent)->v->flags & (FL_MONSTER|FL_CLIENT)))
//			backoff = 0;	//don't bounce/slide, just pass straight through.
		else
			backoff = w->remasterlogic?1.5/*gib...*/:2;
	}
	else
		backoff = 1;

	if (backoff)
		ClipVelocity (ent->v->velocity, trace.plane.normal, ent->v->velocity, backoff);


// stop if on ground
	if ((-DotProduct(gravitydir, trace.plane.normal) > 0.7) && (movetype != MOVETYPE_BOUNCEMISSILE))
	{
		float bouncespeed;
		float bouncestop = ent->xv->bouncestop;
		if (!bouncestop)
			bouncestop = 60;
		else
			bouncestop *= movevars.gravity * (ent->xv->gravity?ent->xv->gravity:1);
		if (sv_gameplayfix_bouncedownslopes.ival)
			bouncespeed = DotProduct(trace.plane.normal, ent->v->velocity);
		else
			bouncespeed = -DotProduct(gravitydir, ent->v->velocity);
		if (bouncespeed < bouncestop || movetype != MOVETYPE_BOUNCE )
		{
			ent->v->flags = (int)ent->v->flags | FL_ONGROUND;
			ent->v->groundentity = EDICT_TO_PROG(w->progs, trace.ent);
			VectorClear (ent->v->velocity);
			VectorClear (ent->v->avelocity);
		}
	}

// check for in water
	WPhys_CheckWaterTransition (w, ent);
}

/*
===============================================================================

STEPPING MOVEMENT

===============================================================================
*/

/*
=============
SV_Physics_Step

Monsters freefall when they don't have a ground entity, otherwise
all movement is done with discrete steps.

This is also used for objects that have become still on the ground, but
will fall if the floor is pulled out from under them.
FIXME: is this true?
=============
*/
static void WPhys_Physics_Step (world_t *w, wedict_t *ent)
{
	qboolean	hitsound;
	qboolean	freefall;
	int fl = ent->v->flags;
	const float *gravitydir;
	vec3_t oldorg;
	VectorCopy(ent->v->origin, oldorg);

	if (ent->xv->gravitydir[2] || ent->xv->gravitydir[1] || ent->xv->gravitydir[0])
		gravitydir = ent->xv->gravitydir;
	else
		gravitydir = w->g.defaultgravitydir;

	if (-DotProduct(gravitydir, ent->v->velocity) >= (1.0 / 32.0) && (fl & FL_ONGROUND))
	{
		fl &= ~FL_ONGROUND;
		ent->v->flags = fl;
	}

// frefall if not onground
	if (fl & (FL_ONGROUND | FL_FLY))
		freefall = false;
	else
		freefall = true;
	if (fl & FL_SWIM)
		freefall = ent->v->waterlevel <= 0;
	if (freefall)
	{
		hitsound = -DotProduct(gravitydir, ent->v->velocity) < movevars.gravity*-0.1;

		WPhys_AddGravity (w, ent, gravitydir);
		WPhys_CheckVelocity (w, ent);
		WPhys_FlyMove (w, ent, gravitydir, host_frametime, NULL);
		World_LinkEdict (w, ent, true);

		if ( (int)ent->v->flags & FL_ONGROUND )	// just hit ground
		{
#if defined(HEXEN2) && defined(HAVE_SERVER)
			if (w==&sv.world && progstype == PROG_H2 && ((int)ent->v->flags & FL_MONSTER))
				;	//hexen2 monsters do not make landing sounds.
			else
#endif
			if (hitsound && *sv_sound_land.string)
			{
				w->Event_Sound(NULL, ent, 0, sv_sound_land.string, 255, 1, 0, 0, 0);
			}
		}
	}

// regular thinking
	WPhys_RunThink (w, ent);

	if (!VectorEquals(ent->v->origin, oldorg))
		WPhys_CheckWaterTransition (w, ent);
}

//============================================================================

#ifndef CLIENTONLY
void SV_ProgStartFrame (void)
{

// let the progs know that a new frame has started
	pr_global_struct->self = EDICT_TO_PROG(svprogfuncs, sv.world.edicts);
	pr_global_struct->other = EDICT_TO_PROG(svprogfuncs, sv.world.edicts);
	pr_global_struct->time = sv.world.physicstime;
#ifdef VM_Q1
	if (svs.gametype == GT_Q1QVM)
		Q1QVM_StartFrame(false);
	else
#endif
	{
		if (pr_global_ptrs->StartFrame)
			PR_ExecuteProgram (svprogfuncs, *pr_global_ptrs->StartFrame);
	}
}
#endif












/*
=============
SV_CheckStuck

This is a big hack to try and fix the rare case of getting stuck in the world
clipping hull.
=============
*/
static void WPhys_CheckStuck (world_t *w, wedict_t *ent)
{
	int		i, j;
	int		z;
	vec3_t	org;
//return;
	if (!World_TestEntityPosition (w, ent))
	{
		VectorCopy (ent->v->origin, ent->v->oldorigin);
		return;
	}

	VectorCopy (ent->v->origin, org);
	VectorCopy (ent->v->oldorigin, ent->v->origin);
	if (!World_TestEntityPosition (w, ent))
	{
		Con_DPrintf ("Unstuck.\n");
		World_LinkEdict (w, ent, true);
		return;
	}

	for (z=0 ; z < movevars.stepheight ; z++)
		for (i=-1 ; i <= 1 ; i++)
			for (j=-1 ; j <= 1 ; j++)
			{
				ent->v->origin[0] = org[0] + i;
				ent->v->origin[1] = org[1] + j;
				ent->v->origin[2] = org[2] + z;
				if (!World_TestEntityPosition (w, ent))
				{
					Con_DPrintf ("Unstuck.\n");
					World_LinkEdict (w, ent, true);
					return;
				}
			}

	VectorCopy (org, ent->v->origin);
	Con_DPrintf ("player is stuck.\n");
}

/*
=============
SV_CheckWater
=============

for players
*/
static qboolean WPhys_CheckWater (world_t *w, wedict_t *ent)
{
	vec3_t	point;
	int		cont;
	int hc;
	trace_t tr;

	//check if we're on a ladder, and if so fire a trace forwards to ensure its a valid ladder instead of a random volume
	hc = ent->xv->hitcontentsmaski;	//lame
	ent->xv->hitcontentsmaski = ~0;
	tr = World_Move(w, ent->v->origin, ent->v->mins, ent->v->maxs, ent->v->origin, 0, ent);
	ent->xv->hitcontentsmaski = hc;
	if (tr.contents & FTECONTENTS_LADDER)
	{
		vec3_t flatforward;
		flatforward[0] = cos((M_PI/180)*ent->v->angles[1]);
		flatforward[1] = sin((M_PI/180)*ent->v->angles[1]);
		flatforward[2] = 0;
		VectorMA (ent->v->origin, 24, flatforward, point);

		tr = World_Move(w, ent->v->origin, ent->v->mins, ent->v->maxs, point, 0, ent);
		if (tr.fraction < 1)
			ent->xv->pmove_flags = (int)ent->xv->pmove_flags|PMF_LADDER;
		else if ((int)ent->xv->pmove_flags & PMF_LADDER)
			ent->xv->pmove_flags -= PMF_LADDER;
	}
	else if ((int)ent->xv->pmove_flags & PMF_LADDER)
		ent->xv->pmove_flags -= PMF_LADDER;


	point[0] = ent->v->origin[0];
	point[1] = ent->v->origin[1];
	point[2] = ent->v->origin[2] + ent->v->mins[2] + 1;

	ent->v->waterlevel = 0;
	ent->v->watertype = Q1CONTENTS_EMPTY;
	cont = World_PointContentsAllBSPs (w, point);
	if (cont & FTECONTENTS_FLUID)
	{
		if (cont & FTECONTENTS_LAVA)
			ent->v->watertype = Q1CONTENTS_LAVA;
		else if (cont & FTECONTENTS_SLIME)
			ent->v->watertype = Q1CONTENTS_SLIME;
		else if (cont & FTECONTENTS_WATER)
			ent->v->watertype = Q1CONTENTS_WATER;
		else
			ent->v->watertype = Q1CONTENTS_SKY;
		ent->v->waterlevel = 1;
		point[2] = ent->v->origin[2] + (ent->v->mins[2] + ent->v->maxs[2])*0.5;
		cont = World_PointContentsAllBSPs (w, point);
		if (cont & FTECONTENTS_FLUID)
		{
			ent->v->waterlevel = 2;
			point[2] = ent->v->origin[2] + ent->v->view_ofs[2];
			cont = World_PointContentsAllBSPs (w, point);
			if (cont & FTECONTENTS_FLUID)
				ent->v->waterlevel = 3;
		}
	}

	return ent->v->waterlevel > 1;
}


/*
============
SV_WallFriction

============
*/
static void WPhys_WallFriction (wedict_t *ent, trace_t *trace)
{
	vec3_t		forward, right, up;
	float		d, i;
	vec3_t		into, side;

	AngleVectors (ent->v->v_angle, forward, right, up);
	d = DotProduct (trace->plane.normal, forward);

	d += 0.5;
	if (d >= 0 || IS_NAN(d))
		return;

// cut the tangential velocity
	i = DotProduct (trace->plane.normal, ent->v->velocity);
	VectorScale (trace->plane.normal, i, into);
	VectorSubtract (ent->v->velocity, into, side);

	ent->v->velocity[0] = side[0] * (1 + d);
	ent->v->velocity[1] = side[1] * (1 + d);
}

/*
=====================
SV_TryUnstick

Player has come to a dead stop, possibly due to the problem with limited
float precision at some angle joins in the BSP hull.

Try fixing by pushing one pixel in each direction.

This is a hack, but in the interest of good gameplay...
======================

static int SV_TryUnstick (edict_t *ent, vec3_t oldvel)
{
	int		i;
	vec3_t	oldorg;
	vec3_t	dir;
	int		clip;
	trace_t	steptrace;

	VectorCopy (ent->v->origin, oldorg);
	VectorClear (dir);

	for (i=0 ; i<8 ; i++)
	{
// try pushing a little in an axial direction
		switch (i)
		{
			case 0:	dir[0] = 2; dir[1] = 0; break;
			case 1:	dir[0] = 0; dir[1] = 2; break;
			case 2:	dir[0] = -2; dir[1] = 0; break;
			case 3:	dir[0] = 0; dir[1] = -2; break;
			case 4:	dir[0] = 2; dir[1] = 2; break;
			case 5:	dir[0] = -2; dir[1] = 2; break;
			case 6:	dir[0] = 2; dir[1] = -2; break;
			case 7:	dir[0] = -2; dir[1] = -2; break;
		}

		SV_PushEntity (ent, dir, MOVE_NORMAL);

// retry the original move
		ent->v->velocity[0] = oldvel[0];
		ent->v-> velocity[1] = oldvel[1];
		ent->v-> velocity[2] = 0;
		clip = SV_FlyMove (ent, 0.1, &steptrace);

		if ( fabs(oldorg[1] - ent->v->origin[1]) > 4
		|| fabs(oldorg[0] - ent->v->origin[0]) > 4 )
		{
//Con_DPrintf ("unstuck!\n");
			return clip;
		}

// go back to the original pos and try again
		VectorCopy (oldorg, ent->v->origin);
	}

	VectorClear (ent->v->velocity);
	return 7;		// still not moving
}
*/

/*
=====================
SV_WalkMove

Only used by players
======================
*/
#if 0
#define	SMSTEPSIZE	4
static void SV_WalkMove (edict_t *ent)
{
	vec3_t		upmove, downmove;
	vec3_t		oldorg, oldvel;
	vec3_t		nosteporg, nostepvel;
	int			clip;
	int			oldonground;
	trace_t		steptrace, downtrace;

//
// do a regular slide move unless it looks like you ran into a step
//
	oldonground = (int)ent->v->flags & FL_ONGROUND;
	ent->v->flags = (int)ent->v->flags & ~FL_ONGROUND;

	VectorCopy (ent->v->origin, oldorg);
	VectorCopy (ent->v->velocity, oldvel);

	clip = SV_FlyMove (ent, host_frametime, &steptrace);

	if ( !(clip & 2) )
		return;		// move didn't block on a step

	if (!oldonground && ent->v->waterlevel == 0)
		return;		// don't stair up while jumping

	if (ent->v->movetype != MOVETYPE_WALK)
		return;		// gibbed by a trigger

//	if (sv_nostep.value)
//		return;

	if ( (int)ent->v->flags & FL_WATERJUMP )
		return;

	VectorCopy (ent->v->origin, nosteporg);
	VectorCopy (ent->v->velocity, nostepvel);

//
// try moving up and forward to go up a step
//
	VectorCopy (oldorg, ent->v->origin);	// back to start pos

	VectorCopy (vec3_origin, upmove);
	VectorCopy (vec3_origin, downmove);
	upmove[2] = movevars.stepheight;
	downmove[2] = -movevars.stepheight + oldvel[2]*host_frametime;

// move up
	SV_PushEntity (ent, upmove);	// FIXME: don't link?

// move forward
	ent->v->velocity[0] = oldvel[0];
	ent->v->velocity[1] = oldvel[1];
	ent->v->velocity[2] = 0;
	clip = SV_FlyMove (ent, host_frametime, &steptrace);

// check for stuckness, possibly due to the limited precision of floats
// in the clipping hulls
	if (clip)
	{
		if ( fabs(oldorg[1] - ent->v->origin[1]) < 0.03125
		&& fabs(oldorg[0] - ent->v->origin[0]) < 0.03125 )
		{	// stepping up didn't make any progress
			clip = SV_TryUnstick (ent, oldvel);

//			Con_Printf("Try unstick fwd\n");
		}
	}

// extra friction based on view angle
	if ( clip & 2 )
	{
		vec3_t lastpos, lastvel, lastdown;

//		Con_Printf("couldn't do it\n");

		//retry with a smaller step (allows entering smaller areas with a step of 4)
		VectorCopy (downmove, lastdown);
		VectorCopy (ent->v->origin, lastpos);
		VectorCopy (ent->v->velocity, lastvel);

	//
	// try moving up and forward to go up a step
	//
		VectorCopy (oldorg, ent->v->origin);	// back to start pos

		VectorCopy (vec3_origin, upmove);
		VectorCopy (vec3_origin, downmove);
		upmove[2] = SMSTEPSIZE;
		downmove[2] = -SMSTEPSIZE + oldvel[2]*host_frametime;

	// move up
		SV_PushEntity (ent, upmove);	// FIXME: don't link?

	// move forward
		ent->v->velocity[0] = oldvel[0];
		ent->v->velocity[1] = oldvel[1];
		ent->v->velocity[2] = 0;
		clip = SV_FlyMove (ent, host_frametime, &steptrace);

	// check for stuckness, possibly due to the limited precision of floats
	// in the clipping hulls
		if (clip)
		{
			if ( fabs(oldorg[1] - ent->v->origin[1]) < 0.03125
			&& fabs(oldorg[0] - ent->v->origin[0]) < 0.03125 )
			{	// stepping up didn't make any progress
				clip = SV_TryUnstick (ent, oldvel);

//				Con_Printf("Try unstick up\n");
			}
		}

		if ( fabs(oldorg[1] - ent->v->origin[1])+fabs(oldorg[0] - ent->v->origin[0]) < fabs(oldorg[1] - lastpos[1])+fabs(oldorg[1] - lastpos[1]))
		{	// stepping up didn't make any progress
				//go back
				VectorCopy (lastdown, downmove);
				VectorCopy (lastpos, ent->v->origin);
				VectorCopy (lastvel, ent->v->velocity);

				SV_WallFriction (ent, &steptrace);

//				Con_Printf("wall friction\n");
			}

		else if (clip & 2)
		{
			SV_WallFriction (ent, &steptrace);
//			Con_Printf("wall friction 2\n");
		}
	}

// move down
	downtrace = SV_PushEntity (ent, downmove);	// FIXME: don't link?

	if (downtrace.plane.normal[2] > 0.7)
	{
		if (ent->v->solid == SOLID_BSP)
		{
			ent->v->flags =	(int)ent->v->flags | FL_ONGROUND;
			ent->v->groundentity = EDICT_TO_PROG(svprogfuncs, downtrace.ent);
		}
	}
	else
	{
// if the push down didn't end up on good ground, use the move without
// the step up.  This happens near wall / slope combinations, and can
// cause the player to hop up higher on a slope too steep to climb
		VectorCopy (nosteporg, ent->v->origin);
		VectorCopy (nostepvel, ent->v->velocity);

//		Con_Printf("down not good\n");
	}
}
#else

// 1/32 epsilon to keep floating point happy
/*#define	DIST_EPSILON	(0.03125)
static int WPhys_SetOnGround (world_t *w, wedict_t *ent, const float *gravitydir)
{
	vec3_t end;
	trace_t trace;
	if ((int)ent->v->flags & FL_ONGROUND)
		return 1;
	VectorMA(ent->v->origin, 1, gravitydir, end);
	trace = World_Move(w, ent->v->origin, ent->v->mins, ent->v->maxs, end, MOVE_NORMAL, (wedict_t*)ent);
	if (DotProduct(trace.plane.normal, ent->v->velocity) > 0)
		return 0;	//velocity is away from the plane normal, so this does not count as a contact.
	if (trace.fraction <= DIST_EPSILON && -DotProduct(gravitydir, trace.plane.normal) >= 0.7)
	{
		ent->v->flags = (int)ent->v->flags | FL_ONGROUND;
		ent->v->groundentity = EDICT_TO_PROG(w->progs, trace.ent);
		return 1;
	}
	return 0;
}*/
static void WPhys_WalkMove (world_t *w, wedict_t *ent, const float *gravitydir)
{
	//int originalmove_clip;
	int clip, oldonground, originalmove_flags, originalmove_groundentity;
	vec3_t upmove, downmove, start_origin, start_velocity, originalmove_origin, originalmove_velocity;
	trace_t downtrace, steptrace;

	WPhys_CheckVelocity(w, ent);

	// do a regular slide move unless it looks like you ran into a step
	oldonground = (int)ent->v->flags & FL_ONGROUND;
	ent->v->flags = (int)ent->v->flags & ~FL_ONGROUND;

	VectorCopy (ent->v->origin, start_origin);
	VectorCopy (ent->v->velocity, start_velocity);

	clip = WPhys_FlyMove (w, ent, gravitydir, host_frametime, NULL);

//	WPhys_SetOnGround (w, ent, gravitydir);
	WPhys_CheckVelocity(w, ent);

	VectorCopy(ent->v->origin, originalmove_origin);
	VectorCopy(ent->v->velocity, originalmove_velocity);
	//originalmove_clip = clip;
	originalmove_flags = (int)ent->v->flags;
	originalmove_groundentity = ent->v->groundentity;

	if ((int)ent->v->flags & FL_WATERJUMP)
		return;

//	if (sv_nostep.value)
//		return;

	// if move didn't block on a step, return
	if (clip & 2)
	{
		// if move was not trying to move into the step, return
		if (fabs(start_velocity[0]) < 0.03125 && fabs(start_velocity[1]) < 0.03125)
			return;

		if (ent->v->movetype != MOVETYPE_FLY && ent->v->movetype != MOVETYPE_FLY_WORLDONLY)
		{
			// return if gibbed by a trigger
			if (ent->v->movetype != MOVETYPE_WALK)
				return;

			// only step up while jumping if that is enabled
 			if (!pm_airstep.value)
				if (!oldonground && ent->v->waterlevel == 0)
					return;
		}

		// try moving up and forward to go up a step
		// back to start pos
		VectorCopy (start_origin, ent->v->origin);
		VectorCopy (start_velocity, ent->v->velocity);

		// move up
		VectorScale(gravitydir, -movevars.stepheight, upmove);
		// FIXME: don't link?
		WPhys_PushEntity(w, ent, upmove, MOVE_NORMAL);

		// move forward
		VectorMA(ent->v->velocity, -DotProduct(gravitydir, ent->v->velocity), gravitydir, ent->v->velocity);	//ent->v->velocity[2] = 0;
		clip = WPhys_FlyMove (w, ent, gravitydir, host_frametime, &steptrace);
		VectorMA(ent->v->velocity, DotProduct(gravitydir, start_velocity), gravitydir, ent->v->velocity);	//ent->v->velocity[2] += start_velocity[2];

		WPhys_CheckVelocity(w, ent);

		// check for stuckness, possibly due to the limited precision of floats
		// in the clipping hulls
		if (clip
		 && fabs(originalmove_origin[1] - ent->v->origin[1]) < 0.03125
		 && fabs(originalmove_origin[0] - ent->v->origin[0]) < 0.03125)
		{
//			Con_Printf("wall\n");
			// stepping up didn't make any progress, revert to original move
			VectorCopy(originalmove_origin, ent->v->origin);
			VectorCopy(originalmove_velocity, ent->v->velocity);
			//clip = originalmove_clip;
			ent->v->flags = originalmove_flags;
			ent->v->groundentity = originalmove_groundentity;
			// now try to unstick if needed
			//clip = SV_TryUnstick (ent, oldvel);
			return;
		}

		//Con_Printf("step - ");

		// extra friction based on view angle
		if ((clip & 2) && sv_wallfriction.value)
		{
//			Con_Printf("wall\n");
			WPhys_WallFriction (ent, &steptrace);
		}
	}
	else if (!sv_gameplayfix_stepdown.ival || !oldonground || -DotProduct(gravitydir,start_velocity) > 0 || ((int)ent->v->flags & FL_ONGROUND) || ent->v->waterlevel >= 2)
		return;

	// move down
	VectorScale(gravitydir, movevars.stepheight + (1/32.0) - DotProduct(gravitydir,start_velocity)*host_frametime, downmove);
	// FIXME: don't link?
	downtrace = WPhys_PushEntity (w, ent, downmove, MOVE_NORMAL);

	if (downtrace.fraction < 1 && -DotProduct(gravitydir, downtrace.plane.normal) > 0.7)
	{
		if (DotProduct(downtrace.plane.normal, ent->v->velocity)<=0) //Spike: moving away from the surface should not count as onground.
		// LordHavoc: disabled this check so you can walk on monsters/players
		//if (ent->v->solid == SOLID_BSP)
		{
			//Con_Printf("onground\n");
			ent->v->flags =	(int)ent->v->flags | FL_ONGROUND;
			ent->v->groundentity = EDICT_TO_PROG(w->progs, downtrace.ent);
		}
	}
	else
	{
		//Con_Printf("slope\n");
		// if the push down didn't end up on good ground, use the move without
		// the step up.  This happens near wall / slope combinations, and can
		// cause the player to hop up higher on a slope too steep to climb
		VectorCopy(originalmove_origin, ent->v->origin);
		VectorCopy(originalmove_velocity, ent->v->velocity);
		//clip = originalmove_clip;
		ent->v->flags = originalmove_flags;
		ent->v->groundentity = originalmove_groundentity;
	}

//	WPhys_SetOnGround (w, ent, gravitydir);
	WPhys_CheckVelocity(w, ent);
}
#endif

#ifdef HEXEN2
void WPhys_MoveChain(world_t *w, wedict_t *ent, wedict_t *movechain, float *initial_origin, float *initial_angle)
{
	qboolean orgunchanged;
	vec3_t moveorg, moveang;
	VectorSubtract(ent->v->origin, initial_origin, moveorg);
	VectorSubtract(ent->v->angles, initial_angle, moveang);
	orgunchanged=!DotProduct(moveorg,moveorg);
	if (!orgunchanged || DotProduct(moveang,moveang))
	{
		int i;
		for(i=16; i && movechain != w->edicts && !ED_ISFREE(movechain); i--, movechain = PROG_TO_WEDICT(w->progs, movechain->xv->movechain))
		{
			if ((int)movechain->v->flags & FL_MOVECHAIN_ANGLE)
				VectorAdd(movechain->v->angles, moveang, movechain->v->angles);	//FIXME: axial only
			if (!orgunchanged)
			{
				VectorAdd(movechain->v->origin, moveorg, movechain->v->origin);
				World_LinkEdict(w, movechain, false);

				//chainmoved is called only for origin changes, not angle ones, apparently.
				if (movechain->xv->chainmoved)
				{
					*w->g.self = EDICT_TO_PROG(w->progs, movechain);
					*w->g.other = EDICT_TO_PROG(w->progs, ent);
#ifdef VM_Q1
					if (svs.gametype == GT_Q1QVM && w == &sv.world)
						Q1QVM_ChainMoved();
					else
#endif
						PR_ExecuteProgram(w->progs, movechain->xv->chainmoved);
				}
			}
		}
	}
}
#endif

/*
================
SV_RunEntity

================
*/
void WPhys_RunEntity (world_t *w, wedict_t *ent)
{
#ifdef HEXEN2
	wedict_t	*movechain;
	vec3_t	initial_origin = {0},initial_angle = {0};
#endif
	const float *gravitydir;

#ifndef CLIENTONLY
	edict_t *svent = (edict_t*)ent;
	if (ent->entnum > 0 && ent->entnum <= sv.allocated_client_slots && w == &sv.world)
	{	//a client woo.
		qboolean readyforjump = false;

#if defined(NQPROT) && defined(HAVE_LEGACY)
		if (svs.clients[ent->entnum-1].state == cs_connected)
		{	//nq is buggy and calls playerprethink/etc while the player is still connecting.
			//some mods depend on this, hopefully unintentionally (as is the case with Arcane Dimensions).
			//so don't do anything if we're qw, but use crappy behaviour for nq+h2.
			if (progstype != PROG_NQ || sv_gameplayfix_spawnbeforethinks.ival)
				return;
		}
		else
#endif
		{
			if (svs.clients[ent->entnum-1].state < cs_spawned)
				return;		// unconnected slot
		}

		if (svs.clients[ent->entnum-1].protocol == SCP_BAD)
			svent->v->fixangle = FIXANGLE_NO;	//bots never get fixangle cleared otherwise

		host_client = &svs.clients[ent->entnum-1];
		SV_ClientThink();

		if (!host_client->spectator)
		{
			if (progstype == PROG_QW)	//detect if the mod should do a jump
				if (svent->v->button2)
					if ((int)svent->v->flags & FL_JUMPRELEASED)
						readyforjump = true;

			//
			// call standard client pre-think
			//
			pr_global_struct->time = sv.world.physicstime;
			pr_global_struct->self = EDICT_TO_PROG(svprogfuncs, ent);
#ifdef VM_Q1
			if (svs.gametype == GT_Q1QVM)
				Q1QVM_PlayerPreThink();
			else
#endif
				if (pr_global_ptrs->PlayerPreThink)
					PR_ExecuteProgram (svprogfuncs, *pr_global_ptrs->PlayerPreThink);

			if (readyforjump)	//qw progs can't jump for themselves...
			{
				if (!svent->v->button2 && !((int)ent->v->flags & FL_JUMPRELEASED) && ent->v->velocity[2] <= 0)
					svent->v->velocity[2] += 270;
			}
		}
	}
	else
#endif
	{
		if (ent->lastruntime == w->framenum)
			return;
		ent->lastruntime = w->framenum;
#ifndef CLIENTONLY
		if (progstype == PROG_QW && w == &sv.world)	//we don't use the field any more, but qw mods might.
			ent->v->lastruntime = w->physicstime;
		svent = NULL;
#endif
	}


#ifdef HEXEN2
	movechain = PROG_TO_WEDICT(w->progs, ent->xv->movechain);
	if (movechain != w->edicts)
	{
		VectorCopy(ent->v->origin,initial_origin);
		VectorCopy(ent->v->angles,initial_angle);
	}
#endif


	if (ent->xv->customphysics)
	{
		*w->g.time = w->physicstime;
		*w->g.self = EDICT_TO_PROG(w->progs, ent);
		PR_ExecuteProgram (w->progs, ent->xv->customphysics);
	}
	else switch ( (int)ent->v->movetype)
	{
	case MOVETYPE_PUSH:
		WPhys_Physics_Pusher (w, ent);
		break;
	case MOVETYPE_NONE:
		if (!WPhys_RunThink (w, ent))
			return;
		break;
	case MOVETYPE_NOCLIP:
	case MOVETYPE_ANGLENOCLIP:
		WPhys_Physics_Noclip (w, ent);
		break;
	case MOVETYPE_H2PUSHPULL:
#if defined(HEXEN2) && !defined(CLIENTONLY)
		if (w == &sv.world && progstype == PROG_H2)
			WPhys_Physics_Step (w, ent);	//hexen2 pushable object (basically exactly movetype_step)
		else
#endif
			WPhys_Physics_Pusher (w, ent);	//non-solid pusher, for tenebrae compat
		break;
	case MOVETYPE_STEP:
		WPhys_Physics_Step (w, ent);
		break;
	case MOVETYPE_FOLLOW:
		WPhys_Physics_Follow (w, ent);
		break;
	case MOVETYPE_FLY_WORLDONLY:
	case MOVETYPE_FLY:
#ifndef CLIENTONLY
		if (svent)
		{	//NQ players with movetype_fly are not like non-players.
			if (!WPhys_RunThink (w, ent))
				return;
			if (ent->xv->gravitydir[2] || ent->xv->gravitydir[1] || ent->xv->gravitydir[0])
				gravitydir = ent->xv->gravitydir;
			else
				gravitydir = w->g.defaultgravitydir;
			WPhys_CheckStuck (w, ent);
			WPhys_WalkMove (w, ent, gravitydir);
			break;
		}
#endif
		//fallthrough
	case MOVETYPE_H2SWIM:
	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
	case MOVETYPE_BOUNCEMISSILE:
	case MOVETYPE_FLYMISSILE:
		WPhys_Physics_Toss (w, ent);
		break;
	case MOVETYPE_WALK:
		if (!WPhys_RunThink (w, ent))
			return;

		if (ent->xv->gravitydir[2] || ent->xv->gravitydir[1] || ent->xv->gravitydir[0])
			gravitydir = ent->xv->gravitydir;
		else
			gravitydir = w->g.defaultgravitydir;

		if (!WPhys_CheckWater (w, ent) && ! ((int)ent->v->flags & FL_WATERJUMP) ) //Vanilla Bug: the QC checks waterlevel inside PlayerPreThink, with waterlevel from a different position from the origin.
			if (!((int)ent->xv->pmove_flags & PMF_LADDER))
				WPhys_AddGravity (w, ent, gravitydir);
		WPhys_CheckStuck (w, ent);

		WPhys_WalkMove (w, ent, gravitydir);

#ifndef CLIENTONLY
		if (!svent)
#endif
			World_LinkEdict (w, ent, true);
		break;
#ifdef USERBE
	case MOVETYPE_PHYSICS:
		if (WPhys_RunThink(w, ent))
			World_LinkEdict (w, ent, true);
		w->rbe_hasphysicsents = true;
		break;
#endif
	default:
//		SV_Error ("SV_Physics: bad movetype %i on %s", (int)ent->v->movetype, PR_GetString(w->progs, ent->v->classname));
		break;
	}

#ifdef HEXEN2
	if (movechain != w->edicts)
		WPhys_MoveChain(w, ent, movechain, initial_origin, initial_angle);
#endif

#ifndef CLIENTONLY
	if (svent)
	{
		World_LinkEdict (w, ent, true);

		if (!host_client->spectator)
		{
			pr_global_struct->time = w->physicstime;
			pr_global_struct->self = EDICT_TO_PROG(w->progs, ent);
#ifdef VM_Q1
			if (svs.gametype == GT_Q1QVM)
				Q1QVM_PostThink();
			else
#endif
			{
				if (pr_global_ptrs->PlayerPostThink)
					PR_ExecuteProgram (w->progs, *pr_global_ptrs->PlayerPostThink);
			}
		}
	}
#endif
}

/*
================
SV_RunNewmis

================
*/
void WPhys_RunNewmis (world_t *w)
{
	wedict_t	*ent;

	if (!w->g.newmis)	//newmis variable is not exported.
		return;

	if (!sv_gameplayfix_multiplethinks.ival)
		return;

	if (!*w->g.newmis)
		return;
	ent = PROG_TO_WEDICT(w->progs, *w->g.newmis);
	host_frametime = 0.05;
	*w->g.newmis = 0;

	WPhys_RunEntity (w, ent);

	host_frametime = *w->g.frametime;
}

trace_t WPhys_Trace_Toss (world_t *w, wedict_t *tossent, wedict_t *ignore)
{
	int i;
	float gravity;
	vec3_t move, end;
	trace_t trace;

	vec3_t origin, velocity;

	// this has to fetch the field from the original edict, since our copy is truncated
	gravity = tossent->xv->gravity;
	if (!gravity)
		gravity = 1.0;
	gravity *= sv_gravity.value * 0.05;

	VectorCopy (tossent->v->origin, origin);
	VectorCopy (tossent->v->velocity, velocity);

	WPhys_CheckVelocity (w, tossent);

	for (i = 0;i < 200;i++) // LordHavoc: sanity check; never trace more than 10 seconds
	{
		velocity[2] -= gravity;
		VectorScale (velocity, 0.05, move);
		VectorAdd (origin, move, end);
		trace = World_Move (w, origin, tossent->v->mins, tossent->v->maxs, end, MOVE_NORMAL, tossent);
		VectorCopy (trace.endpos, origin);

		if (trace.fraction < 1 && trace.ent && trace.ent != ignore)
			break;

		if (Length(velocity) > sv_maxvelocity.value)
		{
//			Con_DPrintf("Slowing %s\n", PR_GetString(w->progs, tossent->v->classname));
			VectorScale (velocity, sv_maxvelocity.value/Length(velocity), velocity);
		}
	}

	trace.fraction = 0; // not relevant
	return trace;
}

/*
Run an individual physics frame. This might be run multiple times in one frame if we're running slow, or not at all.
*/
void World_Physics_Frame(world_t *w)
{
	int i;
	qboolean retouch;
	wedict_t *ent;

	w->framenum++;

	i = *w->g.physics_mode;
	if (i == 0)
	{
		/*physics mode 0 = none*/
		return;
	}
	if (i == 1)
	{
		/*physics mode 1 = thinks only*/
		for (i=0 ; i<w->num_edicts ; i++)
		{
			ent = (wedict_t*)EDICT_NUM_PB(w->progs, i);
			if (ED_ISFREE(ent))
				continue;

			WPhys_RunThink (w, ent);
		}
		return;
	}
	/*physics mode 2 = normal movetypes*/

	retouch = (w->g.force_retouch && (*w->g.force_retouch >= 1));

	//
	// treat each object in turn
	// even the world gets a chance to think
	//
	for (i=0 ; i<w->num_edicts ; i++)
	{
		ent = (wedict_t*)EDICT_NUM_PB(w->progs, i);
		if (ED_ISFREE(ent))
			continue;

		if (retouch)
			World_LinkEdict (w, ent, true);	// force retouch even for stationary

#ifdef HAVE_SERVER
		if (i > 0 && i <= sv.allocated_client_slots && w == &sv.world)
		{
			if (!svs.clients[i-1].isindependant)
			{
				if (sv_nqplayerphysics.ival || SV_PlayerPhysicsQC || svs.clients[i-1].state < cs_spawned)
				{
					WPhys_RunEntity (w, ent);
					WPhys_RunNewmis (w);
				}
				else
				{
					unsigned int newt;
					unsigned int delt;
					newt = sv.time*1000;
					delt = newt - svs.clients[i-1].lastruncmd;
					if (delt > (int)(1000/77.0) || delt < -10)
					{
						float ft = host_frametime;
						host_client = &svs.clients[i-1];
						sv_player = svs.clients[i-1].edict;

						SV_PreRunCmd();
#ifndef NEWSPEEDCHEATPROT
						svs.clients[i-1].last_check = 0;
#endif
						svs.clients[i-1].lastcmd.msec = bound(0, delt, 255);
						SV_RunCmd (&svs.clients[i-1].lastcmd, true);
						svs.clients[i-1].lastcmd.impulse = 0;
						SV_PostRunCmd();
						host_client->lastruncmd = sv.time*1000;
						*w->g.frametime = host_frametime = ft;
					}
				}
			}
//			else
//				World_LinkEdict(w, (wedict_t*)ent, true);
			continue;		// clients are run directly from packets
		}
#endif

		WPhys_RunEntity (w, ent);
		WPhys_RunNewmis (w);
	}

	if (retouch)
		*w->g.force_retouch-=1;
}

#ifdef HAVE_SERVER
/*
================
SV_Physics

================
*/
qboolean SV_Physics (void)
{
	int		i;
	qboolean moved = false;
	int maxtics = sv_limittics.ival;
	double trueframetime = host_frametime;
	double maxtic = sv_maxtic.value;
	double mintic = sv_mintic.value;
	if (sv_nqplayerphysics.ival)
		if (mintic < 0.013)
			mintic = 0.013;	//NQ physics can't cope with low rates and just generally bugs out.
	if (maxtic < mintic)
		maxtic = mintic;

	if (maxtics>1&&sv.spawned_observer_slots==0&&sv.spawned_client_slots==0)
		maxtics = 1;	//no players on the server. let timings slide

	//keep gravity tracking the cvar properly
	movevars.gravity = sv_gravity.value;

	if (svs.gametype != GT_PROGS && svs.gametype != GT_Q1QVM && svs.gametype != GT_HALFLIFE 
#ifdef VM_LUA
		&& svs.gametype != GT_LUA
#endif
		)	//make tics multiples of sv_maxtic (defaults to 0.1)
	{
		if (svs.gametype == GT_QUAKE2)
			mintic = maxtic = 0.1;	//fucking fuckity fuck. we should warn about this.
		mintic = max(mintic, 1/1000.0);

		for(;;)
		{
			host_frametime = sv.time - sv.world.physicstime;
			if (host_frametime<0)
			{
				if (host_frametime < -1)
					sv.world.physicstime = sv.time;
				host_frametime = 0;
			}
			if (!maxtics--)
			{	//don't loop infinitely if we froze (eg debugger or suspend/hibernate)
				sv.world.physicstime = sv.time;
				break;
			}
			if (!host_frametime || (host_frametime < mintic && realtime))
				break;
			if (host_frametime > maxtic)
				host_frametime = maxtic;
			sv.world.physicstime += host_frametime;
			moved = true;

			switch(svs.gametype)
			{
#ifdef Q2SERVER
			case GT_QUAKE2:
				ge->RunFrame();
				break;
#endif
#ifdef Q3SERVER
			case GT_QUAKE3:
				q3->sv.RunFrame();
				break;
#endif
			default:
				break;
			}
		}
		host_frametime = trueframetime;
		return moved;
	}

	if (svs.gametype != GT_HALFLIFE && /*sv.botsonthemap &&*/ progstype == PROG_QW)
	{
		//DP_SV_BOTCLIENT - make the bots move with qw physics.
		//They only move when there arn't any players on the server, but they should move at the right kind of speed if there are... hopefully
		//they might just be a bit lagged. they will at least be as smooth as other players are.

		usercmd_t ucmd;
		client_t *oldhost;
		edict_t *oldplayer;

#ifdef VM_Q1
		if (svs.gametype == GT_Q1QVM)
		{
			pr_global_struct->self = EDICT_TO_PROG(svprogfuncs, sv.world.edicts);
			pr_global_struct->other = EDICT_TO_PROG(svprogfuncs, sv.world.edicts);
			pr_global_struct->time = sv.world.physicstime;
			Q1QVM_StartFrame(true);
		}
#endif
		if (1)
		{
			memset(&ucmd, 0, sizeof(ucmd));
			for (i = 0; i < sv.allocated_client_slots; i++)
			{
				if (svs.clients[i].state > cs_zombie && svs.clients[i].protocol == SCP_BAD && svs.clients[i].msecs >= 1000.0/77)
				{	//then this is a bot
					oldhost = host_client;
					oldplayer = sv_player;
					host_client = &svs.clients[i];
					host_client->isindependant = true;
					sv_player = host_client->edict;
					host_client->localtime = sv.time;

					SV_PreRunCmd();

					if (svs.gametype == GT_Q1QVM)
					{
						ucmd = svs.clients[i].lastcmd;
						ucmd.msec = svs.clients[i].msecs;
					}
					else
					{
						ucmd.msec = svs.clients[i].msecs;
						ucmd.angles[0] = (short)(sv_player->v->v_angle[0] * (65535/360.0f));
						ucmd.angles[1] = (short)(sv_player->v->v_angle[1] * (65535/360.0f));
						ucmd.angles[2] = (short)(sv_player->v->v_angle[2] * (65535/360.0f));
						ucmd.forwardmove = sv_player->xv->movement[0];
						ucmd.sidemove = sv_player->xv->movement[1];
						ucmd.upmove = sv_player->xv->movement[2];
						ucmd.buttons = (sv_player->v->button0?1:0) | (sv_player->v->button2?2:0);
					}
					ucmd.msec = min(ucmd.msec, 250);

					SV_RunCmd(&ucmd, false);
					SV_PostRunCmd();

					host_client->lastcmd = ucmd;	//allow the other clients to predict this bot.

					host_client = oldhost;
					sv_player = oldplayer;
				}
			}
		}
	}

// don't bother running a frame if sys_ticrate seconds haven't passed
	while (1)
	{
		host_frametime = sv.time - sv.world.physicstime;
		if (host_frametime < 0)
		{
			sv.world.physicstime = sv.time;
			break;
		}
		if (host_frametime <= 0 || host_frametime < mintic)
			break;
		if (host_frametime > maxtic && maxtic>0)
		{
			if (maxtics-- <= 0)
			{
				//timewarp, as we're running too slowly
				sv.world.physicstime = sv.time;
				break;
			}
			host_frametime = maxtic;
		}
		if (!host_frametime)
			continue;

		moved = true;

#ifdef HLSERVER
		if (svs.gametype == GT_HALFLIFE)
		{
			SVHL_RunFrame();
			sv.world.physicstime += host_frametime;
			continue;
		}
#endif

		pr_global_struct->frametime = host_frametime;

		SV_ProgStartFrame ();

		PR_RunThreads(&sv.world);

#ifdef USERBE
		if (sv.world.rbe)
		{
#ifdef RAGDOLL
			rag_doallanimations(&sv.world);
#endif
			sv.world.rbe->RunFrame(&sv.world, host_frametime, sv_gravity.value);
		}
#endif


		World_Physics_Frame(&sv.world);

#ifdef VM_Q1
		if (svs.gametype == GT_Q1QVM)
		{
			pr_global_struct->self = EDICT_TO_PROG(svprogfuncs, sv.world.edicts);
			pr_global_struct->other = EDICT_TO_PROG(svprogfuncs, sv.world.edicts);
			pr_global_struct->time = sv.world.physicstime+host_frametime;
			Q1QVM_EndFrame();
		}
		else
#endif
			if (EndFrameQC)
		{
			pr_global_struct->self = EDICT_TO_PROG(svprogfuncs, sv.world.edicts);
			pr_global_struct->other = EDICT_TO_PROG(svprogfuncs, sv.world.edicts);
			pr_global_struct->time = sv.world.physicstime+host_frametime;
			PR_ExecuteProgram (svprogfuncs, EndFrameQC);
		}

#ifdef NETPREPARSE
		NPP_Flush();	//flush it just in case there was an error and we stopped preparsing. This is only really needed while debugging.
#endif

		sv.world.physicstime += host_frametime;
	}
	host_frametime = trueframetime;
	return moved;
}
#endif

void SV_SetMoveVars(void)
{
	movevars.stopspeed		    = sv_stopspeed.value;
	movevars.maxspeed			= sv_maxspeed.value;
	movevars.spectatormaxspeed  = sv_spectatormaxspeed.value;
	movevars.accelerate		    = sv_accelerate.value;
	movevars.airaccelerate	    = sv_airaccelerate.value;
	movevars.wateraccelerate	= sv_wateraccelerate.value;
	movevars.friction			= sv_friction.value;
	movevars.waterfriction	    = sv_waterfriction.value;
	movevars.entgravity			= 1.0;
	movevars.stepheight			= *sv_stepheight.string?sv_stepheight.value:PM_DEFAULTSTEPHEIGHT;
	movevars.watersinkspeed		= *pm_watersinkspeed.string?pm_watersinkspeed.value:60;
	movevars.flyfriction		= *pm_flyfriction.string?pm_flyfriction.value:4;
	movevars.edgefriction		= *pm_edgefriction.string?pm_edgefriction.value:2;
	movevars.flags				= MOVEFLAG_VALID|MOVEFLAG_NOGRAVITYONGROUND|(*pm_edgefriction.string?0:MOVEFLAG_QWEDGEBOX);

	//FTESurf: Counter-Strike: Source movement (engine/common/pm_source.c).
	SV_SetSourceMoveVars();
}

/*
FTESurf Patch 133: the report, split out of SV_SetMoveVars.

Which physics a session ran under is the first thing you need to know when a
recorded time looks wrong, and it is not otherwise visible anywhere -- so this
is worth printing.  Once, though.

It used to sit at the bottom of SV_SetMoveVars, whose comment said "once per
map load" and which nothing enforced: SV_SpawnServer deliberately calls
SV_SetMoveVars TWICE (sv_init.c, once early to have the values in place while
the mod spawns, and once at the end to pick up any pm_* cvar a mod stuffcmd'd
on the way through).  Both calls printed, so every map load said everything
twice.  Hanging the print off the SECOND call site instead makes it
structurally once per SV_SpawnServer, with no latch to re-arm.
*/
void SV_ReportMoveVars(void)
{
	if (movevars.physicsmode != PHYSMODE_SOURCE)
		return;

	Con_Printf("^5Momentum movement^7: %g Hz tick, accel %g, airaccel %g, aircap %g, friction %g, stopspeed %g, gravity %g, jump %g, hull %g/%g, maxvel %g/axis\n",
		1.0/movevars.ticrate, movevars.accelerate, movevars.airaccelerate,
		movevars.maxairspeed, movevars.friction, movevars.stopspeed,
		movevars.gravity, movevars.jumpvelocity,
		movevars.standheight, movevars.duckheight, movevars.maxvelocity);
	Con_Printf("^5Momentum movement^7: eye %g/%g, viewscale %g (mid-air duck shifts the origin %g)\n",
		movevars.viewheight, movevars.duckviewheight, movevars.viewscale,
		movevars.viewscale * (movevars.standheight - movevars.duckheight));

	/* Quake BSPs ship PRECOMPUTED clip hulls and Q1BSP_ChooseHull (q1bsp.c:1527)
	   snaps whatever we ask for onto the nearest one -- a 62-tall request lands on
	   hull 1, which is 56, and a 45-tall duck request lands there too because
	   Quake has no hull 3.  So on a Q1 map the player is the wrong size and
	   ducking has no collision effect at all.  Source BSPs trace the real box via
	   BIH_Trace and are exact.  A Q1 map compiled with ericw-tools -wrbrushes
	   carries a BSPX BRUSHLIST lump, which makes gl_model.c swap in BIH_Trace and
	   is exact too -- so this warns rather than refuses. */
	if (sv.world.worldmodel && sv.world.worldmodel->fromgame == fg_quake)
		Con_Printf(CON_WARNING "Momentum movement on a Quake BSP: the player collides at this map's precompiled hull size, not %g/%g, and ducking may not shrink the hull at all. Source BSPs are exact.\n",
			movevars.standheight, movevars.duckheight);

	if (movevars.stamina)
		Con_Printf("^5CS:S stamina^7: jump %g%%, land %g%%, recovery rate %g (jump penalty lasts %.2fs)\n",
			movevars.staminajumpcost, movevars.staminalandcost, movevars.staminarecovery,
			movevars.staminarecovery > 0 ? movevars.staminajumpcost/movevars.staminarecovery : 0);
}

//FTESurf: kept separate from SV_SetMoveVars so the three other places that
//refresh movevars per-command (sv_user.c) can share one definition of what
//"the Source parameters" are.  Every value here is CVAR_SERVERINFO, so the
//client parses the same numbers in CL_ParseServerinfo and predicts with them.
void SV_SetSourceMoveVars(void)
{
	movevars.physicsmode		= pm_physicsmode.ival;
	movevars.ticrate			= pm_ticrate.value > 0 ? pm_ticrate.value : 0.015;
	movevars.maxairspeed		= pm_maxairspeed.value > 0 ? pm_maxairspeed.value : 30;
	movevars.jumpvelocity		= pm_jumpvelocity.value > 0 ? pm_jumpvelocity.value : 301.9933774;
	movevars.standablenormal	= pm_standablenormal.value > 0 ? pm_standablenormal.value : 0.7;
	movevars.bounce				= pm_sourcebounce.value;
	//ONE velocity cap, as Momentum has: pm_maxvelocity is an optional override and
	//defaults to 0.  Both keys are CVAR_SERVERINFO and CL_ParseServerinfo resolves
	//them in this same order, so the client predicts against the identical number.
	movevars.maxvelocity		= pm_maxvelocity.value > 0 ? pm_maxvelocity.value :
								  (sv_maxvelocity.value > 0 ? sv_maxvelocity.value : 3500);
	movevars.standheight		= pm_standheight.value > 0 ? pm_standheight.value : 62;
	movevars.duckheight			= pm_duckheight.value > 0 ? pm_duckheight.value : 45;
	movevars.duckspeed			= pm_duckspeed.value > 0 ? pm_duckspeed.value : 0.34;
	movevars.viewheight			= pm_viewheight.value > 0 ? pm_viewheight.value : 64;
	movevars.duckviewheight		= pm_duckviewheight.value > 0 ? pm_duckviewheight.value : 47;
	movevars.viewscale			= pm_viewscale.value > 0 ? pm_viewscale.value : 0.5;
	movevars.noclipspeed		= pm_noclipspeed.value > 0 ? pm_noclipspeed.value : 4;
	movevars.stamina			= pm_stamina.value;
	movevars.staminajumpcost	= pm_staminajumpcost.value;
	movevars.staminalandcost	= pm_staminalandcost.value;
	movevars.staminarecovery	= pm_staminarecovery.value > 0 ? pm_staminarecovery.value : 19;
	movevars.normalizejump		= pm_normalizejump.value;
	movevars.jumpaddrise		= pm_jumpaddrise.value;
	movevars.jumpzoffset		= pm_jumpzoffset.value;
	movevars.walkspeed			= pm_walkspeed.value;
	movevars.groundtracedist	= pm_groundtracedist.value;
	movevars.bumpcount			= pm_bumpcount.value;
	movevars.snaptoground		= pm_snaptoground.value;
	movevars.groundquadrants	= pm_groundquadrants.value;
	movevars.fixslopes			= pm_fixslopes.value;
	movevars.fixedges			= pm_fixedges.value;
	movevars.fixrampbugs		= pm_fixrampbugs.value;
	movevars.rampretrace		= pm_rampretrace.value;
	movevars.ladders			= pm_ladders.value;					//Patch 260
	movevars.ladderdampen		= pm_ladderdampen.value > 0 ? pm_ladderdampen.value : 0.2;
	movevars.ladderangle		= pm_ladderangle.value ? pm_ladderangle.value : -0.707;
}

/*
===========================================================================
FTESurf Patch 170: the movement ruleset lock.

A timed game whose physics can be retuned from the console is a game with no
times in it.  This restores every cvar that feeds the mover to this game's
default at each map load, and refuses changes afterwards unless sv_cheats is
1 -- so the ruleset is a property of the install, and departing from it is a
deliberate act you have to name.

THE CANONICAL VALUE IS defaultstr, NOT enginevalue.  `exec cfg/default.cfg`
is followed by an automatic `cvar_lockdefaults 1` (cmd.c:1067-1070), so every
`set` in a game's default.cfg becomes that cvar's default -- which is exactly
"what this game considers correct", maintained in one readable file rather
than duplicated into a table here.  enginevalue would be FTE's Quake numbers
(sv_airaccelerate 0.7, sv_maxspeed 320, sv_stopspeed 100), which is the
opposite of what a Source game wants; that is the trap CVAR_CHEAT falls into
at cvar.c:1163, and the reason this is not implemented with that flag.

The other two reasons it is not CVAR_CHEAT: Cvar_LockDefaults_f deliberately
SKIPS cheat cvars (cvar.c:566), so their defaultstr could never be the config
value anyway; and both cls.allow_cheats (cl_main.c:3173) and SV_MayCheat()
(sv_ccmds.c:36) hand out cheats unconditionally to any server with a single
client slot -- which is every FTESurf session ever played.  A cheat flag here
would have been silently inert.  sv_cheats.ival is read directly for that
reason: it is the only one of the three that means what it says.
===========================================================================
*/
extern cvar_t sv_cheats;

static cvar_t *pms_lockedmovevars[] =
{
	&pm_lockmovement,	/*locked by itself: see SV_MovementLocked*/

	/*read by SV_SetMoveVars*/
	&sv_stopspeed,		&sv_maxspeed,		&sv_accelerate,
	&sv_airaccelerate,	&sv_wateraccelerate,&sv_friction,
	&sv_waterfriction,	&sv_stepheight,		&pm_watersinkspeed,
	&pm_flyfriction,	&pm_edgefriction,

	/*read per-frame rather than latched into movevars*/
	&sv_gravity,		&sv_maxvelocity,

	/*read by SV_SetSourceMoveVars*/
	&pm_physicsmode,	&pm_ticrate,		&pm_maxairspeed,
	&pm_jumpvelocity,	&pm_standablenormal,&pm_sourcebounce,
	&pm_maxvelocity,	&pm_standheight,	&pm_duckheight,
	&pm_duckspeed,		&pm_viewheight,		&pm_duckviewheight,
	&pm_viewscale,
	&pm_stamina,		&pm_staminajumpcost,&pm_staminalandcost,
	&pm_staminarecovery,&pm_normalizejump,	&pm_jumpaddrise,
	&pm_jumpzoffset,
	&pm_walkspeed,		&pm_groundtracedist,&pm_bumpcount,
	&pm_snaptoground,	&pm_groundquadrants,&pm_fixslopes,
	&pm_fixedges,		&pm_fixrampbugs,	&pm_rampretrace,
	&pm_ladders,		&pm_ladderdampen,	&pm_ladderangle,	//Patch 260

	/*FTE's anti-bunnyhop family. Having these off is a rule, not a taste.*/
	&pm_bunnyspeedcap,	&pm_bunnyfriction,	&pm_ktjump,
	&pm_walljump,		&pm_autobunny,		&pm_airstep,
	&pm_pground,		&pm_stepdown,		&pm_slidefix,
	&pm_slidyslopes,

	/*NOT locked: pm_noclipspeed (noclip already voids the run, so how fast
	  you fly while voided is a preference) and sv_spectatormaxspeed.*/
	NULL
};

/*
FTESurf Patch 224: the gamemode's overlay on top of that table.

Patch 170 says the canonical value of a movement cvar is its defaultstr, i.e.
whatever cfg/default.cfg set before the automatic cvar_lockdefaults.  That is
still true for the BASE ruleset, and it is exactly the right answer for a game
with one ruleset.  This game has more than one: a bhop map is not a surf map
running slowly, it is a different tick and a different pace, and until now
every map in the library got surf's numbers because surf's numbers were the
only numbers there were.

So the canonical value becomes "the gamemode's override if there is one, and
defaultstr otherwise", in ONE function -- SV_MovementCanonical -- which both
the lock and its per-cvar callback ask.  Neither of them learns what a gamemode
is, and no third place can disagree with them about what the ruleset is.

The overlay is rebuilt from scratch at every map load (SV_ApplyGamemode), so
it can never carry one map's mode into the next; and it is consulted rather
than baked into defaultstr, so the base ruleset stays readable in
cfg/default.cfg rather than being mutated out from under it.
*/
#define PMS_MAXOVERRIDE 64
typedef struct
{
	cvar_t	*var;
	char	*val;		/*Z_StrDup'd; the mode's value for this cvar*/
} pms_override_t;
static pms_override_t	pms_over[PMS_MAXOVERRIDE];
static int				pms_numover;

static const char *SV_MovementCanonical(cvar_t *var)
{
	int i;
	for (i = 0; i < pms_numover; i++)
		if (pms_over[i].var == var)
			return pms_over[i].val;
	return var->defaultstr;
}

static qboolean SV_MovementLocked(void)
{
	/*The GAME's default decides whether the ruleset is locked -- not the live
	  value.  Reading the live value would leave a hole you could walk through
	  from the menu, where there is no server up to refuse `pm_lockmovement 0`
	  and the setting would then survive into the next map.*/
	if (!pm_lockmovement.defaultstr || !Q_atoi(pm_lockmovement.defaultstr))
		return false;
	return !sv_cheats.ival;
}

/*Fires after the value has already been committed (cvar.c:1041-1046), so this
  reverts rather than refuses.  Cvar_ForceSet re-enters us exactly once, and
  that pass returns at the strcmp below, so the recursion is bounded at one.*/
static void QDECL SV_MovementVar_Callback (struct cvar_s *var, char *oldvalue)
{
	const char *canon;
	if (sv.state != ss_active)
		return;		/*not in a game yet; the map spawn re-applies anyway*/
	if (!SV_MovementLocked())
		return;
	canon = SV_MovementCanonical(var);
	if (!canon || !strcmp(var->string, canon))
		return;

	Cvar_ForceSet(var, canon);
	Con_Printf(CON_WARNING"%s is part of the locked movement ruleset - "
			   "restored to \"%s\".\nUse \"sv_cheats 1\" first if you mean to "
			   "change it; anything you set then is not a legal run.\n",
			   var->name, canon);
}

void SV_LockMovementVars(void)
{
	cvar_t **v;
	int n = 0;

	if (!SV_MovementLocked())
		return;

	for (v = pms_lockedmovevars; *v; v++)
	{
		const char *canon = SV_MovementCanonical(*v);
		if (!canon || !strcmp((*v)->string, canon))
			continue;
		Cvar_ForceSet(*v, canon);
		n++;
	}

	if (n)
		Con_Printf("^5movement lock^7: %i cvar%s restored to the ruleset default\n",
					n, (n==1)?"":"s");
}

/*Hooked after registration rather than declared with CVARFC, so that adding a
  cvar to the table above is the only edit needed to lock it.*/
void SV_HookMovementLock(void)
{
	cvar_t **v;
	for (v = pms_lockedmovevars; *v; v++)
		Cvar_Hook(*v, SV_MovementVar_Callback);
}

/*
===========================================================================
FTESurf Patch 224: the movement ruleset is a property of the GAMEMODE.

WHAT WAS ACTUALLY WRONG.  Every map in the library ran surf's numbers -- 66.667
Hz, sv_airaccelerate 150 -- because nothing anywhere read the map and picked
anything else.  On a bhop map that is not a tuning preference, it is the wrong
game: these maps were built against CS:S servers running -tickrate 100, and a
third of the library is bhop or climb.

AND IT COULD NOT BE FIXED FROM THE CONSOLE, which is the part worth writing
down.  Patch 170's lock (above) restores every movement cvar to its default at
each map load and reverts any live change, so `sv_airaccelerate 1000` typed at
the console was reverted twice over -- immediately by the callback, and again
at the next map spawn.  A per-gamemode ruleset therefore cannot live outside
the lock; it has to BE the lock's idea of correct, which is why this sits in
this file rather than in the gamecode.

HOW THE MODE IS DECIDED, best evidence first:

  1. sv_gamemode, if it names one.  An explicit answer beats a derived one.
  2. data/mapmeta.txt column 9 -- Momentum's own gamemode id for this map,
     already shipped for 2271 maps and already read by the map browser
     (src/menu/m_main.qc).  This is a LOOKUP and it is right about maps whose
     name lies.
  3. the map name's prefix, for the ~700 maps Momentum has never indexed.
     A guess, but a good one, and it is the same prefix table the browser
     censused (surf 2025, bhop 240, kz 18, ...).

Nothing else is consulted, and in particular the BSP is not: a map that is not
in the index and does not say what it is in its name has nothing to read.

WHERE THE NUMBERS LIVE: cfg/mode_<name>.cfg in the gamedir, not in a table
here.  That is the same argument Patch 170 makes for defaultstr -- "what this
game considers correct, maintained in one readable file rather than duplicated
into a table" -- and it means a ruleset can be tuned, diffed and reviewed
without an engine build.  The specific mode is tried first and its CATEGORY
second (mode_kz.cfg, then mode_climb.cfg), so a category file covers its whole
family and a specific one can still depart from it.

surf has no file, deliberately.  Surf IS the base ruleset in cfg/default.cfg;
a mode_surf.cfg would be a second copy of it, free to disagree.

A mode may not set sv_cheats, pm_lockmovement, or any CVAR_CHEAT cvar.  A
ruleset that could turn the lock off would not be a ruleset.
===========================================================================
*/
cvar_t	sv_gamemode = CVARFD("sv_gamemode", "", CVAR_NOTFROMSERVER,
	"Which movement ruleset a map loads. Empty (or \"auto\") detects it from "
	"data/mapmeta.txt and then from the map name. A mode name -- surf, bhop, "
	"kz, climb, ahop, rj, sj, conc, defrag -- pins it for every map. \"none\" "
	"runs the base cfg/default.cfg ruleset everywhere, which is what this "
	"engine did before Patch 224.");

/*Momentum's gamemode ids, as they appear in mapmeta.txt column 9. Kept in ITS
  numbering rather than renumbered, so the file and this table cannot drift.
  Ported from src/menu/m_main.qc, which took them from Momentum's own
  panorama/scripts/common/web/enums/gamemode.enum.ts after an earlier
  reconstructed-by-census version turned out to be wrong for every id from 3 up.*/
static const char *pms_modename[] =
{
	NULL, "surf", "bhop", "bhop-hl1", "climb", "kz", "climb-16",
	"rj", "sj", "ahop", "conc", "df-cpm", "df-vq3", "df-vtg"
};
#define PMS_NUMMODES (int)(sizeof(pms_modename)/sizeof(pms_modename[0]))

/*GamemodeToGamemodeCategory, same source. Three ids are climb variants and
  three are defrag variants; the category is what a ruleset usually wants.*/
static const char *SV_GamemodeCategory(int id)
{
	switch(id)
	{
	case 1:						return "surf";
	case 2: case 3:				return "bhop";
	case 4: case 5: case 6:		return "climb";
	case 7:						return "rj";
	case 8:						return "sj";
	case 9:						return "ahop";
	case 10:					return "conc";
	case 11: case 12: case 13:	return "defrag";
	}
	return NULL;
}

/*The prefix fallback. Same set the map browser censused across all three
  mounts; the counts there are why this order is not alphabetical.*/
static const struct { const char *prefix; int mode; } pms_modeprefix[] =
{
	{"surf_",	1},		{"bhop_",	2},		{"kz_",		5},
	{"trikz_",	5},		{"climb_",	4},		{"xc_",		6},
	{"ahop_",	9},		{"rj_",		7},		{"sj_",		8},
	{"conc_",	10},	{"defrag_",	11},	{"df_",		11},
	{NULL, 0}
};

static int SV_GamemodeFromName(const char *mapname)
{
	int i;
	for (i = 0; pms_modeprefix[i].prefix; i++)
		if (!Q_strncasecmp(mapname, pms_modeprefix[i].prefix,
						   strlen(pms_modeprefix[i].prefix)))
			return pms_modeprefix[i].mode;
	return 0;
}

/*
data/mapmeta.txt, written by tools/mapmeta.py:

    meta <name> <tier> <linear> <stages> <bonuses> <page> <cell> <uuid> <mode> [tsrc]

A linear scan of 172 KB once per map load, which is nothing next to the BSP --
and a hash table here would have to be invalidated whenever the file changed,
which is a cache to get wrong in exchange for microseconds.

FTESurf Patch 227: the row is now read WHOLE rather than for its mode alone.

The tier is column 2 and this function was already walking past it to reach
column 9. It exists nowhere else the client can see: the map browser reads this
file, but the browser is menu.dat and the HUD is csprogs.dat, two separate progs.
Publishing the row as serverinfo is the one answer that is right in all three
places the game needs it -- live, in a demo (serverinfo is recorded), and for a
remote client in a P2P session whose own mapmeta.txt may not match the server's.

'-' means UNKNOWN and is not the same as 0. atoi("-") is 0, and there is no tier
0, no gamemode 0 and no map with 0 segments, so the sentinel survives the
conversion unambiguously in every column that uses it.
*/
typedef struct
{
	int mode;
	int tier;
	int linear;
	int stages;
	int bonuses;
	char tsrc[16];		/*rank / sugg / alias / over, or "" for a file written
						  before the column existed*/
} svmapmeta_t;

static qboolean SV_MapMetaLookup(const char *mapname, svmapmeta_t *out)
{
	char *file, *l, *e, *p;
	size_t sz, namelen = strlen(mapname);
	int col;
	qboolean found = false;

	memset(out, 0, sizeof(*out));

	file = FS_LoadMallocFile("data/mapmeta.txt", &sz);
	if (!file)
		return false;

	for (l = file; *l; l = e)
	{
		for (e = l; *e && *e != '\n'; e++)
			;
		if (*e)
			*e++ = 0;	/*terminate THIS line: COM_Parse skips newlines, so
						  without this a short row would read the next row's
						  columns and silently mode a map from its neighbour.*/

		if (strncmp(l, "meta ", 5))
			continue;
		if (Q_strncasecmp(l+5, mapname, namelen) || l[5+namelen] != ' ')
			continue;

		/*columns 2..10. Read rather than indexed by offset because the uuid
		  column's width is not something to rely on. The loop still stops on the
		  first empty token, so a row that is short of column 10 keeps everything
		  it did reach -- which is what makes the tsrc column additive exactly the
		  way the map browser treats it.*/
		p = l+5+namelen;
		for (col = 2; col <= 10; col++)
		{
			p = COM_Parse(p);
			if (!com_token[0])
				break;
			switch(col)
			{
			case 2:		out->tier    = atoi(com_token);	break;
			case 3:		out->linear  = atoi(com_token);	break;
			case 4:		out->stages  = atoi(com_token);	break;
			case 5:		out->bonuses = atoi(com_token);	break;
			case 9:		out->mode    = atoi(com_token);	break;
			case 10:	Q_strncpyz(out->tsrc, com_token, sizeof(out->tsrc));	break;
			}
		}
		found = true;
		break;
	}

	FS_FreeFile(file);
	return found;
}

/*Drop the current overlay. pms_numover is zeroed BEFORE anything is restored,
  because SV_MovementCanonical is what the per-cvar callback consults -- restore
  with the entries still live and the callback puts the mode's value straight
  back, and the ruleset never changes again.*/
static void SV_ClearGamemodeOverrides(qboolean restore)
{
	pms_override_t old[PMS_MAXOVERRIDE];
	int n = pms_numover, i;

	memcpy(old, pms_over, sizeof(old[0]) * n);
	pms_numover = 0;

	for (i = 0; i < n; i++)
	{
		/*Restores cvars the lock does NOT police too -- sv_mintic, cl_netfps,
		  fs_starthop. Those are not in pms_lockedmovevars, so SV_LockMovementVars
		  will not put them back and the previous mode would otherwise leak into
		  the next map.*/
		if (restore && old[i].var->defaultstr &&
			strcmp(old[i].var->string, old[i].var->defaultstr))
			Cvar_ForceSet(old[i].var, old[i].var->defaultstr);
		Z_Free(old[i].val);
	}
	memset(pms_over, 0, sizeof(pms_over));
}

static char pms_appliedmode[32];	/*"" when the base ruleset is running*/
static char pms_appliedsrc[16];		/*forced / mapmeta / name / none*/
static char pms_appliedfile[MAX_QPATH];

static qboolean SV_LoadGamemodeRuleset(const char *modename)
{
	char path[MAX_QPATH], name[64];
	char *file, *l, *e, *p;
	size_t sz;
	cvar_t *var;

	Q_snprintfz(path, sizeof(path), "cfg/mode_%s.cfg", modename);
	file = FS_LoadMallocFile(path, &sz);
	if (!file)
		return false;

	Q_strncpyz(pms_appliedfile, path, sizeof(pms_appliedfile));

	for (l = file; *l; l = e)
	{
		for (e = l; *e && *e != '\n'; e++)
			;
		if (*e)
			*e++ = 0;	/*per line, so a missing value cannot eat the next line's
						  cvar name and set the wrong thing to a plausible number*/

		p = COM_Parse(l);
		if (!com_token[0])
			continue;			/*blank, or a whole-line // comment*/

		/*`set`/`seta` accepted so the file reads like every other cfg here,
		  even though nothing is being registered.*/
		if (!strcmp(com_token, "set") || !strcmp(com_token, "seta") ||
			!strcmp(com_token, "sets") || !strcmp(com_token, "setr"))
		{
			p = COM_Parse(p);
			if (!com_token[0])
				continue;
		}
		Q_strncpyz(name, com_token, sizeof(name));

		p = COM_Parse(p);
		if (!com_token[0])
		{
			Con_Printf(CON_WARNING"%s: \"%s\" has no value - ignored\n", path, name);
			continue;
		}

		var = Cvar_FindVar(name);
		if (!var)
		{
			Con_Printf(CON_WARNING"%s: no cvar named \"%s\" - ignored\n", path, name);
			continue;
		}
		if (var == &pm_lockmovement || var == &sv_cheats || var == &sv_gamemode ||
			(var->flags & (CVAR_CHEAT|CVAR_SEMICHEAT)))
		{
			Con_Printf(CON_WARNING"%s: \"%s\" may not be set by a gamemode "
					   "ruleset - ignored\n", path, name);
			continue;
		}
		if (pms_numover >= PMS_MAXOVERRIDE)
		{
			Con_Printf(CON_WARNING"%s: more than %i cvars - the rest ignored\n",
					   path, PMS_MAXOVERRIDE);
			break;
		}

		/*Recorded BEFORE it is set, and that order is load-bearing: the set
		  fires the lock's callback, which asks SV_MovementCanonical what this
		  cvar should be. With the entry already in place the callback agrees
		  and returns; without it, the callback reverts the line we just read.*/
		pms_over[pms_numover].var = var;
		pms_over[pms_numover].val = Z_StrDup(com_token);
		pms_numover++;
		Cvar_ForceSet(var, com_token);
	}

	FS_FreeFile(file);
	return true;
}

/*
Called from SV_SpawnServer, in the one window where it is both possible and
correct: after the map name is known and the entities have spawned, and BEFORE
SV_LockMovementVars / SV_SetMoveVars / SV_ReportMoveVars read the values and
before any client sends `new` (SV_New_f writes movevars into the serverdata
message, which is how a client learns what to predict with).  A tick later and
the first map of a session would be predicted against the previous ruleset.
*/
void SV_ApplyGamemode(const char *mapname)
{
	char name[MAX_QPATH];
	int mode = 0;
	size_t l;
	const char *src = "none", *cat;
	svmapmeta_t meta;
	qboolean havemeta;

	SV_ClearGamemodeOverrides(true);
	*pms_appliedmode = *pms_appliedsrc = *pms_appliedfile = 0;
	InfoBuf_SetValueForKey(&svs.info, "gamemode", "");
	/*FTESurf Patch 227: cleared here with gamemode, so a map with no row cannot
	  wear the previous map's tier.*/
	InfoBuf_SetValueForKey(&svs.info, "maptier", "");
	InfoBuf_SetValueForKey(&svs.info, "maplinear", "");
	InfoBuf_SetValueForKey(&svs.info, "mapstages", "");
	InfoBuf_SetValueForKey(&svs.info, "mapbonuses", "");
	InfoBuf_SetValueForKey(&svs.info, "maptiersrc", "");

	if (!mapname || !*mapname)
		return;

	/*svs.name is the bare map name today, but this is called with whatever the
	  spawn was given and a stray "maps/" or ".bsp" would silently defeat every
	  prefix test below -- which fails as "no gamemode", the quietest possible
	  wrong answer.*/
	if (!Q_strncasecmp(mapname, "maps/", 5))
		mapname += 5;
	Q_strncpyz(name, mapname, sizeof(name));
	l = strlen(name);
	if (l > 4 && !Q_strcasecmp(name+l-4, ".bsp"))
		name[l-4] = 0;
	mapname = name;

	/*
	FTESurf Patch 227: read the row ONCE, HERE, and unconditionally.

	The lookup used to hang off gamemode detection three branches down, which is
	the wrong place for anything but the mode: `sv_gamemode surf` skips that
	branch entirely and `sv_gamemode none` returns above it, so on exactly the
	installs that set the cvar the tier would silently never be published. Same
	single file read as before -- it has just stopped being conditional on a
	question it is not being asked.
	*/
	havemeta = SV_MapMetaLookup(mapname, &meta);
	if (havemeta)
	{
		if (meta.tier > 0)
			InfoBuf_SetValueForKey(&svs.info, "maptier", va("%i", meta.tier));
		InfoBuf_SetValueForKey(&svs.info, "maplinear",  va("%i", meta.linear));
		InfoBuf_SetValueForKey(&svs.info, "mapstages",  va("%i", meta.stages));
		InfoBuf_SetValueForKey(&svs.info, "mapbonuses", va("%i", meta.bonuses));
		if (*meta.tsrc)
			InfoBuf_SetValueForKey(&svs.info, "maptiersrc", meta.tsrc);
	}

	if (*sv_gamemode.string && Q_strcasecmp(sv_gamemode.string, "auto"))
	{
		int i;
		if (!Q_strcasecmp(sv_gamemode.string, "none"))
		{
			Q_strncpyz(pms_appliedsrc, "off", sizeof(pms_appliedsrc));
			return;
		}
		for (i = 1; i < PMS_NUMMODES; i++)
			if (!Q_strcasecmp(sv_gamemode.string, pms_modename[i]))
			{
				mode = i;
				break;
			}
		if (!mode)
		{
			/*A category name is a legal answer too -- "bhop" is both.*/
			for (i = 1; i < PMS_NUMMODES; i++)
			{
				cat = SV_GamemodeCategory(i);
				if (cat && !Q_strcasecmp(sv_gamemode.string, cat))
				{
					mode = i;
					break;
				}
			}
		}
		if (!mode)
		{
			Con_Printf(CON_WARNING"sv_gamemode \"%s\" is not a mode name - "
					   "detecting instead\n", sv_gamemode.string);
		}
		else
			src = "forced";
	}

	if (!mode)
	{	//Patch 227: from the row read above rather than from a second scan of the file
		mode = havemeta ? meta.mode : 0;
		if (mode)
			src = "mapmeta";
	}
	if (!mode)
	{
		mode = SV_GamemodeFromName(mapname);
		if (mode)
			src = "name";
	}

	if (mode <= 0 || mode >= PMS_NUMMODES)
	{
		Con_Printf("^5movement^7: %s has no gamemode - base ruleset\n", mapname);
		Q_strncpyz(pms_appliedsrc, "none", sizeof(pms_appliedsrc));
		return;
	}

	Q_strncpyz(pms_appliedmode, pms_modename[mode], sizeof(pms_appliedmode));
	Q_strncpyz(pms_appliedsrc, src, sizeof(pms_appliedsrc));
	InfoBuf_SetValueForKey(&svs.info, "gamemode", pms_appliedmode);

	cat = SV_GamemodeCategory(mode);
	if (!SV_LoadGamemodeRuleset(pms_appliedmode))
		if (!cat || !strcmp(cat, pms_appliedmode) || !SV_LoadGamemodeRuleset(cat))
		{
			/*No file is not an error. surf has none by design, and a mode
			  nobody has written a ruleset for should run the base one rather
			  than refuse to load.*/
			Con_Printf("^5movement^7: %s is %s (%s), base ruleset\n",
					   mapname, pms_appliedmode, pms_appliedsrc);
			return;
		}

	Con_Printf("^5movement^7: %s is %s (%s), %i cvar%s from %s\n",
			   mapname, pms_appliedmode, pms_appliedsrc, pms_numover,
			   (pms_numover==1)?"":"s", pms_appliedfile);
}

/*
`movement` -- what ruleset is running, where it came from, and every cvar that
departs from cfg/default.cfg because of it.

The three columns are the whole point: a mode that says it set pm_ticrate and a
pm_ticrate that still reads 0.015 is the failure this is for, and one column
could not show it.
*/
void SV_Movement_f(void)
{
	int i;

	if (sv.state != ss_active)
	{
		Con_Printf("^5movement^7: no server running\n");
		return;
	}

	Con_Printf("^5movement^7: %s%s^7 on %s, from %s%s\n",
			   *pms_appliedmode?"":"^3",
			   *pms_appliedmode?pms_appliedmode:"base ruleset",
			   svs.name,
			   *pms_appliedsrc?pms_appliedsrc:"nothing",
			   *pms_appliedfile?va(" (%s)", pms_appliedfile):"");

	if (!SV_MovementLocked())
		Con_Printf("  ^3the ruleset lock is off^7 - %s\n",
				   sv_cheats.ival?"sv_cheats is 1, so no run here is legal"
								 :"pm_lockmovement's default is 0");

	if (pms_numover)
	{
		Con_Printf("  %-24s %-12s %-12s\n", "cvar", "live", "base");
		for (i = 0; i < pms_numover; i++)
		{
			cvar_t *v = pms_over[i].var;
			qboolean applied = !strcmp(v->string, pms_over[i].val);
			Con_Printf("  %-24s %s%-12s^7 %-12s\n", v->name,
					   applied?"":"^1", v->string,
					   v->defaultstr?v->defaultstr:"?");
		}
	}
	else
		Con_Printf("  no overrides - every value is cfg/default.cfg's\n");

	SV_ReportMoveVars();
}
#endif
