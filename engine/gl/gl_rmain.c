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
// r_main.c

#include "quakedef.h"

#ifdef GLQUAKE
#include "glquake.h"
#include "renderque.h"
#include "shader.h"
#include "vr.h"

void R_RenderBrushPoly (msurface_t *fa);

#define PROJECTION_DISTANCE			200
#define MAX_STENCIL_ENTS			128

extern int		gl_stencilbits;

//mplane_t	frustum[4];

//
// view origin
//
//vec3_t	vup;
//vec3_t	vpn;
//vec3_t	vright;
//vec3_t	r_origin;

extern cvar_t	gl_part_flame;
extern cvar_t	r_bloom;
extern cvar_t	r_wireframe, r_wireframe_smooth;
extern cvar_t	r_portalmaxviews, r_portaldebug;	//FTESurf Patch 206
extern cvar_t	r_portalscissor;					//FTESurf Patch 208
extern cvar_t	r_outline;

cvar_t	gl_affinemodels = CVARFD("gl_affinemodels","0", CVAR_ARCHIVE, "Use affine texture sampling for models. This replicates software rendering's distortions.");
cvar_t	gl_finish = CVAR("gl_finish","0");
cvar_t	gl_dither = CVAR("gl_dither", "1");
extern cvar_t	r_stereo_separation;
extern cvar_t	r_stereo_convergence;
extern cvar_t	r_stereo_method;
extern cvar_t	r_postprocshader, r_fxaa, r_graphics;
extern cvar_t	r_hdr_framebuffer;

extern cvar_t	gl_screenangle;

extern cvar_t	gl_mindist;
extern cvar_t	vid_srgb;

extern cvar_t	ffov;

extern cvar_t	gl_motionblur;
extern cvar_t	gl_motionblurscale;

extern cvar_t r_tessellation;
extern cvar_t gl_ati_truform_type;
extern cvar_t r_tessellation_level;

extern cvar_t r_portaldrawplanes;
extern cvar_t r_portalonly;

extern	cvar_t	scr_fov;

static shader_t *scenepp_rescaled;
static shader_t *scenepp_antialias;
static shader_t *scenepp_waterwarp;
static shader_t *scenepp_gamma;

// post processing stuff
static texid_t sceneblur_texture;
static texid_t scenepp_texture_warp;
static texid_t scenepp_texture_edge;

texid_t scenepp_postproc_cube;
static int scenepp_postproc_cube_size;

static fbostate_t fbo_vr;
static fbostate_t fbo_gameview;
static fbostate_t fbo_postproc;
static fbostate_t fbo_postproc_cube;

// KrimZon - init post processing - called in GL_CheckExtensions, when they're called
// I put it here so that only this file need be changed when messing with the post
// processing shaders
static void GL_InitSceneProcessingShaders_WaterWarp (void)
{
	scenepp_waterwarp = NULL;
	if (gl_config.arb_shader_objects)
	{
		scenepp_waterwarp = R_RegisterShader("waterwarp", SUF_NONE,
			"{\n"
				"program underwaterwarp\n"
				"{\n"
					"map $sourcecolour\n"
				"}\n"
				"{\n"
					"map $upperoverlay\n"
				"}\n"
				"{\n"
					"map $loweroverlay\n"
				"}\n"
			"}\n"
			);
		scenepp_waterwarp->defaulttextures->upperoverlay = scenepp_texture_warp;
		scenepp_waterwarp->defaulttextures->loweroverlay = scenepp_texture_edge;
	}
}

void GL_ShutdownPostProcessing(void)
{
	GLBE_FBO_Destroy(&fbo_vr);
	GLBE_FBO_Destroy(&fbo_gameview);
	GLBE_FBO_Destroy(&fbo_postproc);
	GLBE_FBO_Destroy(&fbo_postproc_cube);
	R_BloomShutdown();
}

void GL_InitSceneProcessingShaders (void)
{
	if (gl_config.arb_shader_objects)
	{
		GL_InitSceneProcessingShaders_WaterWarp();
	}

	scenepp_gamma = R_RegisterShader("fte_scenegamma", 0, 
		"{\n"
			"program defaultgammacb\n"
			"affine\n"
			"{\n"
				"map $sourcecolour\n"
				"nodepthtest\n"
			"}\n"
		"}\n"
		);

	scenepp_rescaled = R_RegisterShader("fte_rescaler", 0, 
		"{\n"
			"program default2d\n"
			"affine\n"
			"{\n"
				"map $sourcecolour\n"
				"nodepthtest\n"
			"}\n"
		"}\n"
		);
	scenepp_antialias = R_RegisterShader("fte_ppantialias", 0, 
		"{\n"
			"program fxaa\n"
			"affine\n"
			"{\n"
				"map $sourcecolour\n"
				"nodepthtest\n"
			"}\n"
		"}\n"
		);

	r_wireframe_smooth.modified = true;
	gl_dither.modified = true;	//fixme: bad place for this, but hey
	vid_srgb.modified = true;
}

#define PP_WARP_TEX_SIZE 64
#define PP_AMP_TEX_SIZE 64
#define PP_AMP_TEX_BORDER 4
void GL_SetupSceneProcessingTextures (void)
{
	int i, x, y;
	unsigned char pp_warp_tex[PP_WARP_TEX_SIZE*PP_WARP_TEX_SIZE*4];
	unsigned char pp_edge_tex[PP_AMP_TEX_SIZE*PP_AMP_TEX_SIZE*4];

	scenepp_postproc_cube = r_nulltex;

	TEXASSIGN(sceneblur_texture, Image_CreateTexture("***postprocess_blur***", NULL, 0));

	if (!gl_config.arb_shader_objects)
		return;

	TEXASSIGN(scenepp_texture_warp, Image_CreateTexture("***postprocess_warp***", NULL, IF_NOMIPMAP|IF_NOGAMMA|IF_LINEAR));
	TEXASSIGN(scenepp_texture_edge, Image_CreateTexture("***postprocess_edge***", NULL, IF_NOMIPMAP|IF_NOGAMMA|IF_LINEAR));

	// init warp texture - this specifies offset in
	for (y=0; y<PP_WARP_TEX_SIZE; y++)
	{
		for (x=0; x<PP_WARP_TEX_SIZE; x++)
		{
			float fx, fy;

			i = (x + y*PP_WARP_TEX_SIZE) * 4;

			fx = sin(((double)y / PP_WARP_TEX_SIZE) * M_PI * 2);
			fy = cos(((double)x / PP_WARP_TEX_SIZE) * M_PI * 2);

			pp_warp_tex[i  ] = (fx+1.0f)*127.0f;
			pp_warp_tex[i+1] = (fy+1.0f)*127.0f;
			pp_warp_tex[i+2] = 0;
			pp_warp_tex[i+3] = 0xff;
		}
	}

	Image_Upload(scenepp_texture_warp, TF_RGBX32, pp_warp_tex, NULL, PP_WARP_TEX_SIZE, PP_WARP_TEX_SIZE, 1, IF_LINEAR|IF_NOMIPMAP|IF_NOGAMMA);

	// TODO: init edge texture - this is ampscale * 2, with ampscale calculated
	// init warp texture - this specifies offset in
	for (y=0; y<PP_AMP_TEX_SIZE; y++)
	{
		for (x=0; x<PP_AMP_TEX_SIZE; x++)
		{
			float fx = 1, fy = 1;

			i = (x + y*PP_AMP_TEX_SIZE) * 4;

			if (x < PP_AMP_TEX_BORDER)
			{
				fx = (float)x / PP_AMP_TEX_BORDER;
			}
			if (x > PP_AMP_TEX_SIZE - PP_AMP_TEX_BORDER)
			{
				fx = (PP_AMP_TEX_SIZE - (float)x) / PP_AMP_TEX_BORDER;
			}
			
			if (y < PP_AMP_TEX_BORDER)
			{
				fy = (float)y / PP_AMP_TEX_BORDER;
			}
			if (y > PP_AMP_TEX_SIZE - PP_AMP_TEX_BORDER)
			{
				fy = (PP_AMP_TEX_SIZE - (float)y) / PP_AMP_TEX_BORDER;
			}

			//avoid any sudden changes.
			fx=sin(fx*M_PI*0.5);
			fy=sin(fy*M_PI*0.5);

			//lame
			fx = fy = min(fx, fy);

			pp_edge_tex[i  ] = fx * 255;
			pp_edge_tex[i+1] = fy * 255;
			pp_edge_tex[i+2] = 0;
			pp_edge_tex[i+3] = 0xff;
		}
	}

	Image_Upload(scenepp_texture_edge, TF_RGBX32, pp_edge_tex, NULL, PP_AMP_TEX_SIZE, PP_AMP_TEX_SIZE, 1, IF_LINEAR|IF_NOMIPMAP|IF_NOGAMMA);
}

void R_RotateForEntity (float *m, float *modelview, const entity_t *e, const model_t *mod)
{
	if (e->flags & RF_WEAPONMODEL)
	{
		float em[16];
		float vm[16];

		if (e->flags & RF_WEAPONMODELNOBOB)
		{
			vm[0] = r_refdef.weaponmatrix[0][0];
			vm[1] = r_refdef.weaponmatrix[0][1];
			vm[2] = r_refdef.weaponmatrix[0][2];
			vm[3] = 0;

			vm[4] = r_refdef.weaponmatrix[1][0];
			vm[5] = r_refdef.weaponmatrix[1][1];
			vm[6] = r_refdef.weaponmatrix[1][2];
			vm[7] = 0;

			vm[8] = r_refdef.weaponmatrix[2][0];
			vm[9] = r_refdef.weaponmatrix[2][1];
			vm[10] = r_refdef.weaponmatrix[2][2];
			vm[11] = 0;

			vm[12] = r_refdef.weaponmatrix[3][0];
			vm[13] = r_refdef.weaponmatrix[3][1];
			vm[14] = r_refdef.weaponmatrix[3][2];
			vm[15] = 1;
		}
		else
		{
			vm[0] = r_refdef.weaponmatrix_bob[0][0];
			vm[1] = r_refdef.weaponmatrix_bob[0][1];
			vm[2] = r_refdef.weaponmatrix_bob[0][2];
			vm[3] = 0;

			vm[4] = r_refdef.weaponmatrix_bob[1][0];
			vm[5] = r_refdef.weaponmatrix_bob[1][1];
			vm[6] = r_refdef.weaponmatrix_bob[1][2];
			vm[7] = 0;

			vm[8] = r_refdef.weaponmatrix_bob[2][0];
			vm[9] = r_refdef.weaponmatrix_bob[2][1];
			vm[10] = r_refdef.weaponmatrix_bob[2][2];
			vm[11] = 0;

			vm[12] = r_refdef.weaponmatrix_bob[3][0];
			vm[13] = r_refdef.weaponmatrix_bob[3][1];
			vm[14] = r_refdef.weaponmatrix_bob[3][2];
			vm[15] = 1;
		}

		em[0] = e->axis[0][0];
		em[1] = e->axis[0][1];
		em[2] = e->axis[0][2];
		em[3] = 0;

		em[4] = e->axis[1][0];
		em[5] = e->axis[1][1];
		em[6] = e->axis[1][2];
		em[7] = 0;

		em[8] = e->axis[2][0];
		em[9] = e->axis[2][1];
		em[10] = e->axis[2][2];
		em[11] = 0;

		em[12] = e->origin[0];
		em[13] = e->origin[1];
		em[14] = e->origin[2];
		em[15] = 1;

		Matrix4_Multiply(vm, em, m);
	}
	else
	{
		m[0] = e->axis[0][0];
		m[1] = e->axis[0][1];
		m[2] = e->axis[0][2];
		m[3] = 0;

		m[4] = e->axis[1][0];
		m[5] = e->axis[1][1];
		m[6] = e->axis[1][2];
		m[7] = 0;

		m[8] = e->axis[2][0];
		m[9] = e->axis[2][1];
		m[10] = e->axis[2][2];
		m[11] = 0;

		m[12] = e->origin[0];
		m[13] = e->origin[1];
		m[14] = e->origin[2];
		m[15] = 1;
	}

	if (e->scale != 1 && e->scale != 0)	//hexen 2 stuff
	{
#ifdef HEXEN2
		float z;
		float escale;
		escale = e->scale;
		switch(e->drawflags&SCALE_TYPE_MASK)
		{
		default:
		case SCALE_TYPE_UNIFORM:
			VectorScale((m+0), escale, (m+0));
			VectorScale((m+4), escale, (m+4));
			VectorScale((m+8), escale, (m+8));
			break;
		case SCALE_TYPE_XYONLY:
			VectorScale((m+0), escale, (m+0));
			VectorScale((m+4), escale, (m+4));
			break;
		case SCALE_TYPE_ZONLY:
			VectorScale((m+8), escale, (m+8));
			break;
		}
		if (mod && (e->drawflags&SCALE_TYPE_MASK) != SCALE_TYPE_XYONLY)
		{
			switch(e->drawflags&SCALE_ORIGIN_MASK)
			{
			case SCALE_ORIGIN_CENTER:
				z = ((mod->maxs[2] + mod->mins[2]) * (1-escale))/2;
				VectorMA((m+12), z, e->axis[2], (m+12));
				break;
			case SCALE_ORIGIN_BOTTOM:
				VectorMA((m+12), mod->mins[2]*(1-escale), e->axis[2], (m+12));
				break;
			case SCALE_ORIGIN_TOP:
				VectorMA((m+12), -mod->maxs[2], e->axis[2], (m+12));
				break;
			}
		}
#else
		VectorScale((m+0), e->scale, (m+0));
		VectorScale((m+4), e->scale, (m+4));
		VectorScale((m+8), e->scale, (m+8));
#endif
	}
	else if (mod && !strcmp(mod->name, "progs/eyes.mdl"))
	{
		/*resize eyes, to make them easier to see*/
		m[14] -= (22 + 8);
		VectorScale((m+0), 2, (m+0));
		VectorScale((m+4), 2, (m+4));
		VectorScale((m+8), 2, (m+8));
	}
	if (mod && !ruleset_allow_larger_models.ival && mod->clampscale != 1 && mod->type == mod_alias)
	{	//possibly this should be on a per-frame basis, but that's a real pain to do
		Con_DPrintf("Rescaling %s by %f\n", mod->name, mod->clampscale);
		VectorScale((m+0), mod->clampscale, (m+0));
		VectorScale((m+4), mod->clampscale, (m+4));
		VectorScale((m+8), mod->clampscale, (m+8));
	}

	Matrix4_Multiply(r_refdef.m_view, m, modelview);
}

//==================================================================================

