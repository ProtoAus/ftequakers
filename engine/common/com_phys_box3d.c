/*
	Box3D physics backend for FTEQW  (nettest)

	A parallel to com_phys_ode.c: it fills the same rigidbodyengine_t (world.h) with
	Box3D calls instead of ODE, and registers itself as "Box3D" so `plug_load box3d`
	switches the server's rigid-body physics onto Box3D (Erin Catto's C fork of Box2D).

	Scope (v1 - prop pipeline):
	  * dynamic props   -> a convex hull built from the collision verts (Box3D has no
	                        dynamic trimesh; same tradeoff as ODE physics_ode_use_decomp 0).
	  * static world/brush -> a baked triangle mesh (b3CreateMesh + b3CreateMeshShape).
	  * box/sphere/capsule/cylinder primitives.
	  * the gravity-gun black hole (physics_addforce -> RBECMD_FORCE -> b3Body_ApplyForce).
	  * multicore via Box3D's OWN internal task scheduler (physics_box3d_threads>1).

	Deliberately stubbed vs ODE:
	  * skeletal ragdolls (all Rag* functions) - a separate path from props.
	  * prop-vs-prop QC .touch events - props still collide physically, they just don't
	    fire QC touch callbacks (v1.1: read b3World_GetContactEvents after the step).

	Player + bullet collision is UNAFFECTED: those go through FTE's own World_HullTrace,
	never the physics engine, exactly as with ODE.

	The transform sync (quaternion <-> Quake angles + the geomcenter offset matrix) mirrors
	com_phys_ode.c 1:1, substituting a b3Quat for ODE's 3x4 rotation matrix. The
	avelocity<->angular-velocity axis map and the r_meshpitch sign match ODE exactly.
*/

//if we're not building as an fte-specific plugin, we must be being built as part of the fte engine itself.
#ifndef FTEPLUGIN
#define FTEPLUGIN
#define Plug_Init Plug_Box3D_Init
#endif

#include "../../plugins/plugin.h"
#include "../../plugins/engine.h"

#ifdef USERBE

#include "pr_common.h"
#include "box3d/box3d.h"

#ifndef DEG2RAD
#define DEG2RAD(d) ((d) * M_PI * (1.0/180.0))
#endif
#ifndef RAD2DEG
#define RAD2DEG(d) ((d)*180.0 / M_PI)
#endif

#ifndef FTEENGINE
#define BZ_Malloc malloc
#define BZ_Free free
#define Z_Free BZ_Free
#define VectorCompare VectorComparestatic
static int VectorCompare (const vec3_t v1, const vec3_t v2)
{
	int		i;
	for (i=0 ; i<3 ; i++)
		if (v1[i] != v2[i])
			return 0;
	return 1;
}
//mathlib.c's VectorAngles(...,meshpitch)/AngleVectorsMesh reference these (as r_meshpitch/r_meshroll,
//which the plugin build #defines to (*cvar_r_meshpitch)); the physics backend must provide them.
cvar_t *cvar_r_meshpitch;
cvar_t *cvar_r_meshroll;
#endif

static rbeplugfuncs_t *rbefuncs;

//cvars
static cvar_t *physics_box3d_threads;		//worker count for Box3D's own internal scheduler. 1 = single-threaded. >1 = multicore.
static cvar_t *physics_box3d_substeps;		//solver sub-steps per world step (Box3D sub-steps internally; higher = more accurate/stable).
static cvar_t *physics_box3d_autodisable;	//let settled bodies sleep (a pile of resting props costs ~0).
static cvar_t *physics_box3d_maxlinearspeed;//clamp on body speed (prevents fly-away); 0 = Box3D default.
static cvar_t *physics_box3d_unitscale;		//Quake units per real metre. Box3D's tolerances are metre-tuned; without this props jam/freeze at QU scale.
static cvar_t *physics_box3d_decomp;		//1 = concave props use the multi-hull convex decomposition (many hull shapes/body); 0 = single convex hull.
static cvar_t *physics_box3d_maxpieces;		//if a prop decomposes into more pieces than this, fall back to a single hull (bounds broadphase proxy count).
static cvar_t *physics_box3d_debug;			//1 = verbose body-build + per-second sim tracing to the console.

//----------------------------------------------------------------------------
// Box3D id <-> edict void* slot.  ids are 8-byte value structs; store the packed
// uint64 in the existing rbe void* slots (index1==0 is null, so 0 == "no body").
//----------------------------------------------------------------------------
#define B3BODY(ed)         b3LoadBodyId((uint64_t)(uintptr_t)(ed)->rbe.body.body)
#define B3SHAPE(ed)        b3LoadShapeId((uint64_t)(uintptr_t)(ed)->rbe.body.geom)
#define SETB3BODY(ed,id)   ((ed)->rbe.body.body = (void*)(uintptr_t)b3StoreBodyId(id))
#define SETB3SHAPE(ed,id)  ((ed)->rbe.body.geom = (void*)(uintptr_t)b3StoreShapeId(id))
#define HAVEB3BODY(ed)     ((ed)->rbe.body.body != NULL)
#define HAVEB3SHAPE(ed)    ((ed)->rbe.body.geom != NULL)

struct box3dctx_s
{
	rigidbodyengine_t pub;		//MUST be first: world->rbe points here and is cast to struct box3dctx_s*

	qboolean hasextraobjs;
	b3WorldId world;			//8-byte value handle, not a pointer
	int substeps;
	int workercount;			//currently-applied worker count (so we can honor live cvar changes)
	vec_t movelimit;
	rbecommandqueue_t *cmdqueuehead;
	rbecommandqueue_t *cmdqueuetail;
};

static void World_Box3D_RunCmd(world_t *world, rbecommandqueue_t *cmd);
static void QDECL World_Box3D_RemoveFromEntity(world_t *world, wedict_t *ed);

//Owned heap blobs behind a body's shapes (b3CreateHull/b3CreateMesh results — Box3D holds meshes by
//reference and does NOT free either on b3DestroyBody, so we must).  A dynamic CONCAVE prop owns MANY
//hulls (one per convex-decomposition piece), hence the count.  Stored in ed->rbe.geomdata; primitives
//(box/sphere/capsule) own no heap and leave geomdata NULL.
typedef struct box3dgeom_s
{
	qboolean	ismesh;		//true: ptr[0] is a b3MeshData*; false: ptr[] are b3HullData*
	int			count;
	void		*ptr[1];	//flexible: `count` pointers
} box3dgeom_t;

static box3dgeom_t *Box3D_AllocGeom(int count)
{
	box3dgeom_t *g = (box3dgeom_t*)BZ_Malloc(sizeof(box3dgeom_t) + (count-1)*sizeof(void*));
	g->ismesh = false;
	g->count = count;
	return g;
}

//----------------------------------------------------------------------------
// helpers
//----------------------------------------------------------------------------

