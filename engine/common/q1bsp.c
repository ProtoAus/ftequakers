#include "quakedef.h"

#include "pr_common.h"

#include "shader.h"
#include "com_bih.h"

extern cvar_t r_decal_noperpendicular;
extern cvar_t mod_loadsurfenvmaps;
extern cvar_t mod_loadmappackages;

/*
Decal functions
*/

#define MAXFRAGMENTVERTS 360
int Fragment_ClipPolyToPlane(float *inverts, float *outverts, int incount, float *plane, float planedist)
{
#define C (sizeof(vecV_t)/sizeof(vec_t))
	float dotv[MAXFRAGMENTVERTS+1];
	char keep[MAXFRAGMENTVERTS+1];
#define KEEP_KILL 0
#define KEEP_KEEP 1
#define KEEP_BORDER 2
	int i;
	int outcount = 0;
	int clippedcount = 0;
	float d, *p1, *p2, *out;
#define FRAG_EPSILON (1.0/32) //0.5

	for (i = 0; i < incount; i++)
	{
		dotv[i] = DotProduct((inverts+i*C), plane) - planedist;
		if (dotv[i]<-FRAG_EPSILON)
		{
			keep[i] = KEEP_KILL;
			clippedcount++;
		}
		else if (dotv[i] > FRAG_EPSILON)
			keep[i] = KEEP_KEEP;
		else
			keep[i] = KEEP_BORDER;
	}
	dotv[i] = dotv[0];
	keep[i] = keep[0];

	if (clippedcount == incount)
		return 0;	//all were clipped
	if (clippedcount == 0)
	{	//none were clipped
		for (i = 0; i < incount; i++)
			VectorCopy((inverts+i*C), (outverts+i*C));
		return incount;
	}

	for (i = 0; i < incount; i++)
	{
		p1 = inverts+i*C;
		if (keep[i] == KEEP_BORDER)
		{
			out = outverts+outcount++*C;
			VectorCopy(p1, out);
			continue;
		}
		if (keep[i] == KEEP_KEEP)
		{
			out = outverts+outcount++*C;
			VectorCopy(p1, out);
		}
		if (keep[i+1] == KEEP_BORDER || keep[i] == keep[i+1])
			continue;
		p2 = inverts+((i+1)%incount)*C;
		d = dotv[i] - dotv[i+1];
		if (d)
			d = dotv[i] / d;

		out = outverts+outcount++*C;
		VectorInterpolate(p1, d, p2, out);
	}
	return outcount;
}

//the plane itself must be a vec4_t, but can have other data packed between
size_t Fragment_ClipPlaneToBrush(vecV_t *points, size_t maxpoints, void *planes, size_t planestride, size_t numplanes, vec4_t face)
{
	int p, a;
	vecV_t verts[MAXFRAGMENTVERTS];
	vecV_t verts2[MAXFRAGMENTVERTS];
	vecV_t *cverts;
	int flip;
//	vec3_t d1, d2, n;
	size_t numverts;

	//generate some huge quad/poly aligned with the plane
	vec3_t tmp;
	vec3_t right, forward;
	double t;
	float *plane;

//	if (face[2] != 1)
//		return 0;

	t = fabs(face[2]);
	if (t > fabs(face[0]) && t > fabs(face[1]))
		VectorSet(tmp, 1, 0, 0);
	else
		VectorSet(tmp, 0, 0, 1);

	CrossProduct(face, tmp, right);
	VectorNormalize(right);
	CrossProduct(face, right, forward);
	VectorNormalize(forward);

	VectorScale(face, face[3],			verts[0]);
	VectorMA(verts[0], 32767, right,		verts[0]);
	VectorMA(verts[0], 32767, forward,	verts[0]);

	VectorScale(face, face[3],			verts[1]);
	VectorMA(verts[1], 32767, right,		verts[1]);
	VectorMA(verts[1], -32767, forward,	verts[1]);

	VectorScale(face, face[3],			verts[2]);
	VectorMA(verts[2], -32767, right,	verts[2]);
	VectorMA(verts[2], -32767, forward,	verts[2]);

	VectorScale(face, face[3],			verts[3]);
	VectorMA(verts[3], -32767, right,	verts[3]);
	VectorMA(verts[3], 32767, forward,	verts[3]);

	numverts = 4;


	//clip the quad to the various other planes
	flip = 0;
	for (p = 0; p < numplanes; p++)
	{
		plane = (float*)((qbyte*)planes + p*planestride);
		if (plane != face)
		{
			vec3_t norm;
			flip^=1;
			VectorNegate(plane, norm);
			if (flip)
				numverts = Fragment_ClipPolyToPlane((float*)verts, (float*)verts2, numverts, norm, -plane[3]);
			else
				numverts = Fragment_ClipPolyToPlane((float*)verts2, (float*)verts, numverts, norm, -plane[3]);

			if (numverts < 3)	//totally clipped.
				return 0;
		}
	}

	if (numverts > maxpoints)
		return 0;

	if (flip)
		cverts = verts2;
	else
		cverts = verts;
	for (p = 0; p < numverts; p++)
	{
		for (a = 0; a < 3; a++)
		{
			float f = cverts[p][a];
			int rounded = floor(f + 0.5);
			//if its within 1/1000th of a qu, just round it.
			if (fabs(f - rounded) < 0.001)
				points[p][a] = rounded;
			else
				points[p][a] = f;
		}
	}

	return numverts;
}

#ifdef HAVE_CLIENT

#define MAXFRAGMENTTRIS 256
vec3_t decalfragmentverts[MAXFRAGMENTTRIS*3];

struct fragmentdecal_s
{
	vec3_t center;

	vec3_t normal;
//	vec3_t tangent1;
//	vec3_t tangent2;

	vec3_t planenorm[6];
	float planedist[6];
	int numplanes;

	vec_t radius;

	//will only appear on surfaces with the matching surfaceflag
	unsigned int surfflagmask;
	unsigned int surfflagmatch;

	void (*callback)(void *ctx, vec3_t *fte_restrict points, size_t numpoints, shader_t *shader);
	void *ctx;

	const msurface_t *surf;	//nettest: surface being clipped against (for r_decal_lightmap)
};

//nettest: the surface the current decal fragment belongs to, threaded out-of-band so the
//ABI-fixed decal callback signature stays unchanged.  Set right before each callback in
//Fragment_ClipPoly; read by CL_AddDecal_Callback for per-pixel lightmap sampling.
const msurface_t *Mod_Decal_CurrentSurface;

//#define SHOWCLIPS
//#define FRAGMENTASTRIANGLES	//works, but produces more fragments.

#ifdef FRAGMENTASTRIANGLES

//if the triangle is clipped away, go recursive if there are tris left.
static void Fragment_ClipTriToPlane(int trinum, float *plane, float planedist, fragmentdecal_t *dec)
{
	float *point[3];
	float dotv[3];

	vec3_t impact1, impact2;
	float t;

	int i, i2, i3;
	int clippedverts = 0;

	for (i = 0; i < 3; i++)
	{
		point[i] = decalfragmentverts[trinum*3+i];
		dotv[i] = DotProduct(point[i], plane)-planedist;
		clippedverts += dotv[i] < 0;
	}

	//if they're all clipped away, scrap the tri
	switch (clippedverts)
	{
	case 0:
		return;	//plane does not clip the triangle.

	case 1:	//split into 3, disregard the clipped vert
		for (i = 0; i < 3; i++)
		{
			if (dotv[i] < 0)
			{	//This is the vertex that's getting clipped.

				if (dotv[i] > -DIST_EPSILON)
					return;	//it's only over the line by a tiny ammount.

				i2 = (i+1)%3;
				i3 = (i+2)%3;

				if (dotv[i2] < DIST_EPSILON)
					return;
				if (dotv[i3] < DIST_EPSILON)
					return;

				//work out where the two lines impact the plane
				t = (dotv[i]) / (dotv[i]-dotv[i2]);
				VectorInterpolate(point[i], t, point[i2], impact1);

				t = (dotv[i]) / (dotv[i]-dotv[i3]);
				VectorInterpolate(point[i], t, point[i3], impact2);

#ifdef SHOWCLIPS
				if (dec->numtris != MAXFRAGMENTTRIS)
				{
					VectorCopy(impact2,					decalfragmentverts[dec->numtris*3+0]);
					VectorCopy(decalfragmentverts[trinum*3+i],	decalfragmentverts[dec->numtris*3+1]);
					VectorCopy(impact1,					decalfragmentverts[dec->numtris*3+2]);
					dec->numtris++;
				}
#endif


				//shrink the tri, putting the impact into the killed vertex.
				VectorCopy(impact2, point[i]);


				if (dec->numtris == MAXFRAGMENTTRIS)
					return;	//:(

				//build the second tri
				VectorCopy(impact1,					decalfragmentverts[dec->numtris*3+0]);
				VectorCopy(decalfragmentverts[trinum*3+i2],	decalfragmentverts[dec->numtris*3+1]);
				VectorCopy(impact2,					decalfragmentverts[dec->numtris*3+2]);
				dec->numtris++;

				return;
			}
		}
		Sys_Error("Fragment_ClipTriToPlane: Clipped vertex not founc\n");
		return;	//can't handle it
	case 2:	//split into 3, disregarding both the clipped.
		for (i = 0; i < 3; i++)
		{
			if (!(dotv[i] < 0))
			{	//This is the vertex that's staying.

				if (dotv[i] < DIST_EPSILON)
					break;	//only just inside

				i2 = (i+1)%3;
				i3 = (i+2)%3;

				//work out where the two lines impact the plane
				t = (dotv[i]) / (dotv[i]-dotv[i2]);
				VectorInterpolate(point[i], t, point[i2], impact1);

				t = (dotv[i]) / (dotv[i]-dotv[i3]);
				VectorInterpolate(point[i], t, point[i3], impact2);

				//shrink the tri, putting the impact into the killed vertex.

#ifdef SHOWCLIPS
				if (dec->numtris != MAXFRAGMENTTRIS)
				{
					VectorCopy(impact1,					decalfragmentverts[dec->numtris*3+0]);
					VectorCopy(point[i2],	decalfragmentverts[dec->numtris*3+1]);
					VectorCopy(point[i3],					decalfragmentverts[dec->numtris*3+2]);
					dec->numtris++;
				}
				if (dec->numtris != MAXFRAGMENTTRIS)
				{
					VectorCopy(impact1,					decalfragmentverts[dec->numtris*3+0]);
					VectorCopy(point[i3],	decalfragmentverts[dec->numtris*3+1]);
					VectorCopy(impact2,					decalfragmentverts[dec->numtris*3+2]);
					dec->numtris++;
				}
#endif

				VectorCopy(impact1, point[i2]);
				VectorCopy(impact2, point[i3]);
				return;
			}
		}
	case 3://scrap it
		//fill the verts with the verts of the last and go recursive (due to the nature of Fragment_ClipTriangle, which doesn't actually know if we clip them away)
#ifndef SHOWCLIPS
		dec->numtris--;
		VectorCopy(decalfragmentverts[dec->numtris*3+0], decalfragmentverts[trinum*3+0]);
		VectorCopy(decalfragmentverts[dec->numtris*3+1], decalfragmentverts[trinum*3+1]);
		VectorCopy(decalfragmentverts[dec->numtris*3+2], decalfragmentverts[trinum*3+2]);
		if (trinum < dec->numtris)
			Fragment_ClipTriToPlane(trinum, plane, planedist, dec);
#endif
		return;
	}
}

static void Fragment_ClipTriangle(fragmentdecal_t *dec, float *a, float *b, float *c)
{
	//emit the triangle, and clip it's fragments.
	int start, i;

	int p;

	if (dec->numtris == MAXFRAGMENTTRIS)
		return;	//:(

	start = dec->numtris;

	VectorCopy(a, decalfragmentverts[dec->numtris*3+0]);
	VectorCopy(b, decalfragmentverts[dec->numtris*3+1]);
	VectorCopy(c, decalfragmentverts[dec->numtris*3+2]);
	dec->numtris++;

	//clip all the fragments to all of the planes.
	//This will produce a quad if the source triangle was big enough.

	for (p = 0; p < 6; p++)
	{
		for (i = start; i < dec->numtris; i++)
			Fragment_ClipTriToPlane(i, dec->planenorm[p], dec->plantdist[p], dec);
	}
}

#else

void Fragment_ClipPoly(fragmentdecal_t *dec, int numverts, float *inverts, shader_t *surfshader)
{
	//emit the triangle, and clip it's fragments.
	int p;
	float verts[MAXFRAGMENTVERTS*C];
	float verts2[MAXFRAGMENTVERTS*C];
	float *cverts;
	int flip;
	vec3_t d1, d2, n;
	size_t numtris;

	if (numverts > MAXFRAGMENTTRIS)
		return;

	if (r_decal_noperpendicular.ival)
	{
		VectorSubtract(inverts+C*1, inverts+C*0, d1);
		for (p = 2; ; p++)
		{
			if (p >= numverts)
				return;
			VectorSubtract(inverts+C*p, inverts+C*0, d2);
			CrossProduct(d1, d2, n);
			if (DotProduct(n,n)>.1)
				break;
		}
		VectorNormalizeFast(n);
		if (DotProduct(n, dec->normal) < 0.1)
			return;	//faces too far way from the normal
	}

	//clip to the first plane specially, so we don't have extra copys
	numverts = Fragment_ClipPolyToPlane(inverts, verts, numverts, dec->planenorm[0], dec->planedist[0]);

	//clip the triangle to the 6 planes.
	flip = 0;
	for (p = 1; p < dec->numplanes; p++)
	{
		flip^=1;
		if (flip)
			numverts = Fragment_ClipPolyToPlane(verts, verts2, numverts, dec->planenorm[p], dec->planedist[p]);
		else
			numverts = Fragment_ClipPolyToPlane(verts2, verts, numverts, dec->planenorm[p], dec->planedist[p]);

		if (numverts < 3)	//totally clipped.
			return;
	}

	if (flip)
		cverts = verts2;
	else
		cverts = verts;

	//decompose the resultant polygon into triangles.

	numtris = 0;
	while(numverts-->2)
	{
		if (numtris == MAXFRAGMENTTRIS)
		{
			Mod_Decal_CurrentSurface = dec->surf;	//nettest: r_decal_lightmap
			dec->callback(dec->ctx, decalfragmentverts, numtris, NULL);
			numtris = 0;
			break;
		}

		VectorCopy((cverts+C*0),			decalfragmentverts[numtris*3+0]);
		VectorCopy((cverts+C*(numverts-1)),	decalfragmentverts[numtris*3+1]);
		VectorCopy((cverts+C*numverts),		decalfragmentverts[numtris*3+2]);
		numtris++;
	}
	if (numtris)
	{
		Mod_Decal_CurrentSurface = dec->surf;	//nettest: r_decal_lightmap
		dec->callback(dec->ctx, decalfragmentverts, numtris, surfshader);
	}
}

#endif

//this could be inlined, but I'm lazy.
static void Fragment_Mesh (fragmentdecal_t *dec, const msurface_t *surf, mesh_t *mesh, mtexinfo_t *texinfo)
{
	int i;
	vecV_t verts[3];
	shader_t *surfshader = texinfo->texture->shader;

	dec->surf = surf;	//nettest: r_decal_lightmap — threaded to the callback via Mod_Decal_CurrentSurface

	if ((surfshader->flags & SHADER_NOMARKS) || !mesh)
		return;

	if (dec->surfflagmask)
	{
		if ((texinfo->flags & dec->surfflagmask) != dec->surfflagmatch)
			return;
	}

	/*if its a triangle fan/poly/quad then we can just submit the entire thing without generating extra fragments*/
	if (mesh->istrifan)
	{
		Fragment_ClipPoly(dec, mesh->numvertexes, mesh->xyz_array[0], surfshader);
		return;
	}

	//Fixme: optimise q3 patches (quad strips with bends between each strip)

	/*otherwise it goes in and out in weird places*/
	for (i = 0; i < mesh->numindexes; i+=3)
	{
		VectorCopy(mesh->xyz_array[mesh->indexes[i+0]], verts[0]);
		VectorCopy(mesh->xyz_array[mesh->indexes[i+1]], verts[1]);
		VectorCopy(mesh->xyz_array[mesh->indexes[i+2]], verts[2]);
		Fragment_ClipPoly(dec, 3, verts[0], surfshader);
	}
}