/*
=============
R_SetupGL
=============
*/
static void R_SetupGL (const float eyematrix[12]/*can be null*/, const vec4_t fovoverrides/*can be null*/, const float projmatrix[16]/*can be null*/, const pxrect_t *viewport/*can be null*/, texid_t fbo/*can be null*/)
{
	int		x, x2, y2, y, w, h;
	vec3_t newa;

	float fov_x, fov_y, fov_l, fov_r, fov_d, fov_u;
	float fovv_x, fovv_y;

	TRACE(("dbg: calling R_SetupGL\n"));

	if (!r_refdef.recurse)
	{
		newa[0] = r_refdef.viewangles[0];
		newa[1] = r_refdef.viewangles[1];
		newa[2] = r_refdef.viewangles[2] + gl_screenangle.value;
		if (eyematrix)
		{
			extern cvar_t in_vraim;
			matrix3x4 basematrix;
			matrix3x4 viewmatrix;

			if (r_refdef.base_known)
			{	//mod is specifying its own base ang+org.
				Matrix3x4_RM_FromAngles(r_refdef.base_angles, r_refdef.base_origin, basematrix[0]);
				Matrix3x4_Multiply(eyematrix, basematrix[0], viewmatrix[0]);
				Matrix3x4_RM_ToVectors(viewmatrix[0], vpn, vright, vup, r_origin);
				VectorNegate(vright, vright);
			}
			else
			{	//mod provides no info.
				//client will fiddle with input_angles
				newa[0] = newa[2] = 0;	//ignore player pitch+roll. sorry. apply the eye's transform on top.
				if (in_vraim.ival)
					newa[1] -= SHORT2ANGLE(r_refdef.playerview->vrdev[VRDEV_HEAD].angles[YAW]);
				Matrix3x4_RM_FromAngles(newa, r_refdef.vieworg, basematrix[0]);
				Matrix3x4_Multiply(eyematrix, basematrix[0], viewmatrix[0]);
				Matrix3x4_RM_ToVectors(viewmatrix[0], vpn, vright, vup, r_origin);
				VectorNegate(vright, vright);
			}
		}
		else
		{
			AngleVectors (newa, vpn, vright, vup);
			VectorCopy(r_refdef.vieworg, r_origin);
		}

		VectorAdd(r_origin, r_refdef.eyeoffset, r_origin);	//used for vr screenshots

		//
		// set up viewpoint
		//
		if (viewport)
		{
			r_refdef.pxrect = *viewport;
		}
		else if (fbo)
		{
			//with VR fbo postprocessing, we disable all viewport.
			r_refdef.pxrect.x = 0;
			r_refdef.pxrect.y = 0;
			r_refdef.pxrect.width = fbo->width;
			r_refdef.pxrect.height = fbo->height;
			r_refdef.pxrect.maxheight = fbo->height;
		}
		else if (r_refdef.flags & (RDF_ALLPOSTPROC|RDF_RENDERSCALE))
		{
			//with fbo postprocessing, we disable all viewport.
			r_refdef.pxrect.x = 0;
			r_refdef.pxrect.y = 0;
			r_refdef.pxrect.width = vid.fbpwidth;
			r_refdef.pxrect.height = vid.fbpheight;
			r_refdef.pxrect.maxheight = vid.fbpheight;
		}
		else if (*r_refdef.rt_destcolour[0].texname)
		{
			//with fbo rendering, we disable all virtual scaling.
			x = r_refdef.vrect.x;
			x2 = r_refdef.vrect.x + r_refdef.vrect.width;
			y = r_refdef.vrect.y;
			y2 = r_refdef.vrect.y + r_refdef.vrect.height;

			w = x2 - x;
			h = y2 - y;

			r_refdef.pxrect.x = x;
			r_refdef.pxrect.y = y;
			r_refdef.pxrect.width = w;
			r_refdef.pxrect.height = h;
			r_refdef.pxrect.maxheight = vid.fbpheight;
		}
		else
		{
			x = floor(r_refdef.vrect.x * (float)vid.fbpwidth/(float)vid.width);
			x2 = ceil((r_refdef.vrect.x + r_refdef.vrect.width) * (float)vid.fbpwidth/(float)vid.width);
			y = floor(r_refdef.vrect.y * (float)vid.fbpheight/(float)vid.height);
			y2 = ceil((r_refdef.vrect.y + r_refdef.vrect.height) * (float)vid.fbpheight/(float)vid.height);


			// fudge around because of frac screen scale
/*			if (x > 0)
				x--;
			if (x2 < vid.fbpwidth)
				x2++;
			if (y2 < vid.fbpheight)
				y2++;
			if (y > 0)
				y--;
*/
			w = x2 - x;
			h = y2 - y;

/*			if (r_refdef.stereomethod == STEREO_CROSSEYED)
			{
				w /= 2;
				if (i)
					x += vid.fbpwidth/2;
			}
*/
			r_refdef.pxrect.x = x;
			r_refdef.pxrect.y = y;
			r_refdef.pxrect.width = w;
			r_refdef.pxrect.height = h;
			r_refdef.pxrect.maxheight = vid.fbpheight;
		}

		if (projmatrix)
		{
			memcpy(r_refdef.m_projection_std, projmatrix, sizeof(r_refdef.m_projection_std));
			memcpy(r_refdef.m_projection_view, projmatrix, sizeof(r_refdef.m_projection_view));
			r_refdef.flipcull = 0;
		}
		else
		{
			if (fovoverrides)
			{
				fov_l = fovoverrides[0];
				fov_r = fovoverrides[1];
				fov_d = fovoverrides[2];
				fov_u = fovoverrides[3];

				fov_x = fov_r-fov_l;
				fov_y = fov_u-fov_d;

				fovv_x = fov_x;
				fovv_y = fov_y;
				r_refdef.flipcull = ((fov_u < fov_d)^(fov_r < fov_l))?SHADER_CULL_FLIP:0;
			}
			else
			{
				fov_x = r_refdef.fov_x;
				fov_y = r_refdef.fov_y;
				fovv_x = r_refdef.fovv_x;
				fovv_y = r_refdef.fovv_y;

				if ((*r_refdef.rt_destcolour[0].texname || *r_refdef.rt_depth.texname) && strcmp(r_refdef.rt_destcolour[0].texname, "megascreeny"))
				{
					r_refdef.pxrect.y = r_refdef.pxrect.maxheight - (r_refdef.pxrect.height+r_refdef.pxrect.y);
					fov_y *= -1;
					fovv_y *= -1;
					r_refdef.flipcull ^= SHADER_CULL_FLIP;
				}
				else if ((r_refdef.flags & RDF_UNDERWATER) && !(r_refdef.flags & RDF_WATERWARP))
				{
					fov_x *= 1 + (((sin(cl.time * 4.7) + 1) * 0.015) * r_waterwarp.value);
					fov_y *= 1 + (((sin(cl.time * 3.0) + 1) * 0.015) * r_waterwarp.value);

					fovv_x *= 1 + (((sin(cl.time * 4.7) + 1) * 0.015) * r_waterwarp.value);
					fovv_y *= 1 + (((sin(cl.time * 3.0) + 1) * 0.015) * r_waterwarp.value);
				}
				fov_l = -fov_x / 2;
				fov_r = fov_x / 2;
				fov_d = -fov_y / 2;
				fov_u = fov_y / 2;
			}

			if (r_xflip.ival)
			{
				float t = fov_l;
				fov_l = fov_r;
				fov_r = t;
				r_refdef.flipcull ^= SHADER_CULL_FLIP;
				fovv_x *= -1;
			}

			if (r_refdef.useperspective)
			{
				float maxdist = r_refdef.maxdist;
				if (sh_config.stencilbits && Sh_StencilShadowsActive())
					maxdist = 0;	//if we're using stencil shadows then force the maxdist to infinite to ensure the shadow volume is sealed.
				Matrix4x4_CM_Projection_Offset(r_refdef.m_projection_std, fov_l, fov_r, fov_d, fov_u, r_refdef.mindist, maxdist, false);
				Matrix4x4_CM_Projection_Offset(r_refdef.m_projection_view, -fovv_x/2, fovv_x/2, -fovv_y/2, fovv_y/2, r_refdef.mindist, maxdist, false);

				r_refdef.m_projection_std[8] += r_refdef.projectionoffset[0];
				r_refdef.m_projection_std[9] += r_refdef.projectionoffset[1];
				r_refdef.m_projection_view[8] += r_refdef.projectionoffset[0];
				r_refdef.m_projection_view[9] += r_refdef.projectionoffset[1];
			}
			else
			{
				Matrix4x4_CM_Orthographic(r_refdef.m_projection_std, -fov_x/2, fov_x/2, -fov_y/2, fov_y/2, r_refdef.mindist, r_refdef.maxdist?r_refdef.maxdist:9999);
				memcpy(r_refdef.m_projection_view, r_refdef.m_projection_std, sizeof(r_refdef.m_projection_view));
			}
		}
		Matrix4x4_CM_ModelViewMatrixFromAxis(r_refdef.m_view, vpn, vright, vup, r_origin);

		//bias the viewmodel depth range to a third: -1 through -0.333 (instead of -1 to 1)
		r_refdef.m_projection_view[2+4*0] *= 0.333;
		r_refdef.m_projection_view[2+4*1] *= 0.333;
		r_refdef.m_projection_view[2+4*2] *= 0.333;
		r_refdef.m_projection_view[2+4*3] *= 0.333;
		r_refdef.m_projection_view[14] -= 0.666;

		GL_ViewportUpdate();
	}

	if (qglLoadMatrixf)
	{
		qglMatrixMode(GL_PROJECTION);
		qglLoadMatrixf(r_refdef.m_projection_std);

		qglMatrixMode(GL_MODELVIEW);
		qglLoadMatrixf(r_refdef.m_view);
	}

#ifdef GL_LINE_SMOOTH
	if (!gl_config.gles && r_wireframe_smooth.modified)
	{
		r_wireframe_smooth.modified = false;
		if (r_wireframe_smooth.ival || (r_outline.ival && !r_wireframe.ival))
		{
			qglEnable(GL_LINE_SMOOTH);
			if (qglHint)
				qglHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
		}
		else
		{
			qglDisable(GL_LINE_SMOOTH);
			if (qglHint)
				qglHint(GL_LINE_SMOOTH_HINT, GL_FASTEST);
		}
	}
#endif
	if (!gl_config.gles && gl_dither.modified)
	{
		gl_dither.modified = false;
		if (gl_dither.ival)
		{
			qglEnable(GL_DITHER);
		}
		else
		{
			qglDisable(GL_DITHER);
		}
	}
}

void Surf_SetupFrame(void);