//Build a Box3D quaternion from a Quake forward/left/up basis (columns of the rotation
//matrix, matching ODE's dBodySetRotation column layout).  b3MakeQuatFromMatrix treats
//m.cx/cy/cz as the images of the X/Y/Z axes, so b3RotateVector(q,axisX)==forward, which is
//the exact inverse of the read path in BodyToEntity.
static b3Quat Box3D_QuatFromFLU(const vec3_t forward, const vec3_t left, const vec3_t up)
{
	b3Matrix3 m;
	m.cx.x = forward[0];	m.cx.y = forward[1];	m.cx.z = forward[2];
	m.cy.x = left[0];		m.cy.y = left[1];		m.cy.z = left[2];
	m.cz.x = up[0];			m.cz.y = up[1];			m.cz.z = up[2];
	return b3NormalizeQuat(b3MakeQuatFromMatrix(&m));
}

//dimension_solid/dimension_hit -> Box3D category/mask.  Box3D collides A vs B when
//(A.category & B.mask) && (B.category & A.mask), which is exactly ODE's nearCallback
//dimension test.  Unset (0) -> collide-all so props collide with the world by default.
static b3Filter Box3D_FilterForEdict(wedict_t *ed)
{
	b3Filter f = b3DefaultFilter();
	uint64_t dsolid = (uint64_t)(unsigned)(int)ed->xv->dimension_solid;
	uint64_t dhit   = (uint64_t)(unsigned)(int)ed->xv->dimension_hit;
	if (dsolid) f.categoryBits = dsolid;
	if (dhit)   f.maskBits     = dhit;
	return f;
}

//----------------------------------------------------------------------------
// lifecycle
//----------------------------------------------------------------------------
static void QDECL World_Box3D_End(world_t *world)
{
	struct box3dctx_s *ctx = (struct box3dctx_s*)world->rbe;
	world->rbe = NULL;
	//b3DestroyWorld frees all bodies + shapes.  Our per-edict b3CreateHull/b3CreateMesh
	//blobs (ed->rbe.geomdata) are freed per-edict by RemoveFromEntity during the engine's
	//shutdown edict sweep (same as ODE); destroying the world just drops the ids.
	b3DestroyWorld(ctx->world);
	while (ctx->cmdqueuehead)
	{
		rbecommandqueue_t *c = ctx->cmdqueuehead;
		ctx->cmdqueuehead = c->next;
		Z_Free(c);
	}
	Z_Free(ctx);
}

static void QDECL World_Box3D_RemoveFromEntity(world_t *world, wedict_t *ed)
{
	if (!ed->rbe.physics)
		return;
	ed->rbe.physics = false;

	if (HAVEB3BODY(ed))
		b3DestroyBody(B3BODY(ed));		//also destroys the attached shape
	ed->rbe.body.body = NULL;
	ed->rbe.body.geom = NULL;

	if (ed->rbe.geomdata)
	{
		//free every hull/mesh blob owned by this body's shapes.  Box3D holds meshes by reference and
		//frees NEITHER on b3DestroyBody, so this must run AFTER the b3DestroyBody above (mirrors the
		//ODE dTriMeshData leak fix).  A concave prop owns one hull per decomposition piece.
		box3dgeom_t *g = (box3dgeom_t*)ed->rbe.geomdata;
		int k;
		for (k = 0; k < g->count; k++)
		{
			if (!g->ptr[k])
				continue;
			if (g->ismesh)
				b3DestroyMesh((b3MeshData*)g->ptr[k]);
			else
				b3DestroyHull((b3HullData*)g->ptr[k]);
		}
		BZ_Free(g);
		ed->rbe.geomdata = NULL;
	}

	rbefuncs->ReleaseCollisionMesh(ed);
	if (ed->rbe.massbuf)
		BZ_Free(ed->rbe.massbuf);
	ed->rbe.massbuf = NULL;
}

//----------------------------------------------------------------------------
// physics engine -> entity  (mirror of World_ODE_Frame_BodyToEntity)
//----------------------------------------------------------------------------
static void World_Box3D_Frame_BodyToEntity(world_t *world, wedict_t *ed)
{
	model_t *model;
	b3BodyId body;
	b3WorldTransform xf;
	b3Vec3 lv, av, bx, by, bz;
	int movetype;
	float bodymatrix[16];
	float entitymatrix[16];
	vec3_t angles, avelocity, forward, left, up, origin, spinvelocity, velocity;

	if (!HAVEB3BODY(ed))
		return;
	movetype = (int)ed->v->movetype;
	if (movetype != MOVETYPE_PHYSICS)
		return;					//static/kinematic bodies never feed back (joints are stubbed)

	body = B3BODY(ed);
	xf = b3Body_GetTransform(body);				//{ b3Pos p; b3Quat q }
	lv = b3Body_GetLinearVelocity(body);
	av = b3Body_GetAngularVelocity(body);		//radians/sec, world

	//forward/left/up = the body basis (columns of the rotation matrix), same as ODE's r[] columns.
	bx = b3RotateVector(xf.q, b3Vec3_axisX);
	by = b3RotateVector(xf.q, b3Vec3_axisY);
	bz = b3RotateVector(xf.q, b3Vec3_axisZ);
	VectorSet(forward, bx.x, bx.y, bx.z);
	VectorSet(left,    by.x, by.y, by.z);
	VectorSet(up,      bz.x, bz.y, bz.z);
	VectorSet(origin,  (float)xf.p.x, (float)xf.p.y, (float)xf.p.z);
	VectorSet(velocity, lv.x, lv.y, lv.z);
	VectorSet(spinvelocity, av.x, av.y, av.z);

	//undo the geomcenter offset (body origin == geomcenter; entity origin != center)
	Matrix4x4_RM_FromVectors(bodymatrix, forward, left, up, origin);
	Matrix4_Multiply(ed->rbe.offsetimatrix, bodymatrix, entitymatrix);
	Matrix3x4_RM_ToVectors(entitymatrix, forward, left, up, origin);

	VectorAngles(forward, up, angles, false);
	avelocity[PITCH] = RAD2DEG(spinvelocity[PITCH]);
	avelocity[YAW]   = RAD2DEG(spinvelocity[ROLL]);
	avelocity[ROLL]  = RAD2DEG(spinvelocity[YAW]);

	if (ed->v->modelindex)
	{
		model = world->Get_CModel(world, ed->v->modelindex);
		if (!model || model->type == mod_alias)
		{
			angles[PITCH]     *= cvar_r_meshpitch->value;
			avelocity[PITCH]  *= cvar_r_meshpitch->value;
		}
	}

	VectorCopy(origin, ed->v->origin);
	VectorCopy(velocity, ed->v->velocity);
	VectorCopy(angles, ed->v->angles);
	VectorCopy(avelocity, ed->v->avelocity);

	//values for BodyFromEntity to check if the qc modified anything next frame
	VectorCopy(origin, ed->rbe.origin);
	VectorCopy(velocity, ed->rbe.velocity);
	VectorCopy(angles, ed->rbe.angles);
	VectorCopy(avelocity, ed->rbe.avelocity);
	ed->rbe.gravity = (b3Body_GetGravityScale(body) != 0.0f);

	rbefuncs->LinkEdict(world, ed, true);
}