#ifdef Q1BSPS
static void Q1BSP_ClipDecalToNodes (model_t *mod, fragmentdecal_t *dec, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;

	if (node->contents < 0)
		return;

	splitplane = node->plane;
	dist = DotProduct (dec->center, splitplane->normal) - splitplane->dist;

	if (dist > dec->radius)
	{
		Q1BSP_ClipDecalToNodes (mod, dec, node->children[0]);
		return;
	}
	if (dist < -dec->radius)
	{
		Q1BSP_ClipDecalToNodes (mod, dec, node->children[1]);
		return;
	}

// mark the polygons
	surf = mod->surfaces + node->firstsurface;
	if (r_decal_noperpendicular.ival)
	{
		for (i=0 ; i<node->numsurfaces ; i++, surf++)
		{
			if (surf->flags & SURF_PLANEBACK)
			{
				if (-DotProduct(surf->plane->normal, dec->normal) > -0.5)
					continue;
			}
			else
			{
				if (DotProduct(surf->plane->normal, dec->normal) > -0.5)
					continue;
			}
			Fragment_Mesh(dec, surf, surf->mesh, surf->texinfo);
		}
	}
	else
	{
		for (i=0 ; i<node->numsurfaces ; i++, surf++)
			Fragment_Mesh(dec, surf, surf->mesh, surf->texinfo);
	}

	Q1BSP_ClipDecalToNodes (mod, dec, node->children[0]);
	Q1BSP_ClipDecalToNodes (mod, dec, node->children[1]);
}
#endif

#ifdef RTLIGHTS
extern int sh_shadowframe;
#else
static int sh_shadowframe;
#endif
#ifdef Q3BSPS
static void Q3BSP_ClipDecalToNodes (fragmentdecal_t *dec, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	**msurf;
	msurface_t	*surf;
	mleaf_t		*leaf;
	int			i;

	if (node->contents != -1)
	{
		leaf = (mleaf_t *)node;
	// mark the polygons
		msurf = leaf->firstmarksurface;
		for (i=0 ; i<leaf->nummarksurfaces ; i++, msurf++)
		{
			surf = *msurf;

#ifdef RTLIGHTS
			//only check each surface once. it can appear in multiple leafs.
			if (surf->shadowframe == sh_shadowframe)
				continue;
			surf->shadowframe = sh_shadowframe;
#endif
			Fragment_Mesh(dec, surf, surf->mesh, surf->texinfo);
		}
		return;
	}

	splitplane = node->plane;
	dist = DotProduct (dec->center, splitplane->normal) - splitplane->dist;

	if (dist > dec->radius)
	{
		Q3BSP_ClipDecalToNodes (dec, node->children[0]);
		return;
	}
	if (dist < -dec->radius)
	{
		Q3BSP_ClipDecalToNodes (dec, node->children[1]);
		return;
	}
	Q3BSP_ClipDecalToNodes (dec, node->children[0]);
	Q3BSP_ClipDecalToNodes (dec, node->children[1]);
}
#endif

void Mod_ClipDecal(struct model_s *mod, vec3_t center, vec3_t normal, vec3_t tangent1, vec3_t tangent2, float size, unsigned int surfflagmask, unsigned int surfflagmatch, void (*callback)(void *ctx, vec3_t *fte_restrict points, size_t numpoints, shader_t *shader), void *ctx)
{	//quad marks a full, independant quad
	int p;
	float r;
	fragmentdecal_t dec;

	VectorCopy(center, dec.center);
	VectorCopy(normal, dec.normal);
	dec.radius = 0;
	dec.callback = callback;
	dec.ctx = ctx;
	dec.surfflagmask = surfflagmask;
	dec.surfflagmatch = surfflagmatch;

	VectorCopy(tangent1,	dec.planenorm[0]);
	VectorNegate(tangent1,	dec.planenorm[1]);
	VectorCopy(tangent2,	dec.planenorm[2]);
	VectorNegate(tangent2,	dec.planenorm[3]);
	VectorCopy(dec.normal,		dec.planenorm[4]);
	VectorNegate(dec.normal,	dec.planenorm[5]);
	for (p = 0; p < 6; p++)
	{
		r = sqrt(DotProduct(dec.planenorm[p], dec.planenorm[p]));
		VectorScale(dec.planenorm[p], 1/r, dec.planenorm[p]);
		r*= size/2;
		if (r > dec.radius)
			dec.radius = r;
		dec.planedist[p] = -(r - DotProduct(dec.center, dec.planenorm[p]));
	}
	dec.numplanes = 6;

	sh_shadowframe++;

	if (!mod || mod->loadstate != MLS_LOADED)
		return;
	else if (mod->type != mod_brush)
		;
#ifdef Q1BSPS
	else if (mod->fromgame == fg_quake || mod->fromgame == fg_halflife)
		Q1BSP_ClipDecalToNodes(mod, &dec, mod->rootnode);
#endif
#ifdef Q3BSPS
	else if (mod->fromgame == fg_quake3 || mod->fromgame == fg_quake2 || mod->fromgame == fg_new)
	{
		if (mod->submodelof)
		{
			msurface_t *surf;
			for (surf = mod->surfaces+mod->firstmodelsurface, p = 0; p < mod->nummodelsurfaces; p++, surf++)
				Fragment_Mesh(&dec, surf, surf->mesh, surf->texinfo);
		}
		else
			Q3BSP_ClipDecalToNodes(&dec, mod->rootnode);
	}
#endif

#ifdef TERRAIN
	if (mod->terrain)
		Terrain_ClipDecal(&dec, center, dec.radius, mod);
#endif
}
#endif

/*
Decal functions

============================================================================

Physics functions (common)
*/
#ifdef Q1BSPS

void Q1BSP_CheckHullNodes(hull_t *hull)
{
	int num, c;
	mclipnode_t	*node;
	for (num = hull->firstclipnode; num < hull->lastclipnode; num++)
	{
		node = hull->clipnodes + num;
		for (c = 0; c < 2; c++)
			if (node->children[c] >= 0)
				if (node->children[c] < hull->firstclipnode || node->children[c] > hull->lastclipnode)
					Sys_Error ("Q1BSP_CheckHull: bad node number");

	}
}


static int Q1_ModelPointContents (mnode_t *node, const vec3_t p)
{
	float d;
	mplane_t *plane;
	while(node->contents >= 0)
	{
		plane = node->plane;
		if (plane->type < 3)
			d = p[plane->type] - plane->dist;
		else
			d = DotProduct(plane->normal, p) - plane->dist;
		node = node->children[d<0];
	}
	return node->contents;
}

#endif//Q1BSPS
/*
==================
SV_HullPointContents

==================
*/
static int Q1_HullPointContents (hull_t *hull, int num, const vec3_t p)
{
	float		d;
	mclipnode_t	*node;
	mplane_t	*plane;

	while (num >= 0)
	{
		node = hull->clipnodes + num;
		plane = hull->planes + node->planenum;

		if (plane->type < 3)
			d = p[plane->type] - plane->dist;
		else
			d = DotProduct (plane->normal, p) - plane->dist;
		if (d < 0)
			num = node->children[1];
		else
			num = node->children[0];
	}

	return num;
}

#define	DIST_EPSILON	(0.03125)
#if 1

static const unsigned int q1toftecontents[] =
{
	0,//EMPTY
	FTECONTENTS_SOLID,//SOLID
	FTECONTENTS_WATER,//WATER
	FTECONTENTS_SLIME,//SLIME
	FTECONTENTS_LAVA,//LAVA
	FTECONTENTS_SKY,//SKY
	FTECONTENTS_SOLID,//STRIPPED
	FTECONTENTS_PLAYERCLIP,//CLIP
	Q2CONTENTS_CURRENT_0,//FLOW_1
	Q2CONTENTS_CURRENT_90,//FLOW_2
	Q2CONTENTS_CURRENT_180,//FLOW_3
	Q2CONTENTS_CURRENT_270,//FLOW_4
	Q2CONTENTS_CURRENT_UP,//FLOW_5
	Q2CONTENTS_CURRENT_DOWN,//FLOW_6
	Q2CONTENTS_WINDOW,//TRANS
	FTECONTENTS_LADDER,//LADDER
};

enum
{
	rht_solid,
	rht_empty,
	rht_impact
};
struct rhtctx_s
{
	unsigned int checkcontents;
	vec3_t start, end;
	mclipnode_t	*clipnodes;
	mplane_t	*planes;
};
static int Q1BSP_RecursiveHullTrace (struct rhtctx_s *ctx, int num, float p1f, float p2f, const vec3_t p1, const vec3_t p2, trace_t *trace)
{
	mclipnode_t	*node;
	mplane_t	*plane;
	float		t1, t2;
	vec3_t		mid;
	int			side;
	float		midf;
	int rht;

reenter:

	if (num < 0)
	{
		unsigned int c = q1toftecontents[-1-num];
		/*hit a leaf*/
		if (c & ctx->checkcontents)
		{
			trace->contents = c;
			if (trace->allsolid)
				trace->startsolid = true;
			return rht_solid;
		}
		else
		{
			trace->allsolid = false;
			if (c & FTECONTENTS_FLUID)
				trace->inwater = true;
			else
				trace->inopen = true;
			return rht_empty;
		}
	}

	/*its a node*/

	/*get the node info*/
	node = ctx->clipnodes + num;
	plane = ctx->planes + node->planenum;

	if (plane->type < 3)
	{
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
	}
	else
	{
		t1 = DotProduct (plane->normal, p1) - plane->dist;
		t2 = DotProduct (plane->normal, p2) - plane->dist;
	}

	/*if its completely on one side, resume on that side*/
	if (t1 >= 0 && t2 >= 0)
	{
		//return Q1BSP_RecursiveHullTrace (hull, node->children[0], p1f, p2f, p1, p2, trace);
		num = node->children[0];
		goto reenter;
	}
	if (t1 < 0 && t2 < 0)
	{
		//return Q1BSP_RecursiveHullTrace (hull, node->children[1], p1f, p2f, p1, p2, trace);
		num = node->children[1];
		goto reenter;
	}

	if (plane->type < 3)
	{
		t1 = ctx->start[plane->type] - plane->dist;
		t2 = ctx->end[plane->type] - plane->dist;
	}
	else
	{
		t1 = DotProduct (plane->normal, ctx->start) - plane->dist;
		t2 = DotProduct (plane->normal, ctx->end) - plane->dist;
	}

	side = t1 < 0;

	midf = t1 / (t1 - t2);
	if (midf < p1f) midf = p1f;
	if (midf > p2f) midf = p2f;
	VectorInterpolate(ctx->start, midf, ctx->end, mid);

	//check the near side
	rht = Q1BSP_RecursiveHullTrace(ctx, node->children[side], p1f, midf, p1, mid, trace);
	if (rht != rht_empty && !trace->allsolid)
		return rht;
	//check the far side
	rht = Q1BSP_RecursiveHullTrace(ctx, node->children[side^1], midf, p2f, mid, p2, trace);
	if (rht != rht_solid)
		return rht;

	if (side)
	{
		/*we impacted the back of the node, so flip the plane*/
		trace->plane.dist = -plane->dist;
		VectorNegate(plane->normal, trace->plane.normal);
		midf = (t1 + DIST_EPSILON) / (t1 - t2);
	}
	else
	{
		/*we impacted the front of the node*/
		trace->plane.dist = plane->dist;
		VectorCopy(plane->normal, trace->plane.normal);
		midf = (t1 - DIST_EPSILON) / (t1 - t2);
	}

	t1 = DotProduct (trace->plane.normal, ctx->start) - trace->plane.dist;
	t2 = DotProduct (trace->plane.normal, ctx->end) - trace->plane.dist;
	midf = (t1 - DIST_EPSILON) / (t1 - t2);

	midf = bound(0, midf, 1);
	trace->fraction = midf;
	VectorCopy (mid, trace->endpos);
	VectorInterpolate(ctx->start, midf, ctx->end, trace->endpos);

	return rht_impact;
}

qboolean Q1BSP_RecursiveHullCheck (hull_t *hull, int num, const vec3_t p1, const vec3_t p2, unsigned int hitcontents, trace_t *trace)
{
	if (VectorEquals(p1, p2))
	{
		/*points cannot cross planes, so do it faster*/
		int q1 = Q1_HullPointContents(hull, num, p1);
		unsigned int c = q1toftecontents[-1-q1];
		trace->contents = c;
		if (c & hitcontents)
			trace->startsolid = true;
		else if (c & FTECONTENTS_FLUID)
		{
			trace->allsolid = false;
			trace->inwater = true;
		}
		else
		{
			trace->allsolid = false;
			trace->inopen = true;
		}
		return true;
	}
	else
	{
		struct rhtctx_s ctx;
		VectorCopy(p1, ctx.start);
		VectorCopy(p2, ctx.end);
		ctx.clipnodes = hull->clipnodes;
		ctx.planes = hull->planes;
		ctx.checkcontents = hitcontents;
		return Q1BSP_RecursiveHullTrace(&ctx, num, 0, 1, p1, p2, trace) != rht_impact;
	}
}

#else
qboolean Q1BSP_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace)
{
	mclipnode_t	*node;
	mplane_t	*plane;
	float		t1, t2;
	float		frac;
	int			i;
	vec3_t		mid;
	int			side;
	float		midf;

// check for empty
	if (num < 0)
	{
		if (num != Q1CONTENTS_SOLID)
		{
			trace->allsolid = false;
			if (num == Q1CONTENTS_EMPTY)
				trace->inopen = true;
			else
				trace->inwater = true;
		}
		else
			trace->startsolid = true;
		return true;		// empty
	}

//
// find the point distances
//
	node = hull->clipnodes + num;
	plane = hull->planes + node->planenum;

	if (plane->type < 3)
	{
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
	}
	else
	{
		t1 = DotProduct (plane->normal, p1) - plane->dist;
		t2 = DotProduct (plane->normal, p2) - plane->dist;
	}

#if 1
	if (t1 >= 0 && t2 >= 0)
		return Q1BSP_RecursiveHullCheck (hull, node->children[0], p1f, p2f, p1, p2, trace);
	if (t1 < 0 && t2 < 0)
		return Q1BSP_RecursiveHullCheck (hull, node->children[1], p1f, p2f, p1, p2, trace);
#else
	if ( (t1 >= DIST_EPSILON && t2 >= DIST_EPSILON) || (t2 > t1 && t1 >= 0) )
		return Q1BSP_RecursiveHullCheck (hull, node->children[0], p1f, p2f, p1, p2, trace);
	if ( (t1 <= -DIST_EPSILON && t2 <= -DIST_EPSILON) || (t2 < t1 && t1 <= 0) )
		return Q1BSP_RecursiveHullCheck (hull, node->children[1], p1f, p2f, p1, p2, trace);
#endif

// put the crosspoint DIST_EPSILON pixels on the near side
	if (t1 < 0)
		frac = (t1 + DIST_EPSILON)/(t1-t2);
	else
		frac = (t1 - DIST_EPSILON)/(t1-t2);
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;

	midf = p1f + (p2f - p1f)*frac;
	for (i=0 ; i<3 ; i++)
		mid[i] = p1[i] + frac*(p2[i] - p1[i]);

	side = (t1 < 0);

// move up to the node
	if (!Q1BSP_RecursiveHullCheck (hull, node->children[side], p1f, midf, p1, mid, trace) )
		return false;

#ifdef PARANOID
	if (Q1BSP_RecursiveHullCheck (sv_hullmodel, mid, node->children[side])
	== Q1CONTENTS_SOLID)
	{
		Con_Printf ("mid PointInHullSolid\n");
		return false;
	}
#endif

	if (Q1_HullPointContents (hull, node->children[side^1], mid)
	!= Q1CONTENTS_SOLID)
// go past the node
		return Q1BSP_RecursiveHullCheck (hull, node->children[side^1], midf, p2f, mid, p2, trace);

	if (trace->allsolid)
		return false;		// never got out of the solid area

//==================
// the other side of the node is solid, this is the impact point
//==================
	if (!side)
	{
		VectorCopy (plane->normal, trace->plane.normal);
		trace->plane.dist = plane->dist;
	}
	else
	{
		VectorNegate (plane->normal, trace->plane.normal);
		trace->plane.dist = -plane->dist;
	}

	while (Q1_HullPointContents (hull, hull->firstclipnode, mid)
	== Q1CONTENTS_SOLID)
	{ // shouldn't really happen, but does occasionally
		if (!(frac < 10000000) && !(frac > -10000000))
		{
			trace->fraction = 0;
			VectorClear (trace->endpos);
			Con_Printf ("nan in traceline\n");
			return false;
		}
		frac -= 0.1;
		if (frac < 0)
		{
			trace->fraction = midf;
			VectorCopy (mid, trace->endpos);
			Con_DPrintf ("backup past 0\n");
			return false;
		}
		midf = p1f + (p2f - p1f)*frac;
		for (i=0 ; i<3 ; i++)
			mid[i] = p1[i] + frac*(p2[i] - p1[i]);
	}

	trace->fraction = midf;
	VectorCopy (mid, trace->endpos);

	return false;
}
#endif

#if 0//def Q1BSPS

/*
the bsp tree we're walking through is the renderable hull
we need to trace a box through the world.
by its very nature, this will reach more nodes than we really want, and as we can follow a node sideways, the underlying bsp structure is no longer 100% reliable (meaning we cross planes that are entirely to one side, and follow its children too)
so all contents and solidity must come from the brushes and ONLY the brushes.
*/
struct traceinfo_s
{
	unsigned int solidcontents;
	trace_t trace;

	qboolean capsule;
	float radius;
	/*set even for sphere traces (used for bbox tests)*/
	vec3_t mins;
	vec3_t maxs;

