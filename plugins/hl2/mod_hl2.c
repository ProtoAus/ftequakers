#include "../plugin.h"
#include "../engine/common/com_mesh.h"


/*
   Half-Life 2 / Source models store much of their data in other files.
   I'm only going to try loading simple static models, so we don't need much data from the .mdl itself.

   FIXME: multiple meshes are still buggy.
   FIXME: materials are not loaded properly.
   FIXME: no lod stuff.
*/

static plugfsfuncs_t *filefuncs;
static plugmodfuncs_t *modfuncs;
static plugfsfuncs_t *fsfuncs;

//FTESurf Build 8: the model's COLLISION shape, which is not its render mesh.
//See mod_phy.c.  hl2_propcollision is registered by VBSP_Init in mod_vbsp.c and
//selects between the .phy hull, a bounding box and the render mesh.
galiasinfo_t *Mod_PHY_CollisionMesh (model_t *mod, int mode,
                                     plugfsfuncs_t *phy_filefuncs,
                                     plugmodfuncs_t *phy_modfuncs,
                                     unsigned int contents);
extern cvar_t *hl2_propcollision;
extern cvar_t *hl2_rigidprops;	//FTESurf Patch 230, registered by VBSP_Init beside the others.
extern cvar_t *hl2_skinfallback;	//FTESurf Patch 234, likewise.

//studiohdr_t::flags.  studiomdl sets this for anything compiled $staticprop:
//the compiler has collapsed the rig, there is no way to pose the model, and
//Source itself uses the flag to refuse to attach one to an animating entity.
#define HL2MDL_STATIC_PROP	0x10

//Utility functions. silly plugins.
float Length(const vec3_t v) {return sqrt(DotProduct(v,v));}
float RadiusFromBounds (const vec3_t mins, const vec3_t maxs)
{
	int		i;
	vec3_t	corner;

	for (i=0 ; i<3 ; i++)
	{
		corner[i] = fabs(mins[i]) > fabs(maxs[i]) ? fabs(mins[i]) : fabs(maxs[i]);
	}

	return Length (corner);
}

//blargh
typedef struct
{
	unsigned int magic;
	unsigned int version;
	unsigned int revisionid;
	char name[64];
	unsigned int filesize;

	vec3_t _80;
	vec3_t _92;
	vec3_t mins;
	vec3_t maxs;
	vec3_t _128;
	vec3_t _140;
	unsigned int flags;			//FTESurf Patch 230: studiohdr_t::flags.  See HL2MDL_STATIC_PROP.
	unsigned int num_bones;
	unsigned int ofs_bones;		//hl2mdlbone_t
	unsigned int _164;
	unsigned int _168;
	unsigned int _172;
	unsigned int _176;
	unsigned int num_anims;
	unsigned int ofs_anims;		//hl2mdlanim_t
	unsigned int _188;
	unsigned int _192;
	unsigned int _196;
	unsigned int _200;
	unsigned int tex_count;
	unsigned int tex_ofs;		//hl2mdltexture_t
	unsigned int texpath_count;
	unsigned int texpath_ofs;	//hl2mdltexturepath_t
	unsigned int texbind_count;	//N slots per skin
	unsigned int skin_count;
	unsigned int texbind_offset;//provides skin|slot->texture mappings
	unsigned int body_count;
	unsigned int body_ofs;		//hl2mdlbody_t
	unsigned int _240;
	unsigned int _244;
	unsigned int _248;
	unsigned int _252;
	unsigned int _256;
	unsigned int _260;
	unsigned int _264;
	unsigned int _268;
	unsigned int _272;
	unsigned int _276;
	unsigned int _280;
	unsigned int _284;
	unsigned int _288;
	unsigned int _292;
	unsigned int _296;
	unsigned int _300;
	unsigned int _304;
	unsigned int _308;
	unsigned int _312;
	unsigned int _316;
	unsigned int _320;
	unsigned int _324;
	unsigned int _328;
	unsigned int _332;
	unsigned int _336;
	unsigned int _340;
	unsigned int _344;
	unsigned int ofs_aniname;	//348
	unsigned int num_aniblocks;
	unsigned int ofs_aniblocks;
	//other stuff?
} hl2mdlheader_t;
typedef struct
{
	unsigned int name_ofs;
	int parent;
	int junk[6];
	vec3_t pos;
	float quat[4];
	vec3_t rot;
	vec3_t posscale;
	vec3_t rotscale;
	float inverse[12];
	int junk3[18];
} hl2mdlbone_t;
typedef struct
{
	qbyte bone;		//skip ones in their base pose.
	qbyte flags;	//says what the data is
	short next;		//to walk the list when its variable sized...
	unsigned short data[1];
} hl2mdlpose_t;
typedef struct
{
	unsigned int _0;
	unsigned int name_ofs;
	float fps;
	unsigned int loop;
	unsigned int numframes;
	unsigned int _20;
	unsigned int _24;
	unsigned int _28;
	unsigned int _32;
	unsigned int _36;
	unsigned int _40;
	unsigned int _44;
	unsigned int _48;
	struct poseofs_s{
		unsigned int externalblock;
		unsigned int ofs_pose;	//hl2mdlpose_t
	} defpose;
	unsigned int _60;
	unsigned int _64;
	unsigned int _68;
	unsigned int _72;
	unsigned int _76;
	unsigned int ofs_posesections;	//{block, ofs}
	unsigned int posespersection;	//0 if its small enough to avoid the extra indirection.
	unsigned int _88;
	unsigned int _92;
	unsigned int _96;
} hl2mdlanim_t;
typedef struct
{
	unsigned int name_ofs;
	unsigned int surf_count;
	unsigned int base;
	unsigned int surf_ofs;
} hl2mdlbody_t;
typedef struct
{
	char name[64];
	unsigned int type;
	unsigned int radius;
	unsigned int mesh_count;
	unsigned int mesh_ofs;
	unsigned int vertex_count;
	unsigned int _84;
	unsigned int _88;
	unsigned int _92;
	unsigned int _96;
	unsigned int _100;
	unsigned int _104;
	unsigned int _108;
	unsigned int _112;
	unsigned int _116;
	unsigned int _120;
	unsigned int _124;
	unsigned int _128;
	unsigned int _132;
	unsigned int _136;
	unsigned int _140;
	unsigned int _144;
} hl2mdlsurf_t;
typedef struct
{
	unsigned int mat_idx;
	unsigned int model_ofs;
	unsigned int vert_count;
	unsigned int vert_first;
	unsigned int _16;
	unsigned int _20;
	unsigned int _24;
	unsigned int _28;
	unsigned int _32;
	unsigned int _36;
	unsigned int _40;
	unsigned int _44;
	unsigned int _48;
	unsigned int _52;
	unsigned int _56;
	unsigned int _60;
	unsigned int _64;
	unsigned int _68;
	unsigned int _72;
	unsigned int _76;
	unsigned int _80;
	unsigned int _84;
	unsigned int _88;
	unsigned int _92;
	unsigned int _96;
	unsigned int _100;
	unsigned int _104;
	unsigned int _108;
	unsigned int _112;
} hl2mdlmesh_t;
typedef struct
{
	unsigned int nameofs;
	unsigned int _4;
	unsigned int _8;
	unsigned int _12;
	unsigned int _16;
	unsigned int _20;
	unsigned int _24;
	unsigned int _28;
	unsigned int _32;
	unsigned int _36;
	unsigned int _40;
	unsigned int _44;
	unsigned int _48;
	unsigned int _52;
	unsigned int _56;
	unsigned int _60;
} hl2mdltexture_t;
typedef struct
{
	unsigned int nameofs;
} hl2mdltexturepath_t;
#pragma pack(push,1)	//urgh wtf is this bullshit
typedef struct
{
	unsigned int numskins;
	unsigned int offsetskin;
} hl2vtxskins_t;
typedef struct
{
	unsigned short foo;
	//no padding
	unsigned int offsetskinname;
} hl2vtxskin_t;
typedef struct
{
	unsigned int version;
	unsigned int vertcachesize;
	unsigned short bonesperstrip;
	unsigned short bonespertri;
	unsigned int bonespervert;
	unsigned int revisionid;
	unsigned int lod_count;
	unsigned int texreplacements_offset;
	unsigned int body_count;
	unsigned int body_ofs;
} hl2vtxheader_t;
typedef struct
{
	unsigned int surf_count;
	unsigned int surf_ofs;
} hl2vtxbody_t;
typedef struct
{
	unsigned int lod_count;
	unsigned int lod_ofs;
} hl2vtxsurf_t;
typedef struct
{
	unsigned int mesh_count;
	unsigned int mesh_ofs;
	float dist;
} hl2vtxlod_t;
typedef struct
{
	unsigned int stripg_count;
	unsigned int stripg_ofs;
	unsigned char flags;
	//no padding (3 bytes)
} hl2vtxmesh_t;
typedef struct
{
	unsigned int vert_count;
	unsigned int vert_ofs;
	unsigned int idx_count;
	unsigned int idx_ofs;
	unsigned int strip_count;
	unsigned int strip_ofs;
	unsigned char flags;
	//no padding (3 bytes)
} hl2vtxstripg_t;
typedef struct
{
	unsigned int idx_num;
	unsigned int idx_ofs;
	unsigned int vert_count;
	unsigned int vert_ofs;
	unsigned short bone_count;
	unsigned char flags;
	//no padding (1 byte)
	unsigned int bonestate_count;
	unsigned int bonestate_ofs;
} hl2vtxstrip_t;
typedef struct
{
	qbyte bone[3];
	qbyte bone_count;
	unsigned short vert;
	qbyte boneID[3];
	//no padding
} hl2vtxvert_t;
typedef struct
{
	unsigned int magic;
	unsigned int version;
	unsigned int revisionid;
	unsigned int lod_count;
	unsigned int lodverts_count[8];
	unsigned int fixups_count;
	unsigned int fixups_offset;
	unsigned int verts_offset;
	unsigned int tangents_offset;
} hl2vvdheader_t;
typedef struct
{
	unsigned int lod;
	unsigned int sourcevert;
	unsigned int numverts;
} hl2vvdfixup_t;
typedef struct
{
	float weight[3];
	qbyte bone[3];
	qbyte numbones;
	vec3_t xyz;
	vec3_t norm;
	vec2_t st;
} hl2vvdvert_t;
#pragma pack(pop)
/*
FTESurf Patch 201.  LEVEL OF DETAIL -- the .vtx already ships it.

Source .mdl assets carry the artist's own reduced meshes in the .vtx, under
ModelLODHeader_t, and this loader parsed the table and then threw it away: the
whole cull was `lod[1]` on the context below, applied through
min(vsurf->lod_count, countof(ctx->lod)) at the two walk sites.  The file's own
header said "FIXME: no lod stuff" and the surface emit said "fixme: lods".

Nothing else had to be written.  FTE's LOD machinery is complete and shipping
and simply had no asset to drive it: galiasinfo_t carries mindist/maxdist
(com_mesh.h), model_t carries maxlod (gl_model.h), R_GAlias_GenerateBatches
picks a level from PROJECTED SCREEN COVERAGE and r_lodscale (gl_alias.c, cvar
already defaults 5) and rejects out-of-range surfaces one line later.  Populate
three fields here and the renderer does the rest, per-prop and size-aware --
which is strictly better than the distance cull hl2_propdist had to be, because
a large prop keeps its detail much further out than a small one.

WHAT LIMITS US, and it is the fixups.

ctx->lod[l].fixup[] maps a LOD-l-compacted vertex index to a source vertex, but
Mod_HL2_LoadIndexes is handed `firstvert + mmesh->vert_first + origMeshVertID`,
and both of those offsets come out of the MDL and are LOD-0-relative.  At l == 0
the fixup map is a pure permutation so the two index spaces coincide and
everything is correct; above 0 they do not, and there is no offset in the MDL
that says where LOD l's copy of a given model starts.

But when the VVD carries NO fixups at all there is nothing to compact: every LOD
indexes the one full vertex array, Mod_HL2_LoadIndexes takes its `else` branch,
and every level is immediately correct.  So that is the gate -- LODs are used
when fixups_count is 0 and the per-LOD vertex counts agree, and any model with
fixups keeps exactly the LOD-0-only behaviour it has today.  Deriving the
per-LOD offsets is a bigger job and is left undone rather than half-done.
*/
#define HL2_MAXLOD 8

