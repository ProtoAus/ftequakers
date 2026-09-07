/*
===========================================================================
  mod_phy.c -- Source .phy VPhysics collision hulls.          FTESurf Build 8

  WHAT THIS IS FOR.

  Until now every Source model in this plugin collided against its VISIBLE
  MESH.  Mod_LoadHL2Model finished by handing mod->meshinfo -- the render
  geometry -- straight to BIH_BuildAlias, and mod_vbsp.c put a BIH_MODEL leaf
  per solid static prop into the world tree pointing at that same model.  The
  file's own header comment admitted it:

      "Static Props - these are solid, but use the visible mesh for collisions
       instead of the special/simpler collision mesh."

  Source does not do that.  Every solid model ships a sibling `.phy` holding a
  VPhysics hull, and that hull is what the player collides with.  The two are
  not the same shape and are not meant to be: props_c17/fence01a is 12
  collision triangles (two boxes) against a render mesh with every rail, post
  and wire in it.  Colliding against the render mesh makes the game STRICTER
  than Source everywhere a prop has foliage, a railing, a grate or a skirt --
  which is exactly the shape of "an invisible block stopped me mid-surf".

  WHAT WAS MEASURED BEFORE ANY OF THIS WAS WRITTEN.

  Every .phy in the mounted CS:S and Half-Life 2 archives was extracted and run
  through this algorithm offline: 3,084 files, 3,081 of them valid, ZERO parse
  failures.  Of those, 2,922 pass the acceptance rules below, and against the
  MDL's own hull bounds the assembled hull's worst overshoot is
      median -0.28   p99 -0.25   (units; negative == INSIDE the render bounds)
  which is what a simplified collision hull should look like.  Only 16 of 2,922
  stick out by more than 4 units and every one of those is a jointed model whose
  solid is expressed in a bone's space -- see THE BOUNDS CHECK below, which is
  what catches them.

  Coordinate convention: IVP works in metres and in its own axis order.  The
  mapping was NOT taken from memory -- all 48 signed axis permutations were
  fitted against the MDL hull bounds of every sample, and one wins by a factor
  of twelve over the next distinguishable candidate:

      HL = ( ivp.x, ivp.z, -ivp.y ) * (1 / 0.0254)

===========================================================================
*/

#include "../plugin.h"
#include "../engine/common/com_mesh.h"

/* IVP is metric; Source is inches.  0.0254 m per inch. */
#define PHY_IVP2HL          (1.0f / 0.0254f)

/* sizeof(IVP_Compact_Ledgetree_Node): two offsets, a sphere, three box bytes
   and a pad.  Getting this wrong walks the tree into hyperspace, so it is
   spelled out rather than sizeof()'d off a struct the compiler may pad. */
#define PHY_NODE_SIZE       28

/* sizeof(IVP_Compact_Triangle): a packed index word plus three 4-byte edges. */
#define PHY_TRI_SIZE        16

/* sizeof(IVP_U_Float_Point4): xyz plus an unused w. */
#define PHY_POINT_SIZE      16

/* The only compact-surface version this parser has ever been checked against.
   Every one of the 3,081 valid files in CS:S and HL2 is 256.  257 exists in
   Strata Source content (two weapon models in Momentum's own tree) and moves
   at least one field; it is REFUSED rather than guessed at, because a hull
   parsed at the wrong offset is not a failure you can see -- it is a plausible
   solid in the wrong place. */
#define PHY_VERSION_IVPS    256

/* modelType 0 is IVP_COMPACT_SURFACE.  Anything else (mopp) is a different
   structure entirely. */
#define PHY_MODELTYPE_IVPS  0

/* Sanity caps.  These are diagnostics, not limits: nothing legitimate comes
   close, so tripping one means the file is not what we think it is. */
#define PHY_MAX_SOLIDS      256
#define PHY_MAX_LEDGES      65536
#define PHY_MAX_TRIS        1048576

