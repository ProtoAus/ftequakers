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
#include "quakedef.h"

static qboolean PM_TransformedHullCheck (model_t *model, framestate_t *framestate, vec3_t start, vec3_t end, vec3_t mins, vec3_t maxs, trace_t *trace, vec3_t origin, vec3_t angles, float scale);
int Q1BSP_HullPointContents(hull_t *hull, vec3_t p);
static	hull_t		box_hull;
static	mclipnode_t	box_clipnodes[6];
static	mplane_t	box_planes[6];

/*
===================
PM_InitBoxHull

Set up the planes and clipnodes so that the six floats of a bounding box
can just be stored out and get a proper hull_t structure.
===================
*/
void PM_InitBoxHull (void)
{
	int		i;
	int		side;

	box_hull.clipnodes = box_clipnodes;
	box_hull.planes = box_planes;
	box_hull.firstclipnode = 0;
	box_hull.lastclipnode = 5;

	for (i=0 ; i<6 ; i++)
	{
		box_clipnodes[i].planenum = i;
		
		side = i&1;
		
		box_clipnodes[i].children[side] = Q1CONTENTS_EMPTY;
		if (i != 5)
			box_clipnodes[i].children[side^1] = i + 1;
		else
			box_clipnodes[i].children[side^1] = Q1CONTENTS_SOLID;
		
		box_planes[i].type = i>>1;
		box_planes[i].normal[i>>1] = 1;
	}
	
}


/*
===================
PM_HullForBox

To keep everything totally uniform, bounding boxes are turned into small
BSP trees instead of being compared directly.
===================
*/
static hull_t	*PM_HullForBox (vec3_t mins, vec3_t maxs)
{
	box_planes[0].dist = maxs[0];
	box_planes[1].dist = mins[0];
	box_planes[2].dist = maxs[1];
	box_planes[3].dist = mins[1];
	box_planes[4].dist = maxs[2];
	box_planes[5].dist = mins[2];

	return &box_hull;
}


static int PM_TransformedModelPointContents (model_t *mod, vec3_t p, vec3_t origin, vec3_t angles)
{
	vec3_t p_l, axis[3];
	VectorSubtract (p, origin, p_l);

	if (!mod->funcs.PointContents)
		return FTECONTENTS_EMPTY;

	// rotate start and end into the models frame of reference
	if (angles[0] || angles[1] || angles[2])
	{
		AngleVectors (angles, axis[0], axis[1], axis[2]);
		VectorNegate(axis[1], axis[1]);
		return mod->funcs.PointContents(mod, axis, p_l);
	}

	return mod->funcs.PointContents(mod, NULL, p_l);
}


/*
==================
PM_PointContents

==================
*/
int PM_PointContents (vec3_t p)
{
	int			num;

	int pc;
	physent_t *pe;
	model_t *pm;

	//check world.
	pm = pmove.physents[0].model;
	if (!pm || pm->loadstate != MLS_LOADED)
		return FTECONTENTS_EMPTY;
	pc = pm->funcs.PointContents(pm, NULL, p);

	//we need this for e2m2 - waterjumping on to plats wouldn't work otherwise.
	for (num = 1; num < pmove.numphysent; num++)
	{
		pe = &pmove.physents[num];

		if (pe->info == pmove.skipent)
			continue;

		pm = pe->model;
		if (pm)
		{
			if (p[0] >= pe->origin[0]+pm->mins[0] && p[0] <= pe->origin[0]+pm->maxs[0] && 
				p[1] >= pe->origin[1]+pm->mins[1] && p[1] <= pe->origin[1]+pm->maxs[1] &&
				p[2] >= pe->origin[2]+pm->mins[2] && p[2] <= pe->origin[2]+pm->maxs[2])
			{
				if (pe->forcecontentsmask)
				{
					if (PM_TransformedModelPointContents(pm, p, pe->origin, pe->angles))
						pc |= pe->forcecontentsmask;
				}
				else
				{
					if (pe->nonsolid)
						continue;
					pc |= PM_TransformedModelPointContents(pm, p, pe->origin, pe->angles);
				}
			}
		}
		else if (pe->forcecontentsmask)
		{
			if (p[0] >= pe->origin[0]+pe->mins[0] && p[0] <= pe->origin[0]+pe->maxs[0] && 
				p[1] >= pe->origin[1]+pe->mins[1] && p[1] <= pe->origin[1]+pe->maxs[1] &&
				p[2] >= pe->origin[2]+pe->mins[2] && p[2] <= pe->origin[2]+pe->maxs[2])
				pc |= pe->forcecontentsmask;
		}
	}

	return pc;
}

int PM_ExtraBoxContents (vec3_t p)
{
	int			num;

	int pc = 0;
	physent_t *pe;
	model_t *pm;
	trace_t tr;

	for (num = 1; num < pmove.numphysent; num++)
	{
		pe = &pmove.physents[num];
		if (!pe->nonsolid)
			continue;
		pm = pe->model;
		if (pm)
		{
			if (pe->forcecontentsmask)
			{
				if (!PM_TransformedHullCheck(pm, PE_FRAMESTATE, p, p, pmove.player_mins, pmove.player_maxs, &tr, pe->origin, pe->angles, pe->scale))
					continue;
				if (tr.startsolid || tr.inwater)
					pc |= pe->forcecontentsmask;
			}
		}
		else if (pe->forcecontentsmask)
		{
			if (p[0]+pmove.player_maxs[0] >= pe->origin[0]+pe->mins[0] && p[0]+pmove.player_mins[0] <= pe->origin[0]+pe->maxs[0] && 
				p[1]+pmove.player_maxs[1] >= pe->origin[1]+pe->mins[1] && p[1]+pmove.player_mins[1] <= pe->origin[1]+pe->maxs[1] &&
				p[2]+pmove.player_maxs[2] >= pe->origin[2]+pe->mins[2] && p[2]+pmove.player_mins[2] <= pe->origin[2]+pe->maxs[2])
				pc |= pe->forcecontentsmask;
		}
	}

	return pc;
}

/*
===============================================================================

LINE TESTING IN HULLS

===============================================================================
*/