/*seriously, how many structs do you need?*/
typedef struct
{
	model_t *mod;
	hl2mdlheader_t *header;
	void *ani;

	float *basepose;
	float *baserelpose;
	galiasbone_t *bones;

	unsigned int num_animations;
	galiasanimation_t *ofs_animations;

	unsigned numverts;
	vec2_t *ofs_st_array;
	vecV_t *ofs_skel_xyz;
	vec3_t *ofs_skel_norm;
	vec3_t *ofs_skel_svect;
	vec3_t *ofs_skel_tvect;
	byte_vec4_t *ofs_skel_idx;
	vec4_t *ofs_skel_weight;
	struct
	{
		unsigned int numfixups;
		index_t *fixup;
	} lod[HL2_MAXLOD];
	unsigned int maxlod;	//how many LOD levels we may USE. 1 = LOD 0 only, the pre-P201 behaviour.
	unsigned int usedlod;	//highest level count actually emitted, for model_t::maxlod.
	qboolean rigid;			//FTESurf Patch 230: this model can never be posed, so emit no skinning data at all.
	/*
	FTESurf Patch 259: does our vertex array come out in the order VRAD's
	sp_N.vhv is written in?

	VRAD writes one baked colour per VTX STRIP-GROUP VERTEX -- the thing Source
	actually renders -- concatenated over LOD0's meshes.  Each of those names a
	.vvd vertex through its origMeshVertID, and possibly through the .vvd's own
	fixup table on top.  We keep the .vvd's raw array and push both of those
	indirections into the INDEXES instead (Mod_HL2_LoadIndexes), so our vertex
	k and VRAD's colour k are the same vertex only when that composition comes
	out as 0,1,2,...  It usually does.  It is not a rule, and the two ways it
	fails are independent:

	  - VVD fixups that are not the identity gather.  2905 of the library's
	    111,985 baked props, concentrated hard -- surf_leesriize_ksf 819,
	    conc_school 287, surf_runewords2 285.
	  - a VTX that does not name every .vvd vertex exactly once in order, which
	    is why surf_demise's tree_douglasfir01d_opt_large has 2031 colours for a
	    2787-vertex LOD0.

	Rather than test either symptom, Mod_HL2_CheckVhvOrder walks LOD0's strip
	groups and checks the composed index directly, which catches both and also
	catches the case equal totals would hide: same count, different order.  A
	model that fails marks its surfaces firstvert -1, meaning "no slice of an
	external per-vertex array addresses this", and its props fall back to
	hl2_lt_baked 1 -- per prop, silently, and without a visual regression.

	Keeping the walk's output as a scatter table instead of reducing it to a
	yes/no would reach every prop; that is the next patch, and it is a matter of
	retaining what this already computes rather than deriving anything new.
	*/
	int *vhvmap;			//VRAD colour index -> our global vertex index, grown as the walk goes
	size_t vhvcount, vhvmax;
	qboolean vhvbad;		//the walk hit something it could not resolve; no table for this model
} hl2parsecontext_t;