//----------------------------------------------------------------------------
// entity -> physics engine  (mirror of World_ODE_Frame_BodyFromEntity)
//----------------------------------------------------------------------------
static void World_Box3D_Frame_BodyFromEntity(world_t *world, wedict_t *ed)
{
	struct box3dctx_s *ctx = (struct box3dctx_s*)world->rbe;
	model_t *model;
	int axisindex;
	int modelindex = 0;
	int movetype = MOVETYPE_NONE;
	int solid = SOLID_NOT;
	int geomtype = GEOMTYPE_SOLID;
	qboolean modified = false;
	qboolean gravity;
	vec3_t angles, avelocity, entmaxs, entmins, forward, geomcenter, geomsize, left, origin, spinvelocity, up, velocity;
	vec_t length, radius, scale;
	vec_t massval = 1.0f;
	float test;
	b3BodyId body = {0};
	b3BodyType bodytype;
	qboolean havebody;

	geomtype  = (int)ed->xv->geomtype;
	solid     = (int)ed->v->solid;
	movetype  = (int)ed->v->movetype;
	scale     = ed->xv->scale ? ed->xv->scale : 1;
	model     = NULL;

	if (!geomtype)
	{
		switch(solid)
		{
		case SOLID_NOT:				geomtype = GEOMTYPE_NONE;		break;
		case SOLID_TRIGGER:			geomtype = GEOMTYPE_NONE;		break;
		case SOLID_BSP:				geomtype = GEOMTYPE_TRIMESH;	break;
		case SOLID_PHYSICS_TRIMESH:	geomtype = GEOMTYPE_TRIMESH;	break;
		case SOLID_PHYSICS_BOX:		geomtype = GEOMTYPE_BOX;		break;
		case SOLID_PHYSICS_SPHERE:	geomtype = GEOMTYPE_SPHERE;		break;
		case SOLID_PHYSICS_CAPSULE:	geomtype = GEOMTYPE_CAPSULE;	break;
		case SOLID_PHYSICS_CYLINDER:geomtype = GEOMTYPE_CYLINDER;	break;
		default:					geomtype = GEOMTYPE_BOX;		break;
		}
	}

	switch(geomtype)
	{
	case GEOMTYPE_TRIMESH:
		modelindex = (int)ed->v->modelindex;
		model = world->Get_CModel(world, modelindex);
		if (model)
		{
			VectorScale(model->mins, scale, entmins);
			VectorScale(model->maxs, scale, entmaxs);
			if (ed->xv->mass)
				massval = ed->xv->mass;
		}
		else
		{
			VectorClear(entmins);
			VectorClear(entmaxs);
			modelindex = 0;
			massval = 1.0f;
		}
		break;
	case GEOMTYPE_BOX:
	case GEOMTYPE_SPHERE:
	case GEOMTYPE_CAPSULE:
	case GEOMTYPE_CAPSULE_X:
	case GEOMTYPE_CAPSULE_Y:
	case GEOMTYPE_CAPSULE_Z:
	case GEOMTYPE_CYLINDER:
	case GEOMTYPE_CYLINDER_X:
	case GEOMTYPE_CYLINDER_Y:
	case GEOMTYPE_CYLINDER_Z:
		VectorCopy(ed->v->mins, entmins);
		VectorCopy(ed->v->maxs, entmaxs);
		if (ed->xv->mass)
			massval = ed->xv->mass;
		break;
	default:
		if (ed->rbe.physics)
			World_Box3D_RemoveFromEntity(world, ed);
		return;
	}

	VectorSubtract(entmaxs, entmins, geomsize);
	if (DotProduct(geomsize,geomsize) == 0)
	{
		if (ed->rbe.physics)
			World_Box3D_RemoveFromEntity(world, ed);
		return;
	}

	if (movetype != MOVETYPE_PHYSICS)
		massval = 1.0f;

	bodytype = (movetype == MOVETYPE_PHYSICS) ? b3_dynamicBody : b3_staticBody;

	//------------------------------------------------------------------
	// create or replace the body+shape
	//------------------------------------------------------------------
	if (!ed->rbe.physics
	 || !VectorCompare(ed->rbe.mins, entmins)
	 || !VectorCompare(ed->rbe.maxs, entmaxs)
	 || ed->rbe.mass != massval
	 || ed->rbe.modelindex != modelindex)
	{
		b3BodyDef bd;
		b3ShapeDef sd;
		b3ShapeId shape = {0};
		float volume = 1.0f;
		int numpieces = 1;		//>1 for a concave prop split into convex decomposition hulls

		modified = true;
		World_Box3D_RemoveFromEntity(world, ed);
		ed->rbe.physics = true;
		VectorCopy(entmins, ed->rbe.mins);
		VectorCopy(entmaxs, ed->rbe.maxs);
		ed->rbe.mass = massval;
		ed->rbe.modelindex = modelindex;
		VectorAvg(entmins, entmaxs, geomcenter);
		ed->rbe.movelimit = min(geomsize[0], min(geomsize[1], geomsize[2]));

		if (massval * geomsize[0] * geomsize[1] * geomsize[2] == 0)
		{
			if (movetype == MOVETYPE_PHYSICS)
				Con_Printf("entity %i (classname %s) .mass * .size == 0\n", NUM_FOR_EDICT(world->progs, (edict_t*)ed), PR_GetString(world->progs, ed->v->classname));
			massval = 1.0f;
			VectorSet(geomsize, 1.0f, 1.0f, 1.0f);
		}

		//create the body (real transform is applied in the "modified" block below)
		bd = b3DefaultBodyDef();
		bd.type = bodytype;
		bd.position.x = ed->v->origin[0];
		bd.position.y = ed->v->origin[1];
		bd.position.z = ed->v->origin[2];
		bd.userData = ed;
		bd.enableSleep = physics_box3d_autodisable->ival ? true : false;
		body = b3CreateBody(ctx->world, &bd);
		SETB3BODY(ed, body);

		sd = b3DefaultShapeDef();
		//STATIC geometry (world BSP, brushes, static props) collides with EVERYTHING regardless of
		//dimension - the ground/walls must stop every prop.  ODE gets this by exempting the world
		//(entnum 0) from its nearCallback dimension test; Box3D filters per-shape, so we give static
		//bodies an all-bits filter.  Without this a weapon drop (dimension_solid = PHYSDROP_DIM = bit 8)
		//falls straight through a world whose hit-mask is the default 255 (bits 0-7): 256 & 255 == 0.
		//Dynamic props still dimension-filter against EACH OTHER via Box3D_FilterForEdict.
		sd.filter = (bodytype == b3_staticBody) ? b3DefaultFilter() : Box3D_FilterForEdict(ed);

		switch(geomtype)
		{
		case GEOMTYPE_TRIMESH:
			Matrix4x4_Identity(ed->rbe.offsetmatrix);
			if (!model)
			{
				Con_Printf("entity %i (classname %s) has no model\n", NUM_FOR_EDICT(world->progs, (edict_t*)ed), PR_GetString(world->progs, ed->v->classname));
				World_Box3D_RemoveFromEntity(world, ed);
				return;
			}

			//CONCAVE dynamic prop -> one convex hull SHAPE per convex-decomposition piece on the single
			//body, so it collides on its TRUE concave shape.  A single hull of a concave rock is bloated
			//(it fills the dents) and collides with geometry the visual clears -> the carry servo fights
			//it -> flicker.  Box3D allows many shapes/body; the pieces are the engine's model->convhulls
			//(the same decomposition player/bullet collision uses, from the .acd sidecar / runtime ACD).
			//Reads convhulls directly (skips GenerateCollisionMesh).  A convex prop = 1 piece (~free).
			if (bodytype == b3_dynamicBody && physics_box3d_decomp->ival && model->convhulls &&
				model->numhulls > 0 && model->numhulls <= physics_box3d_maxpieces->ival)
			{
				int np = model->numhulls;
				b3HullData **hulls;
				int numok = 0, h;
				float totalvol = 0;
				if (np > 128) np = 128;						//load-time cap is ACD_ARRAY(128); be defensive
				hulls = (b3HullData**)BZ_Malloc(np * sizeof(b3HullData*));
				for (h = 0; h < np; h++)
				{
					const convhull_t *ch = &model->convhulls[h];
					int ptcount = ch->numtris * 3, i;	//piece surface tris (3 verts each) = its point cloud
					b3Vec3 *pts;
					b3HullData *ph = NULL;
					if (ptcount >= 6)					//>=2 tris; try the real hull first
					{
						pts = (b3Vec3*)BZ_Malloc(ptcount * sizeof(b3Vec3));
						for (i = 0; i < ptcount; i++)
						{	//model-space vert -> body-local: vert*scale - geomcenter (matches world.c
							//GenerateCollisionMesh_Hull; winding is irrelevant, b3CreateHull derives faces).
							pts[i].x = ch->tris[i][0]*scale - geomcenter[0];
							pts[i].y = ch->tris[i][1]*scale - geomcenter[1];
							pts[i].z = ch->tris[i][2]*scale - geomcenter[2];
						}
						ph = b3CreateHull(pts, ptcount, ptcount < 255 ? ptcount : 255);
						BZ_Free(pts);
					}
					if (!ph)
					{	//tris empty/decimated/degenerate -> a piece has NO usable surface tris, but it
						//ALWAYS has a tight model-space AABB.  Build a solid box from the 8 corners so the
						//piece is never dropped (a dropped piece = a hole = that part sinks through the floor).
						b3Vec3 corners[8];
						const float *pm = ch->mins, *px = ch->maxs;
						for (i = 0; i < 8; i++)
						{
							corners[i].x = ((i&1)?px[0]:pm[0])*scale - geomcenter[0];
							corners[i].y = ((i&2)?px[1]:pm[1])*scale - geomcenter[1];
							corners[i].z = ((i&4)?px[2]:pm[2])*scale - geomcenter[2];
						}
						ph = b3CreateHull(corners, 8, 8);
					}
					if (ph)
					{
						hulls[numok++] = ph;
						totalvol += ph->volume;
					}
				}
				if (numok > 0)
				{
					box3dgeom_t *g;
					Matrix4x4_RM_CreateTranslate(ed->rbe.offsetmatrix, geomcenter[0], geomcenter[1], geomcenter[2]);
					for (h = 0; h < numok; h++)
					{
						b3ShapeId s = b3CreateHullShape(body, &sd, hulls[h]);
						if (h == 0)
							shape = s;					//presence marker (b3DestroyBody frees all shapes)
					}
					g = Box3D_AllocGeom(numok);
					for (h = 0; h < numok; h++)
						g->ptr[h] = hulls[h];			//body now owns these hull blobs (freed on remove)
					ed->rbe.geomdata = g;
					BZ_Free(hulls);
					volume = totalvol;
					numpieces = numok;
					break;								//-> post-switch density/mass
				}
				BZ_Free(hulls);
				//0 usable pieces -> fall through to the single-hull path below.
			}

			if (!rbefuncs->GenerateCollisionMesh(world, model, ed, geomcenter))
			{
				//brush entity built no collision surfaces (a Source func_* with no faces).
				//leave physics=true with no shape (mins/maxs/modelindex were recorded above) so we
				//only retry if those change - do NOT RemoveFromEntity or we re-spam every frame.
				//(same reasoning as com_phys_ode.c)
				return;
			}
			Matrix4x4_RM_CreateTranslate(ed->rbe.offsetmatrix, geomcenter[0], geomcenter[1], geomcenter[2]);

			if (bodytype == b3_staticBody)
			{
				//static world/brush -> baked triangle mesh (Box3D meshes only collide on static bodies).
				//b3MeshDef has no internalValue and no default ctor, so a zero-init is correct.
				b3MeshData *mesh;
				box3dgeom_t *g;
				b3MeshDef md;
				memset(&md, 0, sizeof(md));
				md.vertices      = (b3Vec3*)ed->rbe.vertex3f;	//float[3] == b3Vec3
				md.indices       = ed->rbe.element3i;
				md.vertexCount   = ed->rbe.numvertices;
				md.triangleCount = ed->rbe.numtriangles;
				md.identifyEdges = true;
				mesh = b3CreateMesh(&md, NULL, 0);
				if (!mesh)
				{
					World_Box3D_RemoveFromEntity(world, ed);
					return;
				}
				g = Box3D_AllocGeom(1); g->ismesh = true; g->ptr[0] = mesh;	//free via b3DestroyMesh on remove
				ed->rbe.geomdata = g;
				shape = b3CreateMeshShape(body, &sd, mesh, (b3Vec3){1.0f,1.0f,1.0f});
				volume = geomsize[0]*geomsize[1]*geomsize[2];
			}
			else
			{
				//dynamic prop, no decomposition (single-mesh / convex) -> one convex hull from the
				//collision verts (Box3D has no dynamic trimesh).  Hull verts index with uint8 -> <=255.
				box3dgeom_t *g;
				int maxv = ed->rbe.numvertices < 255 ? ed->rbe.numvertices : 255;
				b3HullData *hull = b3CreateHull((const b3Vec3*)ed->rbe.vertex3f, ed->rbe.numvertices, maxv);
				if (!hull)
				{
					World_Box3D_RemoveFromEntity(world, ed);
					return;
				}
				g = Box3D_AllocGeom(1); g->ptr[0] = hull;					//free via b3DestroyHull on remove
				ed->rbe.geomdata = g;
				shape = b3CreateHullShape(body, &sd, hull);
				volume = hull->volume;
			}
			break;

		case GEOMTYPE_BOX:
			{
				b3BoxHull bh = b3MakeBoxHull(geomsize[0]*0.5f, geomsize[1]*0.5f, geomsize[2]*0.5f);
				Matrix4x4_RM_CreateTranslate(ed->rbe.offsetmatrix, geomcenter[0], geomcenter[1], geomcenter[2]);
				shape = b3CreateHullShape(body, &sd, &bh.base);	//box hull is a value, deep-copied; no geomdata
				volume = geomsize[0]*geomsize[1]*geomsize[2];
			}
			break;

		case GEOMTYPE_SPHERE:
			{
				b3Sphere s;
				radius = geomsize[0]*0.5f;
				s.center = b3Vec3_zero;
				s.radius = radius;
				Matrix4x4_RM_CreateTranslate(ed->rbe.offsetmatrix, geomcenter[0], geomcenter[1], geomcenter[2]);
				shape = b3CreateSphereShape(body, &sd, &s);
				volume = (4.0f/3.0f)*M_PI*radius*radius*radius;
			}
			break;

		case GEOMTYPE_CAPSULE:
		case GEOMTYPE_CAPSULE_X:
		case GEOMTYPE_CAPSULE_Y:
		case GEOMTYPE_CAPSULE_Z:
			{
				b3Capsule c;
				if (geomtype == GEOMTYPE_CAPSULE)
				{
					axisindex = 0;
					if (geomsize[axisindex] < geomsize[1]) axisindex = 1;
					if (geomsize[axisindex] < geomsize[2]) axisindex = 2;
				}
				else
					axisindex = geomtype-GEOMTYPE_CAPSULE_X;
				if (axisindex == 0)      radius = min(geomsize[1], geomsize[2]) * 0.5f;
				else if (axisindex == 1) radius = min(geomsize[0], geomsize[2]) * 0.5f;
				else                     radius = min(geomsize[0], geomsize[1]) * 0.5f;
				length = geomsize[axisindex] - radius*2;
				if (length <= 0) { radius -= (1 - length)*0.5f; length = 1; }
				//capsule axis is centered at geomcenter; endpoints along the chosen axis in body-local space.
				c.center1 = b3Vec3_zero;
				c.center2 = b3Vec3_zero;
				((float*)&c.center1)[axisindex] = -length*0.5f;
				((float*)&c.center2)[axisindex] =  length*0.5f;
				c.radius = radius;
				Matrix4x4_RM_CreateTranslate(ed->rbe.offsetmatrix, geomcenter[0], geomcenter[1], geomcenter[2]);
				shape = b3CreateCapsuleShape(body, &sd, &c);
				volume = M_PI*radius*radius*length + (4.0f/3.0f)*M_PI*radius*radius*radius;
			}
			break;

		case GEOMTYPE_CYLINDER:
		case GEOMTYPE_CYLINDER_X:
		case GEOMTYPE_CYLINDER_Y:
		case GEOMTYPE_CYLINDER_Z:
			{
				//Box3D's cylinder is a tessellated hull about the Y axis.  Reuse ODE's axis-orient
				//offset matrix so a Y-built cylinder lands on the requested axis.
				b3HullData *cyl;
				if (geomtype == GEOMTYPE_CYLINDER)
				{
					axisindex = 0;
					if (geomsize[axisindex] < geomsize[1]) axisindex = 1;
					if (geomsize[axisindex] < geomsize[2]) axisindex = 2;
				}
				else
					axisindex = geomtype-GEOMTYPE_CYLINDER_X;
				if (axisindex == 0)      { Matrix4x4_CM_ModelMatrix(ed->rbe.offsetmatrix, geomcenter[0],geomcenter[1],geomcenter[2], 0,0,90, 1); radius = min(geomsize[1],geomsize[2])*0.5f; }
				else if (axisindex == 1) { Matrix4x4_CM_ModelMatrix(ed->rbe.offsetmatrix, geomcenter[0],geomcenter[1],geomcenter[2], 90,0,0, 1); radius = min(geomsize[0],geomsize[2])*0.5f; }
				else                     { Matrix4x4_CM_ModelMatrix(ed->rbe.offsetmatrix, geomcenter[0],geomcenter[1],geomcenter[2], 0,0,0, 1);  radius = min(geomsize[0],geomsize[1])*0.5f; }
				length = geomsize[axisindex] - radius*2;
				if (length <= 0) { radius -= (1 - length)*0.5f; length = 1; }
				cyl = b3CreateCylinder(length, radius, -length*0.5f, 16);
				if (!cyl)
				{
					World_Box3D_RemoveFromEntity(world, ed);
					return;
				}
				{ box3dgeom_t *g = Box3D_AllocGeom(1); g->ptr[0] = cyl; ed->rbe.geomdata = g; }	//free via b3DestroyHull on remove
				shape = b3CreateHullShape(body, &sd, cyl);
				volume = M_PI*radius*radius*length;
			}
			break;

		default:
			Con_Printf("World_Box3D_BodyFromEntity: unrecognised solid %i\n", solid);
			World_Box3D_RemoveFromEntity(world, ed);
			return;
		}

		SETB3SHAPE(ed, shape);
		//pick a density so TOTAL mass ~= the QC .mass; Box3D derives inertia from geometry.  Set it on
		//EVERY shape (a concave prop has one per decomposition piece) using the total volume, so the
		//summed mass is massval regardless of piece count.
		if (volume < 1e-4f) volume = 1e-4f;
		{
			b3ShapeId shapes[128];
			int ns = b3Body_GetShapes(body, shapes, 128), k;
			for (k = 0; k < ns; k++)
				b3Shape_SetDensity(shapes[k], massval / volume, false);
		}
		b3Body_ApplyMassFromShapes(body);
		Matrix3x4_InvertTo4x4_Simple(ed->rbe.offsetmatrix, ed->rbe.offsetimatrix);

		if (physics_box3d_debug && physics_box3d_debug->ival)
			Con_Printf("[box3d] built ent %i (%s): solid %i geomtype %i, %s pieces=%i verts=%i tris=%i, %s, type=%s mass=%g vol=%g\n",
				NUM_FOR_EDICT(world->progs, (edict_t*)ed), PR_GetString(world->progs, ed->v->classname),
				solid, geomtype,
				(geomtype==GEOMTYPE_TRIMESH)?"mesh":"prim", numpieces, ed->rbe.numvertices, ed->rbe.numtriangles,
				b3Shape_IsValid(shape)?"shape OK":"SHAPE INVALID",
				(bodytype==b3_dynamicBody)?"dynamic":"static",
				b3Body_GetMass(body), volume);
	}

	if (!HAVEB3SHAPE(ed))
		return;						//e.g. a brush that produced no collision surfaces
	havebody = HAVEB3BODY(ed);
	body = B3BODY(ed);

	//------------------------------------------------------------------
	// push current qc transform/velocity into the body (only if the qc moved it)
	//------------------------------------------------------------------
	gravity = true;
	VectorCopy(ed->v->origin, origin);
	VectorCopy(ed->v->velocity, velocity);
	VectorCopy(ed->v->angles, angles);
	VectorCopy(ed->v->avelocity, avelocity);
	if (ed == world->edicts || (ed->xv->gravity && ed->xv->gravity <= 0.01))
		gravity = false;

	{
		vec3_t qangles, qavelocity;
		VectorCopy(angles, qangles);
		VectorCopy(avelocity, qavelocity);
		if (ed->v->modelindex)
		{
			model = world->Get_CModel(world, ed->v->modelindex);
			if (!model || model->type == mod_alias)
			{
				qangles[PITCH]     *= cvar_r_meshpitch->value;
				qavelocity[PITCH]  *= cvar_r_meshpitch->value;
			}
		}
		AngleVectorsFLU(qangles, forward, left, up);
		VectorSet(spinvelocity, DEG2RAD(qavelocity[PITCH]), DEG2RAD(qavelocity[ROLL]), DEG2RAD(qavelocity[YAW]));
	}

	switch (solid)
	{
	case SOLID_BBOX:
	case SOLID_SLIDEBOX:
	case SOLID_CORPSE:
		VectorSet(forward, 1, 0, 0);
		VectorSet(left, 0, 1, 0);
		VectorSet(up, 0, 0, 1);
		VectorSet(spinvelocity, 0, 0, 0);
		break;
	}

	//prevent NANs (IS_NAN needs an lvalue, so route each test through `test`, as ODE does)
	test = DotProduct(origin,origin) + DotProduct(forward,forward) + DotProduct(left,left) + DotProduct(up,up) + DotProduct(velocity,velocity) + DotProduct(spinvelocity,spinvelocity);
	if (IS_NAN(test))
	{
		modified = true;
		test = DotProduct(origin,origin);
		if (IS_NAN(test))
			VectorClear(origin);
		test = DotProduct(forward,forward) * DotProduct(left,left) * DotProduct(up,up);
		if (IS_NAN(test))
		{
			VectorSet(angles, 0, 0, 0);
			VectorSet(forward, 1, 0, 0);
			VectorSet(left, 0, 1, 0);
			VectorSet(up, 0, 0, 1);
		}
		test = DotProduct(velocity,velocity);
		if (IS_NAN(test))
			VectorClear(velocity);
		test = DotProduct(spinvelocity,spinvelocity);
		if (IS_NAN(test))
		{
			VectorClear(avelocity);
			VectorClear(spinvelocity);
		}
	}

	if (!VectorCompare(origin, ed->rbe.origin)
	 || !VectorCompare(velocity, ed->rbe.velocity)
	 || !VectorCompare(angles, ed->rbe.angles)
	 || !VectorCompare(avelocity, ed->rbe.avelocity)
	 || gravity != ed->rbe.gravity)
		modified = true;

	if (modified && havebody)
	{
		float entitymatrix[16];
		float bodymatrix[16];
		b3Quat q;
		b3Pos pos;

		VectorCopy(origin, ed->rbe.origin);
		VectorCopy(velocity, ed->rbe.velocity);
		VectorCopy(angles, ed->rbe.angles);
		VectorCopy(avelocity, ed->rbe.avelocity);
		ed->rbe.gravity = gravity;

		//fold the geomcenter offset into the transform (identical matrix ops to ODE)
		Matrix4x4_RM_FromVectors(entitymatrix, forward, left, up, origin);
		Matrix4_Multiply(ed->rbe.offsetmatrix, entitymatrix, bodymatrix);
		Matrix3x4_RM_ToVectors(bodymatrix, forward, left, up, origin);

		q = Box3D_QuatFromFLU(forward, left, up);
		pos.x = origin[0]; pos.y = origin[1]; pos.z = origin[2];
		b3Body_SetTransform(body, pos, q);

		if (movetype == MOVETYPE_PHYSICS)
		{
			b3Body_SetLinearVelocity(body, (b3Vec3){velocity[0], velocity[1], velocity[2]});
			b3Body_SetAngularVelocity(body, (b3Vec3){spinvelocity[0], spinvelocity[1], spinvelocity[2]});
			b3Body_SetGravityScale(body, gravity ? 1.0f : 0.0f);
			b3Body_SetAwake(body, true);
		}
	}
}