/*returns if it actually did a trace*/
//nettest Patch 57: client mirror of the server's World_HullTrace (server/world.c). Clips
//the swept player box against model->hullplanes (model-space outward normal .xyz + support
//.w), rotated into world by the prop angles, scaled by the prop scale, origin-shifted; an
//enter/leave-fraction loop with the SAME 0.03125 back-off. start/end/mins/maxs are WORLD
//space. Keeps client prediction bit-identical to server authority so SOLID_PHYSICS_TRIMESH
//props don't glitch through / rubber-band / FPS-dip.
//nettest Patch 61: client mirror of the server's World_HullClipOne — clip the swept box
//against ONE convex piece. Returns false on a clean miss. MUST match the server bit-for-bit.
static qboolean PM_HullClipOne (int numplanes, vec4_t *planes, const vec3_t axis[3], const vec3_t origin, float scale,
		const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs,
		float *out_enterfrac, float *out_nearfrac, vec3_t out_hitnorm, qboolean *out_startout, qboolean *out_getout)
{
	vec3_t	nw, ofs, hitnorm;
	float	enterfrac = -1, nearfrac = -1, leavefrac = 2, d1, d2, f, dist, dw;
	qboolean startout = false, getout = false;
	int		j;

	VectorClear (hitnorm);
	for (j = 0; j < numplanes; j++)
	{
		const float *pl = planes[j];
		nw[0] = pl[0]*axis[0][0] + pl[1]*axis[1][0] + pl[2]*axis[2][0];
		nw[1] = pl[0]*axis[0][1] + pl[1]*axis[1][1] + pl[2]*axis[2][1];
		nw[2] = pl[0]*axis[0][2] + pl[1]*axis[1][2] + pl[2]*axis[2][2];
		dw = pl[3] * scale + DotProduct (origin, nw);

		ofs[0] = (nw[0] < 0) ? maxs[0] : mins[0];
		ofs[1] = (nw[1] < 0) ? maxs[1] : mins[1];
		ofs[2] = (nw[2] < 0) ? maxs[2] : mins[2];
		dist = dw - DotProduct (ofs, nw);
		d1 = DotProduct (start, nw) - dist;
		d2 = DotProduct (end,   nw) - dist;
		if (d1 > 0) startout = true;
		if (d2 > 0) getout = true;
		if (d1 > 0 && d2 >= d1) return false;	//in front of a plane: clean miss of this piece
		if (d1 <= 0 && d2 <= 0) continue;		//behind it: inside this plane
		if (d1 > d2)
		{
			f = d1 / (d1 - d2);
			if (f > enterfrac)
			{	//nettest Patch 63: raw enter (union compare) + normal-direction back-off (match server)
				enterfrac = f;
				nearfrac = (d1 - 0.03125) / (d1 - d2);
				VectorCopy (nw, hitnorm);
			}
		}
		else
		{
			f = d1 / (d1 - d2);
			if (f < leavefrac) leavefrac = f;
		}
	}
	*out_startout = startout;
	*out_getout = getout;
	if (enterfrac <= leavefrac)
	{
		*out_enterfrac = enterfrac;
		*out_nearfrac  = nearfrac;
	}
	else
	{
		*out_enterfrac = -1;
		*out_nearfrac  = -1;
	}
	VectorCopy (hitnorm, out_hitnorm);
	return true;
}

//nettest Patch 57/61: client mirror of World_HullTrace. 'usedecomp' clips against the
//per-submesh decomposition (mode 3); else the single hull (mode 2). Union: nearest entered
//piece wins; startsolid if inside any piece. MUST stay bit-identical to the server or
//SOLID_PHYSICS_TRIMESH props glitch through / rubber-band.
static void PM_HullTrace (model_t *model, qboolean usedecomp, vec3_t origin, vec3_t angles, float scale, vec3_t start, vec3_t end, vec3_t mins, vec3_t maxs, trace_t *trace)
{
	vec3_t	axis[3], hitnorm, besthitnorm;
	vec3_t	lmin, lmax;	//nettest Patch 65: swept player box in the prop LOCAL frame, for the per-piece cull
	float	bestenter = 1, bestnear = 1;
	qboolean anystart = false, anyall = false, hashit = false;
	int		h, nh, k;

	memset (trace, 0, sizeof(*trace));
	trace->fraction = 1;
	trace->truefraction = 1;
	trace->inopen = true;	//nettest Patch 61: match the server World_HullTrace
	VectorCopy (end, trace->endpos);

	if (IS_NAN(end[0]) || IS_NAN(end[1]) || IS_NAN(end[2]))	//match the server's guard
		return;

	if (scale <= 0) scale = 1;
	//nettest Patch 64: match the renderer + server World_HullTrace (AngleVectorsMesh = r_meshpitch
	//on pitch, r_meshroll on roll) so the predicted hull matches the visible pitched/rolled model.
	//SOLID_PHYSICS_TRIMESH is always alias; identical to raw AngleVectors at r_meshpitch 1.
	AngleVectorsMesh (angles, axis[0], axis[1], axis[2]);
	VectorNegate (axis[1], axis[1]);

	//nettest Patch 65: swept player box in the prop LOCAL frame for the per-piece cull (mirror the
	//server World_HullTrace EXACTLY — model_pt = axis.(world-origin)).
	{
		vec3_t ds, de, pcenter, phalf;
		for (k = 0; k < 3; k++) { pcenter[k] = (maxs[k]+mins[k])*0.5f; phalf[k] = (maxs[k]-mins[k])*0.5f; }
		VectorSubtract (start, origin, ds);
		VectorSubtract (end,   origin, de);
		for (k = 0; k < 3; k++)
		{
			float c  = DotProduct(axis[k], pcenter);
			float lh = fabs(axis[k][0])*phalf[0] + fabs(axis[k][1])*phalf[1] + fabs(axis[k][2])*phalf[2];
			float a  = DotProduct(ds, axis[k]) + c;
			float b  = DotProduct(de, axis[k]) + c;
			lmin[k] = (a < b ? a : b) - lh;
			lmax[k] = (a > b ? a : b) + lh;
		}
	}

	VectorClear (besthitnorm);
	nh = usedecomp ? model->numhulls : 1;
	for (h = 0; h < nh; h++)
	{
		int np; vec4_t *pl;
		float enterfrac, nearfrac; qboolean startout, getout;
		if (usedecomp)
		{	//per-piece AABB cull (match the server bit-for-bit: piece AABB unscaled -> *scale).
			const convhull_t *ch = &model->convhulls[h];
			if (lmin[0] > ch->maxs[0]*scale || lmax[0] < ch->mins[0]*scale ||
			    lmin[1] > ch->maxs[1]*scale || lmax[1] < ch->mins[1]*scale ||
			    lmin[2] > ch->maxs[2]*scale || lmax[2] < ch->mins[2]*scale)
				continue;
			np = ch->numplanes; pl = ch->planes;
		}
		else           { np = model->numhullplanes;      pl = model->hullplanes; }
		if (np < 4)
			continue;
		if (!PM_HullClipOne (np, pl, axis, origin, scale, start, end, mins, maxs, &enterfrac, &nearfrac, hitnorm, &startout, &getout))
			continue;
		if (!startout)
		{
			anystart = true;
			if (!getout) anyall = true;
		}
		else if (enterfrac > -1)
		{	//nearest entered piece (min raw enterfrac); use its normal-back-off nearfrac
			if (enterfrac < bestenter)
			{
				bestenter = enterfrac;
				bestnear  = nearfrac;
				VectorCopy (hitnorm, besthitnorm);
				hashit = true;
			}
		}
	}

	if (anystart)
	{
		trace->startsolid = true;
		if (anyall)
			trace->allsolid = true;
		return;
	}
	if (hashit)
	{	//nettest Patch 63: fraction = normal back-off (match server World_HullTrace exactly)
		float efn = (bestnear  < 0) ? 0 : bestnear;
		float eft = (bestenter < 0) ? 0 : bestenter;
		trace->fraction = efn;
		trace->truefraction = eft;
		VectorInterpolate (start, efn, end, trace->endpos);
		VectorCopy (besthitnorm, trace->plane.normal);
		VectorNormalize (trace->plane.normal);
		trace->plane.dist = DotProduct (trace->endpos, trace->plane.normal);
		trace->contents = FTECONTENTS_BODY;
	}
}