/*
FTESurf Patch 259.  WHERE EACH OF VRAD'S BAKED COLOURS BELONGS.

sp_N.vhv holds one colour per LOD0 VTX STRIP-GROUP vertex, walked
(bodypart, model, mesh, stripgroup) -- the order this is called in, and the
same walk vrad's ComputeLighting does.  It is NOT one colour per model vertex,
and it is NOT in model vertex order: a strip group's vertex list is ordered for
the post-transform cache, so the k'th colour belongs to whatever
origMeshVertID the k'th strip-group vertex names.

That distinction is the whole of this function, and it is not academic.  Both
readings agree on the COUNT for a lot of models -- surf_garden's s3hedge2 has
15447 either way -- so a length check passes and the colours still land on the
wrong vertices.  Rendered, that is a prop covered in dark blotches, which is
exactly what the first build of this patch produced.

Resolving a strip-group vertex is the same two steps Mod_HL2_LoadIndexes takes,
in the same order: origMeshVertID plus the mesh's base, then the .vvd fixup
table if the model has one.  The result is our GLOBAL vertex index, which is
what the colour array is indexed by, so the table this builds is used as
out[vhvmap[k]] = colour[k].

Grown with plain Malloc rather than sized up front: the total is the sum of
every LOD0 strip group's vertex count and nothing in the headers states it, so
the alternative is walking the whole nest twice.  It is copied onto the model's
memgroup once at the end of Mod_HL2_LoadMeshes.
*/
static void Mod_HL2_BuildVhvMap(hl2parsecontext_t *ctx, const hl2vtxmesh_t *vmesh, unsigned int firstindex)
{
	const hl2vtxstripg_t *vg = (const void*)((const qbyte*)vmesh+vmesh->stripg_ofs);
	size_t g, i;
	if (ctx->vhvbad)
		return;
	for (g = 0; g < vmesh->stripg_count; g++, vg++)
	{
		const hl2vtxvert_t *v = (const void*)((const qbyte*)vg+vg->vert_ofs);
		for (i = 0; i < vg->vert_count; i++)
		{
			size_t ours = (size_t)v[i].vert + firstindex;
			if (ctx->lod[0].numfixups)
			{
				if (ours >= ctx->lod[0].numfixups)
					{ ctx->vhvbad = true; return; }
				ours = ctx->lod[0].fixup[ours];
			}
			if (ours >= ctx->numverts)
				{ ctx->vhvbad = true; return; }	//would scatter outside the array
			if (ctx->vhvcount == ctx->vhvmax)
			{
				size_t nm = ctx->vhvmax ? ctx->vhvmax*2 : 1024;
				int *nv = plugfuncs->Realloc(ctx->vhvmap, sizeof(*nv)*nm);
				if (!nv)
					{ ctx->vhvbad = true; return; }
				ctx->vhvmap = nv;
				ctx->vhvmax = nm;
			}
			ctx->vhvmap[ctx->vhvcount++] = (int)ours;
		}
	}
}
static index_t *Mod_HL2_LoadIndexes(hl2parsecontext_t *ctx, unsigned int *idxcount, const hl2vtxmesh_t *vmesh, unsigned int lod, index_t firstindex, index_t bias)
{
	size_t numidx = 0, g;
	const hl2vtxstripg_t *vg;
	index_t *idx, *ret = NULL;

	vg = (const void*)((const qbyte*)vmesh+vmesh->stripg_ofs);
	for (g = 0; g < vmesh->stripg_count; g++, vg++)
	{
		if (vg->idx_count%3)
		{
			*idxcount = 0;
			return NULL;
		}
		numidx += vg->idx_count;
	}

	ret = idx = plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*idx)*numidx);

	vg = (const void*)((const qbyte*)vmesh+vmesh->stripg_ofs);
	for (g = 0; g < vmesh->stripg_count; g++, vg++)
	{
		const unsigned short *in = (const void*)((const qbyte*)vg+vg->idx_ofs);
		const unsigned short *e = in+vg->idx_count;
		const hl2vtxvert_t *v = (const void*)((const qbyte*)vg+vg->vert_ofs);
		if (ctx->lod[lod].numfixups)
		{
			index_t *fixup = ctx->lod[lod].fixup;
			for(;;)
			{
				if (in == e)
					break;
				*idx++ = fixup[v[*in++].vert+firstindex] - bias;
			}
		}
		else
		{
			for(;;)
			{
				if (in == e)
					break;
				*idx++ = v[*in++].vert+firstindex - bias;
			}
		}
	}
	*idxcount = idx-ret;
	return ret;
}
static qboolean Mod_HL2_LoadVTX(hl2parsecontext_t *ctx, const void *buffer, size_t fsize)
{	//horribly overcomplicated way to express this stuff.
	const hl2mdlheader_t *mdl = ctx->header;
	size_t totalsurfs = 0, b, s, l, m, t, z;
	const hl2vtxheader_t *header = buffer;
	const hl2vtxbody_t *vbody;
	const hl2vtxsurf_t *vsurf;
	const hl2vtxlod_t *vlod;
	const hl2vtxmesh_t *vmesh;
//	const hl2vtxskins_t *vskins;
//	const hl2vtxskin_t *vskin;

	const hl2mdlbody_t *mbody = (const hl2mdlbody_t*)((const qbyte*)mdl + mdl->body_ofs);
	const hl2mdltexture_t *mtex = (const hl2mdltexture_t*)((const qbyte*)mdl + mdl->tex_ofs);
	const unsigned short *skinbind;

	galiasinfo_t *surf=NULL;
	galiasskin_t *skin;
	skinframe_t *skinframe;
	qbyte *skinok;		//FTESurf Patch 234: did each (skin, slot) pair's material actually turn up
	size_t firstvert = 0;

	if (fsize < sizeof(*header) || header->version != 7 || header->revisionid != ctx->header->revisionid || header->body_count == 0)
		return false;

	vbody = (const void*)((const qbyte*)header + header->body_ofs);
	for (b = 0; b < header->body_count; b++, vbody++)
	{
		vsurf = (const void*)((const qbyte*)vbody + vbody->surf_ofs);
		for (s = 0; s < vbody->surf_count; s++, vsurf++)
		{
			vlod = (const void*)((const qbyte*)vsurf + vsurf->lod_ofs);
			for (l = 0; l < min(vsurf->lod_count, ctx->maxlod); l++, vlod++)	/*P201: was countof(ctx->lod), i.e. 1*/
				totalsurfs += vlod->mesh_count;
		}
	}

	if (!totalsurfs)
		return false;

	ctx->mod->meshinfo = surf = plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*surf)*totalsurfs);

	t = mdl->skin_count*mdl->texbind_count;
	skinbind = (const unsigned short*)((const qbyte*)mdl+mdl->texbind_offset);
	skin = plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*skin)*t + sizeof(*skinframe)*t);
	skinframe = (skinframe_t*)(skin+t);
	//FTESurf Patch 234: one byte per (skin, slot), so the second pass below knows which of
	//them found their material.  Freed at the end of this block; not in the model's memgroup
	//because it is scaffolding for the load and not part of the loaded model.
	skinok = plugfuncs->Malloc(t?t:1);
	for (s = 0; s < mdl->skin_count; s++)
	for (t = 0; t < mdl->texbind_count; t++)
	{
		galiasskin_t *ns = &skin[s + t*mdl->skin_count];
		Q_snprintfz(ns->name, sizeof(ns->name), "skin%u %u", (unsigned)s, (unsigned)t);

		m = *skinbind++;
		skinok[s + t*mdl->skin_count] = 0;
		if (mdl->texpath_count)
		{
			for (z = 0; z < mdl->texpath_count; z++) {
				char fsTest[MAX_QPATH];
				const hl2mdltexturepath_t *mpath = (const hl2mdltexturepath_t*)((const qbyte*)mdl + mdl->texpath_ofs + sizeof(hl2mdltexturepath_t) * z);
				Q_strlcpy(skinframe->shadername, (const char*)mdl+mpath->nameofs, sizeof(skinframe->shadername));
				Q_strlcat(skinframe->shadername, (const char*)&mtex[m]+mtex[m].nameofs, sizeof(skinframe->shadername));
				Q_strlcat(skinframe->shadername, ".vmt", sizeof(skinframe->shadername));
				Q_snprintfz(fsTest, sizeof(fsTest), "materials\\%s", skinframe->shadername);

				if (fsfuncs->LocateFile(fsTest, FSLF_IFFOUND, NULL)) {
					skinok[s + t*mdl->skin_count] = 1;	//FTESurf Patch 234
					break;
				}
			}
		}
		else
		{
			modfuncs->StripExtension((const char*)ctx->mod->name, skinframe->shadername, sizeof(skinframe->shadername));
			Q_strlcat(skinframe->shadername, "/", sizeof(skinframe->shadername));
			Q_strlcat(skinframe->shadername, (const char*)&mtex[m]+mtex[m].nameofs, sizeof(skinframe->shadername));
			Q_strlcat(skinframe->shadername, ".vmt", sizeof(skinframe->shadername));
			//no texture-path list to probe against, so there is nothing to be wrong about
			//and nothing to fall back to: treat it as settled.
			skinok[s + t*mdl->skin_count] = 1;
		}

		ns->numframes = 1;	//no skingroups... not that kind anyway.
		ns->skinspeed = 10;
		ns->frame = skinframe;
		skinframe++;
	}

	/*
	FTESurf Patch 234: A MODEL WITH FOUR SKINS AND ONE MISSING MATERIAL HAS THREE.

	The loop above walks EVERY skin family the MDL declares, whether or not anything in the
	map selects it, and when none of the texture-path candidates exists it leaves shadername
	pointing at the last path it tried.  That name then fails to register, and the map reports
	a missing material for a skin nothing is wearing.

	Measured on surf_demise, which is where this was reported:

	  tree_douglasfir01    skin 0 -> firbranch02_singlemat        785 props
	                       skin 1 -> firbranch02_singlemat_snowy    0 props   <- reported missing
	  models.elly.bones    skin 0 -> Bone_Charred  61   skin 1 -> Bone_Blight  34
	                       skin 2 -> Bone_Pearl     0   skin 3 -> Bone_Poison   7
	                                                ^-- reported missing

	2350 static props, skin histogram {0: 2309, 1: 34, 3: 7}; skin 2 does not occur once, and
	no prop_dynamic_override in the 1179-entity lump names either model.  The mapper packed the
	skins they used, which is not a fault.

	Adopting a sibling rather than filtering by what the map instantiates: the static-prop
	lump knows exactly which families are live, but a model's cache is shared across maps and
	a skin table is not, so that answer would have to be recomputed per map for a structure
	that is loaded once.  The sibling costs nothing and is also right for the case the filter
	does not cover -- a prop that DOES select an unshipped skin now draws as a plain fir
	branch instead of a grey default-wall slab.

	Slots are contiguous at skin[t*skin_count + s] (see surf->ofsskins below), so this reads
	only its own slot: a missing skin can never adopt a different material's texture.
	*/
	if (!hl2_skinfallback || hl2_skinfallback->ival)
	{
		for (t = 0; t < mdl->texbind_count; t++)
		{
			size_t good = 0;
			qboolean havegood = false;
			for (s = 0; s < mdl->skin_count; s++)
				if (skinok[s + t*mdl->skin_count])
				{	good = s;	havegood = true;	break;	}
			if (!havegood)
				continue;	//nothing resolved in this slot; leave every skin naming what the MDL asked for
			for (s = 0; s < mdl->skin_count; s++)
			{
				if (skinok[s + t*mdl->skin_count])
					continue;
				Con_DPrintf("%s: skin %u slot %u wants %s, which is installed nowhere; drawing skin %u's %s instead\n",
					ctx->mod->name, (unsigned)s, (unsigned)t,
					skin[s + t*mdl->skin_count].frame->shadername, (unsigned)good,
					skin[good + t*mdl->skin_count].frame->shadername);
				Q_strlcpy(skin[s + t*mdl->skin_count].frame->shadername,
					skin[good + t*mdl->skin_count].frame->shadername,
					sizeof(skin[0].frame->shadername));
			}
		}
	}
	plugfuncs->Free(skinok);

	//FTESurf Patch 259
	ctx->vhvmap = NULL;
	ctx->vhvcount = ctx->vhvmax = 0;
	ctx->vhvbad = false;

	vbody = (const void*)((const qbyte*)header + header->body_ofs);
	for (b = 0; b < header->body_count; b++, vbody++, mbody++)
	{
		const hl2mdlsurf_t *msurf = (const hl2mdlsurf_t*)((const qbyte*)mbody + mbody->surf_ofs);
		vsurf = (const void*)((const qbyte*)vbody + vbody->surf_ofs);
		for (s = 0; s < vbody->surf_count; s++, vsurf++, msurf++)
		{
			unsigned int nlod = min(vsurf->lod_count, ctx->maxlod);	/*P201*/
			if (nlod > ctx->usedlod)
				ctx->usedlod = nlod;
			vlod = (const void*)((const qbyte*)vsurf + vsurf->lod_ofs);
//			vskins = (const hl2vtxskins_t*)((const qbyte*)header + header->texreplacements_offset);
			for (l = 0, t = 0; l < nlod; l++, vlod++/*, vskins++*/)
			{
				const hl2mdlmesh_t *mmesh = (const hl2mdlmesh_t*)((const qbyte*)msurf + msurf->mesh_ofs);
				vmesh = (const void*)((const qbyte*)vlod + vlod->mesh_ofs);
				for (m = 0; m < vlod->mesh_count; m++, vmesh++, mmesh++)
				{
					Q_snprintfz(surf->surfacename, sizeof(surf->surfacename), "%s:%s:l%u:m%u", (const char*)mbody+mbody->name_ofs, msurf->name, (unsigned)l, (unsigned)m);

					//FTESurf Patch 259.  Here, not inside either index branch: the
					//table has to see every LOD0 mesh in this exact order, including
					//the one the malformed-range branch below abandons, or every
					//colour after it lands on the wrong vertex.
					if (l == 0)
						Mod_HL2_BuildVhvMap(ctx, vmesh, firstvert+mmesh->vert_first);

					/*animation info*/
					surf->numanimations = ctx->num_animations;
					surf->ofsanimations = ctx->ofs_animations;	//we have no animation data

					surf->baseframeofs = ctx->basepose;
					surf->ofsbones = ctx->bones;
					surf->numbones = mdl->num_bones;
					surf->shares_bones = 0;	//all the same id

					#ifndef SERVERONLY
					/*skin data*/
					surf->numskins = mdl->skin_count;
					surf->ofsskins = skin+mmesh->mat_idx*mdl->skin_count;

					/*vertdata*/
					surf->ofs_rgbaf = NULL;
					surf->ofs_rgbaub = NULL;
					surf->ofs_st_array = ctx->ofs_st_array;
					#endif
					surf->shares_verts = 0;
					surf->numverts = ctx->numverts;
					/*
					FTESurf Patch 259.  This surface's base offset into the model's
					GLOBAL vertex array -- the only thing the engine needs in order to
					hand a surface its slice of a per-instance per-vertex colour array
					(gl_alias.c, R_GAlias_DrawBatch).  The IQM loader has set it since
					the RGBPROPLIGHT work (com_mesh.c:10393); nothing here ever did, so
					every HL2 surface claimed base 0 and only a single-mesh model could
					ever have been coloured correctly.

					Zero here is not a placeholder: this branch hands the surface the
					WHOLE array (numverts = ctx->numverts) and lets the indexes address
					it, so base 0 is the true answer.  The sliced branch below overrides
					it with its own bias.

					The real base is written here and demoted to -1 after the loop if
					the model turns out not to be in VRAD's order -- the walk cannot
					answer that until it has seen every LOD0 mesh, and a surface
					written early would otherwise carry a verdict from before the
					evidence.
					*/
					surf->firstvert = 0;
					surf->ofs_skel_xyz = ctx->ofs_skel_xyz;
					surf->ofs_skel_norm = ctx->ofs_skel_norm;
					surf->ofs_skel_svect = ctx->ofs_skel_svect;
					surf->ofs_skel_tvect = ctx->ofs_skel_tvect;
					surf->ofs_skel_idx = ctx->ofs_skel_idx;
					surf->ofs_skel_weight = ctx->ofs_skel_weight;

					/*index data*/
					if (mmesh->vert_first+mmesh->vert_count > surf->numverts)
						surf->ofs_indexes = NULL, surf->numindexes = 0;	//erk?
					else if (!ctx->lod[l].numfixups /*&& mdl->num_bones > sh_config.max_gpu_bones*/)
					{	//FIXME: fixups make this too screwy, so we can't use this path when they're in use, which means we will probably exceed out gpu bones limit more than we otherwise would (just a perf issue)
						unsigned int bias = firstvert+mmesh->vert_first;
						surf->numverts = mmesh->vert_count;
						surf->firstvert = (int)bias;	//FTESurf Patch 259: the same bias every other array here takes
						surf->ofs_st_array += bias;
						surf->ofs_skel_xyz += bias;
						surf->ofs_skel_norm += bias;
						surf->ofs_skel_svect += bias;
						surf->ofs_skel_tvect += bias;
						//P230: NULL + bias is not NULL, and com_mesh keys the
						//rigid path on the pointer being NULL.  Guard, or a
						//$staticprop with more than one mesh would land back on
						//the skinning path with a pointer into nothing.
						if (surf->ofs_skel_idx)
							surf->ofs_skel_idx += bias;
						if (surf->ofs_skel_weight)
							surf->ofs_skel_weight += bias;
						surf->shares_verts = surf-(galiasinfo_t*)ctx->mod->meshinfo;

						surf->ofs_indexes = Mod_HL2_LoadIndexes(ctx, &surf->numindexes, vmesh, l, bias, bias);
					}
					else
						surf->ofs_indexes = Mod_HL2_LoadIndexes(ctx, &surf->numindexes, vmesh, l, firstvert+mmesh->vert_first, 0);

					/*misc data*/
					surf->geomset = 0;
					surf->geomid = 0;
					surf->contents = FTECONTENTS_BODY;
					surf->csurface.flags = 0;
					surf->surfaceid = b;
					/*
					P201: the LOD range, in the integer-index convention
					R_GAlias_GenerateBatches selects in and MD3's external
					_1/_2 LODs already use (com_mesh.c).  Level l is drawn
					while the selected index is in [l, l+1); the LAST level
					takes maxdist 0, which the filter reads as "no upper
					bound" so the coarsest mesh keeps drawing forever.
					*/
					surf->mindist = l;
					surf->maxdist = (l+1 < nlod) ? l+1 : 0;

					if (surf != ctx->mod->meshinfo)
						surf[-1].nextsurf = surf;
					surf->nextsurf = NULL;
					surf++;
				}
			}
			firstvert += msurf->vertex_count;
		}
	}

	if (surf == ctx->mod->meshinfo)
	{
		plugfuncs->Free(ctx->vhvmap);	//FTESurf Patch 259: no surfaces, so nothing to hang it on
		ctx->vhvmap = NULL;
		return false;
	}

	/*
	FTESurf Patch 259.  Hand the finished scatter table to the model, on the
	first surface, where mod_vbsp.c looks for it.

	A model that could not produce one marks every surface firstvert -1 --
	"no slice of an external per-vertex array addresses this" -- and its props
	fall back to the single baked mean.  Both readers test for -1 explicitly
	rather than letting it fall out of the arithmetic.
	*/
	if (ctx->vhvbad || !ctx->vhvcount)
	{
		galiasinfo_t *s2;
		for (s2 = (galiasinfo_t*)ctx->mod->meshinfo; s2; s2 = s2->nextsurf)
			s2->firstvert = -1;
	}
	else
	{
		galiasinfo_t *first = (galiasinfo_t*)ctx->mod->meshinfo;
		first->vhvmap = plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(int)*ctx->vhvcount);
		memcpy(first->vhvmap, ctx->vhvmap, sizeof(int)*ctx->vhvcount);
		first->vhvmapcount = (int)ctx->vhvcount;
		first->vhvmodelverts = (int)ctx->numverts;
	}
	plugfuncs->Free(ctx->vhvmap);
	ctx->vhvmap = NULL;

	return true;
}
void CrossProduct (const vec3_t v1, const vec3_t v2, vec3_t cross)
{
	cross[0] = v1[1]*v2[2] - v1[2]*v2[1];
	cross[1] = v1[2]*v2[0] - v1[0]*v2[2];
	cross[2] = v1[0]*v2[1] - v1[1]*v2[0];
}
static qboolean Mod_HL2_LoadVVD(hl2parsecontext_t *ctx, const void *buffer, size_t fsize)
{
	const hl2vvdheader_t *header = buffer;
	size_t lod;
	const hl2vvdvert_t *in;
	const vec4_t *it;
	if (fsize < sizeof(*header) || header->magic != (('I'<<0)|('D'<<8)|('S'<<16)|('V'<<24)) || header->version != 4 || header->revisionid != ctx->header->revisionid || header->lodverts_count[0] == 0)
		return false;

	{
		size_t v, numverts = ctx->numverts = header->lodverts_count[0];
		vec2_t *st = ctx->ofs_st_array		= plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*st)*numverts);
		vecV_t *xyz = ctx->ofs_skel_xyz		= plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*xyz)*numverts);
		vec3_t *norm = ctx->ofs_skel_norm	= plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*norm)*numverts);
		vec3_t *sdir = ctx->ofs_skel_svect	= plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*sdir)*numverts);
		vec3_t *tdir = ctx->ofs_skel_tvect	= plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*tdir)*numverts);
		//FTESurf Patch 230: a $staticprop gets no bone attributes at all -- not
		//NULLed after the fact, never allocated.  20 bytes per vertex, and the
		//two `if (galias->ofs_skel_*)` guards in Mod_BuildVBOs then skip the
		//matching VBO uploads too.
		byte_vec4_t *bone = ctx->ofs_skel_idx = ctx->rigid ? NULL : plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*bone)*numverts);
		vec4_t *weight = ctx->ofs_skel_weight = ctx->rigid ? NULL : plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*weight)*numverts);

		in = (const void*)((const char*)buffer+header->verts_offset);
		it = (const void*)((const char*)buffer+header->tangents_offset);
		for(v = 0; v < numverts; v++, in++, it++)
		{
			Vector2Copy(in->st, st[v]);
			VectorCopy(in->xyz, xyz[v]);
			VectorCopy(in->norm, norm[v]);

			if (!ctx->rigid)
			{
				VectorCopy(in->bone, bone[v]);
				bone[v][3] = bone[v][2];	//make sure its valid, and in cache.
				VectorCopy(in->weight, weight[v]);
				weight[v][3] = 0;			//missing influences cannot influence.
			}

			//tangents are compacted, and for some reason in a different part of the file.
			VectorCopy((*it), sdir[v]);
			CrossProduct(in->norm, (*it), tdir[v]);
			VectorScale(tdir[v], (*it)[3], tdir[v]);
		}
	}

	/*
	P201: decide how many LOD levels this model may use, BEFORE Mod_HL2_LoadVTX
	runs (it is called immediately after us, so the answer is ready in time).

	Fixups mean compacted per-LOD vertex arrays, and the index arithmetic in
	Mod_HL2_LoadVTX is LOD-0-relative -- see the essay at the top of the file.
	No fixups means no compaction happened: every level indexes the one full
	vertex array, and Mod_HL2_LoadIndexes' `else` branch is already correct for
	all of them.

	What the counts look like when that is the case, measured on surf_demise's
	forest (12 models, 4 levels, no fixups):

	    tree_douglasfir01d_opt_large   lodverts = 2787, 2312, 1675, 926
	    tree_douglasfir01f_opt_large   lodverts = 3268, 2721, 1949, 1063

	-- monotonically DECREASING, not equal.  studiomdl orders a model's vertices
	so that each level uses a PREFIX of the array, which is exactly why it had no
	fixups to write.  So the test is the prefix invariant (nonzero, and no larger
	than level 0), not equality.  An earlier draft of this patch demanded equality
	and admitted precisely zero models in the whole library, which is how the
	prefix arrangement got noticed at all.
	*/
	ctx->maxlod = 1;
	if (!header->fixups_count)
	{
		size_t n = header->lod_count;
		if (n > countof(ctx->lod))
			n = countof(ctx->lod);
		for (lod = 1; lod < n; lod++)
			if (!header->lodverts_count[lod] || header->lodverts_count[lod] > header->lodverts_count[0])
				break;
		ctx->maxlod = lod;
	}

	if (header->fixups_count)
	{
		size_t fixups = header->fixups_count, f, v;
		const hl2vvdfixup_t *fixup = (const hl2vvdfixup_t*)((const qbyte*)header+header->fixups_offset);
		/*bounded by maxlod, not by countof: with fixups present maxlod is 1, and
		  building the other seven tables would allocate index arrays nothing reads.*/
		for (lod = 0; lod < ctx->maxlod && lod < header->lod_count; lod++)
		{
			size_t numverts;
			for (numverts=0, f = 0; f < fixups; f++)
			{
				if (fixup[f].lod >= lod)
					numverts += fixup[f].numverts;
			}
			if (numverts != header->lodverts_count[lod])
				continue;
			ctx->lod[lod].numfixups = numverts;
			ctx->lod[lod].fixup = plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(index_t)*numverts);
			for (numverts=0, f = 0; f < fixups; f++)
			{
				if (fixup[f].lod >= lod)
					for (v = 0; v < fixup[f].numverts; v++)
						ctx->lod[lod].fixup[numverts++] = fixup[f].sourcevert+v;
			}
		}
	}
	return true;
}

