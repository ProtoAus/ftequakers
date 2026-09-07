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
// world.c -- world query functions

#include "quakedef.h"
#include "pr_common.h"

#if defined(CSQC_DAT) || !defined(CLIENTONLY)
/*

entities never clip against themselves, or their owner

line of sight checks trace->crosscontent, but bullets don't

*/

#define SOLID_ISTRIGGER(solid) ((solid)==SOLID_TRIGGER||(solid)==SOLID_BSPTRIGGER||(solid)==SOLID_LADDER)

size_t areagridsequence;	//used to avoid poking the same ent twice.

extern cvar_t sv_compatiblehulls;
extern cvar_t sv_gameplayfix_linknonsolid;

typedef struct
{
	vec3_t		boxmins, boxmaxs;// enclose the test object along entire move
	float		*mins, *maxs;	// size of the moving object
	vec3_t		mins2, maxs2;	// size when clipping against mosnters
	float		*start, *end;
	trace_t		trace;
	int			type;
	int			hitcontentsmask;
	wedict_t		*passedict;
#ifdef Q2SERVER
	q2edict_t	*q2passedict;
#endif
	int			hullnum;
	qboolean	capsule;
} moveclip_t;

static unsigned int World_ContentsOfAllLinks (world_t *w, vec3_t pos);
/*
===============================================================================

HULL BOXES

===============================================================================
*/


static	hull_t		box_hull;
static	mclipnode_t	box_clipnodes[6];
static	mplane_t	box_planes[6];

/*
===================
SV_InitBoxHull

Set up the planes and clipnodes so that the six floats of a bounding box
can just be stored out and get a proper hull_t structure.
===================
*/
static void World_InitBoxHull (void)
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
SV_HullForBox

To keep everything totally uniform, bounding boxes are turned into small
BSP trees instead of being compared directly.
===================
*/
hull_t	*World_HullForBox (vec3_t mins, vec3_t maxs)
{
	box_planes[0].dist = maxs[0];
	box_planes[1].dist = mins[0];
	box_planes[2].dist = maxs[1];
	box_planes[3].dist = mins[1];
	box_planes[4].dist = maxs[2];
	box_planes[5].dist = mins[2];

	return &box_hull;
}

static model_t mod_capsule;
qboolean World_BoxTrace(struct model_s *model, int hulloverride, int frame, vec3_t axis[3], vec3_t p1, vec3_t p2, vec3_t mins, vec3_t maxs, unsigned int against, struct trace_s *trace)
{
	hull_t *hull = &box_hull;

	//bbox vs bbox
	//capsule vs bbox (NYI)

	memset (trace, 0, sizeof(trace_t));
	trace->fraction = 1;
	trace->allsolid = true;

	VectorCopy (p2, trace->endpos);
	return Q1BSP_RecursiveHullCheck (hull, hull->firstclipnode, p1, p2, against, trace);
}
qboolean World_CapsuleTrace(struct model_s *model, int hulloverride, const framestate_t *framestate, const vec3_t axis[3], const vec3_t p1, const vec3_t p2, const vec3_t mins, const vec3_t maxs, qboolean capsule, unsigned int against, struct trace_s *trace)
{
	//bbox vs capsule (NYI)
	//capsule vs capsule (NYI)

	memset (trace, 0, sizeof(trace_t));
	trace->fraction = 1;
	trace->allsolid = false;
	VectorCopy(p2, trace->endpos);

	if (capsule)
	{	//capsule vs capsule.
		//no orientation support on either (ignore axis)
		float sr = ((model->maxs[0]-model->mins[0]) + (model->maxs[1]-model->mins[1]))/4.0;
		float sh = (model->maxs[2]-model->mins[2]) - sr*2;
		float mr = ((maxs[0]-mins[0]) + (maxs[1]-mins[1]))/4.0;
		float mh = (maxs[2]-mins[2]) - mr*2;
		vec3_t sup = {0, 0, 1};
		vec3_t dir, sright;
		vec4_t nearestplane;
		float d1, d2;
		vec3_t nearestpoint;
		float neardist;

		//expand the static capsule's height+radius by the mover's height+radius, so that its point+capsule instead
		sr += mr;
		sh += mh;

		VectorSubtract(p1, p2, dir);
		d2=VectorNormalize(dir);
		CrossProduct(sup, dir, sright);
		VectorNormalize(sright);
		CrossProduct(sup, sright, nearestplane);
		VectorNormalize(nearestplane);
		nearestplane[3] = DotProduct(vec3_origin, nearestplane);	//capsule is at 0 0 0
		d1 = DotProduct(nearestplane, p1) - nearestplane[3];
		d2 = DotProduct(nearestplane, p2) - nearestplane[3];
		d2 = -d1 /(d1+d2);
		VectorInterpolate(p1, d2, p2, nearestpoint);

		neardist = VectorLength(nearestpoint);
		if (neardist < sr)
		{
			float x, y, oz, nz;
			//sqrt(h*h-(x*x+y*y))=z
			//change the hypotenuse from the messed up value to the actual radius
			//and update z to match the changed h
			x = DotProduct(sup, nearestpoint) - 0;
			y = DotProduct(sright, nearestpoint) - 0;
			oz = DotProduct(nearestplane, nearestpoint) - nearestplane[3];
			nz = sqrt(sr*sr - (x*x+y*y));

			VectorMA(nearestpoint, nz-oz, dir, trace->endpos);
			trace->fraction = 0;
		}
	}
	return false;
}
model_t *World_CapsuleForBox(const vec3_t mins, const vec3_t maxs)
{
	VectorCopy(mins, mod_capsule.mins);
	VectorCopy(maxs, mod_capsule.maxs);
	mod_capsule.funcs.NativeTrace = World_CapsuleTrace;
	return &mod_capsule;
}
/*
===============================================================================

ENTITY AREA CHECKING

===============================================================================
*/

#if defined(Q2SERVER) || !defined(USEAREAGRID)
/*
===============
SV_CreateAreaNode

===============
*/
static areanode_t *World_CreateAreaNode (world_t *w, int depth, vec3_t mins, vec3_t maxs)
{
	areanode_t	*anode;
	vec3_t		size;
	vec3_t		mins1, maxs1, mins2, maxs2;

	anode = &w->areanodes[w->numareanodes];
	w->numareanodes++;

	ClearLink (&anode->edicts);

	VectorSubtract (maxs, mins, size);

	if (depth == w->areanodedepth || (size[0] < 512 && size[1] < 512))
	{
		anode->axis = -1;
		anode->children[0] = anode->children[1] = NULL;
		return anode;
	}

	if (size[0] > size[1])
		anode->axis = 0;
	else
		anode->axis = 1;

	anode->dist = 0.5 * (maxs[anode->axis] + mins[anode->axis]);
	VectorCopy (mins, mins1);	
	VectorCopy (mins, mins2);	
	VectorCopy (maxs, maxs1);	
	VectorCopy (maxs, maxs2);	

	maxs1[anode->axis] = mins2[anode->axis] = anode->dist;

	anode->children[0] = World_CreateAreaNode (w, depth+1, mins2, maxs2);
	anode->children[1] = World_CreateAreaNode (w, depth+1, mins1, maxs1);

	return anode;
}

/*
===============
SV_ClearWorld

===============
*/
void World_ClearWorld_Nodes (world_t *w, qboolean relink)
{
#if !defined(USEAREAGRID)
	int i;
	wedict_t *ent;
#endif
	int maxdepth;
	vec3_t mins, maxs;
	if (w->worldmodel)
	{
		VectorCopy(w->worldmodel->mins, mins);
		VectorCopy(w->worldmodel->maxs, maxs);
	}
	else
	{
		VectorSet(mins, -4096, -4096, -4096);
		VectorSet(maxs, 4096, 4096, 4096);
	}

	World_InitBoxHull ();

#if !defined(USEAREAGRID)
	memset (&w->portallist, 0, sizeof(w->portallist));
	ClearLink (&w->portallist.edicts);
	w->portallist.axis = -1;
#endif

	maxdepth = 8;

	if (!w->areanodes || w->areanodedepth != maxdepth)
	{
		Z_Free(w->areanodes);
		w->areanodedepth = maxdepth;
		w->areanodes = Z_Malloc(sizeof(*w->areanodes) * (pow(2, w->areanodedepth+1)-1));
	}
	else
		memset (w->areanodes, 0, sizeof(*w->areanodes)*w->numareanodes);
	w->numareanodes = 0;
	World_CreateAreaNode (w, 0, mins, maxs);

#if !defined(USEAREAGRID)
	if (relink)
	{
		for (i=0 ; i<w->num_edicts ; i++)
		{
			ent = WEDICT_NUM_PB(w->progs, i);
			if (!ent)
				continue;
			ent->area.prev = ent->area.next = NULL;
			if (ED_ISFREE(ent))
				continue;
			World_LinkEdict (w, ent, false);	// relink ents so touch functions continue to work.
		}
	}
#endif
}
#endif

#ifdef USEAREAGRID
static void World_ClearWorld_AreaGrid (world_t *w, qboolean relink)
{
	int numareas = 1;
	int i, j;
	wedict_t *ent;
	vec3_t mins, maxs, size;
	if (w->worldmodel)
	{
		VectorCopy(w->worldmodel->mins, mins);
		VectorCopy(w->worldmodel->maxs, maxs);
	}
	else
	{
		VectorSet(mins, -4096, -4096, -4096);
		VectorSet(maxs, 4096, 4096, 4096);
	}
	Vector2Set(w->gridsize, 128, 128);
	for (i = 0; i < 2; i++)
	{
		size[i] = maxs[i] - mins[i];
		size[i] /= w->gridsize[i];
	//enforce a minimum grid size, so things don't end up getting added to every single node
		if (size[i] < 128)
		{
			mins[i] -= (128-size[i])/2 * w->gridsize[i];
			size[i] = 128;
		}
		w->gridscale[i] = size[i];
		w->gridbias[i] = -mins[i];

		numareas *= w->gridsize[i];
	}

	World_InitBoxHull ();

	if (w->gridareas)
		memset (w->gridareas, 0, sizeof(*w->gridareas)*numareas);
	else
		w->gridareas = Z_Malloc(sizeof(*w->gridareas)*numareas);

	for (i = 0; i < numareas; i++)
		ClearLink (&w->gridareas[i].l);
	ClearLink (&w->jumboarea.l);
	ClearLink (&w->portallist.l);


	if (relink)
	{
		for (i=0 ; i<w->num_edicts ; i++)
		{
			ent = WEDICT_NUM_PB(w->progs, i);
			if (!ent)
				continue;
			for (j = 0; j < countof(ent->gridareas); j++)
			{
				if (!ent->gridareas[j].l.prev)
					break;		// not linked in anywhere
				ClearLink(&ent->gridareas[j].l);
			}
			if (ED_ISFREE(ent))
				continue;
			World_LinkEdict (w, ent, false);	// relink ents so touch functions continue to work.
		}
	}
}
#endif

void World_ClearWorld (world_t *w, qboolean relink)
{
#ifdef Q2SERVER
	if (w == &sv.world && svs.gametype == GT_QUAKE2)
		World_ClearWorld_Nodes(w, relink);
	else
#endif
	{
#ifdef USEAREAGRID
		World_ClearWorld_AreaGrid(w, relink);
#else
		World_ClearWorld_Nodes(w, relink);
#endif
	}
}

#if !defined(USEAREAGRID)

/*
===============
SV_UnlinkEdict

===============
*/
void World_UnlinkEdict (wedict_t *ent)
{
	if (!ent->area.prev)
		return;		// not linked in anywhere
	RemoveLink (&ent->area);
	ent->area.prev = ent->area.next = NULL;
}

/*
====================
SV_TouchLinks
====================
*/
void World_TouchLinks (world_t *w, wedict_t *ent, areanode_t *node)
{
	static wedict_t *nodelinks[256];	//all this means is that any more than this will not touch. probably you won't have that many valid triggers
	link_t		*l, *next;
	wedict_t		*touch;

	int linkcount = 0, ln;

	//work out who they are first.
	for (l = node->edicts.next ; l != &node->edicts ; l = next)
	{
		if (linkcount == countof(nodelinks))
			break;
		next = l->next;
		touch = EDICT_FROM_AREA(l);
		if (touch == ent)
			continue;

		if (!touch->v->touch || !SOLID_ISTRIGGER(touch->v->solid))
			continue;

		if (ent->v->absmin[0] > touch->v->absmax[0]
		|| ent->v->absmin[1] > touch->v->absmax[1]
		|| ent->v->absmin[2] > touch->v->absmax[2]
		|| ent->v->absmax[0] < touch->v->absmin[0]
		|| ent->v->absmax[1] < touch->v->absmin[1]
		|| ent->v->absmax[2] < touch->v->absmin[2] )
			continue;

		if (!((int)ent->xv->dimension_solid & (int)touch->xv->dimension_hit))
			continue;

		nodelinks[linkcount++] = touch;
	}

	for (ln = 0; ln < linkcount; ln++)
	{
		touch = nodelinks[ln];

		//make sure nothing moved it away
		if (ED_ISFREE(touch))
			continue;
		if (!touch->v->touch || !SOLID_ISTRIGGER(touch->v->solid))
			continue;

		if (ent->v->absmin[0] > touch->v->absmax[0]
		|| ent->v->absmin[1] > touch->v->absmax[1]
		|| ent->v->absmin[2] > touch->v->absmax[2]
		|| ent->v->absmax[0] < touch->v->absmin[0]
		|| ent->v->absmax[1] < touch->v->absmin[1]
		|| ent->v->absmax[2] < touch->v->absmin[2] )
			continue;

		if (!((int)ent->xv->dimension_solid & (int)touch->xv->dimension_hit))	//didn't change did it?...
			continue;

		w->Event_Touch(w, touch, ent, NULL);

		if (ED_ISFREE(ent))
			break;
	}


// recurse down both sides
	if (node->axis == -1 || ED_ISFREE(ent))
		return;
	
	if (ent->v->absmax[node->axis] > node->dist)
		World_TouchLinks (w, ent, node->children[0]);
	if (ent->v->absmin[node->axis] < node->dist)
		World_TouchLinks (w, ent, node->children[1]);
}
#endif