/* Depth-first walk stack.  Sized generously rather than tightly: a balanced
   tree over 65536 leaves is 16 deep, and this bounds the DEGENERATE case, which
   is the one a malformed file produces.  Heap-allocated, not static -- models
   load on worker threads (VBSP_BuildBIHMain says so), so anything file-scope
   and mutable here would be two props racing over one buffer. */
#define PHY_MAX_DEPTH       1024

/*
  THE BOUNDS CHECK, and why it is not optional.

  A .phy solid belongs to a rigid body.  For a static prop that body is the
  whole model and the hull is in model space -- which is what the 2,922-sample
  measurement above confirms.  For a JOINTED model (a ragdoll, an animated
  door, a gib) each solid is expressed in its bone's space, and merging those
  without the bind-pose transform puts the hull somewhere the model is not.

  Two guards, because one is not enough:

    - solidCount must be 1.  A multi-solid .phy IS a jointed body by
      definition; 162 of the 3,084 samples are, and every one of the worst
      offenders was among them.
    - and even then, the assembled hull must sit inside the model's own bounds
      plus this slack.  That catches the single-solid animated props (doors,
      an APC, a ladder set) whose one solid is still bone-relative.

  8 units is comfortably above the p99 residual of -0.25 and rejects all 16
  outliers.  A rejection costs nothing: the caller falls back to the render
  mesh, which is exactly the behaviour that shipped in build 7.
*/
#define PHY_BOUNDS_SLACK    8.0f

/*
  Census, so "is the .phy actually being used" is a number rather than a hope.
  Counted across every model loaded since the last map change and printed by
  mod_vbsp's static-prop report.  Written from model-loading WORKER THREADS, so
  these are deliberately plain counters whose exact totals do not matter --
  a lost increment costs a diagnostic, never correctness.
*/
volatile int phy_stat_hull;      /* used the .phy hull */
volatile int phy_stat_nofile;    /* no .phy shipped -- render mesh */
volatile int phy_stat_jointed;   /* multi-solid -- a SUBSET of phy_stat_rejected */
volatile int phy_stat_rejected;  /* had a .phy, did not use it (any reason) */
volatile int phy_stat_bbox;      /* hl2_propcollision 2 */
volatile int phy_stat_tris;      /* total collision triangles from hulls */

void Mod_PHY_ResetStats (void)
{
	phy_stat_hull = phy_stat_nofile = phy_stat_jointed = 0;
	phy_stat_rejected = phy_stat_bbox = phy_stat_tris = 0;
}

typedef struct
{
	int size;           /* sizeof(phyheader_t), == 16 */
	int id;
	int solidCount;
	int checksum;
} phyheader_t;

/* Offsets INSIDE one solid, measured from the solid's leading size field.
   size(4) + "VPHY"(4) + version(2) + modelType(2) + surfaceSize(4)
   + dragAxisAreas(12) + axisMapSize(4)  ==  32, where the compact surface
   (IVP's legacysurfaceheader_t) begins. */
#define PHY_SOLID_MAGIC_OFS      4
#define PHY_SOLID_VERSION_OFS    8
#define PHY_SOLID_MODELTYPE_OFS  10
#define PHY_SOLID_SURFACE_OFS    32

/* ...and offsets inside the compact surface itself.
   mass_center(12) rotation_inertia(12) upper_limit_radius(4)
   max_deviation:8|byte_size:24 (4) offset_ledgetree_root(4) dummy[3](12) */
#define PHY_SURF_ROOT_OFS        32
#define PHY_SURF_IVPS_OFS        44

/* compactledge_t: c_point_offset(4) ledgetree_node_offset(4) flags(4)
   n_triangles(2) pad(2) */
#define PHY_LEDGE_HEADER_SIZE    16