static void Angle2Quaternion(const vec3_t angles, vec4_t quaternion)
{
	float	yaw = angles[2] * 0.5;
	float	pitch = angles[1] * 0.5;
	float	roll = angles[0] * 0.5;
	float	siny = sin(yaw);
	float	cosy = cos(yaw);
	float	sinp = sin(pitch);
	float	cosp = cos(pitch);
	float	sinr = sin(roll);
	float	cosr = cos(roll);

	quaternion[0] = sinr * cosp * cosy - cosr * sinp * siny;
	quaternion[1] = cosr * sinp * cosy + sinr * cosp * siny;
	quaternion[2] = cosr * cosp * siny - sinr * sinp * cosy;
	quaternion[3] = cosr * cosp * cosy + sinr * sinp * siny;
}
static signed short Mod_HL2_ReadAnimValue(const void *baseptr, signed short offset, int posenum)
{
	const hlmdl_animvalue_t	*animvalue = (const hlmdl_animvalue_t *) ((const qbyte *) baseptr + offset);
	if (!offset)
		return 0;	//nope, axis not present.
	/* find values including the required frame */
	while(animvalue->num.total <= posenum)
	{
		posenum -= animvalue->num.total;
		animvalue += animvalue->num.valid + 1;
	}
	if (posenum >= animvalue->num.valid)
		posenum = animvalue->num.valid;
	else
		posenum += 1;
	return animvalue[posenum].value;
}
static const void *Mod_HL2_GetExternalBlock(hl2parsecontext_t *ctx, const void *base, int blockidx, int offset)
{
	if (!blockidx)
	{	//data is relative to the parent structure...
		return (const qbyte*)base + offset;
	}
	else
	{	//data is elsewhere...
		const struct {
			int start;
			int end;
		} *block = (const void*)((const qbyte*)ctx->header + ctx->header->ofs_aniblocks);
		block += blockidx;

		if (!ctx->ani)
		{
			vfsfile_t *f = filefuncs->OpenVFS((const char *)ctx->header + ctx->header->ofs_aniname, "rb", FS_GAME);
			if (f)
			{
				size_t sz = f->GetLen(f);
				ctx->ani = plugfuncs->GMalloc(&ctx->mod->memgroup, sz);
				f->ReadBytes(f, ctx->ani, sz);
				f->Close(f);
			}
		}
		if (ctx->ani)
			return (const qbyte*)ctx->ani + block->start + offset;
	}
	return NULL;
}