/*
================
R_RenderScene

r_refdef must be set before the first call
================
*/
static void R_RenderScene_Internal(void)
{
	extern qboolean depthcleared;
	int tmpvisents = cl_numvisedicts;
	TRACE(("dbg: calling R_SetFrustrum\n"));
	if (!r_refdef.recurse)
		R_SetFrustum (r_refdef.m_projection_std, r_refdef.m_view);

	RQ_BeginFrame();

	TRACE(("dbg: calling Surf_DrawWorld\n"));
	Surf_DrawWorld ();		// adds static entities to the list

	S_ExtraUpdate ();	// don't let sound get messed up if going slow

//	R_DrawDecals();

	TRACE(("dbg: calling R_RenderDlights\n"));
	R_RenderDlights ();

	if (r_refdef.recurse)
		RQ_RenderBatch();
	else
		RQ_RenderBatchClear();

	cl_numvisedicts = tmpvisents;

	depthcleared = false;	//whatever is in the depth buffer is no longer useful.

	if (vrui.enabled)
	{
		vec3_t uifwd, uiright, uiup;
		vec3_t diff;
		float d;
		vec3_t ctrlang, ctrlorg, aimdir;
		extern cvar_t cl_vrui_lock;

		if (cl_vrui_lock.ival)
			VRUI_SnapAngle();

//		extern usercmd_t cl_pendingcmd[MAX_SPLITS];
		AngleVectors(vrui.angles, uifwd, uiright, uiup);

		VectorAngles(uiright, uifwd, r_worldentity.angles, false);
		AngleVectors(r_worldentity.angles, r_worldentity.axis[0], r_worldentity.axis[1], r_worldentity.axis[2]);
		VectorNegate(r_worldentity.axis[1], r_worldentity.axis[1]);
		r_worldentity.scale = 1;//0.2;
		VectorMA(r_refdef.vieworg, Cvar_Get("2dz", "256", 0, "")->value*r_worldentity.scale, r_worldentity.axis[2], r_worldentity.origin);
		VectorMA(r_worldentity.origin, -(int)(vid.width/2)*r_worldentity.scale, r_worldentity.axis[0], r_worldentity.origin);
		VectorMA(r_worldentity.origin, -(int)(vid.height/2)*r_worldentity.scale, r_worldentity.axis[1], r_worldentity.origin);
		GL_SetShaderState2D(true);

		VectorCopy(r_refdef.viewangles, ctrlang);
		if (r_refdef.playerview->vrdev[VRDEV_RIGHT].status&VRSTATUS_ANG)
		{
			ctrlang[0] = SHORT2ANGLE(r_refdef.playerview->vrdev[VRDEV_RIGHT].angles[0]);
			ctrlang[1] = SHORT2ANGLE(r_refdef.playerview->vrdev[VRDEV_RIGHT].angles[1]);
			ctrlang[2] = SHORT2ANGLE(r_refdef.playerview->vrdev[VRDEV_RIGHT].angles[2]);
		}
		else
			VectorCopy(r_refdef.viewangles, ctrlang);
		if (r_refdef.playerview->vrdev[VRDEV_RIGHT].status&VRSTATUS_ORG)
			VectorCopy(r_refdef.playerview->vrdev[VRDEV_RIGHT].origin, ctrlorg);
		else
			VectorCopy(r_refdef.vieworg, ctrlorg);

		AngleVectors(ctrlang, aimdir, NULL, NULL);
		VectorSubtract(r_worldentity.origin, ctrlorg, diff);
		//d = DotProduct(diff, vpn); //figure out how far we need to move it to get an impact.
		d = DotProduct(diff, r_worldentity.axis[2]);
		d /= DotProduct(aimdir, r_worldentity.axis[2]);	//compensate for the length.
		VectorMA(ctrlorg, d, aimdir, diff);	//calc the impact point...
		VectorSubtract(diff, r_worldentity.origin, diff);
		mousecursor_x = DotProduct(diff, r_worldentity.axis[0]);
		mousecursor_y = DotProduct(diff, r_worldentity.axis[1]);
		mousecursor_x = bound(0, mousecursor_x, vid.width-1);
		mousecursor_y = bound(0, mousecursor_y, vid.height-1);

#ifdef PLUGINS
		Plug_SBar (r_refdef.playerview);
#else
		if (Sbar_ShouldDraw(r_refdef.playerview))
		{
			SCR_TileClear (sb_lines);
			Sbar_Draw (r_refdef.playerview);
			Sbar_DrawScoreboard (r_refdef.playerview);
		}
		else
			SCR_TileClear (0);
#endif

		SCR_DrawTwoDimensional(true);

		VectorClear(r_worldentity.origin);
		VectorClear(r_worldentity.angles);
		VectorSet(r_worldentity.axis[0], 1,0,0);
		VectorSet(r_worldentity.axis[1], 0,1,0);
		VectorSet(r_worldentity.axis[2], 0,0,1);
		r_worldentity.scale = 1;
		GL_SetShaderState2D(false);
	}
}
static void R_RenderEyeScene (texid_t rendertarget, const pxrect_t *viewport, const vec4_t fovoverride, const float projmatrix[16], const float eyematrix[12])
{
	extern qboolean depthcleared;
	refdef_t refdef = r_refdef;
	int pw = vid.fbpwidth;
	int ph = vid.fbpheight;
	int r = 0;

	extern void CL_ClampPitch (int pnum, float frametime);
/*the vr code tends to be somewhat laggy with its head angles, leaving it to the last minute, so redo this to reduce latency*/
	if ((size_t)(refdef.playerview-cl.playerview) < MAX_SPLITS)
		CL_ClampPitch (refdef.playerview-cl.playerview, 0);

	vrui.enabled = true;
	if (rendertarget)
	{
		r = GLBE_FBO_Update(&fbo_vr, FBO_RB_DEPTH, &rendertarget, 1, r_nulltex,  rendertarget->width, rendertarget->height, 0);
		GL_ForceDepthWritable();
		qglClear (GL_DEPTH_BUFFER_BIT|GL_COLOR_BUFFER_BIT);
		depthcleared = true;
		vid.fbpwidth = rendertarget->width;
		vid.fbpheight = rendertarget->height;
	}

	R_SetupGL (eyematrix, fovoverride, projmatrix, viewport, rendertarget);
	R_RenderScene_Internal();

	if (rendertarget)
	{
		GLBE_FBO_Pop(r);
		if (gl_finish.ival)
			qglFinish();
	}

	r_refdef = refdef;
	vid.fbpwidth = pw;
	vid.fbpheight = ph;
}
static void R_RenderScene (void)
{
	float stereooffset[2];
	int stereoframes = 1;
	int stereomode;
	int i;
	int cull = r_refdef.flipcull;
	unsigned int colourmask = r_refdef.colourmask;
	vec3_t eyeangorg[2];
	float eyematrix[12];
	extern qboolean		depthcleared;

	r_refdef.colourmask = 0u;
	stereomode = r_refdef.stereomethod;
	if (stereomode == STEREO_QUAD)
	{
#ifdef GL_STEREO
		GLint glb;
		qglGetIntegerv(GL_STEREO, &glb);
		if (!glb || !qglDrawBuffer)
#endif
			stereomode = STEREO_OFF;	//we are not a stereo context, so no stereoscopic rendering (this encourages it to otherwise be left enabled, which means the user is more likely to spot that they asked it to give a slower context.
	}


	if (r_refdef.recurse || !stereomode)// || !(r_stereo_separation.value||r_stereo_convergence.value))
	{
		stereooffset[0] = 0;
		stereoframes = 1;
		stereomode = STEREO_OFF;
	}
	else	
	{
		stereooffset[0] = -0.5*r_stereo_separation.value;
		stereooffset[1] = +0.5*r_stereo_separation.value;
		stereoframes = 2;
	}

	if (vid.vr && !r_refdef.recurse && vid.vr->Render(R_RenderEyeScene))
		;	//we drew something VR-ey
	else if (stereomode == STEREO_OFF)
	{
		GL_ForceDepthWritable();
		qglClear (GL_DEPTH_BUFFER_BIT);

		R_SetupGL (NULL, NULL, NULL, NULL, NULL);
		R_RenderScene_Internal();
	}
	else for (i = 0; i < stereoframes; i++)
	{
		r_refdef.colourmask = 0u;
		switch (stereomode)
		{
		default:
		case STEREO_OFF:	//off
			if (i)
				return;
			break;
#ifdef GL_STEREO
		case STEREO_QUAD:	//proper gl stereo rendering
			if (stereooffset[i] < 0)
				qglDrawBuffer(GL_BACK_LEFT);
			else
				qglDrawBuffer(GL_BACK_RIGHT);
			break;
#endif
		case STEREO_RED_CYAN:	//red/cyan(green+blue)
			if (stereooffset[i] < 0)
				r_refdef.colourmask = (SBITS_MASK_GREEN|SBITS_MASK_BLUE);
			else
				r_refdef.colourmask = SBITS_MASK_RED;
			break;
		case STEREO_RED_BLUE: //red/blue
			if (stereooffset[i] < 0)
				r_refdef.colourmask = (SBITS_MASK_GREEN|SBITS_MASK_BLUE);
			else
				r_refdef.colourmask = (SBITS_MASK_RED|SBITS_MASK_GREEN);
			break;
		case STEREO_RED_GREEN:	//red/green
			if (stereooffset[i] < 0)
				r_refdef.colourmask = (SBITS_MASK_GREEN|SBITS_MASK_BLUE);
			else
				r_refdef.colourmask = (SBITS_MASK_RED|SBITS_MASK_BLUE);
			break;
		case STEREO_CROSSEYED:	//eyestrain
			break;
		case STEREO_LEFTONLY:
			if (i != 0)
				continue;
			break;
		case STEREO_RIGHTONLY:
			if (i != 1)
				continue;
			//fixme: depth buffer doesn't need clearing
			break;
		}

		if (!depthcleared)
		{
			GL_ForceDepthWritable();
			qglClear (GL_DEPTH_BUFFER_BIT);
			depthcleared = true;
		}

		eyeangorg[0][0] = 0;
		eyeangorg[0][1] = r_stereo_convergence.value * (i?0.5:-0.5);
		eyeangorg[0][2] = 0;
		VectorSet(eyeangorg[1], 0, stereooffset[i], 0);
		Matrix3x4_RM_FromAngles(eyeangorg[0], eyeangorg[1], eyematrix);
		R_SetupGL (eyematrix, NULL, NULL, NULL, NULL);
		R_RenderScene_Internal ();
	}

	switch (stereomode)
	{
	default:
	case STEREO_OFF:
	case STEREO_LEFTONLY:
	case STEREO_RIGHTONLY:
		break;
	case STEREO_QUAD:
		qglDrawBuffer(GL_BACK);
		break;
	case STEREO_RED_BLUE:	//green should have already been cleared.
	case STEREO_RED_GREEN:	//blue should have already been cleared.
	case STEREO_RED_CYAN:
		break;
	case 5:
		break;
	}

	r_refdef.flipcull = cull;
	r_refdef.colourmask = colourmask;
}
/*generates a new modelview matrix, as well as vpn vectors*/
static void R_MirrorMatrix(plane_t *plane)
{
	float mirror[16];
	float view[16];
	float result[16];

	vec3_t pnorm;
	VectorNegate(plane->normal, pnorm);

	mirror[0] = 1-2*pnorm[0]*pnorm[0];
	mirror[1] = -2*pnorm[0]*pnorm[1];
	mirror[2] = -2*pnorm[0]*pnorm[2];
	mirror[3] = 0;

	mirror[4] = -2*pnorm[1]*pnorm[0];
	mirror[5] = 1-2*pnorm[1]*pnorm[1];
	mirror[6] = -2*pnorm[1]*pnorm[2] ;
	mirror[7] = 0;

	mirror[8]  = -2*pnorm[2]*pnorm[0];
	mirror[9]  = -2*pnorm[2]*pnorm[1];
	mirror[10] = 1-2*pnorm[2]*pnorm[2];
	mirror[11] = 0;

	mirror[12] = -2*pnorm[0]*plane->dist;
	mirror[13] = -2*pnorm[1]*plane->dist;
	mirror[14] = -2*pnorm[2]*plane->dist;
	mirror[15] = 1;

	view[0] = vpn[0];
	view[1] = vpn[1];
	view[2] = vpn[2];
	view[3] = 0;

	view[4] = -vright[0];
	view[5] = -vright[1];
	view[6] = -vright[2];
	view[7] = 0;

	view[8]  = vup[0];
	view[9]  = vup[1];
	view[10] = vup[2];
	view[11] = 0;

	view[12] = r_refdef.vieworg[0];
	view[13] = r_refdef.vieworg[1];
	view[14] = r_refdef.vieworg[2];
	view[15] = 1;

	VectorMA(r_refdef.vieworg, 0.25, plane->normal, r_refdef.pvsorigin);

	Matrix4_Multiply(mirror, view, result);

	vpn[0] = result[0];
	vpn[1] = result[1];
	vpn[2] = result[2];

	vright[0] = -result[4];
	vright[1] = -result[5];
	vright[2] = -result[6];

	vup[0] = result[8];
	vup[1] = result[9];
	vup[2] = result[10];

	r_refdef.vieworg[0] = result[12];
	r_refdef.vieworg[1] = result[13];
	r_refdef.vieworg[2] = result[14];
}
static entity_t *R_FindPortalCamera(entity_t *rent)
{
	int i;
	for (i = 0; i < cl_numvisedicts; i++)
	{
		if (cl_visedicts[i].rtype == RT_PORTALCAMERA)
		{
			if (cl_visedicts[i].keynum == rent->keynum )
				return &cl_visedicts[i];
		}
	}
	return NULL;
}
static entity_t *R_NearestPortal(plane_t *plane)
{
	int i;
	entity_t *best = NULL;
	float dist, bestd = 0;

	//for q3-compat, portals on world scan for a visedict to use for their view.
	for (i = 0; i < cl_numvisedicts; i++)
	{
		if (cl_visedicts[i].rtype == RT_PORTALSURFACE)
		{
			dist = DotProduct(cl_visedicts[i].origin, plane->normal)-plane->dist;
			dist = fabs(dist);
			if (dist < 64 && (!best || dist < bestd))
				best = &cl_visedicts[i];
		}
	}
	return best;
}

static void TransformCoord(vec3_t in, vec3_t planea[3], vec3_t planeo, vec3_t viewa[3], vec3_t viewo, vec3_t result)
{
	int		i;
	vec3_t	local;
	vec3_t	transformed;
	float	d;

	local[0] = in[0] - planeo[0];
	local[1] = in[1] - planeo[1];
	local[2] = in[2] - planeo[2];

	VectorClear(transformed);
	for ( i = 0 ; i < 3 ; i++ )
	{
		d = DotProduct(local, planea[i]);
		VectorMA(transformed, d, viewa[i], transformed);
	}

	result[0] = transformed[0] + viewo[0];
	result[1] = transformed[1] + viewo[1];
	result[2] = transformed[2] + viewo[2];
}
static void TransformDir(vec3_t in, vec3_t planea[3], vec3_t viewa[3], vec3_t result)
{
	int		i;
	float	d;
	vec3_t tmp;

	VectorCopy(in, tmp);

	VectorClear(result);
	for ( i = 0 ; i < 3 ; i++ )
	{
		d = DotProduct(tmp, planea[i]);
		VectorMA(result, d, viewa[i], result);
	}
}

void R_ObliqueNearClip(float *viewmat, mplane_t *wplane);
void CL_DrawDebugPlane(float *normal, float dist, float r, float g, float b, qboolean enqueue);

/*
  FTESurf Patch 206: the portal's plane, factored out of GLR_DrawPortal.

  Three separate loops need to agree about whether a given portal batch is going
  to be rendered this view, and until now only one of them could tell -- the
  test lived inline in the middle of the function that does the rendering.  The
  answer is a pure function of the batch and the current view, so it is one here
  and everybody asks the same question.

  Returns false when the batch has no usable geometry at all.
*/
static qboolean GLR_PortalPlane(batch_t *batch, plane_t *out, mesh_t **outmesh)
{
	plane_t plane, oplane;
	mesh_t *mesh;

	/* FTESurf Patch 203: a portal batch can legitimately have no mesh.

	   Every portal in the engine up to now has been a world surface, whose
	   mesh is always there.  Patch 205's apertures are ALIAS models, and an
	   alias batch carries mesh==NULL until its buildmeshes callback runs
	   (gl_alias.c:2166) -- and that callback sets it back to NULL when the
	   surface produced no indices (:1970-1971).  The caller builds it before
	   calling us, so this is the second of those two cases; either way,
	   dereferencing batch->mesh[] on faith is a null read. */
	if (!batch->mesh || batch->meshes == batch->firstmesh)
		return false;
	mesh = batch->mesh[batch->firstmesh];
	if (!mesh || !mesh->xyz_array)
		return false;

	if (!mesh->normals_array)
	{
		VectorSet(plane.normal, 0, 0, 1);
	}
	else
	{
		VectorCopy(mesh->normals_array[0], plane.normal);
	}

	if (batch->ent == &r_worldentity)
	{
		plane.dist = DotProduct(mesh->xyz_array[0], plane.normal);
	}
	else
	{
		vec3_t point;
		VectorCopy(plane.normal, oplane.normal);
		//rotate the surface normal around its entity's matrix
		plane.normal[0] = oplane.normal[0]*batch->ent->axis[0][0] + oplane.normal[1]*batch->ent->axis[1][0] + oplane.normal[2]*batch->ent->axis[2][0];
		plane.normal[1] = oplane.normal[0]*batch->ent->axis[0][1] + oplane.normal[1]*batch->ent->axis[1][1] + oplane.normal[2]*batch->ent->axis[2][1];
		plane.normal[2] = oplane.normal[0]*batch->ent->axis[0][2] + oplane.normal[1]*batch->ent->axis[1][2] + oplane.normal[2]*batch->ent->axis[2][2];

		//rotate some point on the mesh around its entity's matrix
		point[0] = mesh->xyz_array[0][0]*batch->ent->axis[0][0] + mesh->xyz_array[0][1]*batch->ent->axis[1][0] + mesh->xyz_array[0][2]*batch->ent->axis[2][0] + batch->ent->origin[0];
		point[1] = mesh->xyz_array[0][0]*batch->ent->axis[0][1] + mesh->xyz_array[0][1]*batch->ent->axis[1][1] + mesh->xyz_array[0][2]*batch->ent->axis[2][1] + batch->ent->origin[1];
		point[2] = mesh->xyz_array[0][0]*batch->ent->axis[0][2] + mesh->xyz_array[0][1]*batch->ent->axis[1][2] + mesh->xyz_array[0][2]*batch->ent->axis[2][2] + batch->ent->origin[2];

		//now we can figure out the plane dist
		plane.dist = DotProduct(point, plane.normal);
	}

	*out = plane;
	if (outmesh)
		*outmesh = mesh;
	return true;
}

/*
  FTESurf Patch 206: the geometric half of the verdict -- would GLR_DrawPortal
  render a view through this batch, from the given view?

  Must stay in lockstep with the refusals at the top of GLR_DrawPortal; that is
  the whole point of it existing.  A caller that masks an aperture this says no
  to is writing a hole into the wall.

  `level` is passed rather than read from r_refdef because GLR_DrawPortal's own
  depth-mask loop runs with recurse ALREADY incremented while still using the
  outer view's matrices and origin -- so it has to ask about the outer level,
  and a function that quietly read the global would be off by one exactly where
  it matters.
*/
static qboolean GLR_PortalDrawableAt(batch_t *batch, int level)
{
	plane_t plane;
	float d;

	if (level >= R_MAX_RECURSE-1)
		return false;
	if (!GLR_PortalPlane(batch, &plane, NULL))
		return false;

	d = DotProduct(r_refdef.vieworg, plane.normal) - plane.dist;
	if ((batch->shader->flags & SHADER_AGEN_PORTAL) && d > batch->shader->portaldist)
		return false;
	if (d < -r_refdef.mindist)
		return false;
	return true;
}