typedef struct
{
	const qbyte     *base;      /* the whole file */
	size_t           filelen;

	/* accumulated across every accepted ledge */
	vecV_t          *xyz;
	index_t         *idx;
	size_t           numverts, maxverts;
	size_t           numidx,   maxidx;
} phyctx_t;

static unsigned int PHY_ReadU32 (const qbyte *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1]<<8) | ((unsigned int)p[2]<<16) | ((unsigned int)p[3]<<24);
}
static int PHY_ReadI32 (const qbyte *p)
{
	return (int)PHY_ReadU32(p);
}
static short PHY_ReadI16 (const qbyte *p)
{
	return (short)((unsigned short)p[0] | ((unsigned short)p[1]<<8));
}
static float PHY_ReadF32 (const qbyte *p)
{
	union { unsigned int u; float f; } c;
	c.u = PHY_ReadU32(p);
	return c.f;
}

/*
  Collect the LEAF ledges of one solid's ledge tree.

  Iterative rather than recursive on purpose -- this parses untrusted file data
  and a malformed offset is the normal failure mode, not the exceptional one.
  A recursive walk on a cyclic tree blows the stack; this one just runs out of
  slots and returns false.

  And it walks the TREE rather than the ledges linearly.  A linear walk by
  size_div_16 also picks up the interior "virtual" ledges the tree builds over
  the real ones, which silently doubles the geometry and puts phantom surfaces
  through the middle of the hull.
*/
typedef struct { size_t ofs; int depth; } phynodestack_t;

static qboolean PHY_CollectLedges (phyctx_t *ctx, size_t surf, size_t end,
                                   size_t root, size_t *ledges, size_t *numledges,
                                   phynodestack_t *stack)
{
	int sp = 0;

	*numledges = 0;
	stack[sp].ofs = root;
	stack[sp].depth = 0;
	sp++;

	while (sp > 0)
	{
		size_t o;
		int depth, offr, offl;

		sp--;
		o     = stack[sp].ofs;
		depth = stack[sp].depth;

		if (o < surf || o + PHY_NODE_SIZE > end)
			return false;
		if (depth >= PHY_MAX_DEPTH-1)
			return false;

		offr = PHY_ReadI32(ctx->base + o + 0);
		offl = PHY_ReadI32(ctx->base + o + 4);

		if (offr == 0)
		{	/* leaf: offset_compact_ledge is relative to the node */
			size_t l = o + (size_t)(ptrdiff_t)offl;
			if (offl < 0 && (size_t)(-(ptrdiff_t)offl) > o)
				return false;
			if (l < surf || l + PHY_LEDGE_HEADER_SIZE > end)
				return false;
			if (*numledges >= PHY_MAX_LEDGES)
				return false;
			ledges[(*numledges)++] = l;
			continue;
		}

		if (sp + 2 > PHY_MAX_DEPTH)
			return false;
		/* left child sits immediately after this node; right is offr away */
		stack[sp].ofs = o + PHY_NODE_SIZE;
		stack[sp].depth = depth+1;
		sp++;
		if (offr < 0 && (size_t)(-(ptrdiff_t)offr) > o)
			return false;
		stack[sp].ofs = o + (size_t)(ptrdiff_t)offr;
		stack[sp].depth = depth+1;
		sp++;
	}
	return *numledges > 0;
}

static qboolean PHY_GrowVerts (phyctx_t *ctx, size_t want)
{
	if (want <= ctx->maxverts)
		return true;
	while (ctx->maxverts < want)
		ctx->maxverts = ctx->maxverts ? ctx->maxverts*2 : 256;
	ctx->xyz = plugfuncs->Realloc(ctx->xyz, sizeof(*ctx->xyz)*ctx->maxverts);
	return ctx->xyz != NULL;
}
static qboolean PHY_GrowIdx (phyctx_t *ctx, size_t want)
{
	if (want <= ctx->maxidx)
		return true;
	while (ctx->maxidx < want)
		ctx->maxidx = ctx->maxidx ? ctx->maxidx*2 : 768;
	ctx->idx = plugfuncs->Realloc(ctx->idx, sizeof(*ctx->idx)*ctx->maxidx);
	return ctx->idx != NULL;
}