//----------------------------------------------------------------------------
// per-frame step  (mirror of World_ODE_Frame)
//----------------------------------------------------------------------------
static void QDECL World_Box3D_Frame(world_t *world, double frametime, double gravity)
{
	struct box3dctx_s *ctx = (struct box3dctx_s*)world->rbe;
	int i, want;
	wedict_t *ed;

	if (!(world->rbe_hasphysicsents || ctx->hasextraobjs))
		return;

	ctx->substeps = bound(1, physics_box3d_substeps->ival, 16);

	//honor live changes to the worker count (Box3D's own internal scheduler)
	want = bound(1, physics_box3d_threads->ival, B3_MAX_WORKERS);
	if (want != ctx->workercount)
	{
		b3World_SetWorkerCount(ctx->world, want);
		ctx->workercount = want;
	}

	//1) copy entities -> bodies (create / update / destroy)
	for (i = 0; i < world->num_edicts; i++)
	{
		ed = (wedict_t*)EDICT_NUM_PB(world->progs, i);
		if (!ED_ISFREE(ed))
			World_Box3D_Frame_BodyFromEntity(world, ed);
	}

	//2) drain the command queue (forces / torques / enable-disable, incl. the black hole)
	while (ctx->cmdqueuehead)
	{
		rbecommandqueue_t *cmd = ctx->cmdqueuehead;
		ctx->cmdqueuehead = cmd->next;
		if (!cmd->next)
			ctx->cmdqueuetail = NULL;
		World_Box3D_RunCmd(world, cmd);
		Z_Free(cmd);
	}

	//3) gravity + step.  Box3D sub-steps internally, so a SINGLE step call (unlike ODE's loop).
	b3World_SetGravity(ctx->world, (b3Vec3){0.0f, 0.0f, (float)-gravity});
	b3World_Step(ctx->world, (float)frametime, ctx->substeps);

	//4) copy bodies -> entities (skip worldspawn)
	if (world->rbe_hasphysicsents)
	{
		for (i = 1; i < world->num_edicts; i++)
		{
			ed = (wedict_t*)EDICT_NUM_PB(world->progs, i);
			if (!ED_ISFREE(ed))
				World_Box3D_Frame_BodyToEntity(world, ed);
		}
	}

	//per-second sim trace: is the world actually stepping bodies?  Reports the awake body count and
	//a sample dynamic prop's true physics Z (from the body, not the entity) so we can see it fall.
	if (physics_box3d_debug && physics_box3d_debug->ival)
	{
		static double dbgaccum = 0;
		dbgaccum += frametime;
		if (dbgaccum >= 1.0)
		{
			int sampleent = 0;
			float samplez = 0;
			b3Counters ct;
			b3Profile pr;
			dbgaccum = 0;
			for (i = 1; i < world->num_edicts; i++)
			{
				ed = (wedict_t*)EDICT_NUM_PB(world->progs, i);
				if (!ED_ISFREE(ed) && HAVEB3BODY(ed) && (int)ed->v->movetype == MOVETYPE_PHYSICS)
				{
					b3Pos p = b3Body_GetPosition(B3BODY(ed));
					sampleent = i;
					samplez = (float)p.z;
					break;
				}
			}
			ct = b3World_GetCounters(ctx->world);
			pr = b3World_GetProfile(ctx->world);
			//MULTITHREADING CONFIRMATION: `workers` = threads Box3D is using; `tasks` = parallel jobs
			//it forked this step (>1 with workers>1 => the solver IS running in parallel); solve/collide
			//in ms.  With workers=1 tasks stays ~1.  Push the body count up (spawnflood) to see tasks climb.
			//`pairs` is the broadphase (proxy update + pair-finding) time - THIS is what a big settling
			//pile of many-piece props costs (solve/collide stay ~0 once asleep).  Watch it spike while a
			//flood settles then drop to ~0 at rest; lower physics_box3d_maxpieces to shrink it.
			Con_Printf("[box3d] dt=%g awake=%i/%i bodies contacts=%i | workers=%i tasks=%i pairs=%.2fms collide=%.2fms solve=%.2fms step=%.2fms | sample ent %i z=%g\n",
				frametime, b3World_GetAwakeBodyCount(ctx->world), ct.bodyCount, ct.contactCount,
				b3World_GetWorkerCount(ctx->world), ct.taskCount, pr.pairs, pr.collide, pr.solve, pr.step,
				sampleent, samplez);
		}
	}
}

