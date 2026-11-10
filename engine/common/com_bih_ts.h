/* FTESurf Patch 1467: com_bih.c's clip flags (set by the Source mover) and probe record, one set a thread: a
 * tas_rollout_batch worker binds its own (BIH_ThreadState). Separate from com_bih.h, which has no include guard
 * and which pm_source.c does not include. Pointer, not thread-local objects: see pm_thread.h. */
#ifndef COM_BIH_TS_H
#define COM_BIH_TS_H
typedef struct
{
	int sourceclip, phybrushes, phyfix, phypick;	//Patches 492, 764, 888, 1248
	int probe_plane;			/*0 face, 1 back slab, 2-4 in-plane edge, 5-13 bevel, 100-105 axial, -1 not a triangle*/
	int probe_kind;				/*0 nothing, 1 BIH_TRIANGLE, 2 BIH_BRUSH, 3 BIH_PATCHBRUSH*/
	index_t probe_idx[3];
	struct model_s *probe_model;	/*NULL = world/top level, else the submodel (the prop)*/
	vec3_t probe_norm;
	vec3_t probe_tri[3];
	vec3_t probe_modelorg;
	unsigned int probe_contents;
	unsigned int probe_seq;
	char probe_surf[32];
} bih_tstate_t;
extern FTE_TLS bih_tstate_t *bih_ts;
size_t BIH_ThreadStateSize(void);
void BIH_ThreadState(void *state);	//NULL: the main one
#define bih_sourceclip		(bih_ts->sourceclip)
#define bih_phybrushes		(bih_ts->phybrushes)
#define bih_phyfix			(bih_ts->phyfix)
#define bih_phypick			(bih_ts->phypick)
#define bih_probe_plane		(bih_ts->probe_plane)
#define bih_probe_kind		(bih_ts->probe_kind)
#define bih_probe_idx		(bih_ts->probe_idx)
#define bih_probe_model		(bih_ts->probe_model)
#define bih_probe_norm		(bih_ts->probe_norm)
#define bih_probe_tri		(bih_ts->probe_tri)
#define bih_probe_modelorg	(bih_ts->probe_modelorg)
#define bih_probe_contents	(bih_ts->probe_contents)
#define bih_probe_seq		(bih_ts->probe_seq)
#define bih_probe_surf		(bih_ts->probe_surf)
#endif