static qboolean PM_TransformedHullCheck (model_t *model, framestate_t *framestate, vec3_t start, vec3_t end, vec3_t player_mins, vec3_t player_maxs, trace_t *trace, vec3_t origin, vec3_t angles, float scale)
{
	vec3_t		start_l, end_l;
	int i;
	vec3_t		axis[3];

	//nettest Patch 57: a SOLID_PHYSICS_TRIMESH prop with a convex hull uses the SAME hull
	//trace the server does (sv_prop_collision 2, the default) so client prediction matches
	//authority — no glitch-through, no per-triangle FPS dip, correct scale. The SERVERINFO
	//cvar is synced to the client, so both sides pick the same mode.
	if (model && (model->numhullplanes >= 4 || model->numhulls > 0) &&
	    (player_mins[0]!=player_maxs[0] || player_mins[1]!=player_maxs[1] || player_mins[2]!=player_maxs[2]))
	{
		static cvar_t *pm_propcol;
		int cm;
		if (!pm_propcol)
			pm_propcol = Cvar_Get("sv_prop_collision", "2", CVAR_SERVERINFO, NULL);
		cm = pm_propcol ? pm_propcol->ival : 2;
		if (cm == 3 && (model->numhulls > 0 || model->numhullplanes >= 4))
		{	//convex decomposition (mode 3) — mirror the server's per-submesh union
			PM_HullTrace (model, model->numhulls > 0, origin, angles, scale, start, end, player_mins, player_maxs, trace);
			return true;	//endpos already world-space
		}
		if (cm == 2 && model->numhullplanes >= 4)
		{	//single convex hull (mode 2)
			PM_HullTrace (model, false, origin, angles, scale, start, end, player_mins, player_maxs, trace);
			return true;	//endpos already world-space
		}
	}

	// subtract origin offset
	VectorSubtract (start, origin, start_l);
	VectorSubtract (end, origin, end_l);

	// sweep the box through the model
	if (model && model->funcs.NativeTrace)
	{
		if (angles[0] || angles[1] || angles[2])
		{
			//nettest Patch 64: mirror the server World_TransformedTrace EXACTLY — an alias/IQM
			//model's basis uses r_meshpitch/r_meshroll (AngleVectorsMesh), a brush uses raw. The
			//client previously used raw here while the server applied meshpitch -> a mode-1
			//(sv_prop_collision 1, per-triangle) prediction desync on a pitched prop. Now matched.
			if (model->type == mod_alias)
				AngleVectorsMesh (angles, axis[0], axis[1], axis[2]);
			else
				AngleVectors (angles, axis[0], axis[1], axis[2]);
			VectorNegate(axis[1], axis[1]);
			model->funcs.NativeTrace(model, 0, framestate, axis, start_l, end_l, player_mins, player_maxs, pmove.capsule, MASK_PLAYERSOLID, trace);
		}
		else
		{
			for (i = 0; i < 3; i++)
			{
				if (start_l[i]+player_mins[i] > model->maxs[i] && end_l[i] + player_mins[i] > model->maxs[i])
					return false;
				if (start_l[i]+player_maxs[i] < model->mins[i] && end_l[i] + player_maxs[i] < model->mins[i])
					return false;
			}
			model->funcs.NativeTrace(model, 0, framestate, NULL, start_l, end_l, player_mins, player_maxs, pmove.capsule, MASK_PLAYERSOLID, trace);
		}
	}
	else
	{
		for (i = 0; i < 3; i++)
		{
			if (start_l[i]+player_mins[i] > box_planes[0+i*2].dist && end_l[i] + player_mins[i] > box_planes[0+i*2].dist)
				return false;
			if (start_l[i]+player_maxs[i] < box_planes[1+i*2].dist && end_l[i] + player_maxs[i] < box_planes[1+i*2].dist)
				return false;
		}

		memset (trace, 0, sizeof(trace_t));
		trace->fraction = 1;
		trace->allsolid = true;
		Q1BSP_RecursiveHullCheck (&box_hull, box_hull.firstclipnode, start_l, end_l, MASK_PLAYERSOLID, trace);
	}

	trace->endpos[0] += origin[0];
	trace->endpos[1] += origin[1];
	trace->endpos[2] += origin[2];
	return true;
}