//----------------------------------------------------------------------------
// command queue  (mirror of World_ODE_PushCommand / World_ODE_RunCmd)
//----------------------------------------------------------------------------
static void QDECL World_Box3D_PushCommand(world_t *world, rbecommandqueue_t *val)
{
	struct box3dctx_s *ctx = (struct box3dctx_s*)world->rbe;
	rbecommandqueue_t *cmd = (rbecommandqueue_t*)BZ_Malloc(sizeof(*cmd));
	world->rbe_hasphysicsents = qtrue;
	memcpy(cmd, val, sizeof(*cmd));
	cmd->next = NULL;
	if (ctx->cmdqueuehead)
	{
		rbecommandqueue_t *ot = ctx->cmdqueuetail;
		ot->next = ctx->cmdqueuetail = cmd;
	}
	else
		ctx->cmdqueuetail = ctx->cmdqueuehead = cmd;
}

static void World_Box3D_RunCmd(world_t *world, rbecommandqueue_t *cmd)
{
	wedict_t *ed = cmd->edict;
	b3BodyId body;
	if (!ed || !HAVEB3BODY(ed))
		return;
	body = B3BODY(ed);
	switch(cmd->command)
	{
	case RBECMD_ENABLE:
		b3Body_Enable(body);
		b3Body_SetAwake(body, true);
		break;
	case RBECMD_DISABLE:
		b3Body_Disable(body);
		break;
	case RBECMD_FORCE:
		//the gravity-gun black hole: physics_addforce(e, force, e.origin).
		//force is ignored if the body is asleep -> must wake it (last arg true).
		b3Body_ApplyForce(body, (b3Vec3){cmd->v1[0], cmd->v1[1], cmd->v1[2]},
		                        (b3Pos){cmd->v2[0], cmd->v2[1], cmd->v2[2]}, true);
		break;
	case RBECMD_TORQUE:
		b3Body_ApplyTorque(body, (b3Vec3){cmd->v1[0], cmd->v1[1], cmd->v1[2]}, true);
		break;
	}
}