/*
  FTESurf Patch 206: the per-view portal budget.

  Angular size is the metric because it is the one the eye uses.  A doorway
  filling a third of the screen is worth a scene render; the same doorway across
  the map, four pixels wide, is not -- and crucially, a portal seen THROUGH
  another portal is scored from the recursed eye, where it is large, which is
  what makes the next room's door render open while everything past it renders
  closed.  That is the behaviour asked for, and it falls out of the metric
  rather than being special-cased.

  One set per recursion level, because GLBE_SubmitMeshesPortals re-enters at
  every level and each level's budget is its own question.

  THE VERDICT IS CACHED, and that is a correctness requirement rather than an
  optimisation.  R_GAlias_DrawBatch points every alias batch at ONE file-static
  mesh_t (gl_alias.c:1934-1935) which describes that batch only between its own
  build and its own submit.  A portal's plane therefore cannot be recomputed
  later from batch->mesh -- it would read whichever aperture was built last, and
  eighteen doors would all answer with the geometry of one.  So each batch is
  built and judged in the same step, once, and every loop afterwards does a
  pointer lookup and touches no mesh at all.  That also deletes the quadratic
  buildmeshes traffic GLR_DrawPortal's mask loop used to generate.
*/
static int portalscenes;	//FTESurf Patch 206: scene renders this frame, every level
#define PORTALBUDGETMAX 16
#define PORTALTABLEMAX 256	//surf_tripportals has 116 doors; past this they render closed
static struct
{
	struct
	{
		batch_t	*batch;
		float	score;
		qboolean drawable;
		/* FTESurf Patch 208: the aperture's bounds on screen, in THIS view.
		   Measured in the same build-and-judge pass and for the same reason as
		   `score` -- see the header above -- and additionally because this pass
		   is the last moment before R_ObliqueNearClip rewrites
		   m_projection_std in place (gl_rmain.c, GLR_DrawPortal), so every
		   rectangle at a level comes from one consistent projection.
		   haverect false means "do not clip", never "cull". */
		qboolean haverect;
		srect_t	rect;
	} ent[PORTALTABLEMAX];
	int		numents;
	batch_t	*chosen[PORTALBUDGETMAX];
	int		count;
	qboolean limited;
} portalbudget[R_MAX_RECURSE];

/*
  FTESurf Patch 208: one aperture vertex, in world space.

  This was spelled out twice inside GLR_PortalAngularSize and Patch 208 needs it
  a third time, so it is a function.  The axis rows are not optional decoration:
  cl_portal.qc builds all 443 of the library's doors from ONE unit quad and puts
  each door's half-width and half-height into the axis rows as a per-axis scale
  (RF_USEAXIS), so the model-space vertex says nothing whatever about where the
  door is or how big it is.
*/
static void GLR_PortalVertex(batch_t *batch, mesh_t *mesh, int v, vec3_t out)
{
	if (batch->ent == &r_worldentity)
	{
		VectorCopy(mesh->xyz_array[v], out);
		return;
	}
	out[0] = mesh->xyz_array[v][0]*batch->ent->axis[0][0] + mesh->xyz_array[v][1]*batch->ent->axis[1][0] + mesh->xyz_array[v][2]*batch->ent->axis[2][0] + batch->ent->origin[0];
	out[1] = mesh->xyz_array[v][0]*batch->ent->axis[0][1] + mesh->xyz_array[v][1]*batch->ent->axis[1][1] + mesh->xyz_array[v][2]*batch->ent->axis[2][1] + batch->ent->origin[1];
	out[2] = mesh->xyz_array[v][0]*batch->ent->axis[0][2] + mesh->xyz_array[v][1]*batch->ent->axis[1][2] + mesh->xyz_array[v][2]*batch->ent->axis[2][2] + batch->ent->origin[2];
}

/*
  FTESurf Patch 208: the aperture's bounding rectangle on screen, in the current
  view, as srect_t fractions of r_refdef.pxrect (x from the left, y from the
  BOTTOM -- GLBE_Scissor's convention, not the 2d one).

  Modelled on Sh_ScissorForBox (gl_shadow.c), which is the engine's working
  example of this projection, with three deliberate differences.

  It walks the aperture's own VERTICES over the whole firstmesh..meshes range
  rather than a bounding box.  A world-space AABB of a yawed 88x88 door is far
  looser than its four corners, and the range must match what the depth mask
  actually submits (GLBE_SubmitBatch takes the whole range) -- a rectangle that
  excluded part of the masked quad would punch a fresh hole in the wall.

  It NEVER CULLS.  Sh_ScissorForBox returns "fully offscreen" and its callers
  skip the light; here the analogous answers all mean "do not clip", which is
  bit-for-bit the pre-Patch-208 behaviour.  In particular a vertex at or behind
  the near plane makes it decline outright instead of interpolating the crossing
  the way Sh_ScissorForBox does: after a w that has passed through zero, the
  0..1 clamp can produce a rectangle SMALLER than the true footprint, and a
  rectangle short at the aperture rim is Patch 206's transparent square back
  again as a one-pixel fringe -- appearing exactly while you walk through the
  door, which is the worst possible moment for it. Declining to clip while the
  eye is in the doorway is also just correct: from in there the aperture really
  does fill the view, and containment is the near clip's job.

  For the same reason the rectangle is padded by two pixels: it must be a strict
  SUPERSET of the aperture's rasterised footprint.

  dmin/dmax are pinned to 0 and 1 rather than derived. R_ObliqueNearClip
  (r_surf.c) rewrites only the z row of m_projection_std, so screen x and y stay
  trustworthy while any depth computed from it does not -- and GLBE_Scissor
  feeds dmin/dmax straight into qglDepthBoundsEXT.
*/
static qboolean GLR_PortalScreenRect(batch_t *batch, srect_t *out)
{
	mesh_t *mesh;
	vec3_t p;
	vec4_t v, tv;
	float ncpdist, x, y, x1, y1, x2, y2, padx, pady;
	int m, i;
	qboolean any = false;

	if (!r_portalscissor.ival)
		return false;
	if (!batch->mesh)
		return false;

	ncpdist = DotProduct(r_refdef.vieworg, vpn) + r_refdef.mindist;
	x1 = y1 = 1;
	x2 = y2 = -1;

	for (m = batch->firstmesh; m < batch->meshes; m++)
	{
		mesh = batch->mesh[m];
		if (!mesh || !mesh->xyz_array)
			continue;
		for (i = 0; i < mesh->numvertexes; i++)
		{
			GLR_PortalVertex(batch, mesh, i, p);

			if (ncpdist - DotProduct(p, vpn) > 0)
				return false;	//straddles the near plane: do not clip at all

			VectorCopy(p, v);
			v[3] = 1;
			Matrix4x4_CM_Transform4(r_refdef.m_view, v, tv);
			Matrix4x4_CM_Transform4(r_refdef.m_projection_std, tv, v);
			if (v[3] <= 0)
				return false;	//cannot happen after the test above; cheap insurance

			x = v[0] / v[3];
			y = v[1] / v[3];
			if (x < x1) x1 = x;
			if (x > x2) x2 = x;
			if (y < y1) y1 = y;
			if (y > y2) y2 = y;
			any = true;
		}
	}
	if (!any)
		return false;

	x1 = (1+x1)*0.5;
	x2 = (1+x2)*0.5;
	y1 = (1+y1)*0.5;
	y2 = (1+y2)*0.5;

	padx = (r_refdef.pxrect.width  > 0) ? 2.0f/r_refdef.pxrect.width  : 0;
	pady = (r_refdef.pxrect.height > 0) ? 2.0f/r_refdef.pxrect.height : 0;
	x1 -= padx;	x2 += padx;
	y1 -= pady;	y2 += pady;

	if (x1 < 0) x1 = 0;
	if (y1 < 0) y1 = 0;
	if (x2 > 1) x2 = 1;
	if (y2 > 1) y2 = 1;
	if (x2 <= x1 || y2 <= y1)
		return false;	//degenerate: do not clip

	out->x		= x1;
	out->y		= y1;
	out->width	= x2 - x1;
	out->height	= y2 - y1;
	out->dmin	= 0;
	out->dmax	= 1;
	return true;
}

//angular radius of the aperture as seen from the eye: size over distance.
static float GLR_PortalAngularSize(batch_t *batch)
{
	mesh_t *mesh;
	vec3_t centre, point, d;
	float radius = 0, dist, l;
	int i;

	if (!batch->mesh || batch->meshes == batch->firstmesh)
		return 0;
	mesh = batch->mesh[batch->firstmesh];
	if (!mesh || !mesh->xyz_array || !mesh->numvertexes)
		return 0;

	VectorClear(centre);
	for (i = 0; i < mesh->numvertexes; i++)
	{
		GLR_PortalVertex(batch, mesh, i, point);
		VectorAdd(centre, point, centre);
	}
	VectorScale(centre, 1.0/mesh->numvertexes, centre);

	for (i = 0; i < mesh->numvertexes; i++)
	{
		GLR_PortalVertex(batch, mesh, i, point);
		VectorSubtract(point, centre, d);
		l = DotProduct(d, d);
		if (l > radius)
			radius = l;
	}
	radius = sqrt(radius);

	VectorSubtract(centre, r_refdef.vieworg, d);
	dist = sqrt(DotProduct(d, d));
	if (dist < 1)
		dist = 1;	//standing in the doorway: as big as it gets
	return radius / dist;
}

//called once per GLBE_SubmitMeshesPortals, before either loop touches anything
void GLR_PortalBudgetBegin(batch_t **worldlist, batch_t *dynamiclist)
{
	int level = r_refdef.recurse;
	int limit = r_portalmaxviews.ival;
	batch_t *batch;
	int i, j, k;

	if (level < 0 || level >= R_MAX_RECURSE)
		return;
	portalbudget[level].numents = 0;
	portalbudget[level].count = 0;
	portalbudget[level].limited = false;

	/* FTESurf Patch 206: nothing at all while the view is outside the world.

	   A portal recursed from the void is meaningless twice over -- its PVS
	   sample comes from a cluster that does not exist, so the far side renders
	   as garbage or as nothing -- and it is not cheap meaninglessness: each one
	   is still a full scene.  Noclipping out through the ceiling of
	   surf_kitsune therefore paid for eighteen scene renders per frame to look
	   at eighteen rectangles of nothing.

	   r_viewcluster == -1 is the engine's own "outside the world" (r_surf.c:2446,
	   and the same test r_voidvis is built on).  Gating on the cluster rather
	   than on pmovetype covers every way of getting there, and deliberately does
	   NOT consult r_voidvis, which is a separate feature about what the WORLD
	   draws and which ftesurf.cfg has switched off. */
	if (r_viewcluster == -1)
	{
		portalbudget[level].limited = true;	//nothing recorded: nothing qualifies
		return;
	}

	portalbudget[level].limited = true;
	if (limit <= 0 || limit > PORTALBUDGETMAX)
		limit = PORTALBUDGETMAX;	//"unlimited" still cannot exceed the table

	/* Pass one: build and judge in the same step, and remember the answer.  This
	   is the only place batch->mesh may be read for a portal, for the reason in
	   the header above. */
	for (i = 0; i < 2; i++)
		for (batch = i?dynamiclist:worldlist[SHADER_SORT_PORTAL]; batch; batch = batch->next)
		{
			if (portalbudget[level].numents >= PORTALTABLEMAX)
				break;
			if (batch->buildmeshes)
				batch->buildmeshes(batch);
			j = portalbudget[level].numents++;
			portalbudget[level].ent[j].batch = batch;
			portalbudget[level].ent[j].drawable = GLR_PortalDrawableAt(batch, level);
			portalbudget[level].ent[j].score =
				portalbudget[level].ent[j].drawable ? GLR_PortalAngularSize(batch) : 0;
			//FTESurf Patch 208: measured here and nowhere else, for the two
			//reasons in the struct comment -- the shared alias mesh_t, and the
			//projection that R_ObliqueNearClip is about to rewrite.
			portalbudget[level].ent[j].haverect =
				portalbudget[level].ent[j].drawable &&
				GLR_PortalScreenRect(batch, &portalbudget[level].ent[j].rect);
		}

	/* Pass two: keep the largest `limit` of them.  Selection sort over at most
	   sixteen kept entries -- the table can be hundreds long but the kept set
	   never is, so this is linear in the table and trivial in the limit. */
	for (k = 0; k < limit; k++)
	{
		int best = -1;
		for (j = 0; j < portalbudget[level].numents; j++)
		{
			if (!portalbudget[level].ent[j].drawable)
				continue;
			if (best < 0 || portalbudget[level].ent[j].score > portalbudget[level].ent[best].score)
			{
				//skip ones already taken
				for (i = 0; i < portalbudget[level].count; i++)
					if (portalbudget[level].chosen[i] == portalbudget[level].ent[j].batch)
						break;
				if (i == portalbudget[level].count)
					best = j;
			}
		}
		if (best < 0)
			break;
		portalbudget[level].chosen[portalbudget[level].count++] =
			portalbudget[level].ent[best].batch;
	}

	/* Instrument the CONSUMER, not the producer.  "18 doors on this map" is what
	   the loader believes; what decides the frame is how many portal batches
	   survived culling into THIS view and how many of those got a scene, and
	   until this line said so both numbers were guesses.  Change-triggered, per
	   recursion level, developer only. */
	{
		static int lastcand[R_MAX_RECURSE], lastdrawn[R_MAX_RECURSE];
		static int lastreported = -1;
		int cand = 0;
		for (j = 0; j < portalbudget[level].numents; j++)
			if (portalbudget[level].ent[j].drawable)
				cand++;

		/* The per-FRAME total is the number that answers "is this the lag", and
		   it is not any one level's count -- it is the whole tree.  Level 0
		   starting again means the previous frame is finished and countable. */
		if (!level)
		{
			if (portalscenes != lastreported)
			{
				lastreported = portalscenes;
				Con_DPrintf("portals: %i scene renders last frame\n", portalscenes);
			}
			portalscenes = 0;
		}

		if (cand != lastcand[level] || portalbudget[level].count != lastdrawn[level])
		{
			lastcand[level] = cand;
			lastdrawn[level] = portalbudget[level].count;
			Con_DPrintf("portals: depth %i, %i in view, %i rendered, %i closed\n",
				level, cand, portalbudget[level].count, cand - portalbudget[level].count);
		}
	}
}