//a portal is flush with a world surface behind it.
//this causes problems. namely that we can't pass through the portal plane if the bsp behind it prevents out origin from getting through.
//so if the trace was clipped and ended infront of the portal, continue the trace to the edges of the portal cutout instead.
/*
  FTESurf Patch 206.  The quietest failure in the portal path, made audible.

  When this window refuses, NOTHING downstream can report it, and that is not an
  oversight in the logging -- it is structural.  A refused carve leaves the world
  trace stopping the player's BOX short of the plane, while the portal's own
  point trace only reaches the aperture AT the plane; the box trace therefore has
  the smaller fraction and wins the comparison in PM_PlayerTrace, so total.entnum
  stays 0 (the world).  PM_PlayerTracePortals' `if (impact->isportal)` is never
  entered, camera_transform is never called, and BOTH of Patch 203's refusal
  prints -- written precisely to catch this -- are unreachable.  The player just
  stops, silently, and every log says nothing at all.

  TWO filters, and the first one is not optional.  AddPortalsToPmove adds EVERY
  portal on the map to every move regardless of distance (sv_user.c:7158-7197) --
  that is deliberate, it is what makes the exit-side carve possible -- so this
  function runs eighteen times per trace on surf_kitsune and seventeen of those
  are doors thousands of units away refusing by thousands of units.  Reporting
  those buries the one that matters: the first run of this printed 18 lines per
  trace and several thousand lines per second.  Only a near miss is a diagnosis.

  The second is printing on change rather than per frame, the way
  Surf_VoidVisReport does: a player leaning on a doorway generates the same line
  hundreds of times a second, and a diagnostic that floods gets turned off.
*/
#define PM_PORTALREPORTDIST 256
static void PM_PortalCSGReport(int entnum, int plane, float miss)
{
	static const char *edge[6] = {"in front of it", "behind it", "past its right edge",
	                              "past its left edge", "above it", "below it"};
	/* Keyed PER DOOR, not one slot for the lot.  A single last-seen slot looks
	   right and is defeated by the commonest case there is: two doorways in
	   reach of the same trace alternate, each counting as "a change" from the
	   other, and the line prints hundreds of times a second anyway.

	   PATCH 208 CORRECTS THE FIX FOR THAT, which claimed "a collision costs one
	   extra line, never a missed diagnosis".  It costs an unbounded number of
	   lines, because a collision does not happen once -- it happens on every
	   trace, forever, and it is exactly the alternation the single slot was
	   replaced to cure.  Sixteen slots hashed as entnum&15 alias every pair of
	   doors sixteen apart, and surf_kitsune's entry hall has FOUR such doors in
	   range at once: 1 with 17, and 3 with 19.  Standing still in front of them
	   wrote about a thousand lines per door in one screenshot's worth of time,
	   dragged the game to 0.9 fps, and cost a verification run.

	   So: linear probing, which gives distinct entities distinct slots outright
	   instead of hoping they do not collide.  A full table (more doors in range
	   at once than there are slots) degrades to overwriting at the hash, i.e. to
	   the old behaviour, which is the right floor to fall back to.

	   The threshold is the other half.  At one unit, WALKING toward a door is a
	   change every single tick -- 250 u/s is 2.5 units a tick -- so the approach
	   floods even with the table behaving.  Eight units bounds one approach to
	   at most PM_PORTALREPORTDIST/8 lines per door while still printing the
	   refusal itself exactly, which is the line anyone is reading this for. */
	#define PM_PORTALREPORTSLOTS 64
	#define PM_PORTALREPORTDELTA 8
	static int   lastplane[PM_PORTALREPORTSLOTS];
	static float lastmiss[PM_PORTALREPORTSLOTS];
	static int   lastent[PM_PORTALREPORTSLOTS];	//0 == free; a portal is never entity 0
	int slot, probe, i;

	if (plane < 0 || plane > 5)
		return;
	if (-miss > PM_PORTALREPORTDIST)
		return;		//a door on the other side of the map, not a diagnosis

	slot = probe = (unsigned int)entnum % PM_PORTALREPORTSLOTS;
	for (i = 0; i < PM_PORTALREPORTSLOTS; i++)
	{
		probe = (slot + i) % PM_PORTALREPORTSLOTS;
		if (!lastent[probe] || lastent[probe] == entnum)
			break;		//free slot, or this door's own slot
	}
	if (i == PM_PORTALREPORTSLOTS)
		probe = slot;	//table full: overwrite at the hash rather than lose the report
	slot = probe;

	if (lastent[slot] == entnum && lastplane[slot] == plane &&
		fabs(miss - lastmiss[slot]) < PM_PORTALREPORTDELTA)
		return;
	lastent[slot] = entnum;
	lastplane[slot] = plane;
	lastmiss[slot] = miss;

	Con_DPrintf("portal %i: aperture refused, %s by %.1f units\n",
		entnum, edge[plane], -miss);
}