/*
  One ledge -> triangles appended to ctx.

  A ledge's points live at `c_point_offset` from the ledge and are indexed by
  the triangles' edges.  There is NO point count in the format: the array is
  sized by the highest index any of this ledge's triangles reaches, which is
  what everything that reads these files does.  Slightly wasteful when ledges
  share a point array; correct in every case.
*/
static qboolean PHY_ReadLedge (phyctx_t *ctx, size_t lo, size_t surf, size_t end)
{
	int      cpo    = PHY_ReadI32(ctx->base + lo + 0);
	short    ntri   = PHY_ReadI16(ctx->base + lo + 12);
	size_t   points, firstvert;
	int      t, k;
	unsigned maxidx = 0;
	qboolean any = false;

	if (ntri <= 0)
		return true;                    /* an empty ledge is legal, not an error */
	if (lo + PHY_LEDGE_HEADER_SIZE + (size_t)ntri*PHY_TRI_SIZE > end)
		return false;
	if (ctx->numidx/3 + (size_t)ntri > PHY_MAX_TRIS)
		return false;

	/* first pass: how many points does this ledge actually reach? */
	for (t = 0; t < ntri; t++)
	{
		const qbyte *tp = ctx->base + lo + PHY_LEDGE_HEADER_SIZE + (size_t)t*PHY_TRI_SIZE;
		for (k = 0; k < 3; k++)
		{
			unsigned e = PHY_ReadU32(tp + 4 + k*4) & 0xffff;   /* start_point_index:16 */
			if (!any || e > maxidx)
				maxidx = e;
			any = true;
		}
	}
	if (!any)
		return true;

	if (cpo < 0 && (size_t)(-(ptrdiff_t)cpo) > lo)
		return false;
	points = lo + (size_t)(ptrdiff_t)cpo;
	if (points < surf || points + ((size_t)maxidx+1)*PHY_POINT_SIZE > end)
		return false;

	firstvert = ctx->numverts;
	if (!PHY_GrowVerts(ctx, firstvert + maxidx + 1))
		return false;
	if (!PHY_GrowIdx(ctx, ctx->numidx + (size_t)ntri*3))
		return false;

	{
		unsigned i;
		for (i = 0; i <= maxidx; i++)
		{
			const qbyte *pp = ctx->base + points + (size_t)i*PHY_POINT_SIZE;
			float x = PHY_ReadF32(pp+0);
			float y = PHY_ReadF32(pp+4);
			float z = PHY_ReadF32(pp+8);
			vecV_t *out = &ctx->xyz[firstvert + i];
			/* HL = (x, z, -y) * 39.37 -- fitted, see the header note. */
			(*out)[0] =  x * PHY_IVP2HL;
			(*out)[1] =  z * PHY_IVP2HL;
			(*out)[2] = -y * PHY_IVP2HL;
			/* vecV_t is an aligned vec4 on every target but web; leaving w
			   uninitialised means the SSE-width copies downstream carry
			   whatever was in the heap.  The compiler folds this away where
			   vecV_t is a vec3. */
			if (sizeof(vecV_t) >= sizeof(float)*4)
				((float*)out)[3] = 1;
		}
	}

	for (t = 0; t < ntri; t++)
	{
		const qbyte *tp = ctx->base + lo + PHY_LEDGE_HEADER_SIZE + (size_t)t*PHY_TRI_SIZE;
		for (k = 0; k < 3; k++)
		{
			unsigned e = PHY_ReadU32(tp + 4 + k*4) & 0xffff;
			ctx->idx[ctx->numidx++] = (index_t)(firstvert + e);
		}
	}

	ctx->numverts = firstvert + maxidx + 1;
	return true;
}