	vec3_t start;
	vec3_t end;

	vec3_t	up;
	vec3_t	capsulesize;
	vec3_t	extents;
};

static void Q1BSP_ClipToBrushes(struct traceinfo_s *traceinfo, mbrush_t *brush)
{
	struct mbrushplane_s *plane;
	struct mbrushplane_s *enterplane;
	int i, j;
	vec3_t ofs;
	qboolean startout, endout;
	float d1,d2,dist,enterdist=0;
	float f, enterfrac, exitfrac;

	for (; brush; brush = brush->next)
	{
		/*ignore if its not solid to us*/
		if (!(traceinfo->solidcontents & brush->contents))
			continue;

		startout = false;
		endout = false;
		enterplane= NULL;
		enterfrac = -1;
		exitfrac = 10;
		for (i = brush->numplanes, plane = brush->planes; i; i--, plane++)
		{
			/*calculate the distance based upon the shape of the object we're tracing for*/
			if (traceinfo->capsule)
			{
				dist = DotProduct(traceinfo->up, plane->normal);
				dist = dist*(traceinfo->capsulesize[(dist<0)?1:2]) - traceinfo->capsulesize[0];
				dist = plane->dist - dist;

				//dist = plane->dist + traceinfo->radius;
			}
			else
			{
				for (j=0 ; j<3 ; j++)
				{
					if (plane->normal[j] < 0)
						ofs[j] = traceinfo->maxs[j];
					else
						ofs[j] = traceinfo->mins[j];
				}
				dist = DotProduct (ofs, plane->normal);
				dist = plane->dist - dist;
			}

			d1 = DotProduct (traceinfo->start, plane->normal) - dist;
			d2 = DotProduct (traceinfo->end, plane->normal) - dist;

			if (d1 >= 0)
				startout = true;
			if (d2 > 0)
				endout = true;

			//if we're fully outside any plane, then we cannot possibly enter the brush, skip to the next one
			if (d1 > 0 && d2 >= 0)
				goto nextbrush;

			//if we're fully inside the plane, then whatever is happening is not relevent for this plane
			if (d1 < 0 && d2 <= 0)
				continue;

			f = d1 / (d1-d2);
			if (d1 > d2)
			{
				//entered the brush. favour the furthest fraction to avoid extended edges (yay for convex shapes)
				if (enterfrac < f)
				{
					enterfrac = f;
					enterplane = plane;
					enterdist = dist;
				}
			}
			else
			{
				//left the brush, favour the nearest plane (smallest frac)
				if (exitfrac > f)
				{
					exitfrac = f;
				}
			}
		}

		if (!startout)
		{
			traceinfo->trace.startsolid = true;
			if (!endout)
				traceinfo->trace.allsolid = true;
			traceinfo->trace.contents |= brush->contents;
			return;
		}
		if (enterfrac != -1 && enterfrac < exitfrac)
		{
			//impact!
			if (enterfrac < traceinfo->trace.fraction)
			{
				traceinfo->trace.fraction = enterfrac;
				traceinfo->trace.plane.dist = enterdist;
				VectorCopy(enterplane->normal, traceinfo->trace.plane.normal);
				traceinfo->trace.contents = brush->contents;
			}
		}
nextbrush:
		;
	}
}
static void Q1BSP_InsertBrush(mnode_t *node, mbrush_t *brush, vec3_t bmins, vec3_t bmaxs)
{
	vec3_t nearp, farp;
	float nd, fd;
	int i;
	while(1)
	{
		if (node->contents < 0) /*leaf, so no smaller node to put it in (I'd be surprised if it got this far)*/
		{
			brush->next = node->brushes;
			node->brushes = brush;
			return;
		}

		for (i = 0; i < 3; i++)
		{
			if (node->plane->normal[i] > 0)
			{
				nearp[i] = bmins[i];
				farp[i] = bmaxs[i];
			}
			else
			{
				nearp[i] = bmaxs[i];
				farp[i] = bmins[i];
			}
		}

		nd = DotProduct(node->plane->normal, nearp) - node->plane->dist;
		fd = DotProduct(node->plane->normal, farp) - node->plane->dist;

		/*if its fully on either side, continue walking*/
		if (nd < 0 && fd < 0)
			node = node->children[1];
		else if (nd > 0 && fd > 0)
			node = node->children[0];
		else
		{
			/*plane crosses bbox, so insert here*/
			brush->next = node->brushes;
			node->brushes = brush;
			return;
		}
	}
}
static void Q1BSP_RecursiveBrushCheck (struct traceinfo_s *traceinfo, mnode_t *node, float p1f, float p2f, const vec3_t p1, const vec3_t p2)
{
	mplane_t	*plane;
	float		t1, t2;
	float		frac;
	int			i;
	vec3_t		mid;
	int			side;
	float		midf;
	float		offset;

	if (node->brushes)
	{
		Q1BSP_ClipToBrushes(traceinfo, node->brushes);
	}

	if (traceinfo->trace.fraction < p1f)
	{
		//already hit something closer than this node
		return;
	}

	if (node->contents < 0)
	{
		//we're in a leaf
		return;
	}

//
// find the point distances
//
	plane = node->plane;

	if (plane->type < 3)
	{
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
		if (plane->normal[plane->type] < 0)
			offset = -traceinfo->mins[plane->type];
		else
			offset = traceinfo->maxs[plane->type];
	}
	else
	{
		t1 = DotProduct (plane->normal, p1) - plane->dist;
		t2 = DotProduct (plane->normal, p2) - plane->dist;
		offset = 0;
		for (i = 0; i < 3; i++)
		{
			if (plane->normal[i] < 0)
				offset += plane->normal[i] * -traceinfo->mins[i];
			else
				offset += plane->normal[i] * traceinfo->maxs[i];
		}
	}

	/*if we're fully on one side of the trace, go only down that side*/
	if (t1 >= offset && t2 >= offset)
	{
		Q1BSP_RecursiveBrushCheck (traceinfo, node->children[0], p1f, p2f, p1, p2);
		return;
	}
	if (t1 < -offset && t2 < -offset)
	{
		Q1BSP_RecursiveBrushCheck (traceinfo, node->children[1], p1f, p2f, p1, p2);
		return;
	}

// put the crosspoint DIST_EPSILON pixels on the near side
	if (t1 == t2)
	{
		side = 0;
		frac = 0;
	}
	else if (t1 < 0)
	{
		frac = (t1 + DIST_EPSILON)/(t1-t2);
		side = 1;
	}
	else
	{
		frac = (t1 - DIST_EPSILON)/(t1-t2);
		side = 0;
	}
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;

	midf = p1f + (p2f - p1f)*frac;
	for (i=0 ; i<3 ; i++)
		mid[i] = p1[i] + frac*(p2[i] - p1[i]);

// move up to the node
	Q1BSP_RecursiveBrushCheck (traceinfo, node->children[side], p1f, midf, p1, mid);

// go past the node
	Q1BSP_RecursiveBrushCheck (traceinfo, node->children[side^1], midf, p2f, mid, p2);
}
#endif	//Q1BSPS

static unsigned int Q1BSP_TranslateContents(enum q1contents_e contents)
{
	safeswitch(contents)
	{
	case Q1CONTENTS_EMPTY:			return FTECONTENTS_EMPTY;
	case Q1CONTENTS_SOLID:			return FTECONTENTS_SOLID;
	case Q1CONTENTS_WATER:			return FTECONTENTS_WATER;
	case Q1CONTENTS_SLIME:			return FTECONTENTS_SLIME;
	case Q1CONTENTS_LAVA:			return FTECONTENTS_LAVA;
	case Q1CONTENTS_SKY:			return FTECONTENTS_SKY|FTECONTENTS_PLAYERCLIP|FTECONTENTS_MONSTERCLIP;
	case Q1CONTENTS_LADDER:			return FTECONTENTS_LADDER;
	case HLCONTENTS_CLIP:			return FTECONTENTS_PLAYERCLIP|FTECONTENTS_MONSTERCLIP;
	case HLCONTENTS_CURRENT_0:		return FTECONTENTS_WATER|Q2CONTENTS_CURRENT_0;		//q2 is better than nothing, right?
	case HLCONTENTS_CURRENT_90:		return FTECONTENTS_WATER|Q2CONTENTS_CURRENT_90;
	case HLCONTENTS_CURRENT_180:	return FTECONTENTS_WATER|Q2CONTENTS_CURRENT_180;
	case HLCONTENTS_CURRENT_270:	return FTECONTENTS_WATER|Q2CONTENTS_CURRENT_270;
	case HLCONTENTS_CURRENT_UP:		return FTECONTENTS_WATER|Q2CONTENTS_CURRENT_UP;
	case HLCONTENTS_CURRENT_DOWN:	return FTECONTENTS_WATER|Q2CONTENTS_CURRENT_DOWN;
	case HLCONTENTS_TRANS:			return FTECONTENTS_EMPTY;
	case Q1CONTENTS_MONSTERCLIP:	return FTECONTENTS_MONSTERCLIP;
	case Q1CONTENTS_PLAYERCLIP:		return FTECONTENTS_PLAYERCLIP;
	case Q1CONTENTS_CORPSE:			return FTECONTENTS_CORPSE;

	safedefault:
		Con_Printf("Q1BSP_TranslateContents: Unknown contents type - %i", contents);
		return FTECONTENTS_SOLID;
	}
}

int Q1BSP_HullPointContents(hull_t *hull, const vec3_t p)
{
	return Q1BSP_TranslateContents(Q1_HullPointContents(hull, hull->firstclipnode, p));
}

#ifdef Q1BSPS
unsigned int Q1BSP_PointContents(model_t *model, const vec3_t axis[3], const vec3_t point)
{
	int contents;
	if (axis)
	{
		vec3_t transformed;
		transformed[0] = DotProduct(point, axis[0]);
		transformed[1] = DotProduct(point, axis[1]);
		transformed[2] = DotProduct(point, axis[2]);
		return Q1BSP_PointContents(model, NULL, transformed);
	}
	else
	{
		if (!model->firstmodelsurface)
		{
			contents = Q1BSP_TranslateContents(Q1_ModelPointContents(model->nodes, point));
		}
		else
			contents = Q1BSP_HullPointContents(&model->hulls[0], point);
	}
#ifdef TERRAIN
	if (model->terrain)
		contents |= Heightmap_PointContents(model, NULL, point);
#endif
	return contents;
}

void Q1BSP_LoadBrushes(model_t *model, bspx_header_t *bspx, void *mod_base)
{
	const struct {
		unsigned int ver;
		unsigned int modelnum;
		unsigned int numbrushes;
		unsigned int numplanes;
	} *srcmodel;
	const struct {
		float mins[3];
		float maxs[3];
		signed short contents;
		unsigned short numplanes;
	} *srcbrush;
	/*
	Note to implementors:
	a pointy brush with angles pointier than 90 degrees will extend further than any adjacent brush, thus creating invisible walls with larger expansions.
	the engine inserts 6 axial planes acording to the bbox, thus the qbsp need not write any axial planes
	note that doing it this way probably isn't good if you want to query textures...
	*/
	const struct{
		vec3_t normal;
		float dist;
	} *srcplane;

	static vec3_t axis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	unsigned int br, pl;
	q2cbrush_t *brush;
	q2cbrushside_t *sides;	//grr!
	mplane_t *planes;	//bulky?
	size_t lumpsizeremaining;
	unsigned int numplanes;

	unsigned int srcver, srcmodelidx, modbrushes, modplanes;

	srcmodel = BSPX_FindLump(bspx, mod_base, "BRUSHLIST", &lumpsizeremaining);
	if (!srcmodel)
		return;

	while (lumpsizeremaining)
	{
		if (lumpsizeremaining < sizeof(*srcmodel))
			return;
		srcver = LittleLong(srcmodel->ver);
		srcmodelidx = LittleLong(srcmodel->modelnum);
		modbrushes = LittleLong(srcmodel->numbrushes);
		modplanes = LittleLong(srcmodel->numplanes);
		if (srcver != 1 || lumpsizeremaining < sizeof(*srcmodel) + modbrushes*sizeof(*srcmodel) + modplanes*sizeof(*srcplane))
			return;
		lumpsizeremaining -= ((const char*)(srcmodel+1) + modbrushes*sizeof(*srcbrush) + modplanes*sizeof(*srcplane)) - (const char*)srcmodel;

		if (srcmodelidx > model->numsubmodels)
			return;

		brush = ZG_Malloc(&model->memgroup, sizeof(*brush)*modbrushes +
											sizeof(*sides)*(modbrushes*6+modplanes) +
											sizeof(*planes)*(modbrushes*6+modplanes));
		sides = (void*)(brush + modbrushes);
		planes = (void*)(sides + modbrushes*6+modplanes);
		model->submodels[srcmodelidx].brushes = brush;
		srcbrush = (const void*)(srcmodel+1);
		for (br = 0; br < modbrushes; br++, srcbrush = (const void*)srcplane)
		{
			/*byteswap it all in place*/
			brush->absmins[0] = LittleFloat(srcbrush->mins[0]);
			brush->absmins[1] = LittleFloat(srcbrush->mins[1]);
			brush->absmins[2] = LittleFloat(srcbrush->mins[2]);
			brush->absmaxs[0] = LittleFloat(srcbrush->maxs[0]);
			brush->absmaxs[1] = LittleFloat(srcbrush->maxs[1]);
			brush->absmaxs[2] = LittleFloat(srcbrush->maxs[2]);
			numplanes = (unsigned short)LittleShort(srcbrush->numplanes);

			/*make sure planes don't overflow*/
			if (numplanes > modplanes)
				return;
			modplanes-=numplanes;

			/*set up the mbrush from the file*/
			brush->contents = Q1BSP_TranslateContents(LittleShort(srcbrush->contents));
			brush->brushside = sides;
			for (srcplane = (const void*)(srcbrush+1); numplanes --> 0; srcplane++)
			{
				planes->normal[0] = LittleFloat(srcplane->normal[0]);
				planes->normal[1] = LittleFloat(srcplane->normal[1]);
				planes->normal[2] = LittleFloat(srcplane->normal[2]);
				planes->dist = LittleFloat(srcplane->dist);
				CategorizePlane(planes);

				sides->surface = NULL;
				sides++->plane = planes++;
			}

			/*and add axial planes acording to the brush's bbox*/
			for (pl = 0; pl < 3; pl++)
			{
				VectorCopy(axis[pl], planes->normal);
				planes->dist = brush->absmaxs[pl];
				CategorizePlane(planes);

				sides->surface = NULL;
				sides++->plane = planes++;
			}
			for (pl = 0; pl < 3; pl++)
			{
				VectorNegate(axis[pl], planes->normal);
				planes->dist = -brush->absmins[pl];
				CategorizePlane(planes);

				sides->surface = NULL;
				sides++->plane = planes++;
			}
			brush->numsides = sides - brush->brushside;
			brush++;
		}
		model->submodels[srcmodelidx].numbrushes = brush-model->submodels[srcmodelidx].brushes;

		/*move on to the next model*/
		srcmodel = (const void*)srcbrush;
	}
}