static void PM_PortalCSG(physent_t *portal, int entnum, float *trmin, float *trmax, vec3_t start, vec3_t end, trace_t *trace)
{
	vec4_t planes[6];	//far, near, right, left, up, down
	int plane;
	vec3_t worldpos;
	vec3_t center;		//P203: the aperture's centre, world space
	vec3_t halfsize;	//P203: the aperture's half-extents, on world axes
	float hwidth, hheight;	//P203: those half-extents along the portal's own right and up
	int hitplane = -1;
	float bestfrac;
	vec3_t movedir;		//P214: which way this trace is trying to go
	vec3_t botcorner;	//P216: the player box's lowest corner along the aperture's up
	float aperturebottom;	//P216: the opening's own bottom edge, with no box slop on it
	float feetdist;
	//only run this code if we impacted on the portal's parent.
	if (trace->fraction == 1 && !trace->startsolid)
		return;

	if (trace->startsolid)
		VectorCopy(start, worldpos);	//make sure we use a sane valid position.
	else
		VectorCopy(trace->endpos, worldpos);

	//determine the csg area. normals should be facing in
	AngleVectors(portal->angles, planes[1], planes[3], planes[5]);
	VectorNegate(planes[1], planes[0]);
	VectorNegate(planes[3], planes[2]);
	VectorNegate(planes[5], planes[4]);

	/* ---- FTESurf Patch 203, gap G2: the window, from the portal's own bounds ----

	   This function is what lets a player walk THROUGH a portal that is flush
	   against a wall: it carves the world behind the aperture out of the trace.
	   It was carving a fixed box -- portalradius 128, halved to 64, then shrunk
	   by a hardcoded 24 on each side plane -- so the accepted region was +-40
	   about the portal ORIGIN along right and up, for every portal in existence.

	   That cannot reach the player.  Source's linked_portal_door origin is the
	   CENTRE of the rectangle; FTE's player origin is at the FEET.  For an 88-tall
	   door standing on the floor the feet are 44 below centre, 44 > 40, so the up
	   plane rejected, the function took its `return; //end is already outside`, the
	   wall stayed solid, and PMSrc_CheckStuck then shoved the player up to 16 units
	   every tick they stood in the doorway.  The portal was unusable on foot.

	   Two changes.  The window now comes from the entity's own mins/maxs -- which
	   Patch 203's other half finally copies into the physent -- projected onto the
	   portal's local right and up.  QC bounds are world-axis-aligned, so the
	   projection is exact at 0/90/180/270 yaw and conservative in between; that is
	   the same limitation the box trace in PM_PlayerTrace carries, and it is stated
	   in both places rather than pretended away.

	   The other is the +=24.  Its intent is right -- the player's box must be
	   entirely inside the aperture before we dare carve the world -- but the corner
	   it wants is the box's MINIMUM support along the plane normal, and both 24 and
	   the commented-out DotProduct(nearest, ...) use the MAXIMUM.  With a hull of
	   (-16,-16,0)..(16,16,72) the difference is the whole disagreement: the maximum
	   reading demands the player's feet be 28..44 above the door's centre, i.e.
	   floating; the minimum reading gives feet in [floor, floor+16], which is a
	   72-tall player fitting inside an 88-tall hole and is the answer.

	   Note that only the four SIDE planes move.  Planes 0 and 1 stay anchored on
	   portal->origin: that pair is the portal PLANE, the thing camera_transform
	   mirrors about and the thing whose crossing sets trace->entnum below, and it
	   does not belong to the box. */
	for (plane = 0; plane < 3; plane++)
	{
		center[plane] = portal->origin[plane] + (portal->mins[plane] + portal->maxs[plane]) * 0.5;
		halfsize[plane] = (portal->maxs[plane] - portal->mins[plane]) * 0.5;
		if (halfsize[plane] < 0)
			halfsize[plane] = 0;
	}
	hwidth  = fabs(planes[3][0])*halfsize[0] + fabs(planes[3][1])*halfsize[1] + fabs(planes[3][2])*halfsize[2];
	hheight = fabs(planes[5][0])*halfsize[0] + fabs(planes[5][1])*halfsize[1] + fabs(planes[5][2])*halfsize[2];
	if (hwidth <= 0 || hheight <= 0)
	{	//no usable bounds. fall back to the pre-P203 fixed window so anything that
		//reached here without them behaves exactly as it did before.
		hwidth = hheight = 128/2;
		VectorCopy(portal->origin, center);
	}

	planes[0][3] = DotProduct(portal->origin, planes[0]) - (4.0/32);
	planes[1][3] = DotProduct(portal->origin, planes[1]) - (4.0/32);	//an epsilon beyond the portal. this needs to cover funny angle differences
	planes[2][3] = DotProduct(center, planes[2]) - hwidth;
	planes[3][3] = DotProduct(center, planes[3]) - hwidth;
	planes[4][3] = DotProduct(center, planes[4]) - hheight;
	planes[5][3] = DotProduct(center, planes[5]) - hheight;

	/* ---- FTESurf Patch 216: keep the opening's OWN bottom edge ----
	   The loop below offsets every side plane by some corner of the player's box,
	   which is right for deciding whether they line up with the hole and wrong for
	   deciding where the hole is.  This is the line the doorway FLOOR sits on, and
	   no amount of player height may be allowed to move it. */
	aperturebottom = planes[5][3];

	//if we're actually inside the csg region
	for (plane = 0; plane < 6; plane++)
	{
		vec3_t corner;
		float d = DotProduct(worldpos, planes[plane]);
		int k;
		if (!plane)
		{	//front plane gets further away with size: the box need only TOUCH the
			//portal plane to count, so expand by its support along this normal.
			for (k = 0; k < 3; k++)
				corner[k] = (planes[plane][k]>=0)?trmax[k]:trmin[k];
			planes[plane][3] -= DotProduct(corner, planes[plane]);
		}
		else if (plane>1)
		{	/* ---- FTESurf Patch 206: the side planes test the box's CENTRE ----

			   P203 used the box's MINIMUM support here, which spells "the whole
			   player must be inside the aperture".  That reading is defensible
			   and it is unplayable: an 88-tall opening minus a 72-tall standing
			   hull leaves SIXTEEN UNITS of freedom, sitting at the bottom of the
			   opening.  On surf_kitsune, whose apertures span z 860..948, the
			   accepted band for the feet was [860, 876].  The ducked hull is 54
			   tall, which widens it to [860, 894] -- and that is the entire
			   reason the doors could only be entered by crouch-jumping, which is
			   how the user found it.  P203's own test fixture set a +50 u/s lift
			   specifically so the feet would arrive at ~870, the middle of the
			   band, and its comment says so: the measurement was built to land
			   inside the thing it was measuring.

			   The centre is the honest test.  Your middle is in the hole, so you
			   are going through the hole -- which is also what Source asks
			   (WorldSpaceCenter against the door rectangle).  The band becomes
			   [824, 912]: the whole doorway.

			   It is safe to let half the box sit inside the wall during a
			   crossing because the EXIT carves symmetrically -- this same
			   function is applied to the arrival from PM_TestPlayerPosition's
			   fallback below, so a landing that is partly inside the exit wall is
			   accepted for exactly the same reason the departure was.

			   Planes 0 and 1 are deliberately untouched: that pair is the portal
			   PLANE, the thing camera_transform mirrors about, and it does not
			   belong to the box. */
			for (k = 0; k < 3; k++)
				corner[k] = (trmin[k] + trmax[k]) * 0.5;
			planes[plane][3] -= DotProduct(corner, planes[plane]);
		}
		if (d - planes[plane][3] >= 0)
			continue;	//endpos is inside
		else
		{
			PM_PortalCSGReport(entnum, plane, d - planes[plane][3]);
			return;		//end is already outside
		}
	}
	//yup, we're inside, the trace shouldn't end where it actually did
	bestfrac = 1;
	hitplane = -1;
	for (plane = 0; plane < 6; plane++)
	{
		float ds = DotProduct(start, planes[plane]) - planes[plane][3];
		float de = DotProduct(end, planes[plane]) - planes[plane][3];
		float frac;
		if (ds >= 0 && de < 0)
		{
			frac = (ds - (1/32.0)) / (ds - de);
			if (frac < bestfrac)
			{
				if (frac < 0)
					frac = 0;
				hitplane = plane;
				bestfrac = frac;
				/* No VectorInterpolate here any more.  It was writing trace->endpos
				   from inside the search, so the `return` below could leave a trace
				   whose endpos had moved but whose fraction had not -- and the value
				   was overwritten unconditionally further down anyway.  The winning
				   fraction is all this loop needs to produce. */
			}
		}
	}

	/* ---- FTESurf Patch 216, part 1: nothing BELOW the opening is forgiven ----

	   Patch 206 tests the side planes against the box's CENTRE, so plane 5 accepts a
	   player whose centre is above the opening's bottom edge -- their feet 36 units
	   under it, for a (-16,-16,0)..(16,16,72) hull.  On surf_kitsune's portal_hub_exit
	   the opening's bottom edge IS the doorway floor (aperture z 96..272, floor 96),
	   so that reading says "a player buried up to 36 units in the floor is in the
	   doorway".  It is what the clear below then forgives, and forgiving it is exactly
	   why a player who ends up under the floor STAYS there: PM_TestPlayerPosition's
	   point test (:856-869) hands this function a start == end probe, we clear
	   allsolid, the position is reported VALID, and so PMSrc_CheckStuck never runs and
	   never pushes them out.  The user's words for that state, in order across two
	   builds: "stuck in the floor 64~ units, you can jump out", then "fall 30~ units
	   down, now you slide around and I can't jump out".  36 is the number the centre
	   offset predicts and it is the middle of those two reports.

	   The centre reading is right for LINING UP with a hole and wrong for standing in
	   one.  A hole in a wall has a bottom edge; below that edge is not the doorway,
	   it is the floor.  So the box's own lowest corner is tested against the
	   rectangle's own bottom edge, with no slop in either.

	   This costs normal play nothing, and that is checkable rather than hopeful: a
	   player standing in the doorway is in open air, their world trace is not
	   allsolid, and the point test above never calls this function at all.  The gate
	   can only ever fire on a position that is ALREADY inside solid brushwork, where
	   the honest answer is "no, get out". */
	for (plane = 0; plane < 3; plane++)
		botcorner[plane] = (planes[5][plane] >= 0) ? trmin[plane] : trmax[plane];
	feetdist = DotProduct(worldpos, planes[5]) + DotProduct(botcorner, planes[5]);
	if (feetdist < aperturebottom - (1/32.0))
	{
		PM_PortalCSGReport(entnum, 5, feetdist - aperturebottom);
		return;		//under the opening: that is the floor, not the wall it is set into
	}

	/* The player's box is in the aperture, so the wall it straddles does not count
	   FOR THEM.  This half is the actual contract: it is what PM_TestPlayerPosition's
	   point test (start == end, which reaches no plane crossing at all and so lands
	   here with hitplane -1) needs in order to accept a landing that is partly inside
	   the exit wall, and it is what stops a player who arrives that way from being
	   pinned by PM_SlideMove's startsolid bail (pmove.c:268).  It is unconditional
	   below the gate above and deliberately above both gates below -- forgiving the
	   wall and travelling through it are separate permissions, which is the whole
	   lesson of 214 and 215. */
	trace->startsolid = trace->allsolid = false;

	/* ---- FTESurf Patch 214: only a trace HEADING INTO the aperture may be
	   elongated ----

	   ELONGATING IS NOT THE SAME PERMISSION AS IGNORING THE WALL, and conflating the
	   two deleted the floor.

	   Patch 212 corrected the aperture from 88 to 176 units, which also doubled the
	   collision box the window above is derived from -- and Patch 206 tests the four
	   side planes against the player box's CENTRE, so the bottom plane sits at
	   `door_z - halfheight - 36` for a (-16,-16,0)..(16,16,72) hull.  On
	   surf_kitsune's portal_hub_exit (origin z 184) that moved from z 104 to z 60,
	   and the doorway floor is at z 96.  Measured, not inferred: the engine's own
	   PM_PortalCSGReport logged the `below it` edge 58 times in the pre-fix run and
	   every one of them at exactly 8.0 units, i.e. feet at 96 against a plane at 104.
	   After the size fix it logs none, because the plane is 36 units UNDER the floor.

	   So the player's downward ground trace began ending INSIDE the window.  It
	   crosses none of the six planes, so hitplane is -1 and bestfrac is 1, and the
	   code below then set fraction = 1 -- deleting the floor out from under anyone
	   standing in a doorway.  They sink to about z 60, where the window refuses again
	   and the brush closes around them, and PM_TestPlayerPosition's portal fallback
	   clears allsolid so the embedded position is judged VALID.  The user's words
	   were "you can fall inbetween them and get stuck in the floor, you can jump out".

	   WHAT THIS GATE IS NOT.  The first attempt gated on `hitplane == 1`, reasoning
	   that a crossing must leave through the portal plane because that is the only
	   case that stamps entnum below.  That is true of the FINAL tick and false of
	   every tick before it, and it broke the crossing outright -- measured, not
	   argued: with it, walking the hub door left the timer at 0:00.000 and the player
	   in the hub, where the build without it reached stage 1 at 0:03.840.  A player
	   walking at ~3 units a tick first stops with their box FACE on the plane, which
	   is 16 units of origin travel short of it; the whole point of the elongation is
	   to carry those 16 units, and across that stretch the trace crosses no plane at
	   all.  hitplane -1 is the normal case for a crossing, not an anomaly.

	   The honest discriminator is DIRECTION.  The carve exists to let a trace pass
	   THROUGH the aperture, so it applies to a trace trying to go through it: one
	   whose motion has a component along the portal's inward normal.  A walk into the
	   doorway has one.  A downward ground trace -- the thing that decides whether you
	   are standing on anything -- is perpendicular to it and has none, so the floor
	   is left exactly as the world reported it.  PM_TestPlayerPosition's point test
	   (start == end) has none either, and does not need one: it wants the solidity
	   clear above, which is unconditional and stays that way.

	   This is deliberately independent of aperture size: it touches no plane
	   position, no half-extent, and no QC.  Shrinking the portal back would "fix" the
	   fall too, and would be wrong -- 176 is the measured width of the doorway. */
	VectorSubtract(end, start, movedir);
	if (DotProduct(movedir, planes[1]) >= 0)
		return;		//not heading into the aperture: nothing to pass through

	/* ---- FTESurf Patch 216, part 2: only the wall the portal is SET INTO ----

	   PATCH 214 WAS NECESSARY AND NOT SUFFICIENT, and the hole in it is worth stating
	   plainly because it is the same mistake one level down.  214 asks "is this trace
	   heading into the aperture?", which excludes the purely vertical ground probe and
	   nothing else.  A player who is walking forward AND falling -- the exact state
	   you are in for the tick you arrive at a doorway, and every tick of a surf ramp
	   -- has a movement vector with BOTH components, so 214 waves it through and the
	   elongation below then sets fraction to bestfrac, which is 1, which discards the
	   floor that same trace had just found.  Direction says where you are going; it
	   says nothing about what you hit.

	   What you hit is recorded, and it is the honest discriminator.  The carve exists
	   to remove one specific surface: the face of the wall the portal is mounted in,
	   whose normal is parallel to the portal's own.  A floor, a ceiling or a side wall
	   that happens to lie inside the window is not that surface and must stay solid.
	   0.5 is +-60 degrees, which covers a wall that is not perfectly axial without
	   admitting anything that could be stood on (Quake calls 0.7 the walkable limit).

	   startsolid is exempt because there is no surface to ask about -- an embedded
	   trace reports no plane.  That case is the arrival-inside-the-exit-wall unstick,
	   it is already bounded by part 1 above (which refuses anything under the
	   opening), and leaving it carved is what keeps a legitimate arrival from being
	   welded in place. */
	if (!trace->startsolid && fabs(DotProduct(trace->plane.normal, planes[1])) < 0.5)
		return;		//we hit a floor/ceiling/side wall, not the portal's own wall

	//if we cross the front of the portal, don't shorten the trace, that will artificially clip us
	if (hitplane == 0 && trace->fraction > bestfrac)
		return;
	//okay, elongate to clip to the portal hole properly.
	trace->fraction = bestfrac;
	VectorInterpolate(start, bestfrac, end, trace->endpos);

	if (hitplane >= 0)
	{
		VectorCopy(planes[hitplane], trace->plane.normal);
		trace->plane.dist = planes[hitplane][3];
		if (hitplane == 1)
			trace->entnum = entnum;
	}
}