static float HalfToFloat(unsigned short val)
{	//hl2 supported shitty low-ram consoles. yay for data compression?
	union
	{
		float f;
		unsigned int u;
	} u;
	if (val&0x7c00)
		u.u = (((val&0x7c00)>>10)-15+127)<<23;	//read exponent, rebias it, and reshift.
	else
		u.u = 0;	//denormal (or 0).
	u.u |= ((val & 0x3ff)<<13);//shift up the mantissa, but don't fold
	u.u |= (val&0x8000)<<16;	//retain the sign bit.
	return u.f;
}
static void Mod_HL2_ReadPose(hl2parsecontext_t *ctx, const hl2mdlanim_t *anim, int posenum, matrix3x4 *out, unsigned int numbones, const hl2mdlbone_t *boneinfo)
{	//hl2 models seem to have both animations and sequences
	vec3_t org;
	vec4_t quat;
	static vec3_t scale={1,1,1};
	const unsigned short *data;
	const hl2mdlpose_t *pose;
	struct poseofs_s poseofs = anim->defpose;
	int sect = 0;
	if (anim->posespersection)
	{	//can be stored in groups, so we don't have to walk as many rle entries.
		sect = posenum/anim->posespersection;
		poseofs = ((const struct poseofs_s*)((const qbyte*)anim + anim->ofs_posesections))[sect];
		posenum -= sect * anim->posespersection;
	}

	memcpy(out, ctx->baserelpose, sizeof(*out)*numbones);
	pose = Mod_HL2_GetExternalBlock(ctx, anim, poseofs.externalblock, poseofs.ofs_pose);
	if (!pose || pose->bone==255)
		return;
	for (;;)
	{
		data = pose->data;

		if (pose->flags & 0x2)
		{	//static data shared between all frames in this cluster
			quat[0] = ((int)(data[0]       )-0x8000) / (float)0x8000;
			quat[1] = ((int)(data[1]       )-0x8000) / (float)0x8000;
			quat[2] = ((int)(data[2]&0x7fff)-0x4000) / (float)0x4000;
			quat[3] = 1 - DotProduct(quat, quat);
			quat[3] = sqrt(quat[3]);
			if (data[2]&0x8000)
				quat[3] *= -1;
			data+=3;
		}
		else if (pose->flags & 0x20)
		{	//apparently these are 21+21+21+1 bits each
			quint64_t q64 = *(const quint64_t*)data;
			quat[0] = (((int)(q64>> 0)&((1<<21)-1))-(1<<20)) / (float)(1<<20);
			quat[1] = (((int)(q64>>21)&((1<<21)-1))-(1<<20)) / (float)(1<<20);
			quat[2] = (((int)(q64>>42)&((1<<21)-1))-(1<<20)) / (float)(1<<20);
			quat[3] = 1 - DotProduct(quat, quat);
			quat[3] = sqrt(quat[3]);
			if (q64&(((quint64_t)1)<<63))
				quat[3] *= -1;
			data+=sizeof(q64)/sizeof(*data);
		}
		else if (pose->flags & 0x8)
		{	//animated version (using some sort of RLE)
			vec3_t ang;
			if (pose->flags & 0x10)
				VectorClear(ang);
			else
				VectorCopy(boneinfo[pose->bone].rot, ang);
			ang[0] += Mod_HL2_ReadAnimValue(data, data[0], posenum) * boneinfo[pose->bone].rotscale[0];
			ang[1] += Mod_HL2_ReadAnimValue(data, data[1], posenum) * boneinfo[pose->bone].rotscale[1];
			ang[2] += Mod_HL2_ReadAnimValue(data, data[2], posenum) * boneinfo[pose->bone].rotscale[2];
			data+=3;
			Angle2Quaternion(ang, quat);
		}
		else if (pose->flags & 0x10)
			Vector4Set(quat, 0, 0, 0, 1);
		else
			VectorCopy(boneinfo[pose->bone].quat, quat);

		if (pose->flags & 1)
		{	//static data
			org[0] = HalfToFloat(data[0]);
			org[1] = HalfToFloat(data[1]);
			org[2] = HalfToFloat(data[2]);
			data+=3;
		}
		else
		{
			if (pose->flags & 0x10)
				VectorClear(org);
			else
				VectorCopy(boneinfo[pose->bone].pos, org);
			if (pose->flags & 0x4)
			{	//animated version (using some sort of RLE)
				org[0] += Mod_HL2_ReadAnimValue(data, data[0], posenum) * boneinfo[pose->bone].posscale[0];
				org[1] += Mod_HL2_ReadAnimValue(data, data[1], posenum) * boneinfo[pose->bone].posscale[1];
				org[2] += Mod_HL2_ReadAnimValue(data, data[2], posenum) * boneinfo[pose->bone].posscale[2];
				data+=3;
			}
		}

		//FIXME: bone controllers

		modfuncs->GenMatrixPosQuat4Scale(org, quat, scale, (float*)(out + pose->bone));
		if (!pose->next)
			break;
		pose = (const hl2mdlpose_t*)((const qbyte*)pose + pose->next);
	}
}


