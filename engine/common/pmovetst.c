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
static void PM_PortalCSG(physent_t *portal, int entnum, float *trmin, float *trmax, vec3_t start, vec3_t end, trace_t *trace)
{
	vec4_t planes[6];	//far, near, right, left, up, down
	int plane;
	vec3_t worldpos;
	float portalradius = 128;
	int hitplane = -1;
	float bestfrac;
	//only run this code if we impacted on the portal's parent.
	if (trace->fraction == 1 && !trace->startsolid)
		return;
	if (!portalradius)
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

	portalradius/=2;
	planes[0][3] = DotProduct(portal->origin, planes[0]) - (4.0/32);
	planes[1][3] = DotProduct(portal->origin, planes[1]) - (4.0/32);	//an epsilon beyond the portal. this needs to cover funny angle differences
	planes[2][3] = DotProduct(portal->origin, planes[2]) - portalradius;
	planes[3][3] = DotProduct(portal->origin, planes[3]) - portalradius;
	planes[4][3] = DotProduct(portal->origin, planes[4]) - portalradius;
	planes[5][3] = DotProduct(portal->origin, planes[5]) - portalradius;

	//if we're actually inside the csg region
	for (plane = 0; plane < 6; plane++)
	{
		vec3_t nearest;
		float d = DotProduct(worldpos, planes[plane]);
		int k;
		for (k = 0; k < 3; k++)
			nearest[k] = (planes[plane][k]>=0)?trmax[k]:trmin[k];
		if (!plane)	//front plane gets further away with side
			planes[plane][3] -= DotProduct(nearest, planes[plane]);
		else if (plane>1)	//side planes get nearer with size
			planes[plane][3] += 24;//+= DotProduct(nearest, planes[plane]);
		if (d - planes[plane][3] >= 0)
			continue;	//endpos is inside
		else
			return;		//end is already outside
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
				VectorInterpolate(start, frac, end, trace->endpos);
			}
		}
	}
	trace->startsolid = trace->allsolid = false;
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

		if (!pe->model || pe->model->loadstate != MLS_LOADED)
		{
			vec3_t mins, maxs;

			VectorSubtract (pe->mins, pmove.player_maxs, mins);
			VectorSubtract (pe->maxs, pmove.player_mins, maxs);
			PM_HullForBox (mins, maxs);

			// trace a line through the apropriate clipping hull
			if (!PM_TransformedHullCheck (NULL, NULL, start, end, pmove.player_mins, pmove.player_maxs, &trace, pe->origin, pe->angles, pe->scale))
				continue;
		}
		else if (pe->isportal)
		{
			//make sure we don't hit the world if we're inside the portal
			PM_PortalCSG(pe, i, pmove.player_mins, pmove.player_maxs, start, end, &total);

			// trace a line through the apropriate clipping hull
			if (!PM_TransformedHullCheck (pe->model, PE_FRAMESTATE, start, end, vec3_origin, vec3_origin, &trace, pe->origin, pe->angles, pe->scale))
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