/*
================
PM_TestPlayerPosition

Returns false if the given player position is not valid (in solid)
================
*/
qboolean PM_TestPlayerPosition (vec3_t pos, qboolean ignoreportals)
{
	int			i, j;
	physent_t	*pe;
	vec3_t		mins, maxs;
	hull_t		*hull;
	trace_t		trace;
	int			csged = false;

	for (i=0 ; i< pmove.numphysent ; i++)
	{
		pe = &pmove.physents[i];

		if (pe->info == pmove.skipent)
			continue;

		if (pe->nonsolid)
			continue;

		if (pe->forcecontentsmask && !(pe->forcecontentsmask & MASK_PLAYERSOLID))
			continue;

	// get the clipping hull
		if (pe->isportal)
		{
			if (ignoreportals)
				continue;
			//if the trace ended up inside a portal region, then its not valid.
			if (pe->model)
			{
				if (!PM_TransformedHullCheck (pe->model, PE_FRAMESTATE, pos, pos, vec3_origin, vec3_origin, &trace, pe->origin, pe->angles, pe->scale))
					continue;
				if (trace.allsolid)
					return false;
			}
			else
			{
				hull = PM_HullForBox (pe->mins, pe->maxs);
				VectorSubtract(pos, pe->origin, mins);
				if (Q1BSP_HullPointContents(hull, mins) & MASK_PLAYERSOLID)
					return false;
			}
		}
		else
		{
			if (pe->model)
			{
				if (!PM_TransformedHullCheck (pe->model, PE_FRAMESTATE, pos, pos, pmove.player_mins, pmove.player_maxs, &trace, pe->origin, pe->angles, pe->scale))
					continue;
				if (trace.allsolid)
				{
					for (j = i+1; j < pmove.numphysent && trace.allsolid; j++)
					{
						pe = &pmove.physents[j];
						if (pe->isportal)
							PM_PortalCSG(pe, j, pmove.player_mins, pmove.player_maxs, pos, pos, &trace);
					}
					if (trace.allsolid)
						return false;
					csged = true;
				}
			}
			else
			{
				VectorSubtract (pe->mins, pmove.player_maxs, mins);
				VectorSubtract (pe->maxs, pmove.player_mins, maxs);
				hull = PM_HullForBox (mins, maxs);
				VectorSubtract(pos, pe->origin, mins);

				if (Q1BSP_HullPointContents(hull, mins) & MASK_PLAYERSOLID)
					return false;
			}
		}
	}

	if (!csged && !ignoreportals)
	{
		//the point the player is returned to if the portal dissipates
		pmove.safeorigin_known = true;
		VectorCopy (pmove.origin, pmove.safeorigin);
	}

	return true;
}