/*
  Parse the whole file into ctx.  Returns false and leaves the caller to free
  whatever was accumulated.
*/
static qboolean PHY_Parse (phyctx_t *ctx, const char *dbgname)
{
	phyheader_t hdr;
	size_t off, base, end, surf, root;
	size_t         *ledges = NULL;
	phynodestack_t *stack  = NULL;
	size_t numledges, i;
	int ssize, ver, mtype;
	qboolean ok = false;

	if (ctx->filelen < sizeof(phyheader_t))
		return false;

	hdr.size       = PHY_ReadI32(ctx->base + 0);
	hdr.id         = PHY_ReadI32(ctx->base + 4);
	hdr.solidCount = PHY_ReadI32(ctx->base + 8);
	hdr.checksum   = PHY_ReadI32(ctx->base + 12);

	if (hdr.size < (int)sizeof(phyheader_t) || (size_t)hdr.size > ctx->filelen)
		return false;
	if (hdr.solidCount < 1 || hdr.solidCount > PHY_MAX_SOLIDS)
		return false;

	/* See THE BOUNDS CHECK: a multi-solid .phy is a jointed body and its solids
	   are bone-relative.  Refusing them here is not a limitation, it is the
	   correctness rule -- and it is 162 of 3,084 files, all ragdolls and gibs
	   that are never solid static props. */
	if (hdr.solidCount != 1)
	{
		phy_stat_jointed++;
		Con_DPrintf("%s: .phy has %i solids (jointed) -- using the render mesh\n",
		            dbgname, hdr.solidCount);
		return false;
	}

	off = (size_t)hdr.size;
	if (off + 16 > ctx->filelen)
		return false;

	ssize = PHY_ReadI32(ctx->base + off);
	if (ssize < PHY_SOLID_SURFACE_OFS + PHY_SURF_IVPS_OFS ||
	    off + 4 + (size_t)ssize > ctx->filelen)
		return false;

	base = off;
	end  = off + 4 + (size_t)ssize;

	if (memcmp(ctx->base + base + PHY_SOLID_MAGIC_OFS, "VPHY", 4))
		return false;

	ver   = PHY_ReadI16(ctx->base + base + PHY_SOLID_VERSION_OFS);
	mtype = PHY_ReadI16(ctx->base + base + PHY_SOLID_MODELTYPE_OFS);
	if (ver != PHY_VERSION_IVPS || mtype != PHY_MODELTYPE_IVPS)
	{
		Con_DPrintf("%s: .phy version %i type %i is not the checked layout -- using the render mesh\n",
		            dbgname, ver, mtype);
		return false;
	}

	surf = base + PHY_SOLID_SURFACE_OFS;
	if (surf + PHY_SURF_IVPS_OFS + 4 > end)
		return false;
	/* "IVPS" sits at a fixed place in the compact surface.  If it is not there,
	   the layout is not the one these offsets describe and nothing below is
	   trustworthy. */
	if (memcmp(ctx->base + surf + PHY_SURF_IVPS_OFS, "IVPS", 4))
		return false;

	{
		int r = PHY_ReadI32(ctx->base + surf + PHY_SURF_ROOT_OFS);
		if (r < 0)
			return false;
		root = surf + (size_t)r;
	}

	/* Heap, not file scope: models load on worker threads. */
	ledges = plugfuncs->Malloc(sizeof(*ledges)*PHY_MAX_LEDGES);
	stack  = plugfuncs->Malloc(sizeof(*stack)*PHY_MAX_DEPTH);
	if (!ledges || !stack)
		goto done;

	if (!PHY_CollectLedges(ctx, surf, end, root, ledges, &numledges, stack))
		goto done;

	for (i = 0; i < numledges; i++)
		if (!PHY_ReadLedge(ctx, ledges[i], surf, end))
			goto done;

	ok = (ctx->numidx >= 3);

done:
	plugfuncs->Free(ledges);
	plugfuncs->Free(stack);
	return ok;
}