// matches engine Mod_InsertEvent from parses a foo.mdl.events file and inserts the events into the relevant animations
static void Mod_InsertEvent(zonegroup_t *mem, galiasanimation_t *anims, unsigned int numanimations, unsigned int eventanimation, float eventpose, int eventcode, const char *eventdata)
{
	galiasevent_t *ev, **link;
	if (eventanimation >= numanimations)
	{
		Con_Printf("Mod_InsertEvent: invalid frame index\n");
		return;
	}
	ev = plugfuncs->GMalloc(mem, sizeof(*ev) + strlen(eventdata)+1);
	ev->data = (char*)(ev+1);

	ev->timestamp = eventpose;
	ev->timestamp /= anims[eventanimation].rate;
	ev->code = eventcode;
	strcpy(ev->data, eventdata);
	link = &anims[eventanimation].events;
	while (*link && (*link)->timestamp <= ev->timestamp)
		link = &(*link)->next;
	ev->next = *link;
	*link = ev;
}

// matches engine Mod_ParseModelEvents
static qboolean Mod_ParseModelEvents(model_t *mod, galiasanimation_t *anims, unsigned int numanimations)
{
	unsigned int anim;
	float pose;
	int eventcode;

	const char *modelname = mod->name;
	zonegroup_t *mem = &mod->memgroup;
	char fname[MAX_QPATH], tok[2048];
	size_t fsize;
	char *line, *file, *eol;
	Q_snprintfz(fname, sizeof(fname), "%s.events", modelname);
	line = file = filefuncs->LoadFile(fname, &fsize);
	if (!file)
		return false;
	while(line && *line)
	{
		eol = strchr(line, '\n');
		if (eol)
			*eol = 0;

		line = cmdfuncs->ParseToken(line, tok, sizeof(tok), 0);
		anim = strtoul(tok, NULL, 0);
		line = cmdfuncs->ParseToken(line, tok, sizeof(tok), 0);
		pose = strtod(tok, NULL);
		line = cmdfuncs->ParseToken(line, tok, sizeof(tok), 0);
		eventcode = (long)strtol(tok, NULL, 0);
		line = cmdfuncs->ParseToken(line, tok, sizeof(tok), 0);
		Mod_InsertEvent(mem, anims, numanimations, anim, pose, eventcode, tok);

		if (eol)
			line = eol+1;
		else
			break;
	}
	plugfuncs->Free(file);
	return true;
}