/*
================
PM_AnyPortals

FTESurf Patch 206.  Is there a portal anywhere in this move's physent set?

pm_source.c has one portal-aware trace site and seventeen plain ones, and the
plain ones are not merely unaware -- they are actively fooled.  PM_PortalCSG
ELONGATES a trace through the wall when it accepts, and stamps trace->entnum
only when the move leaves through the portal plane itself (hitplane == 1).  A
move that enters the carved region and leaves through a SIDE plane therefore
comes back looking like an ordinary world trace that happens to have travelled
through solid brushwork.  Cache a trace like that and reuse it later and the
player is moved through the wall with no transform ever running.

There is no per-trace flag to test for that, so the honest question is the
coarse one: does this map have portals at all?  1290 of the library's 1308 maps
answer no and take a single early-out, so the conservative behaviour on the
other 18 costs nothing anywhere else.
================
*/
qboolean PM_AnyPortals (void)
{
	int i;
	for (i = 0; i < pmove.numphysent; i++)
		if (pmove.physents[i].isportal)
			return true;
	return false;
}

/*
================
PM_PlayerTrace
================
*/
trace_t PM_PlayerTrace (vec3_t start, vec3_t end, unsigned int solidmask)
{
	trace_t		trace, total;
	int			i, j;
	physent_t	*pe;

// fill in a default trace
	memset (&total, 0, sizeof(trace_t));
	total.fraction = 1;
	total.entnum = -1;
	VectorCopy (end, total.endpos);

	for (i=0 ; i< pmove.numphysent ; i++)
	{
		pe = &pmove.physents[i];

		if (pe->nonsolid)
			continue;
		if (pe->info == pmove.skipent)
			continue;
		if (pe->forcecontentsmask && !(pe->forcecontentsmask & solidmask))
			continue;

		if (pe->isportal)
		{
			/* FTESurf Patch 203: the portal test now comes FIRST, and splits on
			   whether it has a model rather than being reachable only when it
			   does.  A modelless SOLID_PORTAL used to fall into the plain box
			   branch below, which never calls PM_PortalCSG -- so the one entity
			   type whose whole purpose is to let you through a wall was the one
			   that never carved it. */

			//make sure we don't hit the world if we're inside the portal
			PM_PortalCSG(pe, i, pmove.player_mins, pmove.player_maxs, start, end, &total);

			if (!pe->model || pe->model->loadstate != MLS_LOADED)
			{
				/* P203: point-size against the entity's OWN box, unexpanded, and
				   with no rotation.

				   Point-size because that is what SOLID_PORTAL means (pr_common.h
				   :711, "traces always use point-size") and what the model-backed
				   branch below already does.  It is also the only reading that
				   works: the traversal fires on trace.entnum, and PM_PortalTransform
				   then mirrors trace.endpos.  Expand the box by the player hull and
				   the trace stops ~16 units short of the plane, so the point handed
				   to the transform has not crossed yet and comes out on the WRONG
				   SIDE of the exit -- which is exactly how the QC teleport this
				   replaces used to leave players inside the wall.

				   No rotation because pe->mins/maxs came from QC setsize and are
				   already world-axis-aligned; PM_PortalCSG derives the aperture from
				   the same box the same way.  Exact at 0/90/180/270 yaw, which is
				   every door in surf_kitsune, and conservative in between -- an
				   off-axis door fires slightly early at its corners. */
				PM_HullForBox (pe->mins, pe->maxs);
				if (!PM_TransformedHullCheck (NULL, NULL, start, end, vec3_origin, vec3_origin, &trace, pe->origin, vec3_origin, 1))
					continue;
			}
			else
			{
				// trace a line through the apropriate clipping hull
				if (!PM_TransformedHullCheck (pe->model, PE_FRAMESTATE, start, end, vec3_origin, vec3_origin, &trace, pe->origin, pe->angles, pe->scale))
					continue;
			}
		}
		else if (!pe->model || pe->model->loadstate != MLS_LOADED)
		{
			vec3_t mins, maxs;

			VectorSubtract (pe->mins, pmove.player_maxs, mins);
			VectorSubtract (pe->maxs, pmove.player_mins, maxs);
			PM_HullForBox (mins, maxs);

			// trace a line through the apropriate clipping hull
			if (!PM_TransformedHullCheck (NULL, NULL, start, end, pmove.player_mins, pmove.player_maxs, &trace, pe->origin, pe->angles, pe->scale))
				continue;
		}
		else
		{
			// trace a line through the apropriate clipping hull
			if (!PM_TransformedHullCheck (pe->model, PE_FRAMESTATE, start, end, pmove.player_mins, pmove.player_maxs, &trace, pe->origin, pe->angles, pe->scale))
				continue;

			if (trace.allsolid)
			{
				for (j = i+1; j < pmove.numphysent && trace.allsolid; j++)
				{
					pe = &pmove.physents[j];
					if (pe->isportal)
						PM_PortalCSG(pe, j, pmove.player_mins, pmove.player_maxs, start, end, &trace);
				}
				pe = &pmove.physents[i];
			}
		}

		if (trace.allsolid)
			trace.startsolid = true;
		if (trace.startsolid && pe->isportal)
			trace.startsolid = false;
//		if (trace.startsolid)
//			trace.fraction = 0;

	// did we clip the move?
		if (trace.fraction < total.fraction || (trace.startsolid && !total.startsolid))
		{
			// fix trace up by the offset
			total = trace;
			total.entnum = i;
		}
	}

//	//this is needed to avoid *2 friction. some id bug.
	if (total.startsolid)
		total.fraction = 0;
	return total;
}

//for use outside the pmove code. lame, but works.
trace_t PM_TraceLine (vec3_t start, vec3_t end)
{
	VectorClear(pmove.player_mins);
	VectorClear(pmove.player_maxs);
	return PM_PlayerTrace(start, end, MASK_PLAYERSOLID);
}