hull_t *Q1BSP_ChooseHull(model_t *model, int forcehullnum, const vec3_t mins, const vec3_t maxs, vec3_t offset)
{
	hull_t *hull;
	vec3_t size;
	VectorSubtract (maxs, mins, size);
	if (forcehullnum >= 1 && forcehullnum <= MAX_MAP_HULLSM && model->hulls[forcehullnum-1].available)
		hull = &model->hulls[forcehullnum-1];
	else
	{
#ifdef HEXEN2
		if (model->hulls[5].available)
		{	//choose based on hexen2 sizes.

			if (size[0] < 3) // Point
				hull = &model->hulls[0];
			else if (size[0] <= 8.1 && model->hulls[4].available)
				hull = &model->hulls[4];	//Pentacles
			else if (size[0] <= 32.1 && size[2] <= 28.1)  // Half Player
				hull = &model->hulls[3];
			else if (size[0] <= 32.1)  // Full Player
				hull = &model->hulls[1];
			else // Golumn
				hull = &model->hulls[5];
		}
		else
#endif
		{
			if (size[0] < 3 || !model->hulls[1].available)
				hull = &model->hulls[0];
			else if (size[0] <= 32.1 || !model->hulls[2].available)
			{
				if (size[2] < 54.1 && model->hulls[3].available)
					hull = &model->hulls[3]; // 32x32x36 (half-life's crouch)
				else if (model->hulls[1].available)
					hull = &model->hulls[1];
				else
					hull = &model->hulls[0];
			}
			else
				hull = &model->hulls[2];
		}
	}

	VectorSubtract (hull->clip_mins, mins, offset);
	return hull;
}
qboolean Q1BSP_Trace(model_t *model, int forcehullnum, const framestate_t *framestate, const vec3_t axis[3], const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs, qboolean capsule, unsigned int hitcontentsmask, trace_t *trace)
{
	hull_t *hull;
	vec3_t start_l, end_l;
	vec3_t offset;

	memset (trace, 0, sizeof(trace_t));
	trace->fraction = 1;
	trace->allsolid = true;

	hull = Q1BSP_ChooseHull(model, forcehullnum, mins, maxs, offset);

//fix for hexen2 monsters half-walking into walls.
//	if (forent.flags & FL_MONSTER)
//	{
//		offset[0] = 0;
//		offset[1] = 0;
//	}

	if (axis)
	{
		vec3_t tmp;
		VectorSubtract(start, offset, tmp);
		start_l[0] = DotProduct(tmp, axis[0]);
		start_l[1] = DotProduct(tmp, axis[1]);
		start_l[2] = DotProduct(tmp, axis[2]);
		VectorSubtract(end, offset, tmp);
		end_l[0] = DotProduct(tmp, axis[0]);
		end_l[1] = DotProduct(tmp, axis[1]);
		end_l[2] = DotProduct(tmp, axis[2]);
		Q1BSP_RecursiveHullCheck(hull, hull->firstclipnode, start_l, end_l, hitcontentsmask, trace);

		if (trace->fraction == 1)
		{
			VectorCopy (end, trace->endpos);
		}
		else
		{
			vec3_t iaxis[3];
			vec3_t norm;
			Matrix3x3_RM_Invert_Simple((void *)axis, iaxis);
			VectorCopy(trace->plane.normal, norm);
			trace->plane.normal[0] = DotProduct(norm, iaxis[0]);
			trace->plane.normal[1] = DotProduct(norm, iaxis[1]);
			trace->plane.normal[2] = DotProduct(norm, iaxis[2]);

			/*just interpolate it, its easier than inverse matrix rotations*/
			VectorInterpolate(start, trace->fraction, end, trace->endpos);
		}
	}
	else
	{
		VectorSubtract(start, offset, start_l);
		VectorSubtract(end, offset, end_l);
		Q1BSP_RecursiveHullCheck(hull, hull->firstclipnode, start_l, end_l, hitcontentsmask, trace);

		if (trace->fraction == 1)
		{
			VectorCopy (end, trace->endpos);
		}
		else
		{
			VectorAdd (trace->endpos, offset, trace->endpos);
		}
	}

#ifdef TERRAIN
	if (model->terrain && trace->fraction)
	{
		trace_t hmt;
		Heightmap_Trace(model, forcehullnum, framestate, axis, start, end, mins, maxs, capsule, hitcontentsmask, &hmt);
		if (hmt.fraction < trace->fraction)
			*trace = hmt;
	}
#endif

	return trace->fraction != 1;
}

/*
Physics functions (common)

============================================================================

Rendering functions (Client only)
*/
#ifdef HAVE_CLIENT

extern int	r_dlightframecount;

//goes through the nodes marking the surfaces near the dynamic light as lit.
void Q1BSP_MarkLights (dlight_t *light, dlightbitmask_t bit, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;

	float		l, maxdist;
	int			j, s, t;
	vec3_t		impact;
	vec4_t		*lmvecs;
	float		*lmvecscale;

	if (node->contents < 0)
		return;

	splitplane = node->plane;
	if (splitplane->type < 3)
		dist = light->origin[splitplane->type] - splitplane->dist;
	else
		dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;

	if (dist > light->radius)
	{
		Q1BSP_MarkLights (light, bit, node->children[0]);
		return;
	}
	if (dist < -light->radius)
	{
		Q1BSP_MarkLights (light, bit, node->children[1]);
		return;
	}

	maxdist = light->radius*light->radius;

// mark the polygons
	surf = currentmodel->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		//Yeah, you can blame LordHavoc for this alternate code here.
		for (j=0 ; j<3 ; j++)
			impact[j] = light->origin[j] - surf->plane->normal[j]*dist;

		if (currentmodel->facelmvecs)
			lmvecs = currentmodel->facelmvecs[surf-currentmodel->surfaces].lmvecs, lmvecscale = currentmodel->facelmvecs[surf-currentmodel->surfaces].lmvecscale;
		else
			lmvecs = surf->texinfo->vecs, lmvecscale = surf->texinfo->vecscale;

		// clamp center of light to corner and check brightness
		l = DotProduct (impact, lmvecs[0]) + lmvecs[0][3] - surf->texturemins[0];
		s = l+0.5;if (s < 0) s = 0;else if (s > surf->extents[0]) s = surf->extents[0];
		s = (l - s)*lmvecscale[0]; //FIXME
		l = DotProduct (impact, lmvecs[1]) + lmvecs[1][3] - surf->texturemins[1];
		t = l+0.5;if (t < 0) t = 0;else if (t > surf->extents[1]) t = surf->extents[1];
		t = (l - t)*lmvecscale[1];
		// compare to minimum light
		if ((s*s+t*t+dist*dist) < maxdist)
		{
			if (surf->dlightframe != r_dlightframecount)
			{
				surf->dlightbits = bit;
				surf->dlightframe = r_dlightframecount;
			}
			else
				surf->dlightbits |= bit;
		}
	}

	Q1BSP_MarkLights (light, bit, node->children[0]);
	Q1BSP_MarkLights (light, bit, node->children[1]);
}

//combination of R_AddDynamicLights and R_MarkLights
static void Q1BSP_StainNode_r (model_t *model, mnode_t *node, float *parms)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;

	if (node->contents < 0)
		return;

	splitplane = node->plane;
	dist = DotProduct ((parms+1), splitplane->normal) - splitplane->dist;

	if (dist > (*parms))
	{
		Q1BSP_StainNode_r (model, node->children[0], parms);
		return;
	}
	if (dist < (-*parms))
	{
		Q1BSP_StainNode_r (model, node->children[1], parms);
		return;
	}

// mark the polygons
	surf = model->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		if (surf->flags&~(SURF_DRAWALPHA|SURF_DONTWARP|SURF_PLANEBACK))
			continue;
		Surf_StainSurf(model, surf, parms);
	}

	Q1BSP_StainNode_r (model, node->children[0], parms);
	Q1BSP_StainNode_r (model, node->children[1], parms);
}
static void Q1BSP_StainNode (model_t *model, float *parms)
{
	Q1BSP_StainNode_r(model, model->rootnode, parms);
}


static qbyte *q1frustumvis;	//temp use
static int q1_visframecount;//temp use
static int q1_framecount;	//temp use
struct q1bspprv_s
{
	int visframecount;
	int framecount;
	int oldviewclusters[2];
	qboolean detachedscanned;	//have we looked for non-coplanar (misc_external_mesh) faces yet?
	qboolean hasdetachedsurfs;	//...and did we find any? (gates the extra PrepareFrame work)
};
#define BACKFACE_EPSILON	0.01
static void Q1BSP_RecursiveWorldNode (mnode_t *node, unsigned int clipflags)
{
	int			c, side, clipped;
	mplane_t	*plane, *clipplane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	double		dot;

start:

	if (node->contents == Q1CONTENTS_SOLID)
		return;		// solid

	if (node->visframe != q1_visframecount)
		return;

	for (c = 0, clipplane = r_refdef.frustum; c < r_refdef.frustum_numworldplanes; c++, clipplane++)
	{
		if (!(clipflags & (1 << c)))
			continue;	// don't need to clip against it

		clipped = BOX_ON_PLANE_SIDE (node->minmaxs, node->minmaxs + 3, clipplane);
		if (clipped == 2)
			return;
		else if (clipped == 1)
			clipflags -= (1<<c);	// node is entirely on screen
	}

// if a leaf node, draw stuff
	if (node->contents < 0)
	{
		pleaf = (mleaf_t *)node;

		c = (pleaf - cl.worldmodel->leafs)-1;
		q1frustumvis[c>>3] |= 1<<(c&7);

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				(*mark++)->visframe = q1_framecount;
			} while (--c);
		}
		return;
	}

// node is just a decision point, so go down the apropriate sides

// find which side of the node we are on
	plane = node->plane;

	switch (plane->type)
	{
	case PLANE_X:
		dot = r_origin[0] - plane->dist;
		break;
	case PLANE_Y:
		dot = r_origin[1] - plane->dist;
		break;
	case PLANE_Z:
		dot = r_origin[2] - plane->dist;
		break;
	default:
		dot = DotProduct (r_origin, plane->normal) - plane->dist;
		break;
	}

	if (dot >= 0)
		side = 0;
	else
		side = 1;

// recurse down the children, front side first
	Q1BSP_RecursiveWorldNode (node->children[side], clipflags);

// draw stuff
	c = node->numsurfaces;

	if (c)
	{
		surf = cl.worldmodel->surfaces + node->firstsurface;

		if (dot < 0 -BACKFACE_EPSILON)
			side = SURF_PLANEBACK;
		else if (dot > BACKFACE_EPSILON)
			side = 0;
		{
			for ( ; c ; c--, surf++)
			{
				if (surf->visframe != q1_framecount)
					continue;

				//backface cull against the surface's OWN plane, not the node's. For a
				//normal brush face those are the same plane so sdot==dot and this is
				//identical; but a qbsp mesh importer (misc_external_mesh) can leave
				//triangles on a node whose plane differs from the triangle's, and the
				//node dot would then cull them by the wrong plane -- they vanish from one
				//side of that plane and cast ghost shadows from the other.
				{
					double sdot = DotProduct(r_origin, surf->plane->normal) - surf->plane->dist;
					if (((sdot < 0) ^ !!(surf->flags & SURF_PLANEBACK)))
						continue;		// wrong side
				}

				Surf_RenderDynamicLightmaps (surf);
				surf->sbatch->mesh[surf->sbatch->meshes++] = surf->mesh;
			}
		}
	}

// recurse down the back side
	//GLR_RecursiveWorldNode (node->children[!side], clipflags);
	node = node->children[!side];
	goto start;
}

static void Q1BSP_OrthoRecursiveWorldNode (mnode_t *node, unsigned int clipflags)
{
	//when rendering as ortho the front and back sides are technically equal. the only culling comes from frustum culling.

	int			c, clipped;
	mplane_t	*clipplane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;

	if (node->contents == Q1CONTENTS_SOLID)
		return;		// solid

	if (node->visframe != q1_visframecount)
		return;

	for (c = 0, clipplane = r_refdef.frustum; c < r_refdef.frustum_numworldplanes; c++, clipplane++)
	{
		if (!(clipflags & (1 << c)))
			continue;	// don't need to clip against it

		clipped = BOX_ON_PLANE_SIDE (node->minmaxs, node->minmaxs + 3, clipplane);
		if (clipped == 2)
			return;
		else if (clipped == 1)
			clipflags -= (1<<c);	// node is entirely on screen
	}

// if a leaf node, draw stuff
	if (node->contents < 0)
	{
		pleaf = (mleaf_t *)node;

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				(*mark++)->visframe = q1_framecount;
			} while (--c);
		}
		return;
	}

// recurse down the children
	Q1BSP_OrthoRecursiveWorldNode (node->children[0], clipflags);
	Q1BSP_OrthoRecursiveWorldNode (node->children[1], clipflags);

// draw stuff
	c = node->numsurfaces;

	if (c)
	{
		surf = cl.worldmodel->surfaces + node->firstsurface;

		for ( ; c ; c--, surf++)
		{
			if (surf->visframe != q1_framecount)
				continue;

			Surf_RenderDynamicLightmaps (surf);
			surf->sbatch->mesh[surf->sbatch->meshes++] = surf->mesh;
		}
	}
	return;
}

mleaf_t *Q1BSP_LeafForPoint (model_t *model, vec3_t p);	//defined further down; the
												//submerged-camera probe below needs it

//Is this leaf's contents a fluid the player can see through when r_wateralpha < 1?
//Shared by both halves of the wateralpha PVS extension in Q1BSP_MarkLeaves: the sweep
//that pulls every fluid leaf into an above-water camera's PVS, and the probe that pulls
//the above-water leaf into a submerged one's.  The HL CURRENT_* values are conveyor
//water - still water as far as rendering is concerned.
//nettest: latched state for the `r_waterinfo` diagnostic in r_surf.c.  See the
//note beside want_fluid_merge below for why "did MarkLeaves run at all" has to
//be recorded separately from "did the merge fire".
float q1bsp_marktime = -1;		//realtime the last time MarkLeaves ran (-1 = never)
int q1bsp_wantmerge = -1;		//...and whether the fluid merge was wanted then
int q1bsp_fluidtotal = -1;		//fluid leafs in the world
int q1bsp_fluidmerged = -1;		//...of which, ones the camera could see
int q1bsp_fluidshoreents = -1;	//total entries in the shore table (0 = adjacency unavailable)
int q1bsp_fluidnovis = -1;		//fluid leafs skipped for having no vis data of their own
int q1bsp_fluidbodies = -1;		//connected bodies of water those fluid leafs form

static qboolean Q1BSP_ContentsIsFluid(int contents)
{
	return contents == Q1CONTENTS_WATER ||
	       contents == Q1CONTENTS_SLIME ||
	       contents == Q1CONTENTS_LAVA  ||
	       contents == HLCONTENTS_CURRENT_0 ||
	       contents == HLCONTENTS_CURRENT_90 ||
	       contents == HLCONTENTS_CURRENT_180 ||
	       contents == HLCONTENTS_CURRENT_270 ||
	       contents == HLCONTENTS_CURRENT_UP ||
	       contents == HLCONTENTS_CURRENT_DOWN;
}

//nettest: THE SHORE TABLE, and why "is the fluid leaf in the camera's PVS" cannot work.
//
//The gate this replaces asked exactly that, reasoning that vis is symmetric so a fluid
//leaf you cannot see is one whose water you cannot see either.  Symmetric it is - and
//that is precisely what makes it useless HERE.  This feature exists for maps whose vis
//compiler treated water as an OPAQUE boundary; on such a map the compiler generates no
//portals through the water surface, so no air leaf ever has a water leaf in its PVS and
//the test is false for every fluid leaf on the map, from every viewpoint.  Measured with
//`r_waterinfo` on two unrelated maps: "merging 0 of 83" on th_ep1_01 and "merging 0 of 8"
//on cs_rats2.  Not a tuning problem - the feature had been dead since that gate landed,
//independently of the temporal-scene-cache kill fixed in r_surf.c.
//
//What the camera can actually see is the water SURFACE, and it sees it from the air leaf
//on the near side.  So the correct question is "is any leaf ADJACENT to this pool
//visible", and adjacency is static geometry - computed once per map rather than per
//frame.  Two leafs are adjacent when their bounding boxes touch.
//
//Adjacency alone is not enough, and measuring said so: on th_ep1_01 only 37 of the 83
//fluid leafs touch a non-fluid leaf at all.  The other 46 are INTERIOR - the middle of a
//body of water, whose neighbours are all more water.  So the fluid leafs are also grouped
//into connected BODIES (union-find over fluid-to-fluid adjacency), and a body counts as
//visible when any of its members can see a shore.  That takes coverage to 83 of 83 on
//th_ep1_01 (2 bodies) and 8 of 8 on cs_rats2 (1 body).
//
//Grouping by GEOMETRY is what keeps this from becoming the old bug.  The version before
//the broken gate set every fluid bit unconditionally and let pools merge each other
//through the PVS, so one visible puddle dragged in water from across the map.  A body is
//a single connected volume of water: seeing into one end of a river legitimately means
//seeing along it, and a pool in another room is simply a different body.
//
//Cost is O(fluidleafs * clusters) once at first use.  HL maps run 8-83 fluid leafs
//against 800-1900 clusters, i.e. a few hundred thousand box tests - immeasurable next to
//loading the map.  Bounded anyway, and the table simply stays empty past the bound, which
//degrades to the old direct-PVS test rather than to a stall.
#define Q1BSP_FLUIDADJ_BUDGET 8000000
static model_t *fluidadj_model;
static int fluidadj_count;
static int *fluidadj_cluster;	//cluster index of each fluid leaf
static int *fluidadj_ofs;		//where its shore list starts in fluidadj_shore
static int *fluidadj_cnt;		//how long that list is
static int *fluidadj_shore;		//flat list of adjacent non-fluid cluster indices
static int *fluidadj_body;		//which connected body of water each fluid leaf belongs to
static int fluidadj_numbodies;
static qbyte *fluidadj_bodyvis;	//per-frame scratch: is this body's shore visible

static qboolean Q1BSP_LeafBoxesTouch(const float *a, const float *b)
{	//minmaxs is mins[0..2] then maxs[3..5].  One unit of slop: BSP leaf bounds are
	//integral and neighbours share a plane exactly, so this is about float equality,
	//not about reaching into the next room.
	const float e = 1;
	if (a[0] > b[3]+e || a[1] > b[4]+e || a[2] > b[5]+e)
		return false;
	if (b[0] > a[3]+e || b[1] > a[4]+e || b[2] > a[5]+e)
		return false;
	return true;
}