/*
  The full verdict: geometry, then the budget.  Every loop that renders a portal
  or masks one asks this, so all of them agree -- which is the invariant Patch
  206 exists to restore.  A pure table lookup: it must not touch batch->mesh,
  because by the time most callers ask, the shared alias mesh_t describes
  somebody else.
*/
qboolean GLR_PortalWouldDrawAt(batch_t *batch, int level)
{
	int i;
	if (level < 0 || level >= R_MAX_RECURSE || !portalbudget[level].limited)
		return GLR_PortalDrawableAt(batch, level);
	for (i = 0; i < portalbudget[level].count; i++)
		if (portalbudget[level].chosen[i] == batch)
			return true;
	return false;
}

qboolean GLR_PortalWouldDraw(batch_t *batch)
{
	return GLR_PortalWouldDrawAt(batch, r_refdef.recurse);
}

/*
  FTESurf Patch 208: the aperture's screen rectangle, by lookup.

  Same contract as GLR_PortalWouldDrawAt and for the same reason: a pure table
  read that touches no mesh, because by the time this is asked the shared alias
  mesh_t describes whichever batch was built last.  Returning false means "do
  not clip" -- the pre-Patch-208 full-screen render -- and never "cull".
*/
static qboolean GLR_PortalScreenRectAt(batch_t *batch, int level, srect_t *out)
{
	int i;
	if (level < 0 || level >= R_MAX_RECURSE || !portalbudget[level].limited)
		return false;
	for (i = 0; i < portalbudget[level].numents; i++)
	{
		if (portalbudget[level].ent[i].batch != batch)
			continue;
		if (!portalbudget[level].ent[i].haverect)
			return false;
		*out = portalbudget[level].ent[i].rect;
		return true;
	}
	return false;
}

qboolean GLR_DrawPortal(batch_t *batch, batch_t **blist, batch_t *depthmasklist[2], int portaltype)
{
	entity_t *view, *surfent;
//	GLdouble glplane[4];
	plane_t plane, oplane;
	float vmat[16];
	refdef_t oldrefdef;
	vec3_t r;
	int i;
	mesh_t *mesh;
	pvsbuffer_t newvis;
	float ivmat[16], trmat[16];

	if (!GLR_PortalPlane(batch, &plane, &mesh))
		return false;

	//if we're too far away from the surface, don't draw anything
	if (batch->shader->flags & SHADER_AGEN_PORTAL)
	{
		/*there's a portal alpha blend on that surface, that fades out after this distance*/
		if (DotProduct(r_refdef.vieworg, plane.normal)-plane.dist > batch->shader->portaldist)
			return false;
	}
	/* ---- FTESurf Patch 206: THIS is the transparent square -------------------

	   Every refusal in this function -- no mesh, no xyz_array, the
	   alphagen-portal distance above, and the behind-the-plane test below -- was
	   invisible to the caller, which went on to write the aperture into the
	   depth buffer regardless.  A depth mask with no scene rendered behind it
	   does not hide the portal, it deletes the WALL: the world is rejected
	   there, and since r_clear defaults to 0 and nothing clears colour inside a
	   recursion, what shows through is the PREVIOUS FRAME.  Smeary, not black,
	   which is why it reads as "a square transparency".

	   It bit here and not upstream because a world portal surface is split by
	   SURF_PLANEBACK at load (gl_model.c:3653-3656) and its back side is a
	   different batch that gets culled.  An alias-model aperture with `cull
	   none` is in the list from BOTH sides, so on a map of paired doors roughly
	   half of them are behind you at any instant, and each punched an 88x88 hole
	   in the wall it was set into.

	   Report the refusal.  The caller then masks only what it actually drew. */
	//if we're behind it, then also don't draw anything. for our purposes, behind is when the entire near clipplane is behind.
	if (DotProduct(r_refdef.vieworg, plane.normal)-plane.dist < -r_refdef.mindist)
		return false;

	if (r_refdef.recurse >= R_MAX_RECURSE-1)
	{	/* Out of recursion.  BEM_DEPTHDARK paints the surface black for a shader
		   that has passes and draws nothing at all for a zero-pass portal
		   shader; either way no view was rendered, so it must not be masked. */
		/* FTESurf Patch 210: ...and nothing at all for a SHADER_NODRAW one.  That
		   is the r_portalfbo 0 fallback, whose material keeps its $refraction pass
		   so the FBO route can use it when the cvar is on; painting that pass here
		   would show a stale portal image from some other aperture.  Every other
		   submit path already honours NODRAW; this one, being ours, did not. */
		if (!(batch->shader->flags & SHADER_NODRAW))
		{
			GLBE_SelectMode(BEM_DEPTHDARK);
			GLBE_SubmitBatch(batch);
			GLBE_SelectMode(BEM_STANDARD);
		}
		return false;
	}


	TRACE(("GLR_DrawPortal: portal type %i\n", portaltype));

	oldrefdef = r_refdef;
	r_refdef.recurse+=1;

	r_refdef.externalview = true;

	switch(portaltype)
	{
	case 1: /*fbo explicit mirror (fucked depth, working clip plane)*/
		//fixme: pvs is surely wrong?
//		r_refdef.flipcull ^= SHADER_CULL_FLIP;
		R_MirrorMatrix(&plane);
		Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);

		VectorCopy(mesh->xyz_array[0], r_refdef.pvsorigin);
		for (i = 1; i < mesh->numvertexes; i++)
			VectorAdd(r_refdef.pvsorigin, mesh->xyz_array[i], r_refdef.pvsorigin);
		VectorScale(r_refdef.pvsorigin, 1.0/mesh->numvertexes, r_refdef.pvsorigin);
		break;
	
	case 2:	/*fbo refraction (fucked depth, working clip plane)*/
	case 3:	/*screen copy refraction (screen depth, fucked clip planes)*/
		/*refraction image (same view, just with things culled*/
		r_refdef.externalview = oldrefdef.externalview;
		VectorNegate(plane.normal, plane.normal);
		plane.dist = -plane.dist;

		//use the player's origin for r_viewleaf, because there's not much we can do anyway*/
		VectorCopy(r_origin, r_refdef.pvsorigin);

		if (cl.worldmodel && cl.worldmodel->funcs.ClusterPVS && !r_novis.ival)
		{
			int clust, i, j;
			float d;
			vec3_t point;
			r_refdef.forcevis = false;
			r_refdef.forcedvis = NULL;
			newvis.buffer = alloca(newvis.buffersize=cl.worldmodel->pvsbytes);
			for (i = batch->firstmesh; i < batch->meshes; i++)
			{
				mesh = batch->mesh[i];
				if (!mesh->xyz_array || !mesh->numvertexes)	//nettest: skip degenerate meshes — numvertexes==0 would div-by-zero the centroid (VectorScale 1/numvertexes) below and leave forcedvis NULL with forcevis set
					continue;
				r_refdef.forcevis = true;
				VectorClear(point);
				for (j = 0; j < mesh->numvertexes; j++)
					VectorAdd(point, mesh->xyz_array[j], point);
				VectorScale(point, 1.0f/mesh->numvertexes, point);
				d = DotProduct(point, plane.normal) - plane.dist;
				d += 0.1;	//an epsilon on the far side
				VectorMA(point, d, plane.normal, point);

				clust = cl.worldmodel->funcs.ClusterForPoint(cl.worldmodel, point, NULL);
				if (i == batch->firstmesh)
					r_refdef.forcedvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, clust, &newvis, PVM_REPLACE);
				else
					r_refdef.forcedvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, clust, &newvis, PVM_MERGE);
			}
//			memset(newvis, 0xff, pvsbytes);
		}
		Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);
		break;

	case 0:		/*q3 portal*/
	default:
#ifdef CSQC_DAT
		if (CSQC_SetupToRenderPortal(batch->ent->keynum))
		{
			oplane = plane;

			//transform the old surface plane into the new view matrix
			Matrix4_Invert(r_refdef.m_view, ivmat);
			Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);
			Matrix4_Multiply(ivmat, vmat, trmat);
			plane.normal[0] = -(oplane.normal[0] * trmat[0] + oplane.normal[1] * trmat[1] + oplane.normal[2] * trmat[2]);
			plane.normal[1] = -(oplane.normal[0] * trmat[4] + oplane.normal[1] * trmat[5] + oplane.normal[2] * trmat[6]);
			plane.normal[2] = -(oplane.normal[0] * trmat[8] + oplane.normal[1] * trmat[9] + oplane.normal[2] * trmat[10]);
			plane.dist = -oplane.dist + trmat[12]*oplane.normal[0] + trmat[13]*oplane.normal[1] + trmat[14]*oplane.normal[2];

			if (Cvar_Get("temp_useplaneclip", "1", 0, "temp")->ival)
				portaltype = 1;	//make sure the near clipplane is used.
			break;
		}
#endif
		surfent = batch->ent;
		if (batch->ent->keynum)
			view = R_FindPortalCamera(batch->ent);
		else
		{
			view = R_NearestPortal(&plane);
			if (view)
			{	//for q3bsps where the portal surface is embedded in the bsp itself, we need an extra leyer of indirection.
				entity_t *oc = R_FindPortalCamera(view);
				if(oc)
				{
					surfent = view;
					view = oc;
				}
			}
		}

		if (view && view->rtype == RT_PORTALCAMERA)
		{	//q1-style portal, where the portal is defined via attachments
			//the portal plane itself is assumed to be facing directly forwards from the entity that we're drawing, and with the same origin.
			oplane = plane;

			TransformCoord(r_refdef.vieworg, surfent->axis, surfent->origin, view->axis, view->origin, r_refdef.vieworg);
			TransformDir(vpn, surfent->axis, view->axis, vpn);
			TransformDir(vright, surfent->axis, view->axis, vright);
			TransformDir(vup, surfent->axis, view->axis, vup);

			//transform the old surface plane into the new view matrix
			Matrix4_Invert(r_refdef.m_view, ivmat);
			Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);
			Matrix4_Multiply(ivmat, vmat, trmat);
			plane.normal[0] = -(oplane.normal[0] * trmat[0] + oplane.normal[1] * trmat[1] + oplane.normal[2] * trmat[2]);
			plane.normal[1] = -(oplane.normal[0] * trmat[4] + oplane.normal[1] * trmat[5] + oplane.normal[2] * trmat[6]);
			plane.normal[2] = -(oplane.normal[0] * trmat[8] + oplane.normal[1] * trmat[9] + oplane.normal[2] * trmat[10]);
			plane.dist = -oplane.dist + trmat[12]*oplane.normal[0] + trmat[13]*oplane.normal[1] + trmat[14]*oplane.normal[2];

			portaltype = 1;	//make sure the near clipplane is used.
			break;
		}

		//portal surfaces with the same origin+oldorigin are explicit mirrors, and skipped in this case.
		if (view && view->rtype == RT_PORTALSURFACE && !VectorCompare(view->origin, view->oldorigin))
		{	//q3-style portal, where a single entity provides orientation+two origins
			float d;
			vec3_t paxis[3], porigin, vaxis[3], vorg;

			oplane = plane;

			/*calculate where the surface is meant to be*/
			VectorCopy(mesh->normals_array[0], paxis[0]);
			PerpendicularVector(paxis[1], paxis[0]);
			CrossProduct(paxis[0], paxis[1], paxis[2]);
			d = DotProduct(view->origin, plane.normal) - plane.dist;
			VectorMA(view->origin, -d, paxis[0], porigin);

			/*grab the camera origin*/
			VectorNegate(view->axis[0], vaxis[0]);
			VectorNegate(view->axis[1], vaxis[1]);
			VectorCopy(view->axis[2], vaxis[2]);
			VectorCopy(view->oldorigin, vorg);

			VectorCopy(vorg, r_refdef.pvsorigin);

			/*rotate it a bit*/
			if (view->framestate.g[FS_REG].frame[1])	//oldframe
			{
				if (view->framestate.g[FS_REG].frame[0])	//newframe
					d = realtime * view->framestate.g[FS_REG].frame[0];	//newframe
				else
					d = view->skinnum + sin(realtime)*4;
			}
			else
				d = view->skinnum;

			if (d)
			{
				vec3_t rdir;
				VectorCopy(vaxis[1], rdir);
				RotatePointAroundVector(vaxis[1], vaxis[0], rdir, d);
				CrossProduct(vaxis[0], vaxis[1], vaxis[2]);
			}

			TransformCoord(oldrefdef.vieworg, paxis, porigin, vaxis, vorg, r_refdef.vieworg);
			TransformDir(vpn, paxis, vaxis, vpn);
			TransformDir(vright, paxis, vaxis, vright);
			TransformDir(vup, paxis, vaxis, vup);
			Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);

			//transform the old surface plane into the new view matrix
			if (Matrix4_Invert(r_refdef.m_view, ivmat))
			{
				Matrix4_Multiply(ivmat, vmat, trmat);
				plane.normal[0] = -(oplane.normal[0] * trmat[0] + oplane.normal[1] * trmat[1] + oplane.normal[2] * trmat[2]);
				plane.normal[1] = -(oplane.normal[0] * trmat[4] + oplane.normal[1] * trmat[5] + oplane.normal[2] * trmat[6]);
				plane.normal[2] = -(oplane.normal[0] * trmat[8] + oplane.normal[1] * trmat[9] + oplane.normal[2] * trmat[10]);
				plane.dist = -oplane.dist + trmat[12]*oplane.normal[0] + trmat[13]*oplane.normal[1] + trmat[14]*oplane.normal[2];
				portaltype = 1;
			}
			break;
		}
		//fixme: q3 gamecode has explicit mirrors. we 'should' just ignore the surface if we've not seen it yet.

		//a portal with no portal entity, or a portal rentity with an origin equal to its oldorigin, is a mirror.
		R_MirrorMatrix(&plane);
		Matrix4x4_CM_ModelViewMatrixFromAxis(vmat, vpn, vright, vup, r_refdef.vieworg);

		VectorCopy(mesh->xyz_array[0], r_refdef.pvsorigin);
		for (i = 1; i < mesh->numvertexes; i++)
			VectorAdd(r_refdef.pvsorigin, mesh->xyz_array[i], r_refdef.pvsorigin);
		VectorScale(r_refdef.pvsorigin, 1.0/mesh->numvertexes, r_refdef.pvsorigin);

		portaltype = 1;
		break;
	}

	/*FIXME: can we get away with stenciling the screen?*/
	/*Add to frustum culling instead of clip planes?*/