//----------------------------------------------------------------------------
// ragdoll + joint entry points - STUBBED in v1 (props do not use these)
//----------------------------------------------------------------------------
static void QDECL World_Box3D_RemoveJointFromEntity(world_t *world, wedict_t *ed)
{
	ed->rbe.joint_type = 0;
	ed->rbe.joint.joint = NULL;
}
static qboolean QDECL World_Box3D_RagMatrixToBody(rbebody_t *bodyptr, float *mat)			{ return false; }
static qboolean QDECL World_Box3D_RagCreateBody(world_t *world, rbebody_t *bodyptr, rbebodyinfo_t *bodyinfo, float *mat, wedict_t *ent)	{ return false; }
static void QDECL World_Box3D_RagMatrixFromJoint(rbejoint_t *joint, rbejointinfo_t *info, float *mat)	{ }
static void QDECL World_Box3D_RagMatrixFromBody(world_t *world, rbebody_t *bodyptr, float *mat)
{
	//write identity so a caller rendering ragdoll bones gets a stable matrix, not garbage.
	Matrix4x4_Identity(mat);
}
static void QDECL World_Box3D_RagEnableJoint(rbejoint_t *joint, qboolean enabled)			{ }
static void QDECL World_Box3D_RagCreateJoint(world_t *world, rbejoint_t *joint, rbejointinfo_t *info, rbebody_t *body1, rbebody_t *body2, vec3_t aaa2[3])	{ }
static void QDECL World_Box3D_RagDestroyBody(world_t *world, rbebody_t *bodyptr)			{ }
static void QDECL World_Box3D_RagDestroyJoint(world_t *world, rbejoint_t *joint)			{ }

