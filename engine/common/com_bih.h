#if 1
	#define BIH_USEBIH
#else
	#define BIH_USEBVH
#endif

typedef struct q2mapsurface_s  // used internally due to name len probs //ZOID
{
	q2csurface_t	c;
	char		rname[32];
} q2mapsurface_t;
typedef struct
{
	mplane_t	*plane;
	q2mapsurface_t	*surface;
} q2cbrushside_t;
typedef struct q2cbrush_s
{
	int			checkcount;		// to avoid repeated testings
	int			contents;
	vec3_t		absmins;
	vec3_t		absmaxs;
	int			numsides;
	q2cbrushside_t *brushside;
} q2cbrush_t;
typedef struct
{
	vec3_t		absmins, absmaxs;

	vecV_t		*xyz_array;
	size_t numverts;
	index_t		*indicies;
	size_t numincidies;

	q2mapsurface_t	*surface;
	int			checkcount;		// to avoid repeated testings
} q3cmesh_t;
typedef struct
{
	vec3_t		absmins, absmaxs;

	int			numfacets;
	q2cbrush_t	*facets;
	q2mapsurface_t	*surface;
	int			checkcount;		// to avoid repeated testings
} q3cpatch_t;


enum bihtype_e
{
	//node types
#ifdef BIH_USEBIH
	BIH_X,
	BIH_Y,
	BIH_Z,
#endif
#ifdef BIH_USEBVH
	BVH_X,
	BVH_Y,
	BVH_Z,
#endif
	BIH_GROUP,


	//leaf types
#if defined(Q2BSPS) || defined(Q3BSPS)
	BIH_BRUSH,
#endif
#ifdef Q3BSPS
	BIH_PATCHBRUSH,
	BIH_TRISOUP,
#endif
	BIH_TRIANGLE,
	BIH_MODEL,
};
struct bihdata_s
{
	unsigned int contents;
	union {
#if defined(Q2BSPS) || defined(Q3BSPS)
		q2cbrush_t *brush;
#endif
#ifdef Q3BSPS
		q2cbrush_t *patchbrush;
		q3cmesh_t *cmesh;
#endif
		struct {
//			vec3_t norm;
			index_t *indexes;	//might be better to just bake 3 indexes instead of using a pointer to them
			vecV_t *xyz;
		} tri;
		struct {
			model_t *model;
			struct bihtransform_s
			{
				vec3_t axis[3];
				vec3_t origin;
				//FTESurf Patch 764: the prop's .phy hull as world-space convex brushes,
				//traced instead of the model while bih_phybrushes is set.  0 = none.
				q2cbrush_t *phybrushes;
				unsigned int numphybrushes;
				//FTESurf Patch 888: phyfix set = the model's old .phy indices wrapped, so
				//mover 9 (bih_phyfix) traces phyfixbrushes, or the model's phyfixed tree if 0.
				q2cbrush_t *phyfixbrushes;
				unsigned int numphyfixbrushes;
				qboolean phyfix;
			} *tr;
		} mesh;
	};
};
struct bihleaf_s
{
	enum bihtype_e type;
	vec3_t mins;
	vec3_t maxs;
	struct bihdata_s data;
};

struct galiasinfo_s;

//generates a BIH tree and updates mod->funcs.NativeTrace|NativeContents funcs
void BIH_Build (model_t *mod, struct bihleaf_s *items, size_t numitems);

#if defined(Q2BSPS) || defined(Q3BSPS)
//FTESurf Patch 319: hand every BIH_BRUSH leaf overlapping a box to a callback, so
//r_showbrushes can draw collision hulls that have no renderable faces at all (a
//nodraw PLAYERCLIP brush is invisible by construction -- VBSP emits no faces for
//it). The brush array is plugin-private and struct bihnode_s is private to
//com_bih.c, so this is the only way in.
void BIH_EnumBrushes (model_t *mod, const vec3_t mins, const vec3_t maxs,
					  void (*cb)(void *ctx, const q2cbrush_t *brush), void *ctx);
//FTESurf Patch 776: also triangles (displacements) and static props' hull pieces, which
//are world-space brushes; a prop without them is counted in propsnohull and skipped.
struct bihenum_s
{
	void *ctx;
	void (*brush)(void *ctx, const q2cbrush_t *brush);
	void (*tri)(void *ctx, const vec_t *v0, const vec_t *v1, const vec_t *v2, unsigned int contents);
	unsigned int props, propsnohull;	//out: BIH_MODEL leaves met, and those without hull pieces
};
void BIH_EnumCollision (model_t *mod, const vec3_t mins, const vec3_t maxs, struct bihenum_s *en);
/* Conservative VBSP air proof; false includes unknown shape or exhausted budget. */
qboolean BIH_HeldAirBoxEmpty(model_t *mod, const vec3_t mins, const vec3_t maxs);
unsigned int BIH_HeldAirBoxTest(unsigned int *checks);
#endif
void BIH_BuildAlias (model_t *mod, struct galiasinfo_s *meshes);

//FTESurf Patch 317: print the identity of whatever stopped the last trace --
//leaf type, model, triangle, surface, and which of BIH_ClipToTriangle's planes.
//com_bih.c owns the bih_probe_* record, and is linked into client and server, so
//the mover (pm_source.c) and solid_here (view.c) can both print the same line.
void BIH_ProbeReport (const trace_t *t, const char *tag);

//...and the record itself, for a caller that wants to LATCH on it rather than
//print every tick (pm_dispprobe 2 does; a surfer clips every single tick).
//Patch 258 declared these as bare externs inside pm_source.c; they are here now
//so the two readers cannot drift apart.
extern int			bih_probe_plane;	/*0 face, 1 back slab, 2-4 in-plane edge, 5-13 bevel, 100-105 axial, -1 not a triangle*/
extern int			bih_probe_kind;		/*0 nothing, 1 BIH_TRIANGLE, 2 BIH_BRUSH, 3 BIH_PATCHBRUSH*/
extern index_t		bih_probe_idx[3];
extern model_t	   *bih_probe_model;	/*NULL = world/top level, else the submodel (the prop)*/
extern vec3_t		bih_probe_norm;
extern vec3_t		bih_probe_tri[3];
extern vec3_t		bih_probe_modelorg;
extern unsigned int	bih_probe_contents;
extern unsigned int	bih_probe_seq;

/* Native artifact ownership. Epochs are volatile, never recording fields.
 * Module NULL selects the running engine; code is then NULL. Main thread only. */
#ifdef PLUGINS
uint64_t Plug_ArtifactFingerprint(void *module, const void *code, unsigned char digest[32]);
#endif
uint64_t Mod_HeldWorldArtifacts(model_t *world, unsigned char bsp[32], unsigned char plugin[32]);