/*	if (qglClipPlane && portaltype)
	{
		GLdouble glplane[4];
		glplane[0] = plane.normal[0];
		glplane[1] = plane.normal[1];
		glplane[2] = plane.normal[2];
		glplane[3] = plane.dist;
		qglClipPlane(GL_CLIP_PLANE0, glplane);
		qglEnable(GL_CLIP_PLANE0);
	}
*/	//fixme: we can probably scissor a smaller frusum
	R_SetFrustum (r_refdef.m_projection_std, vmat);
	if (r_refdef.frustum_numplanes < MAXFRUSTUMPLANES)
	{
		extern int SignbitsForPlane (mplane_t *out);
		mplane_t fp;
		VectorCopy(plane.normal, fp.normal);
		fp.dist = plane.dist;

//		if (DotProduct(fp.normal, vpn) < 0)
//		{
//			VectorNegate(fp.normal, fp.normal);
//			fp.dist *= -1;
//		}

		fp.type = PLANE_ANYZ;
		fp.signbits = SignbitsForPlane (&fp);

		if (portaltype == 1 || portaltype == 2)
			R_ObliqueNearClip(vmat, &fp);

		//our own culling should be an epsilon forwards so we don't still draw things behind the line due to precision issues.
		fp.dist += 0.01;
		r_refdef.frustum[r_refdef.frustum_numplanes++] = fp;
	}
#if 1
	if (depthmasklist && r_portaldebug.ival != 3)
	{
		/*draw already-drawn portals as depth-only, to ensure that their contents are not harmed*/
		/*we can only do this AFTER the oblique perspective matrix is calculated, to avoid depth inconsistancies, while we still have the old view matrix*/
		int i;
		batch_t *dmask = NULL;
		if (qglLoadMatrixf)
		{
			qglMatrixMode(GL_PROJECTION);
			qglLoadMatrixf(r_refdef.m_projection_std);
			qglMatrixMode(GL_MODELVIEW);
		}
		//portals to mask are relative to the old view still.
		currententity = NULL;
		if (gl_config.arb_depth_clamp)
			qglEnable(GL_DEPTH_CLAMP_ARB);	//ignore the near clip plane(ish), this means nearer portals can still mask further ones.
		GL_ForceDepthWritable();
		GLBE_SelectMode(BEM_DEPTHONLY);
		for (i = 0; i < 2; i++)
		{
			for (dmask = depthmasklist[i]; dmask; dmask = dmask->next)
			{
				if (dmask == batch)
					continue;
//				if (dmask->meshes == dmask->firstmesh)
//					continue;
				/* FTESurf Patch 206: mask only the portals that are themselves
				   being rendered in the OUTER view (this loop still runs under
				   the old matrices, which is why it is here and not later).

				   The comment above calls this list "already-drawn portals"; it
				   is not, it is every portal.  Masking one that will never be
				   drawn stencils its aperture out of this scene AND out of every
				   other portal's scene, so a failed aperture cannot be painted
				   by anything at all -- it is locked open as a hole through all
				   eighteen passes.  That is the second half of the transparent
				   square, and it is why the symptom survived being looked at
				   from several angles.

				   oldrefdef.recurse, not r_refdef.recurse: this loop runs after
				   the increment but still under the outer view. */
				if (!GLR_PortalWouldDrawAt(dmask, oldrefdef.recurse))
					continue;
				/* FTESurf Patch 203: build it first.  This loop submits the OTHER
				   portal batches, which nothing has built -- an alias batch's mesh
				   is NULL until buildmeshes runs, and GLBE_SubmitBatch's no-vbo
				   path dereferences mesh[0] immediately (gl_backend.c:5728).  That
				   is the crash: an alias-model portal took the renderer out on the
				   first frame one was visible.

				   Rebuilding rather than merely skipping also fixes what would
				   otherwise be a silent wrong answer.  R_GAlias_DrawBatch hands
				   every alias batch a pointer to ONE file-static mesh_t
				   (gl_alias.c:1934-1935, 1967) which is only valid between its own
				   build and its own submit -- so eighteen already-built portal
				   batches would all describe whichever was built last, and the
				   depth mask would cover one doorway eighteen times.
				   Build-then-submit in step is the discipline
				   GLBE_SubmitMeshesSortList already keeps for the same reason. */
				if (dmask->buildmeshes)
					dmask->buildmeshes(dmask);
				if (!dmask->mesh || dmask->meshes == dmask->firstmesh)
					continue;
				GLBE_SubmitBatch(dmask);
			}
		}
		GLBE_SelectMode(BEM_STANDARD);
		if (gl_config.arb_depth_clamp)
			qglDisable(GL_DEPTH_CLAMP_ARB);

		currententity = NULL;
	}
#endif
//	r_refdef = oldrefdef;
//	return;

	//now determine the stuff the backend will use.
	memcpy(r_refdef.m_view, vmat, sizeof(float)*16);
	VectorAngles(vpn, vup, r_refdef.viewangles, false);
	VectorCopy(r_refdef.vieworg, r_origin);

	//determine r_refdef.flipcull & SHADER_CULL_FLIP based upon whether right is right or not.
	CrossProduct(vpn, vup, r);
	if (DotProduct(r, vright) < 0)
		r_refdef.flipcull |= SHADER_CULL_FLIP;
	else
		r_refdef.flipcull &= ~SHADER_CULL_FLIP;
	if (r_refdef.m_projection_std[5]<0)
		r_refdef.flipcull ^= SHADER_CULL_FLIP;

	/* ---- FTESurf Patch 208: CONTAINMENT ---------------------------------------

	   Below this line the recursed frame may only touch the aperture's own
	   pixels.  This is the answer to "the portal covers the whole wall, and not
	   the doorway".

	   Measured rather than reasoned: r_portaldebug 1 skips exactly the
	   R_RenderScene below and nothing else, and on surf_kitsune's exit doorway
	   that alone brings the entire missing wall back.  So the far scene's colour
	   IS the thing covering the wall.  FTE paints it over the whole screen --
	   there is no scissor and no stencil anywhere in this path, only the FIXME
	   further up this function -- and trusts SHADER_SORT_OPAQUE, which comes
	   later, to repaint everywhere except the hole.  Where that repaint does not
	   happen, the far room keeps the wall.  Clipping the far scene to the
	   doorway makes the question moot instead of answering it, which is the
	   honest description of this patch: it CONTAINS the repaint bug rather than
	   curing it, and r_portalscissor 0 is how you get the raw symptom back.

	   A rectangle is a bounding box, so a yawed door leaves the corners of its
	   projected trapezoid reachable.  Accepted deliberately: a stencil would be
	   exact but this backend tracks neither scissor nor stencil state
	   (gl_shadow.c says so), and stencil shadows contend for the buffer inside
	   this very recursion.

	   depthmasklist is the gate, and it is exact rather than approximate: of
	   GLR_DrawPortal's five call sites only GLBE_SubmitMeshesPortals passes a
	   list; the four GLBE_GenerateBatchTextures FBO paths pass NULL.  So the
	   clip arms only on the main framebuffer -- the path with the leak, and the
	   only one whose portalbudget table was built for this view, so a stale
	   table can never be consulted. */
	if (depthmasklist)
	{
		srect_t ap;
		if (GLR_PortalScreenRectAt(batch, oldrefdef.recurse, &ap))
		{
			if (oldrefdef.portalclip)
			{	/*a portal seen THROUGH a portal: narrow, never widen.  Both
				  rectangles are in the same screen space because R_SetupGL's
				  viewport block is gated on !r_refdef.recurse, so pxrect is
				  still the outermost view's.*/
				float x2 = ap.x + ap.width, y2 = ap.y + ap.height;
				if (x2 > oldrefdef.portalcliprect[0] + oldrefdef.portalcliprect[2])
					x2 = oldrefdef.portalcliprect[0] + oldrefdef.portalcliprect[2];
				if (y2 > oldrefdef.portalcliprect[1] + oldrefdef.portalcliprect[3])
					y2 = oldrefdef.portalcliprect[1] + oldrefdef.portalcliprect[3];
				if (ap.x < oldrefdef.portalcliprect[0]) ap.x = oldrefdef.portalcliprect[0];
				if (ap.y < oldrefdef.portalcliprect[1]) ap.y = oldrefdef.portalcliprect[1];
				ap.width  = (x2 > ap.x) ? x2 - ap.x : 0;
				ap.height = (y2 > ap.y) ? y2 - ap.y : 0;
			}
			r_refdef.portalclip = true;
			r_refdef.portalcliprect[0] = ap.x;
			r_refdef.portalcliprect[1] = ap.y;
			r_refdef.portalcliprect[2] = ap.width;
			r_refdef.portalcliprect[3] = ap.height;
			r_refdef.portalclippx[0] = r_refdef.pxrect.x;
			r_refdef.portalclippx[1] = r_refdef.pxrect.y;
			r_refdef.portalclippx[2] = r_refdef.pxrect.width;
			r_refdef.portalclippx[3] = r_refdef.pxrect.maxheight;
		}
		//else: no measurable rectangle for this door, so this view inherits
		//whatever the parent had -- which for the outermost level is nothing,
		//i.e. exactly the old behaviour.
		BE_Scissor(NULL);	//resolves against r_refdef; see GLBE_ApplyScissor
	}

	Surf_SetupFrame();
	//FIXME: just call Surf_DrawWorld instead?
	if (r_portaldebug.ival != 1)	//P206 bisect: aperture drawn, nothing rendered through it
		R_RenderScene();
//	if (qglClipPlane)
//		qglDisable(GL_CLIP_PLANE0);

	if (r_portaldrawplanes.ival)
	{
		//the front of the plane should generally point away from the camera, and will be drawn in bright green. woo
		CL_DrawDebugPlane(plane.normal, plane.dist+0.01, 0.0, 0.5, 0.0, false);
		CL_DrawDebugPlane(plane.normal, plane.dist-0.01, 0.0, 0.5, 0.0, false);
		//the back of the plane points towards the camera, and will be drawn in blue, for the luls
		VectorNegate(plane.normal, plane.normal);
		plane.dist *= -1;
		CL_DrawDebugPlane(plane.normal, plane.dist+0.01, 0.0, 0.0, 0.2, false);
		CL_DrawDebugPlane(plane.normal, plane.dist-0.01, 0.0, 0.0, 0.2, false);
	}


	r_refdef = oldrefdef;
	/* FTESurf Patch 208: restore THEN resolve, in that order -- the restore is
	   what puts the parent's clip (or none) back in r_refdef, and this call is
	   what makes the GL state agree with it again.  There is no early return
	   between the arming block above and here. */
	if (depthmasklist)
		BE_Scissor(NULL);

	/*broken stuff*/
	AngleVectors (r_refdef.viewangles, vpn, vright, vup);
	VectorCopy (r_refdef.vieworg, r_origin);

	GLBE_SelectEntity(&r_worldentity);

	GL_CullFace(0);//make sure flipcull reversion takes effect

	TRACE(("GLR_DrawPortal: portal drawn\n"));

#ifdef warningmsg
#pragma warningmsg("warning: there's a bug with rtlights in portals, culling is broken or something. May also be loading the wrong matrix")
#endif
	currententity = NULL;
	portalscenes++;	//P206: counted where it happened, not where it was intended
	return true;
}


/*
=============
R_Clear
=============
*/
int gldepthfunc = GL_LEQUAL;
qboolean depthcleared;
void R_Clear (qboolean fbo)
{
	/*tbh, this entire function should be in the backend*/
	{
		if (!depthcleared || fbo)
		{
			GL_ForceDepthWritable();
			//we no longer clear colour here. we only ever (need to) do that at the start of the frame, and this point can be called multiple times per frame.
			//for performance, we clear the depth at the same time we clear colour, so we can skip clearing depth here the first time around each frame.
			//but for multiple scenes, we do need to clear depth still.
			//fbos always get cleared depth, just in case (colour fbos may contain junk, but hey).
			if ((fbo && r_clear.ival) || r_refdef.stereomethod==STEREO_RED_BLUE||r_refdef.stereomethod==STEREO_RED_GREEN)
			{
				qglClearColor(0, 0, 0, 1);
				qglClear (GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
			}
			else
				qglClear (GL_DEPTH_BUFFER_BIT);
		}
		if (!fbo)
			depthcleared = false;
		gldepthmin = 0;
		gldepthmax = 1;
		gldepthfunc=GL_LEQUAL;
	}
}

#if 0
void GLR_SetupFog (void)
{
	if (r_viewleaf)// && r_viewcontents != FTECONTENTS_EMPTY)
	{
		//	static fogcolour;
		float fogcol[4]={0};
		float fogperc;
		float fogdist;

		fogperc=0;
		fogdist=512;
		switch(r_viewcontents)
		{
		case FTECONTENTS_WATER:
			fogcol[0] = 64/255.0;
			fogcol[1] = 128/255.0;
			fogcol[2] = 192/255.0;
			fogperc=0.2;
			fogdist=512;
			break;
		case FTECONTENTS_SLIME:
			fogcol[0] = 32/255.0;
			fogcol[1] = 192/255.0;
			fogcol[2] = 92/255.0;
			fogperc=1;
			fogdist=256;
			break;
		case FTECONTENTS_LAVA:
			fogcol[0] = 192/255.0;
			fogcol[1] = 32/255.0;
			fogcol[2] = 64/255.0;
			fogperc=1;
			fogdist=128;
			break;
		default:
			fogcol[0] = 192/255.0;
			fogcol[1] = 192/255.0;
			fogcol[2] = 192/255.0;
			fogperc=1;
			fogdist=1024;
			break;
		}
		if (fogperc)
		{
			qglFogi(GL_FOG_MODE, GL_LINEAR);
			qglFogfv(GL_FOG_COLOR, fogcol);
			qglFogf(GL_FOG_DENSITY, fogperc);
			qglFogf(GL_FOG_START, 1);
			qglFogf(GL_FOG_END, fogdist);
			qglEnable(GL_FOG);
		}
	}
}
#endif

static void R_RenderMotionBlur(void)
{
#if !defined(ANDROID)
	int vwidth = 1, vheight = 1;
	float vs, vt, cs, ct;
	shader_t *shader;

	//figure out the size of our texture.
	if (sh_config.texture_non_power_of_two_pic)
	{	//we can use any size, supposedly
		vwidth = vid.pixelwidth;
		vheight = vid.pixelheight;
	}
	else
	{	//limit the texture size to square and use padding.
		while (vwidth < vid.pixelwidth)
			vwidth *= 2;
		while (vheight < vid.pixelheight)
			vheight *= 2;
	}

	//blend the last frame onto the scene
	//the maths is because our texture is over-sized (must be power of two)
	cs = vs = (float)vid.pixelwidth / vwidth * 0.5;
	ct = vt = (float)vid.pixelheight / vheight * 0.5;
	vs *= gl_motionblurscale.value;
	vt *= gl_motionblurscale.value;

	//render using our texture
	shader = R_RegisterShader("postproc_motionblur", SUF_NONE,
		"{\n"
			"program default2d\n"
			"{\n"
				"map $sourcecolour\n"
				"blendfunc blend\n"
			"}\n"
		"}\n"
		);
	GLBE_FBO_Sources(sceneblur_texture, r_nulltex);
	R2D_ImageColours(1, 1, 1, gl_motionblur.value);
	R2D_Image(0, 0, vid.width, vid.height, cs-vs, ct+vt, cs+vs, ct-vt, shader);
	GLBE_RenderToTextureUpdate2d(false);

	//grab the current image so we can feed that back into the next frame.
	GL_MTBind(0, GL_TEXTURE_2D, sceneblur_texture);
	//copy the image into the texture so that we can play with it next frame too!
	qglCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, vwidth, vheight, 0);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#endif
}

