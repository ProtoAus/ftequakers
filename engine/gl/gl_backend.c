#include "quakedef.h"

//#define FORCESTATE

void DumpGLState(void);

#ifdef GLQUAKE

#define r_refract_fboival (gl_config.ext_framebuffer_objects && r_refract_fbo.ival)

#include "glquake.h"
#include "shader.h"

#ifdef FORCESTATE
#pragma warningmsg("FORCESTATE is active")
#endif

#ifdef ANDROID
/*android appears to have a bug, and requires f and not i*/
#define qglTexEnvi qglTexEnvf
#endif

extern cvar_t gl_overbright;
extern cvar_t r_multidraw;	//FTESurf Patch 831
extern cvar_t r_instancing;	//FTESurf Patch 831
extern cvar_t r_skyroomneed;	//FTESurf Patch 850
extern cvar_t r_waterskyroom, r_waterforcereflectentities;	//FTESurf Patch 868
int r_capturenoents;	//FTESurf Patch 868: the reflection capture being rendered draws only the world
int r_noentsfirstvisedict;	//...and the firstvisedict it replaced, for a 3D sky drawn inside it
extern unsigned int r_vs_skydrawn, r_vs_skyskipped;
extern qboolean r_fakeshadows;
#ifdef RTLIGHTS
qboolean Sh_AnyLightToDraw(void);
#else
#define Sh_AnyLightToDraw() false
#endif
//FTESurf Patch 831: instanced props, see GLBE_SubmitInstancedRun.
#define INST_TEXELS	18			//RGBA32F texels a prop, laid out there
#define INST_TMU_DATA	14			//texture units the two buffers sit on; a unit's buffer target is ours alone
#define INST_TMU_VC	15
static int be_instancecount;		//>1 while the batch being submitted stands for a run
static int be_instancebase;		//its first texel in inst_buf
static qboolean be_instancefailed;	//something in it drew for the first prop alone: the caller draws the rest
static qboolean be_instdraw;		//the program BE_RenderMeshProgram just bound is the INSTANCED one
static void GLBE_BindInstanceData(const struct programpermu_s *permu);
static void GLBE_InstanceReset(void);
extern cvar_t r_tessellation;
extern cvar_t r_wireframe;
extern cvar_t r_outline;
extern cvar_t r_outline_width;
extern cvar_t r_refract_fbo;
extern cvar_t r_sky_forcedepth_push;	//FTESurf Patch 768, gl_warp.c
extern cvar_t gl_line_width;				//nettest: CSQC debug wireframe line width (3D scene polys / R_DB_Poly)
extern cvar_t gl_2dline_width;				//nettest: 2D drawline width (hit-marker reticle / dummybatch)
extern void R_DB_Poly(batch_t *batch);			//nettest: gl_alias.c scenetris builder; tags CSQC debug-line batches
extern shader_t *shader_draw_fill_trans;
#ifndef GL_ALIASED_LINE_WIDTH_RANGE
#define GL_ALIASED_LINE_WIDTH_RANGE 0x846E
#endif
static float gl_maxlinewidth = 8;			//nettest: GL_ALIASED_LINE_WIDTH_RANGE[1], cached in GLBE_Init

extern texid_t missing_texture;
extern texid_t missing_texture_gloss;
extern texid_t missing_texture_normal;
extern texid_t scenepp_postproc_cube;
extern texid_t r_whiteimage;
extern texid_t r_blackimage;	//nettest: SUNVIS fallback — black = zero sun occlusion = shadows behave as before
extern texid_t r_envcubemap_tex;	//FTESurf Patch 268 B: the $envcubemap sentinel cube (gl_shader.c)

//FTESurf Patch: declared here rather than in render.h, following the texid_t
//externs directly above.  Two backends and renderer.c is not worth a header
//change, which would turn a -Engine build of a shared tree into a -Full one.
extern cvar_t r_reflectcube;
extern cvar_t r_envcubemap;			//FTESurf Patch 268 B: runtime gate for sentinel-only reflections
extern cvar_t r_reflectcube_nosky;	//FTESurf Patch 268 B: neutral grey instead of sky/black when the batch envmap is missing

/*
FTESurf Patch: the census behind r_reflectcube, and it is the CONSUMER that is
counted rather than the loader -- the build 19 lesson about r_voidvis, where
"the setting looks the same either way" turned out to mean "there was nothing
out there to drop at any setting".  A switch that cannot be shown to do anything
is worse than no switch.

Counted here because this is the only place the decision is made.  Read and
zeroed by shader_here, so the number is "batches since you last asked", which
over a second of frames is a clear signal and costs one increment per batch.
*/
int r_reflectcube_used;		//batches that took a cubemap reflection
int r_reflectcube_gated;	//batches that would have, and were stopped
//FTESurf Patch 268 B: the same pair for the $envcubemap sentinel alone, so
//"the ramp reflects now" is a countable subset of the line above rather than a
//number that moved for some other reason.
int r_envcubemap_used;		//of those, ones whose reflectcube was the sentinel
int r_envcubemap_gated;		//sentinel batches stopped by r_envcubemap 0
//FTESurf Patch 268 C: uploads of a non-empty ambient cube, and the ones r_cubelight 0 replaced
//with zeros.  Counted per uniform upload rather than per batch, so read it as a 0-versus-not-0
//falsifier, not a tally.  The cvar is declared here beside its counters for the reason above.
extern cvar_t r_cubelight;
int r_cubelight_used;
int r_cubelight_gated;

#ifdef GLQUAKE
static texid_t shadowmap[3];
static int shadow_fbo_id;
static int shadow_fbo_depth_num;
#endif

#ifndef GLSLONLY
static void GenerateTCMods(const shaderpass_t *pass, int passnum);
#endif

static const char LIGHTPASS_SHADER[] = "\
{\n\
	program rtlight%s\n\
\
	{\n\
		map $diffuse\n\
		nodepth\n\
		blendfunc add\n\
	}\n\
	{\n\
		map $normalmap\n\
	}\n\
	{\n\
		map $specular\n\
	}\n\
	{\n\
		map $lightcubemap\n\
	}\n\
	{\n\
		map $shadowmap\n\
	}\n\
	{\n\
		map $loweroverlay\n\
	}\n\
	{\n\
		map $upperoverlay\n\
	}\n\
}";

extern cvar_t r_glsl_offsetmapping, r_portalrecursion, r_portalonly, r_portaldebug;
extern cvar_t r_portalscissor;	//FTESurf Patch 208

static void BE_SendPassBlendDepthMask(unsigned int sbits);
void GLBE_SubmitBatch(batch_t *batch);
#ifdef RTLIGHTS
static qboolean GLBE_RegisterLightShader(int mode);
#endif

static struct {
	//internal state
	struct {
		int lastpasstmus;
//		int vbo_colour;
//		int vbo_texcoords[SHADER_PASS_MAX];
//		int vbo_deforms;	//holds verticies... in case you didn't realise.

		const shader_t *shader_light[LSHADER_MODES];
		qboolean inited_shader_light[LSHADER_MODES];

		const shader_t *crepskyshader;
		const shader_t *crepopaqueshader;
		const shader_t *depthonlyshader;
		const shader_t *wireframeshader;

		union programhandle_u	allblackshader;
		int allblack_mvp;

		program_t *programfixedemu[8];

		texid_t tex_gbuf[GBUFFER_COUNT];
		int fbo_current;	//the one currently being rendered to
		int coverage;	//FTESurf Patch 871: the blend last sent split its alpha factors (r2d_coverage)
		texid_t tex_sourcecol; /*this is used by $sourcecolour tgen*/
		texid_t tex_sourcedepth;
		texid_t tex_reflectcube;/*used where $reflectcube was invalid or failed*/
		fbostate_t fbo_2dfbo;
		fbostate_t fbo_reflectrefrac[R_MAX_RECURSE];
		fbostate_t fbo_lprepass;
		texid_t tex_reflection[R_MAX_RECURSE];	/*basically a portal rendered to texture*/
		texid_t tex_refraction[R_MAX_RECURSE];	/*the (culled) underwater view*/
		texid_t tex_refractiondepth[R_MAX_RECURSE];	/*the (culled) underwater view*/
		texid_t tex_ripplemap[R_MAX_RECURSE];	/*temp image for waves and things.*/

		int curpatchverts;
		qboolean force2d;
		int currenttmu;
		int blendmode[SHADER_TMU_MAX];
		int texenvmode[SHADER_TMU_MAX];
		int currenttextures[SHADER_TMU_MAX];
		GLenum curtexturetype[SHADER_TMU_MAX];

		polyoffset_t curpolyoffset;
		unsigned int curcull;
		texid_t curshadowmap;

		unsigned int shaderbits;
		unsigned int sha_attr;
		int currentprogram;
		int lastuniform; /*program which was last set, so using the same prog for multiple surfaces on the same ent (ie: world) does not require lots of extra uniform chnges*/

		batch_t dummybatch;
		vbo_t dummyvbo;
		int currentvbo;
		int currentebo;
		int currentvao;

		mesh_t **meshes;
		unsigned int meshcount;
		float modelmatrix[16];
		float modelmatrixinv[16];
		float modelviewmatrix[16];
		float projectionmatrix[16];

		int colourarraytype;
		vec4_t pendingcolourflat;
		//FTESurf Patch 166.  pendingcolourflat has ALWAYS been a leftover as far as
		//the GLSL path is concerned -- nothing cleared it and nothing bound it -- so
		//v_colour could not be trusted from it.  This says "GenerateColourMods just
		//wrote it, for this draw".  Cleared per program draw, set only by the flat
		//branches of GenerateColourMods, and read only where there is no array at
		//all; a program that never asks for colourgens sees exactly the old white.
		qboolean pendingcolourflatvalid;
		int pendingcolourvbo;
		void *pendingcolourpointer;
		int curcolourvbo;
		void *curcolourpointer;

		int pendingvertexvbo;
		void *pendingvertexpointer;
		int curvertexvbo;
		void *curvertexpointer;

		int streamvbo[64];
		int streamebo[64];
		int streamvao[64];
		int streamid;

		int pendingtexcoordparts[SHADER_TMU_MAX];
		int pendingtexcoordvbo[SHADER_TMU_MAX];
		void *pendingtexcoordpointer[SHADER_TMU_MAX];

		float identitylighting;	//set to how bright world lighting should be (reduced by realtime_world_lightmaps)
		float identitylightmap;	//set to how bright lightmaps should be (reduced by overbrights+realtime_world_lightmaps)
		polyoffset_t polyoffset; //mode-specific polygon offsets...

		texid_t temptexture; //$current
		texid_t fogtexture;
		texid_t normalisationcubemap;
		float fogfar;
		int usingweaponviewmatrix;

		batch_t **mbatches;	//model batches (ie: not world)
	};

	//exterior state (paramters)
	struct {
		backendmode_t mode;
		unsigned int flags;
		int oldwidth, oldheight;

		vbo_t *sourcevbo;
		const material_t *curshader;
		qboolean polymodeowned;	//FTESurf Patch 780: r_wireframe/gl_outline hold glPolygonMode for a whole pass
		const entity_t *curentity;
		const batch_t *curbatch;
		const texnums_t *curtexnums;
		const mfog_t *fog;
		const dlight_t *curdlight;

		float curtime;
		float updatetime;

		int lightmode;
		vec3_t lightorg;
		vec3_t lightdir;
		vec3_t lightcolours;
		vec3_t lightcolourscale;
		float lightradius;
		texid_t lighttexture;
		texid_t lightcubemap;
		float lightprojmatrix[16]; /*world space*/
		vec2_t lightshadowmapscale;
		vec4_t lightshadowmapinfo;
		vec4_t lightshadowmapproj;
	};

	//nettest P110: per-slot fake-shadow projections.  Snapshots of lightprojmatrix taken as gl_shadow.c
	//walks the atlas slots, so each slot's consumption matrix is BY CONSTRUCTION the same matrix the
	//legacy single path uses (built in GLBE_SelectDLight's ORTHO branch, r_shadows_bias already baked
	//into [14]) -- it cannot drift out of agreement with the shader.
	float fakeshadowmatrix[MAX_FAKESHADOW_SLOTS][16];
	vec4_t fakeshadowcell[MAX_FAKESHADOW_SLOTS];
	vec4_t fakeshadowinfo[MAX_FAKESHADOW_SLOTS];	//nettest: per-slot metadata (x = sun-suppression for lamp cells)
	int fakeshadowcount;

	int wbatch;
	int maxwbatches;
	batch_t *wbatches;
} shaderstate;

static texnums_t r_nulltexnums;	//nettest: zeroed fallback for shaderstate.curtexnums when a shader is registered-but-not-yet-generated (defaulttextures==NULL, e.g. a Source prop's GLSL material on first frame). TEXLOADED() on all-zero texids is false, so every downstream deref (BE_RenderMeshProgram/DrawPass) safely binds nothing instead of NULL-faulting.

#ifdef _DEBUG
#define DRAWCALL(f) if (sh_config.showbatches) BE_PrintDrawCall(f)
#include "pr_common.h"
static void BE_PrintDrawCall(const char *msg)
{
	char shadername[512];
	char modelname[512];
	Q_snprintfz(shadername, sizeof(shadername), "^[%-16s\\tipimg\\%s\\tipimgtype\\%i\\tip\\%s^]",
			shaderstate.curshader->name,
			shaderstate.curshader->name,shaderstate.curshader->usageflags,
			shaderstate.curshader->name);

	if (shaderstate.curbatch && shaderstate.curbatch->ent)
	{
#ifdef HAVE_SERVER
		int num = shaderstate.curbatch->ent->keynum;
#endif
		if (shaderstate.curbatch->ent->model)
			Q_snprintfz(modelname, sizeof(modelname), " - ^[%s\\modelviewer\\%s^]",
				shaderstate.curbatch->ent->model->name, shaderstate.curbatch->ent->model->name);
		else
			*modelname = 0;
#ifdef HAVE_SERVER
		if (num >= 1 && num < sv.world.num_edicts)
			Con_Printf("%s shader %s ent %i%s - \"%s\"\n", msg, shadername, shaderstate.curbatch->ent->keynum, modelname, sv.world.progs->StringToNative(sv.world.progs, (WEDICT_NUM_PB(sv.world.progs, num))->v->classname));
		else
#endif
			Con_Printf("%s shader %s ent %i%s\n", msg, shadername, shaderstate.curbatch->ent->keynum, modelname);
	}
	else
		Con_Printf("%s shader %s\n", msg, shadername);
}
#else
#define DRAWCALL(f)
#endif

static void BE_PolyOffset(void)
{
	polyoffset_t po;
	po.factor = shaderstate.curshader->polyoffset.factor;
	po.unit = shaderstate.curshader->polyoffset.unit;

#ifdef BEF_PUSHDEPTH
	if (shaderstate.flags & BEF_PUSHDEPTH)
	{
		/*some quake doors etc are flush with the walls that they're meant to be hidden behind, or plats the same height as the floor, etc
		we move them back very slightly using polygonoffset to avoid really ugly z-fighting*/
		extern cvar_t r_polygonoffset_submodel_offset, r_polygonoffset_submodel_factor;
		po.factor += r_polygonoffset_submodel_factor.value;
		po.unit += r_polygonoffset_submodel_offset.value;
	}
#endif
	po.factor += shaderstate.polyoffset.factor;
	po.unit += shaderstate.polyoffset.unit;
	if ((shaderstate.flags & BEF_SKYDEPTH) && r_sky_forcedepth_push.value > 0)
	{	//FTESurf Patch 768: away from the eye, whichever way the depth test runs. Steps only:
		//a slope term grows without bound as the eye nears a sky plane (review).
		extern int gldepthfunc;
		po.unit += ((gldepthfunc == GL_LEQUAL) ? 1 : -1) * r_sky_forcedepth_push.value;
	}

#ifndef FORCESTATE
	if (shaderstate.curpolyoffset.factor != po.factor || shaderstate.curpolyoffset.unit != po.unit)
#endif
	{
		shaderstate.curpolyoffset = po;
		if (shaderstate.curpolyoffset.factor || shaderstate.curpolyoffset.unit)
		{
			qglEnable(GL_POLYGON_OFFSET_FILL);
			qglPolygonOffset(shaderstate.curpolyoffset.factor, shaderstate.curpolyoffset.unit);
		}
		else
			qglDisable(GL_POLYGON_OFFSET_FILL);
	}
}

void GLBE_PolyOffsetStencilShadow
					#ifdef BEF_PUSHDEPTH
							(qboolean pushdepth)
					#else
							(void)
					#endif
{
	extern cvar_t r_polygonoffset_stencil_offset, r_polygonoffset_stencil_factor;
	polyoffset_t po;
	po.factor = r_polygonoffset_stencil_factor.value;
	po.unit = r_polygonoffset_stencil_offset.value;

#ifdef BEF_PUSHDEPTH
	if (pushdepth)
	{
		/*some quake doors etc are flush with the walls that they're meant to be hidden behind, or plats the same height as the floor, etc
		we move them back very slightly using polygonoffset to avoid really ugly z-fighting*/
		extern cvar_t r_polygonoffset_submodel_offset, r_polygonoffset_submodel_factor;
		po.factor += r_polygonoffset_submodel_factor.value;
		po.unit += r_polygonoffset_submodel_offset.value;
	}
#endif

#ifndef FORCESTATE
	if (shaderstate.curpolyoffset.factor != po.factor || shaderstate.curpolyoffset.unit != po.unit)
#endif
	{
		shaderstate.curpolyoffset = po;
		if (shaderstate.curpolyoffset.factor || shaderstate.curpolyoffset.unit)
		{
			qglEnable(GL_POLYGON_OFFSET_FILL);
			qglPolygonOffset(shaderstate.curpolyoffset.factor, shaderstate.curpolyoffset.unit);
		}
		else
			qglDisable(GL_POLYGON_OFFSET_FILL);
	}
}
static void GLBE_PolyOffsetShadowMap(void)
{
	extern cvar_t r_polygonoffset_shadowmap_offset, r_polygonoffset_shadowmap_factor;
	polyoffset_t po;
#if 0//def BEF_PUSHDEPTH
	if (pushdepth)
	{
		/*some quake doors etc are flush with the walls that they're meant to be hidden behind, or plats the same height as the floor, etc
		we move them back very slightly using polygonoffset to avoid really ugly z-fighting*/
		extern cvar_t r_polygonoffset_submodel_offset, r_polygonoffset_submodel_factor;
		po.factor = r_polygonoffset_submodel_factor.value + r_polygonoffset_shadowmap_factor.value;
		po.unit = r_polygonoffset_submodel_offset.value + r_polygonoffset_shadowmap_offset.value;
	}
	else
#endif
	{
		po.factor = r_polygonoffset_shadowmap_factor.value;
		po.unit = r_polygonoffset_shadowmap_offset.value;
	}

#ifndef FORCESTATE
	if (shaderstate.curpolyoffset.factor != po.factor || shaderstate.curpolyoffset.unit != po.unit)
#endif
	{
		shaderstate.curpolyoffset = po;
		if (shaderstate.curpolyoffset.factor || shaderstate.curpolyoffset.unit)
		{
			qglEnable(GL_POLYGON_OFFSET_FILL);
			qglPolygonOffset(shaderstate.curpolyoffset.factor, shaderstate.curpolyoffset.unit);
		}
		else
			qglDisable(GL_POLYGON_OFFSET_FILL);
	}
}

#ifndef GLSLONLY
void GL_TexEnv(GLenum mode)
{
#ifndef FORCESTATE
	if (mode != shaderstate.texenvmode[shaderstate.currenttmu])
#endif
	{
		qglTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
		shaderstate.texenvmode[shaderstate.currenttmu] = mode;
	}
}

static void BE_SetPassBlendMode(int tmu, int pbm)
{
#ifndef FORCESTATE
	if (shaderstate.blendmode[tmu] != pbm)
#endif
	{
		shaderstate.blendmode[tmu] = pbm;
#ifndef FORCESTATE
		if (shaderstate.currenttmu != tmu)
#endif
			GL_SelectTexture(tmu);

		switch (pbm)
		{
		case PBM_DOTPRODUCT:
			GL_TexEnv(GL_COMBINE_ARB);
			qglTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_TEXTURE);
			qglTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_PREVIOUS_ARB);
			qglTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_DOT3_RGB_ARB);
			qglTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 1);
			break;
		case PBM_MODULATE_PREV_COLOUR:
			GL_TexEnv(GL_COMBINE_ARB);
			qglTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_PRIMARY_COLOR_ARB);
			qglTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_PREVIOUS_ARB);
			qglTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);
			qglTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 1);
			break;
		case PBM_REPLACELIGHT:
		//	if (shaderstate.identitylighting != 1)
				goto forcemod;
			GL_TexEnv(GL_REPLACE);
			break;
		case PBM_REPLACE:
			GL_TexEnv(GL_REPLACE);
			break;
		case PBM_DECAL:
			if (tmu == 0)
				goto forcemod;
			GL_TexEnv(GL_DECAL);
			break;
		case PBM_ADD:
			if (tmu == 0)
				goto forcemod;
			GL_TexEnv(GL_ADD);
			break;
		case PBM_OVERBRIGHT:
			GL_TexEnv(GL_COMBINE_ARB);
			qglTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_TEXTURE);
			qglTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_PREVIOUS_ARB);
			qglTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);
			qglTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 1<<gl_overbright.ival);
			break;
		default:
		case PBM_MODULATE:
		forcemod:
			GL_TexEnv(GL_MODULATE);
			break;
		}
	}
}
#endif

/*OpenGL requires glDepthMask(GL_TRUE) or glClear(GL_DEPTH_BUFFER_BIT) will fail*/
void GL_ForceDepthWritable(void)
{
#ifndef FORCESTATE
	if (!(shaderstate.shaderbits & SBITS_MISC_DEPTHWRITE))
#endif
	{
		shaderstate.shaderbits |= SBITS_MISC_DEPTHWRITE;
		qglDepthMask(GL_TRUE);
	}

	qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	shaderstate.shaderbits |= SBITS_MASK_BITS;
}

void GL_SetShaderState2D(qboolean is2d)
{
	shaderstate.usingweaponviewmatrix = -1;	//force projection matrix info to get reset
	shaderstate.updatetime = realtime;
	shaderstate.force2d = is2d;
	if (is2d)
	{
		memcpy(shaderstate.modelviewmatrix, r_refdef.m_view, sizeof(shaderstate.modelviewmatrix));
		if (qglLoadMatrixf)
			qglLoadMatrixf(r_refdef.m_view);
	}
	BE_SelectMode(BEM_STANDARD);


//	if (cl.paused || cls.state < ca_active)
		shaderstate.updatetime = r_refdef.time;
//	else
//		shaderstate.updatetime = cl.servertime;
	BE_SelectEntity(&r_worldentity);
	shaderstate.curtime = shaderstate.updatetime - shaderstate.curentity->shaderTime;
}

void GL_SelectTexture(int target)
{
	shaderstate.currenttmu = target;
	if (qglActiveTextureARB)
		qglActiveTextureARB(target + mtexid0);
}

void GL_SelectVBO(int vbo)
{
#ifndef FORCESTATE
	if (shaderstate.currentvbo != vbo)
#endif
	{
		shaderstate.currentvbo = vbo;
		qglBindBufferARB(GL_ARRAY_BUFFER_ARB, shaderstate.currentvbo);
	}
}
void GL_DeselectVAO(void)
{
	if (shaderstate.currentvao)
	{
		qglBindVertexArray(0);
		shaderstate.currentvao = 0;
	}
}
void GL_SelectEBO(int vbo)
{
	//EBO is part of the current VAO, so keep things matching that
#ifndef FORCESTATE
	if (shaderstate.currentebo != vbo)
#endif
	{
		shaderstate.currentebo = vbo;
		qglBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, shaderstate.currentebo);
	}
}

void GL_MTBind(int tmu, int target, texid_t texnum)
{
	GL_SelectTexture(tmu);

#ifndef FORCESTATE
	if (shaderstate.currenttextures[tmu] == texnum->num)
		return;
#endif

	shaderstate.currenttextures[tmu] = texnum->num;
	if (target)
		qglBindTexture (target, texnum->num);

	if (
#ifndef FORCESTATE
	shaderstate.curtexturetype[tmu] != target &&
#endif
 !gl_config_nofixedfunc)
	{

		if (shaderstate.curtexturetype[tmu])
			qglDisable(shaderstate.curtexturetype[tmu]);
		shaderstate.curtexturetype[tmu] = target;
		if (target)
			qglEnable(target);
	}
}

#if 0//def GLSLONLY
static void GL_LazyBind(int tmu, texid_t texnum)
{
	int glnum = texnum?texnum->num:0;
	if (shaderstate.currenttextures[tmu] != glnum)
	{
		qglBindTextureUnit(tmu, glnum);
		shaderstate.currenttextures[tmu] = glnum;
	}
}
#else
static void GL_LazyBind(int tmu, texid_t texnum)
{
	int glnum;
	GLenum target;
	if (texnum)
	{
		static GLenum imgtab[] = {GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_CUBE_MAP_ARRAY_ARB};
		glnum = texnum->num, target = imgtab[(enum imgtype_e)((texnum->flags & IF_TEXTYPEMASK)>>IF_TEXTYPESHIFT)];
	}
	else
		glnum = 0, target = 0;

#ifndef FORCESTATE
	if (shaderstate.currenttextures[tmu] != glnum)
#endif
	{
		GL_SelectTexture(tmu);

		shaderstate.currenttextures[shaderstate.currenttmu] = glnum;

#ifndef FORCESTATE
		if (shaderstate.curtexturetype[tmu] != target)
#endif
		{
			if (gl_config_nofixedfunc)
			{
				shaderstate.curtexturetype[tmu] = target;
			}
			else
			{
				if (shaderstate.curtexturetype[tmu])
				{
					qglBindTexture (shaderstate.curtexturetype[tmu], 0);
					qglDisable(shaderstate.curtexturetype[tmu]);
				}
				shaderstate.curtexturetype[tmu] = target;
				if (target)
					qglEnable(target);
			}
		}

		if (target)
			qglBindTexture (target, glnum);
	}
}
#endif

static void BE_ApplyAttributes(unsigned int bitstochange, unsigned int bitstoendisable)
{
	unsigned int i;

#ifndef GLSLONLY
	//legacy colour attribute (including flat shaded)
	if ((bitstochange) & (1u<<VATTR_LEG_COLOUR))
	{
		if (!shaderstate.pendingcolourpointer && !shaderstate.pendingcolourvbo)
		{
			if (shaderstate.curcolourpointer || shaderstate.curcolourvbo)
			{
				qglShadeModel(GL_FLAT);
				qglDisableClientState(GL_COLOR_ARRAY);
			}
			shaderstate.curcolourpointer = NULL;
			shaderstate.curcolourvbo = 0;
			qglColor4fv(shaderstate.pendingcolourflat);
		}
		else
		{
		#ifndef FORCESTATE
			if (shaderstate.curcolourpointer != shaderstate.pendingcolourpointer || shaderstate.pendingcolourvbo != shaderstate.curcolourvbo)
		#endif
			{
				if (!shaderstate.curcolourpointer && !shaderstate.curcolourvbo)
				{
					if (qglShadeModel)
						qglShadeModel(GL_SMOOTH);
					bitstoendisable |= (1u<<VATTR_LEG_COLOUR);
				}
				shaderstate.curcolourpointer = shaderstate.pendingcolourpointer;
				shaderstate.curcolourvbo = shaderstate.pendingcolourvbo;
				GL_SelectVBO(shaderstate.curcolourvbo);
				qglColorPointer(4, shaderstate.colourarraytype, 0, shaderstate.curcolourpointer);
			}

			if ((bitstoendisable) & (1u<<VATTR_LEG_COLOUR))
			{
				qglEnableClientState(GL_COLOR_ARRAY);
			}
		}
	}
	else
	{
		if ((bitstoendisable) & (1u<<VATTR_LEG_COLOUR))
		{
			qglDisableClientState(GL_COLOR_ARRAY);
		}
	}

	//legacy tmus
	if ((bitstoendisable|bitstochange) >= (1u<<VATTR_LEG_TMU0))
	{
		for (i = VATTR_LEG_TMU0; (bitstoendisable|bitstochange) >= (1u<<i); i++)
		{
			if ((bitstochange) & (1u<<i))
			{
				qglClientActiveTextureARB(i-VATTR_LEG_TMU0 + mtexid0);
				if (bitstoendisable & (1u<<i))
					qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
				GL_SelectVBO(shaderstate.pendingtexcoordvbo[i-VATTR_LEG_TMU0]);
				qglTexCoordPointer(shaderstate.pendingtexcoordparts[i-VATTR_LEG_TMU0], GL_FLOAT, 0, shaderstate.pendingtexcoordpointer[i-VATTR_LEG_TMU0]);
			}

#ifndef FORCESTATE
			else if (bitstoendisable & (1u<<i))
#endif
			{
				qglClientActiveTextureARB(i-VATTR_LEG_TMU0 + mtexid0);
				if (bitstochange & (1u<<i))
					qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
				else
					qglDisableClientState(GL_TEXTURE_COORD_ARRAY);
			}
		}
	}

	//legacy vertex coords
	if ((bitstochange) & (1u<<VATTR_LEG_VERTEX))
	{
	#ifndef FORCESTATE
		if (shaderstate.currentvao || shaderstate.curvertexpointer != shaderstate.pendingvertexpointer || shaderstate.pendingvertexvbo != shaderstate.curvertexvbo)
	#endif
		{
			shaderstate.curvertexpointer = shaderstate.pendingvertexpointer;
			shaderstate.curvertexvbo = shaderstate.pendingvertexvbo;
			GL_SelectVBO(shaderstate.curvertexvbo);
			qglVertexPointer(3, GL_FLOAT, VECV_STRIDE, shaderstate.curvertexpointer);
		}
		if ((bitstoendisable) & (1u<<VATTR_LEG_VERTEX))
		{
			qglEnableClientState(GL_VERTEX_ARRAY);
		}
	}
	else
	{
		if ((bitstoendisable) & (1u<<VATTR_LEG_VERTEX))
		{
			qglDisableClientState(GL_VERTEX_ARRAY);
		}
	}
#endif

	if (!((bitstochange|bitstoendisable) & ((1u<<VATTR_LEG_FIRST)-1)))
		return;

	for (i = 0; i < VATTR_LEG_FIRST; i++)
	{
		if ((bitstochange) & (1u<<i))
		{
			switch (i)
			{
			case VATTR_VERTEX1:
				/*we still do vertex transforms for billboards and shadows and such*/
				GL_SelectVBO(shaderstate.pendingvertexvbo);
				qglVertexAttribPointer(i, 3, GL_FLOAT, GL_FALSE, VECV_STRIDE, shaderstate.pendingvertexpointer);
				break;
			case VATTR_VERTEX2:
				if (!shaderstate.sourcevbo->coord2.gl.vbo && !shaderstate.sourcevbo->coord2.gl.addr)
				{
					GL_SelectVBO(shaderstate.pendingvertexvbo);
					qglVertexAttribPointer(i, 3, GL_FLOAT, GL_FALSE, VECV_STRIDE, shaderstate.pendingvertexpointer);
				}
				else
				{
					GL_SelectVBO(shaderstate.sourcevbo->coord2.gl.vbo);
					qglVertexAttribPointer(VATTR_VERTEX2, 3, GL_FLOAT, GL_FALSE, VECV_STRIDE, shaderstate.sourcevbo->coord2.gl.addr);
				}
				break;
			case VATTR_COLOUR:
				if (!shaderstate.pendingcolourvbo && !shaderstate.pendingcolourpointer)
				{
					shaderstate.sha_attr &= ~(1u<<i);
					qglDisableVertexAttribArray(i);
					/*
					FTESurf Patch 166.  "No colour ARRAY" is not the same statement
					as "no colour", and this read it as though it were: a pass whose
					rgbgen/alphagen is a CONSTANT has its colour in
					pendingcolourflat and no array at all, and was handed white.

					That is the whole of "hl2_water 3 is a white sheet" and
					"hl2_dither_alpha does nothing" -- vmt/flatdither takes both its
					colour and its coverage from v_colour, and v_colour was
					(1,1,1,1) every frame.  With alpha 1 the dither can never
					discard, so the cvar had nothing to move either.

					Guarded on the flag rather than applied unconditionally,
					because pendingcolourflat is stale for every path that does not
					compute it, and reading a stale one here would tint or vanish
					geometry that renders correctly today.
					*/
					if (shaderstate.pendingcolourflatvalid)
						qglVertexAttrib4f(VATTR_COLOUR,
							shaderstate.pendingcolourflat[0], shaderstate.pendingcolourflat[1],
							shaderstate.pendingcolourflat[2], shaderstate.pendingcolourflat[3]);
					else
						qglVertexAttrib4f(VATTR_COLOUR, 1, 1, 1, 1);
					continue;
				}
				GL_SelectVBO(shaderstate.pendingcolourvbo);
				qglVertexAttribPointer(VATTR_COLOUR, 4, shaderstate.colourarraytype, ((shaderstate.colourarraytype==GL_FLOAT)?GL_FALSE:GL_TRUE), 0, shaderstate.pendingcolourpointer);
				break;
#if MAXRLIGHTMAPS > 1
			case VATTR_COLOUR2:
				GL_SelectVBO(shaderstate.sourcevbo->colours[1].gl.vbo);
				qglVertexAttribPointer(VATTR_COLOUR2, 4, shaderstate.colourarraytype, ((shaderstate.colourarraytype==GL_FLOAT)?GL_FALSE:GL_TRUE), 0, shaderstate.sourcevbo->colours[1].gl.addr);
				break;
			case VATTR_COLOUR3:
				GL_SelectVBO(shaderstate.sourcevbo->colours[2].gl.vbo);
				qglVertexAttribPointer(VATTR_COLOUR3, 4, shaderstate.colourarraytype, ((shaderstate.colourarraytype==GL_FLOAT)?GL_FALSE:GL_TRUE), 0, shaderstate.sourcevbo->colours[2].gl.addr);
				break;
			case VATTR_COLOUR4:
				GL_SelectVBO(shaderstate.sourcevbo->colours[3].gl.vbo);
				qglVertexAttribPointer(VATTR_COLOUR4, 4, shaderstate.colourarraytype, ((shaderstate.colourarraytype==GL_FLOAT)?GL_FALSE:GL_TRUE), 0, shaderstate.sourcevbo->colours[3].gl.addr);
				break;
#endif
			case VATTR_TEXCOORD:
				if (!shaderstate.pendingtexcoordvbo[0] && !shaderstate.pendingtexcoordpointer[0])
				{
					shaderstate.sha_attr &= ~(1u<<i);
					qglDisableVertexAttribArray(i);
					continue;
				}
				GL_SelectVBO(shaderstate.pendingtexcoordvbo[0]);
				qglVertexAttribPointer(VATTR_TEXCOORD, shaderstate.pendingtexcoordparts[0], GL_FLOAT, GL_FALSE, 0, shaderstate.pendingtexcoordpointer[0]);
				break;
			case VATTR_LMCOORD:
				if (!shaderstate.sourcevbo->lmcoord[0].gl.vbo && !shaderstate.sourcevbo->lmcoord[0].gl.addr)
				{
					GL_SelectVBO(shaderstate.sourcevbo->texcoord.gl.vbo);
					qglVertexAttribPointer(VATTR_LMCOORD, 2, GL_FLOAT, GL_FALSE, 0, shaderstate.sourcevbo->texcoord.gl.addr);
				}
				else
				{
					GL_SelectVBO(shaderstate.sourcevbo->lmcoord[0].gl.vbo);
					qglVertexAttribPointer(VATTR_LMCOORD, 2, GL_FLOAT, GL_FALSE, 0, shaderstate.sourcevbo->lmcoord[0].gl.addr);
				}
				break;
#if MAXRLIGHTMAPS > 1
			case VATTR_LMCOORD2:
				GL_SelectVBO(shaderstate.sourcevbo->lmcoord[1].gl.vbo);
				qglVertexAttribPointer(VATTR_LMCOORD2, 2, GL_FLOAT, GL_FALSE, 0, shaderstate.sourcevbo->lmcoord[1].gl.addr);
				break;
			case VATTR_LMCOORD3:
				GL_SelectVBO(shaderstate.sourcevbo->lmcoord[2].gl.vbo);
				qglVertexAttribPointer(VATTR_LMCOORD3, 2, GL_FLOAT, GL_FALSE, 0, shaderstate.sourcevbo->lmcoord[2].gl.addr);
				break;
			case VATTR_LMCOORD4:
				GL_SelectVBO(shaderstate.sourcevbo->lmcoord[3].gl.vbo);
				qglVertexAttribPointer(VATTR_LMCOORD4, 2, GL_FLOAT, GL_FALSE, 0, shaderstate.sourcevbo->lmcoord[3].gl.addr);
				break;
#endif
			case VATTR_NORMALS:
				if (!shaderstate.sourcevbo->normals.gl.addr)
				{
					shaderstate.sha_attr &= ~(1u<<i);
					qglDisableVertexAttribArray(i);
					continue;
				}
				GL_SelectVBO(shaderstate.sourcevbo->normals.gl.vbo);
				qglVertexAttribPointer(VATTR_NORMALS, 3, GL_FLOAT, GL_FALSE, 0, shaderstate.sourcevbo->normals.gl.addr);
				break;
			case VATTR_SNORMALS:
				if (!shaderstate.sourcevbo->svector.gl.addr)
				{
					shaderstate.sha_attr &= ~(1u<<i);
					qglDisableVertexAttribArray(i);
					continue;
				}
				GL_SelectVBO(shaderstate.sourcevbo->svector.gl.vbo);
				qglVertexAttribPointer(VATTR_SNORMALS, 3, GL_FLOAT, GL_FALSE, 0, shaderstate.sourcevbo->svector.gl.addr);
				break;
			case VATTR_TNORMALS:
				if (!shaderstate.sourcevbo->tvector.gl.addr)
				{
					shaderstate.sha_attr &= ~(1u<<i);
					qglDisableVertexAttribArray(i);
					continue;
				}
				GL_SelectVBO(shaderstate.sourcevbo->tvector.gl.vbo);
				qglVertexAttribPointer(VATTR_TNORMALS, 3, GL_FLOAT, GL_FALSE, 0, shaderstate.sourcevbo->tvector.gl.addr);
				break;
			case VATTR_BONENUMS:
				if (!shaderstate.sourcevbo->bonenums.gl.vbo && !shaderstate.sourcevbo->bonenums.gl.addr)
				{
					shaderstate.sha_attr &= ~(1u<<i);
					qglDisableVertexAttribArray(i);
					continue;
				}
				GL_SelectVBO(shaderstate.sourcevbo->bonenums.gl.vbo);
				qglVertexAttribPointer(VATTR_BONENUMS, 4, GL_BONE_INDEX_TYPE, GL_FALSE, 0, shaderstate.sourcevbo->bonenums.gl.addr);
				break;
			case VATTR_BONEWEIGHTS:
				if (!shaderstate.sourcevbo->boneweights.gl.vbo && !shaderstate.sourcevbo->boneweights.gl.addr)
				{
					shaderstate.sha_attr &= ~(1u<<i);
					qglDisableVertexAttribArray(i);
					continue;
				}
				GL_SelectVBO(shaderstate.sourcevbo->boneweights.gl.vbo);
				qglVertexAttribPointer(VATTR_BONEWEIGHTS, 4, GL_FLOAT, GL_FALSE, 0, shaderstate.sourcevbo->boneweights.gl.addr);
				break;
			}
			if ((bitstoendisable) & (1u<<i))
			{
				qglEnableVertexAttribArray(i);
			}
		}
		else
		{
			if ((bitstoendisable) & (1u<<i))
			{
				qglDisableVertexAttribArray(i);
			}
		}
	}
}

static void BE_EnableShaderAttributes(unsigned int progattrmask, int usevao)
{
	unsigned int bitstochange, bitstoendisable;

	if (shaderstate.currentvao != usevao)
	{
		shaderstate.currentvao = usevao;
		qglBindVertexArray(usevao); 
	}

	if (shaderstate.currentvao)
	{
		bitstochange = shaderstate.sourcevbo->vaodynamic&progattrmask;
#if 0
		bitstoendisable = 0;
#else
		bitstoendisable = shaderstate.sourcevbo->vaoenabled^progattrmask;
		if (bitstoendisable)
			bitstochange |= bitstoendisable;
		shaderstate.sourcevbo->vaoenabled = progattrmask;
#endif

		if (bitstochange & (1u<<VATTR_LEG_ELEMENTS))
		{
			qglBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, shaderstate.sourcevbo->indicies.gl.vbo);
		}
	}
	else
	{
		bitstochange = progattrmask;
		bitstoendisable = progattrmask^shaderstate.sha_attr;

		shaderstate.sha_attr = progattrmask;

/*
#ifndef FORCESTATE
		if (shaderstate.currentebo != shaderstate.sourcevbo->indicies.gl.vbo)
#endif
		{
			shaderstate.currentebo = shaderstate.sourcevbo->indicies.gl.vbo;
			qglBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, shaderstate.currentebo);
		}
*/
	}

	if (bitstochange || bitstoendisable)
		BE_ApplyAttributes(bitstochange, bitstoendisable);
}

void GLBE_SetupVAO(vbo_t *vbo, unsigned int vaodynamic, unsigned int vaostatic)
{
	if (qglGenVertexArrays)
	{
		qglGenVertexArrays(1, &vbo->vao);

		if ((vaostatic & VATTR_VERTEX1) && !gl_config_nofixedfunc)
			vaostatic = (vaostatic & ~VATTR_VERTEX1) | VATTR_LEG_VERTEX;
		if ((vaodynamic & VATTR_VERTEX1) && !gl_config_nofixedfunc)
			vaodynamic = (vaodynamic & ~VATTR_VERTEX1) | VATTR_LEG_VERTEX;

		shaderstate.curvertexpointer = NULL;
		shaderstate.curvertexvbo = 0;

		shaderstate.sourcevbo = vbo;
		shaderstate.pendingvertexvbo = shaderstate.sourcevbo->coord.gl.vbo;
		shaderstate.pendingvertexpointer = shaderstate.sourcevbo->coord.gl.addr;
		shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;
		shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
		shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;
		shaderstate.pendingtexcoordparts[0] = 2;

		shaderstate.currentvao = vbo->vao;
		qglBindVertexArray(vbo->vao);
		qglBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, shaderstate.sourcevbo->indicies.gl.vbo);
		BE_ApplyAttributes(vaostatic, vaodynamic|vaostatic);
		GL_SelectVBO(shaderstate.sourcevbo->coord.gl.vbo);
		vbo->vaoenabled = vaodynamic|vaostatic;
		vbo->vaodynamic = vaodynamic;

		shaderstate.curvertexpointer = NULL;
		shaderstate.curvertexvbo = 0;
	}
	else
	{
		GL_DeselectVAO();

		/*always select the coord vbo and indicies ebo, for easy bufferdata*/
		GL_SelectEBO(vbo->indicies.gl.vbo);
		GL_SelectVBO(vbo->coord.gl.vbo);
	}
}

void GL_SelectProgram(GLuint program)
{
	if (shaderstate.currentprogram != program)
	{
		qglUseProgramObjectARB(program);
		shaderstate.currentprogram = program;
	}
}
static void GL_DeSelectProgram(void)
{
	if (shaderstate.currentprogram != 0)
	{
		qglUseProgramObjectARB(0);
		shaderstate.currentprogram = 0;
	}
}


void GLBE_RenderShadowBuffer(unsigned int numverts, int vbo, vecV_t *verts, unsigned numindicies, int ibo, index_t *indicies)
{
	shaderstate.pendingvertexvbo = vbo;
	shaderstate.pendingvertexpointer = verts;

	shaderstate.sourcevbo = &shaderstate.dummyvbo;
	shaderstate.dummyvbo.indicies.gl.vbo = ibo;

	if (shaderstate.mode != BEM_STENCIL)
		GLBE_PolyOffsetShadowMap();

	if (shaderstate.allblackshader.glsl.handle)
	{
		GL_SelectProgram(shaderstate.allblackshader.glsl.handle);

		BE_EnableShaderAttributes(gl_config_nofixedfunc?(1u<<VATTR_VERTEX1):(1u<<VATTR_LEG_VERTEX), 0);

		if (shaderstate.allblackshader.glsl.handle != shaderstate.lastuniform && shaderstate.allblack_mvp != -1)
		{
			float m16[16];
			Matrix4_Multiply(shaderstate.projectionmatrix, shaderstate.modelviewmatrix, m16);
			qglUniformMatrix4fvARB(shaderstate.allblack_mvp, 1, false, m16);
		}
		shaderstate.lastuniform = shaderstate.allblackshader.glsl.handle;

		GL_SelectEBO(ibo);
		qglDrawRangeElements(GL_TRIANGLES, 0, numverts - 1, numindicies, GL_INDEX_TYPE, indicies);
	}
	else
	{
		GL_DeSelectProgram();
		BE_EnableShaderAttributes((1u<<VATTR_LEG_VERTEX), 0);

		//draw cached world shadow mesh
		GL_SelectEBO(ibo);
		qglDrawRangeElements(GL_TRIANGLES, 0, numverts - 1, numindicies, GL_INDEX_TYPE, indicies);
	}
	RQuantAdd(RQUANT_DRAWS, 1);
	DRAWCALL("GLBE_RenderShadowBuffer");
	RQuantAdd(RQUANT_SHADOWINDICIES, numindicies);
	shaderstate.dummyvbo.indicies.gl.vbo = 0;
	shaderstate.sourcevbo = NULL;
}

void GL_CullFace(unsigned int sflags)
{
	if (shaderstate.flags & BEF_FORCETWOSIDED)
		sflags = 0;
	else if (sflags)
		sflags ^= r_refdef.flipcull;

#ifndef FORCESTATE
	if (shaderstate.curcull == sflags)
		return;
#endif
	shaderstate.curcull = sflags;

	if (shaderstate.curcull & SHADER_CULL_FRONT)
	{
		qglEnable(GL_CULL_FACE);
		qglCullFace(GL_FRONT);
	}
	else if (shaderstate.curcull & SHADER_CULL_BACK)
	{
		qglEnable(GL_CULL_FACE);
		qglCullFace(GL_BACK);
	}
	else
	{
		qglDisable(GL_CULL_FACE);
	}
}

static void R_FetchPlayerColour(unsigned int cv, vec3_t rgb)
{
	int i;

	if (cv >= 16)
	{
		rgb[0] = (((cv&0xff0000)>>16)**((unsigned char*)&d_8to24rgbtable[15]+0)) / (256.0*256);
		rgb[1] = (((cv&0x00ff00)>>8)**((unsigned char*)&d_8to24rgbtable[15]+1)) / (256.0*256);
		rgb[2] = (((cv&0x0000ff)>>0)**((unsigned char*)&d_8to24rgbtable[15]+2)) / (256.0*256);
		return;
	}
	i = cv;
	if (i >= 8)
	{
		i<<=4;
	}
	else
	{
		i<<=4;
		i+=15;
	}
	i*=3;
	rgb[0] = host_basepal[i+0] / 255.0;
	rgb[1] = host_basepal[i+1] / 255.0;
	rgb[2] = host_basepal[i+2] / 255.0;
/*	if (!gammaworks)
	{
		*retred = gammatable[*retred];
		*retgreen = gammatable[*retgreen];
		*retblue = gammatable[*retblue];
	}*/
}

#ifdef RTLIGHTS
void GLBE_SetupForShadowMap(dlight_t *dl, int texwidth, int texheight, float shadowscale)
{
	extern cvar_t r_shadow_shadowmapping_bias;
	extern cvar_t r_shadow_shadowmapping_nearclip;
	float n = dl->nearclip?dl->nearclip:r_shadow_shadowmapping_nearclip.value;
	float f = dl->radius;
	float b = r_shadow_shadowmapping_bias.value;

	shaderstate.lightshadowmapproj[0] = shadowscale * (1.0-(1.0/texwidth)) * 0.5/3.0;
	shaderstate.lightshadowmapproj[1] = shadowscale * (1.0-(1.0/texheight)) * 0.5/2.0;
	shaderstate.lightshadowmapproj[2] = 0.5*(f+n)/(n-f);
	shaderstate.lightshadowmapproj[3] = (f*n)/(n-f) - b*n*(1024/texheight);

	shaderstate.lightshadowmapscale[0] = 1.0/texwidth;
	shaderstate.lightshadowmapscale[1] = 1.0/texheight;
}

//nettest P110 --------------------------------------------------------------------------------------
//Multi-direction fake shadows.  gl_shadow.c renders N model-only depth passes into cells of ONE texture,
//each from a different dominant-light direction, and records each pass's projection here for the forward
//pass to consume.  Slot 0 is always the sun, so N=1 is exactly the legacy behaviour.
void GLBE_SetFakeShadowCount(int count)
{
	shaderstate.fakeshadowcount = bound(1, count, MAX_FAKESHADOW_SLOTS);
}

//nettest P114b: force the NEXT GLBE_SelectEntity to re-copy r_refdef.m_projection_std into the cached
//shaderstate.projectionmatrix.  The fake-shadow ATLAS (Sh_GenerateCascadeAtlas / Sh_GenerateFakeShadowsAtlas)
//renders N cells with a DIFFERENT ortho projection each, inside ONE Begin/End pair.  But GLBE_SelectEntity
//only refreshes the cached projection when the entity's DEPTHHACK/XFLIP flags change -- which they never do
//for world/prop casters -- so cells 1..N-1 rendered their caster depth with CELL 0's projection while the
//shader sampled each with its own matrix.  For cascades that magnified every caster's shadow by
//radius(s)/radius(0) (3x, 9x, ...): one prop cast three shadows at three sizes.  Same value that
//GLBE_BeginShadowMap/EndShadowMap already use to invalidate; call this once per cell before its render.
void GLBE_FlushProjection(void)
{
	shaderstate.usingweaponviewmatrix = -1;
}

//Snapshot the projection GLBE_SelectDLight just built for this slot.  Deliberately a COPY rather than a
//rebuild: the ORTHO branch of GLBE_SelectDLight already pairs Matrix4x4_CM_Orthographic with
//ModelViewMatrixFromAxis(axis[0], axis[2], axis[1]) -- the "FAKESHADOWS xyz convention" -- and bakes the
//Patch-96 world-constant r_shadows_bias into [14].  Reproducing that math here is exactly how Patch 93's
//spot-slot attempt came out mirrored and tiny; copying makes disagreement impossible.
void GLBE_CaptureFakeShadowSlot(int slot, const vec4_t cell)
{
	if ((unsigned)slot >= MAX_FAKESHADOW_SLOTS)
		return;
	memcpy(shaderstate.fakeshadowmatrix[slot], shaderstate.lightprojmatrix, sizeof(shaderstate.lightprojmatrix));
	Vector4Copy(cell, shaderstate.fakeshadowcell[slot]);
}

//nettest Phase-1: capture an EXPLICIT projection for this slot instead of copying the one GLBE_SelectDLight
//built.  The PERSPECTIVE (spot) prop-shadow cells render via Sh_GenShadowFace's face-4 view
//(LightMatrixFromAxis(axis[2],axis[1],-axis[0])), but GLBE_SelectDLight's SPOT branch builds a DIFFERENT
//convention (ModelViewMatrixFromAxis(axis[0],axis[1],axis[2]) = xy transposed vs face-4) -- copying that
//would mismatch the rendered depth (the P93 "mirrored/tiny" trap).  The caller instead passes the transform
//it ACTUALLY rendered with, r_refdef.m_projection_std * r_refdef.m_view, which is correct for ANY projection
//by construction (the shader reproduces exactly the clip-space the depth was written in).
void GLBE_CaptureFakeShadowSlotMatrix(int slot, const float *matrix, const vec4_t cell)
{
	if ((unsigned)slot >= MAX_FAKESHADOW_SLOTS)
		return;
	memcpy(shaderstate.fakeshadowmatrix[slot], matrix, sizeof(shaderstate.fakeshadowmatrix[slot]));
	Vector4Copy(cell, shaderstate.fakeshadowcell[slot]);
}

//nettest: the fake-shadow atlas texture (r_shadows_propshadows_showatlas debug view samples it).
texid_t GLBE_GetFakeShadowAtlasTexture(void)
{
	return shadowmap[2];
}

//An unused slot must not keep LAST frame's matrix, or receivers would project into a cell that now holds
//a different direction's depth.  Identity with a huge z translation puts every fragment past the far
//plane, so the shader's `fd < 1.0` box test rejects the slot outright and it costs no taps.
void GLBE_ClearFakeShadowSlot(int slot, const vec4_t cell)
{
	if ((unsigned)slot >= MAX_FAKESHADOW_SLOTS)
		return;
	Matrix4x4_Identity(shaderstate.fakeshadowmatrix[slot]);
	shaderstate.fakeshadowmatrix[slot][14] = 1e9f;
	Vector4Copy(cell, shaderstate.fakeshadowcell[slot]);
	Vector4Set(shaderstate.fakeshadowinfo[slot], 0, 0, 0, 0);	//dead cells must not suppress (or un-suppress) anything
}

//nettest: per-slot metadata for the PERSPECTIVE lamp cells (l_fakeshadowinfo).  x = sun-suppression
//factor: how much a sun-visible receiving pixel washes this lamp's shadow out (the lamp-vs-sun
//brightness ratio, computed by Sh_GeneratePropShadowsAtlas; 0 on maps with no SUNVIS bake, which keeps
//the feature inert there -- the sampler fallback reads "fully sunlit" everywhere on those maps and would
//otherwise erase every lamp shadow).  yzw reserved.
void GLBE_SetFakeShadowSlotInfo(int slot, const vec4_t info)
{
	if ((unsigned)slot >= MAX_FAKESHADOW_SLOTS)
		return;
	Vector4Copy(info, shaderstate.fakeshadowinfo[slot]);
}
//---------------------------------------------------------------------------------------------------

int GLBE_BeginRenderBuffer_DepthOnly(texid_t depthtexture);
qboolean GLBE_BeginShadowMap(int id, int w, int h, uploadfmt_t encoding, int *restorefbo)
{
	if (!gl_config.ext_framebuffer_objects)
		return false;

	if (!TEXVALID(shadowmap[id]) || shadowmap[id]->width != w || shadowmap[id]->height != h || shadowmap[id]->format != encoding || shadowmap[id]->status != TEX_LOADED)
	{
		texid_t tex;
		if (shadowmap[id])
			Image_DestroyTexture(shadowmap[id]);
		tex = shadowmap[id] = Image_CreateTexture(va("***shadowmap2d%i***", id), NULL, IF_NOPURGE|IF_CLAMP|IF_NOMIPMAP|IF_RENDERTARGET);
		tex->width = w;
		tex->height = h;
		tex->format = encoding;
		qglGenTextures(1, &tex->num);
		//nettest: the FBO attach below is cached BY TEXTURE NAME (shadow_fbo_depth_num) -- but GL recycles
		//names, so the gen above typically returns the name the destroy just freed.  The stale cache then
		//SKIPPED the re-attach while the deletion had already detached the old image, leaving the FBO with
		//no depth attachment: every depth render+clear got dropped, the atlas stayed undefined, and every
		//shadow test read "in shadow" -- the whole world went dark on any LIVE r_shadows_res change.
		shadow_fbo_depth_num = 0;
		GL_MTBind(0, GL_TEXTURE_2D, tex);
#ifdef SHADOWDBG_COLOURNOTDEPTH
		qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#else
		if (qglTexStorage2D)
			qglTexStorage2D(GL_TEXTURE_2D, 1, gl_config.formatinfo[encoding].sizedformat, w, h);
		else if (gl_config.formatinfo[encoding].type)
			qglTexImage2D				(GL_TEXTURE_2D, 0, gl_config.formatinfo[encoding].sizedformat, w, h, 0, gl_config.formatinfo[encoding].format, gl_config.formatinfo[encoding].type,	NULL);
		else
			qglCompressedTexImage2D		(GL_TEXTURE_2D, 0, gl_config.formatinfo[encoding].sizedformat, w, h, 0,	0, NULL);
#endif

		qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
#if 0//def SHADOWDBG_COLOURNOTDEPTH
		qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
#else
		qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#endif
		//in case we're using shadow samplers
		if (gl_config.arb_shadow)
		{
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE_ARB, GL_COMPARE_R_TO_TEXTURE_ARB);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC_ARB, GL_LEQUAL);
			//qglTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE_ARB, GL_LUMINANCE);
		}
		tex->status = TEX_LOADED;
	}
	shaderstate.curshadowmap = shadowmap[id];

	/*set framebuffer*/
	*restorefbo = GLBE_BeginRenderBuffer_DepthOnly(shaderstate.curshadowmap);

	//nettest: an incomplete shadow FBO silently drops every depth render+clear and the whole world reads
	//as "in shadow" (dark) -- make it LOUD instead, and fail properly so the caller disables the feature
	//for the frame rather than sampling an undefined atlas.
	if (qglCheckFramebufferStatusEXT)
	{
		GLenum fbstatus = qglCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
		if (fbstatus != GL_FRAMEBUFFER_COMPLETE_EXT)
		{
			static float lastwarn;
			if (realtime > lastwarn + 5)
			{
				lastwarn = realtime;
				Con_Printf(CON_WARNING "GLBE_BeginShadowMap: %ix%i depth fbo incomplete (0x%x) -- shadows disabled this frame\n", w, h, fbstatus);
			}
			GLBE_FBO_Pop(*restorefbo);
			return false;
		}
	}

	shaderstate.usingweaponviewmatrix = -1;		//make sure the projection matrix is updated.

	while(shaderstate.lastpasstmus>0)
	{
		GL_LazyBind(--shaderstate.lastpasstmus, r_nulltex);
	}

	shaderstate.shaderbits &= ~SBITS_MISC_DEPTHWRITE;

	BE_SelectMode(BEM_DEPTHONLY);

	BE_Scissor(NULL);
	qglViewport(0, 0, w, h);
	GL_ForceDepthWritable();
	qglClear (GL_DEPTH_BUFFER_BIT);
#ifdef SHADOWDBG_COLOURNOTDEPTH
	qglColorMask(TRUE,TRUE,TRUE,TRUE);
	qglClearColor(1,1,1,1);
	qglClear (GL_COLOR_BUFFER_BIT);
#endif

	return true;
}
void GLBE_EndShadowMap(int restorefbo)
{
	GLBE_FBO_Pop(restorefbo);
	shaderstate.usingweaponviewmatrix = -1;		//make sure the projection matrix is updated.
}
#endif

//FTESurf Patch 866: what temptexture's storage was allocated as (0 = none).
static int be_currentw, be_currenth;
static GLenum be_currentfmt;
static qboolean be_nopanedepth;	//FTESurf Patch 878: a world or brush pane drawn now writes no depth (GLBE_SubmitNearest)
static qboolean be_flatcopied;	//FTESurf Patch 895: this top-level view has copied the screen for a world/brush pane
extern unsigned int r_vs_copies;
extern double r_vs_copypixels;
extern cvar_t r_nearestsort;

//FTESurf Patch 895: the plugin's Refract (vmt/refract) on a world or brush batch, opaque (NEAREST) or
//blended (waterfallsoft's 8-bit alpha normal map) alike.  Not a particle (dummybatch), not a model.
static qboolean GLBE_FlatRefractPane(void)
{
	static const shader_t *said[32];	//developer: each shader's first answer, once
	static int nsaid;
	const batch_t *b = shaderstate.curbatch;
	const shader_t *sh = shaderstate.curshader;
	const entity_t *ent;
	const program_t *prog = NULL;
	const char *why = NULL;
	int i;
	if (!b || b == &shaderstate.dummybatch)
		why = "dummy batch";
	else if (!(ent = b->ent))
		why = "no entity";
	else if (ent != &r_worldentity && ent->model && ent->model->type != mod_brush)
		why = "model entity";
	else if (!sh)
		why = "no shader";
	else
	{
		prog = sh->prog ? sh->prog : sh->passes[0].prog;	//vmt/refract is a pass program
		if (!prog || !prog->name)
			why = "no program";
		else if (strncmp(prog->name, "vmt/refract", 11))
			why = "another program";
	}
	if (sh && developer.ival && nsaid < countof(said))
	{
		for (i = 0; i < nsaid && said[i] != sh; i++)
			;
		if (i == nsaid)
		{
			said[nsaid++] = sh;
			Con_DPrintf("P895: %s %s (%s)\n", sh->name, why ? why : "shares the view's copy",
				prog && prog->name ? prog->name : "-");
		}
	}
	return !why;
}

static void T_Gen_CurrentRender(int tmu, qboolean pane)
{
	int vwidth, vheight;
	int pwidth = vid.fbpwidth;
	int pheight = vid.fbpheight;
	GLenum fmt;
	//FTESurf Patch 866: bind the last copy rather than leave the unit holding whatever it had.
	if (r_refdef.recurse)
	{
		GL_MTBind(tmu, GL_TEXTURE_2D, TEXVALID(shaderstate.temptexture) ? shaderstate.temptexture : r_blackimage);
		return;
	}
	//FTESurf Patch 895: a world or brush Refract pane binds the view's most recent copy once one exists --
	//Source's brushes never copy (c_baseentity.cpp:1801-1804), its model renderables do
	//(c_baseanimating.cpp:799).  surf_pantheon's waterfalls took 61 a frame.  The water fallback at
	//r_refract_fbo 0 (pane FALSE), scripted shaders, particles and r_nearestsort 0 copy per batch.
	if (pane && r_nearestsort.ival && !shaderstate.force2d && GLBE_FlatRefractPane())
	{
		if (be_flatcopied && TEXVALID(shaderstate.temptexture))
		{
			GL_MTBind(tmu, GL_TEXTURE_2D, shaderstate.temptexture);
			return;
		}
		be_flatcopied = true;
	}

	if (sh_config.texture_non_power_of_two_pic)
	{
		vwidth = pwidth;
		vheight = pheight;
	}
	else
	{
		vwidth = 1;
		vheight = 1;
		while (vwidth < pwidth)
		{
			vwidth *= 2;
		}
		while (vheight < pheight)
		{
			vheight *= 2;
		}
	}

	if (vid.flags&VID_FP16)
		fmt = GL_RGBA16F;
	else if (vid.flags&VID_SRGB_FB)
		fmt = GL_SRGB8_EXT;
	else
		fmt = GL_RGB;

	// copy the scene to texture
	if (!TEXVALID(shaderstate.temptexture))
	{
		TEXASSIGN(shaderstate.temptexture, Image_CreateTexture("***$currentrender***", NULL, 0));
		qglGenTextures(1, &shaderstate.temptexture->num);
	}
	GL_MTBind(tmu, GL_TEXTURE_2D, shaderstate.temptexture);
	if (!shaderstate.force2d)	//FTESurf Patch 878: r_viewstats counts the scene's
		r_vs_copies++, r_vs_copypixels += (double)vwidth * vheight;
	//FTESurf Patch 866: copy into the storage the last copy allocated; CopyTexImage2D
	//reallocated it for every batch that samples the screen (Refract at hl2_refract 1).
	if (be_currentw == vwidth && be_currenth == vheight && be_currentfmt == fmt)
	{
		qglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, vwidth, vheight);
		return;
	}
	be_currentw = vwidth;
	be_currenth = vheight;
	be_currentfmt = fmt;
	qglCopyTexImage2D(GL_TEXTURE_2D, 0, fmt, 0, 0, vwidth, vheight, 0);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void Shader_BindTextureForPass(int tmu, const shaderpass_t *pass)
{
	extern cvar_t gl_specular_fallback;

	texid_t t;
	switch(pass->texgen)
	{
	default:
	case T_GEN_SINGLEMAP:
		t = pass->anim_frames[0];
		break;
	case T_GEN_ANIMMAP:
		t = pass->anim_frames[(int)(pass->anim_fps * shaderstate.curtime) % pass->anim_numframes];
		break;
	case T_GEN_LIGHTMAP:
		if ((unsigned short)shaderstate.curbatch->lightmap[0] >= numlightmaps)
			t = r_whiteimage;
		else
			t = lightmap[shaderstate.curbatch->lightmap[0]]->lightmap_texture;
		break;
	case T_GEN_DELUXMAP:
		{
			unsigned int lmi = shaderstate.curbatch->lightmap[0];
			if (lmi >= numlightmaps || !lightmap[lmi]->hasdeluxe)
				t = missing_texture_normal;
			else
				t = lightmap[lmi+1]->lightmap_texture;
		}
		break;
	case T_GEN_DIFFUSE:
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->base))
			t = shaderstate.curtexnums->base;
		else
			t = missing_texture;
		break;
	case T_GEN_PALETTED:
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->paletted))
			t = shaderstate.curtexnums->paletted;
		else
			t = r_whiteimage;
		break;
	case T_GEN_NORMALMAP:
		t = (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->bump))?shaderstate.curtexnums->bump:missing_texture_normal;
		break;
	case T_GEN_SPECULAR:
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->specular))
			t = shaderstate.curtexnums->specular;
		else if (gl_specular_fallback.value<0 && shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->base))
			t = shaderstate.curtexnums->base;
		else
			t = missing_texture_gloss;
		break;
	case T_GEN_UPPEROVERLAY:
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->upperoverlay))
			t = shaderstate.curtexnums->upperoverlay;
		else
			t = r_nulltex;
		break;
	case T_GEN_LOWEROVERLAY:
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->loweroverlay))
			t = shaderstate.curtexnums->loweroverlay;
		else
			t = r_nulltex;
		break;
	case T_GEN_FULLBRIGHT:
		t = (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->fullbright)) ? shaderstate.curtexnums->fullbright : r_nulltex;	//nettest: guard the curtexnums deref like every other T_GEN_* case — an unresolved Source material/prop skin on frame 1 has curtexnums==NULL and crashed here
		break;
	case T_GEN_REFLECTCUBE:
		/*
		FTESurf Patch 268 B: same chain as before, with two arms inserted.

		The sentinel (`reflectcube "$envcubemap"`, gl_shader.c) is a real loaded
		cube, so the first test would otherwise bind mid-grey and stop.  It means
		"use the batch's baked cubemap", so skip it and fall through.

		r_reflectcube_nosky 0 is the old second line EXACTLY -- an envmap that is
		still loading (or failed) binds GL name 0 and samples black, and a NULL one
		falls to the sky.  1 sends both of those to the neutral grey instead.
		*/
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->reflectcube) && shaderstate.curtexnums->reflectcube != r_envcubemap_tex)
			t = shaderstate.curtexnums->reflectcube;
		else if (shaderstate.curbatch->envmap && (TEXLOADED(shaderstate.curbatch->envmap) || !r_reflectcube_nosky.ival))
			t = shaderstate.curbatch->envmap;
		else if (r_reflectcube_nosky.ival && r_envcubemap_tex)
			t = r_envcubemap_tex;
		else
			t = shaderstate.tex_reflectcube;
		break;
	case T_GEN_REFLECTMASK:
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->reflectmask))
			t = shaderstate.curtexnums->reflectmask;
		else if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->base))
			t = shaderstate.curtexnums->base;
		else
			t = r_whiteimage;
		break;
	case T_GEN_DISPLACEMENT:
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->displacement))
			t = shaderstate.curtexnums->displacement;
		else
			t = r_whiteimage;
		break;
	case T_GEN_OCCLUSION:
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->occlusion))
			t = shaderstate.curtexnums->occlusion;
		else
			t = r_whiteimage;
		break;
	case T_GEN_TRANSMISSION:
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->transmission))
			t = shaderstate.curtexnums->transmission;
		else
			t = r_whiteimage;
		break;
	case T_GEN_THICKNESS:
		if (shaderstate.curtexnums && TEXLOADED(shaderstate.curtexnums->thickness))
			t = shaderstate.curtexnums->thickness;
		else
			t = r_whiteimage;
		break;
	case T_GEN_SHADOWMAP:
		t = shaderstate.curshadowmap;
		break;

	case T_GEN_LIGHTCUBEMAP:
		t = shaderstate.lightcubemap;
		break;
	case T_GEN_SOURCECUBE:
		t = scenepp_postproc_cube;
		break;

#ifdef HAVE_MEDIA_DECODER
	case T_GEN_VIDEOMAP:
		t = Media_UpdateForShader(pass->cin);
		if (!TEXLOADED(t))
			t = shaderstate.curtexnums?shaderstate.curtexnums->base:r_nulltex;
		break;
#endif

	case T_GEN_CURRENTRENDER:
		T_Gen_CurrentRender(tmu, true);
		return;

	case T_GEN_SOURCECOLOUR:
		t = shaderstate.tex_sourcecol;
		break;
	case T_GEN_SOURCEDEPTH:
		t = shaderstate.tex_sourcedepth;
		break;
	case T_GEN_REFLECTION:
		t = shaderstate.tex_reflection[r_refdef.recurse];
		break;
	case T_GEN_REFRACTION:
		/* FTESurf Patch 210: a portal must read the texture the portal was
		   rendered into, whatever r_refract_fbo says.

		   GLBE_GenerateBatchTextures generates into the FBO when
		   `r_refract_fboival || SHADER_HASPORTAL`, but this -- the read side of
		   the same texture -- consulted only r_refract_fboival.  With
		   r_refract_fbo 0 the two disagree: the far view is rendered into
		   tex_refraction and then the surface samples a copy of the CURRENT
		   FRAMEBUFFER instead.  For water that substitution is the whole point of
		   the cvar and is roughly right; for a portal it is not a worse portal, it
		   is not a portal at all, and because a SHADER_SORT_PORTAL surface draws
		   before sky and opaque there is almost nothing in the framebuffer yet, so
		   the doorway comes out black.  Which is indistinguishable from "the
		   recursed scene rendered nothing", and would have cost the next person
		   the same hours it nearly cost this one.

		   r_refract_fbo defaults to 1, so this was latent rather than active. */
		// Depth consumers must sample the capture they generated, even at r_refract_fbo 0.
		if (!r_refract_fboival && !(shaderstate.curshader && (shaderstate.curshader->flags & (SHADER_HASPORTAL|SHADER_HASREFRACTDEPTH))))
		{
			T_Gen_CurrentRender(tmu, false);
			return;
		}
		t = shaderstate.tex_refraction[r_refdef.recurse];
		break;
	case T_GEN_REFRACTIONDEPTH:
		t = shaderstate.tex_refractiondepth[r_refdef.recurse];
		break;
	case T_GEN_RIPPLEMAP:
		t = shaderstate.tex_ripplemap[r_refdef.recurse];
		break;
	case T_GEN_GBUFFERCASE:
		t = shaderstate.tex_gbuf[pass->texgen-T_GEN_GBUFFER0];
		break;
	}
	GL_LazyBind(tmu, t);
}

/*========================================== matrix functions =====================================*/

typedef vec3_t mat3_t[3];
static mat3_t axisDefault={{1, 0, 0},
					{0, 1, 0},
					{0, 0, 1}};

static void Matrix3_Transpose (mat3_t in, mat3_t out)
{
	out[0][0] = in[0][0];
	out[1][1] = in[1][1];
	out[2][2] = in[2][2];

	out[0][1] = in[1][0];
	out[0][2] = in[2][0];
	out[1][0] = in[0][1];
	out[1][2] = in[2][1];
	out[2][0] = in[0][2];
	out[2][1] = in[1][2];
}
static void Matrix3_Multiply_Vec3 (const mat3_t a, const vec3_t b, vec3_t product)
{
	product[0] = a[0][0]*b[0] + a[0][1]*b[1] + a[0][2]*b[2];
	product[1] = a[1][0]*b[0] + a[1][1]*b[1] + a[1][2]*b[2];
	product[2] = a[2][0]*b[0] + a[2][1]*b[1] + a[2][2]*b[2];
}

static int Matrix3_Compare(const mat3_t in, const mat3_t out)
{
	return memcmp(in, out, sizeof(mat3_t));
}

//end matrix functions
/*========================================== tables for deforms =====================================*/
#define frand() (rand()*(1.0/RAND_MAX))
#define FTABLE_SIZE		1024
#define FTABLE_CLAMP(x)	(((int)((x)*FTABLE_SIZE) & (FTABLE_SIZE-1)))
#define FTABLE_EVALUATE(table,x) (table ? table[FTABLE_CLAMP(x)] : frand()*((x)-floor(x)))

static	float	r_sintable[FTABLE_SIZE];
static	float	r_triangletable[FTABLE_SIZE];
static	float	r_squaretable[FTABLE_SIZE];
static	float	r_sawtoothtable[FTABLE_SIZE];
static	float	r_inversesawtoothtable[FTABLE_SIZE];

//#define R_FastSin(x) sin((x)*(2*M_PI))
#define R_FastSin(x) r_sintable[FTABLE_CLAMP(x)]

static float *FTableForFunc ( unsigned int func )
{
	switch (func)
	{
		default:
		case SHADER_FUNC_SIN:
			return r_sintable;

		case SHADER_FUNC_TRIANGLE:
			return r_triangletable;

		case SHADER_FUNC_SQUARE:
			return r_squaretable;

		case SHADER_FUNC_SAWTOOTH:
			return r_sawtoothtable;

		case SHADER_FUNC_INVERSESAWTOOTH:
			return r_inversesawtoothtable;

		case SHADER_FUNC_NOISE:
			return NULL;
	}
}

void Shader_LightPass(struct shaderparsestate_s *ps, const char *shortname, const void *args)
{
	char shadertext[8192*2];
	extern cvar_t r_drawflat;
	sprintf(shadertext, LIGHTPASS_SHADER, (r_lightmap.ival||r_drawflat.ival)?"#FLAT=1.0":"");
	Shader_DefaultScript(ps, shortname, shadertext);
}

extern cvar_t r_fog_linear;
extern cvar_t r_fog_exp2;
void GenerateFogTexture(texid_t *tex, float density, float zscale)
{
#define FOGS 256
#define FOGT 32
	byte_vec4_t fogdata[FOGS*FOGT];
	int s, t;
	float f, z;
	static float fogdensity, fogzscale;
	if (TEXVALID(*tex) && density == fogdensity && zscale == fogzscale)
		return;
	fogdensity = density;
	fogzscale = zscale;

	for(s = 0; s < FOGS; s++)
		for(t = 0; t < FOGT; t++)
		{
			z = (float)s / (FOGS-1);
			z *= zscale;

			if (r_fog_linear.ival) {
				f = 1.0 - ((density - z) / (density/* - r_refdef.globalfog.depthbias*/)); //pow(z, 0.5);
			} else {
				if (!r_fog_exp2.ival)//GL_EXP
					f = 1-exp(-density * z);
				else //GL_EXP2
					f = 1-exp(-(density*density) * z);
			}

			if (f < 0)
				f = 0;
			if (f > 1)
				f = 1;
			f *= (float)t / (FOGT-1);

			fogdata[t*FOGS + s][0] = 255;
			fogdata[t*FOGS + s][1] = 255;
			fogdata[t*FOGS + s][2] = 255;
			fogdata[t*FOGS + s][3] = 255*f;
		}

	if (!TEXVALID(*tex))
		*tex = Image_CreateTexture("***fog***", NULL, IF_CLAMP|IF_NOMIPMAP);
	Image_Upload(*tex, TF_RGBA32, fogdata, NULL, FOGS, FOGT, 1, IF_CLAMP|IF_NOMIPMAP);
}

void GLBE_DestroyFBOs(void)
{
	size_t i;
	GLBE_FBO_Destroy(&shaderstate.fbo_lprepass);

	for (i = 0; i < R_MAX_RECURSE; i++)
	{
		GLBE_FBO_Destroy(&shaderstate.fbo_reflectrefrac[i]);
		if (shaderstate.tex_reflection[i])
		{
			Image_DestroyTexture(shaderstate.tex_reflection[i]);
			shaderstate.tex_reflection[i] = r_nulltex;
		}
		if (shaderstate.tex_refraction[i])
		{
			Image_DestroyTexture(shaderstate.tex_refraction[i]);
			shaderstate.tex_refraction[i] = r_nulltex;
		}
		if (shaderstate.tex_refractiondepth[i])
		{
			Image_DestroyTexture(shaderstate.tex_refractiondepth[i]);
			shaderstate.tex_refractiondepth[i] = r_nulltex;
		}
	}
	if (shaderstate.temptexture)
	{
		Image_DestroyTexture(shaderstate.temptexture);
		shaderstate.temptexture = r_nulltex;
	}
	be_currentw = be_currenth = 0;	//FTESurf Patch 866: the next copy allocates

	//shadowmapping stuff
	if (shadow_fbo_id)
	{
		qglDeleteFramebuffersEXT(1, &shadow_fbo_id);
		shadow_fbo_id = 0;
		shadow_fbo_depth_num = 0;
	}
	for (i = 0; i < countof(shadowmap); i++)
	{
		if (shadowmap[i])
		{
			Image_DestroyTexture(shadowmap[i]);
			shadowmap[i] = r_nulltex;
		}
	}

	//nuke deferred rendering stuff
	for (i = 0; i < countof(shaderstate.tex_gbuf); i++)
	{
		if (shaderstate.tex_gbuf[i])
		{
			Image_DestroyTexture(shaderstate.tex_gbuf[i]);
			shaderstate.tex_gbuf[i] = r_nulltex;
		}
	}
}

void GLBE_Shutdown(void)
{
	size_t u;
	GLBE_FBO_Destroy(&shaderstate.fbo_2dfbo);
	GLBE_DestroyFBOs();

	for (u = 0; u < countof(shaderstate.programfixedemu); u++)
	{
		Shader_ReleaseGeneric(shaderstate.programfixedemu[u]);
		shaderstate.programfixedemu[u] = NULL;
	}

	BZ_Free(shaderstate.wbatches);
	shaderstate.wbatches = NULL;
	shaderstate.maxwbatches = 0;

	//on vid_reload, the gl drivers might have various things bound that have since been destroyed/etc
	//so reset that state to avoid any issues with state
	BE_EnableShaderAttributes(0, 0);
	GL_SelectVBO(0);
	GL_SelectEBO(0);

	for (u = 0; u < countof(shaderstate.currenttextures); u++)
		GL_LazyBind(u, r_nulltex);
	GL_SelectTexture(0);
}

void GLBE_Init(void)
{
	int i;
	double t;

	GLBE_Shutdown();

	memset(&shaderstate, 0, sizeof(shaderstate));
	GLBE_InstanceReset();	//FTESurf Patch 831

	shaderstate.curentity = &r_worldentity;
	be_maxpasses = gl_config_nofixedfunc?1:gl_mtexarbable;
	be_maxpasses = min(SHADER_TMU_MAX, min(be_maxpasses, 32-VATTR_LEG_TMU0));
	sh_config.stencilbits = 0;
#ifndef GLESONLY
	if (!gl_config_gles && gl_config.glversion >= 3.0 && gl_config_nofixedfunc)
	{
		//docs say this line should be okay in gl3+. nvidia do not seem to agree. GL_STENCIL_BITS is depricated however. so for now, just assume.
		qglGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER_EXT, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &sh_config.stencilbits);
		if (qglGetError())
			sh_config.stencilbits = 8;
	}
	else
#endif
		qglGetIntegerv(GL_STENCIL_BITS, &sh_config.stencilbits);

	{	//nettest: cache the driver's max aliased line width so gl_line_width can be clamped, and warn if the driver/profile pins it to 1px
		float lwrange[2] = {1, 1};
		if (qglGetFloatv)
			qglGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, lwrange);
		if (lwrange[1] >= 1)
			gl_maxlinewidth = lwrange[1];
		Con_DPrintf("GL_ALIASED_LINE_WIDTH_RANGE: [%g, %g]\n", lwrange[0], lwrange[1]);
		if (gl_maxlinewidth <= 1)
			Con_Printf("note: GL driver/profile clamps line width to 1px; gl_line_width has no effect (use cl_debug_wire_thickness for quad thickness)\n");
	}

	for (i = 0; i < FTABLE_SIZE; i++)
	{
		t = (double)i / (double)FTABLE_SIZE;

		r_sintable[i] = sin(t * 2*M_PI);

		if (t < 0.25)
			r_triangletable[i] = t * 4.0;
		else if (t < 0.75)
			r_triangletable[i] = 2 - 4.0 * t;
		else
			r_triangletable[i] = (t - 0.75) * 4.0 - 1.0;

		if (t < 0.5)
			r_squaretable[i] = 1.0f;
		else
			r_squaretable[i] = -1.0f;

		r_sawtoothtable[i] = t;
		r_inversesawtoothtable[i] = 1.0 - t;
	}

	shaderstate.identitylighting = 1;
	shaderstate.identitylightmap = 1;
	for (i = 0; i < MAXRLIGHTMAPS; i++)
	{
		shaderstate.dummybatch.lightmap[i] = -1;
		shaderstate.dummybatch.lmlightstyle[i] = INVALID_LIGHTSTYLE;
		shaderstate.dummybatch.vtlightstyle[i] = ~0;
	}

#ifdef RTLIGHTS
	Sh_CheckSettings();
	if ((r_shadow_realtime_dlight.ival && r_shadow_shadowmapping.ival) ||
		(r_shadow_realtime_world.ival  && r_shadow_shadowmapping.ival) )
	{	//we expect some lights that will need shadowmapped shadows.
		GLBE_RegisterLightShader(LSHADER_SMAP);
		GLBE_RegisterLightShader(LSHADER_SMAP|LSHADER_CUBE);
		GLBE_RegisterLightShader(LSHADER_SMAP|LSHADER_SPOT);
	}
	if ((r_shadow_realtime_dlight.ival && (!r_shadow_shadowmapping.ival || !r_shadow_realtime_dlight_shadows.ival)) ||
		(r_shadow_realtime_world.ival  && (!r_shadow_shadowmapping.ival || !r_shadow_realtime_world_shadows.ival )) )
	{	//these are also used when there's no shadow.
		//FIXME: should also happen if there's static world lights without shadows. Move elsewhere?
		GLBE_RegisterLightShader(LSHADER_STANDARD);
		GLBE_RegisterLightShader(LSHADER_STANDARD|LSHADER_CUBE);
		GLBE_RegisterLightShader(LSHADER_STANDARD|LSHADER_SPOT);
	}
#endif

	gl_overbright.modified = true; /*in case the d3d renderer does the same*/
	/*lock the cvar down if the backend can't actually do it*/
	if (
#if 1//defined(QUAKETC)
		//TCs are expected to be using glsl and weird overbright things etc, don't take the risk.
		(!sh_config.progs_supported)
#else
		//Q3 can get away with tex_env_combine for everything, if only because the content allows everything to be flattened to a single pass if needed...
		//some shaders might screw up from our approach though...
		(!gl_config.tex_env_combine && !gl_config_nofixedfunc)
#endif
		&& gl_overbright.ival)
		Cvar_ApplyLatchFlag(&gl_overbright, "0", CVAR_RENDERERLATCH, 0);
	shaderstate.shaderbits = ~SBITS_ATEST_BITS;
	BE_SendPassBlendDepthMask(0);
	currententity = &r_worldentity;

	shaderstate.fogtexture = r_nulltex;

	shaderstate.depthonlyshader = R_RegisterShader("depthonly", SUF_NONE,
				"{\n"
					"program depthonly\n"
					"{\n"
						"depthwrite\n"
						"maskcolor\n"
					"}\n"
				"}\n"
			);
	//if the shader doesn't work, give up with that.
	if (!shaderstate.depthonlyshader || !shaderstate.depthonlyshader->prog)
		shaderstate.depthonlyshader = NULL;

	//make sure the world draws correctly
	r_worldentity.shaderRGBAf[0] = 1;
	r_worldentity.shaderRGBAf[1] = 1;
	r_worldentity.shaderRGBAf[2] = 1;
	r_worldentity.shaderRGBAf[3] = 1;
	r_worldentity.axis[0][0] = 1;
	r_worldentity.axis[1][1] = 1;
	r_worldentity.axis[2][2] = 1;
	r_worldentity.light_avg[0] = 1;
	r_worldentity.light_avg[1] = 1;
	r_worldentity.light_avg[2] = 1;

	R_InitFlashblends();

	memset(&shaderstate.streamvbo, 0, sizeof(shaderstate.streamvbo));
	memset(&shaderstate.streamebo, 0, sizeof(shaderstate.streamebo));
	memset(&shaderstate.streamvao, 0, sizeof(shaderstate.streamvao));
	//only do this where we have to.
	if (qglBufferDataARB && gl_config_nofixedfunc
#ifndef FTE_TARGET_WEB
		&& !gl_config_gles
#endif
		)
	{
		qglGenBuffersARB(sizeof(shaderstate.streamvbo)/sizeof(shaderstate.streamvbo[0]), shaderstate.streamvbo);
		qglGenBuffersARB(sizeof(shaderstate.streamebo)/sizeof(shaderstate.streamebo[0]), shaderstate.streamebo);
		if (qglGenVertexArrays)
			qglGenVertexArrays(sizeof(shaderstate.streamvao)/sizeof(shaderstate.streamvao[0]), shaderstate.streamvao);
	}
}

//end tables

#define MAX_ARRAY_VERTS 65536
static vecV_t		vertexarray[MAX_ARRAY_VERTS];
#if 1//ndef GLSLONLY
static avec4_t		coloursarray[MAX_ARRAY_VERTS];
#ifdef FTE_TARGET_WEB
static float		texcoordarray[1][MAX_ARRAY_VERTS*2];
#else
static float		texcoordarray[SHADER_PASS_MAX][MAX_ARRAY_VERTS*2];
#endif

/*========================================== texture coord generation =====================================*/

static void tcgen_environment(float *st, unsigned int numverts, float *xyz, float *normal)
{
	int			i;
	vec3_t		viewer, reflected;
	float		d;

	vec3_t		rorg;


	RotateLightVector(shaderstate.curentity->axis, shaderstate.curentity->origin, r_origin, rorg);

	for (i = 0 ; i < numverts ; i++, xyz += sizeof(vecV_t)/sizeof(vec_t), normal += 3, st += 2 )
	{
		VectorSubtract (rorg, xyz, viewer);
		VectorNormalizeFast (viewer);

		d = DotProduct (normal, viewer);

		reflected[0] = normal[0]*2*d - viewer[0];
		reflected[1] = normal[1]*2*d - viewer[1];
		reflected[2] = normal[2]*2*d - viewer[2];

		st[0] = 0.5 + reflected[1] * 0.5;
		st[1] = 0.5 - reflected[2] * 0.5;
	}
}

//nettest: GOLDSRC STUDIO CHROME, which is not what tcgen environment does.
//
//StudioSetupChrome (StudioModelRenderer.cpp) builds the chrome s/t axes from the
//VIEW's right and up vectors and then maps the vertex NORMAL onto them:
//
//	CrossProduct(bonedir, m_vRight, chromeupvec);
//	CrossProduct(bonedir, chromeupvec, chromerightvec);
//	pchrome[0] = (DotProduct(normal, chromeright) + 1.0) * 32;   // 64-texel texture
//	pchrome[1] = (DotProduct(normal, chromeup)    + 1.0) * 32;
//
//i.e. a MATCAP: the pattern is fixed to the SCREEN and the model turns inside it.
//tcgen environment is Q3's reflection map instead, and it takes components [1] and
//[2] of a reflection vector computed in the ENTITY's own frame - so the pattern is
//fixed to the MODEL and slides across it as the model rotates.  On a standing
//scientist that swims; on a corpse, whose entity axis is rotated ninety degrees,
//the axes it happens to pick are the wrong two entirely, which is the reported
//"bottom of foot ... looks flickery and streched" (hunger/scientist.mdl's tex[28]
//"Sci2_Chrome1.bmp", flags 0x3 = FLATSHADE|CHROME - the one flagged texture on the
//whole model).
//
//The per-BONE refinement is deliberately not reproduced: it re-derives the axes
//per bone from the direction to that bone, which only bends the mapping for bones
//well off the view centre, and the vertex normals here are already in model space
//with no bone index to hand at this stage.  What matters - and what was wrong - is
//that the basis follows the camera rather than the model.
//
//Its own tcgen rather than a change to tcgen_environment, because every Q3 map in
//the corpus uses `tcgen environment` for real reflections and must not move.
static void tcgen_chrome(float *st, unsigned int numverts, float *normal)
{
	int i;
	vec3_t sax, tax;

	//The camera's right/up expressed in the entity's own frame.  entity->axis[j]
	//is the j'th model axis in world space, so dotting against it picks out the
	//j'th model-space component - the transpose, which for an orthonormal basis is
	//the inverse.
	sax[0] = DotProduct(vright, shaderstate.curentity->axis[0]);
	sax[1] = DotProduct(vright, shaderstate.curentity->axis[1]);
	sax[2] = DotProduct(vright, shaderstate.curentity->axis[2]);
	tax[0] = DotProduct(vup,    shaderstate.curentity->axis[0]);
	tax[1] = DotProduct(vup,    shaderstate.curentity->axis[1]);
	tax[2] = DotProduct(vup,    shaderstate.curentity->axis[2]);

	for (i = 0; i < numverts; i++, normal += 3, st += 2)
	{
		st[0] = 0.5 + DotProduct(normal, sax) * 0.5;
		st[1] = 0.5 - DotProduct(normal, tax) * 0.5;
	}
}

#ifndef GLSLONLY
static void tcgen_fog(float *st, unsigned int numverts, float *xyz, mfog_t *fog)
{
	int			i;

	float z;
	float eye, point;
	vec4_t zmat;

	//generate a simple matrix to calc only the projected z coord
	zmat[0] = -shaderstate.modelviewmatrix[2];
	zmat[1] = -shaderstate.modelviewmatrix[6];
	zmat[2] = -shaderstate.modelviewmatrix[10];
	zmat[3] = -shaderstate.modelviewmatrix[14];

	Vector4Scale(zmat, shaderstate.fogfar, zmat);

	if (fog && fog->visibleplane)
	{
		eye = (DotProduct(r_refdef.vieworg, fog->visibleplane->normal) - fog->visibleplane->dist);
		if (eye < 1)
			eye = 1;	//avoids a nan

		for (i = 0 ; i < numverts ; i++, xyz += sizeof(vecV_t)/sizeof(vec_t), st += 2 )
		{
			z = DotProduct(xyz, zmat) + zmat[3];
			st[0] = z;

			if (fog->visibleplane)
				point = (DotProduct(xyz, fog->visibleplane->normal) - fog->visibleplane->dist);
			else
				point = 1;
			st[1] = point / (point - eye);
			if (st[1] > 31/32.0)
				st[1] = 31/32.0;
		}
	}
	else
	{
		for (i = 0 ; i < numverts ; i++, xyz += sizeof(vecV_t)/sizeof(vec_t), st += 2 )
		{
			z = DotProduct(xyz, zmat) + zmat[3];
			st[0] = z;
			st[1] = 31/32.0;
		}
	}
}
static void GenerateTCFog(int passnum, mfog_t *fog)
{
	int m;
	mesh_t *mesh;
	for (m = 0; m < shaderstate.meshcount; m++)
	{
		mesh = shaderstate.meshes[m];
		if (!mesh->xyz_array)
			memset(texcoordarray[passnum]+mesh->vbofirstvert*2, 0, mesh->numvertexes*sizeof(float)*2);
		else
			tcgen_fog(texcoordarray[passnum]+mesh->vbofirstvert*2, mesh->numvertexes, (float*)mesh->xyz_array, fog);
	}

	shaderstate.pendingtexcoordparts[passnum] = 2;
	shaderstate.pendingtexcoordvbo[passnum] = 0;
	shaderstate.pendingtexcoordpointer[passnum] = texcoordarray[passnum];
}
#endif

static float *tcgen3(const shaderpass_t *pass, int cnt, float *dst, const mesh_t *mesh)
{
	int i;
	vecV_t *src;
	switch (pass->tcgen)
	{
	default:
	case TC_GEN_SKYBOX:
		src = mesh->xyz_array;
		for (i = 0; i < cnt; i++, dst += 3)
		{
			dst[0] = src[i][0] - r_refdef.vieworg[0];
			dst[1] = src[i][1] - r_refdef.vieworg[1];
			dst[2] = src[i][2] - r_refdef.vieworg[2];
		}
		return dst-cnt*3;

//	case TC_GEN_WOBBLESKY:
//	case TC_GEN_REFLECT:
//		break;
	}
}
static float *tcgen(const shaderpass_t *pass, int cnt, float *dst, const mesh_t *mesh)
{
	int i;
	vecV_t *src;
	switch (pass->tcgen)
	{
	default:
	case TC_GEN_BASE:
		return (float*)mesh->st_array;
	case TC_GEN_LIGHTMAP:
		if (!mesh->lmst_array[0])
			return (float*)mesh->st_array;
		else
			return (float*)mesh->lmst_array[0];
	case TC_GEN_NORMAL:
		return (float*)mesh->normals_array;
	case TC_GEN_SVECTOR:
		return (float*)mesh->snormals_array;
	case TC_GEN_TVECTOR:
		return (float*)mesh->tnormals_array;
	case TC_GEN_ENVIRONMENT:
		if (!mesh->normals_array)
			return (float*)mesh->st_array;
		tcgen_environment(dst, cnt, (float*)mesh->xyz_array, (float*)mesh->normals_array);
		return dst;

	case TC_GEN_CHROME:	//nettest: GoldSrc studio chrome, see tcgen_chrome
		if (!mesh->normals_array)
			return (float*)mesh->st_array;
		tcgen_chrome(dst, cnt, (float*)mesh->normals_array);
		return dst;

//	case TC_GEN_DOTPRODUCT:
//		return mesh->st_array[0];
	case TC_GEN_VECTOR:
		src = mesh->xyz_array;
		for (i = 0; i < cnt; i++, dst += 2)
		{
			dst[0] = DotProduct(pass->tcgenvec[0], src[i]);
			dst[1] = DotProduct(pass->tcgenvec[1], src[i]);
		}
		return dst-cnt*2;

//	case TC_GEN_SKYBOX:
//	case TC_GEN_WOBBLESKY:
//	case TC_GEN_REFLECT:
//		break;
	}
}

/*src and dst can be the same address when tcmods are chained*/
static void tcmod(const tcmod_t *tcmod, int cnt, const float *src, float *dst, const mesh_t *mesh)
{
	float *table;
	float t1, t2;
	float cost, sint;
	int j;
	switch (tcmod->type)
	{
		case SHADER_TCMOD_ROTATE:
			cost = tcmod->args[0] * shaderstate.curtime;
			sint = R_FastSin(cost);
			cost = R_FastSin(cost + 0.25);

			for (j = 0; j < cnt; j++, dst+=2,src+=2)
			{
				t1 = cost * (src[0] - 0.5f) - sint * (src[1] - 0.5f) + 0.5f;
				t2 = cost * (src[1] - 0.5f) + sint * (src[0] - 0.5f) + 0.5f;
				dst[0] = t1;
				dst[1] = t2;
			}
			break;

		case SHADER_TCMOD_SCALE:
			t1 = tcmod->args[0];
			t2 = tcmod->args[1];

			for (j = 0; j < cnt; j++, dst+=2,src+=2)
			{
				dst[0] = src[0] * t1;
				dst[1] = src[1] * t2;
			}
			break;

		case SHADER_TCMOD_TURB:
			t1 = tcmod->args[2] + shaderstate.curtime * tcmod->args[3];
			t2 = tcmod->args[1];

			for (j = 0; j < cnt; j++, dst+=2,src+=2)
			{
				dst[0] = src[0] + R_FastSin (src[0]*t2+t1) * t2;
				dst[1] = src[1] + R_FastSin (src[1]*t2+t1) * t2;
			}
			break;

		case SHADER_TCMOD_STRETCH:
			table = FTableForFunc(tcmod->args[0]);
			t2 = tcmod->args[3] + shaderstate.curtime * tcmod->args[4];
			t1 = FTABLE_EVALUATE(table, t2) * tcmod->args[2] + tcmod->args[1];
			t1 = t1 ? 1.0f / t1 : 1.0f;
			t2 = 0.5f - 0.5f * t1;
			for (j = 0; j < cnt; j++, dst+=2,src+=2)
			{
				dst[0] = src[0] * t1 + t2;
				dst[1] = src[1] * t1 + t2;
			}
			break;

		case SHADER_TCMOD_SCROLL:
			t1 = tcmod->args[0] * shaderstate.curtime;
			t2 = tcmod->args[1] * shaderstate.curtime;

			for (j = 0; j < cnt; j++, dst += 2, src+=2)
			{
				dst[0] = src[0] + t1;
				dst[1] = src[1] + t2;
			}
			break;

		case SHADER_TCMOD_TRANSFORM:
			for (j = 0; j < cnt; j++, dst+=2, src+=2)
			{
				t1 = src[0];
				t2 = src[1];
				dst[0] = t1 * tcmod->args[0] + t2 * tcmod->args[2] + tcmod->args[4];
				dst[1] = t1 * tcmod->args[1] + t2 * tcmod->args[3] + tcmod->args[5];
			}
			break;

		case SHADER_TCMOD_PAGE:
			{	//simple atlas bias with no scaling. use a separate tcmod for that.
				int w = tcmod->args[0];
				int h = tcmod->args[1];
				float f = shaderstate.curtime / (tcmod->args[2]*w*h);
				int idx = (f - floor(f))*w*h;
				t1 = (idx%w)/tcmod->args[0];
				t2 = (idx/w)/tcmod->args[1];

				for (j = 0; j < cnt; j++, dst += 2, src+=2)
				{
					dst[0] = src[0] + t1;
					dst[1] = src[1] + t2;
				}
			}
			break;

		default:
			break;
	}
}

static void GenerateTCMods3(const shaderpass_t *pass, int passnum)
{
#if 1
	int m;
	float *src;
	mesh_t *mesh;
	for (m = 0; m < shaderstate.meshcount; m++)
	{
		mesh = shaderstate.meshes[m];

		src = tcgen3(pass, mesh->numvertexes, texcoordarray[passnum]+mesh->vbofirstvert*3, mesh);

		if (src != texcoordarray[passnum]+mesh->vbofirstvert*3)
		{
			//this shouldn't actually ever be true
			memcpy(texcoordarray[passnum]+mesh->vbofirstvert*3, src, sizeof(vec3_t)*mesh->numvertexes);
		}
	}
	shaderstate.pendingtexcoordparts[passnum] = 3;
	shaderstate.pendingtexcoordvbo[passnum] = 0;
	shaderstate.pendingtexcoordpointer[passnum] = texcoordarray[passnum];
#else
	GL_DeselectVAO();
	if (!shaderstate.vbo_texcoords[passnum])
	{
		shaderstate.vbo_texcoords[passnum] = 0;
		qglGenBuffersARB(1, &shaderstate.vbo_texcoords[passnum]);
	}
	GL_SelectVBO(shaderstate.vbo_texcoords[passnum]);

	{
		qglBufferDataARB(GL_ARRAY_BUFFER_ARB, MAX_ARRAY_VERTS*sizeof(float)*3, NULL, GL_STREAM_DRAW_ARB);
		for (; meshlist; meshlist = meshlist->next)
		{
			int i;
			float *src;
			src = tcge3n(pass, meshlist->numvertexes, texcoordarray[passnum], meshlist);
			qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB, meshlist->vbofirstvert*8, meshlist->numvertexes*8, src);
		}
	}

	shaderstate.pendingtexcoordparts[passnum] = 2;
	shaderstate.pendingtexcoordvbo[passnum] = shaderstate.vbo_texcoords[passnum];
	shaderstate.pendingtexcoordpointer[passnum] = NULL;
#endif
}

static void GenerateTCMods(const shaderpass_t *pass, int passnum)
{
#if 1
	int i, m;
	float *src;
	mesh_t *mesh;
	for (m = 0; m < shaderstate.meshcount; m++)
	{
		mesh = shaderstate.meshes[m];

		src = tcgen(pass, mesh->numvertexes, texcoordarray[passnum]+mesh->vbofirstvert*2, mesh);
		//tcgen might return unmodified info
		if (!src)
		{	//don't crash... not much else we can do.
			shaderstate.pendingtexcoordparts[passnum] = 2;
			shaderstate.pendingtexcoordvbo[passnum] = shaderstate.sourcevbo->texcoord.gl.vbo;
			shaderstate.pendingtexcoordpointer[passnum] = shaderstate.sourcevbo->texcoord.gl.addr;
		}
		else if (pass->numtcmods)
		{
			tcmod(&pass->tcmods[0], mesh->numvertexes, src, texcoordarray[passnum]+mesh->vbofirstvert*2, mesh);
			for (i = 1; i < pass->numtcmods; i++)
			{
				tcmod(&pass->tcmods[i], mesh->numvertexes, texcoordarray[passnum]+mesh->vbofirstvert*2, texcoordarray[passnum]+mesh->vbofirstvert*2, mesh);
			}
			src = texcoordarray[passnum]+mesh->vbofirstvert*2;
		}
		else if (src != texcoordarray[passnum]+mesh->vbofirstvert*2)
		{
			//this shouldn't actually ever be true
			memcpy(texcoordarray[passnum]+mesh->vbofirstvert*2, src, 8*mesh->numvertexes);
		}
	}
	shaderstate.pendingtexcoordparts[passnum] = 2;
	shaderstate.pendingtexcoordvbo[passnum] = 0;
	shaderstate.pendingtexcoordpointer[passnum] = texcoordarray[passnum];
#else
	GL_DeselectVAO();
	if (!shaderstate.vbo_texcoords[passnum])
	{
		shaderstate.vbo_texcoords[passnum] = 0;
		qglGenBuffersARB(1, &shaderstate.vbo_texcoords[passnum]);
	}
	GL_SelectVBO(shaderstate.vbo_texcoords[passnum]);

	{
		qglBufferDataARB(GL_ARRAY_BUFFER_ARB, MAX_ARRAY_VERTS*sizeof(float)*2, NULL, GL_STREAM_DRAW_ARB);
		for (; meshlist; meshlist = meshlist->next)
		{
			int i;
			float *src;
			src = tcgen(pass, meshlist->numvertexes, texcoordarray[passnum], meshlist);
			//tcgen might return unmodified info
			if (pass->numtcmods)
			{
				tcmod(&pass->tcmods[0], meshlist->numvertexes, src, texcoordarray[passnum], meshlist);
				for (i = 1; i < pass->numtcmods; i++)
				{
					tcmod(&pass->tcmods[i], meshlist->numvertexes, texcoordarray[passnum], texcoordarray[passnum], meshlist);
				}
				src = texcoordarray[passnum];
			}
			qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB, meshlist->vbofirstvert*8, meshlist->numvertexes*8, src);
		}
	}

	shaderstate.pendingtexcoordparts[passnum] = 2;
	shaderstate.pendingtexcoordvbo[passnum] = shaderstate.vbo_texcoords[passnum];
	shaderstate.pendingtexcoordpointer[passnum] = NULL;
#endif
}

//end texture coords
/*========================================== colour generation =====================================*/

//source is always packed
//dest is packed too
static void colourgen(const shaderpass_t *pass, int cnt, vec4_t *src, vec4_t *dst, const mesh_t *mesh)
{
	switch (pass->rgbgen)
	{
	case RGB_GEN_ENTITY:
		while((cnt)--)
		{
			dst[cnt][0] = shaderstate.curentity->shaderRGBAf[0];
			dst[cnt][1] = shaderstate.curentity->shaderRGBAf[1];
			dst[cnt][2] = shaderstate.curentity->shaderRGBAf[2];
		}
		break;
	case RGB_GEN_ONE_MINUS_ENTITY:
		while((cnt)--)
		{
			dst[cnt][0] = 1-shaderstate.curentity->shaderRGBAf[0];
			dst[cnt][1] = 1-shaderstate.curentity->shaderRGBAf[1];
			dst[cnt][2] = 1-shaderstate.curentity->shaderRGBAf[2];
		}
		break;
	case RGB_GEN_VERTEX_LIGHTING:
#if MAXRLIGHTMAPS > 1
		if (mesh->colors4f_array[1])
		{
			float lm[MAXRLIGHTMAPS];
			lm[0] = d_lightstylevalue[shaderstate.curbatch->vtlightstyle[0]]/256.0f*shaderstate.identitylighting;
			lm[1] = d_lightstylevalue[shaderstate.curbatch->vtlightstyle[1]]/256.0f*shaderstate.identitylighting;
			lm[2] = d_lightstylevalue[shaderstate.curbatch->vtlightstyle[2]]/256.0f*shaderstate.identitylighting;
			lm[3] = d_lightstylevalue[shaderstate.curbatch->vtlightstyle[3]]/256.0f*shaderstate.identitylighting;
			while((cnt)--)
			{
				VectorScale(		mesh->colors4f_array[0][cnt], lm[0], dst[cnt]);
				VectorMA(dst[cnt],	lm[1], mesh->colors4f_array[1][cnt], dst[cnt]);
				VectorMA(dst[cnt],	lm[2], mesh->colors4f_array[2][cnt], dst[cnt]);
				VectorMA(dst[cnt],	lm[3], mesh->colors4f_array[3][cnt], dst[cnt]);
			}
			break;
		}
#endif

		if (shaderstate.identitylighting != 1)
		{
			if (!src)
			{
				while((cnt)--)
				{
					dst[cnt][0] = shaderstate.identitylighting;
					dst[cnt][1] = shaderstate.identitylighting;
					dst[cnt][2] = shaderstate.identitylighting;
				}
				break;
			}
			while((cnt)--)
			{
				dst[cnt][0] = src[cnt][0]*shaderstate.identitylighting;
				dst[cnt][1] = src[cnt][1]*shaderstate.identitylighting;
				dst[cnt][2] = src[cnt][2]*shaderstate.identitylighting;
			}
			break;
		}
	case RGB_GEN_VERTEX_EXACT:
		if (!src)
		{
			while((cnt)--)
			{
				dst[cnt][0] = 1;
				dst[cnt][1] = 1;
				dst[cnt][2] = 1;
			}
			break;
		}

		while((cnt)--)
		{
			dst[cnt][0] = src[cnt][0];
			dst[cnt][1] = src[cnt][1];
			dst[cnt][2] = src[cnt][2];
		}
		break;
	case RGB_GEN_ONE_MINUS_VERTEX:
		while((cnt)--)
		{
			dst[cnt][0] = 1-src[cnt][0];
			dst[cnt][1] = 1-src[cnt][1];
			dst[cnt][2] = 1-src[cnt][2];
		}
		break;
	case RGB_GEN_IDENTITY_LIGHTING:
		if (shaderstate.curbatch->vtlightstyle[0] != 255 && d_lightstylevalue[shaderstate.curbatch->vtlightstyle[0]] != 256)
		{
			//FIXME: 
			while((cnt)--)
			{
				dst[cnt][0] = shaderstate.identitylighting * d_lightstylevalue[shaderstate.curbatch->vtlightstyle[0]]/256.0f;
				dst[cnt][1] = shaderstate.identitylighting * d_lightstylevalue[shaderstate.curbatch->vtlightstyle[0]]/256.0f;
				dst[cnt][2] = shaderstate.identitylighting * d_lightstylevalue[shaderstate.curbatch->vtlightstyle[0]]/256.0f;
			}
		}
		else
		{
			//compensate for overbrights
			while((cnt)--)
			{
				dst[cnt][0] = shaderstate.identitylighting;
				dst[cnt][1] = shaderstate.identitylighting;
				dst[cnt][2] = shaderstate.identitylighting;
			}
		}
		break;
	case RGB_GEN_IDENTITY_OVERBRIGHT:
		while((cnt)--)
		{
			dst[cnt][0] = shaderstate.identitylightmap;
			dst[cnt][1] = shaderstate.identitylightmap;
			dst[cnt][2] = shaderstate.identitylightmap;
		}
		break;
	default:
	case RGB_GEN_IDENTITY:
		while((cnt)--)
		{
			dst[cnt][0] = shaderstate.identitylighting;
			dst[cnt][1] = shaderstate.identitylighting;
			dst[cnt][2] = shaderstate.identitylighting;
		}
		break;
	case RGB_GEN_CONST:
		while((cnt)--)
		{
			dst[cnt][0] = pass->rgbgen_func.args[0];
			dst[cnt][1] = pass->rgbgen_func.args[1];
			dst[cnt][2] = pass->rgbgen_func.args[2];
		}
		break;
	case RGB_GEN_ENTITY_LIGHTING_DIFFUSE:
		R_LightArrays(shaderstate.curentity, mesh->xyz_array, dst, cnt, mesh->normals_array, shaderstate.identitylighting, true);
		break;
	case RGB_GEN_LIGHTING_DIFFUSE:
		R_LightArrays(shaderstate.curentity, mesh->xyz_array, dst, cnt, mesh->normals_array, shaderstate.identitylighting, false);
		break;
	case RGB_GEN_WAVE:
		{
			float *table;
			float c;

			table = FTableForFunc(pass->rgbgen_func.type);
			c = pass->rgbgen_func.args[2] + shaderstate.curtime * pass->rgbgen_func.args[3];
			c = FTABLE_EVALUATE(table, c) * pass->rgbgen_func.args[1] + pass->rgbgen_func.args[0];
			c = bound(0.0f, c, 1.0f);

			while((cnt)--)
			{
				dst[cnt][0] = c;
				dst[cnt][1] = c;
				dst[cnt][2] = c;
			}
		}
		break;

	case RGB_GEN_TOPCOLOR:
		if (cnt)
		{
			R_FetchPlayerColour(shaderstate.curentity->topcolour, dst[0]);
			while((cnt)--)
			{
				dst[cnt][0] = dst[0][0];
				dst[cnt][1] = dst[0][1];
				dst[cnt][2] = dst[0][2];
			}
		}
		break;
	case RGB_GEN_BOTTOMCOLOR:
		if (cnt)
		{
			R_FetchPlayerColour(shaderstate.curentity->bottomcolour, dst[0]);
			while((cnt)--)
			{
				dst[cnt][0] = dst[0][0];
				dst[cnt][1] = dst[0][1];
				dst[cnt][2] = dst[0][2];
			}
		}
		break;
	}
}
#endif

static void BE_GenTempMeshVBO(vbo_t **vbo, mesh_t *m);
static void DeformGen_Text(int stringid, int cnt, vecV_t *src, vecV_t *dst, const mesh_t *mesh)
{
#define maxlen 32
	vecV_t *textverts = vertexarray;
	static vec2_t texttc[maxlen*4];
	extern index_t	r_quad_indexes[];
	static mesh_t textmesh, *meshptr = &textmesh;
	int i;
	vec3_t org;
	vec3_t right;
	vec3_t down;
	float s, t, d;
	char cvarname[64];
	const char *text;
	Q_snprintfz(cvarname, sizeof(cvarname), "r_shadertext_%i", stringid);
	text = Cvar_Get(cvarname, "", 0, "Shader System")->string;
	VectorCopy(mesh->snormals_array[0], right);
	VectorNegate(mesh->tnormals_array[0], down);
	CrossProduct(right, down, org);
	if (DotProduct(mesh->normals_array[0], org) > 0)
		VectorNegate(right, right);
	VectorClear(org);
	for (i = 0; i < cnt; i++)
		VectorAdd(org, src[i], org);
	VectorScale(org, 1.0/i, org);
	for (i = 0, s = 0; i < 4; i++)
	{
		d = DotProduct(right, src[i]) - DotProduct(right, org);
		if (s < d)
			s = d;
	}
	i = strlen(text);
	VectorScale(right, 2*s/i, right);
	VectorScale(down, 2*s/i, down);
	VectorMA(org, -i*0.5, right, org);

	memset(&textmesh, 0, sizeof(textmesh));
	textmesh.indexes = r_quad_indexes;
	textmesh.xyz_array = textverts;
	textmesh.st_array = texttc;

	org[1] += 0;

	for (i = 0; i < maxlen; )
	{
		qbyte c = *text++;
		if (!c)
			break;
		if (c != ' ')
		{
			const float sz = 1 / 16.0f;
			s = (c&15)*sz;
			t = (c>>4)*sz;
			VectorCopy(org, textverts[i*4+0]);
			Vector2Set(texttc[i*4+0], s, t);

			VectorAdd(textverts[i*4+0], right, textverts[i*4+1]);
			Vector2Set(texttc[i*4+1], s+sz, t);

			VectorAdd(textverts[i*4+1], down, textverts[i*4+2]);
			Vector2Set(texttc[i*4+2], s+sz, t+sz);

			VectorAdd(textverts[i*4+0], down, textverts[i*4+3]);
			Vector2Set(texttc[i*4+3], s, t+sz);
			i++;
		}
		VectorAdd(org, right, org);
	}
	textmesh.numindexes = i*6;
	textmesh.numvertexes = i*4;

	BE_GenTempMeshVBO(&shaderstate.sourcevbo, &textmesh);

	shaderstate.meshcount = 1;
	shaderstate.meshes = &meshptr;
#undef maxlen
}
static void deformgen(const deformv_t *deformv, int cnt, vecV_t *src, vecV_t *dst, const mesh_t *mesh)
{
	float *table;
	int j, k;
	float args[4];
	float deflect;
	switch (deformv->type)
	{
	default:
	case DEFORMV_NONE:
		if (src != dst)
			memcpy(dst, src, sizeof(*src)*cnt);
		break;

	case DEFORMV_WAVE:
		if (!mesh->normals_array)
		{
			if (src != dst)
				memcpy(dst, src, sizeof(*src)*cnt);
			return;
		}
		args[0] = deformv->func.args[0];
		args[1] = deformv->func.args[1];
		args[3] = deformv->func.args[2] + deformv->func.args[3] * shaderstate.curtime;
		table = FTableForFunc(deformv->func.type);

		for ( j = 0; j < cnt; j++ )
		{
			deflect = deformv->args[0] * (src[j][0]+src[j][1]+src[j][2]) + args[3];
			deflect = FTABLE_EVALUATE(table, deflect) * args[1] + args[0];

			// Deflect vertex along its normal by wave amount
			VectorMA(src[j], deflect, mesh->normals_array[j], dst[j]);
		}
		break;

	case DEFORMV_RIPPLE:
		//nettest: interactive water ripples.  Sum every live expanding-ring source (r_waterripples,
		//fed by R_AddWaterRipple) as a gaussian-windowed cosine riding an outward-growing wavefront,
		//and push the vertex up/down in Z on top of whatever the wave deform already did.  Pure Z
		//(these are up-facing liquid tops), so no normals needed and xy are left untouched.
		if (src != dst)
			memcpy(dst, src, sizeof(*src)*cnt);
		if (r_waterripple_react.value > 0)
		{
			float react = r_waterripple_react.value;
			float now = shaderstate.updatetime;
			for ( j = 0; j < cnt; j++ )
			{
				float disp = 0;
				for ( k = 0; k < MAX_WATERRIPPLES; k++ )
				{
					const waterripple_t *rip = &r_waterripples[k];
					float age = now - rip->starttime;
					float front, dx, dy, d2, lo, hi, d, x, env, fade;
					if (rip->amp <= 0 || age < 0 || age >= rip->lifetime)
						continue;
					front = age * rip->speed;
					dx = src[j][0] - rip->origin[0];
					dy = src[j][1] - rip->origin[1];
					d2 = dx*dx + dy*dy;
					//only vertices near the current ring radius move -- skip the rest before the sqrt
					lo = front - 3.0f*rip->size;
					hi = front + 3.0f*rip->size;
					if (lo < 0)
						lo = 0;
					if (d2 > hi*hi || d2 < lo*lo)
						continue;
					d = sqrt(d2);
					x = (d - front) / rip->size;			//offset from the wavefront, in 'size' units
					if (x < -3.0f || x > 3.0f)
						continue;
					env = exp(-x*x);						//gaussian ring band
					fade = 1.0f - age / rip->lifetime;
					fade *= fade;							//ease-out over the ripple's life
					disp += rip->amp * fade * env * cos(x * 3.14159265f);
				}
				dst[j][2] += disp * react;
			}
		}
		break;

	case DEFORMV_NORMAL:
		//normal does not actually move the verts, but it does change the normals array
		//we don't currently support that.
		if (src != dst)
			memcpy(dst, src, sizeof(*src)*cnt);
/*
		args[0] = deformv->args[1] * shaderstate.curtime;

		for ( j = 0; j < cnt; j++ )
		{
			args[1] = normalsArray[j][2] * args[0];

			deflect = deformv->args[0] * R_FastSin(args[1]);
			normalsArray[j][0] *= deflect;
			deflect = deformv->args[0] * R_FastSin(args[1] + 0.25);
			normalsArray[j][1] *= deflect;
			VectorNormalizeFast(normalsArray[j]);
		}
*/		break;

	case DEFORMV_MOVE:
		table = FTableForFunc(deformv->func.type);
		deflect = deformv->func.args[2] + shaderstate.curtime * deformv->func.args[3];
		deflect = FTABLE_EVALUATE(table, deflect) * deformv->func.args[1] + deformv->func.args[0];

		for ( j = 0; j < cnt; j++ )
			VectorMA(src[j], deflect, deformv->args, dst[j]);
		break;

	case DEFORMV_BULGE:
		if (!mesh->normals_array)
		{
			if (src != dst)
				memcpy(dst, src, sizeof(*src)*cnt);
			break;
		}
		args[0] = deformv->args[0]/(2*M_PI);
		args[1] = deformv->args[1];
		args[2] = shaderstate.curtime * deformv->args[2]/(2*M_PI);

		for (j = 0; j < cnt; j++)
		{
			deflect = R_FastSin(mesh->st_array[j][0]*args[0] + args[2])*args[1];
			dst[j][0] = src[j][0]+deflect*mesh->normals_array[j][0];
			dst[j][1] = src[j][1]+deflect*mesh->normals_array[j][1];
			dst[j][2] = src[j][2]+deflect*mesh->normals_array[j][2];
		}
		break;

	case DEFORMV_AUTOSPRITE:
		if (mesh->numindexes < 6)
			break;

		for (j = 0; j+3 < cnt; j+=4, src+=4, dst+=4)
		{
			vec3_t mid, d;
			vec2_t mid2;
			float radius, s,t;
			vec2_t *fte_restrict st = &mesh->st_array[j];
			mid[0] = 0.25*(src[0][0] + src[1][0] + src[2][0] + src[3][0]);
			mid[1] = 0.25*(src[0][1] + src[1][1] + src[2][1] + src[3][1]);
			mid[2] = 0.25*(src[0][2] + src[1][2] + src[2][2] + src[3][2]);
			VectorSubtract(src[0], mid, d);
			radius = VectorLength(d) * 0.707;

			mid2[0] = 0.25*(st[0][0] + st[1][0] + st[2][0] + st[3][0]);
			mid2[1] = 0.25*(st[0][1] + st[1][1] + st[2][1] + st[3][1]);

			for (k = 0; k < 4; k++)
			{
				//q3 fully regenerates verts. we don't because that destroys ST coords.
				//even so, if the ST coords are non-centered for some reason then we still need to get the right values as if they were.
				//hence the mid2.
				s = (st[k][0] > mid2[0])?1:-1;
				t = (st[k][1] > mid2[1])?1:-1;
				dst[k][0] = mid[0] + radius*(s*shaderstate.modelviewmatrix[0+0]-t*shaderstate.modelviewmatrix[0+1]);
				dst[k][1] = mid[1] + radius*(s*shaderstate.modelviewmatrix[4+0]-t*shaderstate.modelviewmatrix[4+1]);
				dst[k][2] = mid[2] + radius*(s*shaderstate.modelviewmatrix[8+0]-t*shaderstate.modelviewmatrix[8+1]);
			}
		}
		break;

	case DEFORMV_AUTOSPRITE2:
		if (mesh->numindexes < 6)
			break;

		for (k = 0; k < mesh->numindexes; k += 6)
		{
			int long_axis, short_axis;
			vec3_t axis;
			float len[3];
			mat3_t m0, m1, m2, result;
			float *quad[4];
			vec3_t rot_centre, tv, tv2;

			quad[0] = (float *)(src + mesh->indexes[k+0]);
			quad[1] = (float *)(src + mesh->indexes[k+1]);
			quad[2] = (float *)(src + mesh->indexes[k+2]);

			for (j = 2; j >= 0; j--)
			{
				quad[3] = (float *)(src + mesh->indexes[k+3+j]);
				if (!VectorEquals (quad[3], quad[0]) &&
					!VectorEquals (quad[3], quad[1]) &&
					!VectorEquals (quad[3], quad[2]))
				{
					break;
				}
			}

			// build a matrix were the longest axis of the billboard is the Y-Axis
			VectorSubtract(quad[1], quad[0], m0[0]);
			VectorSubtract(quad[2], quad[0], m0[1]);
			VectorSubtract(quad[2], quad[1], m0[2]);
			len[0] = DotProduct(m0[0], m0[0]);
			len[1] = DotProduct(m0[1], m0[1]);
			len[2] = DotProduct(m0[2], m0[2]);

			if ((len[2] > len[1]) && (len[2] > len[0]))
			{
				if (len[1] > len[0])
				{
					long_axis = 1;
					short_axis = 0;
				}
				else
				{
					long_axis = 0;
					short_axis = 1;
				}
			}
			else if ((len[1] > len[2]) && (len[1] > len[0]))
			{
				if (len[2] > len[0])
				{
					long_axis = 2;
					short_axis = 0;
				}
				else
				{
					long_axis = 0;
					short_axis = 2;
				}
			}
			else //if ( (len[0] > len[1]) && (len[0] > len[2]) )
			{
				if (len[2] > len[1])
				{
					long_axis = 2;
					short_axis = 1;
				}
				else
				{
					long_axis = 1;
					short_axis = 2;
				}
			}

			if (DotProduct(m0[long_axis], m0[short_axis]))
			{
				VectorNormalize2(m0[long_axis], axis);
				VectorCopy(axis, m0[1]);

				if (axis[0] || axis[1])
				{
					VectorVectors(m0[1], m0[2], m0[0]);
				}
				else
				{
					VectorVectors(m0[1], m0[0], m0[2]);
				}
			}
			else
			{
				VectorNormalize2(m0[long_axis], axis);
				VectorNormalize2(m0[short_axis], m0[0]);
				VectorCopy(axis, m0[1]);
				CrossProduct(m0[0], m0[1], m0[2]);
			}

			for (j = 0; j < 3; j++)
				rot_centre[j] = (quad[0][j] + quad[1][j] + quad[2][j] + quad[3][j]) * 0.25;

			if (shaderstate.curentity)
			{
				VectorAdd(shaderstate.curentity->origin, rot_centre, tv);
			}
			else
			{
				VectorCopy(rot_centre, tv);
			}
			VectorSubtract(r_origin, tv, tv);

			// filter any longest-axis-parts off the camera-direction
			deflect = -DotProduct(tv, axis);

			VectorMA(tv, deflect, axis, m1[2]);
			VectorNormalizeFast(m1[2]);
			VectorCopy(axis, m1[1]);
			CrossProduct(m1[1], m1[2], m1[0]);

			Matrix3_Transpose(m1, m2);
			Matrix3_Multiply(m2, m0, result);

			for (j = 0; j < 4; j++)
			{
				int v = ((vecV_t*)quad[j]-src);
				VectorSubtract(quad[j], rot_centre, tv);
				Matrix3_Multiply_Vec3((const vec3_t*)result, tv, tv2);
				VectorAdd(rot_centre, tv2, dst[v]);
			}
		}
		break;

//	case DEFORMV_PROJECTION_SHADOW:
//		break;

	case DEFORMV_TEXT:
		DeformGen_Text(deformv->args[0], cnt, src, dst, mesh);
		break;
	}
}

static void GenerateVertexBlends(const shader_t *shader)
{
	int i, m;
	mesh_t *meshlist;
	vecV_t *ov, *iv1, *iv2;
	float w1, w2;
	for (m = 0; m < shaderstate.meshcount; m++)
	{
		meshlist = shaderstate.meshes[m];

		ov = vertexarray+meshlist->vbofirstvert;
		iv1 = meshlist->xyz_array;
		iv2 = meshlist->xyz2_array;
		w1 = meshlist->xyz_blendw[0];
		w2 = meshlist->xyz_blendw[1];
		for (i = 0; i < meshlist->numvertexes; i++)
		{
			ov[i][0] = iv1[i][0]*w1 + iv2[i][0]*w2;
			ov[i][1] = iv1[i][1]*w1 + iv2[i][1]*w2;
			ov[i][2] = iv1[i][2]*w1 + iv2[i][2]*w2;
		}
		for (i = 0; i < shader->numdeforms; i++)
		{
			deformgen(&shader->deforms[i], meshlist->numvertexes, vertexarray+meshlist->vbofirstvert, vertexarray+meshlist->vbofirstvert, meshlist);
		}
	}

	shaderstate.pendingvertexpointer = vertexarray;
	shaderstate.pendingvertexvbo = 0;
}
static void GenerateVertexDeforms(const shader_t *shader)
{
	int i, m;
	mesh_t *meshlist;
	for (m = 0; m < shaderstate.meshcount; m++)
	{
		meshlist = shaderstate.meshes[m];

		deformgen(&shader->deforms[0], meshlist->numvertexes, meshlist->xyz_array, vertexarray+meshlist->vbofirstvert, meshlist);
		for (i = 1; i < shader->numdeforms; i++)
		{
			deformgen(&shader->deforms[i], meshlist->numvertexes, vertexarray+meshlist->vbofirstvert, vertexarray+meshlist->vbofirstvert, meshlist);
		}
	}

	shaderstate.pendingvertexpointer = vertexarray;
	shaderstate.pendingvertexvbo = 0;
}

#if 1//ndef GLSLONLY

/*======================================alpha ===============================*/

static void alphagen(const shaderpass_t *pass, int cnt, avec4_t *const src, avec4_t *dst, const mesh_t *mesh)
{
	float *table;
	float t;
	float f;
	vec3_t v1, v2;
	int i;

	switch (pass->alphagen)
	{
	default:
	case ALPHA_GEN_IDENTITY:
		if (shaderstate.flags & BEF_FORCETRANSPARENT)
		{
			while(cnt--)
				dst[cnt][3] = shaderstate.curentity->shaderRGBAf[3];
		}
		else
		{
			while(cnt--)
				dst[cnt][3] = 1;
		}
		break;

	case ALPHA_GEN_CONST:
		t = pass->alphagen_func.args[0];
		while(cnt--)
			dst[cnt][3] = t;
		break;

	case ALPHA_GEN_WAVE:
		table = FTableForFunc(pass->alphagen_func.type);
		f = pass->alphagen_func.args[2] + shaderstate.curtime * pass->alphagen_func.args[3];
		f = FTABLE_EVALUATE(table, f) * pass->alphagen_func.args[1] + pass->alphagen_func.args[0];
		t = bound(0.0f, f, 1.0f);
		while(cnt--)
			dst[cnt][3] = t;
		break;

	case ALPHA_GEN_PORTAL:
		//FIXME: should this be per-vert?
		if (r_refdef.recurse || !mesh->xyz_array)
			f = 1;
		else
		{
			VectorAdd(mesh->xyz_array[0], shaderstate.curentity->origin, v1);
			VectorSubtract(r_origin, v1, v2);
			f = VectorLength(v2) * (1.0 / shaderstate.curshader->portaldist);
			f = bound(0.0f, f, 1.0f);
		}

		while(cnt--)
			dst[cnt][3] = f;
		break;

	case ALPHA_GEN_VERTEX:
		if (!src)
		{
			while(cnt--)
			{
				dst[cnt][3] = 1;
			}
			break;
		}

		while(cnt--)
		{
			dst[cnt][3] = src[cnt][3];
		}
		break;

	case ALPHA_GEN_ENTITY:
		f = bound(0, shaderstate.curentity->shaderRGBAf[3], 1);
		while(cnt--)
		{
			dst[cnt][3] = f;
		}
		break;


	case ALPHA_GEN_SPECULAR:
		if (!mesh->normals_array)
		{
			while(cnt--)
				dst[cnt][3] = 0.2;
		}
		else
		{
			VectorSubtract(r_origin, shaderstate.curentity->origin, v1);

			if (!Matrix3_Compare((const vec3_t*)shaderstate.curentity->axis, (const vec3_t*)axisDefault))
			{
				Matrix3_Multiply_Vec3(shaderstate.curentity->axis, v1, v2);
			}
			else
			{
				VectorCopy(v1, v2);
			}

			for (i = 0; i < cnt; i++)
			{
				VectorSubtract(v2, mesh->xyz_array[i], v1);
				f = DotProduct(v1, mesh->normals_array[i] ) * Q_rsqrt(DotProduct(v1,v1));
				f = f * f * f * f * f;
				dst[i][3] = bound (0.0f, f, 1.0f);
			}
		}
		break;
	}
}

static void GenerateColourMods(const shaderpass_t *pass)
{
	unsigned int m;
	mesh_t *meshlist;
	meshlist = shaderstate.meshes[0];

	if (pass->flags & SHADER_PASS_NOCOLORARRAY)
	{
		colourgen(pass, 1, meshlist->colors4f_array[0], &shaderstate.pendingcolourflat, meshlist);
		alphagen(pass, 1, meshlist->colors4f_array[0], &shaderstate.pendingcolourflat, meshlist);
		shaderstate.colourarraytype = 0;
		shaderstate.pendingcolourvbo = 0;
		shaderstate.pendingcolourpointer = NULL;
		shaderstate.pendingcolourflatvalid = true;	//FTESurf Patch 166
	}
	else
	{
		extern cvar_t r_nolightdir;
		if (pass->rgbgen == RGB_GEN_LIGHTING_DIFFUSE || pass->rgbgen == RGB_GEN_ENTITY_LIGHTING_DIFFUSE)
		{
			if (shaderstate.mode == BEM_DEPTHDARK || shaderstate.mode == BEM_DEPTHONLY)
			{
				shaderstate.pendingcolourflat[0] = shaderstate.pendingcolourflat[1] = shaderstate.pendingcolourflat[2] = 0;
				alphagen(pass, 1, meshlist->colors4f_array[0], &shaderstate.pendingcolourflat, meshlist);
				shaderstate.colourarraytype = 0;
				shaderstate.pendingcolourvbo = 0;
				shaderstate.pendingcolourpointer = NULL;
				return;
			}
			if (shaderstate.mode == BEM_LIGHT)
			{
				shaderstate.pendingcolourflat[0] = shaderstate.pendingcolourflat[1] = shaderstate.pendingcolourflat[2] = 1;
				alphagen(pass, 1, meshlist->colors4f_array[0], &shaderstate.pendingcolourflat, meshlist);
				shaderstate.colourarraytype = 0;
				shaderstate.pendingcolourvbo = 0;
				shaderstate.pendingcolourpointer = NULL;
				return;
			}
			if (r_nolightdir.ival || (!shaderstate.curentity->light_range[0] && !shaderstate.curentity->light_range[1] && !shaderstate.curentity->light_range[2]))
			{
				VectorCopy(shaderstate.curentity->light_avg, shaderstate.pendingcolourflat);
				alphagen(pass, 1, meshlist->colors4f_array[0], &shaderstate.pendingcolourflat, meshlist);
				shaderstate.colourarraytype = 0;
				shaderstate.pendingcolourvbo = 0;
				shaderstate.pendingcolourpointer = NULL;
				return;
			}
		}

		//if its vetex lighting, just use the vbo
		if (((pass->rgbgen == RGB_GEN_VERTEX_LIGHTING && shaderstate.identitylighting == 1) || pass->rgbgen == RGB_GEN_VERTEX_EXACT) && pass->alphagen == ALPHA_GEN_VERTEX)
		{
			shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;
			shaderstate.pendingcolourvbo = shaderstate.sourcevbo->colours[0].gl.vbo;
			shaderstate.pendingcolourpointer = shaderstate.sourcevbo->colours[0].gl.addr;
			return;
		}

		for (m = 0; m < shaderstate.meshcount; m++)
		{
			meshlist = shaderstate.meshes[m];

			colourgen(pass, meshlist->numvertexes, meshlist->colors4f_array[0], coloursarray + meshlist->vbofirstvert, meshlist);
			alphagen(pass, meshlist->numvertexes, meshlist->colors4f_array[0], coloursarray + meshlist->vbofirstvert, meshlist);
		}

		shaderstate.colourarraytype = GL_FLOAT;
		shaderstate.pendingcolourvbo = 0;
		shaderstate.pendingcolourpointer = coloursarray;
	}
}

static void BE_GeneratePassTC(const shaderpass_t *pass, int tmu)
{
	if (!pass->numtcmods)
	{
		//if there are no tcmods, pass through here as fast as possible
		switch(pass->tcgen)
		{
		case TC_GEN_BASE:
			shaderstate.pendingtexcoordparts[tmu] = 2;
			shaderstate.pendingtexcoordvbo[tmu] = shaderstate.sourcevbo->texcoord.gl.vbo;
			shaderstate.pendingtexcoordpointer[tmu] = shaderstate.sourcevbo->texcoord.gl.addr;
			break;
		case TC_GEN_LIGHTMAP:
			if (!shaderstate.sourcevbo->lmcoord[0].gl.addr)
			{
				shaderstate.pendingtexcoordparts[tmu] = 2;
				shaderstate.pendingtexcoordvbo[tmu] = shaderstate.sourcevbo->texcoord.gl.vbo;
				shaderstate.pendingtexcoordpointer[tmu] = shaderstate.sourcevbo->texcoord.gl.addr;
			}
			else
			{
				shaderstate.pendingtexcoordparts[tmu] = 2;
				shaderstate.pendingtexcoordvbo[tmu] = shaderstate.sourcevbo->lmcoord[0].gl.vbo;
				shaderstate.pendingtexcoordpointer[tmu] = shaderstate.sourcevbo->lmcoord[0].gl.addr;
			}
			break;
		case TC_GEN_NORMAL:
			shaderstate.pendingtexcoordparts[tmu] = 3;
			shaderstate.pendingtexcoordvbo[tmu] = shaderstate.sourcevbo->normals.gl.vbo;
			shaderstate.pendingtexcoordpointer[tmu] = shaderstate.sourcevbo->normals.gl.addr;
			break;
		case TC_GEN_SVECTOR:
			shaderstate.pendingtexcoordparts[tmu] = 3;
			shaderstate.pendingtexcoordvbo[tmu] = shaderstate.sourcevbo->svector.gl.vbo;
			shaderstate.pendingtexcoordpointer[tmu] = shaderstate.sourcevbo->svector.gl.addr;
			break;
		case TC_GEN_TVECTOR:
			shaderstate.pendingtexcoordparts[tmu] = 3;
			shaderstate.pendingtexcoordvbo[tmu] = shaderstate.sourcevbo->tvector.gl.vbo;
			shaderstate.pendingtexcoordpointer[tmu] = shaderstate.sourcevbo->tvector.gl.addr;
			break;
		case TC_GEN_SKYBOX:
			GenerateTCMods3(pass, tmu);
			break;
			//position - viewpos
//	case TC_GEN_WOBBLESKY:
//	case TC_GEN_REFLECT:
		default:
			//specular highlights and reflections have no fixed data, and must be generated.
			GenerateTCMods(pass, tmu);
			break;
		}
	}
	else
	{
		GenerateTCMods(pass, tmu);
	}
}
#endif

#ifdef GL_STATIC
#define BE_HAVE_BLENDSEP true
#else
#define BE_HAVE_BLENDSEP (qglBlendFuncSeparate != NULL)
#endif
//FTESurf Patch 871: what FTESURF_2D_COVERAGE promises (VF_RT_COVERAGE) needs both
qboolean GLBE_CoverageSupported(void)
{
	return qrenderer == QR_OPENGL && BE_HAVE_BLENDSEP;
}

static void BE_SendPassBlendDepthMask(unsigned int sbits)
{
	unsigned int delta;

	/*2d mode doesn't depth test or depth write*/
	if (shaderstate.force2d)
	{
#ifdef warningmsg
#pragma warningmsg("fixme: q3 doesn't seem to have this, why do we need it?")
#endif
		sbits &= ~(SBITS_MISC_DEPTHWRITE|SBITS_DEPTHFUNC_BITS);
		sbits |= SBITS_MISC_NODEPTHTEST;
	}
	if (shaderstate.flags & (BEF_FORCEADDITIVE|BEF_FORCETRANSPARENT|BEF_FORCENODEPTH|BEF_FORCEDEPTHTEST|BEF_FORCEDEPTHWRITE))
	{
		if (shaderstate.flags & BEF_FORCEADDITIVE)
			sbits = (sbits & ~(SBITS_MISC_DEPTHWRITE|SBITS_BLEND_BITS|SBITS_ATEST_BITS))
						| (SBITS_SRCBLEND_SRC_ALPHA | SBITS_DSTBLEND_ONE);
		else if (shaderstate.flags & BEF_FORCETRANSPARENT)
		{
			if ((sbits & SBITS_BLEND_BITS) == (SBITS_SRCBLEND_ONE| SBITS_DSTBLEND_ZERO) || !(sbits & SBITS_BLEND_BITS) || (sbits & SBITS_ATEST_GE128)) 	/*if transparency is forced, clear alpha test bits*/
				sbits = (sbits & ~(SBITS_BLEND_BITS|SBITS_ATEST_BITS))
							| (SBITS_SRCBLEND_SRC_ALPHA | SBITS_DSTBLEND_ONE_MINUS_SRC_ALPHA | SBITS_ATEST_GT0);
		}

		if (shaderstate.flags & BEF_FORCENODEPTH) 	/*EF_NODEPTHTEST dp extension*/
			sbits |= SBITS_MISC_NODEPTHTEST;
		else
		{
			if (shaderstate.flags & BEF_FORCEDEPTHTEST)
				sbits &= ~SBITS_MISC_NODEPTHTEST;
			if (shaderstate.flags & BEF_FORCEDEPTHWRITE)
				sbits |= SBITS_MISC_DEPTHWRITE;
		}
	}
	if (be_nopanedepth)
		sbits &= ~SBITS_MISC_DEPTHWRITE;
	sbits |= r_refdef.colourmask;
	//FTESurf Patch 871: a coverage target's alpha is written whatever the pass masks (fill_trans masks it)
	if (r2d_coverage && shaderstate.force2d)
		sbits &= ~SBITS_MASK_ALPHA;


	delta = sbits^shaderstate.shaderbits;
	if ((r2d_coverage && shaderstate.force2d) != shaderstate.coverage)
	{
		shaderstate.coverage = r2d_coverage && shaderstate.force2d;
		delta |= SBITS_BLEND_BITS;
	}

#ifdef FORCESTATE
	delta |= ~0;
#endif
	if (!delta)
		return;
	shaderstate.shaderbits = sbits;
	if (delta & SBITS_BLEND_BITS)
	{
		if (sbits & SBITS_BLEND_BITS)
		{
			int src, dst;
			/*unpack the src and dst factors*/
			switch(sbits & SBITS_SRCBLEND_BITS)
			{
			case SBITS_SRCBLEND_ZERO:					src = GL_ZERO;	break;
			default:
			case SBITS_SRCBLEND_ONE:					src = GL_ONE;	break;
			case SBITS_SRCBLEND_DST_COLOR:				src = GL_DST_COLOR;	break;
			case SBITS_SRCBLEND_ONE_MINUS_DST_COLOR:	src = GL_ONE_MINUS_DST_COLOR;	break;
			case SBITS_SRCBLEND_SRC_ALPHA:				src = GL_SRC_ALPHA;	break;
			case SBITS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	src = GL_ONE_MINUS_SRC_ALPHA;	break;
			case SBITS_SRCBLEND_DST_ALPHA:				src = GL_DST_ALPHA;	break;
			case SBITS_SRCBLEND_ONE_MINUS_DST_ALPHA:	src = GL_ONE_MINUS_DST_ALPHA;	break;
			case SBITS_SRCBLEND_ALPHA_SATURATE:			src = GL_SRC_ALPHA_SATURATE;	break;
			}
			switch((sbits & SBITS_DSTBLEND_BITS)>>4)
			{
			case SBITS_DSTBLEND_ZERO>>4:				dst = GL_ZERO;	break;
			default:
			case SBITS_DSTBLEND_ONE>>4:					dst = GL_ONE;	break;
			case SBITS_DSTBLEND_SRC_COLOR>>4:			dst = GL_SRC_COLOR;	break;
			case SBITS_DSTBLEND_ONE_MINUS_SRC_COLOR>>4:	dst = GL_ONE_MINUS_SRC_COLOR;	break;
			case SBITS_DSTBLEND_SRC_ALPHA>>4:			dst = GL_SRC_ALPHA;	break;
			case SBITS_DSTBLEND_ONE_MINUS_SRC_ALPHA>>4:	dst = GL_ONE_MINUS_SRC_ALPHA;	break;
			case SBITS_DSTBLEND_DST_ALPHA>>4:			dst = GL_DST_ALPHA;	break;
			case SBITS_DSTBLEND_ONE_MINUS_DST_ALPHA>>4:	dst = GL_ONE_MINUS_DST_ALPHA;	break;
			}
			qglEnable(GL_BLEND);
			//FTESurf Patch 871: into a coverage target alpha is coverage -- a blend over the target adds
			//to it, an additive one leaves it -- so the target composites as premultiplied.
			if (shaderstate.coverage && BE_HAVE_BLENDSEP && dst == GL_ONE_MINUS_SRC_ALPHA)
				qglBlendFuncSeparate(src, dst, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			else if (shaderstate.coverage && BE_HAVE_BLENDSEP && dst == GL_ONE)
				qglBlendFuncSeparate(src, dst, GL_ZERO, GL_ONE);
			else
				qglBlendFunc(src, dst);
		}
		else
			qglDisable(GL_BLEND);
	}

#ifdef GL_ALPHA_TEST	//alpha test doesn't exist in gles2
	if ((delta & SBITS_ATEST_BITS) && !gl_config_nofixedfunc)
	{
		switch (sbits & SBITS_ATEST_BITS)
		{
		default:
			qglDisable(GL_ALPHA_TEST);
			break;
		case SBITS_ATEST_GT0:
			qglEnable(GL_ALPHA_TEST);
			qglAlphaFunc(GL_GREATER, 0);
			break;
		case SBITS_ATEST_LT128:
			qglEnable(GL_ALPHA_TEST);
			qglAlphaFunc(GL_LESS, 0.5f);
			break;
		case SBITS_ATEST_GE128:
			qglEnable(GL_ALPHA_TEST);
			qglAlphaFunc(GL_GEQUAL, 0.5f);
			break;
		}
	}
#endif

	if (delta & SBITS_MISC_NODEPTHTEST)
	{
		if (sbits & SBITS_MISC_NODEPTHTEST)
			qglDisable(GL_DEPTH_TEST);
		else
			qglEnable(GL_DEPTH_TEST);
	}
	if (delta & SBITS_MISC_DEPTHWRITE)
	{
		if (sbits & SBITS_MISC_DEPTHWRITE)
			qglDepthMask(GL_TRUE);
		else
			qglDepthMask(GL_FALSE);
	}
	if (delta & (SBITS_DEPTHFUNC_BITS))
	{
		extern int gldepthfunc;
		switch (sbits & SBITS_DEPTHFUNC_BITS)
		{
		case SBITS_DEPTHFUNC_EQUAL:
			qglDepthFunc(GL_EQUAL);
			break;
		case SBITS_DEPTHFUNC_FURTHER:
			if (gldepthfunc == GL_LEQUAL)
				qglDepthFunc(GL_GREATER);
			else
				qglDepthFunc(GL_LESS);
			break;
		case SBITS_DEPTHFUNC_CLOSER:
			if (gldepthfunc == GL_LEQUAL)
				qglDepthFunc(GL_LESS);
			else
				qglDepthFunc(GL_GREATER);
			break;
		default:
		case SBITS_DEPTHFUNC_CLOSEREQUAL:
			qglDepthFunc(gldepthfunc);
			break;
		}
	}
	if (delta & (SBITS_MASK_BITS))
	{
		qglColorMask(
				(sbits&SBITS_MASK_RED)?GL_FALSE:GL_TRUE,
				(sbits&SBITS_MASK_GREEN)?GL_FALSE:GL_TRUE,
				(sbits&SBITS_MASK_BLUE)?GL_FALSE:GL_TRUE,
				(sbits&SBITS_MASK_ALPHA)?GL_FALSE:GL_TRUE
				);
	}
	if ((delta & SBITS_TESSELLATION) && qglPNTrianglesiATI)
	{
		if ((sbits & SBITS_TESSELLATION) && r_tessellation.ival)
			qglEnable(GL_PN_TRIANGLES_ATI);
		else
			qglDisable(GL_PN_TRIANGLES_ATI);
	}

#ifdef GL_PERSPECTIVE_CORRECTION_HINT
	if ((delta & SBITS_AFFINE) && qglHint)
	{
		if (!qglHint || (gl_config_gles && gl_config.glversion >= 2) || (!gl_config_gles && gl_config_nofixedfunc))
			;	//doesn't exist in gles2 nor gl3+core contexts
		else if (sbits & SBITS_AFFINE)
			qglHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
		else
			qglHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
	}
#endif
}

//if we're drawing via a VBO then we don't really need DrawRangeElements any more.
//and avoiding so many calls into the driver also gives the driver a chance to optimise the draws instead of constantly checking if anything changed.
static void BE_SubmitMeshChainMultiDraw(int batchtype)
{
	int starti, endi;
	int m;
	mesh_t *mesh;
	static GLsizei counts[1024];
	static const GLvoid *indicies[countof(counts)];
	GLsizei drawcount = 0;
	GL_SelectEBO(shaderstate.sourcevbo->indicies.gl.vbo);

	for (m = 0, mesh = shaderstate.meshes[0]; m < shaderstate.meshcount; )
	{
		starti = mesh->vbofirstelement;
		endi = starti+mesh->numindexes;

		//find consecutive surfaces
		for (++m; m < shaderstate.meshcount; m++)
		{
			mesh = shaderstate.meshes[m];
			if (endi == mesh->vbofirstelement)
			{
				endi = mesh->vbofirstelement+mesh->numindexes;
			}
			else
			{
				break;
			}
		}
		if (drawcount == countof(counts))
		{
			qglMultiDrawElements(batchtype, counts, GL_INDEX_TYPE, indicies, drawcount);
			RQuantAdd(RQUANT_DRAWS, drawcount);
			DRAWCALL("BE_SubmitMeshChain MultiDraw");
			drawcount = 0;
		}
		counts[drawcount] = endi-starti;
		indicies[drawcount] = (index_t*)shaderstate.sourcevbo->indicies.gl.addr + starti;
		drawcount++;
		RQuantAdd(RQUANT_PRIMITIVEINDICIES, endi-starti);
	}
	qglMultiDrawElements(batchtype, counts, GL_INDEX_TYPE, indicies, drawcount);
	RQuantAdd(RQUANT_DRAWS, drawcount);
	DRAWCALL("BE_SubmitMeshChain MultiDraw");
}

static void BE_SubmitMeshChainDraw(qboolean usetesselation)
{
	int startv, starti, endv, endi;
	int m;
	mesh_t *mesh;
	int batchtype;

	if (usetesselation)
	{
		m = (shaderstate.flags & BEF_LINES)?2:3;
		if (shaderstate.curpatchverts != m)
		{
			shaderstate.curpatchverts = m;
			qglPatchParameteriARB(GL_PATCH_VERTICES_ARB, m);
		}
		batchtype = GL_PATCHES_ARB;
	}
	else
		batchtype = (shaderstate.flags & BEF_LINES)?GL_LINES:GL_TRIANGLES;

	if (batchtype == GL_LINES)
	{	//nettest: scoped line thickness.  CSQC debug overlays build via R_DB_Poly (gl_line_width);
		//2D drawline — the hit-marker reticle — draws through the dummybatch (gl_2dline_width);
		//everything else (particle trails, etc.) stays 1px.  glLineWidth is driver-clamped (GLBE_Init).
		float lw = 1;
		if (shaderstate.curbatch && shaderstate.curbatch->buildmeshes == R_DB_Poly)
			lw = bound(1, gl_line_width.value, gl_maxlinewidth);
		else if (shaderstate.curbatch == &shaderstate.dummybatch)
			lw = bound(1, gl_2dline_width.value, gl_maxlinewidth);
		qglLineWidth(lw);
	}

	if (be_instancecount > 1 && !(be_instdraw && shaderstate.meshcount == 1))
		be_instancefailed = true;	//FTESurf Patch 831: this draw covers the first prop only
	else if (be_instancecount > 1)
	{	//FTESurf Patch 831: one draw for the run; vertexlit.glsl's INSTANCED reads each prop's data
		mesh = shaderstate.meshes[0];
		GL_SelectEBO(shaderstate.sourcevbo->indicies.gl.vbo);
		qglDrawElementsInstanced(batchtype, mesh->numindexes, GL_INDEX_TYPE, (index_t*)shaderstate.sourcevbo->indicies.gl.addr + mesh->vbofirstelement, be_instancecount);
		RQuantAdd(RQUANT_DRAWS, 1);
		DRAWCALL("BE_SubmitMeshChain instanced");
		RQuantAdd(RQUANT_PRIMITIVEINDICIES, mesh->numindexes*be_instancecount);
		return;
	}
	if (!shaderstate.streamvbo[0])	//only if we're not forcing vbos elsewhere.
	{
		//q3map2 sucks. it splits static meshes randomly rather than with any pvs consistancy, and then splays them out over 1000 different surfaces.
		//this means we end up needing a boatload of draw calls whenever the batch got split.
		//so skip all that and splurge out a usable index list on demand.
		if (shaderstate.meshcount == 1)
		{
			GL_SelectEBO(shaderstate.sourcevbo->indicies.gl.vbo);
			mesh = shaderstate.meshes[0];
			qglDrawRangeElements(batchtype, mesh->vbofirstvert, mesh->vbofirstvert+mesh->numvertexes - 1, mesh->numindexes, GL_INDEX_TYPE, (index_t*)shaderstate.sourcevbo->indicies.gl.addr + mesh->vbofirstelement);
			RQuantAdd(RQUANT_DRAWS, 1);
			DRAWCALL("BE_SubmitMeshChain 1");
			RQuantAdd(RQUANT_PRIMITIVEINDICIES, mesh->numindexes);
			return;
		}
		else if (r_multidraw.ival && qglMultiDrawElements && shaderstate.sourcevbo->indicies.gl.vbo)
		{	//FTESurf Patch 831: straight from the index buffer. The list below copied every index on the CPU
			//for every draw (5.4% of a surf_demise frame) and the driver copied it again.
			BE_SubmitMeshChainMultiDraw(batchtype);
			return;
		}
		else
		{
			index_t *fte_restrict ilst; //FIXME: this should be cached for multiple-pass shaders.
			GL_SelectEBO(0);

			//FIXME: use a coherant persistently mapped buffer.

			mesh = shaderstate.meshes[0];
			startv = mesh->vbofirstvert;
			endv = startv + mesh->numvertexes;
			endi = mesh->numindexes;
			for (m = 1; m < shaderstate.meshcount; m++)
			{
				mesh = shaderstate.meshes[m];
				endi += mesh->numindexes;

				if (startv > mesh->vbofirstvert)
					startv = mesh->vbofirstvert;
				if (endv < mesh->vbofirstvert+mesh->numvertexes)
					endv = mesh->vbofirstvert+mesh->numvertexes;
			}

			ilst = alloca(endi*sizeof(index_t));
			endi = 0;
			for (m = 0; m < shaderstate.meshcount; m++)
			{
				mesh = shaderstate.meshes[m];
				for (starti = 0; starti < mesh->numindexes; )
					ilst[endi++] = mesh->vbofirstvert + mesh->indexes[starti++];
			}
			qglDrawRangeElements(batchtype, startv, endv - 1, endi, GL_INDEX_TYPE, ilst);
			RQuantAdd(RQUANT_DRAWS, 1);
			DRAWCALL("BE_SubmitMeshChain N");
			RQuantAdd(RQUANT_PRIMITIVEINDICIES, endi);
		}
		return;
	}
	else if (qglMultiDrawElements)
		BE_SubmitMeshChainMultiDraw(batchtype);
#if 0 //def FTE_TARGET_WEB
	else if (shaderstate.meshcount > 1)
	{	//FIXME: not really needed if index lists are consecutive
		index_t *tmp;
		int ebo;

		for (endi = 0, m = 0; m < shaderstate.meshcount; m++)
		{
			mesh = shaderstate.meshes[m];
			endi += mesh->numindexes;
		}
		tmp = alloca(endi * sizeof(*tmp));
		for (endi = 0, m = 0; m < shaderstate.meshcount; m++)
		{
			mesh = shaderstate.meshes[m];
			for (starti = 0; starti < mesh->numindexes; starti++)
				tmp[endi++] = mesh->vbofirstvert + mesh->indexes[starti];
		}

		shaderstate.streamid = (shaderstate.streamid + 1) & (sizeof(shaderstate.streamvbo)/sizeof(shaderstate.streamvbo[0]) - 1);
		ebo = shaderstate.streamebo[shaderstate.streamid];
		GL_SelectEBO(ebo);
		qglBufferDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, sizeof(*tmp) * endi, tmp, GL_STREAM_DRAW_ARB);
		qglDrawElements(batchtype, endi, GL_INDEX_TYPE, NULL);
		RQuantAdd(RQUANT_DRAWS, 1);
		RQuantAdd(RQUANT_PRIMITIVEINDICIES, endi);
	}
#endif
	else
	{
		GL_SelectEBO(shaderstate.sourcevbo->indicies.gl.vbo);

/*
		if (qglLockArraysEXT)
		{
			endv = 0;
			startv = 0x7fffffff;
			for (m = 0; m < shaderstate.meshcount; m++)
			{
				mesh = shaderstate.meshes[m];
				starti = mesh->vbofirstvert;
				if (starti < startv)
					startv = starti;
				endi = mesh->vbofirstvert+mesh->numvertexes;
				if (endi > endv)
					endv = endi;
			}
			qglLockArraysEXT(startv, endv);
		}
*/

		for (m = 0, mesh = shaderstate.meshes[0]; m < shaderstate.meshcount; )
		{
			startv = mesh->vbofirstvert;
			starti = mesh->vbofirstelement;

			endv = startv+mesh->numvertexes;
			endi = starti+mesh->numindexes;

			//find consecutive surfaces
			for (++m; m < shaderstate.meshcount; m++)
			{
				mesh = shaderstate.meshes[m];
				if (endi == mesh->vbofirstelement)
				{
					endv = mesh->vbofirstvert+mesh->numvertexes;
					endi = mesh->vbofirstelement+mesh->numindexes;
				}
				else
				{
					break;
				}
			}
			qglDrawRangeElements(batchtype, startv, endv - 1, endi-starti, GL_INDEX_TYPE, (index_t*)shaderstate.sourcevbo->indicies.gl.addr + starti);
			RQuantAdd(RQUANT_DRAWS, 1);
			DRAWCALL("BE_SubmitMeshChain Fallback");
			RQuantAdd(RQUANT_PRIMITIVEINDICIES, endi-starti);
 		}
/*
		if (qglUnlockArraysEXT)
			qglUnlockArraysEXT();
*/
	}
}

/*
FTESurf Patch 780: a `wireframe` material's triangles go down as lines in every pass that
comes through here (depth, colour, fog), so it never writes a filled depth.  The mode is
restored before returning; the r_wireframe and gl_outline passes, which set it for all
their batches, are left alone.  Where qglPolygonMode is NULL (GLES, core profiles,
GLSLONLY) the surface draws filled, as do rtlight shadow-map casters (GLBE_RenderShadowBuffer).
*/
static void BE_SubmitMeshChain(qboolean usetesselation)
{
#ifdef GL_LINE
	if (shaderstate.curshader && shaderstate.curshader->wireframe && qglPolygonMode &&
		!(shaderstate.flags & BEF_LINES) && !shaderstate.polymodeowned)
	{
		//BE_PolyOffset enables the current offset for GL_POLYGON_OFFSET_FILL only; lines need their own
		qboolean offset = shaderstate.curpolyoffset.factor || shaderstate.curpolyoffset.unit;
		qglLineWidth(1);	//the last GL_LINES batch may have left a debug width
		qglPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		if (offset)
			qglEnable(GL_POLYGON_OFFSET_LINE);
		BE_SubmitMeshChainDraw(usetesselation);
		if (offset)
			qglDisable(GL_POLYGON_OFFSET_LINE);
		qglPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		return;
	}
#endif
	BE_SubmitMeshChainDraw(usetesselation);
}

#ifndef GLSLONLY
static void DrawPass(const shaderpass_t *pass)
{
	int i;
#if MAXRLIGHTMAPS > 1
	int j, k;
#endif
	int tmu;
	int lastpass = pass->numMergedPasses;
	unsigned int attr = (1u<<VATTR_LEG_VERTEX) | (1u<<VATTR_LEG_COLOUR);

	for (i = 0; i < lastpass; i++)
	{
		if (pass[i].texgen == T_GEN_UPPEROVERLAY && !TEXLOADED(shaderstate.curtexnums->upperoverlay))
			continue;
		if (pass[i].texgen == T_GEN_LOWEROVERLAY && !TEXLOADED(shaderstate.curtexnums->loweroverlay))
			continue;
		if (pass[i].texgen == T_GEN_FULLBRIGHT && !TEXLOADED(shaderstate.curtexnums->fullbright))
			continue;
		break;
	}
	if (i == lastpass)
		return;
	BE_SendPassBlendDepthMask(pass[i].shaderbits);
	GenerateColourMods(pass+i);
	tmu = 0;
	for (; i < lastpass; i++)
	{
		if (pass[i].texgen == T_GEN_UPPEROVERLAY && !TEXLOADED(shaderstate.curtexnums->upperoverlay))
			continue;
		if (pass[i].texgen == T_GEN_LOWEROVERLAY && !TEXLOADED(shaderstate.curtexnums->loweroverlay))
			continue;
		if (pass[i].texgen == T_GEN_FULLBRIGHT && !TEXLOADED(shaderstate.curtexnums->fullbright))
			continue;
		Shader_BindTextureForPass(tmu, pass+i);
		attr |= (1u<<(VATTR_LEG_TMU0+tmu));

		BE_GeneratePassTC(pass+i, tmu);

		BE_SetPassBlendMode(tmu, pass[i].blendmode);

		tmu++;

		//add in 
		if (pass[i].texgen == T_GEN_LIGHTMAP)
		{
			//first pass should have been REPLACE
			//second pass should be an ADD
			//this depends upon rgbgens for light levels, so each pass *must* be pushed to hardware individually

#if MAXRLIGHTMAPS > 1
			for (j = 1; j < MAXRLIGHTMAPS && shaderstate.curbatch->lightmap[j] >= 0; j++)
			{
				if (j == 1)
					BE_SetPassBlendMode(tmu, PBM_REPLACE);

				/*make sure no textures linger*/
				for (k = tmu; k < shaderstate.lastpasstmus; k++)
				{
					GL_LazyBind(k, r_nulltex);
				}
				shaderstate.lastpasstmus = tmu;

				/*push it*/
				BE_EnableShaderAttributes(attr, 0);
				BE_SubmitMeshChain(false);
				tmu = 0;

				/*bind the light texture*/
				GL_LazyBind(tmu, lightmap[shaderstate.curbatch->lightmap[j]]->lightmap_texture);

				/*set up the colourmod for this style's lighting*/
				shaderstate.pendingcolourvbo = 0;
				shaderstate.pendingcolourpointer = NULL;

				shaderstate.pendingcolourflat[0] = shaderstate.identitylighting * d_lightstylevalue[shaderstate.curbatch->lmlightstyle[j]]/256.0f;
				shaderstate.pendingcolourflat[1] = shaderstate.identitylighting * d_lightstylevalue[shaderstate.curbatch->lmlightstyle[j]]/256.0f;
				shaderstate.pendingcolourflat[2] = shaderstate.identitylighting * d_lightstylevalue[shaderstate.curbatch->lmlightstyle[j]]/256.0f;
				shaderstate.pendingcolourflat[3] = 1;

				/*pick the correct st coords for this lightmap pass*/
				shaderstate.pendingtexcoordparts[tmu] = 2;
				shaderstate.pendingtexcoordvbo[tmu] = shaderstate.sourcevbo->lmcoord[j].gl.vbo;
				shaderstate.pendingtexcoordpointer[tmu] = shaderstate.sourcevbo->lmcoord[j].gl.addr;

				BE_SetPassBlendMode(tmu, PBM_ADD);
				BE_SendPassBlendDepthMask((pass[0].shaderbits & ~SBITS_BLEND_BITS) | SBITS_SRCBLEND_ONE | SBITS_DSTBLEND_ONE);

				attr = (1u<<VATTR_LEG_VERTEX) | (1u<<VATTR_LEG_COLOUR);
				attr |= (1u<<(VATTR_LEG_TMU0+tmu));

				tmu++;
			}

			//might need to break the pass here
			if (j > 1 && i != lastpass)
			{
				for (k = tmu; k < shaderstate.lastpasstmus; k++)
				{
					GL_LazyBind(k, r_nulltex);
				}
				shaderstate.lastpasstmus = tmu;
				BE_EnableShaderAttributes(attr, 0);

				BE_SubmitMeshChain(false);
				tmu = 0;

				BE_SendPassBlendDepthMask(pass[i+1].shaderbits);
				GenerateColourMods(&pass[i+1]);
			}
#endif
		}
	}

	if (!tmu)
		return;

	for (i = tmu; i < shaderstate.lastpasstmus; i++)
	{
		GL_LazyBind(i, r_nulltex);
	}
	shaderstate.lastpasstmus = tmu;
	BE_EnableShaderAttributes(attr, 0);

	BE_SubmitMeshChain(false);
}
#endif

/*
FTESurf Patch 268 C: Source's six-face ambient cube, +x -x +y -y +z -z in world
axes, for vertexlit.glsl's #BUMPCUBE ratio.  r_cubelight 0 uploads zeros, and the
shader's (cube(n_bump)+e)/(cube(n_geom)+e) is then e/e, exactly 1.0 -- so the gate
is bit-exact without a second permutation.  The entity field is zero on anything
the hl2 plugin did not fill (R_CalcModelLighting clears it), which gives the same
exact 1.0.  Counted only when there is a cube to act on, so used=0 gated=0 means
"nothing here carries one" rather than "the gate did nothing".
Patch 831 made it a function: the instanced props' packer takes the same choice.
*/
static const float *BE_EntityAmbientCube(const entity_t *ent, qboolean count)
{
	static const float zerocube[6*3];
	const float *cube = (const float*)ent->light_cube;
	int k;
	for (k = 0; k < 6*3 && !cube[k]; k++)
		;
	if (k < 6*3)
	{
		if (r_cubelight.ival)
		{
			r_cubelight_used += count;
			//r_cubelight 2: a synthetic cube, lit hard from above and nearly black
			//below, on every entity that has a real one.  It answers the one question
			//the map's own cube cannot: does the bumped-normal path work at all?  A
			//leaf cube can be close to uniform, and then a working ratio and a broken
			//one draw the same picture.  A diagnostic, never a look.
			if (r_cubelight.ival == 2)
			{
				static const float testcube[6*3] = {
					0.30f,0.30f,0.30f,	0.30f,0.30f,0.30f,
					0.30f,0.30f,0.30f,	0.30f,0.30f,0.30f,
					1.00f,1.00f,1.00f,	0.02f,0.02f,0.02f};
				cube = testcube;
			}
		}
		else
		{
			r_cubelight_gated += count;
			cube = zerocube;
		}
	}
	return cube;
}

static void BE_Program_Set_Attributes(const program_t *prog, struct programpermu_s *perm, qboolean entunchanged)
{
	vec4_t param4;
	int r, g;//, b;
	int i;
	unsigned int ph;
	const shaderprogparm_t *p;

	if (perm->factorsuniform != -1)
		qglUniform4fvARB(perm->factorsuniform, countof(shaderstate.curshader->factors), shaderstate.curshader->factors[0]);

	/*don't bother setting it if the ent properties are unchanged (but do if the mesh changed)*/
	if (entunchanged)
		return;

	p = perm->parm;
	for (i = perm->numparms; i > 0; i--, p++)
	{
		ph = p->handle;
		switch(p->type)
		{
/*
		case SP_UBO_ENTITYINFO:
			struct
			{
				vec2_t		blendweights;
					vec2_t	pad1;
				vec3_t		glowmod;
					vec_t	pad2;
				vec3_t		origin;
					vec_t	pad3;
				vec4_t		colormod;
				vec3_t		glowmod;
					vec_t	pad4;
				vec3_t		uppercolour;
					vec_t	pad5;
				vec3_t		lowercolour;
					vec_t	pad6;
				vec3_t		fogcolours;
				vec_t		fogalpha;
				vec3_t		vlightdir;
				vec_t		fogdensity;
				vec3_t		vlightmul;
				vec_t		fogdepthbias;
				vec3_t		vlightadd;
				vec_t		time;
			} u_entityinfo;

			Vector2Copy(shaderstate.meshes[0]->xyz_blendw, u_entityinfo.blendweights);
			VectorCopy(shaderstate.curentity->glowmod, u_entityinfo.glowmod);
			VectorCopy(shaderstate.curentity->origin, u_entityinfo.origin);
			Vector4Copy(shaderstate.curentity->shaderRGBAf, u_entityinfo.colormod);
			R_FetchPlayerColour(shaderstate.curentity->topcolour, u_entityinfo.uppercolour);
			R_FetchPlayerColour(shaderstate.curentity->bottomcolour, u_entityinfo.lowercolour);
			Vector3Copy(r_refdef.globalfog.colour, u_entityinfo.fogcolours);
			u_entityinfo.fogalpha = r_refdef.globalfog.alpha;
			u_entityinfo.fogdensity = r_refdef.globalfog.density;
			u_entityinfo.fogdepthbias = r_refdef.globalfog.depthbias;
			u_entityinfo.time = shaderstate.curtime;
			Vector3Copy(shaderstate.curentity->light_dir, u_entityinfo.vlightdir);
			Vector3Copy(shaderstate.curentity->light_range, u_entityinfo.vlightmul);
			Vector3Copy(shaderstate.curentity->light_avg, u_entityinfo.vlightadd);
			break;
*/

/*
		case SP_UBO_LIGHTINFO:
			struct
			{
				vec3_t		toscreen;
				vec_t		lightradius;
				vec3_t		lightcolours;
					vec_t	pad1;
				vec3_t		lightcolourscale;
					vec_t	pad2;
				vec3_t		lightorigin_modelspace;
					vec_t	pad3;
				matrix4x4_t	lightcubematrix;
				vec4_t		lightshadowmapproj;
				vec2_t		lightshadowmapscale;
					vec2_t	pad4;
			} u_lightinfo;
			{
				float v[4], tempv[4];

				v[0] = shaderstate.lightorg[0];
				v[1] = shaderstate.lightorg[1];
				v[2] = shaderstate.lightorg[2];
				v[3] = 1;

				Matrix4x4_CM_Transform4(shaderstate.modelviewmatrix, v, tempv); 
				Matrix4x4_CM_Transform4(r_refdef.m_projection, tempv, v);

				v[3] *= 2;
				u_lightinfo.toscreen[0] = (v[0]/v[3]) + 0.5;
				u_lightinfo.toscreen[1] = (v[1]/v[3]) + 0.5;
				u_lightinfo.toscreen[2] = (v[2]/v[3]) + 0.5;
			}
			u_lightinfo.lightradius = shaderstate.lightradius;
			Vector3Copy(shaderstate.lightcolours, u_lightinfo.lightcolours);

			Matrix4x4_CM_Transform3(shaderstate.modelmatrixinv, shaderstate.lightorg, u_lightinfo.lightorigin_modelspace);
			Vector3Copy(shaderstate.lightcolourscale, u_lightinfo.lightcolourscale);
			Matrix4_Multiply(shaderstate.lightprojmatrix, shaderstate.modelmatrix, u_lightinfo.lightcubematrix);
			Vector4Copy(shaderstate.lightshadowmapproj, u_lightinfo.lightshadowmapproj);
			Vector2Copy(shaderstate.lightshadowmapscale, u_lightinfo.lightshadowmapscale);
			break;
*/
		case SP_M_VIEW:
			qglUniformMatrix4fvARB(ph, 1, false, r_refdef.m_view);
			break;
		case SP_M_PROJECTION:
			qglUniformMatrix4fvARB(ph, 1, false, shaderstate.projectionmatrix);
			break;
		case SP_M_MODELVIEW:
			qglUniformMatrix4fvARB(ph, 1, false, shaderstate.modelviewmatrix);
			break;
		case SP_M_MODELVIEWPROJECTION:
			{
				float m16[16];
				Matrix4_Multiply(shaderstate.projectionmatrix, shaderstate.modelviewmatrix, m16);
				qglUniformMatrix4fvARB(ph, 1, false, m16);
			}
			break;
		case SP_M_INVMODELVIEWPROJECTION:
			{
				float m16[16], inv[16];
				Matrix4_Multiply(shaderstate.projectionmatrix, shaderstate.modelviewmatrix, m16);
				Matrix4_Invert(m16, inv);
				qglUniformMatrix4fvARB(ph, 1, false, inv);
			}
			break;
		case SP_M_INVMODELVIEW:
			{
				float inv[9];
				Matrix3x4_InvertTo3x3(shaderstate.modelviewmatrix, inv);
				qglUniformMatrix3fvARB(ph, 1, false, inv);
			}
			break;
		case SP_M_MODEL:
			qglUniformMatrix4fvARB(ph, 1, false, shaderstate.modelmatrix);
			break;
		case SP_M_ENTBONES_PACKED:
			qglUniform4fvARB(ph, shaderstate.sourcevbo->numbones*3, shaderstate.sourcevbo->bones);
			break;
		case SP_M_ENTBONES_MAT3X4:
			qglUniformMatrix3x4fv(ph, shaderstate.sourcevbo->numbones, false, shaderstate.sourcevbo->bones);
			break;
		case SP_M_INVVIEWPROJECTION:
			{
				float m16[16], inv[16];
				Matrix4_Multiply(shaderstate.projectionmatrix, r_refdef.m_view, m16);
				Matrix4_Invert(m16, inv);
				qglUniformMatrix4fvARB(ph, 1, false, inv);
			}
			break;

		case SP_E_VBLEND:
			qglUniform2fvARB(ph, 1, shaderstate.meshes[0]->xyz_blendw);
			break;

		case SP_E_VLSCALE:
#if MAXRLIGHTMAPS > 1
			if (perm->permutation & PERMUTATION_LIGHTSTYLES)
			{
				vec4_t colscale[MAXRLIGHTMAPS];
				int j, s;
				for (j = 0; j < MAXRLIGHTMAPS ; j++)
				{
					s = shaderstate.curbatch->vtlightstyle[j];
					if (s == 255)
					{
						for (; j < MAXRLIGHTMAPS ; j++)
						{
							colscale[j][0] = 0;
							colscale[j][1] = 0;
							colscale[j][2] = 0;
							colscale[j][3] = 1;
						}
						break;
					}
					if (shaderstate.curentity->model && shaderstate.curentity->model->engineflags & MDLF_NEEDOVERBRIGHT)
					{
						float sc = (1<<bound(0, gl_overbright.ival, 2)) * shaderstate.identitylighting;
						VectorSet(colscale[j], sc, sc, sc);
					}
					else
					{
						VectorSet(colscale[j], shaderstate.identitylighting, shaderstate.identitylighting, shaderstate.identitylighting);
					}
					colscale[j][3] = 1;

					VectorScale(colscale[j], d_lightstylevalue[s]/256.0f, colscale[j]);
				}

				qglUniform4fvARB(ph, j, (GLfloat*)colscale);
				shaderstate.lastuniform = 0;
			}
			else
#endif
			{
				vec4_t param4;
				if (shaderstate.curbatch->flags & BEF_NODLIGHT)
				{
					Vector4Set(param4, 1, 1, 1, 1);
				}
				else
				{
					Vector4Set(param4, shaderstate.identitylighting, shaderstate.identitylighting, shaderstate.identitylighting, 1);
				}
				qglUniform4fvARB(ph, 1, (GLfloat*)param4);
			}
			break;
		case SP_E_LMSCALE:
#if MAXRLIGHTMAPS > 1
			if (perm->permutation & PERMUTATION_LIGHTSTYLES)
			{
				vec4_t colscale[MAXRLIGHTMAPS];
				int j, s;
				for (j = 0; j < MAXRLIGHTMAPS ; j++)
				{
					s = shaderstate.curbatch->lmlightstyle[j];
					if (s == INVALID_LIGHTSTYLE)
					{
						for (; j < MAXRLIGHTMAPS ; j++)
						{
							colscale[j][0] = 0;
							colscale[j][1] = 0;
							colscale[j][2] = 0;
							colscale[j][3] = 1;
						}
						break;
					}
					if (shaderstate.curentity->model && (shaderstate.curentity->model->engineflags & MDLF_NEEDOVERBRIGHT) && !shaderstate.force2d)
					{
						float sc = (1<<bound(0, gl_overbright.ival, 2)) * shaderstate.identitylighting;
						VectorSet(colscale[j], sc, sc, sc);
					}
					else
					{
						VectorSet(colscale[j], shaderstate.identitylighting, shaderstate.identitylighting, shaderstate.identitylighting);
					}
					colscale[j][3] = 1;

					VectorScale(colscale[j], d_lightstylevalue[s]/256.0f, colscale[j]);
				}

				qglUniform4fvARB(ph, j, (GLfloat*)colscale);
				shaderstate.lastuniform = 0;
			}
			else
#endif
			{
				unsigned short s;
				if (shaderstate.curentity->model && (shaderstate.curentity->model->engineflags & MDLF_NEEDOVERBRIGHT) && !shaderstate.force2d)
				{
					float sc = (1<<bound(0, gl_overbright.ival, 2)) * shaderstate.identitylighting;
					Vector4Set(param4, sc, sc, sc, 1);
				}
				else
				{
					Vector4Set(param4, shaderstate.identitylighting, shaderstate.identitylighting, shaderstate.identitylighting, 1);
				}

				s = shaderstate.curbatch->lmlightstyle[0];	//only one style.
				if (s != INVALID_LIGHTSTYLE)
					VectorScale(param4, d_lightstylevalue[s]/256.0f, param4);

				qglUniform4fvARB(ph, 1, (GLfloat*)param4);
			}
			break;

		case SP_E_GLOWMOD:
			qglUniform3fvARB(ph, 1, (GLfloat*)shaderstate.curentity->glowmod);
			break;
		//nettest: 1 = this entity must NOT receive the r_shadows 2 fake-sun shadowmap.  TWO sources:
		//(1) the first-person VIEWMODEL (drawn INSIDE the local player's body, which is rendered into
		//the fake-sun depth pass while r_shadow_playershadows is on, so the gun was shadowed by its
		//owner) — gated by r_shadows_viewmodel; (2) any entity carrying RF_NOSHADOWRECV, set from the
		//CSQCRF_NOSELFSHADOW renderflag (prop_static "Don't self-shadow" spawnflag).  FAKESHADOWS is a
		//global compile-time define, so this per-entity uniform is the only way the shader can know.
		//Polarity is FAIL-SAFE: an unbound/unsupported uniform reads 0 = normal receive (never "vanish").
		case SP_E_NOSHADOWRECV:
			{
				extern cvar_t r_shadows_viewmodel;
				float supp = (shaderstate.curentity->flags & RF_NOSHADOWRECV) ? 1.0f : 0.0f;
				if ((shaderstate.curentity->flags & RF_WEAPONMODEL) && !r_shadows_viewmodel.ival)
					supp = 1.0f;
				qglUniform1fARB(ph, supp);
			}
			break;
		//nettest: 1 = this entity is the LOCAL FIRST-PERSON BODY (cl_fpbody).  The mod draws the
		//owner's own player model so they can look down and see their legs; the model shader then
		//dithers away everything above a height band so the head — which sits around the camera,
		//the eye being at model Z +20 with the crown at +36 — can never clip into view.
		//A per-entity uniform rather than a shader permutation ON PURPOSE: the body-paint system
		//binds its own runtime `program defaultskin` shaders per surface, so gating inside the one
		//shared shader is what makes a painted body fade correctly too, with no shader cross-product.
		//Polarity is FAIL-SAFE: an unbound/unsupported uniform reads 0 = draw the whole model.
		case SP_E_FPFADE:
			qglUniform1fARB(ph, (shaderstate.curentity->flags & RF_FPFADE) ? 1.0f : 0.0f);
			break;
		//nettest: PER-ENTITY dominant light direction (WORLD space, pointing TOWARD the light) for the model
		//sun form-shade in defaultskin.glsl.  Replaces the single global e_fakesundir #define so a prop beside
		//a lamp shades toward THAT lamp while one in the open shades toward the sun — Source-style dominant
		//light.  Freshness is free: R_CalcModelLighting's sample is cached per entity and the Patch-105 cache
		//VALIDATES ON ORIGIN, so a static prop hits forever and a moving one re-samples as it travels.
		//
		//The value is reconstructed, NOT stored: gl_alias.c writes e->light_dir as the world direction
		//PROJECTED onto the entity's orthonormal axes (light_dir[i] = DotProduct(worlddir, e->axis[i])), so
		//the inverse is simply the axis-weighted sum.  Reconstructing here avoids adding a field to entity_t,
		//which the prebuilt hl2/cod plugins reach via NewSceneEntity (struct-ABI hazard — see ENGINE_PATCHES).
		//
		//FALLBACK to the global sun when there is no per-entity information to be had:
		//  - no deluxemap (.lux/LIGHTINGDIR) => LightPointValues returns a CONSTANT direction, which would look
		//    worse than the sun; so un-relit maps keep exactly their current appearance (zero regression).
		//  - RF_WEAPONMODEL => that branch transforms through the VIEW basis first (gl_alias.c), so the plain
		//    inverse above does not apply.
		case SP_E_SUNDIR:
			{
				extern cvar_t r_sun_dir;
				extern qboolean Sh_EntityLampDir(const entity_t *ent, vec3_t out);
				vec3_t sundir;
				//Patch 110: the reconstruction moved into R_EntityDominantLightDir (gl_alias.c) so this
				//uniform and the fake-shadow direction bucketer share ONE implementation -- a prop's
				//shading direction and its cast-shadow direction cannot disagree.
				//Patch 120a: if this prop is currently shadowed by a map LAMP, shade it from that lamp's
				//actual position instead.  The deluxemap reconstruction below only exists on maps built
				//with `light -bspxlux` (6 of the mod's 29), so everywhere else the "per-prop dominant
				//light" silently fell back to the global sun -- i.e. a prop indoors was form-shaded by a
				//sun it cannot see, on top of a lamp shadow pointing the other way.  The lamp direction
				//needs no bake and is the same light that owns the prop's cast + self shadow, so all
				//three now agree.
				//Patch 120c: BLEND, don't switch.  e->lamp is a single int, so the old select swapped the
				//whole direction vector in one frame -- and defaultskin.glsl maps N.dot.sundir across
				//r_shadows_sunshade_floor..._ceil (0..2 as shipped), so a model could swing 0x..2x in that
				//one frame walking through a doorway.  That, not the shadow itself, is what read as a
				//"hard transition".  Lerping by the same shade fraction the shader uses also settles an
				//inconsistency: Sh_EntityLampDir keys on e->lamp alone and never on the in-shade verdict,
				//so a SUNLIT prop merely standing near a lamp was already being form-shaded by that lamp
				//while casting from the sun.  At shade 0 we are now back on the sun regardless.
				extern float Sh_EntitySunShade(const entity_t *ent);
				vec3_t lampdir;
				float shade = Sh_EntitySunShade(shaderstate.curentity);
				if (!R_EntityDominantLightDir(shaderstate.curentity, sundir))
					VectorCopy(r_sun_dir.vec4, sundir);
				if (shade > 0 && Sh_EntityLampDir(shaderstate.curentity, lampdir))
				{
					VectorScale(sundir, 1.0f - shade, sundir);
					VectorMA(sundir, shade, lampdir, sundir);
					if (!VectorNormalize(sundir))	//exactly opposed at the midpoint: keep the lamp end
						VectorCopy(lampdir, sundir);
				}
				qglUniform3fvARB(ph, 1, sundir);
			}
			break;
		case SP_E_SUNSHADE:
			{	//Patch 120c: how far into shade this entity is, 0..1, time-smoothed engine-side.  The
				//shader fades its SUN form-shade and sun self-shadow by it (and nothing else -- a prop
				//indoors keeps its lamp shadow at full strength).  Fail-safe: 0 for anything the shadow
				//system never classified, which is what an unbound uniform reads too.
				extern float Sh_EntitySunShade(const entity_t *ent);
				qglUniform1fARB(ph, Sh_EntitySunShade(shaderstate.curentity));
			}
			break;
		case SP_E_ORIGIN:
			qglUniform3fvARB(ph, 1, (GLfloat*)shaderstate.curentity->origin);
			break;
		case SP_E_COLOURS:
			qglUniform4fvARB(ph, 1, (GLfloat*)shaderstate.curentity->shaderRGBAf);
			break;
		case SP_S_COLOUR:
			if (shaderstate.colourarraytype)
				qglUniform4fARB(ph, 1, 1, 1, 1);	//invalid use
			else
				qglUniform4fvARB(ph, 1, (GLfloat*)shaderstate.pendingcolourflat);
			break;
		case SP_E_COLOURSIDENT:
			if (shaderstate.flags & BEF_FORCECOLOURMOD)
				qglUniform4fvARB(ph, 1, (GLfloat*)shaderstate.curentity->shaderRGBAf);
			else
				qglUniform4fARB(ph, 1, 1, 1, shaderstate.curentity->shaderRGBAf[3]);
			break;
		case SP_E_TOPCOLOURS:
			R_FetchPlayerColour(shaderstate.curentity->topcolour, param4);
			qglUniform3fvARB(ph, 1, param4);
			break;
		case SP_E_BOTTOMCOLOURS:
			R_FetchPlayerColour(shaderstate.curentity->bottomcolour, param4);
			qglUniform3fvARB(ph, 1, param4);
			break;

		case SP_SOURCESIZE:
			if (shaderstate.tex_sourcecol)
			{
				param4[0] = shaderstate.tex_sourcecol->width;
				param4[1] = shaderstate.tex_sourcecol->height;
			}
			else if (shaderstate.tex_sourcedepth)
			{
				param4[0] = shaderstate.tex_sourcedepth->width;
				param4[1] = shaderstate.tex_sourcedepth->height;
			}
			else
			{
				param4[0] = 1;
				param4[1] = 1;
			}
			qglUniform2fvARB(ph, 1, param4);
			break;
		case SP_RENDERTEXTURESCALE:
			if (sh_config.texture_non_power_of_two_pic)
			{
				param4[0] = 1;
				param4[1] = 1;
			}
			else
			{
				r = 1;
				g = 1;
				while (r < vid.pixelwidth)
					param4[0] *= 2;
				while (g < vid.pixelheight)
					param4[1] *= 2;
				param4[0] = vid.pixelwidth/param4[0];
				param4[1] = vid.pixelheight/param4[1];
			}
			param4[2] = 0;
			param4[3] = 0;
			qglUniform4fvARB(ph, 1, param4);
			break;

		case SP_LIGHTSCREEN:
			{
				float v[4], tempv[4];

				v[0] = shaderstate.lightorg[0];
				v[1] = shaderstate.lightorg[1];
				v[2] = shaderstate.lightorg[2];
				v[3] = 1;

				Matrix4x4_CM_Transform4(shaderstate.modelviewmatrix, v, tempv); 
				Matrix4x4_CM_Transform4(shaderstate.projectionmatrix, tempv, v);

				v[3] *= 2;
				v[0] = (v[0]/v[3]) + 0.5;
				v[1] = (v[1]/v[3]) + 0.5;
				v[2] = (v[2]/v[3]) + 0.5;

				qglUniform3fvARB(ph, 1, v);
			}
			break;
		case SP_LIGHTRADIUS:
			qglUniform1fARB(ph, shaderstate.lightradius);
			break;
		case SP_LIGHTCOLOUR:
			qglUniform3fvARB(ph, 1, shaderstate.lightcolours);
			break;
		case SP_W_FOG:
			qglUniform4fvARB(ph, 2, r_refdef.globalfog.colour);	//and density
			break;
		case SP_W_USER:
			qglUniform4fvARB(ph, countof(r_refdef.userdata), r_refdef.userdata[0]);	//and density
			break;
		case SP_V_EYEPOS:
			qglUniform3fvARB(ph, 1, r_origin);
			break;
		case SP_E_EYEPOS:
			{
				/*eye position in model space*/
				vec3_t t2;
				Matrix4x4_CM_Transform3(shaderstate.modelmatrixinv, r_origin, t2);
				qglUniform3fvARB(ph, 1, t2);
			}
			break;
		case SP_LIGHTPOSITION:
			{
				/*light position in model space*/
				vec3_t t2;
				Matrix4x4_CM_Transform3(shaderstate.modelmatrixinv, shaderstate.lightorg, t2);
				qglUniform3fvARB(ph, 1, t2);
			}
			break;
		case SP_LIGHTDIRECTION:
			{
				/*light position in model space*/
				vec3_t t2;
				Matrix4x4_CM_Transform3x3(shaderstate.modelmatrixinv, shaderstate.lightdir, t2);
				qglUniform3fvARB(ph, 1, t2);
			}
			break;
		case SP_LIGHTCOLOURSCALE:
			qglUniform3fvARB(ph, 1, shaderstate.lightcolourscale);
			break;
		case SP_LIGHTCUBEMATRIX:
			/*light's texture projection matrix*/
			{
				float t[16];
				Matrix4_Multiply(shaderstate.lightprojmatrix, shaderstate.modelmatrix, t);
				qglUniformMatrix4fvARB(ph, 1, false, t);
			}
			break;
		case SP_LIGHTSHADOWMAPPROJ:
			qglUniform4fvARB(ph, 1, shaderstate.lightshadowmapproj);
			break;
		case SP_LIGHTSHADOWMAPSCALE:
			qglUniform2fvARB(ph, 1, shaderstate.lightshadowmapscale);
			break;
		//nettest P110: the N fake-shadow slot projections, composed with the model matrix exactly like
		//SP_LIGHTCUBEMATRIX above (the shaders feed them model-space vertex positions).
		//Upload ALL slots, not just the live count: the glsl loop bound (FAKESHADOWS_COUNT) is COMPILE
		//time, so when the live count SHRINKS the program would keep reading LAST frame's matrices for
		//the vanished cells -- stale shadows flickering in as the budget churns.  The generators clear
		//dead slots (identity + huge z) every frame; excess array elements are ignored by GL.
		case SP_FAKESHADOWMATRIX:
			{
				float t[MAX_FAKESHADOW_SLOTS*16];
				int s;
				for (s = 0; s < MAX_FAKESHADOW_SLOTS; s++)
					Matrix4_Multiply(shaderstate.fakeshadowmatrix[s], shaderstate.modelmatrix, t + s*16);
				qglUniformMatrix4fvARB(ph, MAX_FAKESHADOW_SLOTS, false, t);
			}
			break;
		case SP_FAKESHADOWCELL:
			qglUniform4fvARB(ph, MAX_FAKESHADOW_SLOTS, (GLfloat*)shaderstate.fakeshadowcell);
			break;
		case SP_FAKESHADOWINFO:
			qglUniform4fvARB(ph, MAX_FAKESHADOW_SLOTS, (GLfloat*)shaderstate.fakeshadowinfo);
			break;

		/*static lighting info*/
		case SP_E_L_DIR:
			qglUniform3fvARB(ph, 1, (float*)shaderstate.curentity->light_dir);
			break;
		case SP_E_L_MUL:
			if (shaderstate.mode == BEM_DEPTHDARK)
				qglUniform3fvARB(ph, 1, vec3_origin);
			else
				qglUniform3fvARB(ph, 1, (float*)shaderstate.curentity->light_range);
			break;
		case SP_E_L_AMBIENT:
			if (shaderstate.mode == BEM_DEPTHDARK)
				qglUniform3fvARB(ph, 1, vec3_origin);
			else
				qglUniform3fvARB(ph, 1, (float*)shaderstate.curentity->light_avg);
			break;
		case SP_E_L_AMBIENTCUBE:	//see BE_EntityAmbientCube
			qglUniform3fvARB(ph, 6, BE_EntityAmbientCube(shaderstate.curentity, true));
			break;

		case SP_E_TIME:
			qglUniform1fARB(ph, shaderstate.curtime);
			break;
		case SP_CONST1I:
		case SP_TEXTURE:
			qglUniform1iARB(ph, p->ival[0]);
			break;
		case SP_CONST1F:
			qglUniform1fARB(ph, p->fval[0]);
			break;
		case SP_CONST2F:
			qglUniform3fARB(ph, p->fval[0], p->fval[1], 0);
			break;
		case SP_CONST3F:
			qglUniform3fARB(ph, p->fval[0], p->fval[1], p->fval[2]);
			break;
		case SP_CONST4F:
			qglUniform4fARB(ph, p->fval[0], p->fval[1], p->fval[2], p->fval[3]);
			break;
		case SP_CVARI:
			qglUniform1iARB(ph, ((cvar_t*)p->pval)->ival);
			break;
		case SP_CVARF:
			qglUniform1fARB(ph, ((cvar_t*)p->pval)->value);
			break;
		case SP_CVAR3F:
			qglUniform3fvARB(ph, 1, ((cvar_t*)p->pval)->vec4);
			break;
		case SP_CVAR4F:
			qglUniform3fvARB(ph, 1, ((cvar_t*)p->pval)->vec4);
			break;

		default:
			Host_EndGame("Bad shader program parameter type (%i)", p->type);
			break;
		}
	}
}

static void BE_RenderMeshProgram(const shader_t *shader, const shaderpass_t *pass, program_t *p)
{
	int	i;

	int perm;

	struct programpermu_s *permu;

	perm = 0;
	if (shaderstate.sourcevbo->numbones)
		perm |= PERMUTATION_SKELETAL;
#ifdef NONSKELETALMODELS
	if (shaderstate.sourcevbo->coord2.gl.addr)
		perm |= PERMUTATION_FRAMEBLEND;
#endif
	if (TEXLOADED(shaderstate.curtexnums->bump))
		perm |= PERMUTATION_BUMPMAP;
	if (TEXLOADED(shaderstate.curtexnums->fullbright))
		perm |= PERMUTATION_FULLBRIGHT;
	if ((TEXLOADED(shaderstate.curtexnums->loweroverlay) || TEXLOADED(shaderstate.curtexnums->upperoverlay)))
		perm |= PERMUTATION_UPPERLOWER;
	/*FTESurf Patch 297: the same 2D guard as BE_ProglessFogBegin, on the GLSL
	  side.  sys/fog.h applies the identical linear term, so a 2D shader that
	  DOES carry a program -- and every 2D draw in a core-profile context, which
	  the fixed-function path cannot reach at all -- picks up the same
	  (1-f)*fogcolour lift from a negative fogstart.  Fixing only the
	  fixed-function path would leave the bug alive wherever there is a program,
	  i.e. exactly where the renderer is going.*/
	if (r_refdef.globalfog.density && !shaderstate.force2d)
		perm |= PERMUTATION_FOG;
//	if (TEXLOADED(shaderstate.curtexnums->bump) && shaderstate.curbatch->lightmap[0] >= 0 && lightmap[shaderstate.curbatch->lightmap[0]]->hasdeluxe)
//		perm |= PERMUTATION_DELUXE;
	/*
	FTESurf Patch: r_reflectcube -- the one gate every cubemap reflection passes.

	This is the ONLY place PERMUTATION_REFLECTCUBEMASK is ever set, and that
	permutation is what compiles in the cubemap sampling block in every shader
	that has one (vmt/lightmapped, vmt/vertexlit, vmt/transition, vmt/water,
	defaultwall, ...).  So one test here is a complete off switch.

	IT IS DELIBERATELY DOWNSTREAM OF SHADER GENERATION, and that is the whole
	point of it rather than an implementation detail.  Turning a material's
	$envmap off in the hl2 plugin regenerates the script WITHOUT a reflectcube
	line -- but Shader_Reset preserves shader->defaulttextures across a
	regenerate (it detaches the block, memsets the shader and reattaches it), so
	the texid an EARLIER generation loaded is still sitting in curtexnums and
	this test still passes.  The reflection survives its own cvar.  Reported as
	"Cubemaps off and Env maps off, and one map still reflects"; that map was
	simply the one whose materials had been generated while the cvars were on.

	Fixing the staleness at its source is not available: defaulttextures is also
	written by the MODEL loader (R_BuildLegacyTexnums, Mod_RegisterBasicShader),
	so clearing it on a regenerate would drop the base, bump and luma of every
	Q1BSP wall on the first CVAR_SHADERSYSTEM change in the shared game.

	Not CVAR_SHADERSYSTEM itself, for the same reason: a reload here would cost
	the ~600ms full re-parse and would be exactly the thing that cannot be
	trusted to have happened.  perm is masked against supportedpermutations two
	lines on and simply selects an already-compiled permutation, so this costs
	one integer test per batch and caches nothing.

	FTESurf Patch 268 B adds a SECOND switch inside the same test, for exactly
	the same staleness reason.  A material whose reflectcube is the $envcubemap
	sentinel (gl_shader.c) named no texture of its own: the hl2 plugin emitted it
	for a literal $envmap "env_cubemap", and T_GEN_REFLECTCUBE will bind the
	batch's baked cubemap for it.  Those are the batches Part B turns on, so
	r_envcubemap gates them alone -- 0 restores the pre-268-B picture on a live
	map, which hl2_envcubemap 0 cannot do (defaulttextures outlives a
	regenerate).  The outer test, and everything r_reflectcube 0 does, is
	unchanged; a sentinel batch is counted in BOTH used counters.
	*/
	if ((TEXLOADED(shaderstate.curtexnums->reflectcube) || TEXLOADED(shaderstate.curtexnums->reflectmask)))
	{
		/*
		"Sentinel" here means a batch Part B actually turned ON, which is NOT the
		same as "carries the sentinel image", and the difference is the reflectmask.

		A material with $envmapmask AND $envmap "env_cubemap" already reflected
		before this patch: mat_vmt.c emits the reflectmask unconditionally, and a
		loaded reflectmask alone passes the outer test.  Part B additionally hands
		it the sentinel, so without the !TEXLOADED(reflectmask) term below,
		r_envcubemap 0 would switch that population off too -- and it is supposed to
		mean "the pre-268-B picture", which for those materials is reflecting.  The
		test also keeps the counters honest: envcube used/gated then counts only the
		batches whose reflection is new.
		*/
		qboolean sentinel = (r_envcubemap_tex && shaderstate.curtexnums->reflectcube == r_envcubemap_tex &&
							 !TEXLOADED(shaderstate.curtexnums->reflectmask));
		if (!r_reflectcube.ival)
			r_reflectcube_gated++;
		else if (sentinel && !r_envcubemap.ival)
			r_envcubemap_gated++;
		else
		{
			perm |= PERMUTATION_REFLECTCUBEMASK;
			r_reflectcube_used++;
			if (sentinel)
				r_envcubemap_used++;
		}
	}
#if MAXRLIGHTMAPS > 1
	if (shaderstate.curbatch->lightmap[1] >= 0)
		perm |= PERMUTATION_LIGHTSTYLES;
#endif
	//nettest: baked static-prop per-vertex lighting. Enable the VC (light *= v_colour) permutation
	//only for a prop entity that resolved a placement record AND actually has its per-instance colour
	//array bound (gl_alias.c's override sets colours[0]); this keeps it off for ordinary models and for
	//the count-mismatch case where the override was skipped.
	if (shaderstate.curbatch->ent &&
		(shaderstate.curbatch->ent->vertlightcolors || shaderstate.curbatch->ent->vertlightbytes) &&	//FTESurf Patch 259: either form
		(shaderstate.sourcevbo->colours[0].gl.addr || shaderstate.sourcevbo->colours[0].gl.vbo))
		perm |= PERMUTATION_VC;

	if (be_instancecount > 1 && !be_instancefailed)
		perm |= PERMUTATION_INSTANCED;
	perm &= p->supportedpermutations;
	for (;;)
	{
		permu = p->permu[perm];
		if (!permu)
		{
			p->permu[perm] = permu = Shader_LoadPermutation(p, perm);
			if (!permu)
			{	//failed? copy from 0 so we don't keep re-failing
				permu = p->permu[perm] = p->permu[0];
			}
		}
		if (!(perm & PERMUTATION_INSTANCED) || (permu->permutation & PERMUTATION_INSTANCED))
			break;
		perm &= ~PERMUTATION_INSTANCED;	//FTESurf Patch 831: no INSTANCED variant compiled, so this prop alone, as usual
	}
	be_instdraw = !!(perm & PERMUTATION_INSTANCED);

	GL_SelectProgram(permu->h.glsl.handle);
	if (be_instdraw)
		GLBE_BindInstanceData(permu);

	/*
	FTESurf Patch 166.  THE COLOURGEN HAS TO HAPPEN BEFORE THE ATTRIBUTE BIND,
	and in this build it was not happening at all.

	Two faults, one symptom.  The call below sat inside `#ifndef GLSLONLY`, and
	bothdefs.h:320 defines GLSLONLY for every build we ship -- so `!!fixed` set
	prog->calcgens and NOTHING ever read it.  Patch 159 was correct about which
	flag the shader needed and inert for that reason.

	And even in a build where the block did compile, it ran AFTER
	BE_Program_Set_Attributes, which is what binds v_colour: the colour was
	generated one draw too late to be seen.  So this moves it up rather than
	just widening the #ifdef.

	The texcoord half stays inside the guard.  BE_GeneratePassTC feeds
	pendingtexcoord*, which the GLSL attribute path does not read (it takes tex
	coords straight off the vbo), so calling it here would be cost without
	effect -- and the non-GLSLONLY behaviour is left exactly as it was.
	*/
	shaderstate.pendingcolourflatvalid = false;
	if (p->calcgens)
		GenerateColourMods(pass);

#ifndef FORCESTATE
	if (shaderstate.lastuniform == shaderstate.currentprogram)
		i = true;
	else
#endif
	{
		i = false;
		shaderstate.lastuniform = shaderstate.currentprogram;
	}
	BE_Program_Set_Attributes(p, permu, i);

	BE_SendPassBlendDepthMask(pass->shaderbits);

	for (i = 0; i < pass->numMergedPasses; i++)
	{
		Shader_BindTextureForPass(i, pass+i);
#ifndef GLSLONLY
		if (p->calcgens)
			BE_GeneratePassTC(pass+i, i);
#endif
	}
#if MAXRLIGHTMAPS > 1
	{	//nettest: advance `i` once per DECLARED sampler bit, NOT once per bit we happen to have a
		//texture for.  GLSlang_ProgAutoFields (gl_vidcommon.c) hands out texture units by walking
		//sh_defaultsamplers[] with no permutation test at all, so any sampler declared AFTER these
		//six reads whatever unit this loop leaves it on.
		//
		//The old code bound all six inside `if (perm & PERMUTATION_LIGHTSTYLES)` — and on q1bsp
		//that permutation is NEVER set (gl_model.c forces batch->lightmap[1] = -1 for every batch),
		//while !!samps =LIGHTSTYLED still declares bits 17-19 program-wide.  So `i` stalled 3-6
		//units behind the uniform assignment.  That was latent forever because nothing was ever
		//declared after deluxemap3; s_sunvis is the first sampler that is, and it landed on a unit
		//the `while (lastpasstmus > i)` sweep below had just cleared to r_nulltex — sampling
		//(0,0,0,1) on every surface of every map.
		static const unsigned int lsbits[6] = {
			1u<<S_LIGHTMAP1, 1u<<S_LIGHTMAP2, 1u<<S_LIGHTMAP3,
			1u<<S_DELUXEMAP1,1u<<S_DELUXEMAP2,1u<<S_DELUXEMAP3};
		int n, lm;
		for (n = 0; n < 6; n++)
		{
			if (!(p->defaulttextures & lsbits[n]))
				continue;	//not declared -> ProgAutoFields skipped it -> we must skip it too
			lm = (perm & PERMUTATION_LIGHTSTYLES) ? shaderstate.curbatch->lightmap[(n%3)+1] : -1;
			if (n < 3)
				GL_LazyBind(i++, lm>=0?lightmap[lm]->lightmap_texture:r_nulltex);
			else
				GL_LazyBind(i++, (lm>=0&&lightmap[lm]->hasdeluxe)?lightmap[lm+1]->lightmap_texture:missing_texture_normal);
		}
	}
#endif
	//nettest (SUNVIS): baked per-luxel sun visibility, sharing lightmap[0]'s atlas coords.
	//Bound LAST, matching s_sunvis's position at the end of sh_defaultsamplers[] — that table's
	//order is what GLSlang_ProgAutoFields uses to assign texture units, so the two must agree.
	//Guarded on the program actually asking for it, or `i` would drift out of step for every
	//shader that doesn't.  Test `p`, the program being rendered with — NOT
	//shaderstate.curshader->prog: on the alt-program paths (wireframe, fixed-function emulation,
	//altshader) curshader is still defaultwall while `p` is something else entirely, and testing
	//curshader there would bind a spurious extra texture over a unit that program actually uses.
	//i < SHADER_TMU_MAX is NOT paranoia: GL_LazyBind indexes shaderstate.currenttextures[tmu]
	//with no bounds check of its own, and defaultwall with every permutation on peaks at exactly
	//15 of the 16 units — zero headroom. Overflow would corrupt backend state, not fail cleanly.
	if (p && (p->defaulttextures & (1u<<S_SUNVIS)) && i < SHADER_TMU_MAX)
	{
		int svlm = shaderstate.curbatch->lightmap[0];
		//r_blackimage, not r_whiteimage: the texture holds sun OCCLUSION, so black = fully lit =
		//shadow at full strength = the pre-SUNVIS behaviour. See the polarity note in
		//defaultwall.glsl - getting this backwards deletes every shadow in the game.
		GL_LazyBind(i++, (svlm >= 0 && svlm < numlightmaps && TEXVALID(lightmap[svlm]->sunvis_texture))
						? lightmap[svlm]->sunvis_texture : r_blackimage);
	}
	while (shaderstate.lastpasstmus > i)
		GL_LazyBind(--shaderstate.lastpasstmus, r_nulltex);
	shaderstate.lastpasstmus = i;

	BE_EnableShaderAttributes(permu->attrmask, shaderstate.sourcevbo->vao);
	BE_SubmitMeshChain(permu->h.glsl.usetesselation);
	be_instdraw = false;
}

qboolean GLBE_LightCullModel(vec3_t org, model_t *model)
{
#ifdef RTLIGHTS
	if ((shaderstate.mode == BEM_LIGHT || shaderstate.mode == BEM_STENCIL || shaderstate.mode == BEM_DEPTHONLY))
	{
		float dist;
		vec3_t disp;
		if (model->type == mod_alias)
		{
			VectorSubtract(org, shaderstate.lightorg, disp);
			dist = DotProduct(disp, disp);
			if (dist > model->radius*model->radius + shaderstate.lightradius*shaderstate.lightradius)
				return true;
		}
		else
		{
			int i;

			for (i = 0; i < 3; i++)
			{
				if (shaderstate.lightorg[i]-shaderstate.lightradius > org[i] + model->maxs[i])
					return true;
				if (shaderstate.lightorg[i]+shaderstate.lightradius < org[i] + model->mins[i])
					return true;
			}
		}
	}
#endif
	return false;
}

//Note: Be cautious about using BEM_LIGHT here, as it won't select the light.
void GLBE_SelectMode(backendmode_t mode)
{
	extern cvar_t r_polygonoffset_shadowmap_offset, r_polygonoffset_shadowmap_factor;
	extern int gldepthfunc;

//	shaderstate.lastuniform = 0;
#ifndef FORCESTATE
	if (mode != shaderstate.mode)
#endif
	{
		shaderstate.mode = mode;
		shaderstate.flags = 0;
		shaderstate.polyoffset.factor = 0;
		shaderstate.polyoffset.unit = 0;
		switch (mode)
		{
		default:
			break;
		case BEM_WIREFRAME:
			if (!shaderstate.wireframeshader && gl_config.arb_shader_objects)
				shaderstate.wireframeshader = R_RegisterShader("wireframe", SUF_NONE,
					"{\n"
						"program wireframe\n"
						"{\n"
							"nodepthtest\n"
						"}\n"
					"}\n"
				);
			break;
		case BEM_DEPTHONLY:
			shaderstate.polyoffset.factor = r_polygonoffset_shadowmap_factor.value;
			shaderstate.polyoffset.unit = r_polygonoffset_shadowmap_offset.value;
#ifndef GLSLONLY
			if (!gl_config_nofixedfunc)
			{
				BE_SetPassBlendMode(0, PBM_REPLACE);
				GL_DeSelectProgram();
			}
			else
#endif
				if (!shaderstate.allblackshader.glsl.handle)
			{
				const char *defs[] = {NULL};
				shaderstate.allblackshader = GLSlang_CreateProgram(NULL, "allblackprogram", sh_config.minver, defs, "#include \"sys/skeletal.h\"\nvoid main(){gl_Position = skeletaltransform();}", NULL, NULL, NULL, "void main(){gl_FragColor=vec4(0.0,0.0,0.0,1.0);}", false, NULL);
				shaderstate.allblack_mvp = qglGetUniformLocationARB(shaderstate.allblackshader.glsl.handle, "m_modelviewprojection");
			}
			/*BEM_DEPTHONLY does support mesh writing, but its not the only way its used... FIXME!*/
			while(shaderstate.lastpasstmus>0)
			{
				GL_LazyBind(--shaderstate.lastpasstmus, r_nulltex);
			}

			//we don't write or blend anything (maybe alpha test... but mneh)
			BE_SendPassBlendDepthMask(SBITS_MISC_DEPTHWRITE | SBITS_MASK_BITS);

//			BE_PolyOffset(false);

			GL_CullFace(SHADER_CULL_FRONT);
			break;

#ifdef RTLIGHTS
		case BEM_STENCIL:
			/*BEM_STENCIL doesn't support mesh writing*/
#ifdef BEF_PUSHDEPTH
			GLBE_PolyOffsetStencilShadow(false);
#else
			GLBE_PolyOffsetStencilShadow();
#endif

			if (gl_config_nofixedfunc && !shaderstate.allblackshader.glsl.handle)
			{
				const char *defs[] = {NULL};
				shaderstate.allblackshader = GLSlang_CreateProgram(NULL, "allblackprogram", sh_config.minver, defs, "#include \"sys/skeletal.h\"\nvoid main(){gl_Position = skeletaltransform();}", NULL, NULL, NULL, "void main(){gl_FragColor=vec4(0.0,0.0,0.0,1.0);}", false, NULL);
				shaderstate.allblack_mvp = qglGetUniformLocationARB(shaderstate.allblackshader.glsl.handle, "m_modelviewprojection");
			}

			//disable all tmus
			while(shaderstate.lastpasstmus>0)
			{
				GL_LazyBind(--shaderstate.lastpasstmus, r_nulltex);
			}
#ifndef GLSLONLY
			if (!gl_config_nofixedfunc)
			{
				GL_DeSelectProgram();

			//replace mode please
				BE_SetPassBlendMode(0, PBM_REPLACE);
			}
#endif

			//we don't write or blend anything (maybe alpha test... but mneh)
			BE_SendPassBlendDepthMask(SBITS_DEPTHFUNC_CLOSER | SBITS_MASK_BITS);
			GL_CullFace(0);

			//don't change cull stuff, and
			//don't actually change stencil stuff - caller needs to be
			//aware of how many times stuff is drawn, so they can do that themselves.
			break;
		case BEM_CREPUSCULAR:
			if (!shaderstate.crepopaqueshader)
			{
				shaderstate.crepopaqueshader = R_RegisterShader("crepuscular_opaque", SUF_NONE,
					"{\n"
						"program crepuscular_opaque\n"
					"}\n"
					);
			}
			if (!shaderstate.crepskyshader)
			{
				shaderstate.crepskyshader = R_RegisterShader("crepuscular_sky", SUF_NONE,
					"{\n"
						"program crepuscular_sky\n"
						"{\n"
							"map $diffuse\n"
						"}\n"
						"{\n"
							"map $fullbright\n"
						"}\n"
					"}\n"
					);
			}
			break;
#endif
		case BEM_FOG:
			while(shaderstate.lastpasstmus>0)
			{
				GL_LazyBind(--shaderstate.lastpasstmus, r_nulltex);
			}
			GL_LazyBind(0, shaderstate.fogtexture);
			shaderstate.lastpasstmus = 1;

			Vector4Set(shaderstate.pendingcolourflat, 1, 1, 1, 1);
			shaderstate.pendingcolourvbo = 0;
			shaderstate.pendingcolourpointer = NULL;
#ifndef GLSLONLY
			if (!gl_config_nofixedfunc)
				BE_SetPassBlendMode(0, PBM_MODULATE);
#endif
			BE_SendPassBlendDepthMask(SBITS_SRCBLEND_SRC_ALPHA | SBITS_DSTBLEND_ONE_MINUS_SRC_ALPHA | SBITS_DEPTHFUNC_EQUAL);
			break;
		}
	}
}

void GLBE_SelectEntity(entity_t *ent)
{
	unsigned int fl;
	shaderstate.curentity = ent;
	currententity = ent;
	R_RotateForEntity(shaderstate.modelmatrix, shaderstate.modelviewmatrix, shaderstate.curentity, shaderstate.curentity->model);
	Matrix4_Invert(shaderstate.modelmatrix, shaderstate.modelmatrixinv);
	if (qglLoadMatrixf)
		qglLoadMatrixf(shaderstate.modelviewmatrix);


	fl = shaderstate.curentity->flags&(RF_DEPTHHACK|RF_XFLIP);
	if (shaderstate.usingweaponviewmatrix != fl)
	{
		if ((shaderstate.usingweaponviewmatrix & RF_XFLIP) && shaderstate.usingweaponviewmatrix != -1)
			r_refdef.flipcull ^= SHADER_CULL_FLIP;
		shaderstate.usingweaponviewmatrix = fl;

		if (fl)
			memcpy(shaderstate.projectionmatrix, r_refdef.m_projection_view, sizeof(shaderstate.projectionmatrix));
		else
			memcpy(shaderstate.projectionmatrix, r_refdef.m_projection_std, sizeof(shaderstate.projectionmatrix));

		if (fl&RF_XFLIP)
		{
			shaderstate.projectionmatrix[0] *= -1;
			shaderstate.projectionmatrix[4] *= -1;
			shaderstate.projectionmatrix[8] *= -1;
			shaderstate.projectionmatrix[12] *= -1;
			r_refdef.flipcull ^= SHADER_CULL_FLIP;
		}

		if (qglLoadMatrixf)
		{
			qglMatrixMode(GL_PROJECTION);
			qglLoadMatrixf(shaderstate.projectionmatrix);
			qglMatrixMode(GL_MODELVIEW);
		}
	}

	shaderstate.lastuniform = 0;
}

#ifndef GLSLONLY
static void BE_SelectFog(vec3_t colour, float alpha, float density)
{
	float zscale;

	GL_DeSelectProgram();

	zscale = 2048;	/*this value is meant to be the distance at which fog the value becomes as good as fully fogged, just hack it to 2048...*/
	GenerateFogTexture(&shaderstate.fogtexture, density, zscale);
	shaderstate.fogfar = 1/zscale; /*scaler for z coords*/

	VectorCopy(colour, shaderstate.pendingcolourflat);
	shaderstate.pendingcolourflat[3] = alpha;
	shaderstate.pendingcolourvbo = 0;
	shaderstate.pendingcolourpointer = NULL;
}
#endif
#ifdef RTLIGHTS
static qboolean GLBE_RegisterLightShader(int mode)
{
	if (!shaderstate.inited_shader_light[mode])
	{
		char *name = va("rtlight%s%s%s%s%s", 
			(mode & LSHADER_SMAP)?"#PCF":"",
			(mode & LSHADER_SPOT)?"#SPOT":"",
			(mode & LSHADER_CUBE)?"#CUBE":"",
			(mode & LSHADER_ORTHO)?"#ORTHO":"",
			(gl_config.arb_shadow && (mode & (LSHADER_SMAP|LSHADER_SPOT|LSHADER_CUBE|LSHADER_ORTHO)))?"#USE_ARB_SHADOW":""
			);

		shaderstate.inited_shader_light[mode] = true;
		shaderstate.shader_light[mode] = R_RegisterCustom(NULL, name, SUF_NONE, Shader_LightPass, NULL);
	}

	if (shaderstate.shader_light[mode])
	{
		//make sure it has a program and forget it if it doesn't, to save a compare.
		if (!shaderstate.shader_light[mode]->prog)
		{
			shaderstate.shader_light[mode] = NULL;
			return false;
		}
		return true;
	}
	return false;
}
#endif

qboolean GLBE_SelectDLight(dlight_t *dl, vec3_t colour, vec3_t axis[3], unsigned int lmode)
{
#ifdef RTLIGHTS
	extern cvar_t gl_specular;
#endif

	shaderstate.lastuniform = 0;
	shaderstate.curdlight = dl;
	shaderstate.lightmode =	lmode;

	/*simple info*/
	shaderstate.lightradius = dl->radius;
	VectorCopy(dl->origin, shaderstate.lightorg);
	VectorCopy(axis[0], shaderstate.lightdir);
	VectorCopy(colour, shaderstate.lightcolours);
#ifdef RTLIGHTS
	VectorCopy(dl->lightcolourscales, shaderstate.lightcolourscale);
	shaderstate.lightcolourscale[2] *= gl_specular.value;
	if (lmode & (LSHADER_SPOT|LSHADER_ORTHO))
		shaderstate.lightcubemap = r_nulltex;
	else
		shaderstate.lightcubemap = dl->cubetexture;

	if (TEXLOADED(shaderstate.lightcubemap) && GLBE_RegisterLightShader(shaderstate.lightmode | LSHADER_CUBE))
		shaderstate.lightmode |= LSHADER_CUBE;
	if (!GLBE_RegisterLightShader(shaderstate.lightmode))
		return false;

	/*generate light projection information*/
	if (shaderstate.lightmode & LSHADER_ORTHO)
	{
		float view[16];
		float proj[16];
		float xmin = -dl->radius;
		float ymin = -dl->radius;
		float znear = -dl->radius;
		float xmax = dl->radius;
		float ymax = dl->radius;
		float zfar = dl->radius;
		Matrix4x4_CM_Orthographic(proj, xmin, xmax, ymax, ymin, znear, zfar);
		Matrix4x4_CM_ModelViewMatrixFromAxis(view, axis[0], axis[2], axis[1], dl->origin);
		Matrix4_Multiply(proj, view, shaderstate.lightprojmatrix);
//		Matrix4x4_CM_LightMatrixFromAxis(shaderstate.lightprojmatrix, axis[0], axis[1], axis[2], dl->origin);
		{	//nettest: WORLD-CONSTANT contact bias (r_shadows_bias qu; pv[14] = clip-z
			//translation, 1 qu = 1/radius in ortho NDC).  Replaces pcf.h's old radius-
			//SCALED 0.015 NDC bias (removed there), which grew to ~15qu at distance
			//1024 and cut shadows off well before their caster touched the ground.
			extern cvar_t r_shadows_bias;
			shaderstate.lightprojmatrix[14] -= r_shadows_bias.value / max(1, dl->radius);
		}
	}
	else if (shaderstate.lightmode & LSHADER_SPOT)
	{
		float view[16];
		float proj[16];
		extern cvar_t r_shadow_shadowmapping_nearclip;
		Matrix4x4_CM_Projection_Far(proj, dl->fov, dl->fov, dl->nearclip?dl->nearclip:r_shadow_shadowmapping_nearclip.value, dl->radius, false);
		Matrix4x4_CM_ModelViewMatrixFromAxis(view, axis[0], axis[1], axis[2], dl->origin);
		Matrix4_Multiply(proj, view, shaderstate.lightprojmatrix);
	}
	else if (shaderstate.lightmode & (LSHADER_SMAP|LSHADER_CUBE))
	{
		Matrix4x4_CM_LightMatrixFromAxis(shaderstate.lightprojmatrix, axis[0], axis[1], axis[2], dl->origin);
		/*
		vec3_t down;
		vec3_t back;
		vec3_t right;
		VectorScale(axis[2], -1, down);
		VectorScale(axis[1], 1, right);
		VectorScale(axis[0], 1, back);
		Matrix4x4_CM_ModelViewMatrixFromAxis(shaderstate.lightprojmatrix, down, back, right, dl->origin);
		*/
	}
#endif

	return true;
}

/*
  FTESurf Patch 208: BE_Scissor(NULL) means "no LOCAL scissor", not "no scissor".

  Patch 208 confines a portal's recursed frame to the aperture's projected
  rectangle.  The obstacle is that this backend does not track scissor state at
  all -- gl_shadow.c:5773 says so in as many words -- and the recursed frame runs
  a complete R_RenderScene, which reaches Sh_DrawLights (via GLBE_DrawWorld,
  :7107) whose teardown is a flat BE_Scissor(NULL) at gl_shadow.c:6693.  Setting
  the scissor and hoping would work right up until somebody switched rtlights on.

  So the ambient clip lives in r_refdef (which is saved and restored across the
  recursion for free) and every BE_Scissor call resolves against it here.  A
  caller asking for no scissor gets the aperture; a caller asking for a box gets
  box-intersect-aperture, which for a light is not merely safe but correct and
  faster.  No existing call site changes.

  The intersection is done in FRACTIONS so that the one piece of coordinate
  arithmetic anybody has to trust -- the y flip below -- is inherited rather than
  written out a second time.

  portalclippx guards it: if the render target has changed since the rectangle
  was measured (a reflection/refraction FBO, a shadow atlas -- all of which
  overwrite pxrect and then call GL_ViewportUpdate), the fractions would land
  somewhere meaningless, so the clip simply does not apply while they are
  retargeted.  That is what makes those paths safe with no explicit suspend, and
  GLBE_ViewportUpdate below is what re-arms it on the way back.
*/
static qboolean scissor_ambient;	//the GL scissor currently reflects a portal clip

static void GLBE_ApplyScissor(srect_t *rect)
{
	srect_t r;
	qboolean amb;

	amb =	r_refdef.portalclip &&
			r_refdef.portalclippx[0] == r_refdef.pxrect.x &&
			r_refdef.portalclippx[1] == r_refdef.pxrect.y &&
			r_refdef.portalclippx[2] == r_refdef.pxrect.width &&
			r_refdef.portalclippx[3] == r_refdef.pxrect.maxheight;
	if (amb)
	{
		r.x      = r_refdef.portalcliprect[0];
		r.y      = r_refdef.portalcliprect[1];
		r.width  = r_refdef.portalcliprect[2];
		r.height = r_refdef.portalcliprect[3];
		r.dmin   = 0;
		r.dmax   = 1;
		if (rect)
		{
			float x2 = r.x + r.width, y2 = r.y + r.height;
			if (x2 > rect->x + rect->width)		x2 = rect->x + rect->width;
			if (y2 > rect->y + rect->height)	y2 = rect->y + rect->height;
			if (r.x < rect->x)	r.x = rect->x;
			if (r.y < rect->y)	r.y = rect->y;
			//an empty intersection is a legitimate answer: a light that does not
			//overlap the aperture draws nothing.  It must not go negative.
			r.width  = (x2 > r.x) ? x2 - r.x : 0;
			r.height = (y2 > r.y) ? y2 - r.y : 0;
			if (r.dmin < rect->dmin) r.dmin = rect->dmin;
			if (r.dmax > rect->dmax) r.dmax = rect->dmax;
		}
		rect = &r;
	}
	scissor_ambient = amb;

	if (rect)
	{
		qglScissor(
			floor(r_refdef.pxrect.x + rect->x*r_refdef.pxrect.width),
//			floor(r_refdef.pxrect.y + rect->y*r_refdef.pxrect.height),// - r_refdef.pxrect.maxheight),
			floor(rect->y*r_refdef.pxrect.height + (r_refdef.pxrect.maxheight-r_refdef.pxrect.y)-r_refdef.pxrect.height),
			ceil(rect->width * r_refdef.pxrect.width),
			ceil(rect->height * r_refdef.pxrect.height));
		qglEnable(GL_SCISSOR_TEST);

		if (qglDepthBoundsEXT)
		{
			qglDepthBoundsEXT(rect->dmin, rect->dmax);
			qglEnable(GL_DEPTH_BOUNDS_TEST_EXT);
		}
	}
	else
	{
/*		qglScissor(
			r_refdef.pxrect.x,
			r_refdef.pxrect.y - r_refdef.pxrect.height,
			r_refdef.pxrect.width,
			r_refdef.pxrect.height);
*/		qglDisable(GL_SCISSOR_TEST);
		if (qglDepthBoundsEXT)
			qglDisable(GL_DEPTH_BOUNDS_TEST_EXT);

//		if (qglDepthBoundsEXT)
//			qglDepthBoundsEXT(0, 1);
	}
}

void GLBE_Scissor(srect_t *rect)
{
	GLBE_ApplyScissor(rect);
}

/*
  FTESurf Patch 208: was a bare qglViewport macro in glquake.h.

  Every call site of it is a render-TARGET change -- an FBO push or pop for a
  reflection, a refraction, a ripplemap or a shadow atlas -- and each one either
  invalidates the portal clip (the fractions no longer refer to the same pixels)
  or restores the target it was measured against.  Resolving the scissor here
  means those paths need no explicit suspend or resume: they overwrite pxrect,
  the identity test in GLBE_ApplyScissor fails, their unclipped clears run, and
  the restoring GL_ViewportUpdate on the way out re-arms the aperture.

  The scissor_ambient guard is what keeps the 2d users bit-for-bit unchanged:
  with no portal clip anywhere in the frame this function is exactly the old
  macro, so console.c, pr_csqc.c, pr_menu.c, m_native.c, m_items.c and
  clhl_game.c -- each of which sets a scissor and may cross a GL_Set2D -- behave
  as they always did.
*/
void GLBE_ViewportUpdate(void)
{
	qglViewport(r_refdef.pxrect.x,
				r_refdef.pxrect.maxheight-(r_refdef.pxrect.y+r_refdef.pxrect.height),
				r_refdef.pxrect.width,
				r_refdef.pxrect.height);
	if (r_refdef.portalclip || scissor_ambient)
		GLBE_ApplyScissor(NULL);
}

#if defined(RTLIGHTS) && !defined(GLSLONLY)
texid_t GenerateNormalisationCubeMap(void);
static void BE_LegacyLighting(void)
{
	//bigfoot wants rtlight support without glsl, so here goes madness...
	//register combiners for bumpmapping using 4 tmus...
	float *col;
	float *ldir;
	vec3_t lightdir, rellight;
	float scale;
	int i, m;
	mesh_t *mesh;
	unsigned int attr = (1u<<VATTR_LEG_VERTEX) | (1u<<VATTR_LEG_COLOUR);
	int tmu = 0;

	BE_SendPassBlendDepthMask(SBITS_SRCBLEND_ONE | SBITS_DSTBLEND_ONE);

	//rotate this into modelspace
	Matrix4x4_CM_Transform3(shaderstate.modelmatrixinv, shaderstate.lightorg, rellight);

	for (m = 0; m < shaderstate.meshcount; m++)
	{
		mesh = shaderstate.meshes[m];

		//vbo-only mesh.
		if (!mesh->xyz_array)
			return;
		if (!mesh->normals_array)
			return;

		col = coloursarray[0] + mesh->vbofirstvert*4;
		ldir = texcoordarray[0] + mesh->vbofirstvert*3;
		for (i = 0; i < mesh->numvertexes; i++, col+=4, ldir+=3)
		{
			VectorSubtract(rellight, mesh->xyz_array[i], lightdir);
			scale = VectorNormalize(lightdir);
			scale = 1 - (scale/shaderstate.lightradius);
			VectorScale(shaderstate.lightcolours, scale, col);
			col[3] = 1;
			ldir[0] = -DotProduct(lightdir, mesh->snormals_array[i]);
			ldir[1] = DotProduct(lightdir, mesh->tnormals_array[i]);
			ldir[2] = DotProduct(lightdir, mesh->normals_array[i]);
		}
	}

	if (TEXLOADED(shaderstate.curtexnums->bump) && sh_config.havecubemaps && gl_config.arb_texture_env_dot3 && gl_config.arb_texture_env_combine && be_maxpasses >= 4)
	{	//we could get this down to 2 tmus by arranging for the dot3 result to be written the alpha buffer. But then we'd need to have an alpha buffer too.

		if (!shaderstate.normalisationcubemap)
			shaderstate.normalisationcubemap = GenerateNormalisationCubeMap();

		//tmu0: normalmap+replace+regular tex coords
		GL_LazyBind(tmu, shaderstate.curtexnums->bump);
		BE_SetPassBlendMode(tmu, PBM_REPLACE);
		shaderstate.pendingtexcoordparts[tmu] = 2;
		shaderstate.pendingtexcoordvbo[tmu] = shaderstate.sourcevbo->texcoord.gl.vbo;
		shaderstate.pendingtexcoordpointer[tmu] = shaderstate.sourcevbo->texcoord.gl.addr;
		attr |= (1u<<(VATTR_LEG_TMU0+tmu));
		tmu++;

		//tmu1: normalizationcubemap+dot3+lightdir
		GL_LazyBind(tmu, shaderstate.normalisationcubemap);
		BE_SetPassBlendMode(tmu, PBM_DOTPRODUCT);
		shaderstate.pendingtexcoordparts[tmu] = 3;
		shaderstate.pendingtexcoordvbo[tmu] = 0;
		shaderstate.pendingtexcoordpointer[tmu] = texcoordarray[0];
		attr |= (1u<<(VATTR_LEG_TMU0+tmu));
		tmu++;

		//tmu2: $diffuse+multiply+regular tex coords
		GL_LazyBind(tmu, shaderstate.curtexnums->base);	//texture not used, its just to make sure the code leaves it enabled.
		BE_SetPassBlendMode(tmu, PBM_MODULATE);
		shaderstate.pendingtexcoordparts[tmu] = 2;
		shaderstate.pendingtexcoordvbo[tmu] = shaderstate.sourcevbo->texcoord.gl.vbo;
		shaderstate.pendingtexcoordpointer[tmu] = shaderstate.sourcevbo->texcoord.gl.addr;
		attr |= (1u<<(VATTR_LEG_TMU0+tmu));
		tmu++;
	
		//tmu3: $any+multiply-by-colour+notc
		GL_LazyBind(tmu, shaderstate.curtexnums->base);	//texture not used, its just to make sure the code leaves it enabled.
		BE_SetPassBlendMode(tmu, PBM_MODULATE_PREV_COLOUR);
		shaderstate.pendingtexcoordparts[tmu] = 0;
		shaderstate.pendingtexcoordvbo[tmu] = 0;
		shaderstate.pendingtexcoordpointer[tmu] = NULL;
		tmu++;

		//note we need 4 combiners in the first because we can't use the colour argument in the first without breaking the normals.

		for (i = tmu; i < shaderstate.lastpasstmus; i++)
		{
			GL_LazyBind(i, r_nulltex);
		}
		shaderstate.lastpasstmus = tmu;
	}
	else
	{
		attr |= (1u<<(VATTR_LEG_TMU0));

		//tmu0: $diffuse+multiply+regular tex coords
		//multiplies by vertex colours
		GL_LazyBind(0, shaderstate.curtexnums->base);	//texture not used, its just to make sure the code leaves it enabled.
		BE_SetPassBlendMode(0, PBM_MODULATE);
		shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
		shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;

		for (i = 1; i < shaderstate.lastpasstmus; i++)
		{
			GL_LazyBind(i, r_nulltex);
		}
		shaderstate.lastpasstmus = 1;
	}

	shaderstate.colourarraytype = GL_FLOAT;
	shaderstate.pendingcolourvbo = 0;
	shaderstate.pendingcolourpointer = coloursarray;

	GL_DeSelectProgram();
	BE_EnableShaderAttributes(attr, 0);

	BE_SubmitMeshChain(false);

//	GL_LazyBind(1, 0, r_nulltex);
//	GL_LazyBind(2, 0, r_nulltex);
//	GL_LazyBind(3, 0, r_nulltex);
}

/*
FTESurf Patch 266: distance fog for shaders that have no GLSL program.

This is the FIXME twenty lines into the global-fog block at the bottom of
GLBE_DrawWorld -- "should really be doing this on a per-shader basis, for custom
shaders that don't use glsl".  That block is whole-scene and switches itself off
whenever GLSL is available, so in practice a program-less shader has never been
fogged at all: PERMUTATION_FOG is set in exactly one place, inside
BE_RenderMeshProgram, and DrawMeshes only calls that for the whole shader when
curshader->prog is non-NULL.

Reported as "the clouds take no distance fog".  surf_boreas's
project_tendies/cloods is an UnlitGeneric material, and UnlitGeneric is the one
arm of the hl2 plugin's translator that emits a real PASS instead of a program,
so it has no prog and cannot reach the permutation.  Its neighbours in the same
3D-skybox room are WorldVertexTransition, which do get a program and do fog --
which is why this reads as one material misbehaving rather than a broken region.

WHY FIXED-FUNCTION FOG RATHER THAN AN EXTRA FOG PASS.  A second pass drawn over
the same geometry, which is how the mfog volume path below does it, has no
access to the surface's own alpha: on a translucent surface such as these clouds
it would paint fog across the fully-transparent texels too and put a
fog-coloured rectangle in the sky.  GL's fixed-function fog is applied per
fragment after texturing and modifies RGB only, leaving alpha alone, which is
exactly the semantics sys/fog.h's fog3() gives the program shaders.  It also
cannot touch a shader that HAS a program -- GL ignores it while one is bound --
so there is no risk of double-fogging the surfaces that already work.

EXACTNESS, AND WHAT THIS DELIBERATELY DOES NOT COVER.  Linear maps exactly:
sys/fog.h reads density as the end distance and depthbias as the start, which is
GL_FOG_END/GL_FOG_START.  Exp and exp2 map exactly when depthbias is 0, GL having
no bias term.

`alpha` is the one that cannot be done.  It is not a scale on the fog, it is a
CAP: the gamecode reparametrises Source's min(fogmaxdensity, ramp) into FTE's
alpha plus a shortened end distance (see cl_fog.qc), so the fog opacity is a
linear ramp that stops climbing at alpha.  GL's fixed-function fog always ramps
to fully fogged; the start and end can be reconstructed exactly, but there is no
clamp.  Setting them anyway would leave the near field right and over-fog
everything past the cap, worst where the cap is lowest.

So those maps are declined rather than approximated, and say so once.  Measured:
of 36 env_fog_controllers across a 146-map sample, 22 set fogmaxdensity below 1,
so this covers the other 14 plus every map with no controller at all.
surf_boreas, which is where this was reported, is fogmaxdensity 1 and is exact.
Closing the remaining 61% means giving these shaders a real program -- routing
them through `fixedemu` with a FOG permutation would do it, and would cover the
core-profile path this cannot reach either -- which is a bigger change than the
reported bug needs and is recorded as the follow-up rather than guessed at here.

A silent approximation is the failure mode this patch series keeps having to
undo, so the decline is loud.

FTESurf Patch 297: 2D IS NOT SAFE WITHOUT A GUARD.  The claim that used to sit
here -- "it draws at an eye-space depth of ~0, where both fog modes give a
factor of 1, i.e. no fog" -- is true only for a POSITIVE fog start.  Linear fog
is f = (end - z)/(end - start), so at z~0 it is f = end/(end - start), which is
1 only when start is 0 or positive.  A NEGATIVE start makes f < 1 at zero depth
and every program-less 2D quad on the screen picks up (1-f)*fogcolour.

Source maps set a negative fogstart freely.  surf_tensor2's env_fog_controller
is fogstart -2500, fogend 15000, fogcolor 41 66 90, and :5338 below passes
depthbias to GL_FOG_START verbatim, so:

    f     = 15000 / (15000 - (-2500)) = 0.857143
    added = (1 - f) * (41,66,90)      = (5.86, 9.43, 12.86)  ->  (6,9,13)

added to every console glyph, HUD quad and menu element, with no free parameter
to tune it away.  Measured at (6,9,13) exactly on a frozen scene, and matching
fogcolour x 0.142857 on all three channels -- a blue-dominant residue no font,
blend mode or atlas defect can produce, since those can only scale the text
colour.  Of 1339 library maps, seven have fogenable, maxdensity >= 1 and a
negative fogstart; surf_tensor2 is by far the worst and surf_tensor's -100 is
0.2/255, which is why the original Patch 273 control never showed it.

The guard is one line and the field was already here (:160, set at :513, used
at :3281 and :5364).  Nothing drawn in 2D has a world-space depth to fog.

THE OTHER HALF -- FIXED IN PATCH 300, not here.  An ADDITIVE scene pass has the
same problem for a different reason.  Fixed-function fog mixes toward
GL_FOG_COLOR on RGB, so on a gl_one/gl_one pass a black texel becomes the fog
colour and is then ADDED -- which is why light_glow02/03 show their whole
texture square faintly lit rather than masked.

CORRECTION, and it is the reason this note is being rewritten rather than
ticked off: THIS COMMENT HAD THE SHADER CLASS WRONG.  light_glow02.vmt,
light_glow03.vmt and xen_ray_3.vmt are class "Sprite" (mat_vmt.c's Sprite arm),
not UnlitGeneric.  Only light_glow02_add_noz.vmt -- the env_lightglow material,
hardcoded in cl_sprites.qc -- is UnlitGeneric.  The error was invisible for as
long as the sentence only had to explain the artefact, because BOTH arms emit a
program-less pass and both show it; it became load-bearing the moment a fix had
to name an arm, and a patch written from this sentence would have corrected 166
of surf_tensor2's 429 glows and left 263 on screen.

Patch 300 takes the OTHER route named below -- give those materials a real
top-level program, so they leave this path entirely and reach sys/fog.h's
fog4additive(), which fades toward black.  That is better than forcing
GL_FOG_COLOR black here: it also works on a core profile, where this function is
not called at all, and it lets $nofog and the exact $alphatestreference finally
reach materials that have been computing both and discarding them.  Gating it on
the material being additive is what keeps Patch 266's surf_boreas clouds out of
it -- cloods.vmt has its $additive line commented out and is alpha-blended, so
it stays on this path exactly as before.

r_fog_progless 0 was never the fix for either half -- it re-breaks exactly what
Patch 266 exists for.

DO NOT CARRY THE SEVEN-MAP CENSUS ABOVE OVER TO THAT HALF.  The negative start
is what makes the 2D half visible, because 2D sits at depth ~0 and needs f < 1
THERE.  An additive pass needs no such thing: f < 1 at any distance under any
fog, so (1-f)*fogcolour is added to a texel that should contribute nothing, and
a positive start only moves where it begins.  Measured at 46.1% of pixels on
surf_tensor2 with sprites on.  The 2D half is 7 of 1339 maps; the additive half
is every fogged map that draws additive sprites, ~303 env_fog_controllers.  It
is the larger bug of the two and it is still open.
*/
static qboolean BE_ProglessFogBegin(void)
{
	extern cvar_t r_fog_progless, r_fog_linear, r_fog_exp2;
	static qboolean moaned = false;
	float col[4];

	if (shaderstate.force2d)
		return false;	//Patch 297: 2D has no world depth to fog.  See above.
	if (!r_refdef.globalfog.density || !r_fog_progless.ival)
		return false;
	if (shaderstate.curshader->prog)
		return false;	//has a program; PERMUTATION_FOG already covers it.
	if (shaderstate.curshader->flags & SHADER_SKY)
		return false;	//sky is not at a distance.
	if (!qglFogi || !qglFogf || !qglFogfv)
		return false;	//not bound on this context.

	if (r_refdef.globalfog.alpha < 1 ||
		(!r_fog_linear.ival && r_refdef.globalfog.depthbias))
	{
		if (!moaned)
		{
			moaned = true;
			Con_DPrintf("[fog] program-less surfaces left unfogged: fixed-function fog cannot express alpha %g / depthbias %g. See r_fog_progless.\n",
				r_refdef.globalfog.alpha, r_refdef.globalfog.depthbias);
		}
		return false;
	}

	VectorCopy(r_refdef.globalfog.colour, col);
	col[3] = 1;
	qglFogfv(GL_FOG_COLOR, col);

	if (r_fog_linear.ival)
	{	//density is the end distance and depthbias the start, per sys/fog.h.
		qglFogi(GL_FOG_MODE, GL_LINEAR);
		qglFogf(GL_FOG_START, r_refdef.globalfog.depthbias);
		qglFogf(GL_FOG_END, r_refdef.globalfog.density);
	}
	else
	{
		qglFogi(GL_FOG_MODE, r_fog_exp2.ival?GL_EXP2:GL_EXP);
		qglFogf(GL_FOG_DENSITY, r_refdef.globalfog.density);
	}
	qglEnable(GL_FOG);
	return true;
}
static void BE_ProglessFogEnd(qboolean fogging)
{
	if (fogging)
		qglDisable(GL_FOG);
}
#endif

static void DrawMeshes(void)
{
	const shader_t *altshader;
	const shaderpass_t *p;
	int passno;
	int flags;
	passno = 0;

	if (shaderstate.force2d)
	{
		RQuantAdd(RQUANT_2DBATCHES, 1);
	}
	else if (shaderstate.curentity == &r_worldentity)
	{
		RQuantAdd(RQUANT_WORLDBATCHES, 1);
	}
	else
	{
		RQuantAdd(RQUANT_ENTBATCHES, 1);
	}

	flags = shaderstate.curshader->flags;
	GL_CullFace(flags & (SHADER_CULL_FRONT|SHADER_CULL_BACK));

	if (shaderstate.sourcevbo->coord2.gl.addr && (shaderstate.curshader->numdeforms || !shaderstate.curshader->prog))
		GenerateVertexBlends(shaderstate.curshader);
	else if (shaderstate.curshader->numdeforms)
		GenerateVertexDeforms(shaderstate.curshader);
	else
	{
		shaderstate.pendingvertexpointer = shaderstate.sourcevbo->coord.gl.addr;
		shaderstate.pendingvertexvbo = shaderstate.sourcevbo->coord.gl.vbo;
	}

#ifdef _DEBUG
	if (!shaderstate.pendingvertexpointer && !shaderstate.pendingvertexvbo)
	{
		Con_Printf(CON_ERROR "pendingvertexpointer+vbo are both null! shader is %s\n", shaderstate.curshader->name);
		return;
	}
#endif

#ifdef FTE_TARGET_WEB
	if (!shaderstate.pendingvertexvbo)
	{
		int len = 0, m;
		mesh_t *meshlist;
		for (m = 0; m < shaderstate.meshcount; m++)
		{
			meshlist = shaderstate.meshes[m];
			if (len < meshlist->vbofirstvert + meshlist->numvertexes)
				len = meshlist->vbofirstvert + meshlist->numvertexes;
		}
		len *= sizeof(vecV_t);

		shaderstate.streamid = (shaderstate.streamid + 1) & (sizeof(shaderstate.streamvbo)/sizeof(shaderstate.streamvbo[0]) - 1);
		GL_SelectVBO(shaderstate.pendingvertexvbo = shaderstate.streamvbo[shaderstate.streamid]);
		qglBufferDataARB(GL_ARRAY_BUFFER_ARB, len, shaderstate.pendingvertexpointer, GL_STREAM_DRAW_ARB);
		shaderstate.pendingvertexpointer = NULL;
	}
	if (!shaderstate.sourcevbo->indicies.gl.vbo)
		return;
#endif

	BE_PolyOffset();
	switch(shaderstate.mode)
	{
	case BEM_STENCIL:
		Host_Error("Shader system is not meant to accept stencil meshes\n");
		break;
#ifdef RTLIGHTS
	case BEM_LIGHT:
		altshader = shaderstate.curshader->bemoverrides[shaderstate.lightmode];
		if (!altshader)
			altshader = shaderstate.shader_light[shaderstate.lightmode];
		if (altshader && altshader->prog)
		{
			shaderstate.pendingcolourvbo = shaderstate.sourcevbo->colours[0].gl.vbo;
			shaderstate.pendingcolourpointer = shaderstate.sourcevbo->colours[0].gl.addr;
			shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;
			shaderstate.pendingtexcoordparts[0] = 2;
			shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
			shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;
			BE_RenderMeshProgram(altshader, altshader->passes, altshader->prog);
		}
#ifndef GLSLONLY
		else
			BE_LegacyLighting();
#endif
		break;
	case BEM_GBUFFER:
		altshader = shaderstate.curshader->bemoverrides[bemoverride_gbuffer];
		if (altshader)
		{
			if (altshader->prog)
			{
				shaderstate.pendingcolourvbo = shaderstate.sourcevbo->colours[0].gl.vbo;
				shaderstate.pendingcolourpointer = shaderstate.sourcevbo->colours[0].gl.addr;
				shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;
				shaderstate.pendingtexcoordparts[0] = 2;
				shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
				shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;
				BE_RenderMeshProgram(altshader, altshader->passes, altshader->prog);
			}
			else if (altshader->numpasses && altshader->passes[0].prog)
			{
				shaderstate.pendingcolourvbo = shaderstate.sourcevbo->colours[0].gl.vbo;
				shaderstate.pendingcolourpointer = shaderstate.sourcevbo->colours[0].gl.addr;
				shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;
				shaderstate.pendingtexcoordparts[0] = 2;
				shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
				shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;
				BE_RenderMeshProgram(altshader, altshader->passes, altshader->passes[0].prog);
			}
		}

		break;
#endif
	case BEM_CREPUSCULAR:
		//nettest: a first-person viewmodel is drawn with the weapon-view matrix + RF_DEPTHHACK
		//projection (see :4240), which does NOT map to its on-screen position in this world-space
		//mask FBO -- so as a crepuscular occluder its silhouette lands at the model origin (screen
		//centre) instead of the drawn gun.  A weapon overlay shouldn't cast sun shadow-rays anyway,
		//so skip it here; it still occludes the composited rays via its own opaque pixels.
		if (shaderstate.curentity && (shaderstate.curentity->flags & RF_DEPTHHACK))
			break;
		altshader = shaderstate.curshader->bemoverrides[bemoverride_crepuscular];
		if (!altshader && (shaderstate.curshader->flags & SHADER_SKY))
			altshader = shaderstate.crepskyshader;
		if (!altshader)
			altshader = shaderstate.crepopaqueshader;

		if (altshader && altshader->prog)
		{
			shaderstate.pendingcolourvbo = shaderstate.sourcevbo->colours[0].gl.vbo;
			shaderstate.pendingcolourpointer = shaderstate.sourcevbo->colours[0].gl.addr;
			shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;
			shaderstate.pendingtexcoordparts[0] = 2;
			shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
			shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;
			BE_RenderMeshProgram(altshader, altshader->passes, altshader->prog);
		}
		break;
	case BEM_DEPTHONLY:
		altshader = shaderstate.curshader->bemoverrides[bemoverride_depthonly];
		if (!altshader)
			altshader = shaderstate.depthonlyshader;

		if (altshader && altshader->prog)
		{
			shaderstate.pendingcolourvbo = shaderstate.sourcevbo->colours[0].gl.vbo;
			shaderstate.pendingcolourpointer = shaderstate.sourcevbo->colours[0].gl.addr;
			shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;
			shaderstate.pendingtexcoordparts[0] = 2;
			shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
			shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;
			BE_RenderMeshProgram(altshader, altshader->passes, altshader->prog);
		}
		else
		{
			GL_DeSelectProgram();
#ifdef warningmsg
#pragma warningmsg("fixme: support alpha test")
#endif
			BE_EnableShaderAttributes((1u<<VATTR_LEG_VERTEX), 0);
			BE_SubmitMeshChain(false);	//fixme: dangerous
		}
		break;

	case BEM_FOG:
#ifndef GLSLONLY
		GenerateTCFog(0, NULL);
		BE_EnableShaderAttributes((1u<<VATTR_LEG_VERTEX) | (1u<<VATTR_LEG_COLOUR) | (1u<<VATTR_LEG_TMU0), 0);
		BE_SubmitMeshChain(false);
#endif
		break;

	case BEM_WIREFRAME:
		if (shaderstate.wireframeshader && shaderstate.wireframeshader->prog)
		{
			shaderstate.pendingcolourvbo = shaderstate.sourcevbo->colours[0].gl.vbo;
			shaderstate.pendingcolourpointer = shaderstate.sourcevbo->colours[0].gl.addr;
			shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;
			shaderstate.pendingtexcoordparts[0] = 2;
			shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
			shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;
			BE_RenderMeshProgram(shaderstate.wireframeshader, shaderstate.wireframeshader->passes, shaderstate.wireframeshader->prog);
		}
#ifndef GLSLONLY
		else if (!gl_config_nofixedfunc)
		{
			BE_SetPassBlendMode(0, PBM_REPLACE);
			GL_DeSelectProgram();

			shaderstate.pendingcolourvbo = 0;
			shaderstate.pendingcolourpointer = NULL;
			Vector4Set(shaderstate.pendingcolourflat, 1, 1, 1, 1);
			while(shaderstate.lastpasstmus>0)
			{
				GL_LazyBind(--shaderstate.lastpasstmus, r_nulltex);
			}
			BE_SendPassBlendDepthMask((shaderstate.curshader->passes[0].shaderbits & ~SBITS_BLEND_BITS) | SBITS_SRCBLEND_SRC_ALPHA | SBITS_DSTBLEND_ONE_MINUS_SRC_ALPHA | ((r_wireframe.ival == 1)?SBITS_MISC_NODEPTHTEST:0));

			BE_EnableShaderAttributes((1u<<VATTR_LEG_VERTEX) | (1u<<VATTR_LEG_COLOUR), 0);
			BE_SubmitMeshChain(false);
		}
#endif
		break;
	case BEM_DEPTHDARK:
		if ((shaderstate.curshader->flags & (SHADER_HASLIGHTMAP|SHADER_NODLIGHT))==SHADER_HASLIGHTMAP && !TEXVALID(shaderstate.curtexnums->fullbright))
		{
			if (gl_config.arb_shader_objects)
			{
				if (!shaderstate.allblackshader.glsl.handle)
				{
					const char *defs[] = {NULL};
					shaderstate.allblackshader = GLSlang_CreateProgram(NULL, "allblackprogram", sh_config.minver, defs, "#include \"sys/skeletal.h\"\nvoid main(){gl_Position = skeletaltransform();}", NULL, NULL, NULL, "void main(){gl_FragColor=vec4(0.0,0.0,0.0,1.0);}", false, NULL);
					shaderstate.allblack_mvp = qglGetUniformLocationARB(shaderstate.allblackshader.glsl.handle, "m_modelviewprojection");
				}

				GL_SelectProgram(shaderstate.allblackshader.glsl.handle);
				BE_SendPassBlendDepthMask(shaderstate.curshader->passes[0].shaderbits);
				BE_EnableShaderAttributes(gl_config_nofixedfunc?(1u<<VATTR_VERTEX1):(1u<<VATTR_LEG_VERTEX), 0);
				if (shaderstate.allblackshader.glsl.handle != shaderstate.lastuniform && shaderstate.allblack_mvp != -1)
				{
					float m16[16];
					Matrix4_Multiply(shaderstate.projectionmatrix, shaderstate.modelviewmatrix, m16);
					qglUniformMatrix4fvARB(shaderstate.allblack_mvp, 1, false, m16);
				}
				BE_SubmitMeshChain(shaderstate.allblackshader.glsl.usetesselation);

				shaderstate.lastuniform = shaderstate.allblackshader.glsl.handle;
				break;
			}
#ifndef GLSLONLY
			else if (!gl_config_nofixedfunc)
			{
				GL_DeSelectProgram();
				shaderstate.pendingcolourvbo = 0;
				shaderstate.pendingcolourpointer = NULL;
				Vector4Set(shaderstate.pendingcolourflat, 0, 0, 0, 1);
				while(shaderstate.lastpasstmus>0)
				{
					GL_LazyBind(--shaderstate.lastpasstmus, r_nulltex);
				}

				BE_SetPassBlendMode(0, PBM_REPLACE);
				BE_SendPassBlendDepthMask(shaderstate.curshader->passes[0].shaderbits);

				BE_EnableShaderAttributes((1u<<VATTR_LEG_VERTEX) | (1u<<VATTR_LEG_COLOUR), 0);
				BE_SubmitMeshChain(false);
				break;
			}
#endif
		}
		//fallthrough
	case BEM_STANDARD:
	default:
		if (shaderstate.curshader->prog)
		{
			shaderstate.pendingcolourvbo = shaderstate.sourcevbo->colours[0].gl.vbo;
			shaderstate.pendingcolourpointer = shaderstate.sourcevbo->colours[0].gl.addr;
			shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;

			shaderstate.pendingtexcoordparts[0] = 2;
			shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
			shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;

			BE_RenderMeshProgram(shaderstate.curshader, shaderstate.curshader->passes, shaderstate.curshader->prog);
		}
		else if (gl_config_nofixedfunc)
		{
#ifdef FTE_TARGET_WEB
			int maxverts = 0, m;
			mesh_t *meshlist;
			for (m = 0; m < shaderstate.meshcount; m++)
			{
				meshlist = shaderstate.meshes[m];
				if (maxverts < meshlist->vbofirstvert + meshlist->numvertexes)
					maxverts = meshlist->vbofirstvert + meshlist->numvertexes;
			}
#endif

			while (passno < shaderstate.curshader->numpasses)
			{
				int emumode;
				p = &shaderstate.curshader->passes[passno];
				passno += p->numMergedPasses;

				if (p->prog)
				{
					shaderstate.pendingcolourvbo = shaderstate.sourcevbo->colours[0].gl.vbo;
					shaderstate.pendingcolourpointer = shaderstate.sourcevbo->colours[0].gl.addr;
					shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;

					shaderstate.pendingtexcoordparts[0] = 2;
					shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
					shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;

					BE_RenderMeshProgram(shaderstate.curshader, p, p->prog);
					continue;
				}

				emumode = 0;
				emumode = (p->shaderbits & SBITS_ATEST_BITS) >> SBITS_ATEST_SHIFT;

				GenerateColourMods(p);
				if (!shaderstate.colourarraytype)
				{
					emumode |= 4;
					shaderstate.lastuniform = 0;	//FIXME: s_colour uniform might be wrong.
				}
#ifdef FTE_TARGET_WEB
				else if (!shaderstate.pendingcolourvbo && shaderstate.pendingcolourpointer)
				{
					shaderstate.streamid = (shaderstate.streamid + 1) & (sizeof(shaderstate.streamvbo)/sizeof(shaderstate.streamvbo[0]) - 1);
					GL_SelectVBO(shaderstate.pendingcolourvbo = shaderstate.streamvbo[shaderstate.streamid]);
					switch(shaderstate.colourarraytype)
					{
					case GL_FLOAT:
						qglBufferDataARB(GL_ARRAY_BUFFER_ARB, maxverts * sizeof(vec4_t), shaderstate.pendingcolourpointer, GL_STREAM_DRAW_ARB);
						break;
					case GL_UNSIGNED_BYTE:
						qglBufferDataARB(GL_ARRAY_BUFFER_ARB, maxverts * sizeof(byte_vec4_t), shaderstate.pendingcolourpointer, GL_STREAM_DRAW_ARB);
						break;
					}
					shaderstate.pendingcolourpointer = NULL;
				}
#endif

				BE_GeneratePassTC(p, 0);
#ifdef FTE_TARGET_WEB
				if (!shaderstate.pendingtexcoordvbo[0] && shaderstate.pendingtexcoordpointer[0])
				{
					shaderstate.streamid = (shaderstate.streamid + 1) & (sizeof(shaderstate.streamvbo)/sizeof(shaderstate.streamvbo[0]) - 1);
					GL_SelectVBO(shaderstate.pendingtexcoordvbo[0] = shaderstate.streamvbo[shaderstate.streamid]);
					qglBufferDataARB(GL_ARRAY_BUFFER_ARB, maxverts * sizeof(float) * shaderstate.pendingtexcoordparts[0], shaderstate.pendingtexcoordpointer[0], GL_STREAM_DRAW_ARB);
					shaderstate.pendingtexcoordpointer[0] = NULL;
				}
#endif

				if (!shaderstate.programfixedemu[emumode])
				{
					char *modes[] = {
						"","#ALPHATEST=>0.0","#ALPHATEST=<0.5","#ALPHATEST=>=0.5",
						"#UC","#ALPHATEST=>0.0#UC","#ALPHATEST=<0.5#UC","#ALPHATEST=>=0.5#UC"
					};
					shaderstate.programfixedemu[emumode] = Shader_FindGeneric(va("fixedemu%s", modes[emumode]), QR_OPENGL);
					if (!shaderstate.programfixedemu[emumode])
						break;
				}

				BE_RenderMeshProgram(shaderstate.curshader, p, shaderstate.programfixedemu[emumode]);
			}
			break;
		}
#ifndef GLSLONLY
		else
		{
			//FTESurf Patch 266: the live path on a compatibility context, and the one
			//that had no fog of any kind.  See BE_ProglessFogBegin.
			qboolean fogging = BE_ProglessFogBegin();
			while (passno < shaderstate.curshader->numpasses)
			{
				p = &shaderstate.curshader->passes[passno];
				passno += p->numMergedPasses;
		//		if (p->flags & SHADER_PASS_DETAIL)
		//			continue;

				if (p->prog)
				{
					shaderstate.pendingcolourvbo = shaderstate.sourcevbo->colours[0].gl.vbo;
					shaderstate.pendingcolourpointer = shaderstate.sourcevbo->colours[0].gl.addr;
					shaderstate.colourarraytype = shaderstate.sourcevbo->colours_bytes?GL_UNSIGNED_BYTE:GL_FLOAT;

					shaderstate.pendingtexcoordparts[0] = 2;
					shaderstate.pendingtexcoordvbo[0] = shaderstate.sourcevbo->texcoord.gl.vbo;
					shaderstate.pendingtexcoordpointer[0] = shaderstate.sourcevbo->texcoord.gl.addr;

					BE_RenderMeshProgram(shaderstate.curshader, p, p->prog);
				}
				else
				{
					GL_DeSelectProgram();
					DrawPass(p);
				}
			}
			BE_ProglessFogEnd(fogging);
		}
		if (shaderstate.curbatch->fog && shaderstate.curbatch->fog->shader)
		{
			//FIXME: if glsl, do this fog volume crap properly!

			GL_DeSelectProgram();

			GenerateFogTexture(&shaderstate.fogtexture, shaderstate.curbatch->fog->shader->fog_dist, 2048);
			shaderstate.fogfar = 1.0f/2048; /*scaler for z coords*/

			while(shaderstate.lastpasstmus>1)
			{
				GL_LazyBind(--shaderstate.lastpasstmus, r_nulltex);
			}
			GL_LazyBind(0, shaderstate.fogtexture);
			shaderstate.lastpasstmus = 1;

			Vector4Scale(shaderstate.curbatch->fog->shader->fog_color, (1/255.0), shaderstate.pendingcolourflat);
			shaderstate.pendingcolourvbo = 0;
			shaderstate.pendingcolourpointer = NULL;
			BE_SetPassBlendMode(0, PBM_MODULATE);
			BE_SendPassBlendDepthMask(SBITS_SRCBLEND_SRC_ALPHA | SBITS_DSTBLEND_ONE_MINUS_SRC_ALPHA | (shaderstate.curshader->numpasses?SBITS_DEPTHFUNC_EQUAL:0));

			GenerateTCFog(0, shaderstate.curbatch->fog);
			BE_EnableShaderAttributes((1u<<VATTR_LEG_VERTEX) | (1u<<VATTR_LEG_COLOUR) | (1u<<VATTR_LEG_TMU0), 0);
			BE_SubmitMeshChain(false);
		}
		break;
#endif
	}
}

static void BE_GenTempMeshVBO(vbo_t **vbo, mesh_t *m)
{
	*vbo = &shaderstate.dummyvbo;

	//this code is shit shit shit.
	if (shaderstate.streamvbo[0])
	{
		//use a local. can't use a static, that crashes the compiler due to memory use, so lets eat the malloc or stack or whatever because we really don't have a choice.
		static char *buffer;
		size_t len = 0;
		if (!buffer)
			buffer = malloc(65536 * 33 * sizeof(float));

		//we're not doing vao... but just in case someone added that as an extension...
		GL_DeselectVAO();

		//cycle the vbo. this reduces the likelyhood that we'll have to wait for the vbo to no longer be in use.
		//remember, we can't do nice things like orphan in webgl. we have to create a new buffer every single time.
		//we can't stream with BufferSubData, because browsers are pure shite, and probably mmap and read back huge blocks of code as they translate the calls to direct3d nonsense.
		//we can't use client memory and let the driver do the right thing, because lets face it, our driver is technically a browser. its safer to run everything in a scripted language than to fight the nonsense.
		//how many buffers do we need? no idea. all this memory allocation is going to be shit for performance.
		//but we really do not have a choice. This is the only way to 'stream' without dropping down to <1fps.
		//although arguably we should build our entire hud+2d stuff into a single vbo each frame... meh. At least this keeps the memory use in the driver's 64bit memory space instead of the browser's 32bit one...
		shaderstate.streamid = (shaderstate.streamid + 1) & (sizeof(shaderstate.streamvbo)/sizeof(shaderstate.streamvbo[0]) - 1);
		shaderstate.dummyvbo.vao = shaderstate.streamvao[shaderstate.streamid];
		shaderstate.dummyvbo.vaodynamic = ~0;
		shaderstate.dummyvbo.vaoenabled = 0;
		if (shaderstate.dummyvbo.vao)
		{
			qglBindVertexArray(shaderstate.dummyvbo.vao);
			shaderstate.currentvao = shaderstate.dummyvbo.vao;
		}
		GL_SelectVBO(shaderstate.streamvbo[shaderstate.streamid]);
		GL_SelectEBO(shaderstate.streamebo[shaderstate.streamid]);

		memcpy(buffer+len, m->xyz_array, sizeof(*m->xyz_array) * m->numvertexes);
		shaderstate.dummyvbo.coord.gl.addr = (void*)len;
		shaderstate.dummyvbo.coord.gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
		len += sizeof(*m->xyz_array) * m->numvertexes;

		if (m->xyz2_array)
		{
			memcpy(buffer+len, m->xyz2_array, sizeof(*m->xyz2_array) * m->numvertexes);
			shaderstate.dummyvbo.coord2.gl.addr = (void*)len;
			shaderstate.dummyvbo.coord2.gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
			len += sizeof(*m->xyz2_array) * m->numvertexes;
		}
		else
		{
			shaderstate.dummyvbo.coord2.gl.addr = NULL;
			shaderstate.dummyvbo.coord2.gl.vbo = 0;
		}

		memcpy(buffer+len, m->st_array, sizeof(*m->st_array) * m->numvertexes);
		shaderstate.dummyvbo.texcoord.gl.addr = (void*)len;
		shaderstate.dummyvbo.texcoord.gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
		len += sizeof(*m->st_array) * m->numvertexes;

		//nettest: lightmap texcoords (needed by lightmapped poly/scenetris batches, e.g.
		//per-pixel-lit decals - see r_decal_lightmap). Always set lmcoord[0] (copy or NULL)
		//to clear any stale pointer from a previous mesh.
		if (m->lmst_array[0])
		{
			memcpy(buffer+len, m->lmst_array[0], sizeof(*m->lmst_array[0]) * m->numvertexes);
			shaderstate.dummyvbo.lmcoord[0].gl.addr = (void*)len;
			shaderstate.dummyvbo.lmcoord[0].gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
			len += sizeof(*m->lmst_array[0]) * m->numvertexes;
		}
		else
		{
			shaderstate.dummyvbo.lmcoord[0].gl.addr = NULL;
			shaderstate.dummyvbo.lmcoord[0].gl.vbo = 0;
		}

		if (m->colors4f_array[0])
		{
			memcpy(buffer+len, m->colors4f_array[0], sizeof(*m->colors4f_array[0]) * m->numvertexes);
			shaderstate.dummyvbo.colours[0].gl.addr = (void*)len;
			shaderstate.dummyvbo.colours[0].gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
			len += sizeof(*m->colors4f_array[0]) * m->numvertexes;
			shaderstate.dummyvbo.colours_bytes = false;
		}
		else if (m->colors4b_array)
		{
			memcpy(buffer+len, m->colors4b_array, sizeof(*m->colors4b_array) * m->numvertexes);
			shaderstate.dummyvbo.colours[0].gl.addr = (void*)len;
			shaderstate.dummyvbo.colours[0].gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
			len += sizeof(*m->colors4b_array) * m->numvertexes;
			shaderstate.dummyvbo.colours_bytes = true;
		}
		else
		{
			shaderstate.dummyvbo.colours[0].gl.addr = NULL;
			shaderstate.dummyvbo.colours[0].gl.vbo = 0;
			shaderstate.dummyvbo.colours_bytes = false;
		}

		if (m->normals_array)
		{
			memcpy(buffer+len, m->normals_array, sizeof(*m->normals_array) * m->numvertexes);
			shaderstate.dummyvbo.normals.gl.addr = (void*)len;
			shaderstate.dummyvbo.normals.gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
			len += sizeof(*m->normals_array) * m->numvertexes;
		}
		else
		{
			shaderstate.dummyvbo.normals.gl.addr = NULL;
			shaderstate.dummyvbo.normals.gl.vbo = 0;
		}

		if (m->snormals_array)
		{
			memcpy(buffer+len, m->snormals_array, sizeof(*m->snormals_array) * m->numvertexes);
			shaderstate.dummyvbo.svector.gl.addr = (void*)len;
			shaderstate.dummyvbo.svector.gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
			len += sizeof(*m->snormals_array) * m->numvertexes;
		}
		else
		{
			shaderstate.dummyvbo.svector.gl.addr = NULL;
			shaderstate.dummyvbo.svector.gl.vbo = 0;
		}

		if (m->tnormals_array)
		{
			memcpy(buffer+len, m->tnormals_array, sizeof(*m->tnormals_array) * m->numvertexes);
			shaderstate.dummyvbo.tvector.gl.addr = (void*)len;
			shaderstate.dummyvbo.tvector.gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
			len += sizeof(*m->tnormals_array) * m->numvertexes;
		}
		else
		{
			shaderstate.dummyvbo.tvector.gl.addr = NULL;
			shaderstate.dummyvbo.tvector.gl.vbo = 0;
		}

		if (m->bonenums)
		{
			memcpy(buffer+len, m->bonenums, sizeof(*m->bonenums) * m->numvertexes);
			shaderstate.dummyvbo.bonenums.gl.addr = (void*)len;
			shaderstate.dummyvbo.bonenums.gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
			len += sizeof(*m->bonenums) * m->numvertexes;
		}
		else
		{
			shaderstate.dummyvbo.bonenums.gl.addr = NULL;
			shaderstate.dummyvbo.bonenums.gl.vbo = 0;
		}

		if (m->boneweights)
		{
			memcpy(buffer+len, m->boneweights, sizeof(*m->boneweights) * m->numvertexes);
			shaderstate.dummyvbo.boneweights.gl.addr = (void*)len;
			shaderstate.dummyvbo.boneweights.gl.vbo = shaderstate.streamvbo[shaderstate.streamid];
			len += sizeof(*m->boneweights) * m->numvertexes;
		}
		else
		{
			shaderstate.dummyvbo.boneweights.gl.addr = NULL;
			shaderstate.dummyvbo.boneweights.gl.vbo = 0;
		}

		//FIXME: normals, svector, tvector, bone nums, bone weights

		//now we've got a single buffer in a single place, update the buffer
		qglBufferDataARB(GL_ARRAY_BUFFER_ARB, len, buffer, GL_STREAM_DRAW_ARB);



		//and finally the elements array, which is a much simpler affair
		qglBufferDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, sizeof(*m->indexes) * m->numindexes, m->indexes, GL_STREAM_DRAW_ARB);
		shaderstate.dummyvbo.indicies.gl.addr = (void*)NULL;
		shaderstate.dummyvbo.indicies.gl.vbo = shaderstate.streamebo[shaderstate.streamid];
	}
	else
	{
		//client memory. may be slower. will probably be faster.
		shaderstate.dummyvbo.coord.gl.addr = m->xyz_array;
		shaderstate.dummyvbo.coord2.gl.addr = m->xyz2_array;
		shaderstate.dummyvbo.texcoord.gl.addr = m->st_array;
		//nettest: lightmap texcoords for lightmapped poly batches (r_decal_lightmap). NULL when
		//absent so a stale pointer from a previous mesh can't leak in.
		shaderstate.dummyvbo.lmcoord[0].gl.addr = m->lmst_array[0];
		shaderstate.dummyvbo.lmcoord[0].gl.vbo = 0;
		shaderstate.dummyvbo.indicies.gl.addr = m->indexes;
		shaderstate.dummyvbo.normals.gl.addr = m->normals_array;
		shaderstate.dummyvbo.svector.gl.addr = m->snormals_array;
		shaderstate.dummyvbo.tvector.gl.addr = m->tnormals_array;
		if (m->colors4f_array[0])
		{
			shaderstate.dummyvbo.colours_bytes = false;
			shaderstate.dummyvbo.colours[0].gl.addr = m->colors4f_array[0];
		}
		else
		{
			shaderstate.dummyvbo.colours_bytes = true;
			shaderstate.dummyvbo.colours[0].gl.addr = m->colors4b_array;
		}
		shaderstate.dummyvbo.bonenums.gl.addr = m->bonenums;
		shaderstate.dummyvbo.boneweights.gl.addr = m->boneweights;
	}
	shaderstate.dummyvbo.bones = m->bones;
	shaderstate.dummyvbo.numbones = m->numbones;
}

void GLBE_DrawMesh_List(shader_t *shader, int nummeshes, mesh_t **meshlist, vbo_t *vbo, texnums_t *texnums, unsigned int beflags)
{
	shaderstate.curbatch = &shaderstate.dummybatch;
	shaderstate.curshader = shader->remapto;

	if (!vbo)
	{
		mesh_t *m;
		shaderstate.flags = beflags;
		TRACE(("GLBE_DrawMesh_List: shader %s\n", shader->name));
		if (shaderstate.curentity != &r_worldentity)
			GLBE_SelectEntity(&r_worldentity);
		shaderstate.curtime = shaderstate.updatetime - (shaderstate.curentity->shaderTime + shader->remaptime);
		if (texnums)
			shaderstate.curtexnums = texnums;
		else if (shader->numdefaulttextures)
			shaderstate.curtexnums = shader->defaulttextures + ((int)(shader->defaulttextures_fps * shaderstate.curtime) % shader->numdefaulttextures);
		else
			shaderstate.curtexnums = shader->defaulttextures ? shader->defaulttextures : &r_nulltexnums;	//nettest: guard NULL defaulttextures (registered-but-not-yet-generated shader) — see r_nulltexnums

		while (nummeshes--)
		{
			m = *meshlist++;

			BE_GenTempMeshVBO(&shaderstate.sourcevbo, m);

			shaderstate.meshcount = 1;
			shaderstate.meshes = &m;
			DrawMeshes();
		}
	}
	else
	{
		shaderstate.sourcevbo = vbo;
		shaderstate.flags = beflags;
		if (shaderstate.curentity != &r_worldentity)
			GLBE_SelectEntity(&r_worldentity);
		shaderstate.curtime = shaderstate.updatetime - (shaderstate.curentity->shaderTime + shader->remaptime);
		if (texnums)
			shaderstate.curtexnums = texnums;
		else if (shader->numdefaulttextures)
			shaderstate.curtexnums = shader->defaulttextures + ((int)(shader->defaulttextures_fps * shaderstate.curtime) % shader->numdefaulttextures);
		else
			shaderstate.curtexnums = shader->defaulttextures ? shader->defaulttextures : &r_nulltexnums;	//nettest: guard NULL defaulttextures (registered-but-not-yet-generated shader) — see r_nulltexnums

		shaderstate.meshcount = nummeshes;
		shaderstate.meshes = meshlist;
		DrawMeshes();
	}
}
void GLBE_DrawMesh_Single(shader_t *shader, mesh_t *mesh, vbo_t *vbo, unsigned int beflags)
{
	shader->next = NULL;
	GLBE_DrawMesh_List(shader, 1, &mesh, NULL, NULL, beflags);
}

void GLBE_SubmitBatch(batch_t *batch)
{
	shader_t *sh;
	shaderstate.curbatch = batch;
	if (batch->vbo)
	{
		shaderstate.sourcevbo = batch->vbo;

		if (!batch->vbo->vao)
			batch->vbo->vao = shaderstate.streamvao[0];
		batch->vbo->vaodynamic = ~0;
		batch->vbo->vaoenabled = 0;
	}
	else
	{
		//we're only allowed one mesh per batch if there's no vbo info.
		BE_GenTempMeshVBO(&shaderstate.sourcevbo, batch->mesh[0]);
	}

	sh = batch->shader;
	if (!sh)	//nettest: the portal/depthmask loops call GLBE_SubmitBatch directly (bypassing the !bs guard on the sortlist path), so a still-NULL shader (e.g. a frame-1 unparsed Source water material) would fault on sh->remapto
		return;
	shaderstate.curshader = sh->remapto;
	shaderstate.flags = batch->flags;
	if (shaderstate.curentity != batch->ent)
		GLBE_SelectEntity(batch->ent);
	shaderstate.curtime = shaderstate.updatetime - (shaderstate.curentity->shaderTime + sh->remaptime);
	if (batch->skin)
		shaderstate.curtexnums = batch->skin;
	else if (sh->numdefaulttextures)
		shaderstate.curtexnums = sh->defaulttextures + ((int)(sh->defaulttextures_fps * shaderstate.curtime) % sh->numdefaulttextures);
	else
		shaderstate.curtexnums = sh->defaulttextures ? sh->defaulttextures : &r_nulltexnums;	//nettest: guard NULL defaulttextures (a Source prop's GLSL material on first frame faults in BE_RenderMeshProgram otherwise) — see r_nulltexnums

	if (0)
	{
		int i;
		for (i = batch->firstmesh; i < batch->meshes; i++)
		{
			shaderstate.meshcount = 1;
			shaderstate.meshes = &batch->mesh[i];
			DrawMeshes();
		}
	}
	else
	{
		shaderstate.meshcount = batch->meshes - batch->firstmesh;
		shaderstate.meshes = batch->mesh+batch->firstmesh;
		DrawMeshes();
	}
}

static void GLBE_SubmitMeshesPortals(batch_t **worldlist, batch_t *dynamiclist)
{
	batch_t *batch, *masklists[2];
	int i;
	float il;

	if (!dynamiclist && !worldlist[SHADER_SORT_PORTAL])
		return;	//no portals to draw

	/*attempt to draw portal shaders*/
	if (shaderstate.mode == BEM_STANDARD)
	{
		/* FTESurf Patch 206: decide ONCE, before anything is rendered or masked,
		   which portals get a view this time.  All three loops -- this one, the
		   depth-mask loop below, and GLR_DrawPortal's own mask loop -- then ask
		   the same question and get the same answer.  Their disagreeing about it
		   is the bug this patch is mostly about. */
		GLR_PortalBudgetBegin(worldlist, dynamiclist);

		for (i = 0; i < 2; i++)
		{
			for (batch = i?dynamiclist:worldlist[SHADER_SORT_PORTAL]; batch; batch = batch->next)
			{
				if (batch->meshes == batch->firstmesh)
					continue;

				/* FTESurf Patch 210: a portal that paints ITSELF is not painted here.

				   This loop is the paint-over-the-screen-then-mask-it-back-off
				   design, and it is the reason the far room appeared as a
				   rectangle rather than as the doorway.  A SHADER_HASPORTAL
				   aperture takes the other route entirely: it is rendered to an
				   FBO and sampled per-pixel by its own surface, from the ordinary
				   sort list, in GLBE_SubmitMeshesSortList -> GLBE_GenerateBatchTextures.

				   It must be skipped in BOTH loops or it would pay for two full
				   scene renders and then mask off the one it wanted.  gl_shader.c
				   does the same thing for `dp_camera` by demoting the sort; that
				   is not available here because the sort is what puts this batch in
				   front of GLR_PortalBudgetBegin, which has already scored it. */
				if (batch->shader && (batch->shader->flags & SHADER_HASPORTAL))
					continue;

				if (batch->buildmeshes)
					batch->buildmeshes(batch);

				/* Out of budget: not recursed AND not masked, so the batch falls
				   through to the ordinary sort list where a zero-pass portal
				   shader draws nothing and the wall behind it survives.  That is
				   the "closed" look, and it costs nothing to produce. */
				if (!GLR_PortalWouldDraw(batch))
					continue;

				il = shaderstate.identitylighting;
				masklists[0] = worldlist[SHADER_SORT_PORTAL];
				masklists[1] = dynamiclist;
				GLR_DrawPortal(batch, worldlist, masklists, 0);
				shaderstate.identitylighting = il;

				/*clear depth again*/
				GL_ForceDepthWritable();
				qglClear(GL_DEPTH_BUFFER_BIT);
			}
		}
		//make sure the current scene doesn't draw over the portal where its not meant to. clamp depth so the near clip plane doesn't cause problems.
		if (gl_config.arb_depth_clamp)
			qglEnable(GL_DEPTH_CLAMP_ARB);
		/*draw depth only, to mask it off*/
		if (r_portaldebug.ival == 2)	//P206 bisect: no mask at all
			return;
		GLBE_SelectMode(BEM_DEPTHONLY);
		for (i = 0; i < 2; i++)
		{
			for (batch = i?dynamiclist:worldlist[SHADER_SORT_PORTAL]; batch; batch = batch->next)
			{
//				if (batch->meshes == batch->firstmesh)
//					continue;

				/* FTESurf Patch 203, same reason as the depthmask loop in
				   GLR_DrawPortal: an alias-model portal's mesh pointer is a
				   file-static shared by every alias batch, and it only describes
				   THIS batch between its build and its submit.  The loop above
				   built each one just before recursing a whole scene through it,
				   so by now they all point at whichever was built last.  Rebuild
				   in step; skip the ones that produce nothing rather than letting
				   GLBE_SubmitBatch dereference a NULL mesh[0]. */
				/* FTESurf Patch 206: MASK ONLY WHAT WAS ACTUALLY DRAWN.

				   This loop's job is to stop the ordinary scene painting over a
				   portal's contents.  It had no idea which portals had contents.
				   GLR_DrawPortal refuses on four separate grounds -- above all
				   "the viewer is behind this plane" -- and every refusal still
				   got its wall-killing depth written here, with nothing rendered
				   behind it.  That is the user's "it deletes the wall behind it
				   and has a square transparency in the middle": half of a set of
				   PAIRED doors faces away from you at any instant, and `cull
				   none` means the back face masks just as happily as the front.

				   Ask the same question the renderer asked.  It is a table lookup
				   that touches no mesh, so it goes FIRST and the batches we are
				   not going to mask never pay for a rebuild. */
				/* FTESurf Patch 210: and a self-painting portal is not masked at
				   all -- there is nothing on the screen to protect, and its own
				   opaque surface writes the depth that keeps the world off it. */
				if (batch->shader && (batch->shader->flags & SHADER_HASPORTAL))
					continue;
				if (!GLR_PortalWouldDraw(batch))
					continue;

				/* FTESurf Patch 203, same reason as the depthmask loop in
				   GLR_DrawPortal: an alias-model portal's mesh pointer is a
				   file-static shared by every alias batch, and it only describes
				   THIS batch between its build and its submit.  The loop above
				   built each one just before recursing a whole scene through it,
				   so by now they all point at whichever was built last.  Rebuild
				   in step; skip the ones that produce nothing rather than letting
				   GLBE_SubmitBatch dereference a NULL mesh[0]. */
				if (batch->buildmeshes)
					batch->buildmeshes(batch);
				if (!batch->mesh || batch->meshes == batch->firstmesh)
					continue;

				GLBE_SubmitBatch(batch);
			}
		}
		GLBE_SelectMode(BEM_STANDARD);
		if (gl_config.arb_depth_clamp)
			qglDisable(GL_DEPTH_CLAMP_ARB);
	}
}

//FTESurf Patch 864: depth of water reflection/refraction captures (not portals) being rendered.
static int be_watercapture;

//FTESurf Patch 879: r_capturelog N prints, for N top-level views, each view's water choice and each
//water capture and share: the submitting batch, then "at" the plane and mesh count that place it.
extern cvar_t r_capturelog;
extern int r_topviews;
extern qboolean r_capturemergevis;
qboolean GLR_BatchPlane(batch_t *batch, plane_t *out);
static qboolean GLBE_CaptureLogging(void)
{
	static int firstview, lastview = -1, lastlevel = -1;
	if (r_capturelog.modified)
	{
		r_capturelog.modified = false;
		firstview = r_topviews;
	}
	if (r_capturelog.ival <= 0 || r_topviews - firstview >= r_capturelog.ival)
		return false;
	if (lastview != r_topviews || lastlevel != r_refdef.recurse)
	{
		lastview = r_topviews;
		lastlevel = r_refdef.recurse;
		Con_Printf("[capture] view %i level %i eye %.1f %.1f %.1f\n", r_topviews - firstview, r_refdef.recurse, r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);
	}
	return true;
}
//cs: the shader whose scale and $reflectentities the capture used
static void GLBE_CaptureLog(batch_t *sub, shader_t *cs, const char *kind, batch_t *place)
{
	plane_t p, q;
	if (!GLBE_CaptureLogging())
		return;
	if (!GLR_BatchPlane(sub, &p))
		VectorClear(p.normal), p.dist = 0;
	if (!place || !GLR_BatchPlane(place, &q))
		place = sub, q = p;
	Con_Printf("[capture]  level %i %s %s plane %.3f %.3f %.3f %.2f meshes %i ent %i at %.3f %.3f %.3f %.2f meshes %i scale %g%s\n",
		r_refdef.recurse, kind, sub->shader ? sub->shader->name : "?", p.normal[0], p.normal[1], p.normal[2], p.dist,
		(int)(sub->meshes - sub->firstmesh), sub->ent ? sub->ent->keynum : -1,
		q.normal[0], q.normal[1], q.normal[2], q.dist, (int)(place->meshes - place->firstmesh),
		cs->portalfboscale, cs->reflectnoents ? " noents" : "");
}

//FTESurf Patch 879: Source renders one water reflection and one refraction per view, for the one
//fog volume GetVisibleFogVolume picks from the eye (viewrender.cpp:2400-2420, DetermineWaterRenderInfo
//:2253-2392), and every water material samples those same targets (water.cpp:259-266). Per view the
//water that most fills the screen on the eye's side of it is chosen before anything draws and captured
//once, for all of its batches together (r_waterviews).
extern cvar_t r_waterviews;
#define BE_WATERKINDS (SHADER_HASREFLECT|SHADER_HASREFRACT|SHADER_HASREFRACTDEPTH)
#define BE_WATERPLANES 128	//on-screen facing water planes weighed per view, each over all its batches before the
							//choice (the library's most is 82 up-facing planes on a whole map); at 1 any more share
#define BE_WATERPROBES 3	//traces per plane, from its heaviest meshes
typedef struct
{
	vec3_t eye;				//the view this was chosen for: a submit from anywhere else must not find it
	qboolean under;			//the eye is in water/slime/lava: it looks at undersides
	batch_t **worldbatches;
	batch_t *chosen;		//valid between GLBE_WaterViewsBegin and the end of its GLBE_DrawWorld; NULL: none
	plane_t plane;
	unsigned int need;		//kinds the chosen plane's world batches sample
	unsigned int needall;	//...and every on-screen world water facing the eye (r_waterviews 1 captures them all)
	batch_t combined;		//the chosen plane's on-screen batches as one, then (at 1) every other on-screen water
	mesh_t **mesh;			//facing the eye: the capture's PVS covers the water that samples it
	unsigned int maxmesh;
	batch_t other;			//r_waterviews 2: another plane's batches as one
	mesh_t **omesh;
	unsigned int maxomesh;
	plane_t heldplane;		//what this level's capture targets hold now, and which kinds
	unsigned int helddone;
} bewaterview_t;
static bewaterview_t be_waterview[R_MAX_RECURSE];
static plane_t be_waterlast[MAX_SPLITS];	//each seat's world-drawing top-level view's last choice (hysteresis)
static qboolean be_waterlastvalid[MAX_SPLITS];

static qboolean GLBE_WaterSamePlane(const plane_t *a, const plane_t *b)
{
	return DotProduct(a->normal, b->normal) > 0.999f && fabs(a->dist - b->dist) < 1;
}
//the batch itself or GLBE_SubmitBlendedFaces' stack copy of it
static qboolean GLBE_WaterSameBatch(const batch_t *a, const batch_t *b)
{
	return a == b || (a->mesh == b->mesh && a->firstmesh == b->firstmesh && a->meshes == b->meshes);
}
//a world water batch whose meshes are already this view's (entity batches build theirs at submit, and
//their PVS is their own); ripple shaders render their own target after the captures
static qboolean GLBE_WaterCandidate(batch_t *b)
{
	shader_t *s = b->shader;
	return s && !(s->flags & (SHADER_NODRAW|SHADER_HASPORTAL|SHADER_HASRIPPLEMAP)) && (s->flags & (SHADER_HASREFLECT|SHADER_HASREFRACT)) &&
		b->ent == &r_worldentity && !b->buildmeshes && b->mesh && b->meshes > b->firstmesh;
}
//level water seen from the eye's side: above, a top below the eye; in water, an underside above it.
//Mod_VBSP's walk emits a water's underside whenever the eye is below its plane, in water or not. Source's
//fog volume is level (m_flWaterHeight); sloped water captures on its own.
static qboolean GLBE_WaterFacesEye(const plane_t *p, qboolean under)
{
	if (DotProduct(r_refdef.vieworg, p->normal) - p->dist < -r_refdef.mindist)
		return false;	//behind it: GLR_DrawPortal would refuse the capture
	return under ? p->normal[2] < -0.99f : p->normal[2] > 0.99f;
}
static void GLBE_MeshBox(const mesh_t *mesh, vec3_t lo, vec3_t hi)
{
	int v, k;
	VectorCopy(mesh->xyz_array[0], lo);
	VectorCopy(mesh->xyz_array[0], hi);
	for (v = 1; v < mesh->numvertexes; v++)
		for (k = 0; k < 3; k++)
		{
			if (mesh->xyz_array[v][k] < lo[k]) lo[k] = mesh->xyz_array[v][k];
			if (mesh->xyz_array[v][k] > hi[k]) hi[k] = mesh->xyz_array[v][k];
		}
}
#ifdef PLUGINS
int Plug_OccluderTrace(model_t *mod, const vec3_t start, const vec3_t end);	//engine/common/plugin.c
static qboolean be_occluderasked;	//this map's occluder tree was asked for (at load, or by the first trace)
#endif
//FTESurf Patch 879: build the map plugin's occluder tree during the load when the world has water that can
//capture, so the first frame that sees it does not stall (bhop_matches: 420,554 leaves, 1.1 s). Water that only
//becomes capture water later (an hl2_water change mid-map) still builds it on its first trace, which says so.
void GLBE_WaterViewsNewMap(void)
{
	int i, n = 0, r;
	double t0;
	for (i = 0; i < MAX_SPLITS; i++)
		be_waterlastvalid[i] = false;	//planes of the old map
#ifdef PLUGINS
	be_occluderasked = false;
	if (!r_waterviews.ival || !cl.worldmodel || cl.worldmodel->loadstate != MLS_LOADED)
		return;
	//only what the choice ever weighs: level faces of a capture material (GLBE_WaterFacesEye), so a map whose
	//only capture surfaces are windows or sloped water builds nothing
	for (i = 0; i < cl.worldmodel->nummodelsurfaces; i++)
	{
		msurface_t *s = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface + i];
		shader_t *sh = (s->texinfo && s->texinfo->texture) ? s->texinfo->texture->shader : NULL;
		if (sh && s->plane && fabs(s->plane->normal[2]) > 0.99f &&
			!(sh->flags & (SHADER_NODRAW|SHADER_HASPORTAL|SHADER_HASRIPPLEMAP)) && (sh->flags & (SHADER_HASREFLECT|SHADER_HASREFRACT)))
			n++;
	}
	if (!n)
		return;
	t0 = Sys_DoubleTime();
	r = Plug_OccluderTrace(cl.worldmodel, vec3_origin, vec3_origin);
	if (r >= 0)
		be_occluderasked = true;
	Con_DPrintf("water: %i level capture water faces; occluder tree asked for at map load: %s, %.1f ms (the plugin says built now or already built)\n",
		n, r >= 0 ? "the map plugin answered" : "the map plugin cannot answer (-1): the first occlusion trace asks again", (Sys_DoubleTime() - t0) * 1000);
#else
	Con_DPrintf("water: no plugin hook in this build: occlusion uses the world's movement trace\n");
#endif
}
//nothing opaque between the eye and the point: world brushes and displacements that hide what is behind them
//(the map plugin's occluder tree: no static props -- a chain-link fence is a prop -- and no light-only
//all-nodraw toolsblocklight brushes; Source's MASK_VISIBLE also skips OPAQUE brushes with any nodraw side
//and see-through ones, which still hide water here), else the world's movement trace; then the
//brush entities that draw opaque (func_brush, func_wall, doors), walked as TraceLineR walks them, against
//SOLID only: vbsp makes a non-opaque brush CONTENTS_WINDOW, so glass does not hide water. An invisible
//solid brush (toolsinvisible) still does. An eye inside solid cannot tell: seen.
static qboolean GLBE_WaterSeen(const vec3_t to)
{
	trace_t tr;
	int i, k, r = -1;
	entity_t *pe;
	vec3_t ts, te, lo, hi;
	float ext;
	if (!cl.worldmodel)
		return true;
#ifdef PLUGINS
	if (!be_occluderasked)
	{	//no capture water at load: this trace builds the tree, mid-map
		double t0 = Sys_DoubleTime();
		be_occluderasked = true;
		r = Plug_OccluderTrace(cl.worldmodel, r_refdef.vieworg, to);
		Con_DPrintf("water: occluder tree asked for on the first occlusion trace (not at load): %s, %.1f ms\n",
			r >= 0 ? "the map plugin answered" : "the map plugin cannot answer (-1): the world's movement trace instead",
			(Sys_DoubleTime() - t0) * 1000);
	}
	else
		r = Plug_OccluderTrace(cl.worldmodel, r_refdef.vieworg, to);
#endif
	if (r < 0 && cl.worldmodel->funcs.NativeTrace)
	{
		memset(&tr, 0, sizeof(tr));
		cl.worldmodel->funcs.NativeTrace(cl.worldmodel, 0, NULL, NULL, r_refdef.vieworg, to, vec3_origin, vec3_origin, false, FTECONTENTS_SOLID, &tr);
		r = !tr.startsolid && !tr.allsolid && tr.fraction < 1;
	}
	if (r > 0)
		return false;
	for (k = 0; k < 3; k++)
	{
		lo[k] = min(r_refdef.vieworg[k], to[k]);
		hi[k] = max(r_refdef.vieworg[k], to[k]);
	}
	for (i = 0; i < cl_numvisedicts; i++)
	{
		pe = &cl_visedicts[i];
		if (pe->rtype != RT_MODEL || !pe->model || pe->model == cl.worldmodel || pe->model->type != mod_brush ||
			pe->model->loadstate != MLS_LOADED || !pe->model->funcs.NativeTrace ||
			(pe->flags & (RF_ADDITIVE|RF_NODEPTHTEST|RF_TRANSLUCENT|RF_EXTERNALMODEL)))
			continue;
		for (ext = 0, k = 0; k < 3; k++)	//TraceLineR's trivial reject: the segment's box against the model's reach
		{
			ext = max(ext, fabs(pe->model->maxs[k]));
			ext = max(ext, fabs(pe->model->mins[k]));
		}
		for (k = 0; k < 3; k++)
			if (lo[k] > pe->origin[k] + ext || hi[k] < pe->origin[k] - ext)
				break;
		if (k < 3)
			continue;
		VectorSubtract(r_refdef.vieworg, pe->origin, ts);
		VectorSubtract(to, pe->origin, te);
		memset(&tr, 0, sizeof(tr));
		pe->model->funcs.NativeTrace(pe->model, 0, &pe->framestate, pe->axis, ts, te, vec3_origin, vec3_origin, false, FTECONTENTS_SOLID, &tr);
		if (!tr.startsolid && tr.fraction < 1)
			return false;
	}
	return true;
}
//the solid angle a triangle subtends at the eye (Van Oosterom & Strackee)
static double GLBE_TriSolidAngle(const vec3_t p0, const vec3_t p1, const vec3_t p2)
{
	double a[3], b[3], c[3], x[3], la, lb, lc, num, den;
	int k;
	for (k = 0; k < 3; k++)
	{
		a[k] = p0[k] - r_refdef.vieworg[k];
		b[k] = p1[k] - r_refdef.vieworg[k];
		c[k] = p2[k] - r_refdef.vieworg[k];
	}
	x[0] = b[1]*c[2] - b[2]*c[1];
	x[1] = b[2]*c[0] - b[0]*c[2];
	x[2] = b[0]*c[1] - b[1]*c[0];
	la = sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
	lb = sqrt(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
	lc = sqrt(c[0]*c[0] + c[1]*c[1] + c[2]*c[2]);
	num = fabs(a[0]*x[0] + a[1]*x[1] + a[2]*x[2]);
	den = la*lb*lc + (a[0]*b[0] + a[1]*b[1] + a[2]*b[2])*lc + (a[0]*c[0] + a[1]*c[1] + a[2]*c[2])*lb +
		(b[0]*c[0] + b[1]*c[1] + b[2]*c[2])*la;
	return 2 * atan2(num, den);
}
//the solid angle of a face's part inside the view frustum -- the exact screen share, so a pool the eye
//floats over outweighs a smaller water farther off however its faces are cut -- and a probe point on that part:
//the solid-angle-weighted mean of its clipped triangles' vertex averages (not the true solid-angle centroid,
//but always on the visible part). Over the mesh's triangles (its indexes), so a displacement grid weighs too.
static double GLBE_FaceSolidAngle(const mesh_t *mesh, vec3_t centre)
{
	vec3_t buf[2][3 + MAXFRUSTUMPLANES + 1];	//a triangle clipped by each plane gains at most one vertex
	const int cap = sizeof(buf[0]) / sizeof(buf[0][0]);
	unsigned int t;
	int i, k, o, p, n, cur;
	double sum = 0, w;
	vec3_t c;
	VectorClear(centre);
	if (!mesh->indexes)
		return 0;
	for (t = 0; t + 2 < (unsigned int)mesh->numindexes; t += 3)
	{
		for (i = 0; i < 3; i++)
		{
			unsigned int v = mesh->indexes[t+i];
			if (v >= (unsigned int)mesh->numvertexes)
				break;
			VectorCopy(mesh->xyz_array[v], buf[0][i]);
		}
		if (i < 3)
			continue;
		for (n = 3, cur = 0, p = 0; p < r_refdef.frustum_numplanes && n >= 3; p++)
		{
			const mplane_t *pl = &r_refdef.frustum[p];
			vec3_t *in = buf[cur], *out = buf[cur^1];
			for (o = 0, i = 0; i < n; i++)
			{
				const float *a = in[i], *b = in[(i+1)%n];
				float da = DotProduct(a, pl->normal) - pl->dist, db = DotProduct(b, pl->normal) - pl->dist;
				if (da >= 0 && o < cap)
				{
					VectorCopy(a, out[o]);	//not out[o++]: the macro names its target three times
					o++;
				}
				if ((da >= 0) != (db >= 0) && o < cap)
				{
					float f = da / (da - db);
					for (k = 0; k < 3; k++)
						out[o][k] = a[k] + f * (b[k] - a[k]);
					o++;
				}
			}
			n = o;
			cur ^= 1;
		}
		if (n < 3)
			continue;
		for (w = 0, i = 1; i + 1 < n; i++)
			w += GLBE_TriSolidAngle(buf[cur][0], buf[cur][i], buf[cur][i+1]);
		if (w <= 0)
			continue;
		for (VectorClear(c), i = 0; i < n; i++)
			VectorAdd(c, buf[cur][i], c);
		VectorMA(centre, w / n, c, centre);
		sum += w;
	}
	if (sum > 0)
		VectorScale(centre, 1 / sum, centre);
	return sum;
}
//the on-screen test both the weight and the capture skip use: some face's clipped polygon is not empty
//(the walk culls per node, so a node's off-screen faces are listed)
static qboolean GLBE_WaterOnScreen(const batch_t *b)
{
	unsigned int m;
	vec3_t lo, hi, c;
	for (m = b->firstmesh; m < b->meshes; m++)
	{
		const mesh_t *mesh = b->mesh[m];
		if (!mesh || !mesh->xyz_array || mesh->numvertexes < 3)
			continue;
		GLBE_MeshBox(mesh, lo, hi);
		if (!R_CullBox(lo, hi) && GLBE_FaceSolidAngle(mesh, c) > 0)
			return true;
	}
	return false;
}
typedef struct
{
	plane_t p;
	float w;				//summed solid angle of its meshes inside the frustum
	batch_t *b;				//the batch of its heaviest mesh
	float bw;
	vec3_t probe[BE_WATERPROBES];	//its heaviest faces' on-screen centres, just off the surface on the eye's side
	float pw[BE_WATERPROBES];
	qboolean traced, seen;
} bewaterplane_t;
//weigh one batch's on-screen faces into a plane's slot
static float GLBE_WaterWeigh(batch_t *b, const plane_t *p, bewaterplane_t *slot)
{
	float sum = 0, w;
	unsigned int m;
	int t;
	for (m = b->firstmesh; m < b->meshes; m++)
	{
		mesh_t *mesh = b->mesh[m];
		vec3_t lo, hi, c;
		if (!mesh || !mesh->xyz_array || mesh->numvertexes < 3)
			continue;
		GLBE_MeshBox(mesh, lo, hi);
		if (R_CullBox(lo, hi))
			continue;
		w = GLBE_FaceSolidAngle(mesh, c);
		if (w <= 0)
			continue;
		sum += w;
		for (t = 0; t < BE_WATERPROBES; t++)	//keep the heaviest few as trace targets
			if (w > slot->pw[t])
			{
				int s;
				for (s = BE_WATERPROBES-1; s > t; s--)
				{
					slot->pw[s] = slot->pw[s-1];
					VectorCopy(slot->probe[s-1], slot->probe[s]);
				}
				slot->pw[t] = w;
				VectorMA(c, 2, p->normal, slot->probe[t]);
				break;
			}
		if (w > slot->bw)
		{
			slot->bw = w;
			slot->b = b;
		}
	}
	return sum;
}
static qboolean GLBE_WaterPlaneSeen(bewaterplane_t *slot)
{
	int s;
	if (!slot->traced)
	{
		slot->traced = true;
		for (slot->seen = false, s = 0; s < BE_WATERPROBES && slot->pw[s] > 0 && !slot->seen; s++)
			slot->seen = GLBE_WaterSeen(slot->probe[s]);
	}
	return slot->seen;
}
//one batch over every candidate on `pl` -- `first`'s meshes first (GLR_PortalPlane and the reflection's
//pvsorigin read the first one), then that plane's other on-screen batches -- and with `all`, after them
//every other on-screen candidate facing the eye, so the capture's PVS covers the water that samples it
//(as one load-merged batch's did: Mod_Batches_Build splits by plane only if the shader captured at load)
static void GLBE_WaterGather(bewaterview_t *wv, batch_t *first, const plane_t *pl, qboolean all,
	batch_t *out, mesh_t ***arr, unsigned int *max, unsigned int *need)
{
	unsigned int n = 0, k;
	int i, pass;
	batch_t *b;
	plane_t p;
	qboolean same;
	for (pass = 0; pass < 3; pass++)
		for (i = 0; i < SHADER_SORT_COUNT; i++)
			for (b = wv->worldbatches[i]; b; b = b->next)
			{
				if ((pass == 0) != GLBE_WaterSameBatch(b, first) || !GLBE_WaterCandidate(b) || !GLR_BatchPlane(b, &p))
					continue;
				same = GLBE_WaterSamePlane(&p, pl);
				if ((pass == 1 && !same) || (pass == 2 && (same || !all || !GLBE_WaterFacesEye(&p, wv->under))))
					continue;
				if (pass && !GLBE_WaterOnScreen(b))
					continue;	//never drawn this view (GLBE_GenerateBatchTextures skips it)
				if (same)
					*need |= b->shader->flags & BE_WATERKINDS;
				if (n + (b->meshes - b->firstmesh) > *max)
				{
					*max = n + (b->meshes - b->firstmesh) + 64;
					*arr = BZ_Realloc(*arr, *max * sizeof(**arr));
				}
				for (k = b->firstmesh; k < b->meshes; k++)
					(*arr)[n++] = b->mesh[k];
			}
	*out = *first;
	out->next = NULL;
	out->mesh = *arr;
	out->firstmesh = 0;
	out->meshes = out->maxmeshes = n;
}
//before this view submits anything: pick its water
static void GLBE_WaterViewsBegin(batch_t **worldbatches)
{
	int level = r_refdef.recurse, i, n = 0, s, t, best, hidden = -1, kept = 0, seat = 0;
	bewaterview_t *wv;
	batch_t *b;
	plane_t p;
	bewaterplane_t planes[BE_WATERPLANES], probe, *slot;
	float w;

	if (level >= R_MAX_RECURSE)
		return;
	wv = &be_waterview[level];
	wv->chosen = NULL;
	wv->need = wv->needall = 0;
	wv->helddone = 0;
	if (r_refdef.playerview && r_refdef.playerview >= cl.playerview && r_refdef.playerview < cl.playerview + MAX_SPLITS)
		seat = r_refdef.playerview - cl.playerview;
	//nothing here can capture: past the recursion limit, or inside a water capture (Patch 864). A view
	//without the world (the avatar card's renderscene) leaves the seat's hold alone.
	if (!r_waterviews.ival || !worldbatches || level >= r_portalrecursion.ival || be_watercapture)
	{
		if (!level && worldbatches)
			be_waterlastvalid[seat] = false;
		return;
	}
	VectorCopy(r_refdef.vieworg, wv->eye);
	wv->worldbatches = worldbatches;
	//each view's own eye (r_viewcontents is the top-level one's): a portal's far camera, a sky room's
	wv->under = cl.worldmodel && cl.worldmodel->funcs.PointContents &&
		(cl.worldmodel->funcs.PointContents(cl.worldmodel, NULL, r_refdef.vieworg) & (FTECONTENTS_WATER|FTECONTENTS_SLIME|FTECONTENTS_LAVA));
	for (i = 0; i < SHADER_SORT_COUNT; i++)
		for (b = worldbatches[i]; b; b = b->next)
		{
			if (!GLBE_WaterCandidate(b) || !GLR_BatchPlane(b, &p) || !GLBE_WaterFacesEye(&p, wv->under))
				continue;
			for (s = 0; s < n; s++)
				if (GLBE_WaterSamePlane(&planes[s].p, &p))
					break;
			if (s < n)
				w = GLBE_WaterWeigh(b, &p, &planes[s]);
			else
			{	//a slot only for a plane with water on screen
				memset(&probe, 0, sizeof(probe));
				probe.p = p;
				w = GLBE_WaterWeigh(b, &p, &probe);
				if (w <= 0 || n == BE_WATERPLANES)
					continue;
				planes[s = n++] = probe;
			}
			if (w > 0)
			{
				planes[s].w += w;
				wv->needall |= b->shader->flags & BE_WATERKINDS;	//only on-screen water asks
			}
		}
	//the heaviest plane whose water the eye can see past the world's solids (Source picks the eye's fog
	//volume engine-side; vvis only flags it, vvis.cpp:803-860): a pool behind a wall must not set every
	//other water's mirror plane. None seen: the heaviest anyway.
	for (best = -1, t = 0; t < 4; t++)
	{
		int top = -1;
		for (s = 0; s < n; s++)
			if (planes[s].w > 0 && (top < 0 || planes[s].w > planes[top].w))
				top = s;
		if (top < 0)
			break;
		if (best < 0)
			best = top;
		if (GLBE_WaterPlaneSeen(&planes[top]))
		{
			if (best != top)
				hidden = best;
			best = top;
			break;
		}
		planes[top].w = -planes[top].w;	//tried
	}
	if (best >= 0 && !level && be_waterlastvalid[seat])
	{	//the top-level view keeps last frame's water while it still has half the best's share and is seen,
		//so faces cut across a pool or a seam cannot flip the mirror plane frame to frame
		for (s = 0; s < n; s++)
			if (s != best && GLBE_WaterSamePlane(&planes[s].p, &be_waterlast[seat]))
			{
				if (fabs(planes[s].w) >= 0.5f * fabs(planes[best].w) && GLBE_WaterPlaneSeen(&planes[s]))
				{
					best = s;
					kept = 1;
				}
				break;
			}
	}
	if (!level)
	{
		be_waterlastvalid[seat] = best >= 0;
		if (best >= 0)
			be_waterlast[seat] = planes[best].p;
	}
	if (best < 0)
		return;
	slot = &planes[best];
	if (GLBE_CaptureLogging())
		Con_Printf("[capture]  level %i choose %.3f %.3f %.3f %.2f weight %g%s%s of %i planes%s\n", level,
			slot->p.normal[0], slot->p.normal[1], slot->p.normal[2], slot->p.dist, fabs(slot->w),
			kept ? " (kept)" : "", wv->under ? " (under water)" : "", n,
			hidden >= 0 ? va(", hidden %.3f %.3f %.3f %.2f weight %g", planes[hidden].p.normal[0], planes[hidden].p.normal[1],
				planes[hidden].p.normal[2], planes[hidden].p.dist, fabs(planes[hidden].w)) : "");
	wv->chosen = slot->b;
	wv->plane = slot->p;
	GLBE_WaterGather(wv, slot->b, &slot->p, r_waterviews.ival == 1, &wv->combined, &wv->mesh, &wv->maxmesh, &wv->need);
}
//the end of the view that chose: nothing after it may share its targets
static void GLBE_WaterViewsEnd(void)
{
	if (r_refdef.recurse < R_MAX_RECURSE)
	{
		be_waterview[r_refdef.recurse].chosen = NULL;
		be_waterview[r_refdef.recurse].helddone = 0;
	}
}

static qboolean GLBE_GenerateBatchTextures(batch_t *batch, shader_t *bs)
{
	int oldfbo;
	float oldil;
	int oldbem;
	unsigned int cf = bs->flags;	//FTESurf Patch 879: the kinds this call captures,
	shader_t *cs = bs;				//the shader whose scale and $reflectentities decide them,
	batch_t *cb = batch;			//and the batch whose meshes place them
	qboolean viashare = false;
	if (r_refdef.recurse >= r_portalrecursion.ival || r_refdef.recurse == R_MAX_RECURSE)
		return false;
	//FTESurf Patch 864: no water capture inside a water capture.  r_portalrecursion 2
	//(kitsune's doors) let surf_garden's ponds render 1+2+4 views facing the pond,
	//6.2 ms against 3.0 at 1.  Source has one reflection target, so it never nests
	//either.  The skipped water is what the recursion limit already skips.
	if (!r_refdef.recurse)
		be_watercapture = 0;	//the main view: nothing left counted by an aborted frame
	if (be_watercapture && !(bs->flags & SHADER_HASPORTAL))
		return false;
	//these flags require rendering some view as an fbo
	//(BEM_DEPTHDARK is used when lightmap scale is 0, but still shows any emissive stuff)
	if (shaderstate.mode != BEM_STANDARD && shaderstate.mode != BEM_DEPTHDARK)
		return false;

	/* FTESurf Patch 210: the budget applies here too.

	   Patch 206 capped portal views at r_portalmaxviews because surf_tripportals
	   has 116 doors and each one is a whole scene.  That cap lived entirely in
	   GLBE_SubmitMeshesPortals, which a self-painting portal no longer goes
	   through -- so without this line the FBO route would quietly reintroduce the
	   unbounded cost the cap exists to prevent, on the one map that proves it.

	   The table was built for this level moments ago, by GLR_PortalBudgetBegin at
	   the top of GLBE_SubmitMeshesPortals: GLBE_SubmitMeshes calls that for
	   SHADER_SORT_PORTAL and only then submits the same sort list, so the scores
	   are current and were measured under this view.  The two recursion guards
	   above run FIRST because past r_portalrecursion the table for this level was
	   never rebuilt and would answer from a different subtree.

	   Returning false means the caller skips the batch entirely, which for an
	   opaque aperture is exactly Patch 207's "renders closed": nothing is drawn,
	   so whatever the doorway is set into survives. */
	if ((bs->flags & SHADER_HASPORTAL) && !GLR_PortalWouldDraw(batch))
		return false;

	//FTESurf Patch 879: the view's water plane is captured once (GLBE_WaterViewsBegin). Below
	//GLR_DrawPortal's out-of-recursion path, which would submit the combined batch itself.
	if (r_waterviews.ival && r_refdef.recurse < R_MAX_RECURSE-1 && GLBE_WaterCandidate(batch))
	{
		bewaterview_t *wv = &be_waterview[r_refdef.recurse];
		plane_t p;
		unsigned int need;
		qboolean chosenplane;
		//nothing of it on screen (the walk lists a node's off-screen faces): no capture, nothing to draw
		if (!GLBE_WaterOnScreen(batch))
			return false;
		if (wv->chosen && VectorCompare(wv->eye, r_refdef.vieworg) && GLR_BatchPlane(batch, &p) && GLBE_WaterFacesEye(&p, wv->under))
		{
			chosenplane = GLBE_WaterSamePlane(&p, &wv->plane) || r_waterviews.ival == 1;
			if (chosenplane)	//at 1 every water facing the eye samples the chosen plane's capture, as in Source
			{
				p = wv->plane;
				need = ((r_waterviews.ival == 1) ? wv->needall : wv->need) | (bs->flags & BE_WATERKINDS);
			}
			else	//2, another plane: its own capture, over all of its batches
				need = bs->flags & BE_WATERKINDS;
			if (!wv->helddone || !GLBE_WaterSamePlane(&p, &wv->heldplane))
			{
				wv->heldplane = p;
				wv->helddone = 0;
			}
			if (!(need & ~wv->helddone))
			{
				GLBE_CaptureLog(batch, chosenplane ? wv->chosen->shader : bs, "shared", chosenplane ? &wv->combined : &wv->other);
				return true;
			}
			if (chosenplane)
			{
				cs = wv->chosen->shader;
				cb = &wv->combined;
			}
			else
			{
				GLBE_WaterGather(wv, batch, &p, false, &wv->other, &wv->omesh, &wv->maxomesh, &need);
				cb = &wv->other;
			}
			need &= ~wv->helddone;
			wv->helddone |= need;
			cf = (bs->flags & ~BE_WATERKINDS) | need;
			viashare = true;
		}
	}
	//anything else captured here overwrites those kinds of the level's targets (a portal's refraction too)
	if (!viashare)
		be_waterview[r_refdef.recurse].helddone &= ~(bs->flags & BE_WATERKINDS);

	oldbem = shaderstate.mode;
	oldil = shaderstate.identitylighting;

	if ((cf & SHADER_HASREFLECT) && gl_config.ext_framebuffer_objects)
	{
		float renderscale = cs->portalfboscale;
		vrect_t orect = r_refdef.vrect;
		pxrect_t oprect = r_refdef.pxrect;
		if (!shaderstate.tex_reflection[r_refdef.recurse])
		{
			shaderstate.tex_reflection[r_refdef.recurse] = Image_CreateTexture("***tex_reflection***", NULL, 0);
			if (!shaderstate.tex_reflection[r_refdef.recurse]->num)
				qglGenTextures(1, &shaderstate.tex_reflection[r_refdef.recurse]->num);
		}

		r_refdef.vrect.x = 0;
		r_refdef.vrect.y = 0;
		r_refdef.vrect.width = max(1, vid.fbvwidth * renderscale);
		r_refdef.vrect.height = max(1, vid.fbvheight * renderscale);
		r_refdef.pxrect.x = 0;
		r_refdef.pxrect.y = 0;
		r_refdef.pxrect.width = max(1, vid.fbpwidth * renderscale);
		r_refdef.pxrect.height = max(1, vid.fbpheight * renderscale);
		if (shaderstate.tex_reflection[r_refdef.recurse]->width!=r_refdef.pxrect.width || shaderstate.tex_reflection[r_refdef.recurse]->height!=r_refdef.pxrect.height)
		{
			shaderstate.tex_reflection[r_refdef.recurse]->width = r_refdef.pxrect.width;
			shaderstate.tex_reflection[r_refdef.recurse]->height = r_refdef.pxrect.height;
			GL_MTBind(0, GL_TEXTURE_2D, shaderstate.tex_reflection[r_refdef.recurse]);

			if ((vid.flags&VID_FP16) && sh_config.texfmt[PTI_RGBA16F])
				qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, shaderstate.tex_reflection[r_refdef.recurse]->width, shaderstate.tex_reflection[r_refdef.recurse]->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			else if ((vid.flags&(VID_SRGBAWARE|VID_FP16)) && sh_config.texfmt[PTI_RGBA8_SRGB])
				qglTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8_EXT, shaderstate.tex_reflection[r_refdef.recurse]->width, shaderstate.tex_reflection[r_refdef.recurse]->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			else
				qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, shaderstate.tex_reflection[r_refdef.recurse]->width, shaderstate.tex_reflection[r_refdef.recurse]->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		oldfbo = GLBE_FBO_Update(&shaderstate.fbo_reflectrefrac[r_refdef.recurse], FBO_RB_DEPTH, &shaderstate.tex_reflection[r_refdef.recurse], 1, r_nulltex, shaderstate.tex_reflection[r_refdef.recurse]->width, shaderstate.tex_reflection[r_refdef.recurse]->height, 0);
		r_refdef.pxrect.maxheight = shaderstate.fbo_reflectrefrac[r_refdef.recurse].rb_size[1];
		GL_ViewportUpdate();
		GL_ForceDepthWritable();
		qglClearColor(0, 0, 0, 1);
		qglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		GLBE_CaptureLog(batch, cs, "reflect", cb);
		be_watercapture++;
		r_capturenoents = cs->reflectnoents && !r_waterforcereflectentities.ival;	//Patch 868
		r_capturemergevis = cb != batch;
		GLR_DrawPortal(cb, cl.worldmodel->batches, NULL, 1);
		r_capturemergevis = false;
		r_capturenoents = 0;
		be_watercapture--;
		GLBE_FBO_Pop(oldfbo);
		r_refdef.vrect = orect;
		r_refdef.pxrect = oprect;
		GL_ViewportUpdate();
	}
	if (cf & (SHADER_HASREFRACT|SHADER_HASREFRACTDEPTH))
	{
		// A refraction depth attachment requires an FBO; screen-copy mode has none.
		if (r_refract_fboival || (cf&(SHADER_HASPORTAL|SHADER_HASREFRACTDEPTH)))
		{
			float renderscale = min(1, cs->portalfboscale);
			vrect_t ovrect = r_refdef.vrect;
			pxrect_t oprect = r_refdef.pxrect;
			r_refdef.vrect.x = 0;
			r_refdef.vrect.y = 0;
			r_refdef.vrect.width = max(1, vid.fbvwidth * renderscale);
			r_refdef.vrect.height = max(1, vid.fbvheight * renderscale);
			r_refdef.pxrect.x = 0;
			r_refdef.pxrect.y = 0;
			r_refdef.pxrect.width = max(1, vid.fbpwidth * renderscale);
			r_refdef.pxrect.height = max(1, vid.fbpheight * renderscale);

			if (!shaderstate.tex_refraction[r_refdef.recurse])
			{
				shaderstate.tex_refraction[r_refdef.recurse] = Image_CreateTexture("***tex_refraction***", NULL, 0);
				if (!shaderstate.tex_refraction[r_refdef.recurse]->num)
					qglGenTextures(1, &shaderstate.tex_refraction[r_refdef.recurse]->num);
			}
			if (shaderstate.tex_refraction[r_refdef.recurse]->width != r_refdef.pxrect.width || shaderstate.tex_refraction[r_refdef.recurse]->height != r_refdef.pxrect.height)
			{
				shaderstate.tex_refraction[r_refdef.recurse]->width = r_refdef.pxrect.width;
				shaderstate.tex_refraction[r_refdef.recurse]->height = r_refdef.pxrect.height;
				GL_MTBind(0, GL_TEXTURE_2D, shaderstate.tex_refraction[r_refdef.recurse]);
				if ((vid.flags&VID_FP16) && sh_config.texfmt[PTI_RGBA16F])
					qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, r_refdef.pxrect.width, r_refdef.pxrect.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
				else if ((vid.flags&(VID_SRGBAWARE|VID_FP16)) && sh_config.texfmt[PTI_RGBA16F])
					qglTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8_EXT, r_refdef.pxrect.width, r_refdef.pxrect.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
				else
					qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, r_refdef.pxrect.width, r_refdef.pxrect.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}
			if (cf & SHADER_HASREFRACTDEPTH)
			{
				if (!shaderstate.tex_refractiondepth[r_refdef.recurse])
				{
					shaderstate.tex_refractiondepth[r_refdef.recurse] = Image_CreateTexture("***tex_refractiondepth***", NULL, 0);
					if (!shaderstate.tex_refractiondepth[r_refdef.recurse]->num)
						qglGenTextures(1, &shaderstate.tex_refractiondepth[r_refdef.recurse]->num);
				}
				if (shaderstate.tex_refractiondepth[r_refdef.recurse]->width != r_refdef.pxrect.width || shaderstate.tex_refractiondepth[r_refdef.recurse]->height != r_refdef.pxrect.height)
				{
					shaderstate.tex_refractiondepth[r_refdef.recurse]->width = r_refdef.pxrect.width;
					shaderstate.tex_refractiondepth[r_refdef.recurse]->height = r_refdef.pxrect.height;
					GL_MTBind(0, GL_TEXTURE_2D, shaderstate.tex_refractiondepth[r_refdef.recurse]);
					qglTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24_ARB, r_refdef.pxrect.width, r_refdef.pxrect.height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, NULL);
					qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
					qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
					qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				}
				oldfbo = GLBE_FBO_Update(&shaderstate.fbo_reflectrefrac[r_refdef.recurse], FBO_TEX_DEPTH, &shaderstate.tex_refraction[r_refdef.recurse], 1, shaderstate.tex_refractiondepth[r_refdef.recurse], r_refdef.pxrect.width, r_refdef.pxrect.height, 0);
			}
			else
			{
				oldfbo = GLBE_FBO_Update(&shaderstate.fbo_reflectrefrac[r_refdef.recurse], FBO_RB_DEPTH, &shaderstate.tex_refraction[r_refdef.recurse], 1, r_nulltex, r_refdef.pxrect.width, r_refdef.pxrect.height, 0);
			}
			r_refdef.pxrect.maxheight = shaderstate.fbo_reflectrefrac[r_refdef.recurse].rb_size[1];
			GL_ViewportUpdate();

			GL_ForceDepthWritable();
			qglClearColor(0, 0, 0, 1);
			qglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			if (bs->flags&SHADER_HASPORTAL)
			{
				/* FTESurf: SAY WHETHER THE PORTAL ACTUALLY DREW.

				   The FBO was just cleared to opaque black three lines up, and this
				   call's return value was thrown away -- so every one of
				   GLR_DrawPortal's six refusal paths ends with the quad faithfully
				   painting that black clear onto the doorway.  "The portal renders
				   black" and "the portal was refused and you are looking at the
				   clear colour" are the same pixels and completely different bugs,
				   and telling them apart by screenshot is not possible: it cost this
				   investigation a run each on vis, areas, the skyroom, standoff
				   distance and lightmaps before anyone asked the function.

				   Counter-limited rather than rate-limited: this is a per-frame call
				   site and the interesting information is all in the first few. */
				qboolean drew = GLR_DrawPortal(batch, cl.worldmodel->batches, NULL, 0);
				static int portalreports = 0;
				/* developer.ival is tested HERE and not left to Con_DPrintf.  Only the
				   PRINT is developer-gated; qglReadPixels below is not, and a
				   synchronous readback stalls the pipeline -- shipping twelve of those
				   per map load for every player, to fill a log nobody asked for, is
				   the kind of diagnostic cost Patch 209a was written about. */
				if (portalreports < 12 && developer.ival)
				{
					/* READ THE FBO BACK.  The queued-mesh count in r_surf.c says the
					   recursed view emits 6222 meshes, which is what it should -- but
					   emitted is not drawn, and the doorway is still black.  Those two
					   facts can only be reconciled by looking at the pixels the
					   recursion actually produced, and the FBO is still bound here
					   (GLBE_FBO_Pop is below), so a readback costs one stall on a
					   frame that is already being instrumented.
					   Centre 8x8: if this is ~0 the recursion drew nothing visible and
					   the shader is innocent; if it is bright the image exists and the
					   fault is downstream in the sample. */
					GLubyte px[8*8*4];
					int i, sum = 0, mx = 0;
					qglReadPixels(r_refdef.pxrect.width/2 - 4, r_refdef.pxrect.height/2 - 4,
						8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px);
					for (i = 0; i < 8*8*4; i++)
					{
						sum += px[i];
						if (px[i] > mx)
							mx = px[i];
					}
					portalreports++;
					Con_DPrintf("[portal] recurse %i: GLR_DrawPortal %s (fbo %ix%i, meshes %u..%u) fbo centre mean %i max %i\n",
						r_refdef.recurse, drew?"DREW":"REFUSED",
						shaderstate.tex_refraction[r_refdef.recurse]->width,
						shaderstate.tex_refraction[r_refdef.recurse]->height,
						(unsigned)batch->firstmesh, (unsigned)batch->meshes,
						sum/(8*8*4), mx);
				}
			}
			else
			{
				GLBE_CaptureLog(batch, cs, (cf & SHADER_HASREFRACTDEPTH)?"refract+depth":"refract", cb);
				be_watercapture++;
				GLR_DrawPortal(cb, cl.worldmodel->batches, NULL, ((cf & SHADER_HASREFRACTDEPTH)?3:2));	//fixme
				be_watercapture--;
			}
			GLBE_FBO_Pop(oldfbo);

			r_refdef.vrect = ovrect;
			r_refdef.pxrect = oprect;
			GL_ViewportUpdate();
		}
		else
		{
			GLBE_CaptureLog(batch, cs, "refractcopy", cb);
			be_watercapture++;	//a portal never gets here: it forces the FBO branch
			GLR_DrawPortal(cb, cl.worldmodel->batches, NULL, 3);
			be_watercapture--;
		}
	}
	if ((bs->flags & SHADER_HASRIPPLEMAP) && gl_config.ext_framebuffer_objects)
	{
		float renderscale = bs->portalfboscale;
		vrect_t orect = r_refdef.vrect;
		pxrect_t oprect = r_refdef.pxrect;
		r_refdef.vrect.x = 0;
		r_refdef.vrect.y = 0;
		r_refdef.vrect.width = max(1, vid.fbvwidth * renderscale);
		r_refdef.vrect.height = max(1, vid.fbvheight * renderscale);
		r_refdef.pxrect.x = 0;
		r_refdef.pxrect.y = 0;
		r_refdef.pxrect.width = max(1, vid.fbpwidth * renderscale);
		r_refdef.pxrect.height = max(1, vid.fbpheight * renderscale);

		if (!shaderstate.tex_ripplemap[r_refdef.recurse])
		{
			//FIXME: can we use RGB8 instead?
			shaderstate.tex_ripplemap[r_refdef.recurse] = Image_CreateTexture("***tex_ripplemap***", NULL, 0);
			if (!shaderstate.tex_ripplemap[r_refdef.recurse]->num)
				qglGenTextures(1, &shaderstate.tex_ripplemap[r_refdef.recurse]->num);
		}
		if (shaderstate.tex_ripplemap[r_refdef.recurse]->width != r_refdef.pxrect.width || shaderstate.tex_ripplemap[r_refdef.recurse]->height != r_refdef.pxrect.height)
		{
			shaderstate.tex_ripplemap[r_refdef.recurse]->width = r_refdef.pxrect.width;
			shaderstate.tex_ripplemap[r_refdef.recurse]->height = r_refdef.pxrect.height;
			GL_MTBind(0, GL_TEXTURE_2D, shaderstate.tex_ripplemap[r_refdef.recurse]);
			qglTexImage2D(GL_TEXTURE_2D, 0, /*(gl_config.glversion>3.1)?GL_RGBA8_SNORM:*/GL_RGBA16F, r_refdef.pxrect.width, r_refdef.pxrect.height, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		oldfbo = GLBE_FBO_Update(&shaderstate.fbo_reflectrefrac[r_refdef.recurse], 0, &shaderstate.tex_ripplemap[r_refdef.recurse], 1, r_nulltex, r_refdef.pxrect.width, r_refdef.pxrect.height, 0);
		r_refdef.pxrect.maxheight = shaderstate.fbo_reflectrefrac[r_refdef.recurse].rb_size[1];
		GL_ViewportUpdate();

		qglClearColor(0, 0, 0, 1);
		qglClear(GL_COLOR_BUFFER_BIT);

//		r_refdef.waterheight = DotProduct(batch->mesh[0]->xyz_array[0], batch->mesh[0]->normals_array[0]);

		r_refdef.recurse+=1; //paranoid, should stop potential infinite loops
		GLBE_SubmitMeshes(cl.worldmodel->batches, SHADER_SORT_RIPPLE, SHADER_SORT_RIPPLE);
		r_refdef.recurse-=1;
		GLBE_FBO_Pop(oldfbo);

		r_refdef.vrect = orect;
		r_refdef.pxrect = oprect;
		GL_ViewportUpdate();
	}
	BE_SelectMode(oldbem);
	shaderstate.identitylighting = oldil;
	return true;
}
/*
FTESurf Patch 831 -- INSTANCED PROPS.  After r_batchsort, a run of alias batches with the same
shader, model, surface, skin, envmap, fog, flags and shader time differs only in what the shader
reads per entity: matrices, eye position, lighting, colour and the baked vertex colours.  Those
go into a texture buffer, INST_TEXELS texels a prop, and the run is ONE draw of vertexlit.glsl's
INSTANCED permutation.  The matrices are GLBE_SelectEntity's and the model-view-projection is
multiplied as SP_M_MODELVIEWPROJECTION does, so a prop lands on the same pixels either way.
Texels: 0-3 the model-view-projection's columns, 4-6 the model matrix's first three columns,
7 eye position in model space + the colours' first texel, 8 ambient, 9 light direction,
10 light multiplier, 11 colourident, 12-17 the ambient cube.
*/
static GLuint inst_buf, inst_tex;
static size_t inst_bufsize, inst_bufused;	//bytes
static float *inst_data;
static size_t inst_cap;					//floats
static qboolean inst_broken;			//a program had no INSTANCED permutation: off until a restart
static int inst_report = -1;			//host_framecount whose runs print (r_instancing 2)
static void GLBE_InstanceReset(void)
{	//the context, and so its objects, are new (GLBE_Init)
	inst_buf = inst_tex = 0;
	inst_bufsize = inst_bufused = 0;
	inst_broken = false;
}
extern int host_framecount;
GLuint VertLightPool_Texture(void);
int R_GAlias_InstanceColours(batch_t *b);

//every program the standard path will draw this shader with (DrawMeshes: the shader's, else each
//merged pass's) has an INSTANCED permutation
static qboolean GLBE_InstanceShader(const shader_t *s)
{
	int passno;
	if (s->remapto)
		s = s->remapto;	//what DrawMeshes draws
	//the walker's own special cases (skipped, or captures made before the submit) stay with it
	if (s->flags & (SHADER_NODRAW|SHADER_SKY|SHADER_HASREFLECT|SHADER_HASREFRACT|SHADER_HASRIPPLEMAP|SHADER_HASPORTAL))
		return false;
	if (s->prog)
		return !!(s->prog->supportedpermutations & PERMUTATION_INSTANCED);
	if (!s->numpasses)
		return false;
	for (passno = 0; passno < s->numpasses; passno += max(1, s->passes[passno].numMergedPasses))
		if (!s->passes[passno].prog || !(s->passes[passno].prog->supportedpermutations & PERMUTATION_INSTANCED))
			return false;
	return true;
}
static qboolean GLBE_InstanceCandidate(const batch_t *b)
{
	return b->buildmeshes == R_GAlias_DrawBatch && b->ent && b->shader && !b->fog && GLBE_InstanceShader(b->shader) &&
		!(b->ent->flags & (RF_DEPTHHACK|RF_XFLIP));
}
static qboolean GLBE_InstanceSame(const batch_t *a, const batch_t *b)
{
	return b->buildmeshes == a->buildmeshes && b->shader == a->shader && b->ent->model == a->ent->model &&
		b->user.alias.surfrefs[0] == a->user.alias.surfrefs[0] && b->skin == a->skin &&
		b->envmap == a->envmap && b->fog == a->fog && b->flags == a->flags &&
		b->ent->shaderTime == a->ent->shaderTime && !(b->ent->flags & (RF_DEPTHHACK|RF_XFLIP));
}
//the program has just been selected for a run's batch: hand it the two buffers and its base
static void GLBE_BindInstanceData(const struct programpermu_s *permu)
{
	GL_SelectTexture(INST_TMU_DATA);
	qglBindTexture(GL_TEXTURE_BUFFER_FTE, inst_tex);
	GL_SelectTexture(INST_TMU_VC);
	qglBindTexture(GL_TEXTURE_BUFFER_FTE, VertLightPool_Texture());
	qglUniform1iARB(permu->instuniform[0], INST_TMU_DATA);
	qglUniform1iARB(permu->instuniform[1], INST_TMU_VC);
	qglUniform1iARB(permu->instuniform[2], be_instancebase);
}
//how many of the count batches from first it drew as one: 2 or more, or 1 (first alone: the caller draws
//the rest), or 0 (none). *alone: batches from first the caller draws as usual without trying again.
static int GLBE_SubmitInstancedRun(batch_t *first, int count, int *alone)
{
	batch_t *b;
	entity_t *e;
	int i, vc;
	float *d, m[16], mv[16], inv[16];
	const float *proj, *cube;
	vec3_t eye;
	size_t bytes = (size_t)count*INST_TEXELS*16;

	if (inst_cap < (size_t)count*INST_TEXELS*4)
	{
		inst_cap = (size_t)count*INST_TEXELS*4*2;
		inst_data = BZ_Realloc(inst_data, inst_cap*sizeof(float));
	}
	//GLBE_SelectEntity's projection for an entity with neither flag
	proj = shaderstate.usingweaponviewmatrix ? r_refdef.m_projection_std : shaderstate.projectionmatrix;
	*alone = 0;
	for (i = 0, b = first; i < count; i++, b = b->next)
	{
		b->buildmeshes(b);
		vc = (b->meshes == 1) ? R_GAlias_InstanceColours(b) : -1;
		if (vc < 0)
		{
			if (inst_report == host_framecount)
				Con_Printf("r_instancing: %s: member %i of %i cannot join (meshes %u, bones %i, colours %p)\n", first->shader->name, i, count, b->meshes, b->vbo?b->vbo->numbones:-1, (void*)b->ent->vertlightbytes);
			if (i < 2)
			{
				*alone = i+1;
				return 0;
			}
			count = i;	//the members before it are a run of their own
			break;
		}
		e = b->ent;
		d = inst_data + (size_t)i*INST_TEXELS*4;
		R_RotateForEntity(m, mv, e, e->model);
		Matrix4_Multiply(proj, mv, d);
		d[16] = m[0]; d[17] = m[1]; d[18] = m[2]; d[19] = 0;
		d[20] = m[4]; d[21] = m[5]; d[22] = m[6]; d[23] = 0;
		d[24] = m[8]; d[25] = m[9]; d[26] = m[10]; d[27] = 0;
		Matrix4_Invert(m, inv);
		Matrix4x4_CM_Transform3(inv, r_origin, eye);
		VectorCopy(eye, d+28); d[31] = vc;
		VectorCopy(e->light_avg, d+32); d[35] = 0;
		VectorCopy(e->light_dir, d+36); d[39] = 0;
		VectorCopy(e->light_range, d+40); d[43] = 0;
		if (b->flags & BEF_FORCECOLOURMOD)
			Vector4Copy(e->shaderRGBAf, d+44);
		else
			Vector4Set(d+44, 1, 1, 1, e->shaderRGBAf[3]);
		cube = BE_EntityAmbientCube(e, false);	//the counters stay the single-draw uploads'
		for (vc = 0; vc < 6; vc++)
		{
			VectorCopy(cube+vc*3, d+48+vc*4);
			d[48+vc*4+3] = 0;
		}
	}
	bytes = (size_t)count*INST_TEXELS*16;
	//the shared alias mesh and colour slice now describe the last; make them the first's again
	first->buildmeshes(first);
	if (first->meshes != 1)
		return 0;

	if (!inst_buf)
	{
		qglGenBuffersARB(1, &inst_buf);
		qglGenTextures(1, &inst_tex);
	}
	qglBindBufferARB(GL_TEXTURE_BUFFER_FTE, inst_buf);
	if (inst_bufused + bytes > inst_bufsize)
	{	//a fresh store; the driver keeps the old one for draws still reading it
		inst_bufsize = max(inst_bufsize, max(bytes, (size_t)1<<20));
		qglBufferDataARB(GL_TEXTURE_BUFFER_FTE, inst_bufsize, NULL, GL_STREAM_DRAW_ARB);
		inst_bufused = 0;
		GL_SelectTexture(INST_TMU_DATA);
		qglBindTexture(GL_TEXTURE_BUFFER_FTE, inst_tex);
		qglTexBuffer(GL_TEXTURE_BUFFER_FTE, GL_RGBA32F, inst_buf);
	}
	qglBufferSubDataARB(GL_TEXTURE_BUFFER_FTE, inst_bufused, bytes, inst_data);
	qglBindBufferARB(GL_TEXTURE_BUFFER_FTE, 0);
	be_instancebase = (int)(inst_bufused / 16);
	inst_bufused += bytes;

	be_instancecount = count;
	be_instancefailed = false;
	GLBE_SubmitBatch(first);
	be_instancecount = 0;
	if (inst_report == host_framecount)
		Con_Printf("r_instancing: %s x%i%s\n", first->shader->name, count, be_instancefailed?" -- NO INSTANCED PERMUTATION, drawn one by one":"");
	if (be_instancefailed)
	{	//drawn as the first prop alone: the program could not take the run. Off until a restart.
		inst_broken = true;
		return 1;
	}
	return count;
}

static void GLBE_SubmitMeshesSortRange(batch_t *sortlist, batch_t *end)
{
	batch_t *batch, *n;
	shader_t *bs;
	int run, used, alone, noinst = 0;
	qboolean instancing = r_instancing.ival && !inst_broken && shaderstate.mode == BEM_STANDARD &&
		qglDrawElementsInstanced && qglTexBuffer && !r_fakeshadows && !Sh_AnyLightToDraw();
	int ncand = 0, nalias = 0, nruns = 0;
	if (r_instancing.ival == 2)
	{	//one frame's report, every view of it
		inst_report = host_framecount;
		Cvar_ForceSet(&r_instancing, "1");
		Con_Printf("r_instancing: gates: cvar %i broken %i mode %i (standard %i) DrawElementsInstanced %i TexBuffer %i fakeshadows %i lights %i\n",
			r_instancing.ival, inst_broken, shaderstate.mode, BEM_STANDARD, qglDrawElementsInstanced!=NULL, qglTexBuffer!=NULL, r_fakeshadows, Sh_AnyLightToDraw());
	}
	for (batch = sortlist; batch && batch != end; batch = batch->next)
	{
		if (batch->meshes == batch->firstmesh)
			continue;

		if (inst_report == host_framecount && batch->buildmeshes == R_GAlias_DrawBatch)
		{
			nalias++;
			if (GLBE_InstanceCandidate(batch))
				ncand++;
			else if (nalias <= 3)
				Con_Printf("r_instancing: not a candidate: %s prog %p supported %#x\n", batch->shader->name, (void*)batch->shader->prog, batch->shader->prog?batch->shader->prog->supportedpermutations:0);
		}
		if (noinst)
			noinst--;
		else if (instancing && !inst_broken && GLBE_InstanceCandidate(batch))
		{
			for (run = 1, n = batch->next; n && n != end && run < 4096 && GLBE_InstanceSame(batch, n); n = n->next)
				run++;
			nruns += run > 1;
			if (run > 1)
			{
				used = GLBE_SubmitInstancedRun(batch, run, &alone);
				if (used)
				{
					while (--used)
						batch = batch->next;
					continue;
				}
				noinst = alone-1;	//this one is drawn below
			}
		}

		if (batch->flags & BEF_NODLIGHT)
			if (shaderstate.mode == BEM_LIGHT)
				continue;
		if (batch->flags & BEF_NOSHADOWS)
			if (shaderstate.mode == BEM_STENCIL || shaderstate.mode == BEM_DEPTHONLY)	//fixme: depthonly is not just shadows.
				continue;

		//buildmeshes updates shaders and generates pose information for sufaces that need it.
		//the shader flags checked *after* this call may be a performance issue if it generated lots of new mesh data.
		//FIXME: should we assume that the batch's shader will have the same flags?
		if (batch->buildmeshes)
		{
			TRACE(("GLBE_SubmitMeshesSortList: build\n"));
			batch->buildmeshes(batch);
		}

		bs = batch->shader;

		TRACE(("GLBE_SubmitMeshesSortList: shader %s\n", bs->name));

		//FIXME:!!
		if (!bs)
		{
			Con_Printf("Shader not set...\n");
			if (batch->texture)
				bs = R_TextureAnimation(0, batch->texture)->shader;
			else
				continue;
		}

		if ((bs->flags & SHADER_NODRAW) || !batch->meshes)
			continue;
		if (bs->flags & SHADER_NODLIGHT)
			if (shaderstate.mode == BEM_LIGHT)
				continue;
		if (bs->flags & SHADER_NOSHADOWS)
			if (shaderstate.mode == BEM_STENCIL || shaderstate.mode == BEM_DEPTHONLY)	//fixme: depthonly is not just shadows.
				continue;
		if (bs->flags & SHADER_SKY)
		{
			if (shaderstate.mode == BEM_STANDARD || shaderstate.mode == BEM_DEPTHDARK)// || shaderstate.mode == BEM_WIREFRAME)
			{
				float il = shaderstate.identitylighting;	//this stuff sucks!
				if (R_DrawSkyChain(batch))
				{
					shaderstate.identitylighting = il;
					continue;
				}
				shaderstate.identitylighting = il;
			}
			else if (/*shaderstate.mode != BEM_FOG &&*/ shaderstate.mode != BEM_CREPUSCULAR && shaderstate.mode != BEM_WIREFRAME)
				continue;
		}

		if ((bs->flags & (SHADER_HASREFLECT | SHADER_HASREFRACT | SHADER_HASRIPPLEMAP)) && shaderstate.mode != BEM_WIREFRAME)
		{
			if (!GLBE_GenerateBatchTextures(batch, bs))
				continue;

			/* FTESurf Patch 220: REBUILD BEFORE SUBMITTING.  This is the black doorway.

			   GLBE_GenerateBatchTextures has just rendered one or more WHOLE SCENES --
			   GLR_DrawPortal -> R_RenderScene for a portal or a reflection, and a
			   nested GLBE_SubmitMeshes for a ripplemap.  R_GAlias_DrawBatch hands
			   every alias batch in the engine a pointer to ONE file-static mesh_t
			   (gl_alias.c:1934-1935, :1967) which describes THIS batch only between
			   its own build and its own submit, and every alias model in that
			   recursed view -- plus GLR_PortalBudgetBegin's own build loop
			   (gl_rmain.c:1481-1482) -- has since memset and refilled it.

			   So batch->mesh now carries a foreign model's numvertexes/numindexes/
			   vbofirstelement while GLBE_SubmitBatch still draws from THIS batch's
			   vbo.  For a linked_portal_door that is a prop's index count read out
			   of a four-vertex quad's element buffer: nothing usable rasterises, the
			   aperture is never painted, and what shows in the doorway is whatever
			   was behind it.  The far view in the FBO was correct the whole time,
			   which is exactly why measuring it proved nothing.

			   The discipline and the reason are already spelled out at :5994-5997
			   and gl_rmain.c:1968-1971 (Patch 203).  This was the one
			   submit-after-recursion path that never got it -- Patch 210 is the
			   first thing to route an ALIAS batch through here, and before it only
			   brush water/reflect surfaces arrived, whose meshes live in per-batch
			   storage (r_surf.c:2821-2829) rather than in a shared static. */
			if (batch->buildmeshes && r_portaldebug.ival != 4)
			{
				/* FTESurf Patch 220: SAY THAT IT HAPPENED, don't infer it from pixels.
				   "the doorway looks better now" is not evidence -- this map animates.
				   Sample the header the batch was ABOUT to be submitted with, then say
				   so when the rebuild changes it.  A material whose mesh survives the
				   recursion prints nothing, so this is self-limiting, and it is
				   counter-limited on top because it is a per-frame call site.
				   developer.ival is tested here rather than left to Con_DPrintf so the
				   three reads cost nothing when nobody is looking. */
				int owasi = 0, owasv = 0;
				unsigned int owase = 0;
				qboolean osampled = (developer.ival && batch->mesh &&
					batch->meshes != batch->firstmesh && batch->mesh[batch->firstmesh]);
				if (osampled)
				{
					owasi = batch->mesh[batch->firstmesh]->numindexes;
					owasv = batch->mesh[batch->firstmesh]->numvertexes;
					owase = batch->mesh[batch->firstmesh]->vbofirstelement;
				}

				batch->buildmeshes(batch);
				if (!batch->mesh || batch->meshes == batch->firstmesh)
					continue;

				if (osampled)
				{
					static int clobberreports = 0;
					mesh_t *nm = batch->mesh[batch->firstmesh];
					if (clobberreports < 12 && nm &&
						(owasi != nm->numindexes || owasv != nm->numvertexes || owase != nm->vbofirstelement))
					{
						clobberreports++;
						Con_DPrintf("[portal] %s: mesh clobbered by the recursed view -- was about to submit idx %i vert %i firstelem %u, rebuilt to idx %i vert %i firstelem %u\n",
							bs->name, owasi, owasv, owase,
							nm->numindexes, nm->numvertexes, nm->vbofirstelement);
					}
				}
			}

			if ((bs->flags&SHADER_HASPORTAL) && shaderstate.mode != BEM_DEPTHONLY && gl_config.arb_depth_clamp)
			{	//this little bit of code is meant to prevent issues when the near clip plane intersects the portal surface, allowing us to be that little bit closer to the portal.
				qglEnable(GL_DEPTH_CLAMP_ARB);
				GLBE_SubmitBatch(batch);
				qglDisable(GL_DEPTH_CLAMP_ARB);
				continue;
			}
		}

		GLBE_SubmitBatch(batch);
	}
	if (inst_report == host_framecount && nalias)
		Con_Printf("r_instancing: list: %i alias batches, %i candidates, %i runs of 2+\n", nalias, ncand, nruns);
}

static void GLBE_SubmitMeshesSortList(batch_t *sortlist)
{
	GLBE_SubmitMeshesSortRange(sortlist, NULL);
}

typedef struct
{
	batch_t *batch;
	mesh_t *mesh; // NULL replays the whole batch, building procedural meshes only then.
	double depth;
	size_t sequence;
	qboolean foreground;
} glblendface_t;

static qboolean gl_blendtrace_pending;
static qboolean gl_blendtrace_started;
static int gl_blendtrace_frame;
extern int host_framecount;

void GLBE_BlendTrace_f(void)
{
	if (Cmd_IsInsecure())
		return;
	if (qrenderer != QR_OPENGL || r_blendsort.ival != 2)
	{
		Con_Printf("ALPHATRACE requires OpenGL and r_blendsort 2\n");
		return;
	}
	gl_blendtrace_pending = true;
	gl_blendtrace_started = false;
	Con_Printf("ALPHATRACE armed for every view of the next frame drawn; close the console if needed\n");
}

// Use the same model matrix as GLBE_SelectEntity, including axis sign and scale.
static double GLBE_BlendCenterDepth(const double *center, const float *matrix)
{
	double depth = 0, world;
	int j;
	for (j = 0; j < 3; j++)
	{
		if (IS_NAN(matrix[j]) || IS_NAN(matrix[4+j]) ||
			IS_NAN(matrix[8+j]) || IS_NAN(matrix[12+j]) ||
			IS_NAN(r_origin[j]) || IS_NAN(vpn[j]))
			return 0;
		world = matrix[j]*center[0] + matrix[4+j]*center[1] +
			matrix[8+j]*center[2] + matrix[12+j];
		depth += (world - r_origin[j])*vpn[j];
	}
	return depth;
}

static double GLBE_BlendFaceDepth(const mesh_t *mesh, const float *matrix)
{
	double center[3] = {0, 0, 0};
	int v, j;
	if (!mesh->xyz_array || mesh->numvertexes <= 0)
		return 0;
	for (v = 0; v < mesh->numvertexes; v++)
		for (j = 0; j < 3; j++)
		{
			if (IS_NAN(mesh->xyz_array[v][j]))
				return 0;
			center[j] += mesh->xyz_array[v][j];
		}
	for (j = 0; j < 3; j++)
		center[j] /= mesh->numvertexes;
	return GLBE_BlendCenterDepth(center, matrix);
}

// Mode2: BSP edge splits must not change a face's sorting centre.
static double GLBE_BlendAreaDepth(const mesh_t *mesh, const float *matrix)
{
	double center[3] = {0, 0, 0}, total = 0;
	unsigned int a, b, c;
	int t, j;
	if (!mesh->xyz_array || mesh->numvertexes <= 0 || !mesh->indexes ||
		mesh->numindexes <= 0 || mesh->numindexes % 3)
		return GLBE_BlendFaceDepth(mesh, matrix);
	for (t = 0; t < mesh->numindexes; t += 3)
	{
		double ab[3], ac[3], cross[3], area;
		a = mesh->indexes[t]; b = mesh->indexes[t+1]; c = mesh->indexes[t+2];
		if (a >= (unsigned int)mesh->numvertexes || b >= (unsigned int)mesh->numvertexes ||
			c >= (unsigned int)mesh->numvertexes)
			return GLBE_BlendFaceDepth(mesh, matrix);
		for (j = 0; j < 3; j++)
		{
			if (IS_NAN(mesh->xyz_array[a][j]) || IS_NAN(mesh->xyz_array[b][j]) ||
				IS_NAN(mesh->xyz_array[c][j]))
				return GLBE_BlendFaceDepth(mesh, matrix);
			ab[j] = (double)mesh->xyz_array[b][j] - mesh->xyz_array[a][j];
			ac[j] = (double)mesh->xyz_array[c][j] - mesh->xyz_array[a][j];
		}
		cross[0] = ab[1]*ac[2] - ab[2]*ac[1];
		cross[1] = ab[2]*ac[0] - ab[0]*ac[2];
		cross[2] = ab[0]*ac[1] - ab[1]*ac[0];
		area = sqrt(cross[0]*cross[0] + cross[1]*cross[1] + cross[2]*cross[2]);
		if (area <= 0)
			continue;
		total += area;
		for (j = 0; j < 3; j++)
			center[j] += area * ((double)mesh->xyz_array[a][j] +
				mesh->xyz_array[b][j] + mesh->xyz_array[c][j]) / 3;
	}
	if (total <= 0)
		return GLBE_BlendFaceDepth(mesh, matrix);
	for (j = 0; j < 3; j++)
		center[j] /= total;
	return GLBE_BlendCenterDepth(center, matrix);
}

// Procedural commands must not build or retain the shared alias mesh until replay.
static double GLBE_BlendBatchDepth(const batch_t *batch, const float *matrix)
{
	double center[3] = {0, 0, 0}, depth = 0;
	unsigned int m, count = 0;
	int j;
	model_t *model = batch->ent->model;
	if (batch->buildmeshes == R_DB_Poly)
	{
		unsigned int surface = batch->user.poly.surface, first, num, v;
		// Scenetris use r_worldentity, but their live positions describe the command.
		if (!cl_stris || !cl_strisvertv || surface >= cl_numstris)
			return 0;
		if (cl_stris[surface].firstvert < 0 || cl_stris[surface].numvert <= 0)
			return 0;
		first = cl_stris[surface].firstvert;
		num = cl_stris[surface].numvert;
		if (first > cl_numstrisvert || num > cl_numstrisvert - first)
			return 0;
		for (v = 0; v < num; v++)
			for (j = 0; j < 3; j++)
			{
				if (IS_NAN(cl_strisvertv[first+v][j]))
					return 0;
				center[j] += cl_strisvertv[first+v][j];
			}
		for (j = 0; j < 3; j++)
			center[j] /= num;
		return GLBE_BlendCenterDepth(center, matrix);
	}
	if (!batch->buildmeshes && batch->mesh)
	{
		for (m = batch->firstmesh; m < batch->meshes; m++)
			if (batch->mesh[m] && batch->mesh[m]->xyz_array && batch->mesh[m]->numvertexes > 0)
			{
				depth += GLBE_BlendFaceDepth(batch->mesh[m], matrix);
				count++;
			}
		if (count)
			return depth / count;
	}
	if (model)
	{
		for (j = 0; j < 3; j++)
		{
			if (IS_NAN(model->mins[j]) || IS_NAN(model->maxs[j]))
				return 0;
			center[j] = ((double)model->mins[j] + model->maxs[j]) * 0.5;
		}
	}
	return GLBE_BlendCenterDepth(center, matrix);
}

// Keep stock CSQC wire annotations in the legacy post-world layer. Their
// centre depths are not glass-plane constraints; arbitrary shaders/models
// remain conservative barriers. Opaque depth testing is unchanged.
static qboolean GLBE_BlendForeground(const batch_t *batch)
{
	shader_t *sh = batch->shader;
	if (batch->ent->flags & (RF_WEAPONMODEL | RF_DEPTHHACK))
		return true;
	if (r_refdef.recurse || !r_refdef.useperspective || shaderstate.mode != BEM_STANDARD ||
		batch->ent != &r_worldentity || batch->buildmeshes != R_DB_Poly || batch->fog || batch->vbo ||
		!(batch->flags & BEF_LINES) || (batch->flags & ~(BEF_LINES | BEF_NOSHADOWS | BEF_NODLIGHT)))
		return false;
	return sh && sh == shader_draw_fill_trans && sh->remapto == sh &&
		sh->numpasses == 1 && !sh->numdeforms && !sh->passes[0].prog;
}

static int GLBE_BlendFaceCompare(const void *va, const void *vb)
{
	const glblendface_t *a = va, *b = vb;
	if (a->foreground != b->foreground) return a->foreground ? 1 : -1;
	if (a->depth > b->depth) return -1;
	if (a->depth < b->depth) return 1;
	return (a->sequence > b->sequence) - (a->sequence < b->sequence);
}

static qboolean GLBE_BlendBatchImmediate(const batch_t *batch)
{
	shader_t *sh = batch->shader;
	unsigned int special = SHADER_HASREFLECT | SHADER_HASREFRACT |
		SHADER_HASRIPPLEMAP | SHADER_HASPORTAL | SHADER_SKY;
	// Only recursion-capable/unknown materials stay outside the sortable stream.
	return !sh || !sh->remapto || !batch->ent ||
		((sh->flags | sh->remapto->flags) & special);
}

static qboolean GLBE_BlendFaceEligible(const batch_t *batch)
{
	unsigned int m;
	if (GLBE_BlendBatchImmediate(batch) || batch->buildmeshes || !batch->mesh)
		return false;
	if (batch->flags & BEF_FORCEADDITIVE)
		return false;
	if (batch->ent->flags & (RF_WEAPONMODEL | RF_DEPTHHACK | RF_XFLIP))
		return false;
	// Efficient scene-cache VBOs have drawable indices but no CPU positions.
	// Preserve them as complete coarse commands rather than lose geometry.
	for (m = batch->firstmesh; m < batch->meshes; m++)
		if (batch->mesh[m] && batch->mesh[m]->numvertexes > 0 && batch->mesh[m]->numindexes > 0 &&
			!batch->mesh[m]->xyz_array)
			return false;
	return true;
}

// Mode2 shares default1's conservative static-world raster proof.
static qboolean GLBE_BlendWorldBatchEligible(batch_t *batch);
static qboolean GLBE_BlendWorldRasterEmpty(const batch_t *batch, const mesh_t *mesh);

#define GLBLEND_PLANE_FACES 128
#define GLBLEND_PLANE_VERTS 64
#define GLBLEND_PLANE_INDEXES 192
#define GLBLEND_PLANE_TOTALVERTS 2048
#define GLBLEND_PLANE_WORK 65536
#define GLBLEND_PLANE_EPS 0.125

typedef struct
{
	mesh_t *mesh;
	double normal[3], origin[3], eye;
	double mins[2], maxs[2];
	size_t slot;
} glblendplane_t;

typedef struct
{
	unsigned int admitted, edges, work, moved;
	int status; // 0 unchanged, 1 ordered, -1 budget, -2 cycle.
	int pass; // mode2 graph: 0 overlap, 1 overlap-confirmed, 2 P742.
} glblendplanestats_t;

static qboolean GLBE_BlendPlaneShaderEligible(const shader_t *shader)
{
	int p;
	// Even a vertex-only material program can move positions. No CPU-plane proof.
	if (!shader || shader->numdeforms || shader->prog || shader->numpasses < 0 || shader->numpasses > SHADER_PASS_MAX)
		return false;
	for (p = 0; p < shader->numpasses; p++)
		if ((shader->passes[p].shaderbits & SBITS_TESSELLATION) || shader->passes[p].prog)
			return false;
	return true;
}

static qboolean GLBE_BlendPlaneViewEligible(void)
{
	const float *p = r_refdef.m_projection_std;
	int i;
	if (!r_refdef.useperspective || shaderstate.mode != BEM_STANDARD)
		return false;
	for (i = 0; i < 16; i++)
		if (IS_NAN(p[i])) return false;
	// R_SetupGL also accepts external matrices. x/z,y/z bounds require this eye.
	// Allow asymmetric/mirrored frusta and the oblique near-clip z row.
	return p[0] != 0 && p[5] != 0 && p[1] == 0 && p[4] == 0 &&
		p[3] == 0 && p[7] == 0 && p[11] == -1 && p[15] == 0 &&
		p[12] == 0 && p[13] == 0;
}

// Plane-side proofs read the eye from r_origin; GL and the overlap test use
// m_view. Ray-disjoint screen bounds hold in any image frame (roll, mirror,
// skyroom spin), so only the eye must agree. Stereo/VR eye offsets do not.
static qboolean GLBE_BlendViewEyeAgrees(void)
{
	const float *m = r_refdef.m_view;
	double det, eye[3], c[3][3];
	int j;
	for (j = 0; j < 16; j++)
		if (IS_NAN(m[j])) return false;
	for (j = 0; j < 3; j++)
		if (IS_NAN(r_origin[j])) return false;
	// GL's eye solves R*eye = -t, R's row k being m[k], m[4+k], m[8+k];
	// c holds R's cofactors, so R's inverse is c transposed over det.
	c[0][0] = (double)m[5]*m[10] - (double)m[9]*m[6];
	c[0][1] = (double)m[9]*m[2] - (double)m[1]*m[10];
	c[0][2] = (double)m[1]*m[6] - (double)m[5]*m[2];
	c[1][0] = (double)m[8]*m[6] - (double)m[4]*m[10];
	c[1][1] = (double)m[0]*m[10] - (double)m[8]*m[2];
	c[1][2] = (double)m[4]*m[2] - (double)m[0]*m[6];
	c[2][0] = (double)m[4]*m[9] - (double)m[8]*m[5];
	c[2][1] = (double)m[8]*m[1] - (double)m[0]*m[9];
	c[2][2] = (double)m[0]*m[5] - (double)m[4]*m[1];
	det = m[0]*c[0][0] + m[4]*c[0][1] + m[8]*c[0][2];
	if (fabs(det) < 1e-6) return false;
	// Translation is float-rounded at map coordinates.
	for (j = 0; j < 3; j++)
	{
		eye[j] = -(c[0][j]*m[12] + c[1][j]*m[13] + c[2][j]*m[14]) / det;
		if (fabs(eye[j] - r_origin[j]) > 1.0/32) return false;
	}
	return true;
}

static qboolean GLBE_BlendPlaneGeometry(const glblendface_t *face, glblendplane_t *plane)
{
	mesh_t *mesh = face->mesh;
	double best = 0, norm[3] = {0, 0, 0}, origin[3] = {0, 0, 0};
	double p[3], a[3], b[3], cross[3], area, depth, side, xy;
	qboolean nearspan = false, projected = false;
	int v, t, j, k;
	unsigned int ia, ib, ic;
	if (!mesh || face->foreground || face->batch->ent != &r_worldentity ||
		face->batch->buildmeshes || face->batch->flags ||
		!GLBE_BlendPlaneShaderEligible(face->batch->shader) || !GLBE_BlendPlaneShaderEligible(face->batch->shader->remapto) ||
		mesh->xyz2_array || mesh->numbones || !mesh->xyz_array || !mesh->indexes ||
		mesh->numvertexes < 3 || mesh->numvertexes > GLBLEND_PLANE_VERTS ||
		mesh->numindexes < 3 || mesh->numindexes > GLBLEND_PLANE_INDEXES || mesh->numindexes % 3)
		return false;
	for (j = 0; j < 3; j++)
		if (IS_NAN(r_origin[j]) || IS_NAN(vpn[j]) || IS_NAN(vright[j]) || IS_NAN(vup[j]))
			return false;
	for (v = 0; v < mesh->numvertexes; v++)
		for (j = 0; j < 3; j++)
			if (IS_NAN(mesh->xyz_array[v][j]))
				return false;
	// Indexed triangles avoid a duplicate/collinear first-three-vertex plane.
	for (t = 0; t < mesh->numindexes; t += 3)
	{
		ia = mesh->indexes[t]; ib = mesh->indexes[t+1]; ic = mesh->indexes[t+2];
		if (ia >= (unsigned int)mesh->numvertexes || ib >= (unsigned int)mesh->numvertexes || ic >= (unsigned int)mesh->numvertexes)
			return false;
		for (j = 0; j < 3; j++)
		{
			a[j] = (double)mesh->xyz_array[ib][j] - mesh->xyz_array[ia][j];
			b[j] = (double)mesh->xyz_array[ic][j] - mesh->xyz_array[ia][j];
		}
		cross[0] = a[1]*b[2] - a[2]*b[1];
		cross[1] = a[2]*b[0] - a[0]*b[2];
		cross[2] = a[0]*b[1] - a[1]*b[0];
		area = cross[0]*cross[0] + cross[1]*cross[1] + cross[2]*cross[2];
		if (area > best)
		{
			best = area;
			for (j = 0; j < 3; j++)
			{
				norm[j] = cross[j];
				origin[j] = mesh->xyz_array[ia][j];
			}
		}
	}
	if (best < 1e-8)
		return false;
	plane->eye = 0;
	for (j = 0; j < 3; j++)
	{
		plane->normal[j] = norm[j] / sqrt(best);
		plane->origin[j] = origin[j];
		plane->eye += plane->normal[j] * ((double)r_origin[j] - origin[j]);
	}
	if (fabs(plane->eye) <= 2*GLBLEND_PLANE_EPS)
		return false;
	for (v = 0; v < mesh->numvertexes; v++)
	{
		side = depth = 0;
		for (j = 0; j < 3; j++)
		{
			p[j] = (double)mesh->xyz_array[v][j] - r_origin[j];
			side += plane->normal[j] * ((double)mesh->xyz_array[v][j] - origin[j]);
			depth += p[j]*vpn[j];
		}
		if (fabs(side) > GLBLEND_PLANE_EPS)
			return false;
		if (depth <= GLBLEND_PLANE_EPS)
		{
			if (r_blendsort.ival != 2) return false;
			nearspan = true;
			continue;
		}
		for (k = 0; k < 2; k++)
		{
			xy = 0;
			for (j = 0; j < 3; j++)
				xy += p[j] * (k ? vup[j] : vright[j]);
			xy /= depth;
			if (!projected || xy < plane->mins[k]) plane->mins[k] = xy;
			if (!projected || xy > plane->maxs[k]) plane->maxs[k] = xy;
		}
		projected = true;
	}
	if (!projected) return false;
	if (nearspan)
	{
		// No false disjoint proof across the eye plane. Float-input dot products
		// fit these bounds; full original vertices still prove every plane side.
		plane->mins[0] = plane->mins[1] = -1e300;
		plane->maxs[0] = plane->maxs[1] = 1e300;
	}
	plane->mesh = mesh;
	return true;
}

// Homogeneous winding keeps its sign through positive-w clipping, unlike a
// divide of an original behind-eye vertex. This extension is mode2-only.
static qboolean GLBE_BlendWorldRasterEmptyNear(batch_t *batch, const mesh_t *mesh)
{
	const float *view = r_refdef.m_view, *proj = r_refdef.m_projection_std;
	double h[GLBLEND_PLANE_VERTS][3], p[3], terms[6], area, scale;
	unsigned int cull, ia, ib, ic;
	int v, t, j;
	qboolean nearspan = false, behind = true, nondegenerate = false;
	//FTESurf Patch 832: the caller has proven the batch GLBE_BlendWorldBatchEligible (the gather's
	//cullworld). Testing it again here walked every face of the batch once per face: 11.7% of a
	//frame on surf_voyager in Lex's setup.
	if (r_blendsort.ival != 2 ||
		!mesh || !mesh->xyz_array || !mesh->indexes || mesh->xyz2_array || mesh->numbones ||
		mesh->numvertexes < 3 || mesh->numvertexes > GLBLEND_PLANE_VERTS ||
		mesh->numindexes < 3 || mesh->numindexes > GLBLEND_PLANE_INDEXES || mesh->numindexes % 3)
		return false;
	if (IS_NAN(proj[0]) || IS_NAN(proj[5])) return false;
	for (j = 0; j < 16; j++)
		if (IS_NAN(view[j])) return false;
	for (v = 0; v < mesh->numvertexes; v++)
	{
		for (j = 0; j < 3; j++)
		{
			if (IS_NAN(mesh->xyz_array[v][j])) return false;
			p[j] = mesh->xyz_array[v][j];
		}
		h[v][0] = proj[0]*(view[0]*p[0] + view[4]*p[1] + view[8]*p[2] + view[12]);
		h[v][1] = proj[5]*(view[1]*p[0] + view[5]*p[1] + view[9]*p[2] + view[13]);
		h[v][2] = -(view[2]*p[0] + view[6]*p[1] + view[10]*p[2] + view[14]);
		if (h[v][2] <= GLBLEND_PLANE_EPS) nearspan = true;
		if (h[v][2] > -GLBLEND_PLANE_EPS) behind = false;
	}
	// Every point of a wholly behind-eye face has w < 0, so no cull state draws
	// it. Kept, it is an unadmitted barrier that forces status -3 when turning.
	if (behind) return true;
	if (!nearspan) return false;
	cull = batch->shader->remapto->flags & (SHADER_CULL_FRONT | SHADER_CULL_BACK);
	if (!cull) return false;
	cull ^= r_refdef.flipcull;
	for (t = 0; t < mesh->numindexes; t += 3)
	{
		ia = mesh->indexes[t]; ib = mesh->indexes[t+1]; ic = mesh->indexes[t+2];
		if (ia >= (unsigned int)mesh->numvertexes || ib >= (unsigned int)mesh->numvertexes || ic >= (unsigned int)mesh->numvertexes)
			return false;
		if (ia == ib || ib == ic || ia == ic) continue;
		terms[0] = h[ia][0]*h[ib][1]*h[ic][2];
		terms[1] = h[ia][1]*h[ib][2]*h[ic][0];
		terms[2] = h[ia][2]*h[ib][0]*h[ic][1];
		terms[3] = -h[ia][2]*h[ib][1]*h[ic][0];
		terms[4] = -h[ia][1]*h[ib][0]*h[ic][2];
		terms[5] = -h[ia][0]*h[ib][2]*h[ic][1];
		area = scale = 0;
		for (j = 0; j < 6; j++) { area += terms[j]; scale += fabs(terms[j]); }
		if (fabs(area) <= 1e-6*scale + 1e-12) return false;
		nondegenerate = true;
		if (cull & SHADER_CULL_FRONT)
		{
			if (area < 0) return false;
		}
		else if (!(cull & SHADER_CULL_BACK) || area > 0)
			return false;
	}
	return nondegenerate;
}

// -1: a before b; +1: b before a; 0: no strict separation proof.
static int GLBE_BlendPlaneSide(const glblendplane_t *a, const glblendplane_t *b)
{
	double side, lo = 0, hi = 0;
	int v, j;
	for (v = 0; v < a->mesh->numvertexes; v++)
	{
		side = 0;
		for (j = 0; j < 3; j++)
			side += b->normal[j] * ((double)a->mesh->xyz_array[v][j] - b->origin[j]);
		if (!v || side < lo) lo = side;
		if (!v || side > hi) hi = side;
	}
	if (lo > 2*GLBLEND_PLANE_EPS) return b->eye > 0 ? 1 : -1;
	if (hi < -2*GLBLEND_PLANE_EPS) return b->eye < 0 ? 1 : -1;
	return 0;
}

#define GLBLEND_CLIP_VERTS 16
#define GLBLEND_OVERLAP_VERTS 32
// Overlap proofs spend their own budget; once it is gone, pairs get P742 edges.
#define GLBLEND_OVERLAP_WORK 65536
#define GLBLEND_ORDER_DISJOINT 2
// Depths within this many world units tie, so a shared edge or T-junction seam
// gives no sign: 20x the float rounding (~0.0005) of ~15000-unit coordinates.
// A width cut instead dropped real thin overlaps of up to ~200 px (review).
#define GLBLEND_DEPTH_TIE 0.01
#define GLBLEND_PAIR_UNSEEN 3	// screen bounds disjoint
#define GLBLEND_PAIR_OPPOSED 4	// opposite strict plane proofs
#define GLBLEND_PAIR_PENDING 5	// overlap verdict not computed yet
typedef struct { double x, y, iw; } glblendproject_t;
typedef struct
{
	glblendproject_t *verts;	// GLBLEND_CLIP_VERTS per triangle
	signed char *count;		// per triangle: polygon vertices, 0 if empty
	int state;			// 0 pending, 1 projected, -1 uncertain or out of budget
} glblendprojface_t;

// Clip an indexed world triangle before division; iw is affine on its screen
// polygon. -1 is uncertainty, -2 budget, 0 empty.
static int GLBE_BlendProjectTriangle(const mesh_t *mesh, int t, glblendproject_t *out, unsigned int *work)
{
	double h[2][GLBLEND_CLIP_VERTS][4], view[4], da, db, f, area;
	// The world's GL modelview, not r_origin/vright: GLR_DrawPortal rebuilds
	// those without screen roll or eye offsets, m_view keeps them.
	const float *proj = r_refdef.m_projection_std, *m = r_refdef.m_view;
	int i, j, k, v, plane, axis, sign, n = 3, count, src = 0, dst = 1;
	unsigned int index;
	for (j = 0; j < 16; j++) if (IS_NAN(proj[j]) || IS_NAN(m[j])) return -1;
	for (v = 0; v < 3; v++)
	{
		if (*work >= GLBLEND_OVERLAP_WORK) return -2;
		(*work)++;
		index = mesh->indexes[t+v];
		if (index >= (unsigned int)mesh->numvertexes) return -1;
		for (j = 0; j < 3; j++)
		{
			if (IS_NAN(mesh->xyz_array[index][j])) return -1;
			view[j] = m[12+j];
		}
		for (k = 0; k < 3; k++)
			for (j = 0; j < 3; j++)
				view[j] += (double)m[k*4+j]*mesh->xyz_array[index][k];
		view[3] = 1;
		for (j = 0; j < 4; j++)
		{
			h[0][v][j] = 0;
			for (k = 0; k < 4; k++) h[0][v][j] += proj[k*4+j]*view[k];
		}
	}
	for (plane = 0; plane < 6 && n; plane++)
	{
		axis = plane/2; sign = (plane & 1) ? -1 : 1; count = 0;
		for (i = 0; i < n; i++)
		{
			if (*work >= GLBLEND_OVERLAP_WORK) return -2;
			(*work)++;
			j = (i+1)%n;
			da = h[src][i][3] + sign*h[src][i][axis];
			db = h[src][j][3] + sign*h[src][j][axis];
			if (da >= 0)
			{
				if (count == GLBLEND_CLIP_VERTS) return -1;
				memcpy(h[dst][count++], h[src][i], sizeof(h[dst][0]));
			}
			if ((da >= 0) != (db >= 0))
			{
				if (count == GLBLEND_CLIP_VERTS) return -1;
				f = da/(da-db);
				for (k = 0; k < 4; k++) h[dst][count][k] = h[src][i][k] + f*(h[src][j][k]-h[src][i][k]);
				count++;
			}
		}
		n = count; src ^= 1; dst ^= 1;
	}
	if (n < 3) return 0;
	for (i = 0; i < n; i++)
	{
		if (h[src][i][3] <= 0) return -1;
		out[i].x = h[src][i][0]/h[src][i][3];
		out[i].y = h[src][i][1]/h[src][i][3];
		out[i].iw = 1/h[src][i][3];
	}
	area = 0;
	for (i = 0; i < n; i++) { j = (i+1)%n; area += out[i].x*out[j].y - out[j].x*out[i].y; }
	// A degenerate (edge-on or collinear-split) triangle covers no pixel.
	if (fabs(area) < 1e-16) return 0;
	if (area < 0)
		for (i = 0; i < n/2; i++) { glblendproject_t swap = out[i]; out[i] = out[n-1-i]; out[n-1-i] = swap; }
	return n;
}

// Each admitted face is projected once per sort, on first use.
static void GLBE_BlendProjectFace(const mesh_t *mesh, glblendprojface_t *face, unsigned int *work)
{
	int t, n;
	face->state = 1;
	for (t = 0; t+2 < mesh->numindexes; t += 3)
	{
		n = GLBE_BlendProjectTriangle(mesh, t, face->verts + (t/3)*GLBLEND_CLIP_VERTS, work);
		if (n < 0) { face->state = -1; return; }
		face->count[t/3] = n;
	}
}

// 1/w is affine on the convex screen intersection, so its vertices bound the
// depth order. hint: a strict plane order that needs only coverage confirmed.
// Uncertainty or a spent budget returns hint (P742's edge).
static int GLBE_BlendOverlapOrder(const mesh_t *a, glblendprojface_t *fa, const mesh_t *b, glblendprojface_t *fb, int hint, unsigned int *work)
{
	glblendproject_t clip[2][GLBLEND_OVERLAP_VERTS];
	const glblendproject_t *pa, *pb;
	double da, db, f, area, det, dx = 0, dy = 0, offset = 0, za, zb, diff, eps;
	int at, bt, na, nb, n, count, src, dst, i, j, k;
	qboolean seen = false, positive = false, negative = false;
	if (!fa->state) GLBE_BlendProjectFace(a, fa, work);
	if (!fb->state) GLBE_BlendProjectFace(b, fb, work);
	if (fa->state < 0 || fb->state < 0) return hint;
	for (at = 0; at < a->numindexes/3; at++)
	{
		na = fa->count[at]; pa = fa->verts + at*GLBLEND_CLIP_VERTS;
		if (!na) continue;
		for (bt = 0; bt < b->numindexes/3; bt++)
		{
			nb = fb->count[bt]; pb = fb->verts + bt*GLBLEND_CLIP_VERTS;
			if (!nb) continue;
			memcpy(clip[0], pa, na*sizeof(*pa)); n = na; src = 0; dst = 1;
			for (k = 0; k < nb && n; k++)
			{
				j = (k+1)%nb; count = 0;
				for (i = 0; i < n; i++)
				{
					glblendproject_t *p = &clip[src][i], *q = &clip[src][(i+1)%n];
					if (*work >= GLBLEND_OVERLAP_WORK) return hint;
					(*work)++;
					da = (pb[j].x-pb[k].x)*(p->y-pb[k].y) - (pb[j].y-pb[k].y)*(p->x-pb[k].x);
					db = (pb[j].x-pb[k].x)*(q->y-pb[k].y) - (pb[j].y-pb[k].y)*(q->x-pb[k].x);
					if (da >= 0)
					{
						if (count == GLBLEND_OVERLAP_VERTS) return hint;
						clip[dst][count++] = *p;
					}
					if ((da >= 0) != (db >= 0))
					{
						if (count == GLBLEND_OVERLAP_VERTS) return hint;
						f = da/(da-db);
						clip[dst][count].x = p->x + f*(q->x-p->x);
						clip[dst][count].y = p->y + f*(q->y-p->y);
						clip[dst][count++].iw = p->iw + f*(q->iw-p->iw);
					}
				}
				n = count; src ^= 1; dst ^= 1;
			}
			if (n < 3) continue;
			area = 0;
			for (i = 0; i < n; i++) { j = (i+1)%n; area += clip[src][i].x*clip[src][j].y - clip[src][j].x*clip[src][i].y; }
			if (!area) continue;
			if (hint) return hint;
			seen = true;
			for (i = 1; i < nb-1; i++)
			{
				det = (pb[i].x-pb[0].x)*(pb[i+1].y-pb[0].y) - (pb[i+1].x-pb[0].x)*(pb[i].y-pb[0].y);
				if (fabs(det) < 1e-16) continue;
				dx = ((pb[i].iw-pb[0].iw)*(pb[i+1].y-pb[0].y) - (pb[i+1].iw-pb[0].iw)*(pb[i].y-pb[0].y))/det;
				dy = ((pb[i].x-pb[0].x)*(pb[i+1].iw-pb[0].iw) - (pb[i+1].x-pb[0].x)*(pb[i].iw-pb[0].iw))/det;
				offset = pb[0].iw - dx*pb[0].x - dy*pb[0].y;
				break;
			}
			if (i == nb-1) return 0;
			for (i = 0; i < n; i++)
			{
				za = clip[src][i].iw; zb = dx*clip[src][i].x + dy*clip[src][i].y + offset;
				// A depth gap dw moves 1/w by about dw/w^2.
				diff = za-zb; eps = GLBLEND_DEPTH_TIE*fabs(za*zb) + 1e-15;
				if (diff > eps) positive = true;
				if (diff < -eps) negative = true;
			}
			if (positive && negative) return 0;
		}
	}
	if (!seen) return GLBLEND_ORDER_DISJOINT;
	return positive ? 1 : negative ? -1 : 0;
}

static void GLBE_BlendPlaneSort(glblendface_t *faces, size_t n, glblendplanestats_t *stats)
{
	glblendplane_t *planes;
	glblendface_t *ordered;
	glblendprojface_t *proj = NULL;
	glblendproject_t *projverts = NULL;
	signed char *projcount = NULL, *pair = NULL, *verdict = NULL;
	unsigned char *edges = NULL;
	unsigned int degree[GLBLEND_PLANE_FACES] = {0}, order[GLBLEND_PLANE_FACES];
	unsigned int verts = 0, tris = 0, i, j, pick, step, from, to, cost, idx, owork = 0;
	size_t f;
	int ab, ba;
	stats->admitted = stats->edges = stats->work = stats->moved = 0;
	stats->status = stats->pass = 0;
	// The point-eye and divided screen bounds prove only perspective rays.
	// Nonstandard modes may select an override program with different geometry.
	if (!GLBE_BlendPlaneViewEligible())
		return;
	planes = BZ_Malloc(GLBLEND_PLANE_FACES * sizeof(*planes));
	for (f = 0; f < n; f++)
	{
		glblendplane_t candidate;
		if (!GLBE_BlendPlaneGeometry(&faces[f], &candidate))
			continue;
		verts += candidate.mesh->numvertexes;
		if (stats->admitted == GLBLEND_PLANE_FACES || verts > GLBLEND_PLANE_TOTALVERTS)
		{ stats->status = -1; BZ_Free(planes); return; }
		candidate.slot = f;
		planes[stats->admitted++] = candidate;
	}
	if (stats->admitted < 2)
	{ BZ_Free(planes); return; }
	edges = BZ_Malloc(stats->admitted * stats->admitted);
	pair = BZ_Malloc(stats->admitted * stats->admitted);
	// Plane-side proofs once per pair, on P742's budget.
	for (i = 0; i < stats->admitted; i++)
		for (j = i+1; j < stats->admitted; j++)
		{
			idx = i*stats->admitted + j;
			if (planes[i].maxs[0] < planes[j].mins[0] || planes[j].maxs[0] < planes[i].mins[0] ||
				planes[i].maxs[1] < planes[j].mins[1] || planes[j].maxs[1] < planes[i].mins[1])
			{ pair[idx] = GLBLEND_PAIR_UNSEEN; continue; }
			cost = planes[i].mesh->numvertexes + planes[j].mesh->numvertexes;
			if (cost > GLBLEND_PLANE_WORK - stats->work)
			{ stats->status = -1; goto done; }
			stats->work += cost;
			ab = GLBE_BlendPlaneSide(&planes[i], &planes[j]);
			ba = -GLBE_BlendPlaneSide(&planes[j], &planes[i]);
			pair[idx] = (ab && ba && ab != ba) ? GLBLEND_PAIR_OPPOSED : ab ? ab : ba;
		}
	// Pass 0 refines unproved pairs by screen overlap; pass 1 also drops strict
	// edges between faces that never overlap. Pass 2 is P742's graph: a mix of
	// proofs and baseline edges can cycle, or cross a barrier, where
	// all-baseline did not. Mode1 runs pass 2 only.
	stats->pass = r_blendsort.ival == 2 ? 0 : 2;
	if (!stats->pass)
	{
		for (i = 0; i < stats->admitted; i++)
			tris += planes[i].mesh->numindexes/3;
		verdict = BZ_Malloc(stats->admitted * stats->admitted);
		memset(verdict, GLBLEND_PAIR_PENDING, stats->admitted * stats->admitted);
		proj = BZ_Malloc(stats->admitted * sizeof(*proj));
		projverts = BZ_Malloc(tris * GLBLEND_CLIP_VERTS * sizeof(*projverts));
		projcount = BZ_Malloc(tris);
		for (i = 0, tris = 0; i < stats->admitted; i++)
		{
			proj[i].verts = projverts + tris*GLBLEND_CLIP_VERTS;
			proj[i].count = projcount + tris;
			proj[i].state = 0;
			tris += planes[i].mesh->numindexes/3;
		}
	}
rebuild:
	stats->edges = 0;
	memset(edges, 0, stats->admitted * stats->admitted);
	memset(degree, 0, sizeof(degree));
	for (i = 0; i < stats->admitted; i++)
		for (j = i+1; j < stats->admitted; j++)
		{
			idx = i*stats->admitted + j;
			ab = pair[idx];
			if (ab == GLBLEND_PAIR_UNSEEN)
				continue;
			if (stats->pass == 2)
			{
				if (ab == GLBLEND_PAIR_OPPOSED) ab = 0;
			}
			// Opposite strict orders cannot share a positive view ray.
			else if (ab == GLBLEND_PAIR_OPPOSED)
				continue;
			else if (!ab || stats->pass == 1)
			{
				if (verdict[idx] == GLBLEND_PAIR_PENDING)
					verdict[idx] = GLBE_BlendOverlapOrder(planes[i].mesh, &proj[i], planes[j].mesh, &proj[j], ab, &owork);
				ab = verdict[idx];
				if (ab == GLBLEND_ORDER_DISJOINT) continue;
			}
			// Genuine depth reversals/uncertainty preserve baseline order.
			if (!ab) ab = -1;
			from = ab < 0 ? i : j; to = ab < 0 ? j : i;
			edges[from*stats->admitted + to] = 1;
			degree[to]++;
			stats->edges++;
		}
	// Stable Kahn priority is baseline rank. Never compare planes in qsort.
	for (step = 0; step < stats->admitted; step++)
	{
		for (pick = 0; pick < stats->admitted; pick++)
			if (!degree[pick]) break;
		if (pick == stats->admitted)
		{
			if (stats->pass < 2) { stats->pass++; goto rebuild; }
			stats->status = -2; goto done;
		}
		order[step] = pick;
		degree[pick] = GLBLEND_PLANE_FACES + 1;
		for (j = 0; j < stats->admitted; j++)
			if (edges[pick*stats->admitted+j]) degree[j]--;
	}
	// A fixed ineligible slot is not a fixed pairwise draw order. Abstain if
	// any moved world face would cross a dynamic/unsupported command.
	for (i = 0; i < stats->admitted; i++)
	{
		from = i < order[i] ? i : order[i];
		to = i > order[i] ? i : order[i];
		if (planes[to].slot - planes[from].slot != to - from)
		{
			if (stats->pass < 2) { stats->pass++; goto rebuild; }
			stats->status = -3; goto done;
		}
	}
	ordered = BZ_Malloc(stats->admitted * sizeof(*ordered));
	for (i = 0; i < stats->admitted; i++)
	{
		ordered[i] = faces[planes[order[i]].slot];
		stats->moved += order[i] != i;
	}
	for (i = 0; i < stats->admitted; i++)
		faces[planes[i].slot] = ordered[i];
	BZ_Free(ordered);
	stats->status = 1;
done:
	stats->work += owork;
	if (projcount) BZ_Free(projcount);
	if (projverts) BZ_Free(projverts);
	if (proj) BZ_Free(proj);
	if (verdict) BZ_Free(verdict);
	BZ_Free(pair);
	BZ_Free(edges);
	BZ_Free(planes);
}

static void GLBE_SubmitBlendedFaces(batch_t *world, batch_t *dynamic)
{
	batch_t *lists[2] = {world, dynamic}, *batch, view;
	glblendface_t *faces;
	size_t count = 0, n = 0, f;
	unsigned int m;
	int l;
	qboolean cullworld, trace;
	glblendplanestats_t stats = {0};
	float matrix[16], modelview[16];

	// Finish capture/portal/sky recursion before gathering this view's records.
	// The temp-batch pool appends during recursion; this view's batch addresses survive.
	for (l = 0; l < 2; l++)
		for (batch = lists[l]; batch; batch = batch->next)
			if (GLBE_BlendBatchImmediate(batch))
			{
				view = *batch;
				view.next = NULL;
				GLBE_SubmitMeshesSortList(&view);
			}
	// Every pass of the armed frame traces: portal and skyroom views, light passes,
	// the main view. r_portalonly never reaches the main view, so a frame disarms.
	if (gl_blendtrace_pending && gl_blendtrace_started && gl_blendtrace_frame != host_framecount)
		gl_blendtrace_pending = false;
	trace = gl_blendtrace_pending;
	if (trace)
	{
		gl_blendtrace_started = true;
		gl_blendtrace_frame = host_framecount;
		Con_Printf("ALPHATRACE begin recurse=%d mode=%d perspective=%d vieweligible=%d eye=%d flip=%x worldflags=%x\n",
			r_refdef.recurse, shaderstate.mode, r_refdef.useperspective, GLBE_BlendPlaneViewEligible(), GLBE_BlendViewEyeAgrees(),
			(unsigned int)r_refdef.flipcull, (unsigned int)r_worldentity.flags);
		Con_Printf("ALPHATRACE eye=%.9g,%.9g,%.9g forward=%.9g,%.9g,%.9g\n",
			r_origin[0], r_origin[1], r_origin[2], vpn[0], vpn[1], vpn[2]);
		Con_Printf("ALPHATRACE projection0=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
			r_refdef.m_projection_std[0], r_refdef.m_projection_std[1], r_refdef.m_projection_std[2], r_refdef.m_projection_std[3],
			r_refdef.m_projection_std[4], r_refdef.m_projection_std[5], r_refdef.m_projection_std[6], r_refdef.m_projection_std[7]);
		Con_Printf("ALPHATRACE projection8=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
			r_refdef.m_projection_std[8], r_refdef.m_projection_std[9], r_refdef.m_projection_std[10], r_refdef.m_projection_std[11],
			r_refdef.m_projection_std[12], r_refdef.m_projection_std[13], r_refdef.m_projection_std[14], r_refdef.m_projection_std[15]);
	}
	for (l = 0; l < 2; l++)
		for (batch = lists[l]; batch; batch = batch->next)
			if (!GLBE_BlendBatchImmediate(batch) && batch->meshes > batch->firstmesh)
				count += GLBE_BlendFaceEligible(batch) ? batch->meshes - batch->firstmesh : 1;
	if (!count)
	{
		if (trace) Con_Printf("ALPHATRACE empty blend lists recurse=%d\n", r_refdef.recurse);
		return;
	}
	// Invocation-owned heap storage survives recursive views; never frame-global scratch.
	faces = BZ_Malloc(count * sizeof(*faces));
	for (l = 0; l < 2; l++)
		for (batch = lists[l]; batch; batch = batch->next)
			if (!GLBE_BlendBatchImmediate(batch) && batch->meshes > batch->firstmesh)
			{
				R_RotateForEntity(matrix, modelview, batch->ent, batch->ent->model);
				if (!GLBE_BlendFaceEligible(batch))
				{
					faces[n].batch = batch;
					faces[n].mesh = NULL;
					faces[n].depth = GLBE_BlendBatchDepth(batch, matrix);
					faces[n].sequence = n;
					faces[n].foreground = GLBE_BlendForeground(batch);
					n++;
					continue;
				}
				cullworld = GLBE_BlendPlaneViewEligible() && GLBE_BlendWorldBatchEligible(batch);
				if (cullworld && shaderstate.curentity != batch->ent)
					GLBE_SelectEntity(batch->ent);
				for (m = batch->firstmesh; m < batch->meshes; m++)
				{
					mesh_t *mesh = batch->mesh[m];
					if (!mesh || !mesh->xyz_array || mesh->numvertexes <= 0 || mesh->numindexes <= 0)
						continue;
					// Invisible backfaces must not create cycles in the plane graph.
					if (cullworld && (GLBE_BlendWorldRasterEmpty(batch, mesh) || GLBE_BlendWorldRasterEmptyNear(batch, mesh)))
						continue;
					faces[n].batch = batch;
					faces[n].mesh = mesh;
					faces[n].depth = GLBE_BlendAreaDepth(mesh, matrix);
					faces[n].sequence = n;
					faces[n].foreground = false;
					n++;
				}
			}
	qsort(faces, n, sizeof(*faces), GLBE_BlendFaceCompare);
	// Static-world plane relations refine only eligible baseline slots. Portal and
	// skyroom views sort too: their r_origin is m_view's eye, and the portal oblique
	// near clip rewrites only the projection z row.
	if (GLBE_BlendViewEyeAgrees())
		GLBE_BlendPlaneSort(faces, n, &stats);
	if (trace)
	{
		Con_Printf("ALPHATRACE result raw=%u gathered=%u admitted=%u status=%d edges=%u work=%u moved=%u pass=%d recurse=%d\n",
			(unsigned int)count, (unsigned int)n, (unsigned int)stats.admitted, stats.status,
			(unsigned int)stats.edges, (unsigned int)stats.work, (unsigned int)stats.moved, stats.pass, r_refdef.recurse);
		for (f = 0; f < n && f < GLBLEND_PLANE_FACES; f++)
		{
			batch_t *b = faces[f].batch;
			const shader_t *sh = b->shader, *rs = sh ? sh->remapto : NULL;
			const mesh_t *mesh = faces[f].mesh;
			static const float zero[3] = {0};
			const float *v = mesh && mesh->xyz_array && mesh->numvertexes ? mesh->xyz_array[0] : zero;
			glblendplane_t plane;
			Con_Printf("ALPHATRACE cmd=%u seq=%u world=%d mesh=%d verts=%u idx=%u vbo=%d flags=%x fog=%d cb=%d poly=%d safe=%d geom=%d depth=%.12g xyz=%.9g,%.9g,%.9g\n",
				(unsigned int)f, (unsigned int)faces[f].sequence, b->ent == &r_worldentity, !!mesh,
				mesh ? (unsigned int)mesh->numvertexes : 0, mesh ? (unsigned int)mesh->numindexes : 0,
				!!b->vbo, (unsigned int)b->flags, !!b->fog, !!b->buildmeshes, b->buildmeshes == R_DB_Poly,
				GLBE_BlendWorldBatchEligible(b), GLBE_BlendPlaneGeometry(&faces[f], &plane), faces[f].depth, v[0], v[1], v[2]);
			Con_Printf("ALPHATRACE shader=%u flags=%x/%x prog=%d/%d passes=%d/%d deforms=%d/%d passprog=%d/%d bits=%x/%x\n",
				(unsigned int)f, sh ? (unsigned int)sh->flags : 0, rs ? (unsigned int)rs->flags : 0,
				sh && sh->prog, rs && rs->prog, sh ? sh->numpasses : 0, rs ? rs->numpasses : 0,
				sh ? sh->numdeforms : 0, rs ? rs->numdeforms : 0,
				sh && sh->numpasses && sh->passes[0].prog, rs && rs->numpasses && rs->passes[0].prog,
				sh && sh->numpasses ? (unsigned int)sh->passes[0].shaderbits : 0,
				rs && rs->numpasses ? (unsigned int)rs->passes[0].shaderbits : 0);
		}
		if (f < n) Con_Printf("ALPHATRACE remaining commands omitted (bounded to %u)\n", GLBLEND_PLANE_FACES);
		Con_Printf("ALPHATRACE end\n");
	}
	for (f = 0; f < n; f++)
	{
		view = *faces[f].batch;
		view.next = NULL;
		if (faces[f].mesh)
		{
			view.mesh = &faces[f].mesh;
			view.firstmesh = 0;
			view.meshes = view.maxmeshes = 1;
		}
		GLBE_SubmitMeshesSortList(&view);
	}
	BZ_Free(faces);
}

// Default ordering keeps the legacy world/dynamic boundary and recursion order.
// Only ordinary single-pass static-world runs may be split into planar faces.
static qboolean GLBE_BlendWorldBatchEligible(batch_t *batch)
{
	if (!GLBE_BlendFaceEligible(batch) || batch->ent != &r_worldentity || batch->buildmeshes || batch->fog || batch->flags ||
		batch->meshes <= batch->firstmesh || batch->shader->numpasses != 1 || batch->shader->remapto->numpasses != 1)
		return false;
	// A rear/near-span face can share its material batch with visible panes.
	// Admit geometry per face in the sorter; unsupported slots stay barriers.
	return GLBE_BlendPlaneShaderEligible(batch->shader) && GLBE_BlendPlaneShaderEligible(batch->shader->remapto);
}

// Only default1's static, single-pass world path may omit proven raster-empty
// commands. Use projected triangle winding, not the plane's largest triangle.
static qboolean GLBE_BlendWorldRasterEmpty(const batch_t *batch, const mesh_t *mesh)
{
	const float *view = r_refdef.m_view, *proj = r_refdef.m_projection_std;
	double xy[GLBLEND_PLANE_VERTS][2], p[3], w, x, y, ax, ay, bx, by, area, scale;
	unsigned int cull, ia, ib, ic;
	int v, t, j;
	qboolean nondegenerate = false;
	if ((batch->flags & BEF_FORCETWOSIDED) || !mesh || !mesh->xyz_array || !mesh->indexes ||
		mesh->xyz2_array || mesh->numbones || mesh->numvertexes < 3 || mesh->numvertexes > GLBLEND_PLANE_VERTS ||
		mesh->numindexes < 3 || mesh->numindexes > GLBLEND_PLANE_INDEXES || mesh->numindexes % 3)
		return false;
	cull = batch->shader->remapto->flags & (SHADER_CULL_FRONT | SHADER_CULL_BACK);
	if (!cull) return false;
	cull ^= r_refdef.flipcull;
	// Nonfinite scales cannot prove winding; NaN comparisons would hide a draw.
	if (IS_NAN(proj[0]) || IS_NAN(proj[5])) return false;
	for (j = 0; j < 16; j++)
		if (IS_NAN(view[j])) return false;
	for (v = 0; v < mesh->numvertexes; v++)
	{
		for (j = 0; j < 3; j++)
		{
			if (IS_NAN(mesh->xyz_array[v][j])) return false;
			p[j] = mesh->xyz_array[v][j];
		}
		w = -(view[2]*p[0] + view[6]*p[1] + view[10]*p[2] + view[14]);
		// Eye-spanning triangles require clipping; uncertainty keeps the command.
		if (w <= GLBLEND_PLANE_EPS) return false;
		x = view[0]*p[0] + view[4]*p[1] + view[8]*p[2] + view[12];
		y = view[1]*p[0] + view[5]*p[1] + view[9]*p[2] + view[13];
		// Asymmetric projection offsets cancel in triangle edge differences.
		xy[v][0] = proj[0]*x/w;
		xy[v][1] = proj[5]*y/w;
	}
	for (t = 0; t < mesh->numindexes; t += 3)
	{
		ia = mesh->indexes[t]; ib = mesh->indexes[t+1]; ic = mesh->indexes[t+2];
		if (ia >= (unsigned int)mesh->numvertexes || ib >= (unsigned int)mesh->numvertexes || ic >= (unsigned int)mesh->numvertexes)
			return false;
		if (ia == ib || ib == ic || ia == ic) continue;
		ax = xy[ib][0] - xy[ia][0]; ay = xy[ib][1] - xy[ia][1];
		bx = xy[ic][0] - xy[ia][0]; by = xy[ic][1] - xy[ia][1];
		area = ax*by - ay*bx;
		scale = fabs(ax*by) + fabs(ay*bx);
		if (fabs(area) <= 1e-6*scale + 1e-12) return false;
		nondegenerate = true;
		// GL's default front winding is CCW; FRONT wins if both flags are set.
		if (cull & SHADER_CULL_FRONT)
		{
			if (area < 0) return false;
		}
		else if (!(cull & SHADER_CULL_BACK) || area > 0)
			return false;
	}
	return nondegenerate;
}

static void GLBE_SubmitBlendedWorld(batch_t *world)
{
	glblendface_t *faces;
	glblendplanestats_t stats;
	batch_t *batch = world, *first, view;
	size_t count, f;
	unsigned int m, inspected;
	qboolean overflow;
	if (!world) return;
	if (r_refdef.recurse || !GLBE_BlendPlaneViewEligible())
	{
		GLBE_SubmitMeshesSortList(world);
		return;
	}
	faces = BZ_Malloc(GLBLEND_PLANE_FACES * sizeof(*faces));
	while (batch)
	{
		first = batch;
		count = inspected = 0;
		overflow = false;
		while (batch && GLBE_BlendWorldBatchEligible(batch))
		{
			// Match actual world submission: a preceding entity may leave an
			// RF_XFLIP contribution in flipcull. Preserve the view-level flip.
			if (shaderstate.curentity != batch->ent)
				GLBE_SelectEntity(batch->ent);
			for (m = batch->firstmesh; m < batch->meshes; m++)
			{
				if (overflow || inspected == GLBLEND_PLANE_FACES) { overflow = true; break; }
				inspected++;
				// Empty draws add neither graph constraints nor crossing barriers.
				if (GLBE_BlendWorldRasterEmpty(batch, batch->mesh[m])) continue;
				faces[count].batch = batch;
				faces[count].mesh = batch->mesh[m];
				faces[count].depth = 0;
				faces[count].sequence = count;
				faces[count].foreground = false;
				count++;
			}
			batch = batch->next;
		}
		if (batch == first)
		{
			// Unsupported geometry/captures are barriers, submitted at their old slot.
			GLBE_SubmitMeshesSortRange(batch, batch->next);
			batch = batch->next;
			continue;
		}
		memset(&stats, 0, sizeof(stats));
		if (!overflow) GLBE_BlendPlaneSort(faces, count, &stats);
		if (stats.moved)
			for (f = 0; f < count; f++)
			{
				view = *faces[f].batch;
				view.next = NULL;
				view.mesh = &faces[f].mesh;
				view.firstmesh = 0;
				view.meshes = view.maxmeshes = 1;
				GLBE_SubmitMeshesSortList(&view);
			}
		else
			// No proven change (including budgets/cycles): retain whole-batch replay.
			GLBE_SubmitMeshesSortRange(first, batch);
	}
	BZ_Free(faces);
}

/*
FTESurf Patch 782 -- r_budget's GPU half.  A GL_TIMESTAMP query wherever the frame moves from one
kind of work to the next; the time between two marks belongs to the category the first one opened.
A frame's marks are read back GLB_FRAMES frames later, and only once its last one is available, so
reading never stalls the pipeline (a frame still in flight by then is dropped, and counted).
*/
#define GLB_FRAMES	4
#define GLB_MARKS	192
static struct
{
	qboolean made;
	GLuint q[GLB_FRAMES][GLB_MARKS];
	unsigned char cat[GLB_FRAMES][GLB_MARKS];
	int nmarks[GLB_FRAMES];
	qboolean pending[GLB_FRAMES];
	unsigned int frame;
	int slot;		//the frame being recorded, -1 when not recording
	qboolean on;	//r_budget was on last frame: switching it on drops what was recorded before
	int cur;		//its current category
	int depth;		//inside a pushed pass or view: its own marks are ignored
	double sum[GPUB_MAX], total;
	int frames, dropped;
} glb = {false, {{0}}, {{0}}, {0}, {0}, 0, -1};

static void GLBudget_Mark(int cat)
{
	int s = glb.slot;
	if (glb.nmarks[s] >= GLB_MARKS-1)
		return;	//one is kept for the end mark; the rest of the frame stays in the current category
	qglQueryCounter(glb.q[s][glb.nmarks[s]], GL_TIMESTAMP);
	glb.cat[s][glb.nmarks[s]++] = cat;
	glb.cur = cat;
}

static void GLBudget_Read(int s)
{
	GLuint avail = 0;
	unsigned long long t, prev, first;
	int i, n = glb.nmarks[s];

	glb.pending[s] = false;
	if (n < 2)
		return;
	qglGetQueryObjectuivARB(glb.q[s][n-1], GL_QUERY_RESULT_AVAILABLE_ARB, &avail);
	if (!avail)
	{
		glb.dropped++;
		return;
	}
	qglGetQueryObjectui64v(glb.q[s][0], GL_QUERY_RESULT_ARB, &first);
	prev = first;
	for (i = 1; i < n; i++)
	{
		qglGetQueryObjectui64v(glb.q[s][i], GL_QUERY_RESULT_ARB, &t);
		if (t > prev)
			glb.sum[glb.cat[s][i-1]] += (t - prev) / 1000000.0;	//ns to ms
		prev = t;
	}
	if (prev > first)
		glb.total += (prev - first) / 1000000.0;
	glb.frames++;
}

void GLBudget_FrameBegin(void)
{
	extern cvar_t r_budget;
	int s;

	glb.slot = -1;
	glb.depth = 0;
	if (!r_budget.ival || !qglQueryCounter || !qglGetQueryObjectui64v || !qglGetQueryObjectuivARB || !qglGenQueriesARB)
	{
		glb.on = false;
		return;
	}
	if (!glb.made)
	{
		qglGenQueriesARB(GLB_FRAMES*GLB_MARKS, &glb.q[0][0]);
		glb.made = true;
	}
	if (!glb.on)
	{	//frames recorded before it was last switched off are not this window's
		memset(glb.pending, 0, sizeof(glb.pending));
		glb.on = true;
	}
	s = glb.frame++ % GLB_FRAMES;
	if (glb.pending[s])
		GLBudget_Read(s);
	glb.nmarks[s] = 0;
	glb.slot = s;
	//in a map, the GPU first waits while the CPU runs QC and prediction; a menu-only frame is all 2D
	GLBudget_Mark((cls.state == ca_active && cl.worldmodel) ? GPUB_WAIT : GPUB_2D);
}
void GLBudget_FrameEnd(void)
{
	int s = glb.slot;
	if (s < 0)
		return;
	qglQueryCounter(glb.q[s][glb.nmarks[s]], GL_TIMESTAMP);	//always room: Mark keeps it free
	glb.cat[s][glb.nmarks[s]++] = GPUB_OTHER;
	glb.pending[s] = true;
	glb.slot = -1;
}
void GLBudget_Set(int cat)
{
	if (glb.slot < 0 || glb.depth)
		return;
	if (r_refdef.recurse)
		cat = GPUB_PORTALS;	//a nested view (portal, mirror, reflection) is one item, whatever it draws
	if (cat != glb.cur)
		GLBudget_Mark(cat);
}
int GLBudget_Push(int cat)
{
	int prev = glb.cur;
	if (glb.slot < 0)
		return prev;
	if (!glb.depth && cat != glb.cur)
		GLBudget_Mark(cat);
	glb.depth++;
	return prev;
}
void GLBudget_Pop(int prev)
{
	if (glb.slot < 0 || !glb.depth)
		return;
	if (!--glb.depth && prev != glb.cur)
		GLBudget_Mark(prev);
}
void GLBudget_Reset(void)
{	//the context and its query objects are gone; a new one makes its own
	memset(&glb, 0, sizeof(glb));
	glb.slot = -1;
}
int R_BudgetGPUCollect(double ms[GPUB_MAX], double *total)
{
	int i, n = glb.frames;
	for (i = 0; i < GPUB_MAX; i++)
	{
		ms[i] = glb.sum[i];
		glb.sum[i] = 0;
	}
	*total = glb.total;
	glb.total = 0;
	glb.frames = 0;
	return n;
}
//which category a sort level's world (or model) batches are
static int GLBudget_SortCat(int sort, qboolean world)
{
	if (sort == SHADER_SORT_SKY)
		return GPUB_SKY;
	if (sort > SHADER_SORT_SEETHROUGH)
		return GPUB_TRANSPARENT;
	return world ? GPUB_WORLD : GPUB_MODELS;
}

/*
FTESurf Patch 878: a pane that samples the screen ($currentrender: Refract at hl2_refract 1) sorts
NEAREST (gl_shader.c Shader_Finish), after everything it can show.  Source draws its refracting
renderables (models: C_BaseAnimating, c_baseanimating.cpp:799; brushes never copy, c_baseentity.cpp:
1801-1804) back to front, each after a fresh copy (viewrender.cpp:4316-4374), and skips them in its
water views (:4360-4363).  Here: world batches first in their old order, then the model list's
other batches, then MODEL panes back to front by centre, each binding (so copying) the screen, then
weapons and no-depth-test draws.  Only a model pane writes depth; a world or brush pane's depth
write is dropped (be_nopanedepth) so the depth the material asks for (mat_vmt.c VMT_RefractBlend)
cannot hide a pane behind it.  r_nearestsort 0 keeps the submission order.
*/
typedef struct
{
	batch_t *batch;
	double depth;
	size_t sequence;
} glnearpane_t;

static qboolean GLBE_NearestPane(const batch_t *batch)
{
	const shader_t *sh = batch->shader;
	return batch->meshes > batch->firstmesh && sh && sh->numpasses &&
		sh->passes[0].texgen == T_GEN_CURRENTRENDER && !GLBE_BlendBatchImmediate(batch);
}

static qboolean GLBE_NearestFlatPane(const batch_t *batch)
{
	return GLBE_NearestPane(batch) &&
		(batch->ent == &r_worldentity || !batch->ent->model || batch->ent->model->type == mod_brush);
}

static qboolean GLBE_NearestLate(const batch_t *batch)
{
	return GLBE_BlendForeground(batch) || (batch->flags & BEF_FORCENODEPTH);
}

static qboolean GLBE_NearestModelPane(const batch_t *batch)
{
	return GLBE_NearestPane(batch) && !GLBE_NearestFlatPane(batch) && !GLBE_NearestLate(batch);
}

static int GLBE_NearPaneCompare(const void *va, const void *vb)
{
	const glnearpane_t *a = va, *b = vb;
	if (a->depth > b->depth) return -1;
	if (a->depth < b->depth) return 1;
	return (a->sequence > b->sequence) - (a->sequence < b->sequence);
}

//the list's batches that pick() accepts, in order: a world or brush pane alone with its depth
//write dropped, the rest in runs (so instancing still sees its neighbours)
static void GLBE_SubmitNearestSome(batch_t *list, qboolean (*pick)(const batch_t *b))
{
	batch_t *batch, *run, one;
	for (batch = list; batch; )
	{
		if (!pick(batch))
			batch = batch->next;
		else if (GLBE_NearestFlatPane(batch))
		{
			one = *batch;
			one.next = NULL;
			be_nopanedepth = true;
			GLBE_SubmitMeshesSortList(&one);
			be_nopanedepth = false;
			batch = batch->next;
		}
		else
		{
			for (run = batch; run && pick(run) && !GLBE_NearestFlatPane(run); run = run->next)
				;
			GLBE_SubmitMeshesSortRange(batch, run);
			batch = run;
		}
	}
}
static qboolean GLBE_PickAll(const batch_t *b)
{
	return true;
}
static qboolean GLBE_PickEarly(const batch_t *b)
{
	return !GLBE_NearestModelPane(b) && !GLBE_NearestLate(b);
}
static qboolean GLBE_PickLate(const batch_t *b)
{
	return !GLBE_NearestModelPane(b) && GLBE_NearestLate(b);
}

extern cvar_t r_nearestsort;
static void GLBE_SubmitNearest(batch_t *world, batch_t *dynamic)
{
	glnearpane_t stackpanes[64], *panes = stackpanes;
	size_t count = 0, n = 0, i;
	float matrix[16], modelview[16];
	batch_t *batch, one;

	if (!r_nearestsort.ival)
	{
		GLBE_SubmitNearestSome(world, GLBE_PickAll);
		GLBE_SubmitNearestSome(dynamic, GLBE_PickAll);
		return;
	}
	GLBE_SubmitNearestSome(world, GLBE_PickAll);
	GLBE_SubmitNearestSome(dynamic, GLBE_PickEarly);
	for (batch = dynamic; batch; batch = batch->next)
		count += GLBE_NearestModelPane(batch);
	if (count > countof(stackpanes))	//heap, as GLBE_SubmitBlendedFaces: it survives recursive views
		panes = BZ_Malloc(count * sizeof(*panes));
	for (batch = dynamic; batch; batch = batch->next)
		if (GLBE_NearestModelPane(batch) && !(r_refdef.recurse && r_refdef.capturekind >= 1))
		{
			R_RotateForEntity(matrix, modelview, batch->ent, batch->ent->model);
			panes[n].batch = batch;
			panes[n].depth = GLBE_BlendBatchDepth(batch, matrix);
			panes[n].sequence = n;
			n++;
		}
	qsort(panes, n, sizeof(*panes), GLBE_NearPaneCompare);
	for (i = 0; i < n; i++)
	{
		one = *panes[i].batch;
		one.next = NULL;
		GLBE_SubmitMeshesSortList(&one);
	}
	if (panes != stackpanes)
		BZ_Free(panes);
	GLBE_SubmitNearestSome(dynamic, GLBE_PickLate);
}

void GLBE_SubmitMeshes (batch_t **worldbatches, int start, int stop)
{
	int i, prev;
	int portaldepth = r_portalrecursion.ival;

	for (i = start; i <= stop; i++)
	{
		if (i == SHADER_SORT_BLEND && r_blendsort.ival == 2)
		{
			GLBudget_Set(GPUB_TRANSPARENT);
			GLBE_SubmitBlendedFaces(worldbatches ? worldbatches[i] : NULL, shaderstate.mbatches[i]);
			continue;
		}
		if (i == SHADER_SORT_NEAREST && shaderstate.mode == BEM_STANDARD)
		{
			GLBudget_Set(GPUB_TRANSPARENT);
			GLBE_SubmitNearest(worldbatches ? worldbatches[i] : NULL, shaderstate.mbatches[i]);
			continue;
		}
		if (worldbatches)
		{
			if (i == SHADER_SORT_PORTAL && r_refdef.recurse < portaldepth)
			{
				prev = GLBudget_Push(GPUB_PORTALS);
				GLBE_SubmitMeshesPortals(worldbatches, shaderstate.mbatches[i]);
				GLBudget_Pop(prev);

				if (!r_refdef.recurse && r_portalonly.ival)
					return;
			}

			if (worldbatches[i])
				GLBudget_Set(GLBudget_SortCat(i, true));
			if (i == SHADER_SORT_BLEND && r_blendsort.ival == 1)
				GLBE_SubmitBlendedWorld(worldbatches[i]);
			else
				GLBE_SubmitMeshesSortList(worldbatches[i]);
		}
		if (shaderstate.mbatches[i])
			GLBudget_Set(GLBudget_SortCat(i, false));
		GLBE_SubmitMeshesSortList(shaderstate.mbatches[i]);
	}
}

#if (defined(GLQUAKE) || defined(VKQUAKE)) && defined(MULTITHREAD)
#define THREADEDWORLD
#endif

extern double r_loaderstalltime;
void GLBE_UpdateLightmaps(void)
{
	double starttime;
	lightmapinfo_t *lm;
	int lmidx;

#ifdef THREADEDWORLD
	extern int webo_blocklightmapupdates;
	if (webo_blocklightmapupdates == 3)
		return;	//we've not had a new scene to render yet. don't bother uploading while something's still painting, its going to be redundant.
	webo_blocklightmapupdates |= 2;	//FIXME: round-robin it? one lightmap per frame?
#endif

	starttime = Sys_DoubleTime();
	for (lmidx = 0; lmidx < numlightmaps; lmidx++)
	{
		lm = lightmap[lmidx];
		if (!lm)
			continue;

		//nettest (SUNVIS): baked sun visibility is STATIC - no styles, no stains, no dlights -
		//so unlike the lightmap it is uploaded once for the whole page and then never touched
		//again. That is why it needs no rectchange tracking and no PBO path.
		//the L8 guard matters: if the driver has no single-channel format the upload would
		//silently produce garbage. Skipping leaves sunvis_texture invalid, the bind falls back
		//to r_whiteimage, and shadows behave exactly as they did before.
		if (lm->sunvis_modified && lm->sunvis_pixels && gl_config.formatinfo[PTI_L8].internalformat)
		{
			if (!TEXVALID(lm->sunvis_texture))
			{
				TEXASSIGN(lm->sunvis_texture, Image_CreateTexture(va("***sunvis %i***", lmidx), NULL, IF_LINEAR|IF_NOMIPMAP|IF_CLAMP));
				qglGenTextures(1, &lm->sunvis_texture->num);
			}
			GL_MTBind(0, GL_TEXTURE_2D, lm->sunvis_texture);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			qglTexImage2D(GL_TEXTURE_2D, 0, gl_config.formatinfo[PTI_L8].internalformat, lm->width, lm->height, 0,
						gl_config.formatinfo[PTI_L8].format, gl_config.formatinfo[PTI_L8].type, lm->sunvis_pixels);
#ifndef FTE_TARGET_WEB
			//single-channel formats are commonly stored as GL_RED, so swizzle like the lightmap
			//path does or the shader's .r would read the wrong component on some drivers.
			if (gl_config.glversion >= (gl_config.gles?3.0:3.3))
			{
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, gl_config.formatinfo[PTI_L8].swizzle_r);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, gl_config.formatinfo[PTI_L8].swizzle_g);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, gl_config.formatinfo[PTI_L8].swizzle_b);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, gl_config.formatinfo[PTI_L8].swizzle_a);
			}
#endif
			lm->sunvis_texture->format = PTI_L8;
			lm->sunvis_texture->width  = lm->width;
			lm->sunvis_texture->height = lm->height;
			lm->sunvis_texture->depth  = 1;
			lm->sunvis_texture->status = TEX_LOADED;
			lm->sunvis_modified = false;
		}

		if (lm->modified)
		{
			int t = lm->rectchange.t;	//pull them out now, in the hopes that it'll be more robust with respect to r_dynamic -1
			int b = lm->rectchange.b;
#ifdef _DEBUG
			if (t >= b)
				Con_Printf("Dodgy lightmaps\n");
			else
#endif
			if (!TEXVALID(lm->lightmap_texture))
			{
				extern cvar_t r_lightmap_nearest;
				TEXASSIGN(lm->lightmap_texture, Image_CreateTexture(va("***lightmap %i***", lmidx), NULL, (r_lightmap_nearest.ival?IF_NEAREST:IF_LINEAR)|IF_NOMIPMAP));
				qglGenTextures(1, &lm->lightmap_texture->num);
				GL_MTBind(0, GL_TEXTURE_2D, lm->lightmap_texture);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				if (qglTexStorage2D && gl_config.formatinfo[lm->fmt].sizedformat)
				{
					qglTexStorage2D(GL_TEXTURE_2D, 1, gl_config.formatinfo[lm->fmt].sizedformat, lm->width, lm->height);
					if (lm->pbo_handle)
					{
						qglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, lm->pbo_handle);
						qglTexSubImage2D(GL_TEXTURE_2D, 0, 0, t, lm->width, b-t, gl_config.formatinfo[lm->fmt].format, gl_config.formatinfo[lm->fmt].type, (char*)NULL + t*lm->width*lm->pixbytes);
						qglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
					}
					else
						qglTexSubImage2D(GL_TEXTURE_2D, 0, 0, t, lm->width, b-t, gl_config.formatinfo[lm->fmt].format, gl_config.formatinfo[lm->fmt].type, lm->lightmaps+t*lm->width*lm->pixbytes);
				}
				else
					qglTexImage2D(GL_TEXTURE_2D, 0, gl_config.formatinfo[lm->fmt].internalformat,	lm->width, lm->height, 0, gl_config.formatinfo[lm->fmt].format, gl_config.formatinfo[lm->fmt].type,	lm->lightmaps);

#ifndef FTE_TARGET_WEB
				if (gl_config.glversion >= (gl_config.gles?3.0:3.3))
				{
					qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, gl_config.formatinfo[lm->fmt].swizzle_r);
					qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, gl_config.formatinfo[lm->fmt].swizzle_g);
					qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, gl_config.formatinfo[lm->fmt].swizzle_b);
					qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, gl_config.formatinfo[lm->fmt].swizzle_a);
				}
#endif

				//for completeness.
				lm->lightmap_texture->format = lm->fmt;
				lm->lightmap_texture->width = lm->width;
				lm->lightmap_texture->height = lm->height;
				lm->lightmap_texture->depth = 1;
				lm->lightmap_texture->status = TEX_LOADED;
			}
			else
			{
				GL_MTBind(0, GL_TEXTURE_2D, lm->lightmap_texture);
				if (lm->pbo_handle)
				{
					qglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, lm->pbo_handle);
					qglTexSubImage2D(GL_TEXTURE_2D, 0, 0, t, lm->width, b-t, gl_config.formatinfo[lm->fmt].format, gl_config.formatinfo[lm->fmt].type, (char*)NULL + t*lm->width*lm->pixbytes);
					qglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
				}
				else
					qglTexSubImage2D(GL_TEXTURE_2D, 0, 0, t, lm->width, b-t, gl_config.formatinfo[lm->fmt].format, gl_config.formatinfo[lm->fmt].type, lm->lightmaps+t*lm->width*lm->pixbytes);
			}
			lm->modified = false;
			lm->rectchange.l = lm->width;
			lm->rectchange.t = lm->height;
			lm->rectchange.r = 0;
			lm->rectchange.b = 0;
		}
	}
	r_loaderstalltime += Sys_DoubleTime()-starttime;
}

batch_t *GLBE_GetTempBatch(void)
{
	batch_t *b;
	if (shaderstate.wbatch >= shaderstate.maxwbatches)
	{
		shaderstate.wbatch++;
		return NULL;
	}
	b = &shaderstate.wbatches[shaderstate.wbatch++];
	b->fog = NULL;
	return b;
}

/*called from shadowmapping code*/
#ifdef RTLIGHTS
void GLBE_BaseEntTextures(const qbyte *worldpvs, const int *worldareas)
{
	batch_t *batches[SHADER_SORT_COUNT];
	batch_t **ob = shaderstate.mbatches;
	shaderstate.mbatches = batches;
	BE_GenModelBatches(batches, shaderstate.curdlight, shaderstate.mode, worldpvs, worldareas);
	GLBE_SubmitMeshes(NULL, SHADER_SORT_PORTAL, SHADER_SORT_SEETHROUGH+1);
	GLBE_SelectEntity(&r_worldentity);
	shaderstate.mbatches = ob;
}
#endif

void GLBE_RenderToTextureUpdate2d(qboolean destchanged)
{
	unsigned int width = 0, height = 0;
	if (destchanged)
	{
		if (*r_refdef.rt_destcolour[0].texname)
		{
			texid_t tex = R2D_RT_GetTexture(r_refdef.rt_destcolour[0].texname, &width, &height);
			GLBE_FBO_Update(&shaderstate.fbo_2dfbo, 0, &tex, 1, r_nulltex, width, height, 0);
		}
		else
			GLBE_FBO_Push(NULL);

		GL_Set2D(false);
	}
	else
	{
		shaderstate.tex_sourcecol = R2D_RT_GetTexture(r_refdef.rt_sourcecolour.texname, &width, &height);
		shaderstate.tex_sourcedepth = R2D_RT_GetTexture(r_refdef.rt_depth.texname, &width, &height);

		shaderstate.tex_reflectcube = R_GetDefaultEnvmap();
	}
}
void GLBE_FBO_Sources(texid_t sourcecolour, texid_t sourcedepth)
{
	shaderstate.tex_sourcecol = sourcecolour;
	shaderstate.tex_sourcedepth = sourcedepth;
}
int GLBE_FBO_Push(fbostate_t *state)
{
	int newfbo;
	int oldfbo = shaderstate.fbo_current;
	if (state)
		newfbo = state->fbo;
	else
		newfbo = 0;
	if (shaderstate.fbo_current == newfbo)	//don't bother if its not changed (also avoids crashes when fbos are not supported)
		return oldfbo;
	qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, shaderstate.fbo_current=newfbo);
	return oldfbo;
}
void GLBE_FBO_Pop(int oldfbo)
{
	qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, oldfbo);
	shaderstate.fbo_current = oldfbo;
}

void GLBE_FBO_Destroy(fbostate_t *state)
{
	if (state->fbo == shaderstate.fbo_current)
		GLBE_FBO_Push(NULL);

	//wasn't initialised anyway.
	if (!state->fbo)
		return;

	qglDeleteFramebuffersEXT(1, &state->fbo);
	state->fbo = 0;

	if (state->rb_depth)
		qglDeleteRenderbuffersEXT(1, &state->rb_depth);
	state->rb_depth = 0;
	if (state->rb_stencil)
		qglDeleteRenderbuffersEXT(1, &state->rb_stencil);
	state->rb_stencil = 0;
	if (state->rb_depthstencil)
		qglDeleteRenderbuffersEXT(1, &state->rb_depthstencil);
	state->rb_depthstencil = 0;

	state->enables = 0;
}

#ifdef RTLIGHTS
#ifdef SHADOWDBG_COLOURNOTDEPTH
void GLBE_BeginRenderBuffer_DepthOnly(texid_t depthtexture)
{
	if (gl_config.ext_framebuffer_objects)
	{
		if (!shadow_fbo_id)
		{
			int drb;
			qglGenFramebuffersEXT(1, &shadow_fbo_id);
			qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, shadow_fbo_id);

			//create an unnamed depth buffer
//			qglGenRenderbuffersEXT(1, &drb);
//			qglBindRenderbufferEXT(GL_RENDERBUFFER_EXT, drb);
//			qglRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24_ARB, SHADOWMAP_SIZE*3, SHADOWMAP_SIZE*2);
//			qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, drb);

			if (qglDrawBuffer)
				qglDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
			if (qglReadBuffer)
				qglReadBuffer(GL_NONE);
		}
		else
			qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, shadow_fbo_id);

		if (TEXVALID(depthtexture))
			qglFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, depthtexture->num, 0);
	}
}
#else
int GLBE_BeginRenderBuffer_DepthOnly(texid_t depthtexture)
{
	int old = shaderstate.fbo_current;
	if (gl_config.ext_framebuffer_objects)
	{
		if (!shadow_fbo_id)
		{
			qglGenFramebuffersEXT(1, &shadow_fbo_id);
			qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, shadow_fbo_id);
			if (qglDrawBuffers)
				qglDrawBuffers(0, NULL);
			else if (qglDrawBuffer)
				qglDrawBuffer(GL_NONE);
			if (qglReadBuffer)
				qglReadBuffer(GL_NONE);
		}
		else
			qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, shadow_fbo_id);
		shaderstate.fbo_current = shadow_fbo_id;

		if (shadow_fbo_depth_num != depthtexture->num)
		{
			shadow_fbo_depth_num = depthtexture->num;
			if (TEXVALID(depthtexture))
				qglFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, depthtexture->num, 0);
		}
	}
	return old;
}
#endif
#endif

//state->colour is created if usedepth is set and it doesn't previously exist
int GLBE_FBO_Update(fbostate_t *state, unsigned int enables, texid_t *destcol, int mrt, texid_t destdepth, int width, int height, int layer)
{
	static GLenum allcolourattachments[] ={GL_COLOR_ATTACHMENT0_EXT,GL_COLOR_ATTACHMENT1_EXT,GL_COLOR_ATTACHMENT2_EXT,GL_COLOR_ATTACHMENT3_EXT,
									GL_COLOR_ATTACHMENT4_EXT,GL_COLOR_ATTACHMENT5_EXT,GL_COLOR_ATTACHMENT6_EXT,GL_COLOR_ATTACHMENT7_EXT};
	int i;
	int old;

	if (TEXVALID(destdepth))
	{
		enables |= FBO_TEX_DEPTH;
		enables &= ~FBO_RB_DEPTH;
	}

	enables |= (mrt<<16);

	if ((state->enables ^ enables) & ~FBO_RESET)
	{
		GLBE_FBO_Destroy(state);
		state->enables = enables & ~FBO_RESET;
		enables |= FBO_RESET;
	}

	if (!state->fbo)
	{
		qglGenFramebuffersEXT(1, &state->fbo);
		old = GLBE_FBO_Push(state);
		enables |= FBO_RESET;
	}
	else
		old = GLBE_FBO_Push(state);
	if (state->rb_size[0] != width || state->rb_size[1] != height || (enables & FBO_RESET))
	{
		if (state->rb_depth && !(enables & FBO_RB_DEPTH))
		{
			qglDeleteRenderbuffersEXT(1, &state->rb_depth);
			state->rb_depth = 0;
		}
		if (state->rb_stencil && !(enables & FBO_RB_STENCIL))
		{
			qglDeleteRenderbuffersEXT(1, &state->rb_stencil);
			state->rb_stencil = 0;
		}
		state->rb_size[0] = width;
		state->rb_size[1] = height;

		enables |= FBO_RESET;
		if (mrt)
		{	//be careful here, gles2 doesn't support glDrawBuffer. hopefully it'll make things up, but this is worrying.
			if (qglDrawBuffers)
				qglDrawBuffers(mrt, allcolourattachments);
			else if (qglDrawBuffer)
				qglDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
			if (qglReadBuffer)
				qglReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
		}
		else
		{
			if (qglDrawBuffers)
				qglDrawBuffers(0, NULL);
			else if (qglDrawBuffer)
				qglDrawBuffer(GL_NONE);
			if (qglReadBuffer)
				qglReadBuffer(GL_NONE);
		}
	}
	if (enables & FBO_TEX_DEPTH)
	{
		qglFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, destdepth->num, 0);
		//fixme: no stencil
	}
	else if (enables & FBO_RB_DEPTH)
	{
		if (!state->rb_depth)
		{
			//create an unnamed depth buffer
			qglGenRenderbuffersEXT(1, &state->rb_depth);
//			if (!gl_config.ext_packed_depth_stencil)
//				qglGenRenderbuffersEXT(1, &state->rb_stencil);
			enables |= FBO_RESET;	//make sure it gets instanciated
		}

		if (enables & FBO_RESET)
		{
			qglBindRenderbufferEXT(GL_RENDERBUFFER_EXT, state->rb_depth);
			if (gl_config.ext_packed_depth_stencil)
				qglRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, state->rb_size[0], state->rb_size[1]);
			else
			{
				qglRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT16_ARB, state->rb_size[0], state->rb_size[1]);
//				qglBindRenderbufferEXT(GL_RENDERBUFFER_EXT, state->rb_stencil);
//				qglRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_STENCIL_INDEX8_EXT, state->rb_size[0], state->rb_size[1]);
			}
			qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, state->rb_depth);
			if (gl_config.ext_packed_depth_stencil)
				qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, state->rb_depth);
			else
				qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, state->rb_stencil);
		}
	}

	for (i = 0; i < mrt; i++)
	{
		if ((destcol[i]->flags & IF_TEXTYPEMASK) == IF_TEXTYPE_CUBE)
		{
			//fixme: we should probably support whole-cubemap rendering for shadowmaps or something.
			qglFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT+i, GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB + layer, destcol[i]->num, 0);
		}
		else
		{	//layer does not make sense here
			qglFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT+i, GL_TEXTURE_2D, destcol[i]->num, 0);
		}
	}

	i = qglCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
	if (GL_FRAMEBUFFER_COMPLETE_EXT != i)
	{
		switch(i)
		{
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_EXT:
			Con_Printf("glCheckFramebufferStatus reported GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT\n");
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_EXT:
			Con_Printf("glCheckFramebufferStatus reported GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT\n");
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_EXT:
			Con_Printf("glCheckFramebufferStatus reported GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS\n");
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_FORMATS_EXT:
			Con_Printf("glCheckFramebufferStatus reported GL_FRAMEBUFFER_INCOMPLETE_FORMATS\n");
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER_EXT:
			Con_Printf("glCheckFramebufferStatus reported GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER\n");
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER_EXT:
			Con_Printf("glCheckFramebufferStatus reported GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER\n");
			break;
		case GL_FRAMEBUFFER_UNSUPPORTED_EXT:
			Con_Printf("glCheckFramebufferStatus reported GL_FRAMEBUFFER_UNSUPPORTED\n");
			break;
		default:
			Con_Printf("glCheckFramebufferStatus returned %#x\n", i);
			break;
		}

		Con_Printf("FBO Enables: %x %i*%i\n", enables, width, height);
		for (i = 0; i < mrt; i++)
		{
			if (destcol[i])
			{
				Con_Printf("^[\\imgptr\\%#"PRIxSIZE"^]", (quintptr_t)destcol[i]);
				Con_Printf("Colour%i: \"%s\" %x %i*%i*%i%s %s\n", i, destcol[i]->ident, destcol[i]->flags, destcol[i]->width, destcol[i]->height, destcol[i]->depth, (destcol[i]->status != TEX_LOADED)?" NOT LOADED":"",  Image_FormatName(destcol[i]->format));
			}
			else
				Con_Printf("Colour%i: <NO IMAGE>\n", i);
		}
		if (enables & FBO_TEX_DEPTH && destdepth)
		{
			Con_Printf("^[\\imgptr\\%#"PRIxSIZE"^]", (quintptr_t)destdepth);
			Con_Printf("Depth: \"%s\" %x %i*%i*%i%s %s\n", destdepth->ident, destdepth->flags, destdepth->width, destdepth->height, destdepth->depth, (destdepth->status != TEX_LOADED)?" NOT LOADED":"",  Image_FormatName(destdepth->format));
		}
		else if (enables & FBO_TEX_DEPTH)
			Con_Printf("Depth: <NO IMAGE>\n");
		else if (enables & FBO_RB_DEPTH)
			Con_Printf("Depth: <RenderBuffer>\n");
	}
	return old;
}
/*
void GLBE_RenderToTexture(texid_t sourcecol, texid_t sourcedepth, texid_t destcol, texid_t destdepth, qboolean usedepth)
{
	shaderstate.tex_sourcecol = sourcecol;
	shaderstate.tex_sourcedepth = sourcedepth;
	if (!destcol.num)
	{
		shaderstate.fbo_current = 0;
		qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, shaderstate.fbo_current);
	}
	else
	{
		if (usedepth)
		{
			if (!shaderstate.fbo_diffuse)
			{
				qglGenFramebuffersEXT(1, &shaderstate.fbo_diffuse);
				qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, shaderstate.fbo_diffuse);

				//create an unnamed depth buffer
				qglGenRenderbuffersEXT(1, &shaderstate.rb_depth);
				qglBindRenderbufferEXT(GL_RENDERBUFFER_EXT, shaderstate.rb_depth);
				if (gl_config.ext_packed_depth_stencil)
				{
					qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, shaderstate.rb_depth);
				}
				else
				{
					qglGenRenderbuffersEXT(1, &shaderstate.rb_stencil);
					qglBindRenderbufferEXT(GL_RENDERBUFFER_EXT, shaderstate.rb_stencil);
					qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, shaderstate.rb_stencil);
				}
				qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, shaderstate.rb_depth);

				qglDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
				qglReadBuffer(GL_NONE);
			}
			else
				qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, shaderstate.fbo_diffuse);
			shaderstate.fbo_current = shaderstate.fbo_diffuse;

			if (destdepth.num)
			{
				qglFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, destdepth.num, 0);
				qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, 0);
			}
			else
			{
				//resize the depth renderbuffer if its the wrong size now
				if (shaderstate.rb_depth_size[0] != r_refdef.fbo_width || shaderstate.rb_depth_size[1] != r_refdef.fbo_height)
				{
					shaderstate.rb_depth_size[0] = r_refdef.fbo_width;
					shaderstate.rb_depth_size[1] = r_refdef.fbo_height;

					qglBindRenderbufferEXT(GL_RENDERBUFFER_EXT, shaderstate.rb_depth);
					if (gl_config.ext_packed_depth_stencil)
					{
						qglRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, r_refdef.fbo_width, r_refdef.fbo_height);
						qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, shaderstate.rb_depth);
					}
					else
					{
						qglRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24_ARB, r_refdef.fbo_width, r_refdef.fbo_height);

						qglBindRenderbufferEXT(GL_RENDERBUFFER_EXT, shaderstate.rb_stencil);
						qglRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_STENCIL_INDEX8_EXT, r_refdef.fbo_width, r_refdef.fbo_height);
					}
				}
				qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, shaderstate.rb_depth);
				if (gl_config.ext_packed_depth_stencil)
					qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, shaderstate.rb_depth);
				else
					qglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, shaderstate.rb_stencil);
			}
		}
		else
		{
			if (!shaderstate.fbo_depthless)
			{
				qglGenFramebuffersEXT(1, &shaderstate.fbo_depthless);
				qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, shaderstate.fbo_depthless);

				qglDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
				qglReadBuffer(GL_NONE);
			}
			else
				qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, shaderstate.fbo_depthless);

			shaderstate.fbo_current = shaderstate.fbo_depthless;
		}
		
		qglFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, destcol.num, 0);
	}
}
*/

void GLBE_DrawLightPrePass(void)
{
	cvar_t *var;
	unsigned int i;
	qboolean redefine = false;
	texid_t depth, targets[countof(shaderstate.tex_gbuf)];
	const char *s;
	int w = r_refdef.pxrect.width, h = r_refdef.pxrect.height;
	/*
	walls(bumps) -> normalbuffer
	lights+normalbuffer -> lightlevelbuffer
	walls(diffuse)+lightlevelbuffer -> screen

	normalbuffer contains depth in the alpha channel. an actual depthbuffer is also generated at this time, which is used for depth test stuff but not as a shader input.
	*/
	int oldfbo;

	if (r_refdef.recurse)
		return;	//fixme: messy stuff...

	/*do portals*/
	BE_SelectMode(BEM_STANDARD);
	GLBE_SubmitMeshes(cl.worldmodel->batches, SHADER_SORT_PORTAL, SHADER_SORT_PORTAL);

	BE_SelectMode(BEM_GBUFFER);
	for (i = 0; i < countof(shaderstate.tex_gbuf); i++)
	{
		if (!TEXVALID(shaderstate.tex_gbuf[i]) || w != shaderstate.tex_gbuf[i]->width || h != shaderstate.tex_gbuf[i]->height)
		{
			if (!shaderstate.tex_gbuf[i])
			{
				shaderstate.tex_gbuf[i] = Image_CreateTexture(va("***gbuffer %u***", i), NULL, IF_CLAMP|IF_NEAREST|IF_NOMIPMAP|IF_RENDERTARGET);
				qglGenTextures(1, &shaderstate.tex_gbuf[i]->num);
			}
			shaderstate.tex_gbuf[i]->width = w;
			shaderstate.tex_gbuf[i]->height = h;
			redefine = true;
		}
	}

	//something changed, redefine the textures.
	if (redefine)
	{
		static const char *defualtfmts[countof(shaderstate.tex_gbuf)] =
			//depth,	normals,	difflight,	speclight
			{"depth",	"rgba16f",	"rgba16f",	"rgba8",	"", "", "", ""};

		checkglerror();

		for (i = 0; i < countof(shaderstate.tex_gbuf); i++)
		{
			GLint ifmt = 0;
			GLenum dfmt = GL_RGBA;
			GLenum dtype = GL_UNSIGNED_BYTE;
			var = Cvar_Get(va("gl_deferred_gbuffmt_%i", i), defualtfmts[i]?defualtfmts[i]:"", 0, "Deferred Rendering");
			if (!var)
				continue;
			if (!strcmp(var->string, "rgba32f"))
			{
				if (gl_config_gles)
				{	//gles3
					ifmt = GL_RGBA32F;
					dfmt = GL_RGBA;
					dtype = GL_FLOAT;
				}
				else
					ifmt = GL_RGBA32F;
			}
			else if (!strcmp(var->string, "rgba16f"))
			{
				if (gl_config_gles)
				{	//gles3
					ifmt = GL_RGBA16F;
					dfmt = GL_RGBA;
					dtype = GL_HALF_FLOAT;
				}
				else
					ifmt = GL_RGBA16F;
			}
//			else if (!strcmp(var->string, "rgba8s"))
//				ifmt = GL_RGBA8_SNORM;
			else if (!strcmp(var->string, "depth"))
			{
				dtype = GL_UNSIGNED_INT;
				ifmt = GL_DEPTH_COMPONENT;
				dfmt = GL_DEPTH_COMPONENT;
			}
			else if (!strcmp(var->string, "depth16"))
			{
				if (gl_config_gles)
				{
					dtype = GL_UNSIGNED_SHORT;
					ifmt = GL_DEPTH_COMPONENT;
				}
				else
					ifmt = GL_DEPTH_COMPONENT16_ARB;
				dfmt = GL_DEPTH_COMPONENT;
			}
			else if (!strcmp(var->string, "depth24"))
			{
				if (gl_config_gles)
				{
					dtype = GL_UNSIGNED_INT;
					ifmt = GL_DEPTH_COMPONENT;
				}
				else
					ifmt = GL_DEPTH_COMPONENT24_ARB;
				dfmt = GL_DEPTH_COMPONENT;
			}
			else if (!strcmp(var->string, "depth32"))
			{
				if (gl_config_gles)
				{
					dtype = GL_FLOAT;
					ifmt = GL_DEPTH_COMPONENT;
				}
				else
					ifmt = GL_DEPTH_COMPONENT32_ARB;
				dfmt = GL_DEPTH_COMPONENT;
			}
			else if (!strcmp(var->string, "rgb565"))
			{
				dtype = GL_UNSIGNED_SHORT_5_6_5;
				ifmt = GL_RGB;
				dfmt = GL_RGB;
			}
			else if (!strcmp(var->string, "rgba4"))
			{
				dtype = GL_UNSIGNED_SHORT_4_4_4_4;
				ifmt = GL_RGBA;
				dfmt = GL_RGBA;
			}
			else if (!strcmp(var->string, "rgba5551"))
			{
				dtype = GL_UNSIGNED_SHORT_5_5_5_1;
				ifmt = GL_RGBA;
				dfmt = GL_RGBA;
			}
			else if (!strcmp(var->string, "rgba8") || *var->string)
			{
#ifndef GLESONLY
				if (!gl_config_gles)
					ifmt = GL_RGBA8;
				else
#endif
					ifmt = GL_RGBA;
				dfmt = GL_RGBA;
			}
			else
				continue;

			shaderstate.tex_gbuf[i]->status = TEX_LOADED;
			GL_MTBind(0, GL_TEXTURE_2D, shaderstate.tex_gbuf[i]);
			qglTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, dfmt, dtype, NULL);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			if (qglGetError())
				Con_Printf("unable to configure gbuffer image as '%s'\n", var->string);
		}
	}

	/*set the FB up to draw surface info*/
	var = Cvar_Get2("gl_deferred_pre_depth", "0", 0, "gbuffer index used for depth. negative means to use an annonamous renderbuffer", "Deferred Rendering");
	if (var->ival < 0 || var->ival >= countof(shaderstate.tex_gbuf))
		depth = r_nulltex;
	else
		depth = shaderstate.tex_gbuf[var->ival];
	var = Cvar_Get2("gl_deferred_pre_targets", "1", 0, "space-separated list of gbuffer indexes to use for deferred surface information", "Deferred Rendering");
	for (i = 0, s = var->string; *s && i < countof(targets); )
	{
		char token[32];
		int b;
		s = COM_ParseOut(s, token, sizeof(token));
		if (!*token)
			continue;
		b = atoi(token);
		if (b >= 0 && b < countof(shaderstate.tex_gbuf))
			targets[i++] = shaderstate.tex_gbuf[b];
	}

	oldfbo = GLBE_FBO_Update(&shaderstate.fbo_lprepass, depth?FBO_TEX_DEPTH:FBO_RB_DEPTH, targets, i, depth, w, h, 0);
	if (GL_FRAMEBUFFER_COMPLETE_EXT != qglCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT))
	{
		Con_Printf("Bad framebuffer\n");
		return;
	}
	GL_ForceDepthWritable();
	//FIXME: should probably clear colour buffer too.
	qglClear(GL_DEPTH_BUFFER_BIT);

	/*draw surfaces that can be drawn this way*/
	GLBE_SubmitMeshes(cl.worldmodel->batches, SHADER_SORT_OPAQUE, SHADER_SORT_OPAQUE);

	/*reconfigure - now drawing diffuse light info using the previous fb image as a source image*/
	var = Cvar_Get2("gl_deferred_light_targets", "2 3", 0, "space-separated list of gbuffer indexes for lighting to write to", "Deferred Rendering");
	for (i = 0, s = var->string; *s && i < countof(targets); )
	{
		char token[32];
		int b;
		s = COM_ParseOut(s, token, sizeof(token));
		if (!*token)
			continue;
		b = atoi(token);
		if (b >= 0 && b < countof(shaderstate.tex_gbuf))
			targets[i++] = shaderstate.tex_gbuf[b];
	}
	GLBE_FBO_Update(&shaderstate.fbo_lprepass, depth?FBO_TEX_DEPTH:FBO_RB_DEPTH, targets, i, depth, w, h, 0);

	BE_SelectMode(BEM_STANDARD);
	qglClearColor (0,0,0,0);
	qglClear(GL_COLOR_BUFFER_BIT);

	GLBE_SelectEntity(&r_worldentity);
	/*now draw the prelights*/
	GLBE_SubmitMeshes(cl.worldmodel->batches, SHADER_SORT_DEFERREDLIGHT, SHADER_SORT_DEFERREDLIGHT);

	/*final reconfigure - now drawing final surface data onto true framebuffer*/
	GLBE_FBO_Pop(oldfbo);
	if (!oldfbo && qglDrawBuffer)
		qglDrawBuffer(GL_BACK);

	/*now draw the postlight passes (this includes blended stuff which will NOT be lit)*/
	GLBE_SelectEntity(&r_worldentity);
	GLBE_SubmitMeshes(cl.worldmodel->batches, SHADER_SORT_SKY, SHADER_SORT_NEAREST);

#ifdef RTLIGHTS
	/*regular lighting now*/
	GLBE_SelectEntity(&r_worldentity);
	Sh_DrawLights(r_refdef.scenevis);
#endif

	qglClearColor (1,0,0,1);
}

qboolean R_DrawSkyroom(shader_t *skyshader);

void GLBE_DrawWorld (batch_t **worldbatches)
{
#ifdef RTLIGHTS
	extern cvar_t r_shadow_realtime_world, r_shadow_realtime_world_lightmaps;
#endif
	batch_t *batches[SHADER_SORT_COUNT];
	batch_t **ob = shaderstate.mbatches;
	qboolean ownedwas;	//FTESurf Patch 780: polymodeowned across a pass
	int budgetprev;	//FTESurf Patch 782
	RSpeedLocals();
	shaderstate.mbatches = batches;

	shaderstate.usingweaponviewmatrix = -1;

	TRACE(("GLBE_DrawWorld: %p\n", worldbatches));

	//reset batches if we needed more mem, to avoid allocations mid-frame.
	if (!r_refdef.recurse)
	{
		be_flatcopied = false;	//FTESurf Patch 895: each top-level view copies once for its world/brush panes
		if (shaderstate.wbatch + 50 > shaderstate.maxwbatches)
		{
			int newm = shaderstate.wbatch + 100;
			shaderstate.wbatches = BZ_Realloc(shaderstate.wbatches, newm * sizeof(*shaderstate.wbatches));
			memset(shaderstate.wbatches + shaderstate.maxwbatches, 0, (newm - shaderstate.maxwbatches) * sizeof(*shaderstate.wbatches));
			shaderstate.maxwbatches = newm;
		}

		shaderstate.wbatch = 0;
	}
	//if the video mode changed, update any fbos (hopefully this won't happen on mirrors)
	if (shaderstate.oldwidth != vid.pixelwidth || shaderstate.oldheight != vid.pixelheight)
	{
		GLBE_DestroyFBOs();	//will be recreated on demand
		shaderstate.oldwidth = vid.pixelwidth;
		shaderstate.oldheight = vid.pixelheight;

		while(shaderstate.lastpasstmus>0)
		{
			GL_LazyBind(--shaderstate.lastpasstmus, r_nulltex);
		}
#ifdef RTLIGHTS
		Sh_Reset();
#endif
	}

	//memset(batches, 0, sizeof(batches));
	BE_GenModelBatches(batches, shaderstate.curdlight, BEM_STANDARD, r_refdef.scenevis, r_refdef.sceneareas);
	R_GenDlightBatches(batches);
	shaderstate.curentity = &r_worldentity;
//	if (cl.paused || cls.state < ca_active)
		shaderstate.updatetime = r_refdef.time;
//	else
//		shaderstate.updatetime = cl.servertime;

	GLBE_UpdateLightmaps();
	GLBE_WaterViewsBegin(worldbatches);	//FTESurf Patch 879
	if (worldbatches)
	{
		//FTESurf Patch 868: a water capture shows the 2D skybox, not the 3D sky (r_waterskyroom), as
		//Source's above-water views do; with the eye under water its refraction keeps the 3D sky
		//(viewrender.cpp:5912-5927).
		if (worldbatches[SHADER_SORT_SKY] && r_refdef.skyroom_enabled && !r_refdef.skyroom_noleafsky &&	//Patch 880
			!(r_refdef.capturekind >= 1 && !r_refdef.insideportal && !r_waterskyroom.ival &&
			  !(r_viewcontents & (FTECONTENTS_WATER|FTECONTENTS_SLIME|FTECONTENTS_LAVA))))	//FLUID includes SKY
		{
			batch_t *b;
			int seen = 0;
			//FTESurf Patch 850: the 3D sky shows only through sky surfaces, so a water capture with
			//none to draw (a refraction under the water, a reflection of a cave) does not render it.
			//Not a portal: it inherits the near view's areas, so the far side's sky surfaces can be
			//culled while the 3D sky still fills what nothing else drew (surf_kitsune's door).
			qboolean need = (r_refdef.capturekind >= 1 && !r_refdef.insideportal && r_skyroomneed.ival >= 1) ||
				(!r_refdef.recurse && r_skyroomneed.ival >= 2);
			budgetprev = GLBudget_Push(GPUB_SKYROOM);
			for (b = worldbatches[SHADER_SORT_SKY]; b; b = b->next)
			{
				if (b->meshes > b->firstmesh)
					seen++;
				else if (need)
					continue;
				if (R_DrawSkyroom(b->shader))
				{
					GL_CullFace(0);//make sure flipcull reversion takes effect
					currententity = NULL;
					GLBE_SelectEntity(&r_worldentity);
					GL_ForceDepthWritable();
					qglClear(GL_DEPTH_BUFFER_BIT);
					r_refdef.flags |= RDF_SKIPSKY;
					r_vs_skydrawn++;
					break;
				}
			}
			if (!seen && need)
				r_vs_skyskipped++;
			GLBudget_Pop(budgetprev);
		}
		if (gl_overbright.modified)
		{
			int i;
			gl_overbright.modified = false;
			if (gl_overbright.ival > 2)
				gl_overbright.ival = 2;

			for (i = 0; i < SHADER_TMU_MAX; i++)
				shaderstate.blendmode[i] = -1;
		}

#ifdef RTLIGHTS
		if (worldbatches && r_shadow_realtime_world.ival)
			shaderstate.identitylighting = r_shadow_realtime_world_lightmaps.value;
		else
#endif
			shaderstate.identitylighting = r_lightmap_scale.value;
		shaderstate.identitylighting *= r_refdef.hdr_value;
		shaderstate.identitylightmap = shaderstate.identitylighting;
//		shaderstate.identitylightmap *= 1<<gl_overbright.ival;

//		if (cl.worldmodel && cl.worldmodel->fromgame == fg_quake3)
//			shaderstate.identitylighting *= 2;

#ifdef RTLIGHTS
		if (r_lightprepass)
		{
			GLBE_SelectEntity(&r_worldentity);
			GLBE_DrawLightPrePass();
		}
		else
#endif
		{
#ifdef RTLIGHTS
			if (r_fakeshadows)
			{
				//nettest: Sh_GenerateFakeShadows used to sit OUTSIDE every RSPEED_ bucket -- the
				//nearest RSpeedRemark() is the one below, AFTER it.  Its cost (the sun cascades, the
				//per-prop lamp cells and their paired world cells, plus all the per-caster
				//classification and lamp scoring) was therefore counted in Total refresh and CSQC
				//Drawing but attributed to NOTHING.  The sub-buckets simply did not sum to the
				//total, and on a prop-dense map the missing slice was the LARGEST single line in
				//the frame -- bigger than Opaque Batches -- which made r_speeds actively
				//misleading about where the time went.  Read it as "Shadow generation".
				RSpeedRemark();
				budgetprev = GLBudget_Push(GPUB_SHADOWS);
				Sh_GenerateFakeShadows();
				GLBudget_Pop(budgetprev);
				RSpeedEnd(RSPEED_FAKESHADOWS);
			}
#endif
			GLBE_SelectEntity(&r_worldentity);

			if (shaderstate.identitylighting == 0)
				BE_SelectMode(BEM_DEPTHDARK);
			else
				BE_SelectMode(BEM_STANDARD);

			RSpeedRemark();
			//FTESurf Patch 658: was SEETHROUGH+1. GLBE_SubmitMeshes INCLUDES its last bucket (the other
			//renderers' loops stop before it), so UNLITDECAL, where the script particle system's blended
			//shaders sort, was drawn here and again after the lights: every such particle blended twice.
			GLBE_SubmitMeshes(worldbatches, SHADER_SORT_PORTAL, SHADER_SORT_SEETHROUGH);
			RSpeedEnd(RSPEED_OPAQUE);

#ifdef RTLIGHTS
			if (worldbatches)
			{
				RSpeedRemark();
				TRACE(("GLBE_DrawWorld: drawing lights\n"));
				GLBE_SelectEntity(&r_worldentity);
				budgetprev = GLBudget_Push(GPUB_LIGHTS);
				Sh_DrawLights(r_refdef.scenevis);
				GLBudget_Pop(budgetprev);
				RSpeedEnd(RSPEED_RTLIGHTS);
				TRACE(("GLBE_DrawWorld: lights drawn\n"));
			}
#endif
		}

#ifndef GLSLONLY
		if (r_outline.ival && !r_wireframe.ival && qglPolygonMode && qglLineWidth)
		{
			int oc = r_refdef.flipcull;
			shaderstate.identitylighting = 0;
			shaderstate.identitylightmap = 0;
			r_refdef.flipcull ^= SHADER_CULL_FLIP;
			GLBE_SelectMode(BEM_DEPTHDARK);
			shaderstate.polyoffset.unit = 1;
			shaderstate.polyoffset.factor = 1;

			qglEnable(GL_POLYGON_OFFSET_LINE);
			qglLineWidth (bound(0.1, r_outline_width.value, 3.0));
			qglPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			ownedwas = shaderstate.polymodeowned;	//a nested GLBE_DrawWorld (portal, reflection) re-enters here
			shaderstate.polymodeowned = true;
			GLBE_SubmitMeshes(NULL, SHADER_SORT_PORTAL, SHADER_SORT_OPAQUE+1);
			shaderstate.polymodeowned = ownedwas;
			r_refdef.flipcull = oc;
			GLBE_SelectMode(BEM_STANDARD);
			qglDisable(GL_POLYGON_OFFSET_LINE);
			qglPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			qglLineWidth (1);
		}
#endif

		shaderstate.identitylighting = 1;

		RSpeedRemark();
		GLBE_SubmitMeshes(worldbatches, SHADER_SORT_SEETHROUGH+1, SHADER_SORT_NEAREST);
		RSpeedEnd(RSPEED_TRANSPARENTS);

#ifndef GLSLONLY
		if (r_refdef.globalfog.density && (!gl_config.arb_shader_objects || !r_fog_permutation.ival))
		{	//fixed function-only. with global fog. that means we need to hack something in.
			//FIXME: should really be doing this on a per-shader basis, for custom shaders that don't use glsl
			BE_SelectMode(BEM_FOG);

			BE_SelectFog(r_refdef.globalfog.colour, r_refdef.globalfog.alpha, r_refdef.globalfog.density);
			GLBE_SubmitMeshes(worldbatches, SHADER_SORT_PORTAL, SHADER_SORT_NEAREST);
		}
#endif

#ifdef GL_LINE	//no gles
		if (r_wireframe.ival && qglPolygonMode)
		{
			BE_SelectMode(BEM_WIREFRAME);
			qglPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			ownedwas = shaderstate.polymodeowned;
			shaderstate.polymodeowned = true;
			GLBE_SubmitMeshes(worldbatches, SHADER_SORT_PORTAL, SHADER_SORT_NEAREST);
			shaderstate.polymodeowned = ownedwas;
			BE_SelectMode(BEM_STANDARD);
			qglPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		}
#endif
		shaderstate.curdlight = NULL;
	}
	else
	{
		GLBE_SelectEntity(&r_worldentity);
		GLBE_SubmitMeshes(NULL, SHADER_SORT_PORTAL, SHADER_SORT_NEAREST);

#ifdef GL_LINE	//no gles
		if (r_wireframe.ival && qglPolygonMode)
		{
			BE_SelectMode(BEM_WIREFRAME);
			qglPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			ownedwas = shaderstate.polymodeowned;
			shaderstate.polymodeowned = true;
			GLBE_SubmitMeshes(NULL, SHADER_SORT_PORTAL, SHADER_SORT_NEAREST);
			shaderstate.polymodeowned = ownedwas;
			BE_SelectMode(BEM_STANDARD);
			qglPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		}
#endif
	}

	GLBE_WaterViewsEnd();	//FTESurf Patch 879
	GLBE_SelectEntity(&r_worldentity);
//	shaderstate.curtime = shaderstate.updatetime = realtime;
	shaderstate.usingweaponviewmatrix = -1;

	shaderstate.identitylighting = 1;

	shaderstate.mbatches = ob;

	TRACE(("GLBE_DrawWorld: drawn everything\n"));
}

void GLBE_VBO_Begin(vbobctx_t *ctx, size_t maxsize)
{
	COM_AssertMainThread("GLBE_VBO_Begin");	//actually, we should probably just build this in memory and throw it to the main thread as needed, but we still need some buffers indexes. I guess we could build a list of varrays. the other option is to just create a large persistant-mapped buffer, and then just append+reuse from any thread, but that makes destruction messy.

	ctx->maxsize = maxsize;
	ctx->pos = 0;
	ctx->fallback = NULL;
	if (qglBufferStorage)
	{
		ctx->vboid[0] = ctx->vboid[1] = 0;
		qglGenBuffersARB(2, ctx->vboid);
		ctx->fallback = BZ_Malloc(maxsize);
	}
	else if (qglBufferDataARB)
	{
		ctx->vboid[0] = ctx->vboid[1] = 0;
		qglGenBuffersARB(2, ctx->vboid);
		GL_SelectVBO(ctx->vboid[0]);
		//WARNING: in emscripten/webgl, we should probably not pass null.
		qglBufferDataARB(GL_ARRAY_BUFFER_ARB, ctx->maxsize, NULL, GL_STATIC_DRAW_ARB);
	}
	else
		ctx->fallback = BZ_Malloc(maxsize);
}
void GLBE_VBO_Data(vbobctx_t *ctx, void *data, size_t size, vboarray_t *varray)
{
	if (qglBufferStorage)
	{
		memcpy((char*)ctx->fallback + ctx->pos, data, size);
		varray->gl.vbo = ctx->vboid[0];
		varray->gl.addr = (void*)ctx->pos;
	}
	else if (ctx->fallback)
	{
		memcpy((char*)ctx->fallback + ctx->pos, data, size);
		varray->gl.vbo = 0;
		varray->gl.addr = (char*)ctx->fallback + ctx->pos;
	}
	else
	{
		qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB, ctx->pos, size, data);
		varray->gl.vbo = ctx->vboid[0];
		varray->gl.addr = (void*)ctx->pos;
	}
	ctx->pos += size;
}

void GLBE_VBO_Finish(vbobctx_t *ctx, void *edata, size_t esize, vboarray_t *earray, void **vbomem, void **ebomem)
{
	if (ctx->pos > ctx->maxsize)
		Sys_Error("BE_VBO_Finish: too much data given\n");
	if (qglBufferStorage)
	{
		GL_SelectVBO(ctx->vboid[0]);
		qglBufferStorage(GL_ARRAY_BUFFER_ARB, ctx->pos, ctx->fallback, 0);
		BZ_Free(ctx->fallback);
		ctx->fallback = NULL;
		GL_SelectEBO(ctx->vboid[1]);
		qglBufferStorage(GL_ELEMENT_ARRAY_BUFFER_ARB, esize, edata, 0);
		earray->gl.vbo = ctx->vboid[1];
		earray->gl.addr = NULL;
	}
	else if (ctx->fallback)
	{
		void *d = BZ_Malloc(esize);
		memcpy(d, edata, esize);
		earray->gl.vbo = 0;
		earray->gl.addr = d;
	}
	else
	{
		GL_SelectEBO(ctx->vboid[1]);
		qglBufferDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, esize, edata, GL_STATIC_DRAW_ARB);
		earray->gl.vbo = ctx->vboid[1];
		earray->gl.addr = NULL;
	}
}
void GLBE_VBO_Destroy(vboarray_t *vearray, void *vbomem)
{
	if (vearray->gl.vbo)
	{
		qglDeleteBuffersARB(1, &vearray->gl.vbo);
		vearray->gl.vbo = 0;
	}
	else
		BZ_Free(vearray->gl.addr);
	vearray->gl.addr = NULL;
}
#endif
