/*
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
	TODO:
		Lightmaps - skip ssbump stuff properly. currently extra lightstyles are screwed.
		Ent Lighting - leafs have some list of light 'cubes' that define lighting, allowing for ents to get backlit etc properly.
		Areaportals - the server 'needs' a way to specify areaportals on a per-player basis. for now the gamecode will have to explicitly force-open most of the portals in the game (map_noareas can be used as a workaround).
		Static Props - these are solid, but use the visible mesh for collisions instead of the special/simpler collision mesh. This makes it a bit easier to climb up them, which may be a gameplay issue.
		Detail Props - we don't attempt to handle these. They need manual batching or something (eg clutter-shader stuff). Their loss should not affect gameplay much as they can be disabled in HL2 too.
		Dynamic Lighting - r_dynamic not enabled.
		Realtime Lighting - screwed. Doesn't light all world surfaces for some reason.
		RBE(Bullet) - probably screwed.
		Skyboxes - doesn't handle the whole per-face skies nor weird hdr encoding.
		Fog - no fog here... this results in issues where the pvs provides distance culling.
		Load Times - materials are loaded on a single thread. this gets slow.
		Materials - all kinds of screwed.
		Portal2 Gels - we need to repurpose stainmap code or something.
		Refraction - this stuff is horribly expensive.
*/

#include "../plugin.h"
#include "quakedef.h"
#ifdef HAVE_CLIENT
#include "glquake.h"
#endif
#include "com_mesh.h"
#include "com_bih.h"
#include "LzmaDec.h"	//nettest: Strata Source (BSP v25) + newer Valve maps LZMA-compress individual lumps

static plugfsfuncs_t		*filefuncs;
static plugmodfuncs_t		*modfuncs;
static plugthreadfuncs_t	*threadfuncs;

#define Q_strncpyz Q_strlcpy
float VectorNormalize2(const vec3_t in,vec3_t out) {float l = sqrt(DotProduct(in,in)); if (l) l = 1.0/l; VectorScale(in,l,out); return l;}
#define VectorNormalize(v) VectorNormalize2(v,v)
fte_inlinebody float M_LinearToSRGB(float x, float mag);
fte_inlinebody float M_SRGBToLinear(float x, float mag);	//FTESurf Patch 163
vec3_t vec3_origin;
static refdef_t *refdef;

static vec3_t modelorg;
static qbyte *frustumvis;
static int vbsp_nodesequence; //to track which nodes need walking
static int vbsp_surfsequence; //so we don't draw the same surface if its found multiple ways

//FTESurf Build 8: how much of the world walk the AREA test is throwing away.
//Reset per frame in VBSP_PrepareFrame, reported when the camera changes area.
//The leaf-vis and frustum tests have already passed by the time these count,
//so areakill is the geometry that vis WOULD have drawn and areas removed --
//which is the only number that says whether area culling is costing anything.
static int vbsp_leaf_invis, vbsp_leaf_areakill;
/* FTESurf Patch 273: what the marking loop actually touched, so the cluster-bucket
   claim is a measurement and not an assertion.  marks = calls that ran the loop
   (i.e. cache misses); markleaves = leaves visited across them. */
static int vbsp_mark_runs, vbsp_mark_leaves;
/* FTESurf Patch 273: which recursion level's marks the node tree currently holds.
   -1 means "a forcevis view's, i.e. nobody's", so every level re-marks. */
static int vbsp_markowner = -1;

r_qrenderer_t qrenderer = QR_OPENGL;

#define	MAX_VBSP_AREAS		MAX_Q2MAP_AREAS
#define SURF_OFFNODE		SURF_DRAWBACKGROUND	//might as well just reuse that.

static cvar_t *hl2_novis;
static cvar_t *hl2_pvscache;	//FTESurf Patch 273: memoise the headnode descent
static cvar_t *hl2_displacement_scale;
static cvar_t *hl2_favour_ldr;
static cvar_t *hl2_lumpretire;	//FTESurf Patch 298
static cvar_t *hl2_lightmap_repack;	//FTESurf Patch 281: see VBSP_RepackLighting
static cvar_t *hl2_bumpstride;	//FTESurf Patch 268 D1: tag surfaces with their per-style luxel-set stride, see VBSP_ProbeLightmapStride
static cvar_t *map_noareas;
static cvar_t *map_autoopenportals;
static cvar_t *hl2_areaportals;		//FTESurf Build 8
static cvar_t *hl2_contents_remap;
cvar_t *hl2_propcollision;	//FTESurf Build 8: shared with mod_hl2.c, which picks the model's collision shape from it
cvar_t *hl2_propcollision_nophy;	//FTESurf Patch 262: likewise -- "no .phy" means non-solid, not "use the render mesh"

//FTESurf Build 8: the .phy collision-hull census, filled in by mod_phy.c as
//models load and reported with the static-prop counts below.
//FTESurf Build 10: mat_vmt.c's counters and the two cvars that drive them.
extern int vmt_stat_water, vmt_stat_refract, vmt_stat_translucent;
extern int vmt_stat_missing;
#define VMT_MISSING_LIST 256	//FTESurf Patch 174/231, must match mat_vmt.c
#define VMT_MISSING_SHOW 24		//FTESurf Patch 231: how many of them the warning prints; `hl2_missing` prints the rest
static int vmt_census_atload;	//FTESurf Patch 231: the count as of the load-time warning, see there
//FTESurf Patch 233: what each of those names IS, worked out once while the world model is
//in hand and kept so `hl2_missing` can print it later without one.  See VBSP_MissReason.
static char vmt_miss_reason[VMT_MISSING_LIST][72];
static int vmt_miss_reasons;
//...and how many of them are on something the player can end up looking at.  Published as
//hl2_unresolved_visible, which is what decides whether the HUD banner is worth drawing.
static int vmt_miss_visible;
extern char vmt_stat_missing_name[VMT_MISSING_LIST][MAX_QPATH];
extern int vmt_stat_unparsed;	//FTESurf Patch 181, found but would not parse -- a different fault
extern char vmt_stat_unparsed_name[VMT_MISSING_LIST][MAX_QPATH];
void Mat_VMT_ResetStats (void);
extern cvar_t *hl2_water, *hl2_refract, *hl2_translucent;
extern cvar_t *hl2_animated;	//FTESurf Patch 195, owned by mat_vmt.c like the rest
extern cvar_t *hl2_vertexcolor;	//FTESurf Patch 232, likewise
extern cvar_t *hl2_decallit;	//FTESurf Patch 236's kill switch, added by 238
extern cvar_t *hl2_vmtkeys;		//FTESurf Patch 250, same ownership
extern cvar_t *hl2_emissive;	//FTESurf Patch 250, the $emissiveblend pass
extern cvar_t *hl2_seamless;	//FTESurf Patch 251, $seamless_scale triplanar projection
extern cvar_t *hl2_bumpmap2;	//FTESurf Patch 251, the second normal map
extern int vmt_stat_seamless, vmt_stat_bumpmap2;	//FTESurf Patch 251
extern int vmt_stat_animated, vmt_stat_animunlit;	//FTESurf Patch 195, 253
extern cvar_t *hl2_imposter;	//FTESurf Patch 263, WindowImposter
extern cvar_t *hl2_twotexture;	//FTESurf, UnlitTwoTexture's second layer
extern cvar_t *hl2_twoframes;	//FTESurf Patch 286, and its flipbook
extern int vmt_stat_twotexture, vmt_stat_proxfade, vmt_stat_proxframe;	//FTESurf
extern int vmt_stat_twoframes, vmt_stat_twoframesdecl;	//FTESurf Patch 286
extern int vmt_stat_imposter;
extern int vmt_stat_hdrenvmap;	//FTESurf Patch 264
extern int vmt_stat_ignorez;	//FTESurf Patch 290
extern int vmt_stat_addprog, vmt_stat_addpass;	//FTESurf Patch 300
extern cvar_t *hl2_additivefog;	//FTESurf Patch 300
extern int vmt_stat_unknown;	//FTESurf Patch 263, materials whose shader class is unimplemented
extern char vmt_stat_unknown_name[4][64];
extern char vmt_stat_waterargs[256];	//FTESurf Patch 251
//FTESurf Patch 254: the water material's own fog, for the engine's FOGTYPE_WATER slot.
extern char  vmt_stat_waterfog[64];
extern float vmt_stat_waterfogstart, vmt_stat_waterfogend;
extern int   vmt_stat_waterfog_set;
void Mat_VMT_KeyCensus_f (void);
/*
FTESurf Patch 233: the $bottommaterial link mat_vmt.c now keeps, and the two operations
this file performs on the census with it.  See VMT_BottomRecord in mat_vmt.c for what the
link is and why a name rule could not have produced it.
*/
const char *Mat_VMT_BottomParent(const char *name);
qboolean Mat_VMT_CensusMissing(const char *name);
void Mat_VMT_CensusForget(const char *name);
static cvar_t *hl2_bottommaterial;
cvar_t *hl2_skinfallback;		//FTESurf Patch 234: read by Mod_LoadHL2Model in mod_hl2.c, registered here with the rest
extern cvar_t *hl2_dither_alpha, *hl2_bumpmap, *hl2_envmap;	//FTESurf Build 13, all owned by mat_vmt.c
extern cvar_t *hl2_envcubemap, *hl2_envmap_source;	//FTESurf Patch 268 B, likewise
extern cvar_t *hl2_cubelight, *hl2_bumpgreenfix;	//FTESurf Patch 268 C, likewise
extern cvar_t *hl2_dither_force;	//FTESurf Patch 174, likewise
cvar_t *hl2_texdiag;	//FTESurf build 11: the engine's r_texdiag, borrowed -- see VBSP_GenerateMaterials.
							//Patch 239 dropped `static`: img_vtf.c reports the per-file image format under it.
static cvar_t *hl2_propdist;	//FTESurf Patch 193: default draw distance for props that declare no fade -- see VBSP_PrepareFrame.
cvar_t *hl2_rigidprops;			//FTESurf Patch 230: read by Mod_LoadHL2Model in mod_hl2.c, registered here with the rest.

extern volatile int phy_stat_hull, phy_stat_nofile, phy_stat_jointed;
extern volatile int phy_stat_rejected, phy_stat_bbox, phy_stat_tris;
extern volatile int phy_stat_tris_outward, phy_stat_tris_inverted;	//FTESurf Patch 317
void Mod_PHY_ResetStats (void);
cvar_t *hl2_phywinding;	//FTESurf Patch 317: read by Mod_LoadHL2Model in mod_hl2.c, registered here beside hl2_dispwinding
/*FTESurf Patch 318 -- see the essay above VBSP_AddBrushBevels.  Up here because
  VBSP_LoadBrushes resets the counters and sits well above the generator. */
static cvar_t *hl2_brushbevels;
static volatile int vbsp_stat_bevelbrushes;	//brushes that gained at least one plane
static volatile int vbsp_stat_bevelplanes;	//planes added in total
static volatile int vbsp_stat_bevelfull;	//brushes left alone for want of room
static cvar_t *hl2_dispcollision;
static cvar_t *hl2_dispflags;	//FTESurf Patch 250
static cvar_t *hl2_dispwinding;	//FTESurf Patch 256
cvar_t *hl2_hidetools;	//FTESurf: shared with mat_vmt.c
static cvar_t *hl2_overlays;	//FTESurf: LUMP_OVERLAYS -> info_overlay entity synthesis
static cvar_t *hl2_cubemaps;		//FTESurf Build 13: env_cubemap, see VBSP_LoadCubemaps
static cvar_t *hl2_lt_baked;		//FTESurf Patch 163: sp_N.vhv static prop lighting, see VBSP_LoadPropBakedLight
static cvar_t *hl2_lt_baked_vc;		//FTESurf Patch 259: the live on/off for the per-vertex half of it
static int vbsp_vc_live = -1;		//...as last applied, so a change can invalidate the cached lighting solve
static model_t *vbsp_vc_mod;		//...and which map that was for
static int cube_stat_count, cube_stat_missing, cube_stat_default;

char *Q_strlwr(char *s)
{
	char *ret=s;
	while(*s)
	{
		if (*s >= 'A' && *s <= 'Z')
			*s=*s-'A'+'a';
		s++;
	}

	return ret;
}

void AddPointToBounds (const vec3_t v, vec3_t mins, vec3_t maxs)
{
	int		i;
	vec_t	val;

	for (i=0 ; i<3 ; i++)
	{
		val = v[i];
		if (val < mins[i])
			mins[i] = val;
		if (val > maxs[i])
			maxs[i] = val;
	}
}

void ClearBounds (vec3_t mins, vec3_t maxs)
{
	mins[0] = mins[1] = mins[2] = FLT_MAX;
	maxs[0] = maxs[1] = maxs[2] = -FLT_MAX;
}


/*
==================
BoxOnPlaneSide

Returns 1, 2, or 1 + 2
==================
*/
int VARGS BoxOnPlaneSide (const vec3_t emins, const vec3_t emaxs, const mplane_t *p)
{
	float	dist1, dist2;
	int		sides;

// general case
	switch (p->signbits)
	{
	default:
	case 0:
dist1 = p->normal[0]*emaxs[0] + p->normal[1]*emaxs[1] + p->normal[2]*emaxs[2];
dist2 = p->normal[0]*emins[0] + p->normal[1]*emins[1] + p->normal[2]*emins[2];
		break;
	case 1:
dist1 = p->normal[0]*emins[0] + p->normal[1]*emaxs[1] + p->normal[2]*emaxs[2];
dist2 = p->normal[0]*emaxs[0] + p->normal[1]*emins[1] + p->normal[2]*emins[2];
		break;
	case 2:
dist1 = p->normal[0]*emaxs[0] + p->normal[1]*emins[1] + p->normal[2]*emaxs[2];
dist2 = p->normal[0]*emins[0] + p->normal[1]*emaxs[1] + p->normal[2]*emins[2];
		break;
	case 3:
dist1 = p->normal[0]*emins[0] + p->normal[1]*emins[1] + p->normal[2]*emaxs[2];
dist2 = p->normal[0]*emaxs[0] + p->normal[1]*emaxs[1] + p->normal[2]*emins[2];
		break;
	case 4:
dist1 = p->normal[0]*emaxs[0] + p->normal[1]*emaxs[1] + p->normal[2]*emins[2];
dist2 = p->normal[0]*emins[0] + p->normal[1]*emins[1] + p->normal[2]*emaxs[2];
		break;
	case 5:
dist1 = p->normal[0]*emins[0] + p->normal[1]*emaxs[1] + p->normal[2]*emins[2];
dist2 = p->normal[0]*emaxs[0] + p->normal[1]*emins[1] + p->normal[2]*emaxs[2];
		break;
	case 6:
dist1 = p->normal[0]*emaxs[0] + p->normal[1]*emins[1] + p->normal[2]*emins[2];
dist2 = p->normal[0]*emins[0] + p->normal[1]*emaxs[1] + p->normal[2]*emaxs[2];
		break;
	case 7:
dist1 = p->normal[0]*emins[0] + p->normal[1]*emins[1] + p->normal[2]*emins[2];
dist2 = p->normal[0]*emaxs[0] + p->normal[1]*emaxs[1] + p->normal[2]*emaxs[2];
		break;
	}

	sides = 0;
	if (dist1 >= p->dist)
		sides = 1;
	if (dist2 < p->dist)
		sides |= 2;

	return sides;
}


#define Host_Error Sys_Errorf

enum hllumps_e
{	//note how this order matches q1 so well...
	//and yet the format is disturbingly similar to q2... huh... :P
	VLUMP_ENTITIES		= LUMP_ENTITIES,
	VLUMP_PLANES		= LUMP_PLANES,
	VLUMP_TEXTURES		= LUMP_TEXTURES,
	VLUMP_VERTEXES		= LUMP_VERTEXES,
	VLUMP_VISIBILITY	= LUMP_VISIBILITY,
	VLUMP_NODES			= LUMP_NODES,
	VLUMP_TEXINFO		= LUMP_TEXINFO,
	VLUMP_FACES_LDR		= LUMP_FACES,
	VLUMP_LIGHTING_LDR	= LUMP_LIGHTING,
//	VLUMP_FOO			= 9,//LUMP_CLIPNODES
	VLUMP_LEAFS			= LUMP_LEAFS,
//	VLUMP_FOO			= 11,//LUMP_MARKSURFACES
	VLUMP_EDGES			= LUMP_EDGES,
	VLUMP_SURFEDGES		= LUMP_SURFEDGES,
	VLUMP_MODELS		= LUMP_MODELS,
	VLUMP_WORLDLIGHTS_LDR = 15,	//FTESurf Patch 302: dworldlight_t[], the direct lights VRAD used. See VBSP_LoadWorldLights.
	VLUMP_LEAFFACES		= 16,
	VLUMP_LEAFBRUSHES	= 17,
	VLUMP_BRUSHES		= 18,
	VLUMP_BRUSHSIDES	= 19,
	VLUMP_AREAS			= 20,
	VLUMP_AREAPORTALS	= 21,
//	VLUMP_FOO			= 22,
//	VLUMP_FOO			= 23,
//	VLUMP_FOO			= 24,
//	VLUMP_FOO			= 25,
	VLUMP_DISP_INFO		= 26,
//	VLUMP_FOO			= 27,
//	VLUMP_FOO			= 28,
//	VLUMP_FOO			= 29,
//	VLUMP_FOO			= 30,
//	VLUMP_FOO			= 31,
	VLUMP_DISP_LMALPHA	= 32,
	VLUMP_DISP_VERTS	= 33,
	VLUMP_DISP_LMCOORDS	= 34,
	VLUMP_GAMELUMP		= 35,
//	VLUMP_FOO			= 36,
//	VLUMP_FOO			= 37,
//	VLUMP_FOO			= 38,
//	VLUMP_FOO			= 39,

	VLUMP_ZIPFILE		= 40,
	VLUMP_AREAPORTALVERTS = 41,
	VLUMP_CUBEMAPS		= 42,	//FTESurf Build 13: env_cubemap origins + face size
	VLUMP_STRINGDATA	= 43,
	VLUMP_STRINGOFFSETS	= 44,
	VLUMP_OVERLAYS		= 45,	//FTESurf: baked info_overlay decals. See VBSP_LoadEntities.
//	VLUMP_FOO			= 46,
//	VLUMP_FOO			= 47,
	VLUMP_DISP_TRIFLAGS	= 48,
//	VLUMP_FOO			= 49,
//	VLUMP_FOO			= 50,
	VLUMP_LEAFLIGHTI_HDR= 51,	//indexes into VLUMP_LEAFLIGHTV_HDR, two shorts per leaf.
	VLUMP_LEAFLIGHTI_LDR= 52,
	VLUMP_LIGHTING_HDR	= 53,
	VLUMP_WORLDLIGHTS_HDR = 54,	//FTESurf Patch 302
	VLUMP_LEAFLIGHTV_HDR= 55,
	VLUMP_LEAFLIGHTV_LDR= 56,
//	VLUMP_FOO			= 57,
	VLUMP_FACES_HDR		= 58,
//	VLUMP_FOO			= 59,
//	VLUMP_FOO			= 60,
//	VLUMP_FOO			= 61,
//	VLUMP_FOO			= 62,
//	VLUMP_FOO			= 63,
	HL2_MAXLUMPS 		= 64
};
typedef struct {
	unsigned int fileofs;
	unsigned int filelen;
	unsigned int version;
	unsigned int fourcc;
} vlump_t;
typedef struct {
	unsigned int magic;
	unsigned int version;
	vlump_t lumps[HL2_MAXLUMPS];
} dvbspheader_t;

typedef struct
{
	int		numareaportals;
	int		firstareaportal;
} carea_t;
typedef struct
{
	int		floodnum;			// if two areas have equal floodnums, they are connected
	int		floodvalid;			// flags the area as having been visited (sequence numbers matching prv->floodvalid)
} careaflood_t;
typedef struct cmodel_s
{
	vec3_t		mins, maxs;
	vec3_t		origin;		// for sounds or lights
	mnode_t		*headnode;
	mleaf_t		*headleaf;
	int		numsurfaces;
	int		firstsurface;

	int firstbrush;
	int num_brushes;
} cmodel_t;
typedef struct dispinfo_s
{
    struct msurface_s *surf;
    vec3_t aamin;
    vec3_t aamax;
    pvscache_t pvs; //which pvs clusters this displacement is visible in...
    unsigned int contents;
    unsigned int collflags;	//FTESurf Patch 250: DISPSURF_* -- the mapper's per-displacement collision opt-out.
    unsigned int width;
    unsigned int height;
    vecV_t *xyz;    //(width+1)*(height+1)
    index_t *idx;   //width*height*6;
    index_t *cidx;  //FTESurf Patch 256: the same triangles for COLLISION, wound outward.
                    //Aliases idx unless the parent face is SURF_PLANEBACK; see the essay
                    //above VBSP_LoadDisplacements' winding block.  Never handed to a mesh.
    size_t numindexes;
    struct dispvert_s
    {
        vec3_t norm;
        vec2_t st;
        float alpha;
    } *verts;
    //unsigned short *flags;
} dispinfo_t;

typedef struct vbspinfo_s
{
	/*
	FTESurf Patch 273 -- ONE VIS CACHE PER RECURSION LEVEL.

	This was a single slot, and on any map with a sky_camera two views want it
	every frame: the player's (recurse 0) and the 3D skybox's (recurse 1).  They
	evicted each other, so NEITHER ever hit and the leaf-marking loop ran twice a
	frame standing still.  Patch 271 had to poison the slot after a recursed view
	precisely because the ONE visbuf was shared -- its own essay says so:
	"prv->vcache.visbuf is one buffer shared by every recursion level, so a
	recursed view necessarily overwrites what the main view computed".

	Giving each level its own buffer removes the hazard at the root instead of
	working around it: the poison is no longer needed, both views cache, and the
	buffers become STABLE and DISTINCT -- which is also what lets the headnode
	memo below tell the two views apart by pointer.
	*/
	struct
	{
		qbyte *vis;
		pvsbuffer_t visbuf;
		int viewcluster[2];
	} vcache[R_MAX_RECURSE];

	/*
	FTESurf Patch 273 -- leaves bucketed by cluster, so VBSP_MarkLeaves walks the
	VISIBLE clusters instead of every leaf in the map.  See the essay above the
	marking loop.  Built once at the tail of VBSP_LoadLeafs, allocated from
	mod->memgroup so it is freed with the model and needs no teardown.

	clusterfirstleaf == NULL means "not built", and every consumer keeps its
	original whole-map loop as the fallback.  Strictly additive by design: if the
	precompute is ever wrong, the old path is one branch away.
	*/
	unsigned int	*clusterfirstleaf;	//[numclusters+1], prefix offsets into clusterleafs
	unsigned int	*clusterleafs;		//[numclusteredleafs] leaf indices, cluster-major
	unsigned int	numclusteredleafs;

	int				numbrushsides;
	q2cbrushside_t *brushsides;

	q2mapsurface_t	*surfaces;
	dispinfo_t		**surfdisp;

	int				numleafbrushes;
	q2cbrush_t		**leafbrushes;

	int				numcmodels;
	cmodel_t		*cmodels;

	int				numbrushes;
	q2cbrush_t		*brushes;

	int				numvisibility;
	q2dvis_t		*vis;
	qbyte			*phscalced;

	struct vbsptexinfo_s
	{
		vec4_t lmvecs[2];
		qboolean bumped;	//FTESurf Patch 268 D1: SURF_BUMPLIGHT -- VRAD wrote NUM_BUMP_VECTS+1 = 4 luxel sets per lightstyle for this face
	} *texinfo;

	int				numareas;
	int				floodvalid;
	int				lastreportedarea;	//FTESurf Build 8: edge-detects the area print in VBSP_PrepareFrame
	int				areastatframes;		//...and one-shots the area-cull cost report
	int				markstatframes;		//FTESurf Patch 273: REPEATING window for the mark census
	double			nextareareport;		//...and rate-limits it on an area boundary
	careaflood_t	areaflood[MAX_VBSP_AREAS];
	//areas have a list of portals that open into other areas.
	carea_t			*areas;	//indexes into q2areaportals for flooding
	size_t			numareaportals;
	q2dareaportal_t	*areaportals;

	//and this is the state that is actually changed. booleans.
	qbyte			portalopen[MAX_Q2MAP_AREAPORTALS];	//memset will work if it's a qbyte, really it should be a qboolean
	mplane_t		**portalplane;
	qbyte			portalquerying[MAX_Q2MAP_AREAPORTALS];
	mesh_t			*portalpoly;		//[numq2areaportals]
	int				*occlusionqueries; //[numq2areaportals]

	struct mleaflight_s
	{
		struct leaflightpoint_s
		{
			vec3_t rgb[6];
			qbyte x, y, z;
		} *point;
		int count;
	} *leaflight;

	/*
	FTESurf Patch 302 -- THE HALF OF SOURCE'S LIGHT CACHE WE NEVER IMPLEMENTED.

	The leaf ambient cube above is BOUNCE ONLY.  VRAD puts a light into it only
	when the light carries DWL_FLAGS_INAMBIENTCUBE, which IsLeafAmbientSurfaceLight
	(utils/vrad/leaf_ambient_lighting.cpp:183-198) grants only to dim, unstyled
	emit_surface lights -- on surf_tensor2 that is 0 of 603.  Source makes up the
	difference at runtime: a static prop with no .vhv bake is lit by the light
	cache, which is the ambient cube PLUS the direct contribution of every nearby
	worldlight.  We had only the first half, so every .vhv-less prop on every map
	drew at roughly a fifth of its Source brightness.

	Measured, on the prop in Lex's surf_tensor2 save012 screenshot pair
	(models/props/surf/tensor2/stage2_detail02.mdl): the block renders at 0.40x
	the side wall beside it where CS:S renders it at 1.02x, while every other
	surface in the same frame matches CS:S to within 5%.  Summing these lights
	offline predicts a 4.44/4.66/4.77 boost; the screenshots demand 4.53/4.95/5.90.

	Kept per map, filtered at load to the set that can contribute statically.
	*/
	struct vworldlight_s
	{
		vec3_t	origin;
		vec3_t	intensity;
		vec3_t	normal;			//emitting surface's normal, or the spot axis
		int		cluster;
		int		type;			//emittype_t, already filtered to 0/1/2
		float	stopdot;		//start of a spotlight's penumbra
		float	stopdot2;		//end of it
		float	exponent;
		float	radius;			//hard cutoff, 0 = none
		float	constant_attn, linear_attn, quadratic_attn;
	} *worldlights;
	size_t		numworldlights;
	//Suppressed for exactly one population: props whose .vhv bake already
	//contains VRAD's direct light.  Set around the HL2_CalcModelLighting call in
	//the prop loop, never anywhere else.  See the essay there.
	qboolean	wl_suppress;
	/*
	FTESurf Patch 304: enabled for exactly one population -- static props, which
	are the only models guaranteed to render through plugins/hl2/glsl/vmt/
	vertexlit.glsl and therefore the only ones that get the matching HALF-lambert
	ramp.  Set around the same HL2_CalcModelLighting call wl_suppress is.

	Note the OPPOSITE sense to wl_suppress, and it is not an inconsistency.  Patch
	302's term is consumer-agnostic: it is simply more light in the cube, and a
	player model wants it as much as a prop does.  This one is a
	RE-PARAMETERISATION of the same cube into the shader's two slots, and it is
	only correct against the half-lambert ramp.  A consumer that reads these
	fields with a plain lambert -- anything on engine/shaders/glsl/defaultskin.glsl
	-- must keep the legacy fold or it gets the new base with the old ramp, which
	measured WORSE than either (0.996 stops against today's 0.574 on bounce-only
	cubes).  So the gate is per-consumer, and it is here because here is the only
	place that knows which population the call belongs to.
	*/
	qboolean	fold_prop;
	//A PVS row of our own, because VBSP_ClusterPVS's default buffer is a single
	//file-scope static that the next caller overwrites (:6281).
	pvsbuffer_t	wl_pvs;
	int			wl_pvscluster;	//which cluster wl_pvs currently holds, -2 = none

	size_t numdisplacements;
	dispinfo_t *displacements;

	size_t numstaticprops;
	//FTESurf Patch 152: the prop-lighting census, accumulated as the renderer
	//actually lights each prop (see the r_texdiag block in the draw loop).  It
	//cannot be gathered at load time -- VBSP_PointLeafnum is stubbed until the
	//model reaches MLS_LOADED.
	size_t	lit_n, lit_floor, lit_solid;
	double	lit_r, lit_g, lit_b;
	qboolean lit_reported;
	size_t	lit_baked;	//FTESurf Patch 163: props lit from their own sp_N.vhv
	size_t	lit_vc;		//FTESurf Patch 259: ...of which these kept it PER VERTEX
	size_t	lit_vcverts;	//...totalling this many vertices, i.e. 4 bytes each
	struct staticprop_s
	{
		float fademindist;
		float fademaxdist;
		qboolean solid;
		struct bihtransform_s transform;
		vec3_t	lightorg;
		//FTESurf Patch 163: the mean of VRAD's own baked per-vertex lighting for
		//THIS prop instance, 0..255 in the same gamma space res_ambient uses.
		vec3_t	baked;
		qboolean hasbaked;
		/*
		FTESurf Patch 262: this prop asked to be solid and got no BIH leaf, because
		it wants a hull and its model ships no .phy.  Recorded per prop rather than
		derived from the model, because it is a per-prop decision -- m_Solid 2 keeps
		its collision on the same model -- and because by the time `prop_census`
		asks, the model still has the trace tree every OTHER user of it needs.
		*/
		qboolean nocollide;
		/*
		FTESurf Patch 259: the same bake, kept per vertex instead of collapsed to
		its mean.  A greyscale multiplier in RGBA bytes, normalised so the prop's
		brightest vertex is exactly 255; vcgain is the factor that normalisation
		took out, which goes back into `baked` so the absolute level is unchanged.
		Both NULL unless hl2_lt_baked is 2.

		Two arrays because they are in two different orders and the file arrives
		before the model does.  vcraw is what the .vhv holds, in VRAD's own LOD0
		strip-group order, read at map load; vc is the same colours scattered into
		our vertex order through the model's vhvmap, which cannot happen until the
		model has finished loading.  The first frame that lights the prop does the
		scatter and frees vcraw.
		*/
		qbyte	*vcraw;
		unsigned int vcrawverts;
		qbyte	*vc;
		unsigned int vcverts;
		float	vcgain;
		//FTESurf Patch 257: why this prop did or did not reach the scene on the
		//last frame that drew props.  One of PROPV_*, for `prop_census`.
		qbyte	lastverdict;
		//...and, when that was PROPV_PVS, which half of the test said no.
		qbyte	lastpvswhy;
		//Distinct real areas its query box spanned, when it went through
		//VBSP_FindTouchedLeafs.  >2 means pvscache's two slots could not hold them.
		qbyte	nareas;
		entity_t ent;
	} *staticprops;

	/*
	FTESurf Patch 281: the lighting lump as the FILE holds it -- Source's linear
	ColorRGBExp32, 4 bytes a luxel -- and its length.  VBSP_LoadLighting converts
	the whole lump into mod->lightdata as 4-byte E5BGR9 words counted from byte 0,
	which is only right when every face's lightofs is a multiple of 4.  Nine maps
	in the Momentum library break that, and VBSP_RepackLighting needs the raw
	bytes back to convert those faces from where they really start.  Points into
	mod_base (the file buffer, or the decompressed lump table on an LZMA map), so
	it lives exactly as long as the load does; NULL when the map has no lighting
	or the server loaded it.
	lightrepacked is set only when VBSP_RepackLighting actually rewrote
	mod->lightdata (not on its early returns, not on the hl2_lightmap_repack 0
	arm), so VBSP_ProbeLightmapStride (Patch 268 D1) knows the layout is already
	one luxel set per style and no stride tag applies.
	*/
	const qbyte		*lightraw;
	size_t			lightrawsize;
	qboolean		lightrepacked;

	unsigned int contentsremap[32];
} vbspinfo_t;

/*
FTESurf Patch 257 -- PROP DRAW ACCOUNTING.

There has never been a "what did the prop loader drop, and what did the frame
cull" diagnostic.  ent_census covers entities with no spawn function and
hl2_missing covers materials; between them a static prop that is solid and not
drawn -- an invisible wall you have to surf around -- had nothing to report it,
and finding one meant flipping r_novis / hl2_propcollision / hl2_propdist by
hand and watching.

The counts are per-frame and the verdict is per-prop, because "241 of 429 drawn"
does not tell you whether the ONE you care about was culled, and on surf_garden
the one that matters is index 170.
*/
#define PROPV_DRAWN			0
#define PROPV_NOTLOADED		1	//model still loading, or failed to load
#define PROPV_FADED			2	//past its own FadeMaxDist
#define PROPV_PROPDIST		3	//past hl2_propdist (declares no fade of its own)
#define PROPV_PVS			4	//its leaves are not in the view PVS
#define PROPV_FRUSTUM		5	//off-screen
#define PROPV_UNVISITED		6	//the loop never reached it this frame
#define PROPV_MAX			7
static const char *vbsp_propverdict[PROPV_MAX] =
{
	"drawn", "model not loaded", "faded out", "beyond hl2_propdist",
	"PVS culled", "frustum culled", "not visited"
};
static struct
{
	model_t		*mod;
	unsigned int total;
	unsigned int n[PROPV_MAX];
	unsigned int leafoverflow;	//lump leaf list exceeded MAX_ENT_LEAFS at load
	unsigned int headnodetested;//...and ended up on the weaker headnode test
	//FTESurf Patch 259: of the props that DREW, how many drew with their
	//per-vertex bake bound, and how many had one loaded but declined it.
	//The load-time line counts what was READ; only this counts what is USED,
	//and the two legitimately differ -- a prop whose vertex order is not
	//VRAD's keeps its array and never binds it.
	unsigned int vcdrawn, vcdeclined;
} vbsp_propstat;

/*
FTESurf Patch 262: props that ASKED to be solid and got no collision, because
their model ships no .phy and Source would not collide them either.  Per map,
counted while the BIH is built, so it is the number of prop INSTANCES that
stopped being walls -- not the number of models.  Reported by the load-time
collision-shape line and by `prop_census`.
*/
static unsigned int vbsp_prop_nocollide;

/*
FTESurf Patch 262: does this model ship a .phy collision hull at all?

Answered here rather than passed down from the model loader, and probed off the
filesystem rather than inferred from the loaded model, for one reason: the
decision it feeds is per PROP and the loader only knows the MODEL.  m_Solid 2
asks for the model's bounding box, which Source builds with no .phy at all, so a
model shared between a solid-6 prop and a solid-2 prop has two different right
answers and only the prop record can pick between them.

Cached per model pointer for the life of the call.  A map has a few hundred
distinct prop models against a few thousand props, so this is a handful of
filesystem hits per map rather than one per prop -- and it is a load-time cost
either way, never a per-frame one.
*/
struct vbsp_phyprobe_s
{
	model_t *m;
	qboolean has;
};
static qboolean VBSP_ModelHasPhy(struct vbsp_phyprobe_s *cache, size_t *count, model_t *m)
{
	char name[MAX_QPATH];
	void *f;
	size_t flen, j;

	for (j = 0; j < *count; j++)
		if (cache[j].m == m)
			return cache[j].has;

	modfuncs->StripExtension(m->name, name, sizeof(name));
	Q_strncatz(name, ".phy", sizeof(name));
	f = filefuncs->LoadFile(name, &flen);
	if (f)
		plugfuncs->Free(f);

	cache[*count].m = m;
	cache[*count].has = !!f;
	(*count)++;
	return !!f;
}

/*
FTESurf Patch 257: WHICH HALF of the visibility test rejected the last entity.

VBSP_EdictInFatPVS has two independent rejection paths -- the area/areaportal
walk, and then either the leaf-cluster list or the headnode descent -- and
"PVS culled" does not say which.  They have completely different fixes, and
reading the code cannot settle it: the area walk's outcome depends on the map's
runtime area numbering and the headnode path's on a topnode computed at
run time.

It is a file-static rather than an out-parameter because the function is
published through mod->funcs.EdictInFatPVS (:7186) and its signature is fixed.
Written on every path out; read immediately after the call that set it.
*/
#define VBSP_PVSWHY_VISIBLE		0
#define VBSP_PVSWHY_AREA		1	//the areaportal walk found no connected area
#define VBSP_PVSWHY_LEAFS		2	//none of its clusters is in the view PVS
#define VBSP_PVSWHY_HEADNODE	3	//VBSP_HeadnodeVisible said no
#define VBSP_PVSWHY_MAX			4
static int vbsp_pvs_why;
static const char *vbsp_pvswhy_name[VBSP_PVSWHY_MAX] =
{
	"visible", "area walk", "cluster list", "headnode test"
};
//FTESurf Patch 257: distinct real areas the last VBSP_FindTouchedLeafs box spanned.
//pvscache_t has only two slots for them; more than two means the record is lossy.
static unsigned int vbsp_lasttouch_nareas;

static q2mapsurface_t	nullsurface;

static int		VBSP_NumInlineModels (model_t *model);
static cmodel_t	*VBSP_InlineModel (model_t *model, char *name);
static void VBSP_FinalizeBrush(model_t *mod, q2cbrush_t *brush);
static void	FloodAreaConnections (vbspinfo_t	*prv);

/*
===============================================================================

					MAP LOADING

===============================================================================
*/

static unsigned int VBSP_TranslateContentBits(vbspinfo_t *prv, unsigned int source)
{
	unsigned int ret = 0;
	if (source & 0x0000ffff)
	{
		if (source & 0x0000000f)
		{
			if (source & 0x00000001)	ret |= prv->contentsremap[ 0];
			if (source & 0x00000002)	ret |= prv->contentsremap[ 1];
			if (source & 0x00000004)	ret |= prv->contentsremap[ 2];
			if (source & 0x00000008)	ret |= prv->contentsremap[ 3];
		}
		if (source & 0x000000f0)
		{
			if (source & 0x00000010)	ret |= prv->contentsremap[ 4];
			if (source & 0x00000020)	ret |= prv->contentsremap[ 5];
			if (source & 0x00000040)	ret |= prv->contentsremap[ 6];
			if (source & 0x00000080)	ret |= prv->contentsremap[ 7];
		}
		if (source & 0x000000f00)
		{
			if (source & 0x00000100)	ret |= prv->contentsremap[ 8];
			if (source & 0x00000200)	ret |= prv->contentsremap[ 9];
			if (source & 0x00000400)	ret |= prv->contentsremap[10];
			if (source & 0x00000800)	ret |= prv->contentsremap[11];
		}
		//FTESurf Patch 250: was 0x000000f00, a copy-paste of the nibble gate above,
		//so bits 12-15 were only translated when an unrelated bit 8-11 happened to
		//be set.  CONTENTS_AREAPORTAL (0x8000) is bit 15 and areaportal brushes
		//carry nothing else, so it was dropped on essentially every one in the
		//library.  Vis only -- Q2CONTENTS_AREAPORTAL is in no solid mask, and a
		//gate can only ever remove bits, so this never made anything solid.
		if (source & 0x0000f000)
		{
			if (source & 0x00001000)	ret |= prv->contentsremap[12];
			if (source & 0x00002000)	ret |= prv->contentsremap[13];
			if (source & 0x00004000)	ret |= prv->contentsremap[14];
			if (source & 0x00008000)	ret |= prv->contentsremap[15];
		}
	}
	if (source & 0xffff0000)
	{
		if (source & 0x000f0000)
		{
			if (source & 0x00010000)	ret |= prv->contentsremap[16];
			if (source & 0x00020000)	ret |= prv->contentsremap[17];
			if (source & 0x00040000)	ret |= prv->contentsremap[18];
			if (source & 0x00080000)	ret |= prv->contentsremap[19];
		}
		if (source & 0x00f00000)
		{
			if (source & 0x00100000)	ret |= prv->contentsremap[20];
			if (source & 0x00200000)	ret |= prv->contentsremap[21];
			if (source & 0x00400000)	ret |= prv->contentsremap[22];
			if (source & 0x00800000)	ret |= prv->contentsremap[23];
		}
		if (source & 0x0f000000)
		{
			if (source & 0x01000000)	ret |= prv->contentsremap[24];
			if (source & 0x02000000)	ret |= prv->contentsremap[25];
			if (source & 0x04000000)	ret |= prv->contentsremap[26];
			if (source & 0x08000000)	ret |= prv->contentsremap[27];
		}
		if (source & 0xf0000000)
		{
			if (source & 0x10000000)	ret |= prv->contentsremap[28];
			if (source & 0x20000000)	ret |= prv->contentsremap[29];
			if (source & 0x40000000)	ret |= prv->contentsremap[30];
			if (source & 0x80000000)	ret |= prv->contentsremap[31];
		}
	}
	return ret;
}
static void VBSP_TranslateContentBits_Setup(vbspinfo_t *prv)
{
	size_t i, j;
	char contname[64];
	const char *contremap = hl2_contents_remap->string;
	static const struct {
		const char *name;
		unsigned int contents;
	} knowncontents[] =
	{
		{"empty", FTECONTENTS_EMPTY},
		{"solid", FTECONTENTS_SOLID},
		{"window", FTECONTENTS_WINDOW},
		{"lava", FTECONTENTS_LAVA},
		{"slime", FTECONTENTS_SLIME},
		{"water", FTECONTENTS_WATER},
		{"ladder", FTECONTENTS_LADDER},
		{"playerclip", FTECONTENTS_PLAYERCLIP},
		{"monsterclip", FTECONTENTS_MONSTERCLIP},
		{"clip", FTECONTENTS_PLAYERCLIP|FTECONTENTS_MONSTERCLIP},
		{"body", FTECONTENTS_BODY},
		{"corpse", FTECONTENTS_CORPSE},
		{"detail", FTECONTENTS_DETAIL},
		{"sky", FTECONTENTS_SKY},
		{"Q2SOLID", Q2CONTENTS_SOLID},
		{"Q2WINDOW", Q2CONTENTS_WINDOW},
		{"Q2AUX", Q2CONTENTS_AUX},
		{"Q2LAVA", Q2CONTENTS_LAVA},
		{"Q2SLIME", Q2CONTENTS_SLIME},
		{"Q2WATER", Q2CONTENTS_WATER},
		{"Q2MIST", Q2CONTENTS_MIST},
		{"Q2AREAPORTAL", Q2CONTENTS_AREAPORTAL},
		{"Q2PLAYERCLIP", Q2CONTENTS_PLAYERCLIP},
		{"Q2MONSTERCLIP", Q2CONTENTS_MONSTERCLIP},
		{"Q2CURRENT_0", Q2CONTENTS_CURRENT_0},
		{"Q2CURRENT_90", Q2CONTENTS_CURRENT_90},
		{"Q2CURRENT_180", Q2CONTENTS_CURRENT_180},
		{"Q2CURRENT_270", Q2CONTENTS_CURRENT_270},
		{"Q2CURRENT_UP", Q2CONTENTS_CURRENT_UP},
		{"Q2CURRENT_DOWN", Q2CONTENTS_CURRENT_DOWN},
		{"Q2ORIGIN", Q2CONTENTS_ORIGIN},
		{"Q2MONSTER", Q2CONTENTS_MONSTER},
		{"Q2DEADMONSTER", Q2CONTENTS_DEADMONSTER},
		{"Q2DETAIL", Q2CONTENTS_DETAIL},
		{"Q2TRANSLUCENT", Q2CONTENTS_TRANSLUCENT},
		{"Q2LADDER", Q2CONTENTS_LADDER},
		{"Q3SOLID", Q3CONTENTS_SOLID},
		{"Q3LAVA", Q3CONTENTS_LAVA},
		{"Q3SLIME", Q3CONTENTS_SLIME},
		{"Q3WATER", Q3CONTENTS_WATER},
		{"Q3NOTTEAM1", Q3CONTENTS_NOTTEAM1},
		{"Q3NOTTEAM2", Q3CONTENTS_NOTTEAM2},
		{"Q3NOBOTCLIP", Q3CONTENTS_NOBOTCLIP},
		{"Q3AREAPORTAL", Q3CONTENTS_AREAPORTAL},
		{"Q3PLAYERCLIP", Q3CONTENTS_PLAYERCLIP},
		{"Q3MONSTERCLIP", Q3CONTENTS_MONSTERCLIP},
		{"Q3TELEPORTER", Q3CONTENTS_TELEPORTER},
		{"Q3JUMPPAD", Q3CONTENTS_JUMPPAD},
		{"Q3CLUSTERPORTAL", Q3CONTENTS_CLUSTERPORTAL},
		{"Q3DONOTENTER", Q3CONTENTS_DONOTENTER},
		{"Q3BOTCLIP", Q3CONTENTS_BOTCLIP},
		{"Q3MOVER", Q3CONTENTS_MOVER},
		{"Q3ORIGIN", Q3CONTENTS_ORIGIN},
		{"Q3BODY", Q3CONTENTS_BODY},
		{"Q3CORSE", Q3CONTENTS_CORPSE},
		{"Q3DETAIL", Q3CONTENTS_DETAIL},
		{"Q3STRUCTURAL", Q3CONTENTS_STRUCTURAL},
		{"Q3TRANSLUCENT", Q3CONTENTS_TRANSLUCENT},
		{"Q3TRIGGER", Q3CONTENTS_TRIGGER},
		{"Q3NODROP", Q3CONTENTS_NODROP},
	};
	if (!*contremap)
		for (i = 0; i < 32; i++)
			prv->contentsremap[i] = 1<<i;
	else for (i = 0; i < 32; i++)
	{
		contremap = cmdfuncs->ParseToken(contremap, contname, sizeof(contname), NULL);
		if (!contremap || !*contname)
			prv->contentsremap[i] = 0;
		else
		{
			char *tmp;
			int bit = strtol(contname, &tmp, 10);
			if (!*tmp)
			{
				if (bit >= 0)
					prv->contentsremap[i] = 1<<bit;
			}
			else
			{
				for (j = 0; j < countof(knowncontents); j++)
				{
					if (!Q_strcasecmp(contname, knowncontents[j].name))
					{
						prv->contentsremap[i] = knowncontents[j].contents;
						break;
					}
				}
				if (j == countof(knowncontents))
					Con_Printf(CON_WARNING"%s: Unknown bit name %s\n", hl2_contents_remap->name, contname);
			}
		}
	}
}

static void VBSP_SetParent (mnode_t *node, mnode_t *parent)
{
	node->parent = parent;
	if (node->contents != -1)
		return;
	VBSP_SetParent (node->children[0], node);
	VBSP_SetParent (node->children[1], node);
}

static void VBSP_FindBrushRange (vbspinfo_t	*prv, mnode_t *node, size_t *firstbrush, size_t *lastbrush)
{
	size_t u, b;
	mleaf_t *leaf;
	while (node->contents == -1)
	{	//walk every node to find every leaf...
		VBSP_FindBrushRange(prv, node->children[0], firstbrush, lastbrush);
		node = node->children[1];
	}

	leaf = (mleaf_t*)node;
	for (u = 0; u < leaf->numleafbrushes; u++)
	{
		b = prv->leafbrushes[leaf->firstleafbrush+u]-prv->brushes;
		if (*firstbrush > b)
			*firstbrush = b;
		if (*lastbrush < b+1)
			*lastbrush = b+1;
	}
}

static qboolean VBSP_LoadVertexes (model_t *loadmodel, qbyte *mod_base, vlump_t *l)
{
	dvertex_t	*in;
	mvertex_t	*out;
	size_t			i, count;

	in = (void *)(mod_base + l->fileofs);
	count = l->filelen / sizeof(*in);
	if (l->filelen % sizeof(*in) || count > SANITY_LIMIT(*out))
	{
		Con_Printf (CON_ERROR "VBSP_LoadVertexes: funny lump size in %s\n", loadmodel->name);
		return false;
	}
	out = plugfuncs->GMalloc(&loadmodel->memgroup, count*sizeof(*out));

	loadmodel->vertexes = out;
	loadmodel->numvertexes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out->position[0] = LittleFloat (in->point[0]);
		out->position[1] = LittleFloat (in->point[1]);
		out->position[2] = LittleFloat (in->point[2]);
	}

	return true;
}
static qboolean VBSP_LoadEdges (model_t *loadmodel, qbyte *mod_base, vlump_t *l)
{
	medge_t *out;
	size_t 	i, count;
	//Strata v25 (LUMP_EDGES_VERSION 1) widened the vertex indices from 16-bit to 32-bit.
	size_t elemsz = (l->version >= 1) ? sizeof(dledge_t) : sizeof(dsedge_t);

	count = l->filelen / elemsz;
	if (l->filelen % elemsz || count > SANITY_LIMIT(*out))
	{
		Con_Printf ("VBSP_LoadEdges: funny lump size in %s\n", loadmodel->name);
		return false;
	}
	out = plugfuncs->GMalloc(&loadmodel->memgroup, (count + 1) * sizeof(*out));

	loadmodel->edges = out;
	loadmodel->numedges = count;

	if (l->version >= 1)
	{
		dledge_t *in = (void *)(mod_base + l->fileofs);
		for ( i=0 ; i<count ; i++, in++, out++)
		{
			out->v[0] = LittleLong(in->v[0]);
			out->v[1] = LittleLong(in->v[1]);
		}
	}
	else
	{
		dsedge_t *in = (void *)(mod_base + l->fileofs);
		for ( i=0 ; i<count ; i++, in++, out++)
		{
			out->v[0] = (unsigned short)LittleShort(in->v[0]);
			out->v[1] = (unsigned short)LittleShort(in->v[1]);
		}
	}

	return true;
}
static qboolean VBSP_LoadSurfedges (model_t *loadmodel, qbyte *mod_base, vlump_t *l)
{
	size_t		i, count;
	unsigned int		*in, *out;

	in = (void *)(mod_base + l->fileofs);
	count = l->filelen / sizeof(*in);
	if (l->filelen % sizeof(*in) || count > SANITY_LIMIT(*out))
	{
		Con_Printf (CON_ERROR "VBSP_LoadSurfedges: funny lump size in %s\n",loadmodel->name);
		return false;
	}
	out = plugfuncs->GMalloc(&loadmodel->memgroup, count*sizeof(*out));

	loadmodel->surfedges = out;
	loadmodel->numsurfedges = count;

	for ( i=0 ; i<count ; i++)
		out[i] = LittleLong (in[i]);

	return true;
}
static qboolean VBSP_LoadMarksurfaces (model_t *loadmodel, qbyte *mod_base, vlump_t *l)
{
	size_t		i, j, count;
	msurface_t **out;
	//Strata v25 (LUMP_LEAFFACES_VERSION 1) widened leaffaces from 16-bit to 32-bit.
	qboolean wide = (l->version >= 1);
	size_t elemsz = wide ? sizeof(unsigned int) : sizeof(unsigned short);
	unsigned short *ins = (void *)(mod_base + l->fileofs);
	unsigned int   *inl = (void *)(mod_base + l->fileofs);

	count = l->filelen / elemsz;
	if (l->filelen % elemsz || count > SANITY_LIMIT(*out))
	{
		Con_Printf (CON_ERROR "VBSP_LoadMarksurfaces: funny lump size in %s\n",loadmodel->name);
		return false;
	}
	out = plugfuncs->GMalloc(&loadmodel->memgroup, count*sizeof(*out));

	loadmodel->marksurfaces = out;
	loadmodel->nummarksurfaces = count;

	for ( i=0 ; i<count ; i++)
	{
		j = wide ? LittleLong(inl[i]) : (unsigned short)LittleShort(ins[i]);
		if (j >= loadmodel->numsurfaces)
		{
			Con_Printf (CON_ERROR "VBSP_LoadMarksurfaces: bad surface number\n");
			return false;
		}
		out[i] = loadmodel->surfaces + j;
	}

	return true;
}
//FTESurf: LUMP_OVERLAYS (45) -> synthesised info_overlay entities -------------------------------
//Source bakes its info_overlay decals into lump 45 as GEOMETRY, not entities, and this plugin has
//never read the lump: 230 of the 1310 shipped maps carry one, 9702 overlays in total, and every one
//of them currently renders as nothing.  surf_rise alone has 105.
//
//Rather than append real msurface_t's -- which needs mod->surfaces, nummodelsurfaces, the
//marksurfaces array and every leaf's surface range grown in step, the exact coupling the guard
//comment at VBSP_LoadFaces documents after it produced a multi-GB heap corruption -- decode the lump
//into {"classname" "info_overlay" ...} blocks appended to the entity string, and let the EXISTING
//CSQC decal path place them through adddecal_static.  No new builtin, no new lump loader ordering,
//no new renderer path; the failure mode of a bad record is a missing decal and a census line.
//
//The record layout is chosen by the LUMP's OWN version field (lumps[45].version), NOT by the BSP
//header version -- a distinction that cost a day, because 'Strata v25 uses a bigger stride' is true
//by correlation and wrong as a rule:
//   0 -> 352 bytes, nTexInfo int16, facecount+renderorder packed in a u16   (221 maps, 9397 records)
//   1 -> 356 bytes, the same struct with nTexInfo and the packed facecount widened to int32, so
//        every field after +8 shifts by +4                                  (3 maps, 156 records)
//   2 -> 360 bytes, exactly as version 1 plus a trailing int32 (always -1)  (1 map, 4 records)
//   3 -> Strata, and not an array of fixed records at all:
//            int numOverlays; int numFaceIndices; int aFaces[numFaceIndices];
//        followed by numOverlays 108-byte records whose uv points are 2D and whose basis U is an
//        explicit vec3 instead of being smuggled into the uv z components.  (5 maps, 145 records;
//        3 of those 5 are an 8-byte {0,0} stub, i.e. an EMPTY overlay lump that a stride test
//        would happily misread as garbage.)
//In versions 0/1/2 the basis U vector is packed into the Z components of the four uv points --
//U = (uv[0].z, uv[1].z, uv[2].z) -- and uv[3].z is a spare flag (0 on 8952 records, 1.0 on 445).
typedef struct
{
	int		texinfo;
	int		facecount;
	float	flU[2], flV[2];		//(StartU,EndU),(StartV,EndV) texture range over the quad
	float	uv[4][2];			//quad corners, in (U,V) basis coordinates relative to origin
	vec3_t	basisU;
	vec3_t	origin;
	vec3_t	normal;
} vbspoverlay_t;

static void VBSP_ParseOverlayRec(const qbyte *b, unsigned int lumpver, vbspoverlay_t *out)
{
	int i;
	if (lumpver >= 3)
	{	//Strata: 108-byte record, 2D uv points, explicit basis U, faces held in a shared pool
		out->texinfo   = LittleLong(*(const int*)(b+4));
		out->facecount = LittleLong(*(const int*)(b+16));
		for (i = 0; i < 2; i++)	out->flU[i]    = LittleFloat(((const float*)(b+20))[i]);
		for (i = 0; i < 2; i++)	out->flV[i]    = LittleFloat(((const float*)(b+28))[i]);
		for (i = 0; i < 4; i++)
		{
			out->uv[i][0] = LittleFloat(((const float*)(b+36))[i*2+0]);
			out->uv[i][1] = LittleFloat(((const float*)(b+36))[i*2+1]);
		}
		for (i = 0; i < 3; i++)	out->basisU[i] = LittleFloat(((const float*)(b+68))[i]);
		for (i = 0; i < 3; i++)	out->origin[i] = LittleFloat(((const float*)(b+80))[i]);
		for (i = 0; i < 3; i++)	out->normal[i] = LittleFloat(((const float*)(b+92))[i]);
	}
	else
	{
		const size_t o = lumpver ? 4 : 0;	//versions 1/2 widen the header, shifting everything after it
		if (lumpver)
		{
			out->texinfo   = LittleLong(*(const int*)(b+4));
			out->facecount = LittleLong(*(const int*)(b+8)) & 0x3fff;	//high 2 bits are the render order
		}
		else
		{
			out->texinfo   = (short)LittleShort(*(const short*)(b+4));
			out->facecount = (unsigned short)LittleShort(*(const short*)(b+6)) & 0x3fff;
		}
		for (i = 0; i < 2; i++)	out->flU[i] = LittleFloat(((const float*)(b+264+o))[i]);
		for (i = 0; i < 2; i++)	out->flV[i] = LittleFloat(((const float*)(b+272+o))[i]);
		for (i = 0; i < 4; i++)
		{
			out->uv[i][0] = LittleFloat(((const float*)(b+280+o))[i*3+0]);
			out->uv[i][1] = LittleFloat(((const float*)(b+280+o))[i*3+1]);
			if (i < 3)
				out->basisU[i] = LittleFloat(((const float*)(b+280+o))[i*3+2]);
		}
		for (i = 0; i < 3; i++)	out->origin[i] = LittleFloat(((const float*)(b+328+o))[i]);
		for (i = 0; i < 3; i++)	out->normal[i] = LittleFloat(((const float*)(b+340+o))[i]);
	}
}

//Turn one decoded record into an info_overlay block.  Returns the number of bytes written (0 = the
//record was rejected).  Everything the CSQC arm needs is resolved to world space HERE, because this
//is where the format knowledge lives; what remains in QC is policy (which of these to place).
static size_t VBSP_EmitOverlayEnt(model_t *mod, const vbspoverlay_t *o, char *out, size_t outsize)
{
	vec3_t U, V, N, imageup, centre;
	float du, dv, su, tv, d, W, H, shear, span;
	float lo[2], hi[2], mid[2], srange[2], trange[2];
	int i, sgnV;
	const char *material;

	if (o->texinfo < 0 || o->texinfo >= mod->numtexinfo)
		return 0;
	if (!mod->texinfo[o->texinfo].texture)
		return 0;
	material = mod->texinfo[o->texinfo].texture->name;
	if (!*material)
		return 0;

	VectorCopy(o->normal, N);
	if (VectorNormalize(N) < 0.5)
		return 0;
	//Re-orthogonalise U rather than assert it: what_is_this.bsp ships 5 overlays whose packed U is
	//either not unit (|U| = 0.696) or very nearly PARALLEL to N (|U.N| up to 0.9976), and V = NxU
	//collapses to zero there.  Every other map in the library is within 6e-5 of perfect.
	VectorCopy(o->basisU, U);
	d = DotProduct(U, N);
	VectorMA(U, -d, N, U);
	if (VectorNormalize(U) < 0.5)
		return 0;	//U was parallel to N -- no recoverable tangent frame
	CrossProduct(N, U, V);

	//Bounding rectangle of the quad in basis space, so a sheared or trapezoidal quad degrades to its
	//bounding box instead of being dropped.  `shear` reports how far it actually is from a rectangle
	//so the QC side can decide; 93% of the library is exact to 1e-6 and 97.85% within 10%.
	lo[0] = hi[0] = o->uv[0][0];
	lo[1] = hi[1] = o->uv[0][1];
	for (i = 1; i < 4; i++)
	{
		if (o->uv[i][0] < lo[0]) lo[0] = o->uv[i][0];
		if (o->uv[i][0] > hi[0]) hi[0] = o->uv[i][0];
		if (o->uv[i][1] < lo[1]) lo[1] = o->uv[i][1];
		if (o->uv[i][1] > hi[1]) hi[1] = o->uv[i][1];
	}
	W = hi[0]-lo[0];
	H = hi[1]-lo[1];
	if (W <= 0 || H <= 0)
		return 0;	//degenerate quad
	mid[0] = 0.5*(lo[0]+hi[0]);
	mid[1] = 0.5*(lo[1]+hi[1]);

	span = (W > H) ? W : H;
	shear = fabs(o->uv[1][0]-o->uv[0][0]);
	d = fabs(o->uv[2][0]-o->uv[3][0]);	if (d > shear) shear = d;
	d = fabs(o->uv[3][1]-o->uv[0][1]);	if (d > shear) shear = d;
	d = fabs(o->uv[2][1]-o->uv[1][1]);	if (d > shear) shear = d;
	shear /= span;

	//Source's corner order is c0=(s0,t0) c1=(s0,t1) c2=(s1,t1) c3=(s1,t0), so texture-s runs c0->c3
	//and pairs with flU, texture-t runs c0->c1 and pairs with flV.  su/tv are the basis-space spans
	//those two ranges are laid across.
	du = o->flU[1]-o->flU[0];
	dv = o->flV[1]-o->flV[0];
	su = o->uv[3][0]-o->uv[0][0];
	tv = o->uv[1][1]-o->uv[0][1];
	if (su == 0 || tv == 0)
		return 0;	//a degenerate texture mapping: no direction for one of the axes
	sgnV = (tv*dv > 0) ? 1 : -1;
	VectorScale(V, (float)-sgnV, imageup);	//world-space direction of image-UP

	/*
	  The texture range the ENGINE must map across the footprint, which is not the same thing as
	  flU/flV: the engine's s axis runs one way across the quad and Source's u axis runs the other
	  whenever the overlay is mirrored, and only a range with s1 < s0 can say so.

	  Working it out rather than case-splitting on signs.  With up = N and side = imageup, the frame
	  the engine builds is
	      axis[0] = -N        axis[2] = imageup = -sgnV*V        axis[1] = axis[0] x axis[2] = -sgnV*U
	  and s runs 0..1 along +axis[1], t runs 0..1 along -axis[2].  So in basis coordinates measured
	  from the quad centre, s=0 sits at u = +sgnV*W/2 and s=1 at u = -sgnV*W/2; t=0 sits at
	  v = -sgnV*H/2 and t=1 at v = +sgnV*H/2.  Evaluating Source's own linear texture map at those
	  four points gives the range directly, and it comes out as the identity (0,1),(0,1) for both of
	  the two conventions that cover 99% of the library -- which is the check that this is right.
	*/
	srange[0] = o->flU[0] + (mid[0] + sgnV*0.5*W - o->uv[0][0]) * du/su;
	srange[1] = o->flU[0] + (mid[0] - sgnV*0.5*W - o->uv[0][0]) * du/su;
	trange[0] = o->flV[0] + (mid[1] - sgnV*0.5*H - o->uv[0][1]) * dv/tv;
	trange[1] = o->flV[0] + (mid[1] + sgnV*0.5*H - o->uv[0][1]) * dv/tv;

	//The quad centre, in world space.  Measured centred on vecOrigin for 9397 of 9397 records, but
	//deriving it costs nothing and is correct for the one that is not.
	VectorCopy(o->origin, centre);
	VectorMA(centre, mid[0], U, centre);
	VectorMA(centre, mid[1], V, centre);

	//Q_snprintfz returns TRUE when it truncated; a half-written block would corrupt the entity string
	//(an unterminated brace swallows everything after it), so refuse the record instead.
	if (Q_snprintfz(out, outsize,
		"{\n"
		"\"classname\" \"info_overlay\"\n"
		"\"material\" \"%s\"\n"
		"\"origin\" \"%g %g %g\"\n"
		"\"normal\" \"%g %g %g\"\n"
		"\"imageup\" \"%g %g %g\"\n"
		"\"extent\" \"%g %g\"\n"
		"\"srange\" \"%g %g\"\n"
		"\"trange\" \"%g %g\"\n"
		"\"shear\" \"%g\"\n"
		"\"faces\" \"%i\"\n"
		"}\n",
		material,
		centre[0], centre[1], centre[2],
		N[0], N[1], N[2],
		imageup[0], imageup[1], imageup[2],
		W, H,
		srange[0], srange[1],
		trange[0], trange[1],
		shear,
		o->facecount))
		return 0;
	return strlen(out);
}

/*
================================================================================
FTESurf Patch 288: color_correction -- the whole-screen grade FTE never applied.

Reported as part of the portal-particle message: "they are slightly to small and
wayy to blue.  maybe the hue is actually colourcorrection, maybe they actually
white."

The hunch was right, and it is the reason the particles are not retuned by hand.
surf_tensor2's PCF genuinely is blue and its particle texture is provably neutral
(reflectivity R=G=B exactly), so the particles are correct as authored.  What was
missing is everything AROUND them: the map runs two color_correction entities
over the whole frame, so in Source the entire scene is graded cool and they do
not stand out.  Before this the only mention of the feature anywhere in the
engine was $toolcolorcorrection sitting in mat_vmt.c's ignore list.

WHAT IS IMPLEMENTED, and it is narrow on purpose:

  minfalloff -1 / maxfalloff -1   global, no distance term -- weight is maxweight.
                                  That is both of tensor2's and the common idiom.
  StartDisabled 1 + a logic_auto  OnMapSpawn "<targetname>,Enable" counts as
                                  enabled at load.  That is how tensor2 turns
                                  both of its on and it is what a naive reader
                                  would get wrong: taking StartDisabled at face
                                  value produces a map with a grade that never
                                  applies, and looks exactly like a map with no
                                  grade.

WHAT IS DECLINED, counted rather than ignored:

  a real falloff                  needs a per-pixel distance to the entity, which
                                  nothing has measured a need for.
  color_correction_volume         needs a volume test; 4 maps in the library.
  more than CC_MAX at once        4 is the library maximum; the counter says so
                                  if that is ever wrong.
  fadeInDuration / Disable at
  runtime                         the weights are baked into the shader, so a map
                                  that animates its grade holds the final value.

A DECLINED FEATURE THAT DRAWS NOTHING IS INDISTINGUISHABLE FROM ONE THAT IS NOT
THERE, which is why every one of those has a number in the census line.
================================================================================
*/
#define CC_MAX			4		//the library's maximum simultaneously active
#define CC_ENABLERS		16		//logic_auto Enable targets we will remember
#define CC_KEY			64

static cvar_t *hl2_colourcorrection;	//registered beside hl2_twoframes
static char  cc_shader[MAX_QPATH];		//"" when this map has no grade
static char  cc_script[1024];			//handed to the material loader on demand
static float cc_w[CC_MAX];				//the accepted weights, in pass order
static int   cc_n;						//how many LUTs the shader samples
static int   cc_seen, cc_over, cc_falloff, cc_disabled, cc_volumes;

//The generated shader is handed over by mat_vmt.c's material loader, because a
//plugin has no other way to define a shader by name.  See Shader_LoadVMT.
const char *VBSP_ColourCorrectScript(const char *name)
{
	if (!*cc_shader || strcmp(name, cc_shader))
		return NULL;
	return cc_script;
}

static void VBSP_LoadColourCorrection(model_t *loadmodel, const qbyte *entdata, size_t entlen)
{
	struct
	{
		char file[MAX_QPATH];
		char target[CC_KEY];
		float weight;
		float minfall, maxfall;
		int disabled;
	} cc[CC_MAX*4];		//room to SEE more than we can use, so cc_over is honest
	char enabler[CC_ENABLERS][CC_KEY];
	char passes[768];
	char tmp[MAX_QPATH+64];
	int enablers = 0, found = 0;
	char *text, *data;
	char token[1024];
	char key[CC_KEY];
	unsigned int hash;
	char base[MAX_QPATH];
	int i, j;

	passes[0] = 0;

	cc_shader[0] = 0;
	cc_script[0] = 0;
	cc_n = cc_seen = cc_over = cc_falloff = cc_disabled = cc_volumes = 0;

	//Cleared even when the feature is off, and this is not defensive noise: the
	//cvar is machine-set and persists across a map change, so a map with no
	//grade MUST write "" or it inherits the previous map's.
	if (!hl2_colourcorrection || !hl2_colourcorrection->ival)
	{
		cvarfuncs->ForceSetString("r_colourcorrection", "");
		return;
	}

	//A NUL-terminated copy, because 3 of the 1310 shipped maps do not store the
	//terminator inside the lump and ParseToken would run off the end of it.
	text = plugfuncs->Malloc(entlen+1);
	if (!text)
		return;
	memcpy(text, entdata, entlen);
	text[entlen] = 0;

	data = text;
	for (;;)
	{
		char classname[CC_KEY] = "";
		char target[CC_KEY] = "";
		char file[MAX_QPATH] = "";
		char onmapspawn[CC_ENABLERS][CC_KEY];
		int spawns = 0;
		float weight = 1, minfall = -1, maxfall = -1;
		int disabled = 0;

		data = cmdfuncs->ParseToken(data, token, sizeof(token), NULL);
		if (!data)
			break;
		if (*token != '{')
			continue;

		for (;;)
		{
			data = cmdfuncs->ParseToken(data, key, sizeof(key), NULL);
			if (!data || *key == '}')
				break;
			data = cmdfuncs->ParseToken(data, token, sizeof(token), NULL);
			if (!data)
				break;

			if (!Q_strcasecmp(key, "classname"))		Q_strlcpy(classname, token, sizeof(classname));
			else if (!Q_strcasecmp(key, "targetname"))	Q_strlcpy(target, token, sizeof(target));
			else if (!Q_strcasecmp(key, "filename"))	Q_strlcpy(file, token, sizeof(file));
			else if (!Q_strcasecmp(key, "maxweight"))	weight = atof(token);
			else if (!Q_strcasecmp(key, "minfalloff"))	minfall = atof(token);
			else if (!Q_strcasecmp(key, "maxfalloff"))	maxfall = atof(token);
			else if (!Q_strcasecmp(key, "StartDisabled"))	disabled = atoi(token);
			else if (!Q_strcasecmp(key, "OnMapSpawn") && spawns < CC_ENABLERS)
				Q_strlcpy(onmapspawn[spawns++], token, CC_KEY);
		}

		if (!Q_strcasecmp(classname, "logic_auto"))
		{
			/*
			"<target>,<input>,<param>,<delay>,<times>" -- comma separated, and
			the ONLY one that matters here is Enable.  Read rather than assumed:
			tensor2's logic_auto fires four OnMapSpawn outputs and three of them
			go to the env_tonemap_controller.
			*/
			for (i = 0; i < spawns; i++)
			{
				char *c = strchr(onmapspawn[i], ',');
				if (!c)
					continue;
				*c++ = 0;
				if (Q_strncasecmp(c, "Enable", 6) || (c[6] && c[6] != ','))
					continue;
				if (enablers < CC_ENABLERS && *onmapspawn[i])
					Q_strlcpy(enabler[enablers++], onmapspawn[i], CC_KEY);
			}
			continue;
		}
		if (!Q_strcasecmp(classname, "color_correction_volume"))
		{
			cc_volumes++;
			continue;
		}
		if (Q_strcasecmp(classname, "color_correction") || !*file)
			continue;

		cc_seen++;
		if (found < countof(cc))
		{
			Q_strlcpy(cc[found].file, file, sizeof(cc[found].file));
			Q_strlcpy(cc[found].target, target, sizeof(cc[found].target));
			cc[found].weight = weight;
			cc[found].minfall = minfall;
			cc[found].maxfall = maxfall;
			cc[found].disabled = disabled;
			found++;
		}
	}
	plugfuncs->Free(text);

	//Resolve in a second pass, because a logic_auto may be written either side of
	//the entity it enables and the lump has no ordering guarantee.
	hash = 2166136261u;	//FNV-1a
	for (i = 0; i < found; i++)
	{
		const char *s;

		if (cc[i].minfall != -1 || cc[i].maxfall != -1)
		{
			cc_falloff++;
			continue;
		}
		if (cc[i].disabled)
		{
			for (j = 0; j < enablers; j++)
				if (*cc[i].target && !Q_strcasecmp(enabler[j], cc[i].target))
					break;
			if (j == enablers)
			{
				cc_disabled++;
				continue;
			}
		}
		if (cc[i].weight <= 0)
			continue;	//an explicit zero is off, and not worth a sampler
		if (cc_n >= CC_MAX)
		{
			cc_over++;
			continue;
		}

		//$clamp because a LUT edge must NOT wrap -- a wrapped white would sample
		//black -- and $linear because trilinear interpolation between the 32
		//grid points is the entire reason a 32-cube is enough.
		Q_snprintfz(tmp, sizeof(tmp),
			"\t{\n\t\tmap \"$3d:$clamp:$linear:%s\"\n\t\tnodepthtest\n\t}\n", cc[i].file);
		Q_strlcat(passes, tmp, sizeof(passes));
		for (s = cc[i].file; *s; s++)
			hash = (hash ^ (unsigned char)*s) * 16777619u;
		hash = (hash ^ (unsigned int)(cc[i].weight * 10000)) * 16777619u;
		cc_w[cc_n] = cc[i].weight;
		cc_n++;
	}

	if (!cc_n)
	{
		cvarfuncs->ForceSetString("r_colourcorrection", "");
		return;
	}

	/*
	THE NAME HAS TO BE CONTENT-DERIVED.  R_LoadShader returns an EXISTING shader
	when one already carries the name and discards the body it was handed, so a
	fixed name would give the second graded map of a session the first one's
	grade -- silently, and only on the second map, which is the worst way to find
	a bug like that.  Map base name for legibility, FNV-1a of the files and their
	weights for correctness.
	*/
	//"maps/surf_tensor2.bsp" -> "surf_tensor2".  COM_FileBase is engine-only --
	//a plugin is a separately linked shared object and sees only the function
	//tables, so calling it links but does not load.  Same open-coded strip
	//VBSP_LoadCubemaps uses and records at :9061.
	{
		char *s = strrchr(loadmodel->name, '/');
		Q_strlcpy(base, s ? s+1 : loadmodel->name, sizeof(base));
		s = strrchr(base, '.');
		if (s)
			*s = 0;
	}
	Q_snprintfz(cc_shader, sizeof(cc_shader), "ftesurf/cc/%s_%08x", base, hash);

	/*
	The pass list was built above while the weights were being resolved; the
	program line needs the final count, so the script is assembled only now.

	NO `affine` AND NO `nomipmaps`, and the first version had both.  They are
	copied from gl_rmain.c's own scenepp_gamma and scenepp_antialias scripts,
	which is where anyone would look for the shape of a post-process shader --
	and this parser does not know either of them at the top level:

	    Unknown shader directive parsing ftesurf/cc/...: "affine"
	    Unknown shader directive parsing ftesurf/cc/...: "nomipmaps"
	    <code>: unexpected indentation in ftesurf/cc/...

	That third line is the damage.  An unrecognised top-level token leaves the
	parser positioned inside a PASS, so the next `{` is read as a nested brace
	and gl_shader.c:5761 eats the whole block to resynchronise -- which silently
	throws a pass away.  The shader came out `prog 0 passes 3` with no LUT ever
	loaded and no colour change on screen at all.

	Neither was needed: nothing here is affine-mapped, and a PTI_3D arrives with
	mipcount 1 from a loader that cannot be asked for more ("we can't generate
	3d mips" -- merged.h).
	*/
	{
		char args[256];
		int k;
		args[0] = 0;
		for (k = 0; k < cc_n; k++)
		{
			Q_snprintfz(tmp, sizeof(tmp), "#W%i=%f", k, cc_w[k]);
			Q_strlcat(args, tmp, sizeof(args));
			if (k)
			{
				Q_snprintfz(tmp, sizeof(tmp), "#LUT%i", k);
				Q_strlcat(args, tmp, sizeof(args));
			}
		}
		/*
		NO OPENING BRACE, AND THAT IS THE WHOLE BUG THE FIRST TWO RUNS HAD.

		gl_rmain.c's own post-process scripts are written as complete shaders --
		"{\n program fxaa\n { map $sourcecolour } \n}\n" -- because they go
		through R_RegisterShader.  A material loader's script does NOT: it
		arrives at Shader_LoadMaterialString -> Shader_ReadShader, which begins
		parsing INSIDE the shader body (gl_shader.c:8321-8325).  There, a `{` at
		the top level opens a PASS and a `}` ends the shader.  So the body runs
		from the first directive to a single closing brace, which is exactly the
		shape every mat_vmt.c arm emits and why none of them start with one.

		With the extra brace the whole shader became one pass: `program` is also
		a pass keyword (:4847), so it bound pass->prog and left shader->prog
		NULL; the next `{` was then a brace inside a pass, which :5761 eats to
		resynchronise.  The symptom was exact and completely opaque --

		    <code>: unexpected indentation in ftesurf/cc/surf_tensor2_ef3661e2
		    [shader] ftesurf/cc/surf_tensor2_ef3661e2  sort 5 prog 0 passes 3

		-- three passes, no program, no LUT ever requested, and not one pixel
		different on screen with the grade "on".
		*/
		Q_snprintfz(cc_script, sizeof(cc_script),
			"\tprogram \"vmt/ccorrect%s\"\n"
			"\t{\n\t\tmap $sourcecolour\n\t\tnodepthtest\n\t}\n"
			"%s"
			"}\n",
			args, passes);
	}

	cvarfuncs->ForceSetString("r_colourcorrection", cc_shader);
	Con_DPrintf("[cc] %s generated as %s:\n%s", loadmodel->name, cc_shader, cc_script);
}

static qboolean VBSP_LoadEntities (model_t *loadmodel, qbyte *mod_base, vlump_t *lumps)
{
	vlump_t *l = &lumps[VLUMP_ENTITIES];
	const qbyte *entdata = mod_base + l->fileofs;
	const qbyte *ov;
	size_t entlen, ovlen, recbase, stride, need, used;
	unsigned int ovver;
	int count, i, emitted = 0;
	char *buf;
	qboolean ok;

	/*
	FTESurf Patch 288.  FIRST, and before every one of the four early returns
	below -- the overlay path bails out on maps with no overlay lump, on a lump
	whose layout does not match, and on a lump with zero records, and a map may
	perfectly well have a colour grade and none of those.  Putting this after any
	of them would grade some maps and not others for a reason that has nothing to
	do with colour.
	*/
	VBSP_LoadColourCorrection(loadmodel, entdata, l->filelen);

	if (!hl2_overlays || !hl2_overlays->value)
		return modfuncs->LoadEntities(loadmodel, entdata, l->filelen);

	ovlen = lumps[VLUMP_OVERLAYS].filelen;
	ovver = lumps[VLUMP_OVERLAYS].version;
	ov    = mod_base + lumps[VLUMP_OVERLAYS].fileofs;
	if (!ovlen)
		return modfuncs->LoadEntities(loadmodel, entdata, l->filelen);

	if (ovver >= 3)
	{
		int numfaceidx;
		if (ovlen < 8)
		{
			Con_Printf(CON_WARNING "VBSP: overlay lump (version %u) is %u bytes, too short for its header, in %s\n", ovver, (unsigned)ovlen, loadmodel->name);
			return modfuncs->LoadEntities(loadmodel, entdata, l->filelen);
		}
		count      = LittleLong(((const int*)ov)[0]);
		numfaceidx = LittleLong(((const int*)ov)[1]);
		stride     = 108;
		recbase    = 8 + 4*(size_t)numfaceidx;
		if (count < 0 || numfaceidx < 0 || recbase + (size_t)count*stride != ovlen)
		{
			Con_Printf(CON_WARNING "VBSP: overlay lump version %u does not match the Strata layout (%u overlays, %u face indices, %u bytes) in %s -- overlays skipped\n",
						ovver, (unsigned)count, (unsigned)numfaceidx, (unsigned)ovlen, loadmodel->name);
			return modfuncs->LoadEntities(loadmodel, entdata, l->filelen);
		}
	}
	else
	{
		stride  = (ovver == 0) ? 352 : (ovver == 1) ? 356 : 360;
		recbase = 0;
		if (ovlen % stride)
		{
			Con_Printf(CON_WARNING "VBSP: overlay lump version %u is %u bytes, not a multiple of its %u-byte record, in %s -- overlays skipped\n",
						ovver, (unsigned)ovlen, (unsigned)stride, loadmodel->name);
			return modfuncs->LoadEntities(loadmodel, entdata, l->filelen);
		}
		count = ovlen/stride;
	}
	if (!count)
		return modfuncs->LoadEntities(loadmodel, entdata, l->filelen);

	//1307 of the 1310 shipped maps store a trailing NUL byte INSIDE the entity lump.  Text appended
	//after it is invisible to every parser -- the engine's own COM_ParseOut and the CSQC
	//getentitytoken path (QCC_COM_Parse) both stop dead at the first NUL and report no error -- so
	//the synthesis would silently do nothing.  Trim first.  Only on this path: with the feature off,
	//the original filelen is passed through untouched so the kill switch is exact.
	entlen = l->filelen;
	while (entlen && !entdata[entlen-1])
		entlen--;

	need = entlen + (size_t)count*512 + 2;
	buf = plugfuncs->Malloc(need);
	if (!buf)
		return modfuncs->LoadEntities(loadmodel, entdata, l->filelen);
	memcpy(buf, entdata, entlen);
	used = entlen;
	if (used && buf[used-1] != '\n')
		buf[used++] = '\n';

	for (i = 0; i < count; i++)
	{
		vbspoverlay_t o;
		size_t n;
		VBSP_ParseOverlayRec(ov + recbase + (size_t)i*stride, ovver, &o);
		n = VBSP_EmitOverlayEnt(loadmodel, &o, buf+used, need-used);
		if (n)
		{
			used += n;
			emitted++;
		}
	}

	Con_DPrintf("VBSP: %s: %i of %i baked overlays synthesised as info_overlay (lump 45 version %u)\n",
				loadmodel->name, emitted, count, ovver);

	//LoadEntities (Mod_LoadEntitiesBlob) COPIES the blob into its own Z_Malloc block and
	//NUL-terminates it, so this temporary can go straight back.
	ok = modfuncs->LoadEntities(loadmodel, buf, used);
	plugfuncs->Free(buf);
	return ok;
}

/*
=================
CMod_LoadSubmodels
=================
*/
static qboolean VBSP_LoadSubmodels (model_t *loadmodel, qbyte *mod_base, vlump_t *l)
{
	vbspinfo_t	*prv = (vbspinfo_t*)loadmodel->meshinfo;
	q2dmodel_t	*in;
	cmodel_t	*out;
	int			i, j, count;
	size_t		firstbrush, lastbrush;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
	{
		Con_Printf (CON_ERROR "VBSP_LoadSubmodels: funny lump size\n");
		return false;
	}
	count = l->filelen / sizeof(*in);

	if (count < 1)
	{
		Con_Printf (CON_ERROR "Map with no models\n");
		return false;
	}
	if (count > SANITY_MAX_Q2MAP_MODELS)
	{
		Con_Printf (CON_ERROR "Map has too many models\n");
		return false;
	}

	out = prv->cmodels = plugfuncs->GMalloc(&loadmodel->memgroup, count * sizeof(*prv->cmodels));
	prv->numcmodels = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{	// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (in->mins[j]) - 1;
			out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
			out->origin[j] = LittleFloat (in->origin[j]);
		}
		out->headnode = loadmodel->nodes + LittleLong (in->headnode);
		out->firstsurface = LittleLong (in->firstface);
		out->numsurfaces = LittleLong (in->numfaces);


		firstbrush = ~0u;
		lastbrush = 0u;
		VBSP_FindBrushRange(prv, out->headnode, &firstbrush, &lastbrush);
		if (lastbrush > firstbrush)
		{
			out->firstbrush = firstbrush;
			out->num_brushes = lastbrush-firstbrush;
		}
	}

	AddPointToBounds(prv->cmodels[0].mins, loadmodel->mins, loadmodel->maxs);
	AddPointToBounds(prv->cmodels[0].maxs, loadmodel->mins, loadmodel->maxs);

	return true;
}

/*
=================
CMod_LoadBrushes

=================
*/
static qboolean VBSP_LoadBrushes (model_t *mod, qbyte *mod_base, vlump_t *l)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	q2dbrush_t	*in;
	q2cbrush_t	*out;
	int			i, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
	{
		Con_Printf (CON_ERROR "VBSP_LoadBrushes: funny lump size\n");
		return false;
	}
	count = l->filelen / sizeof(*in);

	if (count > SANITY_LIMIT(*out))
	{
		Con_Printf (CON_ERROR "Map has too many brushes");
		return false;
	}

	prv->brushes = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out) * (count+1));

	out = prv->brushes;

	prv->numbrushes = count;

	/*Patch 318: reset here rather than at map load, because this is the one
	  place the counters are filled and submodel brushes come out of the same
	  lump -- there is no second pass to double-count. */
	vbsp_stat_bevelbrushes = vbsp_stat_bevelplanes = vbsp_stat_bevelfull = 0;

	for (i=0 ; i<count ; i++, out++, in++)
	{
		//FIXME: missing bounds checks
		out->brushside = &prv->brushsides[LittleLong(in->firstside)];
		out->numsides = LittleLong(in->numsides);
		out->contents = VBSP_TranslateContentBits(prv, LittleLong(in->contents));
		VBSP_FinalizeBrush(mod, out);
	}

	return true;
}

/*
=================
CMod_LoadPlanes
=================
*/
static qboolean VBSP_LoadPlanes (model_t *mod, qbyte *mod_base, vlump_t *l)
{
	int			i, j;
	mplane_t	*out;
	dplane_t 	*in;
	int			count;
	int			bits;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
	{
		Con_Printf (CON_ERROR "VBSP_LoadPlanes: funny lump size\n");
		return false;
	}
	count = l->filelen / sizeof(*in);

	if (count < 1)
	{
		Con_Printf (CON_ERROR "Map with no planes\n");
		return false;
	}
	// need to save space for box planes
	if (count > SANITY_LIMIT(*out))
	{
		Con_Printf (CON_ERROR "Map has too many planes (%i)\n", count);
		return false;
	}

	mod->planes = out = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out) * count);
	mod->numplanes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}

		out->dist = LittleFloat (in->dist);
		out->type = LittleLong (in->type);
		out->signbits = bits;
	}

	return true;
}

/*
=================
CMod_LoadLeafBrushes
=================
*/
static qboolean VBSP_LoadLeafBrushes (model_t *mod, qbyte *mod_base, vlump_t *l)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	int			i;
	q2cbrush_t	**out;
	int			count;
	//Strata v25 (LUMP_LEAFBRUSHES_VERSION 1) widened leafbrushes from 16-bit to 32-bit.
	qboolean wide = (l->version >= 1);
	size_t elemsz = wide ? sizeof(unsigned int) : sizeof(unsigned short);
	unsigned short *ins = (void *)(mod_base + l->fileofs);
	unsigned int   *inl = (void *)(mod_base + l->fileofs);

	if (l->filelen % elemsz)
	{
		Con_Printf (CON_ERROR "VBSP_LoadLeafBrushes: funny lump size\n");
		return false;
	}
	count = l->filelen / elemsz;

	if (count < 1)
	{
		Con_Printf (CON_ERROR "Map with no planes\n");
		return false;
	}
	// need to save space for box planes
	if (count > SANITY_LIMIT(**out))
	{
		Con_Printf (CON_ERROR "Map has too many leafbrushes\n");
		return false;
	}

	//prv->numbrushes is because of submodels being weird.
	out = prv->leafbrushes = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out) * (count+prv->numbrushes));
	prv->numleafbrushes = count;

	for ( i=0 ; i<count ; i++, out++)
		*out = prv->brushes + (wide ? LittleLong(inl[i]) : (unsigned short)(short)LittleShort(ins[i]));

	return true;
}

/*
=================
CMod_LoadAreas
=================
*/
static qboolean VBSP_LoadAreas (model_t *mod, qbyte *mod_base, vlump_t *l)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	int			i;
	carea_t	*out;
	q2darea_t 	*in;
	int			count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
	{
		Con_Printf (CON_ERROR "VBSP_LoadAreas: funny lump size\n");
		return false;
	}
	count = l->filelen / sizeof(*in);

	if (count > MAX_Q2MAP_AREAS)
	{
		Con_Printf (CON_ERROR "Map has too many areas\n");
		return false;
	}

	out = prv->areas = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out) * count);
	prv->numareas = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out->numareaportals = LittleLong (in->numareaportals);
		out->firstareaportal = LittleLong (in->firstareaportal);
	}

	return true;
}

/*
=================
CMod_LoadVisibility
=================
*/
static qboolean VBSP_LoadVisibility (model_t *mod, qbyte *mod_base, vlump_t *l)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	int		i;

	prv->numvisibility = l->filelen;
//	if (l->filelen > MAX_Q2MAP_VISIBILITY)
//	{
//		Con_Printf (CON_ERROR "Map has too large visibility lump\n");
//		return false;
//	}

	/*
	FTESurf Patch 209b: a map may ship with NO visibility lump at all, and 49 of the 1310 installed
	maps do -- 46 at BSP v20 plus ahop_rainbow, df_cavernish and df_map at v25 (counted by
	decompressing lump 4 of every map).  Unguarded, that was heap corruption at load time:
	GMalloc(0) returns a NON-NULL pointer to a zero-byte block (zone.c ZG_Malloc always adds
	its header and returns newm+1), so the byte-swap below READ AND WROTE numclusters four
	bytes past the end, and then swapped that garbage count times eight more bytes.

	It also left mod->vis non-NULL, which defeats the `!model->vis` guard in VBSP_MarkLeaves
	and lets VBSP_ClusterPVS index prv->vis->bitofs[cluster] into the same zero-byte block.

	No vis must mean EVERYTHING VISIBLE, never nothing: leaving mod->vis NULL makes
	VBSP_MarkLeaves return NULL, which is the whole-model path, which is correct (slow, but
	these maps are unvised precisely because the compiler was never run).

	The numclusters bound is the second half and is not a formality -- the lump length is the
	only thing that can contradict a corrupt count, and without it a hostile or truncated
	lump walks the same loop off the end.
	*/
	/* NOT sizeof(q2dvis_t): bitofs[8][2] in that struct is a placeholder for a
	   variable-length array, so sizeof is 68 and would reject a perfectly valid lump for a
	   small map.  The real fixed part is the numclusters field alone. */
	if (l->filelen < (int)offsetof(q2dvis_t, bitofs))
	{
		if (l->filelen)
			Con_Printf(CON_WARNING "%s: visibility lump is %i bytes, too short for its own header -- treating the map as unvised\n", mod->name, l->filelen);
		prv->numvisibility = 0;
		prv->vis = NULL;
		mod->vis = NULL;
		mod->numclusters = 0;
		mod->pvsbytes = 0;
		return true;
	}

	prv->vis = plugfuncs->GMalloc(&mod->memgroup, l->filelen);
	memcpy (prv->vis, mod_base + l->fileofs, l->filelen);

	mod->vis = prv->vis;

	prv->vis->numclusters = LittleLong (prv->vis->numclusters);
	if (prv->vis->numclusters == 0)
	{	/* FTESurf Patch 277(b): a header with no rows is an unvised map, not a
		   vised one in which nothing is visible.  Left as a 0-cluster vis,
		   VBSP_LoadLeafs would bound every cluster by 0 and rewrite them all to
		   -1, and the listen server would then send no entity at all.  No
		   library map does this (census: none with numclusters 0). */
		Con_Printf(CON_WARNING "%s: visibility lump has a header but 0 clusters -- treating the map as unvised\n", mod->name);
		prv->numvisibility = 0;
		prv->vis = NULL;
		mod->vis = NULL;
		mod->numclusters = 0;
		mod->pvsbytes = 0;
		return true;
	}
	if (prv->vis->numclusters < 0 ||
		(size_t)prv->vis->numclusters > (l->filelen - offsetof(q2dvis_t, bitofs)) / sizeof(prv->vis->bitofs[0]))
	{
		Con_Printf(CON_ERROR "%s: visibility lump claims %i clusters but is only %i bytes -- treating the map as unvised\n", mod->name, prv->vis->numclusters, l->filelen);
		prv->numvisibility = 0;
		prv->vis = NULL;
		mod->vis = NULL;
		mod->numclusters = 0;
		mod->pvsbytes = 0;
		return true;
	}
	for (i=0 ; i<prv->vis->numclusters ; i++)
	{
		prv->vis->bitofs[i][0] = LittleLong (prv->vis->bitofs[i][0]);
		prv->vis->bitofs[i][1] = LittleLong (prv->vis->bitofs[i][1]);
	}
	mod->numclusters = prv->vis->numclusters;
	mod->pvsbytes = ((mod->numclusters + 31)>>3)&~3;

	return true;
}

/*
static qbyte *CM_LeafnumPVS (model_t *model, int leafnum, qbyte *buffer, unsigned int buffersize)
{
	return CM_ClusterPVS(model, CM_LeafCluster(model, leafnum), buffer, buffersize);
}
*/

#ifdef HAVE_CLIENT

/*extern int	r_dlightframecount;
static void VBSP_MarkLights (dlight_t *light, dlightbitmask_t bit, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;

	if (node->contents != -1)
	{
		mleaf_t *leaf = (mleaf_t *)node;
		msurface_t **mark;

		i = leaf->nummarksurfaces;
		mark = leaf->firstmarksurface;
		while(i--!=0)
		{
			surf = *mark++;
			if (surf->dlightframe != r_dlightframecount)
			{
				surf->dlightbits = 0;
				surf->dlightframe = r_dlightframecount;
			}
			surf->dlightbits |= bit;
		}
		return;
	}

	splitplane = node->plane;
	dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;

	if (dist > light->radius)
	{
		VBSP_MarkLights (light, bit, node->children[0]);
		return;
	}
	if (dist < -light->radius)
	{
		VBSP_MarkLights (light, bit, node->children[1]);
		return;
	}

// mark the polygons
	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		if (surf->dlightframe != r_dlightframecount)
		{
			surf->dlightbits = 0u;
			surf->dlightframe = r_dlightframecount;
		}
		surf->dlightbits |= bit;
	}

	VBSP_MarkLights (light, bit, node->children[0]);
	VBSP_MarkLights (light, bit, node->children[1]);
}

static void VBSP_StainNode (mnode_t *node, float *parms)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;

	if (node->contents != -1)
		return;

	splitplane = node->plane;
	dist = DotProduct ((parms+1), splitplane->normal) - splitplane->dist;

	if (dist > (*parms))
	{
		VBSP_StainNode (node->children[0], parms);
		return;
	}
	if (dist < (-*parms))
	{
		VBSP_StainNode (node->children[1], parms);
		return;
	}

// mark the polygons
	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		if (surf->flags&~(SURF_DONTWARP|SURF_PLANEBACK))
			continue;
		Surf_StainSurf(surf, parms);
	}

	VBSP_StainNode (node->children[0], parms);
	VBSP_StainNode (node->children[1], parms);
}
*/

#endif

typedef struct
{
	float		vecs[2][4];		// [s/t][xyz offset]
	float		lmvecs[2][4];	//well that's awkward
	int			flags;			// miptex flags + overrides
	int			textureindex;
} hltexinfo_t;
static qboolean VBSP_LoadSurfaces (model_t *mod, qbyte *mod_base, vlump_t *l)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	hltexinfo_t	*in;
	q2mapsurface_t	*out;
	int			i, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
	{
		Con_Printf (CON_ERROR "VBSP_LoadSurfaces: funny lump size\n");
		return false;
	}
	count = l->filelen / sizeof(*in);
	if (count < 1)
	{
		Con_Printf (CON_ERROR "Map with no surfaces\n");
		return false;
	}
//	if (count > MAX_Q2MAP_TEXINFO)
//		Host_Error ("Map has too many surfaces");

	mod->numtexinfo = count;
	out = prv->surfaces = plugfuncs->GMalloc(&mod->memgroup, count * sizeof(*prv->surfaces));

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		Q_strncpyz (out->c.name, "FIXME", sizeof(out->c.name));
		Q_strncpyz (out->rname, "FIXME", sizeof(out->rname));
		out->c.flags = LittleLong (in->flags);
		out->c.value = 0;
	}

	return true;
}

typedef struct
{
	vec3_t reflectivity;		//not very useful to us.
	unsigned int stringindex;
	unsigned int width;
	unsigned int height;
	unsigned int width2;	//no idea why there's two of these.
	unsigned int height2;
} hltexture_t;
#define TIHL2_LIGHT			TI_LIGHT
#define TIHL2_SKYBOX		0x2
#define TIHL2_SKYROOM		0x4
#define TIHL2_WARP			TI_WARP

#define	TIHL2_TRANS			TI_TRANS33
#define TIHL2_NOPORTAL		0x20
#define TIHL2_TRIGGER		0x40
#define TIHL2_NODRAW		TI_NODRAW
#define TIHL2_HINT		0x100
#define TIHL2_SKIP		0x200
#define TIHL2_NOLIGHT		0x400
#define TIHL2_BUMPLIGHT		0x800	//FTESurf Patch 281 (VBSP_RepackLighting reads it from the raw lump) and Patch 268 D1 (VBSP_LoadTexInfo stores it, VBSP_ProbeLightmapStride uses it): VRAD wrote four luxel sets per lightstyle for this face (vrad/lightmap.cpp:3402-3411)
//#define TIHL2_NOSHADOWS	0x1000
//#define TIHL2_NODECALS	0x2000
//#define TIHL2_NOCHOP		0x4000
//#define TIHL2_HITBOX		0x8000

static qboolean VBSP_LoadTexInfo (model_t *mod, qbyte *mod_base, vlump_t *lumps, char *mapname)
{	//texinfo->textures->stringoffsets->strings. gah, so many lumps just to find the texture name to use!
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	hltexinfo_t *in;
	mtexinfo_t *out;
	int 	i, j, count;
	char	sname[256];
	int texcount;
	hltexture_t *textures = (void*)(mod_base + lumps[VLUMP_TEXTURES].fileofs);
	unsigned int *stringoffsets = (void*)(mod_base + lumps[VLUMP_STRINGOFFSETS].fileofs);
	char *strings = mod_base + lumps[VLUMP_STRINGDATA].fileofs;
	unsigned int flags;

	in = (void *)(mod_base + lumps[VLUMP_TEXINFO].fileofs);
	if (lumps[VLUMP_TEXINFO].filelen % sizeof(*in))
	{
		Con_Printf ("VBSP_LoadTexInfo: funny lump size in %s\n", mod->name);
		return false;
	}
	count = lumps[VLUMP_TEXINFO].filelen / sizeof(*in);
	out = plugfuncs->GMalloc(&mod->memgroup, count*sizeof(*out));
	prv->texinfo = plugfuncs->GMalloc(&mod->memgroup, count*sizeof(*prv->texinfo));

	mod->textures = plugfuncs->GMalloc(&mod->memgroup, sizeof(texture_t *)*count);
	texcount = 0;

	mod->texinfo = out;
	mod->numtexinfo = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		hltexture_t *texture = ((in->textureindex>=0)?textures + in->textureindex:NULL);
		char *texturename = (texture?strings + stringoffsets[texture->stringindex]:"INVALID");
		flags = LittleLong (in->flags);

		for (j=0 ; j<4 ; j++)
			out->vecs[0][j] = LittleFloat (in->vecs[0][j]);
		for (j=0 ; j<4 ; j++)
			out->vecs[1][j] = LittleFloat (in->vecs[1][j]);
		out->vecscale[0] = 1.0/Length (out->vecs[0]);
		out->vecscale[1] = 1.0/Length (out->vecs[1]);

		for (j=0 ; j<4 ; j++)
			prv->texinfo[i].lmvecs[0][j] = LittleFloat (in->lmvecs[0][j]);
		for (j=0 ; j<4 ; j++)
			prv->texinfo[i].lmvecs[1][j] = LittleFloat (in->lmvecs[1][j]);
		prv->texinfo[i].bumped = !!(flags & TIHL2_BUMPLIGHT);	//FTESurf Patch 268 D1

		if (flags & (TIHL2_SKYBOX|TIHL2_SKYROOM))
			Q_snprintfz(sname, sizeof(sname), "sky/%s", texturename);
		else
			Q_snprintfz(sname, sizeof(sname), "%s", texturename);
		if (flags & (TIHL2_WARP))
			Q_strncatz(sname, "#WARP", sizeof(sname));

		/*
		FTESurf Patch 264: give the COLLISION surface its real name.

		VBSP_LoadSurfaces above fills every prv->surfaces[] entry with the
		literal string "FIXME" and never revisits it, so trace->surface->name is
		useless on every Source map -- and that is the one piece of identity a
		trace carries on its own.  Everything else the debug commands print is
		re-derived from the endpoint by Mod_GetSurfaceNearPoint, which is a
		nearest-distance search that knows nothing about the ray.

		This runs in VBSP_LoadTexInfo rather than VBSP_LoadSurfaces because the
		name does not exist yet up there: the texdata string table is only
		resolved here.  The two loops are the same length over the same lump
		(both take count from VLUMP_TEXINFO), VBSP_LoadSurfaces is called first
		(:7819 before :7821), and com_bih.c binds each brush side to
		prv->surfaces[texinfo], so index i lines up in all three places.

		sname, not texturename, so it matches texinfo->texture->name exactly --
		including the "sky/" prefix and the "#WARP" suffix.  The point is that
		the two can be COMPARED: when the trace's own name and the surface a
		point lookup picked disagree, that disagreement is the finding.

		AND LOWERCASED, which is the whole reason that comparison works.  The
		texture name is passed through Q_strlwr four lines below (:1933) and this
		copy is not, so the first version of this printed
		"side=SURF_MINIGOLF/G nearpoint=surf_minigolf/grid" -- two spellings of
		one surface, disagreeing on every Source material that has a capital in
		it, which is most of them.  A cross-check that reports a difference where
		there is none is worse than no cross-check: it manufactures exactly the
		false lead this command exists to kill.

		c.name is short and will truncate; rname is the longer one.  Neither is
		the answer, both are a cross-check.  c.value is deliberately left at 0 --
		the Q2/Q3/CoD loaders put CONTENTS there and it is not worth the risk of
		a shared path reading a texinfo index as a contents word.
		*/
		Q_strncpyz (prv->surfaces[i].c.name, sname, sizeof(prv->surfaces[i].c.name));
		Q_strncpyz (prv->surfaces[i].rname, sname, sizeof(prv->surfaces[i].rname));
		Q_strlwr (prv->surfaces[i].c.name);
		Q_strlwr (prv->surfaces[i].rname);
		/*
		FTESurf Patch 239: SURF_TRANS DELIBERATELY EMITS NOTHING.  What stood
		here was, verbatim from the Q2 loader it was copied from (gl_q2bsp.c),

			else if (out->flags & TIHL2_TRANS)
				Q_strncatz(sname, "#ALPHA=1", sizeof(sname));

		plus three commented-out siblings.  It has never once executed.  `out`
		comes from GMalloc -> Z_Malloc -> calloc and this loop does not write
		out->flags until eight lines below, so the test was `0 & 0x10` on every
		texinfo of every map ever loaded -- not stale data, deterministically
		zero.  (The Q2 original reads a local it has already assigned.)  The
		`else` also mis-bound: Q2 chains it off an `if (TI_TRANS66)`, and there
		is no TIHL2_TRANS66 here -- Source's 0x20 is NOPORTAL -- so it attached
		to the #WARP append above instead.

		Repairing the condition would still be wrong, which is why this is a
		deletion and not a fix.  VBSP world textures register through
		Shader_DefaultBSPLM, which reads no '#' arguments at all, and R_LoadShader
		truncates the name at the first '#' before the material loaders ever see
		it -- so mat_vmt.c is handed the bare name and a map-side argument can
		never reach it.  #ALPHA=1 is also the shader's own default, and means
		fully opaque.  Measured: correcting the variable would rename 159,651
		texinfos across the library and change zero pixels.

		Nor is there anything left to say.  VBSP sets SURF_TRANS because the
		material is translucent, and mat_vmt.c already reaches that conclusion
		from the VMT itself ($translucent, fractional $alpha, $additive, all of
		which land on st->blendfunc).  Of the SURF_TRANS materials whose VMT is
		packed in the map and resolvable through its include chain, 95.5% say so
		in the VMT; the residue is Refract, translucent by type.  163,360 of
		4,062,783 texinfos (4.0%) in 1203 of 1310 maps carry the flag -- it is
		redundant, not unused.

		If texinfo flags ever do need to reach the program, note that no
		permutation of vmt/lightmapped.glsl means "translucent": blending is a
		blendfunc, not a #define, so that wiring belongs beside st->blendfunc in
		mat_vmt.c and not in a texture name.
		*/

		if (flags & (TIHL2_SKYBOX|TIHL2_SKYROOM))
			out->flags |= TI_SKY;

		if (flags & (TIHL2_WARP))
			out->flags |= TI_WARP;

		if (flags & TIHL2_NOLIGHT)
			out->flags |= TEX_SPECIAL;

		/*
		FTESurf Patch 265: TIHL2_TRIGGER joins TIHL2_NODRAW here.

		VBSP flags a trigger brush's faces SURF_TRIGGER (0x40).  It does NOT
		also set SURF_NODRAW (0x80) -- a TOOLS/TOOLSTRIGGER face carries 0x0440,
		trigger plus nolight -- so the hidetools test further down (which reads
		only TI_NODRAW) never fired on one and every such face was drawn.

		Most of them are invisible anyway because sv_entities.qc drops the model
		for the `trigger_` classes.  What that misses is a brush entity textured
		with a trigger tool material that is NOT named trigger_*: `func_dustmotes`
		is the reported one (903 instances across 77 maps), and on surf_boreas
		its `*29` is a 6144 x 28160 x 4608 slab that fills the screen with an
		opaque blue-grey panel over the snow slope.  Reported as "a brush model
		... it doesn't have transparency or translucency applied to any of the
		faces/textures like it should have".  It should not have transparency;
		it should not be drawn at all.

		Doing it on the FACE FLAG rather than on a classname list is the point.
		The classname list in sv_entities.qc already exists, already contains
		func_dustmotes, and still did not fix this -- because that list makes
		things non-SOLID, and several of its members (func_illusionary,
		func_detail_illusionary, func_static) are decorative geometry that MUST
		stay visible.  Visibility is a property of the material, not the class,
		and Source agrees: `showtriggers_toggle` exists precisely because trigger
		surfaces are never drawn without it.

		Censused over all 1310 installed maps.  SURF_TRIGGER is on 1,914,568 of
		23,735,419 faces (8.1%) in 1194 maps, and NONE of them also carries
		SURF_NODRAW -- so every one of those faces is drawn today and every one
		of them stops being drawn here.  That is a big number and it is stated
		rather than buried: this is the largest single change to what the
		library draws since hidetools itself.

		Why it is nonetheless right, and the evidence is the world model: 724 of
		those faces, across 50 maps, are on model 0.  No entity logic can hide a
		world face -- sv_entities.qc's `trigger_` block only drops brush-entity
		models -- so if Source hid triggers by entity alone, those 724 would be
		visible slabs in Momentum.  They are not.  Source skips them on the
		FACE, which is what this now does too.

		54 distinct materials carry the flag.  53 are tools materials
		(tools/toolstrigger and 30-odd map-local respellings, toolsteleport,
		toolssoundscape, toolstimer, toolsnogrenade, the whole tools/trigger
		subtree).

		THE 54th IS A REAL EXCEPTION AND IS NOT A TOOLS MATERIAL.
		`overlays/no_entry` -- 30 faces on bhop_recharge, rj_doom, surf_nameless
		and what_is_this -- is TF2's respawn-room visualiser: `%compiletrigger 1`
		AND a drawn UnlitGeneric with $translucent, $outline, $softedges and a
		PlayerProximity proxy that fades it in as you approach.  In TF2 it is
		visible on purpose, drawn by func_respawnroomvisualizer, which is a class
		that opts back in to rendering.  We have no such opt-in, so those 30
		faces go dark.  Accepted deliberately: it is a team barrier with no team
		to block on a surf, bhop or rocket-jump map, and 30 faces against
		1.9 million is the right side of the trade.  If it ever needs to come
		back, the discriminator is the entity class, not the flag -- give
		func_respawnroomvisualizer a spawn function that clears SURF_NODRAW on
		its own submodel, the same shape as the func_* spawn functions in
		sv_entities.qc.

		Still gated on hl2_hidetools, so `hl2_hidetools 0` draws them all again
		-- which is exactly Source's own `showtriggers_toggle`, and is how the
		func_dustmotes slab was identified in the first place.
		*/
		if (flags & (TIHL2_NODRAW|TIHL2_TRIGGER))
			out->flags |= TI_NODRAW;

// 		if (flags & TIHL2_HINT)
// 			out->flags |= TI_HINT;

// 		if (flags & TIHL2_SKIP)
// 			out->flags |= TI_SKIP;

		//compact the textures.
		for (j=0; j < texcount; j++)
		{
			if (!Q_strcasecmp(sname, mod->textures[j]->name))
			{
				out->texture = mod->textures[j];
				break;
			}
		}
		if (j == texcount)	//load a new one
		{
			out->texture = plugfuncs->GMalloc(&mod->memgroup, sizeof(texture_t));
			Q_strncpyz(out->texture->name, sname, sizeof(out->texture->name));
			Q_strlwr(out->texture->name);
			if (texture)
			{
				out->texture->vwidth = texture->width;
				out->texture->vheight = texture->height;
			}
			else
			{
				out->texture->vwidth = out->texture->vheight = 128;
			}

			mod->textures[texcount++] = out->texture;
		}
	}

	mod->numtextures = texcount;

	return true;
}

typedef struct
{
	//shared with q2dnode_t
	int			planenum;
	int			children[2];	// negative numbers are -(leafs+1), not nodes
	short		mins[3];		// for frustom culling
	short		maxs[3];
	unsigned short	firstface;
	unsigned short	numfaces;	// counting both sides

	//new for hl2
	unsigned short	area;
	unsigned short pad;
} hl2dnode_t;
typedef struct
{	//Strata v25 (LUMP_NODES_VERSION 1): float bounds + 32-bit face refs
	int			planenum;
	int			children[2];
	float		mins[3];
	float		maxs[3];
	unsigned int	firstface;
	unsigned int	numfaces;
	short		area;
} strata_dnode_t;
static qboolean VBSP_LoadNodes (model_t *mod, qbyte *mod_base, vlump_t *l)
{
	int			child;
	mnode_t		*out;
	int			i, j, count;
	qboolean	wide = (l->version >= 1);
	size_t		elemsz = wide ? sizeof(strata_dnode_t) : sizeof(hl2dnode_t);
	hl2dnode_t	*ins = (void *)(mod_base + l->fileofs);
	strata_dnode_t *inl = (void *)(mod_base + l->fileofs);

	if (l->filelen % elemsz)
	{
		Con_Printf (CON_ERROR "VBSP_LoadNodes: funny lump size\n");
		return false;
	}
	count = l->filelen / elemsz;

	if (count < 1)
	{
		Con_Printf (CON_ERROR "Map has no nodes\n");
		return false;
	}
	if (count > SANITY_LIMIT(*out))
	{
		Con_Printf (CON_ERROR "Map has too many nodes\n");
		return false;
	}

	out = plugfuncs->GMalloc(&mod->memgroup, sizeof(mnode_t)*count);

	mod->nodes = out;
	mod->numnodes = count;

	for (i=0 ; i<count ; i++, out++)
	{
		int planenum, children[2];
		unsigned int firstface, numfaces;

		memset(out, 0, sizeof(*out));

		if (wide)
		{
			for (j=0 ; j<3 ; j++)
			{
				out->minmaxs[j]   = LittleFloat (inl[i].mins[j]);
				out->minmaxs[3+j] = LittleFloat (inl[i].maxs[j]);
			}
			planenum    = LittleLong(inl[i].planenum);
			firstface   = LittleLong(inl[i].firstface);
			numfaces    = LittleLong(inl[i].numfaces);
			children[0] = LittleLong(inl[i].children[0]);
			children[1] = LittleLong(inl[i].children[1]);
		}
		else
		{
			for (j=0 ; j<3 ; j++)
			{
				out->minmaxs[j]   = LittleShort (ins[i].mins[j]);
				out->minmaxs[3+j] = LittleShort (ins[i].maxs[j]);
			}
			planenum    = LittleLong(ins[i].planenum);
			firstface   = (unsigned short)LittleShort(ins[i].firstface);
			numfaces    = (unsigned short)LittleShort(ins[i].numfaces);
			children[0] = LittleLong(ins[i].children[0]);
			children[1] = LittleLong(ins[i].children[1]);
		}

		out->plane = mod->planes + planenum;
		out->firstsurface = firstface;
		out->numsurfaces = numfaces;
		out->contents = -1;	// differentiate from leafs

		for (j=0 ; j<2 ; j++)
		{
			child = children[j];
			out->childnum[j] = child;
			if (child < 0)
				out->children[j] = (mnode_t *)(mod->leafs + -1-child);
			else
				out->children[j] = mod->nodes + child;
		}
	}

	VBSP_SetParent (mod->nodes, NULL);	// sets nodes and leafs

	return true;
}
typedef struct
{
	//copied from q2dleaf_t
	int				contents;			// OR of all brushes (NOTE: hl2 doesn't seem to have these all set properly, at least for detail brushes)

	short			cluster;
	short			area;

	short			mins[3];			// for frustum culling
	short			maxs[3];

	unsigned short	firstleafface;
	unsigned short	numleaffaces;

	unsigned short	firstleafbrush;
	unsigned short	numleafbrushes;

	//new for hl2
	short leafwaterid;
	struct	//present in v19. gone in v20.
	{
		qbyte rgb[3];
		signed char e;
	} light[6];
	short pad;
} hl2dleaf_t;
typedef struct
{	//Strata v25 (LUMP_LEAFS_VERSION 2): 32-bit cluster + float bounds + 32-bit leafface/leafbrush refs
	int				contents;
	int				cluster;
	int				areaflags;		// area:17, flags:15
	float			mins[3];
	float			maxs[3];
	unsigned int	firstleafface;
	unsigned int	numleaffaces;
	unsigned int	firstleafbrush;
	unsigned int	numleafbrushes;
	int				leafwaterid;
} strata_dleaf_t;

static qboolean VBSP_LoadLeafs (model_t *mod, qbyte *mod_base, vlump_t *l, int ver)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	int			i, j;
	mleaf_t		*out;
	qbyte		*inbase = mod_base + l->fileofs;
	int			count;
	qboolean	wide = (l->version >= 2);	//LUMP_LEAFS_VERSION 2 = Strata widened struct
	size_t		insize;
	struct leaflightpoint_s *lightpoint = NULL;
	int			clusterbound;					//FTESurf Patch 277(b): exclusive upper bound on a leaf's cluster, see below
	int			badclusters = 0, badleaf = 0, badvalue = 0;

	if (wide)
		insize = sizeof(strata_dleaf_t);	//56, but a different layout to v19's 56
	else if (ver == 19)
		insize = 56;	//older maps have some lighting info here.
	else
		insize = 32;

	if (l->filelen % insize)
	{
		Con_Printf (CON_ERROR "VBSP_LoadLeafs: funny lump size\n");
		return false;
	}
	count = l->filelen / insize;

	if (count < 1)
	{
		Con_Printf (CON_ERROR "Map with no leafs\n");
		return false;
	}
	// need to save space for box planes
	if (count > SANITY_LIMIT(*out))
	{
		Con_Printf (CON_ERROR "Map has too many leafs\n");
		return false;
	}

	out = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out) * (count+1));
	mod->numclusters = 0;

	mod->leafs = out;
	mod->numleafs = count;

	/*
	FTESurf Patch 277(b) (Patch 273's open item b) -- A LEAF'S CLUSTER IS MAP DATA AND WAS NEVER CHECKED.

	The loop below derives mod->numclusters as max(cluster)+1, so it cannot bound the
	value against itself, and VBSP_ClusterPVS/PHS then index prv->vis->bitofs[cluster]
	with that unchecked number.  On the Strata path the read is a raw signed int32
	(a cluster of 0x7fffffff makes the +1 below signed overflow); on the classic path it
	is a uint16 with only 0xffff mapped to -1, so 0..65534 all pass.

	The bound that IS trustworthy here is the vis lump's own count: VBSP_LoadModel calls
	VBSP_LoadVisibility four lines before VBSP_LoadLeafs, and it has already checked
	numclusters against the lump length (and turned a 0-cluster header into "no vis").  When there is no vis lump prv->vis is NULL
	and the clusters are only ever used as indices into arrays sized from their own
	maximum, so the only hard bound is that a map cannot have more clusters than leafs
	(vbsp assigns one cluster per non-solid leaf: prtfile.cpp:185-190, writebsp.cpp:130);
	that keeps the unvised path working and stops a corrupt value from sizing the
	Patch 273 bucket index in the gigabytes.

	MEASURED, not assumed: a census of all 1310 library maps (lumps LZMA-decoded,
	stride from the lump version) found NO leaf this rewrites -- 0 clusters >= the vis
	count, 0 below -1, and on all 1261 vised maps max(cluster)+1 equals the vis count
	exactly.  So on a shipped map this is inert and the output is byte-identical; it
	exists for the map that is not shipped yet.  A rewritten leaf becomes -1 (no
	cluster): it is never marked from a PVS row, and if the view is in it the whole
	model is drawn (VBSP_MarkLeaves' clusters[0] == -1 branch).  If the rewritten leaf
	was its cluster's only member the derived count drops below the vis count; that is
	harmless (every row and bucket walk is sized from mod->numclusters) and it is why
	the bucketed line on a fixture reads one cluster short.  Counted and reported ONCE
	per map after the loop.
	*/
	clusterbound = prv->vis ? prv->vis->numclusters : count;

	if (!wide && ver == 19)
	{
		prv->leaflight = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out) * count);
		lightpoint = plugfuncs->GMalloc(&mod->memgroup, sizeof(*lightpoint) * count);
	}

	for ( i=0 ; i<count ; i++, out++)
	{
		memset(out, 0, sizeof(*out));

		if (wide)
		{
			strata_dleaf_t *in = (strata_dleaf_t*)(inbase + (size_t)i*insize);
			for (j=0 ; j<3 ; j++)
			{
				out->minmaxs[j]   = LittleFloat (in->mins[j]);
				out->minmaxs[3+j] = LittleFloat (in->maxs[j]);
			}
			out->contents = VBSP_TranslateContentBits(prv, LittleLong(in->contents));
			out->cluster = LittleLong(in->cluster);		//already signed; -1 = no cluster
			out->area = LittleLong(in->areaflags) & 0x1ffff;	//low 17 bits; upper 15 are flags
			out->firstleafbrush = LittleLong(in->firstleafbrush);
			out->numleafbrushes = LittleLong(in->numleafbrushes);
			out->firstmarksurface = mod->marksurfaces + LittleLong(in->firstleafface);
			out->nummarksurfaces = LittleLong(in->numleaffaces);
		}
		else
		{
			hl2dleaf_t *in = (hl2dleaf_t*)(inbase + (size_t)i*insize);
			for (j=0 ; j<3 ; j++)
			{
				out->minmaxs[j]   = LittleShort (in->mins[j]);
				out->minmaxs[3+j] = LittleShort (in->maxs[j]);
			}
			out->contents = VBSP_TranslateContentBits(prv, LittleLong(in->contents));
			out->cluster = (unsigned short)LittleShort (in->cluster);
			if (out->cluster == 0xffff)
				out->cluster = -1;
			out->area = (unsigned short)LittleShort (in->area) & 0x1ff;	//upper part is flags.
			out->firstleafbrush = (unsigned short)LittleShort (in->firstleafbrush);
			out->numleafbrushes = (unsigned short)LittleShort (in->numleafbrushes);
			out->firstmarksurface = mod->marksurfaces + (unsigned short)LittleShort(in->firstleafface);
			out->nummarksurfaces = (unsigned short)LittleShort(in->numleaffaces);

			if (lightpoint)
			{
				for (j = 0; j < 6; j++)
				{
					float e = pow(2, in->light[j].e);
					lightpoint->rgb[j][0] = e * in->light[j].rgb[0];
					lightpoint->rgb[j][1] = e * in->light[j].rgb[1];
					lightpoint->rgb[j][2] = e * in->light[j].rgb[2];
				}
				prv->leaflight[i].count = 1;
				prv->leaflight[i].point = lightpoint++;
			}
		}

		if (out->cluster < -1 || out->cluster >= clusterbound)
		{	//FTESurf Patch 277(b): see the essay above the loop.  Counted here, reported once below.
			if (!badclusters++)
			{
				badleaf = i;
				badvalue = out->cluster;
			}
			out->cluster = -1;
		}

		if (out->cluster >= mod->numclusters)
			mod->numclusters = out->cluster + 1;
	}
	mod->pvsbytes = ((mod->numclusters + 31)>>3)&~3;

	if (badclusters)	//FTESurf Patch 277(b): one line per map, never per leaf
		Con_Printf(CON_WARNING "%s: %i of %i leafs have a cluster outside 0..%i (first: leaf %i = %i) -- treated as -1 (no cluster)\n",
			mod->name, badclusters, count, clusterbound - 1, badleaf, badvalue);

	/*
	FTESurf Patch 273 -- BUCKET THE LEAVES BY CLUSTER.

	IT HAS TO BE HERE AND NOWHERE EARLIER, and that is not a style preference.
	VBSP_LoadVisibility runs FIRST (see the load order in VBSP_LoadMap) and sets
	mod->numclusters from the vis lump's own header; the loop directly above then
	OVERWRITES it with max(leaf->cluster)+1.  Building the index against the
	earlier value gives an array of the wrong length, and the failure mode is a
	rare out-of-bounds write on some other map -- not a crash on load here.  The
	line above this comment is the first point at which numclusters is final.

	`out` has been walked off the end of the array by the loop, so re-derive from
	mod->leafs rather than reusing it.

	GMalloc is calloc-backed, so first[] starts zeroed and the histogram is a
	straight ++.  Both arrays hang off mod->memgroup and die with the model.
	*/
	if (mod->numclusters > 0 && count > 0)
	{
		unsigned int *first;
		unsigned int n = 0, k;
		mleaf_t *lv = mod->leafs;
		int c;

		first = plugfuncs->GMalloc(&mod->memgroup, sizeof(*first) * (mod->numclusters + 1));

		for (i = 0; i < count; i++)			//pass 1: histogram into first[c+1]
		{
			c = lv[i].cluster;
			if (c >= 0 && c < mod->numclusters)
			{	first[c+1]++;	n++;	}
		}
		for (i = 0; i < mod->numclusters; i++)	//pass 2: prefix sum
			first[i+1] += first[i];

		prv->clusterleafs = plugfuncs->GMalloc(&mod->memgroup, sizeof(*prv->clusterleafs) * (n?n:1));

		for (i = 0; i < count; i++)			//pass 3: scatter, first[] as cursor
		{
			c = lv[i].cluster;
			if (c >= 0 && c < mod->numclusters)
				prv->clusterleafs[first[c]++] = (unsigned int)i;
		}
		//pass 3 left first[c] == end-of-cluster-c, i.e. shifted one left.
		//slide it back so first[c]..first[c+1] brackets cluster c again.
		for (k = mod->numclusters; k > 0; k--)
			first[k] = first[k-1];
		first[0] = 0;

		prv->clusterfirstleaf = first;
		prv->numclusteredleafs = n;

		Con_DPrintf("%s: %u of %i leafs bucketed into %i clusters (%u KB)\n",
			mod->name, n, count, mod->numclusters,
			(unsigned)((sizeof(*first)*(mod->numclusters+1) + sizeof(*prv->clusterleafs)*n) >> 10));
	}

	return true;
}

typedef struct
{
	unsigned short portalnum;
	unsigned short otherarea;

	unsigned short firstvert;
	unsigned short numverts;
	unsigned int planenum;
} hl2dareaportal_t;
typedef struct
{	//Strata v25 (LUMP_AREAPORTALS_VERSION 1): all 32-bit
	unsigned int	portalnum;
	unsigned int	otherarea;
	unsigned int	firstvert;
	unsigned int	numverts;
	int				planenum;
} strata_dareaportal_t;
static qboolean VBSP_LoadAreaPortals (model_t *mod, qbyte *mod_base, vlump_t *l, vlump_t *lump_verts)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	int			i;
	q2dareaportal_t		*out;
	int			count, vcount;
	vec3_t		*inverts;
	mesh_t		mesh;
	//Strata v25 widened all fields (12->20 bytes).
	qboolean wide = (l->version >= 1);
	size_t elemsz = wide ? sizeof(strata_dareaportal_t) : sizeof(hl2dareaportal_t);
	hl2dareaportal_t    *ins = (void *)(mod_base + l->fileofs);
	strata_dareaportal_t *inl = (void *)(mod_base + l->fileofs);

	if (l->filelen % elemsz)
	{
		Con_Printf (CON_ERROR "VBSP_LoadAreaPortals: funny lump size\n");
		return false;
	}
	count = l->filelen / elemsz;

	inverts = (void *)(mod_base + lump_verts->fileofs);
	if (lump_verts->filelen % sizeof(*inverts))
	{
		Con_Printf (CON_ERROR "VBSP_LoadAreaPortals: funny lump size\n");
		return false;
	}
	vcount = lump_verts->filelen / sizeof(*inverts);

	if (count > MAX_Q2MAP_AREAS)
	{
		Con_Printf (CON_ERROR "Map has too many areas\n");
		return false;
	}

	out = prv->areaportals = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out) * count);
	prv->portalpoly = plugfuncs->GMalloc(&mod->memgroup, sizeof(*prv->portalpoly)*count);
	prv->portalplane = plugfuncs->GMalloc(&mod->memgroup, sizeof(*prv->portalplane)*count);
	prv->numareaportals = count;

	mesh.xyz_array = plugfuncs->GMalloc(&mod->memgroup, sizeof(*mesh.xyz_array)*vcount);
	for (i=0 ; i<vcount ; i++)
	{
		mesh.xyz_array[i][0] = LittleFloat(inverts[i][0]);
		mesh.xyz_array[i][1] = LittleFloat(inverts[i][1]);
		mesh.xyz_array[i][2] = LittleFloat(inverts[i][2]);
	}
	mesh.indexes = plugfuncs->GMalloc(&mod->memgroup, sizeof(*mesh.indexes)*64*3);
	mesh.st_array = plugfuncs->GMalloc(&mod->memgroup, sizeof(*mesh.st_array)*64);
	for (i=0 ; i<64 ; i++)
	{
		Vector2Set(mesh.st_array[i], 0,0);
		mesh.indexes[i*3+0] = 0;
		mesh.indexes[i*3+1] = i+1;
		mesh.indexes[i*3+2] = i+2;
	}

	for (i=0 ; i<count ; i++, out++)
	{
		unsigned int portalnum, otherarea, firstvert, numverts;
		int planenum;
		if (wide)
		{
			portalnum = LittleLong(inl[i].portalnum);  otherarea = LittleLong(inl[i].otherarea);
			firstvert = LittleLong(inl[i].firstvert);   numverts  = LittleLong(inl[i].numverts);
			planenum  = LittleLong(inl[i].planenum);
		}
		else
		{
			portalnum = (unsigned short)LittleShort(ins[i].portalnum);  otherarea = (unsigned short)LittleShort(ins[i].otherarea);
			firstvert = (unsigned short)LittleShort(ins[i].firstvert);   numverts  = (unsigned short)LittleShort(ins[i].numverts);
			planenum  = LittleLong(ins[i].planenum);
		}
		out->portalnum = portalnum;
		out->otherarea = otherarea;

		prv->portalplane[i] = mod->planes + planenum;

		prv->portalpoly[i].xyz_array = mesh.xyz_array+firstvert;
		prv->portalpoly[i].st_array = mesh.st_array;
		prv->portalpoly[i].numvertexes = numverts;

		if (prv->portalpoly[i].numvertexes>2)
		{
			prv->portalpoly[i].istrifan = true;
			prv->portalpoly[i].indexes = mesh.indexes;
			prv->portalpoly[i].numindexes = (prv->portalpoly[i].numvertexes-2)*3;
		}
	}

	return true;
}

/*
FTESurf Patch 250: THE DISPLACEMENT THE MAPPER TICKED "NO COLLISION" ON.

Source overloads ddispinfo_t's minTess field.  vbsp writes

	pDisp->minTess = pMapDisp->flags;
	pDisp->minTess |= 0x80000000;			//utils/vbsp/disp_vbsp.cpp:326-327

and every reader tests the magic bit before believing the low bits:

	if ( ( minTess & 0x80000000 ) != 0 )
	{	// If the high bit is set, this represents FLAGS (SURF_NOPHYSICS_COLL, etc.)
		int nFlags = minTess;			//public/builddisp.cpp:758-761

so the field is a minimum tessellation only when the magic bit is CLEAR.  Every
displacement in a 90-map sample of the library had it set, so in practice this
is a flags word and nothing else -- but the test is cheap and a map compiled by
an older vbsp would otherwise read a tessellation level as a collision opt-out.

WHY IT MATTERS.  The hull sweep that player movement runs is gated on it:

	bool CDispCollTree::AABBTree_SweepAABB(...)
	{
		if ( CheckFlags( CCoreDispInfo::SURF_NOHULL_COLL ) )
			return false;			//public/dispcoll_common.cpp:892-897

and that gate is SEPARATE from the caller's contents mask.  Reading contents
alone is not enough and is how this was missed: surf_demise's elly/animated_smoke
carries CONTENTS_WINDOW|CONTENTS_TRANSLUCENT, and CONTENTS_WINDOW *is* in
MASK_PLAYERSOLID, so by the brush rules the smoke should stop you.  It does not,
because all 552 of its displacements also carry minTess 0x80000006 -- the mapper
ticked "no physics collision" and "no hull collision" in Hammer and vbsp wrote it
down.  Across the sample, 2184 of 4719 displacements (46%) are NOHULL, spread
over 7 of the 28 maps that use displacements at all.  Every one of them has been
an invisible wall here.

Source's ray path adds a second gate these do not:
	if ( !( m_nContents & MASK_OPAQUE ) ) return false;	//dispcoll_common.cpp:573,660
MASK_OPAQUE is SOLID|MOVEABLE|OPAQUE and excludes WINDOW, so a translucent
displacement is invisible to bullets and line-of-sight as well.  Not implemented
-- see the note at the BIH build for why FTE cannot express hull-vs-ray
separately, and what that costs.
*/
#define DISPSURF_NOPHYSICS_COLL	0x2	//vphysics only -- FTE has no vphysics for world geometry
#define DISPSURF_NOHULL_COLL	0x4	//box/hull sweeps: the player walks through
#define DISPSURF_NORAY_COLL		0x8	//point traces: bullets and line-of-sight
#define DISPSURF_FLAGSVALID		0x80000000u	//"minTess is really a flags word"

typedef struct
{	//the main displacement lump
	vec3_t position;	//not really sure how to use this.
	int firstvert;		//((1<<power)+1) squared verts
	/*
	m_iDispTriStart -- an index into VLUMP_DISP_TRIFLAGS, not flags.  UNREAD, and
	deliberately so: the only bit in that lump we would act on is
	DISPTRI_TAG_REMOVE (bspfile.h:581), which would mean "this triangle is neither
	drawn nor solid", and vbsp cannot emit it.  map.cpp:1223 rebuilds the tag word
	from scratch -- `nTriTags = 0`, then WALKABLE and BUILDABLE and nothing else --
	so REMOVE, SURFACE and the two SURFPROP bits are dropped by the compiler before
	they ever reach a .bsp.  Confirmed against the library rather than inferred:
	across all 1310 maps, 178459 displacements and 20.5M triangles, exactly two tag
	words ever occur, 0x0002 (WALKABLE) and 0x0006 (WALKABLE|BUILDABLE), and 41.8%
	of triangles carry one -- so the lump is populated and the zero is real, not an
	unwritten field.  Both surviving bits are NPC navigation hints, which this
	engine has no use for.  Reading the lump would cost a per-triangle indirection
	on every displacement in the game to act on a bit that is never set.
	*/
	int firsttriflags;
	int power;
	int mintess;	//minTess, OR the DISPSURF_* flags when DISPSURF_FLAGSVALID is set. See above.
	float smoothangle;
	unsigned int contents;
	unsigned short faceidx; //mod->surfaces index
	unsigned int lightmapalphaoffset;	//erk, rgba? that's going to restrict lightmap formats.
	unsigned int lightofs;	//extents? cabbage? oh noes we've gone mad again! or is this some special blend weights in addition to the surface's lighting?
	struct
	{	//erk
		unsigned short peer;
		unsigned char orientation;
		unsigned char span;
		unsigned char peerspan;
	} edgepeers[8];	//two per edge
	struct
	{	//no idea how this works
		unsigned short peer[4];
		unsigned char numpeers;
	} cornerpeers[4];
	unsigned int allowedverts[10];
} hl2ddisplacement_t;
typedef struct
{
	vec3_t norm;
	float dist;
	float alpha;	//vertex alpha.
} hl2displacementvert_t;
static qboolean VBSP_LoadDisplacements (model_t *mod, qbyte *mod_base, vlump_t *lumps)
{
	vbspinfo_t			*prv = (vbspinfo_t*)mod->meshinfo;
	vlump_t 			*l = &lumps[VLUMP_DISP_INFO];
	vlump_t 			*vl = &lumps[VLUMP_DISP_VERTS];
	dispinfo_t			*out;
	int					i, count, x, y;
	hl2displacementvert_t *inv;
	struct dispvert_s	*verts;
	vecV_t				*xyz;
	index_t 			*indexes;
	index_t 			*cindexes;	//FTESurf Patch 256
	size_t				maxverts, maxflipped;
	msurface_t 			*surf;
	float *sverts[4];
	signed int e, idx;
	float fx,fy;
	vec3_t p, base;
	size_t stride;

	int primary;
	float pdist,dist;

	//Strata v25 (LUMP_DISPINFO_VERSION 1): m_iMapFace (faceidx) widened 16->32 bit and the neighbour
	//arrays grew, so the record STRIDE is 232 not 176.  Every field the loader reads (position, firstvert,
	//power, contents, faceidx) sits at the SAME offset in both layouts -- only faceidx's width and the
	//overall stride differ -- so read fields through the classic struct but stride/faceidx per version.
	qbyte  *dispbase = mod_base + l->fileofs;
	qboolean dispwide = (l->version >= 1);
	size_t  dispstride = dispwide ? 232 : sizeof(hl2ddisplacement_t);
	#define DISPAT(idx) ((hl2ddisplacement_t*)(dispbase + (size_t)(idx)*dispstride))
	#define DISPFACE(dp) (dispwide ? (unsigned int)LittleLong(*(unsigned int*)((qbyte*)(dp)+36)) : (dp)->faceidx)

	if (l->filelen % dispstride)
	{
		Con_Printf ("VBSP_LoadDisplacements: funny lump size in %s\n",mod->name);
		return false;
	}
	count = l->filelen / dispstride;
	if (!count)
		return true;	//nothing to worry about.
	out = plugfuncs->GMalloc(&mod->memgroup, count*sizeof(*out));

	for (maxverts = 0, i = 0; i < count; i++)
		maxverts += ((1<<DISPAT(i)->power)) * ((1<<DISPAT(i)->power));
	indexes = plugfuncs->GMalloc(&mod->memgroup, maxverts*sizeof(*indexes)*6);

	/*
	FTESurf Patch 256: THE COLLISION TRIANGLES ARE WOUND BACKWARDS ON A THIRD OF
	ALL DISPLACEMENTS, AND THE PLAYER STANDS FOUR UNITS ABOVE THE TERRAIN.

	com_bih.c:305 builds a collision triangle's face plane as
	cross(p1-p2, p3-p2), and com_bih.c:309 then gives it a hardcoded four units
	of solid BEHIND that plane:  planes[1].dist = -planes[0].dist + 4.  The prism
	is one-sided, so which way that plane points decides whether those four units
	are buried under the terrain (right) or stacked on top of it (wrong).

	Nothing here ever told it.  The winding of the index pattern below is fixed in
	(x,y) grid space, and whether that comes out clockwise or anticlockwise in the
	world depends on the order of the four base verts -- which comes from the face,
	which Source winds around its OUTWARD normal, which is (side ? -plane : +plane).
	So on a dface_t with side set -- SURF_PLANEBACK here, :2495 -- every triangle
	is handed to the BIH inside out.

	MEASURED, reimplementing this whole function over the map library (23 maps with
	displacements, 10288 displacements, 1.1M collision triangles):

		side==0   7183 displacements   741880 tris outward (99.66%)     2536 inverted
		side==1   3105 displacements     1287 tris outward  (0.36%)   354585 INVERTED

	That is not a tendency, it is the discriminator, and side==1 is 30.2% of every
	displacement in the library.  On all of them the player rests 4/|n_z| units up
	-- 4 flat, 5.7 on a 45 degree face -- on an invisible slab, with a perfectly
	standable up normal, so nothing about it reads as a collision bug.  Where such
	a displacement meets a correctly wound neighbour that is a 4-6 unit step with
	no visual cue at all: the seam snag.

	The fix is a separate index array rather than a flip in place, because idx is
	memcpy'd straight into the render mesh at :2761 and the renderer is not part of
	this bug.  Cost is one index_t per index on the flipped third of displacements.
	*/
	for (maxflipped = 0, i = 0; i < count; i++)
	{
		unsigned int fi = DISPFACE(DISPAT(i));
		if (fi < (unsigned int)mod->numsurfaces &&
			(mod->surfaces[fi].flags & SURF_PLANEBACK))
			maxflipped += ((1<<DISPAT(i)->power)) * ((1<<DISPAT(i)->power));
	}
	cindexes = maxflipped ? plugfuncs->GMalloc(&mod->memgroup, maxflipped*sizeof(*cindexes)*6) : NULL;

	for (maxverts = 0, i = 0; i < count; i++)
		maxverts += ((1<<DISPAT(i)->power)+1) * ((1<<DISPAT(i)->power)+1);
	verts = plugfuncs->GMalloc(&mod->memgroup, maxverts*sizeof(*verts));
	xyz = plugfuncs->GMalloc(&mod->memgroup, maxverts*sizeof(*xyz));

	prv->displacements = out;
	prv->numdisplacements = count;
	for (i = 0; i < count; i++, out++)
	{
		hl2ddisplacement_t *in = DISPAT(i);
		unsigned int faceidx = DISPFACE(in);
		surf = mod->surfaces + faceidx;
		if (surf->numedges != 4)
		{
			Con_Printf ("VBSP_LoadDisplacements: displacement surface doesn't have 4 edges in %s\n",mod->name);
			return false;
		}

		//find the 4 verts... messy.
		for (x = 0; x < 4; x++)
		{
			e = mod->surfedges[surf->firstedge+x];
			idx = e < 0;
			if (idx)
				e = -e;
			if (e < 0 || e >= mod->numedges)
				sverts[x] = mod->vertexes[0].position;
			else
				sverts[x] = mod->vertexes[mod->edges[e].v[idx]].position;
		}

		//this is just stupid and pointless.
		//the in->position point tells us which point is the primary one, instead of just rotating the edges.
		primary = 0;
		pdist = FLT_MAX;
		for (x = 0; x < 4; x++)
		{
			VectorSubtract(sverts[x], in->position, p);
			dist = DotProduct(p,p);
			if (dist < pdist)
			{
				pdist = dist;
				primary = x;
			}
		}

		out->surf = surf;
		prv->surfdisp[faceidx] = out;	//the surface needs to be able to get its proper info when building vbos
		ClearBounds(out->aamin, out->aamax);
		out->contents = VBSP_TranslateContentBits(prv,in->contents);
		//FTESurf Patch 250: only a flags word when the magic bit says so; a bare
		//minTess is a tessellation level and means no opt-out at all.
		out->collflags = (in->mintess & DISPSURF_FLAGSVALID) ? (in->mintess & ~DISPSURF_FLAGSVALID) : 0;
		out->width = (1<<in->power);
		out->height = (1<<in->power);

		out->idx = indexes;
		stride = out->width+1;
		for (y=0 ; y<out->height ; y++)
		for (x=0 ; x<out->width ; x++)
		{
			if ((x+y)&1)
			{	//a diamond pattern - flipping alternately
				*indexes++ = (x+0)+(y+1)*stride;
				*indexes++ = (x+1)+(y+1)*stride;
				*indexes++ = (x+1)+(y+0)*stride;
				*indexes++ = (x+0)+(y+1)*stride;
				*indexes++ = (x+1)+(y+0)*stride;
				*indexes++ = (x+0)+(y+0)*stride;
			}
			else
			{
				*indexes++ = (x+0)+(y+0)*stride;
				*indexes++ = (x+0)+(y+1)*stride;
				*indexes++ = (x+1)+(y+1)*stride;
				*indexes++ = (x+0)+(y+0)*stride;
				*indexes++ = (x+1)+(y+1)*stride;
				*indexes++ = (x+1)+(y+0)*stride;
			}
		}
		out->numindexes = indexes - out->idx;

		//FTESurf Patch 256: the outward-wound copy the BIH gets.  Swapping the
		//last two indices of each triangle is the whole flip; the render mesh
		//keeps out->idx untouched.  hl2_dispwinding 0 aliases them again and
		//reproduces every build before this one exactly.
		if (cindexes && hl2_dispwinding->ival && (surf->flags & SURF_PLANEBACK))
		{
			out->cidx = cindexes;
			for (x = 0; x < (int)out->numindexes; x += 3)
			{
				*cindexes++ = out->idx[x+0];
				*cindexes++ = out->idx[x+2];
				*cindexes++ = out->idx[x+1];
			}
		}
		else
			out->cidx = out->idx;

		inv = (void *)(mod_base + vl->fileofs);
		inv += in->firstvert;
		out->verts = verts;
		out->xyz = xyz;
		for (y = 0; y <= out->height; y++)
			for (x = 0; x <= out->width; x++)
			{
				//I have no idea if this is right. probably not. oh well.
				fx = (float)x/out->width;
				fy = (float)y/out->height;
				VectorClear(base);
				VectorMA(base, (1-fx)*(1-fy), sverts[(primary+0)&3], base);
				VectorMA(base, (1-fx)*(  fy), sverts[(primary+1)&3], base);
				VectorMA(base, (  fx)*(  fy), sverts[(primary+2)&3], base);
				VectorMA(base, (  fx)*(1-fy), sverts[(primary+3)&3], base);
				verts->st[0] = DotProduct(base, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3];
				verts->st[1] = DotProduct(base, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3];

				VectorScale(inv->norm, inv->dist*hl2_displacement_scale->value, p);
				VectorNormalize2(p, verts->norm);
				VectorAdd(base, p, (*xyz));
				verts->alpha = inv->alpha;
				AddPointToBounds((*xyz), out->aamin, out->aamax);

				inv++;
				verts++;
				xyz++;
			}

		surf->mesh->numindexes = out->numindexes;
		surf->mesh->numvertexes = verts-out->verts;
	}
	return true;
}

typedef struct
{
	short		planenum;
	qbyte		side;
	qbyte		onnode;	//o.O

	int			firstedge;		// we must support > 64k edges
	unsigned short		numedges;
	unsigned short		texinfo;

	unsigned short		dispinfo;
	short		fogvolume;

// lighting info
	qbyte		styles[4];
	int			lightofs;		// start of [numstyles*surfsize] samples
	float		surfacearea;
	int			extents_min[2];
	int			extents_size[2];

	int			origface;
	unsigned short numprims;
	unsigned short firstprim;
	unsigned int smoothinggroup;
} hl2dface_t;
typedef struct
{	//Strata v25 (LUMP_FACES_VERSION 2): 32-bit planenum/numedges/texinfo/dispinfo/fogvolume
	unsigned int	planenum;
	qbyte			side;
	qbyte			onnode;
	int				firstedge;
	int				numedges;
	int				texinfo;
	int				dispinfo;
	int				fogvolume;
	qbyte			styles[4];
	int				lightofs;
	float			surfacearea;
	int				extents_min[2];
	int				extents_size[2];
	int				origface;
	unsigned int	primbits;		// m_EnableShadows:1 | m_NumPrims:31
	unsigned int	firstprim;
	unsigned int	smoothinggroup;
} strata_dface_t;


typedef struct
{
	int			padding[8];
	short		planenum;
	qbyte		side;
	qbyte		onnode;	//o.O

	int			firstedge;		// we must support > 64k edges
	unsigned short		numedges;
	unsigned short		texinfo;

	unsigned short		dispinfo;
	short		fogvolume;

// lighting info
	qbyte		styles[8];
	qbyte		day[8];
	qbyte		night[8];
	int			lightofs;		// start of [numstyles*surfsize] samples
	float		surfacearea;
	int			extents_min[2];
	int			extents_size[2];

	int			origface;
	unsigned int smoothinggroup;
} vampiredface_t;

static qboolean VBSP_LoadFaces_Vampire (model_t *mod, qbyte *mod_base, vlump_t *lumps, int version)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	vlump_t *l = &lumps[VLUMP_FACES_LDR];
	vampiredface_t	*in;
	msurface_t 	*out;
	int			i, count, surfnum;
	int			planenum;
	int			ti, st;
	int			lumpsize = sizeof(*in);

	mesh_t		*meshes;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % lumpsize)
	{
		Con_Printf ("VBSP_LoadFaces_Vampire: funny lump size in %s\n",mod->name);
		return false;
	}
	count = l->filelen / lumpsize;
	out = plugfuncs->GMalloc(&mod->memgroup, count*sizeof(*out));
	prv->surfdisp = plugfuncs->GMalloc(&mod->memgroup, count * sizeof(*prv->surfdisp));

	meshes = plugfuncs->GMalloc(&mod->memgroup, count*sizeof(*meshes));

	mod->surfaces = out;
	mod->numsurfaces = count;

	mod->lightmaps.surfstyles = 1;

	for ( surfnum=0 ; surfnum<count ; surfnum++, in = (void*)((qbyte*)in+lumpsize), out++)
	{
		out->firstedge = LittleLong(in->firstedge);
		out->numedges = (unsigned short)LittleShort(in->numedges);
		out->flags = 0;
		out->mesh = meshes+surfnum;
		out->mesh->numvertexes = out->numedges;
		out->mesh->numindexes = (out->mesh->numvertexes-2)*3;

		planenum = (unsigned short)LittleShort(in->planenum);
		if (in->side)
			out->flags |= SURF_PLANEBACK;
		if (!in->onnode)
			out->flags |= SURF_OFFNODE;

		out->plane = mod->planes + planenum;

		ti = (unsigned short)LittleShort (in->texinfo);
		if (ti < 0 || ti >= mod->numtexinfo)
		{
			Con_Printf (CON_ERROR "VBSP_LoadFaces: bad texinfo number\n");
			return false;
		}
		out->texinfo = mod->texinfo + ti;

		if (out->texinfo->flags & TI_SKY)
		{
			out->flags |= SURF_DRAWSKY|SURF_DRAWTILED;
		}
		if (out->texinfo->flags & TI_WARP)
		{
			out->flags |= SURF_DRAWTURB|SURF_DRAWTILED;
		}
		/*
		FTESurf: hide the tool brushes.

		VBSP writes Source's SURF_NODRAW (0x80) into the texinfo lump for every
		clip, playerclip, nodraw, skip, hint and areaportal face, and we have been
		reading it into texinfo->flags since the loader was written (TIHL2_NODRAW
		above) -- and then never looking at it again.  The two lines above are the
		pattern: a texinfo flag only reaches the renderer once it is copied onto
		the SURFACE.  Without this, surf_null draws its ramp clip brushes as solid
		lightmapped walls.

		Mod_Batches_Generate already knows what to do with it (gl_model.c, it swaps
		in a "surfaceparm nodraw" shader), so this is the whole fix.

		COLLISION IS UNAFFECTED.  Brush contents come from the brush/brushside
		lumps, not from faces -- a playerclip still clips, it just stops being
		painted.
		*/
		if ((out->texinfo->flags & TI_NODRAW) && hl2_hidetools->ival)
			out->flags |= SURF_NODRAW;

		out->lmshift = 0;
		out->texturemins[0] = in->extents_min[0];
		out->texturemins[1] = in->extents_min[1];
		out->extents[0] = in->extents_size[0];
		out->extents[1] = in->extents_size[1];

	// lighting info

		for (i=0 ; i<Q1Q2BSP_STYLESPERSURF ; i++)
		{
			st = in->styles[i];
			if (st == 255)
				st = INVALID_LIGHTSTYLE;
			else if (mod->lightmaps.maxstyle < st)
				mod->lightmaps.maxstyle = st;
			out->styles[i] = st;
		}
		for (; i<MAXCPULIGHTMAPS ; i++)
			out->styles[i] = INVALID_LIGHTSTYLE;
		for (i = 0; i<MAXRLIGHTMAPS ; i++)
			out->vlstyles[i] = INVALID_VLIGHTSTYLE;
		i = LittleLong(in->lightofs);
		if (i == -1 || !mod->lightdata)
			out->samples = NULL;
		else
			out->samples = mod->lightdata + i;

	// set the drawing flags

		if (out->texinfo->flags & TI_WARP)
			out->flags |= SURF_DRAWTURB;
	}

	return true;
}

#ifdef HAVE_CLIENT
/*
FTESurf Patch 281 -- one luxel, from Source's linear ColorRGBExp32 to the sRGB
E5BGR9 the renderer wants.  This is VBSP_LoadLighting's loop body, lifted out
unchanged so VBSP_RepackLighting can run the identical conversion a face at a
time.
*/
static unsigned int VBSP_ConvertLuxel(const qbyte *src)
{
	int e = 0;
	float m;
	float scale;
	unsigned int hdr;
	vec3_t rgb;

	//decode input
	m = pow(2, (signed char)src[3])/255.0;
	rgb[0] = m * src[0];
	rgb[1] = m * src[1];
	rgb[2] = m * src[2];

	//rescale its gamma ramp to something we can actually use properly
	rgb[0] = M_LinearToSRGB(rgb[0], 1.0);
	rgb[1] = M_LinearToSRGB(rgb[1], 1.0);
	rgb[2] = M_LinearToSRGB(rgb[2], 1.0);

	//encode output
	m = max(max(rgb[0], rgb[1]), rgb[2]);
	if (m < 0)
		m = 0;

	if (m >= 0.5)
	{	//positive exponent
		while (m >= (1<<(e)) && e < 30-15)	//don't do nans.
			e++;
	}
	else
	{	//negative exponent...
		while (m < 1/(1<<-e) && e > -14)	//don't do denormals.
			e--;
	}

	scale = pow(2, e-9);
	hdr = ((e+15)<<27);
	hdr |= bound(0, (int)(rgb[0]/scale + 0.5), 0x1ff)<<0;
	hdr |= bound(0, (int)(rgb[1]/scale + 0.5), 0x1ff)<<9;
	hdr |= bound(0, (int)(rgb[2]/scale + 0.5), 0x1ff)<<18;
	return hdr;
}

/*
FTESurf Patch 281 -- faces whose lightmap does not start on a 4-byte boundary.

THE CONTRACT.  Surf_BuildLightMap reads a face's E5BGR9 samples as
((unsigned int*)src)[i] (engine/client/r_surf.c:1556, :1854) and r_texdiag's
LUX census does the same (:4791), so surf->samples must be 4-byte aligned.
VBSP_LoadFaces hands out mod->lightdata + lightofs with the file's byte
offset unchanged, and VBSP_LoadLighting converted the lump as 4-byte words
counted from byte 0.  Stock VRAD keeps every lightofs on that grid: a face
block is 4 bytes of average colour per style followed by
styles * sets * luxels * 4 bytes of samples (mp/src/utils/vrad/lightmap.cpp:3396,
radial.cpp:739), all multiples of 4.

NINE MAPS DO NOT.  bhop_24, surf_mjk, surf_at_the_limit, surf_windrunner,
surf_swagtoast, surf_borderlands, surf_legends, surf_sup and surf_colours
(1310 censused, 1199 of the 1208 parseable at exactly 0.0% misaligned) store
one undocumented extra byte per luxel after each face's sample sets, so
lightofs drifts off the grid and 69-75% of their lit faces land on residue
1, 2 or 3.  Their lighting lump carries lumpVersion 1 like everyone else's,
so there is no flag to read.  Measured on surf_borderlands: 2264 of 3154 lit
faces misaligned, {0:890, 1:736, 2:809, 3:719}, uniformly across materials
(DEV/VALUESAND40 76% of its area, the six SKYPPY3 metals 73-80%).

WHAT A MISALIGNED FACE LOOKED LIKE.  Two misreads compound.  The whole-lump
conversion, striding 4 from byte 0, takes a colour byte of one luxel as the
exponent of the next, so inside those faces it writes words that are either
saturated (E field 30) or dark; the reader then takes them at a further 1-3
byte shift, so the exponent field it sees is 31 (51% of luxels at residue 1,
12% at residue 2) or 0 (46% / 86% / 93% at residue 1/2/3).  E=31 makes
264 * 2^7 * 2^7 * 511 = 2.2e9 per channel, which passes 2^31 in the unsigned
blocklights accumulator, comes back into int r = *bl++ negative
(r_surf.c:927) and Surf_PackE5BRG9 clamps it to zero; E=0 is 2^-24 and rounds
to zero.  Replaying the exact path over the converted lump -- note that
1/(1<<-e) in both exponent loops (mod_vbsp.c VBSP_ConvertLuxel, r_surf.c:803)
is INTEGER division, so every value below 0.5 takes e = -1 -- gives 91% / 94%
/ 96% of luxels at residue 1/2/3 EXACTLY zero and 8% / 6% / 2% blown to
saturated primaries (the cyan and magenta streaks in the user's screenshot),
against 81% normal and 0% blown for aligned faces; 1718 of the 3154 lit faces,
61% of the lit area, are more than 90% exact zero.  So the map came out as
smooth, exact-zero black on three fifths of its area with a lit face here and
there -- the 28% of faces that happened to be aligned -- which is what was
reported as "the textures are all messed up".

THE FIX IS NOT "ROUND LIGHTOFS DOWN": that shifts every luxel of the face by
up to three quarters of a sample and reads the previous face's tail as the
first one.  Instead, when any lit face is off the grid, rebuild the lightmap
one face at a time from the RAW lump: each face's samples are converted from
where the file really puts them into a fresh buffer at a 4-byte position, and
surf->samples is repointed.  Only set 0 of each style is copied -- the flat,
non-directional set, which is what the reader consumes (radial.cpp:739 packs
it first; r_surf.c:1563 advances one set per style) -- so a bumped face's
block shrinks 4x and the per-style stride the reader assumes becomes true
for it.  Where style k's set 0 sits is the stock formula
lightofs + k*sets*luxels*4: measured on surf_sup's 2629 two-style faces, all
71,229 exponent bytes at that offset are in VRAD's range, versus 43% out of
range if the extra bytes were interleaved per style.  Bounds are checked per
style against the raw lump; a style off the end is written black and counted.
No map in the nine overruns.

Nothing else reads lightdata by offset on this path: sunvisdata is set only by
the Q1 BSPX loader (gl_model.c:2544) and deluxdata is never set for VBSP, so
the byte positions can change freely.  The converted whole-lump buffer stays
in the model's memgroup until the model is freed (there is no group free); on
the nine maps that is 1-28 MB held for the map's lifetime, noted rather than
hidden.

DIAGNOSTIC, not gated: one Con_Printf naming the map, the count and the new
size, on the nine maps only.  Every stock map returns silently at the first
census.  hl2_lightmap_repack 0 is the A/B arm: it prints the same count and
leaves the old read in place, so "bhop_24 is black" can be reproduced on
demand.
*/
/* Luxels of one face, clamped at 0.  msurface_t.extents is a short filled from
   the file's int extents; a lit face whose extents came through as -2 or less
   would otherwise count negative, walk the pass-2 cursor backwards over the
   previous face and hand memset a wrapped size.  VBSP_ProbeLightmapStride
   (Patch 268 D1) guards the same case the same way.  A 0-luxel face keeps a
   non-NULL samples pointer and Mod_LightmapAllocSurf gives it no lightmap. */
static int VBSP_RepackLuxels(const msurface_t *surf)
{
	int smax = (surf->extents[0]>>surf->lmshift)+1;
	int tmax = (surf->extents[1]>>surf->lmshift)+1;
	return (smax > 0 && tmax > 0) ? smax*tmax : 0;
}

static void VBSP_RepackLighting(model_t *mod, qbyte *mod_base, vlump_t *lumps)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	const hltexinfo_t *tin = (const hltexinfo_t*)(mod_base + lumps[VLUMP_TEXINFO].fileofs);
	msurface_t	*surf;
	unsigned int *newdata, *dst;
	size_t		surfnum, lit = 0, misaligned = 0, truncated = 0, words = 0, cursor = 0;
	int			k, i, nst, sets, luxels;

	if (!mod->lightdata || !prv->lightraw)
		return;

	//pass 1: the census, and the size of the dense one-set-per-style layout.
	for (surfnum = 0; surfnum < mod->numsurfaces; surfnum++)
	{
		surf = mod->surfaces + surfnum;
		if (!surf->samples)
			continue;
		lit++;
		if ((surf->samples - mod->lightdata) & 3)
			misaligned++;
		for (nst = 0; nst < MAXCPULIGHTMAPS && surf->styles[nst] != INVALID_LIGHTSTYLE; nst++)
			;
		words += (size_t)nst * VBSP_RepackLuxels(surf);
	}
	if (!misaligned)
		return;	//every stock VRAD map: nothing to do and nothing to say.

	if (hl2_lightmap_repack && !hl2_lightmap_repack->ival)
	{
		Con_Printf("VBSP: %s: %u of %u lit faces have a lightofs off the 4-byte grid; hl2_lightmap_repack 0, left as-is (they draw black)\n",
			mod->name, (unsigned)misaligned, (unsigned)lit);
		return;
	}

	//pass 2: convert each face from where the file really keeps it.
	newdata = plugfuncs->GMalloc(&mod->memgroup, words * sizeof(*newdata));
	for (surfnum = 0; surfnum < mod->numsurfaces; surfnum++)
	{
		size_t rawofs;
		surf = mod->surfaces + surfnum;
		if (!surf->samples)
			continue;
		rawofs = surf->samples - mod->lightdata;	//== the file's lightofs: VBSP_LoadFaces added it unchanged
		luxels = VBSP_RepackLuxels(surf);
		for (nst = 0; nst < MAXCPULIGHTMAPS && surf->styles[nst] != INVALID_LIGHTSTYLE; nst++)
			;
		sets = (LittleLong(tin[surf->texinfo - mod->texinfo].flags) & TIHL2_BUMPLIGHT) ? 4 : 1;
		dst = newdata + cursor;
		for (k = 0; k < nst; k++, dst += luxels)
		{
			const qbyte *src = prv->lightraw + rawofs + (size_t)k * sets * luxels * 4;
			if (rawofs + ((size_t)k * sets + 1) * luxels * 4 > prv->lightrawsize)
			{	//off the end of the lump: black, which is what an unreadable style was before.
				memset(dst, 0, luxels * sizeof(*dst));
				truncated++;
				continue;
			}
			for (i = 0; i < luxels; i++)
				dst[i] = VBSP_ConvertLuxel(src + 4*i);
		}
		surf->samples = (qbyte*)(newdata + cursor);
		cursor += (size_t)nst * luxels;
	}

	Con_Printf("VBSP: %s: %u of %u lit faces had a lightofs off the 4-byte grid -- lightmap repacked per face, %u -> %u bytes%s\n",
		mod->name, (unsigned)misaligned, (unsigned)lit, (unsigned)mod->lightdatasize, (unsigned)(words * sizeof(*newdata)),
		truncated ? " (some styles ran off the lump and were written black)" : "");
	mod->lightdata = (qbyte*)newdata;
	mod->lightdatasize = words * sizeof(*newdata);
	prv->lightrepacked = true;	//tells VBSP_ProbeLightmapStride (Patch 268 D1) the layout is now one set per style
}
#endif

static qboolean VBSP_LoadFaces (model_t *mod, qbyte *mod_base, vlump_t *lumps, int version)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	vlump_t *l = &lumps[VLUMP_FACES_LDR];
	vlump_t *l2 = &lumps[VLUMP_FACES_HDR];
	qbyte		*inbase;
	msurface_t 	*out;
	int			i, count, surfnum;
	int			planenum;
	int			ti, st;
	int			lumpsize = sizeof(hl2dface_t);
	int			prefix = 0;
	int			nodrawn = 0;	//FTESurf: tool faces hidden, reported below
	qboolean	wide;

	mesh_t		*meshes;

	if (version == 17)
	{
		return VBSP_LoadFaces_Vampire(mod, mod_base, lumps, version);
	}

	if (l2->filelen && !(hl2_favour_ldr->ival && lumps[VLUMP_LIGHTING_LDR].filelen))
		l = l2;

	wide = (l->version >= 2);	//Strata v25 LUMP_FACES_VERSION 2 = 32-bit fields
	if (wide)
		lumpsize = sizeof(strata_dface_t);
	else if (version == 18)
	{	//this version seems to have rgbx*4 prefixed
		prefix = 4*4;
		lumpsize += 4*4;
	}
	inbase = (qbyte *)(mod_base + l->fileofs + prefix);
	if (l->filelen % lumpsize)
	{
		Con_Printf ("VBSP_LoadFaces: funny lump size in %s\n",mod->name);
		return false;
	}
	count = l->filelen / lumpsize;
	out = plugfuncs->GMalloc(&mod->memgroup, count*sizeof(*out));
	prv->surfdisp = plugfuncs->GMalloc(&mod->memgroup, count * sizeof(*prv->surfdisp));

	meshes = plugfuncs->GMalloc(&mod->memgroup, count*sizeof(*meshes));

	mod->surfaces = out;
	mod->numsurfaces = count;

	mod->lightmaps.surfstyles = 1;

	for ( surfnum=0 ; surfnum<count ; surfnum++, out++)
	{
		int firstedge, numedges, lightofs, extmin[2], extsize[2];
		qbyte side, onnode, *styles;

		if (wide)
		{
			strata_dface_t *in = (strata_dface_t*)(inbase + (size_t)surfnum*lumpsize);
			firstedge  = LittleLong(in->firstedge);
			numedges   = LittleLong(in->numedges);
			planenum   = LittleLong(in->planenum);
			side       = in->side;
			onnode     = in->onnode;
			ti         = LittleLong(in->texinfo);
			extmin[0]  = LittleLong(in->extents_min[0]);  extmin[1]  = LittleLong(in->extents_min[1]);
			extsize[0] = LittleLong(in->extents_size[0]); extsize[1] = LittleLong(in->extents_size[1]);
			styles     = in->styles;
			lightofs   = LittleLong(in->lightofs);
		}
		else
		{
			hl2dface_t *in = (hl2dface_t*)(inbase + (size_t)surfnum*lumpsize);
			firstedge  = LittleLong(in->firstedge);
			numedges   = (unsigned short)LittleShort(in->numedges);
			planenum   = (unsigned short)LittleShort(in->planenum);
			side       = in->side;
			onnode     = in->onnode;
			ti         = (unsigned short)LittleShort(in->texinfo);
			extmin[0]  = in->extents_min[0];  extmin[1]  = in->extents_min[1];
			extsize[0] = in->extents_size[0]; extsize[1] = in->extents_size[1];
			styles     = in->styles;
			lightofs   = LittleLong(in->lightofs);
		}

		out->firstedge = firstedge;
		out->numedges = numedges;
		out->flags = 0;
		out->mesh = meshes+surfnum;
		out->mesh->numvertexes = out->numedges;
		out->mesh->numindexes = (out->mesh->numvertexes-2)*3;

		if (side)
			out->flags |= SURF_PLANEBACK;
		if (!onnode)
			out->flags |= SURF_OFFNODE;

		out->plane = mod->planes + planenum;

		if (ti < 0 || ti >= mod->numtexinfo)
		{
			Con_Printf (CON_ERROR "VBSP_LoadFaces: bad texinfo number\n");
			return false;
		}
		out->texinfo = mod->texinfo + ti;

		if (out->texinfo->flags & TI_SKY)
		{
			out->flags |= SURF_DRAWSKY|SURF_DRAWTILED;
		}
		if (out->texinfo->flags & TI_WARP)
		{
			out->flags |= SURF_DRAWTURB|SURF_DRAWTILED;
		}
		if ((out->texinfo->flags & TI_NODRAW) && hl2_hidetools->ival)
		{
			out->flags |= SURF_NODRAW;	//FTESurf: tool brushes -- see the note in the loader above
			nodrawn++;
		}

		out->lmshift = 0;
		out->texturemins[0] = extmin[0];
		out->texturemins[1] = extmin[1];
		out->extents[0] = extsize[0];
		out->extents[1] = extsize[1];

	// lighting info

		for (i=0 ; i<Q1Q2BSP_STYLESPERSURF ; i++)
		{
			st = styles[i];
			if (st == 255)
				st = INVALID_LIGHTSTYLE;
			else if (mod->lightmaps.maxstyle < st)
				mod->lightmaps.maxstyle = st;
			out->styles[i] = st;
		}
		for (; i<MAXCPULIGHTMAPS ; i++)
			out->styles[i] = INVALID_LIGHTSTYLE;
		for (i = 0; i<MAXRLIGHTMAPS ; i++)
			out->vlstyles[i] = INVALID_VLIGHTSTYLE;
		i = lightofs;
		if (i == -1 || !mod->lightdata)
			out->samples = NULL;
		else
			out->samples = mod->lightdata + i;

	// set the drawing flags

		if (out->texinfo->flags & TI_WARP)
			out->flags |= SURF_DRAWTURB;
	}

#ifdef HAVE_CLIENT
	VBSP_RepackLighting(mod, mod_base, lumps);	//FTESurf Patch 281: after every surf->samples is set, before anything reads one
#endif

	/*
	FTESurf: how many faces the compiler marked invisible.

	Worth a line, because until now the answer was "all of them are drawn
	anyway" and there was no way to see that from inside the game.  On a surf
	map these are the ramp clip brushes and the playerclip shell around the
	route, and they are not a rounding error -- they were being lit, batched
	and rasterised every frame purely to be looked through.
	*/
	if (nodrawn)
		Con_DPrintf("VBSP: %i of %i faces hidden (tool brushes)\n", nodrawn, count);

	return true;
}

#ifdef HAVE_CLIENT
/*
FTESurf Patch 268 D1: how many luxel SETS does each lightstyle occupy in this map's
lighting lump, and tell the engine's style walk.

Source's VRAD writes a face flagged SURF_BUMPLIGHT as NUM_BUMP_VECTS+1 = 4 luxel sets per
style -- the flat map and three radiosity-normal-map basis maps (vrad/lightmap.cpp:3401-3411;
radial.cpp:700 loops k over styles and :739 lays a face out as [style][set][luxel]) -- and
nstyles*4 bytes of per-face average colour BEFORE the block, which lightofs already skips
(lightmap.cpp:3396-3399).  Surf_BuildLightMap (engine/client/r_surf.c) advanced one set per
style, so on a bumped face with two styles it summed basis map 0 in as "style 1" -- at
264/256, because nothing sets a style above 0 on a Source map (gl_rlight.c:187-189;
sv_main.qc:440 sets style 0 only) -- and the REAL second style's map was never read.  Which
way a face moves when that is fixed depends on the map: where the second style is a bright
switchable light (surf_affliction style 37) the face gets about 2x BRIGHTER, where it is a
dim one (bhop_orgrimmar style 32, bhop_floodwaters 32/35/36) the face darkens 30-50%.  Set 0
of style 0 sits exactly at lightofs, so single-style bumped faces were always right; this
is the style walk only.

THE STRIDE IS PROBED, NOT ASSUMED.  Over the 1310-map library (Patch 268 record): 1117 maps
store 1 set unbumped / 4 bumped; 150 (83 BSP v21, 67 Strata v25) store, after every style's
sets, one extra 4-byte-per-luxel record that is not RGBE data (exponent bytes -119..124 on
one face, all-zero or all-exponent-0 on others: bhop_floodwaters faces 1360, 550, 60); 9 v21
maps store 1 / 4 and then append nstyles*luxels bytes (all zero on every face read) AFTER
the last style -- a per-face trailer, which is why it is NOT a per-style extra here (surf_sup
faces 22/31/792/795: style 1 decodes cleanly at lightofs + luxels*4 and reads garbage one
luxel-count of bytes later); 26 are unlit; 8 fit nothing (7 Strata df_ maps whose faces
share lightofs, and one 2/5 map with a single odd face).  Each candidate predicts the whole
lump's size from nothing but the faces, so at most one can equal it; when none does, every
surface is left untagged and the walk is exactly what it was.  That fallback is printed,
never silent, and so is hl2_bumpstride 0, so the log always says which walk a screenshot
came from.

The verdict travels on each surface as SURF_LMSTRIDE (gl_model.h): extra bytes per luxel
per style beyond E5BGR9's own four, i.e. what sits BETWEEN one style's flat map and the
next's.  A face whose block would run past the lump is left untagged and counted; the loader
never bounds-checked style 0 either, but tagging widens the read and must not widen it off
the end.

Those same nine trailer maps are also the ones whose lightofs values sit off the 4-byte
grid, and VBSP_RepackLighting (Patch 281, run at the tail of VBSP_LoadFaces) has by now
rebuilt their lightdata as one luxel set per style, copying only set 0 of each style.  On
such a map the raw layout is gone, no candidate can match the new size, and the walk is
already right -- so the probe says so and returns before it could print UNPROVEN.
*/
static void VBSP_ProbeLightmapStride (model_t *mod)
{
	vbspinfo_t *prv = (vbspinfo_t*)mod->meshinfo;
	//candidate layouts: luxel sets per style unbumped / bumped, and trailer bytes per luxel
	//per style that sit AFTER the last style (size proof only -- never walked, never tagged)
	static const struct { int flat, bump, trailer; const char *name; } cand[3] =
		{ {1,4,0,"1/4"}, {2,5,0,"2/5"}, {1,4,1,"1/4+trailer"} };
	size_t predicted[3] = {0,0,0};
	unsigned lit = 0, bumped = 0, styled = 0, bumpedstyled = 0, tagged = 0, clipped = 0;
	int i, c, chosen = -1, nfit = 0;
	const char *why;

	if (!mod->lightdata || !mod->lightdatasize || !mod->surfaces || !mod->texinfo || !prv->texinfo)
		return;	//unlit map (26 in the library ship a 0-byte lighting lump) or no renderer: no samples, nothing walked, nothing to print

	if (prv->lightrepacked)
	{	//Patch 281 rebuilt lightdata dense: set 0 of every style, nothing between styles, so the one-set walk is exact and there is no raw layout left to prove.
		Con_Printf("VBSP: %s lightmap was repacked per face (Patch 281): one luxel set per lightstyle, no stride tag needed\n", mod->name);
		return;
	}

	for (i = 0; i < mod->numsurfaces; i++)
	{
		msurface_t *surf = mod->surfaces + i;
		int ns, smax, tmax;
		size_t lux, base;
		qboolean b;
		if (!surf->samples)
			continue;
		for (ns = 0; ns < MAXCPULIGHTMAPS && surf->styles[ns] != INVALID_LIGHTSTYLE; ns++)
			;
		smax = (surf->extents[0]>>surf->lmshift)+1;
		tmax = (surf->extents[1]>>surf->lmshift)+1;
		if (!ns || smax <= 0 || tmax <= 0)
			continue;	//gl_model.c:3960 denies these a lightmap anyway (surf_huecomundo has one)
		lux = (size_t)smax*tmax;
		base = (size_t)ns*lux*4;
		b = prv->texinfo[surf->texinfo - mod->texinfo].bumped;
		lit++;
		if (b)
			bumped++;
		if (ns > 1)
		{
			styled++;
			if (b)
				bumpedstyled++;
		}
		for (c = 0; c < 3; c++)
			predicted[c] += (size_t)ns*4 + base*(b?cand[c].bump:cand[c].flat) + (size_t)ns*lux*cand[c].trailer;
	}
	if (!lit)
		return;
	for (c = 0; c < 3; c++)
	{
		if (predicted[c] == (size_t)mod->lightdatasize)
		{
			chosen = c;
			nfit++;
		}
	}

	if (nfit != 1)
	{
		Con_Printf(CON_WARNING "VBSP: %s lightmap stride UNPROVEN -- lighting lump %u bytes; 1/4 predicts %u, 2/5 %u, 1/4+trailer %u (%u lit faces, %u bumped, %u lightstyled, %u bumped+lightstyled): style walk left at one luxel set\n",
			mod->name, (unsigned)mod->lightdatasize, (unsigned)predicted[0], (unsigned)predicted[1], (unsigned)predicted[2],
			lit, bumped, styled, bumpedstyled);
		return;
	}

	if (!hl2_bumpstride->ival)
		why = "hl2_bumpstride 0: style walk left at one luxel set";
	else
	{
		for (i = 0; i < mod->numsurfaces; i++)
		{
			msurface_t *surf = mod->surfaces + i;
			int ns, smax, tmax, extra;
			size_t lux, ofs;
			qboolean b;
			if (!surf->samples)
				continue;
			for (ns = 0; ns < MAXCPULIGHTMAPS && surf->styles[ns] != INVALID_LIGHTSTYLE; ns++)
				;
			smax = (surf->extents[0]>>surf->lmshift)+1;
			tmax = (surf->extents[1]>>surf->lmshift)+1;
			if (!ns || smax <= 0 || tmax <= 0)
				continue;
			lux = (size_t)smax*tmax;
			b = prv->texinfo[surf->texinfo - mod->texinfo].bumped;
			extra = 4*((b?cand[chosen].bump:cand[chosen].flat) - 1);	//bytes between styles only; max 16, fits the 5-bit field.  The trailer is AFTER the last style and is never walked.
			if (!extra)
				continue;	//one set per style: today's walk is already right for this face
			ofs = surf->samples - mod->lightdata;
			if (ofs + (size_t)ns*lux*(4+extra) > (size_t)mod->lightdatasize)
			{
				clipped++;
				continue;
			}
			surf->flags |= extra << SURF_LMSTRIDE_SHIFT;
			tagged++;
		}
		why = tagged ? "style walk advances past the extra sets (hl2_bumpstride 1)" : "no face needs more than one set";
	}
	Con_Printf("VBSP: %s lightmap stride %s: %i luxel set(s) per lightstyle flat, %i bumped%s; %u lit faces, %u bumped, %u lightstyled (%u of those bumped); %u tagged, %u left untagged (block past lump) -- %s\n",
		mod->name, cand[chosen].name, cand[chosen].flat, cand[chosen].bump, cand[chosen].trailer?" plus a per-face trailer of one byte per luxel per style after the last style":"",
		lit, bumped, styled, bumpedstyled, tagged, clipped, why);
}
#endif
#ifdef HAVE_CLIENT
static void VBSP_BuildSurfMesh(model_t *mod, msurface_t *surf, builddata_t *bd)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;

	unsigned int vertidx;
	int i, lindex, edgevert;
	mesh_t *mesh = surf->mesh;
	float *vec;
	float s, t, miss;
	int sty;
	struct vbsptexinfo_s *vtexinfo;

	//displacement surfaces...
	dispinfo_t *d = prv->surfdisp[surf-mod->surfaces];
	if (d)
	{
		struct dispvert_s *dv = d->verts;

		mesh->istrifan = false;

		memcpy(mesh->indexes, d->idx, sizeof(index_t)*d->numindexes);

		//output the renderable verticies
		for (i=0 ; i<mesh->numvertexes ; i++, dv++)
		{
			//xyz
			VectorCopy (d->xyz[i], mesh->xyz_array[i]);

			//st
			mesh->st_array[i][0] = dv->st[0];
			mesh->st_array[i][1] = dv->st[1];
			if (surf->texinfo->texture->vwidth)
				mesh->st_array[i][0] /= surf->texinfo->texture->vwidth;
			if (surf->texinfo->texture->vheight)
				mesh->st_array[i][1] /= surf->texinfo->texture->vheight;

			//lmst
			s = (float)(i%(d->width+1))/d->width;
			t = (float)(i/(d->width+1))/d->height;
			for (sty = 0; sty < 1; sty++)
			{
				mesh->lmst_array[sty][i][0] = (s*surf->extents[0] + surf->light_s[sty] + 0.5) / (mod->lightmaps.width);
				mesh->lmst_array[sty][i][1] = (t*surf->extents[1] + surf->light_t[sty] + 0.5) / (mod->lightmaps.height);
			}

			//normals
			VectorCopy(surf->plane->normal, mesh->normals_array[i]);
			VectorCopy(surf->texinfo->vecs[0], mesh->snormals_array[i]);
			VectorNegate(surf->texinfo->vecs[1], mesh->tnormals_array[i]);

			//rgba
			for (sty = 0; sty < 1; sty++)
			{
				mesh->colors4f_array[sty][i][0] = 1;
				mesh->colors4f_array[sty][i][1] = 1;
				mesh->colors4f_array[sty][i][2] = 1;
				mesh->colors4f_array[sty][i][3] = ((int)dv->alpha&255)/255.0;
			}
		}
		return;
	}

	//regular surfaces...
	mesh->istrifan = true;

	//output the mesh's indicies
	for (i=0 ; i<mesh->numvertexes-2 ; i++)
	{
		mesh->indexes[i*3] = 0;
		mesh->indexes[i*3+1] = i+1;
		mesh->indexes[i*3+2] = i+2;
	}
	//output the renderable verticies
	for (i=0 ; i<mesh->numvertexes ; i++)
	{
		lindex = mod->surfedges[surf->firstedge + i];
		edgevert = lindex <= 0;
		if (edgevert)
			lindex = -lindex;
		if (lindex < 0 || lindex >= mod->numedges)
			vertidx = 0;
		else
			vertidx = mod->edges[lindex].v[edgevert];
		vec = mod->vertexes[vertidx].position;

		s = DotProduct (vec, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3];
		t = DotProduct (vec, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3];

		VectorCopy (vec, mesh->xyz_array[i]);

		mesh->st_array[i][0] = s;
		mesh->st_array[i][1] = t;
		if (surf->texinfo->texture->vwidth)
			mesh->st_array[i][0] /= surf->texinfo->texture->vwidth;
		if (surf->texinfo->texture->vheight)
			mesh->st_array[i][1] /= surf->texinfo->texture->vheight;

		vtexinfo = &prv->texinfo[surf->texinfo-mod->texinfo];
		s = DotProduct (vec, vtexinfo->lmvecs[0]) + vtexinfo->lmvecs[0][3];
		t = DotProduct (vec, vtexinfo->lmvecs[1]) + vtexinfo->lmvecs[1][3];
		for (sty = 0; sty < 1; sty++)
		{
			mesh->lmst_array[sty][i][0] = (s - surf->texturemins[0] + (surf->light_s[sty]<<surf->lmshift) + (1<<surf->lmshift)*0.5) / (mod->lightmaps.width<<surf->lmshift);
			mesh->lmst_array[sty][i][1] = (t - surf->texturemins[1] + (surf->light_t[sty]<<surf->lmshift) + (1<<surf->lmshift)*0.5) / (mod->lightmaps.height<<surf->lmshift);
		}

		//figure out the texture directions, for bumpmapping and stuff
// 		if (surf->flags & SURF_PLANEBACK)
// 			VectorNegate(surf->plane->normal, mesh->normals_array[i]);
// 		else
		VectorCopy(surf->plane->normal, mesh->normals_array[i]);
		VectorCopy(surf->texinfo->vecs[0], mesh->snormals_array[i]);
		VectorNegate(surf->texinfo->vecs[1], mesh->tnormals_array[i]);
		//the s+t vectors are axis-aligned, so fiddle them so they're normal aligned instead
		miss = -DotProduct(mesh->normals_array[i], mesh->snormals_array[i]);
		VectorMA(mesh->snormals_array[i], miss, mesh->normals_array[i], mesh->snormals_array[i]);
		miss = -DotProduct(mesh->normals_array[i], mesh->tnormals_array[i]);
		VectorMA(mesh->tnormals_array[i], miss, mesh->normals_array[i], mesh->tnormals_array[i]);
		VectorNormalize(mesh->snormals_array[i]);
		VectorNormalize(mesh->tnormals_array[i]);

		//q1bsp has no colour information (fixme: sample from the lightmap?)
		for (sty = 0; sty < 1; sty++)
		{
			mesh->colors4f_array[sty][i][0] = 1;
			mesh->colors4f_array[sty][i][1] = 1;
			mesh->colors4f_array[sty][i][2] = 1;
			mesh->colors4f_array[sty][i][3] = 1;
		}
	}
}


static void VBSP_LoadLighting (model_t *mod, qbyte *mod_base, vlump_t *ldr, vlump_t *hdr)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	qbyte *src;
	unsigned int *out;
	size_t count;
	if (hdr->filelen && !(hl2_favour_ldr->ival && ldr->filelen))
	{
		mod->lightdatasize = hdr->filelen;
		src = (mod_base + hdr->fileofs);
	}
	else if (ldr->filelen)
	{
		mod->lightdatasize = ldr->filelen;
		src = (mod_base + ldr->fileofs);
	}
	else
		return;

	mod->lightmaps.fmt = LM_E5BGR9;
	mod->lightdata = (qbyte*)(out = plugfuncs->GMalloc(&mod->memgroup, mod->lightdatasize));

	//FTESurf Patch 281: keep the raw lump.  This whole-lump pass assumes every
	//face's lightofs is a multiple of 4; VBSP_RepackLighting (called at the end
	//of VBSP_LoadFaces, once the faces say where they really start) redoes the
	//faces of the nine maps for which that is false, from these bytes.
	prv->lightraw = src;
	prv->lightrawsize = mod->lightdatasize;

	//convert from linear e8bgr8 to srgb e5bgr9
	for (count = mod->lightdatasize/4; count --> 0; src+=4)
		*out++ = VBSP_ConvertLuxel(src);
}
#endif

typedef struct
{
	unsigned short planenum;
	short texinfo;
	unsigned short dispinfo;
	short bevel;
} hl2dbrushside_t;
typedef struct
{	//Strata v25 (LUMP_BRUSHSIDES_VERSION 1): 32-bit planenum/texinfo + a `thin` byte
	unsigned int	planenum;
	int				texinfo;
	int				dispinfo;
	unsigned char	bevel;
	unsigned char	thin;
} strata_dbrushside_t;
static qboolean VBSP_LoadBrushSides (model_t *mod, qbyte *mod_base, vlump_t *l)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	unsigned int			i, j;
	q2cbrushside_t	*out;
	int			count;
	unsigned int			num;
	//Strata v25 widened planenum (16->32) and texinfo (16->32) and added a `thin` byte.
	qboolean wide = (l->version >= 1);
	size_t elemsz = wide ? sizeof(strata_dbrushside_t) : sizeof(hl2dbrushside_t);
	hl2dbrushside_t     *ins = (void *)(mod_base + l->fileofs);
	strata_dbrushside_t *inl = (void *)(mod_base + l->fileofs);

	if (l->filelen % elemsz)
	{
		Con_Printf (CON_ERROR "VBSP_LoadBrushSides: funny lump size\n");
		return false;
	}
	count = l->filelen / elemsz;

	// need to save space for box planes
	if (count > SANITY_MAX_MAP_BRUSHSIDES)
	{
		Con_Printf (CON_ERROR "Map has too many brushsides (%i)\n", count);
		return false;
	}

	out = prv->brushsides = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out) * count);
	prv->numbrushsides = count;

	for ( i=0 ; i<count ; i++, out++)
	{
		num = wide ? LittleLong(inl[i].planenum) : (unsigned short)LittleShort(ins[i].planenum);
		out->plane = &mod->planes[num];
		j = wide ? (unsigned int)LittleLong(inl[i].texinfo) : (unsigned short)LittleShort(ins[i].texinfo);
		if (j >= mod->numtexinfo)	//also catches texinfo == -1 (wraps huge)
			out->surface = &nullsurface;
		else
			out->surface = &prv->surfaces[j];
	}

	return true;
}

typedef struct {
	unsigned int count;
	struct {
		unsigned int id;
		unsigned short flags;
		unsigned short version;
		unsigned int ofs;
		unsigned int len;
	} sl[1];
} hlgamelumpheader_t;
#ifdef HAVE_CLIENT
/*
=================
VBSP_LoadPropBakedLight						FTESurf Patch 163

VRAD BAKES THE LIGHTING FOR EVERY STATIC PROP AND WE HAVE NEVER READ IT.

Reported as "they still don't have the correct hue, with each of them a slightly
different colour per model", against a Momentum screenshot of the same corridor
where all ten read as one warm brown.

The leaf ambient cube is NOT how Source lights a static prop.  It is the
fallback -- for dynamic models, and for props that set
STATIC_PROP_NO_PER_VERTEX_LIGHTING (12 of surf_666's 653).  For everything else
VRAD runs a full radiosity gather at every vertex of the prop and writes the
result into the map's own pakfile as sp_<index>.vhv, one file per PROP
INSTANCE.  surf_666 ships 633 of them.  That is why our props came out with a
different colour each: we were reading a six-sample-per-leaf average and picking
whichever sample happened to be nearest, and neighbouring props kept landing on
different samples -- and in one leaf, on a sample VRAD had written blue.

What the two data sets say about the ten props in question:

  our leaf ambient cube   left five R/B 0.22-0.66, right five R/B 1.62-1.78
  VRAD's own bake         all ten mean (22, 15, 13), R/B 1.67, peak (58,41,38)

Symmetric geometry, symmetric lighting, and warm -- which is the screenshot.

THE BYTE ORDER IS BGRA, and that was measured rather than recalled.  Over the
551 props that also have a usable ambient cube, the correlation between the
prop's baked hue and its leaf's ambient hue is +0.757 reading the bytes as BGR
and -0.757 reading them as RGB.  Two VRAD outputs for the same point cannot
disagree that consistently; only one ordering can be right, and the sign says
which.  (91% of those leaves are warm and only 7% of the props have byte0 >
byte2, which is the same finding stated the other way.)

We collapse the per-vertex data to one colour, because FTE lights a model with
an ambient + a directional term per ENTITY and the prop's mesh is shared by
every instance of that model -- per-vertex colours would need a per-instance
vertex stream.  The mean is the honest collapse: it is exactly what VRAD says
the prop's lit surface averages to.  What is lost is the gradient WITHIN a
prop, not the difference BETWEEN props, and the report was about the latter.

FTESurf Patch 259 -- AND THE ENGINE TURNS OUT TO HAVE HAD THAT PER-INSTANCE
VERTEX STREAM ALL ALONG.

The paragraph above is right about the constraint and wrong about the
conclusion.  entity_t::vertlightcolors, PropLight_Find and the VC shader
permutation were built for the Quake side's RGBPROPLIGHT lump and do exactly
this: swap ONE surface's colour attribute to a per-instance array while the
positions stay in the model's shared VBO (gl_alias.c, R_GAlias_DrawBatch).
Nothing about it was Quake-specific.  Three things were missing, and none of
them was the hard part:

  - mod_hl2.c never set galiasinfo_t::firstvert, so a surface could not find
    its slice of a global-order colour array;
  - vmt/vertexlit.glsl never declared `!!permu VC`, so `perm &=
    supportedpermutations` stripped the permutation before it could run;
  - and this function threw the distribution away one line after reading it.

WHAT IT COSTS, MEASURED RATHER THAN GUESSED.  Draw calls: nothing.  Props are
already submitted one entity_t per prop per frame (VBSP_DrawProps), FTE has no
hardware instancing and does no cross-prop batching, so there was never a batch
to lose.  Per-frame CPU: two pointer writes per surface, after a lighting solve
that light_known already caches for the life of the map.  RAM: 4 bytes per LOD0
vertex per prop instance, which over the whole 1310-map Momentum library is 312
maps that ship a bake at all, mean 2.2 MiB each, worst surf_chungus_fungus at
25.0 MiB (531 props, 6,390,951 verts).  Storing the multiplier as vec4 floats
the way the Quake path does would be 4x all of those numbers -- 97 MiB on that
one map, re-read by the driver every frame -- which is why this path is bytes.

WHAT INDEX SPACE THE FILE IS IN, WHICH IS NOT THE ONE IT LOOKS LIKE.

It is tempting -- and it is what this patch first assumed -- to read the .vhv as
one colour per .vvd LOD0 vertex.  On surf_garden that is exactly right: prop 170
has 15447 colours against lodverts_count[0] = 15447, and its three LOD0 blocks
[2721, 4082, 8644] are the MDL's own mesh layout.  It is also a coincidence.

surf_demise's tree_douglasfir01d_opt_large says so plainly.  Its .vhv LOD0 block
is 2031 colours; its .vvd says LOD0 has 2787 vertices.  The file is not short or
corrupt -- its LOD3 block is 926, which IS that model's lodverts_count[3], and
its checksum matches the .mdl.  What 2031 is, exactly, is the sum of the .vtx's
LOD0 strip-group vertex counts.  Measured across four models spanning both cases
and every LOD they have, the .vhv block counts equal the .vtx strip-group counts
every time -- 2031/1857/1506/926 against 2031/1857/1506/926, and s3hedge2's
2721/4082/8644 against the same three.

So VRAD writes one colour per VTX STRIP-GROUP VERTEX, which is what Source
actually renders, and each of those names a .vvd vertex through its
origMeshVertID.  We keep the .vvd's own array (Mod_HL2_LoadVVD copies it
straight through) and push that indirection into the INDEXES instead
(Mod_HL2_LoadIndexes).  The two index spaces therefore coincide only when every
.vvd vertex is used exactly once and in order -- common, and not a rule.

It usually does NOT, and testing the counts does not reveal it.  s3hedge2 has
15447 colours for 15447 vertices, so a length check passes -- and the first
build of this patch bound the file directly on that basis and drew the fountain
covered in dark blotches, because a strip group's vertex list is ordered for the
post-transform cache and not by vertex index.

So mod_hl2.c walks LOD0's strip groups at load and keeps what that walk resolves:
vhvmap[k] is our global vertex index for VRAD's colour k (galiasinfo_t, first
surface).  Scattering through it -- out[vhvmap[k]] = colour[k] -- is correct by
construction rather than by a coincidence we could only partly verify, and it
subsumes the .vvd fixup case, which is just another indirection in the same
composition.  Measured: it takes surf_demise from 132 of 464 drawn props to
463 of 463, and conc_school from 1 of 3 to 3 of 3.

The scatter cannot happen at map load, because the .vhv is read while the model
is still a name -- models load on worker threads.  So these colours stay in
VRAD's order in `vcraw` until the first frame that lights the prop, which is
also the frame that already has the model; see the block in VBSP_DrawProps.
Vertices the strip groups never name are prefilled with the byte that reproduces
the plain mean, so an unreferenced vertex lands where mode 1 would put it.

THE NORMALISATION, AND WHY IT IS TO THE PEAK AND NOT THE MEAN.  A normalised
ubyte attribute can only carry 0..1, and a mean-centred multiplier has to
exceed 1 for every vertex brighter than average -- so the Quake path's
"~1.0-centred, clamp to r_propvertexlight_max" cannot be expressed in bytes.
Normalising to the PEAK instead makes the byte range exact: store
m_v = enc(lum_v)/enc(lum_peak) in 0..1, and multiply the gain enc(lum_peak)/
enc(lum_mean) back into `baked`, which is the entity's base light.  The product
is enc(lum_v) either way, so nothing is scaled away -- and the quantisation
error left over is a uniform +-0.5/255 of the prop's BRIGHTEST vertex, i.e.
half a step of the 8-bit gamma-encoded value VRAD itself wrote.  Storing it
mean-centred and clamped would have been the lossy choice, not this.

Greyscale, for the same reason the Quake path is: `baked` already carries this
prop's colour, taken from the mean of this same file.  A per-channel multiplier
here would apply that hue twice.  What varies per vertex is where the light
falls, and that is a luminance.

Format is HardwareVerts (studio/HardwareVerts.h): a 40-byte header -- and it is
40, not 24, because six ints are followed by int m_nUnused[4] -- then one
28-byte MeshHeader_t per mesh, then the colours at each mesh's own offset.  Only
LOD 0 is summed; the lower LODs carry their own copies of the same lighting and
we never draw them.
=================
*/
typedef struct
{
	int				version;
	int				checksum;
	unsigned int	vertexflags;
	unsigned int	vertexsize;
	unsigned int	numverts;
	int				nummeshes;
	int				unused[4];
} vhvheader_t;
typedef struct
{
	unsigned int	lod;
	unsigned int	numverts;
	unsigned int	offset;
	unsigned int	unused[4];
} vhvmesh_t;
//FTESurf Patch 259: the sRGB decode is a pow() per channel per vertex and this loop now runs over
//every LOD0 vertex of every prop in the map (6.4M on the library's worst).  The input is a byte, so
//there are only 256 answers; build them once.
static float vbsp_srgb2lin[256];
/*
FTESurf Patch 296: VRAD'S ACTUAL CURVE, which is not sRGB.

Settled by citation and then confirmed by a single measured number.  A .vhv byte
is written by ConvertLinearToRGBA8888 (utils/vrad/lightmap.cpp:3583-3601) through
LinearToVertexLight (public/mathlib/mathlib.h:1410-1428), which is a lookup into
the table built at mathlib/color_conversion.cpp:233-261:

	f = pow(i/1024.0, 1.0/gamma);  lineartovertex[i] = f * overbrightFactor;

and VRAD initialises that with MathLib_Init(2.2f, 2.2f, 0.0f, 2.0f, ...) at
utils/vrad/vrad.cpp:2356 -- gamma 2.2, overbright 2, so overbrightFactor 0.5.
The whole encode is therefore

	byte = round(255 * min(1, 0.5 * pow(clamp(linear,0,4), 1/2.2)))

THE CONFIRMING MEASUREMENT: the maximum colour byte over every LOD0 vertex of
every prop is exactly 239, in BOTH the CS:S and the Momentum build of
surf_fantasy.  round(255 * 0.5 * 4^(1/2.2)) = 239.  Nothing else produces that
ceiling, so the exponent, the clamp and the 0.5 are all pinned by it -- and so,
incidentally, is hl2_lt_baked_scale's default of 2, which is exactly
1/overbrightFactor.  (The gl_overbright argument at the encode site below reaches
the same number by a different route; both are true, this one is primary.)

The inverse is pow(b/255 * 2, 2.2) = pow(b/127.5, 2.2), so the table runs to
2^2.2 = 4.5948 rather than to 1 -- the 0.5 is undone here rather than clamped
away.  vbsp_srgb2lin inverts sRGB instead (1/2.4 with a linear toe), which is a
few percent out and worst in the shadows.

vbsp_bytelin is the third arm: no decode at all, mean the bytes as written.  It
exists because the level (acc[], decoded) and the per-vertex multiplier
(encsum/encpeak, raw bytes) are derived in DIFFERENT SPACES, and that split is
its own error -- a surf_boreas prop draws a median 1.66x brighter than 2x VRAD's
byte at its own mean vertex, ranging 0.31x to 2.4x per prop.  Arm 2 is 1.000 by
construction and so measures the size of that split directly.

Three tables and a pointer rather than a branch: this loop runs over every LOD0
vertex of every prop in the map, 6.4M on the library's worst.
*/
static float vbsp_vrad2lin[256];
static float vbsp_bytelin[256];
static void VBSP_InitBakeTables(void)
{
	unsigned int i;
	if (vbsp_srgb2lin[255] != 0)
		return;
	for (i = 0; i < 256; i++)
	{
		vbsp_srgb2lin[i] = M_SRGBToLinear(i*(1/255.0f), 1);
		vbsp_vrad2lin[i] = (float)pow(i*(1/127.5), 2.2);
		vbsp_bytelin[i]  = (float)i;
	}
}
static qboolean VBSP_LoadPropBakedLight(model_t *mod, struct staticprop_s *sent, unsigned int lumpidx, qboolean pervertex)
{
	char name[64];
	size_t fsize = 0, m, k, n = 0;
	qbyte *f;
	const vhvheader_t *h;
	const vhvmesh_t *mesh;
	double acc[3] = {0,0,0};
	//Patch 259: the ENCODED luminance of each vertex, which is what the shader multiplies.
	//Deliberately the luminance of the bytes as VRAD wrote them, not the encode of the linear
	//luminance: the ratio between two of these is exactly the ratio Source's own renderer applies,
	//and it costs no pow().  encsum/encpeak below are in the same 0..255 units as the file.
	double encsum = 0;
	float encpeak = 0;
	//FTESurf Patch 296: the file's vertex STRIDE, and how many colours that is.
	unsigned int vsize, ncol;
	static cvar_t *vhv12;
	//FTESurf Patch 296: which decode curve, and the table that implements it.
	static cvar_t *curve;
	const float *dec;
	int curvearm;

	VBSP_InitBakeTables();
	/*
	FTESurf Patch 296: DEFAULT 1.  It was 0, and the note that kept it there said
	"1 is measured DARKER than today on surf_fantasy".  That measurement was real
	and its reading was wrong, for two reasons that have both since been found:

	  - it was a screen-pixel comparison, and turning the bake on ALSO flips the
	    directional fraction from FS_BAKED_DIR to 0.0 at the apply site (:8451),
	    so what it saw was largely props going FLAT, not props going dim; and
	  - every prop-level number it was checked against came out of the PROPLIGHT
	    census, which until this patch reported sent->baked AFTER the per-prop
	    peak gain -- inflated 2.05x on fantasy and 20.98x on boreas.  See the
	    note at the census itself.

	The read was never in doubt; what was in doubt was whether three 4-byte
	colours per vertex is really what a 12-byte .vhv holds.  It is, and this is
	no longer an inference from hue statistics:

	  - on 465 of surf_fantasy's 2345 props the 12 bytes are THREE BIT-IDENTICAL
	    COPIES of one BGRA (17.98% of all 3,563,988 LOD0 vertices).  A 4x BGR
	    reading cannot produce a period-4 repeat.
	  - against the CS:S build of the same map, which ships ordinary 4-byte
	    files for 2343 props, per-vertex Pearson r is 0.764 over 300
	    checksum-matched props; a shuffled control (same prop against a random
	    equal-vertex-count prop) is 0.236.
	  - the MEAN of the three out-correlates every individual slot
	    (0.654 / 0.658 / 0.692), which is the radiosity-normal signature and is
	    the direct justification for averaging them.
	  - the maximum colour byte is 239 in both builds -- the same
	    LinearToVertexLight ceiling -- so the two are the same encode and take
	    the same decode.

	And it cannot regress a map that works today: the 67 maps this admits
	currently get ZERO baked prop lighting, so the comparison is bake against
	nothing, not bake against bake.  4-byte maps take the ncol == 1 branch below
	whatever this says.  0 is still bit-for-bit the old picture.
	*/
	if (!vhv12) vhv12 = cvarfuncs->GetNVFDG("hl2_lt_vhv12", "1", 0, "Accept VRAD's 12-byte-per-vertex baked static prop lighting (three radiosity-normal basis colours), collapsing it to one colour by the mean that utils/vrad/lightmap.cpp:3479-3549 constructs them to satisfy. 67 maps ship it and lose all baked prop lighting with this off. 0 = the pre-Patch-296 picture.", "");
	//Read at load, like hl2_lt_baked and hl2_bumpstride: an A/B needs a map
	//change or a second process, because `map <the same name>` does not reload.
	if (!curve) curve = cvarfuncs->GetNVFDG("hl2_lt_baked_curve", "1", 0, "Which curve decodes VRAD's baked static prop lighting.\n0: sRGB, the pre-Patch-296 behaviour, bit-for-bit.\n1: VRAD's own pow(b/127.5, 2.2), which is what utils/vrad wrote and the only one that round-trips a byte exactly (default).\n2: no decode -- mean the stored bytes. Diagnostic: the level and the per-vertex multiplier are then derived in the same space, so it measures how far apart 0 and 1 hold them.", "");
	curvearm = curve ? curve->ival : 1;
	if (curvearm < 0 || curvearm > 2)
		curvearm = 1;
	dec = (curvearm == 2) ? vbsp_bytelin : (curvearm == 1) ? vbsp_vrad2lin : vbsp_srgb2lin;

	/*
	FTESurf Patch 257: sp_hdr_N.vhv FIRST.  Patch 163 only ever asked for
	sp_N.vhv, which is the name VRAD writes for an LDR compile -- so on every map
	compiled -hdr this function has returned false for every prop and the whole
	patch has been dead code there.

	Not a guess and not a new census: the line Patch 163 already prints says it.
	ftesurf/logs/ has "props: 0 of 298", "props: 0 of 3126" sitting next to
	"props: 1587 of 1587" and surf_demise's "2350 of 2350".  surf_garden ships
	428 sp_hdr_N.vhv and zero sp_N.vhv; surf_demise ships 2350 sp_N.vhv and no
	HDR set.  That is the entire difference between the two.

	The visible cost is exactly what Patch 163 was written to fix, back again on
	the HDR maps: with no bake every prop falls to the leaf ambient cube and the
	hl2_lt_min floor, which is the flat over-bright foliage in the surf_garden
	report against Momentum's properly shaded reference.

	HDR then LDR is Source's own order (CStaticProp::LoadLighting), and it has to
	be an order rather than a choice: a map may ship both sets, and the HDR one
	is what VRAD lit the world with.  Whichever file answers then goes through
	the same validation below -- the version/vertexsize/offset checks were always
	the thing standing between us and a stale file from another mounted map, and
	they do not care which name found it.
	*/
	Q_snprintfz(name, sizeof(name), "sp_hdr_%u.vhv", lumpidx);
	f = filefuncs->LoadFile(name, &fsize);
	if (!f)
	{
		Q_snprintfz(name, sizeof(name), "sp_%u.vhv", lumpidx);
		f = filefuncs->LoadFile(name, &fsize);
	}
	if (!f)
		return false;		//no bake for this prop: the ambient cube stays its answer

	if (fsize < sizeof(*h))
		goto reject;
	h = (const vhvheader_t*)f;
	//Validated rather than trusted: the map's pakfile is mounted as a plain
	//searchpath and these names carry no map prefix, so a stale sp_N.vhv from
	//another mounted map could in principle answer.  A file that is not
	//version 2 with a KNOWN vertex stride and in-range mesh offsets is not one.
	//(It was "4-byte colours" until Patch 296; that spelling of the test is what
	//discarded every baked prop on 67 maps, so the stride is now a set.)
	/*
	FTESurf Patch 296.  VERTEXSIZE IS A STRIDE, NOT A FORMAT TAG, and rejecting
	everything but 4 threw away every baked prop on surf_fantasy -- 2345 of 2345,
	which is the whole of "props: 0 of 2345 lit from VRAD's baked vertex lighting"
	sitting in the logs next to boreas's "1587 of 1587".

	Measured, from the two maps' own pakfiles:
	    boreas  sp_0.vhv      vertexsize 4   flags 0x4   0c 0b 0a ff
	    fantasy sp_hdr_0.vhv  vertexsize 12  flags 0x2   27 53 63 00 2c 5e 70 00 25 4e 5e 00
	Twelve bytes is THREE BGRA colours -- the three radiosity-normal-map basis
	colours VRAD writes for a bumped prop, the same three the world's bumped
	lightmap carries.

	HOW THAT IS KNOWN, stated carefully because the obvious evidence is wrong.
	In sp_hdr_0.vhv byte slots 3, 7 and 11 are 0 on all 1244 vertices, which looks
	like three alpha bytes -- but ACROSS THE WHOLE MAP THEY ARE NOT: 14.05% of
	surf_fantasy's 3,563,988 LOD0 vertices carry a nonzero one (slot 3 on 12.14%,
	slot 7 on 8.00%, slot 11 on 13.50%, values to 144, smooth histogram, 687 of the
	2345 props).  Do not rest the layout on those zeros.  What does establish it:
	the fourth byte is not an RGBExp32 exponent (an exponent clusters at 0..5 and
	246..255 as a signed char, not a smooth 1..144), it is not a multiplicative
	alpha the engine honours (86% of vertices would be black), and the competing
	4xBGR reading -- 12 = NUM_BUMP_VECTS+1 colours, which is exactly how the world
	bumped lightmap is laid out -- is refuted by hue: the sample splits as
	(39,83,99)(44,94,112)(37,78,94), three colours of ONE hue at relative magnitude
	1.00/1.13/0.95, where 4xBGR would make three of the four carry a zero channel.
	Across the map each colour's luminance against the mean of its three runs
	p01 0.457 / p50 1.000 / p99 1.577, which is the RNM signature.

	Gate on vertexsize and not on vertexflags, deliberately.  hardwareverts.h
	defines no flag enum at all (0x2 vs 0x4 is undecoded here), and its own comment
	on m_nVertexSize concedes the format is underspecified: "this won't be adequate,
	need some concept of byte format i.e. rgbexp32 vs rgba8888".  The stride is the
	one field Valve documents as meaning what it says, and it is what the reader
	actually needs.

	Collapsed to one colour by averaging the three, which is what a flat normal
	sees: the basis vectors sit at equal angles to it, so the RNM weights are equal.
	The linear average is taken in LINEAR for the same reason the loop below decodes
	before it accumulates.  Keeping the three separately is what per-pixel bumped
	prop lighting would want and is deliberately NOT done here -- that needs a
	shader and a vertex format, and this is the read.
	*/
	vsize = LittleLong(h->vertexsize);
	if (LittleLong(h->version) != 2)
		goto reject;
	if (vsize == 4)
		ncol = 1;
	else if (vsize == 12 && vhv12 && vhv12->ival)
		ncol = 3;
	else
		goto reject;
	if (h->nummeshes <= 0 || (size_t)LittleLong(h->nummeshes) > 1024)
		goto reject;
	if (sizeof(*h) + (size_t)LittleLong(h->nummeshes)*sizeof(*mesh) > fsize)
		goto reject;

	mesh = (const vhvmesh_t*)(f + sizeof(*h));
	for (m = 0; m < (size_t)LittleLong(h->nummeshes); m++)
	{
		size_t nv = LittleLong(mesh[m].numverts);
		size_t o  = LittleLong(mesh[m].offset);
		const qbyte *c;
		if (LittleLong(mesh[m].lod) != 0)
			continue;
		if (o > fsize || nv > (fsize-o)/vsize)
			goto reject;
		c = f + o;
		for (k = 0; k < nv; k++, c += vsize)
		{	//BGRA, and DECODED before it is averaged.  VRAD writes these
			//gamma-encoded; averaging encoded bytes averages the wrong
			//quantity and biases a prop with any contrast on it upward.
			//Patch 296: ncol of them, averaged -- see the note above.
			float e = 0;
			unsigned int sb = 0, sg = 0, sr = 0;
			unsigned int q;
			for (q = 0; q < ncol; q++)
			{
				const qbyte *p = c + q*4;
				//AVERAGE THE STORED BYTES, NOT THE DECODED VALUES.  Source defines the
				//flat reconstruction as the mean of what is stored: VRAD pre-scales the
				//triple so the arithmetic mean of the three BYTES equals what a
				//non-bumped compile would have written (LinearToBumpedLightmap,
				//utils/vrad/lightmap.cpp), and the live combine divides by sum(dp) to
				//match (lightmappedgeneric_ps2_3_x.h).  Decoding first and averaging
				//after is a mean in the wrong space, and sRGB decode is convex, so it
				//comes out BRIGHTER every time -- measured over all 2345 props of
				//surf_fantasy: p50 +1.4%, p95 +10.0%, max +16.8%, against 4-byte maps
				//that are unaffected.  That is a systematic brightening of exactly the
				//maps this patch newly admits, relative to the ones hl2_lt_baked_scale
				//was tuned on.
				sr += p[2];
				sg += p[1];
				sb += p[0];
				//Patch 259: and the encoded luminance, for the per-vertex multiplier.
				//Already a linear form on the bytes, so it is the byte-mean's luminance
				//and agrees with the collapse above by construction.
				e += p[2]*0.299f + p[1]*0.587f + p[0]*0.114f;
			}
			//Patch 296: `dec` is the curve chosen at the top of this function.
			//At arm 2 it is the identity, so acc[] is a mean of the bytes.
			acc[0] += dec[(sr + ncol/2)/ncol];
			acc[1] += dec[(sg + ncol/2)/ncol];
			acc[2] += dec[(sb + ncol/2)/ncol];
			e /= ncol;
			encsum += e;
			if (e > encpeak)
				encpeak = e;
		}
		n += nv;
	}
	if (!n)
		goto reject;

	/*
	Back into the same space res_ambient lives in, with the same overbright the
	world already gets.

	Surf_LightmapShift gives a VBSP world lightmap `shift = gl_overbright`
	(1 by default, r_surf.c:71-72).  That halves the luxel on the way into the
	texture and has the shader multiply it back, so a world surface is drawn at
	TWICE its raw VRAD value.  Model lighting has never had that factor, from
	the ambient cube or from here, and both read the same VRAD units.  That is
	why props have looked too dark next to the walls, and it is a good part of
	what hl2_lt_min was quietly compensating for.

	APPLIED AFTER THE ENCODE, WHICH IS WHERE THE WORLD APPLIES IT.  The obvious
	place is in linear, before the gamma -- and it is wrong twice over.  The
	world's overbright is a plain multiply of the ALREADY-ENCODED lightmap
	texel, done in the shader; and doing it in linear forces a clamp to 1.0
	before pow(), which on d1_canals_02 blows out a channel on 23.6% of its 356
	props at scale 4 while surf_666, being dark, clips nothing and looks fine.
	A brightness that is correct on one map and destroys a quarter of another is
	not a brightness, it is a coincidence.

	Nothing clamps here at all.  A light value above 1 is not an error -- the
	world's own overbright reaches 2 -- and it is multiplied by an albedo before
	anything reaches the framebuffer, which is where clamping belongs.
	*/
	{
		static cvar_t *obr;
		float s;
		int j;
		if (!obr) obr = cvarfuncs->GetNVFDG("hl2_lt_baked_scale", "2", 0, "Overbright applied to VRAD's baked static prop lighting. 2 is the same factor gl_overbright already gives the world lightmap and which model lighting has never had; 1 is the raw baked value, as VRAD wrote it.", "");
		s = obr->value;
		for (j = 0; j < 3; j++)
		{
			float lin = (float)(acc[j]/n);
			if (lin < 0) lin = 0;
			//FTESurf Patch 296: the re-encode has to be the inverse of whichever
			//decode ran, or the pair does not round-trip and the level moves for
			//a reason that has nothing to do with the light.
			switch (curvearm)
			{
			case 2:
				//No curve at all: acc[] is already a mean of bytes.
				sent->baked[j] = lin * s;
				break;
			case 1:
				//VRAD's own.  A single byte b decodes to (b/127.5)^2.2 and comes
				//back as exactly b, so this arm is lossless on a flat prop.  No
				//clamp: lin tops out at 2^2.2 = 4.5948 and 127.5*4.5948^(1/2.2)
				//is 255 exactly, which is the ceiling of the encode by
				//construction rather than by luck.
				sent->baked[j] = 127.5f * (float)pow(lin, 1.0/2.2) * s;
				break;
			default:
				if (lin > 1) lin = 1;	//only reachable from a corrupt file: the source byte cannot exceed 1
				sent->baked[j] = M_LinearToSRGB(lin, 1) * 255.0f * s;
				break;
			}
		}

		/*
		FTESurf Patch 259: keep the distribution as well as its mean.

		The multiplier is normalised to the prop's PEAK, not its mean, because a
		normalised ubyte carries 0..1 and a mean-centred multiplier has to exceed
		1 for every vertex above average.  The gain that takes out goes straight
		back into `baked`, so the product is unchanged at the mean vertex and this
		mode differs from mode 1 only in where the light sits, never in how much
		of it there is.

		THE ONE CLAMP.  baked*gain is the brightness the PEAK vertex will draw at,
		and there is a physical ceiling on it: VRAD wrote a byte, so no vertex can
		be brighter than white times hl2_lt_baked_scale.  The product can exceed
		that for a strongly saturated prop, because `baked` carries per-channel
		colour while the gain is derived from luminance -- a deep blue prop's blue
		channel encodes far above its own luminance.  Clamping the gain rather
		than the colour caps the peak at exactly what a white vertex would draw
		and leaves the hue alone.  gain is >= 1 by construction (peak >= mean) and
		this can only push it back towards 1.
		*/
		if (pervertex && encpeak > 0 && encsum > 0)
		{
			float encmean = (float)(encsum/n);
			float gainraw = encpeak/encmean;
			float gain = gainraw;
			float peakch = max(max(sent->baked[0], sent->baked[1]), sent->baked[2]);
			float ceiling = 255.0f * s;
			float overshoot;
			if (peakch > 0 && gain*peakch > ceiling)
				gain = ceiling/peakch;
			if (gain < 1)
				gain = 1;

			/*
			FTESurf Patch 296: AND THE CLAMP HAS TO MOVE vcraw WITH IT.

			The clamp above caps the PEAK vertex.  But the per-vertex bytes below are
			normalised to the UNCLAMPED encpeak while `baked` is multiplied by the
			CLAMPED gain, and the draw is baked * vcraw/255 -- so at the mean vertex a
			clamped prop lands on baked_pre * gain_clamped/gainraw.  The ENTIRE prop
			darkens by the overshoot, not just its peak, which is the opposite of what
			the essay above promises ("differs from mode 1 only in where the light sits,
			never in how much of it there is").  The no-vc fallback divides by the
			clamped gain and lands on baked_pre exactly, so the same prop is half a stop
			darker with per-vertex lighting ON than OFF.

			Measured: the clamp fires on 1.8% of surf_boreas's 1587 4-byte props and,
			once Patch 296 admits them, 11.6% of surf_fantasy's 2345 -- the 12-byte maps
			are simply more saturated, and gain is luminance-derived while baked is
			per-channel.  Overshoot where it fires: p50 1.423x (-0.51 stops), p95 2.536x,
			max 3.44x.  That is the darkening this patch was blamed for.

			Scaling inv by the overshoot puts the MEAN back on baked_pre and lets the
			bright vertices saturate their own byte instead, so the peak still draws at
			exactly the ceiling.  When the clamp does not fire, overshoot is exactly
			1.0f and `x * 1.0f` is bit-identical in IEEE -- so 98.2% of every existing
			4-byte prop is untouched, by construction rather than by measurement.
			*/
			overshoot = gainraw/gain;

			//Malloc, not GMalloc: this is VRAD's order, not ours, and it is freed
			//the moment the scatter has moved it into ours.
			sent->vcraw = plugfuncs->Malloc(n*4);
			sent->vcrawverts = (unsigned int)n;
			sent->vcgain = gain;
			for (j = 0; j < 3; j++)
				sent->baked[j] *= gain;

			//Pass 2, in FILE ORDER -- one colour per LOD0 strip-group vertex, which
			//is what the model's vhvmap is indexed by.  Greyscale: `baked` above
			//already carries this prop's colour, from the mean of this same file.
			{
				float inv = 255.0f/encpeak * overshoot;	//Patch 296: see THE CLAMP note above -- 1.0f exactly when unclamped
				qbyte *out = sent->vcraw;
				for (m = 0; m < (size_t)LittleLong(h->nummeshes); m++)
				{
					size_t nv = LittleLong(mesh[m].numverts);
					const qbyte *c = f + LittleLong(mesh[m].offset);
					if (LittleLong(mesh[m].lod) != 0)
						continue;
					for (k = 0; k < nv; k++, c += vsize, out += 4)
					{	//Patch 296: the same ncol-way average as the pass above, so
						//the multiplier and the mean it is normalised against are
						//derived from one quantity and cannot drift apart.
						float e = 0;
						unsigned int q;
						int b;
						for (q = 0; q < ncol; q++)
						{
							const qbyte *p = c + q*4;
							e += p[2]*0.299f + p[1]*0.587f + p[0]*0.114f;
						}
						b = (int)((e/ncol) * inv + 0.5f);
						if (b < 0) b = 0;
						if (b > 255) b = 255;
						out[0] = out[1] = out[2] = (qbyte)b;
						out[3] = 255;
					}
				}
			}
		}
	}
	plugfuncs->Free(f);
	sent->hasbaked = true;
	return true;
reject:
	plugfuncs->Free(f);
	return false;
}
#endif

/*
FTESurf Patch 228 -- THE STRIDE WAS NEVER A PROPERTY OF THE VERSION NUMBER.

VBSP_LoadStaticProps used to pick the per-prop record size from a switch on the
sprp lump version, walk `numprops*propsize` bytes, and bail at `if (size)` --
silently, with no message -- when the arithmetic did not land exactly on the end
of the lump.  A census of all 1310 maps in the Momentum library says that bail
was firing on 82 of them and discarding 32,127 static props:

    bspver sprp stride  code said   maps   props
        21   10     76         72     18    2374   <- bhop_ambience is one of these
        21   11     80         76     45   19994   <- `case 11: 19*4` matches NO map
        25   13     88         88     19    9759   <- right stride, wrong leaf-ref width

The premise was wrong.  CS:GO's "version 10" and Source 2013's "version 10" are
different records that share a version number, so no switch on the version can
ever be right.  What actually varies is a four-byte field VBSP inserts at offset
72 on every `bspver >= 21` lump, measured as zero in all 32,127 records:

     0..67  canonical, through DiffuseModulation   (v5-v8 genuinely differ here,
                                                    and the sequential parse below
                                                    is correct for them)
    68..71  uint  FlagsEx                (v10+)
    72..75  uint  <inserted>             bspver>=21 ONLY -- absent from the 502
                                         canonical v10@72 maps, which must not move
    76..79  float UniformScale           (v11)
    76..87  vec3  Scale                  (v12/v13, engine takes X)

So the stride is MEASURED -- `size / numprops` -- and the version supplies only a
BASE, meaning "canonical bytes through the scale".  `extra = stride - base` is
then the inserted field, and the scale is read at `72 + extra` rather than at a
hardcoded offset.  A record the lump does not divide evenly, or whose extra is
implausible, is now reported instead of dropped in silence.

This also fixes Strata (v13) twice over.  Its leaf-reference array is 32-bit, not
16-bit, which is the whole reason its correctly-sized 88-byte records failed the
old check; and with `base 84` its scale lands at 76..87, where the measurement
finds it (9,538 of 9,759 at exactly 1.0, the rest plausible: 2.0, 13.0, 5.0, and a
genuine non-uniform 0.868/0.672/0.463).  The OLD code read scale.X from offset 72
-- the inserted field -- so every Strata prop in the library loaded at scale ZERO.
*/
#define SPRP_MAXEXTRA	16		//generous: every lump measured inserts exactly 4.

typedef struct
{
	qbyte	*modelref;	size_t nummodels;
	qbyte	*leafref;	size_t numleafrefs;
	qbyte	*prop;		size_t numprops;
	size_t	stride;		//MEASURED, not assumed
} sprphdr_t;

//Walks the sprp header at an assumed leaf-reference width and reports whether the
//prop block then divides evenly into plausible records.  Every advance is bounds
//checked, because a wrong width guess would otherwise read off the end of the lump.
static qboolean VBSP_ParseSprpHeader(qbyte *lump, size_t lumpsize, size_t leafrefsize, size_t basestride, sprphdr_t *out)
{
	qbyte *o = lump;
	size_t avail = lumpsize, n;

	if (avail < 4) return false;
	n = (size_t)(unsigned int)LittleLong(*(int*)o);	o += 4; avail -= 4;
	if (n > avail/128) return false;
	out->nummodels = n;		out->modelref = o;
	o += n*128;				avail -= n*128;

	if (avail < 4) return false;
	n = (size_t)(unsigned int)LittleLong(*(int*)o);	o += 4; avail -= 4;
	if (n > avail/leafrefsize) return false;
	out->numleafrefs = n;	out->leafref = o;
	o += n*leafrefsize;		avail -= n*leafrefsize;

	if (avail < 4) return false;
	n = (size_t)(unsigned int)LittleLong(*(int*)o);	o += 4; avail -= 4;
	out->numprops = n;		out->prop = o;

	if (!n)
	{	//a lump that declares no props is legal, and has no stride to measure.
		out->stride = basestride;
		return !avail;
	}
	if (avail % n) return false;
	out->stride = avail / n;
	if (out->stride < basestride || out->stride - basestride > SPRP_MAXEXTRA)
		return false;
	return true;
}

//Strata (sprp v12+) widened the leaf-reference array from unsigned short to
//unsigned int.  Nothing else about the array changed.
static unsigned int VBSP_SprpLeafRef(const qbyte *base, size_t width, size_t idx)
{
	if (width == 4)
		return (unsigned int)LittleLong(*(const int*)(base + idx*4));
	return (unsigned short)LittleShort(*(const short*)(base + idx*2));
}

static qboolean VBSP_LoadStaticProps(model_t *mod, qbyte *offset, size_t size, int version)	//present on server, because they're potentially solid.
{
	vbspinfo_t *prv = mod->meshinfo;
	struct {
		const char name[128];
	} *modelref;
	size_t nummodels, numleafrefs, numprops, i;
	qbyte *leafref;
	size_t leafrefsize, leafrefidx;
	struct staticprop_s *sent;
	entity_t *ent;

	size_t modelindex;

	qboolean skip = false;
	int dxlevel = 95, cpulevel=0, gpulevel=0;
	unsigned int leafcount, l;
	qbyte propflags;
	unsigned int propflagsex = 0;	//v10+ lump flags. Source domain, NOT entity_t.flags -- see Patch 200 below.
	size_t nolightorg = 0;	//props lit at their own origin because they carry no lighting origin

	qbyte *prop, *propbase;
	size_t propsize;		//the MEASURED per-record stride
	size_t propbasesize;	//canonical bytes through the scale, for this version
	size_t propextra;		//the field VBSP inserts at offset 72 on bspver>=21
	sprphdr_t hdr;

	//A BASE, not a stride: "canonical bytes up to and including the scale".
	//The real stride is measured below, because it is not a function of version.
	switch(version)
	{
	case 4:
		propbasesize = 14*4;
		break;
	case 5:	//+fadescale
		propbasesize = 15*4;
		break;
	case 6://+dxlevels
		propbasesize = 16*4;
		break;
	case 7:	//+rgba
	case 8: //-dxlevels+[cg]pulevels
		propbasesize = 17*4;
		break;
	case 9:	//+360
	case 10://-360+flags
		propbasesize = 18*4;
		break;
	case 11://+uniform scale
		propbasesize = 19*4;
		break;
	case 12://CS:GO. UNVERIFIED -- no map in the library carries props at v12, so this
	case 13://assumes Strata's layout. v13 itself IS verified: 19 maps, 9759 props.
		propbasesize = 21*4;	//...68 FlagsEx, 72..83 vec3 Scale.
		break;
	default:
		//Used to `return true` with nothing said, so a whole map's props could go
		//missing and look like a map with no props.
		Con_Printf(CON_WARNING "%s: static prop lump version %i is not supported -- its props will not load\n",
			mod->name, version);
		return true;
	}


	/*
	Try the 16-bit leaf-reference array, then the 32-bit one.  Choosing by version
	alone would repeat the mistake the stride switch made -- the width is
	discoverable from whether the prop block then divides evenly, so discover it.
	Strata writes 32-bit refs; everything before it writes 16-bit.
	*/
	leafrefsize = 2;
	if (!VBSP_ParseSprpHeader(offset, size, leafrefsize, propbasesize, &hdr))
	{
		leafrefsize = 4;
		if (!VBSP_ParseSprpHeader(offset, size, leafrefsize, propbasesize, &hdr))
		{
			Con_Printf(CON_WARNING "%s: static prop lump v%i does not divide into records "
				"(%u bytes, expected a stride of at least %u) -- its props will not load\n",
				mod->name, version, (unsigned)size, (unsigned)propbasesize);
			return true;
		}
	}

	nummodels	= hdr.nummodels;
	modelref	= (void*)hdr.modelref;
	numleafrefs	= hdr.numleafrefs;
	leafref		= hdr.leafref;
	numprops	= hdr.numprops;
	propbase	= hdr.prop;
	propsize	= hdr.stride;
	propextra	= propsize - propbasesize;

	//Worth seeing exactly once per map: it is the difference between "this map has
	//no props" and "this map's props were thrown away", which used to look identical.
	if (propextra || leafrefsize != 2)
		Con_DPrintf("%s: sprp v%i -- %u-byte records (%u canonical + %u inserted), %u-bit leaf refs\n",
			mod->name, version, (unsigned)propsize, (unsigned)propbasesize,
			(unsigned)propextra, (unsigned)(leafrefsize*8));

	prv->staticprops = plugfuncs->GMalloc(&mod->memgroup, sizeof(*prv->staticprops)*numprops);
	for (i = 0, sent = prv->staticprops; i < numprops; i++)
	{
		/*
		Anchor the cursor per record rather than letting the field-by-field parse
		below run on from wherever the last one stopped.  Two things need that: a
		record can be abandoned mid-parse (the dx/cpu/gpu `skip` paths `continue`
		without reading the rest of it), and with an inserted field the fields no
		longer reach the end of the record anyway.  This one line is what actually
		fixes the 18 v10@76 maps.
		*/
		prop = propbase + i*propsize;

		ent = &sent->ent;
		skip = false;
		ent->playerindex = -1;
		ent->topcolour = TOP_DEFAULT;
		ent->bottomcolour = BOTTOM_DEFAULT;
		ent->scale = 1;
		ent->flags = RF_NOSHADOW;
		ent->shaderRGBAf[0] = 1;
		ent->shaderRGBAf[1] = 1;
		ent->shaderRGBAf[2] = 1;
		ent->shaderRGBAf[3] = 1;
		ent->framestate.g[FS_REG].frame[0] = 0;
		ent->framestate.g[FS_REG].lerpweight[0] = 1;


		ent->origin[0] = LittleFloat(*(float*)prop);					prop += sizeof(float);
		ent->origin[1] = LittleFloat(*(float*)prop);					prop += sizeof(float);
		ent->origin[2] = LittleFloat(*(float*)prop);					prop += sizeof(float);
		ent->angles[0] = LittleFloat(*(float*)prop);					prop += sizeof(float);
		ent->angles[1] = LittleFloat(*(float*)prop);					prop += sizeof(float);
		ent->angles[2] = LittleFloat(*(float*)prop);					prop += sizeof(float);
		modelindex = (unsigned short)LittleShort(*(short*)prop);		prop += sizeof(unsigned short);
		leafrefidx = (unsigned short)LittleShort(*(unsigned short*)prop);				prop += sizeof(unsigned short);
		leafcount = LittleShort(*(unsigned short*)prop);				prop += sizeof(unsigned short);
		sent->solid = *prop;											prop += sizeof(qbyte);
		propflags = *prop;												prop += sizeof(qbyte);
		ent->skinnum = LittleLong(*(unsigned int*)prop);				prop += sizeof(unsigned int);
		sent->fademindist = LittleFloat(*(float*)prop);					prop += sizeof(float);
		sent->fademaxdist = LittleFloat(*(float*)prop);					prop += sizeof(float);
		sent->lightorg[0] = LittleFloat(*(float*)prop);					prop += sizeof(float);
		sent->lightorg[1] = LittleFloat(*(float*)prop);					prop += sizeof(float);
		sent->lightorg[2] = LittleFloat(*(float*)prop);					prop += sizeof(float);

		/*
		FTESurf Patch 152 -- THE PALE PROPS, third and last cause.

		StaticPropLump_t::LightingOrigin is only written when the prop carries
		STATIC_PROP_USE_LIGHTING_ORIGIN (0x02), which is the flag byte above
		that this code was throwing away.  Without the flag the field is unused
		and Source lights the prop at its OWN origin -- so reading it
		unconditionally sampled (0,0,0) for every prop that does not set it.

		Measured on surf_666: 653 props, ZERO with the flag set, 333 with an
		all-zero LightingOrigin.  (0,0,0) is nowhere near this map, so
		VBSP_PointLeafnum returned leaf 0 -- the solid leaf, which carries no
		ambient -- for all 653, and every prop fell back to a flat grey.  The
		[texdiag] PROPLIGHT / LEAFAMB census reports exactly that: "0 off-map,
		653 in a leaf with no ambient" against "11505 of 24951 leaves carry
		ambient".  The data was there the whole time; nothing was asking for it.

		The zero test is kept as well as the flag test: a prop that sets the flag
		and then stores the origin of the coordinate system is asking for the
		same wrong answer, and there is no map where (0,0,0) is a meaningful
		place to sample light from.
		*/
		/*
		FTESurf Patch 257 -- and only if it is a POINT ON THE MAP.

		The zero test above is the shape this field is usually wrong in.
		surf_garden is wrong in a different one: 57 of its 429 props set the
		0x02 flag over a lighting origin that is uninitialised VBSP stack
		memory -- (-1.778, -2.397e34, 2.164), (0, nan, -20.73),
		(nan, nan, nan), denormals around 1e-40, and (-0.0006571, -1.542, 0),
		which is finite and non-zero and still nowhere near a map whose world
		box is x[-16256,-416] y[192,16032].

		The flag byte is uninitialised along WITH it: those records carry
		0x3F/0x7F/0xBA/0xBE/0xBF/0xFF against a sane majority of
		0x00/0x40/0x10, and the garbage flags and the garbage vectors are the
		same 57 records.  So 0x02 is necessary and not sufficient on this
		content, and the sufficient half is cheap: a lighting origin is only
		usable if it is somewhere in the map.

		cmodels[0] is worldspawn's own box and VBSP_LoadSubmodels has already
		filled it in -- it runs at :6965 and the game lump at :6989.  mod->mins
		is NOT set until :7202, after this, which is why the box comes from
		here.  The fallback is VBSP's own coordinate limit, so a map that
		somehow reaches here without submodels still rejects the impossible
		values.

		Written as a containment test rather than an isfinite() call on
		purpose: NaN fails every ordered comparison and both infinities fall
		outside any finite box, so one test rejects all three shapes.
		*/
		{
			qboolean usable = (propflags & 0x02) &&
							  (sent->lightorg[0] || sent->lightorg[1] || sent->lightorg[2]);
			if (usable)
			{
				vec3_t wmin, wmax;
				int a;
				if (prv->cmodels && prv->numcmodels)
				{
					VectorCopy(prv->cmodels[0].mins, wmin);
					VectorCopy(prv->cmodels[0].maxs, wmax);
				}
				else
				{
					VectorSet(wmin, -16384, -16384, -16384);
					VectorSet(wmax,  16384,  16384,  16384);
				}
				for (a = 0; a < 3; a++)
					if (!(sent->lightorg[a] >= wmin[a] && sent->lightorg[a] <= wmax[a]))
					{
						usable = false;
						break;
					}
			}
			if (!usable)
			{
				VectorCopy(ent->origin, sent->lightorg);
				nolightorg++;
			}
		}
		if (version >= 5)
		{
			/*ent->fadescale = LittleFloat(*(float*)prop);*/			prop += sizeof(float);
		}
		/*
		FTESurf Patch 257 -- 0 MEANS UNSET, AND THESE FOUR BYTES ARE NOT ALWAYS
		CPU/GPU LEVELS.

		The old v8+ test was `prop[0] > cpulevel || cpulevel > prop[1]` with
		cpulevel hardcoded 0.  The second half can never fire against an
		unsigned byte, so the whole thing reduced to "drop every prop whose
		MinCPULevel is non-zero" -- with no zero-guard, though the v6 branch
		right below has always had one.  Source's own test is per-bound and
		1-based (`m_nMinCPULevel > 0 && m_nMinCPULevel-1 > nCPULevel`); 0 there
		means the mapper set no bound at all.

		CENSUS, all 1310 maps: only 564 carry a v8+ sprp, and only THREE of
		those write a non-zero byte here at all --

		    what_is_this    256 props (90,0,0,0), 39 (95,0,0,0), 4 (0,0,90,0)
		    surf_shoria       4 props (95,0,0,0)
		    surf_classics3    2 props (0,1,0,1)

		-- so 299 props on what_is_this and 4 on surf_shoria have been dropped
		outright, and that is the whole population.

		90 and 95 ARE NOT CPU LEVELS.  Source's cpu_level runs 0-2 and gpu_level
		0-3; 60/70/80/81/90/95 are dxlevels.  Those two maps carry the v6
		MinDXLevel/MaxDXLevel ushort pair in the slot a v10 header says holds
		four level bytes, and reading them as CPU levels is what dropped the
		props.  surf_classics3's (0,1,0,1) is the genuine CPU/GPU convention.
		Both shapes exist in the same lump version, so the byte magnitude is
		what tells them apart -- nothing below 16 is a dxlevel and nothing at or
		above it is a cpu/gpu level.

		Each bound is then applied only if it was set, which is the fix in both
		branches.  On what_is_this the 295 (90,0,…)/(95,0,…) props come back
		(dxlevel 95 clears a floor of 90 or 95 and there is no ceiling), and the
		4 that ask for a dx9.0 ceiling stay dropped -- Source skips those at
		dx95 too, they are the low-hardware stand-ins.
		*/
		if (version >= 8)
		{
			unsigned int b0 = prop[0], b1 = prop[1], b2 = prop[2], b3 = prop[3];
			prop += sizeof(qbyte)*4;
			if (b0 >= 16 || b1 >= 16 || b2 >= 16 || b3 >= 16)
			{	//really the v6 dxlevel pair, written into the v8+ slot
				unsigned int minlev = b0 | (b1<<8);
				unsigned int maxlev = b2 | (b3<<8);
				skip |= (minlev > 0 && (unsigned)dxlevel < minlev);
				skip |= (maxlev > 0 && (unsigned)dxlevel > maxlev);
			}
			else
			{	//cpu levels then gpu levels, 1-based, 0 = no bound
				skip |= (b0 > 0 && b0-1 > (unsigned)cpulevel);
				skip |= (b1 > 0 && b1-1 < (unsigned)cpulevel);
				skip |= (b2 > 0 && b2-1 > (unsigned)gpulevel);
				skip |= (b3 > 0 && b3-1 < (unsigned)gpulevel);
			}
		}
		else if (version >= 6)
		{
			unsigned short minlev, maxlev;
			minlev = LittleShort(*(unsigned short*)prop);				prop += sizeof(unsigned short);
			maxlev = LittleShort(*(unsigned short*)prop);				prop += sizeof(unsigned short);
			skip |= (minlev > 0 && (unsigned)dxlevel < minlev);
			skip |= (maxlev > 0 && (unsigned)dxlevel > maxlev);
		}
		if (version >= 7)
		{
			/*
			FTESurf Patch 163: only when it was actually written.

			m_DiffuseModulation is a per-prop RGBA tint, and it reads
			(0, 1, 0, 0) on 620 of surf_666's 653 props -- a constant, not a
			modulation, because CS:S's VBSP does not populate this field.
			Applied literally it multiplies the prop by black at zero alpha.
			Nothing downstream currently reads shaderRGBAf for these, which is
			the only reason it has never shown; that is luck, not a design.

			Alpha zero is the tell: a modulation that makes the prop invisible
			is not one the mapper asked for.
			*/
			if (prop[3])
				VectorScale(prop, 1/255.0, ent->shaderRGBAf);
			prop += sizeof(qbyte)*4;
		}

		if (version == 9)
		{
			/*disablex360 = LittleLong(*(int*)prop);*/					prop += sizeof(int);
		}
		if (version >= 10)
		{
			/*
			FTESurf Patch 200 -- THE PROPS THAT SWING AROUND THE PLAYER.

			This line used to be `ent->flags = LittleLong(...)`, blitting the
			static-prop lump's flags dword straight into an engine entity_t's
			`flags` -- which is not a Source flag word at all, it is FTE's RF_*
			RENDER flag bitfield (protocol.h:1640-1682).  Two unrelated flag
			domains, no translation, no mask.

			The bit that hurts is 1<<21.  In FTE that is Q2EXRF_FLARE, aliased
			as RF_XFLIP (protocol.h:1670,1682), and GLBE_SelectEntity reacts to
			it (gl_backend.c:4716-4735) by swapping the entity onto
			r_refdef.m_projection_view -- the VIEWMODEL projection, built from
			the gun FOV and depth-crunched into the near third at
			gl_rmain.c:635-640 -- then negating projection X and flipping the
			cull winding.  R_RotateForEntity still places the prop in world
			space (only RF_WEAPONMODEL takes the view-space branch), so the
			result is a prop anchored in the world but drawn through the gun's
			camera with a mirrored X: it floats over everything and appears to
			orbit the player as they turn.  Reported on surf_rise and
			surf_gigapede, and it is the same props every load because the value
			comes out of the BSP.

			surf_gigapede has five props and exactly one with a nonzero flag
			word: gigapederamps/ramp2/ramp2.mdl at 0x200020, which is
			RF_TRANSLUCENT|RF_XFLIP.  That is the ramp.  surf_rise has five at
			0x200020 and 46 trees/rocks at 0x800080 (Q2RF_BEAM|RF_NOSHADOWRECV).
			Library-wide the blit lands a nonzero RF_ word on 759 prop instances
			across 41 maps.

			The second bug on the same line was quieter and hit EVERY prop of
			every v10+ map: it was `=`, not `|=`, so it destroyed the
			RF_NOSHADOW set at the top of this loop.

			Source's flags are not FTE's, and nothing in this word has an RF_
			equivalent worth translating -- STATIC_PROP_FLAG_FADES and friends
			are handled by the fademin/fademax fields above.  So it is read into
			a local and dropped, and the comment records what it is rather than
			the code pretending to use it.
			*/
			propflagsex = LittleLong(*(unsigned int*)prop);				prop += sizeof(unsigned int);
			(void)propflagsex;
		}
		/*
		FTESurf Patch 228 -- the inserted field, and it belongs HERE: after FlagsEx
		and before the scale.  Measured zero in all 32,127 records across the 82
		maps that carry it, so it is stepped over rather than read.  On the 502
		canonical v10@72 maps propextra is 0 and this line does nothing, which is
		the property that keeps those maps byte-identical to before.
		*/
		prop += propextra;

		if (version >= 12)
		{	//Strata: non-uniform Vector scale (engine entity_t has a single scale, so take X)
			ent->scale = LittleFloat(*(float*)prop);					prop += sizeof(float)*3;
		}
		else if (version >= 11)
		{
			ent->scale = LittleFloat(*(float*)prop);					prop += sizeof(float);
		}

		/*
		Zero is exactly what the pre-228 v13 path produced: it read the scale from
		offset 72 -- the inserted field -- so every Strata prop in the library
		loaded at scale 0.  An unreadable scale is 1, never invisible.

		KNOWN GAP, not introduced here: bihtransform_s (com_bih.h:94-98) carries
		axis and origin but no scale, and VBSP_BuildBIHMain bounds each leaf as
		origin +/- model->radius, also unscaled.  So a scaled static prop now
		RENDERS at its true size and still COLLIDES at 1.0.  That affects ~3.5k of
		the 32k props this patch recovers; the rest are exactly 1.0.  Fixing it
		means threading a scale through BIH_RecursiveTrace, which is its own job.
		*/
		if (ent->scale <= 0 || ent->scale > 64)
			ent->scale = 1;

		//okay, we parsed the prop data now...
		skip |= modelindex >= nummodels;
		if (skip)
			continue;	//we're ignoring it for some reason

#ifdef HAVE_CLIENT
		/*
		FTESurf Patch 163.  Keyed on `i`, the LUMP index -- not on the compacted
		index sent-staticprops.  VRAD numbers sp_N.vhv by position in the lump
		and this loop skips props the dx/cpu/gpu levels rule out, so the two
		stop agreeing at the first skipped prop and every prop after it would be
		lit with its neighbour's bake.
		*/
		sent->hasbaked = false;
		sent->vcraw = NULL;			//FTESurf Patch 259
		sent->vcrawverts = 0;
		sent->vc = NULL;
		sent->vcverts = 0;
		sent->vcgain = 1;
		//The engine's own per-vertex path (RGBPROPLIGHT) never runs for these --
		//R_CalcModelLighting returns early on light_known, which HL2_CalcModelLighting
		//always sets -- so nothing else will ever clear this pointer.  Say so here.
		ent->vertlightcolors = NULL;
		ent->vertlightbytes = NULL;
		ent->vertlightverts = 0;
		if (qrenderer != QR_NONE && hl2_lt_baked && hl2_lt_baked->ival)
		{
			if (VBSP_LoadPropBakedLight(mod, sent, (unsigned int)i, hl2_lt_baked->ival >= 2))
			{
				prv->lit_baked++;
				if (sent->vcraw)
				{
					prv->lit_vc++;			//FTESurf Patch 259
					prv->lit_vcverts += sent->vcrawverts;
				}
			}
		}
#endif

		ent->model = modfuncs->BeginSubmodelLoad(modelref[modelindex].name);
		modfuncs->AngleVectors(ent->angles, ent->axis[0], ent->axis[1], ent->axis[2]);
		VectorNegate(ent->axis[1],ent->axis[1]);

		//transforms, in case we need to recursively walk its bih
		VectorCopy(ent->axis[0], prv->staticprops[i].transform.axis[0]);
		VectorCopy(ent->axis[1], prv->staticprops[i].transform.axis[1]);
		VectorCopy(ent->axis[2], prv->staticprops[i].transform.axis[2]);
		VectorCopy(ent->origin, prv->staticprops[i].transform.origin);

		//Hack: special value to flag it for linking once we've loaded its model. this needs to go when we make them solid.
		//The second test is new with Patch 228: a record whose leaf range runs past
		//the end of the lump's own array would otherwise read whatever follows it.
		//It degrades to the same -2 path as an overflow, which recomputes the leaf
		//set from the bounding box once the model is up -- a safe answer, not a guess.
		if (leafcount > countof(ent->pvscache.leafnums) ||
			leafrefidx + leafcount > numleafrefs)
			ent->pvscache.num_leafs = -2;	//overflow. calculate it later once its loaded.
		else
		{
			ent->pvscache.areanum = -1;	//FIXME
			ent->pvscache.areanum2 = -1;
			ent->pvscache.num_leafs = leafcount;	//overflow. calculate it later once its loaded.
			for (l = 0; l < leafcount; l++)
			{
				mleaf_t *lf = mod->leafs + VBSP_SprpLeafRef(leafref, leafrefsize, leafrefidx++);
				ent->pvscache.leafnums[l]/*actually clusters*/ = lf->cluster;
				//and try to track the areas too. not quite so reliable...
				if (ent->pvscache.areanum == -1)
					ent->pvscache.areanum = lf->area;
				else
					ent->pvscache.areanum2 = lf->area;
			}
		}
		//Hack: lighting is wrong.
//		ent->light_type = ELT_UNKNOWN;
#if 0
		VectorSet(ent->light_dir, 0, 0.707, 0.707);
		VectorSet(ent->light_avg, 0.75, 0.75, 0.75);
		VectorSet(ent->light_range, 0.5, 0.5, 0.5);
#endif

		//not all props will be emitted, according to d3d levels...
		prv->numstaticprops++;
		sent++;
	}

	if (nolightorg)
		Con_DPrintf("%s: %u of %u static props carry no lighting origin -- lit at their own\n",
			mod->name, (unsigned)nolightorg, (unsigned)prv->numstaticprops);
	//FTESurf Patch 163.  A real print, matching the props/water/decals lines:
	//the gap between the two numbers is the only signal that a map's props are
	//falling back to the ambient cube, and that is the case worth seeing.
	if (prv->numstaticprops)
		Con_Printf("props: %u of %u lit from VRAD's baked vertex lighting\n",
			(unsigned)prv->lit_baked, (unsigned)prv->numstaticprops);
	//FTESurf Patch 259.  Says the mode rather than leaving it to be inferred, and
	//states the price in the same line: this is the only allocation in the plugin
	//that scales with the map's total prop VERTEX count rather than its prop count.
	//This counts what was READ.  `prop_census` counts what a frame actually DREW
	//with, which is the number that says the feature is on.
	if (prv->lit_vc)
		Con_Printf("props: %u of those kept per-vertex (%.1f KiB, %u verts)\n",
			(unsigned)prv->lit_vc, prv->lit_vcverts*4/1024.0, (unsigned)prv->lit_vcverts);

	return true;
}
//malloc/free ISzAlloc for the LZMA one-call decoder, shared by the game-lump sub-lump decompressor
//(just below) and the top-level lump decompressor (VBSP_DecompressLumps, further down).
static void *VBSP_LzmaAlloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void  VBSP_LzmaFree (ISzAllocPtr p, void *a)     { (void)p; free(a); }
static const ISzAlloc vbsp_lzma_alloc = { VBSP_LzmaAlloc, VBSP_LzmaFree };

//A game-lump SUB-lump can be individually LZMA-compressed (flags bit 0), which Strata (and lzma-packed
//classic maps like surf_angst) do for their static props.  Framing is the same Source 17-byte 'LZMA'
//header VBSP_DecompressLumps handles.  Returns a model-lifetime plain buffer (+ size via *outsize), or
//NULL on failure.  This is separate from VBSP_DecompressLumps, which only touches the 64 TOP-level lumps.
static qbyte *VBSP_DecompressGameSubLump(model_t *mod, qbyte *d, size_t complen, size_t *outsize)
{
	unsigned int actual, lzmasize;
	SizeT destLen, srcLen;
	ELzmaStatus status;
	SRes res;
	qbyte *out;
	if (complen < 17 || !(d[0]=='L' && d[1]=='Z' && d[2]=='M' && d[3]=='A'))
		return NULL;
	actual   = (unsigned)d[4] | ((unsigned)d[5]<<8) | ((unsigned)d[6]<<16) | ((unsigned)d[7]<<24);
	lzmasize = (unsigned)d[8] | ((unsigned)d[9]<<8) | ((unsigned)d[10]<<16) | ((unsigned)d[11]<<24);
	if ((size_t)17 + lzmasize > complen)
		return NULL;
	out = plugfuncs->GMalloc(&mod->memgroup, actual?actual:1);
	destLen = actual;
	srcLen  = lzmasize;
	res = LzmaDecode(out, &destLen, d+17, &srcLen, d+12, 5, LZMA_FINISH_END, &status, &vbsp_lzma_alloc);
	if (res != SZ_OK || destLen != actual)
		return NULL;
	*outsize = actual;
	return out;
}
/*
FTESurf Patch 231: a game-lump id printed as four raw %c's can truncate its own line.

Con_Printf in a plugin formats into a buffer and hands the result to plugfuncs->Print
as a NUL-terminated C string (plugins/plugin.c:221-231).  So a %c whose argument is
zero does not print a zero byte -- it ENDS THE STRING, and everything after it in the
format is lost: the remaining id bytes, the version, and the trailing newline.  The
next console line then continues on the same row, which is why a load reads

    Unsupported gamelump id/version maps/surf_aesthetic.bsp: 1 material(s) did not resolve:

with two unrelated messages welded together.  Four characters, always printable.
*/
static const char *VBSP_GameLumpTag(unsigned int id, char *out5)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		int c = (id >> (24-i*8)) & 0xff;
		out5[i] = (c >= 32 && c < 127) ? (char)c : '.';
	}
	out5[4] = 0;
	return out5;
}

static qboolean VBSP_LoadGameLump(model_t *mod, qbyte *mod_base, vlump_t *l)
{
	size_t i;
	char tag[5];
	hlgamelumpheader_t *blob = (void*)(mod_base + l->fileofs);
	if (!l->filelen)
		return true; //missing
	if (l->filelen < sizeof(*blob) + sizeof(*blob->sl)*(blob->count-1))
		return false;	//not even enough space for the header...
	for (i = 0; i < blob->count; i++)
	{
		qbyte *sub = mod_base+blob->sl[i].ofs/*sigh - absolute file offset*/;
		size_t sublen = blob->sl[i].len;
		//FTESurf Patch 231: the table is terminated by a zero entry, whose only job is
		//to bound the length of the sub-lump before it.  Both surf_aesthetic and
		//surf_demise carry one, and every map I dumped does.  It is not an unsupported
		//game lump and reporting it as one was pure noise -- see VBSP_GameLumpTag.
		if (!blob->sl[i].id && !blob->sl[i].len)
			continue;
		if (blob->sl[i].flags & 1)	//LZMA-compressed sub-lump (Strata / lzma-packed maps)
		{
			if (sublen < 17)
				continue;	//too small for even a Source 'LZMA' header - a degenerate/empty sub-lump (e.g. Strata writes a 12-byte stub for an empty sprp). nothing to load.
			sub = VBSP_DecompressGameSubLump(mod, sub, sublen, &sublen);
			if (!sub)
			{
				Con_Printf(CON_ERROR "VBSP: gamelump %s LZMA decode failed in %s\n", VBSP_GameLumpTag(blob->sl[i].id, tag), mod->name);
				continue;
			}
		}
#define LUMPTYPE(a,b,c,d, minver, maxver) (blob->sl[i].id == (((qbyte)a<<24)|((qbyte)b<<16)|((qbyte)c<<8)|((qbyte)d<<0)) && blob->sl[i].version >= minver && blob->sl[i].version <= maxver)
		if (LUMPTYPE('s','p','r','p', 4,13))	//static props (placed by mapper); v11/v13 = Strata
			VBSP_LoadStaticProps(mod, sub, sublen, blob->sl[i].version);
		else if (LUMPTYPE('d','p','r','p', 4,4))	//dynamic props (generated by textures)
			;
		else if (LUMPTYPE('d','p','l','t', 0,0))	//detail prop ldr lighting
			;
		else if (LUMPTYPE('d','p','l','h', 0,0))	//detail prop hdr lighting
			;
		else
			Con_Printf("Unsupported gamelump id/version %s (0x%08x) v%i in %s\n", VBSP_GameLumpTag(blob->sl[i].id, tag), (unsigned int)blob->sl[i].id, blob->sl[i].version, mod->name);
#undef LUMPTYPE
	}
	return true;
}

#ifdef HAVE_CLIENT
static void VBSP_GenerateMaterials(void *ctx, void *data, size_t a, size_t b)
{
	model_t *mod = ctx;
	const char *script;

	if (!a)
	{	//submodels share textures, so only do this if 'a' is 0 (inline index, 0 = world).
		for(a = 0; a < mod->numtextures; a++)
		{
			script = NULL;
			if (!strncmp(mod->textures[a]->name, "sky/", 4))
				script =
					"{\n"
						"sort sky\n"
						"surfaceparm nodlight\n"
						"skyparms - - -\n"
					"}\n";
			mod->textures[a]->shader = modfuncs->RegisterBasicShader(mod, mod->textures[a]->name, SUF_LIGHTMAP, script, PTI_INVALID, 0, 0, NULL, NULL);

			/*
			FTESurf build 11: WHICH MATERIALS ARE NOT LIGHTMAPPED.

			SHADER_HASLIGHTMAP decides whether Mod_LightmapAllocSurf gives this
			material's surfaces a lightmap at all (gl_model.c:3924).  A material
			whose VMT compiled to a shader without one draws at FULL TEXTURE
			BRIGHTNESS -- pale and flat, against a world the lightmap has
			darkened and tinted.  That is indistinguishable from the outside
			from a lightmap that loaded wrong, and the two want opposite fixes.

			Gated on r_texdiag rather than developer, which cfg/default.cfg
			resets to 0 after the command line is applied -- the same reasoning
			as the Q1 texture report this mirrors (gl_model.c:1895-1912).
			*/
			if (hl2_texdiag && hl2_texdiag->ival)
			{
				shader_t *sh = mod->textures[a]->shader;
				if (!sh || !(sh->flags & SHADER_HASLIGHTMAP))
					Con_Printf("[texdiag] VMT NO LIGHTMAP %-34s shader=%s\n",
						mod->textures[a]->name, sh?sh->name:"(none)");
			}
		}

		/*
		FTESurf Patch 233: THE UNDERSIDE OF THE WATER, DRAWN AS THE WATER.

		A water VMT's $bottommaterial is not a property of the water as far as the BSP
		is concerned -- VBSP copies the string into the texdata string table and gives
		the underwater faces a texinfo pointing at it, so it arrives here as an ordinary
		world material and goes missing like any other one the mapper did not ship.
		Two of the three maps reported were exactly this, and both are drawn: 325 faces
		on surf_aesthetic, 7 on surf_demise, all SURF_WARP, none SURF_NODRAW.

		mat_vmt.c kept the link this time (VMT_BottomRecord), so the material that
		DEFINES the underside is available by name.  Registering the parent's name here
		hands this texture the SAME shader object the water surfaces got -- RegisterBasicShader
		dedups on name, and both texinfos carry SURF_WARP so the #WARP argument matches on
		both sides.  No new shader path, and the result is what $bottommaterial means.

		A SECOND PASS rather than doing it inline above, because inline would depend on the
		parent's texdata entry preceding the bottom material's.  It does on both maps -- VBSP
		appends bottom materials last, index 54 of 55 and 25 of 26 -- but nothing guarantees
		it, and this walks a list that is 26 to 55 entries long.

		Gated on the census, not on the table: a map that DOES ship its bottom material has
		a link here too, and must keep the material it shipped.
		*/
		if (!hl2_bottommaterial || hl2_bottommaterial->ival)
		{
			size_t t;
			for (t = 0; t < mod->numtextures; t++)
			{
				char base[MAX_QPATH], stem[MAX_QPATH], want[MAX_QPATH];
				const char *parent, *hash, *miss = NULL;
				shader_t *sh;
				size_t l;

				hash = strchr(mod->textures[t]->name, '#');
				l = hash ? (size_t)(hash - mod->textures[t]->name) : strlen(mod->textures[t]->name);
				if (l >= sizeof(base))
					continue;
				memcpy(base, mod->textures[t]->name, l);
				base[l] = 0;

				parent = Mat_VMT_BottomParent(base);
				if (!parent)
					continue;

				/*
				The census does not necessarily hold the name the texdata table holds.  P231
				stopped counting names that carry a foreign extension, because R_LoadShader
				probes such a name twice and only the stripped probe can succeed -- so
				surf_demise's "...water_tar_beneath.vmf" is recorded as "...water_tar_beneath".
				Ask for both spellings, and forget whichever one is actually there.
				*/
				if (Mat_VMT_CensusMissing(base))
					miss = base;
				else
				{
					char *dot, *sl;
					Q_strlcpy(stem, base, sizeof(stem));
					sl = strrchr(stem, '/');
					dot = strrchr(sl?sl:stem, '.');
					if (dot && Q_strcasecmp(dot, ".vmt"))
					{
						*dot = 0;
						if (Mat_VMT_CensusMissing(stem))
							miss = stem;
					}
				}
				if (!miss)
					continue;	//the map shipped its bottom material after all; keep what it shipped

				Q_snprintfz(want, sizeof(want), "%s%s", parent, hash?hash:"");
				sh = modfuncs->RegisterBasicShader(mod, want, SUF_LIGHTMAP, NULL, PTI_INVALID, 0, 0, NULL, NULL);
				if (!sh)
					continue;	//nothing forgotten: the warning must still name it
				mod->textures[t]->shader = sh;
				Mat_VMT_CensusForget(miss);
				Con_DPrintf("%s: %s is the underside of %s; drawing it as that\n",
					mod->name, base, parent);
			}
		}
	}
	modfuncs->Batches_Build(mod, data);
	if (data)
		plugfuncs->Free(data);
}
#endif

/*
==================
CM_InlineModel
==================
*/
static cmodel_t	*VBSP_InlineModel (model_t *model, char *name)
{
	vbspinfo_t	*prv = (vbspinfo_t*)model->meshinfo;
	int		num;

	if (!name)
		Host_Error("Bad model\n");
	else if (name[0] != '*')
		Host_Error("Bad model\n");

	num = atoi (name+1);

	if (num < 1 || num >= prv->numcmodels)
		Host_Error ("CM_InlineModel: bad number");

	return &prv->cmodels[num];
}

static int VBSP_NumInlineModels (model_t *model)
{
	vbspinfo_t	*prv = (vbspinfo_t*)model->meshinfo;
	return prv->numcmodels;
}

static int VBSP_LeafContents (model_t *model, int leafnum)
{
	if (leafnum < 0 || leafnum >= model->numleafs)
		Host_Error ("CM_LeafContents: bad number");
	return model->leafs[leafnum].contents;
}

static int VBSP_LeafCluster (model_t *model, int leafnum)
{
	if (leafnum < 0 || leafnum >= model->numleafs)
		Host_Error ("CM_LeafCluster: bad number");
	return model->leafs[leafnum].cluster;
}

static int VBSP_LeafArea (model_t *model, int leafnum)
{
	if (leafnum < 0 || leafnum >= model->numleafs)
		Host_Error ("CM_LeafArea: bad number");
	return model->leafs[leafnum].area;
}

//=======================================================================

/*
==================
CM_PointLeafnum_r

==================
*/
static int VBSP_PointLeafnum_r (model_t *mod, const vec3_t p, int num)
{
	float		d;
	mnode_t		*node;
	mplane_t	*plane;

	while (num >= 0)
	{
		node = mod->nodes + num;
		plane = node->plane;

		if (plane->type < 3)
			d = p[plane->type] - plane->dist;
		else
			d = DotProduct (plane->normal, p) - plane->dist;
		if (d < 0)
			num = node->childnum[1];
		else
			num = node->childnum[0];
	}

	return -1 - num;
}

static int VBSP_PointLeafnum (model_t *mod, const vec3_t p)
{
	if (mod->loadstate != MLS_LOADED)
		return 0;		// sound may call this without map loaded
	return VBSP_PointLeafnum_r (mod, p, 0);
}

/*
FTESurf Patch 152: does this point sit in a leaf that carries an ambient cube.

"Is there lighting data here" is a different question from "is it dark here",
and the prop lighting path needs the first one -- see the note at its call site.
Returns false before the model is loaded, because PointLeafnum answers 0 (the
solid leaf) unconditionally until then and a caller would be told "no ambient"
about the whole map.
*/
static qboolean VBSP_LeafHasAmbient (model_t *mod, const vec3_t p)
{
	vbspinfo_t *prv = (vbspinfo_t*)mod->meshinfo;
	int leafnum;
	if (!prv || !prv->leaflight || mod->loadstate != MLS_LOADED)
		return false;
	leafnum = VBSP_PointLeafnum(mod, p);
	if (leafnum <= 0 || leafnum >= (int)mod->numleafs)
		return false;
	return prv->leaflight[leafnum].count != 0;
}

static int VBSP_PointCluster (model_t *mod, const vec3_t p, int *area)
{
	int leaf;
	if (mod->loadstate != MLS_LOADED)
		return 0;		// sound may call this without map loaded

	leaf = VBSP_PointLeafnum_r (mod, p, 0);
	if (area)
		*area = VBSP_LeafArea(mod, leaf);
	return VBSP_LeafCluster(mod, leaf);
}

static unsigned int VBSP_PointContents (model_t *mod, const vec3_t axis[3], const vec3_t p)
{
	vec3_t np;
	if (mod->loadstate != MLS_LOADED)
		return 0;

	if (axis)
	{
		np[0] = DotProduct(p, axis[0]);
		np[1] = DotProduct(p, axis[1]);
		np[2] = DotProduct(p, axis[2]);
		p = np;
	}

	return VBSP_LeafContents(mod, VBSP_PointLeafnum_r (mod, p, 0));
}

/*
=============
CM_BoxLeafnums

Fills in a list of all the leafs touched
=============
*/
static int		leaf_count, leaf_maxcount;
static int		*leaf_list;
static const float	*leaf_mins, *leaf_maxs;
static int		leaf_topnode;

static void VBSP_BoxLeafnums_r (model_t *mod, int nodenum)
{
	mplane_t	*plane;
	mnode_t		*node;
	int		s;

	while (1)
	{
		if (nodenum < 0)
		{
			if (leaf_count >= leaf_maxcount)
				return;
			leaf_list[leaf_count++] = -1 - nodenum;
			return;
		}

		node = &mod->nodes[nodenum];
		plane = node->plane;
//		s = BoxOnPlaneSide (leaf_mins, leaf_maxs, plane);
		s = BOX_ON_PLANE_SIDE(leaf_mins, leaf_maxs, plane);
		if (s == 1)
			nodenum = node->childnum[0];
		else if (s == 2)
			nodenum = node->childnum[1];
		else
		{	// go down both
			if (leaf_topnode == -1)
				leaf_topnode = nodenum;
			VBSP_BoxLeafnums_r (mod, node->childnum[0]);
			nodenum = node->childnum[1];
		}

	}
}

static int	VBSP_BoxLeafnums_headnode (model_t *mod, const vec3_t mins, const vec3_t maxs, int *list, int listsize, int headnode, int *topnode)
{
	leaf_list = list;
	leaf_count = 0;
	leaf_maxcount = listsize;
	leaf_mins = mins;
	leaf_maxs = maxs;

	leaf_topnode = -1;

	VBSP_BoxLeafnums_r (mod, headnode);

	if (topnode)
		*topnode = leaf_topnode;

	return leaf_count;
}

static int	VBSP_BoxLeafnums (model_t *mod, const vec3_t mins, const vec3_t maxs, int *list, int listsize, int *topnode)
{
	return VBSP_BoxLeafnums_headnode (mod, mins, maxs, list,
		listsize, mod->hulls[0].firstclipnode, topnode);
}

static void VBSP_FindTouchedLeafs(model_t *model, struct pvscache_s *ent, const float *mins, const float *maxs)
{
#define MAX_TOTAL_ENT_LEAFS		128
	int			leafs[MAX_TOTAL_ENT_LEAFS];
	int			clusters[MAX_TOTAL_ENT_LEAFS];
	int num_leafs;
	int			topnode;
	int i, j;
	int			area;
	unsigned int nareas = 0;	//FTESurf Patch 257: distinct REAL areas this box touched
	/*
	FTESurf Patch 257 -- THE SENTINEL WAS THE WRONG ONE, AND IT ONLY SHOWS ON AN
	ENTITY BIG ENOUGH TO REACH OUTSIDE THE MAP.

	This was `int nullarea = -1;` while VBSP_EdictInFatPVS -- the only consumer of
	what this function writes -- uses `int nullarea = 0;`.  Producer and consumer
	disagreed about which value means "no area", so area 0 was recorded here as a
	real area and then read there as "entity is fully outside the world, invisible
	to all".

	FTE's own Q2 BSP code does not have this bug, and the diff says why: it derives
	the sentinel from the format in BOTH functions --

	    engine/common/gl_q2bsp.c:7232  int nullarea = (mod->fromgame == fg_quake2)?0:-1;
	    engine/common/gl_q2bsp.c:7282  int nullarea = (model->fromgame == fg_quake2)?0:-1;

	-- so the two can never drift apart.  This plugin forked both functions and
	hardcoded each side to a different constant.  VBSP is Quake2-derived (areas,
	areaportals and clusters are all Q2 semantics, and VBSP numbers real areas from
	1 with 0 reserved for solid), so 0 is correct on both sides.

	MEASURED, surf_garden prop 170 (models/surfgarden/s3hedge2.mdl), the missing
	stage-3 hedge, via `prop_census 170`:

	    verdict last frame : PVS culled -- rejected by the area walk
	    area               : areanum 0, areanum2 9, headnode 0

	The query box is origin +/- model->radius = +/-2451 around (-12656, 6640, 2984),
	so it reaches z 533 while the world floor is z 2304.  It therefore touched solid
	leaves OUTSIDE the map, whose area is 0.  That 0 took slot 1, an unrelated
	distant region took slot 2 as area 9, and the area the prop actually occupies
	never got a slot at all -- so the walk found nothing connected to the viewer and
	culled it.  headnode 0 is the tree root, i.e. the headnode test would have
	passed; the area walk killed it first.

	Only two props on that map reach here (leafcount 43 and 68 against
	MAX_ENT_LEAFS 32), which is exactly the reported symptom: the hedge is missing
	on stages 3 and 10 and present on every other stage.

	Skipping area 0 can only REMOVE spurious culls, never add them: an entity that
	touches no real area at all still ends with areanum == areanum2 == 0, and the
	consumer rejects that identically to the old (0, -1) pair.
	*/
	int nullarea = 0;

	//ent->num_leafs == q2's ent->num_clusters
	ent->num_leafs = 0;
	ent->areanum = nullarea;
	ent->areanum2 = nullarea;

	if (!mins || !maxs)
		return;

	//get all leafs, including solids
	num_leafs = VBSP_BoxLeafnums (model, mins, maxs,
		leafs, MAX_TOTAL_ENT_LEAFS, &topnode);

	// set areas
	for (i=0 ; i<num_leafs ; i++)
	{
		clusters[i] = VBSP_LeafCluster (model, leafs[i]);
		area = VBSP_LeafArea (model, leafs[i]);
		if (area != nullarea)
		{	// doors may legally straggle two areas,
			// but nothing should ever need more than that
			if (ent->areanum != nullarea && ent->areanum != area)
			{
				if (ent->areanum2 != area)
					nareas++;		//FTESurf Patch 257, see below
				ent->areanum2 = area;
			}
			else
			{
				if (ent->areanum != area)
					nareas++;
				ent->areanum = area;
			}
		}
	}
	/*
	FTESurf Patch 257: how many distinct real areas this box actually spanned.

	Two slots is Quake2's assumption -- "doors may legally straddle two areas, but
	nothing should ever need more than that" -- and it holds for a door.  It does
	not obviously hold for a static prop queried with an origin +/- radius box,
	which for surf_garden's stage-3 hedge is 4902 units on a side against a model
	that is 1998 x 3308 x 769.  If a box spans three or more areas the record is
	lossy and the entity can still be wrongly culled from the third.

	Published rather than acted on: the sentinel fix above is the measured defect,
	and this is the instrument that says whether it was sufficient.  `prop_census`
	prints it, so a prop that is STILL culled by the area walk will say so with the
	count that explains why.  Nothing is decided on a guess.
	*/
	vbsp_lasttouch_nareas = nareas;

	if (num_leafs >= MAX_TOTAL_ENT_LEAFS)
	{	// assume we missed some leafs, and mark by headnode
		ent->num_leafs = -1;
		ent->headnode = topnode;
	}
	else
	{
		ent->num_leafs = 0;
		for (i=0 ; i<num_leafs ; i++)
		{
			if (clusters[i] == -1)
				continue;		// not a visible leaf
			for (j=0 ; j<i ; j++)
				if (clusters[j] == clusters[i])
					break;
			if (j == i)
			{
				if (ent->num_leafs == MAX_ENT_LEAFS)
				{	// assume we missed some leafs, and mark by headnode
					ent->num_leafs = -1;
					ent->headnode = topnode;
					break;
				}

				ent->leafnums[ent->num_leafs++] = clusters[i];
			}
		}
	}
}

/*
===============================================================================

BOX TRACING

===============================================================================
*/

/*
==================
VBSP_AddBrushBevels			FTESurf Patch 318

The brush edge bevels the map was compiled without, and that BIH_ClipBoxToBrush
has therefore never had.

THE DEFECT.  Sweeping an AABB against a convex brush is a point query against
the Minkowski sum of the two.  That sum's faces are: the brush's own face
planes, the six box face planes, and ONE PLANE PER PAIRING of a brush edge with
a box edge direction.  BIH_ClipBoxToBrush builds the first group (the brush's
sides) and the second (the `if (tr->shape)` axial block against absmins/absmaxs)
and has never had the third.  What is left is a strict SUPERSET of the true sum,
bulging outward along every slanted edge -- the same defect, on the same
argument, that Patch 258 fixed for triangles with BIH_TriangleBevels.  The brush
path was not touched then.

Quake 2 and Source both know this, and both solve it at COMPILE time: qbsp3 and
VBSP's AddBrushBevels (utils/vbsp/map.cpp:501 in the Momentum tree) append the
missing planes to the brush as extra sides, so the runtime clipper needs no
adjacency information.  The runtime is therefore only ever as correct as the
compile was -- and a map can simply not have them.

WHAT IT COST, measured.  surf_boreas brush 170 is a PLAYERCLIP|DETAIL wedge at
the foot of the lock-11 ramp: 7 real faces plus 5 axial bevels, twelve sides,
and NO edge bevels.  It has a 51-degree top face meeting two 45-degree vertical
faces, so the bulge is enormous.  Riding the ramp at 11494.4 -13173.5 2575.9 the
clipper reports the player 0.107 units INSIDE it and stops them dead on side
1509, whose normal is (-0.707 -0.707 0) -- z of zero, so not a surface you slide
along, a wall.  An exact separating-axis test over the brush's 38 hull vertices
puts the player 10.5 units CLEAR of it.  The wall is not there.  It is a phantom
that persists for about 24 units of travel, and the first REAL contact, further
in, is against the top face, whose normal z of 0.625 is a surface you surf.

Running VBSP's own algorithm offline over that brush produces the planes it
should have carried -- chiefly (0 0.870 0.492) and (-0.870 0 0.492) -- and with
them the clipper agrees with the exact test to within 0.004 units at every
sample along the approach.  So this is not a new heuristic; it is the missing
half of a construction Source already specifies, run at load instead of at
compile because we cannot recompile other people's maps.

WHY IT IS SAFE, which is the part that matters.  Every candidate here must pass
the same test VBSP applies: it is kept only if EVERY vertex of the hull is
behind it.  A plane that supports the hull cannot cut solid out of it, so this
can only ever SHRINK the swept volume toward the true Minkowski sum, never below
it.  Falling through the world is the failure mode to fear, and the support test
rules it out by construction rather than by sampling.

Two deliberate departures from VBSP:
  - VBSP walks only sides 6..n (its axial pass swaps the axial planes to the
    front first).  We walk every side.  Extra candidates still have to pass the
    support test, so this can only add planes that were always valid, and it
    does not depend on our side order matching VBSP's.
  - VBSP skips axial EDGES, and so do we: an axial edge's bevels are the axial
    planes, which BIH_ClipBoxToBrush already applies at runtime from
    absmins/absmaxs.  Generating them again would be pure cost.

A brush that runs out of room is left exactly as it was -- correct-as-before
rather than half-beveled, since a partial set is still a superset and the
counters say it happened.  The first caps tried here (512 winding points, 96
bevels) were hit on five of the first six maps swept, which is a cap too tight
and not a map being strange; these are sized so the sweep reports zero.  The
buffers are heap, not stack: 4096 vecV_t is 64K and models can load on worker
threads, which is the crash com_mesh.c:3310 is about.
==================
*/
#define VBSP_MAXBRUSHSIDES	256		/*the existing planes[] cap in FinalizeBrush*/
#define VBSP_MAXWINDVERTS	4096	/*total winding points over one brush*/
#define VBSP_MAXBEVELS		512		/*extra sides one brush may gain*/

static qboolean VBSP_PlaneKnown (const vec4_t *list, int count, const vec3_t normal, float dist)
{
	int i;
	for (i = 0; i < count; i++)
		if (fabs(list[i][0]-normal[0]) < 0.01 && fabs(list[i][1]-normal[1]) < 0.01 &&
		    fabs(list[i][2]-normal[2]) < 0.01 && fabs(list[i][3]-dist)   < 0.01)
			return true;
	return false;
}

static void VBSP_AddBrushBevels (model_t *mod, q2cbrush_t *brush, vec4_t *planes)
{
	vecV_t		   *wverts;
	int				wfirst[VBSP_MAXBRUSHSIDES], wcount[VBSP_MAXBRUSHSIDES];
	vec4_t		   *bevels;
	int				numbevels = 0, nverts = 0;
	int				i, j, k, a, dir, v;
	q2cbrushside_t *nside;
	mplane_t	   *nplane;
	vec3_t			edge, axis, normal;
	float			dist, len;

	wverts = plugfuncs->Malloc(sizeof(*wverts)*VBSP_MAXWINDVERTS);
	bevels = plugfuncs->Malloc(sizeof(*bevels)*VBSP_MAXBEVELS);
	if (!wverts || !bevels)
	{
		plugfuncs->Free(wverts);
		plugfuncs->Free(bevels);
		return;
	}

	/*windings first.  The edge walk needs each face's own ring and the support
	  test needs every point on the hull, and both come from the same pass. */
	for (i = 0; i < brush->numsides; i++)
	{
		wfirst[i] = nverts;
		wcount[i] = modfuncs->ClipPlaneToBrush(wverts+nverts, VBSP_MAXWINDVERTS-nverts,
		                                       planes, sizeof(planes[0]), brush->numsides, planes[i]);
		nverts += wcount[i];
		if (nverts >= VBSP_MAXWINDVERTS)
		{	/*no room to be sure the support test sees the whole hull, and a
			  support test that cannot see every point is not a support test. */
			vbsp_stat_bevelfull++;
			goto done;
		}
	}

	for (i = 0; i < brush->numsides; i++)
	{
		for (j = 0; j < wcount[i]; j++)
		{
			k = (j+1) % wcount[i];
			VectorSubtract (wverts[wfirst[i]+j], wverts[wfirst[i]+k], edge);
			len = VectorLength (edge);
			if (len < 0.5)
				continue;
			VectorScale (edge, 1/len, edge);
			for (a = 0; a < 3; a++)
			{	/*snap, so a hair off axial still reads as axial*/
				if (fabs(edge[a] - 1) < 0.001)	edge[a] =  1;
				if (fabs(edge[a] + 1) < 0.001)	edge[a] = -1;
				if (fabs(edge[a])     < 0.001)	edge[a] =  0;
			}
			if (edge[0] == 1 || edge[0] == -1 ||
			    edge[1] == 1 || edge[1] == -1 ||
			    edge[2] == 1 || edge[2] == -1)
				continue;	/*axial edge -- the runtime axial block already has these*/

			for (a = 0; a < 3; a++)
			for (dir = -1; dir <= 1; dir += 2)
			{
				VectorClear (axis);
				axis[a] = dir;
				CrossProduct (edge, axis, normal);
				if (VectorNormalize (normal) < 0.5)
					continue;	/*edge parallel to this axis; there is no such face*/
				dist = DotProduct (wverts[wfirst[i]+j], normal);

				if (VBSP_PlaneKnown (planes, brush->numsides, normal, dist))
					continue;
				if (VBSP_PlaneKnown (bevels, numbevels, normal, dist))
					continue;

				/*THE SUPPORT TEST.  Every point on the hull must be behind it.*/
				for (v = 0; v < nverts; v++)
					if (DotProduct (wverts[v], normal) - dist > 0.1)
						break;
				if (v != nverts)
					continue;	/*it cuts the brush -- not part of the outer hull*/

				if (numbevels == VBSP_MAXBEVELS)
				{
					vbsp_stat_bevelfull++;
					numbevels = 0;
					goto done;	/*all or nothing; a partial set is still a superset*/
				}
				VectorCopy (normal, bevels[numbevels]);
				bevels[numbevels][3] = dist;
				numbevels++;
			}
		}
	}

	if (!numbevels)
		goto done;

	/*brushside[] points into the shared lump array, so the brush cannot grow in
	  place -- give this one its own.  Planes likewise: the lump's are shared. */
	nside  = plugfuncs->GMalloc(&mod->memgroup, sizeof(*nside)  * (brush->numsides + numbevels));
	nplane = plugfuncs->GMalloc(&mod->memgroup, sizeof(*nplane) * numbevels);
	if (!nside || !nplane)
		goto done;
	memcpy (nside, brush->brushside, sizeof(*nside) * brush->numsides);

	for (i = 0; i < numbevels; i++)
	{
		VectorCopy (bevels[i], nplane[i].normal);
		nplane[i].dist = bevels[i][3];
		/*A bevel is non-axial by construction (an axial one would have been the
		  edge's own axis and was skipped), so PLANE_ANYX is right -- and giving
		  it a type < 3 it does not have would make PlaneDiff read one component
		  in place of the dot product.  signbits still has to be honest. */
		nplane[i].type = 3;
		nplane[i].signbits = (nplane[i].normal[0] < 0 ? 1 : 0) |
		                     (nplane[i].normal[1] < 0 ? 2 : 0) |
		                     (nplane[i].normal[2] < 0 ? 4 : 0);

		nside[brush->numsides+i].plane   = &nplane[i];
		/*BIH_ClipBoxToBrush dereferences leadside->surface->c for the trace's
		  surface, so this may not be NULL.  A bevel is not a real face and has
		  no texture of its own; borrowing side 0's is what VBSP does too. */
		nside[brush->numsides+i].surface = brush->brushside[0].surface;
	}

	brush->brushside = nside;
	brush->numsides += numbevels;
	vbsp_stat_bevelbrushes++;
	vbsp_stat_bevelplanes += numbevels;
done:
	plugfuncs->Free(wverts);
	plugfuncs->Free(bevels);
}

static void VBSP_FinalizeBrush(model_t *mod, q2cbrush_t *brush)
{
	vecV_t verts[256];
	vec4_t planes[VBSP_MAXBRUSHSIDES];
	int i, j;
	ClearBounds(brush->absmins, brush->absmaxs);
	if (brush->numsides > VBSP_MAXBRUSHSIDES)
		return;
	for (i = 0; i < brush->numsides; i++)
	{
		VectorCopy(brush->brushside[i].plane->normal, planes[i]);
		planes[i][3] = brush->brushside[i].plane->dist;
	}
	for (i = 0; i < brush->numsides; i++)
	{
		//most brushes are axial, which can save some a little loadtime
		if (planes[i][0] == 1)
			brush->absmaxs[0] = planes[i][3];
		else if (planes[i][1] == 1)
			brush->absmaxs[1] = planes[i][3];
		else if (planes[i][2] == 1)
			brush->absmaxs[2] = planes[i][3];
		else if (planes[i][0] == -1)
			brush->absmins[0] = -planes[i][3];
		else if (planes[i][1] == -1)
			brush->absmins[1] = -planes[i][3];
		else if (planes[i][2] == -1)
			brush->absmins[2] = -planes[i][3];
		else
		{
			j = modfuncs->ClipPlaneToBrush(verts, countof(verts), planes, sizeof(planes[0]), brush->numsides, planes[i]);
			while (j-- > 0)
				AddPointToBounds(verts[j], brush->absmins, brush->absmaxs);
		}
	}

	/*Patch 318.  After the bounds, because the runtime axial block reads
	  absmins/absmaxs and those must describe the brush, not the beveled side
	  list -- the bevels support the same hull, so the bounds do not change. */
	if (!hl2_brushbevels || hl2_brushbevels->ival)
		VBSP_AddBrushBevels(mod, brush, planes);
}

/*
===============================================================================

AREAPORTALS

===============================================================================
*/

static void FloodArea_r (vbspinfo_t	*prv, size_t areaidx, int floodnum)
{
	size_t		i;

	careaflood_t *flood = &prv->areaflood[areaidx];
	if (flood->floodvalid == prv->floodvalid)
	{
		if (flood->floodnum == floodnum)
			return;
		Con_Printf ("FloodArea_r: reflooded\n");
		return;
	}

	flood->floodnum = floodnum;
	flood->floodvalid = prv->floodvalid;

	{
		carea_t *area = &prv->areas[areaidx];
		q2dareaportal_t	*p = &prv->areaportals[area->firstareaportal];
		for (i=0 ; i<area->numareaportals ; i++, p++)
		{
			if (prv->portalopen[p->portalnum])
				FloodArea_r (prv, p->otherarea, floodnum);
		}
	}
}

/*
====================
FloodAreaConnections


====================
*/
static void	FloodAreaConnections (vbspinfo_t	*prv)
{
	size_t		i;
	int		floodnum;

	// all current floods are now invalid
	prv->floodvalid++;
	floodnum = 0;

	// area 0 is not used
	for (i=0 ; i<prv->numareas ; i++)
	{
		if (prv->areaflood[i].floodvalid == prv->floodvalid)
			continue;		// already flooded into
		floodnum++;
		FloodArea_r (prv, i, floodnum);
	}
}

static void	VBSP_SetAreaPortalState (model_t *mod, unsigned int portalnum, unsigned int area1, unsigned int area2, qboolean open)
{
	vbspinfo_t	*prv;
	prv = (vbspinfo_t*)mod->meshinfo;
	if (portalnum > prv->numareaportals)
		return;
	if (prv->portalopen[portalnum] == open)
		return;
	prv->portalopen[portalnum] = open;
	FloodAreaConnections (prv);
}

static qboolean	VBSP_AreasConnected (model_t *mod, unsigned int area1, unsigned int area2)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;

	if (map_noareas->value)
		return true;

	if (area1 == ~0 || area2 == ~0)
		return area1 == area2;
	if (area1 > prv->numareas || area2 > prv->numareas)
		Host_Error ("area > numareas");

	if (prv->areaflood[area1].floodnum == prv->areaflood[area2].floodnum)
		return true;
	return false;
}


/*
=================
CM_WriteAreaBits

Writes a length qbyte followed by a bit vector of all the areas
that area in the same flood as the area parameter

This is used by the client refreshes to cull visibility
=================
*/
static int VBSP_WriteAreaBits (model_t *mod, qbyte *buffer, int area, qboolean merge)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	int		i;
	int		floodnum;
	int		bytes;
	int		nullarea = 0;

	bytes = (prv->numareas+7)>>3;

	if (map_noareas->value || (area == nullarea && !merge))
	{	// for debugging, send everything
		if (!merge)
			memset (buffer, 255, bytes);
	}
	else
	{
		if (!merge)
			memset (buffer, 0, bytes);

		floodnum = prv->areaflood[area].floodnum;
		for (i=0 ; i<prv->numareas ; i++)
		{
			if (prv->areaflood[i].floodnum == floodnum)
				buffer[i>>3] |= 1<<(i&7);
		}
	}

	return bytes;
}

/*
===================
CM_WritePortalState

Returns a size+pointer to the data that needs to be written into a saved game. 
===================
*/
static size_t VBSP_SaveAreaPortalBlob (model_t *mod, void **data)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;

	*data = prv->portalopen;
	return sizeof(prv->portalopen);
}

/*
===================
CM_ReadPortalState

Reads the portal state from a savegame file
and recalculates the area connections
===================
*/
static size_t VBSP_LoadAreaPortalBlob (model_t *mod, void *ptr, size_t ptrsize)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;

	memcpy(prv->portalopen, ptr, min(ptrsize,sizeof(prv->portalopen)));

	FloodAreaConnections (prv);
	return sizeof(prv->portalopen);
}


/*
===============================================================================

PVS / PHS

===============================================================================
*/

/*
===================
CM_DecompressVis
===================
*/
static void VBSP_DecompressVis (model_t *mod, qbyte *in, qbyte *out, qboolean merge)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	int		c;
	qbyte	*out_p;
	int		row;

	row = (mod->numclusters+7)>>3;
	out_p = out;

	if (!in || !prv->numvisibility)
	{	// no vis info, so make all visible
		while (row)
		{
			*out_p++ = 0xff;
			row--;
		}
		return;
	}

	if (merge)
	{
		do
		{
			if (*in)
			{
				*out_p++ |= *in++;
				continue;
			}

			out_p += in[1];
			in += 2;
		} while (out_p - out < row);
	}
	else
	{
		do
		{
			if (*in)
			{
				*out_p++ = *in++;
				continue;
			}

			c = in[1];
			in += 2;
			if ((out_p - out) + c > row)
			{
				c = row - (out_p - out);
				Con_DPrintf ("warning: Vis decompression overrun\n");
			}
			while (c)
			{
				*out_p++ = 0;
				c--;
			}
		} while (out_p - out < row);
	}
}

static pvsbuffer_t	pvsrow;
static pvsbuffer_t	phsrow;

/*
FTESurf Patch 277(b) (Patch 273's open item b): the query side of the same bound.

Every cluster that reaches VBSP_ClusterPVS/PHS comes from a leaf (VBSP_LeafCluster,
the pvscache leafnums, the refdef view clusters), and VBSP_LoadLeafs now clamps every
leaf's cluster below prv->vis->numclusters -- so this should never fire, and the
counter is how that is checked rather than assumed (vbsp_pvsstat prints it).  If it
does fire the answer is ALL VISIBLE, the same answer an unvised map gets: drawing
more can never cull the player's view against the wrong row, and a merge only ever
widens.  Printed once per map load (reset with the Patch 273 counters), because a
caller that gets this wrong gets it wrong every frame.
*/
static unsigned int vbsp_badclusterqueries;
static void VBSP_BadClusterQuery (model_t *mod, int cluster, const char *what)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	if (!vbsp_badclusterqueries++)
		Con_Printf(CON_WARNING "%s: %s queried for cluster %i but the vis lump has %i -- answered all-visible (printed once per map; vbsp_pvsstat counts)\n",
			mod->name, what, cluster, prv->vis->numclusters);
}



static qbyte	*VBSP_ClusterPVS (model_t *mod, int cluster, pvsbuffer_t *buffer, pvsmerge_t merge)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	if (!buffer)
		buffer = &pvsrow;
	if (buffer->buffersize < mod->pvsbytes)
		buffer->buffer = plugfuncs->Realloc(buffer->buffer, buffer->buffersize=mod->pvsbytes);

	/*
	FTESurf Patch 209c: two guards, and the merge one is the subtle half.

	prv->vis can now legitimately be NULL -- VBSP_LoadVisibility leaves it so for an unvised
	map instead of pointing at a zero-byte block -- and the bitofs[] lookup below
	dereferences it BEFORE VBSP_DecompressVis ever gets to notice.  No vis means everything
	visible, and that is true of a merge as much as of a replace.

	PVM_MERGE must never WIPE.  The `cluster == -1` memset used to be unconditional, so
	merging an invalid cluster destroyed the row the preceding PVM_REPLACE had just built --
	and VBSP_MarkLeaves does exactly that pair for a two-cluster view (the clusters[1] !=
	clusters[0] branch).  The engine's own Q3 branch already guards it this way
	(gl_q2bsp.c:7128-7131); the Q2 branch this was copied from does not.  "No cluster"
	contributes nothing to a merge, so the correct behaviour is to leave the row alone.
	*/
	if (!prv->vis)
		memset (buffer->buffer, 0xff, (mod->numclusters+7)>>3);
	else if (cluster == -1)
	{
		if (merge != PVM_MERGE)
			memset (buffer->buffer, 0, (mod->numclusters+7)>>3);
	}
	else if (cluster < -1 || cluster >= prv->vis->numclusters)
	{	//FTESurf Patch 277(b): bitofs[] is numclusters long; anything else is a wild read.  See VBSP_BadClusterQuery.
		VBSP_BadClusterQuery(mod, cluster, "PVS");
		memset (buffer->buffer, 0xff, (mod->numclusters+7)>>3);
	}
	else
		VBSP_DecompressVis (mod, ((qbyte*)prv->vis) + prv->vis->bitofs[cluster][DVIS_PVS], buffer->buffer, merge==PVM_MERGE);
	return buffer->buffer;
}

static qbyte	*VBSP_ClusterPHS (model_t *mod, int cluster, pvsbuffer_t *buffer)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;

	if (!buffer)
		buffer = &phsrow;
	if (buffer->buffersize < mod->pvsbytes)
		buffer->buffer = plugfuncs->Realloc(buffer->buffer, buffer->buffersize=mod->pvsbytes);

	/*FTESurf Patch 209c, same NULL guard as VBSP_ClusterPVS: an unvised map has no lump to index into, and "no
	  vis" has to mean everything HEARABLE too, or the PHS silences the map.*/
	if (!prv->vis)
		memset (buffer->buffer, 0xff, (mod->numclusters+7)>>3);
	else if (cluster == -1)
		memset (buffer->buffer, 0, (mod->numclusters+7)>>3);
	else if (cluster < -1 || cluster >= prv->vis->numclusters)
	{	//FTESurf Patch 277(b): same bound as VBSP_ClusterPVS; "no vis" means everything hearable too.
		VBSP_BadClusterQuery(mod, cluster, "PHS");
		memset (buffer->buffer, 0xff, (mod->numclusters+7)>>3);
	}
	else
		VBSP_DecompressVis (mod, ((qbyte*)prv->vis) + prv->vis->bitofs[cluster][DVIS_PHS], buffer->buffer, false);
	return buffer->buffer;
}

static unsigned int  VBSP_FatPVS (model_t *mod, const vec3_t org, pvsbuffer_t *result, qboolean merge)
{
	int	leafs[64];
	int		i, j, count;
	vec3_t	mins, maxs;

	for (i=0 ; i<3 ; i++)
	{
		mins[i] = org[i] - 8;
		maxs[i] = org[i] + 8;
	}

	count = VBSP_BoxLeafnums (mod, mins, maxs, leafs, countof(leafs), NULL);
	if (count < 1)
		Sys_Errorf ("SV_Q2FatPVS: count < 1");

	// convert leafs to clusters
	for (i=0 ; i<count ; i++)
		leafs[i] = VBSP_LeafCluster(mod, leafs[i]);

	//grow the buffer if needed
	if (result->buffersize < mod->pvsbytes)
		result->buffer = plugfuncs->Realloc(result->buffer, result->buffersize=mod->pvsbytes);

	if (count == 1 && leafs[0] == -1)
	{	//if the only leaf is the outside then broadcast it.
		memset(result->buffer, 0xff, mod->pvsbytes);
		i = count;
	}
	else
	{
		i = 0;
		if (!merge)
			mod->funcs.ClusterPVS(mod, leafs[i++], result, PVM_REPLACE);
		// or in all the other leaf bits
		for ( ; i<count ; i++)
		{
			for (j=0 ; j<i ; j++)
				if (leafs[i] == leafs[j])
					break;
			if (j != i)
				continue;		// already have the cluster we want
			mod->funcs.ClusterPVS(mod, leafs[i], result, PVM_MERGE);
		}
	}
	return mod->pvsbytes;
}

/*
=============
CM_HeadnodeVisible

Returns true if any leaf under headnode has a cluster that
is potentially visible
=============
*/
/*
FTESurf Patch 273 -- the instrument for the single most expensive thing on this
map.  MEASURED, surf_tensor2 spawn, sv_perfdump, A/B/A/B alternated:

    Snapshot build   sv_nopvs 0 : 1761.01 / 1657.26 us per render frame
                     sv_nopvs 1 :   32.31 /   30.39
    real framerate              :  192.0 / 210.2  ->  398.1 / 411.8

i.e. the per-entity visibility test is 98% of the whole `Server` bucket, and
skipping it DOUBLES the real framerate.  sv_nopvs is a diagnostic, not a fix --
it sends every entity to every client -- so the question these counters answer
is which half of the test is expensive: the ordinary leaf-list compare, or this
function, which descends the WHOLE node tree with no box test and no early-out
for any entity whose leaf list overflowed (num_leafs -1, headnode = topnode,
which is the ROOT for anything straddling it).

`calls` is entries into the test from EdictInFatPVS; `hn` is entries into THIS
function's recursion; `visits` counts every node/leaf it touches.  If visits/frame
is on the order of entities x numleafs, the tree descent is the cost and the fix
belongs in what FindTouchedLeafs records, not in the caller.
*/
static unsigned int vbsp_pvs_calls, vbsp_pvs_hn, vbsp_pvs_visits, vbsp_pvs_overflow;

static qboolean VBSP_HeadnodeVisible (model_t *mod, int nodenum, const qbyte *visbits)
{
	int		leafnum;
	int		cluster;
	mnode_t	*node;

	vbsp_pvs_visits++;

	if (nodenum < 0)
	{
		leafnum = -1-nodenum;
		cluster = mod->leafs[leafnum].cluster;
		if (cluster == -1)
			return false;
		if (visbits[cluster>>3] & (1<<(cluster&7)))
			return true;
		return false;
	}

	node = &mod->nodes[nodenum];
	if (VBSP_HeadnodeVisible(mod, node->childnum[0], visbits))
		return true;
	return VBSP_HeadnodeVisible(mod, node->childnum[1], visbits);
}

/*
FTESurf Patch 273 -- MEMOISE THE DESCENT.  It is a pure function, and it was
being recomputed thousands of times a second for the same two arguments.

VBSP_HeadnodeVisible(mod, nodenum, visbits) reads nothing else and writes
nothing, so for a fixed model its answer depends only on (nodenum, visbits).
Within one server frame every entity is tested against the SAME pvs buffer, and
the entities that reach here are precisely the ones whose leaf list overflowed --
which happens when their box spans the map, which means topnode is usually the
ROOT.  So the server was asking the identical question, of the identical tree,
with the identical visbits, dozens of times per frame.

MEASURED before this cache, surf_tensor2 spawn, over a 4000ms window:

    573,246 visibility tests, 31,104 headnode descents, 341,374,532 node visits
    -- and with sv_nopvs 1 (server skipped, renderer's prop pass still running)
       782,772 tests, 27,306 descents, 122,968,020 visits.

    So the SERVER's share is ~3,800 descents and ~218 MILLION node visits per
    second, i.e. ~57,000 visits per descent: whole-tree, every time.  The prop
    pass's descents are shallow (~4,500) because a prop's topnode is deep.
    `Snapshot build` was 1714us/frame with it and 28.7us without.

The cache is direct-mapped on nodenum and thrown away whenever the visbits
change.  Comparing the visbits costs one memcmp of pvsbytes (762 bytes on
tensor2, ~95 words) -- three orders of magnitude less than the single descent it
replaces, and it is exact rather than heuristic: same bits, same answer.

Keyed on the model too, so a second VBSP model cannot be served another's answer.
*/
/*
ONE SET PER PVS BUFFER, and that is the whole design -- MEASURED, twice.

The first version kept a single table keyed on a memcmp of the visbits.  It took
`Snapshot build` from 1922us to 160us, because the server's entities nearly all
carry headnode 0 (the root) and so ask the identical question all frame.  But
20,010 descents a second SURVIVED, at ~5,500 visits each, and they were in the
renderer.  I assumed the 232 static props were colliding in a 64-entry
direct-mapped table and raised it to 512.

THAT CHANGED NOTHING: 20,560 descents against 20,010, World walking 481-497us
against 496-507us.  The misses were never collisions.  Three DIFFERENT pvs
buffers reach this function every frame -- the main view's, the 3D skybox's, and
the server's snapshot -- and a single set means each one's contents fail the
memcmp against the last caller's and wipe the table.  It is the same one-slot
eviction bug as the vis cache in VBSP_MarkLeaves, in a second place, found the
same way: by measuring a fix that should have worked and did not.

So key on the BUFFER, not just its contents.  Each caller owns a stable buffer
(surf_frustumvis[recurse] for the renderer, cameras->pvs.buffer for the server),
so a pointer compare separates them for free, and the memcmp then only has to
catch the view actually moving -- which is what it was for.  Four sets, because
R_MAX_RECURSE views plus the server is three, and the fourth costs 3KB.

Still exact.  VBSP_HeadnodeVisible reads nothing but its arguments and writes
nothing, so for a fixed model the answer is a pure function of (nodenum,
visbits); a hit is returned only when the model matches, the node matches, and
the visbits compare equal byte for byte.
*/
#define HNV_CACHE 256		//power of two; direct-mapped on nodenum, per set
#define HNV_SETS  4			//one per distinct pvs buffer in flight
static model_t     *vbsp_hnv_mod;
static unsigned int vbsp_hnv_next;	//round-robin victim
static struct
{
	const qbyte	*owner;			//the caller's buffer, the cheap half of the key
	qbyte		*bits;			//our copy of its contents, the exact half
	size_t		 bitsize;
	int			 node[HNV_CACHE];
	qboolean	 res[HNV_CACHE];
} vbsp_hnv[HNV_SETS];
static unsigned int vbsp_pvs_hnhit;

static qboolean VBSP_HeadnodeVisibleCached (model_t *mod, int nodenum, const qbyte *visbits)
{
	size_t n = mod->pvsbytes;
	unsigned int slot, i, s;

	if (!hl2_pvscache || !hl2_pvscache->ival)
	{	//the A/B arm, and the escape hatch if this is ever suspected
		vbsp_pvs_hn++;
		return VBSP_HeadnodeVisible(mod, nodenum, visbits);
	}

	if (vbsp_hnv_mod != mod)
	{	//new map: every set is about someone else's tree
		for (i = 0; i < HNV_SETS; i++)
			vbsp_hnv[i].owner = NULL;
		vbsp_hnv_mod = mod;
		vbsp_hnv_next = 0;
	}

	for (s = 0; s < HNV_SETS; s++)
		if (vbsp_hnv[s].owner == visbits)
			break;
	if (s == HNV_SETS)
	{	//a buffer we are not tracking: take the round-robin victim
		s = vbsp_hnv_next++ & (HNV_SETS-1);
		vbsp_hnv[s].owner = visbits;
		vbsp_hnv[s].bitsize = 0;		//force the reload below
	}

	if (vbsp_hnv[s].bitsize < n)
	{
		vbsp_hnv[s].bits = plugfuncs->Realloc(vbsp_hnv[s].bits, n?n:1);
		vbsp_hnv[s].bitsize = n;
		memset(vbsp_hnv[s].node, 0xff, sizeof(vbsp_hnv[s].node));	//-1 == empty
		if (n) memcpy(vbsp_hnv[s].bits, visbits, n);
	}
	else if (n && memcmp(vbsp_hnv[s].bits, visbits, n))
	{	//this view moved to a different cluster: its cached answers are stale
		memcpy(vbsp_hnv[s].bits, visbits, n);
		memset(vbsp_hnv[s].node, 0xff, sizeof(vbsp_hnv[s].node));
	}

	slot = ((unsigned int)nodenum) & (HNV_CACHE-1);
	if (vbsp_hnv[s].node[slot] == nodenum)
	{
		vbsp_pvs_hnhit++;
		return vbsp_hnv[s].res[slot];
	}

	vbsp_pvs_hn++;
	vbsp_hnv[s].res[slot] = VBSP_HeadnodeVisible(mod, nodenum, visbits);
	vbsp_hnv[s].node[slot] = nodenum;
	return vbsp_hnv[s].res[slot];
}

static void VBSP_PvsStat_f(void)
{	//FTESurf Patch 273 -- see the essay above VBSP_HeadnodeVisible.
	Con_Printf("vbsp_pvsstat: %u visibility tests, %u overflowed to the headnode test\n",
		vbsp_pvs_calls, vbsp_pvs_overflow);
	Con_Printf("              of those: %u cache HITS, %u actual tree descents (%u node visits, %.1f per descent)\n",
		vbsp_pvs_hnhit, vbsp_pvs_hn, vbsp_pvs_visits,
		vbsp_pvs_hn ? vbsp_pvs_visits/(double)vbsp_pvs_hn : 0.0);
	Con_Printf("              %u out-of-range cluster queries since this map loaded (Patch 277b; must be 0)\n",
		vbsp_badclusterqueries);	//FTESurf Patch 277(b): NOT reset here -- it re-arms the once-per-map warning, so it resets with the map
	vbsp_pvs_calls = vbsp_pvs_hn = vbsp_pvs_visits = vbsp_pvs_overflow = vbsp_pvs_hnhit = 0;
}

static qboolean VBSP_EdictInFatPVS(model_t *mod, const pvscache_t *ent, const qbyte *pvs, const int *areas)
{
	int i,l;
	int nullarea = 0;
	vbsp_pvs_calls++;						//FTESurf Patch 273
	vbsp_pvs_why = VBSP_PVSWHY_VISIBLE;		//FTESurf Patch 257, see the enum
	/*
	FTESurf Patch 257 -- AN AREA RECORD THAT COULD NOT BE BUILT MUST NOT CULL.

	pvscache_t holds exactly two areas, and Quake2's own comment in
	VBSP_FindTouchedLeafs says why that was thought to be enough: "doors may
	legally straggle two areas, but nothing should ever need more than that".
	True of a door.  Not true of a static prop, whose visibility box here is
	origin +/- model->radius.

	MEASURED, surf_garden prop 170 -- the missing stage-3 hedge -- after the
	sentinel fix below had already corrected areanum from 0 to a real area:

	    verdict last frame : PVS culled -- rejected by the area walk
	    area               : areanum 4, areanum2 9, headnode 0
	    areas box spanned  : 16
	    pvs leaf state     : num_leafs -1 (headnode test -- leaf list overflowed)

	Sixteen areas into two slots.  {4, 9} is an arbitrary two of them, chosen by
	leaf traversal order, and the player was standing in one of the other
	fourteen -- so the walk found nothing connected to the viewer and culled a
	prop that was directly in front of them.  Fixing the sentinel was necessary
	and could never have been sufficient.

	num_leafs == -1 is the existing, precise statement of "this entity's
	traversal overflowed and the record is approximate".  The area pair is built
	by that same truncated walk, so it is unreliable for exactly the same reason
	and at exactly the same times.  Skipping the area test for those entities is
	therefore not a special case -- it is the same admission, applied to the
	other half of what the walk produced.

	FAIL OPEN, NOT CLOSED.  Areas are a culling optimisation: an areaportal lets
	the engine drop a whole room behind a closed door.  Getting it wrong in the
	permissive direction submits a few extra entities; getting it wrong in the
	restrictive direction deletes geometry the player is looking at, and is
	invisible in profiling.  The entities affected are only those big enough to
	overflow 32 clusters, which are the ones visible from most of the map anyway,
	so the over-draw is bounded and small: two props on surf_garden.

	They are still culled by the PVS -- VBSP_HeadnodeVisible below -- and by the
	frustum test in the caller.  This removes one unsound test, not all of them.
	*/
	if (areas && ent->num_leafs != -1)
	{
		for (i = 1; ; i++)
		{
			if (i > areas[0])
			{
				vbsp_pvs_why = VBSP_PVSWHY_AREA;
				return false;	//none of the camera's areas could see the entity
			}
			if (areas[i] == ent->areanum)
			{
				if (areas[i] != nullarea)
					break;
				//else entity is fully outside the world, invisible to all...
			}
			else if (VBSP_AreasConnected (mod, areas[i], ent->areanum))
				break;
			// doors can legally straddle two areas, so
			// we may need to check another one
			else if (ent->areanum2 != nullarea && VBSP_AreasConnected (mod, areas[i], ent->areanum2))
				break;
		}
	}

	if (ent->num_leafs == -1)
	{	// too many leafs for individual check, go by headnode
		vbsp_pvs_overflow++;	//FTESurf Patch 273
		if (!VBSP_HeadnodeVisibleCached (mod, ent->headnode, pvs))
		{
			vbsp_pvs_why = VBSP_PVSWHY_HEADNODE;
			return false;
		}
	}
	else
	{	// check individual leafs
		for (i=0 ; i < ent->num_leafs ; i++)
		{
			l = ent->leafnums[i];
			if (l < 0)
				continue;	//FTESurf Patch 277(b): a static prop's list keeps -1 clusters verbatim (the game-lump loader stores lf->cluster as is), and pvs[-1] is the byte before the buffer
			if (pvs[l >> 3] & (1 << (l&7) ))
				break;
		}
		if (i == ent->num_leafs)
		{
			vbsp_pvs_why = VBSP_PVSWHY_LEAFS;
			return false;		// not visible
		}
	}
	return true;
}


/*
===============================================================================

Collision Stuff.

===============================================================================
*/

static void VBSP_BuildBIHSubmodel(model_t *mod, int submodel)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	cmodel_t	*sub = &prv->cmodels[submodel];

	struct bihleaf_s *bihleaf, *l;
	size_t i;

	bihleaf = l = plugfuncs->Malloc(sizeof(*bihleaf)*sub->num_brushes);
	for (i = 0; i < sub->num_brushes; i++)
	{
		q2cbrush_t *b = &prv->brushes[sub->firstbrush+i];
		l->type = BIH_BRUSH;
		l->data.brush = b;

		l->data.contents = b->contents;
		VectorCopy(b->absmins, l->mins);
		VectorCopy(b->absmaxs, l->maxs);
		l++;
	}

	modfuncs->BIH_Build(mod, bihleaf, l-bihleaf);
	plugfuncs->Free(bihleaf);
}
static void VBSP_LightPointValues	(struct model_s *model, const vec3_t point, vec3_t res_diffuse, vec3_t res_ambient, vec3_t res_dir);
//FTESurf Patch 268 C: the six faces of that same nearest sample, LINEAR, for #BUMPCUBE; defined beside it.
static void VBSP_LightPointCube		(struct model_s *model, const vec3_t point, vec3_t res_cube[6]);
//FTESurf Patch 268 C2: which way VBSP_LightPointValues points the model light direction it derives
//from that cube.  Registered in VBSP_Init; read there and by the static-prop light cache invalidation.
static cvar_t *hl2_lt_dirsign;
static cvar_t *hl2_lt_baked_dir;	//FTESurf Patch 296
//FTESurf Patch 302: add LUMP_WORLDLIGHTS' direct light to the leaf ambient cube
//for models that have no VRAD bake.  Read in VBSP_LightPointValues and by the
//static-prop light cache invalidation; registered in VBSP_Init.
static cvar_t *hl2_lt_worldlight;
//FTESurf Patch 308: the falsifier for the derived v21/Strata worldlight layout.
//Read at LOAD, inside VBSP_LoadWorldLights, so an A/B needs a map change or a
//second process -- there is no live arm and the description says so.
static cvar_t *hl2_lt_wl21;
/*
FTESurf Patch 304: how the cube is folded into the shader's two slots.

THIS CVAR IS READ TWICE, and that is the point of it.  The C below reads it to
choose the fold, and plugins/hl2/glsl/vmt/vertexlit.glsl reads the SAME cvar
through !!cvardf to choose the matching ramp.  gl_shader.c:2291-2294 Cvar_Gets
the name and force-adds CVAR_SHADERSYSTEM, so changing it live recompiles the
program and re-solves the cache in the same frame -- and, more importantly, the
two halves cannot drift apart, because there is only one value.  A base computed
for a half-lambert ramp and rendered with a lambert one is worse than either fold
on its own; making that state unrepresentable is worth more than the cvar costs.

0/1 ONLY.  The define reaches the preprocessor as "#define hl2_lt_fold %g", so a
fractional value would emit a float literal into an #if, which is not legal
there.  The same sharp edge already exists on r_glsl_rtenvsphere (vertexlit.glsl
:283) and is inherited, not introduced.
*/
static cvar_t *hl2_lt_fold;
/*
FTESurf Patch 233: WHAT IS THIS NAME, AND WILL I EVER SEE IT.

"3 material(s) did not resolve" followed by three paths answers neither question a player
actually has, and the report that prompted this said so: *"Idk if they are in the map file?
or something? it's not clearly stated the issue?"*.  The four answers the loader can give
honestly, in the order they are worth knowing:

  a prop skin          a name from an MDL's texture table.  Source convention puts every
                       model material under models/, and nothing else in a Source map is
                       there, so the prefix is exact rather than a guess.
  N world face(s)      a texdata name with faces on it -- the count IS the exposure.
  ...under the water   those faces carry SURF_WARP, so the surface is water or a water
                       underside and is only visible from in or under it.  Taken from the
                       texinfo flag rather than from the "_beneath" spelling, because the
                       spelling is the mapper's and the flag is the compiler's.
  no face uses it      in the texdata string table and on no surface, so it cannot be seen
                       at all.  This is the one that used to make people hunt for a pack.

Counting faces per name is O(surfaces) and runs once per missing name, so a map with 24
missing materials and 27,000 faces does about 650,000 pointer comparisons, once, on a map
that is already in trouble.  Cheap enough not to need an index.
*/
static int VBSP_MissFaces(model_t *mod, const char *name, int *warp)
{
	int i, n = 0;
	size_t l = strlen(name);

	*warp = 0;
	if (!mod->surfaces || !mod->texinfo)
		return 0;
	for (i = 0; i < mod->numsurfaces; i++)
	{
		const mtexinfo_t *ti = mod->surfaces[i].texinfo;
		if (!ti || !ti->texture)
			continue;
		/*
		Two things sit between the census name and the texture name and both have to be
		stepped over.  The texture carries the texinfo's shader arguments ("...#WARP"), and
		the census carries the EXTENSION-STRIPPED spelling of any name with a foreign
		extension (P231, and surf_demise's "...water_tar_beneath.vmf" is exactly that), so
		a bare prefix test reports a material on 7 faces as being on none.
		*/
		if (Q_strncasecmp(ti->texture->name, name, l))
			continue;
		if (ti->texture->name[l] == '.')
		{	//an extension is only an extension while it stays inside the last path component
			const char *e;
			for (e = ti->texture->name + l + 1; *e && *e != '#'; e++)
				if (*e == '/' || *e == '\\')
					break;
			if (*e && *e != '#')
				continue;
		}
		else if (ti->texture->name[l] && ti->texture->name[l] != '#')
			continue;
		if (ti->flags & TI_WARP)
			*warp = 1;
		n++;
	}
	return n;
}

//true if this name is something the player can actually end up looking at.
static qboolean VBSP_MissReason(model_t *mod, const char *name, char *out, size_t outsize)
{
	int faces, warp;

	if (!Q_strncasecmp(name, "models/", 7))
	{
		Q_strlcpy(out, "a prop skin", outsize);
		return true;
	}

	faces = VBSP_MissFaces(mod, name, &warp);
	if (!faces)
	{
		Q_strlcpy(out, "named by the map but drawn by no face", outsize);
		return false;
	}
	if (warp)
		Q_snprintfz(out, outsize, "%i world face(s), water -- only visible from in or under it", faces);
	else
		Q_snprintfz(out, outsize, "%i world face(s)", faces);
	return true;
}

static void VBSP_BuildBIHMain(void *ctx, void *unusedp, size_t unuseda, size_t unusedb)
{	//NOTE: must be on main thread because we're waiting for submodels for their size.
	model_t		*mod = ctx;
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	cmodel_t	*sub = &prv->cmodels[0];

	struct bihleaf_s *bihleaf, *l;
	size_t bihleafs, i;
	struct vbsp_phyprobe_s *phyprobe = NULL;	//FTESurf Patch 262
	size_t phyprobes = 0;

	/*
	FTESurf Build 7: hl2_propcollision 0 drops every prop out of the collision
	tree, so a route that is blocked by a prop can be told apart from a route
	blocked by world geometry in one map load.  Counted per solidity value at
	developer 1, because "which props are even claiming to be solid" is the
	first thing you want to know when a map has an invisible wall in it --
	0 is Source's SOLID_NONE and 6 its SOLID_VPHYSICS, and anything else is a
	prop asking for a collision shape we approximate with its render mesh.
	*/
	{
		size_t solidity[8], nsolid = 0;
		memset(solidity, 0, sizeof(solidity));
		for (i = 0; i < prv->numstaticprops; i++)
		{
			int s = prv->staticprops[i].solid;
			solidity[(s < 0 || s > 7) ? 7 : s]++;
			if (s)
				nsolid++;
		}
		if (prv->numstaticprops)
			Con_DPrintf("%s: %u static props, %u solid (solidity 0:%u 2:%u 6:%u other:%u)%s\n",
				mod->name, (unsigned)prv->numstaticprops, (unsigned)nsolid,
				(unsigned)solidity[0], (unsigned)solidity[2], (unsigned)solidity[6],
				(unsigned)(prv->numstaticprops - solidity[0] - solidity[2] - solidity[6]),
				hl2_propcollision->ival ? "" : " -- DISABLED by hl2_propcollision 0");

		//(the .phy collision census is printed at the BOTTOM of this function,
		//not here -- see the note there.)

		/*
		FTESurf Patch 152: does this map's leaf ambient cube contain anything.

		THE PROP COLOUR CENSUS IS NOT HERE, and that is not a style choice.
		VBSP_PointLeafnum returns 0 unconditionally while
		`mod->loadstate != MLS_LOADED`, and this function IS the work that
		finishes the load -- so any lighting query made from here reports the
		solid leaf for every point in the map and looks exactly like a map with
		no ambient data.  I chased that artifact for two rebuilds.  The census
		that measures what the RENDERER gets lives in the per-frame prop loop,
		where the model is loaded; see [texdiag] PROPLIGHT.

		What can honestly be answered here is the table itself, because that is
		a read of loaded data rather than a query against the tree.
		*/
		if (hl2_texdiag && hl2_texdiag->ival && prv->numstaticprops && prv->leaflight)
		{
			size_t li, withamb = 0;
			for (li = 0; li < mod->numleafs; li++)
				if (prv->leaflight[li].count)
					withamb++;
			Con_Printf("[texdiag] LEAFAMB %s: %u of %u leaves carry ambient\n",
				mod->name, (unsigned)withamb, (unsigned)mod->numleafs);
		}

		//And the displacements, which are the other candidate for an invisible
		//wall and which are counted in TRIANGLES because that is what each one
		//costs the collision tree.
		if (prv->numdisplacements)
		{
			size_t tris = 0, nohull = 0, noray = 0, nohulltris = 0;
			size_t flipped = 0;	//FTESurf Patch 256
			for (i = 0; i < prv->numdisplacements; i++)
			{
				size_t t = prv->displacements[i].numindexes/3;
				unsigned int f = prv->displacements[i].collflags;
				tris += t;
				//Patch 256: cidx only differs from idx when this one was re-wound.
				if (prv->displacements[i].cidx != prv->displacements[i].idx)
					flipped++;
				//FTESurf Patch 250: NORAY without NOHULL cannot be expressed here
				//(one contents word per BIH leaf), so count it separately rather
				//than let it look handled.  Zero across the sample; if a map ever
				//prints one, that is the case to go and implement.
				if (f & DISPSURF_NOHULL_COLL)	{ nohull++; nohulltris += t; }
				else if (f & DISPSURF_NORAY_COLL) noray++;
			}
			Con_DPrintf("%s: %u displacements, %u collision triangles%s\n",
				mod->name, (unsigned)prv->numdisplacements, (unsigned)tris,
				hl2_dispcollision->ival ? "" : " -- DISABLED by hl2_dispcollision 0");
			if (nohull || noray)
				Con_DPrintf("%s: %u displacements (%u triangles) flagged no-hull-collision%s%s\n",
					mod->name, (unsigned)nohull, (unsigned)nohulltris,
					noray ? " -- plus ray-only opt-outs this cannot express, see Patch 250" : "",
					hl2_dispflags->ival ? "" : " -- IGNORED by hl2_dispflags 0");
			//FTESurf Patch 256.  Printed even at zero when the cvar is off, because
			//"0 re-wound" and "the switch is off" are the two things that look alike.
			if (flipped || !hl2_dispwinding->ival)
				Con_DPrintf("%s: %u of %u displacements re-wound outward for collision (%.1f%%)%s\n",
					mod->name, (unsigned)flipped, (unsigned)prv->numdisplacements,
					100.0*flipped/prv->numdisplacements,
					hl2_dispwinding->ival ? "" : " -- DISABLED by hl2_dispwinding 0");

			/*
			Patch 256: and WHERE.  An offline reimplementation of this loader got
			the Strata strides wrong twice and produced probe points that looked
			plausible and were not, so the engine names its own: the centroid of
			the first few re-wound displacements, which is a coordinate you can
			setpos to and trace against.  Only at developer 1, only three of them.
			*/
			if (prv->numdisplacements && hl2_dispwinding->ival)
			{
				size_t pass, shown;
				//Two passes: re-wound ones, then untouched ones as controls.
				//FLAT ones only -- a steep displacement's centroid is inside the
				//surface, not above it, so a downward trace from there proves
				//nothing.  Print aamax[2], which IS a point you can stand above.
				for (pass = 0; pass < 2; pass++)
				{
					shown = 0;
					for (i = 0; i < prv->numdisplacements && shown < 3; i++)
					{
						dispinfo_t *d = &prv->displacements[i];
						vec3_t c;
						qboolean rewound = (d->cidx != d->idx);
						if (rewound != !pass)
							continue;
						if (d->aamax[2] - d->aamin[2] > 16)
							continue;					//not flat enough to aim at
						if (d->aamax[0]-d->aamin[0] < 128 || d->aamax[1]-d->aamin[1] < 128)
							continue;					//too small to hit reliably
						VectorAvg(d->aamin, d->aamax, c);
						Con_DPrintf("%s:   %s disp %u  setpos %.1f %.1f %.1f  "
							"(top %.2f, z span %.1f)\n", mod->name,
							pass ? "UNTOUCHED" : "re-wound ", (unsigned)i,
							c[0], c[1], d->aamax[2] + 64, d->aamax[2],
							d->aamax[2] - d->aamin[2]);
						shown++;
					}
				}
			}
		}
	}

	vbsp_prop_nocollide = 0;	//FTESurf Patch 262: per map, and this runs once per map
	//FTESurf Patch 262: one slot per prop is the worst case (every prop a distinct
	//model) and costs 16 bytes each for the length of this function.  Sized that way
	//rather than grown, because the alternative is a realloc inside the fill loop.
	phyprobes = 0;
	phyprobe = prv->numstaticprops ? plugfuncs->Malloc(sizeof(*phyprobe)*prv->numstaticprops) : NULL;

	bihleafs = sub->num_brushes;
	if (hl2_dispcollision->ival)
	for (i = 0; i < prv->numdisplacements; i++)
		bihleafs += prv->displacements[i].numindexes/3;
	if (hl2_propcollision->ival)
	for (i = 0; i < prv->numstaticprops; i++)
	{
		if (prv->staticprops[i].solid)
		{
			if (prv->staticprops[i].ent.model && prv->staticprops[i].ent.model->loadstate == MLS_NOTLOADED)
				modfuncs->GetModel(prv->staticprops[i].ent.model->publicname, MLV_WARN);	//we use threads, so these'll load in time.
			bihleafs++;
		}
	}

	bihleaf = l = plugfuncs->Malloc(sizeof(*bihleaf)*bihleafs);

	//now we have enough storage, spit them out providing bounds info.
	for (i = 0; i < sub->num_brushes; i++)
	{
		q2cbrush_t *b = &prv->brushes[sub->firstbrush+i];
		l->type = BIH_BRUSH;
		l->data.brush = b;

		l->data.contents = b->contents;
		VectorCopy(b->absmins, l->mins);
		VectorCopy(b->absmaxs, l->maxs);
		l++;
	}
	if (hl2_dispcollision->ival)
	for (i = 0; i < prv->numdisplacements; i++)
	{
		dispinfo_t *d = &prv->displacements[i];
		unsigned int contents = d->contents;

		size_t j;

		/*
		FTESurf Patch 250: the mapper's opt-out, applied.

		Source gates the hull sweep and the ray test separately (SURF_NOHULL_COLL
		vs SURF_NORAY_COLL, dispcoll_common.cpp:892 and :569).  A BIH leaf carries
		ONE contents word and BIH_RecursiveTrace does a single
		"node->data.contents & tr->hitcontents" against whatever mask the caller
		passed, so there is no place to put "solid to bullets, not to the player".

		So NOHULL clears every bit any solid mask tests, which also stops point
		traces -- a real divergence, and worth naming rather than burying: 1335 of
		the 2184 NOHULL displacements in the sample are NOHULL *without* NORAY, so
		this is not a theoretical case.  In a surf game nothing shoots, and the
		alternative -- leaving them solid to the player -- is the bug being fixed.

		The bits cleared are MASK_BOXSOLID's (bspfile.h:683) plus MONSTERCLIP and
		CORPSE, i.e. everything world.c:2823-2877 can put in hitcontents.  What is
		left -- TRANSLUCENT, WATER, LADDER, SKY -- is what PointContents reports,
		and a displacement you can walk through should still say what it is.
		*/
		if (hl2_dispflags->ival && (d->collflags & DISPSURF_NOHULL_COLL))
			contents &= ~(FTECONTENTS_SOLID|Q2CONTENTS_WINDOW|FTECONTENTS_PLAYERCLIP|
			              FTECONTENTS_MONSTERCLIP|FTECONTENTS_BODY|FTECONTENTS_CORPSE);

		for (j = 0; j < d->numindexes; j+=3)
		{
			index_t *v = d->cidx+j;	//FTESurf Patch 256: outward-wound, not the render copy
			vec_t *v1 = d->xyz[v[0]], *v2 = d->xyz[v[1]], *v3 = d->xyz[v[2]];

			l->type = BIH_TRIANGLE;
			l->data.tri.xyz = d->xyz;
			l->data.tri.indexes = v;

			l->data.contents = contents;
			VectorCopy(v1, l->mins);
			VectorCopy(v1, l->maxs);
			AddPointToBounds(v2, l->mins, l->maxs);
			AddPointToBounds(v3, l->mins, l->maxs);
			l++;
		}
	}
	if (hl2_propcollision->ival)
	for (i = 0; i < prv->numstaticprops; i++)
	{
		if (!prv->staticprops[i].solid)
			continue;

		l->type = BIH_MODEL;
		l->data.mesh.model = prv->staticprops[i].ent.model;
		l->data.mesh.tr = &prv->staticprops[i].transform;

		/*
		FTESurf Patch 228: the NULL test has to come BEFORE the wait, not after it.
		The wait dereferences the model, and so did the warning that was supposed to
		catch a missing one -- from inside its own `!model` branch.  Neither had ever
		fired, because on the maps that reach this loop every prop has a model; but
		this patch brings 32k more props here, from 82 maps whose assets nobody has
		exercised, so "had never fired" stops being reassuring.
		*/
		if (!l->data.mesh.model)
		{
			Con_Printf(CON_WARNING "%s: a solid static prop has no model at all\n", mod->name);
			continue;
		}

		//*cry*
		while (l->data.mesh.model->loadstate==MLS_LOADING)
			threadfuncs->WaitForCompletion(l->data.mesh.model, &l->data.mesh.model->loadstate, MLS_LOADING);

		if (!l->data.mesh.model->funcs.NativeTrace || !l->data.mesh.model->funcs.NativeContents)
		{
			Con_Printf("%s has no collision info and must not be used as a solid static prop\n", l->data.mesh.model->name);
			continue;
		}

		/*
		FTESurf Patch 262.  A prop that asks for a HULL and whose model ships no
		.phy does not collide, because in Source it does not either: no
		$collisionmodel means no vcollide means no entry in the collision list.
		Substituting the render mesh -- which is what mod_hl2.c does, and still
		does for every other use of the model -- is not a coarser hull, it is a
		hull where Source has none, made of exactly the geometry that should let
		you through.  Census over the 1310-map library, counting only props that
		ask for a hull: 34,514 of 110,959 solid props across 380 maps.  On
		surf_garden 383 of 420 -- ivory shrubs, cypresses, azaleas.

		m_Solid 2/3/4 are excluded on purpose.  Those ask for the model's bounding
		box, which Source builds from the studio bounds with no .phy involved, so
		they keep whatever shape hl2_propcollision chose for them.  That exclusion
		is the whole reason this test lives here and not in the model loader: the
		loader sees a model, and two props sharing one model can ask for different
		things.  Ten models across eight maps in the library actually do.

		Only in mode 1.  Modes 2 and 3 are deliberate global overrides and mode 0
		has already skipped every prop, so none of them should be second-guessed.
		*/
		if (hl2_propcollision->ival == 1 &&
			!(hl2_propcollision_nophy && hl2_propcollision_nophy->ival) &&
			prv->staticprops[i].solid != 2 && prv->staticprops[i].solid != 3 &&
			prv->staticprops[i].solid != 4 &&
			!VBSP_ModelHasPhy(phyprobe, &phyprobes, l->data.mesh.model))
		{
			prv->staticprops[i].nocollide = true;
			vbsp_prop_nocollide++;
			continue;
		}
		prv->staticprops[i].nocollide = false;

		l->data.contents = ~0u; //yuck!

		//a clever person could probably do a better job.
		l->mins[0] = prv->staticprops[i].ent.origin[0] - l->data.mesh.model->radius;
		l->mins[1] = prv->staticprops[i].ent.origin[1] - l->data.mesh.model->radius;
		l->mins[2] = prv->staticprops[i].ent.origin[2] - l->data.mesh.model->radius;
		l->maxs[0] = prv->staticprops[i].ent.origin[0] + l->data.mesh.model->radius;
		l->maxs[1] = prv->staticprops[i].ent.origin[1] + l->data.mesh.model->radius;
		l->maxs[2] = prv->staticprops[i].ent.origin[2] + l->data.mesh.model->radius;
		l++;
	}

	modfuncs->BIH_Build(mod, bihleaf, l-bihleaf);
	plugfuncs->Free(bihleaf);
	plugfuncs->Free(phyprobe);	//FTESurf Patch 262
	phyprobe = NULL;

	/*
	FTESurf Build 8: what the props are actually colliding against.

	DOWN HERE, not with the solidity census at the top of this function, and
	that placement is the whole reason the first version of this printed all
	zeros: the prop models are only REQUESTED by the counting loop above and
	only guaranteed finished by the WaitForCompletion in the fill loop, both of
	which run after the census block.  This is the first point at which the
	totals are final.

	"hull" is the number that matters -- how many models stopped colliding
	against their visible geometry.  See mod_phy.c.
	*/
	if (prv->numstaticprops && hl2_propcollision->ival)
		Con_DPrintf("%s: collision shapes -- %i .phy hull (%i tris), %i bbox, "
			"%i render mesh (%i unusable of which %i jointed), %i models with no .phy\n",
			mod->name, phy_stat_hull, phy_stat_tris, phy_stat_bbox,
			phy_stat_rejected, phy_stat_rejected, phy_stat_jointed,
			phy_stat_nofile);

	/*
	FTESurf Patch 317.  The winding census for this map's .phy hulls, and the
	line that says the hulls just moved.

	A Con_Printf rather than a DPrintf for the same reason Patch 262's is: on a
	map whose route runs over prop ramps this changes where the floor is by
	4/|n_z| units, so every recorded time on it stops being comparable, and this
	is the line the player whose PB just broke needs to find.  Only printed when
	something was actually re-wound.

	`outward` here counts faces that were ANTICLOCKWISE from outside, i.e. the
	ones that were wrong for the BIH and have been flipped.  If that is not
	essentially 100% of the total, the premise of this patch does not hold on
	this map -- say so rather than trusting the default.
	*/
	if (hl2_phywinding->ival && (phy_stat_tris_outward || phy_stat_tris_inverted))
	{
		int tot = phy_stat_tris_outward + phy_stat_tris_inverted;
		Con_Printf("%s: .phy collision hulls re-wound (hl2_phywinding %i) -- %i of %i faces "
			"(%.1f%%) were inside out and their four-unit slab has moved off the visible "
			"surface. Times on this map are not comparable with a build before Patch 317.\n",
			mod->name, hl2_phywinding->ival, phy_stat_tris_outward, tot,
			tot ? (100.0*phy_stat_tris_outward)/tot : 0.0);
	}

	/*
	FTESurf Patch 318, and a Con_Printf for the same reason as the line above:
	this one moves WORLD collision, so it is the line the player whose time just
	changed needs to find.  Only printed when a brush actually gained a plane.

	`bevelfull` is not a warning to ignore: a brush that ran out of room kept the
	superset it had, which is correct-as-before but still capable of the phantom.
	If it is ever non-zero on a real map the caps in VBSP_AddBrushBevels are too
	tight and should be raised rather than explained away.
	*/
	if (vbsp_stat_bevelbrushes || vbsp_stat_bevelfull)
		Con_Printf("%s: brush edge bevels generated (hl2_brushbevels %i) -- %i planes across "
			"%i brushes the map compiled without them%s. Box sweeps against those brushes no "
			"longer bulge along their slanted edges; times on this map are not comparable with "
			"a build before Patch 318.\n",
			mod->name, hl2_brushbevels?hl2_brushbevels->ival:1,
			vbsp_stat_bevelplanes, vbsp_stat_bevelbrushes,
			vbsp_stat_bevelfull ? va(", and %i MORE were left alone for want of room", vbsp_stat_bevelfull) : "");

	/*
	FTESurf Patch 262, and a real Con_Printf rather than a DPrintf: this is the
	line that says how much of the map you can now walk through, and it is the
	first thing to read when something that used to stop you stops stopping you.
	Printed whenever it is non-zero, and printed as a count of PROP INSTANCES
	because that is what the player meets -- surf_garden reads 420, surf_boreas 0.
	*/
	if (vbsp_prop_nocollide)
		Con_Printf("%s: %u static props are not solid -- their models ship no .phy, "
			"and Source does not collide a prop with no collision hull. "
			"`hl2_propcollision_nophy 1` restores the old render-mesh collision.\n",
			mod->name, vbsp_prop_nocollide);

	/*
	FTESurf Build 10: the materials that cost extra scene renders.

	A non-zero water or refract count is the answer to "why is this map slower
	than that one" -- see hl2_water.  Printed even when both are zero would be
	noise, so it is not.
	*/
	/*
	FTESurf build 11: name the map that needs an asset mount, and the command.

	CS:GO and TF2 used to be mounted permanently so that two maps out of 1308
	had their fallback textures.  That cost 13 seconds of EVERY launch -- boot
	12.0s -> 3.0s and +map 7.0s -> 3.0s with them removed, because their two big
	VPKs are 85% of the 339,103 file entries the engine indexes, and the shader
	rescan walks the whole index four times over.

	So they are unmounted (ftesurf/fs_addons.txt) and this replaces them.  Not a
	warning to be ignored: it is the only thing standing between "this map looks
	wrong" and the one line that fixes it, and it costs nothing on the 1306 maps
	that resolve.  Printed at CON_WARNING so it survives developer 0.
	*/
	/*
	FTESurf Patch 174: list them, and publish the count.

	The console line above was correct and still went unread -- reported as
	"all of surf_utopia's textures are missing, I believe the mount isn't
	working correctly", which is the right symptom and the wrong cause, and
	which the existing message answers in full.  It answers it in a place a
	player is not looking: a map load prints several hundred lines, and this was
	two of them.

	So it now names every material rather than the first (the list is what you
	want once you accept the diagnosis) and sets hl2_unresolved, which the HUD
	reads to put the count on screen where the missing textures are.  A cvar
	rather than a new stat because the plugin has no way to reach the server's
	stat table and this costs nothing on the 1306 maps that resolve.
	*/
	/*
	FTESurf Patch 231: STOP BLAMING THE ASSET PACK.

	The advice above was two hardcoded fs_load lines printed for every miss on every
	map, and it is wrong far more often than it is right.  tools/mapdeps.py --audit
	over the 1411-map library: 335 maps need an addon pack, and 593 reference content
	that NO pack supplies.  Every one of that 593 was being told to mount CS:GO or TF2.

	Both maps this was reported on are in that bucket, and neither has anything to do
	with an unmounted pack:

	  surf_aesthetic  needs CS:GO for 26 files, all 26 of which fs_automount served
	                  correctly from the asset cache.  Its ONE unresolved material is
	                  water/clear_beneath, which is in no pack, no VPK, and not in the
	                  map's own pakfile either.
	  surf_demise     has no data/mapdeps.txt line at all -- because mapdeps.py already
	                  established that no pack resolves it -- and was recommended one
	                  regardless.  Its miss is liquidpack/water/unique/
	                  water_tar_beneath.vmf, which is the $bottommaterial of the water
	                  material in its own pakfile, spelled .vmf for .vmt by the mapper
	                  and never shipped in either spelling.

	Both are the same shape: VBSP writes a water VMT's $bottommaterial into the texdata
	string table as a real texinfo, so the underside of the water is an ordinary world
	surface, and it goes missing when the mapper does not ship the material.  It is only
	visible from under the water, and no command a player can type will fix it.

	fs.c publishes what fs_automount actually did (fs_automount_state, P231), so the
	engine can now say the true thing instead of the same thing.  The ONLY state in
	which fs_load is the right answer is 3.
	*/
	//FTESurf Patch 231: remember what THIS line said.  Materials keep failing after it
	//prints -- props stream in, the HUD loads, and the previous map's cached models get
	//their shaders re-registered -- so by the time anyone types hl2_missing the running
	//total is much larger and carries names from a map they have already left.
	//surf_aesthetic: 1 here, 101 by the time the map is playable, 13 of them `gfx/` and
	//most of the rest surf_demise's props from the load before.  Both numbers are true
	//and they answer different questions, so hl2_missing reports the pair rather than
	//letting the difference look like a fault.
	vmt_census_atload = vmt_stat_missing;
	vmt_miss_reasons = vmt_miss_visible = 0;

	if (vmt_stat_missing)
	{
		int i, shown = vmt_stat_missing;
		int state;
		char spec[MAX_QPATH*4], mapshort[MAX_QPATH], *dot;
		const char *sl;

		//FTESurf Patch 233: classify every name we RECORDED, not just the ones about to be
		//printed -- hl2_missing shows the rest and the visible count has to cover all of them.
		vmt_miss_reasons = (vmt_stat_missing > VMT_MISSING_LIST) ? VMT_MISSING_LIST : vmt_stat_missing;
		for (i = 0; i < vmt_miss_reasons; i++)
			if (VBSP_MissReason(mod, vmt_stat_missing_name[i], vmt_miss_reason[i], sizeof(vmt_miss_reason[0])))
				vmt_miss_visible++;
		//past the cap the names were never stored, so nothing can be proven inert: count them all.
		vmt_miss_visible += vmt_stat_missing - vmt_miss_reasons;

		if (shown > VMT_MISSING_SHOW)
			shown = VMT_MISSING_SHOW;

		Con_Printf(CON_WARNING "%s: %i material(s) did not resolve:\n",
			mod->name, vmt_stat_missing);
		for (i = 0; i < shown; i++)
			Con_Printf(CON_WARNING "    %-42s -- %s\n",
				vmt_stat_missing_name[i], vmt_miss_reason[i]);
		if (vmt_stat_missing > shown)
			Con_Printf(CON_WARNING "    ...and %i more (`hl2_missing` lists every one)\n",
				vmt_stat_missing - shown);

		//"maps/surf_aesthetic.bsp" -> "surf_aesthetic", which is the argument
		//fs_cache_clear takes and the name mapdeps.txt is keyed on.
		sl = strrchr(mod->name, '/');
		Q_strlcpy(mapshort, sl?sl+1:mod->name, sizeof(mapshort));
		dot = strrchr(mapshort, '.');
		if (dot)
			*dot = 0;

		state = (int)cvarfuncs->GetFloat("fs_automount_state");
		if (!cvarfuncs->GetString("fs_automount_spec", spec, sizeof(spec)))
			*spec = 0;

		switch (state)
		{
		case 0:	//mapdeps.txt was read and names no pack for this map
			Con_Printf(CON_WARNING
				"  No asset pack supplies these.  data/mapdeps.txt names none for this map,\n"
				"  which is tools/mapdeps.py having already resolved every material against\n"
				"  every pack and the map's own pakfile.  Mounting something will not bring\n"
				"  them back: this map references content that was never shipped with it.\n");
			break;
		case 1:	//its pack is mounted from disk
			Con_Printf(CON_WARNING
				"  \"%s\" is already mounted for this map, so these are missing from it too.\n"
				"  The map references content that was never shipped with it.\n", spec);
			break;
		case 2:	//its pack is being served from <gamedir>_cache
			Con_Printf(CON_WARNING
				"  \"%s\" is being served from the asset cache for this map, so these are\n"
				"  missing from it too.  If you suspect the cache is short rather than the map,\n"
				"    fs_cache_clear %s\n"
				"  and reload -- that makes it re-prove itself against the real pack.\n",
				spec, mapshort);
			break;
		case 3:	//it names a pack that is not installed -- the one case fs_load answers
			Con_Printf(CON_WARNING
				"  This map asks for \"%s\", which is not installed here.  Mount it with:\n"
				"    fs_load %s\n"
				"  then reload the map.  See ftesurf/fs_addons.txt for why it is not mounted\n"
				"  by default.\n", spec, spec);
			break;
		default:	//4, or anything a future fs.c invents: claim nothing
			Con_Printf(CON_WARNING
				"  fs_automount did not run for this map (it is off, or data/mapdeps.txt is\n"
				"  missing), so nothing was mounted on its behalf.  Set fs_automount 1 and\n"
				"  reload, or fs_load the pack by hand.\n");
			break;
		}
	}
	/*
	FTESurf Patch 181: materials that WERE found and would not parse.

	Reported separately and quietly, because the fix is ours and not the
	player's.  Lumping them in with the missing list produced advice that could
	not possibly help: surf_tensor2 reported two "unresolved" materials only
	AFTER fs_automount mounted CS:GO for it, and the message asked the player to
	mount CS:GO.  What actually happens is that CS:GO's copy of the material is
	richer than the stub in the map's own pakfile and VMT_ParseBlock gives up on
	it, so the surface falls back to a default shader rather than going missing.
	*/
	if (vmt_stat_unparsed)
	{
		int i, shown = vmt_stat_unparsed;
		if (shown > VMT_MISSING_SHOW)	//P231: the storage is 256 now; the print stays readable
			shown = VMT_MISSING_SHOW;
		Con_DPrintf("%s: %i material(s) were found but did not parse (using defaults):\n",
			mod->name, vmt_stat_unparsed);
		for (i = 0; i < shown; i++)
			Con_DPrintf("    %s\n", vmt_stat_unparsed_name[i]);
		if (vmt_stat_unparsed > shown)
			Con_DPrintf("    ...and %i more\n", vmt_stat_unparsed - shown);
	}
#ifdef HAVE_CLIENT
	//Set on every load, cleared included: a map that resolves has to be able to
	//take the banner back down, and "the last map that failed" is not a fact
	//about the map you are standing in.
	cvarfuncs->SetFloat("hl2_unresolved", vmt_stat_missing);
	cvarfuncs->SetString("hl2_unresolved_first",
		vmt_stat_missing ? vmt_stat_missing_name[0] : "");
	/*
	FTESurf Patch 233: and how many of them are on something you can look at.

	Every unresolved name used to earn the same red banner in the middle of the screen,
	including names that are in the texdata string table and on no surface at all.  The
	banner is the one a player reads, so it is the one that has to be worth reading; the
	console still lists everything, and hl2_missing still says which is which.
	*/
	cvarfuncs->SetFloat("hl2_unresolved_visible", vmt_miss_visible);

	/*
	FTESurf Patch 254: the map's underwater fog, published for the gamecode.

	Source takes the fog you see while submerged from the WATER MATERIAL's
	$fogcolor/$fogstart/$fogend, not from env_fog_controller -- so it is data only
	this plugin ever sees.  mat_vmt.c has parsed $fogcolor and $fogend all along
	and spent them on the hl2_water dither and #FOGTINT; $fogstart was
	recognised-and-discarded until now.

	Cvars are the channel because they are the only plugin->QC channel there is,
	and it is a proven one -- hl2_unresolved above is read straight out of CSQC by
	cl_gfx.qc for the missing-materials banner.  The gamecode hands these to the
	engine's `waterfog` command once at map load and never thinks about
	submersion again: gl_rmain.c swaps to cl.fog[FOGTYPE_WATER] by itself
	whenever RDF_UNDERWATER is set.

	Cleared when a map has no water, for the same reason the unresolved counts
	are: "the last map that had water" is not a fact about this one.
	*/
	cvarfuncs->SetString("hl2_waterfog", vmt_stat_waterfog_set ? vmt_stat_waterfog : "");
	cvarfuncs->SetFloat("hl2_waterfog_start", vmt_stat_waterfog_set ? vmt_stat_waterfogstart : 0);
	cvarfuncs->SetFloat("hl2_waterfog_end",   vmt_stat_waterfog_set ? vmt_stat_waterfogend : 0);
#endif

	/*
	FTESurf Patch 217: the TRANSLUCENT count gets to print on its own.

	This was gated on `vmt_stat_water || vmt_stat_refract`, so the translucency
	census -- which is in the same line -- was invisible on every map with no water
	and no refract material.  surf_kitsune is one: nine grid materials that decide
	whether its whole world is blended or solid, and no way to read the count that
	says which.  Diagnosing $translucent/$alpha there meant inferring the answer
	from screen brightness, which is exactly the kind of proxy that gets a wrong
	conclusion believed.
	*/
	if (vmt_stat_water || vmt_stat_refract || vmt_stat_translucent)
		Con_DPrintf("%s: %i water material(s) at hl2_water %i, %i refract at "
			"hl2_refract %i, %i translucent\n",
			mod->name, vmt_stat_water, hl2_water?hl2_water->ival:1,
			vmt_stat_refract, hl2_refract?hl2_refract->ival:1,
			vmt_stat_translucent);

	//FTESurf Patch 251: and WHICH KEYS reached vmt/water.glsl. See the note beside
	//vmt_stat_waterargs in mat_vmt.c -- a water surface does not stop the
	//shader_here trace, so this is the only place that can honestly answer it.
	//Guarded only on there BEING water: "(not recorded)" is a finding -- it means
	//the material generated somewhere this counter did not see -- and hiding it
	//behind *vmt_stat_waterargs would print nothing in exactly that case.
	if (vmt_stat_water)
		Con_DPrintf("%s: water program args %s\n", mod->name,
			*vmt_stat_waterargs ? vmt_stat_waterargs : "(not recorded)");

	/*
	FTESurf Patch 263: WindowImposter, and the classes still unimplemented.

	Two lines rather than one, because they answer different questions. The
	first says "this map uses the fake-skybox shader and here is the switch";
	the second is the blind spot itself -- a class this file does not implement
	draws as vmt/unlit with whatever texture the default-fill guessed, and until
	this patch nothing anywhere said so. The names matter more than the count:
	a number tells you something is wrong, a name tells you what to go and read.
	*/
	if (vmt_stat_imposter)
		Con_DPrintf("%s: %i WindowImposter material(s) at hl2_imposter %i\n",
			mod->name, vmt_stat_imposter, hl2_imposter?hl2_imposter->ival:1);

	/*
	FTESurf Patch 264: how many $envmap cubemaps only exist as .hdr.vtf.

	GENERATIONS, not materials, and the word is load-bearing.  This counts calls
	to Shader_GenerateFromVMT that took the fallback, and a material is generated
	again for every model skin family that names it -- so surf_agony reports 102
	against an offline census of 45 distinct references, because
	proxychains/decay_props/ramp_02.vmt and its siblings are regenerated a dozen
	times each for the props that use them.  Neither number is wrong; they count
	different things, and saying "material(s)" here would have quietly invited the
	same packed-versus-drawn conflation this build has now made twice.

	surf_bikini_bottom prints 1, which is the whole of its census entry, and
	surf_era prints nothing at all -- that silence is the control.  13,291 baked
	cubemaps in the library ship both cX_Y_Z.vtf and cX_Y_Z.hdr.vtf; if the
	fallback ever became an override, this line would appear on every HDR-compiled
	map in the library instead of on thirteen.
	*/
	if (vmt_stat_hdrenvmap)
		Con_DPrintf("%s: %i $envmap generation(s) resolved only as .hdr.vtf\n",
			mod->name, vmt_stat_hdrenvmap);

	//Patch 290.  Declined rather than emitted, because `nodepth` is a pass keyword
	//and the generic tail has no pass to put it in -- see the essay at mat_vmt.c.
	if (vmt_stat_ignorez)
		Con_DPrintf("%s: %i material(s) ask for $ignorez -- declined, no top-level depth key\n",
			mod->name, vmt_stat_ignorez);

	/*
	Patch 300.  Both numbers, not just the first: "moved" alone cannot tell a map
	with no additive materials apart from a gate that declined every one of them.

	"so far" is load-bearing and is not hedging.  This census runs at MAP LOAD and
	therefore sees only the materials the BSP itself names.  A material the CSQC
	asks for later -- every env_sprite and env_lightglow goes through
	R_BeginPolygon, which resolves by name at DRAW time -- is translated after
	this line has already printed, so it is counted in neither number.

	surf_tensor2 is the case that matters and it prints NOTHING here: its 429
	glow sprites are all CSQC-loaded and no world face references a sprite
	material, so both counters are still 0 when this runs.  The three shaders do
	move -- `[shader] sprites/light_glow03 ... prog 1` at developer 1 says so --
	but this line cannot see them.  Anyone using this number to decide whether the
	patch is active on a map will get the wrong answer on exactly the map it was
	written for; read the per-material [shader] lines instead.
	*/
	if (vmt_stat_addprog || vmt_stat_addpass)
		Con_DPrintf("%s: %i additive material(s) on vmt/unlit so they fog to black, %i left on a pass (world materials only, so far)\n",
			mod->name, vmt_stat_addprog, vmt_stat_addpass);

	if (vmt_stat_unknown)
	{
		int u;
		Con_DPrintf("%s: %i material(s) name a shader class this build does not implement:",
			mod->name, vmt_stat_unknown);
		for (u = 0; u < 4 && *vmt_stat_unknown_name[u]; u++)
			Con_DPrintf(" %s", vmt_stat_unknown_name[u]);
		Con_DPrintf("%s\n", (vmt_stat_unknown > u) ? " ..." : "");
	}

	/*
	FTESurf Patch 251.  Its own line rather than more columns on the one above,
	because these two answer a different question: not "what did this cost" but
	"did the map have anything for the feature to act on".  Both are rare enough
	(26 and 42 materials in a 200-map sample) that a silent zero is the expected
	case and a non-zero is the interesting one.
	*/
	if (vmt_stat_seamless || vmt_stat_bumpmap2)
		Con_DPrintf("%s: %i $seamless_scale material(s) at hl2_seamless %i, "
			"%i $bumpmap2 at hl2_bumpmap2 %i\n",
			mod->name, vmt_stat_seamless, hl2_seamless?hl2_seamless->ival:1,
			vmt_stat_bumpmap2, hl2_bumpmap2?hl2_bumpmap2->ival:1);

	/*
	FTESurf Patch 253.  The flipbook count, and how many of them are unlit.

	The split is the point, not the total: an UnlitGeneric flipbook takes a
	DIFFERENT program permutation (#UNLIT) from a lightmapped one, and getting that
	wrong is silent -- the material still animates, it is just multiplied by a
	lightmap it should never have had.  Printing the two numbers is what makes
	"49% of them are unlit" checkable per map instead of only in a static census.
	*/
	if (vmt_stat_animated)
		Con_DPrintf("%s: %i AnimatedTexture flipbook(s) at hl2_animated %i, "
			"%i of them unlit (#UNLIT, no lightmap page)\n",
			mod->name, vmt_stat_animated, hl2_animated?hl2_animated->ival:0,
			vmt_stat_animunlit);

	/*
	FTESurf.  UnlitTwoTexture, and how many of them got their proxy chain.

	The second number is the one that can go wrong quietly.  A two-layer
	material draws whether or not its PlayerProximity chain resolved, so a
	shield with a broken graph walk looks like a shield -- just one that is
	visible from across the map, which is what it looked like BEFORE any of
	this.  "4 twotexture, 0 with a distance fade" is the shape of that failure
	and it is not visible in a screenshot.
	*/
	if (vmt_stat_twotexture)
		Con_DPrintf("%s: %i UnlitTwoTexture material(s) at hl2_twotexture %i, "
			"%i with a distance fade, %i with a distance-driven frame "
			"(%i sampling the flipbook at hl2_twoframes %i, %i declined)\n",
			mod->name, vmt_stat_twotexture, hl2_twotexture?hl2_twotexture->ival:1,
			vmt_stat_proxfade, vmt_stat_proxframe,
			vmt_stat_twoframes, hl2_twoframes?hl2_twoframes->ival:1,
			vmt_stat_twoframesdecl);

	/*
	FTESurf Patch 288.  Printed whenever the map mentions colour correction AT
	ALL, including when nothing survives, because "0 active" and "no line" are
	different states and only one of them is a bug.  A grade that silently fails
	to load looks exactly like a map that was never graded.
	*/
	if (cc_seen || cc_volumes)
		Con_DPrintf("%s: %i color_correction (%i active at hl2_colourcorrection %i, "
			"%i declined for falloff, %i still disabled, %i over the %i cap), "
			"%i color_correction_volume declined\n",
			mod->name, cc_seen, cc_n, hl2_colourcorrection?hl2_colourcorrection->ival:1,
			cc_falloff, cc_disabled, cc_over, CC_MAX, cc_volumes);

#ifdef HAVE_CLIENT
	/*
	FTESurf Build 13.  Worth a line at CON_WARNING rather than DPrintf when a
	map has none: "0 cubemaps" is the state in which every env_cubemap surface
	used to reflect the SKY, and it is not obvious from looking at the map that
	the author never ran buildcubemaps.
	*/
	if (cube_stat_default)
		Con_DPrintf("%s: no env_cubemaps built -- using the map's cubemapdefault%s\n",
			mod->name, cube_stat_missing?", WHICH IS ALSO MISSING":"");
	else if (cube_stat_count)
		Con_DPrintf("%s: %i env_cubemap(s), %i whose baked VTF is missing\n",
			mod->name, cube_stat_count, cube_stat_missing);
#endif

	mod->funcs.PointContents		= VBSP_PointContents;
}

/*
===============================================================================

Rendering stuff

===============================================================================
*/

#ifdef HAVE_CLIENT
static qboolean VBSP_CullBox (vec3_t mins, vec3_t maxs)
{
	//this isn't very precise.
	//checking each plane individually can be problematic
	//if you have a large object behind the view, it can cross multiple planes, and be infront of each one at some point, yet should still be outside the view.
	//this is quite noticable with terrain where the potential height of a section is essentually infinite.
	//note that this is not a concern for spheres, just boxes.
	int		i;

	for (i = 0; i < refdef->frustum_numplanes; i++)
		if (BOX_ON_PLANE_SIDE (mins, maxs, &refdef->frustum[i]) == 2)
			return true;
	return false;
}
// nettest: SAFE world-surface emit. A VBSP/Source world face reached by the leaf/node
// walk can have NO valid render batch: mod->nummodelsurfaces is narrowed to
// cmodels[0].numsurfaces (see ~"nummodelsurfaces = prv->cmodels[0].numsurfaces"), so
// Mod_Batches only assigns surf->sbatch + counts maxmeshes over THAT range — yet the
// walk emits any leaf-marked surface (validated only vs the LARGER loadmodel->numsurfaces).
// Blindly doing surf->sbatch->mesh[meshes++] then either deref's a NULL sbatch or overruns
// the batch's mesh[] block (sized maxmeshes*R_MAX_RECURSE), smashing an adjacent heap
// allocation header — a later malloc reads the corrupted size and tries a multi-GB request
// (the observed ~1GB->10GB-in-1s ramp, then crash, on the d1_canals team-select camera).
// Guard the write: drop the surface instead of corrupting the heap. Warns a few times at
// developer 1 so the path can be confirmed/quantified.
static int vbsp_emit_dropped = 0;
#define VBSP_EMIT_SURF(s) do { \
		if ((s)->sbatch && (s)->sbatch->meshes < (s)->sbatch->maxmeshes*R_MAX_RECURSE) \
			(s)->sbatch->mesh[(s)->sbatch->meshes++] = (s)->mesh; \
		else if (vbsp_emit_dropped++ < 8) \
			Con_DPrintf("[vbsp] surf-emit guard tripped: sbatch=%p meshes=%i cap=%i — surface dropped (heap overrun averted)\n", \
				(void*)(s)->sbatch, (s)->sbatch?(int)(s)->sbatch->meshes:-1, (s)->sbatch?(int)((s)->sbatch->maxmeshes*R_MAX_RECURSE):0); \
	} while(0)

static void VBSP_RecursiveWorldNode (model_t *model, mnode_t *node)
{
	int			c, side;
	mplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	double		dot;

	int sidebit;

	if (node->contents == FTECONTENTS_SOLID)
		return;		// solid

	if (node->visframe != vbsp_nodesequence)
		return;
	if (VBSP_CullBox (node->minmaxs, node->minmaxs+3))
		return;

// if a leaf node, draw stuff
	if (node->contents != -1)
	{
		pleaf = (mleaf_t *)node;

		// check for door connected areas
		vbsp_leaf_invis++;		//FTESurf: counted before the test, see below
		if (! (refdef->areabits[pleaf->area>>3] & (1<<(pleaf->area&7)) ) )
		{
			vbsp_leaf_areakill++;
			return;		// not visible
		}

		c = pleaf->cluster;
		if (c >= 0)
			frustumvis[c>>3] |= 1<<(c&7);

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				surf = *mark++;
				if (surf->flags & SURF_OFFNODE)
				{
					if (surf->visframe != vbsp_surfsequence)
					{	//only add once, it might be in multiple leafs.
						surf->visframe = vbsp_surfsequence;
						modfuncs->RenderDynamicLightmaps (surf);
						VBSP_EMIT_SURF(surf);
					}
				}
				else
					surf->visframe = vbsp_surfsequence;
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
		dot = modelorg[0] - plane->dist;
		break;
	case PLANE_Y:
		dot = modelorg[1] - plane->dist;
		break;
	case PLANE_Z:
		dot = modelorg[2] - plane->dist;
		break;
	default:
		dot = DotProduct (modelorg, plane->normal) - plane->dist;
		break;
	}

	if (dot >= 0)
	{
		side = 0;
		sidebit = 0;
	}
	else
	{
		side = 1;
		sidebit = SURF_PLANEBACK;
	}

// recurse down the children, front side first
	VBSP_RecursiveWorldNode (model, node->children[side]);

	// draw stuff
	for ( c = node->numsurfaces, surf = model->surfaces + node->firstsurface; c ; c--, surf++)
	{
		if (surf->visframe != vbsp_surfsequence)
			continue;

		if ( (surf->flags & SURF_PLANEBACK) != sidebit )
			continue;		// wrong side

		surf->visframe = 0;//vbsp_surfsequence;//-1;

		modfuncs->RenderDynamicLightmaps (surf);

		VBSP_EMIT_SURF(surf);
	}


// recurse down the back side
	VBSP_RecursiveWorldNode (model, node->children[!side]);
}
static qbyte *VBSP_MarkLeaves (model_t *model, int clusters[2])
{
	vbspinfo_t	*prv = (vbspinfo_t*)model->meshinfo;
	mnode_t	*node;
	int		i;

	int cluster;
	mleaf_t	*leaf;
	qbyte *vis;

	int portal = refdef->recurse;
	//FTESurf Patch 273: each recursion level owns a cache slot. Clamped because
	//gl_backend.c stops recursing AT R_MAX_RECURSE, not below it.
	int lvl = (portal < 0) ? 0 : (portal >= R_MAX_RECURSE ? R_MAX_RECURSE-1 : portal);

	if (refdef->forcevis)
	{
		vis = refdef->forcedvis;
		prv->vcache[lvl].vis = NULL;
		if (!vis)	//nettest: forcedvis can be NULL (a degenerate portal/water mesh, or ClusterPVS returning NULL, leaves forcevis set but forcedvis NULL) — the vis[] deref below would SIGSEGV. Fall through to the whole-model "all surfaces" path (VBSP_PrepareFrame handles surfvis==NULL). This is the d1_canals spawn-in water-reflection crash.
			return NULL;
	}
	else if (hl2_novis->ival || clusters[0] == -1 || !model->vis)
		return NULL;	//use some blind whole-model thing
	else
	{
		/*
		FTESurf Patch 271 -- `portal ||` USED TO BE THE FIRST TERM ABOVE, and it
		made the 3D skybox draw the entire map.

		refdef->recurse is non-zero for every recursed view.  Mirrors, portals and
		water all hand one down together with a real PVS in refdef->forcedvis, so
		they take the branch above and are unaffected.  R_DrawSkyroom does NOT:
		gl_warp.c:361,364 clear forcevis and forcedvis before recursing, so the
		skyroom arrived here with recurse=1 and nothing else, hit `portal ||`,
		and returned NULL -- which VBSP_PrepareFrame answers by emitting EVERY
		surface in the world model (:6595-6601 below) with no PVS, no frustum and
		no area test.

		MEASURED on surf_tensor2, whose sky_camera sits in cluster 1534:

		    what the PVS at cluster 1534 admits .......      30 world faces
		    what the skyroom pass emitted .............  24,762
		    the worldspawn model's face count .........  24,762   <- exactly
		    what the MAIN view emitted, same frame ....     506

		824x what it should draw, and 49x the real view, on every frame.  The
		engine's own [world] census (r_surf.c:3994) prints `vis yes` for that pass
		and is NOT the tell -- VBSP_PrepareFrame reports *surfvis_out = frustumvis
		(:6627), which is never NULL.  The mesh COUNT is the tell, and it matched
		nummodelsurfaces to the face.

		    spawn      236 -> 545 fps    draw indices 304,787 -> 12,605
		    stage 5    320 -> 1061 fps
		    surf_tensor (no sky_camera, 61% of the geometry)   1337 fps

		THE FIX IS Q1BSP'S, VERBATIM IN SHAPE.  Q1BSP_MarkLeaves does not bail on
		a recursed view: it skips the cache, POISONS it, and computes a real PVS
		anyway (q1bsp.c:2225-2242).  That is all this does.  The poison is the
		load-bearing half -- prv->vcache.visbuf is one buffer shared by every
		recursion level, so a recursed view necessarily overwrites what the main
		view computed, and leaving vcache.vis pointing at it would serve the sky
		camera's PVS to the player's own view on the next frame.

		Why overwriting it mid-frame is nonetheless safe: the main view finishes
		consuming its own PVS before the skyroom can run.  entvis is read at
		r_surf.c:4005 (CL_LinkStaticEntities), and BE_DrawWorld -- which is what
		reaches gl_backend.c:7413 and calls R_DrawSkyroom -- is not until :4020.
		surfvis is already per-recursion (surf_frustumvis[r_refdef.recurse],
		r_surf.c:3957) and was never the problem.

		Costs one extra PVS decompression per frame on a map with a skyroom,
		against 24,762 surfaces.  A sky_camera in solid still gives cluster -1 and
		still takes the whole-model path above, so the degenerate case is exactly
		what shipped before.
		*/
		/*
		PATCH 273 REPLACES THE POISON ABOVE WITH SOMETHING THAT CACHES BOTH VIEWS.

		271's poison existed for one reason, which its own text states: the ONE
		visbuf was shared by every recursion level, so leaving vcache.vis pointing
		at it after a recursed view would serve the sky camera's PVS to the
		player's view on the next frame.  vcache is now per level, so that hazard
		is gone and both views can keep their decompressed PVS.

		BUT NOT THEIR NODE MARKS, and this is the trap.  node->visframe is one int
		and vbsp_nodesequence is one global, so the tree can only hold ONE view's
		marks at a time -- whichever marked last.  Simply returning early on a vis
		hit would let the main view draw against the sky camera's marks, which is
		the exact regression the poison prevented and would look like chunks of the
		map missing.  q1bsp.c:2211-2213 documents the same constraint.

		So the two halves are cached separately, which is what they always should
		have been:

		  the PVS decompression   per level, hit whenever that level's cluster
		                          pair is unchanged
		  the node marking        redone whenever the tree's marks belong to a
		                          different level than the one asking

		On a sky_camera map that still re-marks twice a frame -- but a re-mark is
		now ~82 leaf visits instead of 19,846, because of the cluster buckets
		below, so it no longer matters.  Making the marking cheap is what makes
		keeping the marks unnecessary.
		*/
		vis = prv->vcache[lvl].vis;
		if (vis &&
			prv->vcache[lvl].viewcluster[0] == clusters[0] &&
			prv->vcache[lvl].viewcluster[1] == clusters[1])
		{
			if (vbsp_markowner == lvl)
				return vis;			//our PVS *and* our marks: nothing to do
			goto remark;			//our PVS, someone else's marks
		}

		if (clusters[1] != clusters[0])	// may have to combine two clusters because of solid water boundaries
		{
			vis = VBSP_ClusterPVS (model, clusters[0], &prv->vcache[lvl].visbuf, PVM_REPLACE);
			vis = VBSP_ClusterPVS (model, clusters[1], &prv->vcache[lvl].visbuf, PVM_MERGE);
		}
		else
			vis = VBSP_ClusterPVS (model, clusters[0], &prv->vcache[lvl].visbuf, PVM_FAST);

		prv->vcache[lvl].vis = vis;
		prv->vcache[lvl].viewcluster[0] = clusters[0];
		prv->vcache[lvl].viewcluster[1] = clusters[1];
	}
remark:
	vbsp_markowner = refdef->forcevis ? -1 : lvl;	//forcevis marks belong to nobody

	vbsp_nodesequence++;
	vbsp_mark_runs++;

	/*
	FTESurf Patch 273 -- THIS LOOP WAS O(WHOLE MAP) TO LIGHT UP 3.6% OF IT.

	Marking is proportional to what the PVS ADMITS, not to how big the map is, so
	walk the visible clusters instead of every leaf.  The cluster->leaf buckets
	are built once at the tail of VBSP_LoadLeafs.

	WHY IT MATTERS SO MUCH MORE ON SOURCE MAPS THAN IT LOOKS.  The cache above
	returns before this loop, so a stationary camera normally pays nothing --
	surf_tensor measures 9.93us of `World walking`.  But a map with a sky_camera
	renders TWO world views per frame, and the recursed one skips the cache and
	poisons it (:6465-6470, Patch 271, and correct):

	    [world] recurse 0: area 22 cluster 6000 vis yes -> 506 meshes
	    [world] recurse 1: area  8 cluster 1534 vis yes ->  20 meshes

	so on surf_tensor2 NEITHER view can ever hit, and this loop ran twice a frame,
	standing perfectly still, for 39,692 leaf visits over 2.06MB of mleaf_t --
	each hit then chasing node->parent through a separate 1.75MB array.  `visframe`
	is at offset 4 and `cluster` at offset 72, different cache lines, so a hit
	dirties a line the scan has already passed.

	What it becomes at that same spawn: 762 byte tests (most zero, rejecting eight
	clusters each), ~219 bucket lookups and ~714 leaf visits.  The PVS row itself
	is 12 cache lines.

	THE MARKED SET IS IDENTICAL, which is the whole point -- this is arithmetic,
	not a policy change.  Every leaf with a set cluster bit is in exactly one
	bucket; leaves with cluster -1 were skipped by the old loop and are never put
	in a bucket by the precompute.  If the two ever disagree, this is a bug and
	not a tuning question.

	NOT DONE, deliberately: giving each recursion level its own cache slot.  That
	needs node->visframe to become per-recursion, and R_MAX_RECURSE is 6
	(render.h:272) on a field shared with Q1/Q2/Doom -- +20 bytes on both mnode_t
	and mleaf_t, ~790KB here, an engine<->plugin ABI break, and it WIDENS the hot
	field, making the cache behaviour above worse.  q1bsp.c:2211-2213 explains the
	same trap ("we only have one 'visframe' field in nodes"), and gl_q2bsp.c has
	the multi-slot cache already (:7831-7832) and is still wrong for this reason.
	With the buckets, the double-marking simply stops being expensive.
	*/
	if (prv->clusterfirstleaf)
	{
		int nc = model->numclusters;
		int nb = (nc + 7) >> 3;		//exactly the bytes the old loop could reach
		int b, bit;
		unsigned int k, e;

		for (b = 0; b < nb; b++)
		{
			int bits = vis[b];
			if (!bits)
				continue;			//eight clusters rejected by one test
			for (bit = 0; bit < 8; bit++)
			{
				if (!(bits & (1<<bit)))
					continue;
				cluster = (b<<3) + bit;
				if (cluster >= nc)
					break;			//padding bits in the final byte
				k = prv->clusterfirstleaf[cluster];
				e = prv->clusterfirstleaf[cluster+1];
				vbsp_mark_leaves += e - k;
				for ( ; k < e; k++)
				{
					node = (mnode_t *)&model->leafs[prv->clusterleafs[k]];
					do
					{
						if (node->visframe == vbsp_nodesequence)
							break;
						node->visframe = vbsp_nodesequence;
						node = node->parent;
					} while (node);
				}
			}
		}
	}
	else
	for (i=0,leaf=model->leafs ; i<model->numleafs ; i++, leaf++)
	{
		cluster = leaf->cluster;
		if (cluster == -1)
			continue;
		vbsp_mark_leaves++;
		if (vis[cluster>>3] & (1<<(cluster&7)))
		{
			node = (mnode_t *)leaf;
			do
			{
				if (node->visframe == vbsp_nodesequence)
					break;
				node->visframe = vbsp_nodesequence;
				node = node->parent;
			} while (node);
		}
	}
	return vis;
}

/*
=================
HL2_RetintFromBaked							FTESurf Patch 163

The prop's own bake decides the colour and the level.  The leaf ambient cube
keeps only one job: which way the light comes from.

FTE lights a model as `ambient + max(dot(n,dir),0) * mul` (defaultskin.glsl:75-96
-- e_light_ambient is light_avg and e_light_mul is light_range, and yes those two
are the opposite way round from the names in VBSP_LightPointValues' own
signature).  Averaged over every possible normal that expression comes to
ambient + 0.1761*mul: a quarter from the lit hemisphere, minus a quarter of the
13/44 defaultskin applies to the unlit one.  So the split below is chosen to put
that average exactly on the baked mean.

WHY THE SPLIT IS FIXED AND MOSTLY AMBIENT, rather than borrowed from the cube.
VRAD's per-vertex bake has ALREADY integrated the directional lighting -- each
vertex colour is the finished answer for that vertex, cosine and all, and Source
applies no further N.L to it.  Reproducing our single mean as a strongly
directional term would therefore apply the shading twice: bright where the
cube's dominant direction happens to point and near-black on the other side of
the same prop, neither of which VRAD said.  Most of it has to come back as a
constant.

FS_BAKED_DIR is a presentation choice and is named as one.  At 0 the prop is
exactly its baked mean from every angle -- most faithful to the data, and flat
enough that a cylinder reads as a silhouette.  0.30 keeps enough form for the
geometry to be legible while leaving 70% of the light where the bake put it.

A consequence worth stating: this OVERRIDES hl2_lt_min for props that have a
bake, because it sets the absolute level rather than scaling it.  That is
correct -- the floor exists to rescue props whose lighting we could not
determine, and for these we now can.

FTESurf Patch 259 MAKES THE SPLIT AN ARGUMENT, AND PASSES 0 FOR PER-VERTEX
PROPS.  Every word of the paragraph above about applying the shading twice is
true; the split survives only because a single mean gives the geometry no other
way to read as solid.  When the prop carries its own per-vertex bake it has
that form already -- VRAD's, cosine and occlusion and all -- so the honest
directional fraction there is exactly zero, and any non-zero value would be
FTE's N.L multiplied on top of VRAD's.
=================
*/
#define FS_BAKED_DIR 0.30f
static void HL2_RetintFromBaked(entity_t *e, const vec3_t baked, float dirfrac)
{
	const float dmean = 0.1761f;
	vec3_t m;
	VectorScale(baked, 1/255.0f, m);
	VectorScale(m, 1.0f - dirfrac,          e->light_avg);
	VectorScale(m, dirfrac / dmean,         e->light_range);
}

qboolean HL2_CalcModelLighting(entity_t *e, model_t *clmodel, refdef_t *r_refdef, model_t *mod)
{
	vec3_t lightdir;
	vec3_t shadelight, ambientlight;

	e->light_dir[0] = 0; e->light_dir[1] = 1; e->light_dir[2] = 0;
	mod->funcs.LightPointValues(mod, e->origin, shadelight, ambientlight, lightdir);

	e->light_dir[0] = DotProduct(lightdir, e->axis[0]);
	e->light_dir[1] = DotProduct(lightdir, e->axis[1]);
	e->light_dir[2] = DotProduct(lightdir, e->axis[2]);

	VectorNormalize(e->light_dir);

	shadelight[0] *= 1/255.0f;
	shadelight[1] *= 1/255.0f;
	shadelight[2] *= 1/255.0f;
	ambientlight[0] *= 1/255.0f;
	ambientlight[1] *= 1/255.0f;
	ambientlight[2] *= 1/255.0f;

	//calculate average and range, to allow for negative lighting dotproducts
	VectorCopy(shadelight, e->light_avg);
	VectorCopy(ambientlight, e->light_range);

	//FTESurf Patch 268 C: and the six faces themselves, for #BUMPCUBE.  Filled whatever
	//hl2_cubelight says -- that cvar decides what is compiled, and this runs once per prop
	//for the life of the map inside the light_known cache, so skipping it saves nothing.
	//Sampled at the same point as the lines above, which for a static prop is its lighting
	//origin (the caller swaps it into e->origin for exactly this call).
	if (mod->funcs.LightPointValues == VBSP_LightPointValues)
		VBSP_LightPointCube(mod, e->origin, e->light_cube);
	else
		memset(e->light_cube, 0, sizeof(e->light_cube));

	e->light_known = 1;
	return e->light_known-1;
}

static void VBSP_PrepareFrame(model_t *mod, refdef_t *r_refdef, int area, int clusters[2], pvsbuffer_t *vis, qbyte **entvis_out, qbyte **surfvis_out)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	qbyte *surfvis, *entvis;

	refdef = r_refdef;

	if (vis->buffersize < mod->pvsbytes)
		vis->buffer = plugfuncs->Realloc(vis->buffer, vis->buffersize=mod->pvsbytes);
	frustumvis = vis->buffer;
	memset(frustumvis, 0, mod->pvsbytes);

	if (!r_refdef->areabitsknown)
	{	//generate the info each frame, as the gamecode didn't tell us what to use.
		int leafnum = VBSP_PointLeafnum (mod, r_refdef->vieworg);
		int clientarea = VBSP_LeafArea (mod, leafnum);
		VBSP_WriteAreaBits(mod, r_refdef->areabits, clientarea, false);
		r_refdef->areabitsknown = true;

		/*
		FTESurf Build 8.  AREA CULLING IS NOT A THEORY, so print what it is
		doing the moment it changes.

		VBSP_RecursiveWorldNode refuses any leaf whose area bit is clear, and
		the areaportals that connect areas start CLOSED (map_autoopenportals
		defaults to 0) and are opened only by a func_areaportal spawn function.
		A map with real areas and no such entity therefore draws ONE AREA AND
		NOTHING ELSE -- surf_666 has fourteen.

		Area 0 is the exception and is why this is not always catastrophic:
		VBSP_WriteAreaBits sends everything when the camera is in the null area.

		Printed on a change rather than per frame, so walking a map traces out
		which areas exist and how much each one can see.  developer 1.
		*/
		/* Rate-limited, not just edge-detected.  Standing on an area boundary
		   flips the camera's leaf every frame, and an edge-detect alone then
		   prints twice per frame forever -- which is the same mistake as the
		   `waits without server frames` latch this build also fixes. */
		if (clientarea != prv->lastreportedarea &&
		    r_refdef->time >= prv->nextareareport)	//Con_DPrintf gates on developer itself
		{
			int i, n = 0;
			for (i = 0; i < (int)prv->numareas; i++)
				if (r_refdef->areabits[i>>3] & (1<<(i&7)))
					n++;
			prv->lastreportedarea = clientarea;
			prv->nextareareport = r_refdef->time + 2;
			Con_DPrintf("area %i of %u: %i area(s) reachable%s\n",
				clientarea, (unsigned)prv->numareas,
				(clientarea == 0) ? (int)prv->numareas : n,
				(clientarea == 0) ? " (null area -- everything sent)" : "");
		}

		/* And the number that decides whether any of that COSTS anything:
		   leafs the vis and frustum tests already accepted, which the area test
		   then threw away.  Accumulated rather than sampled -- the per-frame
		   figure is noise, and the question is whether it is ever non-zero.
		   Reported once, a few seconds in, so it lands after the map has
		   settled and does not repeat. */
		/* FTESurf Patch 273.  runs = how many times the vis cache MISSED and had to
		   re-mark (2 per frame on a sky_camera map, ~0 on a map without one while
		   standing still); leaves = how many leaves those runs touched.  Before the
		   cluster buckets this was runs*numleafs by construction.

		   REPEATING, unlike the area census beside it, and that is the point: the
		   number is a property of the VANTAGE, not of the map.  A one-shot report at
		   frame 300 would only ever describe wherever the player happened to spawn,
		   and the vantage this patch most needs to check is the widest view on the
		   map -- where most clusters are visible and the bucket walk could in
		   principle be SLOWER than the loop it replaces.  Counters reset with each
		   report so every window stands alone. */
		if (++prv->markstatframes >= 300)
		{
			Con_DPrintf("mark/300f: %i runs, %i leaf visits (%.1f per run, of %i leafs; index %s)\n",
				vbsp_mark_runs, vbsp_mark_leaves,
				vbsp_mark_runs ? vbsp_mark_leaves/(float)vbsp_mark_runs : 0.0f,
				(int)mod->numleafs, prv->clusterfirstleaf?"ON":"off (fallback loop)");
			prv->markstatframes = 0;
			vbsp_mark_runs = vbsp_mark_leaves = 0;
		}
		if (++prv->areastatframes == 300)
			Con_DPrintf("area cull over 300 frames: dropped %i of %i vis-passing leafs (%.1f%%)\n",
				vbsp_leaf_areakill, vbsp_leaf_invis,
				vbsp_leaf_invis ? (100.0*vbsp_leaf_areakill)/vbsp_leaf_invis : 0.0);
	}


	entvis = surfvis = VBSP_MarkLeaves(mod, clusters);
	VectorCopy (r_refdef->vieworg, modelorg);
	if (!surfvis)
	{
		size_t i;
		msurface_t *surf;
		for (i = 0; i < mod->nummodelsurfaces; i++)
		{
			surf = &mod->surfaces[i];
			modfuncs->RenderDynamicLightmaps (surf);
			VBSP_EMIT_SURF(surf);
		}
	}
	else
	{
		size_t i;
		dispinfo_t *disp;
		msurface_t *surf;
		int areas[2];
		areas[0] = 1;
		areas[1] = area;
		vbsp_surfsequence++;
		VBSP_RecursiveWorldNode (mod, mod->nodes);
		for (i = 0; i < prv->numdisplacements; i++)
		{
			disp = &prv->displacements[i];
			if (VBSP_EdictInFatPVS(mod, &disp->pvs, surfvis, areas))
			{
				surf = disp->surf;
				if (surf->visframe == vbsp_surfsequence)
					continue;	//nettest: already emitted by the leaf walk this frame — re-adding would overrun surf->sbatch->mesh[] (sized once per surface) and corrupt the heap. Dedup like the leaf walk.
				surf->visframe = vbsp_surfsequence;
				modfuncs->RenderDynamicLightmaps (surf);
				VBSP_EMIT_SURF(surf);
			}
		}
	}

	*surfvis_out = frustumvis;
	*entvis_out = entvis;



	if (prv->numstaticprops)
	{
		struct staticprop_s *sent;
		entity_t *src, *ent;
		float d;
		size_t i;
		vec3_t disp;

		/*
		FTESurf Patch 152 -- the prop-lighting census, and WHY IT IS HERE.

		Not at load time.  VBSP_PointLeafnum returns 0 unconditionally while
		`mod->loadstate != MLS_LOADED`, and the load worker that would be the
		obvious home for this runs before that -- so a census taken there
		reports the solid leaf for every prop and is indistinguishable from a
		map with no ambient data at all.  I chased that artifact for two
		rebuilds before noticing the guard.

		And not inside the per-prop lighting block below either, because that
		only ever sees props the PVS and frustum let through -- on a camera
		looking at a wall it reports four props and tells you nothing.  One
		explicit pass over ALL of them, once, the first frame the model is up.

		  solid  = props whose leaf carries no ambient cube
		  floor  = props the hl2_lt_min lift had to touch
		  R/B    = mean red over mean blue, which is what a HUE bug moves and
		           what a brightness bug cannot
		*/
		if (hl2_texdiag && hl2_texdiag->ival && !prv->lit_reported &&
			mod->loadstate == MLS_LOADED)
		{
			float mn = 0;
			//FTESurf Patch 296: the second column, which is what this line
			//printed BEFORE 296.  Kept so the first run after the change is
			//self-checking -- it has to reproduce the old numbers exactly while
			//the first column drops to the level that is actually drawn.
			double pk_r = 0, pk_g = 0, pk_b = 0;
			//Default MUST match the one in VBSP_LightPointValues: GetNVFDG
			//creates the cvar on first call, and whichever of the two runs
			//first decides its default for the session.
			cvar_t *minamb = cvarfuncs->GetNVFDG("hl2_lt_min", "16", 0, "", "");
			if (minamb) mn = minamb->value;
			prv->lit_reported = true;

			for (i = 0; i < prv->numstaticprops; i++)
			{
				vec3_t dif, amb, dir, sample;
				vec3_t lev, old;
				float lum;

				// The SAME choice the draw path makes, or the census measures
				// something nobody looks at.
				VectorCopy(prv->staticprops[i].lightorg, sample);
				if (!VBSP_LeafHasAmbient(mod, sample))
				{
					VectorCopy(prv->staticprops[i].ent.origin, sample);
					if (!VBSP_LeafHasAmbient(mod, sample))
						prv->lit_solid++;
				}

				VBSP_LightPointValues(mod, sample, dif, amb, dir);

				//FTESurf Patch 163: report the colour that is DRAWN, not the one
				//the cube proposed.  A census of a value the renderer overrides
				//is exactly the kind of measurement that sent Build 13 chasing
				//the wrong thing for a build.
				//
				//FTESurf Patch 296: ...and 163 then reported a value nothing
				//draws either, in TWO independent ways, for four months.
				//
				//  1. sent->baked has already been multiplied by the per-prop
				//     peak gain at :4670, so it is the value of the prop's
				//     BRIGHTEST vertex, not its level.  The draw divides it
				//     straight back out (:8469, and the v_colour multiply in
				//     vertexlit.glsl), so the census was inflated by a factor
				//     that varies per prop -- mean 20.98 on surf_boreas, 2.05
				//     on surf_fantasy.  That is why boreas' props "measured"
				//     3.2x fantasy's while storing 4.6x DARKER bytes.
				//  2. for a prop with no bake it accumulated res_ambient, the
				//     cube's six-face mean -- but HL2_CalcModelLighting puts
				//     res_DIFFUSE into light_avg (:7906), which is the shader's
				//     base, and res_ambient into light_range.  res_diffuse is
				//     res_ambient plus the dominant-face deviation, so the
				//     no-bake population was under-reported by exactly the
				//     directional boost at the same time as the baked one was
				//     over-reported by the gain.
				//
				//The two errors point opposite ways, which is precisely why
				//comparing a baked map against an unbaked one through this line
				//could not be corrected by scaling either side.
				if (prv->staticprops[i].hasbaked)
				{
					//vcgain is seeded to 1 at :5225 and only ever written at
					//:4668, so the divide is safe on every prop including every
					//4-byte one at hl2_lt_baked 1.
					float g = prv->staticprops[i].vcgain;
					if (g <= 0) g = 1;
					VectorCopy(prv->staticprops[i].baked, old);
					VectorScale(prv->staticprops[i].baked, 1.0f/g, lev);
				}
				else
				{
					VectorCopy(amb, old);
					VectorCopy(dif, lev);
				}

				prv->lit_r += lev[0];
				prv->lit_g += lev[1];
				prv->lit_b += lev[2];
				prv->lit_n++;
				pk_r += old[0];
				pk_g += old[1];
				pk_b += old[2];

				lum = 0.3f*amb[0] + 0.59f*amb[1] + 0.11f*amb[2];
				if (mn > 0 && lum <= mn*1.001f && !prv->staticprops[i].hasbaked)
					prv->lit_floor++;
			}

			if (prv->lit_n)
				Con_Printf("[texdiag] PROPLIGHT %s: %u props,"
					" %u lit from their own VRAD bake,"
					" %u in a leaf with no ambient, %u at the hl2_lt_min floor"
					" -- mean %.1f %.1f %.1f, R/B %.2f"
					" (pre-296 column %.1f %.1f %.1f)\n",
					mod->name, (unsigned)prv->lit_n, (unsigned)prv->lit_baked,
					(unsigned)prv->lit_solid, (unsigned)prv->lit_floor,
					prv->lit_r/prv->lit_n, prv->lit_g/prv->lit_n, prv->lit_b/prv->lit_n,
					(prv->lit_b > 0.001) ? (prv->lit_r/prv->lit_b) : 0.0,
					pk_r/prv->lit_n, pk_g/prv->lit_n, pk_b/prv->lit_n);
		}
		int areas[2];	//nettest: same area set the displacement cull uses, but areas[] is scoped inside the else above, so rebuild it here.
		areas[0] = 1;
		areas[1] = area;

		/*
		FTESurf Patch 259: the live half of the per-vertex switch.

		hl2_lt_baked is CVAR_MAPLATCH because it decides what is READ off disk,
		and that cannot change without reloading the map.  Whether the loaded
		array is USED costs nothing to change, and being able to flip it at the
		same vantage is the difference between an A/B and two screenshots taken
		minutes apart -- so hl2_lt_baked_vc is an ordinary cvar.

		It has to invalidate light_known, because the retint that goes with it is
		computed exactly once per prop for the life of the map (that is the whole
		reason this feature is free per frame) and would otherwise keep the split
		and the gain belonging to the other mode.  One pass over the array on the
		frame the cvar changes, and never again.
		*/
		{
			int want = (hl2_lt_baked_vc && hl2_lt_baked_vc->ival) ? 1 : 0;
			//FTESurf Patch 268 C2: hl2_lt_dirsign changes light_dir inside the same once-per-prop
			//cache, so flipping it has to invalidate that cache exactly as this switch does.  Its
			//own static rather than a bit in vbsp_vc_live, which is read as a plain boolean below.
			static int dirsign_live = -1;
			int wantsign = (hl2_lt_dirsign && hl2_lt_dirsign->ival) ? 1 : 0;
			//FTESurf Patch 296: hl2_lt_baked_dir lands in the same once-per-prop
			//cache, so it needs the same treatment.  Float equality is right --
			//an unchanged cvar yields a bit-identical value -- and the -1 seed
			//is a value the clamp below can never produce, so frame 1
			//invalidates exactly once and never again.
			static float bakeddir_live = -1;
			float wantdir = hl2_lt_baked_dir ? hl2_lt_baked_dir->value : 0.0f;
			if (wantdir < 0) wantdir = 0;
			if (wantdir > 1) wantdir = 1;
			//FTESurf Patch 302: hl2_lt_worldlight lands in this same once-per-prop
			//cache, so it needs the same treatment.  The -1 seed is a value the
			//boolean below can never produce, so frame 1 invalidates exactly once.
			//Without this the cvar would appear to do nothing until a map change,
			//and the A/B would have to be two processes -- which is precisely the
			//shape of test that cannot separate a real change from run-to-run drift.
			static int worldlight_live = -1;
			int wantwl = (hl2_lt_worldlight && hl2_lt_worldlight->ival) ? 1 : 0;
			//FTESurf Patch 304: same cache, same treatment.  The shader half of
			//this cvar reloads itself (CVAR_SHADERSYSTEM, added by gl_shader.c
			//:2294), so without this line a live toggle would swap the RAMP while
			//every prop kept the base and amp the old ramp was solved for -- the
			//one state the single-cvar design exists to make unreachable.
			static int fold_live = -1;
			int wantfold = (hl2_lt_fold && hl2_lt_fold->ival) ? 1 : 0;
			//FTESurf Patch 308: hl2_cubelight now decides whether the worldlight
			//term goes into the cube of a BAKED prop, so it changes solved data and
			//not merely which shader is compiled.  It already regenerates materials
			//(CVAR_SHADERSYSTEM), and without this line the new shader would be
			//handed the cube the old one was solved for -- the same disagreeing-pair
			//failure hl2_lt_fold is written to make unreachable.  r_cubelight, which
			//gates the UPLOAD rather than the solve, needs nothing here and is still
			//the right knob for a live A/B within one arm.
			static int cubelight_live = -1;
			int wantcube = (hl2_cubelight && hl2_cubelight->ival) ? 1 : 0;
			if (want != vbsp_vc_live || vbsp_vc_mod != mod || wantsign != dirsign_live || wantdir != bakeddir_live || wantwl != worldlight_live || wantfold != fold_live || wantcube != cubelight_live)
			{
				cubelight_live = wantcube;
				fold_live = wantfold;
				worldlight_live = wantwl;
				bakeddir_live = wantdir;
				dirsign_live = wantsign;
				vbsp_vc_live = want;
				vbsp_vc_mod = mod;
				for (i = 0; i < prv->numstaticprops; i++)
					prv->staticprops[i].ent.light_known = 0;
			}
		}

		//FTESurf Patch 257: reset the per-frame accounting.  Every prop starts
		//"not visited" so that an early return out of this loop is visible as
		//itself rather than silently reading as culled.
		{
			unsigned int v;
			if (vbsp_propstat.mod != mod)
			{	//new map: the two leaf-overflow counters below are per-map, not
				//per-frame, because the branch that feeds them runs once per prop.
				vbsp_propstat.leafoverflow = 0;
				vbsp_propstat.headnodetested = 0;
			}
			vbsp_propstat.mod = mod;
			vbsp_propstat.total = prv->numstaticprops;
			for (v = 0; v < PROPV_MAX; v++)
				vbsp_propstat.n[v] = 0;
			vbsp_propstat.n[PROPV_UNVISITED] = prv->numstaticprops;
			vbsp_propstat.vcdrawn = vbsp_propstat.vcdeclined = 0;	//FTESurf Patch 259
			for (i = 0; i < prv->numstaticprops; i++)
				prv->staticprops[i].lastverdict = PROPV_UNVISITED;
		}
#define PROP_VERDICT(v) do { \
			vbsp_propstat.n[PROPV_UNVISITED]--; \
			vbsp_propstat.n[v]++; \
			sent->lastverdict = (v); \
		} while(0)

		for (i = 0; i < prv->numstaticprops; i++)
		{
			sent = &prv->staticprops[i];
			src = &sent->ent;

			if (sent->fademaxdist)
			{
				VectorSubtract(refdef->vieworg, src->origin, disp);
				d = VectorLength(disp);
				if (d > sent->fademaxdist)
				{
					PROP_VERDICT(PROPV_FADED);
					continue;	//skip it.
				}
				d -= sent->fademindist;
				d /= sent->fademaxdist-sent->fademindist;
				if (d < 0)
					d = 0;
			}
			/*
			FTESurf Patch 193: a default draw distance for props that declare none.

			Measured on surf_demise, standing at its own info_player_start with
			r_speeds 2: 23.2 fps, of which Opaque Batches is 26.9ms of a 43.0ms
			frame, 5,230,080 draw indices, 1453 Ent Batches against 59 World
			Batches.  r_drawentities 3 (skip mod_alias, i.e. exactly the static
			props -- mod_hl2.c:1021) gives 271.4 fps, 1,119,782 indices, 57 Ent
			Batches.  So the props are 4.11M of the 5.2M indices and 25.6ms of the
			26.9ms, and everything else on the map together is a 3.7ms frame.
			r_drawentities 2 (skip brush entities instead) gives 22.9 fps -- no
			change -- so brush entities cost nothing here and batching them, the
			originally proposed fix, would have bought nothing.

			Why they are all drawn: 2350 of surf_demise's props, which is ALL of
			them, carry FadeMaxDist 0.  The block above is the only distance test
			there is, so a prop 24,000 units away is submitted at full detail --
			and the median prop distance from that viewpoint is 24,185 units.
			Source would not draw them either: a prop with no fade distance of its
			own falls back to the engine's r_propsmaxdist, so having no limit at
			all is our divergence, not the map author's intent.  (FTE also has no
			MDL LOD support -- mod_hl2.c:11 "FIXME: no lod stuff", :381 "must
			remain at 1 until fixups are handled" -- so every one of those distant
			props is drawn at LOD0.  Fixing that is the better answer and a much
			bigger one; this is the cheap half.)

			Default 0 = OFF, i.e. exactly the behaviour that shipped before, because
			this trades draw distance for framerate and which side of that trade a
			player wants is theirs to pick -- on a surf map the far scenery is often
			the view.  Props that set their own fade distance are untouched either
			way; this only supplies a limit where the map supplied none.
			*/
			else if (hl2_propdist && hl2_propdist->value > 0)
			{
				VectorSubtract(refdef->vieworg, src->origin, disp);
				if (VectorLength(disp) > hl2_propdist->value)
				{
					PROP_VERDICT(PROPV_PROPDIST);
					continue;
				}
				d = 0;
			}
			else
				d = 0;

			if (!src->model || src->model->loadstate != MLS_LOADED)
			{
				if (src->model && src->model->loadstate == MLS_NOTLOADED)
					modfuncs->GetModel(src->model->publicname, MLV_WARN);	//we use threads, so these'll load in time.
				PROP_VERDICT(PROPV_NOTLOADED);
				continue;
			}

			//nettest: cull invisible props. Must run BEFORE NewSceneEntity (and before the lighting calc, which is pointless for culled props).
			//pvscache must be valid first: props whose leaf set overflowed at load were flagged num_leafs==-2 and need VBSP_FindTouchedLeafs to fill it in.
			//Moved this populate up from below the lighting block so the PVS test sees real leaf/area data.
			if (src->pvscache.num_leafs==-2)
			{
				vec3_t absmin, absmax;
				float r = src->model->radius;
				VectorSet(absmin, -r,-r,-r);
				VectorSet(absmax, r,r,r);
				VectorAdd(absmin, src->origin, absmin);
				VectorAdd(absmax, src->origin, absmax);
				VBSP_FindTouchedLeafs(mod, &src->pvscache, absmin, absmax);
				sent->nareas = (vbsp_lasttouch_nareas > 255) ? 255 : (qbyte)vbsp_lasttouch_nareas;
				//FTESurf Patch 257: count it.  This branch runs once per prop for
				//the life of the map (num_leafs stops being -2 here), so the two
				//counters below are a per-MAP property, not a per-frame one.
				vbsp_propstat.leafoverflow++;
				if (src->pvscache.num_leafs == -1)
					vbsp_propstat.headnodetested++;
			}

			//PVS/area cull, exactly like the displacement loop above (VBSP_EdictInFatPVS at ~3587).
			//Only valid when we actually have view PVS: surfvis==NULL means portal-recursion / novis / no model vis, where everything must draw.
			//src->pvscache.leafnums hold CLUSTERS, and surfvis is the cluster-PVS from VBSP_MarkLeaves, so this is index-consistent (same as displacements).
			if (surfvis && !VBSP_EdictInFatPVS(mod, &src->pvscache, surfvis, areas))
			{
				sent->lastpvswhy = vbsp_pvs_why;	//FTESurf Patch 257: set by the call above
				PROP_VERDICT(PROPV_PVS);
				continue;	//prop's leaves aren't in the view PVS and its area can't be reached — not visible.
			}

			//cheap frustum cull on the model's radius box (props through the portal still pass PVS above, so this only drops what's off-screen).
			{
				vec3_t cmin, cmax;
				float r = src->model->radius;
				VectorSet(cmin, src->origin[0]-r, src->origin[1]-r, src->origin[2]-r);
				VectorSet(cmax, src->origin[0]+r, src->origin[1]+r, src->origin[2]+r);
				if (VBSP_CullBox(cmin, cmax))
				{
					PROP_VERDICT(PROPV_FRUSTUM);
					continue;
				}
			}

#if 1
			if (!src->light_known)
			{
				/*
				FTESurf Patch 152: choose the sample point BEFORE sampling, on
				whether it has anything to sample.

				This used to sample the lighting origin and then re-sample at
				the prop's own origin "if it's dark":

				    if (VectorLength(src->light_range) < 0.25)
				        HL2_CalcModelLighting(...);

				-- the right idea, which could never fire.  A miss does not come
				back dark: VBSP_LightPointValues answers a flat 192 when the leaf
				carries no ambient cube, and 192/255 = 0.75 is three times that
				threshold.  So the fallback was dead code and every prop whose
				lighting origin landed in solid stayed grey.

				Asking the leaf directly is both correct and cheaper -- one tree
				walk instead of a second full lighting solve -- and it separates
				"no data here" from "genuinely dark", which a brightness test
				cannot do.  A prop deep inside geometry can still miss both
				points; that is a real property of the map's leaf partition and
				it falls back to hl2_lt_min, which is what that is for.
				*/
				vec3_t tmp, sample;

				VectorCopy(sent->lightorg, sample);
				if (!VBSP_LeafHasAmbient(mod, sample))
					VectorCopy(src->origin, sample);

				VectorCopy(src->origin, tmp);
				VectorCopy(sample, src->origin);
				/*
				FTESurf Patch 302 -- THE CONTROL ARM, ENFORCED BY CONSTRUCTION.

				A prop with a .vhv already carries VRAD's direct light inside the
				bake, so adding the worldlight term to it would count the same
				lights twice.  It is tempting to argue the point is moot --
				HL2_RetintFromBaked OVERWRITES light_avg and light_range rather
				than accumulating (:8026-8027), so the inflated values would be
				discarded a few lines below -- but two things survive that
				overwrite: light_dir, which the bake path deliberately keeps from
				the cube solve, and light_cube[6], which #AMBIENTCUBE and
				#BUMPCUBE read.  On surf_boreas that is 1587 of 1587 props whose
				lighting is already correct.

				So the suppression is real and it is set here rather than tested
				downstream, because here is the only place that knows which
				population this call belongs to.  Cleared immediately after: every
				OTHER caller of LightPointValues -- players, viewmodels, any
				non-prop model -- SHOULD get the direct term, exactly as Source's
				light cache gives it to them.
				*/
				prv->wl_suppress = sent->hasbaked;
				//FTESurf Patch 304: and the split fold, on the same two conditions
				//and for the same two reasons.  A static prop is the only model
				//guaranteed to render through vertexlit.glsl, which is the only
				//shader carrying the matching half-lambert ramp; and a prop with a
				//.vhv already has VRAD's own shading per vertex, so re-splitting
				//the cube under it would shade it twice.
				prv->fold_prop = !sent->hasbaked;
				HL2_CalcModelLighting(src, src->model, r_refdef, mod);
				prv->wl_suppress = false;
				prv->fold_prop = false;
				VectorCopy(tmp, src->origin);

				//FTESurf Patch 163: VRAD already answered this question for this
				//exact prop.  Applied after, because it replaces the cube's
				//colour and level while keeping its direction and its split.
				if (sent->hasbaked)
				{
					/*
					FTESurf Patch 259.  Both halves of the per-vertex mode are
					decided here, in the block light_known already caches, so the
					per-frame cost of the whole feature is the two pointer stores
					below and nothing else.

					vc_live is the live switch: hl2_lt_baked chooses what gets
					LOADED and is latched to the map, but once the array is in
					memory turning the multiply off is free, and being able to
					A/B it against mode 1 without reloading the map is the point.
					The lighting solve is cached, so flipping it has to invalidate
					that -- see the vc_lastlive block above the prop loop.

					The directional fraction goes to 0 when the per-vertex data is
					actually bound, because VRAD's bake already contains the
					shading; see the essay on HL2_RetintFromBaked.  When it is not
					bound we are back to a single mean and the 0.30 split is what
					keeps the geometry legible.
					*/
					/*
					THE SCATTER, which happens here because here is the first
					point at which both halves exist.

					The .vhv is read at map load, when the model is still a name;
					models load on worker threads and are only guaranteed up at
					MLS_LOADED, which the loop has already checked above.  So the
					colours sit in VRAD's order until the first frame that lights
					the prop, and this moves them into ours -- once per prop for
					the life of the map, inside the same light_known cache that
					makes the rest of this free.

					Every vertex the strip groups never name is prefilled with the
					byte that reproduces the plain mean, 255/gain, so an
					unreferenced vertex draws at exactly what hl2_lt_baked 1 would
					give it rather than at the peak-normalised level.  Without
					that prefill an uncovered vertex would come out about 1/gain
					too bright, which on these models is most of a stop.
					*/
					if (sent->vcraw && !sent->vc && src->model->meshinfo)
					{
						const galiasinfo_t *first = (const galiasinfo_t*)src->model->meshinfo;
						if (first->vhvmap && first->vhvmodelverts > 0 && first->firstvert >= 0)
						{
							unsigned int nv = (unsigned int)first->vhvmodelverts;
							unsigned int nm = (unsigned int)first->vhvmapcount;
							int meanb = (int)(255.0f/sent->vcgain + 0.5f);
							qbyte *out;
							unsigned int k;
							if (meanb < 0) meanb = 0;
							if (meanb > 255) meanb = 255;
							out = plugfuncs->GMalloc(&mod->memgroup, nv*4);
							for (k = 0; k < nv; k++)
							{
								out[k*4+0] = out[k*4+1] = out[k*4+2] = (qbyte)meanb;
								out[k*4+3] = 255;
							}
							if (nm > sent->vcrawverts)
								nm = sent->vcrawverts;	//a shorter file colours what it can
							for (k = 0; k < nm; k++)
							{
								unsigned int d = (unsigned int)first->vhvmap[k];
								if (d >= nv)
									continue;			//already rejected at build time; belt
								out[d*4+0] = sent->vcraw[k*4+0];
								out[d*4+1] = sent->vcraw[k*4+1];
								out[d*4+2] = sent->vcraw[k*4+2];
							}
							sent->vc = out;
							sent->vcverts = nv;
						}
						plugfuncs->Free(sent->vcraw);	//either scattered, or this model will never take it
						sent->vcraw = NULL;
					}

					if (sent->vc && vbsp_vc_live)
					{
						//FTESurf Patch 296: the 0.0f is now a cvar, still
						//defaulting to 0.0f.  It is here rather than hard-coded
						//because this is the one arm the bumped-rock report
						//lands on, and being able to move it live at a fixed
						//vantage is worth more than the literal.  It cannot fix
						//that report -- the term is per-VERTEX against the
						//geometric normal (vertexlit.glsl:109-123), so no value
						//of it reaches a normal map -- but it does decide
						//whether a baked prop has any silhouette at all.
						float bd = hl2_lt_baked_dir ? hl2_lt_baked_dir->value : 0.0f;
						if (bd < 0) bd = 0;
						if (bd > 1) bd = 1;	//above 1 makes light_avg negative
						src->vertlightbytes = sent->vc;
						src->vertlightverts = (int)sent->vcverts;
						HL2_RetintFromBaked(src, sent->baked, bd);
					}
					else
					{
						/*
						Falling back to the mean, which means undoing the peak gain:
						sent->baked has carried it since the .vhv loaded, whether or
						not the scatter ever succeeded.  Keyed on the gain and not on
						sent->vc, because a prop whose model turned out to have no
						scatter table has the gain and no array -- and reading that as
						mode 1 without dividing would draw it most of a stop too
						bright, which is the one thing the fallback must not do.
						vcgain is 1 whenever no gain was applied, so this is safe to
						do unconditionally.
						*/
						vec3_t mean;
						src->vertlightbytes = NULL;
						src->vertlightverts = 0;
						VectorScale(sent->baked, 1.0f/sent->vcgain, mean);
						HL2_RetintFromBaked(src, mean, FS_BAKED_DIR);
					}
				}
			}
#endif

			ent = modfuncs->NewSceneEntity();
			if (!ent)
				return;		//scene entity budget spent; the rest stay PROPV_UNVISITED, which is what happened
			*ent = *src;
			ent->framestate.g[FS_REG].frametime[0] = refdef->time;
			ent->framestate.g[FS_REG].frametime[1] = refdef->time;
			if (d)
			{
				ent->shaderRGBAf[3] *= 1-d;
				ent->flags |= RF_TRANSLUCENT;
			}
			//FTESurf Patch 259: counted at the point of submission, so it is what
			//the frame actually did rather than what the map loaded.
			if (sent->vc || sent->vcraw)
			{
				if (ent->vertlightbytes)
					vbsp_propstat.vcdrawn++;
				else
					vbsp_propstat.vcdeclined++;
			}
			PROP_VERDICT(PROPV_DRAWN);
		}
#undef PROP_VERDICT
	}
}
static void VBSP_InfoForPoint (struct model_s *mod, vec3_t pos, int *area, int *cluster, unsigned int *contentbits)
{
	int leaf = VBSP_PointLeafnum_r (mod, pos, 0);
	*area = VBSP_LeafArea(mod, leaf);
	*cluster = VBSP_LeafCluster(mod, leaf);
	*contentbits = VBSP_LeafContents(mod, leaf);
}


#ifdef RTLIGHTS
static int vbsp_shadowsequence;
static qbyte *shadowedpvs;
static model_t *shadowmodel;
static void VBSP_WalkShadows (dlight_t *dl, void (*callback)(msurface_t *surf), mnode_t *node)
{
	int			c, side;
	mplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	double		dot;

	float		l, maxdist;
	int			j, s, t;
	vec3_t		impact;
	struct vbsptexinfo_s *vtexinfo;
	vbspinfo_t *prv;

	if (node->shadowframe != vbsp_shadowsequence)
		return;

	//if light areabox is outside node, ignore node + children
	for (c = 0; c < 3; c++)
	{
		if (dl->origin[c] + dl->radius < node->minmaxs[c])
			return;
		if (dl->origin[c] - dl->radius > node->minmaxs[3+c])
			return;
	}

// if a leaf node, draw stuff
	if (node->contents != -1)
	{
		pleaf = (mleaf_t *)node;

		if (pleaf->cluster >= 0)
			shadowedpvs[pleaf->cluster>>3] |= 1<<(pleaf->cluster&7);

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				surf = *mark++;
				if (surf->flags & SURF_OFFNODE)
				{
					if (surf->shadowframe != vbsp_shadowsequence)
					{	//if its not on a node then its probably not a nice flat surface, so don't bother trying to cull it in fancy ways that depend on its plane.
						surf->shadowframe = vbsp_shadowsequence;
						callback(surf);
					}
				}
				else
					surf->shadowframe = vbsp_shadowsequence;
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
		dot = dl->origin[0] - plane->dist;
		break;
	case PLANE_Y:
		dot = dl->origin[1] - plane->dist;
		break;
	case PLANE_Z:
		dot = dl->origin[2] - plane->dist;
		break;
	default:
		dot = DotProduct (dl->origin, plane->normal) - plane->dist;
		break;
	}

	if (dot >= 0)
		side = 0;
	else
		side = 1;

// recurse down the children, front side first
	VBSP_WalkShadows (dl, callback, node->children[side]);

// draw stuff
  	c = node->numsurfaces;
	if (c)
	{
		prv = shadowmodel->meshinfo;
		surf = shadowmodel->surfaces + node->firstsurface;
		maxdist = dl->radius*dl->radius;
		for ( ; c ; c--, surf++)
		{
			if (surf->shadowframe != vbsp_shadowsequence)
				continue;

			if ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK))
				continue;		// wrong side

			/*if (surf->flags & (SURF_DRAWALPHA | SURF_DRAWTILED))
			{	// no shadows
				continue;
			}*/

			//is the light on the right side?
			if (surf->flags & SURF_PLANEBACK)
			{//inverted normal.
				if (-DotProduct(surf->plane->normal, dl->origin)+surf->plane->dist >= dl->radius)
					continue;
			}
			else
			{
				if (DotProduct(surf->plane->normal, dl->origin)-surf->plane->dist >= dl->radius)
					continue;
			}

			//Yeah, you can blame LordHavoc for this alternate code here.
			for (j=0 ; j<3 ; j++)
				impact[j] = dl->origin[j] - surf->plane->normal[j]*dot;

			vtexinfo = &prv->texinfo[surf->texinfo-shadowmodel->texinfo];
			// clamp center of light to corner and check brightness
			l = DotProduct (impact, vtexinfo->lmvecs[0]) + vtexinfo->lmvecs[0][3] - surf->texturemins[0];
			s = l;if (s < 0) s = 0;else if (s > surf->extents[0]) s = surf->extents[0];
			s = (l - s)*surf->texinfo->vecscale[0];
			l = DotProduct (impact, vtexinfo->lmvecs[1]) + vtexinfo->lmvecs[1][3] - surf->texturemins[1];
			t = l;if (t < 0) t = 0;else if (t > surf->extents[1]) t = surf->extents[1];
			t = (l - t)*surf->texinfo->vecscale[1];
			// compare to minimum light
			if ((s*s+t*t+dot*dot) < maxdist)
				callback(surf);
		}
	}

// recurse down the back side
	VBSP_WalkShadows (dl, callback, node->children[!side]);
}

static void VBSP_MarkShadows(model_t *model, dlight_t *dl, const qbyte *lvis)
{
	mnode_t *node;
	int i;
	mleaf_t *leaf;
	int cluster;

//	if (!dl->die)
	{
		//static
		//variation on mark leaves
		//FTESurf Patch 273: same cluster-bucket walk as VBSP_MarkLeaves (see the
		//essay there), on shadowframe instead of visframe.  This one runs once per
		//realtime light per frame, so on a map with several it multiplies.
		vbspinfo_t *prv = (vbspinfo_t*)model->meshinfo;
		if (prv->clusterfirstleaf)
		{
			int nc = model->numclusters;
			int nb = (nc + 7) >> 3;
			int b, bit;
			unsigned int k, e;

			for (b = 0; b < nb; b++)
			{
				int bits = lvis[b];
				if (!bits)
					continue;
				for (bit = 0; bit < 8; bit++)
				{
					if (!(bits & (1<<bit)))
						continue;
					cluster = (b<<3) + bit;
					if (cluster >= nc)
						break;
					for (k = prv->clusterfirstleaf[cluster], e = prv->clusterfirstleaf[cluster+1]; k < e; k++)
					{
						node = (mnode_t *)&model->leafs[prv->clusterleafs[k]];
						do
						{
							if (node->shadowframe == vbsp_shadowsequence)
								break;
							node->shadowframe = vbsp_shadowsequence;
							node = node->parent;
						} while (node);
					}
				}
			}
		}
		else
		for (i=0,leaf=model->leafs ; i<model->numleafs ; i++, leaf++)
		{
			cluster = leaf->cluster;
			if (cluster == -1)
				continue;
			if (lvis[cluster>>3] & (1<<(cluster&7)))
			{
				node = (mnode_t *)leaf;
				do
				{
					if (node->shadowframe == vbsp_shadowsequence)
						break;
					node->shadowframe = vbsp_shadowsequence;
					node = node->parent;
				} while (node);
			}
		}
	}
/*	else
	{
		//dynamic lights will be discarded after this frame anyway, so only include leafs that are visible
		//variation on mark leaves
		for (i=0,leaf=model->leafs ; i<model->numleafs ; i++, leaf++)
		{
			cluster = leaf->cluster;
			if (cluster == -1)
				continue;
			if (lvis[cluster>>3] & (1<<(cluster&7)))
			{
				node = (mnode_t *)leaf;
				do
				{
					if (node->shadowframe == vbsp_shadowsequence)
						break;
					node->shadowframe = vbsp_shadowsequence;
					node = node->parent;
				} while (node);
			}
		}
	}*/
}
static void VBSP_GenerateShadowMesh(model_t *model, dlight_t *dl, const qbyte *lightvis, qbyte *litvis, void (*callback)(msurface_t *surf))
{
	vbspinfo_t *prv = model->meshinfo;
	dispinfo_t *disp;
	int i;
	//globals are evil
	shadowmodel = model;
	shadowedpvs = litvis;	//this is an output
	vbsp_shadowsequence++;

	VBSP_MarkShadows(model, dl, lightvis);

	VBSP_WalkShadows(dl, callback, model->nodes);

	for (i = 0; i < prv->numdisplacements; i++)
	{
		disp = &prv->displacements[i];
		if (VBSP_EdictInFatPVS(model, &disp->pvs, litvis, NULL))
		{
			callback(disp->surf);
		}
	}
}
#endif




static void VBSP_LoadLeafLight (model_t *mod, qbyte *mod_base, vlump_t *hdridx, vlump_t *ldridx, vlump_t *hdrvals, vlump_t *ldrvals, int version, qboolean usehdr)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	vlump_t *lump_idx, *lump_vals;
	struct leaflightpoint_s *point;
	size_t i, j;
	unsigned short *in;
	qbyte *inpoint;

	if (version == 17 || version == 19)
		return; //nope. this info is in the leafs.

	/*
	FTESurf Patch 152 -- THE PALE PROPS, and this is the actual cause.

	This used to read:

	    if (hdridx && hdrvals)  lump_idx = hdridx, lump_vals = hdrvals;

	which tests the POINTERS.  They are addresses of entries in the lump
	directory array and are never null, so the HDR ambient cube won exactly
	always -- on maps that have no HDR lighting at all.

	VRAD writes a full-size HDR ambient lump on an LDR-only compile and fills it
	with ZEROS.  Measured on surf_666 (VBSP v20, LIGHTING_HDR lump is 0 bytes):

	    LEAF_AMBIENT_LIGHTING_HDR   24951 entries, 4159 sampled, ALL ZERO
	    LEAF_AMBIENT_LIGHTING       132972 entries, 1 zero, mean R/B 1.80

	So every static prop and every model on the map was lit by nothing, then
	lifted to a flat grey by the hl2_lt_min floor below -- pale, neutral and
	brighter than the warm world around it, which is exactly what was reported.
	The LDR cube's R/B of 1.80 is the map's real tone; the world lightmap
	measures 1.88 independently.

	Choose by CONTENT, and by the same question the face loader asks
	(VBSP_LoadFaces): is this map being drawn in HDR at all.  Mixing an HDR
	ambient cube with LDR lightmaps would be wrong even if both were populated,
	so the two decisions must agree.  A stub is caught by the extra test that
	the HDR *lighting* lump exists -- an ambient cube without a lightmap to go
	with it is a placeholder, whatever its size.
	*/
	if (hdridx && hdrvals && hdridx->filelen && hdrvals->filelen && usehdr)
		lump_idx = hdridx, lump_vals = hdrvals;
	else if (ldridx && ldrvals && ldridx->filelen && ldrvals->filelen)
		lump_idx = ldridx, lump_vals = ldrvals;
	else if (hdridx && hdrvals && hdridx->filelen && hdrvals->filelen)
		lump_idx = hdridx, lump_vals = hdrvals;	//no LDR at all: HDR is all there is
	else
		return;	//unsupported.

	if (lump_vals->filelen%(7*4))
		return;

	if (lump_idx->filelen != mod->numleafs*sizeof(short)*2)
		return;	//erk?

	//easy enough to load some of the data...
	point = plugfuncs->GMalloc(&mod->memgroup, sizeof(*point)*(lump_vals->filelen/(7*4)));
	inpoint = mod_base + lump_vals->fileofs;
	for (i = 0; i < lump_vals->filelen/(7*4); i++)
	{
		for (j = 0; j < 6; j++, inpoint+=4)
		{
			float e = pow(2, (signed char)inpoint[3]);
			point[i].rgb[j][0] = e * inpoint[0];
			point[i].rgb[j][1] = e * inpoint[1];
			point[i].rgb[j][2] = e * inpoint[2];
		}
		point[i].x = *inpoint++;
		point[i].y = *inpoint++;
		point[i].z = *inpoint++;
		inpoint++;
	}

	//Which cube got used, and whether it has anything in it.  One line, because
	//"every prop on this map is grey" was invisible from inside the game for
	//three builds and this is the line that would have said it.
	{
		size_t n = lump_vals->filelen/(7*4), k, nz = 0;
		for (k = 0; k < n; k++)
			if (point[k].rgb[0][0] + point[k].rgb[0][1] + point[k].rgb[0][2] > 0.0001)
				nz++;
		Con_DPrintf("%s: leaf ambient %s, %u points, %u non-zero%s\n",
			mod->name, (lump_vals == hdrvals) ? "HDR" : "LDR",
			(unsigned)n, (unsigned)nz,
			nz ? "" : CON_WARNING" -- every model on this map will fall back to hl2_lt_min");
	}

	prv->leaflight = plugfuncs->GMalloc(&mod->memgroup, sizeof(*prv->leaflight)*mod->numleafs);
	in = (unsigned short*)(mod_base + lump_idx->fileofs);
	for (i = 0; i < mod->numleafs; i++)
	{
		prv->leaflight[i].count = LittleShort(*in++);
		prv->leaflight[i].point = point + (unsigned short)LittleShort(*in++);
	}
}

/*
FTESurf Patch 302 -- LUMP_WORLDLIGHTS, and why the leaf ambient cube was never
going to be enough on its own.

VRAD writes the lights it used into LUMP_WORLDLIGHTS (15, LDR) and
LUMP_WORLDLIGHTS_HDR (54).  Source's light cache lights a static prop that has
no .vhv bake from the ambient cube PLUS these; we had only the cube, which
VRAD fills with bounce alone -- IsLeafAmbientSurfaceLight
(utils/vrad/leaf_ambient_lighting.cpp:183-198) admits a light to the cube only
if it is an unstyled emit_surface dim enough to pass a 1/512^2 test, and on
surf_tensor2 that is 0 of 603 lights.  The other 603 simply never reached any
model.

WHAT IS KEPT, and each exclusion is a decision, not an oversight:

  emit_surface/point/spotlight  KEPT.  These have real distance falloff, so a
    light in another room contributes almost nothing even before the PVS test.

  emit_skylight/emit_skyambient  DROPPED.  Both return 1.0 from
    Engine_WorldLightDistanceFalloff -- no falloff at ANY distance -- so they
    apply everywhere unless you can answer "does this point see sky".  Source
    answers it with a trace that must end on a SURF_SKY face.  We have no such
    trace yet, and the map's own LEAF_FLAGS_SKY cannot stand in for one: it
    means "3D sky is somewhere in this leaf's PVS" (bspfile.h:741) and is set
    on 52.8% of surf_tensor2's leaves including the sealed concrete room the
    prop in the bug report stands in.  Measured, these two are 0.054 0.112
    0.263 against 0.029 0.042 0.054 for every local light combined -- roughly
    3x everything else, and blue.  Applying them wrongly indoors would be a
    worse bug than the one being fixed.  See hl2_lt_worldlight's description.

  style != 0  DROPPED.  A switchable or animated light's contribution is a
    function of the lightstyle, and this term is solved once per prop.

  DWL_FLAGS_INAMBIENTCUBE  DROPPED.  VRAD already put that light in the cube
    we are adding to; counting it again would double it.

  cluster < 0  DROPPED.  A light with no cluster cannot be PVS-tested, and
    pvs[-1] reads the byte before the buffer (see :6702).

THE LUMP VERSION -- and FTESurf Patch 308, which is where the 100-byte variant
stopped being a guess.

Patch 302 refused lump version 1 (a 100-byte stride, measured on surf_fantasy)
because the extra 12 bytes are undocumented: public/bspfile.h caps at
BSPVERSION 20 and worldlights is absent from its versioned-lumps enum, and
Momentum's own client rejects such a lump outright
(game/client/momentum/worldlight.cpp:161).  Refusing beat guessing.  It also
claimed the refusal "costs nothing today, because v21 maps ship full .vhv
bakes and take the baked path".  THAT PART WAS WRONG, and it is the reason
this is a patch rather than a cleanup: the bake path suppresses this term for
PROPS only (VBSP_AddWorldLightCube's prv->wl_suppress), while players,
viewmodels and every unbaked prop on those maps were left on bounce-only
lighting.  Measured over the installed library: 1314 maps, 123 of them
lumpversion 1, 39,641 worldlights refused.  Patch 302 and Patch 304 were both
inert on all 123.

THE LAYOUT IS NOW DERIVED RATHER THAN GUESSED, and the key is that
surf_fantasy ships TWICE -- CS:S (bsp v20, lump version 0, stride 88) and
Momentum (bsp v21, lump version 1, stride 100) -- with the SAME 777 lights.
Pairing the two builds by origin matches 777 of 777, which turns the v20 build
into a Rosetta stone: for every v20 field, scan the v21 record for the offset
that reproduces it across all 777 lights.  The answer is a single 12-byte
insertion immediately after `normal`, everything below shifted by 12:

    origin 0, intensity 12, normal 24, <12 NEW BYTES at 36>, cluster 48,
    type 52, style 56, stopdot 60, stopdot2 64, exponent 68, radius 72,
    constant_attn 76, linear_attn 80, quadratic_attn 84, flags 88,
    texinfo 92, owner 96

Element-wise agreement with the CS:S build, 777 paired lights:
    origin, style, stopdot, stopdot2, exponent, radius, the attenuation
    triple, texinfo, owner ....... 100.00%
    intensity, normal, type ...... 99.74%  (2 lights; the map was RECOMPILED)
    cluster ....................... 52.12%  (a function of the vis tree, which
                                             must differ between two compiles)
    flags ......................... 0.26%   (the v21 compile sets Strata's new
                                             CASTENTITYSHADOWS bit, 0x2)
Every field that has to survive a recompile does; the three that do not, do
not for reasons that are themselves checkable.  The inserted 12 bytes read as
three floats and are ALL ZERO on all 777 -- consistent with Strata's added
`Vector shadow_cast_offset`, which is also what its name in the engine that
writes them would predict, and which we have no use for either way.

Validated beyond the one map it was derived from: across all 123 v21 maps in
the library, the derived offsets give a type in 0..5 and a style in 0..63 for
every light and a unit-length `normal` for every light, with ZERO maps
failing.  That is 39,641 independent chances to disagree.

hl2_lt_wl21 0 restores the refusal exactly, for a map where this is suspected.
*/
#define DWL_FLAGS_INAMBIENTCUBE	0x0001	//public/bspfile.h:910-911
enum
{	//public/bspfile.h:899-907
	VWL_SURFACE = 0,
	VWL_POINT = 1,
	VWL_SPOTLIGHT = 2,
	VWL_SKYLIGHT = 3,
	VWL_QUAKELIGHT = 4,
	VWL_SKYAMBIENT = 5
};
typedef struct
{	//public/bspfile.h:914-935.  All members are 4 bytes, so this is 88 with no
	//padding on any target we build for; the static assert below says so.
	float	origin[3];
	float	intensity[3];
	float	normal[3];
	int		cluster;
	int		type;
	int		style;
	float	stopdot;
	float	stopdot2;
	float	exponent;
	float	radius;
	float	constant_attn;
	float	linear_attn;
	float	quadratic_attn;
	int		flags;
	int		texinfo;
	int		owner;
} dworldlight_t;
/*
FTESurf Patch 308: the v21/Strata record.  Spelled out as its own struct rather
than as a stride plus offset arithmetic, because the ONE thing that can go wrong
here is a field landing in the wrong slot, and a struct makes that a compile-time
statement the reader can check against the table in the essay above.  The field
names and order are identical to dworldlight_t apart from shadow_cast_offset;
VBSP_WorldLight21 copies one into the other so there is a single parsing loop and
no second copy of the filtering rules to drift.
*/
typedef struct
{
	float	origin[3];
	float	intensity[3];
	float	normal[3];
	float	shadow_cast_offset[3];	//the 12 new bytes; zero on all 39,641 measured
	int		cluster;
	int		type;
	int		style;
	float	stopdot;
	float	stopdot2;
	float	exponent;
	float	radius;
	float	constant_attn;
	float	linear_attn;
	float	quadratic_attn;
	int		flags;
	int		texinfo;
	int		owner;
} dworldlight21_t;
static void VBSP_WorldLight21 (const dworldlight21_t *in, dworldlight_t *out)
{	//shadow_cast_offset is READ AND DROPPED: it offsets the shadow-casting origin
	//for Strata's entity shadows, which this engine does not do, and it is zero on
	//every light in the library anyway.
	memcpy(out->origin,    in->origin,    sizeof(out->origin));
	memcpy(out->intensity, in->intensity, sizeof(out->intensity));
	memcpy(out->normal,    in->normal,    sizeof(out->normal));
	out->cluster		= in->cluster;
	out->type			= in->type;
	out->style			= in->style;
	out->stopdot		= in->stopdot;
	out->stopdot2		= in->stopdot2;
	out->exponent		= in->exponent;
	out->radius			= in->radius;
	out->constant_attn	= in->constant_attn;
	out->linear_attn	= in->linear_attn;
	out->quadratic_attn	= in->quadratic_attn;
	out->flags			= in->flags;
	out->texinfo		= in->texinfo;
	out->owner			= in->owner;
}
//Source's cube face order, +x -x +y -y +z -z -- utils/vrad/leaf_ambient_lighting.cpp:20-28.
static const vec3_t vbsp_boxdirections[6] =
{
	{ 1, 0, 0}, {-1, 0, 0},
	{ 0, 1, 0}, { 0,-1, 0},
	{ 0, 0, 1}, { 0, 0,-1}
};
static void VBSP_LoadWorldLights (model_t *mod, qbyte *mod_base, vlump_t *hdr, vlump_t *ldr, qboolean usehdr)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	vlump_t		*l;
	dworldlight_t *in;
	size_t		i, count, kept = 0, stride;
	size_t		drop_sky = 0, drop_style = 0, drop_cube = 0, drop_cluster = 0;
	qboolean	v21;

	prv->wl_pvscluster = -2;	//GMalloc zeroes prv, and 0 is a real cluster number.

	/*
	The same three-way the ambient cube makes (:9014-9021) and for the same
	reason: VRAD writes a full-size HDR lump of zeros on an LDR-only compile, so
	the choice is by CONTENT and must agree with the lightmap's.  Note that this
	FALLS BACK, which is exactly why neither half may be put on Patch 298's
	retirement list while a renderer exists -- see the census at :9522.
	*/
	if (hdr && hdr->filelen && usehdr)
		l = hdr;
	else if (ldr && ldr->filelen)
		l = ldr;
	else if (hdr && hdr->filelen)
		l = hdr;
	else
		return;

	//FTESurf Patch 308: version 1 is Strata's 100-byte record, derived above.
	//Anything else is still refused -- this accepts the two layouts it can name,
	//it does not accept "whatever stride divides the lump", which is how a wrong
	//guess would look from here.
	v21 = (l->version == 1) && hl2_lt_wl21 && hl2_lt_wl21->ival;
	if (l->version != 0 && !v21)
	{	//Two different reasons land here and they want different words: a version
		//nobody has a layout for, versus Strata's version 1 turned off by hand.
		//Reading "unknown layout" in a log while holding hl2_lt_wl21 0 would send
		//the next reader looking for a parser bug that is not there.
		if (l->version == 1)
			Con_DPrintf("%s: worldlights lump is Strata's version 1 -- skipped by hl2_lt_wl21 0"
				CON_WARNING " (models fall back to bounce-only lighting)\n", mod->name);
		else
			Con_DPrintf("%s: worldlights lump is version %i -- skipped" CON_WARNING
				" (only 0 and Strata's 1 have a known layout; models keep bounce-only lighting)\n",
				mod->name, l->version);
		return;
	}
	stride = v21 ? sizeof(dworldlight21_t) : sizeof(dworldlight_t);
	if (sizeof(dworldlight_t) != 88 || sizeof(dworldlight21_t) != 100 || l->filelen % stride)
	{
		Con_Printf(CON_ERROR "VBSP_LoadWorldLights: funny lump size\n");
		return;
	}
	count = l->filelen / stride;
	if (!count)
		return;	//GMalloc(0) returns a non-NULL zero-byte block (:1955), so guard first.

	in = (dworldlight_t*)(mod_base + l->fileofs);
	prv->worldlights = plugfuncs->GMalloc(&mod->memgroup, sizeof(*prv->worldlights)*count);
	for (i = 0; i < count; i++)
	{
		dworldlight_t rec;
		int type, style, flags, cluster;
		struct vworldlight_s *out;
		int j;

		//FTESurf Patch 308: one parsing loop for both layouts.  The v21 record is
		//widened into the v20 one FIRST, so every filter, every field read and the
		//census below are written once and cannot diverge between the two strides.
		if (v21)
			VBSP_WorldLight21((const dworldlight21_t*)((qbyte*)in + i*stride), &rec);
		else
			rec = in[i];

		type = LittleLong(rec.type);
		style = LittleLong(rec.style);
		flags = LittleLong(rec.flags);
		cluster = LittleLong(rec.cluster);

		if (type != VWL_SURFACE && type != VWL_POINT && type != VWL_SPOTLIGHT)
			{ drop_sky++; continue; }
		if (style != 0)
			{ drop_style++; continue; }
		if (flags & DWL_FLAGS_INAMBIENTCUBE)
			{ drop_cube++; continue; }
		if (cluster < 0)
			{ drop_cluster++; continue; }

		out = &prv->worldlights[kept++];
		for (j = 0; j < 3; j++)
		{
			out->origin[j]		= LittleFloat(rec.origin[j]);
			out->intensity[j]	= LittleFloat(rec.intensity[j]);
			out->normal[j]		= LittleFloat(rec.normal[j]);
		}
		out->cluster		= cluster;
		out->type			= type;
		out->stopdot		= LittleFloat(rec.stopdot);
		out->stopdot2		= LittleFloat(rec.stopdot2);
		out->exponent		= LittleFloat(rec.exponent);
		out->radius			= LittleFloat(rec.radius);
		out->constant_attn	= LittleFloat(rec.constant_attn);
		out->linear_attn	= LittleFloat(rec.linear_attn);
		out->quadratic_attn	= LittleFloat(rec.quadratic_attn);
	}
	prv->numworldlights = kept;

	//One line per map.  The drop counts are here because "the props are still
	//dark" and "every light was filtered out" look identical from inside the
	//game, and that is the distinction this line exists to make.
	//FTESurf Patch 308 adds the layout to it, because "v21 accepted" and "v21
	//still refused" were previously distinguishable only by the ABSENCE of this
	//line, and an absent line reads as "the map has no worldlights".
	Con_DPrintf("%s: worldlights %s v%i (%u-byte%s), %u of %u usable (%u sky, %u styled, %u already in cube, %u clusterless)%s\n",
		mod->name, (l == hdr) ? "HDR" : "LDR", l->version,
		(unsigned)stride, v21 ? ", Strata" : "",
		(unsigned)kept, (unsigned)count, (unsigned)drop_sky, (unsigned)drop_style,
		(unsigned)drop_cube, (unsigned)drop_cluster,
		kept ? "" : CON_WARNING" -- hl2_lt_worldlight can do nothing on this map");
}

/*
FTESurf Patch 302 -- the direct term, added into a six-face cube.

This is AddEmitSurfaceLights (utils/vrad/leaf_ambient_lighting.cpp:92-136) with
the emitter test generalised from emit_surface to the three local types, and
VRAD's per-light TestLine visibility replaced by a PVS test.

WHAT THE PVS TEST IS AND IS NOT.  VRAD traces a ray to every light.  We test
only whether the light's cluster is in the sample point's PVS, which is a
strictly looser question: light will leak to a point that is around a corner
but still in the visible set.  It is not a rounding detail -- ungated, the same
sum at the surf_tensor2 prop comes out 38-55x instead of 4.4-4.8x, because 377
of the map's lights "reach" that point geometrically and only the ones sharing
its PVS are real.  So the PVS test is doing nearly all of the work here, and a
per-light trace is the known next refinement rather than a nicety.
*/
/*
FTESurf Patch 308, the second half: `ignoresuppress`.

prv->wl_suppress means "this prop has a .vhv bake, so its direct light is already
counted -- do not add it again".  That is right for VBSP_LightPointValues, whose
output IS the prop's level.  It is WRONG for VBSP_LightPointCube, whose output is
read by exactly one thing: vertexlit.glsl's #BUMPCUBE, which uses it as a RATIO,

    light *= clamp(pow((cube(n_bumped)+eps) / (cube(n_geom)+eps), 1/2.2), 0, 4)

A ratio of a cube against itself carries no level.  It is exactly 1.0 whenever the
bumped normal equals the geometric one -- a flat normal map, an unbumped material,
or an empty cube -- and it is invariant under scaling the cube, so it cannot
double-count a bake no matter how bright the bake is.  What it CAN carry is the
cube's anisotropy, and that is precisely what the suppression was starving it of.

Measured, over 200 real leaf ambient cubes and the normal-tilt distribution of
fan/moss_n.vtf (mean 6.8 deg), as a change in 0..255 levels on a mid-grey pixel:

    bounce-only cube (what a baked prop gets today)   mean 1.72   above 3 levels on  11 of 200
    with the direct term added                        mean 4.41   above 3 levels on 193 of 200

i.e. the ratio sits BELOW the ~3-level threshold at which anything is visible on
screen, and adding the light it was already allowed to see for unbaked props puts
it above.  This is the same fact that made Patch 268 C measure nothing, read from
the other end -- and note that 268 C's headline "mean ratio 0.9941" never was
evidence for its conclusion: the mean of a ratio over a symmetric tilt
distribution is ~1 by construction.  On a hard top-lit cube the mean is 1.6001
while the MEDIAN pixel moves 0.22 levels.  Mean and spread are independent here;
only the spread is visible.

Gated on hl2_cubelight rather than on a cvar of its own, deliberately: that cvar
is exactly "compile the shader that reads this cube", the cube has no other
consumer that is ever injected, and one value driving both halves is the Patch 304
lesson.  hl2_cubelight 0 is therefore bit-for-bit today, including this.
*/
static void VBSP_AddWorldLightCube (model_t *mod, const vec3_t point, vec3_t cube[6], qboolean ignoresuppress)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	qbyte		*visbits;
	int			leafnum, cluster;
	size_t		i;
	int			j, f;

	if (!prv || !prv->worldlights || !prv->numworldlights)
		return;
	if (!hl2_lt_worldlight || !hl2_lt_worldlight->ival)
		return;
	if (prv->wl_suppress && !ignoresuppress)
		return;

	leafnum = VBSP_PointLeafnum(mod, point);
	if (leafnum < 0 || leafnum >= mod->numleafs)
		return;		//NOT leafnum=0 as :9073 does: leaf 0 is solid, and answering
					//a lighting question from the solid leaf would invent light.
	cluster = mod->leafs[leafnum].cluster;
	if (cluster < 0 || cluster >= mod->numclusters)
		return;

	//PVM_REPLACE guarantees the return is our own buffer (gl_model.h:262-272),
	//so the row survives whatever else calls VBSP_ClusterPVS this frame.
	if (prv->wl_pvscluster != cluster)
	{
		VBSP_ClusterPVS(mod, cluster, &prv->wl_pvs, PVM_REPLACE);
		prv->wl_pvscluster = cluster;
	}
	visbits = prv->wl_pvs.buffer;
	if (!visbits)
		return;

	for (i = 0; i < prv->numworldlights; i++)
	{
		const struct vworldlight_s *wl = &prv->worldlights[i];
		vec3_t	delta, dir;
		float	d2, dist, falloff, angle, ratio;
		int		c = wl->cluster;

		if (c >= mod->numclusters || !(visbits[c>>3] & (1<<(c&7))))
			continue;

		VectorSubtract(wl->origin, point, delta);
		d2 = DotProduct(delta, delta);

		//Engine_WorldLightDistanceFalloff, worldlight.cpp:36-87.
		if (wl->type == VWL_SURFACE)
		{
			if (wl->radius != 0 && d2 > wl->radius*wl->radius)
				continue;
			//InvRSquared (public/mathlib/vector.h:2176-2190) clamps the
			//DENOMINATOR to a minimum of 1, so a light inside 1 unit saturates
			//at 1.0 rather than exploding.  That clamp is load-bearing.
			falloff = 1.0f / ((d2 > 1.0f) ? d2 : 1.0f);
		}
		else
		{
			dist = sqrt(d2);
			if (wl->radius != 0 && dist > wl->radius)
				continue;
			falloff = wl->constant_attn + wl->linear_attn*dist + wl->quadratic_attn*d2;
			if (falloff < 1e-6f)
				falloff = 1e-6f;
			falloff = 1.0f / falloff;
		}
		if (falloff <= 0)
			continue;

		if (d2 < 1e-8f)
			continue;	//coincident with the light; no direction to give it.
		dist = sqrt(d2);
		VectorScale(delta, 1.0f/dist, dir);

		/*
		Engine_WorldLightAngle (leaf_ambient_lighting.cpp:57-73), emitter side
		only.  VRAD calls it with the NORMALISED DELTA in the snormal slot for
		cube work, which makes its first dot exactly 1 -- the receiving surface's
		own cosine is applied per cube face below instead, which is the only way
		a six-face cube can be built from a single call.
		*/
		if (wl->type == VWL_SURFACE)
		{
			angle = -DotProduct(dir, wl->normal);
			if (angle <= 0.01f)		//ON_EPSILON/10; behind the emitting surface
				continue;
		}
		else if (wl->type == VWL_SPOTLIGHT)
		{
			float dot2 = -DotProduct(dir, wl->normal);
			if (dot2 <= wl->stopdot2)
				continue;			//outside the penumbra entirely
			if (dot2 >= wl->stopdot)
				angle = 1.0f;		//inside the hot spot
			else
			{
				float span = wl->stopdot - wl->stopdot2;
				float e = (wl->exponent < 1.0f) ? 1.0f : wl->exponent;
				if (span < 1e-6f)
					span = 1e-6f;
				angle = pow((dot2 - wl->stopdot2) / span, e);
			}
		}
		else
			angle = 1.0f;			//a point light emits equally in every direction

		ratio = falloff * angle;
		if (ratio <= 0)
			continue;

		for (f = 0; f < 6; f++)
		{
			float t = DotProduct(vbsp_boxdirections[f], dir);
			if (t > 0)
				for (j = 0; j < 3; j++)
					cube[f][j] += wl->intensity[j] * (t * ratio);
		}
	}
}

/*
FTESurf Patch 304: read the cube the way Source reads it -- the n^2 convex
combination of the three faces a normal actually sees.

    out = sum_i  dir[i]^2 * cube[ the face light arrives from on axis i ]

Source's face order is +x -x +y -y +z -z, rgb[0] being light arriving FROM +x, so
a normal with a positive component on an axis takes the even face of that pair.
For a unit dir the three weights sum to 1, which makes this an interpolation and
not a gain: it can never return more than the brightest face it touches.

This is the same reconstruction the #AMBIENTCUBE path already performs in the
vertex shader (vertexlit.glsl:113-116) and the one VRAD itself would have used to
bake a .vhv for this prop, which is exactly why it is the right function to
define "the level on the lit side" and "the level on the unlit side" with.
*/
static void VBSP_CubeAlongDir (vec3_t cube[6], const vec3_t dir, vec3_t out)
{
	int i;
	VectorClear(out);
	for (i = 0; i < 3; i++)
	{
		float w = dir[i]*dir[i];
		if (w > 0)
			VectorMA(out, w, cube[i*2 + ((dir[i] >= 0)?0:1)], out);
	}
}
static void VBSP_LightPointValues	(struct model_s *model, const vec3_t point, vec3_t res_diffuse, vec3_t res_ambient, vec3_t res_dir)
{
	vbspinfo_t	*prv = (vbspinfo_t*)model->meshinfo;
	int leafnum = VBSP_PointLeafnum(model,point);
	if (leafnum < 0 || leafnum >= model->numleafs)	//nettest: a point outside the loaded leafs (a prop near the void / map edge) gives an out-of-range leaf -> OOB read of leafs[]/leaflight[count]; clamp it
		leafnum = 0;
	mleaf_t *leaf = model->leafs+leafnum;
	struct mleaflight_s *leaflight = prv->leaflight+leafnum;
	struct leaflightpoint_s *best, *lp;
	size_t i, j, d, bd=~0;
	int xyz[3];
	vec3_t diff[6];
	float sig[6];

	/*
	FTESurf Patch 152 -- the gamma, which is why props needed a floor at all.

	Source's ambient cube is stored as ColorRGBExp32 and decodes to LINEAR light
	(c * 2^exponent).  Putting a linear value on screen needs a gamma encode; a
	multiply is not one.  surf_666's typical leaf decodes to about 0.034 linear:

	    linear * 160          ->    5.4 / 255   -- black
	    sRGB encode * 255     ->   55.6 / 255   -- what Source shows

	That factor of ten is the whole reason hl2_lt_min had to sit at 64 and do
	the lighting itself, which is what made every prop the same flat colour.

	The sRGB encode WAS the default and was turned off with the note "was 1,
	which pushed unbounded HDR leaf-ambient over 255 = white blow-out".  That
	diagnosis is right and the remedy was too broad: HDR ambient is unbounded so
	pow() runs away, but LDR ambient is bounded and the encode is exactly
	correct for it.  So the value is CLAMPED to 1.0 first -- crude tone mapping,
	but bounded -- and then encoded, which is right for LDR and safe for HDR.
	*/
	static cvar_t *srgbmag, *scale, *forceface;
	if (!srgbmag)	srgbmag		= cvarfuncs->GetNVFDG("hl2_lt_srgb_mag","1",	0, "sRGB-encode model lighting (0 = plain linear scale by hl2_lt_scale). Source's ambient cube is LINEAR light and needs a gamma encode to be displayed; a multiply is not one. Values are clamped to 1.0 first so an HDR cube cannot run away through pow().", "");
	if (!scale)		scale		= cvarfuncs->GetNVFDG("hl2_lt_scale",	"255",	0, "Model-lighting brightness scale, applied after the encode. 255 maps a fully-lit luxel to white. The old default of 160 belonged to the linear path, where it was compensating for a missing gamma encode rather than setting a brightness.", "");
	if (!forceface)	forceface	= cvarfuncs->GetNVFDG("hl2_lt_face",	"-1",	0, "TEST", "TEST");

	if (prv->leaflight && leaflight->count)
	{
		for (i = 0; i < 3; i++)
			xyz[i] = 255*(point[i] - leaf->minmaxs[i]) / (leaf->minmaxs[3+i]-leaf->minmaxs[i]);
		for (i = 0, best=lp = leaflight->point; i < leaflight->count; i++, lp++)
		{
			int m[3];
			m[0] = xyz[0] - lp->x;
			m[1] = xyz[1] - lp->y;
			m[2] = xyz[2] - lp->z;
			d = DotProduct(m,m);
			if (bd > d)
			{
				bd = d;
				best = lp;
			}
		}


		/*
		FTESurf Patch 302 -- the direct light goes in HERE, into the cube, and
		not into the entity fields further downstream.

		Everything below this point -- the dominant-direction solve, the diffuse
		split, hl2_lt_dirsign's two arms, the sRGB encode, hl2_lt_scale and
		hl2_lt_min's colour-preserving floor -- is a decomposition OF THIS CUBE.
		Adding the term here means all of it applies to the corrected cube
		exactly as it would have if VRAD had written the light into the cube in
		the first place, with no second copy of that arithmetic to drift.

		It also puts the term on the correct side of a naming trap.  The two
		fields this eventually feeds are transposed with respect to their names:
		res_diffuse becomes light_avg -> e_light_ambient, the shader's FLAT base,
		while res_ambient becomes light_range -> e_light_mul, the DIRECTIONAL
		amplitude gated by max(0,dot(n,e_light_dir)) (gl_alias.c:1602-1605).  A
		term written into one of those by hand lifts either only the lit faces or
		only the flat base; written into the cube it correctly does both.

		vbsp_wl_cube is a plain local: the six faces are small, this runs once per
		prop inside the light_known cache, and leaving best->rgb untouched keeps
		the loaded lump read-only so a second model sampling the same leaf is not
		looking at someone else's accumulation.
		*/
		vec3_t lcube[6];
		vec3_t *rgb = best->rgb;
		if (prv->numworldlights && hl2_lt_worldlight && hl2_lt_worldlight->ival && !prv->wl_suppress)
		{
			for (j = 0; j < 6; j++)
				VectorCopy(best->rgb[j], lcube[j]);
			VBSP_AddWorldLightCube(model, point, lcube, false);	//the LEVEL: suppression stands
			rgb = lcube;
		}

		VectorClear(res_ambient);
		for (j = 0; j < 6; j++)
			VectorAdd(res_ambient, rgb[j], res_ambient);
		VectorScale(res_ambient, 1.0/6, res_ambient);

		//try and figure out an average dir for the brightest direction
		for (j = 0; j < 6; j++)
		{
			VectorSubtract(rgb[j], res_ambient, diff[j]);
			sig[j] = VectorLength(diff[j]);
		}
		if (hl2_lt_fold && hl2_lt_fold->ival && prv->fold_prop)
		{
			/*
			FTESurf Patch 304 -- WHICH END OF THE RAMP THE FLAT BASE IS.

			The shader renders  base + w(n) * amp,  where base is e_light_ambient
			and is applied to EVERY vertex regardless of which way it faces.  The
			legacy fold below sets base = mean + the deviation toward the light --
			that is, roughly the value of the BRIGHTEST face -- and amp = the plain
			mean.  So a face turned away from every light in the map receives the
			bright-side level, and a face turned toward the light receives the
			bright side plus the mean on top of it.  Measured on tensor2's
			stage2_detail02 against what VRAD would have baked: the lit faces ran
			1.3-1.9x hot and the face pointing away from the light ran 13.6x hot.
			That single prop spanned 0.99 to 13.6, which is why no global scale
			could ever have fixed it.

			The fix is to put the base at the OTHER end of the ramp.  base is the
			level on the face turned away from the dominant direction, amp is the
			difference to the face turned toward it, and w(n) carries the model
			from one to the other.  Both levels are read with the same n^2 convex
			combination VRAD and #AMBIENTCUBE use, so the two ends are answers to
			the same question the rest of the engine asks, not new constants.

			AND THE RAMP HAS TO CHANGE WITH IT.  w(n) is a HALF-lambert in this
			mode -- 0.5+0.5*dot, which Source uses on models for exactly this
			reason -- because a plain lambert clamps flat at the base over the
			whole shadow hemisphere, and the true irradiance there keeps falling.
			vertexlit.glsl reads this same cvar through !!cvardf to pick the ramp,
			so the two halves cannot disagree.  Measured over a uniform sphere of
			normals rather than the handful the camera sees:

			            today      this fold     (mean |log2(rendered/truth)|)
			  tensor2   1.037        0.283
			  bounce    0.463        0.230       over 200 real leaf cubes
			            and it beat the legacy fold on 200 of those 200.

			The direction is taken from the SIGNED luminance difference of each
			opposing pair unconditionally -- it is not hl2_lt_dirsign's choice
			here.  This fold is a statement about which end of the ramp is which,
			so a direction that points the wrong way does not merely shade the
			wrong side, it swaps base and amp and inverts the model.  On the
			unsigned direction the same measurement gives 0.825 instead of 0.283.
			hl2_lt_dirsign still governs the legacy fold below, where it is a
			preference; here it would be a correctness bug and is not offered.
			*/
			static const vec3_t lumw = {0.299f, 0.587f, 0.114f};
			vec3_t lit, unlit, away;
			float len;

			for (j = 0; j < 3; j++)
				res_dir[j] = DotProduct(rgb[j*2], lumw) - DotProduct(rgb[j*2+1], lumw);
			len = VectorLength(res_dir);
			if (len < 1e-9)
			{
				/*
				A cube with no direction in it at all -- every face equal.  There
				is no lit end and no unlit end, so the honest answer is the flat
				level with NO directional half, which is also exactly what the n^2
				reconstruction returns for such a cube from every normal.  res_dir
				still has to be a unit vector because the consumer normalises it
				and would otherwise divide by zero.
				*/
				VectorSet(res_dir, 0, 0, 1);
				VectorCopy(res_ambient, res_diffuse);	//still the six-face mean at this point
				VectorClear(res_ambient);
			}
			else
			{
				VectorScale(res_dir, 1.0f/len, res_dir);
				VectorNegate(res_dir, away);
				VBSP_CubeAlongDir(rgb, res_dir, lit);
				VBSP_CubeAlongDir(rgb, away, unlit);
				VectorCopy(unlit, res_diffuse);
				//clamped at zero per channel: the direction is chosen on luminance,
				//so a strongly tinted cube can have one channel darker on the lit
				//side, and a negative amp would subtract light from the lit face.
				for (j = 0; j < 3; j++)
					res_ambient[j] = (lit[j] > unlit[j]) ? lit[j]-unlit[j] : 0;
			}
		}
		else if (hl2_lt_dirsign && hl2_lt_dirsign->ival)
		{
			/*
			FTESurf Patch 268 C2: point the direction TOWARD the light, which is what every
			consumer assumes -- vertexlit.glsl and defaultskin.glsl add
			max(0, dot(n, e_light_dir)) * e_light_mul, and gl_alias.c's inverse describes
			light_dir as "pointing TOWARD the light".

			The unsigned form in the else arm gets it backwards.  sig[] is the LENGTH of
			each face's deviation from the mean, so for a cube lit from above (+z 1.0, the
			other five 0.2, mean 0.333) sig[+z] is 0.667 and sig[-z] 0.133, and
			sig[-z] - sig[+z] points DOWN.  A length also has no sign at all: a cube bright
			on +x and dark on -x by the same amount gives equal lengths and no x component.
			Source's cube order is +x -x +y -y +z -z, rgb[0] being light arriving FROM +x.

			The signed luminance difference of each opposing pair fixes both, and the
			diffuse level then takes the face the direction now points at.  Off by default:
			on a map without per-vertex bakes this changes which side of every prop -- and
			of the player model -- is the lit one.
			*/
			static const vec3_t lumw = {0.299f, 0.587f, 0.114f};
			for (j = 0; j < 3; j++)
				res_dir[j] = DotProduct(rgb[j*2], lumw) - DotProduct(rgb[j*2+1], lumw);
			VectorNormalize(res_dir);

			VectorCopy(res_ambient, res_diffuse);
			for (j = 0; j < 3; j++)
			{
				if (res_dir[j]>=0)
					VectorMA(res_diffuse, res_dir[j], diff[j*2+0], res_diffuse);
				else
					VectorMA(res_diffuse, -res_dir[j], diff[j*2+1], res_diffuse);
			}
		}
		else
		{
			for (j = 0; j < 3; j++)
				res_dir[j] = sig[j*2+1] - sig[j*2];
			VectorNormalize(res_dir);

			//figure out how much light there should be in that direction.
			VectorCopy(res_ambient, res_diffuse);
			for (j = 0; j < 3; j++)
			{
				if (res_dir[j]>=0)	//nettest: was res_dir[0] on all 3 axes — leaned every prop's shading toward the X faces; index per-axis
					VectorMA(res_diffuse, res_dir[j], diff[j*2+1], res_diffuse);
				else
					VectorMA(res_diffuse, -res_dir[j], diff[j*2+0], res_diffuse);
			}
		}

		if (forceface->ival >= 0)
		{
			VectorCopy(rgb[forceface->ival], res_diffuse);
			VectorCopy(rgb[forceface->ival], res_ambient);
			VectorClear(res_dir);
			res_dir[forceface->ival/3] = (forceface->ival&1)?-1:1;
		}

		if (srgbmag->value)
		{
			for (j = 0; j < 3; j++)
			{
				//Clamp BEFORE the encode.  pow() on an unbounded HDR luxel is
				//what blew this out last time; 1.0 is "fully lit" and anything
				//above it has nowhere brighter to go on an 8-bit screen anyway.
				float a = res_ambient[j], d = res_diffuse[j];
				if (a > srgbmag->value) a = srgbmag->value;
				if (d > srgbmag->value) d = srgbmag->value;
				if (a < 0) a = 0;
				if (d < 0) d = 0;
				res_diffuse[j] = M_LinearToSRGB(d, srgbmag->value)*scale->value;
				res_ambient[j] = M_LinearToSRGB(a, srgbmag->value)*scale->value;
			}

			/*for (j = 0; j < 6; j++)
			{
				res_cube[j][0] = M_LinearToSRGB(best->rgb[j][0], srgbmag->value)*scale->value;
				res_cube[j][1] = M_LinearToSRGB(best->rgb[j][1], srgbmag->value)*scale->value;
				res_cube[j][2] = M_LinearToSRGB(best->rgb[j][2], srgbmag->value)*scale->value;
			}*/
		}
		else
		{
			VectorScale(res_diffuse, scale->value, res_diffuse);
			VectorScale(res_ambient, scale->value, res_ambient);

			/*for (j = 0; j < 6; j++)
				VectorScale(best->rgb[j], scale->value, res_cube[j]);*/
		}

	}
	else
	{
		VectorSet(res_dir, 0,0.707,0.707);
		VectorSet(res_diffuse, 64,64,64);
		VectorSet(res_ambient, 192,192,192);
	}

	/*
	Minimum ambient, so models and viewmodels are never pure black on a Source
	map whose leaf ambient cube is missing or computes ~0.

	FTESurf Patch 148 -- THIS IS THE PALE PROPS, and it was a hue bug.

	It used to floor EACH CHANNEL INDEPENDENTLY:

	    for (k = 0; k < 3; k++)
	        if (res_ambient[k] < m) res_ambient[k] = m;

	A per-channel floor is a desaturation operator.  A warm leaf ambient of
	(90,55,35) -- R/B 2.57 -- comes out (90,64,64), R/B 1.41.  A dimmer warm one,
	(60,38,24), comes out (64,64,64): PERFECTLY NEUTRAL, and brighter than it
	should be.  That is exactly what was reported and measured -- surf_666's
	stage-1 ramp (which is models/props/666/s1_ramp1b.mdl, a static prop, not the
	brush entity three builds of this looked for) rendered at 0.73x its own
	texture with R/B 1.01 while the warm world floor beside it read 0.29x and
	R/B 1.59.  Every prop in a dim-to-moderate leaf went grey, which is why it
	read as "the entities in general look pale".

	Scale instead of clamp.  Lifting the whole colour by m/luminance guarantees
	the same thing the floor was for -- nothing renders black -- and cannot
	change the hue, because it is a multiply.  A leaf that is genuinely zero has
	no hue to preserve and gets the neutral floor, which is the only case where
	the old behaviour was right.

	FTESurf Patch 158 -- AND THE LIFT HAS TO DESATURATE AS IT LIFTS.  Patch 148
	turned pale props into BLUE ones, and this is where.

	Reported as "bright blue brushwork on the left of surf_666's !s 2, and it is
	a warm map".  Measured: they are four models/props/s2_d_1.mdl static props,
	mirrored by four identical ones on the right that look correct, so it is not
	the material (same model, same skin, same angles) and not the world lightmap
	(the faces around both sets measure R/B 2.51).  It is the leaf ambient cube,
	and the cube is real:

	  left  props, leaf 16884, nearest sample: every face R/B 0.22 - 0.66,
	                                           magnitudes 0.0010 - 0.0059
	  right props, leaf 16919, nearest sample: every face R/B 1.62 - 1.78,
	                                           magnitudes 0.0101 - 0.0557

	VRAD really did write a blue cube there.  It is also ten times darker than
	the one next to it and sits at ONE OR TWO UNITS of an 8-bit mantissa, which
	is the quantisation floor: at that level the hue is noise, not information.
	Source draws it as near-black and you never see the colour.

	We drew it as electric blue, because the floor multiplies.  sRGB-encoding
	(0.0016, 0.0003, 0.0059) gives about (4.4, 1.0, 17.6); its luminance is 3.85,
	so `m/lum` is 4.16 and the result is (18, 4, 73).  The lift is not adding a
	tint -- it is AMPLIFYING one that was always there and was always below the
	threshold of visibility.  Patch 148's per-channel floor accidentally hid it,
	by desaturating everything, which is why this arrived as a regression the
	moment the hue was preserved honestly.

	So the lift keeps the hue only to the extent that the sample earned it.
	t = lum/m is the fraction of the floor the data actually reached; the colour
	is blended toward its own luminance-grey by (1-t).  At t=1 -- a sample that
	needs no lift -- this is exactly Patch 148 and the warm props stay warm.  As
	t goes to 0 it becomes the neutral floor, which is the zero-luminance branch
	below, so the two cases are now one continuous rule rather than a cliff.

	`hl2_lt_min 0` turns the floor off entirely and is the exactly-like-Source
	setting: near-black leaf ambient renders near-black.  That is only a sensible
	option now because Patch 152 made the cube itself arrive correct.
	*/
	{
		static cvar_t *minamb;
		float m, lum;
		//16, not 64.  This exists so nothing renders pure black, and that is all
		//it should do.  At 64 it was lifting 486 of surf_666's 653 props and was
		//effectively the map's lighting -- which it could only be because the
		//ambient cube was arriving as zeros and, once that was fixed, arriving
		//un-gamma-encoded and ten times too dark.  With both fixed the real
		//values average 60/255 and the floor goes back to being a floor.
		if (!minamb) minamb = cvarfuncs->GetNVFDG("hl2_lt_min", "16", 0, "Minimum model ambient on Source/HL2 maps (0-255), applied by SCALING the colour rather than clamping each channel -- a per-channel clamp desaturates, which is what made every prop in a dim leaf render pale grey. 0 = off.", "");
		m = minamb->value;
		if (m > 0)
		{
			/*
			FTESurf Patch 304: WHICH VECTOR THE FLOOR IS ALLOWED TO ASK.

			This floor exists so nothing renders pure black, so it has to key on
			the darkest value the model will actually show.  Under the legacy fold
			that is res_ambient, which is the six-face mean and a fair proxy for
			"how bright is this sample".  Under the split fold res_ambient is the
			directional AMPLITUDE -- a difference, not a level -- and it is
			legitimately near zero for a bright, evenly-lit sample.  Reading it
			there would send a perfectly well-lit prop down the lum<=0.001 branch
			and clamp it flat to 16, and the lum<m branch would multiply a bright
			base by m/amp.  The darkest value the split fold can render is base,
			at w(n)=0, which is res_diffuse -- so that is what it asks.

			Both vectors are still scaled by the SAME factor, which is what keeps
			base:amp constant and so lifts the level without flattening the
			shading.  Only the question changes, not the remedy.
			*/
			float *level = (hl2_lt_fold && hl2_lt_fold->ival && prv->fold_prop) ? res_diffuse : res_ambient;
			lum = 0.3f*level[0] + 0.59f*level[1] + 0.11f*level[2];
			if (lum <= 0.001f)
			{	//no colour to preserve.
				VectorSet(res_diffuse, m, m, m);
				//...but under the split fold the directional half is a difference
				//that may be perfectly good -- a prop lit hard from one side has a
				//genuinely black unlit face -- and overwriting it with the floor
				//would delete the shading instead of flooring it.
				if (level == res_ambient)
					VectorSet(res_ambient, m, m, m);
			}
			else if (lum < m)
			{
				//t is how much of the floor the sample reached, and so how much
				//of its hue is signal rather than mantissa noise.  Each vector
				//is desaturated toward ITS OWN post-lift grey, not toward m --
				//res_diffuse carries the directional term and lands at a
				//different luminance, and pulling it to the ambient's grey
				//would flatten the shading as well as the colour.
				float t = lum/m, k;
				float ga, gd;
				int c;
				VectorScale(res_ambient, m/lum, res_ambient);
				VectorScale(res_diffuse, m/lum, res_diffuse);
				ga = 0.3f*res_ambient[0] + 0.59f*res_ambient[1] + 0.11f*res_ambient[2];
				gd = 0.3f*res_diffuse[0] + 0.59f*res_diffuse[1] + 0.11f*res_diffuse[2];
				k = 1.0f - t;
				for (c = 0; c < 3; c++)
				{
					res_ambient[c] = res_ambient[c]*t + ga*k;
					res_diffuse[c] = res_diffuse[c]*t + gd*k;
				}
			}
		}
	}
}
#else
static void VBSP_LightPointValues	(struct model_s *model, const vec3_t point, vec3_t res_diffuse, vec3_t res_ambient, vec3_t res_dir)
{
	VectorSet(res_diffuse, 64,64,64);
	VectorSet(res_ambient, 192,192,192);
	VectorSet(res_dir, 0,0.707,0.707);
}
static void VBSP_LoadLeafLight (model_t *mod, qbyte *mod_base, vlump_t *hdridx, vlump_t *ldridx, vlump_t *hdrvals, vlump_t *ldrvals, int version, qboolean usehdr)
{
}
#endif

/*
FTESurf Patch 268 C: the ambient cube itself, for vertexlit.glsl's #BUMPCUBE.

The same nearest sample VBSP_LightPointValues picks -- same leaf, same clamp, same
255-grid search, same strict tie-break -- written out again rather than hoisted out
of it, so that function is untouched and "hl2_cubelight 0 is today" needs no
argument.  Outside the #if above on purpose: with no leaf lighting loaded,
prv->leaflight is NULL and this answers zeros, which the shader turns into exactly 1.

The six faces go out LINEAR, as the lump decoded them.  The shader takes the ratio
of two evaluations of this one cube, which no scale could change, and applies the
display gamma to the ratio.  No hl2_lt_min floor either: a floor is about the level,
and the level still comes from VBSP_LightPointValues.
*/
static void VBSP_LightPointCube(struct model_s *model, const vec3_t point, vec3_t res_cube[6])
{
	vbspinfo_t *prv = (vbspinfo_t*)model->meshinfo;
	struct mleaflight_s *leaflight;
	struct leaflightpoint_s *best, *lp;
	mleaf_t *leaf;
	size_t i, d, bd = ~0;
	int leafnum, xyz[3], j;

	memset(res_cube, 0, sizeof(vec3_t)*6);
	if (!prv || !prv->leaflight || model->numleafs <= 0)
		return;
	leafnum = VBSP_PointLeafnum(model, point);
	if (leafnum < 0 || leafnum >= model->numleafs)
		leafnum = 0;
	leaf = model->leafs + leafnum;
	leaflight = prv->leaflight + leafnum;
	if (!leaflight->count)
		return;

	for (j = 0; j < 3; j++)
		xyz[j] = 255*(point[j] - leaf->minmaxs[j]) / (leaf->minmaxs[3+j]-leaf->minmaxs[j]);
	for (i = 0, best = lp = leaflight->point; i < (size_t)leaflight->count; i++, lp++)
	{
		int m[3];
		m[0] = xyz[0] - lp->x;
		m[1] = xyz[1] - lp->y;
		m[2] = xyz[2] - lp->z;
		d = DotProduct(m,m);
		if (bd > d)
		{
			bd = d;
			best = lp;
		}
	}
	for (j = 0; j < 6; j++)
		VectorCopy(best->rgb[j], res_cube[j]);
	//FTESurf Patch 302: the same term VBSP_LightPointValues adds, so #AMBIENTCUBE
	//(which REPLACES light.rgb outright, vertexlit.glsl:113-116) and #BUMPCUBE's
	//ratio see the same light the flat path does.  Both are off by default; if
	//they were fed the bounce-only cube while the flat path got the corrected
	//one, turning either on would silently darken every prop back again.
	//
	//FTESurf Patch 308: and on a BAKED prop the term is added anyway when
	//hl2_cubelight is on, because this cube feeds only a ratio and a ratio carries
	//no level -- see the essay on VBSP_AddWorldLightCube.  Without this, every prop
	//on a map that ships .vhv bakes (surf_fantasy: 2345 of 2345) gets a bounce-only
	//cube, whose anisotropy is too small to see -- which is the whole reason
	//"the rocks have no bumpmap relief" survived Patch 268 C and Patch 302 both.
	VBSP_AddWorldLightCube(model, point, res_cube,
		(hl2_cubelight && hl2_cubelight->ival) ? true : false);
}







static void VBSP_ComputeChecksum(model_t *mod, void *data, size_t length)
{
	unsigned int checksum = LittleLong (filefuncs->BlockChecksum(data, length));
	mod->checksum = mod->checksum2 = checksum;
}

//nettest: LZMA lump decompression (Source lump-compression, used by Strata BSP v25 and repacked classic
//maps).  A compressed lump begins with a 17-byte Source header -- magic 'LZMA', u32 uncompressed size,
//u32 lzma size, 5 LZMA property bytes -- then a raw LZMA1 stream.  Without this, a compressed lump's byte
//count doesn't divide by its struct size and every Load* rejects it as "funny lump size".
//(the malloc/free ISzAlloc vbsp_lzma_alloc is defined earlier, before VBSP_LoadGameLump, so the
//game-lump sub-lump decompressor can share it with this top-level lump decompressor.)

/*
FTESurf Patch 298 -- what a lump's length will be AFTER decompression, computed BEFORE it.

A compressed lump's filelen is its COMPRESSED size and its fourcc carries the uncompressed size;
an uncompressed lump has fourcc 0 and a filelen that is already the real length.  Every "does this
lump have any contents" test in VBSP_LoadModel used to run after the decompressor, where filelen
is always the real length.  Patch 298 hoists those tests ABOVE the decompressor (it has to -- the
answers decide what is worth decompressing), so they have to keep giving the same answer.  Reading
the fourcc is what makes the hoist provably equivalent rather than merely usually right: a lump
whose LZMA stream expands to zero bytes would otherwise flip a choice simply by being compressed.
*/
static unsigned int VBSP_LumpRealLen(qbyte *mod_base, size_t filelen, vlump_t *l)
{
	qbyte *d;
	if (l->filelen <= 0 || (size_t)l->fileofs + l->filelen > filelen)
		return 0;
	if (l->filelen < 17)
		return l->filelen;	//too short to carry the 17-byte Source LZMA header
	d = mod_base + l->fileofs;
	if (d[0]=='L' && d[1]=='Z' && d[2]=='M' && d[3]=='A')
		return (unsigned)d[4] | ((unsigned)d[5]<<8) | ((unsigned)d[6]<<16) | ((unsigned)d[7]<<24);
	return l->filelen;
}

//Decompress every compressed TOP-LEVEL lump into one model-lifetime buffer and rewrite its fileofs/filelen,
//so every Load* (which reads mod_base+fileofs) transparently sees plain data.  The whole file is copied
//first, so uncompressed lumps AND the game lump's ABSOLUTE sub-offsets stay valid.  Returns the new base,
//the original base if nothing was compressed (classic maps pay nothing), or NULL on a decode failure.
//
/*
FTESurf Patch 298 -- skiplump: DO NOT DECOMPRESS WHAT NOTHING IS GOING TO READ.

Measured on a NanoPi R6C (rk3588, aarch64) running the dedicated server, gdb sampling a map load
fourteen times:  every single sample was inside

    VBSP_LoadMap -> VBSP_LoadModel -> VBSP_DecompressLumps -> LzmaDecode -> LzmaDec_DecodeReal_3

with the four worker threads parked in futex_wait.  Map load on this engine is single-threaded LZMA
and essentially nothing else.  Load time tracks LZMA OUTPUT BYTES at roughly 17 MB/s, which fits
four maps spanning two orders of magnitude:

    map               on-disk   decompressed   LZMA out   load
    surf_affliction    200 MB       478 MB      ~350 MB   19.7 s
    surf_reverie       187 MB       326 MB      ~189 MB   13.7 s
    kz_bhop_badg3s     495 MB       495 MB         none    2.4 s   <-- biggest file, fastest load
    surf_kitsune         4 MB        12 MB        ~9 MB    0.5 s

kz_bhop_badg3s is the control: the largest map in the library loads in a tenth of surf_affliction's
time because VBSP wrote none of its lumps compressed.  Static props were the original suspect and
are innocent -- `hl2_propcollision 0`, which loads no prop model at all, moved surf_affliction from
19450ms to 19507ms.

WHAT IS BEING DECOMPRESSED.  On surf_affliction, LIGHTING and LIGHTING_HDR are 35 MB each on disk
and 167 MB each expanded: 334 MB, 71% of the whole decompression bill, for a map whose collision
and entities are a few MB.  A dedicated server has no renderer and cannot use a lightmap at all,
and VBSP_LoadModel already knows that -- VBSP_LoadLighting sits behind `if (noerrors && haverenderer)`.
The guard was simply applied twelve lines too late to save the expensive part.

AND HALF OF IT IS WASTED ON THE CLIENT TOO.  VBSP_LoadLighting is an if/else-if: it reads exactly
ONE of the LDR/HDR pair.  The other 167 MB was decompressed and never looked at on every machine
that has ever loaded the map.  Same for the leaf ambient pair.

WHAT MAY BE SKIPPED IS DECIDED BY THE CALLER, not here, and the rule it must obey is that a lump is
only listed when the branch that would read it provably cannot be taken in THIS configuration --
never merely "is probably unused".  VBSP_LoadLeafLight is the reason that distinction is written
down: it falls back from HDR to LDR and then back to HDR again, so "the map is HDR" is NOT on its
own sufficient to retire the LDR half.  See the skip table in VBSP_LoadModel.

Only COMPRESSED lumps are ever retired.  An uncompressed one costs no decode time and no extra
memory (it is already in the file buffer that was copied wholesale), so skipping it would buy
nothing and would change behaviour on classic maps for no reason.

A RETIRED LUMP'S DIRECTORY ENTRY IS LEFT EXACTLY AS THE FILE HAS IT -- not zeroed.  The first
draft of this patch zeroed filelen, on the reasoning that a lump left pointing at an LZMA header
would be read as struct data.  That is wrong here and the counter-example is load-bearing:
VBSP_LoadFaces:3671 chooses between the LDR and HDR FACE lumps by probing

    l2->filelen && !(hl2_favour_ldr->ival && lumps[VLUMP_LIGHTING_LDR].filelen)

-- it reads LIGHTING_LDR's LENGTH as an existence flag, and it is NOT behind `haverenderer`,
because faces are needed for surfaces and collision on a dedicated server too.  Zeroing
LIGHTING_LDR would therefore have silently moved the SERVER onto the HDR face set while every
client stayed on the LDR one: different geometry on the two sides of prediction, from a patch
whose entire purpose is to not change what gets loaded.  Note also that this predicate is NOT the
same question as usehdr below -- its first term is FACES_HDR, not LIGHTING_HDR -- so the hoisted
value cannot simply be substituted into it either.

Leaving the entry alone makes every existence probe in the file see precisely what it saw before
the patch, which is the property that has to hold.  What makes it safe to leave a retired lump
pointing at undecompressed bytes is the caller's invariant: a lump is only ever listed when no
branch that reads its CONTENTS can be taken in this configuration.  The readers, all verified:
LIGHTING_{LDR,HDR} -> VBSP_LoadLighting (behind haverenderer, and an if/else-if that takes one);
LEAFLIGHT{I,V}_{HDR,LDR} -> VBSP_LoadLeafLight (behind haverenderer, three-way choice);
CUBEMAPS -> VBSP_LoadCubemaps (behind haverenderer); DISP_LMALPHA and DISP_LMCOORDS -> nothing in
this plugin reads them at all.  Anything added to the skip table later owes the same census.

FTESurf Patch 302 paying that: WORLDLIGHTS_{LDR,HDR} -> VBSP_LoadWorldLights, behind haverenderer,
sole reader, and no one reads their LENGTH either.  Retired on the !haverenderer arm ONLY.  They are
deliberately NOT retired when drawing, even though the loader picks one: it picks by content and
FALLS BACK LDR->HDR the way VBSP_LoadLeafLight does, which is the exact three-way this rule was
written around, so "the map is HDR" does not make the LDR half dead.
*/
static qbyte *VBSP_DecompressLumps(model_t *mod, qbyte *mod_base, size_t filelen, dvbspheader_t *header, const qboolean *skiplump)
{
	size_t i, extra = 0, tail, saved = 0;
	int anycompressed = 0, numskipped = 0;
	qbyte *newbase;

	for (i = 0; i < HL2_MAXLUMPS; i++)
	{
		vlump_t *l = &header->lumps[i];
		qbyte *d;
		unsigned int actual;
		if (l->filelen < 17 || (size_t)l->fileofs + l->filelen > filelen)
			continue;
		d = mod_base + l->fileofs;
		if (d[0]=='L' && d[1]=='Z' && d[2]=='M' && d[3]=='A')
		{
			actual = (unsigned)d[4] | ((unsigned)d[5]<<8) | ((unsigned)d[6]<<16) | ((unsigned)d[7]<<24);
			if (skiplump && skiplump[i])
			{	//Patch 298: retired -- costs no decode and no tail space.  The directory entry is
				//deliberately left untouched; see the essay above for why zeroing it is wrong.
				saved += actual;
				numskipped++;
				continue;
			}
			extra += actual;
			anycompressed = 1;
		}
	}
	if (numskipped)
		Con_DPrintf("%s: %i compressed lump%s retired unread, %.1f MB not decompressed\n",
					mod->name, numskipped, numskipped==1?"":"s", saved/(1024*1024.0));
	if (!anycompressed)
		return mod_base;	//uncompressed classic map: leave it exactly as-is

	newbase = plugfuncs->GMalloc(&mod->memgroup, filelen + extra);
	memcpy(newbase, mod_base, filelen);
	tail = filelen;
	for (i = 0; i < HL2_MAXLUMPS; i++)
	{
		vlump_t *l = &header->lumps[i];
		qbyte *d;
		unsigned int actual, lzmasize;
		SizeT destLen, srcLen;
		ELzmaStatus status;
		SRes res;
		if (l->filelen < 17 || (size_t)l->fileofs + l->filelen > filelen)
			continue;
		if (skiplump && skiplump[i])
			continue;	//Patch 298: retired above.  Its entry still describes the compressed bytes,
						//which is what every existence probe in this file expects to see.
		d = mod_base + l->fileofs;	//read the (compressed) source from the ORIGINAL file (never overwritten)
		if (!(d[0]=='L' && d[1]=='Z' && d[2]=='M' && d[3]=='A'))
			continue;
		actual   = (unsigned)d[4] | ((unsigned)d[5]<<8) | ((unsigned)d[6]<<16) | ((unsigned)d[7]<<24);
		lzmasize = (unsigned)d[8] | ((unsigned)d[9]<<8) | ((unsigned)d[10]<<16) | ((unsigned)d[11]<<24);
		if ((size_t)17 + lzmasize > l->filelen)
		{
			Con_Printf(CON_ERROR "VBSP: lump %i LZMA stream overruns lump in %s\n", (int)i, mod->name);
			return NULL;
		}
		destLen = actual;
		srcLen  = lzmasize;
		res = LzmaDecode(newbase + tail, &destLen, d + 17, &srcLen, d + 12, 5, LZMA_FINISH_END, &status, &vbsp_lzma_alloc);
		if (res != SZ_OK || destLen != actual)
		{
			Con_Printf(CON_ERROR "VBSP: lump %i LZMA decode failed (%i) in %s\n", (int)i, (int)res, mod->name);
			return NULL;
		}
		l->fileofs = tail;	//point this lump at its decompressed copy in the tail
		l->filelen = actual;
		tail += actual;
	}
	return newbase;
}

#ifdef HAVE_CLIENT
/*
=================
VBSP_LoadCubemaps							FTESurf Build 13

env_cubemap, which we did not implement AT ALL, and which is why some of
surf_666's brushes render bright saturated blue on a map with no blue in it.

$envmap env_cubemap is the standard Source idiom -- it means "reflect the baked
cubemap from the nearest env_cubemap entity".  20 of surf_666's 149 packed VMTs
say exactly that, on its tile floors and its glass.  mat_vmt.c saw the name,
picked a vmt/lightmapped#ENVFROM* permutation so the shader samples
s_reflectcube, and then deliberately emitted no `reflectcube` line for it
(there was no per-surface cubemap to name).  So the backend fell through:

    curtexnums->reflectcube   unset, because we never wrote one
    curbatch->envmap          never set, because nothing filled surf->envmap
    tex_reflectcube           = R_GetDefaultEnvmap() = THE SKYBOX

Every env_cubemap surface in the game was reflecting the sky.  Measured on
surf_666, whose sky is sky45, straight out of the six VTF headers' own
reflectivity field:

    sky45up  0.0000 0.1528 0.1286      sky45rt  0.0000 0.0514 0.0406
    sky45dn  0.0000 0.0001 0.0001      sky45ft  0.0000 0.0497 0.0398
    sky45lf  0.0000 0.0514 0.0407      sky45bk  0.0000 0.0631 0.0475

RED IS EXACTLY ZERO ON ALL SIX FACES.  A pure cyan sky, mirrored onto the tile
and glass of a map whose world lightmap measures R/B 1.88.  That is the report.

The engine already has the whole mechanism and it was simply never fed:
menvmap_t/mod->envmaps, msurface_t::envmap, the batch key in gl_model.c:3617
and the bind in gl_backend.c:1408.  It is driven by BSPX ENVMAP+SURFENVMAP for
Q1BSP (q1bsp.c:3230).  LUMP_CUBEMAPS is the same data -- origin plus face size
-- so this fills the same fields from it, and nearest-by-centroid is what
Source does too.

WHEN A MAP HAS NO CUBEMAPS AT ALL, and surf_666 has none (LUMP_CUBEMAPS is 0
bytes, zero env_cubemap entities -- the mapper never ran buildcubemaps), Source
falls back to the map's own materials/maps/<map>/cubemapdefault.vtf, a neutral
grey.  It does NOT fall back to the sky.  So a synthetic default entry is
created for that case, which is what keeps the skybox out of the reflection
even on a map that never built any.
=================
*/
static void VBSP_LoadCubemaps (model_t *mod, qbyte *mod_base, vlump_t *l)
{
	vbspinfo_t	*prv = (vbspinfo_t*)mod->meshinfo;
	menvmap_t	*out;
	size_t		count, i, n;
	unsigned	best;
	char		base[MAX_QPATH], imagename[MAX_QPATH], *s;
	struct { int origin[3]; int size; } *in = (void*)(mod_base + l->fileofs);

	mod->envmaps = NULL;
	mod->numenvmaps = 0;
	cube_stat_count = cube_stat_missing = cube_stat_default = 0;

	if (hl2_cubemaps && !hl2_cubemaps->ival)
		return;

	count = (l->filelen % sizeof(*in)) ? 0 : l->filelen / sizeof(*in);

	//"maps/surf_666.bsp" -> "surf_666".  COM_FileBase is engine-only; plugins
	//are separately linked shared objects and see only the function tables.
	s = strrchr(mod->name, '/');
	Q_strlcpy(base, s ? s+1 : mod->name, sizeof(base));
	s = strrchr(base, '.');
	if (s)
		*s = 0;

	if (!count)
	{	//no cubemaps built.  Source shows cubemapdefault here; anything is
		//better than the sky, which is what we were showing.
		out = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out));
		VectorClear(out->origin);
		out->cubesize = 32;
		Q_snprintfz(imagename, sizeof(imagename), "materials/maps/%s/cubemapdefault.vtf", base);
		out->image = modfuncs->GetTexture(imagename, NULL, IF_TEXTYPE_CUBE|IF_LINEAR|IF_NOREPLACE, NULL, NULL, 0, 0, PTI_INVALID);
		mod->envmaps = out;
		mod->numenvmaps = 1;
		cube_stat_default = 1;
	}
	else
	{
		out = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out)*count);
		for (i = 0; i < count; i++)
		{
			out[i].origin[0] = LittleLong(in[i].origin[0]);
			out[i].origin[1] = LittleLong(in[i].origin[1]);
			out[i].origin[2] = LittleLong(in[i].origin[2]);
			out[i].cubesize  = LittleLong(in[i].size);

			//VBSP names the baked face by the entity's INTEGER origin.  A
			//cubesize of 0 means "use the map default", which vtex writes at
			//the same path, so the name is the same either way.
			Q_snprintfz(imagename, sizeof(imagename), "materials/maps/%s/c%i_%i_%i.vtf", base,
						(int)out[i].origin[0], (int)out[i].origin[1], (int)out[i].origin[2]);
			out[i].image = modfuncs->GetTexture(imagename, NULL, IF_TEXTYPE_CUBE|IF_LINEAR|IF_NOREPLACE, NULL, NULL, 0, 0, PTI_INVALID);
		}
		mod->envmaps = out;
		mod->numenvmaps = count;
		cube_stat_count = count;
	}

	/*
	Nearest cubemap per surface, by face centroid -- Source's rule.

	The centroid is walked out of surfedges/edges/vertexes here rather than
	read from surf->mesh, because at load time the mesh is ALLOCATED but not
	yet FILLED: VBSP_BuildSurfMesh runs later, from the batch builder's
	buildfunc, and gl_model.c reads surf->envmap to KEY those batches.  Reading
	xyz_array here would give every surface the centroid of the origin, which
	is the same class of mistake as build 12's lighting origin.
	*/
	for (i = 0; i < mod->numsurfaces; i++)
	{
		msurface_t *surf = mod->surfaces + i;
		dispinfo_t *d = prv->surfdisp ? prv->surfdisp[i] : NULL;
		vec3_t mins, maxs, mid;
		float bestdist = FLT_MAX, dist;
		int e;

		if (d)
		{	//a displacement already carries its own axis-aligned bounds, built
			//when its verts were.  Cheaper and exactly as good as re-walking them.
			VectorCopy(d->aamin, mins);
			VectorCopy(d->aamax, maxs);
		}
		else if (surf->numedges > 0)
		{
			qboolean first = true;
			for (e = 0; e < surf->numedges; e++)
			{
				int lindex = mod->surfedges[surf->firstedge + e];
				int edgevert = lindex <= 0;
				unsigned vertidx;
				if (edgevert)
					lindex = -lindex;
				if (lindex < 0 || lindex >= mod->numedges)
					continue;
				vertidx = mod->edges[lindex].v[edgevert];
				if (first)
				{
					VectorCopy(mod->vertexes[vertidx].position, mins);
					VectorCopy(mod->vertexes[vertidx].position, maxs);
					first = false;
				}
				else
					AddPointToBounds(mod->vertexes[vertidx].position, mins, maxs);
			}
			if (first)
				continue;
		}
		else
			continue;

		VectorAvg(mins, maxs, mid);

		best = 0;
		for (n = 0; n < mod->numenvmaps; n++)
		{
			vec3_t diff;
			VectorSubtract(mod->envmaps[n].origin, mid, diff);
			dist = DotProduct(diff, diff);
			if (bestdist > dist)
			{
				bestdist = dist;
				best = n;
			}
		}
		surf->envmap = mod->envmaps[best].image;
	}

	for (i = 0; i < mod->numenvmaps; i++)
		if (!TEXLOADED(mod->envmaps[i].image))
			cube_stat_missing++;
}
#endif

static qboolean VBSP_LoadModel(model_t *mod, qbyte *mod_base, size_t filelen, char *loadname)
{
	dvbspheader_t *srcheader = (void*)mod_base;
	dvbspheader_t header;
	vbspinfo_t *prv = mod->meshinfo;
	size_t i;
	qboolean noerrors = true;
#ifdef HAVE_CLIENT
	qboolean haverenderer = qrenderer != QR_NONE;
#else
	const qboolean haverenderer = false;	//Patch 298: a SERVERONLY build has no renderer by construction
#endif
	qboolean usehdr;						//Patch 298: hoisted above VBSP_DecompressLumps, see below
	qboolean lumpskip[HL2_MAXLUMPS];

	VBSP_TranslateContentBits_Setup(prv);

	mod->lightmaps.width = LMBLOCK_SIZE_MAX;
	mod->lightmaps.height = LMBLOCK_SIZE_MAX;

	mod->fromgame = fg_new;
	mod->engineflags |= MDLF_NEEDOVERBRIGHT;
	header.version = LittleLong(srcheader->version);
	for (i=0 ; i<HL2_MAXLUMPS ; i++)
	{
		header.lumps[i].filelen = LittleLong (srcheader->lumps[i].filelen);
		header.lumps[i].fileofs = LittleLong (srcheader->lumps[i].fileofs);
		header.lumps[i].version = LittleLong (srcheader->lumps[i].version);	//nettest: per-lump version drives 32-bit struct choice (v25)
		header.lumps[i].fourcc  = LittleLong (srcheader->lumps[i].fourcc);	//(LZMA uncompressed-size hint)
		//fixme: truncate lumps if they go off the end
	}

	/*
	FTESurf Patch 298 -- the retirement list, built before anything is decompressed.

	The rule for putting a lump on this list is not "it looks unused".  It is: NO BRANCH THAT READS
	THIS LUMP'S CONTENTS CAN BE TAKEN IN THIS CONFIGURATION.  VBSP_LoadLeafLight is why that has to
	be stated so carefully -- it chooses HDR, then falls back to LDR, then falls back to HDR again,
	so "the map is being drawn in HDR" is NOT on its own enough to retire the LDR half: a map whose
	HDR ambient lumps are VRAD's all-zero stubs (see Patch 152) reaches the LDR branch anyway.

	The lengths come from VBSP_LumpRealLen rather than from filelen directly, because filelen is
	still the COMPRESSED size at this point; that is what makes hoisting these tests above the
	decompressor equivalent to leaving them below it.
	*/
	{
		unsigned int lit_ldr = VBSP_LumpRealLen(mod_base, filelen, &header.lumps[VLUMP_LIGHTING_LDR]);
		unsigned int lit_hdr = VBSP_LumpRealLen(mod_base, filelen, &header.lumps[VLUMP_LIGHTING_HDR]);
		unsigned int ami_hdr = VBSP_LumpRealLen(mod_base, filelen, &header.lumps[VLUMP_LEAFLIGHTI_HDR]);
		unsigned int amv_hdr = VBSP_LumpRealLen(mod_base, filelen, &header.lumps[VLUMP_LEAFLIGHTV_HDR]);
		unsigned int ami_ldr = VBSP_LumpRealLen(mod_base, filelen, &header.lumps[VLUMP_LEAFLIGHTI_LDR]);
		unsigned int amv_ldr = VBSP_LumpRealLen(mod_base, filelen, &header.lumps[VLUMP_LEAFLIGHTV_LDR]);
		qboolean ambhdr;

		//VBSP_LoadLighting's own choice, verbatim, and the one the leaf ambient cube must agree with.
		usehdr = lit_hdr && !(hl2_favour_ldr->ival && lit_ldr);
		//VBSP_LoadLeafLight's FIRST branch, verbatim.  Only when this is true is the LDR half dead.
		ambhdr = usehdr && ami_hdr && amv_hdr;

		memset(lumpskip, 0, sizeof(lumpskip));
		if (hl2_lumpretire && !hl2_lumpretire->ival)
			;	//Patch 298 falsifier: retire nothing, i.e. exactly the pre-patch behaviour.
				//`usehdr` above is still the value the load below uses either way.
		else if (!haverenderer)
		{	//A dedicated server draws nothing.  VBSP_LoadLighting, VBSP_LoadLeafLight and
			//VBSP_LoadCubemaps are all behind `haverenderer` below, and the two displacement
			//lightmap lumps have no reader anywhere in this plugin.  On surf_affliction this is
			//334 MB of lightmap that was being expanded and held for a process that has no GL
			//context to upload it to -- 71% of the map's entire decompression cost.
			lumpskip[VLUMP_LIGHTING_LDR]	= true;
			lumpskip[VLUMP_LIGHTING_HDR]	= true;
			lumpskip[VLUMP_LEAFLIGHTI_HDR]	= true;
			lumpskip[VLUMP_LEAFLIGHTI_LDR]	= true;
			lumpskip[VLUMP_LEAFLIGHTV_HDR]	= true;
			lumpskip[VLUMP_LEAFLIGHTV_LDR]	= true;
			lumpskip[VLUMP_CUBEMAPS]		= true;
			lumpskip[VLUMP_DISP_LMALPHA]	= true;
			lumpskip[VLUMP_DISP_LMCOORDS]	= true;
			//FTESurf Patch 302, and it owes this table the same census the rule
			//demands: VLUMP_WORLDLIGHTS_{LDR,HDR} have exactly one reader,
			//VBSP_LoadWorldLights, and it is inside the `if (noerrors &&
			//haverenderer)` block below.  Nothing else in this plugin touches
			//either lump -- not by contents and not by length, which is the
			//distinction VBSP_LoadFaces:3671 makes load-bearing for LIGHTING_LDR.
			//So on a dedicated server no branch that reads them can be taken.
			//53 KB decompressed on surf_tensor2 for data a server cannot draw.
			lumpskip[VLUMP_WORLDLIGHTS_LDR]	= true;
			lumpskip[VLUMP_WORLDLIGHTS_HDR]	= true;
		}
		else
		{	//Drawing: retire the half of each pair whose branch cannot be reached.  This is the
			//half of the win that lands on the PLAYER's machine -- a map ships both lightmaps and
			//the renderer has only ever used one of them.
			lumpskip[usehdr ? VLUMP_LIGHTING_LDR : VLUMP_LIGHTING_HDR] = true;
			if (ambhdr)
				lumpskip[VLUMP_LEAFLIGHTI_LDR] = lumpskip[VLUMP_LEAFLIGHTV_LDR] = true;
			else if (ami_ldr && amv_ldr)	//the LDR branch is the one that will be taken
				lumpskip[VLUMP_LEAFLIGHTI_HDR] = lumpskip[VLUMP_LEAFLIGHTV_HDR] = true;
		}
	}

	//nettest: transparently decompress any LZMA-compressed lumps (repoints mod_base + rewrites the lump
	//table, so every Load* below sees plain data).  Must run before LoadMapArchive / the Load* calls.
	mod_base = VBSP_DecompressLumps(mod, mod_base, filelen, &header, lumpskip);
	if (!mod_base)
		return false;

	if (header.lumps[VLUMP_ZIPFILE].filelen)
		modfuncs->LoadMapArchive(mod, mod_base+header.lumps[VLUMP_ZIPFILE].fileofs, header.lumps[VLUMP_ZIPFILE].filelen);

	// load into heap
	noerrors = noerrors && VBSP_LoadVertexes		(mod, mod_base, &header.lumps[VLUMP_VERTEXES]);
	noerrors = noerrors && VBSP_LoadEdges			(mod, mod_base, &header.lumps[VLUMP_EDGES]);
	noerrors = noerrors && VBSP_LoadSurfedges		(mod, mod_base, &header.lumps[VLUMP_SURFEDGES]);
#ifdef HAVE_CLIENT
	if (noerrors && haverenderer)
		VBSP_LoadLighting							(mod, mod_base, &header.lumps[VLUMP_LIGHTING_LDR], &header.lumps[VLUMP_LIGHTING_HDR]);
#endif
	noerrors = noerrors && VBSP_LoadSurfaces		(mod, mod_base, &header.lumps[VLUMP_TEXINFO]);
	noerrors = noerrors && VBSP_LoadPlanes			(mod, mod_base, &header.lumps[VLUMP_PLANES]);
	noerrors = noerrors && VBSP_LoadTexInfo			(mod, mod_base, header.lumps, loadname);
	if (noerrors)	//takes the whole lump table: it also reads VLUMP_OVERLAYS, and it must run after
		VBSP_LoadEntities							(mod, mod_base, header.lumps);	//VBSP_LoadTexInfo so nTexInfo resolves to a material name
	noerrors = noerrors && VBSP_LoadFaces			(mod, mod_base, header.lumps, header.version);
#ifdef HAVE_CLIENT
	if (noerrors && haverenderer)	//FTESurf Patch 268 D1: needs texinfo, faces and the lighting lump; before displacements only because nothing later moves samples or extents
		VBSP_ProbeLightmapStride				(mod);
#endif
	noerrors = noerrors && VBSP_LoadDisplacements	(mod, mod_base, header.lumps);
	noerrors = noerrors && VBSP_LoadMarksurfaces	(mod, mod_base, &header.lumps[VLUMP_LEAFFACES]);
	noerrors = noerrors && VBSP_LoadVisibility		(mod, mod_base, &header.lumps[VLUMP_VISIBILITY]);
	noerrors = noerrors && VBSP_LoadBrushSides		(mod, mod_base, &header.lumps[VLUMP_BRUSHSIDES]);
	noerrors = noerrors && VBSP_LoadBrushes			(mod, mod_base, &header.lumps[VLUMP_BRUSHES]);
	noerrors = noerrors && VBSP_LoadLeafBrushes		(mod, mod_base, &header.lumps[VLUMP_LEAFBRUSHES]);
	noerrors = noerrors && VBSP_LoadLeafs			(mod, mod_base, &header.lumps[VLUMP_LEAFS], header.version);
	noerrors = noerrors && VBSP_LoadNodes			(mod, mod_base, &header.lumps[VLUMP_NODES]);
	noerrors = noerrors && VBSP_LoadSubmodels		(mod, mod_base, &header.lumps[VLUMP_MODELS]);
	noerrors = noerrors && VBSP_LoadAreas			(mod, mod_base, &header.lumps[VLUMP_AREAS]);
	noerrors = noerrors && VBSP_LoadAreaPortals		(mod, mod_base, &header.lumps[VLUMP_AREAPORTALS], &header.lumps[VLUMP_AREAPORTALVERTS]);
#ifdef HAVE_CLIENT
	if (noerrors && haverenderer)
	{
		/*
		The SAME question VBSP_LoadFaces asks, asked once and handed to both:
		is this map being drawn in HDR.  An HDR ambient cube belongs with HDR
		lightmaps and an LDR one with LDR lightmaps -- and a map compiled LDR
		still ships a full-size, all-zero HDR ambient lump, which is what used
		to win here.  See VBSP_LoadLeafLight.
		*/
		//Patch 298: `usehdr` is now computed once, above VBSP_DecompressLumps, because the
		//retirement list needs the answer before it can know which lumps are worth expanding.
		//It is the same expression, off VBSP_LumpRealLen instead of raw filelen.

		VBSP_LoadLeafLight						(mod, mod_base, &header.lumps[VLUMP_LEAFLIGHTI_HDR], &header.lumps[VLUMP_LEAFLIGHTI_LDR],
																	&header.lumps[VLUMP_LEAFLIGHTV_HDR], &header.lumps[VLUMP_LEAFLIGHTV_LDR], header.version, usehdr);

		//FTESurf Patch 302: after VBSP_LoadLeafLight because it answers the same
		//HDR-or-LDR question and augments the very cube that loads, and before
		//VBSP_LoadGameLump below, which is what parses the static props.  Called
		//bare, like its two neighbours: a missing or malformed optional lighting
		//lump must leave props on bounce-only light, never fail the map load.
		VBSP_LoadWorldLights						(mod, mod_base, &header.lumps[VLUMP_WORLDLIGHTS_HDR], &header.lumps[VLUMP_WORLDLIGHTS_LDR], usehdr);

		//after LoadFaces (needs surfaces + surfedges) and LoadDisplacements
		//(needs surfdisp), before the batch builder reads surf->envmap.
		VBSP_LoadCubemaps							(mod, mod_base, &header.lumps[VLUMP_CUBEMAPS]);
	}
#endif
	noerrors = noerrors && VBSP_LoadGameLump		(mod, mod_base, &header.lumps[VLUMP_GAMELUMP]);

	if (!noerrors)
		return false;
#ifdef HAVE_SERVER
	mod->funcs.FatPVS				= VBSP_FatPVS;
	mod->funcs.EdictInFatPVS		= VBSP_EdictInFatPVS;
	mod->funcs.FindTouchedLeafs		= VBSP_FindTouchedLeafs;
#endif
#ifdef HAVE_CLIENT
	mod->funcs.LightPointValues		= VBSP_LightPointValues;
//	mod->funcs.StainNode			= VBSP_StainNode;
//	mod->funcs.MarkLights			= VBSP_MarkLights;
	mod->funcs.GenerateShadowMesh	= VBSP_GenerateShadowMesh;
#endif
	mod->funcs.ClusterPVS			= VBSP_ClusterPVS;
	mod->funcs.ClusterPHS			= VBSP_ClusterPHS;
	mod->funcs.ClusterForPoint		= VBSP_PointCluster;

	mod->funcs.SetAreaPortalState	= VBSP_SetAreaPortalState;
	mod->funcs.AreasConnected		= VBSP_AreasConnected;
	mod->funcs.LoadAreaPortalBlob	= VBSP_LoadAreaPortalBlob;
	mod->funcs.SaveAreaPortalBlob	= VBSP_SaveAreaPortalBlob;

	mod->funcs.PrepareFrame			= VBSP_PrepareFrame;
	mod->funcs.InfoForPoint			= VBSP_InfoForPoint;

	//displacements suck
	for (i = 0; i < prv->numdisplacements; i++)
		VBSP_FindTouchedLeafs(mod, &prv->displacements[i].pvs, prv->displacements[i].aamin, prv->displacements[i].aamax);
//	if (noerrors)
//		CM_CreatePatchesForLeafs (mod, prv);

	return true;
}

/*
==================
CM_LoadMap

Loads in the map and all submodels
==================
*/
static qboolean VBSP_LoadMap (model_t *mod, void *filein, size_t filelen)
{
	unsigned		*buf;
	int				i;
	dvbspheader_t	header;
	model_t			*wmod = mod;
	char			loadname[32];

#ifdef HAVE_CLIENT
	qbyte *facedata = NULL;
	unsigned int facesize = 0;
#endif
	vbspinfo_t	*prv;

	filefuncs->FileBase (mod->name, loadname, sizeof(loadname));

	// free old stuff, just in case.
	mod->meshinfo = prv = plugfuncs->GMalloc(&mod->memgroup, sizeof(*prv));

	mod->type = mod_brush;

	//
	// load the file
	//
	buf = (unsigned	*)filein;
	if (!buf)
	{
		Con_Printf (CON_ERROR "Couldn't load %s\n", mod->name);
		return false;
	}

	header = *(dvbspheader_t *)(buf);
	header.magic = LittleLong(header.magic);
	header.version = LittleLong(header.version);

	ClearBounds(mod->mins, mod->maxs);

	switch(header.version)
	{
	case 17:	//vampire
	case 18:	//beta
	case 19:	//hl2,cs:s,hl2dm
	case 20:	//portal, l4d, hl2ep2
	case 21:	//cs:go, portal 2, l4d2
	//case 22:	//dota 2
	//case 23:	//dota 2
	case 25:	//nettest: Strata Source (Momentum/P2CE/etc). Widened 32-bit lumps + LZMA lump compression -- gated per lump.version below.
	//case 27:	//'contagion'
	//case 29:	//'titanfall'
		if (!VBSP_LoadModel(mod, filein, filelen, loadname))
			return false;
		break;
	default:
		Con_Printf (CON_ERROR "VBSP with unknown version (%s: %i should be 18, 19, 20, 21, or 25)\n"
			, mod->name, header.version);
		return false;
	}

	/*
	FTESurf Build 8.  AREAPORTALS NOW START OPEN, WHICH IS WHAT SOURCE DOES.

	This used to start every portal CLOSED unless map_autoopenportals was set,
	on the Quake 2 model where the gamecode opens each one as its door moves.
	Applied to a Source map that is simply wrong, and it is not a small wrong:

	  * A leaf whose area bit is clear is not drawn at all
	    (VBSP_RecursiveWorldNode), and the prop and displacement loops apply the
	    same test.
	  * Source initialises its portal state OPEN and closes portals on demand --
	    a func_areaportal follows the door it is tied to, and an untied one just
	    stays open forever.
	  * And a Source map does not need a func_areaportal per area.  surf_666 has
	    FOURTEEN areas, ONE (dummy) areaportal entry and NO func_areaportal
	    entities at all, so there was never anything that could have opened
	    them.  Measured with the area print in VBSP_PrepareFrame: standing at
	    the spawn reported `area 10 of 14: 1 area(s) visible <- SEALED IN`.
	    Thirteen fourteenths of the map could not be drawn from where you stand.

	map_autoopenportals is left alone -- it is the engine's shared cvar and only
	ever forces open, which is now the default anyway.  hl2_areaportals 0 is the
	switch that goes the other way, so the old behaviour is one cvar away and
	the A/B stays possible.

	NOT DONE, deliberately, and it is a performance opportunity rather than a
	bug: honouring a func_areaportal's StartOpen 0 would let a sealed section
	stay culled.  We freeze doors shut (see SV_SpawnDoor), so a portal closed at
	spawn would stay closed forever -- and hiding a route the player can walk
	down is a far worse failure than drawing a room they cannot see into.
	*/
	if (hl2_areaportals->ival)
		memset (prv->portalopen, 1, sizeof(prv->portalopen));	//Source's initial state
	else
		memset (prv->portalopen, 0, sizeof(prv->portalopen));	//Quake 2's, for the A/B
	FloodAreaConnections (prv);
	prv->lastreportedarea = -1;	//FTESurf: -1 so area 0 still counts as a change
	prv->areastatframes = 0;
	prv->nextareareport = 0;
	vbsp_leaf_invis = vbsp_leaf_areakill = 0;
	vbsp_mark_runs = vbsp_mark_leaves = 0;	//FTESurf Patch 273
	prv->markstatframes = 0;
	vbsp_pvs_calls = vbsp_pvs_hn = vbsp_pvs_visits = vbsp_pvs_overflow = vbsp_pvs_hnhit = 0;
	vbsp_badclusterqueries = 0;	//FTESurf Patch 277(b): the once-per-map cluster-query warning re-arms with the map
	//and drop the headnode memo: its key is a model pointer, and this one is new.
	vbsp_hnv_mod = NULL;

	//FTESurf Build 8: the prop models load after this point (lazily, on worker
	//threads, driven from VBSP_BuildBIHMain), so this is where their collision
	//census starts.  Mod_ClearAll has already dropped the last map's models.
	Mod_PHY_ResetStats();
	Mat_VMT_ResetStats();

	mod->nummodelsurfaces = mod->numsurfaces;
	memset(&mod->batches, 0, sizeof(mod->batches));
	mod->vbos = NULL;

	mod->numsubmodels = VBSP_NumInlineModels(mod);

	//in case anyone wants to know the typical player size...
	VectorSet(mod->hulls[1].clip_mins, -16,-16,-36);
	VectorSet(mod->hulls[1].clip_maxs, 16,16,36);
	mod->hulls[0].firstclipnode = prv->cmodels[0].headnode-mod->nodes;
	mod->rootnode = prv->cmodels[0].headnode;
	mod->nummodelsurfaces = prv->cmodels[0].numsurfaces;

#ifdef HAVE_CLIENT
	if (qrenderer != QR_NONE)
	{
		builddata_t *bd = plugfuncs->Malloc(sizeof(*bd) + facesize*mod->nummodelsurfaces);
		bd->buildfunc = VBSP_BuildSurfMesh;
		bd->paintlightmaps = true;
		memcpy(bd+1, facedata + mod->firstmodelsurface*facesize, facesize*mod->nummodelsurfaces);
		threadfuncs->AddWork(WG_MAIN, VBSP_GenerateMaterials, mod, bd, 0, 0);
	}
#endif

	for (i=1 ; i< mod->numsubmodels ; i++)
	{
		cmodel_t	*bm;

		char	name[MAX_QPATH];

		Q_snprintfz (name, sizeof(name), "*%i:%s", i, wmod->publicname);
		mod = modfuncs->BeginSubmodelLoad(name);
		*mod = *wmod;
		mod->archive = NULL;
		mod->entities_raw = NULL;
		mod->submodelof = wmod;
		Q_strncpyz(mod->publicname, name, sizeof(mod->publicname));
		Q_snprintfz (mod->name, sizeof(mod->name), "*%i:%s", i, wmod->name);
		memset(&mod->memgroup, 0, sizeof(mod->memgroup));

		bm = VBSP_InlineModel (wmod, name);

		mod->hulls[0].firstclipnode = -1;	//no nodes,
		if (bm->headleaf)
		{
			mod->leafs = bm->headleaf;
			mod->nodes = NULL;
			mod->hulls[0].firstclipnode = -1;	//make it refer directly to the first leaf, for things that still use numbers.
			mod->rootnode = (mnode_t*)bm->headleaf;
		}
		else
		{
			mod->leafs = wmod->leafs;
			mod->nodes = wmod->nodes;
			mod->hulls[0].firstclipnode = bm->headnode - mod->nodes;	//determine the correct node index
			mod->rootnode = bm->headnode;
		}
		mod->nummodelsurfaces = bm->numsurfaces;
		mod->firstmodelsurface = bm->firstsurface;

		VBSP_BuildBIHSubmodel(mod, i);

		memset(&mod->batches, 0, sizeof(mod->batches));
		mod->vbos = NULL;

		VectorCopy (bm->maxs, mod->maxs);
		VectorCopy (bm->mins, mod->mins);
#ifdef HAVE_CLIENT
		mod->radius = RadiusFromBounds (mod->mins, mod->maxs);

		if (qrenderer != QR_NONE)
		{
			builddata_t *bd = plugfuncs->Malloc(sizeof(*bd) + facesize*mod->nummodelsurfaces);
			bd->buildfunc = VBSP_BuildSurfMesh;
			bd->paintlightmaps = true;
			memcpy(bd+1, facedata + mod->firstmodelsurface*facesize, facesize*mod->nummodelsurfaces);
			threadfuncs->AddWork(WG_MAIN, VBSP_GenerateMaterials, mod, bd, i, 0);
		}
#endif
		modfuncs->EndSubmodelLoad(mod, MLS_LOADED);
	}

	//urgh, we need to wait for models to load in order to get their sizes. that requires being on the main thread and the caller will think we're loaded on completion so we can't safely pingpong it back before generating the bih tree
	threadfuncs->AddWork(WG_MAIN, VBSP_BuildBIHMain, wmod, NULL, 0, 0);

	//main thread should have a load of work to do now. worker thread should now be free to compute the hash before its finally marked as loaded and the temp file memory goes away.
	/*
	  ftesurf Patch 321: WMOD, NOT MOD.  `mod` is the loop variable -- the submodel
	  loop above reassigns it on every iteration (:11216) -- so on any map with brush
	  entities this wrote the map's hash onto the LAST submodel, "*1398:surf_x", and
	  the world model kept the zero Mod_FindName's memset left it.

	  Nothing read a submodel's checksum, and everything that matters reads the world
	  model's: sv_mapcheck compares sv.world.worldmodel->checksum against what the
	  client sends, and the client sends cl.worldmodel->checksum2.  Both were zero, on
	  both machines, so the comparison passed for every client and every map -- the
	  check did not fail, it was INERT, which is why five years of servers never
	  noticed.  sv_mapcheck defaults 1, so it looked switched on the whole time.

	  Measured before fixing: of the 1312 maps in the library, 1287 have more than one
	  submodel and took the broken path; the 25 with exactly one never entered the loop
	  and had been hashing correctly all along.  That is also why it cannot be caught by
	  spot-checking -- pick df_map and it works.
	*/
	VBSP_ComputeChecksum(wmod, filein, filelen);
	return true;
}


/*
FTESurf Patch 231: the whole unresolved list, on demand.

The warning prints 24 and counts the rest, which is right for a warning and useless
for an audit.  surf_demise is the case that needed this: the HUD says 41 unresolved,
tools/mapdeps.py --map surf_demise resolves everything except one bottom material,
and settling which of those is true requires the other forty to be NAMED.  A capped
line cannot do it, and neither can a count.

Prints the P181 "found but would not parse" list too, because the pair of them is the
whole picture and the second one is otherwise developer-1 only.
*/
/*
FTESurf Patch 257 -- `prop_census`, the diagnostic that did not exist.

`ent_census` reports entities with no spawn function and `hl2_missing` reports
materials that would not resolve.  A static prop that is solid and not drawn --
which is an invisible wall you have to surf around -- fell between them, and
finding one meant flipping r_novis / hl2_propdist / hl2_propcollision by hand
and watching the screen.

Bare: what the last frame that drew props did with all of them.
With an argument: one prop, by lump index or by a substring of its model name,
with the verdict AND the facts that decide it -- which is the form you need,
because "241 of 429 drawn" cannot tell you about the one you are standing in
front of.
*/
static void VBSP_PropCensus_f(void)
{
	model_t *mod = vbsp_propstat.mod;
	vbspinfo_t *prv;
	char arg[256];
	unsigned int v, i, shown = 0;
	qboolean byindex;
	size_t alen;

	if (!mod || !(prv = (vbspinfo_t*)mod->meshinfo) || !prv->numstaticprops)
	{
		Con_Printf("prop_census: no static props have been drawn yet -- load a map and render a frame first.\n");
		return;
	}

	cmdfuncs->Argv(1, arg, sizeof(arg));
	if (!*arg)
	{
		Con_Printf("%s: %u static props, last frame --\n", mod->name, vbsp_propstat.total);
		for (v = 0; v < PROPV_MAX; v++)
			if (vbsp_propstat.n[v])
				Con_Printf("  %6u  %s\n", vbsp_propstat.n[v], vbsp_propverdict[v]);
		if (vbsp_propstat.leafoverflow)
			Con_Printf("  %6u prop(s) had a lump leaf list longer than MAX_ENT_LEAFS and were re-derived\n"
					   "         from a bounding box at first draw; %u of those ended up on the weaker\n"
					   "         headnode visibility test rather than a cluster list.\n",
					   vbsp_propstat.leafoverflow, vbsp_propstat.headnodetested);
		//FTESurf Patch 259.  Of the props that DREW, how many drew from VRAD's
		//per-vertex bake.  This is the line that says the feature is on: the
		//load-time count says only that the files were read.
		if (vbsp_propstat.vcdrawn || vbsp_propstat.vcdeclined)
		{
			Con_Printf("  %6u of the drawn props used VRAD's per-vertex bake\n", vbsp_propstat.vcdrawn);
			if (vbsp_propstat.vcdeclined)
				Con_Printf("  %6u had one loaded and declined it -- hl2_lt_baked_vc is 0, or\n"
						   "         the model has no scatter table so the colours cannot be placed.\n",
						   vbsp_propstat.vcdeclined);
		}
		/*
		FTESurf Patch 262: solidity, which this never reported and which is the
		other half of "something is stopping me".  The per-verdict list above is
		about DRAWING; a prop can be perfectly visible and still be the wrong
		shape to run into, or -- since this patch -- no shape at all.
		*/
		{
			unsigned int sol = 0, sbox = 0, sother = 0;
			for (v = 0; v < prv->numstaticprops; v++)
			{
				int s = prv->staticprops[v].solid;
				if (!s)
					continue;
				sol++;
				if (s == 2)
					sbox++;
				else if (s != 6)
					sother++;
			}
			Con_Printf("  %6u ask to be solid (%u SOLID_VPHYSICS, %u SOLID_BBOX, %u other)\n",
				sol, sol-sbox-sother, sbox, sother);
			if (vbsp_prop_nocollide)
				Con_Printf("  %6u of those collide against nothing -- their model ships no .phy.\n"
						   "         `hl2_propcollision_nophy 1` gives them the visible mesh back.\n",
						   vbsp_prop_nocollide);
		}
		Con_Printf("`prop_census <index>` or `prop_census <part of a model name>` for one prop.\n");
		return;
	}

	alen = strlen(arg);
	byindex = (arg[0] >= '0' && arg[0] <= '9');
	for (i = 0; i < prv->numstaticprops; i++)
	{
		struct staticprop_s *sent = &prv->staticprops[i];
		entity_t *src = &sent->ent;
		const char *nm = src->model ? src->model->name : "(no model)";
		qboolean hit;

		if (byindex)
			hit = (i == (unsigned int)atoi(arg));
		else
		{
			const char *s;
			hit = false;
			for (s = nm; *s; s++)
				if (!Q_strncasecmp(s, arg, alen)) { hit = true; break; }
		}
		if (!hit)
			continue;

		shown++;
		Con_Printf("prop %u: %s\n", i, nm);
		Con_Printf("    verdict last frame : %s%s%s\n",
			(sent->lastverdict < PROPV_MAX) ? vbsp_propverdict[sent->lastverdict] : "?",
			(sent->lastverdict == PROPV_PVS) ? " -- rejected by the " : "",
			(sent->lastverdict == PROPV_PVS && sent->lastpvswhy < VBSP_PVSWHY_MAX)
				? vbsp_pvswhy_name[sent->lastpvswhy] : "");
		Con_Printf("    origin             : %.1f %.1f %.1f\n",
			src->origin[0], src->origin[1], src->origin[2]);
		//FTESurf Patch 262: the RAW m_Solid, not a boolean.  0 and 6 are the map's
		//own answer to "should this stop the player", and printing them as the same
		//"yes" hid the fact that 1..6 were all being treated as SOLID_VPHYSICS.
		Con_Printf("    m_Solid            : %i (%s)%s\n", (int)sent->solid,
			(sent->solid == 0) ? "SOLID_NONE" :
			(sent->solid == 2) ? "SOLID_BBOX" :
			(sent->solid == 6) ? "SOLID_VPHYSICS" : "other -- approximated",
			sent->nocollide ? "  <-- NOT SOLID: the model ships no .phy" : "");
		Con_Printf("    fade               : min %g max %g%s\n",
			sent->fademindist, sent->fademaxdist,
			sent->fademaxdist ? "" : " (none; hl2_propdist applies)");
		Con_Printf("    area               : areanum %i, areanum2 %i, headnode %i%s\n",
			src->pvscache.areanum, src->pvscache.areanum2, src->pvscache.headnode,
			(sent->nareas > 2) ? "  <-- box spans more areas than the 2 slots hold" : "");
		if (sent->nareas)
			Con_Printf("    areas box spanned  : %u\n", sent->nareas);
		Con_Printf("    pvs leaf state     : num_leafs %i%s\n", src->pvscache.num_leafs,
			(src->pvscache.num_leafs == -1) ? " (headnode test -- leaf list overflowed)" :
			(src->pvscache.num_leafs == -2) ? " (not yet re-derived; prop has never been reached)" :
			(src->pvscache.num_leafs ==  0) ? " (NO VISIBLE LEAVES -- this prop can never pass the PVS test)" : "");
		Con_Printf("    lighting           : %s, sampled at %.1f %.1f %.1f\n",
			sent->hasbaked ? "VRAD baked (.vhv)" : "leaf ambient cube",
			sent->lightorg[0], sent->lightorg[1], sent->lightorg[2]);
		//FTESurf Patch 296: the LEVEL, which nothing printed until now.  The old
		//line said only which SOURCE the prop was lit from, so "these two pines
		//are different brightnesses" could not be checked except by eye against a
		//screenshot.  mean is what the prop draws at its average vertex (baked
		//divided by the peak gain); peak is the stored sent->baked, which is what
		//shader_here's `light avg` prints and is gain times larger.
		if (sent->hasbaked)
		{
			float g = sent->vcgain;
			if (g <= 0) g = 1;
			Con_Printf("    baked level        : mean %.1f %.1f %.1f   peak %.1f %.1f %.1f (gain %.2f)\n",
				sent->baked[0]/g, sent->baked[1]/g, sent->baked[2]/g,
				sent->baked[0], sent->baked[1], sent->baked[2], g);
		}
		/*
		FTESurf Patch 304: the SOLVED pair, for a prop with no bake -- which until
		now printed which source it was lit from and not one number from it.

		This is the instrument the fold change is judged with, and it exists
		because the alternative is a screenshot.  A screenshot can only show the
		faces pointing at the camera, and that is the population where this
		error is smallest by a factor of ten: on tensor2's stage2_detail02 the
		camera-facing faces ran 1.3x hot while the face turned away ran 13.6x.
		Reading base and amp directly tests BOTH ends of the ramp at once,
		because the unlit end IS base and the lit end IS base+amp -- no vantage
		can hide half of it.

		Printed in the 0-255 screen units VBSP_LightPointValues produced;
		HL2_CalcModelLighting stores them scaled by 1/255, so this undoes that.
		The direction is the entity's, i.e. already projected onto its axes.
		*/
		else
			Con_Printf("    solved level       : base %.1f %.1f %.1f  directional %.1f %.1f %.1f\n"
					   "                         lit end %.1f %.1f %.1f  dir %.2f %.2f %.2f  (fold %i)%s\n",
				src->light_avg[0]*255, src->light_avg[1]*255, src->light_avg[2]*255,
				src->light_range[0]*255, src->light_range[1]*255, src->light_range[2]*255,
				(src->light_avg[0]+src->light_range[0])*255,
				(src->light_avg[1]+src->light_range[1])*255,
				(src->light_avg[2]+src->light_range[2])*255,
				src->light_dir[0], src->light_dir[1], src->light_dir[2],
				(hl2_lt_fold && hl2_lt_fold->ival) ? 1 : 0,
				src->light_known ? "" : "  <-- STALE: this prop has not been solved since the last invalidation");
		//FTESurf Patch 259.  Says whether the per-vertex array is there AND whether
		//it is being drawn, because hl2_lt_baked_vc can turn the second off with the
		//first still loaded, and "mine looks like mode 1" is exactly that case.
		if (sent->vc || sent->vcraw)
			Con_Printf("    per-vertex bake    : %u verts (%.1f KiB), peak gain %.2f -- %s\n",
				sent->vc ? sent->vcverts : sent->vcrawverts,
				(sent->vc ? sent->vcverts : sent->vcrawverts)*4/1024.0, sent->vcgain,
				src->vertlightbytes ? "APPLIED" :
				sent->vcraw ? "read, not yet scattered (prop has never been drawn)" :
				!vbsp_vc_live ? "loaded but off (hl2_lt_baked_vc 0)" :
				"REJECTED -- this model has no scatter table, so the colours cannot"
					" be placed; drawn from the mean instead");
		else if (sent->hasbaked)
			Con_Printf("    per-vertex bake    : not kept (hl2_lt_baked is %i, needs 2)\n",
				hl2_lt_baked ? hl2_lt_baked->ival : 0);
		if (src->model)
			Con_Printf("    model              : radius %g, mins %.0f %.0f %.0f, maxs %.0f %.0f %.0f\n",
				src->model->radius,
				src->model->mins[0], src->model->mins[1], src->model->mins[2],
				src->model->maxs[0], src->model->maxs[1], src->model->maxs[2]);
	}
	if (!shown)
		Con_Printf("prop_census: no prop matched \"%s\".\n", arg);
}

static void VBSP_Missing_f(void)
{
	int i, n;

	if (!vmt_stat_missing && !vmt_stat_unparsed)
	{
		Con_Printf("hl2_missing: every material on the loaded map resolved.\n");
		return;
	}

	if (vmt_stat_missing)
	{
		n = (vmt_stat_missing > VMT_MISSING_LIST) ? VMT_MISSING_LIST : vmt_stat_missing;
		Con_Printf("%i material(s) did not resolve (%i of them by the end of the map load;\n"
			"the rest failed afterwards -- streamed props, the HUD, and the previous map's\n"
			"cached models being re-registered, so names from a map you have left can appear):\n",
			vmt_stat_missing, vmt_census_atload);
		//FTESurf Patch 233: the reason column was worked out at load time, while the world
		//model was in hand.  Names that arrived AFTER it have no measurement behind them and
		//must not be given one -- a prop skin is still identifiable by its path, and anything
		//else gets nothing rather than a guess.
		for (i = 0; i < n; i++)
			Con_Printf("    %-42s -- %s\n", vmt_stat_missing_name[i],
				(i < vmt_miss_reasons) ? vmt_miss_reason[i] :
				!Q_strncasecmp(vmt_stat_missing_name[i], "models/", 7) ? "a prop skin, loaded after the map" :
				"loaded after the map; not measured");
		if (vmt_stat_missing > n)
			Con_Printf("    ...and %i more, beyond the %i this build records.\n",
				vmt_stat_missing - n, VMT_MISSING_LIST);
		//FTESurf Patch 233: the number the banner uses, and why it may be smaller.
		Con_Printf("%i of the load-time %i are on something you can look at (hl2_unresolved_visible);\n"
			"the rest are named by the map and drawn by nothing.\n",
			vmt_miss_visible, vmt_census_atload);
	}
	if (vmt_stat_unparsed)
	{
		n = (vmt_stat_unparsed > VMT_MISSING_LIST) ? VMT_MISSING_LIST : vmt_stat_unparsed;
		Con_Printf("%i material(s) were found but did not parse (drawn with defaults):\n",
			vmt_stat_unparsed);
		for (i = 0; i < n; i++)
			Con_Printf("    %s\n", vmt_stat_unparsed_name[i]);
		if (vmt_stat_unparsed > n)
			Con_Printf("    ...and %i more, beyond the %i this build records.\n",
				vmt_stat_unparsed - n, VMT_MISSING_LIST);
	}
}

qboolean VBSP_Init(void)
{
	filefuncs = plugfuncs->GetEngineInterface(plugfsfuncs_name, sizeof(*filefuncs));
	modfuncs = plugfuncs->GetEngineInterface(plugmodfuncs_name, sizeof(*modfuncs));
	if (modfuncs && modfuncs->version != MODPLUGFUNCS_VERSION)
		modfuncs = NULL;
	threadfuncs = plugfuncs->GetEngineInterface(plugthreadfuncs_name, sizeof(*threadfuncs));

	//nettest: a dedicated/headless server has no client Image/renderer interface (the same condition the
	//"hl2: VTF/VMT/TTH support unavailable" banners report).  Mark qrenderer QR_NONE so the renderer-only
	//material + lightmap passes are skipped on load — they call modfuncs->RegisterBasicShader / Batches_Build,
	//which a SERVERONLY engine leaves NULL (engine/common/plugin.c) => call-through-NULL crash on any VBSP map.
	//(Mirrors VTF_Init's interface probe in img_vtf.c; non-NULL on the client so it keeps QR_OPENGL there.)
	if (!plugfuncs->GetEngineInterface(plugimagefuncs_name, sizeof(plugimagefuncs_t)))
		qrenderer = QR_NONE;

	if (modfuncs && filefuncs && threadfuncs)
	{
		modfuncs->RegisterModelFormatMagic("Source (V)BSP", "VBSP",4, VBSP_LoadMap);

#define MAPOPTIONS "Map Cvar Options"

		/*
		FTESurf Build 11: CVAR_MAPLATCH, not CVAR_RENDERERLATCH.

		Every cvar below is read exactly once, while a map loads.  They were
		registered CVAR_RENDERERLATCH, and the comments beside them said "so it
		needs a map reload" -- but that is not what that flag means.  Only
		renderer.c ever calls Cvar_ApplyLatches(CVAR_RENDERERLATCH), so the new
		value sat in latched_string until a vid_reload and `hl2_water 0` followed
		by `map <the same map>` genuinely did nothing.  The engine says so, in a
		line easily lost in a map load's output:

		    variable hl2_water will be changed after a vid_reload

		CVAR_MAPLATCH is applied by SV_SpawnServer immediately before the world
		model is loaded (sv_init.c:968) and by the client on connect
		(cl_main.c:2309), which is exactly when these are read.  So set, reload
		the map, and it applies -- which is what the descriptions promise.

		Only one latch flag is allowed per cvar (CVAR_LATCHMASK, cvar.h:148), so
		this is a swap rather than an addition.
		*/
		/*
		FTESurf Patch 223.  This is the engine's OWN r_novis -- GetNVFDG hands
		back the existing cvar rather than minting a second one, exactly as the
		r_texdiag line further down says of itself.  It was registered with the
		description belonging to hl2_displacement_scale on the line below,
		copy-pasted, so `cvarlist r_novis` described the wrong cvar entirely.
		*/
		hl2_novis = cvarfuncs->GetNVFDG("r_novis", "0", 0, "Draw every leaf, ignoring the PVS. Diagnostic: makes a vis problem visible by removing vis.", MAPOPTIONS);
		hl2_pvscache = cvarfuncs->GetNVFDG("hl2_pvscache", "1", 0, "FTESurf (P273): reuse the answer to the whole-tree entity-visibility descent when the same node is asked about the same PVS. Exact, not approximate -- the test is a pure function of those two. Measured on surf_tensor2 it was 98% of the listen server's per-frame cost. 0 restores the old recompute-every-time behaviour, so a suspected culling bug is a one-command A/B.", MAPOPTIONS);
		hl2_lumpretire = cvarfuncs->GetNVFDG("hl2_lumpretire", "1", CVAR_MAPLATCH, "FTESurf (P298): skip LZMA-decompressing the BSP lumps nothing in this configuration will read -- both lightmaps on a dedicated server, and the unused half of the LDR/HDR pair when drawing. Map load on this engine is single-threaded LZMA and almost nothing else, so this is most of it: 334 MB of the 350 MB surf_affliction expands is lightmap. 0 restores the old decompress-everything behaviour, which makes the whole patch a one-command A/B.", MAPOPTIONS);
		/*
		FTESurf Patch 223.  Was CVAR_RENDERERLATCH|CVAR_CHEAT -- two latch bits
		at once, which is the very bug the essay below diagnoses and fixes for
		hl2_favour_ldr, left standing on the line above it.  cvar.h:148 puts
		BOTH inside CVAR_LATCHMASK ("you're only allowed one of these"), so this
		was never merely untidy.

		Resolved to CVAR_CHEAT rather than to CVAR_MAPLATCH, which is the other
		way it could have gone.  The read is at BSP load (:1926), so MAPLATCH is
		what the *reload* semantics want -- but this cvar multiplies how far
		displacement geometry moves, in a game that records timed runs, and the
		cheat bit is the property somebody deliberately asked for here.  Keeping
		it costs a developer one `map` command; dropping it would quietly retire
		a protection.  It is console-only -- not a gfx_menu row -- so nothing
		user-facing depended on the latch behaviour.
		*/
		hl2_displacement_scale = cvarfuncs->GetNVFDG("hl2_displacement_scale", "1", CVAR_CHEAT, "Multiplier for how far displacements can move.", MAPOPTIONS);
		/*
		FTESurf build 11.  Was CVAR_RENDERERLATCH|CVAR_CHEAT -- two latch bits at
		once, which cvar.h:148 says is not allowed ("you're only allowed one of
		these") and which resolved to RENDERERLATCH, so it needed a vid_reload.
		It selects which lighting lump is READ, so a map reload is the moment
		that matters and MAPLATCH is the flag that means it.
		*/
		hl2_favour_ldr = cvarfuncs->GetNVFDG("hl2_favour_ldr", "0", CVAR_MAPLATCH, "Which of a Source map's two lightmaps to load, when it ships both.\n0: HDR (default). Source pairs its HDR lightmaps with a tonemap and an autoexposure pass that we do not have, so a brightly-lit surface can come out washed out.\n1: LDR -- the same map lit for a fixed exposure, which is what a Source engine with HDR off would show.", MAPOPTIONS);
		/*
		FTESurf Patch 268 D1.  Default 1 because it is a correctness fix and nothing else:
		the old walk read a bumped face's basis maps as if they were the next lightstyle's
		flat map, and the per-map size proof in VBSP_ProbeLightmapStride refuses to tag any
		map whose layout it cannot equate to its lighting lump, on which the walk stays
		exactly as it was.  0 is the falsifier for an A/B, and the per-map print names
		which walk was used either way.  MAPLATCH: the tags are written while the BSP is
		read, so a map reload is when a change takes effect.
		*/
		hl2_bumpstride = cvarfuncs->GetNVFDG("hl2_bumpstride", "1", CVAR_MAPLATCH, "Walk a Source map's lightstyles at the stride VRAD wrote them: a $bumpmap face stores four luxel sets per style (flat plus three radiosity-normal-map basis maps), and 150 library maps add a non-lightmap record after every style's sets.\n0: the old walk, which summed a basis map in as the next lightstyle at full brightness and never read the real one, on ~67,000 faces across 153 library maps.\n1: probe the layout per map and skip the extra sets (default). Faces then show their real second style -- brighter where it is a bright switchable light, darker where it is dim. Maps whose layout cannot be proven from the lighting lump size fall back to 0 and say so in the console.", MAPOPTIONS);
		/*
		FTESurf: hide the brushes Source hides.

		Clip, playerclip, nodraw, skip, hint and areaportal faces were all being
		drawn -- lit, batched and rasterised every frame purely to be looked
		through.  Three separate things mark such a face and none of them were
		acted on: the compiler's own SURF_NODRAW in the texinfo lump, the
		%compile* keys in a tool VMT, and a material with $alpha 0.

		A cvar rather than a hard-coded behaviour because it is the only way to
		see a clip brush when you need to (working out WHY a route blocks), and
		because it makes the change A/B-able against a map that looks wrong.
		MAPLATCH: all three are decided at load, so it needs a map reload.
		*/
		hl2_hidetools = cvarfuncs->GetNVFDG("hl2_hidetools", "1", CVAR_MAPLATCH, "Hide clip/nodraw/skip/hint tool brushes, as Source does. 0 draws them, which is occasionally useful for working out why a route is blocked.", MAPOPTIONS);
		/*
		FTESurf Patch 281: see VBSP_RepackLighting.  1 is the fix; 0 is the
		pre-281 read, kept as the A/B arm so "bhop_24 draws black" can be
		reproduced on demand.  MAPLATCH because the repack happens inside the
		map load; a pure correctness fix, so it defaults ON.
		*/
		hl2_lightmap_repack = cvarfuncs->GetNVFDG("hl2_lightmap_repack", "1", CVAR_MAPLATCH, "FTESurf (P281): when a Source map's face lightmaps do not all start on a 4-byte boundary (nine Momentum maps store an undocumented extra byte per luxel), rebuild the lightmap one face at a time so every face's samples are aligned. 0 reads the lump the old way, which draws about 72% of those maps' faces black.", MAPOPTIONS);
		/*
		This one is CONSOLE-ONLY, and not for want of a menu row -- fs_overlays is
		the row (gfx_menu page 3), because it is the only one of the two that a
		menu CAN drive.

		The synthesis happens inside the BSP loader, and the menu's reload key is
		`retry` -> `map_restart`, which respawns the server and re-initialises CSQC
		but does NOT re-run the loader: Mod_ForName hands back the cached world
		model unless the .bsp's mtime moved (sv_init.c, the Mod_PurgeModel guard).
		So this cvar cannot change anything on a restart, only on a genuine load of
		a different map. Measured: with developer 1 the "baked overlays synthesised"
		line below prints exactly once per real load, however many restarts follow.

		fs_overlays keeps the synthesis and skips the PLACING, which is read on
		CSQC's per-map entity walk and therefore does respond to a restart. That is
		the switch a user wants; this one is for proving what the entity string
		looks like without it.
		*/
		hl2_overlays = cvarfuncs->GetNVFDG("hl2_overlays", "1", CVAR_MAPLATCH, "Read a Source map's baked decals (LUMP_OVERLAYS, lump 45) and synthesise them onto the entity string as info_overlay, for the CSQC decal layer to place. 230 of the 1310 shipped maps carry one -- 9702 decals that have never been drawn at all, 105 of them on surf_rise. 0 leaves the entity string exactly as the map wrote it.\nTakes effect only on a genuine map LOAD, not on a map_restart/retry: the world model is cached, so the loader does not re-run. Use fs_overlays to turn overlays off without a reload.", MAPOPTIONS);

		/*
		FTESurf Build 7 asked the question; Build 8 answers it.

		Static props used to collide against their VISIBLE MESH -- VBSP_BuildBIH
		puts a BIH_MODEL leaf per solid prop straight off the render model, and
		mod_hl2.c built that model's trace tree from the render triangles.  Source
		does not: every solid model ships a sibling .phy holding a VPhysics hull,
		and props_c17/fence01a is 12 collision triangles against a render mesh
		with every rail, post and wire in it.  Colliding against the render mesh
		made us STRICTER than Source wherever a prop has foliage, a railing, a
		grate or a skirt -- the exact shape of "an invisible block stopped me".

		mod_phy.c reads the hull now, so mode 1 is the real thing.  The other
		three modes exist so the question stays falsifiable in one map load:

		    0  props do not collide at all
		    1  the .phy VPhysics hull, falling back to the render mesh when a
		       model ships none or ships a jointed one            (DEFAULT)
		    2  the model's bounding box -- what Source's "solid" 2 asks for
		    3  the render mesh, i.e. exactly what build 7 shipped

		Modes 1/2/3 are decided in the MODEL loader (Mod_PHY_CollisionMesh), so
		they apply to every Source model rather than only to static props; mode 0
		is this file's, and drops the prop leaves entirely.

		MAPLATCH because both the model trees and the world BIH are built once at
		load -- changing it needs a map reload to mean anything.
		*/
		/*
		FTESurf Patch 193.  NOT latched: it is read per prop per frame in
		VBSP_PrepareFrame, so it applies the instant it is set and can be dialled
		against the framerate without a map reload.  See the essay at that read.
		*/
		/*
		FTESurf Patch 195.  CVAR_SHADERSYSTEM, because it changes which shader a
		material is generated with -- so a change re-reads the VMTs and rebuilds
		them, which is exactly what has to happen and what hl2_translucent beside
		it already relies on.
		*/
		hl2_animated = cvarfuncs->GetNVFDG("hl2_animated", "0", CVAR_SHADERSYSTEM, "Source's AnimatedTexture proxy -- materials whose frames all live in one VTF (smoke, fire, screens, water).\n0: draw frame 0, which is what shipped before and what every one of these materials has always done (default).\n1: cycle them at the rate the material asks for. 3,342 materials across 741 maps carry the proxy, so this moves a large part of the library onto a separate GLSL 130 shader (vmt/animated) that nothing has exercised before -- which is why it is not on by default.", MAPOPTIONS);

		hl2_propdist = cvarfuncs->GetNVFDG("hl2_propdist", "0", CVAR_ARCHIVE, "Maximum draw distance, in units, for static props that declare NO fade distance of their own -- which on some maps is all of them (surf_demise: 2350 of 2350, median distance from the start 24,185 units, and drawing them costs 4.11M of the frame's 5.2M draw indices).\n0: no limit -- draw every prop at any distance (default, and what shipped before).\n>0: skip props further away than this. Source applies its own r_propsmaxdist in the same situation, so a limit here is closer to the map author's intent than none; the trade is that distant scenery disappears, which on a wide-open map is part of the view. Props that set their own fade distance are unaffected either way.", MAPOPTIONS);

		hl2_rigidprops = cvarfuncs->GetNVFDG("hl2_rigidprops", "1", CVAR_MAPLATCH, "Load models compiled $staticprop as rigid geometry instead of as one-bone skeletal models.\n0: what shipped before -- every Source model carries bone weights, so com_mesh.c's rigid static-VBO path is unreachable and any prop surface whose material is not VertexLitGeneric is re-skinned on the CPU and re-uploaded every frame.\n1: emit no skinning data for a model studiomdl has already flattened (default). Measured: 73 of 73 prop models on surf_demise, 54 of 54 on surf_garden and 220 of 238 on ahop_coast are one-bone $staticprop with no animations, so the skinning they pay for is an identity transform. Models with a real rig -- ragdolls, NPCs, viewmodels -- are unaffected either way.", MAPOPTIONS);

		hl2_propcollision = cvarfuncs->GetNVFDG("hl2_propcollision", "1", CVAR_MAPLATCH, "How props collide.\n0: props are non-solid -- use this to find out whether a prop is what is blocking a route.\n1: against their .phy VPhysics hull, as Source does, falling back to the visible mesh when there is no usable hull (default).\n2: against the model's bounding box (Source's \"solid\" 2).\n3: against the visible mesh -- stricter than Source, and what shipped before build 8.", MAPOPTIONS);

		hl2_phywinding = cvarfuncs->GetNVFDG("hl2_phywinding", "1", CVAR_MAPLATCH, "Whether a .phy VPhysics hull's COLLISION triangles are re-wound to face outward.\nA BIH triangle is a one-sided prism with four units of solid behind its face plane, and that plane's direction comes from the winding: com_bih.c takes cross(p1-p2, p3-p2), so it needs CLOCKWISE-from-outside. IVP's compactledge triangles are ANTICLOCKWISE from outside, and mod_phy.c's (x, z, -y) conversion is a determinant +1 rotation that preserves that -- so every .phy hull in the game has been inside out since Build 8, with its four units of solid stacked ON TOP of the visible surface. Measured over 166 .phy files: 25181 of 25182 faces anticlockwise, 0 clockwise. The effect is that you stand 4/|n_z| units above the model (4 flat, 6.81 on the 54-degree surf_boreas ramp) with a perfectly standable-looking normal, and the model's interior is hollow. Where such a hull meets correctly-wound brush or displacement geometry, that step is the seam snag.\nThis is Patch 256's displacement defect on the other geometry path -- and the path Patch 256's own text named as also affected, then did not touch.\n0: leave the file's winding alone (every build from 8 to 316).\n1: re-wind for collision (default). The render mesh is untouched either way; only NativeTrace changes.\n2: measure each ledge and let it vote. Robust to a file that disagrees with the census. The vote is per LEDGE, never per triangle: two adjacent coplanar triangles with slabs on opposite sides would be a real 4-8 unit step in the middle of a flat face.\nhl2_propcollision 2 (bounding box) and 3 (render mesh) are NOT affected -- both of those windings were checked and are already correct.", MAPOPTIONS);

		hl2_brushbevels = cvarfuncs->GetNVFDG("hl2_brushbevels", "1", CVAR_MAPLATCH, "Whether the missing EDGE BEVELS are generated for world brushes at load.\nSweeping a bounding box against a convex brush is a point query against the Minkowski sum of the two, whose faces are the brush's own planes, the six box planes, and one plane per pairing of a brush edge with a box axis. BIH_ClipBoxToBrush has the first two groups and has never had the third, so the volume it sweeps is a strict SUPERSET of the truth, bulging outward along every slanted edge -- the same defect, on the same argument, that Patch 258 fixed for triangles.\nQuake 2 and Source both solve this at COMPILE time (VBSP's AddBrushBevels), so the runtime is only ever as correct as the compile was -- and maps ship without them. surf_boreas brush 170, the PLAYERCLIP wedge at the foot of the lock-11 ramp, is one: twelve sides, no edge bevels, a 51-degree top face meeting 45-degree walls. Riding the ramp there the clipper calls the player 0.107 units inside it and stops them dead on a normal with z of zero -- a wall, not a surface you slide along -- while an exact separating-axis test over the brush's 38 hull vertices puts them 10.5 units clear. The phantom lasts about 24 units of travel.\n0: leave brushes exactly as the map compiled them (every build before this one).\n1: generate the missing planes at load, using VBSP's own algorithm (default). A candidate is kept only if EVERY vertex of the hull is behind it, so it can only shrink the swept volume toward the true sum, never below it -- it cannot open a hole. With them the clipper agrees with the exact test to within 0.004 units.\nThis changes world collision, so times set before it are not comparable on maps where the count below is non-zero.", MAPOPTIONS);

		/*
		FTESurf Patch 262.  Mode 1's "falling back to the visible mesh when there is
		no usable hull" covered two different situations, and only one of them was a
		fallback.

		A model that ships a .phy we cannot read -- jointed, or a hull whose solids
		are bone-relative and escape the model bounds -- does collide in Source, and
		the render mesh is the honest approximation.  A model that ships no .phy at
		all does NOT collide in Source: no $collisionmodel means no vcollide means no
		entry in the collision list.  Substituting the render mesh there is not a
		coarser hull, it is a hull where Source has none, made of exactly the geometry
		that should let you through.

		Census over the 1310-map library: of 110,959 props with a non-zero m_Solid,
		34,514 (31.1%) across 380 maps ask for a hull and ship no .phy.  On
		surf_garden 383 of 420 -- ivory shrubs, cypresses, azaleas.  surf_demise
		1525, surf_nuclear 2657, surf_drift 1926.  surf_boreas, surf_nyx and
		bhop_canals have none and must not move.

		Those figures are the SECOND census.  The first said 60,650 across 604 maps
		and put bhop_canals at 2065 of 2065; the engine's own load-time count said 0,
		because the census had probed only the map's pakfile and the Momentum tree
		while canals is built out of props_wasteland and props_c17, whose .phy files
		live in the CS:S and HL2 VPKs.  Re-probed across every mounted pack the two
		routes agree per map exactly, which is worth having because they share
		nothing: one parses the sprp lump and walks VPK directories, the other loads
		the models.

		Kept as a cvar and not just as a fix because it changes collision on 380 maps,
		and collision parity with Momentum is the whole point of a timing game: if a
		surface is solid here and not there, a run that uses it is not comparable.
		This is the switch that says which of the two you are running.

		MAPLATCH, and it decides what the MODEL loader builds, so a model already
		cached from an earlier map keeps the tree it was built with -- the same
		caveat hl2_propcollision has always carried.
		*/
		hl2_propcollision_nophy = cvarfuncs->GetNVFDG("hl2_propcollision_nophy", "0", CVAR_MAPLATCH, "What to do with a static prop that asks for a collision hull and whose model ships no .phy -- 31.1% of the library's solid props, and 383 of surf_garden's 420.\n0: nothing collides against it, which is what Source does with a model that has no collision data (default).\n1: collide against the visible mesh, which is what shipped before Patch 262 -- every shrub, fence and railing solid at the shape of its own triangles.\nOnly applies to hl2_propcollision 1, and never to props asking for a bounding box (m_Solid 2/3/4), which Source builds from the model bounds with no .phy at all. A .phy we merely cannot read still falls back to the visible mesh either way.", MAPOPTIONS);

		/*
		The other half of the same question, and on current evidence the more
		likely one.  Displacements go into the collision tree as ONE BIH LEAF PER
		TRIANGLE (VBSP_BuildBIH), and surf_666 -- the map that gets you stuck --
		has 100 displacements and 11,076 displacement vertices, while surf_null
		and surf_utopia, which do not get you stuck, have exactly zero.

		Turning this off makes displacement surfaces non-solid, so you WILL fall
		through terrain and through any ramp built as a displacement.  It is a
		diagnostic, not a play setting: if a spot stops trapping you with this at
		0, the displacement collision is what is trapping you.
		*/
		hl2_dispcollision = cvarfuncs->GetNVFDG("hl2_dispcollision", "1", CVAR_MAPLATCH, "Whether displacement surfaces collide.\n0: displacements are non-solid -- you will fall through terrain, but it identifies a displacement as the thing trapping you.\n1: normal (default).", MAPOPTIONS);
		hl2_dispwinding = cvarfuncs->GetNVFDG("hl2_dispwinding", "1", CVAR_MAPLATCH, "Whether displacement COLLISION triangles are re-wound to face outward.\nA BIH triangle is a one-sided prism with 4 units of solid behind its face plane (com_bih.c:305-309), and the plane's direction comes from the triangle's winding.  On a dface_t with side set (SURF_PLANEBACK) a displacement's triangles come out inside out, so those 4 units sit ON TOP of the terrain and you stand 4-6 units above the visible surface -- invisibly, with a standable up normal, until you reach the seam with a correctly wound neighbour.\nMeasured over the library: 30.2% of all displacements are side==1, and 99.64% of their collision triangles are inverted.\n0: leave the winding alone (every build before Patch 256).\n1: re-wind for collision only; the render mesh is untouched (default).", MAPOPTIONS);
		hl2_dispflags = cvarfuncs->GetNVFDG("hl2_dispflags", "1", CVAR_MAPLATCH, "Whether a displacement's own \"no collision\" flags are honoured.\nSource stores them in ddispinfo_t's minTess field and gates the player's hull sweep on SURF_NOHULL_COLL, separately from the contents mask -- so a displacement can be CONTENTS_SOLID and still be walked through.\n0: ignore them, every displacement collides (what shipped before Patch 250).\n1: honour them (default). 46% of displacements in a 90-map sample are flagged NOHULL; surf_demise's smoke walls are 552 of them.", MAPOPTIONS);
		/*
		FTESurf Build 13.  0 turns env_cubemap reflections off entirely, which
		is ALSO the pre-build-13 behaviour minus the bug: with no per-surface
		cubemap the backend used to substitute the SKYBOX, which is why
		surf_666's tile and glass came out cyan on a warm map.  See
		VBSP_LoadCubemaps.  Worth having as a toggle because a cubemap fetch is
		a dependent texture read on every env-mapped pixel.
		*/
		hl2_cubemaps = cvarfuncs->GetNVFDG("hl2_cubemaps", "1", CVAR_MAPLATCH, "Load a Source map's baked env_cubemap reflections and pick the nearest per surface, as Source does.\n0: no cubemap reflections at all (cheaper; env-mapped surfaces go flat).\n1: normal (default).", MAPOPTIONS);
		/*
		FTESurf Patch 163.  VRAD's own per-vertex lighting for each static prop,
		shipped inside the map as sp_<index>.vhv -- which is what Source lights a
		static prop with.  The leaf ambient cube is the fallback, not the rule,
		and using it for everything is what gave neighbouring copies of the same
		prop visibly different colours.  See VBSP_LoadPropBakedLight.
		*/
		/*
		FTESurf Patch 259 adds mode 2 and keeps 0 and 1 exactly as they were, so
		falling back is instant and needs no other cvar to be found.  The cost of
		2 is RAM at map load -- 4 bytes per prop VERTEX, mean 2.2 MiB and worst
		25.0 MiB across the 312 maps in the library that ship a bake -- and
		nothing per frame: props are already one entity each, FTE does not
		instance them, and the lighting solve is cached for the life of the map.
		*/
		hl2_lt_baked = cvarfuncs->GetNVFDG("hl2_lt_baked", "2", CVAR_MAPLATCH, "Light static props from VRAD's own baked per-vertex lighting (the map's sp_N.vhv files), which is what Source does.\n0: use the leaf ambient cube for every prop -- coarser, and neighbouring props can land on different samples.\n1: cheap -- one colour per prop, the mean of its bake. Correct level and hue, no shading within the prop.\n2: keep the bake per vertex, as VRAD wrote it and as Source draws it (default). Costs 4 bytes per prop vertex at map load; see hl2_lt_baked_vc to A/B it against 1 without reloading.", MAPOPTIONS);
		hl2_lt_baked_vc = cvarfuncs->GetNVFDG("hl2_lt_baked_vc", "1", 0, "Whether the per-vertex bake loaded by hl2_lt_baked 2 is actually applied. Live -- no map reload -- so this is the switch to A/B with; hl2_lt_baked itself is latched because it decides what gets read off disk. `prop_census` reports how many of the props drawn last frame used it.\n0: draw those props from the mean instead, i.e. exactly hl2_lt_baked 1.\n1: normal (default). No effect at all when hl2_lt_baked is 0 or 1.", MAPOPTIONS);
		/*
		FTESurf Build 8.  See the memset in Mod_LoadVBSP for the whole story.
		1 is Source's own initial state and restores the thirteen fourteenths of
		surf_666 that could not be drawn; 0 is the Quake 2 behaviour this used to
		have, kept so the difference can be seen rather than argued about.
		*/
		hl2_areaportals = cvarfuncs->GetNVFDG("hl2_areaportals", "1", CVAR_MAPLATCH, "Initial state of a Source map's areaportals.\n0: closed, and opened only by gamecode -- the Quake 2 rule, which seals off every area a Source map has because nothing closes or opens them.\n1: open, which is what Source itself initialises them to (default).", MAPOPTIONS);
		/*
		FTESurf Build 10 -- the price of Source water and Source glass.

		Both are RENDER TARGETS in the shader the VMT compiles to, and the
		backend fills a render target by rendering the world into it again
		(gl_backend.c:5732/5781).  See the block at the top of mat_vmt.c for
		what each setting drops.

		FTESurf Patch 161 -- CVAR_SHADERSYSTEM, NOT CVAR_MAPLATCH.  Asked as
		"is it possible for these to swap without a vid_restart?"  Yes, and
		without a map reload either: the flag for "changing this invalidates
		every shader" already exists and is exactly these cvars' semantics.
		cvar.c:992-1001 calls Shader_NeedReload the moment the value changes,
		and the next frame re-parses -- which for a generated shader means
		re-running Shader_GenerateFromVMT with the new value.  It is not in
		CVAR_LATCHMASK (cvar.h:148), so it is not a latch at all: the cvar takes
		its new value immediately and the flush is a side effect.

		Build 11 found MAPLATCH by ruling out RENDERERLATCH, and MAPLATCH was
		correct -- it made these work.  It was just a bigger hammer than the job
		needs, and it made comparing two settings cost a map load each.

		hl2_cubemaps stays MAPLATCH: it decides what VBSP_LoadCubemaps puts in
		mod->envmaps while the BSP is being read, which no shader flush revisits.
		*/
		hl2_water = cvarfuncs->GetNVFDG("hl2_water", "3", CVAR_SHADERSYSTEM, "How Source water is drawn, and what it costs.\n0: flat translucent -- a $fogcolor sheet, no extra scene render, but it still blends and still sorts.\n1: refraction, no reflection -- one extra scene render, and what Source's own cheap water does.\n2: refraction and reflection -- two extra scene renders per batch, which is what shipped before build 10.\n3: dithered flat (default) -- an ordered dither of $fogcolor. Opaque geometry with a discard: no blend, no sort, no render target, and it writes depth, so what is under the water is z-rejected instead of overdrawn. The cheapest of the four.", MAPOPTIONS);
		hl2_refract = cvarfuncs->GetNVFDG("hl2_refract", "2", CVAR_SHADERSYSTEM, "How Source's Refract materials (glass, windows) are drawn.\n0: plain translucency, with no framebuffer copy.\n1: Source's refraction, which copies the framebuffer once per batch.\n2: dithered flat (default) -- the pane keeps its own texture, $alpha sets the coverage, and it becomes opaque geometry that writes depth.", MAPOPTIONS);
		hl2_vertexcolor = cvarfuncs->GetNVFDG("hl2_vertexcolor", "1", CVAR_SHADERSYSTEM, "FTESurf Patch 232: $vertexcolor / $vertexalpha on LightmappedGeneric and WorldVertexTransition materials -- the per-vertex tint and opacity a mapper paints onto a DISPLACEMENT (every other surface carries white at alpha 1, so this cannot change them).\n0: ignore both keys, which is what every build before P232 did.\n1: draw what the material asks for (default). 41 transition materials in 20 maps and 494 LightmappedGeneric ones in 111 maps carry them; surf_rise's ivy is the reported case.", MAPOPTIONS);
		hl2_bottommaterial = cvarfuncs->GetNVFDG("hl2_bottommaterial", "1", CVAR_SHADERSYSTEM, "FTESurf Patch 233: a water material's $bottommaterial -- the underside of the water -- when the map names one and does not ship it.\n0: leave it unresolved, which is what every build before P233 did; the underside draws with the default wall shader and the map reports a missing material.\n1: draw it as the water material that names it (default). surf_aesthetic's water/clear_beneath is 325 faces and surf_demise's water_tar_beneath 7; neither exists in any pack, in either spelling.", MAPOPTIONS);
		hl2_skinfallback = cvarfuncs->GetNVFDG("hl2_skinfallback", "1", CVAR_SHADERSYSTEM, "FTESurf Patch 234: what a model does when one of its skins names a material that was never shipped.\n0: nothing -- that skin draws with the default wall shader, and the map reports a missing material.\n1: use another skin of the same slot that did resolve (default). surf_demise ships three of its four bone skins and one of its two fir-branch skins; no prop on the map selects the missing ones, so this silences two reports and changes nothing on screen.", MAPOPTIONS);
		hl2_translucent = cvarfuncs->GetNVFDG("hl2_translucent", "1", CVAR_SHADERSYSTEM, "$translucent world materials (windows, grates, railings).\n0: draw them OPAQUE -- they keep their texture and lightmap but stop being sorted, which is what they actually cost on a map with a lot of glazing.\n1: as the material author wrote them (default).", MAPOPTIONS);
		hl2_decallit = cvarfuncs->GetNVFDG("hl2_decallit", "1", CVAR_SHADERSYSTEM, "FTESurf Patch 236: how a LightmappedGeneric material that also sets $decal is lit -- graffiti, stains, ivy, and every other decal painted straight onto a world brush face.\n0: route it to the model shader on the strength of $decal alone, which is what every build before P236 did. It costs the surface its lightmap, and (since P232 hands that shader two permutations it does not declare) its whole program: the material then draws as a flat opaque rectangle of whatever the artist left in the transparent texels. This is only here to reproduce that on demand.\n1: keep the lightmapped shader the material asked to be (default). 18 of bhop_futile's 68 world textures are decals; 224 maps also place infodecal.", MAPOPTIONS);
		hl2_vmtkeys = cvarfuncs->GetNVFDG("hl2_vmtkeys", "0", 0, "How loudly to report .vmt keys this build does not implement.\n0: record them silently and let `vmt_keys` print the summary (default).\n1: also print each distinct key once per map, the first material that used it.\n2: print every occurrence, which is what shipped before Patch 250 -- a re-parsed material repeats its whole key list, so one unresolved model VMT with a $treeSway block was measured at 576 lines.\nEither way `vmt_keys` has the counts; this only decides whether they also go past you on the way.", MAPOPTIONS);
		cmdfuncs->AddCommand("vmt_keys", Mat_VMT_KeyCensus_f, "Lists the .vmt keys the current map used that this build does not implement, split into real Source shader parameters and names Source does not declare either (material-proxy variables, engine-branch extensions, typos). See hl2_vmtkeys.");
		hl2_emissive = cvarfuncs->GetNVFDG("hl2_emissive", "1", CVAR_SHADERSYSTEM, "FTESurf Patch 250: $emissiveblendenabled, Source's scrolling self-illumination pass -- a flow map displaces the lookup into an emissive texture which is then scrolled, so the layer churns rather than slides.\n0: ignore the keys, which is what every build before P250 did. The six materials in the library that use it have no $basetexture at all, so they draw as nothing.\n1: draw it (default). surf_demise's three summoning circles and its two credits banners are the reported case.", MAPOPTIONS);
		hl2_seamless = cvarfuncs->GetNVFDG("hl2_seamless", "1", CVAR_SHADERSYSTEM, "FTESurf Patch 251: $seamless_scale, Source's triplanar projection. The material stops using its UVs and projects its base map three times down the world axes, blended by the surface normal squared, so one texture drapes over a displacement with no seam anywhere.\n0: ignore the key and use the material's own UVs, which is what every build before P251 did -- on a cliff that shows as visible stretching and a seam at every change of slope.\n1: project it (default). 367 materials across 89 of the library's 1310 maps, split roughly evenly between LightmappedGeneric and WorldVertexTransition; surf_outra (28), surf_surreal (24) and surf_demise (13, over 1221 terrain faces) carry the most.", MAPOPTIONS);
		hl2_bumpmap2 = cvarfuncs->GetNVFDG("hl2_bumpmap2", "1", CVAR_SHADERSYSTEM, "FTESurf Patch 251: $bumpmap2, WorldVertexTransition's second normal map, blended by the same factor as the two base textures.\n0: use the first normal map everywhere, which is what every build before P251 did.\n1: blend them (default).\nHONEST SCOPE: transition.glsl only reads the normal to warp a cubemap reflection, so of the 481 materials across 175 maps that carry $bumpmap2, only the 130 (27%) that also carry $envmap look any different. The rest are correct and invisible until this shader lights from the normal for something other than a reflection. surf_demise is 6 of 6.", MAPOPTIONS);

		/*
		FTESurf Build 13: the on/off switches, so a feature's cost can be
		measured instead of argued about.  CVAR_SHADERSYSTEM as of Patch 161 --
		they change the generated shader text, and that flag means "changing
		this invalidates every shader", so they now apply on the next frame
		rather than on the next map load.  See the note above hl2_water.
		*/
		/*
		FTESurf Patch 174: default 0.55 -> 1.

		0.55 was chosen against water, where the $fogend curve lands high (0.35
		to 0.90) and needed pulling back.  Glass shares the multiplier and starts
		from a 0.25 fallback, so the same number took panes down to 0.14 -- an
		empty frame rather than a window.  1 is "whatever the material asked
		for", which is the only default that does not silently second-guess every
		VMT in the map; hl2_dither_force is the knob for overriding them all
		deliberately, and it does it by replacement rather than by scaling.
		*/
		hl2_dither_alpha = cvarfuncs->GetNVFDG("hl2_dither_alpha", "1", CVAR_SHADERSYSTEM, "Coverage MULTIPLIER for the dithered water/glass modes (hl2_water 3, hl2_refract 2), applied to each material's own derived coverage. Lower = more see-through. 1 (default) is the material's own value untouched. Because it scales, the faintest surfaces stay the faintest -- see hl2_dither_force to give every dithered surface the same coverage instead.", MAPOPTIONS);
		hl2_dither_force = cvarfuncs->GetNVFDG("hl2_dither_force", "0", CVAR_SHADERSYSTEM, "One coverage for EVERY dithered surface (hl2_water 3, hl2_refract 2), replacing the per-material derivation rather than scaling it.\n0: off -- each material keeps its own coverage, scaled by hl2_dither_alpha (default).\n0.03 to 1: that coverage, on all of them. Use this when a map's own $alpha values leave panes too faint to see.", MAPOPTIONS);
		hl2_bumpmap = cvarfuncs->GetNVFDG("hl2_bumpmap", "1", CVAR_SHADERSYSTEM, "Load $bumpmap/$normalmap from Source materials.\n0: no normal maps -- flat lighting, and it also drops the per-pixel work that reads them.\n1: normal (default).", MAPOPTIONS);
		hl2_envmap = cvarfuncs->GetNVFDG("hl2_envmap", "1", CVAR_SHADERSYSTEM, "Source $envmap reflections on world and model materials, INCLUDING named skybox cubemaps.\n0: no reflections at all; env-mapped surfaces draw as plain diffuse+lightmap.\n1: normal (default). See also hl2_cubemaps, which leaves the shader path alone and only controls the map's own baked env_cubemaps.", MAPOPTIONS);
		//FTESurf Patch 268 B: the literal env_cubemap sentinel, and the Source-parity
		//bits for the envmap term.  Both owned by mat_vmt.c, registered here with the
		//rest.  SHADERSYSTEM because both decide what gets COMPILED into the material.
		hl2_envcubemap = cvarfuncs->GetNVFDG("hl2_envcubemap", "0", CVAR_SHADERSYSTEM, "$envmap env_cubemap on models and unpatched brush faces reflects the map's nearest baked cubemap.\n0: today -- such a material reflects nothing UNLESS it also carries $envmapmask, which is the one spelling that already worked.\n1: on. Regenerates materials, so use r_envcubemap for a live A/B on a loaded map.", MAPOPTIONS);
		hl2_envmap_source = cvarfuncs->GetNVFDG("hl2_envmap_source", "0", CVAR_SHADERSYSTEM, "Source parity for the envmap term, a bitmask; 0 = today's arithmetic.\n1: Source's mask rules -- no mask key means FULL strength rather than 1-alpha, only $basealphaenvmapmask is inverted, and both it and $envmapmask are ignored on a bumped VertexLitGeneric as Source's own helper does.\n2: $envmapcontrast (parsed and discarded before now) plus Source's tint->contrast->saturation order; this also hands WorldVertexTransition its $envmaptint/$envmapsaturation, which that shader has never applied at all.\n4: VertexLitGeneric adds the reflection AFTER lighting, so a dark prop no longer dims its own reflection.", MAPOPTIONS);
		//FTESurf Patch 268 C: per-pixel ambient-cube relief on bumped props, and its two
		//companions.  hl2_cubelight and hl2_bumpgreenfix are mat_vmt.c's and SHADERSYSTEM,
		//because they decide what is compiled; hl2_lt_dirsign is this file's, and a plain
		//cvar, because it only changes a cached light direction (see VBSP_PrepareFrame).
		hl2_cubelight = cvarfuncs->GetNVFDG("hl2_cubelight", "0", CVAR_SHADERSYSTEM, "Per-pixel relief on bumped Source props: the leaf's six-face ambient cube evaluated with the NORMAL-MAPPED normal, as Source lights them, applied as a ratio over the prop's existing (baked) lighting.\n0: today -- a VertexLitGeneric with $bumpmap and no $envmap samples no normal map at all and draws flat.\n1: on. Regenerates materials, so use r_cubelight for a live A/B on a loaded map.", MAPOPTIONS);
		hl2_bumpgreenfix = cvarfuncs->GetNVFDG("hl2_bumpgreenfix", "0", CVAR_SHADERSYSTEM, "Green channel of a bumped VertexLitGeneric's normal map.\n0: today -- flipped, which mirrors the relief along V relative to Source (Source never flips it; handedness rides on the tangent's w).\n1: unflipped, as Source. Also bends the Patch 268 B reflection the same way.", MAPOPTIONS);
		hl2_lt_dirsign = cvarfuncs->GetNVFDG("hl2_lt_dirsign", "0", 0, "Direction of the model light derived from the leaf ambient cube.\n0: today -- from unsigned face deviations, which points AWAY from a single bright face and drops the sign of a symmetric pair.\n1: the signed luminance difference of each opposing face pair, pointing toward the light. Visible only on props with no .vhv bake, and on models that are not static props.", MAPOPTIONS);
		//FTESurf Patch 296.  REGISTERED HERE AS WELL AS LAZILY IN THE LOADER, for the
		//reason the essay below (:10261-10281) gives for the other four: the loader's
		//own GetNVFDG does not run until the first prop of the first map is read, so
		//until then the name does not exist, the menu's cvar() reads 0, and cvarlist
		//cannot show it.  hl2_lt_face is the standing example of getting this wrong.
		//GetNVFDG returns an existing cvar rather than minting a duplicate, so the two
		//registrations are the same cvar and the defaults must agree -- so if you change
		//one of the three below, change the copy in VBSP_LoadPropBakedLight with it.
		cvarfuncs->GetNVFDG("hl2_lt_vhv12", "1", 0, "Accept VRAD's 12-byte-per-vertex baked static prop lighting: three radiosity-normal basis colours per vertex instead of one colour, which the loader rejected outright before Patch 296.\n67 of the library's maps ship it (27,178 props) and lose ALL their baked prop lighting without this -- surf_fantasy read \"props: 0 of 2345\" with 2345 .vhv files sitting in its own pakfile.\n0: reject it, as every build before Patch 296 did (bit-for-bit that picture).\n1: read it, averaging the three basis colours (default). That mean is not an approximation: utils/vrad/lightmap.cpp:3479-3549 SCALES the three so their arithmetic mean equals the unbumped value, and asserts it in a dead variable at :3533. Proven on the files as well -- on 465 of surf_fantasy's props the 12 bytes are three bit-identical copies of one BGRA, and against the CS:S build's ordinary 4-byte files the mean out-correlates every individual basis (r 0.764 matched, 0.236 shuffled).", MAPOPTIONS);
		//FTESurf Patch 296: the decode curve, and the directional fraction for a
		//baked prop.  Same two-site rule as above.
		cvarfuncs->GetNVFDG("hl2_lt_baked_curve", "1", 0, "Which curve decodes VRAD's baked static prop lighting.\n0: sRGB -- the pre-Patch-296 behaviour, bit-for-bit.\n1: VRAD's own, pow(b/127.5, 2.2) (default). This is what utils/vrad actually wrote: LinearToVertexLight (public/mathlib/mathlib.h:1410-1428) over a pow(x, 1/2.2) table scaled by overbrightFactor 0.5 (mathlib/color_conversion.cpp:233-261, MathLib_Init at utils/vrad/vrad.cpp:2356). The stored-byte ceiling of 239, measured over every prop of two separate builds of surf_fantasy, is round(255*0.5*4^(1/2.2)) and pins all three terms.\n2: no decode -- mean the stored bytes. Diagnostic only: it puts the level in the same space as the per-vertex multiplier and so measures how far apart the other two hold them.\nRead at load; `map <the same name>` does not reload, so an A/B needs a different map in between or a second process.", MAPOPTIONS);
		hl2_lt_baked_dir = cvarfuncs->GetNVFDG("hl2_lt_baked_dir", "0", 0, "Directional fraction given to a prop lit from its own VRAD .vhv bake, 0 to 1.\n0: today -- the bake is used flat, because VRAD already baked the light's direction into each vertex and re-applying a direction over it would shade it twice (default).\n1 (or any fraction): split that much of the level into a max(0,N.L) term along the leaf ambient cube's dominant direction, which gives a nearly-uniform bake some silhouette.\nNOTE: this is per-VERTEX against the geometric normal, so it can never produce relief on a bumpmapped prop -- hl2_cubelight is the knob for that. And at any non-zero value hl2_lt_dirsign 1 becomes a prerequisite, or it lights the wrong side.", MAPOPTIONS);

		//FTESurf Patch 263: WindowImposter, Source's fake-second-skybox shader. See the arm in mat_vmt.c.
		hl2_imposter = cvarfuncs->GetNVFDG("hl2_imposter", "1", CVAR_SHADERSYSTEM, "Source's WindowImposter shader -- a brush that draws a cubemap by VIEW DIRECTION, used to fake a second skybox in one room (155 materials across 59 maps).\n0: draw them nodraw, so whatever is behind shows through -- on a fake-sky slab that is the map's real sky.\n1: as authored (default). Before this existed the class was unimplemented and these surfaces drew as the notexture checkerboard.", MAPOPTIONS);
		//FTESurf: UnlitTwoTexture's second layer and its material proxies. See the arm in mat_vmt.c.
		cvarfuncs->GetNVFDG("hl2_fog_alphamul", "0", CVAR_SHADERSYSTEM, "FTESurf Patch 299: whether a fogged Source surface is multiplied by its own base-texture ALPHA.\n0: no (default). Fog the colour and leave the alpha to the blender, which is what Source does.\n1: yes -- the pre-Patch-299 picture, bit-for-bit. That is what the engine's fog4() has always done (gl_vidcommon.c:1887, `vec4(fog3(rgb),1.0) * regularcolour.a`), and it is correct only for a PREMULTIPLIED blend, which this plugin never emits.\nSource does not treat base alpha as opacity unless the material says so: on an opaque VertexLitGeneric it is a mask ($basealphaenvmapmask and friends) and the surface is solid. With no fog the opaque blend discards it, so nothing shows; the moment fog is on, the mask multiplies every pixel. Measured on surf_tensor2's 3D skybox, the mean base alpha of the eight building materials in frame splits cleanly into 0.028-0.094 and 0.920-0.984, and it matches the screen object for object -- the 0.03 props drew at 3% of the fog colour, a black cutout against a correctly fogged sky, which is the reported 'bright black' buildings, and the low-alpha window panes inside the 0.92-0.96 textures are the black windows beside them.\nTranslucent materials are affected too and in the same direction: mat_vmt.c:3719/3885 emit `src_alpha one_minus_src_alpha`, so GL already applies the alpha and fog4() was applying it a second time. Additive (fog4additive) and water (fog4blend) never multiplied and are unchanged either way.", MAPOPTIONS);

		hl2_twotexture = cvarfuncs->GetNVFDG("hl2_twotexture", "1", CVAR_SHADERSYSTEM, "Source's UnlitTwoTexture shader, which MULTIPLIES two texture layers -- combine shields and force fields, HL2 monitors and displays, fog cards, and five of Momentum's own powerup props (48 materials in the mounted packs).\n0: draw $basetexture alone, which is what this did before the second layer existed -- a still image of one of the two layers.\n1: both layers, with the second one's TextureScroll proxy and the PlayerProximity distance fade and frame ramp compiled into the shader (default).", MAPOPTIONS);

		//FTESurf Patch 286: the flipbook half of the above, switchable on its own
		//so "denser as you approach" can be A/B'd without also losing the second
		//layer -- hl2_twotexture 0 changes three things at once and is the wrong
		//control for this one.
		/*
		FTESurf Patch 288.  READ IN TWO PLACES, on purpose.

		At MAP LOAD it gates generation, so 0 at launch costs nothing at all --
		no LUT textures, no post-process pass, no FBO round trip.  And it is a
		LIVE uniform inside the shader (`!!cvarf hl2_colourcorrection`), so on a
		map that was loaded with it on, 0 / 1 / 0 at a fixed vantage is a
		three-picture A/B with nothing else changing between the frames.  That
		matters here more than usual: `map x` twice does not reload, so an A/B
		that needed a map change would be the kind of measurement this project
		has already recorded getting wrong.

		Fractional values work and are useful -- .5 is "half the grade", which is
		how a wrong LUT is told apart from a right LUT applied too strongly.
		*/
		hl2_colourcorrection = cvarfuncs->GetNVFDG("hl2_colourcorrection", "1", CVAR_SHADERSYSTEM, "Source's color_correction entities -- a 32x32x32 lookup table applied to the whole frame, which is how a Source map's author set its colour.  FTE has never applied any of them, so every graded map in the library has been rendering ungraded.\nOnly the global form is implemented (minfalloff/maxfalloff -1, which is the common idiom); a correction with a real distance falloff, and color_correction_volume entirely, are counted and declined in the map's census line.\n0: no grade, the way every build before this one looked.  Live -- it takes effect on the current frame, and 0 is bit-for-bit the ungraded scene.\n1: apply the map's own grade (default).  Going from 0 to 1 needs a map load if the map was loaded with it off, because the shader is generated then.\nFractional values scale the grade: .5 is half of it.", MAPOPTIONS);

		hl2_additivefog = cvarfuncs->GetNVFDG("hl2_additivefog", "1", CVAR_SHADERSYSTEM, "Draw ADDITIVE Source materials with a real GLSL program, so that fog fades them toward BLACK instead of toward the fog colour.\nA shader with no top-level program gets fixed-function GL fog, which mixes toward GL_FOG_COLOR on RGB and cannot know the surface is additive.  On a gl_one/gl_one sprite the only mask is black -- light_glow02/03.vtf are BGR888 with no alpha channel at all -- so a masked-out texel picks up fogcolour*(1-f) and is then ADDED, and the sprite draws as a flat translucent rectangle instead of a glow.  surf_tensor2 is the reported case: a negative fogstart (-2500) makes its fog factor 0.857 even at zero depth, so the lift is there at every distance.\nOnly ADDITIVE materials move, which is what keeps surf_boreas's cloud layer (Patch 266) out of it -- cloods.vmt has its $additive line commented out and is alpha-blended, so it never enters this path.\n0: keep every additive material on a pass, i.e. the pre-P300 behaviour, fog artefact included.\n1: give them vmt/unlit and the correct fog4additive (default).  1,659 additive UnlitGeneric and 838 additive Sprite materials in the library are eligible.", MAPOPTIONS);

		hl2_twoframes = cvarfuncs->GetNVFDG("hl2_twoframes", "1", CVAR_SHADERSYSTEM, "The DISTANCE-DRIVEN FLIPBOOK on an UnlitTwoTexture material.  comshieldwall.vtf is 31 frames and its proxy chain picks one by how far away you are -- frame 0 inside 120 units, frame 30 beyond 270 -- so a combine shield gets DENSER as you approach and not merely brighter.\nOnly materials that point $basetexture and $texture2 at the SAME file can take it (7 of the 48 in the mounted packs), because a 2DArray sampler has to be filled by a pass and only a one-sampler shader has ever worked here; the rest are counted as declined in the map's census line.\n0: draw frame 0, which is the close-up frame and the right one to be stuck on.\n1: sample the frame the distance asks for (default).", MAPOPTIONS);

		/*
		FTESurf Patch 174: the four model-lighting cvars, registered HERE as well
		as where they are read.

		Each of them is created lazily, inside the function that first needs it
		(VBSP_LoadPropBakedLight, VBSP_LeafAmbient, VBSP_ModelLighting), which
		means that until a Source map has been loaded once they do not exist at
		all.  That is invisible from a console -- you would just type the name and
		get a value -- but it is not invisible from a menu: the gfx menu reads
		these with cvar(), and an unregistered name reads 0, so every one of them
		would have shown "Off" on a fresh start and cycled from the wrong place.

		Registering them up front costs four cvars and makes the menu's reading
		of them true at all times.  The lazy calls still stand: GetNVFDG hands
		back the cvar that already exists rather than making a second one, so
		this is one cvar with two registration sites, not a duplicate.

		They carry NO latch flag, and that is correct -- they are read while a map
		loads, so the value changes immediately and the next load is what applies
		it.  The menu marks them * for exactly that reason.
		*/
		cvarfuncs->GetNVFDG("hl2_lt_baked_scale", "2", 0, "Overbright applied to VRAD's baked static prop lighting. 2 is the same factor gl_overbright already gives the world lightmap and which model lighting has never had; 1 is the raw baked value, as VRAD wrote it.", MAPOPTIONS);
		cvarfuncs->GetNVFDG("hl2_lt_min", "16", 0, "Minimum model ambient on Source/HL2 maps (0-255), applied by SCALING the colour rather than clamping each channel -- a per-channel clamp desaturates, which is what made every prop in a dim leaf render pale grey. 0 = off.", MAPOPTIONS);
		cvarfuncs->GetNVFDG("hl2_lt_scale", "255", 0, "Model-lighting brightness scale, applied after the encode. 255 maps a fully-lit luxel to white.", MAPOPTIONS);
		cvarfuncs->GetNVFDG("hl2_lt_srgb_mag", "1", 0, "sRGB-encode model lighting (0 = plain linear scale by hl2_lt_scale). Source's ambient cube is LINEAR light and needs a gamma encode to be displayed; a multiply is not one.", MAPOPTIONS);
		//FTESurf Patch 302.  A plain cvar, not CVAR_MAPLATCH: the lump is loaded
		//either way and this only decides whether the loaded data is used, which
		//is the same distinction hl2_lt_baked (latched, decides what is READ) and
		//hl2_lt_baked_vc (plain, decides whether it is USED) already draw.  Read
		//in VBSP_LightPointValues and invalidated in VBSP_PrepareFrame, so it is
		//live and A/B-able inside one process.
		//FTESurf Patch 304.  Registered HERE, in VBSP_Init, which runs at plugin
		//load and therefore before any material compiles a program -- so this
		//default is the one vertexlit.glsl's !!cvardf finds already present
		//(gl_shader.c:2291 Cvar_Gets the name and keeps an existing value).  The
		//"=1" on the cvardf line is only the fallback for a build where this
		//plugin never loaded, which is a build with no vertexlit.glsl either.
		hl2_lt_fold = cvarfuncs->GetNVFDG("hl2_lt_fold", "1", 0, "How the leaf ambient cube is folded into the shader's two lighting slots, for static props with no .vhv bake.\n0: the legacy fold -- the flat base is the mean PLUS the deviation toward the light, so a face turned away from every light still receives the bright-side level. Measured 13.6x too bright on tensor2's worst face. This is the falsifier and is bit-for-bit the pre-304 behaviour.\n1: default. The flat base is the level on the face turned AWAY from the dominant direction, the directional half is the difference to the face turned toward it, and vertexlit.glsl ramps between them with a HALF-lambert, as Source does on models. Mean error over a uniform sphere of normals falls from 1.037 to 0.283 stops on tensor2, and from 0.463 to 0.230 over 200 real leaf cubes.\n0/1 ONLY -- the value is compiled into the shader as a preprocessor constant, and a fraction would emit a float literal into an #if.", MAPOPTIONS);
		hl2_lt_worldlight = cvarfuncs->GetNVFDG("hl2_lt_worldlight", "1", 0, "Light models with no VRAD bake from the map's worldlights as well as the leaf ambient cube, as Source's light cache does.\n0: the leaf ambient cube alone, which VRAD fills with BOUNCE ONLY, so a prop with no .vhv draws at roughly a fifth of its Source brightness. This is the falsifier -- it is bit-for-bit the pre-302 behaviour.\n1: default. Add the direct contribution of every emit_surface/point/spotlight whose cluster is in the sample point's PVS. Sky lights are deliberately excluded until a sky-visibility trace exists -- they have no distance falloff, so without one they would light sealed indoor rooms at full sun. Does nothing to props that DO have a .vhv bake; their direct light is already in it.", MAPOPTIONS);
		//FTESurf Patch 308.  Registered here, beside the term it feeds, and NOT
		//latched: VBSP_LoadWorldLights reads it once per map load, so changing it
		//needs a map change to take effect and the description has to say so.
		hl2_lt_wl21 = cvarfuncs->GetNVFDG("hl2_lt_wl21", "1", 0, "Read the worldlights lump of a v21 (Strata) map, whose record is 100 bytes rather than 88.\n0: refuse it, the pre-308 behaviour. 123 of the 1314 installed maps ship this lump version, so hl2_lt_worldlight -- and with it Patch 304's fold -- silently did nothing on any of them, including surf_fantasy.\n1: default. The layout is a single 12-byte insertion after `normal` (Strata's shadow_cast_offset, zero on all 39,641 lights measured), derived by pairing the CS:S and Momentum builds of surf_fantasy light-for-light rather than guessed: every field that must survive a recompile agrees on 777 of 777, and across all 123 maps every light yields a valid type, a valid style and a unit normal.\nRead at map load -- an A/B needs a map change or a second process.", MAPOPTIONS);

		/*
		FTESurf Patch 174: the load result, published for the HUD.

		Written by Mod_LoadVBSP at the end of every map load and read by CSQC.
		NOSAVE because it is an observation about the map you are in, not a
		setting -- writing it into ftesurf.cfg would put a stale banner on screen
		on the next launch before any map had loaded.
		*/
		cvarfuncs->GetNVFDG("hl2_unresolved", "0", CVAR_NOSAVE, "How many materials the map that is loaded failed to resolve. Set by the loader; see the console for the list and the fs_load line that fixes it.", MAPOPTIONS);
		cvarfuncs->GetNVFDG("hl2_unresolved_first", "", CVAR_NOSAVE, "The first material the loaded map failed to resolve, as a hint at which asset pack it wants.", MAPOPTIONS);
		//FTESurf Patch 233: ...and how many of them are on a face or a prop rather than only
		//in the texdata string table.  The HUD banner draws on THIS, not on the raw count.
		cvarfuncs->GetNVFDG("hl2_unresolved_visible", "0", CVAR_NOSAVE, "How many of hl2_unresolved are on something you can actually look at -- a drawn world face, or a prop skin. A material named in the map's texdata table that no face uses cannot be seen and does not count. `hl2_missing` says which is which.", MAPOPTIONS);

		/*
		FTESurf Patch 254: the loaded map's underwater fog, read out of the first
		Water material's $fogcolor/$fogstart/$fogend.  Outputs, not settings --
		the gamecode reads them at map load and emits `waterfog`.  Empty/0 means
		the map has no water material that declares fog, and the gamecode then
		leaves FOGTYPE_WATER alone.
		*/
		cvarfuncs->GetNVFDG("hl2_waterfog", "", CVAR_NOSAVE, "The loaded map's underwater fog colour as \"r g b\" in 0..1, taken from the first Water material's $fogcolor. Empty if no water material declares one. Set by the loader; read by the gamecode, which turns it into the engine's `waterfog`.", MAPOPTIONS);
		cvarfuncs->GetNVFDG("hl2_waterfog_start", "0", CVAR_NOSAVE, "The loaded map's underwater fog start distance ($fogstart on the same Water material as hl2_waterfog).", MAPOPTIONS);
		cvarfuncs->GetNVFDG("hl2_waterfog_end", "0", CVAR_NOSAVE, "The loaded map's underwater fog end distance ($fogend on the same Water material as hl2_waterfog) -- how far you can see while submerged.", MAPOPTIONS);

		//FTESurf Patch 231: and the list the warning is too short to print.
		if (cmdfuncs)
		{
			cmdfuncs->AddCommand("hl2_missing", VBSP_Missing_f, "ftesurf (P231): list every material the loaded map failed to resolve, and every one that was found but would not parse. The map-load warning shows the first 24 of the first list; this shows all of both.");
			cmdfuncs->AddCommand("prop_census", VBSP_PropCensus_f, "ftesurf (P257): what the last frame did with the map's static props -- how many drew, and how many each cull dropped. `prop_census <index>` or `prop_census <part of a model name>` reports one prop's verdict together with the fade, PVS and lighting state behind it. Use it when something is solid but not drawn.");
			cmdfuncs->AddCommand("vbsp_pvsstat", VBSP_PvsStat_f, "ftesurf (P273): how many entity visibility tests ran since the last call, how many of those fell back to the whole-tree headnode descent, and how many nodes that descent touched. sv_perfdump says the visibility test is ~98% of the server's cost on a big Source map; this says whether the tree descent is why.");
		}

		//The engine already owns r_texdiag; GetNVFDG hands back the existing cvar
		//rather than making a second one, so this is a pointer to it and not a copy.
		hl2_texdiag = cvarfuncs->GetNVFDG("r_texdiag", "0", 0, "Per-texture load diagnostic.", MAPOPTIONS);
		map_noareas = cvarfuncs->GetNVFDG("map_noareas", "0", 0, "Ignore areaportals.", MAPOPTIONS);
		map_autoopenportals = cvarfuncs->GetNVFDG("map_autoopenportals", "0", CVAR_RENDERERLATCH, "When set to 1, force-opens all area portals. Normally these start closed and are opened by doors when they move, but this requires the gamecode to signal this.", MAPOPTIONS);
		hl2_contents_remap = cvarfuncs->GetNVFDG("hl2_contents_remap",
			"/*solid*/SOLID "
			"/*window*/WINDOW "
			"/*aux*/Q2AUX "
			"/*grate*/CLIP "		//would otherwise be LAVA
			"/*slime*/SLIME "
			"/*water*/WATER "
			"/*mist*/Q2MIST "
			"/*opaque*/7 "
			"/*testfogvolume*/8 "
			"/*??*/9 "
			"/*??*/10 "
			"/*team1*/11 "
			"/*team2*/12 "
			"/*ignorenodrawopaque*/13 "
			"/*movable*/-1 "	//would otherwise be LADDER
			"/*areaportal*/Q2AREAPORTAL "
			"/*playerclip*/PLAYERCLIP "
			"/*monsterclip*/MONSTERCLIP "
			"/*current_0*/Q2CURRENT_0 "
			"/*current_90*/Q2CURRENT_90 "
			"/*current_180*/Q2CURRENT_180 "
			"/*current_270*/Q2CURRENT_270 "
			"/*current_up*/Q2CURRENT_UP "
			"/*current_down*/Q2CURRENT_DOWN "
			"/*origin*/Q2ORIGIN "
			"/*monster*/BODY "
			"/*deadmonster*/CORPSE "
			"/*detail*/DETAIL "
			"/*translucent*/Q2TRANSLUCENT "
			"/*ladder*/LADDER "	//would otherwise be Q2LADDER
			"/*hitbox*/30 "
			"/*??*/SKY"
			/*
			FTESurf Patch 223: CVAR_RENDERERLATCH -> CVAR_MAPLATCH.  Its only
			read is at BSP load (:506), so it was demanding a vid_reload for
			something a map reload settles -- the same mismatch between flag and
			read site that hl2_favour_ldr and hl2_displacement_scale had.  No
			cheat bit on this one, so there is no trade-off to weigh.
			*/
			,CVAR_MAPLATCH, "Specifies a table for hl2->internal contentbits (one entry for each source bit).", MAPOPTIONS);

		return true;
	}
	return false;
}