/*
===============
SV_LinkEdict

===============
*/
void QDECL World_LinkEdict (world_t *w, wedict_t *ent, qboolean touch_triggers)
{
	pvec_t *mins;
	pvec_t *maxs;
	int solid;
	qboolean rotatedbox;	//nettest: does this entity's box turn with .angles?

#ifdef USEAREAGRID
	World_UnlinkEdict (ent);	// unlink from old position
#else
	areanode_t	*node;
	
	if (ent->area.prev)
		World_UnlinkEdict (ent);	// unlink from old position
#endif
	
	if (ent == w->edicts)
		return;		// don't add the world

	if (ED_ISFREE(ent))
		return;

	mins = ent->v->mins;
	maxs = ent->v->maxs;
	if (!ent->v->solid)
		ent->solidsize = ES_SOLID_BSP;
	else
	{
		model_t *mod;
		if (ent->v->solid == SOLID_BSP)
			mod = w->Get_CModel(w, ent->v->modelindex);
		else
			mod = NULL;
		if (mod && mod->type == mod_brush)
		{
			mins = mod->mins;
			maxs = mod->maxs;
			ent->solidsize = ES_SOLID_BSP;
		}
		else
			ent->solidsize = COM_EncodeSize(mins, maxs);
	}

// set the abs box
	solid = ent->v->solid;
	rotatedbox = (solid == SOLID_BSP||solid == SOLID_BSPTRIGGER||solid == SOLID_PHYSICS_BOX||solid == SOLID_PHYSICS_TRIMESH);

	//nettest: a NON-SOLID brush model still needs its box rotated, for PVS.
	//
	//The list above is the set of solid types whose COLLISION broadphase needs the
	//expansion, but absmin/absmax is also what FindTouchedLeafs() below derives
	//pvsinfo from - so a rotating brush entity that happens to be non-solid got a
	//PVS box that never turned with it.  Worse, the box is origin+mins/maxs and a
	//brush model's bounds are not centred on its origin: They Hunger's headlight
	//brush *26 spans x -450..-150, a box that does not even contain its own origin.
	//So the entity was tested for visibility against a slab bolted to one side of
	//the truck and pointing a fixed direction no matter which way the truck faced -
	//it passed PVS from places it should not and drew through the level, while the
	//truck body (SOLID_BSP, and therefore rotated here) behaved correctly.  That
	//asymmetry is exactly what the func_tracktrain TT_PASSABLE flag produces.
	//
	//Only the PVS box is affected: SOLID_NOT returns before the area-grid link
	//below, so this cannot change collision. Gated on modelindex+angles first so
	//the Get_CModel call is only paid by rotating model entities, and inline brush
	//models are already resident as part of the world.
	if (!rotatedbox && solid == SOLID_NOT && ent->v->modelindex &&
		(ent->v->angles[0] || ent->v->angles[1] || ent->v->angles[2]) )
	{
		model_t *bmod = w->Get_CModel(w, ent->v->modelindex);
		if (bmod && bmod->type == mod_brush)
			rotatedbox = true;
	}

	if (rotatedbox &&
	(ent->v->angles[0] || ent->v->angles[1] || ent->v->angles[2]) )
	{	// expand for rotation
		// SOLID_PHYSICS_BOX added by the OBB patch: a ROTATED physics box needs the
		// enlarged broadphase AABB here, or its tilted corners get area-grid culled
		// before World_OBBTrace's oriented narrowphase ever runs. -- FTE patch (#3)
		// SOLID_PHYSICS_TRIMESH added by Patch 53: Patch 52 routes its swept-box (player)
		// path through World_OBBTrace too, so a rotated trimesh prop needs the SAME
		// expansion — without it the player only collided inside the un-rotated AABB
		// (a long van rotated 90deg lost its length). Point/bullet traces use the mesh
		// and don't need it, but this conservative AABB is harmless for them.
		//nettest Patch 59: EXACT rotated AABB. Take the world min/max of all 8 box corners
		//rotated by the entity angles, using the SAME axis convention as World_OBBTrace /
		//World_HullTrace so the broadphase exactly wraps the oriented narrowphase. The old q2
		//"max half-extent cube" UNDER-covered a long box turned ~22-45deg -- its corners stuck
		//out past the cube, so the player fell through the rotated van there. Tight AND any angle.
		//nettest Patch 64: the alias-mesh props (PHYSICS_BOX/TRIMESH) now build their narrowphase
		//basis with r_meshpitch (AngleVectorsMesh, matching the render) — the broadphase MUST use
		//the SAME basis per solid type or a pitched van falls through. Brush (BSP) stays raw.
		int i, k;
		vec3_t axis[3];
		if (solid == SOLID_PHYSICS_BOX || solid == SOLID_PHYSICS_TRIMESH)
			AngleVectorsMesh(ent->v->angles, axis[0], axis[1], axis[2]);	//alias mesh
		else
			AngleVectors(ent->v->angles, axis[0], axis[1], axis[2]);		//brush
		VectorNegate(axis[1], axis[1]);
		for (k = 0; k < 3; k++)
		{
			float lo = 0, hi = 0;
			for (i = 0; i < 3; i++)
			{	//max/min of (corner . axis[i][k]) over the box picks maxs/mins by the sign
				float a = axis[i][k];
				if (a > 0) { hi += maxs[i]*a; lo += mins[i]*a; }
				else       { hi += mins[i]*a; lo += maxs[i]*a; }
			}
			ent->v->absmin[k] = ent->v->origin[k] + lo - 0.1;
			ent->v->absmax[k] = ent->v->origin[k] + hi + 0.1;
		}
	}
	else
	{
		VectorAdd (ent->v->origin, mins, ent->v->absmin);
		VectorAdd (ent->v->origin, maxs, ent->v->absmax);
	}

	//some fancy things can mean the ent's aabb is larger than its collision box.
#ifdef USERBE
//	if (ent->rbe.body.body)
//		w->rbe->ExpandBodyAABB(w->rbe, &ent->rbe.body, ent->v->absmin, env->v->absmax);
#endif
#ifdef SKELETALOBJECTS
	if (ent->xv->skeletonindex)
		skel_updateentbounds(w, ent);
#endif

//
// to make items easier to pick up and allow them to be grabbed off
// of shelves, the abs sizes are expanded
//
	if ((int)ent->v->flags & FL_ITEM)
	{
		ent->v->absmin[0] -= 15;
		ent->v->absmin[1] -= 15;
		ent->v->absmin[2] -= 1;
		ent->v->absmax[0] += 15;
		ent->v->absmax[1] += 15;
		ent->v->absmax[2] += 1;
	}
	else
	{	// because movement is clipped an epsilon away from an actual edge,
		// we must fully check even when bounding boxes don't quite touch
		ent->v->absmin[0] -= 1;
		ent->v->absmin[1] -= 1;
		ent->v->absmin[2] -= 1;
		ent->v->absmax[0] += 1;
		ent->v->absmax[1] += 1;
		ent->v->absmax[2] += 1;
	}
	
// link to PVS leafs
	if (w->worldmodel && w->worldmodel->loadstate == MLS_LOADED && w->worldmodel->funcs.FindTouchedLeafs)
	{
		w->worldmodel->funcs.FindTouchedLeafs(w->worldmodel, &ent->pvsinfo, ent->v->absmin, ent->v->absmax);
	}

	if (ent->v->solid == SOLID_NOT && !sv_gameplayfix_linknonsolid.ival)
		return;

#ifdef USEAREAGRID
	// find the first node that the ent's box crosses
	if (ent->v->solid == SOLID_PORTAL)
	{
		ent->gridareas[0].ed = ent;
		InsertLinkBefore (&ent->gridareas[0].l, &w->portallist.l);
	}
	else
	{
		int ming[2], maxg[2], g[2], ga;
		CALCAREAGRIDBOUNDS(w, ent->v->absmin, ent->v->absmax);

		if ((maxg[0]-ming[0])*(maxg[1]-ming[1]) > countof(ent->gridareas)				//entity is too large to fit in our grid.
			|| ming[0]<0||ming[1]<0||maxg[0]>=w->gridsize[0]||maxg[1]>=w->gridsize[1])	//entity crosses the boundary of the world.
		{	//shove it in the overflow
			ent->gridareas[0].ed = ent;
			InsertLinkBefore (&ent->gridareas[0].l, &w->jumboarea.l);
		}
		else
		{
			for (ga = 0, g[0] = ming[0]; g[0] < maxg[0]; g[0]++)
				for (    g[1] = ming[1]; g[1] < maxg[1]; g[1]++, ga++)
				{
					ent->gridareas[ga].ed = ent;
					InsertLinkBefore (&ent->gridareas[ga].l, &w->gridareas[g[0] + g[1]*w->gridsize[0]].l);
				}
		}
	}
#else
// find the first node that the ent's box crosses
	if (ent->v->solid == SOLID_PORTAL)
		node = &w->portallist;
	else
	{
		node = w->areanodes;
		while (1)
		{
			if (node->axis == -1)
				break;
			if (ent->v->absmin[node->axis] > node->dist)
				node = node->children[0];
			else if (ent->v->absmax[node->axis] < node->dist)
				node = node->children[1];
			else
				break;		// crosses the node
		}
	}
	
// link it in	

	InsertLinkBefore (&ent->area, &node->edicts);
#endif
	
// if touch_triggers, touch all entities at this node and decend for more
	if (touch_triggers && ent->v->solid != SOLID_NOT)
		World_TouchAllLinks (w, ent);
}


#ifdef Q2SERVER
void VARGS WorldQ2_UnlinkEdict(world_t *w, q2edict_t *ent)
{
	if (!ent->area.prev)
		return;		// not linked in anywhere
	RemoveLink (&ent->area);
	ent->area.prev = ent->area.next = NULL;
}

void VARGS WorldQ2_LinkEdict(world_t *w, q2edict_t *ent)
{
	areanode_t	*node;

	if (ent->area.prev)
		WorldQ2_UnlinkEdict (w, ent);	// unlink from old position
		
	if (ent == ge->edicts)
		return;		// don't add the world

	if (!ent->inuse)
		return;

	// set the size
	VectorSubtract (ent->maxs, ent->mins, ent->size);
	
	// encode the size into the entity_state for client prediction
	if (ent->solid == Q2SOLID_BBOX && !(ent->svflags & SVF_DEADMONSTER))
	{	// assume that x/y are equal and symetric
		ent->s.solid = COM_EncodeSize(ent->mins, ent->maxs);
	}
	else if (ent->solid == Q2SOLID_BSP)
	{
		ent->s.solid = ES_SOLID_BSP;		// a solid_bbox will never create this value
	}
	else
		ent->s.solid = 0;

	// set the abs box
	if (ent->solid == Q2SOLID_BSP && 
	(ent->s.angles[0] || ent->s.angles[1] || ent->s.angles[2]) )
	{	// expand for rotation
		float		max, v;
		int			i;

		max = 0;
		for (i=0 ; i<3 ; i++)
		{
			v =fabs( ent->mins[i]);
			if (v > max)
				max = v;
			v =fabs( ent->maxs[i]);
			if (v > max)
				max = v;
		}
		for (i=0 ; i<3 ; i++)
		{
			ent->absmin[i] = ent->s.origin[i] - max;
			ent->absmax[i] = ent->s.origin[i] + max;
		}
	}
	else
	{	// normal
		VectorAdd (ent->s.origin, ent->mins, ent->absmin);	
		VectorAdd (ent->s.origin, ent->maxs, ent->absmax);
	}

	// because movement is clipped an epsilon away from an actual edge,
	// we must fully check even when bounding boxes don't quite touch
	ent->absmin[0] -= 1;
	ent->absmin[1] -= 1;
	ent->absmin[2] -= 1;
	ent->absmax[0] += 1;
	ent->absmax[1] += 1;
	ent->absmax[2] += 1;

// link to PVS leafs
	{
		pvscache_t cache;
		w->worldmodel->funcs.FindTouchedLeafs(w->worldmodel, &cache, ent->absmin, ent->absmax);

		//evilness: copy into the q2 state (we don't have anywhere else to store it, and there's a chance that the gamecode will care).
		ent->num_clusters = cache.num_leafs;
		if (ent->num_clusters > (int)countof(ent->clusternums))
			ent->num_clusters = (int)countof(ent->clusternums);
		memcpy(ent->clusternums, cache.leafnums, min(sizeof(ent->clusternums), sizeof(cache.leafnums)));
		ent->headnode = cache.headnode;
		ent->areanum = cache.areanum;
		ent->areanum2 = cache.areanum2;
	}

	// if first time, make sure old_origin is valid
	if (!ent->linkcount)
	{
		VectorCopy (ent->s.origin, ent->s.old_origin);
	}
	ent->linkcount++;

	if (ent->solid == Q2SOLID_NOT)
		return;

// find the first node that the ent's box crosses
	node = w->areanodes;
	while (1)
	{
		if (node->axis == -1)
			break;
		if (ent->absmin[node->axis] > node->dist)
			node = node->children[0];
		else if (ent->absmax[node->axis] < node->dist)
			node = node->children[1];
		else
			break;		// crosses the node
	}

	// link it in	
	InsertLinkBefore (&ent->area, &node->edicts);
}
#endif




/*
===============================================================================

POINT TESTING IN HULLS

===============================================================================
*/

/*
==================
SV_PointContents

==================
*/
int World_PointContentsWorldOnly (world_t *w, vec3_t p)
{
	return w->worldmodel->funcs.PointContents(w->worldmodel, NULL, p);
}

int World_PointContentsAllBSPs (world_t *w, vec3_t p)
{
	int c = w->worldmodel->funcs.PointContents(w->worldmodel, NULL, p);
	c |= World_ContentsOfAllLinks(w, p);
	return c;
}

//===========================================================================

/*
============
SV_TestEntityPosition

A small wrapper around SV_BoxInSolidEntity that never clips against the
supplied entity.
============
*/
wedict_t	*World_TestEntityPosition (world_t *w, wedict_t *ent)
{
	trace_t	trace;

	trace = World_Move (w, ent->v->origin, ent->v->mins, ent->v->maxs, ent->v->origin, ((ent->v->solid == SOLID_NOT || ent->v->solid == SOLID_TRIGGER || ent->v->solid == SOLID_BSPTRIGGER)?MOVE_NOMONSTERS:0), ent);
	
	if (trace.startsolid || trace.allsolid)
		return trace.ent?trace.ent:w->edicts;
		
	return NULL;
}

//wrapper function. Rotates the start and end positions around the angles if needed.
//qboolean TransformedHullCheck (hull_t *hull, vec3_t start, vec3_t end, trace_t *trace, vec3_t angles)
qboolean World_TransformedTrace (struct model_s *model, int hulloverride, framestate_t *framestate, vec3_t start, vec3_t end, vec3_t mins, vec3_t maxs, qboolean capsule, struct trace_s *trace, vec3_t origin, vec3_t angles, unsigned int hitcontentsmask)
{
	vec3_t		start_l, end_l;
	vec3_t		axis[3];
	qboolean	result;

	memset (trace, 0, sizeof(trace_t));
	trace->fraction = 1;
	trace->allsolid = true;
	trace->startsolid = false;
	trace->inopen = true;	//probably wrong...
	VectorCopy (end, trace->endpos);

	if (IS_NAN(end[0]) || IS_NAN(end[1]) || IS_NAN(end[2]))
	{
		Con_DPrintf("Nan in traceline\n");
		return false;
	}

	// don't rotate non bsp ents. Too small to bother.
	if (model && model->loadstate == MLS_LOADED && model->funcs.NativeTrace)	//nettest: the CSQC (world.c:2283) and hitmodel trace paths null-check NativeTrace but this server worldmodel path didn't — a model reported MLS_LOADED before its deferred BIH build set NativeTrace would call a NULL fn ptr; the box-hull else-branch below is a safe fallback
	{
		VectorSubtract (start, origin, start_l);
		VectorSubtract (end, origin, end_l);

		if (angles[0] || angles[1] || angles[2])
		{
			if (model->type == mod_alias)
			{
				axis[2][0] = angles[0] * r_meshpitch.value;
				axis[2][1] = angles[1];
				axis[2][2] = angles[2] * r_meshroll.value;
				AngleVectors (axis[2], axis[0], axis[1], axis[2]);
			}
			else
				AngleVectors (angles, axis[0], axis[1], axis[2]);
			VectorNegate(axis[1], axis[1]);
			result = model->funcs.NativeTrace (model, hulloverride, framestate, axis, start_l, end_l, mins, maxs, capsule, hitcontentsmask, trace);
		}
		else
		{
			result = model->funcs.NativeTrace (model, hulloverride, framestate, NULL, start_l, end_l, mins, maxs, capsule, hitcontentsmask, trace);
		}

		VectorAdd (trace->endpos, origin, trace->endpos);
	}
	else if (hitcontentsmask & FTECONTENTS_BODY)
	{
		hull_t *hull = &box_hull;

		VectorSubtract (start, origin, start_l);
		VectorSubtract (end, origin, end_l);
		VectorCopy (end_l, trace->endpos);
		result = Q1BSP_RecursiveHullCheck (hull, hull->firstclipnode, start_l, end_l, MASK_PLAYERSOLID, trace);
		VectorAdd (trace->endpos, origin, trace->endpos);
		if (trace->contents)
			trace->contents = FTECONTENTS_BODY;
	}
	else
		result = false;

	return result;
}