static void Q1BSP_BuildFluidAdjacency(model_t *model)
{
	int i, j, n, total, pass;
	int nc = model->numclusters;

	if (fluidadj_model == model)
		return;

	BZ_Free(fluidadj_cluster);	fluidadj_cluster = NULL;
	BZ_Free(fluidadj_ofs);		fluidadj_ofs = NULL;
	BZ_Free(fluidadj_cnt);		fluidadj_cnt = NULL;
	BZ_Free(fluidadj_shore);	fluidadj_shore = NULL;
	BZ_Free(fluidadj_body);		fluidadj_body = NULL;
	BZ_Free(fluidadj_bodyvis);	fluidadj_bodyvis = NULL;
	fluidadj_model = model;
	fluidadj_count = 0;
	fluidadj_numbodies = 0;
	q1bsp_fluidshoreents = 0;

	if (!model->leafs || nc <= 0)
		return;

	fluidadj_cluster = BZ_Malloc(sizeof(int)*nc);
	for (i = 0; i < nc; i++)
		if (Q1BSP_ContentsIsFluid(model->leafs[i+1].contents))
			fluidadj_cluster[fluidadj_count++] = i;
	if (!fluidadj_count)
		return;
	if ((double)fluidadj_count * nc > Q1BSP_FLUIDADJ_BUDGET)
	{	//pathological map; leave the shore lists empty rather than stall the load.
		Con_DPrintf("wateralpha extendpvs: %i fluid leafs x %i clusters exceeds the adjacency budget; falling back to the direct PVS test\n", fluidadj_count, nc);
		return;
	}

	fluidadj_ofs = BZ_Malloc(sizeof(int)*fluidadj_count);
	fluidadj_cnt = BZ_Malloc(sizeof(int)*fluidadj_count);

	//counted first so the flat array can be sized exactly, then filled.
	for (pass = 0; pass < 2; pass++)
	{
		total = 0;
		for (i = 0; i < fluidadj_count; i++)
		{
			const float *fb = model->leafs[fluidadj_cluster[i]+1].minmaxs;
			if (pass)
				fluidadj_ofs[i] = total;
			n = 0;
			for (j = 0; j < nc; j++)
			{
				mleaf_t *ol;
				if (j == fluidadj_cluster[i])
					continue;
				ol = &model->leafs[j+1];
				if (ol->contents == Q1CONTENTS_SOLID)
					continue;	//you cannot stand in it, so it cannot be a viewpoint
				if (Q1BSP_ContentsIsFluid(ol->contents))
					continue;	//one pool must not vouch for the next - that cascades
				if (!Q1BSP_LeafBoxesTouch(fb, ol->minmaxs))
					continue;
				if (pass)
					fluidadj_shore[total+n] = j;
				n++;
			}
			if (pass)
				fluidadj_cnt[i] = n;
			total += n;
		}
		if (!pass)
			fluidadj_shore = BZ_Malloc(sizeof(int)*(total?total:1));
	}
	q1bsp_fluidshoreents = total;	//nettest: for r_waterinfo

	//Group the fluid leafs into connected bodies of water.  Union-find over
	//fluid-to-fluid bbox adjacency, then the roots are renumbered 0..n so the
	//per-frame visible flags can be a flat array.  O(fluid^2) once - 83 leafs is
	//under 7000 box tests.
	//
	//The parent array is SEPARATE from the output array, and that is not tidiness.
	//The first version of this unioned in place and then flattened in place, writing
	//each entry's answer over the parent pointer the NEXT entry's find() still had to
	//walk through.  As soon as one chain passed through an already-rewritten entry the
	//walk read a negative "root id" as a parent index and indexed the array with it -
	//an out-of-bounds read that crashed the moment r_wateralpha_extendpvs was set to 1.
	{
		int *parent = BZ_Malloc(sizeof(int)*fluidadj_count);
		int *dense  = BZ_Malloc(sizeof(int)*fluidadj_count);
		for (i = 0; i < fluidadj_count; i++)
		{
			parent[i] = i;			//parent = self
			dense[i]  = -1;			//no body id assigned yet
		}
		for (i = 0; i < fluidadj_count; i++)
		{
			const float *ib = model->leafs[fluidadj_cluster[i]+1].minmaxs;
			for (j = i+1; j < fluidadj_count; j++)
			{
				int ra, rb;
				if (!Q1BSP_LeafBoxesTouch(ib, model->leafs[fluidadj_cluster[j]+1].minmaxs))
					continue;
				for (ra = i; parent[ra] != ra; ra = parent[ra])
					;
				for (rb = j; parent[rb] != rb; rb = parent[rb])
					;
				if (ra != rb)
					parent[ra] = rb;
			}
		}
		fluidadj_body = BZ_Malloc(sizeof(int)*fluidadj_count);
		for (i = 0; i < fluidadj_count; i++)
		{
			int r;
			for (r = i; parent[r] != r; r = parent[r])
				;
			if (dense[r] < 0)
				dense[r] = fluidadj_numbodies++;
			fluidadj_body[i] = dense[r];
		}
		BZ_Free(parent);
		BZ_Free(dense);
	}
	fluidadj_bodyvis = BZ_Malloc(fluidadj_numbodies?fluidadj_numbodies:1);
	q1bsp_fluidbodies = fluidadj_numbodies;	//nettest: for r_waterinfo
	Con_DPrintf("wateralpha extendpvs: %i fluid leafs in %i connected bodies, %i shore entries\n",
				fluidadj_count, fluidadj_numbodies, total);
}

static qbyte *Q1BSP_MarkLeaves (model_t *model, int clusters[2])
{
	static qbyte	*cvis;
	qbyte *vis;
	mnode_t	*node;
	int		i;
	int portal = r_refdef.recurse;
	static pvsbuffer_t pvsbuf;
	//nettest: snapshot of the camera's own PVS, taken before the wateralpha fluid
	//merge writes into pvsbuf.  Kept as a separate allocation because ClusterPVS
	//may realloc pvsbuf out from under us.  Static and never freed, matching
	//pvsbuf above.
	static qbyte *basevis;
	static size_t basevisbytes;
	struct q1bspprv_s *prv = model->meshinfo;

	q1_framecount = ++prv->framecount;

	//for portals to work, we need two sets of any pvs caches
	//this means lights can still check pvs at the end of the frame despite recursing in the mean time
	//however, we still need to invalidate the cache because we only have one 'visframe' field in nodes.

	if (r_refdef.forcevis)
	{
		vis = r_refdef.forcedvis;

		prv->oldviewclusters[0] = -1;
		prv->oldviewclusters[1] = -2;
		cvis = NULL;
	}
	else
	{
		if (!portal)
		{
			if (((prv->oldviewclusters[0] == clusters[0] && prv->oldviewclusters[1] == clusters[1]) && !r_novis.ival) || r_novis.ival & 2)
				if (cvis)
				{
					q1_visframecount = prv->visframecount;
					return cvis;
				}

			prv->oldviewclusters[0] = clusters[0];
			prv->oldviewclusters[1] = clusters[1];
		}
		else
		{
			prv->oldviewclusters[0] = -1;
			prv->oldviewclusters[1] = -2;
			cvis = NULL;
		}

		if (r_novis.ival)
		{
			if (pvsbuf.buffersize < model->pvsbytes)
				pvsbuf.buffer = BZ_Realloc(pvsbuf.buffer, pvsbuf.buffersize=model->pvsbytes);
			vis = cvis = pvsbuf.buffer;
			memset (pvsbuf.buffer, 0xff, pvsbuf.buffersize);

			prv->oldviewclusters[0] = -1;
			prv->oldviewclusters[1] = -2;
		}
		else
		{
			// Opt-in via r_wateralpha_extendpvs because modern compilers
			// (ericw-tools vis with transparent-water support, qbsp -trans,
			// etc.) already produce BSPs whose vis data correctly accounts
			// for transparent water surfaces.  On those maps the fluid
			// merge is redundant AND harmful — it adds far-away leafs that
			// the fluid leafs can see for unrelated reasons, blowing the
			// PVS up to nearly r_novis levels and ruining culling.  Only
			// enable on legacy maps (vanilla GoldSrc, q1 with classic vis)
			// where the compiler treated water as opaque.
			qboolean want_fluid_merge = (model->leafs &&
			                             r_wateralpha.value < 1.0f &&
			                             r_wateralpha_extendpvs.ival);

			//nettest: latched for the `r_waterinfo` command (r_surf.c).  Three
			//separate things silently disable this feature and the console gives
			//no sign of which - the cvar, the alpha, and the temporal scene
			//cache returning before MarkLeaves is ever reached.  Recording the
			//fact that we GOT here is what distinguishes the third from the
			//first two, so it is written unconditionally and before the gate.
			q1bsp_marktime = realtime;
			q1bsp_wantmerge = want_fluid_merge;

			{	//nettest: report why the extension is or is not running.  Change-gated,
				//so a steady state prints once.  r_wateralpha is easily left at 1 by a
				//map cfg or a worldspawn key, which silently disables the whole thing.
				//
				//Note there is a SECOND way this silently does nothing: the temporal
				//scene cache.  When r_temporalscenecache is on, Surf_DrawWorld takes the
				//webostate branch (client/r_surf.c) and returns before ever calling
				//model->funcs.PrepareFrame, so MarkLeaves - and everything below - is
				//skipped entirely and the cached PVS is used as-is.  The cvar's default
				//is empty, which auto-enables the cache on maps with >6000 leafs, so
				//wateralpha extendpvs stops working on exactly the big maps where it
				//would matter most.  If that needs fixing, it has to be fixed there.
				static int lastwant = -1;
				if ((int)want_fluid_merge != lastwant && r_wateralpha_extendpvs.ival >= 2)
				{
					lastwant = want_fluid_merge;
					Con_Printf("wateralpha extendpvs: %s (r_wateralpha %g, extendpvs %i)\n",
								want_fluid_merge?"ON":"off", r_wateralpha.value, r_wateralpha_extendpvs.ival);
				}
			}

			if (clusters[1] >= 0 && clusters[1] != clusters[0])
			{
				vis = cvis = model->funcs.ClusterPVS(model, clusters[0], &pvsbuf, PVM_REPLACE);
				vis = cvis = model->funcs.ClusterPVS(model, clusters[1], &pvsbuf, PVM_MERGE);
			}
			else if (want_fluid_merge)
			{
				// PVM_FAST returns a pointer INTO model->pvs (shared
				// persistent vis data).  Writing to it from the fluid
				// merge below would corrupt the map's vis data and
				// produce drifting rendering artifacts that compound
				// across frames.  Force REPLACE into pvsbuf so we own
				// the buffer and can OR fluid bits into it safely.
				vis = cvis = model->funcs.ClusterPVS(model, clusters[0], &pvsbuf, PVM_REPLACE);
			}
			else
				vis = cvis = model->funcs.ClusterPVS(model, clusters[0], &pvsbuf, PVM_FAST);

			// Water transparency PVS extension: when r_wateralpha is < 1,
			// the player can see THROUGH water surfaces, but the vis
			// compiler treated those surfaces as opaque boundaries, so
			// the leafs BEHIND the water are missing from the camera's
			// baked PVS.  For every fluid leaf the camera can see, OR
			// that leaf's own PVS into the result: that brings in the
			// underwater geometry, pool floors and decorations without
			// modifying the on-disk vis data.
			//
			//nettest: this used to walk EVERY leaf in the model and, for
			//every fluid leaf anywhere on the map, force-set its visible
			//bit before merging.  That is what made distant water render
			//through walls under r_showtris - not the merge, the
			//unconditional bit-set on line "vis[j>>3] |= ...".  Every
			//water surface in the map was submitted from everywhere.
			//
			//The gate is now "the fluid leaf is in the camera's own PVS",
			//which is the correct rule for a transparent surface: you can
			//only see through a surface you can see.  The old comment here
			//argued such a gate "never fires from a distance", but vis is
			//symmetric - if no sight line reaches the fluid leaf then its
			//water surface is genuinely not visible and drawing it is the
			//bug, not the fix.  Both confirmed-working cases survive:
			//looking into a pool still merges that pool (its leaf is in
			//PVS), and the submerged camera is handled geometrically by
			//the probe below, independently of this loop.
			//
			//Test against a SNAPSHOT, never the live buffer.  A version
			//that tested the growing vis set is order-dependent: 146 of
			//873 viewpoints on th_ep1_00 give a different answer scanning
			//ascending vs descending, max delta 34 clusters, because a
			//pool merged early can pull in a pool the camera cannot see,
			//which then merges its own PVS in turn and cascades.
			//
			// Cost: O(numleafs) bitmap test + one ClusterPVS call per
			// VISIBLE fluid leaf (was: per fluid leaf in the entire map).
			// HL maps typically have 0-50 fluid leafs total, each
			// ClusterPVS call is a decompress+OR over pvsbytes (~500 B for
			// HL maps).  Sub-millisecond.  Skipped entirely when
			// wateralpha == 1 so opaque-water rendering is bit-for-bit
			// unchanged.
			if (want_fluid_merge)
			{
				int f, k;
				int fluidtotal = 0, fluidmerged = 0, fluidnovis = 0;

				Q1BSP_BuildFluidAdjacency(model);
				fluidtotal = fluidadj_count;

				if (basevisbytes < model->pvsbytes)
					basevis = BZ_Realloc(basevis, basevisbytes = model->pvsbytes);
				memcpy(basevis, vis, model->pvsbytes);

				// Every table has to be present before any of it is indexed.
				// Q1BSP_BuildFluidAdjacency returns early on a map that busts
				// the adjacency budget, leaving the fluid COUNT set but the
				// tables NULL - which the two passes below would happily walk
				// straight off. Nothing merges in that case, which is the same
				// answer as before this feature existed.
				if (!fluidadj_count || !fluidadj_body || !fluidadj_bodyvis)
					goto fluidmergedone;

				// PASS 1 - which BODIES of water can the camera see a shore of.
				// Bodies rather than individual leafs because most fluid leafs
				// are interior and touch nothing but more water; see the note
				// above Q1BSP_BuildFluidAdjacency.
				memset(fluidadj_bodyvis, 0, fluidadj_numbodies);
				for (f = 0; f < fluidadj_count; f++)
				{
					int fc = fluidadj_cluster[f];
					int bd = fluidadj_body[f];

					if (fluidadj_bodyvis[bd])
						continue;	//already established

					// Direct hit first: on a map compiled by a vis tool that
					// DOES understand transparent water the fluid leaf really is
					// in the camera's PVS, and then this costs one bit test.
					if (basevis[fc>>3] & (1<<(fc&7)))
					{
						fluidadj_bodyvis[bd] = 1;
						continue;
					}
					if (fluidadj_cnt)
					{	// ...otherwise, can we see its shore?
						int first = fluidadj_ofs[f], cnt = fluidadj_cnt[f];
						for (k = 0; k < cnt; k++)
						{
							int sc = fluidadj_shore[first+k];
							if (basevis[sc>>3] & (1<<(sc&7)))
							{
								fluidadj_bodyvis[bd] = 1;
								break;
							}
						}
					}
				}

				// PASS 2 - merge every leaf of every visible body.
				for (f = 0; f < fluidadj_count; f++)
				{
					int fc = fluidadj_cluster[f];

					if (!fluidadj_bodyvis[fluidadj_body[f]])
						continue;

					// A fluid leaf with NO vis data at all is the dangerous case:
					// Q1BSP_DecompressVis answers "everything is visible" for a
					// NULL row (q1bsp.c:2709-2717) and does so by OVERWRITING,
					// so merging one is r_novis for the whole frame - geometry
					// from the far end of the map drawn over a depth-less HL
					// sky, which is exactly the "faces flickering in the
					// distance" report.  Skip it; we have already decided the
					// surface is visible, and drawing that does not depend on
					// this merge.
					if (!model->leafs[fc+1].compressed_vis)
					{
						fluidnovis++;
						continue;
					}

					fluidmerged++;
					vis = cvis = model->funcs.ClusterPVS(model, fc, &pvsbuf, PVM_MERGE);
					// Mark the fluid leaf itself, which the old code got for
					// free from basevis and this no longer can: reaching a pool
					// through its body's shore means its own bit was never set,
					// and without it the geometry filed inside the water leaf -
					// the pool floor - stays culled.  Done through the RETURNED
					// pointer because ClusterPVS may have reallocated pvsbuf.
					vis[fc>>3] |= (1<<(fc&7));
				}
fluidmergedone:

				q1bsp_fluidtotal  = fluidtotal;		//nettest: for r_waterinfo
				q1bsp_fluidmerged = fluidmerged;
				q1bsp_fluidnovis  = fluidnovis;

				{	//nettest: developer readout for tuning this.  The old code
					//merged fluidtotal every frame from every viewpoint; anything
					//well below that is the gate doing its job.  Printed only when
					//the count changes, so it does not flood a moving camera.
					static int lastmerged = -1;
					if (fluidmerged != lastmerged && r_wateralpha_extendpvs.ival >= 2)
					{
						lastmerged = fluidmerged;
						Con_Printf("wateralpha extendpvs: merging %i of %i fluid leafs\n", fluidmerged, fluidtotal);
					}
				}
				// The SUBMERGED camera — the loop above cannot help it.
				//
				// Everything that loop merges is another FLUID leaf's PVS, and
				// on a map whose compiler treated water as opaque a fluid leaf
				// can see little but other fluid leafs.  With the eye under the
				// surface the result is a PVS holding the pool and nothing else:
				// look up through a half-transparent surface and the entire
				// world above it is missing.  Vis is symmetric, so this is not
				// an oversight in the loop — there is genuinely no above-water
				// leaf reachable from down here for it to merge.
				//
				// Fix it geometrically instead.  Step straight up from the eye
				// until the leaf stops being fluid and merge THAT leaf's PVS:
				// literally "what you would see with your head out of the
				// water".  Together with the loop above (which keeps the
				// underwater geometry in) that gives the both-ways visibility a
				// transparent surface implies.
				//
				// Solid and sky leafs are stepped over rather than ending the
				// search, so swimming under a ledge or through a submerged pipe
				// still finds the open air past it.  The walk is bounded by the
				// world's own maxs and a fixed step count, so water with solid
				// rock all the way up just costs a few dozen point-in-leaf
				// descents (a handful of plane dots each) and merges nothing.
				if (clusters[0] >= 0 && Q1BSP_ContentsIsFluid(model->leafs[clusters[0]+1].contents))
				{
					vec3_t probe;
					float ceiling = model->maxs[2];
					int step;
					VectorCopy(r_refdef.vieworg, probe);
					for (step = 0; step < 64 && probe[2] < ceiling; step++)
					{
						mleaf_t *l;
						int c;
						probe[2] += 32;
						l = Q1BSP_LeafForPoint(model, probe);
						c = l->contents;
						if (c == Q1CONTENTS_SOLID || c == Q1CONTENTS_SKY)
							continue;	//rock or skybox overhead - keep climbing
						if (Q1BSP_ContentsIsFluid(c))
							continue;	//still submerged
						{	//broke the surface
							int aircluster = (int)(l - model->leafs) - 1;
							if (aircluster >= 0 && aircluster < model->numclusters)
							{
								vis[aircluster>>3] |= (1<<(aircluster&7));
								vis = cvis = model->funcs.ClusterPVS(model, aircluster, &pvsbuf, PVM_MERGE);
							}
						}
						break;
					}
				}

				// Force PVS recompute next frame even from the same
				// camera position — the merge state above isn't
				// captured by the (clusters[0], clusters[1]) cache key,
				// so without this a stationary camera with wateralpha
				// just toggled wouldn't pick up the change.  The submerged
				// probe above needs it for a second reason: it keys off the
				// eye ORIGIN, which moves within a leaf.
				prv->oldviewclusters[0] = -1;
				prv->oldviewclusters[1] = -2;
			}
		}
	}

	prv->visframecount++;
	q1_visframecount = prv->visframecount;

	if (clusters[0] < 0)
	{
		//to improve spectating, when the camera is in a wall, we ignore any sky leafs.
		//this prevents seeing the upwards-facing sky surfaces within the sky volumes.
		//this will not affect inwards facing sky, so sky will basically appear as though it is identical to solid brushes.
		for (i=0 ; i<model->numclusters ; i++)
		{
			if (vis[i>>3] & (1<<(i&7)))
			{
				if (model->leafs[i+1].contents == Q1CONTENTS_SKY)
					continue;
				node = (mnode_t *)&model->leafs[i+1];
				do
				{
					if (node->visframe == q1_visframecount)
						break;
					node->visframe = q1_visframecount;
					node = node->parent;
				} while (node);
			}
		}
	}
	else
	{
		for (i=0 ; i<model->numclusters ; i++)
		{
			if (vis[i>>3] & (1<<(i&7)))
			{
				node = (mnode_t *)&model->leafs[i+1];
				do
				{
					if (node->visframe == q1_visframecount)
						break;
					node->visframe = q1_visframecount;
					node = node->parent;
				} while (node);
			}
		}
	}
	return vis;
}