#if 1
#include "shader.h"
/*FIXME: we could use geometry shaders to draw to all 6 faces at once*/
qboolean R_RenderScene_Cubemap(void)
{
	int cmapsize = 512;
	int i;
	static vec3_t ang[6] =
				{	{0, -90, 0}, {0, 90, 0},
					{90, 0, 0}, {-90, 0, 0},
					{0, 0, 0}, {0, -180, 0}	};
	vec3_t saveang;
	vec3_t saveorg;

	vrect_t vrect;
	pxrect_t prect;

	shader_t *shader;
	int facemask;
	extern cvar_t r_projection;
	int oldfbo = -1;
	qboolean usefbo = true;		//this appears to be a 20% speedup in my tests.
	qboolean fboreset = false;
	int osm = r_refdef.stereomethod;

	/*needs glsl*/
	if (!gl_config.arb_shader_objects)
		return false;

	if (!*ffov.string || !strcmp(ffov.string, "0"))
		ffov.value = scr_fov.value;

	facemask = 0;
	switch(r_projection.ival)
	{
	default:	//invalid.
		return false;
	case PROJ_STEREOGRAPHIC:
		shader = R_RegisterShader("postproc_stereographic", SUF_NONE,
				"{\n"
					"program postproc_stereographic\n"
					"{\n"
						"map $sourcecube\n"
					"}\n"
				"}\n"
				);

		facemask |= 1<<4; /*front view*/
		if (ffov.value > 70)
		{
			facemask |= (1<<0) | (1<<1); /*side/top*/
			if (ffov.value > 85)
				facemask |= (1<<2) | (1<<3); /*bottom views*/
			if (ffov.value > 300)
				facemask |= 1<<5; /*back view*/
		}
		break;
	case PROJ_FISHEYE:
		shader = R_RegisterShader("postproc_fisheye", SUF_NONE,
				"{\n"
					"program postproc_fisheye\n"
					"{\n"
						"map $sourcecube\n"
					"}\n"
				"}\n"
				);

		//fisheye view sees up to a full sphere
		facemask |= 1<<4; /*front view*/
		if (ffov.value > 77)
			facemask |= (1<<0) | (1<<1) | (1<<2) | (1<<3); /*side/top/bottom views*/
		if (ffov.value > 270)
			facemask |= 1<<5; /*back view*/
		break;
	case PROJ_PANORAMA:
		shader = R_RegisterShader("postproc_panorama", SUF_NONE,
				"{\n"
					"program postproc_panorama\n"
					"{\n"
						"map $sourcecube\n"
					"}\n"
				"}\n"
				);

		//panoramic view needs at most the four sides
		facemask |= 1<<4; /*front view*/
		if (ffov.value > 90)
		{
			facemask |= (1<<0) | (1<<1); /*side views*/
			if (ffov.value > 270)
				facemask |= 1<<5; /*back view*/
		}
		facemask = 0x3f;
		break;
	case PROJ_LAEA:
		shader = R_RegisterShader("postproc_laea", SUF_NONE,
				"{\n"
					"program postproc_laea\n"
					"{\n"
						"map $sourcecube\n"
					"}\n"
				"}\n"
				);

		facemask |= 1<<4; /*front view*/
		if (ffov.value > 90)
		{
			facemask |= (1<<0) | (1<<1) | (1<<2) | (1<<3); /*side/top/bottom views*/
			if (ffov.value > 270)
				facemask |= 1<<5; /*back view*/
		}
		break;

	case PROJ_EQUIRECTANGULAR:
		shader = R_RegisterShader("postproc_equirectangular", SUF_NONE,
				"{\n"
					"program postproc_equirectangular\n"
					"{\n"
						"map $sourcecube\n"
					"}\n"
				"}\n"
				);

		facemask = 0x3f;
#if 0
		facemask |= 1<<4; /*front view*/
		if (ffov.value > 90)
		{
			facemask |= (1<<0) | (1<<1) | (1<<2) | (1<<3); /*side/top/bottom views*/
			if (ffov.value > 270)
				facemask |= 1<<5; /*back view*/
		}
#endif
		break;
	case PROJ_PANINI:
		shader = R_RegisterShader("postproc_panini", SUF_NONE,
				"{\n"
					"program postproc_panini\n"
					"{\n"
						"map $sourcecube\n"
					"}\n"
				"}\n"
				);

		facemask |= 1<<4; /*front view*/
		if (ffov.value > 70)
		{
			facemask |= (1<<0) | (1<<1); /*side/top*/
			if (ffov.value > 85)
				facemask |= (1<<2) | (1<<3); /*bottom views*/
			if (ffov.value > 300)
				facemask |= 1<<5; /*back view*/
		}
		break;
	}

	//FIXME: we should be able to rotate the view

	vrect = r_refdef.vrect;
	prect = r_refdef.pxrect;
//	prect.x = (vrect.x * vid.pixelwidth)/vid.width;
//	prect.width = (vrect.width * vid.pixelwidth)/vid.width;
//	prect.y = (vrect.y * vid.pixelheight)/vid.height;
//	prect.height = (vrect.height * vid.pixelheight)/vid.height;

	if (sh_config.texture_non_power_of_two_pic)
	{
		if (usefbo)
		{
			cmapsize = prect.width > prect.height?prect.width:prect.height;
			if (cmapsize > 4096)//sh_config.texture_maxsize)
				cmapsize = 4096;//sh_config.texture_maxsize;
		}
		else
			cmapsize = prect.width < prect.height?prect.width:prect.height;
	}
	else if (!usefbo)
	{
		while (cmapsize > prect.width || cmapsize > prect.height)
		{
			cmapsize /= 2;
		}
	}

	if (usefbo)
	{
		r_refdef.flags |= RDF_FISHEYE;
		vid.fbpwidth = vid.fbpheight = cmapsize;
	}

	//FIXME: gl_max_size

	VectorCopy(r_refdef.vieworg, saveorg);
	VectorCopy(r_refdef.viewangles, saveang);
	saveang[2] = 0;

	r_refdef.stereomethod = STEREO_OFF;

	if (!TEXVALID(scenepp_postproc_cube) || cmapsize != scenepp_postproc_cube_size)
	{
		if (!TEXVALID(scenepp_postproc_cube))
		{
			scenepp_postproc_cube = Image_CreateTexture("***fish***", NULL, IF_TEXTYPE_CUBE|IF_RENDERTARGET|IF_CLAMP|IF_LINEAR);
			qglGenTextures(1, &scenepp_postproc_cube->num);
		}
		else
		{
			qglDeleteTextures(1, &scenepp_postproc_cube->num);
			scenepp_postproc_cube->num = 0;
			GL_MTBind(0, GL_TEXTURE_CUBE_MAP_ARB, scenepp_postproc_cube);
			qglGenTextures(1, &scenepp_postproc_cube->num);
		}

		GL_MTBind(0, GL_TEXTURE_CUBE_MAP_ARB, scenepp_postproc_cube);
		for (i = 0; i < 6; i++)
			qglCopyTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB + i, 0, GL_RGB, 0, 0, cmapsize, cmapsize, 0);
		qglTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		qglTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		qglTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		qglTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

		scenepp_postproc_cube_size = cmapsize;

		fboreset = true;
	}

	vrect = r_refdef.vrect;	//save off the old vrect

	r_refdef.vrect.width = (cmapsize * vid.fbvwidth) / vid.fbpwidth;
	r_refdef.vrect.height = (cmapsize * vid.fbvheight) / vid.fbpheight;
	r_refdef.vrect.x = 0;
	r_refdef.vrect.y = prect.y;

	ang[0][0] = -saveang[0];
	ang[0][1] = -90;
	ang[0][2] = -saveang[0];

	ang[1][0] = -saveang[0];
	ang[1][1] = 90;
	ang[1][2] = saveang[0];
	ang[5][0] = -saveang[0]*2;

	//in theory, we could use a geometry shader to duplicate the polygons to each face.
	//that would of course require that every bit of glsl had such a geometry shader.
	//it would at least reduce cpu load quite a bit.
	for (i = 0; i < 6; i++)
	{
		if (!(facemask & (1<<i)))
			continue;

		if (usefbo)
		{
			int r = GLBE_FBO_Update(&fbo_postproc_cube, FBO_RB_DEPTH|(fboreset?FBO_RESET:0), &scenepp_postproc_cube, 1, r_nulltex,  cmapsize, cmapsize, i);
			fboreset = false;
			if (oldfbo < 0)
				oldfbo = r;
		}

		r_refdef.fov_x = 90;
		r_refdef.fov_y = 90;
		r_refdef.viewangles[0] = saveang[0]+ang[i][0];
		r_refdef.viewangles[1] = saveang[1]+ang[i][1];
		r_refdef.viewangles[2] = saveang[2]+ang[i][2];

		R_Clear (usefbo);

		GL_SetShaderState2D(false);

		// render normal view
		R_RenderScene ();

		if (!usefbo)
		{
			GL_MTBind(0, GL_TEXTURE_CUBE_MAP_ARB, scenepp_postproc_cube);
			qglCopyTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB + i, 0, 0, 0, 0, vid.fbpheight - (prect.y + cmapsize), cmapsize, cmapsize);
		}
	}

	if (usefbo)
		GLBE_FBO_Pop(oldfbo);

	r_refdef.vrect = vrect;
	r_refdef.pxrect = prect;
	VectorCopy(saveorg, r_refdef.vieworg);
	r_refdef.stereomethod = osm;

	//GL_ViewportUpdate();
	GL_Set2D(false);
	// go 2d
/*	qglMatrixMode(GL_PROJECTION);
	qglPushMatrix();
	qglLoadIdentity ();
	qglOrtho  (0, vid.width, vid.height, 0, -99999, 99999);
	qglMatrixMode(GL_MODELVIEW);
	qglPushMatrix();
	qglLoadIdentity ();
*/
	// draw it through the shader
	if (r_projection.ival == PROJ_EQUIRECTANGULAR)
	{
		//note vr screenshots have requirements here
		R2D_Image(vrect.x, vrect.y, vrect.width, vrect.height, 0, 1, 1, 0, shader);
	}
	else if (r_projection.ival == PROJ_PANORAMA)
	{
		float saspect = .5;
		float taspect = vrect.height / vrect.width * ffov.value / 90;//(0.5 * vrect.width) / vrect.height;
		R2D_Image(vrect.x, vrect.y, vrect.width, vrect.height, -saspect, taspect, saspect, -taspect, shader);
	}
	else if (vrect.width > vrect.height)
	{
		float aspect = (0.5 * vrect.height) / vrect.width;
		R2D_Image(vrect.x, vrect.y, vrect.width, vrect.height, -0.5, aspect, 0.5, -aspect, shader);
	}
	else
	{
		float aspect = (0.5 * vrect.width) / vrect.height;
		R2D_Image(vrect.x, vrect.y, vrect.width, vrect.height, -aspect, 0.5, aspect, -0.5, shader);
	}

	if (R2D_Flush)
		R2D_Flush();

	//revert the matricies
/*	qglMatrixMode(GL_PROJECTION);
	qglPopMatrix();
	qglMatrixMode(GL_MODELVIEW);
	qglPopMatrix();
*/
	return true;
}
#endif

texid_t R_RenderPostProcess (texid_t sourcetex, texid_t sourcedepth, int type, shader_t *shader, char *restexname)
{
	if (r_refdef.flags & type)
	{
		r_refdef.flags &= ~type;

		if (r_refdef.flags & RDF_ALLPOSTPROC)
		{	//there's other post-processing passes that still need to be applied.
			//thus we need to write this output to a texture.
			int w = (r_refdef.vrect.width * vid.pixelwidth) / vid.width;
			int h = (r_refdef.vrect.height * vid.pixelheight) / vid.height;
			if (R2D_Flush)
				R2D_Flush();
			GLBE_FBO_Sources(sourcetex, sourcedepth);
			sourcetex = R2D_RT_Configure(restexname, w, h, TF_RGBA32, RT_IMAGEFLAGS);
			GLBE_FBO_Update(&fbo_postproc, 0, &sourcetex, 1, r_nulltex, w, h, 0);
			qglViewport(0,0,w,h);
			R2D_ScalePic(0, 0, vid.fbvwidth, vid.fbvheight, shader);
			if (R2D_Flush)
				R2D_Flush();
			GLBE_RenderToTextureUpdate2d(true);
		}
		else
		{	//yay, dump it to the screen
			//update stuff now that we're not rendering the 3d scene
			if (R2D_Flush)
				R2D_Flush();
			GLBE_FBO_Sources(sourcetex, sourcedepth);
			R2D_ScalePic(r_refdef.vrect.x, r_refdef.vrect.y, r_refdef.vrect.width, r_refdef.vrect.height, shader);
			if (R2D_Flush)
				R2D_Flush();
		}
	}

	return sourcetex;
}