//Oriented-box trace. Clips a swept player box (mins..maxs, axis-aligned in WORLD
//space) against entity 'ent's bounding box ROTATED by 'eang'. This is the box
//analogue of the BSP/alias rotated trace in World_TransformedTrace: rotate the
//trace into the box's local frame, clip against the (player-expanded) axis-aligned
//box hull, then rotate the result plane normal back to world space. box_hull stays
//axis-aligned the whole time (we rotate the TRACE, never the hull planes), so the
//shared static box_hull is not corrupted for the next entity in the clip loop.
//Lets a tumbling SOLID_PHYSICS_BOX prop (e.g. the filing cabinet) be walked on /
//shot as its true oriented shape — smooth like a convex hull, not the per-triangle
//mesh trace and not the axis-aligned AABB. -- FTE patch (ENGINE_PATCHES.md #3)
static void World_OBBTrace (wedict_t *ent, vec3_t start, vec3_t end, vec3_t mins, vec3_t maxs, vec3_t eorg, vec3_t eang, unsigned int hitcontentsmask, trace_t *trace)
{
	vec3_t	axis[3], iaxis[3];
	vec3_t	phalf, pcenter, boxmins, boxmaxs;
	vec3_t	start_l, end_l, tmp, norm;
	hull_t	*hull;
	int		i;

	memset (trace, 0, sizeof(*trace));
	trace->fraction = 1;
	trace->allsolid = true;
	trace->startsolid = false;
	trace->inopen = true;
	VectorCopy (end, trace->endpos);

	//physics boxes present BODY contents only, exactly like the axis-aligned path.
	if (!(hitcontentsmask & FTECONTENTS_BODY))
		return;
	if (IS_NAN(end[0]) || IS_NAN(end[1]) || IS_NAN(end[2]))
		return;

	//nettest Patch 64: build the basis the SAME way the renderer does for an alias/IQM model
	//(AngleVectorsMesh applies r_meshpitch to pitch + r_meshroll to roll) so the oriented box
	//matches the VISIBLE model when the prop is pitched/rolled. SOLID_PHYSICS_BOX is always an
	//alias mesh. At r_meshpitch 1 this is identical to raw AngleVectors. (Earlier "raw is
	//intentional" comment was a mis-diagnosis: World_OBBTrace consumes axis world->local exactly
	//like the confirmed-correct bullet path Mod_Trace, so it needs the same meshpitch axis.)
	AngleVectorsMesh (eang, axis[0], axis[1], axis[2]);
	VectorNegate (axis[1], axis[1]);

	//Minkowski-expand the prop box by the player box, measured in the prop's LOCAL
	//frame (the world-axis player box projects to a conservative local AABB under
	//rotation). Tracing the player ORIGIN ray against the expanded box gives the
	//swept collision. For a point trace (bullets, mins==maxs==0) the expansion is
	//zero, so it is an exact ray-vs-oriented-box test.
	for (i = 0; i < 3; i++)
	{
		phalf[i]   = (maxs[i] - mins[i]) * 0.5;
		pcenter[i] = (maxs[i] + mins[i]) * 0.5;
	}
	for (i = 0; i < 3; i++)
	{
		float h = fabs(axis[i][0])*phalf[0] + fabs(axis[i][1])*phalf[1] + fabs(axis[i][2])*phalf[2];
		float c = DotProduct(axis[i], pcenter);
		boxmins[i] = ent->v->mins[i] - (c + h);
		boxmaxs[i] = ent->v->maxs[i] - (c - h);
	}
	hull = World_HullForBox (boxmins, boxmaxs);

	//Rotate the trace into the prop local frame (relative to its origin).
	VectorSubtract (start, eorg, tmp);
	start_l[0] = DotProduct(tmp, axis[0]);
	start_l[1] = DotProduct(tmp, axis[1]);
	start_l[2] = DotProduct(tmp, axis[2]);
	VectorSubtract (end, eorg, tmp);
	end_l[0] = DotProduct(tmp, axis[0]);
	end_l[1] = DotProduct(tmp, axis[1]);
	end_l[2] = DotProduct(tmp, axis[2]);

	Q1BSP_RecursiveHullCheck (hull, hull->firstclipnode, start_l, end_l, MASK_PLAYERSOLID, trace);

	if (trace->fraction == 1)
		VectorCopy (end, trace->endpos);
	else
	{
		//rotate the hit normal back to world space; interpolate endpos along the
		//world-space ray (matches q1bsp's rotated path).
		Matrix3x3_RM_Invert_Simple ((void *)axis, iaxis);
		VectorCopy (trace->plane.normal, norm);
		trace->plane.normal[0] = DotProduct(norm, iaxis[0]);
		trace->plane.normal[1] = DotProduct(norm, iaxis[1]);
		trace->plane.normal[2] = DotProduct(norm, iaxis[2]);
		VectorInterpolate (start, trace->fraction, end, trace->endpos);
	}
	if (trace->contents)
		trace->contents = FTECONTENTS_BODY;
}

//nettest Patch 56: TRUE convex-hull trace — the smooth, mesh-shaped analogue of
//World_OBBTrace. Instead of the entity AABB it clips the swept player box against the
//model's convex HULL (incremental QuickHull built from the verts at load, com_mesh.c).
//Each hull face plane (model space) is rotated into world by the entity angles, scaled,
//shifted by the origin, pushed out by the box (nearest corner, like CM_ClipBoxToBrush),
//then an enter/leave-fraction loop finds first contact. Convex + watertight -> leak-free,
//no seam stutter, O(numhullplanes). Requires model->numhullplanes>=4 (checked by caller).
//nettest Patch 61: clip the swept box against ONE convex piece's planes (model space, rotated
//by 'axis', scaled, origin-shifted). Outputs that piece's entry fraction (-1 = no valid entry),
//hit normal, and start/exit flags. Returns false if the box CLEANLY MISSES the piece (stays in
//front of some plane the whole sweep) — the caller skips it.
static qboolean World_HullClipOne (int numplanes, vec4_t *planes, const vec3_t axis[3], const vec3_t eorg, float scale,
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
		const float *pl = planes[j];	//.xyz = model-space outward unit normal, .w = dist
		nw[0] = pl[0]*axis[0][0] + pl[1]*axis[1][0] + pl[2]*axis[2][0];
		nw[1] = pl[0]*axis[0][1] + pl[1]*axis[1][1] + pl[2]*axis[2][1];
		nw[2] = pl[0]*axis[0][2] + pl[1]*axis[1][2] + pl[2]*axis[2][2];
		dw = pl[3] * scale + DotProduct (eorg, nw);

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
		{	//entering
			f = d1 / (d1 - d2);
			if (f > enterfrac)
			{	//nettest Patch 63: track raw enter (for the union compare) + a back-off in the
				//plane-NORMAL direction (like CM_ClipBoxToBrush) so a tangential slide keeps a
				//fixed clearance off the surface instead of resting on it.
				enterfrac = f;
				nearfrac = (d1 - 0.03125) / (d1 - d2);	//DIST_EPSILON back-off, normal direction
				VectorCopy (nw, hitnorm);
			}
		}
		else
		{	//leaving
			f = d1 / (d1 - d2);
			if (f < leavefrac) leavefrac = f;
		}
	}
	*out_startout = startout;
	*out_getout = getout;
	if (enterfrac <= leavefrac)	//valid entry only if enter<=leave
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