//misc_external_mesh bakes triangles into the world as faces that are NOT coplanar with any
//BSP split, so qbsp files each under the deepest node containing its leafs, not a node on
//its own plane. The perspective world walk (Q1BSP_RecursiveWorldNode) marks a surface
//visible only when it reaches that surface's leaf, mid-traversal, and draws a node's faces
//BETWEEN its front and back subtrees -- so a detached face can be drawn before its leaf has
//been reached and gets skipped, popping in and out as the camera crosses the filed node's
//plane. Flag those faces once here (a normal brush face shares its node's plane pointer; a
//detached one does not) so PrepareFrame can mark them visible up-front, before the walk.
static void Q1BSP_ScanDetachedSurfs (model_t *model)
{
	struct q1bspprv_s *prv = model->meshinfo;
	int n;

	prv->detachedscanned = true;
	prv->hasdetachedsurfs = false;

	for (n = 0; n < model->numnodes; n++)
	{
		mnode_t *node = model->nodes + n;
		msurface_t *surf = model->surfaces + node->firstsurface;
		unsigned int s;
		for (s = 0; s < node->numsurfaces; s++, surf++)
		{
			if (surf->plane != node->plane)
			{
				surf->flags |= SURF_DETACHED;
				prv->hasdetachedsurfs = true;
			}
		}
	}
}

static void Q1BSP_PrepareFrame(model_t *model, refdef_t *refdef, int area, int clusters[2], pvsbuffer_t *vis, qbyte **entvis_out, qbyte **surfvis_out)
{
	qbyte *entvis = *entvis_out = Q1BSP_MarkLeaves (model, clusters);

	if (vis->buffersize < model->pvsbytes)
		vis->buffer = BZ_Realloc(vis->buffer, vis->buffersize=model->pvsbytes);
	q1frustumvis = vis->buffer;
	memset(q1frustumvis, 0, model->pvsbytes);

	if (model != cl.worldmodel)
		; //global abuse...
	else if (r_refdef.useperspective)
	{
		//Mark detached (misc_external_mesh) faces visible from the PVS before the walk, so
		//the node draw-loop picks them up regardless of traversal order. Done every frame
		//(a cached-vis result can skip MarkLeaves' own node loop, but q1_framecount is
		//still bumped, so it matches what the walk will test against). Gated on
		//hasdetachedsurfs, so a map with no imported mesh pays only the one-time scan.
		struct q1bspprv_s *prv = model->meshinfo;
		if (!prv->detachedscanned)
			Q1BSP_ScanDetachedSurfs (model);
		if (prv->hasdetachedsurfs && entvis)
		{
			int i;
			for (i = 0; i < model->numclusters; i++)
			{
				mleaf_t *leaf;
				msurface_t **mark;
				int c;
				if (!(entvis[i>>3] & (1<<(i&7))))
					continue;
				leaf = &model->leafs[i+1];
				mark = leaf->firstmarksurface;
				c = leaf->nummarksurfaces;
				while (c-- > 0)
				{
					msurface_t *surf = *mark++;
					if (surf->flags & SURF_DETACHED)
						surf->visframe = q1_framecount;
				}
			}
		}
		Q1BSP_RecursiveWorldNode (model->nodes, 0x1f);
	}
	else
		Q1BSP_OrthoRecursiveWorldNode (model->nodes, 0x1f);
	*surfvis_out = q1frustumvis;
}


#endif
/*
Rendering functions (Client only)

==============================================================================

Server only functions
*/
#ifdef HAVE_SERVER
static qbyte *Q1BSP_ClusterPVS (model_t *model, int cluster, pvsbuffer_t *buffer, pvsmerge_t merge);

//does the recursive work of Q1BSP_FatPVS
static void SV_Q1BSP_AddToFatPVS (model_t *mod, const vec3_t org, mnode_t *node, pvsbuffer_t *pvsbuffer)
{
	mplane_t	*plane;
	float	d;

	while (1)
	{
	// if this is a leaf, accumulate the pvs bits
		if (node->contents < 0)
		{
			if (node->contents != Q1CONTENTS_SOLID)
			{
				Q1BSP_ClusterPVS(mod, ((mleaf_t *)node - mod->leafs)-1, pvsbuffer, PVM_MERGE);
			}
			return;
		}

		plane = node->plane;
		d = DotProduct (org, plane->normal) - plane->dist;
		if (d > 8)
			node = node->children[0];
		else if (d < -8)
			node = node->children[1];
		else
		{	// go down both
			SV_Q1BSP_AddToFatPVS (mod, org, node->children[0], pvsbuffer);
			node = node->children[1];
		}
	}
}

/*
=============
Q1BSP_FatPVS

Calculates a PVS that is the inclusive or of all leafs within 8 pixels of the
given point.
=============
*/
static unsigned int Q1BSP_FatPVS (model_t *mod, const vec3_t org, pvsbuffer_t *pvsbuffer, qboolean add)
{
	if (pvsbuffer->buffersize < mod->pvsbytes)
		pvsbuffer->buffer = BZ_Realloc(pvsbuffer->buffer, pvsbuffer->buffersize=mod->pvsbytes);
	if (!add)
		Q_memset (pvsbuffer->buffer, 0, mod->pvsbytes);
	SV_Q1BSP_AddToFatPVS (mod, org, mod->nodes, pvsbuffer);
	return mod->pvsbytes;
}

#endif
static qboolean Q1BSP_EdictInFatPVS(model_t *mod, const struct pvscache_s *ent, const qbyte *pvs, const int *areas)
{
	int i;

	//if (areas)areas[0] is the area count... but q1bsp has no areas so we ignore it entirely.

	if (ent->num_leafs < 0)
		return true;	//it's in too many leafs for us to cope with. Just trivially accept it.

	for (i=0 ; i < ent->num_leafs ; i++)
		if (pvs[ent->leafnums[i] >> 3] & (1 << (ent->leafnums[i]&7) ))
			return true;	//we might be able to see this one.

	return false;	//none of this ents leafs were visible, so neither is the ent.
}

/*
===============
SV_FindTouchedLeafs

Links the edict to the right leafs so we can get it's potential visability.
===============
*/
static void Q1BSP_RFindTouchedLeafs (model_t *wm, struct pvscache_s *ent, mnode_t *node, const float *mins, const float *maxs)
{
	mplane_t	*splitplane;
	mleaf_t		*leaf;
	int			sides;
	int			leafnum;

// add an efrag if the node is a leaf

	if (node->contents < 0)
	{
		//ignore solid leafs. this should include leaf 0 (which has no pvs info)
		if (node->contents == Q1CONTENTS_SOLID)
			return;

		if ((unsigned)ent->num_leafs >= MAX_ENT_LEAFS)
		{
			ent->num_leafs = -1;	//too many. mark it as such so we can trivially accept huge mega-big brush models.
			return;
		}

		leaf = (mleaf_t *)node;
		leafnum = leaf - wm->leafs - 1;

		ent->leafnums[ent->num_leafs] = leafnum;
		ent->num_leafs++;
		return;
	}

// NODE_MIXED

	splitplane = node->plane;
	sides = BOX_ON_PLANE_SIDE(mins, maxs, splitplane);

// recurse down the contacted sides
	if (sides & 1)
		Q1BSP_RFindTouchedLeafs (wm, ent, node->children[0], mins, maxs);

	if (sides & 2)
		Q1BSP_RFindTouchedLeafs (wm, ent, node->children[1], mins, maxs);
}
static void Q1BSP_FindTouchedLeafs(model_t *mod, struct pvscache_s *ent, const float *mins, const float *maxs)
{
	ent->num_leafs = 0;
	if (mins && maxs)
		Q1BSP_RFindTouchedLeafs (mod, ent, mod->nodes, mins, maxs);
}


/*
Server only functions

==============================================================================

PVS type stuff
*/

/*
===================
Mod_DecompressVis
===================
*/
static qbyte *Q1BSP_DecompressVis (qbyte *in, model_t *model, qbyte *decompressed, unsigned int buffersize, qboolean merge)
{
	int		c;
	qbyte	*out;
	int		row;

	row = (model->numclusters+7)>>3;
	out = decompressed;

	if (buffersize < row)
		row = buffersize;

	if (!in)
	{	// no vis info, so make all visible
		while (row)
		{
			*out++ = 0xff;
			row--;
		}
		return decompressed;
	}

	if (merge)
	{
		do
		{
			if (*in)
			{
				*out++ |= *in++;
				continue;
			}
			out += in[1];
			in += 2;
		} while (out - decompressed < row);
	}
	else
	{
		do
		{
			if (*in)
			{
				*out++ = *in++;
				continue;
			}

			c = in[1];
			in += 2;
			if ((out - decompressed) + c > row)
			{
				c = row - (out - decompressed);
				Con_DPrintf ("warning: Vis decompression overrun\n");
			}
			while (c)
			{
				*out++ = 0;
				c--;
			}
		} while (out - decompressed < row);
	}

	return decompressed;
}

static pvsbuffer_t	mod_novis;
static pvsbuffer_t	mod_tempvis;

void Q1BSP_Shutdown(void)
{
	Z_Free(mod_novis.buffer);
	memset(&mod_novis, 0, sizeof(mod_novis));
	Z_Free(mod_tempvis.buffer);
	memset(&mod_tempvis, 0, sizeof(mod_tempvis));
}

//pvs is 1-based. clusters are 0-based. otherwise, q1bsp has a 1:1 mapping.
static qbyte *Q1BSP_ClusterPVS (model_t *model, int cluster, pvsbuffer_t *buffer, pvsmerge_t merge)
{
	if (cluster == -1)
	{
		if (merge == PVM_FAST)
		{
			if (mod_novis.buffersize < model->pvsbytes)
			{
				mod_novis.buffer = BZ_Realloc(mod_novis.buffer, mod_novis.buffersize=model->pvsbytes);
				memset(mod_novis.buffer, 0xff, mod_novis.buffersize);
			}
			return mod_novis.buffer;
		}
		if (buffer->buffersize < model->pvsbytes)
			buffer->buffer = BZ_Realloc(buffer->buffer, buffer->buffersize=model->pvsbytes);
		memset(buffer->buffer, 0xff, model->pvsbytes);
		return buffer->buffer;
	}

	if (merge == PVM_FAST && model->pvs)
		return model->pvs + cluster * model->pvsbytes;

	cluster++;

	if (!buffer)
		buffer = &mod_tempvis;

	if (buffer->buffersize < model->pvsbytes)
		buffer->buffer = BZ_Realloc(buffer->buffer, buffer->buffersize=model->pvsbytes);

	return Q1BSP_DecompressVis (model->leafs[cluster].compressed_vis, model, buffer->buffer, buffer->buffersize, merge==PVM_MERGE);
}

static qbyte *Q1BSP_ClusterPHS (model_t *model, int cluster, pvsbuffer_t *buffer)
{
	return model->phs + cluster*model->pvsbytes;
}

//returns the leaf number, which is used as a bit index into the pvs.
static int Q1BSP_LeafnumForPoint (model_t *model, vec3_t p)
{
	mnode_t		*node;
	float		d;
	mplane_t	*plane;

	if (!model)
	{
		Sys_Error ("Q1BSP_LeafnumForPoint: bad model");
	}
	if (!model->nodes)
		return 0;

	node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return (mleaf_t *)node - model->leafs;
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return 0;	// never reached
}

mleaf_t *Q1BSP_LeafForPoint (model_t *model, vec3_t p)
{
	return model->leafs + Q1BSP_LeafnumForPoint(model, p);
}

static void Q1BSP_ClustersInSphere_Union(mleaf_t *firstleaf, const vec3_t center, float radius, mnode_t *node, qbyte *out, qbyte *unionwith)
{	//this is really for rtlights.
	float		t1, t2;
	mplane_t	*plane;
	while (1)
	{
		if (node->contents < 0)
		{	//leaf! mark/merge it.
			size_t c = (mleaf_t *)node - firstleaf;
			if (c == -1)
				return;
			if (unionwith)
				out[c>>3] |= (1<<(c&7)) & unionwith[c>>3];
			else
				out[c>>3] |= (1<<(c&7));
			return;
		}

		plane = node->plane;
		if (plane->type < 3)
			t1 = center[plane->type] - plane->dist;
		else
			t1 = DotProduct (plane->normal, center) - plane->dist;
		t2 = t1 - radius;
		t1 = t1 + radius;

		//if the sphere is fully to one side, only walk that side.
		if (t1 > 0 && t2 > 0)
		{
			node = node->children[0];
			continue;
		}
		if (t1 < 0 && t2 < 0)
		{
			node = node->children[1];
			continue;
		}

		//both sides are within the sphere
		Q1BSP_ClustersInSphere_Union(firstleaf, center, radius, node->children[0], out, unionwith);
		node = node->children[1];
		continue;
	}
}
static qbyte *Q1BSP_ClustersInSphere(model_t *mod, const vec3_t center, float radius, pvsbuffer_t *fte_restrict pvsbuffer, const qbyte *fte_restrict unionwith)
{
	if (!mod)
		Sys_Error ("Q1BSP_ClustersInSphere: bad model");
	if (!mod->nodes)
		return NULL;

	if (pvsbuffer->buffersize < mod->pvsbytes)
		pvsbuffer->buffer = BZ_Realloc(pvsbuffer->buffer, pvsbuffer->buffersize=mod->pvsbytes);
	Q_memset (pvsbuffer->buffer, 0, mod->pvsbytes);
	Q1BSP_ClustersInSphere_Union(mod->leafs+1, center, radius, mod->nodes, pvsbuffer->buffer, NULL);//unionwith);
	return pvsbuffer->buffer;
}