/*
================
R_RenderView

r_refdef must be set before the first call
================
*/
void GLR_RenderView (void)
{
	int dofbo = *r_refdef.rt_destcolour[0].texname || *r_refdef.rt_depth.texname;
	double	time1 = 0, time2;
	texid_t sourcetex = r_nulltex;
	texid_t sourcedepth = r_nulltex;
	shader_t *custompostproc = NULL;
	float renderscale;	//extreme, but whatever
	int oldfbo = 0;
	qboolean forcedfb = false;
	qboolean fbdepth = false;

	checkglerror();

	if (r_norefresh.value || !vid.fbpwidth || !vid.fbpwidth)
		return;

	//when loading/bugged, its possible that the world is still loading.
	//in this case, don't act as a wallhack (unless the world is meant to be hidden anyway)
	if (!(r_refdef.flags & RDF_NOWORLDMODEL))
	{
		//FIXME: fbo stuff
		if (!r_worldentity.model || !cl.worldmodel)
			r_refdef.flags |= RDF_NOWORLDMODEL;
		else if (r_worldentity.model->loadstate != MLS_LOADED || !cl.worldmodel)
		{
			GL_Set2D (false);
			R2D_ImageColours(0, 0, 0, 1);
			R2D_FillBlock(r_refdef.vrect.x, r_refdef.vrect.y, r_refdef.vrect.width, r_refdef.vrect.height);
			R2D_ImageColours(1, 1, 1, 1);
			return;
		}
//		Sys_Error ("R_RenderView: NULL worldmodel");
	}

	//check if we're underwater (this also limits damage from stereo wallhacks).
	Surf_SetupFrame();
	r_refdef.flags &= ~(RDF_ALLPOSTPROC|RDF_RENDERSCALE);

	if (dofbo || (r_refdef.flags & RDF_NOWORLDMODEL))
		renderscale = 1;
	else
	{
		renderscale = r_renderscale.value;
		if (R_CanBloom())
			r_refdef.flags |= RDF_BLOOM;
	}

	//check if we can do underwater warp
	if (cls.protocol != CP_QUAKE2)	//quake2 tells us directly
	{
		if (r_viewcontents & FTECONTENTS_FLUID)
			r_refdef.flags |= RDF_UNDERWATER;
		else
			r_refdef.flags &= ~RDF_UNDERWATER;
	}
	if (r_refdef.flags & RDF_UNDERWATER)
	{
		extern cvar_t r_projection;
		if (!r_waterwarp.value || r_projection.ival)
			r_refdef.flags &= ~RDF_UNDERWATER;	//no warp at all
		else if (r_waterwarp.value > 0 && scenepp_waterwarp)
			r_refdef.flags |= RDF_WATERWARP;	//try fullscreen warp instead if we can
	}

	if (!r_refdef.globalfog.density)
	{
		extern cvar_t r_fog_linear;

		int fogtype = ((r_refdef.flags & RDF_UNDERWATER) && cl.fog[FOGTYPE_WATER].density)?FOGTYPE_WATER:FOGTYPE_AIR;
		CL_BlendFog(&r_refdef.globalfog, &cl.oldfog[fogtype], realtime, &cl.fog[fogtype]);

		if (!r_fog_linear.ival)
			r_refdef.globalfog.density /= 64;	//FIXME
	}

	if (!(r_refdef.flags & RDF_NOWORLDMODEL))
	{
		if (r_fxaa.ival)
			r_refdef.flags |= RDF_ANTIALIAS;
		if (*r_postprocshader.string)
			custompostproc = R_RegisterCustom(NULL, r_postprocshader.string, SUF_NONE, NULL, NULL);
		else if (!r_graphics.ival)
			custompostproc = R_RegisterShader("postproc_ascii", 0, 
				"{\n"
					"program postproc_ascii\n"
					"affine\n"
					"{\n"
						"map $sourcecolour\n"
						"nodepthtest\n"
					"}\n"
				"}\n"
				);
		if (custompostproc)
			r_refdef.flags |= RDF_CUSTOMPOSTPROC;

		if (r_hdr_framebuffer.ival && !(vid.flags & VID_FP16))	//primary use of this cvar is to fix q3shader overbrights (so bright lightmaps can oversaturate then drop below 1 by modulation with the lightmap
			forcedfb = true;
		if (custompostproc)
		{
			int i;
			for (i = 0; i < custompostproc->numpasses; i++)
				if (custompostproc->passes[i].texgen == T_GEN_SOURCEDEPTH)
				{
					fbdepth = true;
					break;
				}
		}
		if (vid_hardwaregamma.ival == 4 && (v_gamma.value != 1 || v_contrast.value != 1 || v_contrastboost.value != 1|| v_brightness.value != 0))
			r_refdef.flags |= RDF_SCENEGAMMA;
	}

	//disable stuff if its simply not supported.
	if (dofbo || !gl_config.arb_shader_objects || !gl_config.ext_framebuffer_objects || !sh_config.texture_non_power_of_two_pic)
	{
		forcedfb &= !dofbo && gl_config.ext_framebuffer_objects && sh_config.texture_non_power_of_two_pic;
		r_refdef.flags &= ~(RDF_ALLPOSTPROC);	//block all of this stuff
	}
	if (dofbo)
		forcedfb = false;
	else if (renderscale != 1)
		forcedfb = gl_config.ext_framebuffer_objects && sh_config.texture_non_power_of_two_pic;

	/* FTESurf Patch 208: belt and braces.  A portal clip only ever lives inside
	   GLR_DrawPortal, which restores it on the way out, but this is the top of
	   every 3d view and r_refdef is assembled upstream by several different
	   paths -- so start each one with the clip provably down, and let the
	   BE_Scissor below actually disable it rather than resolve to a stale box. */
	r_refdef.portalclip = false;
	BE_Scissor(NULL);
	if (dofbo)
	{
		unsigned int flags = 0;
		texid_t col[R_MAX_RENDERTARGETS], depth = r_nulltex;
		unsigned int cw=0, ch=0, dw=0, dh=0;
		int mrt;

		if (!gl_config.ext_framebuffer_objects && sh_config.texture_non_power_of_two_pic)
		{
			Con_DPrintf(CON_WARNING"Render targets are not supported on this gpu.\n");
			return;	//not supported on this gpu. you'll just get black textures or something.
		}

		//3d views generally ignore source colour+depth.
		//FIXME: support depth with no colour
		for (mrt = 0; mrt < R_MAX_RENDERTARGETS; mrt++)
		{
			if (*r_refdef.rt_destcolour[mrt].texname)
			{
				col[mrt] = R2D_RT_GetTexture(r_refdef.rt_destcolour[mrt].texname, &cw, &ch);
				if (!TEXVALID(col[mrt]))
					break;
			}
			else
			{
				col[mrt] = r_nulltex;
				break;
			}
		}
		if (*r_refdef.rt_depth.texname)
			depth = R2D_RT_GetTexture(r_refdef.rt_depth.texname, &dw, &dh);

		if (mrt)
		{ 	//colour (with or without depth)
			if (*r_refdef.rt_depth.texname && (dw != cw || dh != ch))
			{
				Con_Printf("RT: destcolour and depth render targets are of different sizes\n");	//should check rgb/depth modes too I guess.
				depth = r_nulltex;
			}
			vid.fbvwidth = vid.fbpwidth = cw;
			vid.fbvheight = vid.fbpheight = ch;
		}
		else
		{	//depth, with no colour
			vid.fbvwidth = vid.fbpwidth = dw;
			vid.fbvheight = vid.fbpheight = dh;
		}
		if (TEXVALID(depth))
			flags |= FBO_TEX_DEPTH;
		else
			flags |= FBO_RB_DEPTH;
		oldfbo = GLBE_FBO_Update(&fbo_gameview, flags, col, mrt, depth, vid.fbpwidth, vid.fbpheight, 0);
	}
	else if ((r_refdef.flags & (RDF_ALLPOSTPROC)) || forcedfb)
	{
		unsigned int rtflags = IF_NOMIPMAP|IF_CLAMP|IF_RENDERTARGET|IF_NOSRGB;
		enum uploadfmt fmt;
		unsigned int fboflags = 0;

		r_refdef.flags |= RDF_RENDERSCALE;

		//the game needs to be drawn to a texture for post processing
		if (1)//vid.framebuffer)
		{
			vid.fbpwidth = (r_refdef.vrect.width * r_refdef.pxrect.width) / vid.width;
			vid.fbpheight = (r_refdef.vrect.height * r_refdef.pxrect.height) / vid.height;
		}
		else
		{
			vid.fbpwidth = (r_refdef.vrect.width * vid.pixelwidth) / vid.width;
			vid.fbpheight = (r_refdef.vrect.height * vid.pixelheight) / vid.height;
		}

		if (renderscale < 0)
		{
			renderscale = -renderscale;
			rtflags |= IF_NEAREST;
			vid.fbpwidth *= renderscale;
			vid.fbpheight *= renderscale;
		}
		else
		{
			rtflags |= IF_LINEAR;
			vid.fbpwidth *= renderscale;
			vid.fbpheight *= renderscale;
		}

		//well... err... meh.
		vid.fbpwidth = bound(1, vid.fbpwidth, sh_config.texture2d_maxsize);
		vid.fbpheight = bound(1, vid.fbpheight, sh_config.texture2d_maxsize);

		vid.fbvwidth = vid.fbpwidth;
		vid.fbvheight = vid.fbpheight;

		fmt = PTI_RGBA8;
		if (r_hdr_framebuffer.ival < 0)
		{	//cvar change handler will set ival negative if it matches a known format name, doesn't mean its supported.
			fmt = -r_hdr_framebuffer.ival;
			if (fmt >= PTI_FIRSTCOMPRESSED || !sh_config.texfmt[fmt])
				fmt = PTI_RGB565;
		}
		else if ((r_refdef.flags&RDF_SCENEGAMMA)||(vid.flags&(VID_SRGBAWARE|VID_FP16))||r_hdr_framebuffer.ival)
		{	//gamma ramps really need higher colour precision, otherwise the entire thing looks terrible.
			if (sh_config.texfmt[PTI_B10G11R11F])
				fmt = PTI_B10G11R11F;
			else if (sh_config.texfmt[PTI_RGBA16F])
				fmt = PTI_RGBA16F;
			else if (sh_config.texfmt[PTI_A2BGR10])
				fmt = PTI_A2BGR10;
		}

		sourcetex = R2D_RT_Configure("rt/$lastgameview", vid.fbpwidth, vid.fbpheight, fmt, rtflags);
		if (fbdepth)
		{
			if (sh_config.texfmt[PTI_DEPTH24_8] && !r_shadow_shadowmapping.ival)
				fmt = PTI_DEPTH24_8;
			else if (sh_config.texfmt[PTI_DEPTH32])
				fmt = PTI_DEPTH32;
			else
				fmt = PTI_DEPTH16;
		}
		else
			fmt = PTI_INVALID;
		sourcedepth = (fmt != PTI_INVALID)?R2D_RT_Configure("rt/$lastgameviewdepth", vid.fbpwidth, vid.fbpheight, fmt, rtflags):r_nulltex;
		if (sourcedepth)
			fboflags = FBO_TEX_DEPTH;
		else
			fboflags = FBO_RB_DEPTH;

		oldfbo = GLBE_FBO_Update(&fbo_gameview, fboflags, &sourcetex, 1, sourcedepth, vid.fbpwidth, vid.fbpheight, 0);
		dofbo = true;
	}
	else if (vid.framebuffer)
	{
		vid.fbvwidth = vid.width;
		vid.fbvheight = vid.height;
		vid.fbpwidth = vid.framebuffer->width;
		vid.fbpheight = vid.framebuffer->height;
	}
	else
	{
		vid.fbvwidth = vid.width;
		vid.fbvheight = vid.height;
		vid.fbpwidth = vid.pixelwidth;
		vid.fbpheight = vid.pixelheight;
	}
	r_refdef.flipcull = 0;

	if (qglPNTrianglesiATI)
	{
		if (gl_ati_truform_type.ival)
		{	//linear
			qglPNTrianglesiATI(GL_PN_TRIANGLES_NORMAL_MODE_ATI, GL_PN_TRIANGLES_NORMAL_MODE_LINEAR_ATI);
			qglPNTrianglesiATI(GL_PN_TRIANGLES_POINT_MODE_ATI, GL_PN_TRIANGLES_POINT_MODE_CUBIC_ATI);
		}
		else
		{	//quadric
			qglPNTrianglesiATI(GL_PN_TRIANGLES_NORMAL_MODE_ATI, GL_PN_TRIANGLES_NORMAL_MODE_QUADRATIC_ATI);
			qglPNTrianglesiATI(GL_PN_TRIANGLES_POINT_MODE_ATI, GL_PN_TRIANGLES_POINT_MODE_CUBIC_ATI);
		}
		qglPNTrianglesfATI(GL_PN_TRIANGLES_TESSELATION_LEVEL_ATI, r_tessellation_level.value);
	}

	if (gl_finish.ival)
	{
		RSpeedMark();
		qglFinish ();
		RSpeedEnd(RSPEED_SUBMIT);
	}

	if (r_speeds.ival)
	{
		time1 = Sys_DoubleTime ();
	}

	if (!(r_refdef.flags & RDF_NOWORLDMODEL) && R_RenderScene_Cubemap())
	{

	}
	else
	{
		GL_SetShaderState2D(false);

		R_Clear (dofbo);

	//	GLR_SetupFog ();

		// render normal view
		R_RenderScene ();
	}

//	qglDisable(GL_FOG);

	if (r_speeds.ival)
	{
//		glFinish ();
		time2 = Sys_DoubleTime ();

		RQuantAdd(RQUANT_MSECS, (int)((time2-time1)*1000000));

	//	Con_Printf ("%3i ms  %4i wpoly %4i epoly\n", (int)((time2-time1)*1000), c_brush_polys, c_alias_polys);
	}

	checkglerror();

	//nettest: POST-PROCESS / RESOLVE bucket.  Everything from here to the end of this function --
	//the FBO pop, the scenepp_rescaled resolve blit that r_renderscale exists to feed, the whole
	//R_RenderPostProcess chain (gamma / waterwarp / custom / fxaa), R_BloomBlend, the R2D_Flush that
	//actually SUBMITS all of it, and motion blur -- sat in no RSPEED_* bucket at all.  Before this,
	//gl_rmain.c contained exactly one RSpeed pair in the whole file (the gl_finish block above), so
	//this entire stage was counted in Total refresh and CSQC Drawing yet attributed to nothing.
	//It is also the only stage that scales with r_renderscale on BOTH sides: the source read scales
	//with renderscale^2, the write with the output resolution.
	{
	RSpeedMark();

	//update stuff now that we're not rendering the 3d scene
	if (dofbo)
		GLBE_FBO_Pop(oldfbo);

	GLBE_RenderToTextureUpdate2d(false);
	GL_Set2D (2);

	// SCENE POST PROCESSING

	if (forcedfb && !(r_refdef.flags & RDF_ALLPOSTPROC))
	{
		GLBE_FBO_Sources(sourcetex, sourcedepth);
		R2D_Image(r_refdef.vrect.x, r_refdef.vrect.y, r_refdef.vrect.width, r_refdef.vrect.height, 0, 1, 1, 0, scenepp_rescaled);
	}
	else
	{
		if (r_refdef.flags & RDF_SCENEGAMMA)
		{
			R2D_ImageColours (v_gammainverted.ival?v_gamma.value:(1/v_gamma.value), v_contrast.value, v_brightness.value, v_contrastboost.value);
			sourcetex = R_RenderPostProcess (sourcetex, sourcedepth, RDF_SCENEGAMMA, scenepp_gamma, "rt/$gammaed");
			R2D_ImageColours (1, 1, 1, 1);
		}
		sourcetex = R_RenderPostProcess (sourcetex, sourcedepth, RDF_WATERWARP, scenepp_waterwarp, "rt/$waterwarped");
		sourcetex = R_RenderPostProcess (sourcetex, sourcedepth, RDF_CUSTOMPOSTPROC, custompostproc, "rt/$postproced");
		sourcetex = R_RenderPostProcess (sourcetex, sourcedepth, RDF_ANTIALIAS, scenepp_antialias, "rt/$antialiased");
		if (r_refdef.flags & RDF_BLOOM)
			R_BloomBlend(sourcetex, r_refdef.vrect.x, r_refdef.vrect.y, r_refdef.vrect.width, r_refdef.vrect.height);
	}

	if (R2D_Flush)
		R2D_Flush();

	GLBE_FBO_Sources(r_nulltex, r_nulltex);

	if (gl_motionblur.value>0 && gl_motionblur.value < 1 && qglCopyTexImage2D)
		R_RenderMotionBlur();

	if (gl_screenangle.value)
		GL_Set2D (false);	//make sure any hud stuff is rotated properly.

	RSpeedEnd(RSPEED_POSTPROC);
	}

	checkglerror();
}

#endif