//Trace a swept player box against a prop's convex collision. 'usedecomp' true (sv_prop_collision
//3 + model->numhulls>0) clips against the per-submesh DECOMPOSITION (a set of convex pieces whose
//union approximates a CONCAVE shape); else against the single all-verts hull (mode 2). The union
//is: the player stops at the NEAREST piece it enters; it is startsolid if inside ANY piece; it
//passes freely through concave gaps where no piece is entered. Convex + watertight -> leak-free.
static void World_HullTrace (wedict_t *ent, model_t *model, qboolean usedecomp, vec3_t start, vec3_t end, vec3_t mins, vec3_t maxs, vec3_t eorg, vec3_t eang, float scale, unsigned int hitcontentsmask, trace_t *trace)
{
	vec3_t	axis[3], hitnorm, besthitnorm;
	vec3_t	lmin, lmax;	//nettest Patch 65: swept player box in the prop LOCAL frame, for the per-piece cull
	float	bestenter = 1, bestnear = 1;
	qboolean anystart = false, anyall = false, hashit = false;
	int		h, nh, k;

	memset (trace, 0, sizeof(*trace));
	trace->fraction = 1;
	trace->truefraction = 1;	//nettest Patch 61: match the client PM_HullTrace (was left 0)
	trace->inopen = true;
	VectorCopy (end, trace->endpos);

	if (!(hitcontentsmask & FTECONTENTS_BODY))
		return;
	if (IS_NAN(end[0]) || IS_NAN(end[1]) || IS_NAN(end[2]))
		return;

	//nettest Patch 64: build the basis like the renderer (AngleVectorsMesh = r_meshpitch on pitch,
	//r_meshroll on roll) so the hull matches the VISIBLE alias/IQM model when pitched/rolled.
	//SOLID_PHYSICS_TRIMESH is always an alias mesh. Identical to raw AngleVectors at r_meshpitch 1.
	AngleVectorsMesh (eang, axis[0], axis[1], axis[2]);
	VectorNegate (axis[1], axis[1]);

	//nettest Patch 65: project the swept player box into the prop LOCAL frame ONCE (model_pt =
	//axis.(world-eorg), matching World_HullClipOne). Used to skip far decomposition pieces below.
	{
		vec3_t ds, de, pcenter, phalf;
		for (k = 0; k < 3; k++) { pcenter[k] = (maxs[k]+mins[k])*0.5f; phalf[k] = (maxs[k]-mins[k])*0.5f; }
		VectorSubtract (start, eorg, ds);
		VectorSubtract (end,   eorg, de);
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
		{	//per-piece AABB cull: skip a piece the swept box can't reach (piece AABB is unscaled
			//model space -> compare vs scale*bounds). Only skips clean-misses -> result-neutral.
			const convhull_t *ch = &model->convhulls[h];
			if (lmin[0] > ch->maxs[0]*scale || lmax[0] < ch->mins[0]*scale ||
			    lmin[1] > ch->maxs[1]*scale || lmax[1] < ch->mins[1]*scale ||
			    lmin[2] > ch->maxs[2]*scale || lmax[2] < ch->mins[2]*scale)
				continue;
			np = ch->numplanes; pl = ch->planes;
		}
		else           { np = model->numhullplanes;      pl = model->hullplanes; }
		if (np < 4)
			continue;	//empty/degenerate piece
		if (!World_HullClipOne (np, pl, axis, eorg, scale, start, end, mins, maxs, &enterfrac, &nearfrac, hitnorm, &startout, &getout))
			continue;	//clean miss of this piece
		if (!startout)
		{	//started inside this piece -> embedded in the (union) solid
			anystart = true;
			if (!getout) anyall = true;
		}
		else if (enterfrac > -1)
		{	//a real contact — keep the piece the box enters FIRST (min raw enterfrac); its
			//nearfrac (normal back-off) is the fraction the player actually moves to.
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
	{	//started inside the solid (any piece)
		trace->startsolid = true;
		if (anyall)
			trace->allsolid = true;
		return;
	}
	if (hashit)
	{	//nettest Patch 63: fraction = the normal-direction back-off (player rests a fixed
		//clearance off the surface -> smooth slide); truefraction = the true contact point.
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

/*
==================
SV_ClipMoveToEntity

Handles selection or creation of a clipping hull, and offseting (and
eventually rotation) of the end points
==================
*/
static trace_t World_ClipMoveToEntity (world_t *w, wedict_t *ent, vec3_t eorg, vec3_t eang, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int hullnum, qboolean hitmodel, qboolean capsule, unsigned int hitcontentsmask)	//hullnum overrides min/max for q1 style bsps
{
	trace_t		trace;
	model_t		*model;
	int mdlidx = ent->v->modelindex;
	framestate_t framestate;
	int solid = ent->v->solid;
	//nettest Patch 55/61: convex-hull (sv_prop_collision 2) / decomposition (3) swept-box routing.
	qboolean usehull = false;
	qboolean usedecomp = false;
	model_t *hullmodel = NULL;

// get the clipping hull
	if ((solid == SOLID_BSP || solid == SOLID_BSPTRIGGER || solid == SOLID_PORTAL) && mdlidx)
	{
		model = w->Get_CModel(w, mdlidx);
		if (!model || !model->funcs.PointContents)
		{
//			Host_Error("SOLID_BSP with non bsp model (classname: %s)", PR_GetString(w->progs, ent->v->classname));
			model = NULL;
		}
	}
	else if (solid == SOLID_PHYSICS_TRIMESH && mdlidx)
	{
		//nettest Patch 54/55: bullets (point/line) ALWAYS hit the exact per-triangle
		//mesh. A SWEPT BOX (player) picks its collision SHAPE from sv_prop_collision:
		//  0 = oriented box (World_OBBTrace), 1 = per-triangle mesh (fixed
		//  Mod_Trace_Trisoup swept-box), 2 = convex hull (World_HullTrace, k-DOP).
		//So the prop's player collision matches the model, not a too-small box.
		static cvar_t *propcol;
		if (!propcol)
			propcol = Cvar_Get("sv_prop_collision", "2", CVAR_SERVERINFO,
				"Static-prop (SOLID_PHYSICS_TRIMESH) player collision shape: 0=oriented box, 1=per-triangle mesh (exact but slow/leaky), 2=convex hull (smooth, fast, watertight; default), 3=convex decomposition (per-submesh hulls -> concave-aware on multi-part models; single-mesh props fall back to the single hull). Bullets always use the exact mesh.");

		model = w->Get_CModel(w, mdlidx);
		if (!model || !model->funcs.NativeTrace)
			model = NULL;
		if (model && (maxs[0] > mins[0] || maxs[1] > mins[1] || maxs[2] > mins[2]))
		{	//swept box == player movement
			int cm = propcol ? propcol->ival : 2;
			if (cm == 3 && (model->numhulls > 0 || model->numhullplanes >= 4))
			{	//convex decomposition (per-submesh); single-mesh props fall back to the single hull
				usehull = true; usedecomp = (model->numhulls > 0); hullmodel = model; model = NULL;
			}
			else if (cm == 2 && model->numhullplanes >= 4)
			{	//single convex hull
				usehull = true; hullmodel = model; model = NULL;
			}
			else if (cm == 1)
				;	//keep model → per-triangle mesh (fixed swept-box trace)
			else
				model = NULL;	//0, or hull unavailable → oriented box
		}
		//point/line (bullet): model kept → exact mesh, unchanged.
	}
	else
		model = NULL;

	if (!model || model->loadstate != MLS_LOADED)
	{
		vec3_t boxmins, boxmaxs;
		model = NULL;
		VectorSubtract (ent->v->mins, maxs, boxmins);
		VectorSubtract (ent->v->maxs, mins, boxmaxs);

//		if (ent->xv->geomtype == GEOMTYPE_CAPSULE && !hitmodel)
//			model = World_CapsuleForBox(boxmins, boxmaxs);
//		else
			World_HullForBox(boxmins, boxmaxs);
	}

	w->Get_FrameState(w, ent, &framestate);

// trace a line through the apropriate clipping hull
	if (solid == SOLID_PORTAL)
	{
		//solid_portal cares only about origins and as such has no mins/max
		World_TransformedTrace(model, 0, &framestate, start, end, vec3_origin, vec3_origin, capsule, &trace, eorg, eang, hitcontentsmask);
		if (trace.startsolid)	//portals should not block traces. this prevents infinite looping
			trace.startsolid = false;
		hitmodel = false;
	}
	else if (solid == SOLID_CORPSE && w->usesolidcorpse)
		goto scorpse;
	else if (ent->v->skin < 0)
	{	//if forcedcontents is set, then ALL brushes in this model are forced to the specified contents value.
		//we achive this by tracing against ALL then forcing it after.
		int forcedcontents;
		safeswitch((enum q1contents_e)(int)ent->v->skin)
		{
		case Q1CONTENTS_EMPTY:			forcedcontents = FTECONTENTS_EMPTY;			break;
		case Q1CONTENTS_SOLID:			forcedcontents = FTECONTENTS_SOLID;			break;
		case Q1CONTENTS_WATER:			forcedcontents = FTECONTENTS_WATER;			break;
		case Q1CONTENTS_SLIME:			forcedcontents = FTECONTENTS_SLIME;			break;
		case Q1CONTENTS_LAVA:			forcedcontents = FTECONTENTS_LAVA;			break;
		case Q1CONTENTS_SKY:			forcedcontents = FTECONTENTS_SKY;			break;
		case Q1CONTENTS_LADDER:			forcedcontents = FTECONTENTS_LADDER;		break;
		case HLCONTENTS_CLIP:			forcedcontents = FTECONTENTS_PLAYERCLIP|FTECONTENTS_MONSTERCLIP;break;
		case HLCONTENTS_CURRENT_0:		forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_0;		break;
		case HLCONTENTS_CURRENT_90:		forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_90;		break;
		case HLCONTENTS_CURRENT_180:	forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_180;		break;
		case HLCONTENTS_CURRENT_270:	forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_270;		break;
		case HLCONTENTS_CURRENT_UP:		forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_UP;		break;
		case HLCONTENTS_CURRENT_DOWN:	forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_DOWN;		break;
		case HLCONTENTS_TRANS:			forcedcontents = FTECONTENTS_EMPTY;			break;
		case Q1CONTENTS_MONSTERCLIP:	forcedcontents = FTECONTENTS_MONSTERCLIP;	break;
		case Q1CONTENTS_PLAYERCLIP:		forcedcontents = FTECONTENTS_PLAYERCLIP;	break;
		case Q1CONTENTS_CORPSE:scorpse: forcedcontents = FTECONTENTS_CORPSE;		break;
		safedefault:					forcedcontents = 0;							break;
		}
		//nettest: A CLIP BRUSH DOES NOT EXIST IN HULL 0.
		//
		//GoldSrc compiles CONTENTS_CLIP (func_monsterclip, func_playerclip, the CLIP
		//texture) into the COLLISION hulls only.  Hull 0 - the point hull that bullets,
		//line-of-sight checks, gunfire and every other traceline run through - contains
		//no clip geometry at all, which is why a Half-Life monster penned in by a
		//monsterclip can still shoot straight through it.
		//
		//A forced-contents entity has no hulls of its own to differ: the same brush
		//answers every trace shape identically, so the distinction has to be made here
		//or a monsterclip volume blocks a monster's bullets and its sight as well as its
		//feet.  A POINT trace (mins == maxs, necessarily the origin) is exactly the hull-0
		//case, so that is the test.
		//
		//Scoped to a forced contents that is NOTHING BUT clip: HLCONTENTS_CLIP and the
		//two Q1 clip values are the only ones affected, and a brush forcing water, solid,
		//ladder or corpse keeps blocking point traces exactly as before.
		{
			const int clipbits = FTECONTENTS_PLAYERCLIP|FTECONTENTS_MONSTERCLIP;
			if ((forcedcontents & clipbits) && !(forcedcontents & ~clipbits)
				&& mins[0] == maxs[0] && mins[1] == maxs[1] && mins[2] == maxs[2])
				forcedcontents = 0;	//falls into the clean-miss arm below
		}
		if (hitcontentsmask & forcedcontents)
		{
			World_TransformedTrace(model, hullnum, &framestate, start, end, mins, maxs, capsule, &trace, eorg, eang, ~0u);
			if (trace.contents)
				trace.contents = forcedcontents;
		}
		else
		{
			memset (&trace, 0, sizeof(trace_t));
			trace.fraction = trace.truefraction = 1;
			trace.allsolid = false;
			trace.startsolid = false;
			trace.inopen = true;	//probably wrong...
			VectorCopy (end, trace.endpos);
		}
	}
	else if (usehull)
	{
		//nettest Patch 55/61: convex-hull (mode 2) / decomposition (mode 3) swept-box player
		//trace — smooth mesh-shaped collision against the model's hull(s).
		float sc = ent->xv->scale; if (sc <= 0) sc = 1;
		World_HullTrace(ent, hullmodel, usedecomp, start, end, mins, maxs, eorg, eang, sc, hitcontentsmask, &trace);
	}
	else if ((solid == SOLID_PHYSICS_BOX || solid == SOLID_PHYSICS_TRIMESH) && !model && (eang[0] || eang[1] || eang[2]))
	{
		//Oriented physics box: collide against the box ROTATED by the entity
		//angles instead of its axis-aligned AABB. This is what lets a tumbling
		//SOLID_PHYSICS_BOX prop (the filing cabinet) be walked on / shot as its
		//real oriented shape. Scoped to SOLID_PHYSICS_BOX so SOLID_BBOX (items,
		//players, etc.) keeps its cheap axis-aligned behaviour. -- FTE patch.
		//nettest: ALSO the swept-box player path for SOLID_PHYSICS_TRIMESH (whose model
		//was NULL'd above for box sweeps) — point traces keep the per-triangle mesh, the
		//player box gets this smooth oriented bbox.
		World_OBBTrace(ent, start, end, mins, maxs, eorg, eang, hitcontentsmask, &trace);
	}
	else if (solid == SOLID_PHYSICS_TRIMESH && model && ent->xv->scale > 0 && ent->xv->scale != 1)
	{
		//nettest: scale the per-triangle model trace by the entity's .scale so a scaled
		//IQM/MD3 prop collides at its VISUAL size.  Uniform scale commutes with the
		//origin-translate + angle-rotate inside World_TransformedTrace, so we trace the
		//(unscaled) mesh against the ray + moving box pre-scaled about eorg by 1/scale,
		//then un-scale the endpos back.  trace.fraction + the plane normal are
		//scale-invariant under uniform scale, so they need no fix-up.
		float sc = ent->xv->scale, inv = 1.0f/sc;
		vec3_t ss, se, sm, sM;
		VectorSubtract(start, eorg, ss); VectorMA(eorg, inv, ss, ss);
		VectorSubtract(end,   eorg, se); VectorMA(eorg, inv, se, se);
		VectorScale(mins, inv, sm); VectorScale(maxs, inv, sM);
		World_TransformedTrace(model, hullnum, &framestate, ss, se, sm, sM, capsule, &trace, eorg, eang, hitcontentsmask);
		VectorSubtract(trace.endpos, eorg, trace.endpos); VectorMA(eorg, sc, trace.endpos, trace.endpos);
	}
	else
		World_TransformedTrace(model, hullnum, &framestate, start, end, mins, maxs, capsule, &trace, eorg, eang, hitcontentsmask);

// if using hitmodel, we know it hit the bounding box, so try a proper trace now.
	if (hitmodel && (trace.fraction != 1 || trace.startsolid) && !model)
	{
		//okay, we hit the bbox
		model = w->Get_CModel(w, mdlidx);

		if (model && model->funcs.NativeTrace && model->loadstate == MLS_LOADED)
		{
			//do the second trace, using the actual mesh.
			World_TransformedTrace(model, hullnum, &framestate, start, end, mins, maxs, capsule, &trace, eorg, eang, hitcontentsmask);
		}
	}

// did we clip the move?
	if (trace.fraction < 1 || trace.startsolid || trace.allsolid)
		trace.ent = ent;

	return trace;
}

#ifdef USEAREAGRID

/*
================
SV_AreaEdicts
================
*/
int World_AreaEdicts (world_t *w, vec3_t mins, vec3_t maxs, wedict_t **list, int maxcount, int areatype)
{
	wedict_t *check;
	areagridlink_t *start, *l;
	size_t count = 0;
	int ming[2], maxg[2], g[2], ga;
	CALCAREAGRIDBOUNDS(w, mins, maxs);

	areagridsequence++;

	//check ents that are just too large first
	start = &w->jumboarea;
	for (l=(areagridlink_t*)start->l.next  ; l != start ; l = (areagridlink_t*)l->l.next)
	{
		check = l->ed;

//		if (check->gridareasequence == areagridsequence)
//			continue;
		check->gridareasequence = areagridsequence;
	
		if (areatype != AREA_ALL)
		{
			if (check->v->solid == SOLID_NOT)
				continue;		// deactivated

			if ((check->v->solid == SOLID_TRIGGER||check->v->solid == SOLID_BSPTRIGGER) != (areatype == AREA_TRIGGER))
				continue;
		}

		if (check->v->absmin[0] > maxs[0]
		|| check->v->absmin[1] > maxs[1]
		|| check->v->absmin[2] > maxs[2]
		|| check->v->absmax[0] < mins[0]
		|| check->v->absmax[1] < mins[1]
		|| check->v->absmax[2] < mins[2])
			continue;		// not touching

		if (count == maxcount)
		{
			Con_DPrintf ("World_AreaEdicts: MAXCOUNT\n");
			return count;
		}

		list[count] = check;
		count++;
	}

	//check the actual grid now.
	for (ga = 0, g[0] = ming[0]; g[0] < maxg[0]; g[0]++)
	{
		for (    g[1] = ming[1]; g[1] < maxg[1]; g[1]++, ga++)
		{
			start = &w->gridareas[g[0] + g[1]*w->gridsize[0]];
			for (l=(areagridlink_t*)start->l.next  ; l != start ; l = (areagridlink_t*)l->l.next)
			{
				check = l->ed;

				if (check->gridareasequence == areagridsequence)
					continue;
				check->gridareasequence = areagridsequence;
			
				if (areatype != AREA_ALL)
				{
					if (check->v->solid == SOLID_NOT)
						continue;		// deactivated

					if ((check->v->solid == SOLID_TRIGGER||check->v->solid == SOLID_BSPTRIGGER) != (areatype == AREA_TRIGGER))
						continue;
				}

				if (check->v->absmin[0] > maxs[0]
				|| check->v->absmin[1] > maxs[1]
				|| check->v->absmin[2] > maxs[2]
				|| check->v->absmax[0] < mins[0]
				|| check->v->absmax[1] < mins[1]
				|| check->v->absmax[2] < mins[2])
					continue;		// not touching

				if (count == maxcount)
				{
					Con_Printf ("World_AreaEdicts: MAXCOUNT\n");
					return count;
				}

				list[count] = check;
				count++;
			}
		}
	}
	return count;
}

#else
static float	*area_mins, *area_maxs;
static wedict_t	**area_list;
#ifdef Q2SERVER
static q2edict_t	**area_q2list;
#endif
static int		area_count, area_maxcount;
static int		area_type;
static void World_AreaEdicts_r (areanode_t *node)
{
	link_t		*l, *next, *start;
	wedict_t		*check;

	// touch linked edicts
	start = &node->edicts;

	for (l=start->next  ; l != start ; l = next)
	{
		next = l->next;
		check = EDICT_FROM_AREA(l);

		if (check->v->solid == SOLID_NOT)
			continue;		// deactivated

		/*q2 still has solid/trigger lists, emulate that here*/
		if ((check->v->solid == SOLID_TRIGGER||check->v->solid == SOLID_BSPTRIGGER) != (area_type == AREA_TRIGGER))
			continue;

		if (check->v->absmin[0] > area_maxs[0]
		|| check->v->absmin[1] > area_maxs[1]
		|| check->v->absmin[2] > area_maxs[2]
		|| check->v->absmax[0] < area_mins[0]
		|| check->v->absmax[1] < area_mins[1]
		|| check->v->absmax[2] < area_mins[2])
			continue;		// not touching

		if (area_count == area_maxcount)
		{
			Con_Printf ("SV_AreaEdicts: MAXCOUNT\n");
			return;
		}

		area_list[area_count] = check;
		area_count++;
	}
	
	if (node->axis == -1)
		return;		// terminal node

	// recurse down both sides
	if ( area_maxs[node->axis] > node->dist )
		World_AreaEdicts_r ( node->children[0] );
	if ( area_mins[node->axis] < node->dist )
		World_AreaEdicts_r ( node->children[1] );
}

/*
================
SV_AreaEdicts
================
*/
int World_AreaEdicts (world_t *w, vec3_t mins, vec3_t maxs, wedict_t **list, int maxcount, int areatype)
{
	area_mins = mins;
	area_maxs = maxs;
	area_list = list;
	area_count = 0;
	area_maxcount = maxcount;
	area_type = areatype;

	World_AreaEdicts_r (w->areanodes);

	return area_count;
}
#endif

#ifdef Q2SERVER
const float	*area_mins, *area_maxs;
q2edict_t	**area_q2list;
int		area_count, area_maxcount;
int		area_type;
static void WorldQ2_AreaEdicts_r (areanode_t *node)
{
	link_t		*l, *next, *start;
	q2edict_t		*check;

	// touch linked edicts
	start = &node->edicts;

	for (l=start->next  ; l != start ; l = next)
	{
		if (!l)
		{
			int i;
			World_ClearWorld(&sv.world, false);
			check = ge->edicts;
			for (i = 0; i < ge->num_edicts; i++, check = (q2edict_t	*)((char *)check + ge->edict_size))
				memset(&check->area, 0, sizeof(check->area));
			Con_Printf ("SV_AreaEdicts: Bad links\n");
			return;
		}
		next = l->next;
		check = Q2EDICT_FROM_AREA(l);

		if (check->solid == Q2SOLID_NOT)
			continue;		// deactivated

		/*q2 still has solid/trigger lists, emulate that here*/
		if ((check->solid == Q2SOLID_TRIGGER) != (area_type == AREA_TRIGGER))
			continue;

		if (check->absmin[0] > area_maxs[0]
		|| check->absmin[1] > area_maxs[1]
		|| check->absmin[2] > area_maxs[2]
		|| check->absmax[0] < area_mins[0]
		|| check->absmax[1] < area_mins[1]
		|| check->absmax[2] < area_mins[2])
			continue;		// not touching

		if (area_count == area_maxcount)
		{
			Con_Printf ("SV_AreaEdicts: MAXCOUNT\n");
			return;
		}

		area_q2list[area_count] = check;
		area_count++;
	}
	
	if (node->axis == -1)
		return;		// terminal node

	// recurse down both sides
	if ( area_maxs[node->axis] > node->dist )
		WorldQ2_AreaEdicts_r ( node->children[0] );
	if ( area_mins[node->axis] < node->dist )
		WorldQ2_AreaEdicts_r ( node->children[1] );
}

int VARGS WorldQ2_AreaEdicts (world_t *w, const vec3_t mins, const vec3_t maxs, q2edict_t **list,
	int maxcount, int areatype)
{
	area_mins = mins;
	area_maxs = maxs;
	area_q2list = list;
	area_count = 0;
	area_maxcount = maxcount;
	area_type = areatype;

	WorldQ2_AreaEdicts_r (w->areanodes);

	return area_count;
}
#endif

/*
================
SV_HeadnodeForEntity

Returns a headnode that can be used for testing or clipping an
object of mins/maxs size.
Offset is filled in to contain the adjustment that must be added to the
testing object's origin to get a point to use with the returned hull.
================
*/

#ifdef Q2SERVER
static model_t *WorldQ2_ModelForEntity (world_t *w, q2edict_t *ent)
{
	model_t	*model;

// decide which clipping hull to use, based on the size
	if (ent->solid == Q2SOLID_BSP)
	{	// explicit hulls in the BSP model
		model = w->Get_CModel(w, ent->s.modelindex);

		if (!model)
			SV_Error ("Q2SOLID_BSP with a non bsp model");

		if (model->loadstate == MLS_LOADED)
			return model;
	}

	// create a temp hull from bounding box sizes

	return CM_TempBoxModel (ent->mins, ent->maxs);
}
#endif

#ifdef Q2SERVER
void WorldQ2_ClipMoveToEntities (world_t *w, moveclip_t *clip )
{
	int			i, num;
	q2edict_t		*touchlist[MAX_Q2EDICTS], *touch;
	trace_t		trace;
	model_t		*model;
	float		*angles;

	num = WorldQ2_AreaEdicts (w, clip->boxmins, clip->boxmaxs, touchlist
		, MAX_Q2EDICTS, AREA_SOLID);

	// be careful, it is possible to have an entity in this
	// list removed before we get to it (killtriggered)
	for (i=0 ; i<num ; i++)
	{
		touch = touchlist[i];
		if (touch->solid == Q2SOLID_NOT)
			continue;
		if (touch == clip->q2passedict)
			continue;
		if (clip->trace.allsolid)
			return;
		if (clip->q2passedict)
		{
		 	if (touch->owner == clip->q2passedict)
				continue;	// don't clip against own missiles
			if (clip->q2passedict->owner == touch)
				continue;	// don't clip against owner
		}

		if (touch->svflags & SVF_DEADMONSTER)
		if ( !(clip->hitcontentsmask & Q2CONTENTS_DEADMONSTER))
				continue;

		// might intersect, so do an exact clip
		model = WorldQ2_ModelForEntity (w, touch);
		angles = touch->s.angles;
		if (touch->solid != Q2SOLID_BSP)
			angles = vec3_origin;	// boxes don't rotate

		if (touch->svflags & SVF_MONSTER)
			World_TransformedTrace (model, 0, NULL, clip->start, clip->end, clip->mins2, clip->maxs2, false, &trace, touch->s.origin, angles, clip->hitcontentsmask);
		else
			World_TransformedTrace (model, 0, NULL, clip->start, clip->end, clip->mins, clip->maxs, false, &trace, touch->s.origin, angles, clip->hitcontentsmask);

		if (trace.allsolid || trace.startsolid ||
		trace.fraction < clip->trace.fraction)
		{
			trace.ent = (edict_t *)touch;
		 	if (clip->trace.startsolid)
			{
				clip->trace = trace;
				clip->trace.startsolid = true;
			}
			else
				clip->trace = trace;
		}
		else if (trace.startsolid)
			clip->trace.startsolid = true;
	}
#undef ped
}
#endif
//===========================================================================

//a portal is flush with a world surface behind it.
//this causes problems. namely that we can't pass through the portal plane if the bsp behind it prevents out origin from getting through.
//so if the trace was clipped and ended infront of the portal, continue the trace to the edges of the portal cutout instead.
void World_PortalCSG(wedict_t *portal, float *trmin, float *trmax, vec3_t start, vec3_t end, trace_t *trace)
{
	vec4_t planes[6];	//far, near, right, left, up, down
	int plane;
	vec3_t worldpos;
	float bestfrac;
	int hitplane;
	float portalradius = portal->v->impulse;
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
	AngleVectors(portal->v->angles, planes[1], planes[3], planes[5]);
	VectorNegate(planes[1], planes[0]);
	VectorNegate(planes[3], planes[2]);
	VectorNegate(planes[5], planes[4]);

	portalradius/=2;
	planes[0][3] = DotProduct(portal->v->origin, planes[0]) - (4.0/32);
	planes[1][3] = DotProduct(portal->v->origin, planes[1]) - (4.0/32);	//an epsilon beyond the portal
	planes[2][3] = DotProduct(portal->v->origin, planes[2]) - portalradius;
	planes[3][3] = DotProduct(portal->v->origin, planes[3]) - portalradius;
	planes[4][3] = DotProduct(portal->v->origin, planes[4]) - portalradius;
	planes[5][3] = DotProduct(portal->v->origin, planes[5]) - portalradius;

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
			planes[plane][3] += 24;//DotProduct(nearest, planes[plane]);
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
			frac = (ds) / (ds - de);
			if (frac < bestfrac)
			{
				if (frac < 0)
					frac = 0;
				bestfrac = frac;
				hitplane = plane;
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
			trace->ent = portal;
	}
}

/*
====================
SV_ClipToEverything

like SV_ClipToLinks, but doesn't use the links part. This can be used for checking triggers, solid entities, not-solid entities.
Sounds pointless, I know.
====================
*/
static void World_ClipToEverything (world_t *w, moveclip_t *clip)
{
	int e;
	trace_t		trace;
	wedict_t		*touch;
	for (e=1 ; e<w->num_edicts ; e++)
	{
		touch = (wedict_t*)EDICT_NUM_PB(w->progs, e);

		if (ED_ISFREE(touch))
			continue;
		if (touch->v->solid == SOLID_NOT && !((int)touch->v->flags & FL_FINDABLE_NONSOLID))
			continue;
		if ((touch->v->solid == SOLID_TRIGGER||touch->v->solid == SOLID_BSPTRIGGER) && !((int)touch->v->flags & FL_FINDABLE_NONSOLID))
			continue;

		if (touch == clip->passedict)
			continue;

		if (clip->type & MOVE_NOMONSTERS && touch->v->solid != SOLID_BSP)
			continue;

		if (clip->passedict)
		{
			if (w->usesolidcorpse)
			{
				// don't clip corpse against character
				if (clip->passedict->v->solid == SOLID_CORPSE && (touch->v->solid == SOLID_SLIDEBOX || touch->v->solid == SOLID_CORPSE))
					continue;
				// don't clip character against corpse
				if (clip->passedict->v->solid == SOLID_SLIDEBOX && touch->v->solid == SOLID_CORPSE)
					continue;
			}
			if (!((int)clip->passedict->xv->dimension_hit & (int)touch->xv->dimension_solid))
				continue;
		}

		if (clip->boxmins[0] > touch->v->absmax[0]
				|| clip->boxmins[1] > touch->v->absmax[1]
				|| clip->boxmins[2] > touch->v->absmax[2]
				|| clip->boxmaxs[0] < touch->v->absmin[0]
				|| clip->boxmaxs[1] < touch->v->absmin[1]
				|| clip->boxmaxs[2] < touch->v->absmin[2] )
			continue;

		if (clip->passedict && clip->passedict->v->size[0] && !touch->v->size[0])
			continue;	// points never interact

	// might intersect, so do an exact clip
//		if (clip->trace.allsolid)
//			return;
		if (clip->passedict)
		{
		 	if ((wedict_t*)PROG_TO_EDICT(w->progs, touch->v->owner) == clip->passedict)
				continue;	// don't clip against own missiles
			if ((wedict_t*)PROG_TO_EDICT(w->progs, clip->passedict->v->owner) == touch)
				continue;	// don't clip against owner
		}

		if (touch->v->solid == SOLID_PORTAL)
		{
			//make sure we don't hit the world if we're inside the portal
			World_PortalCSG(touch, clip->mins, clip->maxs, clip->start, clip->end, &clip->trace);
		}

		if ((int)touch->v->flags & FL_MONSTER)
			trace = World_ClipMoveToEntity (w, touch, touch->v->origin, touch->v->angles, clip->start, clip->mins2, clip->maxs2, clip->end, clip->hullnum, clip->type & MOVE_HITMODEL, clip->capsule, clip->hitcontentsmask);
		else
			trace = World_ClipMoveToEntity (w, touch, touch->v->origin, touch->v->angles, clip->start, clip->mins, clip->maxs, clip->end, clip->hullnum, clip->type & MOVE_HITMODEL, clip->capsule, clip->hitcontentsmask);

		if (trace.fraction < clip->trace.fraction)
		{
			//trace traveled less, but don't forget if we started in a solid.
			trace.startsolid |= clip->trace.startsolid;
			trace.allsolid |= clip->trace.allsolid;

			if (clip->type & MOVE_ENTCHAIN)
			{
				touch->v->chain = EDICT_TO_PROG(w->progs, clip->trace.ent?clip->trace.ent:w->edicts);
				clip->trace.ent = touch;
			}
			else
			{
				if (clip->trace.startsolid && !trace.startsolid)
					trace.ent = clip->trace.ent;	//something else hit earlier, that one gets the trace entity, but not the fraction. yeah, combining traces like this was always going to be weird.
				else
					trace.ent = touch;
				clip->trace = trace;
			}
		}
		else if (trace.startsolid || trace.allsolid)
		{
			//even if the trace traveled less, we still care if it was in a solid.
			clip->trace.startsolid |= trace.startsolid;
			clip->trace.allsolid |= trace.allsolid;
			clip->trace.contents |= trace.contents;
			if (!clip->trace.ent || trace.fraction == clip->trace.fraction)	//xonotic requires that second test (DP has no check at all, which would end up reporting mismatched fraction/ent results, so yuck).
			{
				clip->trace.ent = touch;
			}
		}
	}
}

#ifdef USEAREAGRID

void World_TouchAllLinks (world_t *w, wedict_t *ent)
{
	wedict_t *touchedicts[2048], *touch;
	int num, solid;
	num = World_AreaEdicts(w, ent->v->absmin, ent->v->absmax, touchedicts, countof(touchedicts), AREA_TRIGGER);
	while (num-- > 0)
	{
		touch = touchedicts[num];

		//make sure nothing moved it away
		if (ED_ISFREE(touch))
			continue;
		solid =touch->v->solid;
		if (!touch->v->touch || (solid!= SOLID_TRIGGER && solid!= SOLID_BSPTRIGGER))
			continue;
		if (touch == ent)
			continue;

		if (ent->v->absmin[0] > touch->v->absmax[0]
		|| ent->v->absmin[1] > touch->v->absmax[1]
		|| ent->v->absmin[2] > touch->v->absmax[2]
		|| ent->v->absmax[0] < touch->v->absmin[0]
		|| ent->v->absmax[1] < touch->v->absmin[1]
		|| ent->v->absmax[2] < touch->v->absmin[2] )
			continue;

		if (!((int)ent->xv->dimension_solid & (int)touch->xv->dimension_hit))	//didn't change did it?...
			continue;

		if (solid == SOLID_BSPTRIGGER)
		{
			if (!World_ClipMoveToEntity(w, touch, touch->v->origin, touch->v->angles, ent->v->origin, ent->v->mins, ent->v->maxs, ent->v->origin, 0, false, (ent->xv->geomtype == GEOMTYPE_CAPSULE), MASK_WORLDSOLID).startsolid)
				continue;
		}

		w->Event_Touch(w, touch, ent, NULL);

		if (ED_ISFREE(ent))
			break;
	}
}

void World_UnlinkEdict (wedict_t *ent)
{
	size_t i;
	for (i = 0; i < countof(ent->gridareas); i++)
	{
		if (!ent->gridareas[i].l.prev)
			return;		// not linked in anywhere
		RemoveLink (&ent->gridareas[i].l);
		ent->gridareas[i].l.prev = ent->gridareas[i].l.next = NULL;
	}
}

//nettest: a "phys prop" solid type — collidable geometry that isn't a player/monster/item.
//MOVE_HITPROPS lets MOVE_NOMONSTERS traces (e.g. weather particles) also hit these. SOLID_PHYSICS_BOX..CYLINDER are 32..36.
#define SOLID_ISPHYSPROP(s) ((s) >= SOLID_PHYSICS_BOX && (s) <= SOLID_PHYSICS_CYLINDER)
static void World_ClipToLinks (world_t *w, areagridlink_t *node, moveclip_t *clip)
{
	link_t		*l, *next;
	wedict_t		*touch;
	trace_t		trace;

// touch linked edicts
	for (l = node->l.next ; l != &node->l ; l = next)
	{
		next = l->next;
		touch = ((areagridlink_t*)l)->ed;

		if (touch->gridareasequence == areagridsequence)
			continue;
		touch->gridareasequence = areagridsequence;

		if (touch->v->solid == SOLID_NOT)
			continue;
		if (touch == clip->passedict)
			continue;

		/*if its a trigger, we only clip against it if the flags are aligned*/
		if (SOLID_ISTRIGGER(touch->v->solid))
		{
			if (!(clip->type & MOVE_TRIGGERS))
				continue;
			if (!((int)touch->v->flags & FL_FINDABLE_NONSOLID))
				continue;
		}

		if (clip->type & MOVE_LAGGED)
		{
			//can't touch lagged ents - we do an explicit test for them later.
			if (touch->entnum-1 < w->maxlagents)
				if (w->lagents[touch->entnum-1].present)
					continue;
		}

		if ((clip->type & MOVE_NOMONSTERS) && (touch->v->solid != SOLID_BSP && touch->v->solid != SOLID_PORTAL)
			&& !((clip->type & MOVE_HITPROPS) && SOLID_ISPHYSPROP(touch->v->solid)))	//nettest clipprops: MOVE_HITPROPS keeps phys props
			continue;

		if (clip->passedict)
		{
			if (w->usesolidcorpse)
			{
#if 1
//				if (!(clip->hitcontentsmask & ((touch->v->solid == SOLID_CORPSE)?FTECONTENTS_CORPSE:FTECONTENTS_BODY)))
//					continue;
#else
				// don't clip corpse against character
				if (clip->passedict->v->solid == SOLID_CORPSE && (touch->v->solid == SOLID_SLIDEBOX || touch->v->solid == SOLID_CORPSE))
					continue;
				// don't clip character against corpse
				if (clip->passedict->v->solid == SOLID_SLIDEBOX && touch->v->solid == SOLID_CORPSE)
					continue;
#endif
			}
			if (!((int)clip->passedict->xv->dimension_hit & (int)touch->xv->dimension_solid))
				continue;
		}

		if (clip->boxmins[0] > touch->v->absmax[0]
		|| clip->boxmins[1] > touch->v->absmax[1]
		|| clip->boxmins[2] > touch->v->absmax[2]
		|| clip->boxmaxs[0] < touch->v->absmin[0]
		|| clip->boxmaxs[1] < touch->v->absmin[1]
		|| clip->boxmaxs[2] < touch->v->absmin[2] )
			continue;

		if (clip->passedict && clip->passedict->v->size[0] && !touch->v->size[0])
			continue;	// points never interact

	// might intersect, so do an exact clip
//		if (clip->trace.allsolid)
//			return;
		if (clip->passedict)
		{
		 	if ((wedict_t*)PROG_TO_EDICT(w->progs, touch->v->owner) == clip->passedict)
				continue;	// don't clip against own missiles
			if ((wedict_t*)PROG_TO_EDICT(w->progs, clip->passedict->v->owner) == touch)
				continue;	// don't clip against owner
		}

		if (touch->v->solid == SOLID_PORTAL)
		{
			//make sure we don't hit the world if we're inside the portal
			World_PortalCSG(touch, clip->mins, clip->maxs, clip->start, clip->end, &clip->trace);
		}

		if ((int)touch->v->flags & FL_MONSTER)
			trace = World_ClipMoveToEntity (w, touch, touch->v->origin, touch->v->angles, clip->start, clip->mins2, clip->maxs2, clip->end, clip->hullnum, clip->type & MOVE_HITMODEL, clip->capsule, clip->hitcontentsmask);
		else
			trace = World_ClipMoveToEntity (w, touch, touch->v->origin, touch->v->angles, clip->start, clip->mins, clip->maxs, clip->end, clip->hullnum, clip->type & MOVE_HITMODEL, clip->capsule, clip->hitcontentsmask);

		if (trace.fraction < clip->trace.fraction)
		{
			//trace traveled less, but don't forget if we started in a solid.
			trace.startsolid |= clip->trace.startsolid;
			trace.allsolid |= clip->trace.allsolid;

			if (clip->type & MOVE_ENTCHAIN)
			{
				touch->v->chain = EDICT_TO_PROG(w->progs, clip->trace.ent?clip->trace.ent:w->edicts);
				clip->trace.ent = touch;
			}
			else
			{
				if (clip->trace.startsolid && !trace.startsolid)
					trace.ent = clip->trace.ent;	//something else hit earlier, that one gets the trace entity, but not the fraction. yeah, combining traces like this was always going to be weird.
				else
					trace.ent = touch;
				clip->trace = trace;
			}
		}
		else if (trace.startsolid || trace.allsolid)
		{
			//even if the trace traveled less, we still care if it was in a solid.
			clip->trace.startsolid |= trace.startsolid;
			clip->trace.allsolid |= trace.allsolid;
			clip->trace.contents |= trace.contents;
			if (!clip->trace.ent || trace.fraction == clip->trace.fraction)	//xonotic requires that second test (DP has no check at all, which would end up reporting mismatched fraction/ent results, so yuck).
			{
				clip->trace.ent = touch;
			}
		}
	}
}
static void World_ClipToAllLinks (world_t *w, moveclip_t *clip)
{
	int ming[2], maxg[2], g[2];
	areagridsequence++;
	World_ClipToLinks(w, &w->jumboarea, clip);

	CALCAREAGRIDBOUNDS(w, clip->boxmins, clip->boxmaxs);

	for (    g[0] = ming[0]; g[0] < maxg[0]; g[0]++)
		for (g[1] = ming[1]; g[1] < maxg[1]; g[1]++)
		{
			World_ClipToLinks(w, &w->gridareas[g[0] + g[1]*w->gridsize[0]], clip);
		}
}

static unsigned int World_ContentsOfLinks (world_t *w, areagridlink_t *node, vec3_t pos)
{
	link_t		*l, *next;
	wedict_t		*touch;

	model_t		*model;
	int mdlidx;
	vec3_t pos_l, axis[3];
	unsigned int ret = 0, c;

// touch linked edicts
	for (l = node->l.next ; l != &node->l ; l = next)
	{
		next = l->next;
		touch = ((areagridlink_t*)l)->ed;

		if (touch->gridareasequence == areagridsequence)
			continue;
		touch->gridareasequence = areagridsequence;

		if (touch->v->solid != SOLID_BSP)
			continue;

		if (   pos[0] > touch->v->absmax[0]
			|| pos[1] > touch->v->absmax[1]
			|| pos[2] > touch->v->absmax[2]
			|| pos[0] < touch->v->absmin[0]
			|| pos[1] < touch->v->absmin[1]
			|| pos[2] < touch->v->absmin[2] )
			continue;

//		if (touch->v->solid == SOLID_PORTAL)
//			//FIXME: recurse!

		mdlidx = touch->v->modelindex;
		if (!mdlidx)
			continue;
		model = w->Get_CModel(w, mdlidx);
		if (!model || (model->type != mod_brush && model->type != mod_heightmap) || model->loadstate != MLS_LOADED)
			continue;

		VectorSubtract (pos, touch->v->origin, pos_l);
		if (touch->v->angles[0] || touch->v->angles[1] || touch->v->angles[2])
		{
			AngleVectors (touch->v->angles, axis[0], axis[1], axis[2]);
			VectorNegate(axis[1], axis[1]);
			c = model->funcs.PointContents(model, axis, pos_l);
		}
		else
			c = model->funcs.PointContents(model, NULL, pos_l);

		if (c && touch->v->skin < 0)
		{	//if forcedcontents is set, then ALL brushes in this model are forced to the specified contents value.
			//we achive this by tracing against ALL then forcing it after.
			unsigned int forcedcontents;
			safeswitch((enum q1contents_e)(int)touch->v->skin)
			{
			case Q1CONTENTS_EMPTY:			forcedcontents = FTECONTENTS_EMPTY;			break;
			case Q1CONTENTS_SOLID:			forcedcontents = FTECONTENTS_SOLID;			break;
			case Q1CONTENTS_WATER:			forcedcontents = FTECONTENTS_WATER;			break;
			case Q1CONTENTS_SLIME:			forcedcontents = FTECONTENTS_SLIME;			break;
			case Q1CONTENTS_LAVA:			forcedcontents = FTECONTENTS_LAVA;			break;
			case Q1CONTENTS_SKY:			forcedcontents = FTECONTENTS_SKY;			break;
			case HLCONTENTS_CLIP:			forcedcontents = FTECONTENTS_PLAYERCLIP|FTECONTENTS_MONSTERCLIP;	break;
			case HLCONTENTS_CURRENT_0:		forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_0;			break;
			case HLCONTENTS_CURRENT_90:		forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_90;			break;
			case HLCONTENTS_CURRENT_180:	forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_180;			break;
			case HLCONTENTS_CURRENT_270:	forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_270;			break;
			case HLCONTENTS_CURRENT_UP:		forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_UP;			break;
			case HLCONTENTS_CURRENT_DOWN:	forcedcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_DOWN;			break;
			case HLCONTENTS_TRANS:			forcedcontents = FTECONTENTS_EMPTY;			break;
			case Q1CONTENTS_LADDER:			forcedcontents = FTECONTENTS_LADDER;		break;
			case Q1CONTENTS_MONSTERCLIP:	forcedcontents = FTECONTENTS_MONSTERCLIP;	break;
			case Q1CONTENTS_PLAYERCLIP:		forcedcontents = FTECONTENTS_PLAYERCLIP;	break;
			case Q1CONTENTS_CORPSE:			forcedcontents = FTECONTENTS_CORPSE;		break;
			safedefault:					forcedcontents = 0;							break;
			}
			c = forcedcontents;
		}
		ret |= c;
	}
	return ret;
}
static unsigned int World_ContentsOfAllLinks (world_t *w, vec3_t pos)
{
	int ming[2], maxg[2], g[2];
	unsigned int ret;
	areagridsequence++;
	ret = World_ContentsOfLinks(w, &w->jumboarea, pos);

	CALCAREAGRIDBOUNDS(w, pos, pos);

	for (    g[0] = ming[0]; g[0] < maxg[0]; g[0]++)
		for (g[1] = ming[1]; g[1] < maxg[1]; g[1]++)
		{
			ret |= World_ContentsOfLinks(w, &w->gridareas[g[0] + g[1]*w->gridsize[0]], pos);
		}
	return ret;
}
#else
/*
====================
SV_ClipToLinks

Mins and maxs enclose the entire area swept by the move
====================
*/
static void World_ClipToLinks (world_t *w, areanode_t *node, moveclip_t *clip)
{
	link_t		*l, *next;
	wedict_t		*touch;
	trace_t		trace;

// touch linked edicts
	for (l = node->edicts.next ; l != &node->edicts ; l = next)
	{
		next = l->next;
		touch = EDICT_FROM_AREA(l);
		if (touch->v->solid == SOLID_NOT)
			continue;
		if (touch == clip->passedict)
			continue;

		/*if its a trigger, we only clip against it if the flags are aligned*/
		if (SOLID_ISTRIGGER(touch->v->solid))
		{
			if (!(clip->type & MOVE_TRIGGERS))
				continue;
			if (!((int)touch->v->flags & FL_FINDABLE_NONSOLID))
				continue;
		}

		if (clip->type & MOVE_LAGGED)
		{
			//can't touch lagged ents - we do an explicit test for them later.
			if (touch->entnum-1 < w->maxlagents)
				if (w->lagents[touch->entnum-1].present)
					continue;
		}

		if ((clip->type & MOVE_NOMONSTERS) && (touch->v->solid != SOLID_BSP && touch->v->solid != SOLID_PORTAL)
			&& !((clip->type & MOVE_HITPROPS) && SOLID_ISPHYSPROP(touch->v->solid)))	//nettest clipprops: MOVE_HITPROPS keeps phys props
			continue;

		if (clip->passedict)
		{
			if (w->usesolidcorpse)
			{
#if 1
//				if (!(clip->hitcontentsmask & ((touch->v->solid == SOLID_CORPSE)?FTECONTENTS_CORPSE:FTECONTENTS_BODY)))
//					continue;
#else
				// don't clip corpse against character
				if (clip->passedict->v->solid == SOLID_CORPSE && (touch->v->solid == SOLID_SLIDEBOX || touch->v->solid == SOLID_CORPSE))
					continue;
				// don't clip character against corpse
				if (clip->passedict->v->solid == SOLID_SLIDEBOX && touch->v->solid == SOLID_CORPSE)
					continue;
#endif
			}
			if (!((int)clip->passedict->xv->dimension_hit & (int)touch->xv->dimension_solid))
				continue;
		}

		if (clip->boxmins[0] > touch->v->absmax[0]
		|| clip->boxmins[1] > touch->v->absmax[1]
		|| clip->boxmins[2] > touch->v->absmax[2]
		|| clip->boxmaxs[0] < touch->v->absmin[0]
		|| clip->boxmaxs[1] < touch->v->absmin[1]
		|| clip->boxmaxs[2] < touch->v->absmin[2] )
			continue;

		if (clip->passedict && clip->passedict->v->size[0] && !touch->v->size[0])
			continue;	// points never interact

	// might intersect, so do an exact clip
//		if (clip->trace.allsolid)
//			return;
		if (clip->passedict)
		{
		 	if ((wedict_t*)PROG_TO_EDICT(w->progs, touch->v->owner) == clip->passedict)
				continue;	// don't clip against own missiles
			if ((wedict_t*)PROG_TO_EDICT(w->progs, clip->passedict->v->owner) == touch)
				continue;	// don't clip against owner
		}

		if (touch->v->solid == SOLID_PORTAL)
		{
			//make sure we don't hit the world if we're inside the portal
			World_PortalCSG(touch, clip->mins, clip->maxs, clip->start, clip->end, &clip->trace);
		}

		if ((int)touch->v->flags & FL_MONSTER)
			trace = World_ClipMoveToEntity (w, touch, touch->v->origin, touch->v->angles, clip->start, clip->mins2, clip->maxs2, clip->end, clip->hullnum, clip->type & MOVE_HITMODEL, clip->capsule, clip->hitcontentsmask);
		else
			trace = World_ClipMoveToEntity (w, touch, touch->v->origin, touch->v->angles, clip->start, clip->mins, clip->maxs, clip->end, clip->hullnum, clip->type & MOVE_HITMODEL, clip->capsule, clip->hitcontentsmask);

		if (trace.fraction < clip->trace.fraction)
		{
			//trace traveled less, but don't forget if we started in a solid.
			trace.startsolid |= clip->trace.startsolid;
			trace.allsolid |= clip->trace.allsolid;

			if (clip->type & MOVE_ENTCHAIN)
			{
				touch->v->chain = EDICT_TO_PROG(w->progs, clip->trace.ent?clip->trace.ent:w->edicts);
				clip->trace.ent = touch;
			}
			else
			{
				if (clip->trace.startsolid && !trace.startsolid)
					trace.ent = clip->trace.ent;	//something else hit earlier, that one gets the trace entity, but not the fraction. yeah, combining traces like this was always going to be weird.
				else
					trace.ent = touch;
				clip->trace = trace;
			}
		}
		else if (trace.startsolid || trace.allsolid)
		{
			//even if the trace traveled less, we still care if it was in a solid.
			clip->trace.startsolid |= trace.startsolid;
			clip->trace.allsolid |= trace.allsolid;
			clip->trace.contents |= trace.contents;
			if (!clip->trace.ent)
			{
				clip->trace.ent = touch;
			}
		}
	}
	
// recurse down both sides
	if (node->axis == -1)
		return;

	if ( clip->boxmaxs[node->axis] > node->dist )
		World_ClipToLinks (w, node->children[0], clip );
	if ( clip->boxmins[node->axis] < node->dist )
		World_ClipToLinks (w, node->children[1], clip );
}



static unsigned int World_ContentsOfLinks (world_t *w, areanode_t *node, vec3_t pos)
{
	unsigned int c = 0;
	link_t		*l, *next;
	wedict_t		*touch;
	trace_t		trace;

// touch linked edicts
	for (l = node->edicts.next ; l != &node->edicts ; l = next)
	{
		next = l->next;
		touch = EDICT_FROM_AREA(l);
		if (touch->v->solid == SOLID_NOT)
			continue;

		/*if its a trigger, we only clip against it if the flags are aligned*/
		if (touch->v->solid == SOLID_TRIGGER||touch->v->solid == SOLID_BSPTRIGGER)
			continue;

		if (pos[0] > touch->v->absmax[0]
		|| pos[1] > touch->v->absmax[1]
		|| pos[2] > touch->v->absmax[2]
		|| pos[0] < touch->v->absmin[0]
		|| pos[1] < touch->v->absmin[1]
		|| pos[2] < touch->v->absmin[2] )
			continue;

		/*if (touch->v->solid == SOLID_PORTAL)
		{
			//make sure we don't hit the world if we're inside the portal
			World_PortalCSG(touch, vec3_origin, vec3_origin, pos, pos, &clip->trace);
		}*/

		trace = World_ClipMoveToEntity (w, touch, touch->v->origin, touch->v->angles, pos, vec3_origin, vec3_origin, pos, 0, false, false, ~0u);
		if (trace.startsolid)
			c |= trace.contents;
	}

// recurse down both sides
	if (node->axis == -1)
		return c;

	if ( pos[node->axis] > node->dist )
		c |= World_ContentsOfLinks (w, node->children[0], pos );
	if ( pos[node->axis] < node->dist )
		c |= World_ContentsOfLinks (w, node->children[1], pos );

	return c;
}
static unsigned int World_ContentsOfAllLinks (world_t *w, vec3_t pos)
{
	return World_ContentsOfLinks(w, w->areanodes, pos);
}
#endif

#if defined(HAVE_CLIENT) && defined(CSQC_DAT)
//The logic of this function is seriously handicapped vs the other types of trace we could be doing.
static void World_ClipToNetwork (world_t *w, moveclip_t *clip)
{
	int i;
	packet_entities_t *pe = cl.currentpackentities;
	entity_state_t	*touch;

	unsigned int touchcontents;
	model_t *model;
	vec3_t bmins, bmaxs;
	float *ang;
	trace_t trace;
	static framestate_t framestate;	//meh
	int skip;

	if ((clip->type & MOVE_ENTCHAIN) || !pe)
		return;

	//lets say that ssqc ents are in dimension 0x1, as far as the csqc can see.
	if (clip->passedict)
	{
		if (!((int)clip->passedict->xv->dimension_hit & 1))
			return;
		skip = ((csqcedict_t*)clip->passedict)->xv->entnum;
	}
	else
		skip = 0;

	for (i = 0; i < pe->num_entities; i++)
	{
		touch = &pe->entities[i];
		if (touch->number == skip)
			continue; //can happen with deltalisten or certain evil hacks.

		if (touch->solidsize == ES_SOLID_BSP)
		{
			if (touch->modelindex <= 0 || touch->modelindex >= MAX_PRECACHE_MODELS)
				continue;	//erk
			model = cl.model_precache[touch->modelindex];
			if (!model || model->loadstate != MLS_LOADED || !model->funcs.NativeTrace)
				continue;
			VectorCopy(model->mins, bmins);
			VectorCopy(model->maxs, bmaxs);
			ang = touch->angles;

			if (ang[0] || ang[1] || ang[2])
			{	//expand the size to deal with rotations. lazy method.
				int i;
				float v;
				float max = 0;
				for (i=0 ; i<3 ; i++)
				{
					v = fabs( bmins[i]);
					if (v > max)
						max = v;
					v = fabs( bmaxs[i]);
					if (v > max)
						max = v;
				}
				VectorSet(bmins, -max,-max,-max);
				VectorSet(bmaxs,  max, max, max);
			}

			safeswitch((enum q1contents_e)touch->skinnum)
			{
			case Q1CONTENTS_EMPTY:			touchcontents = 0;							break;
			case Q1CONTENTS_SOLID:			touchcontents = FTECONTENTS_SOLID;			break;
			case Q1CONTENTS_WATER:			touchcontents = FTECONTENTS_WATER;			break;
			case Q1CONTENTS_SLIME:			touchcontents = FTECONTENTS_SLIME;			break;
			case Q1CONTENTS_LAVA:			touchcontents = FTECONTENTS_LAVA;			break;
			case Q1CONTENTS_SKY:			touchcontents = FTECONTENTS_SKY;			break;
			case HLCONTENTS_CLIP:			touchcontents = FTECONTENTS_PLAYERCLIP|FTECONTENTS_MONSTERCLIP;	break;
			case HLCONTENTS_CURRENT_0:		touchcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_0;			break;
			case HLCONTENTS_CURRENT_90:		touchcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_90;		break;
			case HLCONTENTS_CURRENT_180:	touchcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_180;		break;
			case HLCONTENTS_CURRENT_270:	touchcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_270;		break;
			case HLCONTENTS_CURRENT_UP:		touchcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_UP;		break;
			case HLCONTENTS_CURRENT_DOWN:	touchcontents = FTECONTENTS_WATER|Q2CONTENTS_CURRENT_DOWN;		break;
			case HLCONTENTS_TRANS:			touchcontents = FTECONTENTS_EMPTY;								break;
			case Q1CONTENTS_LADDER:			touchcontents = FTECONTENTS_LADDER;			break;
			case Q1CONTENTS_MONSTERCLIP:	touchcontents = FTECONTENTS_MONSTERCLIP;	break;
			case Q1CONTENTS_PLAYERCLIP:		touchcontents = FTECONTENTS_PLAYERCLIP;		break;
			case Q1CONTENTS_CORPSE:			touchcontents = FTECONTENTS_CORPSE;			break;
			safedefault:					touchcontents = ~0;							break;	//could be anything... :(
			}
		}
#if 1
		else
			continue;	//only hit brush ents.
#else
		else if (touch->solidsize == ES_SOLID_NOT)
			continue;
		else
		{
			if (clip->type & MOVE_NOMONSTERS)
				continue;
			touchcontents = FTECONTENTS_BODY;
			model = NULL;
			COM_DecodeSize(touch->solidsize, bmins, bmaxs);
			World_HullForBox(bmins, bmaxs);
			ang = vec3_origin;
		}
#endif
		if (!(clip->hitcontentsmask & touchcontents))
			continue;

		if (   clip->boxmins[0] > touch->origin[0]+bmaxs[0]
			|| clip->boxmins[1] > touch->origin[1]+bmaxs[1]
			|| clip->boxmins[2] > touch->origin[2]+bmaxs[2]
			|| clip->boxmaxs[0] < touch->origin[0]+bmins[0]
			|| clip->boxmaxs[1] < touch->origin[1]+bmins[1]
			|| clip->boxmaxs[2] < touch->origin[2]+bmins[2] )
			continue;

		framestate.g[FS_REG].frame[0] = touch->frame;
		framestate.g[FS_REG].lerpweight[0] = 1;

		if (World_TransformedTrace(model, 0, &framestate, clip->start, clip->end, clip->mins, clip->maxs, clip->capsule, &trace, touch->origin, ang, clip->hitcontentsmask))
		{
	// if using hitmodel, we know it hit the bounding box, so try a proper trace now.
			/*if (clip->type & MOVE_HITMODEL && (trace.fraction != 1 || trace.startsolid) && !model)
			{
				//okay, we hit the bbox
				model = w->Get_CModel(w, mdlidx);

				if (model && model->funcs.NativeTrace && model->loadstate == MLS_LOADED)
				{
					//do the second trace, using the actual mesh.
					World_TransformedTrace(model, hullnum, &framestate, start, end, mins, maxs, capsule, &trace, eorg, vec3_origin, hitcontentsmask);
				}
			}*/
		}

		if (touchcontents != ~0)
			trace.contents = touchcontents;

		if (trace.fraction < clip->trace.fraction)
		{
			//trace traveled less, but don't forget if we started in a solid.
			trace.startsolid |= clip->trace.startsolid;
			trace.allsolid |= clip->trace.allsolid;

			if (clip->trace.startsolid && !trace.startsolid)
				trace.ent = clip->trace.ent;	//something else hit earlier, that one gets the trace entity, but not the fraction. yeah, combining traces like this was always going to be weird.
			else
			{
				trace.ent = w->edicts;	//misreport world
				trace.entnum = touch->number;	//with an ssqc ent number
			}
			clip->trace = trace;
		}
		else if (trace.startsolid || trace.allsolid)
		{
			//even if the trace traveled less, we still care if it was in a solid.
			clip->trace.startsolid |= trace.startsolid;
			clip->trace.allsolid |= trace.allsolid;
			if (!clip->trace.ent || trace.fraction == clip->trace.fraction)	//xonotic requires that second test (DP has no check at all, which would end up reporting mismatched fraction/ent results, so yuck).
			{
				clip->trace.contents = trace.contents;
				clip->trace.ent = w->edicts;	//misreport world
				clip->trace.entnum = touch->number;	//with an ssqc ent number
			}
		}
	}
}
#endif

/*
==================
SV_MoveBounds
==================
*/
static void World_MoveBounds (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, vec3_t boxmins, vec3_t boxmaxs)
{
#if 0
// debug to test against everything
boxmins[0] = boxmins[1] = boxmins[2] = -9999;
boxmaxs[0] = boxmaxs[1] = boxmaxs[2] = 9999;
#else
	int		i;
	
	for (i=0 ; i<3 ; i++)
	{
		if (end[i] > start[i])
		{
			boxmins[i] = start[i] + mins[i] - 1;
			boxmaxs[i] = end[i] + maxs[i] + 1;
		}
		else
		{
			boxmins[i] = end[i] + mins[i] - 1;
			boxmaxs[i] = start[i] + maxs[i] + 1;
		}
	}
#endif
}

#if !defined(CLIENTONLY)
qboolean SV_AntiKnockBack(world_t *w, client_t *client)
{
	int seq = client->netchan.incoming_acknowledged;	//our outgoing sequence that was last acked (in qw, this matches the last known-good input frame)
	client_frame_t *frame;
	edict_t *ent = client->edict;
	if (client->protocol != SCP_QUAKEWORLD || !client->frameunion.frames || !ent)
		return false;	//FIXME: support nq protocols too

	//reload player state from the journal (the input frame should already have been applied)
	frame = &client->frameunion.frames[seq&UPDATE_MASK];
	VectorCopy(frame->pmorigin, pmove.origin);
	VectorCopy(frame->pmvelocity, pmove.velocity);
	pmove.pm_type = frame->pmtype;
	pmove.onground = true;//FIXME
	pmove.jump_held = frame->pmjumpheld;
	pmove.waterjumptime = frame->pmwaterjumptime;
	pmove.onladder = frame->pmonladder;

	//stuff not regenerated properly, shouldn't really be changing much or not very significant.
	pmove.world = w;
	VectorCopy(ent->v->mins, pmove.player_mins);
	VectorCopy(ent->v->maxs, pmove.player_maxs);
	pmove.capsule = (ent->xv->geomtype == GEOMTYPE_CAPSULE);
	if (ent->xv->gravitydir[2] || ent->xv->gravitydir[1] || ent->xv->gravitydir[0])
		VectorCopy(ent->xv->gravitydir, pmove.gravitydir);
	else
		VectorCopy(w->g.defaultgravitydir, pmove.gravitydir);

	//FIXME
	VectorCopy(ent->v->oldorigin, pmove.safeorigin);
	pmove.safeorigin_known = false;
	pmove.jump_msec = 0;
	VectorClear(pmove.basevelocity);

	//and apply each more recent frame
	while (++seq <= client->netchan.incoming_sequence)
	{
		if (frame->sequence != seq)
			continue;	//FIXME: lost

		pmove.cmd = frame->cmd;

//		pmove.angles;

//		pmove.numphysent/physents;

		PM_PlayerMove(sv.gamespeed);
	}
	return true;
}
#endif

/*
==================
SV_Move
==================
*/
trace_t World_Move (world_t *w, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int type, wedict_t *passedict)
{
	moveclip_t	clip;
	int			i;
	int hullnum;

	memset ( &clip, 0, sizeof ( moveclip_t ) );

	if (passedict->xv->hull && !(type & MOVE_IGNOREHULL))
		hullnum = passedict->xv->hull;
#ifdef CLIENTONLY
	else
		hullnum = 0;
#else
	else if (sv_compatiblehulls.value)
		hullnum = 0;
	else
	{
		int diff;
		int best;
		hullnum = 0;
		best = 8192;
		//x/y pos/neg are assumed to be the same magnitute.
		//z pos/height are assumed to be different from all the others.
		for (i = 0; i < MAX_MAP_HULLSM; i++)
		{
			if (!w->worldmodel->hulls[i].available)
				continue;
#define sq(x) ((x)*(x))
			diff = sq(w->worldmodel->hulls[i].clip_maxs[2] - maxs[2]) +
				sq(w->worldmodel->hulls[i].clip_mins[2] - mins[2]) +
				sq(w->worldmodel->hulls[i].clip_maxs[1] - maxs[1]) +
				sq(w->worldmodel->hulls[i].clip_mins[0] - mins[0]);
			if (diff < best)
			{
				best = diff;
				hullnum=i;
			}
		}
		hullnum++;
	}
#endif

#if !defined(CLIENTONLY)
	//figure out where the firing player was, and re-run their input frames to calculate their position without any velocity/knockback changes.
	//then update the start position to compensate.
	if ((clip.type & MOVE_LAGGED) && w == &sv.world && passedict->entnum && passedict->entnum <= sv.allocated_client_slots && sv_antilag.ival==3)
	{
		vec3_t nudge;
		if (SV_AntiKnockBack(w, &svs.clients[passedict->entnum-1]))
		{
			VectorSubtract(pmove.origin, passedict->v->origin, nudge);

			VectorAdd(start, nudge, start);
			VectorAdd(end, nudge, end);
		}
	}
#endif

	if (passedict->xv->hitcontentsmaski)
		clip.hitcontentsmask = passedict->xv->hitcontentsmaski;
#ifdef HAVE_LEGACY
	else if (passedict->xv->dphitcontentsmask)
	{
		unsigned int nm=0, fl = passedict->xv->dphitcontentsmask;
		if (fl & DPCONTENTS_SOLID)
			nm |= FTECONTENTS_SOLID;
		if (fl & DPCONTENTS_WATER)
			nm |= FTECONTENTS_WATER;
		if (fl & DPCONTENTS_SLIME)
			nm |= FTECONTENTS_SLIME;
		if (fl & DPCONTENTS_LAVA)
			nm |= FTECONTENTS_LAVA;
		if (fl & DPCONTENTS_SKY)
			nm |= FTECONTENTS_SKY;
		if (fl & DPCONTENTS_BODY)
			nm |= FTECONTENTS_BODY;
		if (fl & DPCONTENTS_CORPSE)
			nm |= FTECONTENTS_CORPSE;
		if (fl & DPCONTENTS_NODROP)
			nm |= Q3CONTENTS_NODROP;
		if (fl & DPCONTENTS_PLAYERCLIP)
			nm |= FTECONTENTS_PLAYERCLIP;
		if (fl & DPCONTENTS_MONSTERCLIP)
			nm |= FTECONTENTS_MONSTERCLIP;
		if (fl & DPCONTENTS_DONOTENTER)
			nm |= Q3CONTENTS_DONOTENTER;
		if (fl & DPCONTENTS_BOTCLIP)
			nm |= Q3CONTENTS_BOTCLIP;
//		if (fl & DPCONTENTS_OPAQUE)
//			nm |= DPCONTENTS_OPAQUE;

		clip.hitcontentsmask = nm;
	}
#endif
/*#ifdef HAVE_LEGACY
	else if (passedict->xv->hitcontentsmask)
		clip.hitcontentsmask = passedict->xv->hitcontentsmask;
#endif*/
	else if (passedict->v->solid == SOLID_SLIDEBOX)
	{
		if ((int)passedict->v->flags & FL_MONSTER)
			clip.hitcontentsmask = FTECONTENTS_SOLID|Q2CONTENTS_WINDOW | FTECONTENTS_BODY | FTECONTENTS_MONSTERCLIP; /*solid only to world*/
		else if (maxs[0] - mins[0] > 0)
			clip.hitcontentsmask = FTECONTENTS_SOLID|Q2CONTENTS_WINDOW | FTECONTENTS_BODY | FTECONTENTS_PLAYERCLIP;	/*impacts playerclip*/
		else
			clip.hitcontentsmask = FTECONTENTS_SOLID|Q2CONTENTS_WINDOW | FTECONTENTS_BODY;	//slidebox passes through corpses
	}
	else if (passedict->v->solid == SOLID_CORPSE && w->usesolidcorpse)
		clip.hitcontentsmask = FTECONTENTS_SOLID|Q2CONTENTS_WINDOW | FTECONTENTS_BODY;	//corpses ignore corpses
	else if (passedict->v->solid == SOLID_TRIGGER||passedict->v->solid == SOLID_BSPTRIGGER)
		clip.hitcontentsmask = FTECONTENTS_SOLID|Q2CONTENTS_WINDOW | FTECONTENTS_BODY;	//triggers ignore corpses too, apparently
	else
		clip.hitcontentsmask = FTECONTENTS_SOLID|Q2CONTENTS_WINDOW | FTECONTENTS_BODY | FTECONTENTS_CORPSE; //regular projectiles.
	clip.capsule = (passedict->xv->geomtype == GEOMTYPE_CAPSULE);

	if (type & MOVE_OTHERONLY)
	{
		wedict_t *other = WEDICT_NUM_UB(w->progs, *w->g.other);
		return World_ClipMoveToEntity (w, other, other->v->origin, other->v->angles, start, mins, maxs, end, hullnum, type & MOVE_HITMODEL, clip.capsule, clip.hitcontentsmask);
	}
#ifdef HAVE_LEGACY
	if ((type&MOVE_WORLDONLY) == MOVE_WORLDONLY)
	{	//for compat with DP
		wedict_t *other = w->edicts;
		return World_ClipMoveToEntity (w, other, other->v->origin, other->v->angles, start, mins, maxs, end, hullnum, type & MOVE_HITMODEL, clip.capsule, clip.hitcontentsmask);
	}
#endif

// clip to world
	clip.trace = World_ClipMoveToEntity (w, w->edicts, w->edicts->v->origin, w->edicts->v->angles, start, mins, maxs, end, hullnum, false, clip.capsule, clip.hitcontentsmask);

	clip.start = start;
	clip.end = end;
	clip.mins = mins;
	clip.maxs = maxs;
	clip.type = type;
	clip.passedict = (passedict!=w->edicts)?passedict:NULL;
	clip.hullnum = 0;//hullnum; //BUG: hexen2's SV_ClipMoveToEntity's move_ent argument is set inconsistantly. This has the effect that the SOLID_BSP's .hull field is used instead of the SOLID_BBOX entity. We can't fix this because hexen2 depends upon it - this is the 'tibet5' bug.
#ifdef Q2SERVER
	clip.q2passedict = NULL;
#endif

	if (type & MOVE_MISSILE)
	{
		if (type & MOVE_NOMONSTERS)
			return clip.trace;	//not sure why you'd really want this, but for the sake of dp compat...

		for (i=0 ; i<3 ; i++)
		{
			clip.mins2[i] = -15;
			clip.maxs2[i] = 15;
		}
	}
	else
	{
		VectorCopy (mins, clip.mins2);
		VectorCopy (maxs, clip.maxs2);
	}
	
// create the bounding box of the entire move
	World_MoveBounds (start, clip.mins2, clip.maxs2, end, clip.boxmins, clip.boxmaxs );

// clip to entities
	if (clip.type & MOVE_EVERYTHING)
		World_ClipToEverything (w, &clip);
	else
	{
		if (clip.type & MOVE_LAGGED)
		{
			clip.type &= ~MOVE_LAGGED;
#ifndef CLIENTONLY
			if (w == &sv.world)
			{
				if (passedict->entnum && passedict->entnum <= sv.allocated_client_slots)
				{
					clip.type |= MOVE_LAGGED;
					w->lagents = svs.clients[passedict->entnum-1].laggedents;
					w->maxlagents = svs.clients[passedict->entnum-1].laggedents_count;
					w->lagentsfrac = svs.clients[passedict->entnum-1].laggedents_frac;
					w->lagentstime = svs.clients[passedict->entnum-1].laggedents_time;
				}
				else if (passedict->v->owner)
				{
					if (passedict->v->owner && passedict->v->owner <= sv.allocated_client_slots)
					{
						clip.type |= MOVE_LAGGED;
						w->lagents = svs.clients[passedict->v->owner-1].laggedents;
						w->maxlagents = svs.clients[passedict->v->owner-1].laggedents_count;
						w->lagentsfrac = svs.clients[passedict->v->owner-1].laggedents_frac;
						w->lagentstime = svs.clients[passedict->v->owner-1].laggedents_time;
					}
				}
			}
#endif
		}
		if (clip.type & MOVE_LAGGED)
		{
			trace_t trace;
			wedict_t *touch;
			vec3_t lp, la;
			int j;

#ifdef USEAREAGRID
			World_ClipToAllLinks (w, &clip);
#else
			World_ClipToLinks (w, w->areanodes, &clip);
#endif

			for (i = 0; i < w->maxlagents; i++)
			{
				if (!w->lagents[i].present)
					continue;
				if (clip.trace.allsolid)
					break;

				touch = (wedict_t*)EDICT_NUM_PB(w->progs, i+1);
				if (touch->v->solid == SOLID_NOT)
					continue;
				if (touch == clip.passedict)
					continue;
				if (SOLID_ISTRIGGER(touch->v->solid))
				{
					if (!(clip.type & MOVE_TRIGGERS))
						continue;
					if (!((int)touch->v->flags & FL_FINDABLE_NONSOLID))
						continue;
				}

				if (clip.type & MOVE_NOMONSTERS && touch->v->solid != SOLID_BSP)
					continue;

				if (clip.passedict)
				{
					if (w->usesolidcorpse)
					{
						// don't clip corpse against character
						if (clip.passedict->v->solid == SOLID_CORPSE && (touch->v->solid == SOLID_SLIDEBOX || touch->v->solid == SOLID_CORPSE))
							continue;
						// don't clip character against corpse
						if (clip.passedict->v->solid == SOLID_SLIDEBOX && touch->v->solid == SOLID_CORPSE)
							continue;
					}
					if (!((int)clip.passedict->xv->dimension_hit & (int)touch->xv->dimension_solid))
						continue;
				}

				VectorInterpolate(touch->v->origin, w->lagentsfrac, w->lagents[i].origin, lp);
				//I hate working with angles
				VectorSubtract(w->lagents[i].angles, touch->v->angles, la);
				for (j = 0; j < 3; j++)
				{
					la[j] = (360.0/65536) * ((int)(la[j]*(65536/360.0)) & 65535);
					if (la[j]<-180)
						la[j] += 360;
					if (la[j]>180)
						la[j] -= 360;
				}
				VectorMA(touch->v->angles, 1, la, la);

				if (clip.boxmins[0] > lp[0]+touch->v->maxs[0]
						|| clip.boxmins[1] > lp[1]+touch->v->maxs[1]
						|| clip.boxmins[2] > lp[2]+touch->v->maxs[2]
						|| clip.boxmaxs[0] < lp[0]+touch->v->mins[0]
						|| clip.boxmaxs[1] < lp[1]+touch->v->mins[1]
						|| clip.boxmaxs[2] < lp[2]+touch->v->mins[2] )
					continue;

				if (clip.passedict && clip.passedict->v->size[0] && !touch->v->size[0])
					continue;	// points never interact

				if (clip.passedict)
				{
	 				if ((wedict_t*)PROG_TO_EDICT(w->progs, touch->v->owner) == clip.passedict)
						continue;	// don't clip against own missiles
					if ((wedict_t*)PROG_TO_EDICT(w->progs, clip.passedict->v->owner) == touch)
						continue;	// don't clip against owner
				}

				if ((clip.type & MOVE_HITMODEL) && w->Event_Backdate)
				{
					w->Event_Backdate(w, touch, w->lagentstime);
					trace = World_ClipMoveToEntity (w, touch, lp, la, clip.start, clip.mins, clip.maxs, clip.end, clip.hullnum, clip.type & MOVE_HITMODEL, clip.capsule, clip.hitcontentsmask);
				}
				else
					trace = World_ClipMoveToEntity (w, touch, lp, la, clip.start, clip.mins, clip.maxs, clip.end, clip.hullnum, clip.type & MOVE_HITMODEL, clip.capsule, clip.hitcontentsmask);

				if (trace.allsolid || trace.startsolid || trace.fraction < clip.trace.fraction)
				{
					if (clip.type & MOVE_ENTCHAIN)
					{
						touch->v->chain = EDICT_TO_PROG(w->progs, clip.trace.ent?clip.trace.ent:w->edicts);
						clip.trace.ent = touch;
					}
					else
					{
						trace.ent = touch;
						clip.trace = trace;
					}
				}
			}
		}
		/*else if (w->rbe_hasphysicsents && passedict->rbe.body.body)
		{
			w->rbe->Trace(w, clip.passedict, clip.start, clip.end, &clip.trace);
		}*/
		else
		{
#ifdef USEAREAGRID
			World_ClipToAllLinks (w, &clip );
#else
			World_ClipToLinks (w, w->areanodes, &clip );
#endif
		}
		World_ClipToLinks(w, &w->portallist, &clip);
	}

#if defined(HAVE_CLIENT) && defined(CSQC_DAT)
	{
		extern world_t csqc_world;
		if (w == &csqc_world)
			World_ClipToNetwork(w, &clip);
	}
#endif

//	if (clip.trace.startsolid)
//		clip.trace.fraction = 0;

//	if (!clip.trace.ent)
//		return clip.trace;

	return clip.trace;
}
#ifdef Q2SERVER
trace_t WorldQ2_Move (world_t *w, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int hitcontentsmask, q2edict_t *passedict)
{
	moveclip_t	clip;

	memset ( &clip, 0, sizeof ( moveclip_t ) );

// clip to world
	w->worldmodel->funcs.NativeTrace(w->worldmodel, 0, NULLFRAMESTATE, NULL, start, end, mins, maxs, false, hitcontentsmask, &clip.trace);
	clip.trace.ent = ge->edicts;

	if (clip.trace.fraction == 0)
		return clip.trace;

	clip.start = start;
	clip.end = end;
	clip.mins = mins;
	clip.maxs = maxs;
	clip.type = MOVE_NORMAL;
	clip.hitcontentsmask = hitcontentsmask;
	clip.passedict = NULL;
	clip.q2passedict = passedict;

	VectorCopy (mins, clip.mins2);
	VectorCopy (maxs, clip.maxs2);
	
// create the bounding box of the entire move
//FIXME: should we use clip.trace.endpos here?	
	World_MoveBounds ( start, clip.mins2, clip.maxs2, end, clip.boxmins, clip.boxmaxs );

// clip to entities
	WorldQ2_ClipMoveToEntities(w, &clip);

	return clip.trace;
}
#endif

static void (QDECL *world_current_physics_engine)(world_t*world);
qboolean QDECL World_RegisterPhysicsEngine(const char *enginename, void(QDECL*startupfunc)(world_t*world))
{
	if (world_current_physics_engine)
		return false;	//no thanks, we already have one.
	world_current_physics_engine = startupfunc;
	return true;
}
void World_RBE_Shutdown(world_t *world)
{
#ifdef USERBE
	unsigned int u;
	wedict_t *ed;
	if (!world->rbe)
		return;

	if (world->progs)
	{
		for (u = 0; u < world->num_edicts; u++)
		{
			ed = WEDICT_NUM_PB(world->progs, u);
			world->rbe->RemoveJointFromEntity(world, ed);
			world->rbe->RemoveFromEntity(world, ed);
		}
	}
	world->rbe->End(world);
	world->rbe = NULL;
#endif
}
void QDECL World_UnregisterPhysicsEngine(const char *enginename)
{
#ifdef RAGDOLL
	rag_uninstanciateall();
#endif

#if defined(CSQC_DAT) && !defined(SERVERONLY)
	{
		extern world_t csqc_world;
		World_RBE_Shutdown(&csqc_world);
	}
#endif
#if !defined(CLIENTONLY)
	World_RBE_Shutdown(&sv.world);
#endif

	world_current_physics_engine = NULL;
}
void World_RBE_Start(world_t *world)
{
	if (world_current_physics_engine)
	{
		if (world->worldmodel)
			world_current_physics_engine(world);
	}
}

void World_Destroy(world_t *world)
{
	World_RBE_Shutdown(world);

#ifdef USEAREAGRID
	Z_Free(world->gridareas);
#else
	Z_Free(world->areanodes);
	world->areanodes = NULL;
	world->areanodedepth = 0;
#endif

	memset(world, 0, sizeof(*world));
}

#ifdef USERBE
static qboolean GenerateCollisionMesh_BSP(world_t *world, model_t *mod, wedict_t *ed, vec3_t geomcenter)
{
	unsigned int sno;
	msurface_t *surf;
	mesh_t *mesh;
	unsigned int numverts;
	unsigned int numindexes,i;
	int *ptr_elements;
	float *ptr_verts;

	numverts = 0;
	numindexes = 0;
	for (sno = 0; sno < mod->nummodelsurfaces; sno++)
	{
		surf = &mod->surfaces[sno+mod->firstmodelsurface];
		if (surf->flags & (SURF_DRAWSKY|SURF_DRAWTURB))
			continue;

		//nettest: a headless server's VBSP/Source world has surf->mesh ALLOCATED but UNFILLED (the renderer
		//Batches_Build that fills xyz_array is skipped on a dedicated server) — fall through to the edge path
		//(exactly what Q1 maps use on a dedicated server, where surf->mesh is NULL) instead of dereferencing
		//the NULL xyz_array later (the crash building the ODE world collision mesh on Source maps).
		if (surf->mesh && surf->mesh->xyz_array)
		{
			mesh = surf->mesh;
			numverts += mesh->numvertexes;
			numindexes += mesh->numindexes;
		}
		else
		{
			numverts += surf->numedges;
			numindexes += (surf->numedges-2) * 3;
		}
	}
	if (!numindexes)
	{
		Con_DPrintf("entity %i (classname %s) has no geometry\n", NUM_FOR_EDICT(world->progs, (edict_t*)ed), PR_GetString(world->progs, ed->v->classname));
		return false;
	}
	ptr_elements = (int*)BZ_Malloc(numindexes*sizeof(*ptr_elements));
	ptr_verts = (float*)BZ_Malloc(numverts*sizeof(vec3_t));

	numverts = 0;
	numindexes = 0;
	for (sno = 0; sno < mod->nummodelsurfaces; sno++)
	{
		surf = &mod->surfaces[sno+mod->firstmodelsurface];
		if (surf->flags & (SURF_DRAWSKY|SURF_DRAWTURB))
			continue;

		if (surf->mesh && surf->mesh->xyz_array)	//nettest: see the count loop above (headless VBSP guard)
		{
			mesh = surf->mesh;
			for (i = 0; i < mesh->numvertexes; i++)
				VectorSubtract(mesh->xyz_array[i], geomcenter, (ptr_verts + 3*(numverts+i)));
			for (i = 0; i < mesh->numindexes; i+=3)
			{
				//flip the triangles as we go
				ptr_elements[numindexes+i+0] = numverts+mesh->indexes[i+2];
				ptr_elements[numindexes+i+1] = numverts+mesh->indexes[i+1];
				ptr_elements[numindexes+i+2] = numverts+mesh->indexes[i+0];
			}
			numverts += mesh->numvertexes;
			numindexes += i;
		}
		else
		{
			float *vec;
			medge_t *edge;
			int lindex;
			for (i = 0; i < surf->numedges; i++)
			{
				lindex = mod->surfedges[surf->firstedge + i];

				if (lindex > 0)
				{
					edge = &mod->edges[lindex];
					vec = mod->vertexes[edge->v[0]].position;
				}
				else
				{
					edge = &mod->edges[-lindex];
					vec = mod->vertexes[edge->v[1]].position;
				}
			
				VectorSubtract(vec, geomcenter, (ptr_verts + 3*(numverts+i)));
			}
			for (i = 2; i < surf->numedges; i++)
			{
				//quake is backwards, not ode
				ptr_elements[numindexes++] = numverts+i;
				ptr_elements[numindexes++] = numverts+i-1;
				ptr_elements[numindexes++] = numverts;
			}
			numverts += surf->numedges;
		}
	}

	ed->rbe.element3i = ptr_elements;
	ed->rbe.vertex3f = ptr_verts;
	ed->rbe.numvertices = numverts;
	ed->rbe.numtriangles = numindexes/3;
	return true;
}

#include "com_mesh.h"
static qboolean GenerateCollisionMesh_Alias(world_t *world, model_t *mod, wedict_t *ed, vec3_t geomcenter)
{
	mesh_t mesh;
	unsigned int numverts;
	unsigned int numindexes,i;
	galiasinfo_t *inf;
	unsigned int surfnum = 0;
	entity_t re;
	int *ptr_elements;
	float *ptr_verts;
	float sc;	//nettest Patch 70: entity scale

	numverts = 0;
	numindexes = 0;
	sc = ed->xv->scale; if (sc <= 0) sc = 1;	//Patch 70: scale verts (geomcenter is already in scaled units)

	//fill in the parts of the entity_t that Alias_GAliasBuildMesh needs.
	world->Get_FrameState(world, ed, &re.framestate);
	re.fatness = ed->xv->fatness;
	re.model = mod;

	inf = (galiasinfo_t*)Mod_Extradata (mod);
	while(inf)
	{
		numverts += inf->numverts;
		numindexes += inf->numindexes;
		inf = inf->nextsurf;
	}

	if (!numindexes)
	{
		Con_DPrintf("entity %i (classname %s) has no geometry\n", NUM_FOR_EDICT(world->progs, (edict_t*)ed), PR_GetString(world->progs, ed->v->classname));
		return false;
	}
	ptr_elements = (int*)BZ_Malloc(numindexes*sizeof(*ptr_elements));
	ptr_verts = (float*)BZ_Malloc(numverts*sizeof(vec3_t));

	numverts = 0;
	numindexes = 0;

	inf = (galiasinfo_t*)Mod_Extradata (mod);
	while(inf)
	{
		Alias_GAliasBuildMesh(&mesh, NULL, inf, surfnum++, &re, false);
		for (i = 0; i < mesh.numvertexes; i++)
		{	//Patch 70: vert*sc - geomcenter (geomcenter already scaled)
			vec3_t vs; VectorScale(mesh.xyz_array[i], sc, vs);
			VectorSubtract(vs, geomcenter, (ptr_verts + 3*(numverts+i)));
		}
		for (i = 0; i < mesh.numindexes; i+=3)
		{
			//flip the triangles as we go
			ptr_elements[numindexes+i+0] = numverts+mesh.indexes[i+2];
			ptr_elements[numindexes+i+1] = numverts+mesh.indexes[i+1];
			ptr_elements[numindexes+i+2] = numverts+mesh.indexes[i+0];
		}
		numverts += inf->numverts;
		numindexes += inf->numindexes;
		inf = inf->nextsurf;
	}

	Alias_FlushCache();	//it got built using an entity on the stack, make sure other stuff doesn't get hurt.

	ed->rbe.element3i = ptr_elements;
	ed->rbe.vertex3f = ptr_verts;
	ed->rbe.numvertices = numverts;
	ed->rbe.numtriangles = numindexes/3;
	return true;
}

//Bullet has a fit if we have any degenerate triangles, so make sure we can determine some surface normal
static void CollisionMesh_CleanupMesh(wedict_t *ed)
{
	float *v1, *v2, *v3;
	vec3_t d1, d2, cr;
	int in, out;
	for (in = 0, out = 0; in < ed->rbe.numtriangles*3; in+=3)
	{
		v1 = &ed->rbe.vertex3f[ed->rbe.element3i[in+0]*3];
		v2 = &ed->rbe.vertex3f[ed->rbe.element3i[in+1]*3];
		v3 = &ed->rbe.vertex3f[ed->rbe.element3i[in+2]*3];
		VectorSubtract(v3, v1, d1);
		VectorSubtract(v2, v1, d2);
		CrossProduct(d1, d2, cr);
		if (DotProduct(cr,cr) == 0)
			continue;
		ed->rbe.element3i[out+0] = ed->rbe.element3i[in+0];
		ed->rbe.element3i[out+1] = ed->rbe.element3i[in+1];
		ed->rbe.element3i[out+2] = ed->rbe.element3i[in+2];
		out+=3;
	}
	ed->rbe.numtriangles = out/3;
}

//nettest Patch 67: build the ODE rigid-body collision mesh from the model's LOW-POLY collision
//HULL (the per-submesh convex DECOMPOSITION convhulls[].tris, else the single hulltris) instead
//of the full render mesh. Same shape the player already collides with (World_HullTrace, Patch
//56/61/65), but tens of tris instead of thousands, so the ODE trimesh-vs-world dCollide stops
//dominating the frame (12 props: ~22ms -> ~ms). Mirrors GenerateCollisionMesh_Alias: BZ_Malloc'd
//on the ENGINE heap (freed by World_ReleaseCollisionMesh; the ODE plugin's BZ_Malloc is plain
//malloc, so this MUST live here) and geomcenter-subtracted. The stored hull tris carry no
//ODE-guaranteed winding, so each tri is oriented OUTWARD by its convex piece's centre -> ODE
//gets correct face normals for stable resting. One independent triangle per hull tri (soup);
//ODE handles it, and each conservative-outward piece encloses the model.
static qboolean GenerateCollisionMesh_Hull(world_t *world, model_t *mod, wedict_t *ed, vec3_t geomcenter)
{
	int totaltris = 0, h, t, nv = 0, ni = 0, numgroups, g;
	int *ptr_elements;
	float *ptr_verts;
	qboolean decomp;
	float sc;	//nettest Patch 70: entity scale (assigned after declarations, used to scale the stored verts)
	//nettest Patch 68: default the ODE body to the SINGLE convex hull (mod->hulltris, one clean
	//shell) NOT the convex DECOMPOSITION soup — the soup's many internal/overlapping faces make ODE
	//trimesh-vs-trimesh generate huge contact counts (the "bucket overflow" + O(n^2) cost when props
	//pile up under the gravgun). physics_ode_use_decomp 1 restores the old soup. Player collision is
	//UNCHANGED (World_HullTrace uses convhulls per sv_prop_collision 3).
	static cvar_t *usedecomp;
	if (!usedecomp) usedecomp = Cvar_Get("physics_ode_use_decomp", "0", 0, NULL);
	decomp = (usedecomp && usedecomp->ival) && (mod->numhulls > 0 && mod->convhulls);

	//nettest Patch 70: scale the stored hull verts by the entity scale. geomcenter (com_phys_ode) is
	//already in SCALED units, so store vert*sc - geomcenter -> the ODE shell matches the scaled
	//AABB/mass/offset + the player hull, and a scaled prop sits/sims at its real size (was floating as
	//1.0). The outward-winding test below uses the UNSCALED verts (uniform scale preserves the sign).
	sc = ed->xv->scale; if (sc <= 0) sc = 1;

	if (decomp)
	{
		for (h = 0; h < mod->numhulls; h++)
			totaltris += mod->convhulls[h].numtris;
	}
	else
		totaltris = mod->numhulltris;
	if (totaltris < 1)
		return false;	//no hull tris -> caller falls back to the render mesh

	ptr_verts    = (float*)BZ_Malloc(totaltris*3 * sizeof(vec3_t));
	ptr_elements = (int*)  BZ_Malloc(totaltris*3 * sizeof(int));

	numgroups = decomp ? mod->numhulls : 1;
	for (g = 0; g < numgroups; g++)
	{
		vec3_t *srctris    = decomp ? mod->convhulls[g].tris    : mod->hulltris;
		int     srcnumtris = decomp ? mod->convhulls[g].numtris : mod->numhulltris;
		vec3_t  center;
		if (decomp)
		{	center[0]=(mod->convhulls[g].mins[0]+mod->convhulls[g].maxs[0])*0.5f;
			center[1]=(mod->convhulls[g].mins[1]+mod->convhulls[g].maxs[1])*0.5f;
			center[2]=(mod->convhulls[g].mins[2]+mod->convhulls[g].maxs[2])*0.5f; }
		else
		{	center[0]=(mod->mins[0]+mod->maxs[0])*0.5f;
			center[1]=(mod->mins[1]+mod->maxs[1])*0.5f;
			center[2]=(mod->mins[2]+mod->maxs[2])*0.5f; }

		for (t = 0; t < srcnumtris; t++)
		{
			float *a = srctris[t*3+0], *b = srctris[t*3+1], *c = srctris[t*3+2];
			vec3_t d1, d2, n, ctr, outward, as, bs, cs;
			VectorSubtract(b, a, d1);
			VectorSubtract(c, a, d2);
			CrossProduct(d1, d2, n);
			ctr[0]=(a[0]+b[0]+c[0])*(1.0f/3.0f); ctr[1]=(a[1]+b[1]+c[1])*(1.0f/3.0f); ctr[2]=(a[2]+b[2]+c[2])*(1.0f/3.0f);
			VectorSubtract(ctr, center, outward);
			//Patch 70: store vert*sc - geomcenter (geomcenter is already scaled); winding decided above on the unscaled verts.
			VectorScale(a, sc, as); VectorScale(b, sc, bs); VectorScale(c, sc, cs);
			VectorSubtract(as, geomcenter, (ptr_verts + 3*(nv+0)));
			if (DotProduct(n, outward) >= 0)
			{	//winding already gives an outward normal -> keep a,b,c
				VectorSubtract(bs, geomcenter, (ptr_verts + 3*(nv+1)));
				VectorSubtract(cs, geomcenter, (ptr_verts + 3*(nv+2)));
			}
			else
			{	//inward -> flip to a,c,b so ODE's face normal points out
				VectorSubtract(cs, geomcenter, (ptr_verts + 3*(nv+1)));
				VectorSubtract(bs, geomcenter, (ptr_verts + 3*(nv+2)));
			}
			ptr_elements[ni+0]=nv+0; ptr_elements[ni+1]=nv+1; ptr_elements[ni+2]=nv+2;
			nv += 3; ni += 3;
		}
	}

	ed->rbe.element3i    = ptr_elements;
	ed->rbe.vertex3f     = ptr_verts;
	ed->rbe.numvertices  = nv;
	ed->rbe.numtriangles = ni/3;
	return true;
}

qboolean QDECL World_GenerateCollisionMesh(world_t *world, model_t *mod, wedict_t *ed, vec3_t geomcenter)
{
	qboolean result;
	//nettest Patch 67: physics_ode_trimesh_from_hull 1 (default; registered by the ODE plugin,
	//read here at body build, reload to apply) -> ODE simulates the low-poly collision HULL. Only
	//mod_alias (IQM/MD3) carries hull tris; brush/other always use their existing collision mesh.
	static cvar_t *odehull;
	if (!odehull)
		odehull = Cvar_Get("physics_ode_trimesh_from_hull", "1", 0, NULL);
	if (odehull && odehull->ival == 1 && mod->type == mod_alias &&
		((mod->numhulls > 0 && mod->convhulls) || mod->numhulltris > 0) &&
		GenerateCollisionMesh_Hull(world, mod, ed, geomcenter))
		result = true;
	else switch(mod->type)
	{
	case mod_brush:
		result = GenerateCollisionMesh_BSP(world, mod, ed, geomcenter);
		break;
	case mod_alias:
		result = GenerateCollisionMesh_Alias(world, mod, ed, geomcenter);
		break;
	case mod_heightmap:
	case mod_halflife:
	case mod_sprite:
	case mod_dummy:
	default:
		return false;	//panic!
	}

	if (result)
	{
		CollisionMesh_CleanupMesh(ed);
		if (ed->rbe.numtriangles > 0)
			return true;
	}
	return false;
}
void QDECL World_ReleaseCollisionMesh(wedict_t *ed)
{
	BZ_Free(ed->rbe.element3i);
	ed->rbe.element3i = NULL;
	BZ_Free(ed->rbe.vertex3f);
	ed->rbe.vertex3f = NULL;
	ed->rbe.numvertices = 0;
	ed->rbe.numtriangles = 0;
}
#endif
#endif