/*
===========================================================================
  Mod_PHY_CollisionMesh

  Build a galiasinfo_t whose triangles are the model's COLLISION shape, for
  BIH_BuildAlias to turn into the model's trace tree.  Returns NULL to mean
  "use the render mesh", which is what shipped before this and is always a
  safe answer.

  mode is hl2_propcollision:
      0  caller should not be asking (props non-solid) -- treated as 3
      1  .phy VPhysics hull, falling back to the render mesh     (default)
      2  the model's bounding box, as Source's "solid" 2 asks for
      3  the render mesh -- build 7 behaviour, kept for A/B

  Everything returned is allocated in mod->memgroup, so it lives exactly as
  long as the model does and is freed with it.
===========================================================================
*/
galiasinfo_t *Mod_PHY_CollisionMesh (model_t *mod, int mode,
                                     plugfsfuncs_t *phy_filefuncs,
                                     plugmodfuncs_t *phy_modfuncs,
                                     unsigned int contents)
{
	galiasinfo_t *out;
	char          name[MAX_QPATH];
	void         *file = NULL;
	size_t        filelen = 0;
	phyctx_t      ctx;
	qboolean      ok;

	/*
	  FTESurf Patch 262 note, because this is where you would expect the change and
	  it is deliberately NOT here.

	  NULL from this function means "use the render mesh", and it covers two
	  situations Source treats differently: a model that ships no .phy at all (which
	  Source does not collide, having no collision data for it), and a .phy we could
	  not use -- jointed, or a hull that escapes the model bounds -- which Source
	  DOES collide and which the render mesh honestly approximates.

	  Acting on that distinction here would mean deciding a PROP's solidity in the
	  MODEL loader, which does not know the prop: m_Solid 2 asks for the model's
	  bounding box and needs no .phy at all, so it must keep colliding.  The decision
	  therefore lives in mod_vbsp's BIH leaf builder, which has the prop record in
	  front of it.  See VBSP_ModelHasPhy there.
	*/

	if (mode == 2)
	{	/* Bounding box: twelve triangles from the model's own bounds.  Doing it
		   here rather than as a new BIH leaf type means every trace path that
		   already understands a model understands this too, with no engine
		   change -- and it works for brush-mounted props, not just static ones. */
		static const int boxidx[36] =
		{
			0,2,3, 0,3,1,   4,5,7, 4,7,6,   /* -z, +z */
			0,1,5, 0,5,4,   2,6,7, 2,7,3,   /* -y, +y */
			0,4,6, 0,6,2,   1,3,7, 1,7,5    /* -x, +x */
		};
		vecV_t  *xyz;
		index_t *idx;
		int i;

		if (mod->mins[0] >= mod->maxs[0] &&
		    mod->mins[1] >= mod->maxs[1] &&
		    mod->mins[2] >= mod->maxs[2])
			return NULL;        /* degenerate bounds -- nothing useful to build */

		out = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out));
		xyz = plugfuncs->GMalloc(&mod->memgroup, sizeof(*xyz)*8);
		idx = plugfuncs->GMalloc(&mod->memgroup, sizeof(*idx)*36);
		memset(out, 0, sizeof(*out));

		for (i = 0; i < 8; i++)
		{
			xyz[i][0] = (i&4) ? mod->maxs[0] : mod->mins[0];
			xyz[i][1] = (i&2) ? mod->maxs[1] : mod->mins[1];
			xyz[i][2] = (i&1) ? mod->maxs[2] : mod->mins[2];
			if (sizeof(vecV_t) >= sizeof(float)*4)
				((float*)&xyz[i])[3] = 1;
		}
		for (i = 0; i < 36; i++)
			idx[i] = (index_t)boxidx[i];

		phy_stat_bbox++;
		Q_strlcpy(out->surfacename, "phy_bbox", sizeof(out->surfacename));
		out->numverts     = 8;
		out->ofs_skel_xyz = xyz;
		out->numindexes   = 36;
		out->ofs_indexes  = idx;
		out->contents     = contents;
		out->shares_verts = -1;
		out->shares_bones = -1;
		out->nextsurf     = NULL;
		return out;
	}

	if (mode != 1)
		return NULL;            /* 3 (or anything unexpected): render mesh */

	phy_modfuncs->StripExtension(mod->name, name, sizeof(name));
	Q_strncatz(name, ".phy", sizeof(name));
	file = phy_filefuncs->LoadFile(name, &filelen);
	if (!file)
	{
		phy_stat_nofile++;
		return NULL;        /* no hull shipped -- the render mesh is all there is */
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.base    = file;
	ctx.filelen = filelen;

	ok = PHY_Parse(&ctx, mod->name);
	plugfuncs->Free(file);

	if (ok)
	{
		/*
		  THE BOUNDS CHECK.  See the note at the top: a hull in the wrong place
		  is worse than a hull that is too fat, because the second is visible
		  and the first is not.  If what we assembled does not sit inside the
		  model, we got it wrong -- say so and fall back.
		*/
		vec3_t mn, mx;
		size_t i;
		int    a;

		VectorCopy(ctx.xyz[0], mn);
		VectorCopy(ctx.xyz[0], mx);
		for (i = 1; i < ctx.numverts; i++)
			for (a = 0; a < 3; a++)
			{
				if (ctx.xyz[i][a] < mn[a]) mn[a] = ctx.xyz[i][a];
				if (ctx.xyz[i][a] > mx[a]) mx[a] = ctx.xyz[i][a];
			}

		for (a = 0; a < 3; a++)
		{
			if (mn[a] < mod->mins[a] - PHY_BOUNDS_SLACK ||
			    mx[a] > mod->maxs[a] + PHY_BOUNDS_SLACK)
			{
				Con_DPrintf("%s: .phy hull [%.1f %.1f %.1f]..[%.1f %.1f %.1f] "
				            "escapes the model [%.1f %.1f %.1f]..[%.1f %.1f %.1f] "
				            "-- bone-relative solid, using the render mesh\n",
				            mod->name, mn[0],mn[1],mn[2], mx[0],mx[1],mx[2],
				            mod->mins[0],mod->mins[1],mod->mins[2],
				            mod->maxs[0],mod->maxs[1],mod->maxs[2]);
				ok = false;
				break;
			}
		}
	}

	if (!ok)
	{
		phy_stat_rejected++;
		plugfuncs->Free(ctx.xyz);
		plugfuncs->Free(ctx.idx);
		return NULL;
	}
	phy_stat_hull++;
	phy_stat_tris += (int)(ctx.numidx/3);

	/* Copy out of the grow buffers into the model's own memgroup, so this
	   lives and dies with the model rather than with the loader. */
	out = plugfuncs->GMalloc(&mod->memgroup, sizeof(*out));
	memset(out, 0, sizeof(*out));

	out->ofs_skel_xyz = plugfuncs->GMalloc(&mod->memgroup, sizeof(vecV_t)*ctx.numverts);
	memcpy(out->ofs_skel_xyz, ctx.xyz, sizeof(vecV_t)*ctx.numverts);
	out->numverts = ctx.numverts;

	out->ofs_indexes = plugfuncs->GMalloc(&mod->memgroup, sizeof(index_t)*ctx.numidx);
	memcpy(out->ofs_indexes, ctx.idx, sizeof(index_t)*ctx.numidx);
	out->numindexes = ctx.numidx;

	Q_strlcpy(out->surfacename, "phy_hull", sizeof(out->surfacename));
	out->contents     = contents;
	out->shares_verts = -1;
	out->shares_bones = -1;
	out->nextsurf     = NULL;

	plugfuncs->Free(ctx.xyz);
	plugfuncs->Free(ctx.idx);

	return out;
}