//----------------------------------------------------------------------------
// start / init
//----------------------------------------------------------------------------
static void QDECL World_Box3D_Start(world_t *world)
{
	struct box3dctx_s *ctx;
	b3WorldDef wd;

	if (world->rbe)
		return;

	if (b3IsDoublePrecision())
	{
		Con_Printf("Box3D plugin: library is double precision, incompatible. Not starting.\n");
		return;
	}

	//Tell Box3D the Quake unit scale BEFORE creating the world or any shape.  Box3D (like Box2D) is
	//tuned for metres: its slop / speculative-contact distance / sleep threshold / contact push-out
	//speed all scale off this global (default 1.0 = "1 unit is 1 metre").  A Quake barrel is ~64 units
	//and ~40 units make a real metre, so at the default scale the tolerances are ~40x too small
	//(contacts only seen within 0.02 QU, overlap resolved at only 3 QU/s) and props jam in place / never
	//fall - they look frozen.  Setting the scale rescales every tolerance to Quake size at once.  ODE
	//has no such concept, which is why it works with raw Quake units unchanged.
	b3SetLengthUnitsPerMeter(bound(1.0f, physics_box3d_unitscale->value, 1024.0f));

	ctx = BZ_Malloc(sizeof(*ctx));
	memset(ctx, 0, sizeof(*ctx));
	world->rbe = &ctx->pub;

	ctx->substeps = bound(1, physics_box3d_substeps->ival, 16);
	ctx->workercount = bound(1, physics_box3d_threads->ival, B3_MAX_WORKERS);

	wd = b3DefaultWorldDef();
	wd.gravity = (b3Vec3){0.0f, 0.0f, 0.0f};				//set per-frame in RunFrame
	wd.enableSleep = physics_box3d_autodisable->ival ? true : false;
	wd.enableContinuous = true;								//CCD dynamic-vs-static, prevents tunnelling
	wd.workerCount = ctx->workercount;						//>1 turns on Box3D's OWN internal thread scheduler
	//enqueueTask/finishTask left NULL -> Box3D creates + owns its worker threads (native Win32 CreateThread).
	ctx->world = b3CreateWorld(&wd);

	if (physics_box3d_maxlinearspeed->value > 0)
		b3World_SetMaximumLinearSpeed(ctx->world, physics_box3d_maxlinearspeed->value);

	ctx->pub.End					= World_Box3D_End;
	ctx->pub.RemoveJointFromEntity	= World_Box3D_RemoveJointFromEntity;
	ctx->pub.RemoveFromEntity		= World_Box3D_RemoveFromEntity;
	ctx->pub.RagMatrixToBody		= World_Box3D_RagMatrixToBody;
	ctx->pub.RagCreateBody			= World_Box3D_RagCreateBody;
	ctx->pub.RagMatrixFromJoint		= World_Box3D_RagMatrixFromJoint;
	ctx->pub.RagMatrixFromBody		= World_Box3D_RagMatrixFromBody;
	ctx->pub.RagEnableJoint			= World_Box3D_RagEnableJoint;
	ctx->pub.RagCreateJoint			= World_Box3D_RagCreateJoint;
	ctx->pub.RagDestroyBody			= World_Box3D_RagDestroyBody;
	ctx->pub.RagDestroyJoint		= World_Box3D_RagDestroyJoint;
	ctx->pub.RunFrame				= World_Box3D_Frame;
	ctx->pub.PushCommand			= World_Box3D_PushCommand;
	ctx->pub.Trace					= NULL;					//engine null-checks it (ODE leaves it unset too)

	Con_Printf("Box3D physics started (%i worker thread%s, %i substeps, %g units/metre).\n",
		ctx->workercount, ctx->workercount==1?"":"s", ctx->substeps, b3GetLengthUnitsPerMeter());
}