//returns the leaf number, which is used as a direct bit index into the pvs.
//-1 for invalid
static int Q1BSP_ClusterForPoint (model_t *model, const vec3_t p, int *area)
{
	mnode_t		*node;
	float		d;
	mplane_t	*plane;

	if (!model)
	{
		Sys_Error ("Q1BSP_ClusterForPoint: bad model");
	}
	if (area)
		*area = 0;	//no areas with q1bsp.
	if (!model->nodes)
		return -1;

	node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return ((mleaf_t *)node - model->leafs) - 1;
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return -1;	// never reached
}


static void Q1BSP_InfoForPoint (struct model_s *mod, vec3_t pos, int *area, int *cluster, unsigned int *contentbits)
{
	mnode_t		*node;
	float		d;
	mplane_t	*plane;

	*area = 0;	//no areas with q1bsp.
	*cluster = -1;
	*contentbits = FTECONTENTS_SOLID;
	if (!mod->nodes)
		return;

	node = mod->nodes;
	while (1)
	{
		if (node->contents < 0)
		{
			*cluster = ((mleaf_t *)node - mod->leafs) - 1;
			*contentbits = Q1BSP_TranslateContents(((mleaf_t *)node)->contents);
			return;	//we're done
		}
		plane = node->plane;
		d = DotProduct (pos,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}
}


/*
PVS type stuff

==============================================================================

Init stuff
*/


void Q1BSP_Init(void)
{
}

//sets up the functions a server needs.
//fills in bspfuncs_t
void Q1BSP_SetModelFuncs(model_t *mod)
{
#ifdef HAVE_SERVER
	mod->funcs.FatPVS				= Q1BSP_FatPVS;
#endif
	mod->funcs.EdictInFatPVS		= Q1BSP_EdictInFatPVS;
	mod->funcs.FindTouchedLeafs		= Q1BSP_FindTouchedLeafs;

	mod->funcs.ClustersInSphere		= Q1BSP_ClustersInSphere;
	mod->funcs.ClusterForPoint		= Q1BSP_ClusterForPoint;
	mod->funcs.ClusterPVS			= Q1BSP_ClusterPVS;
	mod->funcs.ClusterPHS			= Q1BSP_ClusterPHS;
	mod->funcs.NativeTrace			= Q1BSP_Trace;
	mod->funcs.PointContents		= Q1BSP_PointContents;

	mod->funcs.InfoForPoint			= Q1BSP_InfoForPoint;
#ifdef HAVE_CLIENT
	mod->funcs.LightPointValues		= GLQ1BSP_LightPointValues;
	mod->funcs.MarkLights			= Q1BSP_MarkLights;
	mod->funcs.StainNode			= Q1BSP_StainNode;
#ifdef RTLIGHTS
	mod->funcs.GenerateShadowMesh	= Q1BSP_GenerateShadowMesh;
#endif
	mod->funcs.PrepareFrame			= Q1BSP_PrepareFrame;

	{
		struct q1bspprv_s *prv = mod->meshinfo = ZG_Malloc(&mod->memgroup, sizeof(struct q1bspprv_s));
		prv->oldviewclusters[0] = -1;	//make sure its reset properly.
		prv->oldviewclusters[1] = -1;
	}
#endif
}
#endif

/*
Init stuff

==============================================================================

BSPX Stuff
*/
#include "fs.h"

typedef struct {
    char lumpname[24]; // up to 23 chars, zero-padded
    unsigned int fileofs;  // from file start
    unsigned int filelen;
} bspx_lump_t;
struct bspx_header_s {
    char id[4];  // 'BSPX'
    unsigned int numlumps;
	bspx_lump_t lumps[1];
};
//supported lumps (read specs/bspx.txt for more details):
//RGBLIGHTING (.lit)
//LIGHTING_E5BGR9 (hdr lit)
//LIGHTINGDIR (.lux)
//LMSHIFT (lightmap scaling, obsoleted bby DECOUPLED_LM)
//LMOFFSET (lightmap scaling, redundant without LMSHIFT)
//LMSTYLE (for when 4 styles per face are not enough)
//LMSTYLE16 (for when you need more than 256 different lightswitches)
//VERTEXNORMALS (smooth specular)
//BRUSHLIST (no hull size issues)
//ENVMAP (cubemaps)
//SURFENVMAP (cubemaps)
//FACENORMALS (because Quetoo's normals were rejected by ericw for some reason)
//DECOUPLED_LM (upgraded alternative to LM_SHIFT with float scaling and explicit lm sizes)
//LIGHTGRID_OCTREE (lightgrid alternative to floor-based model lighting, but still stuck with 4 8bit ldr styles)
void *BSPX_FindLump(bspx_header_t *bspxheader, void *mod_base, char *lumpname, size_t *lumpsize)
{
	int i;
	*lumpsize = 0;
	if (!bspxheader)
		return NULL;

	for (i = 0; i < bspxheader->numlumps; i++)
	{
		if (!strncmp(bspxheader->lumps[i].lumpname, lumpname, 24))
		{
			*lumpsize = bspxheader->lumps[i].filelen;
			return (char*)mod_base + bspxheader->lumps[i].fileofs;
		}
	}
	return NULL;
}
bspx_header_t *BSPX_Setup(model_t *mod, char *filebase, size_t filelen, lump_t *lumps, size_t numlumps)
{
	size_t i;
	size_t offs = 0;
	bspx_header_t *h;

	for (i = 0; i < numlumps; i++, lumps++)
	{
		if (offs < lumps->fileofs + lumps->filelen)
			offs = lumps->fileofs + lumps->filelen;
	}
	offs = (offs + 3) & ~3;
	if (offs + sizeof(*h) > filelen)
		h = NULL; /*no space for it*/
	else
	{
		h = (bspx_header_t*)(filebase + offs);

		i = LittleLong(h->numlumps);
		/*verify the header*/
		if (*(int*)h->id != (('B'<<0)|('S'<<8)|('P'<<16)|('X'<<24)) ||
			i < 0 ||
			offs + sizeof(*h) + sizeof(h->lumps[0])*(i-1) > filelen)
			h = NULL;
		else
		{
			h->numlumps = i;
			while(i-->0)
			{
				h->lumps[i].fileofs = LittleLong(h->lumps[i].fileofs);
				h->lumps[i].filelen = LittleLong(h->lumps[i].filelen);
				if (h->lumps[i].fileofs + h->lumps[i].filelen > filelen)
					return NULL;	//some sort of corruption/truncation.

				if (offs < h->lumps[i].fileofs + h->lumps[i].filelen)
					offs = h->lumps[i].fileofs + h->lumps[i].filelen;
			}
		}
	}

	if (offs < filelen && mod && !mod->archive && mod_loadmappackages.ival && filelen-offs > 22)//end-of-central-dir being 22 bytes sets a minimum zip size, which should slightly reduce false-positives.
	{	//we have some sort of trailing junk... is it a zip?...
		Mod_LoadMapArchive(mod, filebase+offs, filelen-offs);
	}

	return h;
}

#ifndef HAVE_CLIENT
void BSPX_LoadEnvmaps(model_t *mod, bspx_header_t *bspx, void *mod_base)
{
}
#else
void BSPX_LoadEnvmaps(model_t *mod, bspx_header_t *bspx, void *mod_base)
{
	unsigned int *envidx, idx;
	size_t i;
	char base[MAX_QPATH];
	char imagename[MAX_QPATH];
	menvmap_t *out;
	size_t count;
	denvmap_t *in = BSPX_FindLump(bspx, mod_base, "ENVMAP", &count);
	mod->envmaps = NULL;
	mod->numenvmaps = 0;
	if (!mod_loadsurfenvmaps.ival)
		return;
	if (count%sizeof(*in))
		return;	//erk
	count /= sizeof(*in);
	if (!count)
		return;
	out = ZG_Malloc(&mod->memgroup, sizeof(*out)*count);

	mod->envmaps = out;
	mod->numenvmaps = count;

	COM_FileBase(mod->name, base, sizeof(base));
	for (i = 0; i < count; i++)
	{
		out[i].origin[0] = LittleFloat(in[i].origin[0]);
		out[i].origin[1] = LittleFloat(in[i].origin[1]);
		out[i].origin[2] = LittleFloat(in[i].origin[2]);
		out[i].cubesize = LittleLong(in[i].cubesize);

		Q_snprintfz(imagename, sizeof(imagename), "textures/env/%s_%i_%i_%i", base, (int)mod->envmaps[i].origin[0], (int)mod->envmaps[i].origin[1], (int)mod->envmaps[i].origin[2]);
		out[i].image = Image_GetTexture(imagename, NULL, IF_TEXTYPE_CUBE|IF_LINEAR|IF_NOREPLACE, NULL, NULL, out[i].cubesize, out[i].cubesize, PTI_INVALID);
	}



	//now update surface lists.
	envidx = BSPX_FindLump(bspx, mod_base, "SURFENVMAP", &i);
	if (i/sizeof(*envidx) == mod->numsurfaces)
	{
		for (i = 0; i < mod->numsurfaces; i++)
		{
			idx = LittleLong(envidx[i]);
			if (idx < (unsigned int)count)
				mod->surfaces[i].envmap = out[idx].image;
		}
	}
}

struct bspxrw
{
	const char *fname;
	char *origfile;
	qofs_t origsize;
	int lumpofs;
	fromgame_t fg;

	size_t corelumps;
	size_t totallumps;

	void *archivedata;
	qofs_t archivesize;

	struct
	{
		char lumpname[24]; // up to 23 chars, zero-padded
		void *data;  // from file start
		qofs_t filelen;
	} *lumps;
};
static void Mod_BSPXRW_Free(struct bspxrw *ctx)
{
	FS_FreeFile(ctx->origfile);
	Z_Free(ctx->lumps);
	ctx->corelumps = ctx->totallumps = 0;
	ctx->origfile = NULL;
}
static void Mod_BSPXRW_Write(struct bspxrw *ctx)
{
#if 1
	vfsfile_t *f = FS_OpenVFS(ctx->fname, "wb", FS_GAMEONLY);
	if (f)
	{
		qofs_t bspxofs;
		size_t i, j;
		int pad, paddata = 0;
		int nxlumps = ctx->totallumps-ctx->corelumps;
		lump_t *lumps = alloca(sizeof(*lumps)*ctx->corelumps);
		bspx_lump_t *xlumps = alloca(sizeof(*xlumps)*(ctx->totallumps-ctx->corelumps));
		//bsp header info
		VFS_WRITE(f, ctx->origfile, ctx->lumpofs);
		VFS_WRITE(f, lumps, sizeof(lumps[0])*ctx->corelumps);	//placeholder
		//orig lumps
		for (i = 0; i < ctx->corelumps; i++)
		{
			lumps[i].fileofs = VFS_TELL(f);
			lumps[i].filelen = ctx->lumps[i].filelen;

			VFS_WRITE(f, ctx->lumps[i].data, ctx->lumps[i].filelen);
			//ALL lumps must be 4-aligned, so pad if needed.
			pad = ((ctx->lumps[i].filelen+3)&~3)-ctx->lumps[i].filelen;
			VFS_WRITE(f, &paddata, pad);
		}
		//bspx header
		VFS_WRITE(f, "BSPX", 4);
		VFS_WRITE(f, &nxlumps, sizeof(nxlumps));
		bspxofs = VFS_TELL(f);
		VFS_WRITE(f, xlumps, sizeof(xlumps[0])*(ctx->totallumps-ctx->corelumps)); //placeholder
		//bspx data
		for (i = 0; i < nxlumps; i++)
		{
			j = ctx->corelumps+i;
			xlumps[i].fileofs = VFS_TELL(f);
			xlumps[i].filelen = ctx->lumps[j].filelen;
			memcpy(xlumps[i].lumpname, ctx->lumps[j].lumpname, sizeof(xlumps[i].lumpname));

			VFS_WRITE(f, ctx->lumps[j].data, ctx->lumps[j].filelen);
			//ALL lumps must be 4-aligned, so pad if needed.
			pad = ((ctx->lumps[j].filelen+3)&~3)-ctx->lumps[j].filelen;
			VFS_WRITE(f, &paddata, pad);
		}

		//now reinsert any concatenated zip.
		VFS_WRITE(f, ctx->archivedata, ctx->archivesize);

		//now rewrite both sets of offsets.
		VFS_SEEK(f, ctx->lumpofs);
		VFS_WRITE(f, lumps, sizeof(lumps[0])*ctx->corelumps);
		VFS_SEEK(f, bspxofs);
		VFS_WRITE(f, xlumps, sizeof(xlumps[0])*(ctx->totallumps-ctx->corelumps));

		VFS_CLOSE(f);
	}
#endif
	Mod_BSPXRW_Free(ctx);
}

static qboolean Mod_BSPXRW_SetLump(struct bspxrw *ctx, const char *lumpname, void *data, size_t datasize)
{
	int i;
	for (i = 0; i < ctx->totallumps; i++)
	{
		if (!strcmp(ctx->lumps[i].lumpname, lumpname))
		{	//replace the existing lump
			if (ctx->lumps[i].filelen == datasize && !memcmp(ctx->lumps[i].data, data, datasize))
				return false;	//nothing changed.
			ctx->lumps[i].data = data;
			ctx->lumps[i].filelen = datasize;
			return true;
		}
	}

	Z_ReallocElements((void**)&ctx->lumps, &ctx->totallumps, ctx->totallumps+1, sizeof(*ctx->lumps));
	Q_strncpyz(ctx->lumps[i].lumpname, lumpname, sizeof(ctx->lumps[i].lumpname));
	ctx->lumps[i].data = data;
	ctx->lumps[i].filelen = datasize;
	return true;
}

static qboolean Mod_BSPXRW_Read(struct bspxrw *ctx, const char *fname)
{
	int i;
	lump_t *l;
	const char **corelumpnames = NULL;
	bspx_header_t *bspxheader;
#ifdef Q3BSPS
	static const char *q3corelumpnames[Q3LUMPS_TOTAL] = {"entities","shaders","planes","nodes","leafs","leafsurfs","leafbrushes","submodels","brushes","brushsides","verts","indexes","fogs","surfaces","lightmaps","lightgrid","visibility"
		#ifdef RFBSPS
			,"lightgrididx"
		#endif
		};
#endif
#ifdef Q2BSPS
	static const char *q2corelumpnames[Q2HEADER_LUMPS] = {"entities","planes","vertexes","visibility","nodes","texinfo","faces","lighting","leafs","leaffaces","leafbrushes","edges","surfedges","models","brushes","brushsides","pop","areas","areaportals"};
#endif
	static const char *q1corelumpnames[HEADER_LUMPS] = {"entities","planes","textures","vertexes","visibility","nodes","texinfo","faces","lighting","clipnodes","leafs","marksurfaces","edges","surfedges","models"};
	ctx->archivedata = NULL;
	ctx->archivesize = 0;
	ctx->fname = fname;
	ctx->origfile = FS_MallocFile(ctx->fname, FS_GAME, &ctx->origsize);
	if (!ctx->origfile)
		return false;
	ctx->lumps = 0;
	ctx->totallumps = 0;

	if (ctx->origsize < 4)
		i = 0;
	else
		i = LittleLong(*(int*)ctx->origfile);
	switch(i)
	{
	case 29:
	case 30:
		ctx->fg = ((i==30)?fg_halflife:fg_quake);
		ctx->lumpofs = 4;
		ctx->corelumps = HEADER_LUMPS;
		corelumpnames = q1corelumpnames;
		break;
	case ('I'<<0)+('B'<<8)+('S'<<16)+('P'<<24):	//starting with q2.
		i = LittleLong(*(int*)(ctx->origfile+4));
		ctx->lumpofs = 8;
		switch(i)
		{
#ifdef Q2BSPS
		case BSPVERSION_Q2:
//		case BSPVERSION_Q2W:
			ctx->fg = fg_quake2;
			ctx->corelumps = Q2HEADER_LUMPS;
			corelumpnames = q2corelumpnames;
			break;
#endif
#ifdef Q3BSPS
		case BSPVERSION_Q3:
		case BSPVERSION_RTCW:
			ctx->fg = fg_quake3;
			ctx->corelumps = 17;
			corelumpnames = q3corelumpnames;
			break;
#endif
		default:
			Mod_BSPXRW_Free(ctx);
			Con_Printf(CON_ERROR"%s: Unknown 'IBSP' revision\n", fname);
			return false;
		}
		break;
#ifdef RFBSPS
	case ('R'<<0)+('B'<<8)+('S'<<16)+('P'<<24):
	case ('F'<<0)+('B'<<8)+('S'<<16)+('P'<<24):
		i = LittleLong(*(int*)(ctx->origfile+4));
		ctx->lumpofs = 8;
		switch(i)
		{
		case BSPVERSION_RBSP:	//both rbsp+fbsp are version 1, with the only difference being lightmap sizes.
			ctx->fg = fg_quake3;
			ctx->corelumps = 18;
			corelumpnames = q3corelumpnames;
			break;
		default:
			Mod_BSPXRW_Free(ctx);
			Con_Printf(CON_ERROR"%s: Unknown 'RBSP'/'FBSP' revision\n", fname);
			return false;
		}
		break;
#endif
	default:
		Mod_BSPXRW_Free(ctx);
		Con_Printf(CON_ERROR"%s: Unknown file magic\n", fname);
		return false;
	}

	l = (lump_t*)(ctx->origfile+ctx->lumpofs);
	for (i = 0; i < ctx->corelumps; i++)
	{
		Z_ReallocElements((void**)&ctx->lumps, &ctx->totallumps, ctx->totallumps+1, sizeof(*ctx->lumps));
		ctx->lumps[ctx->totallumps-1].data = ctx->origfile+l[i].fileofs;
		ctx->lumps[ctx->totallumps-1].filelen = l[i].filelen;
		if (corelumpnames)
			Q_snprintfz(ctx->lumps[ctx->totallumps-1].lumpname, sizeof(ctx->lumps[0].lumpname), "%s", corelumpnames[i]);
		else
			Q_snprintfz(ctx->lumps[ctx->totallumps-1].lumpname, sizeof(ctx->lumps[0].lumpname), "lump%u", i);
	}

	bspxheader = BSPX_Setup(NULL, ctx->origfile, ctx->origsize, l, ctx->corelumps);
	if (bspxheader)
	{
		for (i = 0; i < bspxheader->numlumps; i++)
		{
			Z_ReallocElements((void**)&ctx->lumps, &ctx->totallumps, ctx->totallumps+1, sizeof(*ctx->lumps));
			ctx->lumps[ctx->totallumps-1].data = ctx->origfile+bspxheader->lumps[i].fileofs;
			ctx->lumps[ctx->totallumps-1].filelen = bspxheader->lumps[i].filelen;
			memcpy(ctx->lumps[ctx->totallumps-1].lumpname, bspxheader->lumps[i].lumpname, sizeof(ctx->lumps[0].lumpname));
		}
	}
	return true;
}

static unsigned int Mod_NearestCubeForSurf(msurface_t *surf, denvmap_t *envmap, size_t nenvmap)
{	//this is slow, yes.
	size_t n, v;
	unsigned int best = ~0;
	float bestdist = FLT_MAX, dist;
	vec3_t diff, mins, maxs, mid;

	if (surf->mesh && surf->mesh->numvertexes)
	{
		VectorCopy(surf->mesh->xyz_array[0], mins);
		VectorCopy(surf->mesh->xyz_array[0], maxs);
		for (v = 1; v < surf->mesh->numvertexes; v++)
			AddPointToBounds(surf->mesh->xyz_array[v], mins, maxs);
		VectorAvg(mins, maxs, mid);

		for (n = 0; n < nenvmap; n++)
		{
			VectorSubtract(envmap[n].origin, mid, diff);
#if 0
			//axial distance
			dist = fabs(diff[0]) + fabs(diff[1]) + fabs(diff[2]);
#else
			//radial distance (squared)
			dist = DotProduct(diff,diff);
#endif
			if (bestdist > dist)
			{
				best = n;
				bestdist = dist;
			}
		}
	}
	return best;
}

static int QDECL envmapsort(const void *av, const void *bv)
{	//sorts cubemaps in order of size, to make texturearrays easier, if ever. The loader can then just make runs.
	const denvmap_t *a=av, *b=bv;
	if (a->cubesize == b->cubesize)
		return 0;
	if (a->cubesize > b->cubesize)
		return 1;
	return -1;
}

void SCR_ScreenShot_Cubemap(vec3_t origin, int size);

static void Mod_FindCubemaps(qboolean build)
{
	struct bspxrw bspctx;
	if (Mod_BSPXRW_Read(&bspctx, cl.worldmodel->name))
	{
		const char *entlump = Mod_GetEntitiesString(cl.worldmodel), *lmp;
		int nest;
		char key[1024];
		char value[1024];

		qboolean isenvmap;
		float size;
		vec3_t origin;

		denvmap_t *envmap = NULL; //*nenvmap
		size_t	nenvmap = 0;
		unsigned int *envmapidx = NULL;	//*numsurfaces
		size_t nenvmapidx = 0, i;

		//find targetnames, and store their origins so that we can deal with spotlights.
		for (lmp = entlump; ;)
		{
			lmp = COM_Parse(lmp);
			if (com_token[0] != '{')
				break;

			isenvmap = false;
			size = 128;
			VectorClear(origin);

			nest = 1;
			while (1)
			{
				lmp = COM_ParseOut(lmp, key, sizeof(key));
				if (!lmp)
					break; // error
				if (key[0] == '{')
				{
					nest++;
					continue;
				}
				if (key[0] == '}')
				{
					nest--;
					if (!nest)
						break; // end of entity
					continue;
				}
				if (nest!=1)
					continue;
				if (key[0] == '_')
					memmove(key, key+1, strlen(key));
				while (key[strlen(key)-1] == ' ') // remove trailing spaces
					key[strlen(key)-1] = 0;
				lmp = COM_ParseOut(lmp, value, sizeof(value));
				if (!lmp)
					break; // error

				// now that we have the key pair worked out...
				if (!strcmp("classname", key) && !strcmp(value, "env_cubemap"))
					isenvmap = true;
				else if (!strcmp("origin", key))
					sscanf(value, "%f %f %f", &origin[0], &origin[1], &origin[2]);
				else if (!strcmp("size", key))
					sscanf(value, "%f", &size);
			}

			if (isenvmap)
			{
				int e = nenvmap;
				if (ZF_ReallocElements((void**)&envmap, &nenvmap, nenvmap+1, sizeof(*envmap)))
				{
					VectorCopy(origin, envmap[e].origin);
					envmap[e].cubesize = size;
				}
			}
		}

		if (nenvmap)
		{
			qsort(envmap, nenvmap, sizeof(*envmap), envmapsort);	//sort them by size
			if (ZF_ReallocElements((void**)&envmapidx, &nenvmapidx, cl.worldmodel->numsurfaces, sizeof(*envmapidx)))
			{
				for(i = 0; i < cl.worldmodel->numsurfaces; i++)
					envmapidx[i] = Mod_NearestCubeForSurf(cl.worldmodel->surfaces+i, envmap, nenvmap);
			}

			Mod_BSPXRW_SetLump(&bspctx, "ENVMAP", envmap, nenvmap*sizeof(*envmap));
			Mod_BSPXRW_SetLump(&bspctx, "SURFENVMAP", envmapidx, cl.worldmodel->numsurfaces*sizeof(*envmapidx));
			Mod_BSPXRW_Write(&bspctx);

			if (build)
			{
				// cubemaps present. take screenshots for each one.
				for (i = 0; i < nenvmap; i++)
					SCR_ScreenShot_Cubemap(envmap[i].origin, envmap[i].cubesize);
			}
		}
		else
		{
			Con_Printf("No cubemaps found on map\n");
			Mod_BSPXRW_Free(&bspctx);
		}

		Z_Free(envmapidx);
		Z_Free(envmap);
	}
}

static void Mod_FindCubemaps_f(void)
{
	Mod_FindCubemaps(false);
}

static void Mod_BuildCubemaps_f(void)
{
	size_t i;

	if (!cls.state || !cl.worldmodel || cl.worldmodel->loadstate != MLS_LOADED)
	{
		Con_Printf("Please start a map first\n");
		return;
	}

	if (!cl.worldmodel->numenvmaps)
	{
		// lump is not present or is empty. attempt to find cubemaps and then take screenshots.
		Mod_FindCubemaps(true);
		return;
	}

	// cubemaps present. take screenshots for each one.
	for (i = 0; i < cl.worldmodel->numenvmaps; i++)
		SCR_ScreenShot_Cubemap(cl.worldmodel->envmaps[i].origin, cl.worldmodel->envmaps[i].cubesize);
}

static void Mod_Realign_f(void)
{
	struct bspxrw bspctx;
	if (Mod_BSPXRW_Read(&bspctx, cl.worldmodel->name))
		Mod_BSPXRW_Write(&bspctx);
}
static searchpathfuncs_t *Mod_BSPX_List_ArchiveFile_handle;
static void QDECL Mod_BSPX_List_ArchiveFile(int depth, const char *fname, fsbucket_t *filehandle, void *pathhandle)
{
	searchpathfuncs_t *archive = Mod_BSPX_List_ArchiveFile_handle;
	flocation_t loc;
	time_t mtime;
	char timestr[MAX_QPATH], sizestr[MAX_QPATH];
	archive->FindFile(archive, &loc, fname, pathhandle);
	archive->FileStat(archive, &loc, &mtime);

	*timestr = 0;
	strftime(timestr,sizeof(timestr), "%Y-%m-%d %H:%M:%S", localtime(&mtime));
	Con_Printf("\t\t%20s: %-12s %s\n", fname, FS_AbbreviateSize(sizestr,sizeof(sizestr), loc.len), timestr);
}
static void Mod_BSPX_List_f(void)
{
	int i;
	struct bspxrw ctx;
	char *fname = Cmd_Argv(1);
	if (!*fname && cl.worldmodel)
		fname = cl.worldmodel->name;
	if (Mod_BSPXRW_Read(&ctx, fname))
	{
		Con_Printf("%s:\n", fname);
		for (i = 0; i < ctx.totallumps; i++)
			Con_Printf("\t%20s: %-12"PRIuQOFS" csum:%08x\n", ctx.lumps[i].lumpname, ctx.lumps[i].filelen, LittleLong (CalcHashInt(&hash_md4, ctx.lumps[i].data, ctx.lumps[i].filelen)));
		if (ctx.archivesize)
		{
			searchpathfuncs_t *f;
			vfsfile_t *tmp = VFSPIPE_Open(1,true);
			if (tmp)
			{
				VFS_WRITE(tmp, ctx.archivedata, ctx.archivesize);
				f = FSZIP_LoadArchive(tmp, NULL, ctx.fname, ctx.fname, NULL);
				if (!f)
					VFS_CLOSE(tmp);
				else
				{
					Con_Printf("\t%20s: %-12"PRIuQOFS"\n", "archive", ctx.archivesize);
					Mod_BSPX_List_ArchiveFile_handle = f;
					f->BuildHash(f, 0, Mod_BSPX_List_ArchiveFile);
					f->ClosePath(f);
				}
			}
		}
		Mod_BSPXRW_Free(&ctx);
	}
}
void Mod_BSPX_Strip_f(void)
{
	int i;
	struct bspxrw ctx;
	qboolean found = false;
	if (Cmd_Argc() != 3)
		Con_Printf("%s FILENAME NAME: removes an extended lump from the named bsp file\n", Cmd_Argv(0));
	else if (Mod_BSPXRW_Read(&ctx, Cmd_Argv(1)))
	{
		for (i = ctx.corelumps; i < ctx.totallumps;)
		{
			if (!Q_strcasecmp(ctx.lumps[i].lumpname, Cmd_Argv(2)))
			{
				found = true;
				memmove(&ctx.lumps[i], &ctx.lumps[i+1], sizeof(ctx.lumps[0])*(ctx.totallumps-(i+1)));
				ctx.totallumps--;
			}
			else
				i++;
		}
		if (found)
			Mod_BSPXRW_Write(&ctx);
		else
			Mod_BSPXRW_Free(&ctx);
	}
}

static qbyte *Mod_BSPX_Injest_Read(qofs_t *sz, char *name, char *fnamefmt, ...)
{
	va_list		argptr;
	char		fname[MAX_QPATH];
	int i;

	va_start (argptr, fnamefmt);
	if (Q_vsnprintfz (fname,sizeof(fname), fnamefmt,argptr))
		*fname = 0;
	va_end (argptr);
	if (!*fname)
		return NULL;

	for (i = 2; i < Cmd_Argc(); i++)
	{
		if (!Q_strcasecmp(Cmd_Argv(i), "all") || !Q_strcasecmp(Cmd_Argv(i), name))
			return FS_MallocFile(fname, FS_GAME, sz);
	}
	Con_Printf("%s not requested\n", name);
	return NULL;
}
static void Mod_BSPX_Injest_f(void)
{
	qbyte *tmp;
	struct bspxrw ctx;
	qboolean found = false;
	qofs_t sz;
	if (Cmd_Argc() <= 2)
		Con_Printf("%s FILENAME <all|LUMPNAMELIST>: inserts external supplementary resources into the bsp. "CON_WARNING"REMEMBER TO BACK-UP FIRST!\n", Cmd_Argv(0));
	else if (Mod_BSPXRW_Read(&ctx, Cmd_Argv(1)))
	{
		char noext[MAX_QPATH];
		COM_StripExtension(ctx.fname, noext,sizeof(noext));
		if (ctx.fg == fg_quake)
		{
			tmp = Mod_BSPX_Injest_Read(&sz, "lit", "%s.lit", noext);
			if (tmp)
			{
				if (sz == 8+ctx.lumps[LUMP_LIGHTING].filelen*3 && !memcmp(tmp, "QLIT\0x01\x00\x00\x00", 8))
					Con_Printf("Injesting sdr lit file\n"), found|=Mod_BSPXRW_SetLump(&ctx, "RGBLIGHTING", tmp+8, sz-8);		//ldr
				else if (sz == 8+ctx.lumps[LUMP_LIGHTING].filelen*4 && !memcmp(tmp, "QLIT\0x01\x00\x01\x00", 8))
					Con_Printf("Injesting hdr lit file\n"), found|=Mod_BSPXRW_SetLump(&ctx, "LIGHTING_E5BGR9", tmp+8, sz-8);	//hdr
				FS_FreeFile(tmp);
			}

			tmp = Mod_BSPX_Injest_Read(&sz, "lux", "%s.lux", noext);
			if (tmp)
			{
				if (sz == 8+ctx.lumps[LUMP_LIGHTING].filelen*3 && !memcmp(tmp, "QLIT\0x01\x00\x00\x00", 8))
					Con_Printf("Injesting lux file\n"), found|=Mod_BSPXRW_SetLump(&ctx, "LIGHTINGDIR", tmp+8, sz-8);
				FS_FreeFile(tmp);
			}

			tmp = Mod_BSPX_Injest_Read(&sz, "ent", "%s.ent", noext);
			if (tmp)
			{
				if (sz)
					Con_Printf("Injesting ent file\n"), found|=Mod_BSPXRW_SetLump(&ctx, "entities", tmp, sz);
				FS_FreeFile(tmp);
			}

//			tmp = Mod_BSPX_Injest_Read(&sz, "vis", "%s.vis", noext);
//			tmp = Mod_BSPX_Injest_Read(&sz, "rtlights", "%s.rtlights", noext);
//			tmp = Mod_BSPX_Injest_Read(&sz, "way", "%s.way", noext);
			//fixme: cubemaps
		}

		if (found)
			Mod_BSPXRW_Write(&ctx);
		else
		{
			Mod_BSPXRW_Free(&ctx);
			Con_Printf("%s \"%s\": nothing changed.\n", Cmd_Argv(0), Cmd_Argv(1));
		}
	}
}

image_t *Mod_CubemapForOrigin(model_t *wmodel, vec3_t org)
{
	int i;
	menvmap_t       *e;
	float bestdist = FLT_MAX, dist;
	image_t *ret = NULL;
	vec3_t move;
	if (!wmodel || wmodel->loadstate != MLS_LOADED)
		return NULL;
	for ( i=0 , e=wmodel->envmaps ; i<wmodel->numenvmaps ; i++, e++)
	{
		VectorSubtract(org, e->origin, move);
		dist = DotProduct(move,move);
		if (bestdist > dist)
		{
			bestdist = dist;
			ret = e->image;
		}
	}
	return ret;
}

void Mod_BSPX_Init(void)
{
	Cmd_AddCommandD("mod_findcubemaps", Mod_FindCubemaps_f, "Scans the entities of a map to find reflection env_cubemap sites and determines the nearest one to each surface.");
	Cmd_AddCommandD("mod_buildcubemaps", Mod_BuildCubemaps_f, "Automatically builds cubemaps for each env_cubemap present in the map.");
	Cmd_AddCommandD("mod_realign", Mod_Realign_f, "Reads the named bsp and writes it back out with only alignment changes.");
	Cmd_AddCommandD("mod_bspx_list", Mod_BSPX_List_f, "Lists all lumps (and their sizes) in the specified bsp.");
	Cmd_AddCommandD("mod_bspx_strip", Mod_BSPX_Strip_f, "Strips a named extension lump from a bsp file.");
	Cmd_AddCommandD("mod_bspx_injest", Mod_BSPX_Injest_f, "Injests supplemental files (like .lits) into a new bspx file. "CON_WARNING"REMEMBER TO BACK-UP FIRST!");
}

#endif