qboolean QDECL Mod_LoadHL2Model (model_t *mod, void *buffer, size_t fsize)
{
	hl2parsecontext_t ctx = {mod, buffer};
	void *vtx = NULL, *vvd = NULL;
	size_t vtxsize = 0, vvdsize = 0;
	char base[MAX_QPATH];
	qboolean result;
	size_t i, p;
	const char *vtxpostfixes[] = {
		".dx90.vtx",
		".dx80.vtx",
		".vtx",
		".sw.vtx"
	};
	const char *vvdpostfixes[] = {
		".vvd"
	};
	galiasinfo_t *galias;

	for (i = 0; !vtx && i < countof(vtxpostfixes); i++)
	{
		modfuncs->StripExtension(mod->name, base, sizeof(base));
		Q_strncatz(base, vtxpostfixes[i], sizeof(base));
		vtx = filefuncs->LoadFile(base, &vtxsize);
	}
	for (i = 0; !vvd && i < countof(vvdpostfixes); i++)
	{
		modfuncs->StripExtension(mod->name, base, sizeof(base));
		Q_strncatz(base, vvdpostfixes[i], sizeof(base));
		vvd = filefuncs->LoadFile(base, &vvdsize);
	}

	if (ctx.header->num_bones)
	{
		const hl2mdlbone_t *in = (const hl2mdlbone_t*)((const qbyte*)ctx.header + ctx.header->ofs_bones);
		vec3_t scale = {1,1,1};
		ctx.basepose = plugfuncs->GMalloc(&mod->memgroup, sizeof(float)*12*ctx.header->num_bones);
		ctx.baserelpose = alloca(sizeof(float)*12*ctx.header->num_bones);
		ctx.bones = plugfuncs->GMalloc(&mod->memgroup, sizeof(*ctx.bones)*ctx.header->num_bones);

		for (i = 0; i < ctx.header->num_bones; i++)
		{
			float *pose = ctx.baserelpose + 12*i;
			Q_strlcpy(ctx.bones[i].name, (const char*)&in[i]+in[i].name_ofs, sizeof(ctx.bones[i].name));
			ctx.bones[i].parent = in[i].parent;
			modfuncs->GenMatrixPosQuat4Scale(in[i].pos, in[i].quat, scale, pose);
			if (in[i].parent>=0)
				modfuncs->ConcatTransforms((const void*)(ctx.basepose+in[i].parent*12), (const void*)pose, (void*)(ctx.basepose+i*12));
			else
				memcpy(ctx.basepose+12*i, pose, sizeof(float)*12);
			memcpy(ctx.bones[i].inverse, in[i].inverse, sizeof(float)*12);	//use the provided value, but should match modfuncs->M3x4_Invert(ctx.basepose+12*i, ctx.bones[i].inverse);
		}

		if (ctx.header->num_anims)
		{
			galiasanimation_t *a;
			const hl2mdlanim_t *in = (const hl2mdlanim_t*)((const qbyte*)ctx.header + ctx.header->ofs_anims);
			a = ctx.ofs_animations = plugfuncs->GMalloc(&mod->memgroup, sizeof(*ctx.ofs_animations)*ctx.header->num_anims);
			//note that hl2 models have anims and sequences... I'm guessing I ought to be exposing sequences instead of anims.
			for (i = 0; i < ctx.header->num_anims; i++, in++)
			{
				a->skeltype = SKEL_RELATIVE;
				a->numposes = in->numframes;

				a->GetRawBones = NULL;	//FIXME: for delay loading the proper way...
										//FIXME: replace Alias_FindRawSkelData instead, to handle bone controllers etc.
				a->boneofs = plugfuncs->GMalloc(&mod->memgroup, sizeof(float)*a->numposes*12*ctx.header->num_bones);
				a->loop = in->loop;
				a->rate = in->fps;
				a->action = -1;
				a->actionweight = 0;
				a->events = NULL;
				Q_strlcpy(a->name, (const char*)in + in->name_ofs, sizeof(a->name));

				for (p = 0; p < a->numposes; p++)
					Mod_HL2_ReadPose(&ctx, in, p, (matrix3x4*)a->boneofs+p*ctx.header->num_bones, ctx.header->num_bones, (const hl2mdlbone_t*)((const qbyte*)ctx.header + ctx.header->ofs_bones));
				a++;
			}
			ctx.num_animations = a-ctx.ofs_animations;
		}
	}


	/*
	FTESurf Patch 230.  A STATIC PROP IS NOT A SKELETAL MODEL, SO STOP SAYING IT IS.

	Every Source model loaded here comes out of Mod_HL2_LoadVVD with both
	ofs_skel_xyz AND ofs_skel_weight set, because that is what the .vvd carries.
	com_mesh.c:2100 keys its cheapest path on exactly the opposite:

	    if (inf->ofs_skel_xyz && !inf->ofs_skel_weight)   // rigid: static VBO, no bones

	so no Source model has ever taken it.  Every one of them falls into the
	`numbones` branch instead, and what happens there depends on a fact about the
	MATERIAL rather than the model -- gl_alias.c:1954 passes

	    usebones = shader->prog->supportedpermutations & PERMUTATION_SKELETAL

	and of the vmt shaders only vertexlit.glsl declares `!!permu SKELETAL`, while
	mat_vmt.c:1602 makes vmt/unlit the fallback for anything unrecognised.  So a
	prop surface whose material is UnlitGeneric gets usebones == false, which
	means Alias_BuildSkeletalMesh re-skins it on the CPU EVERY FRAME and leaves
	*vbop NULL (com_mesh.c:2099) so the vertices are re-uploaded every frame too.

	MEASURED, from the .mdl headers of every prop model on three maps -- offline,
	no engine involved:

	    surf_demise    73 of 73 models   1 bone, $staticprop, no animations
	    surf_garden    54 of 54 models   1 bone, $staticprop, no animations
	    ahop_coast    220 of 238 models  1 bone, $staticprop, no animations
	                                     (the other 18 are the ragdolls/NPCs,
	                                      up to 56 bones -- untouched by this)

	and on surf_demise 42 of 211 material slots declare UnlitGeneric, so roughly a
	fifth of the prop surfaces on that map are being software-skinned against a
	one-bone identity rig, every frame, forever.

	None of these models can ever be posed, so the skinning is a no-op in the
	first place: FTE composes SKEL_INVERSE_ABSOLUTE matrices (absolute * inverse
	bind-absolute), which is the identity in the bind pose, and a $staticprop has
	no other pose to be in.  Dropping the weights is therefore not an
	approximation -- it is deleting an identity transform.

	So: don't emit skinning data for a model that cannot be skinned.  With
	ofs_skel_idx and ofs_skel_weight NULL, com_mesh takes the rigid branch, the
	vertices live in a static VBO, and Mod_BuildVBOs skips both bone attribute
	uploads (com_mesh.c:4694-4700 guard on exactly these pointers, so this also
	returns 20 bytes per vertex of client memory and VRAM).  It also stops
	gl_backend.c:4365 setting PERMUTATION_SKELETAL -- that is keyed on
	sourcevbo->numbones, which the rigid branch zeroes -- so vertexlit props pick
	up the cheaper non-skeletal vertex shader as well.

	THE GATE IS STUDIOMDL'S OWN ASSERTION, not our inference.  num_bones <= 1 is
	necessary but not sufficient: a one-bone model with a sequence that moves that
	bone is a legal, animatable model (a swinging sign), and stripping its weights
	would freeze it in the bind pose.  $staticprop is the flag studiomdl sets when
	it has collapsed the rig and Source itself will refuse to animate it.  Both
	are required.

	hl2_rigidprops exists so this can be turned off without a rebuild -- it is the
	control half of the A/B, and it is MAPLATCH because it changes how models are
	parsed and the models are already in memory.
	*/
	ctx.rigid = (ctx.header->flags & HL2MDL_STATIC_PROP) && ctx.header->num_bones <= 1
			&& (!hl2_rigidprops || hl2_rigidprops->ival);

	//Say which path each model took.  Without this the A/B is an fps number with
	//no evidence attached: `developer 1` then counts the two kinds directly out
	//of the log, and a model that was expected to be rigid and is not says so.
	Con_DPrintf("MDL %s: %s (flags %#x, %u bone%s)\n", mod->name,
			ctx.rigid?"RIGID":"skeletal", ctx.header->flags,
			ctx.header->num_bones, ctx.header->num_bones==1?"":"s");

	result  = Mod_HL2_LoadVVD(&ctx, vvd, vvdsize);
	result &= Mod_HL2_LoadVTX(&ctx, vtx, vtxsize);
	plugfuncs->Free(vvd);
	plugfuncs->Free(vtx);

	if (ctx.header->num_bones && !mod->meshinfo)
	{	//it has a rig... make sure we spit out a surface so we can read the bones.
		galiasinfo_t *surf = mod->meshinfo = plugfuncs->GMalloc(&mod->memgroup, sizeof(galiasinfo_t));
		surf->numbones = ctx.header->num_bones;
		surf->baseframeofs = ctx.basepose;
		surf->ofsbones = ctx.bones;

		surf->numanimations = ctx.num_animations;
		surf->ofsanimations = ctx.ofs_animations;
		result = true;
	}

	VectorCopy(ctx.header->mins, mod->mins);
	VectorCopy(ctx.header->maxs, mod->maxs);
	galias = (galiasinfo_t*)mod->meshinfo;
	Mod_ParseModelEvents(mod, galias->ofsanimations, galias->numanimations);

	mod->type = mod_alias;
	mod->radius = RadiusFromBounds(mod->mins, mod->maxs);

	/*
	P201: this one assignment is what arms the whole thing.  gl_alias.c takes
	`lod = 0` for any model whose maxlod is 0 -- which was every Source model
	ever loaded -- and only computes a level when it is set.  Left at 0 for a
	single-level model, so nothing without real LODs pays the coverage maths.
	*/
	if (ctx.usedlod > 1)
		mod->maxlod = ctx.usedlod;

	/*
	  FTESurf Build 8.  THE COLLISION TREE IS NOT BUILT FROM THE RENDER MESH
	  ANY MORE.

	  BIH_BuildAlias turns whatever galiasinfo_t chain it is handed into this
	  model's NativeTrace, and until now that was mod->meshinfo -- the geometry
	  you can see.  Source collides against the model's .phy VPhysics hull
	  instead, which is a far simpler shape: a fence is two boxes there and
	  several hundred triangles here, so every rail and wire was solid.

	  Mod_PHY_CollisionMesh returns NULL for "no usable hull", and NULL is not a
	  failure -- it is the build-7 behaviour, and the fallback is deliberate and
	  common (2,922 of the 3,084 .phy files in the mounted CS:S and HL2 archives
	  are usable; the rest are jointed bodies whose solids are bone-relative).

	  Note that the RENDER mesh is untouched either way: only mod->funcs.Native-
	  Trace changes.  And hl2_propcollision is RENDERERLATCH, so a change to it
	  needs a map reload -- models are cached, and a cached model keeps the tree
	  it was built with.
	*/
	{
		galiasinfo_t *collision = NULL;
		int mode = hl2_propcollision ? hl2_propcollision->ival : 1;

		if (mode < 0 || mode > 3)
			mode = 1;
		if (mode != 0)		//0 is mod_vbsp's business (no prop leaves at all)
			collision = Mod_PHY_CollisionMesh(mod, mode, filefuncs, modfuncs,
			                                  FTECONTENTS_BODY);

		modfuncs->BIH_BuildAlias(mod, collision ? collision : mod->meshinfo);
	}
	return result;
}