static void QDECL Plug_Box3D_Shutdown(void)
{
	if (rbefuncs)
		rbefuncs->UnregisterPhysicsEngine("Box3D");
}

static void Box3D_RegisterCvars(void)
{
	cvar_r_meshpitch			= cvarfuncs->GetNVFDG("r_meshpitch",				"1",	0, NULL, NULL);	//mathlib VectorAngles(meshpitch) needs this; also our alias-model pitch sign
	cvar_r_meshroll				= cvarfuncs->GetNVFDG("r_meshroll",					"1",	0, NULL, NULL);
	physics_box3d_threads		= cvarfuncs->GetNVFDG("physics_box3d_threads",		"1",	0, "Worker threads for Box3D's internal multicore scheduler. 1 = single-threaded; 2-8 = multicore (each firing runs the solver's fork/join in parallel). Changing this live re-sizes the pool.", "Box3D Physics");
	physics_box3d_substeps		= cvarfuncs->GetNVFDG("physics_box3d_substeps",		"4",	0, "Solver sub-steps per world step; higher = more accurate/stable stacking, more CPU.", "Box3D Physics");
	physics_box3d_autodisable	= cvarfuncs->GetNVFDG("physics_box3d_autodisable",	"1",	0, "Let settled bodies sleep so a pile of resting props costs ~0.", "Box3D Physics");
	physics_box3d_maxlinearspeed= cvarfuncs->GetNVFDG("physics_box3d_maxlinearspeed","0",	0, "Clamp on body speed to stop fly-aways; 0 = Box3D default.", "Box3D Physics");
	physics_box3d_unitscale		= cvarfuncs->GetNVFDG("physics_box3d_unitscale",	"40",	0, "Quake units per real metre. Box3D's collision tolerances are metre-tuned; at Quake scale the default (1) makes them ~40x too tight and props jam/freeze. 40 suits ~64-unit props. Set before/at map (re)start. Raise if props jitter; lower if they sink/overlap.", "Box3D Physics");
	physics_box3d_decomp		= cvarfuncs->GetNVFDG("physics_box3d_decomp",		"1",	0, "1 = CONCAVE dynamic props (rocks etc.) use the engine's convex DECOMPOSITION as many hull shapes on one body, so they collide + carry on their true shape (convex props decompose to 1 piece = ~free). 0 = one convex hull per prop (cheaper for a giant pile, but concave props collide bloated). Reload/re-spawn to apply.", "Box3D Physics");
	physics_box3d_maxpieces		= cvarfuncs->GetNVFDG("physics_box3d_maxpieces",	"32",	0, "Cap on decomposition pieces (= collision shapes) per prop. A prop that splits into more than this uses a single hull instead. Each piece is its own broadphase proxy, so this bounds the per-frame cost of a big settling pile: lower it (e.g. 4) for smoother floods (coarser concave collision), raise it for the most accurate carried props. Reload/re-spawn to apply.", "Box3D Physics");
	physics_box3d_debug			= cvarfuncs->GetNVFDG("physics_box3d_debug",		"0",	0, "1 = trace each physics body's build (verts/hull/mass/type) + a per-second awake-count/fall sample to the console.", "Box3D Physics");
}

qboolean Plug_Init(void)
{
	rbefuncs = plugfuncs->GetEngineInterface(plugrbefuncs_name, sizeof(*rbefuncs));
	if (rbefuncs && (rbefuncs->version < RBEPLUGFUNCS_VERSION || rbefuncs->wedictsize != sizeof(wedict_t)))
		rbefuncs = NULL;
	if (!rbefuncs)
	{
		Con_Printf("Box3D plugin failed: engine is incompatible.\n");
		return false;
	}

	Box3D_RegisterCvars();

	if (!rbefuncs->RegisterPhysicsEngine)
		Con_Printf("Box3D plugin failed: engine doesn't support physics-engine plugins.\n");
	else if (!rbefuncs->RegisterPhysicsEngine("Box3D", World_Box3D_Start))
		Con_Printf("Box3D plugin failed: a physics plugin is already active.\n");
	else
	{
		plugfuncs->ExportFunction("Shutdown", Plug_Box3D_Shutdown);
		return true;
	}
	return false;
}
#endif	//USERBE