qboolean MDL_Init(void)
{
	filefuncs = plugfuncs->GetEngineInterface(plugfsfuncs_name, sizeof(*filefuncs));
	modfuncs = plugfuncs->GetEngineInterface(plugmodfuncs_name, sizeof(*modfuncs));
	cmdfuncs = plugfuncs->GetEngineInterface(plugcmdfuncs_name, sizeof(*cmdfuncs));
	fsfuncs = plugfuncs->GetEngineInterface(plugfsfuncs_name, sizeof(*fsfuncs));

	if (modfuncs && modfuncs->version != MODPLUGFUNCS_VERSION)
		modfuncs = NULL;

	if (modfuncs && filefuncs)
	{
		modfuncs->RegisterModelFormatMagic("Source model (v44)", "IDST\x2c\0\0\0",8, Mod_LoadHL2Model);
		modfuncs->RegisterModelFormatMagic("Source model (v45)", "IDST\x2d\0\0\0",8, Mod_LoadHL2Model);
		modfuncs->RegisterModelFormatMagic("Source model (v46)", "IDST\x2e\0\0\0",8, Mod_LoadHL2Model);
		modfuncs->RegisterModelFormatMagic("Source model (v47)", "IDST\x2f\0\0\0",8, Mod_LoadHL2Model);
		modfuncs->RegisterModelFormatMagic("Source model (v48)", "IDST\x30\0\0\0",8, Mod_LoadHL2Model);
		modfuncs->RegisterModelFormatMagic("Source model (v49)", "IDST\x31\0\0\0",8, Mod_LoadHL2Model);
		return true;
	}
	return false;
}
