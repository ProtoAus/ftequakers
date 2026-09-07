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
// r_surf.c: surface-related refresh code

#include "quakedef.h"
#ifndef SERVERONLY
#include "glquake.h"
#include "shader.h"
#include "renderque.h"
#include "com_mesh.h"
#include <math.h>

#if (defined(GLQUAKE) || defined(VKQUAKE)) && defined(MULTITHREAD)
#define THREADEDWORLD
int webo_blocklightmapupdates;	//0 no webo, &1=using threadedworld, &2=already uploaded. so update when !=3
#else
#define webo_blocklightmapupdates 0
#endif
#ifdef BEF_PUSHDEPTH
qboolean r_pushdepth;
#endif
qboolean r_dlightlightmaps; //updated each frame, says whether to do lightmap hack dlights.

extern cvar_t		r_ambient;

model_t				*currentmodel;

static size_t		maxblocksize;
static vec3_t		*blocknormals;
static unsigned		*blocklights;

lightmapinfo_t **lightmap;
int numlightmaps;
extern const float rgb9e5tab[32];

extern cvar_t r_stains;
extern cvar_t r_loadlits;
extern cvar_t r_stainfadetime;
extern cvar_t r_stainfadeammount;
extern cvar_t r_lightmap_nearest;
extern cvar_t r_lightmap_format;

double r_loaderstalltime;

extern int r_dlightframecount;

static void Surf_FreeLightmap(lightmapinfo_t *lm);

static int lightmap_shift;
int Surf_LightmapShift (model_t *model)
{
	extern cvar_t gl_overbright_all, gl_overbright;

	if (gl_overbright_all.ival || (model->engineflags & MDLF_NEEDOVERBRIGHT))
		lightmap_shift = bound(0, gl_overbright.ival, 2);
	else
		lightmap_shift = 0;
	return lightmap_shift;
}

void QDECL Surf_RebuildLightmap_Callback (struct cvar_s *var, char *oldvalue)
{
	Mod_RebuildLightmaps();
}

//radius, x y z, r g b
void Surf_StainSurf (model_t *mod, msurface_t *surf, float *parms)
{
	int			sd, td;
	float		dist, rad, minlight;
	float change;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	float amm;
	int lim;
	vec4_t	*lmvecs;
	float	*lmvecscale;
	stmap *stainbase;
	lightmapinfo_t *lm;

	lim = 255 - (r_stains.value*255);

#define stain(x)							\
	change = stainbase[(s)*3+x] + amm*parms[4+x];	\
	stainbase[(s)*3+x] = bound(lim, change, 255);

	if (surf->lightmaptexturenums[0] < 0)
		return;
	lm = lightmap[surf->lightmaptexturenums[0]];

	smax = (surf->extents[0]>>surf->lmshift)+1;
	tmax = (surf->extents[1]>>surf->lmshift)+1;
	if (mod->facelmvecs)
		lmvecs = mod->facelmvecs[surf-mod->surfaces].lmvecs, lmvecscale = mod->facelmvecs[surf-mod->surfaces].lmvecscale;
	else
		lmvecs = surf->texinfo->vecs, lmvecscale = surf->texinfo->vecscale;

	stainbase = lm->stainmaps;
	stainbase += (surf->light_t[0] * lm->width + surf->light_s[0]) * 3;

	rad = *parms;
	dist = DotProduct ((parms+1), surf->plane->normal) - surf->plane->dist;
	rad -= fabs(dist);
	minlight = 0;
	if (rad < minlight)	//not hit
		return;
	minlight = rad - minlight;

	for (i=0 ; i<3 ; i++)
	{
		impact[i] = (parms+1)[i] - surf->plane->normal[i]*dist;
	}

	local[0] = DotProduct (impact, lmvecs[0]) + lmvecs[0][3];
	local[1] = DotProduct (impact, lmvecs[1]) + lmvecs[1][3];

	local[0] -= surf->texturemins[0];
	local[1] -= surf->texturemins[1];

	for (t = 0 ; t<tmax ; t++)
	{
		td = (local[1] - (t<<surf->lmshift))*lmvecscale[1];
		if (td < 0)
			td = -td;
		for (s=0 ; s<smax ; s++)
		{
			sd = (local[0] - (s<<surf->lmshift))*lmvecscale[0];
			if (sd < 0)
				sd = -sd;
			if (sd > td)
				dist = sd + (td>>1);
			else
				dist = td + (sd>>1);
			if (dist < minlight)
			{
				amm = (rad - dist);
				stain(0);
				stain(1);
				stain(2);

				surf->stained = true;
			}
		}
		stainbase += 3*lm->width;
	}

	if (surf->stained)
		surf->cached_dlight=-1;
}

//combination of R_AddDynamicLights and R_MarkLights
/*
static void Surf_StainNode (mnode_t *node, float *parms)
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
		Surf_StainNode (node->children[0], parms);
		return;
	}
	if (dist < (-*parms))
	{
		Surf_StainNode (node->children[1], parms);
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

	Surf_StainNode (node->children[0], parms);
	Surf_StainNode (node->children[1], parms);
}
*/

void Surf_AddStain(vec3_t org, float red, float green, float blue, float radius)
{
	physent_t *pe;
	int i;

	float parms[7];
	if (!cl.worldmodel || cl.worldmodel->loadstate != MLS_LOADED || r_stains.value <= 0)
		return;
	parms[0] = radius;
	parms[1] = org[0];
	parms[2] = org[1];
	parms[3] = org[2];
	parms[4] = red;
	parms[5] = green;
	parms[6] = blue;


	cl.worldmodel->funcs.StainNode(cl.worldmodel, parms);

	//now stain inline bsp models other than world.

	for (i=1 ; i< pmove.numphysent ; i++)	//0 is world...
	{
		pe = &pmove.physents[i];
		if (pe->model && pe->model->surfaces == cl.worldmodel->surfaces && pe->model->loadstate == MLS_LOADED)
		{
			parms[1] = org[0] - pe->origin[0];
			parms[2] = org[1] - pe->origin[1];
			parms[3] = org[2] - pe->origin[2];

			if (pe->angles[0] || pe->angles[1] || pe->angles[2])
			{
				vec3_t f, r, u, temp;
				AngleVectors(pe->angles, f, r, u);
				VectorCopy((parms+1), temp);
				parms[1] = DotProduct(temp, f);
				parms[2] = -DotProduct(temp, r);
				parms[3] = DotProduct(temp, u);
			}


			pe->model->funcs.StainNode(pe->model, parms);
		}
	}
}

void Surf_WipeStains(void)
{
	int i;
	for (i = 0; i < numlightmaps; i++)
	{
		if (!lightmap[i])
			break;
		if (lightmap[i]->stainmaps)
			memset(lightmap[i]->stainmaps, 255, lightmap[i]->width*lightmap[i]->height*3*sizeof(stmap));
	}
}

void Surf_LessenStains(void)
{
	int i;
	msurface_t	*surf;

	int			smax, tmax;
	int			s, t;
	stmap *stain;
	int stride;
	int ammount;
	int limit;
	lightmapinfo_t *lm;

	static float time;

	if (!r_stains.value || !r_stainfadeammount.value)
		return;

	time += host_frametime;
	if (time < r_stainfadetime.value)
		return;
	time-=r_stainfadetime.value;

	ammount = r_stainfadeammount.value;
	limit = 255 - ammount;

	surf = cl.worldmodel->surfaces;
	for (i=0 ; i<cl.worldmodel->numsurfaces ; i++, surf++)
	{
		if (surf->stained)
		{
			lm = lightmap[surf->lightmaptexturenums[0]];

			surf->cached_dlight=-1;//nice hack here...

			smax = (surf->extents[0]>>surf->lmshift)+1;
			tmax = (surf->extents[1]>>surf->lmshift)+1;

			stain = lm->stainmaps;
			stain += (surf->light_t[0] * lm->width + surf->light_s[0]) * 3;

			stride = (lm->width-smax)*3;

			surf->stained = false;

			smax*=3;

			for (t = 0 ; t<tmax ; t++, stain+=stride)
			{
				for (s=0 ; s<smax ; s++)
				{
					if (*stain < limit)	//eventually decay to 255
					{
						*stain += ammount;
						surf->stained=true;
					}
					else	//reset to 255
						*stain = 255;

					stain++;
				}
			}
		}
	}
}

/*
===============
R_AddDynamicLights
===============
*/
static void Surf_AddDynamicLights_Lum (msurface_t *surf)
{
	size_t		lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	float l;
	unsigned	*bl;
	vec4_t		*lmvecs;
	float		*lmvecscale;

	smax = (surf->extents[0]>>surf->lmshift)+1;
	tmax = (surf->extents[1]>>surf->lmshift)+1;
	if (currentmodel->facelmvecs)
		lmvecs = currentmodel->facelmvecs[surf-currentmodel->surfaces].lmvecs, lmvecscale = currentmodel->facelmvecs[surf-currentmodel->surfaces].lmvecscale;
	else
		lmvecs = surf->texinfo->vecs, lmvecscale = surf->texinfo->vecscale;
	for (lnum=rtlights_first; lnum<RTL_FIRST; lnum++)
	{
		if ( !(surf->dlightbits & ((dlightbitmask_t)1u<<lnum) ) )
			continue;		// not lit by this light

		if (!(cl_dlights[lnum].flags & LFLAG_LIGHTMAP))
			continue;

		rad = cl_dlights[lnum].radius;
		dist = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) -
				surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, lmvecs[0]) + lmvecs[0][3];
		local[1] = DotProduct (impact, lmvecs[1]) + lmvecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		l = 256*(cl_dlights[lnum].color[0]*NTSC_RED + cl_dlights[lnum].color[1]*NTSC_GREEN + cl_dlights[lnum].color[2]*NTSC_BLUE);

		bl = blocklights;
		for (t = 0 ; t<tmax ; t++)
		{
			td = (local[1] - (t<<surf->lmshift))*lmvecscale[1];
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = (local[0] - (s<<surf->lmshift))*lmvecscale[0];
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
					bl[0] += (rad - dist)*l;
				bl++;
			}
		}
	}
}

/*
static void Surf_AddDynamicLightNorms (msurface_t *surf)
{
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	vec4_t		*lmvecs;
	float		*lmvecscale;
	float a;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	tex = surf->texinfo;

	for (lnum=rtlights_first; lnum<RTL_FIRST; lnum++)
	{
		if ( !(surf->dlightbits & ((dlightbitmask_t)1u<<lnum) ) )
			continue;		// not lit by this light

		if (!(cl_dlights[lnum].flags & LFLAG_ALLOW_LMHACK))
			continue;

		rad = cl_dlights[lnum].radius;
		dist = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) -
				surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, lmvecs[0]) + lmvecs[0][3];
		local[1] = DotProduct (impact, lmvecs[1]) + lmvecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		a = 256*(cl_dlights[lnum].color[0]*NTSC_RED + cl_dlights[lnum].color[1]*NTSC_GREEN + cl_dlights[lnum].color[2]*NTSC_BLUE);

		for (t = 0 ; t<tmax ; t++)
		{
			td = (local[1] - t*surf->lmscale)*lmvecscale[1];
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = (local[0] - s*surf->lmscale)*lmvecscale[0];
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
				{
//					blocknormals[t*smax + s][0] -= (rad - dist)*(impact[0]-local[0])/8192.0;
//					blocknormals[t*smax + s][1] -= (rad - dist)*(impact[1]-local[1])/8192.0;
					blocknormals[t*smax + s][2] += 0.5*blocknormals[t*smax + s][2]*(rad - dist)/256;
				}
			}
		}
	}
}
*/

#ifdef PEXT_LIGHTSTYLECOL
static void Surf_AddDynamicLights_RGB (msurface_t *surf)
{
	int			lnum;
	float		sd, td;
	float		dist, rad, minlight;
	vec3_t		impact;
	vec2_t		local;
	int			s, t;
	int			i;
	int			smax, tmax;
	vec4_t		*lmvecs;
	float		*lmvecscale;
//	float temp;
	float r, g, b;
	unsigned	*bl;
	vec3_t lightofs;

	smax = (surf->extents[0]>>surf->lmshift)+1;
	tmax = (surf->extents[1]>>surf->lmshift)+1;
	if (currentmodel->facelmvecs)
		lmvecs = currentmodel->facelmvecs[surf-currentmodel->surfaces].lmvecs, lmvecscale = currentmodel->facelmvecs[surf-currentmodel->surfaces].lmvecscale;
	else
		lmvecs = surf->texinfo->vecs, lmvecscale = surf->texinfo->vecscale;

	for (lnum=rtlights_first; lnum<RTL_FIRST; lnum++)
	{
		if ( !(surf->dlightbits & ((dlightbitmask_t)1u<<lnum) ) )
			continue;		// not lit by this light

		rad = cl_dlights[lnum].radius;
		VectorSubtract(cl_dlights[lnum].origin, currententity->origin, lightofs);
		//FIXME: transform by currententity->axis
		dist = DotProduct (lightofs, surf->plane->normal) - surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = lightofs[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, lmvecs[0]) + lmvecs[0][3];
		local[1] = DotProduct (impact, lmvecs[1]) + lmvecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];


		if (r_dynamic.ival == 2)
			r = g = b = 256;
		else
		{
			r = cl_dlights[lnum].color[0]*128;
			g = cl_dlights[lnum].color[1]*128;
			b = cl_dlights[lnum].color[2]*128;
		}

		bl = blocklights;
		if (r < 0 || g < 0 || b < 0)
		{
			for (t = 0 ; t<tmax ; t++)
			{
				td = (local[1] - (t<<surf->lmshift))*lmvecscale[1];
				if (td < 0)
					td = -td;
				for (s=0 ; s<smax ; s++)
				{
					sd = (local[0] - (s<<surf->lmshift))*lmvecscale[0];
					if (sd < 0)
						sd = -sd;
					if (sd > td)
						dist = sd + td*0.5;
					else
						dist = td + sd*0.5;
					if (dist < minlight)
					{
						i = bl[0] + (rad - dist)*r;
						bl[0] = (i<0)?0:i;
						i = bl[1] + (rad - dist)*g;
						bl[1] = (i<0)?0:i;
						i = bl[2] + (rad - dist)*b;
						bl[2] = (i<0)?0:i;
					}
					bl += 3;
				}
			}
		}
		else
		{
			for (t = 0 ; t<tmax ; t++)
			{
				td = (local[1] - (t<<surf->lmshift))*lmvecscale[1];
				if (td < 0)
					td = -td;
				for (s=0 ; s<smax ; s++)
				{
					sd = (local[0] - (s<<surf->lmshift))*lmvecscale[0];
					if (sd < 0)
						sd = -sd;
					if (sd > td)
						dist = sd + td*0.5;
					else
						dist = td + sd*0.5;
					if (dist < minlight)
					{
						bl[0] += (rad - dist)*r;
						bl[1] += (rad - dist)*g;
						bl[2] += (rad - dist)*b;
					}
					bl += 3;
				}
			}
		}
	}
}
#endif



static void Surf_BuildDeluxMap (model_t *wmodel, msurface_t *surf, qbyte *dest, lightmapinfo_t *lm, vec3_t *blocknormals)
{
	int			smax, tmax;
	int			i, j, size;
	qbyte		*lightmap;
	qbyte		*deluxmap;
	unsigned	scale;
	int			maps;
	float intensity;
	vec_t		*bnorm;
	vec3_t temp;

	int stride;

	if (!dest)
		return;

	smax = (surf->extents[0]>>surf->lmshift)+1;
	tmax = (surf->extents[1]>>surf->lmshift)+1;
	size = smax*tmax;
	lightmap = surf->samples;

	// set to full bright if no light data
	if (!wmodel->deluxdata)
	{
		for (i=0 ; i<size ; i++)
		{
			blocknormals[i][0] = 0.9;//surf->orientation[2][0];
			blocknormals[i][1] = 0.8;//surf->orientation[2][1];
			blocknormals[i][2] = 1;//surf->orientation[2][2];
		}
		goto store;
	}

// clear to no light
	for (i=0 ; i<size ; i++)
	{
		blocknormals[i][0] = 0;
		blocknormals[i][1] = 0;
		blocknormals[i][2] = 0;
	}

// add all the lightmaps
	if (lightmap)
	{
		switch(wmodel->lightmaps.fmt)
		{
		case LM_E5BGR9:
			deluxmap = ((surf->samples - wmodel->lightdata)/4)*3 + wmodel->deluxdata;
			for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
			{
				scale = d_lightstylevalue[surf->styles[maps]];
				for (i=0 ; i<size ; i++)
				{
					unsigned lm = ((unsigned int*)lightmap)[i];
					intensity = max3(((lm>>0)&0x1ff),((lm>>9)&0x1ff),((lm>>18)&0x1ff)) * scale * (rgb9e5tab[lm>>27]*(1<<7));
					blocknormals[i][0] += intensity*(deluxmap[i*3+0]-127);
					blocknormals[i][1] += intensity*(deluxmap[i*3+1]-127);
					blocknormals[i][2] += intensity*(deluxmap[i*3+2]-127);
				}
				lightmap += size*4;	// skip to next lightmap
				deluxmap += size*3;
			}
			break;
		case LM_RGB8:
			deluxmap = surf->samples - wmodel->lightdata + wmodel->deluxdata;
			for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
			{
				scale = d_lightstylevalue[surf->styles[maps]];
				for (i=0 ; i<size ; i++)
				{
					intensity = (lightmap[i*3]+lightmap[i*3+1]+lightmap[i*3+2]) * scale;
					blocknormals[i][0] += intensity*(deluxmap[i*3+0]-127);
					blocknormals[i][1] += intensity*(deluxmap[i*3+1]-127);
					blocknormals[i][2] += intensity*(deluxmap[i*3+2]-127);
				}
				lightmap += size*3;	// skip to next lightmap
				deluxmap += size*3;
			}
			break;
		case LM_L8:
			deluxmap = (surf->samples - wmodel->lightdata)*3 + wmodel->deluxdata;
			for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
			{
				scale = d_lightstylevalue[surf->styles[maps]];
				for (i=0 ; i<size ; i++)
				{
					intensity = (lightmap[i]) * scale;
					blocknormals[i][0] += intensity*(deluxmap[i*3+0]-127);
					blocknormals[i][1] += intensity*(deluxmap[i*3+1]-127);
					blocknormals[i][2] += intensity*(deluxmap[i*3+2]-127);
				}
				lightmap += size;	// skip to next lightmap
				deluxmap += size*3;
			}
			break;
		}
	}

store:
	// add all the dynamic lights
//	if (surf->dlightframe == r_dlightframecount)
//		GLR_AddDynamicLightNorms (surf);

// bound, invert, and shift

	switch (lm->fmt)
	{
	default:
		Sys_Error("Bad deluxemap format\n");
		break;
	case PTI_A2BGR10:
		{
			unsigned int *destl = (void*)dest, r;

			stride = (lm->width-smax);
			bnorm = blocknormals[0];
			for (i=0 ; i<tmax ; i++, destl += stride)
			{
				for (j=0 ; j<smax ; j++)
				{
					temp[0] = bnorm[0];
					temp[1] = bnorm[1];
					temp[2] = bnorm[2];	//half the effect? so we emulate light's scalecos of 0.5
					bnorm+=3;
					VectorNormalize(temp);
					r  = (unsigned int)((temp[0]+1)/2*1023)<<0;
					r |= (unsigned int)((temp[1]+1)/2*1023)<<10;
					r |= (unsigned int)((temp[2]+1)/2*1023)<<20;
					*destl++ = r;
				}
			}
		}
		break;
	case PTI_BGRX8:
		stride = (lm->width-smax)*4;
		bnorm = blocknormals[0];
		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				temp[0] = bnorm[0];
				temp[1] = bnorm[1];
				temp[2] = bnorm[2];	//half the effect? so we emulate light's scalecos of 0.5
				VectorNormalize(temp);
				dest[2] = (temp[0]+1)/2*255;
				dest[1] = (temp[1]+1)/2*255;
				dest[0] = (temp[2]+1)/2*255;

				dest += 4;
				bnorm+=3;
			}
		}
		break;
	case PTI_RGBX8:
	case PTI_RGB8:
		stride = (lm->width-smax)*lm->pixbytes;
		bnorm = blocknormals[0];
		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				temp[0] = bnorm[0];
				temp[1] = bnorm[1];
				temp[2] = bnorm[2];	//half the effect? so we emulate light's scalecos of 0.5
				VectorNormalize(temp);
				dest[0] = (temp[0]+1)/2*255;
				dest[1] = (temp[1]+1)/2*255;
				dest[2] = (temp[2]+1)/2*255;

				dest += lm->pixbytes;
				bnorm+=3;
			}
		}
		break;
	}
}

static unsigned int Surf_PackE5BRG9(int r, int g, int b, int shift)
{	//5 bits exponent, 3*9 bits of mantissa. no sign bit.
	int e = 0;
	float m = max(max(r, g), b) / (float)(1u<<shift);
	float scale;

	if (m >= 0.5)
	{	//positive exponent
		while (m >= (1u<<(e)) && e < 30-15)	//don't do nans.
			e++;
	}
	else
	{	//negative exponent...
		while (m < 1/(1u<<-e) && e > -15)	//don't do denormals.
			e--;
	}

	scale = pow(2, e-9);
	scale *= (1u<<shift);

	r = bound(0, r/scale + 0.5, 0x1ff);
	g = bound(0, g/scale + 0.5, 0x1ff);
	b = bound(0, b/scale + 0.5, 0x1ff);

	return ((e+15)<<27) | (b<<18) | (g<<9) | r;
}

static unsigned short Surf_GenHalf(float val)
{	//1-bit sign (ignored here)
	//5-bit exponent (biased by 15)
	//10-bit mantissa (normalised, so effectively 11 bits when exponent!=0)
	union 
	{
		float f;
		unsigned int u;
	} u = {val};
	int e = 0;
	int m;

	e = ((u.u>>23)&0xff) - 127;
	if (e < -15)
		return 0; //too small exponent, treat it as a 0 denormal
	if (e > 15)
		m = 0; //infinity instead of a nan
	else
		m = (u.u&((1u<<23)-1))>>13;
	return ((e+15)<<10) | m;
}
static void Surf_PackRGB16F(void *result, int r, int g, int b, int one)
{
#if 0
	//bulldozer+ or skylake+ supposedly. which means I can't test it, which means I can't enable it.
	__v4sf rgba = (__v4sf){r, g, b, one} / (float)one;
	union
	{
		__v8hi v;
		__v4hi i;
	} tmp;
	//vcvtps2ph writes either a 64bit mem location, or the lower half of an xmm register. unfortunately ts still an xmm register, and that's 128bit.
	//the __vXhi weirdness is because half-floats just don't work anywhere but conversions so __vXhf would cause all sorts of compiler errors, is the theory
	tmp.v = __builtin_ia32_vcvtps2ph(rgba, 1);
	*(__v4hi*)result = tmp.i;
#else
	((unsigned short*)result)[0] = Surf_GenHalf(r / (float)one);
	((unsigned short*)result)[1] = Surf_GenHalf(g / (float)one);
	((unsigned short*)result)[2] = Surf_GenHalf(b / (float)one);
	((unsigned short*)result)[3] = /*Surf_GenHalf(1.0);*/0x0fu<<10; //a standard ieee float should have all but the lead bit set of its exponent, and its mantissa 0.
#endif
}
static void Surf_PackRGBX32F(void *result, int r, int g, int b, int one)
{
	((float*)result)[0] = r/(float)one;
	((float*)result)[1] = g/(float)one;
	((float*)result)[2] = b/(float)one;
	((float*)result)[3] = 1.0;
}

/*any sane compiler will inline and split this, removing the stainsrc stuff
just unpacks the internal lightmap block into texture info ready for upload
merges stains and oversaturates overbrights.
*/
static void Surf_StoreLightmap_RGB(qbyte *dest, unsigned int *bl, int smax, int tmax, unsigned int shift, stmap *stainsrc, lightmapinfo_t *lm)
{
	int r, g, b, m;
	unsigned int i, j;
	int stride;

	switch(lm->fmt)
	{
	default:
		Sys_Error("Surf_StoreLightmap_RGB: Bad format - %s\n", Image_FormatName(lm->fmt));
		break;
	case PTI_A2BGR10:
		stride = (lm->width-smax)<<2;

		shift -= 2;

		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++ >> shift;
				g = *bl++ >> shift;
				b = *bl++ >> shift;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				// quake 2 method, scale highest down to
				// maintain hue
				m = max(max(r, g), b);
				if (m > 1023)
				{
					r *= 1023.0/m;
					g *= 1023.0/m;
					b *= 1023.0/m;
				}

				*(unsigned int*)dest = (3u<<30) | ((b&0x3ff)<<20) | ((g&0x3ff)<<10) | (r&0x3ff);
				dest += 4;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	case PTI_E5BGR9:
		stride = (lm->width-smax)<<2;

		//5bit shared exponent, with bias of 15.
		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++;
				g = *bl++;
				b = *bl++;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				*(unsigned int*)dest = Surf_PackE5BRG9(r,g,b,shift+8);
				dest += 4;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	case PTI_RGBA16F:
		stride = (lm->width-smax)<<3;
		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++;
				g = *bl++;
				b = *bl++;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				Surf_PackRGB16F(dest, r,g,b,1<<(shift+8));
				dest += 8;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	case PTI_RGBA32F:
		shift = 1u<<(shift+8);
		stride = (lm->width-smax)<<4;
		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++;
				g = *bl++;
				b = *bl++;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				Surf_PackRGBX32F(dest, r,g,b,shift);
				dest += sizeof(float)*4;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	case PTI_RGBX8:
	case PTI_RGBA8:
		stride = (lm->width-smax)<<2;

		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++ >> shift;
				g = *bl++ >> shift;
				b = *bl++ >> shift;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				// quake 2 method, scale highest down to
				// maintain hue
				m = max(max(r, g), b);
				if (m > 255)
				{
					r *= 255.0/m;
					g *= 255.0/m;
					b *= 255.0/m;
				}

				dest[0] = r;
				dest[1] = g;
				dest[2] = b;
				dest[3] = 255;

				dest += 4;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	case PTI_BGRX8:
	case PTI_BGRA8:
		stride = (lm->width-smax)<<2;

		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++ >> shift;
				g = *bl++ >> shift;
				b = *bl++ >> shift;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				// quake 2 method, scale highest down to
				// maintain hue
				m = max(max(r, g), b);
				if (m > 255)
				{
					r *= 255.0/m;
					g *= 255.0/m;
					b *= 255.0/m;
				}

				dest[0] = b;
				dest[1] = g;
				dest[2] = r;
				dest[3] = 255;

				dest += 4;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
/*
	case PTI_BGRX8:
	case PTI_BGRA8:
		stride = (lm->width-smax)<<2;

		bl = blocklights;

		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++ >> shift;
				g = *bl++ >> shift;
				b = *bl++ >> shift;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				if (r > 255)
					dest[2] = 255;
				else if (r < 0)
					dest[2] = 0;
				else
					dest[2] = r;

				if (g > 255)
					dest[1] = 255;
				else if (g < 0)
					dest[1] = 0;
				else
					dest[1] = g;

				if (b > 255)
					dest[0] = 255;
				else if (b < 0)
					dest[0] = 0;
				else
					dest[0] = b;

				dest[3] = 255;
				dest += 4;
			}
			if (stainsrc)
				stainsrc += (lmwidth - smax)*3;
		}
		break;
*/
	case PTI_RGB565:
		stride = (lm->width-smax)<<1;

		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++ >> shift;
				g = *bl++ >> shift;
				b = *bl++ >> shift;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				// quake 2 method, scale highest down to
				// maintain hue
				m = max(max(r, g), b);
				if (m > 255)
				{
					r *= 255.0/m;
					g *= 255.0/m;
					b *= 255.0/m;
				}

				*(unsigned short*)dest = (b>>3) | ((g>>2)<<5) | ((r>>3)<<11);
				dest += 2;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	case PTI_RGBA4444:
		stride = (lm->width-smax)<<1;

		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++ >> shift;
				g = *bl++ >> shift;
				b = *bl++ >> shift;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				// quake 2 method, scale highest down to
				// maintain hue
				m = max(max(r, g), b);
				if (m > 255)
				{
					r *= 255.0/m;
					g *= 255.0/m;
					b *= 255.0/m;
				}

				*(unsigned short*)dest = ((r>>4)<<12) | ((g>>4)<<8) | ((b>>4)<<4) | 0x000f;
				dest += 2;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	case PTI_RGBA5551:
		stride = (lm->width-smax)<<1;

		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++ >> shift;
				g = *bl++ >> shift;
				b = *bl++ >> shift;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				// quake 2 method, scale highest down to
				// maintain hue
				m = max(max(r, g), b);
				if (m > 255)
				{
					r *= 255.0/m;
					g *= 255.0/m;
					b *= 255.0/m;
				}

				*(unsigned short*)dest = ((r>>3)<<11) | ((g>>3)<<6) | ((b>>3)<<1) | 0x0001;
				dest += 2;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	case PTI_ARGB4444:
		stride = (lm->width-smax)<<1;

		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++ >> shift;
				g = *bl++ >> shift;
				b = *bl++ >> shift;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				// quake 2 method, scale highest down to
				// maintain hue
				m = max(max(r, g), b);
				if (m > 255)
				{
					r *= 255.0/m;
					g *= 255.0/m;
					b *= 255.0/m;
				}

				*(unsigned short*)dest = 0xf000 | ((r>>4)<<8) | ((g>>4)<<4) | ((b>>4)<<0);
				dest += 2;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	case PTI_ARGB1555:
		stride = (lm->width-smax)<<1;

		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++ >> shift;
				g = *bl++ >> shift;
				b = *bl++ >> shift;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				// quake 2 method, scale highest down to
				// maintain hue
				m = max(max(r, g), b);
				if (m > 255)
				{
					r *= 255.0/m;
					g *= 255.0/m;
					b *= 255.0/m;
				}

				*(unsigned short*)dest = 0x8000 | ((r>>3)<<10) | ((g>>3)<<5) | ((b>>3)<<0);
				dest += 2;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	case PTI_RGB8:
		stride = lm->width*3 - (smax*3);

		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				r = *bl++ >> shift;
				g = *bl++ >> shift;
				b = *bl++ >> shift;

				if (stainsrc)	// merge in stain
				{
					r = (127+r*(*stainsrc++)) >> 8;
					g = (127+g*(*stainsrc++)) >> 8;
					b = (127+b*(*stainsrc++)) >> 8;
				}

				// quake 2 method, scale highest down to
				// maintain hue
				m = max(max(r, g), b);
				if (m > 255)
				{
					r *= 255.0/m;
					g *= 255.0/m;
					b *= 255.0/m;
				}

				dest[0] = r;
				dest[1] = g;
				dest[2] = b;
				dest += 3;
			}
			if (stainsrc)
				stainsrc += (lm->width - smax)*3;
		}
		break;
	}
}
static void Surf_StoreLightmap_Lum(qbyte *dest, unsigned int *bl, int smax, int tmax, unsigned int shift, stmap *stainsrc, unsigned int lmwidth)
{
	int t;
	unsigned int i, j;

	for (i=0 ; i<tmax ; i++, dest += lmwidth)
	{
		for (j=0 ; j<smax ; j++)
		{
			t = *bl++;
			t >>= shift;
			if (t > 255)
				t = 255;
			dest[j] = t;
		}
	}
}

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
static void Surf_BuildLightMap (model_t *model, msurface_t *surf, int map, int shift, int ambient, int *d_lightstylevalue)
{
	int			smax = (surf->extents[0]>>surf->lmshift)+1;
	int			tmax = (surf->extents[1]>>surf->lmshift)+1;
	int			t;
	int			i;
	size_t		size = (size_t)smax*tmax;
	unsigned	scalergb[3];
	unsigned	scale;
	int			maps;
	unsigned	*bl;
	glRect_t	*theRect;
	void		*dest;
	void		*deluxedest;
	void		*stainsrc;
	lightmapinfo_t *lm = lightmap[surf->lightmaptexturenums[map]];
	qbyte		*src = surf->samples;

	shift += 7; // increase to base value
	surf->cached_dlight = (surf->dlightframe == r_dlightframecount);

	if (size > maxblocksize)
	{	//fixme: fill in?
		//Threading: this should not be a problem, all surfaces should have been built from the main thread at map load so it should have maxed it there.
		BZ_Free(blocklights);
		BZ_Free(blocknormals);

		maxblocksize = size;
		blocknormals = BZ_Malloc(maxblocksize * sizeof(*blocknormals));	//already a vector
		blocklights = BZ_Malloc(maxblocksize * 3*sizeof(*blocklights));
	}

	//make sure we flag the output rect properly.
	theRect = &lm->rectchange;
	if (theRect->t > surf->light_t[map])
		theRect->t = surf->light_t[map];
	if (theRect->l > surf->light_s[map])
		theRect->l = surf->light_s[map];
	if (theRect->r < surf->light_s[map]+smax)
		theRect->r = surf->light_s[map]+smax;
	if (theRect->b < surf->light_t[map]+tmax)
		theRect->b = surf->light_t[map]+tmax;

	dest = lm->lightmaps + (surf->light_t[map] * lm->width + surf->light_s[map]) * lm->pixbytes;
	if (!r_stains.value || !surf->stained)
		stainsrc = NULL;
	else
		stainsrc = lm->stainmaps + (surf->light_t[map] * lm->width + surf->light_s[map]) * 3;

	lm->modified = true;
	if (lm->hasdeluxe && model->deluxdata)
	{
		lightmapinfo_t *dlm = lightmap[surf->lightmaptexturenums[map]+1];
		dlm->modified = true;
		theRect = &dlm->rectchange;
		if (theRect->t > surf->light_t[map])
			theRect->t = surf->light_t[map];
		if (theRect->l > surf->light_s[map])
			theRect->l = surf->light_s[map];
		if (theRect->r < surf->light_s[map]+smax)
			theRect->r = surf->light_s[map]+smax;
		if (theRect->b < surf->light_t[map]+tmax)
			theRect->b = surf->light_t[map]+tmax;

		deluxedest = dlm->lightmaps + (surf->light_t[map] * dlm->width + surf->light_s[map]) * dlm->pixbytes;

		Surf_BuildDeluxMap(model, surf, deluxedest, dlm, blocknormals);
	}

	//nettest (SUNVIS): copy this face's baked sun-visibility block into the page at the SAME
	//atlas rect the lightmap just went to.  Static data, so unlike the lightmap there is no
	//style scaling, no stain and no dlight - a straight row-by-row blit.
	//
	//Two traps, both easy to get wrong:
	// * surf->samples is a BYTE pointer into lightdata scaled by the lightmap format (4 for
	//   E5BGR9, 3 for RGB8, 1 for L8), but SUNVIS is always 1 byte per LUXEL - so the luxel
	//   index has to divide that scale back out.
	// * SUNVIS is style-INDEPENDENT: one value per luxel, style 0 only. The deluxemap advances
	//   by size per style; this must not. Only map 0 writes.
	if (map == 0 && lm->sunvis_pixels && model->sunvisdata && surf->samples)
	{
		unsigned int lofsscale;
		switch(model->lightmaps.fmt)
		{
		case LM_E5BGR9:	lofsscale = 4;	break;
		case LM_RGB8:	lofsscale = 3;	break;
		default:
		case LM_L8:		lofsscale = 1;	break;
		}
		if (lofsscale)
		{
			size_t luxel = (size_t)(surf->samples - model->lightdata) / lofsscale;
			//bounds-check against the lump: the face-load path only validates style 0's extent.
			if (luxel + (size_t)smax*tmax <= (size_t)model->lightdatasize / lofsscale)
			{
				qbyte *svsrc = model->sunvisdata + luxel;
				qbyte *svdst = lm->sunvis_pixels + surf->light_t[map] * lm->width + surf->light_s[map];
				int svrow, svcol;
				//INVERTED on the way in: the lump stores sun VISIBILITY (255 = fully lit) because
				//that is the natural thing to bake, but the TEXTURE stores sun OCCLUSION so that
				//black (0) means "fully lit, dynamic shadow at full strength".  That makes every
				//failure mode - sampler unbound, texture never created, format unsupported -
				//land on the OLD behaviour instead of silently deleting every shadow in the game.
				for (svrow = 0; svrow < tmax; svrow++)
				{
					for (svcol = 0; svcol < smax; svcol++)
						svdst[svcol] = 255 - svsrc[svcol];
					svsrc += smax;
					svdst += lm->width;
				}
				lm->sunvis_modified = true;
			}
		}
	}

	if (lm->fmt != PTI_L8)
	{
		// set to full bright if no light data
		if (ambient < 0)
		{
			t = (-1-ambient)*255;
			for (i=0 ; i<size*3 ; i++)
				blocklights[i] = t;
			for (maps = 0 ; maps < MAXCPULIGHTMAPS ; maps++)
			{
				surf->cached_light[maps] = -1-ambient;
				surf->cached_colour[maps] = 0xff;
			}
		}
		else if (r_fullbright.value>0)	//not qw
		{
			for (i=0 ; i<size*3 ; i++)
				blocklights[i] = r_fullbright.value*255*256;
			if (!surf->samples)
			{
				surf->cached_light[0] = d_lightstylevalue[0];
				surf->cached_colour[0] = cl_lightstyle[0].colourkey;
			}
			else
			{
				for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
				{
					surf->cached_light[maps] = d_lightstylevalue[surf->styles[maps]];
					surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
				}
			}
		}
		else if (!model->lightdata)
		{
			/*fullbright if map is not lit. but not overbright*/
			for (i=0 ; i<size*3 ; i++)
				blocklights[i] = 128*256;
		}
		else if (!surf->samples)
		{
			/*no samples, but map is otherwise lit = pure black*/
			for (i=0 ; i<size*3 ; i++)
				blocklights[i] = 0;
			surf->cached_light[0] = d_lightstylevalue[0];
			surf->cached_colour[0] = cl_lightstyle[0].colourkey;
		}
		else
		{
// clear to no light
			t = ambient;
			if (t == 0)
				memset(blocklights, 0, size*3*sizeof(*bl));
			else
			{
				for (i=0 ; i<size*3 ; i++)
				{
					blocklights[i] = t;
				}
			}

// add all the lightmaps
			if (src)
			{
				if (model->lightmaps.prebaked)
					Sys_Error("Surf_BuildLightMap: q3bsp");
				switch(model->lightmaps.fmt)
				{
				case LM_E5BGR9:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
					{
						surf->cached_light[maps] = scale = d_lightstylevalue[surf->styles[maps]];	// 8.8 fraction
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						if (scale)
						{
							VectorScale(cl_lightstyle[surf->styles[maps]].colours, scale, scalergb);
							for (i=0 ; i<size ; i++)
							{
								unsigned int l = ((unsigned int*)src)[i];
								float e = rgb9e5tab[l>>27]*(1<<7);
								blocklights[i*3+0] += scalergb[0] * e * ((l>> 0)&0x1ff);
								blocklights[i*3+1] += scalergb[1] * e * ((l>> 9)&0x1ff);
								blocklights[i*3+2] += scalergb[2] * e * ((l>>18)&0x1ff);
							}
						}
						src += size*4;	// skip to next lightmap
					}
					break;
				case LM_RGB8:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
					{
						surf->cached_light[maps] = scale = d_lightstylevalue[surf->styles[maps]];
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						if (scale)
						{
							VectorScale(cl_lightstyle[surf->styles[maps]].colours, scale, scalergb);
							bl = blocklights;
							for (i=0 ; i<size ; i++)
							{
								*bl++		+=   *src++ * scalergb[0];
								*bl++		+=   *src++ * scalergb[1];
								*bl++		+=   *src++ * scalergb[2];
							}
						}
						else
							src += size*3;	// skip to next lightmap
					}
					break;

				case LM_L8:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ;
						 maps++)
					{
						surf->cached_light[maps] = scale = d_lightstylevalue[surf->styles[maps]];	// 8.8 fraction
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						if (scale)
						{
							VectorScale(cl_lightstyle[surf->styles[maps]].colours, scale, scalergb);
							bl = blocklights;
							for (i=0 ; i<size ; i++)
							{
								*bl++		+= *src * scalergb[0];
								*bl++		+= *src * scalergb[1];
								*bl++		+= *src * scalergb[2];
								src++;
							}
						}
						else
							src += size;	// skip to next lightmap
					}
					break;
				}
			}
		}

		// add all the dynamic lights
		if (surf->dlightframe == r_dlightframecount)
			Surf_AddDynamicLights_RGB (surf);

		Surf_StoreLightmap_RGB(dest, blocklights, smax, tmax, shift, stainsrc, lm);
	}
	else
	{
		// set to full bright if no light data
		if (ambient < 0)
		{
			t = (-1-ambient)*255;
			for (i=0 ; i<size ; i++)
				blocklights[i] = t;
			for (maps = 0 ; maps < MAXCPULIGHTMAPS ; maps++)
			{
				surf->cached_light[maps] = -1-ambient;
				surf->cached_colour[maps] = 0xff;
			}
		}
		else if (r_fullbright.value > 0)
		{	//r_fullbright is meant to be a scaler.
			for (i=0 ; i<size ; i++)
				blocklights[i] = r_fullbright.value*255*256;
			if (!surf->samples)
			{
				surf->cached_light[0] = d_lightstylevalue[0];
				surf->cached_colour[0] = cl_lightstyle[0].colourkey;
			}
			else
			{
				for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
				{
					surf->cached_light[maps] = d_lightstylevalue[surf->styles[maps]];
					surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
				}
			}
		}
		else if (!model->lightdata)
		{	//no scalers here.
			for (i=0 ; i<size ; i++)
				blocklights[i] = 255*256;
			surf->cached_light[0] = d_lightstylevalue[0];
			surf->cached_colour[0] = cl_lightstyle[0].colourkey;
		}
		//surfaces with no light data on lit maps are black
		else if (!surf->samples)
		{
			for (i=0 ; i<size*3 ; i++)
				blocklights[i] = 0;
			surf->cached_light[0] = d_lightstylevalue[0];
			surf->cached_colour[0] = cl_lightstyle[0].colourkey;
		}
		else
		{
// clear to no light
			for (i=0 ; i<size ; i++)
				blocklights[i] = 0;

// add all the lightmaps
			if (src)
			{
				switch(model->lightmaps.fmt)
				{
				case LM_E5BGR9:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
					{
						scale = d_lightstylevalue[surf->styles[maps]];
						surf->cached_light[maps] = scale;	// 8.8 fraction
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						for (i=0 ; i<size ; i++)
						{
							unsigned int lm = ((unsigned int *)src)[i];
							blocklights[i] += max3(((lm>>0)&0x1ff),((lm>>9)&0x1ff),((lm>>18)&0x1ff)) * scale * (rgb9e5tab[lm>>27]*(1<<7));
						}
						src += size*4;	// skip to next lightmap
					}
					break;
				case LM_RGB8:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
					{
						scale = d_lightstylevalue[surf->styles[maps]];
						surf->cached_light[maps] = scale;	// 8.8 fraction
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						for (i=0 ; i<size ; i++)
							blocklights[i] += max3(src[i*3],src[i*3+1],src[i*3+2]) * scale;
						src += size*3;	// skip to next lightmap
					}
					break;
				case LM_L8:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
					{
						scale = d_lightstylevalue[surf->styles[maps]];
						surf->cached_light[maps] = scale;	// 8.8 fraction
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						for (i=0 ; i<size ; i++)
							blocklights[i] += src[i] * scale;
						src += size;	// skip to next lightmap
					}
					break;
				}
			}
// add all the dynamic lights
			if (surf->dlightframe == r_dlightframecount)
				Surf_AddDynamicLights_Lum (surf);
		}

		Surf_StoreLightmap_Lum(dest, blocklights, smax, tmax, shift, stainsrc, lm->width);
	}
}

#if defined(THREADEDWORLD) && (defined(Q1BSPS)||defined(Q2BSPS))
static void Surf_BuildLightMap_Worker (model_t *wmodel, msurface_t *surf, int shift, int ambient, int *d_lightstylevalue)
{
	int			smax, tmax;
	int			t;
	int			i, j;
	size_t		size;
	lightmapinfo_t *lm = lightmap[surf->lightmaptexturenums[0]];
	qbyte		*src;
	unsigned	scalergb[3];
	unsigned	scale;
	int			maps;
	unsigned	*bl;
	qbyte		*dest;
	qbyte		*deluxedest;
	stmap		*stainsrc;
	glRect_t	*theRect;

	static size_t maxblocksize;
	static vec3_t *blocknormals;
	static unsigned int *blocklights;

	shift += 7; // increase to base value
	surf->cached_dlight = false;

	smax = (surf->extents[0]>>surf->lmshift)+1;
	tmax = (surf->extents[1]>>surf->lmshift)+1;
	size = (size_t)smax*tmax;
	src = surf->samples;

	if (size > maxblocksize)
	{	//fixme: fill in?
		maxblocksize = size;
		blocknormals = BZ_Realloc(blocknormals, maxblocksize * sizeof(*blocknormals));	//already a vector
		blocklights = BZ_Realloc(blocklights, maxblocksize * 3*sizeof(*blocklights));
	}

	dest = lm->lightmaps + (surf->light_t[0] * lm->width + surf->light_s[0]) * lm->pixbytes;
	if (!r_stains.value || !surf->stained)
		stainsrc = NULL;
	else
		stainsrc = lm->stainmaps + (surf->light_t[0] * lm->width + surf->light_s[0]) * 3;

	if (lm->hasdeluxe)
	{
		lightmapinfo_t *dlm = lightmap[surf->lightmaptexturenums[0]+1];
		deluxedest = dlm->lightmaps + (surf->light_t[0] * dlm->width + surf->light_s[0]) * dlm->pixbytes;

		Surf_BuildDeluxMap(wmodel, surf, deluxedest, lm, blocknormals);
	}

	if (lm->fmt != PTI_L8)
	{
		// set to full bright if no light data
		if (ambient < 0)
		{	//abslight for hexen2
			t = (-1-ambient)*255;
			for (i=0 ; i<size*3 ; i++)
			{
				blocklights[i] = t;
			}

			for (maps = 0 ; maps < MAXCPULIGHTMAPS ; maps++)
			{
				surf->cached_light[maps] = -1-ambient;
				surf->cached_colour[maps] = 0xff;
			}
		}
		else if (r_fullbright.value>0)
		{	//fullbright cheat
			for (i=0 ; i<size*3 ; i++)
			{
				blocklights[i] = r_fullbright.value*255*256;
			}
		}
		else if (!wmodel->lightdata)
		{	/*fullbright if map is not lit. but not overbright*/
			for (i=0 ; i<size*3 ; i++)
			{
				blocklights[i] = 128*256;
			}
		}
		else
		{
// clear to no light
			t = ambient;
			if (t == 0)
				memset(blocklights, 0, size*3*sizeof(*bl));
			else
			{
				for (i=0 ; i<size*3 ; i++)
				{
					blocklights[i] = t;
				}
			}

// add all the lightmaps
			if (src)
			{
				if (wmodel->lightmaps.prebaked)	//rgb
				{
					/*q3 lightmaps are meant to be pre-built
					this code is misguided, and ought never be executed anyway.
					*/
					int pixbytes = lm->pixbytes;
					bl = blocklights;
					for (i = 0; i < tmax; i++)
					{
						for (j = 0; j < smax; j++)
						{
							bl[0]		= 255*src[(i*pixbytes+j)*3];
							bl[1]		= 255*src[(i*pixbytes+j)*3+1];
							bl[2]		= 255*src[(i*pixbytes+j)*3+2];
							bl+=3;
						}
					}
					Sys_Error("Surf_BuildLightMap_Worker: q3bsp");
				}
				else switch(wmodel->lightmaps.fmt)
				{
				case LM_E5BGR9:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
					{
						surf->cached_light[maps] = scale = d_lightstylevalue[surf->styles[maps]];	// 8.8 fraction
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						if (scale)
						{
							VectorScale(cl_lightstyle[surf->styles[maps]].colours, scale, scalergb);
							for (i=0 ; i<size ; i++)
							{
								unsigned int l = ((unsigned int*)src)[i];
								float e = rgb9e5tab[l>>27]*(1u<<7);
								blocklights[i*3+0] += scalergb[0] * e * ((l>> 0)&0x1ff);
								blocklights[i*3+1] += scalergb[1] * e * ((l>> 9)&0x1ff);
								blocklights[i*3+2] += scalergb[2] * e * ((l>>18)&0x1ff);
							}
						}
						src += size*4;	// skip to next lightmap
					}
					break;
				case LM_RGB8:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
					{
						surf->cached_light[maps] = scale = d_lightstylevalue[surf->styles[maps]];
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						if (scale)
						{
							VectorScale(cl_lightstyle[surf->styles[maps]].colours, scale, scalergb);
							bl = blocklights;
							for (i=0 ; i<size ; i++)
							{
								*bl++		+=   *src++ * scalergb[0];
								*bl++		+=   *src++ * scalergb[1];
								*bl++		+=   *src++ * scalergb[2];
							}
						}
						else
							src += size*3;	// skip to next lightmap
					}
					break;

				case LM_L8:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ;
						 maps++)
					{
						surf->cached_light[maps] = scale = d_lightstylevalue[surf->styles[maps]];	// 8.8 fraction
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						if (scale)
						{
							VectorScale(cl_lightstyle[surf->styles[maps]].colours, scale, scalergb);
							bl = blocklights;
							for (i=0 ; i<size ; i++)
							{
								*bl++		+= *src * scalergb[0];
								*bl++		+= *src * scalergb[1];
								*bl++		+= *src * scalergb[2];
								src++;
							}
						}
						else
							src += size;	// skip to next lightmap
					}
					break;
				}
			}
		}

		if (!r_stains.value || !surf->stained)
			stainsrc = NULL;

		Surf_StoreLightmap_RGB(dest, blocklights, smax, tmax, shift, stainsrc, lm);
	}
	else
	{
	// set to full bright if no light data
		if (r_fullbright.ival)
		{
			for (i=0 ; i<size ; i++)
				blocklights[i] = 255*256;
		}
		else if (!wmodel->lightdata)
		{
			for (i=0 ; i<size*3 ; i++)
			{
				blocklights[i] = 255*256;
			}
			surf->cached_light[0] = d_lightstylevalue[0];
			surf->cached_colour[0] = cl_lightstyle[0].colourkey;
		}
		else
		{
// clear to no light
			for (i=0 ; i<size ; i++)
				blocklights[i] = ambient;

// add all the lightmaps
			if (src)
			{
				switch(wmodel->lightmaps.fmt)
				{
				case LM_E5BGR9:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
					{
						scale = d_lightstylevalue[surf->styles[maps]];
						surf->cached_light[maps] = scale;	// 8.8 fraction
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						if (scale)
							for (i=0 ; i<size ; i++)
							{
								unsigned int lm = ((unsigned int *)lightmap)[i];
								blocklights[i] += max3(((lm>>0)&0x1ff),((lm>>9)&0x1ff),((lm>>18)&0x1ff)) * scale * (rgb9e5tab[lm>>27]*(1<<7));
							}
						lightmap += size*4;	// skip to next lightmap
					}
					break;
				case LM_RGB8:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
					{
						scale = d_lightstylevalue[surf->styles[maps]];
						surf->cached_light[maps] = scale;	// 8.8 fraction
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						if (scale)
							for (i=0 ; i<size ; i++)
								blocklights[i] += max3(src[i*3],src[i*3+1],src[i*3+2]) * scale;
						src += size*3;	// skip to next lightmap
					}
					break;
				case LM_L8:
					for (maps = 0 ; maps < MAXCPULIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE ; maps++)
					{
						scale = d_lightstylevalue[surf->styles[maps]];
						surf->cached_light[maps] = scale;	// 8.8 fraction
						surf->cached_colour[maps] = cl_lightstyle[surf->styles[maps]].colourkey;
						if (scale)
							for (i=0 ; i<size ; i++)
								blocklights[i] += src[i] * scale;
						src += size;	// skip to next lightmap
					}
					break;
				}
			}
		}

		Surf_StoreLightmap_Lum(dest, blocklights, smax, tmax, shift, stainsrc, lm->width);
	}

	//make sure we flag the output rect properly.
	theRect = &lm->rectchange;
	if (theRect->t > surf->light_t[0])
		theRect->t = surf->light_t[0];
	if (theRect->l > surf->light_s[0])
		theRect->l = surf->light_s[0];
	if (theRect->r < surf->light_s[0]+smax)
		theRect->r = surf->light_s[0]+smax;
	if (theRect->b < surf->light_t[0]+tmax)
		theRect->b = surf->light_t[0]+tmax;
	lm->modified = true;

	if (lm->hasdeluxe)
	{
		lightmapinfo_t *dlm = lm+1;
		theRect = &dlm->rectchange;
		if (theRect->t > surf->light_t[0])
			theRect->t = surf->light_t[0];
		if (theRect->l > surf->light_s[0])
			theRect->l = surf->light_s[0];
		if (theRect->r < surf->light_s[0]+smax)
			theRect->r = surf->light_s[0]+smax;
		if (theRect->b < surf->light_t[0]+tmax)
			theRect->b = surf->light_t[0]+tmax;
		dlm->modified = true;
	}
}
#endif

/*
=============================================================

	BRUSH MODELS

=============================================================
*/

/*
================
R_RenderDynamicLightmaps
Multitexture
================
*/
void Surf_RenderDynamicLightmaps (msurface_t *fa)
{
	int			maps;

	//surfaces without lightmaps
	if (fa->lightmaptexturenums[0]<0 || !lightmap)
		return;

	// check for lightmap modification
	if (!fa->samples)
	{
		if (fa->cached_light[0] != d_lightstylevalue[0]
			|| fa->cached_colour[0] != cl_lightstyle[0].colourkey)
			goto dynamic;
	}
	else
	{
		for (maps = 0 ; maps < MAXCPULIGHTMAPS && fa->styles[maps] != INVALID_LIGHTSTYLE ;
			 maps++)
			if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps]
				|| cl_lightstyle[fa->styles[maps]].colourkey != fa->cached_colour[maps])
				goto dynamic;
	}

	if (fa->dlightframe == r_dlightframecount	// dynamic this frame
		|| fa->cached_dlight)			// dynamic previously
	{
		RSpeedLocals();
dynamic:
		RSpeedRemark();

#ifdef _DEBUG
		if ((unsigned)fa->lightmaptexturenums[0] >= numlightmaps)
			Sys_Error("Invalid lightmap index\n");
#endif

		Surf_BuildLightMap (currentmodel, fa, 0, lightmap_shift, r_ambient.value*255, d_lightstylevalue);

		RSpeedEnd(RSPEED_DYNAMIC);
	}
}

#if defined(THREADEDWORLD) && (defined(Q1BSPS)||defined(Q2BSPS))
static void Surf_RenderDynamicLightmaps_Worker (model_t *wmodel, msurface_t *fa, int *d_lightstylevalue)
{
	int			maps;

	//surfaces without lightmaps
	if (fa->lightmaptexturenums[0]<0 || !lightmap)
		return;

	// check for lightmap modification
	if (!fa->samples)
	{
		if (fa->cached_light[0] != d_lightstylevalue[0]
			|| fa->cached_colour[0] != cl_lightstyle[0].colourkey)
			goto dynamic;
	}
	else
	{
		for (maps = 0 ; maps < MAXCPULIGHTMAPS && fa->styles[maps] != INVALID_LIGHTSTYLE ;
			 maps++)
			if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps]
				|| cl_lightstyle[fa->styles[maps]].colourkey != fa->cached_colour[maps])
				goto dynamic;
	}

	return;

dynamic:

#ifdef _DEBUG
	if ((unsigned)fa->lightmaptexturenums[0] >= numlightmaps)
	{
		static float throttle;
		Con_ThrottlePrintf(&throttle, 0, CON_WARNING"Invalid lightmap index\n");
		return;
	}
#endif


	Surf_BuildLightMap_Worker (wmodel, fa, lightmap_shift, r_ambient.value*255, d_lightstylevalue);
}
#endif //THREADEDWORLD

void Surf_RenderAmbientLightmaps (msurface_t *fa, int ambient)
{
	if (!fa->mesh)
		return;

	//surfaces without lightmaps
	if (fa->lightmaptexturenums[0]<0)
		return;

	if (fa->cached_light[0] != ambient || fa->cached_colour[0] != 0xff)
		goto dynamic;

	if (fa->dlightframe == r_dlightframecount	// dynamic this frame
		|| fa->cached_dlight)			// dynamic previously
	{
		RSpeedLocals();
dynamic:
		RSpeedRemark();

		Surf_BuildLightMap (currentmodel, fa, 0, lightmap_shift, -1-ambient, d_lightstylevalue);

		RSpeedEnd(RSPEED_DYNAMIC);
	}
}

/*
=============================================================

	WORLD MODEL

=============================================================
*/



static void Surf_PushChains(batch_t **batches)
{
	batch_t *batch;
	int i;

	if (r_refdef.recurse == R_MAX_RECURSE)
		Sys_Error("Recursed too deep\n");

	if (!r_refdef.recurse)
	{
		for (i = 0; i < SHADER_SORT_COUNT; i++)
		for (batch = batches[i]; batch; batch = batch->next)
		{
			batch->firstmesh = 0;
		}
	}
#if R_MAX_RECURSE > 2
	else if (r_refdef.recurse > 1)
	{
		for (i = 0; i < SHADER_SORT_COUNT; i++)
		for (batch = batches[i]; batch; batch = batch->next)
		{
			/* FTESurf Patch 221: index from the branch's own floor.
			   recursefirst is declared [R_MAX_RECURSE-2] (gl_model.h) -- four
			   entries -- and this branch runs for recurse 2..R_MAX_RECURSE-1, so
			   indexing it by recurse writes past the end at depth 4 and 5, into
			   whatever follows in batch_t.  r_portalrecursion caps live recursion
			   at 2 today (gl_backend.c) so it is not currently reachable, but
			   cfg/testrun/g2a.cfg already sets 3 and the array is exactly the
			   right size once the floor is subtracted.  Paired with the read in
			   Surf_PopChains. */
			batch->recursefirst[r_refdef.recurse-2] = batch->firstmesh;
			batch->firstmesh = batch->meshes;
		}
	}
#endif
	else
	{
		for (i = 0; i < SHADER_SORT_COUNT; i++)
		for (batch = batches[i]; batch; batch = batch->next)
		{
			batch->firstmesh = batch->meshes;
		}
	}
}
static void Surf_PopChains(batch_t **batches)
{
	batch_t *batch;
	int i;

	if (!r_refdef.recurse)
	{
		for (i = 0; i < SHADER_SORT_COUNT; i++)
		for (batch = batches[i]; batch; batch = batch->next)
		{
			batch->meshes = 0;
		}
	}
#if R_MAX_RECURSE > 2
	else if (r_refdef.recurse > 1)
	{
		for (i = 0; i < SHADER_SORT_COUNT; i++)
		for (batch = batches[i]; batch; batch = batch->next)
		{
			batch->meshes = batch->firstmesh;
			//FTESurf Patch 221: the read side of the same -2; see Surf_PushChains.
			batch->firstmesh = batch->recursefirst[r_refdef.recurse-2];
		}
	}
#endif
	else
	{
		for (i = 0; i < SHADER_SORT_COUNT; i++)
		for (batch = batches[i]; batch; batch = batch->next)
		{
			batch->meshes = batch->firstmesh;
			batch->firstmesh = 0;
		}
	}
}

/*
FTESurf Patch 218: put the blended world surfaces back into painter's order.

The world walk emits FRONT-TO-BACK -- near child, then this node's surfaces, then
the far child -- which is right for opaque geometry and exactly inverted for
alpha blending.  Because that order is an exact painter's order for the surfaces
the BSP separates, reading it backwards is an exact back-to-front order, so this
is a reversal and not a sort: no distances, no comparator, no tie-breaks.

Runs on the current view's range only (firstmesh..meshes), so a recursed portal
view reverses its own meshes and leaves the primary view's alone -- see
Surf_PushChains above for how that range is maintained.

The full essay, including what this does NOT fix (ordering BETWEEN batches of
different materials) is on r_blendsort in renderer.c.
*/
static void Surf_SortBlendedChains(batch_t **batches)
{
	batch_t *batch;
	mesh_t *swap;
	int a, b;

	if (!r_blendsort.ival)
		return;

	for (batch = batches[SHADER_SORT_BLEND]; batch; batch = batch->next)
	{
		//guarded rather than relying on the subtraction: meshes is unsigned, so an
		//empty batch would wrap `meshes - 1` to something enormous.
		if (batch->meshes <= batch->firstmesh + 1)
			continue;	//nothing, or one mesh -- already in order either way
		for (a = batch->firstmesh, b = batch->meshes - 1; a < b; a++, b--)
		{
			swap = batch->mesh[a];
			batch->mesh[a] = batch->mesh[b];
			batch->mesh[b] = swap;
		}
	}
}

/*
FTESurf Patch 138: the last cluster pair we actually resolved for the PRIMARY view.

A pvsorigin inside solid, or outside the map entirely, makes InfoForPoint report
cluster -1.  Every consumer then degrades to "no PVS at all" -- and not just for
surfaces.  On a VBSP map VBSP_MarkLeaves bails out on clusters[0] == -1
(mod_vbsp.c:3768) and returns NULL, so the whole-model surface loop runs with no
PVS test AND no frustum test, the static-prop cull is skipped because it is gated
on the same null pointer, and CL_LinkStaticEntities' cull goes with it.  Q1 and
HL maps reach the identical state through Q1BSP_ClusterPVS(-1) -> mod_novis.

Measured on surf_666: 35,302 worldspawn faces and 653 props, with good vis -- the
average cluster sees 1.6% of the map and the worst sees 5.1%.  So stepping into
the void takes you from 1.6% of the map to 100% of it, unculled.  That is the
whole of "super low fps when you're in the void", and Source does not do it
because it keeps the last valid area when the view leaves the world.

Keyed on the world pointer so a stale cluster index cannot survive a map change,
and applied to the primary view ONLY: R_DrawSkyroom bumps r_refdef.recurse and
supplies its own pvsorigin which may legitimately be in solid, and must not
inherit the main view's cache.
*/
static model_t *surf_lastgoodworld;
static int surf_lastgoodcluster[2] = {-1, -1};

/*
FTESurf build 17.  "The view is in the void AND the player is noclipping."

Set once per primary view in Surf_SetupFrame and read by the two entity gates
(CL_LinkStaticEntities and BE_GenModelBatches).  A global rather than a flag in
r_refdef because those two are reached through several call paths that do not
carry the refdef, and because it is strictly per-frame state with one writer.

NOCLIP IS READ FROM THE PMOVE TYPE, not from a cvar or a QC stat.
SV_PMTypeForClient maps MOVETYPE_NOCLIP to PM_SPECTATOR, or to PM_OLD_SPECTATOR
for a client without the newer extensions (sv_user.c:7397-7404), and cl_pred.c
copies it into playerview->pmovetype every frame.  So it is predicted, local,
already there, and correct on a remote server as well as a listen one.

The honest limitation: a real SPECTATOR is the same pm_type, so a spectator in
the void would get this too.  That is arguably right -- a spectator out there
wants to see the map for the same reason -- and this build has no spectators.
*/
qboolean r_voidview;

/*
BUILD 19 -- the entity census, and it is why this cvar is two settings and not
four.

The report was reporting the DECISION and nothing about its consequences, so
"r_voidvis 1 and 2 look the same and cost the same" had no answer in the log.
It does now, and the answer settled it: aimed at the map from outside it, at
EVERY setting including 0 where no gate runs, this reads

    entities 0 (brush 0)

There are nothing out there to drop.  On a VBSP map the static props are emitted
from inside the world model's own prepare-frame (mod_vbsp.c), each one PVS-tested
and radius-culled BEFORE it becomes a visedict, so they never reach the entity
list at all in the void -- and the whole difference between 0 and 1 is the world,
i.e. Patch 178's forcevis.  Both entity gates went; see renderer.c.

Written by BE_GenModelBatches (gl_alias.c), which is the one place every visedict
passes through, and read here.  The world side is r_speeds' job and is not
duplicated.

The counters live in the engine and not in the plugin on purpose: a plugin cannot
reach an engine global, and counting them where the entities are CONSUMED is the
honest place anyway -- it measures what the renderer was handed, not what the
loader believed it emitted.
*/
int r_voidvis_edicts;		/*visedicts offered to the batch generator*/
int r_voidvis_brush;		/*...of which are brush models*/
int r_voidvis_dropped;		/*...dropped before drawing.  Zero since build 19*/

/*
An all-visible PVS, handed to the world through r_refdef.forcedvis.

MEASURED, NOT ASSUMED, and the measurement is why this exists.  The first cut
simply left r_viewcluster at -1, which is the state Patch 138 describes: VBSP's
PrepareFrame bails and the whole model is drawn with no PVS test AND NO FRUSTUM
TEST.  timerefresh on surf_666 from 60,000 units up: 3785 fps with the fallback,
and 27.6 fps without it.  27 fps is precisely the "drops FPS too much" this
feature was asked to avoid, so drawing everything behind the camera as well is
not a cost worth paying to see what is in front of it.

forcevis is the lever that keeps the frustum.  VBSP_MarkLeaves takes
refdef->forcedvis in preference to its own cluster lookup (mod_vbsp.c:4274), and
returning a real vis pointer instead of NULL puts VBSP_PrepareFrame back on the
VBSP_RecursiveWorldNode path -- which frustum-culls and area-culls exactly as it
does indoors.  Set every bit and the only test it loses is the one that has no
answer out here.

The mechanism is not new: this is what the portal and mirror code already does
with a real cluster's PVS.  Sized from the world's own pvsbytes and rebuilt when
that changes, so a map with more clusters cannot read past the end.
*/
static qbyte  *surf_voidvis;
static size_t  surf_voidvisbytes;

static qbyte *Surf_VoidVis(model_t *w)
{
	if (!w || !w->pvsbytes)
		return NULL;
	if (surf_voidvisbytes < w->pvsbytes)
	{
		surf_voidvis = BZ_Realloc(surf_voidvis, w->pvsbytes);
		surf_voidvisbytes = w->pvsbytes;
		memset(surf_voidvis, 0xff, surf_voidvisbytes);
	}
	return surf_voidvis;
}

static qboolean Surf_PlayerIsNoclipping(void)
{
	if (!r_refdef.playerview)
		return false;
	return r_refdef.playerview->pmovetype == PM_SPECTATOR ||
		   r_refdef.playerview->pmovetype == PM_OLD_SPECTATOR;
}

/*
Build 17, and it is here because four benchmark runs were spent guessing at it.

The measurement said r_voidvis was slowing the frame down INSIDE the map, where
both halves of the gate should have been false -- and from the outside there is
no way to tell which half was wrong, or whether the position under test was
where the config thought it was.  Every input to the decision, printed at
developer 1 and only when one of them CHANGES, so a benchmark log carries the
gate's own reasoning next to the numbers it produced instead of beside them.

Change-triggered rather than per-frame: this is called once per view, so an
unconditional print would be 500 lines a second and would itself be the slowest
thing in the frame.
*/
static void Surf_VoidVisReport(qboolean fired)
{
	static int	lastcluster = -2;
	static int	lastpm = -2;
	static int	lastvv = -2;
	static int	lastfired = -2;

	if (!developer.ival)
		return;
	if (r_viewcluster == lastcluster && lastpm == (r_refdef.playerview?r_refdef.playerview->pmovetype:-1) &&
		lastvv == r_voidvis.ival && lastfired == (int)fired)
		return;

	lastcluster = r_viewcluster;
	lastpm = r_refdef.playerview?r_refdef.playerview->pmovetype:-1;
	lastvv = r_voidvis.ival;
	lastfired = fired;

	Con_Printf("voidvis: cluster %i  pmovetype %i (noclip %i)  r_voidvis %i  -> %s\n",
		lastcluster, lastpm, Surf_PlayerIsNoclipping()?1:0, lastvv,
		fired?"VOID VIEW":"off");
	/*
	  Build 19.  LAST FRAME'S counts, deliberately: this runs in Surf_SetupFrame,
	  before BE_GenModelBatches has touched them this frame.  One frame stale is
	  the right trade for keeping the census where the rest of the gate's
	  reasoning is printed -- and the numbers that matter here are steady-state,
	  not per-frame transients.

	  "entities N (brush B), dropped D" reads directly: D of 0 with N large is a
	  mode doing nothing, and B tells you what mode 2 alone could ever add.
	*/
	Con_Printf("        entities %i (brush %i) -- world counts are r_speeds\n",
		r_voidvis_edicts, r_voidvis_brush);
}

//most of this is a direct copy from gl
void Surf_SetupFrame(void)
{
	vec3_t	pvsorg;
	int viewcontents;

	if (!cl.worldmodel || cl.worldmodel->loadstate!=MLS_LOADED)
		r_refdef.flags |= RDF_NOWORLDMODEL;

	R_AnimateLight();

	if (r_refdef.recurse)
	{
		VectorCopy(r_refdef.pvsorigin, pvsorg);
	}
	else
	{
		VectorCopy(r_refdef.vieworg, pvsorg);
		R_UpdateHDR(r_refdef.vieworg);
	}

	r_viewarea = 0;
	viewcontents = 0;
	if (r_refdef.flags & RDF_NOWORLDMODEL)
	{
	}
	else if (cl.worldmodel && cl.worldmodel->loadstate == MLS_LOADED && cl.worldmodel->funcs.InfoForPoint)
	{
		vec3_t	temp;
		unsigned int cont2;
		int area2;
		cl.worldmodel->funcs.InfoForPoint (cl.worldmodel, pvsorg, &r_viewarea, &r_viewcluster, &viewcontents);
		// check above and below so crossing solid water doesn't draw wrong
		if (!viewcontents)
		{	// look down a bit
			VectorCopy (pvsorg, temp);
			temp[2] -= 16;
			cl.worldmodel->funcs.InfoForPoint (cl.worldmodel, temp, &area2, &r_viewcluster2, &cont2);
			if (cont2 & FTECONTENTS_SOLID)
				r_viewcluster2 = r_viewcluster;
		}
		else
		{	// look up a bit
			VectorCopy (pvsorg, temp);
			temp[2] += 16;
			cl.worldmodel->funcs.InfoForPoint (cl.worldmodel, temp, &area2, &r_viewcluster2, &cont2);
			if (cont2 & FTECONTENTS_SOLID)
				r_viewcluster2 = r_viewcluster;
		}
	}
	else
	{
		r_viewcluster = -1;
		r_viewcluster2 = -1;
	}

	/*
	  FTESurf Patch 138 -- the void.  See the note on surf_lastgoodcluster.

	  Both clusters, not just the first: MarkLeaves keys its PVS cache on the
	  PAIR, so a mixed one would thrash it every frame.

	  r_novis 1 still forces the old behaviour, which is what makes this
	  A/B-able: with the fallback working, r_novis 0 and r_novis 1 in the void
	  are the difference between 1.6% of the map and all of it.

	  BUILD 17 puts that reversal behind r_voidvis, for noclip only, because
	  falling into the void and flying out of it want opposite things -- see
	  r_voidview above.
	*/

	/*
	  Cleared for EVERY view, including the recursive ones, and cleared before
	  anything below can set it.  A skyroom or a mirror renders with its own
	  pvsorigin that may legitimately be in solid (see the note at the top of
	  this function), and it must not inherit the main view's answer -- but it
	  also runs AFTER the main view has already set this, so leaving a stale
	  true here would silently drop every entity out of the reflection.
	*/
	r_voidview = false;

	/*
	  And the forced vis with it, for the primary view only.

	  The portal code sets forcevis for the recursive views it renders and
	  clears it again itself, so clearing it here for recurse > 0 would take a
	  mirror's own vis away from it mid-frame.  For the primary view nothing
	  else ever sets it, so leaving a stale one from the frame you flew back
	  indoors would draw the whole map from then on -- silently, and only until
	  the next map change.
	*/
	if (!r_refdef.recurse)
	{
		r_refdef.forcevis = false;
		r_refdef.forcedvis = NULL;
	}

	if (!r_refdef.recurse && !(r_refdef.flags & RDF_NOWORLDMODEL) && cl.worldmodel)
	{
		if (surf_lastgoodworld != cl.worldmodel)
		{	//new world: the old cluster indices mean nothing in it.
			surf_lastgoodworld = cl.worldmodel;
			surf_lastgoodcluster[0] = surf_lastgoodcluster[1] = -1;
		}

		/*
		  Build 17: in the void, noclipping, and asked for.  Leave the cluster
		  at -1 so the whole world is drawn, and tell the entity gates to skip.

		  BOTH HALVES OF THE GATE MATTER and each is load-bearing on its own.
		  Without the cluster test this would fire while noclipping INSIDE the
		  map, where vis is working perfectly and there is nothing to fix.
		  Without the noclip test it would fire on the ordinary fall into the
		  void, which is the exact case Patch 138 exists to keep playable.
		*/
		if (r_viewcluster == -1 && r_voidvis.ival && Surf_PlayerIsNoclipping())
		{
			r_voidview = true;

			/*
			  All bits set, so every cluster is "visible" and the world falls
			  back on its frustum and area tests alone -- see Surf_VoidVis.

			  Only for the PRIMARY view, which is also why it is safe to own
			  this field here: the portal and mirror code sets forcevis for the
			  RECURSIVE views it renders (gl_rmain.c:1225), always with
			  recurse > 0, so nothing else is ever competing for it at this
			  level.  If the buffer cannot be had we simply do not force, and
			  fall through to the old whole-model draw rather than to nothing.
			*/
			r_refdef.forcedvis = Surf_VoidVis(cl.worldmodel);
			r_refdef.forcevis = !!r_refdef.forcedvis;
			Surf_VoidVisReport(true);
		}
		else if (r_viewcluster == -1)
		{
			Surf_VoidVisReport(false);	//before the cluster below is overwritten, or the print lies about it

			/*RANGE-CHECKED, and not as a formality.

			  The pointer test above and the two invalidations in Surf_NewMap /
			  Surf_PreNewMap both assume a new world means a new address -- but
			  Mod_ClearAll frees the old model first, so the allocator is free to
			  hand the same address straight back.  A cluster index from the
			  previous map would then reach VBSP_ClusterPVS, which indexes
			  prv->vis->bitofs[cluster] with NO upper bound of its own
			  (mod_vbsp.c) -- an out-of-range read on the first rendered frame of
			  the new map.  That is an intermittent crash whose cause depends on
			  the allocator, which is the worst kind to be left holding.

			  Bounding against the world's own numclusters makes the cache safe
			  whether or not any invalidation fired.  Everything else here is an
			  optimisation; this line is the correctness.*/
			if (surf_lastgoodcluster[0] < cl.worldmodel->numclusters &&
				surf_lastgoodcluster[1] < cl.worldmodel->numclusters)
			{
				r_viewcluster  = surf_lastgoodcluster[0];
				r_viewcluster2 = surf_lastgoodcluster[1];
			}
		}
		else
		{
			Surf_VoidVisReport(false);
			surf_lastgoodcluster[0] = r_viewcluster;
			surf_lastgoodcluster[1] = r_viewcluster2;
		}
	}

#ifdef TERRAIN
	if (!(r_refdef.flags & RDF_NOWORLDMODEL) && cl.worldmodel && cl.worldmodel->terrain)
	{
		viewcontents |= Heightmap_PointContents(cl.worldmodel, NULL, pvsorg);
	}
#endif

	/*pick up any extra water entities*/
	{
		vec3_t t1,t2;
		VectorCopy(pmove.player_mins, t1);
		VectorCopy(pmove.player_maxs, t2);
		VectorClear(pmove.player_maxs);
		VectorClear(pmove.player_mins);
		viewcontents |= PM_ExtraBoxContents(pvsorg);
		VectorCopy(t1, pmove.player_mins);
		VectorCopy(t2, pmove.player_maxs);
	}
	if (!r_refdef.recurse)
	{
		r_viewcontents = viewcontents;
		if (!r_secondaryview)
			V_SetContentsColor (viewcontents);
	}


	if (r_refdef.playerview->audio.defaulted)
	{
		//first scene is the 'main' scene and audio defaults to that (unless overridden later in the frame)
		r_refdef.playerview->audio.defaulted = false;
		r_refdef.playerview->audio.entnum = r_refdef.playerview->viewentity;
		VectorCopy(r_refdef.vieworg, r_refdef.playerview->audio.origin);
		AngleVectors(r_refdef.viewangles, r_refdef.playerview->audio.forward,r_refdef.playerview->audio.right, r_refdef.playerview->audio.up);
//		I'm fed up of openal users getting audio bugs when underwater.
//		if (r_viewcontents & FTECONTENTS_FLUID)
//			r_refdef.playerview->audio.reverbtype = 1;
//		else
			r_refdef.playerview->audio.reverbtype = 0;
		VectorCopy(r_refdef.playerview->simvel, r_refdef.playerview->audio.velocity);
	}
}

/*
static mesh_t *surfbatchmeshes[256];
static void Surf_BuildBrushBatch(batch_t *batch)
{
	model_t *model = batch->ent->model;
	unsigned int i;
	batch->mesh = surfbatchmeshes;
	batch->meshes = batch->surf_count;
	for (i = 0; i < batch->surf_count; i++)
	{
		surfbatchmeshes[i] = model->surfaces[batch->surf_first + i].mesh;
	}
}
*/

void Surf_GenBrushBatches(batch_t **batches, entity_t *ent)
{
	int i;
	msurface_t *s;
	batch_t *ob;
	model_t *model;
	batch_t *b;
	unsigned int bef;

	model = ent->model;

	if (R_CullEntityBox (ent, model->mins, model->maxs))
		return;

#ifdef RTLIGHTS
	if (BE_LightCullModel(ent->origin, model))
		return;
#endif

// calculate dynamic lighting for bmodel if it's not an
// instanced model
	if (!model->lightmaps.prebaked && lightmap && !(webo_blocklightmapupdates&1))
	{
		int k;

		currententity = ent;
		currentmodel = ent->model;
		if (model->nummodelsurfaces != 0 && r_dlightlightmaps && model->funcs.MarkLights)
		{
			for (k=rtlights_first; k<RTL_FIRST; k++)
			{
				if (!cl_dlights[k].radius)
					continue;
				if (!(cl_dlights[k].flags & LFLAG_LIGHTMAP))
					continue;
				if ((cl_dlights[k].flags & LFLAG_NORMALMODE) && r_shadow_realtime_dlight.ival)
					continue;
				if ((cl_dlights[k].flags & LFLAG_REALTIMEMODE) && r_shadow_realtime_world.ival)
					continue;
				model->funcs.MarkLights (&cl_dlights[k], (dlightbitmask_t)1<<k, model->rootnode);
			}
		}

		Surf_LightmapShift(model);
#ifdef HEXEN2
		if ((ent->drawflags & MLS_MASK) == MLS_ABSLIGHT)
		{
			//update lightmaps.
			for (s = model->surfaces+model->firstmodelsurface,i = 0; i < model->nummodelsurfaces; i++, s++)
				Surf_RenderAmbientLightmaps (s, ent->abslight);
		}
		else if (ent->drawflags & DRF_TRANSLUCENT)
		{
			//update lightmaps.
			for (s = model->surfaces+model->firstmodelsurface,i = 0; i < model->nummodelsurfaces; i++, s++)
				Surf_RenderAmbientLightmaps (s, 255);
		}
		else
#endif
		{
			//update lightmaps.
			for (s = model->surfaces+model->firstmodelsurface,i = 0; i < model->nummodelsurfaces; i++, s++)
				Surf_RenderDynamicLightmaps (s);
		}
		currententity = NULL;
	}

#ifdef BEF_PUSHDEPTH
	if (r_pushdepth && model->submodelof == r_worldentity.model)
		bef = BEF_PUSHDEPTH;
	else
		bef = 0;
#else
	bef = 0;
#endif
	if (ent->flags & RF_ADDITIVE)
		bef |= BEF_FORCEADDITIVE;
#ifdef HEXEN2
	else if ((ent->drawflags & DRF_TRANSLUCENT) && r_wateralpha.value != 1)
	{
		bef |= BEF_FORCETRANSPARENT;
		ent->shaderRGBAf[3] = r_wateralpha.value;
	}
#endif
	else if ((ent->flags & RF_TRANSLUCENT) && cls.protocol != CP_QUAKE3)
		bef |= BEF_FORCETRANSPARENT;
	if (ent->flags & RF_NODEPTHTEST)
		bef |= BEF_FORCENODEPTH;
	if (ent->flags & RF_NOSHADOW)
		bef |= BEF_NOSHADOWS;
	//nettest: r_shadows_bmodels 0 - only MODELS cast shadows, not brush entities.
	//`submodelof` is set only for an inline "*N" submodel of the world, i.e. exactly
	//a func_door / func_wall / func_train / func_pushable and never the world itself
	//(the same test r_pushdepth uses a few lines above), so the world keeps casting
	//and only the func_ classes stop.  Reached by EVERY brush-model entity - CSQC-drawn
	//or engine-networked - and BEF_NOSHADOWS is already honoured by the depth/stencil
	//passes in GLBE_SubmitMeshesSortList, so this one condition covers the whole class.
	if (!r_shadows_bmodels.ival && model->submodelof == r_worldentity.model)
		bef |= BEF_NOSHADOWS;

	//nettest: DRAW A LIQUID BRUSH ENTITY FROM BOTH SIDES.
	//
	//A func_water submodel is a CLOSED box whose every face points OUTWARD - the
	//compiler emits one face per plane, unlike worldspawn water, which it emits on
	//both sides of each plane.  Shader_DefaultBSPWater never writes a `cull` line,
	//so the shader inherits SHADER_CULL_FRONT (gl_shader.c:7798), and a brush
	//ENTITY gets no per-surface plane test at all - the loop below copies
	//model->batches wholesale.  The GPU is therefore the only culler, and from
	//inside the volume every face of the box is backfacing.  Result: swim into a
	//func_water and the surface above your head simply is not drawn.
	//
	//Gated on a NEGATIVE skinnum, which is the engine's own existing marker for
	//"this brush model is a contents volume" - cl_ents.c uses exactly that test to
	//turn a networked brush entity into a physent with a forced contents mask, and
	//CSQC passes .skin through to skinnum (pr_csqc.c:920).  So this reaches
	//func_water / func_slime / func_lava and nothing else: not the world model, not
	//func_door, not func_wall, and it leaves Shader_DefaultBSPWater alone so the
	//WORLD's doubled water faces keep relying on GL cull to avoid drawing twice.
	//
	//(Fixing this in the shader template instead - the obvious `cull none` - would
	//be wrong on any map big enough to auto-enable the temporal scene cache, where
	//Surf_SimpleWorld_Q1BSP walks marksurfaces with no backface test and the
	//world's two coincident water quads would both rasterize.)
	if (ent->skinnum < 0)
	{
		bef |= BEF_FORCETWOSIDED;

		//nettest: AND THE WATER SHADER'S OWN ALPHA IS THE WHOLE ANSWER.
		//
		//Shader_DefaultBSPWater bakes r_wateralpha into the liquid shader
		//(gl_shader.c: "alphagen const %g" plus defaultwarp#ALPHA=%g), and
		//defaultwarp.glsl then multiplies that by e_colourident - which is the
		//ENTITY's alpha, verbatim, via SP_E_COLOURSIDENT in gl_backend.c.  For
		//worldspawn water there is no entity and the factor is 1.  For a
		//func_water the mod translates the GoldSrc `renderamt` key into entity
		//alpha, so the pool renders at r_wateralpha * renderamt/255 while the
		//worldspawn water beside it renders at r_wateralpha.
		//
		//That is the "func_water is about half the transparency of the
		//worldspawn water, I need r_wateralpha 2 to make it look solid" report,
		//and the factor of two is literal: renderamt 128 is the single most
		//common value.  Across the Sven Co-op map set func_water carries
		//renderamt 65, 70, 75, 85, 100, 128, 130, 150, 175, 200, 210 and 255,
		//so how transparent a pool looked was a property of which map it was in.
		//
		//Neither engine multiplies these: GoldSrc has no r_wateralpha and uses
		//renderamt alone, FTE's world water uses r_wateralpha alone.  Doing both
		//is the bug.  r_hlwater_entalpha 1 restores the old compounding.
		if (!r_hlwater_entalpha.ival)
		{
			ent->shaderRGBAf[3] = 1;
			bef &= ~BEF_FORCETRANSPARENT;
		}
	}

	for (i = 0; i < SHADER_SORT_COUNT; i++)
	for (ob = model->batches[i]; ob; ob = ob->next)
	{
		b = BE_GetTempBatch();
		if (!b)
			continue;
		*b = *ob;
		if (b->vbo && b->maxmeshes)
		{
			b->user.meshbuf = *b->mesh[0];
			b->user.meshbuf.numindexes = b->mesh[b->maxmeshes-1]->indexes+b->mesh[b->maxmeshes-1]->numindexes-b->mesh[0]->indexes;
			b->user.meshbuf.numvertexes = b->mesh[b->maxmeshes-1]->xyz_array+b->mesh[b->maxmeshes-1]->numvertexes-b->mesh[0]->xyz_array;

			b->mesh = &b->user.meshptr;
			b->user.meshptr = &b->user.meshbuf;
			b->meshes = b->maxmeshes = 1;
		}
		else
		{
//		if (b->texture)
//			b->shader = R_TextureAnimation(ent->framestate.g[FS_REG].frame[0], b->texture)->shader;
			b->meshes = b->maxmeshes;
		}
		b->ent = ent;
		b->flags = bef;

		if (b->buildmeshes)
			b->buildmeshes(b);

		if (!b->shader)
			b->shader = R_TextureAnimation(ent->framestate.g[FS_REG].frame[0], b->texture)->shader;

		if (bef & BEF_FORCEADDITIVE && b->shader->sort==SHADER_SORT_OPAQUE)
		{
			b->next = batches[SHADER_SORT_ADDITIVE];
			batches[SHADER_SORT_ADDITIVE] = b;
		}
		else if (bef & BEF_FORCETRANSPARENT && b->shader->sort==SHADER_SORT_OPAQUE)
		{
			b->next = batches[SHADER_SORT_BLEND];
			batches[SHADER_SORT_BLEND] = b;
		}
		else
		{
			b->next = batches[b->shader->sort];
			batches[b->shader->sort] = b;
		}
	}
}

#ifdef THREADEDWORLD
struct webostate_s
{
	char dbgid[12];
	struct webostate_s *next;
	int lastvalid;	//keyed to cls.framecount, for cleaning up.
	model_t *wmodel;
	int framecount;
	int cluster[2];
	qboolean generating;
	pvsbuffer_t pvs;
	vboarray_t ebo;
	vboarray_t vbo;
	void *ebomem;
	size_t idxcount;
	int numbatches;
	qbyte areamask[MAX_Q2MAP_AREAS/8];
	int lightstylevalues[MAX_NET_LIGHTSTYLES];	//when using workers that only reprocessing lighting at 10fps, things get too ugly when things go out of sync

//TODO	qbyte *bakedsubmodels;	//flags saying whether each submodel was baked or not. baked submodels need to be untinted uncaled unrotated at origin etc

	vec3_t lastpos;	//for better stale ebo selection when we're generating a new position.

	batch_t *rbatches[SHADER_SORT_COUNT];

	struct wesbatch_s
	{
		qboolean inefficient;	//this batch's shader needs special care with vertex data too
		size_t numidx;
		size_t maxidx;
		size_t firstidx;	//offset into the final ebo
		index_t *idxbuffer;
		batch_t b;
		mesh_t m;
		mesh_t *pm;
		vbo_t vbo;

		size_t maxverts;
	} batches[1];
};
static struct webostate_s *webostates;
static struct webostate_s *webogenerating;
static int webogeneratingstate;	//1 if generating, 0 if not, for waiting for sync.
static void R_DestroyWorldEBO(struct webostate_s *es)
{
	int i;
	if (!es)
		return;

	for (i = 0; i < es->numbatches; i++)
	{
		if (es->batches[i].inefficient)
		{
			BZ_Free(es->batches[i].m.xyz_array);
			BZ_Free(es->batches[i].m.st_array);
			BZ_Free(es->batches[i].m.lmst_array[0]);
			BZ_Free(es->batches[i].m.normals_array);
			BZ_Free(es->batches[i].m.snormals_array);
			BZ_Free(es->batches[i].m.tnormals_array);
		}
		BZ_Free(es->batches[i].idxbuffer);
	}

#ifdef GLQUAKE
	if (qrenderer == QR_OPENGL)
	{
		if (es->ebo.gl.vbo)
			qglDeleteBuffersARB(1, &es->ebo.gl.vbo);
		if (es->vbo.gl.vbo)
			qglDeleteBuffersARB(1, &es->vbo.gl.vbo);
	}
#endif
#ifdef VKQUAKE
	if (qrenderer == QR_VULKAN)
		BE_VBO_Destroy(&es->ebo, es->ebomem);
#endif
	BZ_Free(es);
}
void R_GeneratedWorldEBO(void *ctx, void *data, size_t a_, size_t b_)
{
	double starttime = Sys_DoubleTime();
	size_t idxcount, vertcount;
	unsigned int i;
	model_t *mod;
	batch_t *b, *batch;
	mesh_t *m;
	int sortid;
	struct webostate_s *webostate = ctx;
	webostate->next = webostates;
	webostates = webostate;
	webogenerating = NULL;
	webogeneratingstate = 0;
	webo_blocklightmapupdates = 1;
	mod = webostate->wmodel;

	webostate->lastvalid = cls.framecount;

	for (i = 0, idxcount = 0, vertcount = 0; i < webostate->numbatches; i++)
	{
		idxcount += webostate->batches[i].numidx;
		vertcount += webostate->batches[i].m.numvertexes;
	}
#ifdef GLQUAKE
	if (qrenderer == QR_OPENGL)
	{
		GL_DeselectVAO();

		if (vertcount)
		{
			size_t vc;
			vbo_t *vbo;
			size_t v_coord	= 0;
			size_t v_tc		= v_coord	+ sizeof(vecV_t)*vertcount;
			size_t v_lmtc	= v_tc		+ sizeof(vec2_t)*vertcount;
			size_t v_norm	= v_lmtc	+ sizeof(vec2_t)*vertcount;
			size_t v_snorm	= v_norm	+ sizeof(vec3_t)*vertcount;
			size_t v_tnorm	= v_snorm	+ sizeof(vec3_t)*vertcount;
			size_t v_colour	= v_tnorm	+ sizeof(vec3_t)*vertcount;
			size_t vbosize	= v_colour	+ sizeof(vec4_t)*vertcount;

			if (!webostate->vbo.gl.vbo)
				qglGenBuffersARB(1, &webostate->vbo.gl.vbo);
			GL_SelectVBO(webostate->vbo.gl.vbo);
			qglBufferDataARB(GL_ARRAY_BUFFER_ARB, vbosize, NULL, GL_STATIC_DRAW_ARB);
			for (i = 0, vertcount = 0; i < webostate->numbatches; i++)
			{
				if (webostate->batches[i].inefficient)
				{
					vc = webostate->batches[i].m.numvertexes;

					vbo = &webostate->batches[i].vbo;
					vbo->coord.gl.vbo		= webostate->vbo.gl.vbo;	vbo->coord.gl.addr		= (char*)v_coord	+ sizeof(vecV_t)*vertcount;
					vbo->texcoord.gl.vbo	= webostate->vbo.gl.vbo;	vbo->texcoord.gl.addr	= (char*)v_tc		+ sizeof(vec2_t)*vertcount;
					vbo->lmcoord[0].gl.vbo	= webostate->vbo.gl.vbo;	vbo->lmcoord[0].gl.addr = (char*)v_lmtc		+ sizeof(vec2_t)*vertcount;
					vbo->normals.gl.vbo		= webostate->vbo.gl.vbo;	vbo->normals.gl.addr	= (char*)v_norm		+ sizeof(vec3_t)*vertcount;
					vbo->svector.gl.vbo		= webostate->vbo.gl.vbo;	vbo->svector.gl.addr	= (char*)v_snorm	+ sizeof(vec3_t)*vertcount;
					vbo->tvector.gl.vbo		= webostate->vbo.gl.vbo;	vbo->tvector.gl.addr	= (char*)v_tnorm	+ sizeof(vec3_t)*vertcount;
					vbo->colours[0].gl.vbo	= webostate->vbo.gl.vbo;	vbo->colours[0].gl.addr	= (char*)v_colour	+ sizeof(vec4_t)*vertcount;

					qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB,(qintptr_t)vbo->coord.gl.addr,		vc*sizeof(vecV_t), webostate->batches[i].m.xyz_array);
					qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB,(qintptr_t)vbo->texcoord.gl.addr,	vc*sizeof(vec2_t), webostate->batches[i].m.st_array);
					qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB,(qintptr_t)vbo->lmcoord[0].gl.addr,	vc*sizeof(vec2_t), webostate->batches[i].m.lmst_array[0]);
					qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB,(qintptr_t)vbo->normals.gl.addr,	vc*sizeof(vec3_t), webostate->batches[i].m.normals_array);
					qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB,(qintptr_t)vbo->svector.gl.addr,	vc*sizeof(vec3_t), webostate->batches[i].m.snormals_array);
					qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB,(qintptr_t)vbo->tvector.gl.addr,	vc*sizeof(vec3_t), webostate->batches[i].m.tnormals_array);
					qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB,(qintptr_t)vbo->colours[0].gl.addr,	vc*sizeof(vec4_t), webostate->batches[i].m.colors4f_array[0]);
					webostate->batches[i].m.vbofirstvert = 0;
					vertcount += vc;
				}
			}
		}

		webostate->ebo.gl.addr = NULL;
		if (!webostate->ebo.gl.vbo)
			qglGenBuffersARB(1, &webostate->ebo.gl.vbo);
		GL_SelectEBO(webostate->ebo.gl.vbo);
		qglBufferDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, idxcount*sizeof(index_t), NULL, GL_STATIC_DRAW_ARB);
		for (i = 0, idxcount = 0; i < webostate->numbatches; i++)
		{
			qglBufferSubDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, idxcount*sizeof(index_t), webostate->batches[i].numidx*sizeof(index_t), webostate->batches[i].idxbuffer);
//			BZ_Free(webostate->batches[i].idxbuffer);
//			webostate->batches[i].idxbuffer = NULL;
			webostate->batches[i].firstidx = idxcount;
			idxcount += webostate->batches[i].numidx;
		}
	}
#endif
#ifdef VKQUAKE
	if (qrenderer == QR_VULKAN)
	{	//this malloc is stupid.
		//with vulkan we really should be doing this on the worker instead, at least the staging part.
		index_t *indexes = malloc(sizeof(*indexes) * idxcount);
		BE_VBO_Destroy(&webostate->ebo, webostate->ebomem);
		memset(&webostate->ebo, 0, sizeof(webostate->ebo));
		webostate->ebomem = NULL;
		webostate->ebo.vk.offs = 0;
		for (i = 0, idxcount = 0; i < webostate->numbatches; i++)
		{
			memcpy(indexes + idxcount, webostate->batches[i].idxbuffer, webostate->batches[i].numidx*sizeof(index_t));
//			BZ_Free(webostate->batches[i].idxbuffer);
//			webostate->batches[i].idxbuffer = NULL;
			webostate->batches[i].firstidx = idxcount;
			idxcount += webostate->batches[i].numidx;
		}
		if (idxcount)
			BE_VBO_Finish(NULL, indexes, sizeof(*indexes) * idxcount, &webostate->ebo, NULL, &webostate->ebomem);
		else
		{
			memset(&webostate->ebo, 0, sizeof(webostate->ebo));
			webostate->ebomem = NULL;
		}
		free(indexes);

		vertcount = 0; //unsupported for now.
	}
#endif

	//should be doing this on the worker, but whatever
	for (i = 0, sortid = 0; sortid < SHADER_SORT_COUNT; sortid++)
	{
		webostate->rbatches[sortid] = NULL;
		for (batch = mod->batches[sortid]; batch != NULL; batch = batch->next, i++)
		{
			if (!webostate->batches[i].numidx)
				continue;

			if (batch->shader->flags & SHADER_NODRAW)
				continue;

			m = &webostate->batches[i].m;
			webostate->batches[i].pm = m;
			b = &webostate->batches[i].b;
			memcpy(b, batch, sizeof(*b));

			b->mesh = &webostate->batches[i].pm;
			b->meshes = 1;
			b->vbo = &webostate->batches[i].vbo;
			if (webostate->batches[i].inefficient)
			{	//we had to generate new buffers because there's something evil in the shader..
				m->indexes = webostate->batches[i].idxbuffer;
				b->vbo->vao = 0;
			}
			else
			{
				*b->vbo = *batch->vbo;
				if (b->shader->flags & SHADER_NEEDSARRAYS)
				{	//this ebo cache stuff tracks only indexes, we don't know the actual surfs any more.
					//if NEEDSARRAYS is flagged then the cpu will need access to the mesh data - which it doesn't have.
					//while we could figure out this info, there would be a lot of vertexes that are not referenced, which would be horrendously slow.
					if (b->shader->flags & SHADER_SKY)
						continue;
					b->shader = R_RegisterShader_Vertex(mod, "unsupported");
				}
				m->numvertexes = webostate->batches[i].b.vbo->vertcount;
			}
			b->vbo->indicies = webostate->ebo;
			b->vbo->vao = 0;
			m->numindexes = webostate->batches[i].numidx;
			m->vbofirstelement = webostate->batches[i].firstidx;


			b->next = webostate->rbatches[sortid];
			webostate->rbatches[sortid] = b;
		}
	}

	r_loaderstalltime += Sys_DoubleTime() - starttime;
}
#ifdef Q1BSPS
static void Surf_SimpleWorld_Q1BSP(struct webostate_s *es, qbyte *pvs)
{
	mleaf_t		*leaf;
	msurface_t	*surf, **mark, **end;
	mesh_t		*mesh;
	model_t *wmodel = es->wmodel;
	int l = wmodel->numclusters;
	int fc = es->framecount;
	int i;
	int s, f, lastface;
	struct wesbatch_s *eb;
	for (leaf = wmodel->leafs+l; l-- > 0; leaf--)
	{
		if ((pvs[l>>3] & (1u<<(l&7))) && leaf->nummarksurfaces)
		{
			mark = leaf->firstmarksurface;
			end = mark+leaf->nummarksurfaces;
			while(mark < end)
			{
				surf = *mark++;
				if (surf->visframe != fc)
				{
					surf->visframe = fc;
					Surf_RenderDynamicLightmaps_Worker (wmodel, surf, es->lightstylevalues);

					mesh = surf->mesh;
					eb = &es->batches[surf->sbatch->user.bmodel.ebobatch];
					if (eb->maxidx < eb->numidx + mesh->numindexes)
					{
						//FIXME: pre-allocate
						eb->maxidx = eb->numidx + mesh->numindexes + 512;
						eb->idxbuffer = BZ_Realloc(eb->idxbuffer, eb->maxidx * sizeof(index_t));
					}

					if (eb->inefficient)
					{	//slow path that needs to create new VBOs on the fly too.
						if (eb->maxverts < eb->m.numvertexes + mesh->numvertexes)
						{
							//FIXME: pre-allocate
							eb->maxverts = eb->m.numvertexes + mesh->numvertexes + 512;
							eb->m.xyz_array			= BZ_Realloc(eb->m.xyz_array,			eb->maxverts * sizeof(*eb->m.xyz_array));
							eb->m.st_array			= BZ_Realloc(eb->m.st_array,			eb->maxverts * sizeof(*eb->m.st_array));
							eb->m.lmst_array[0]		= BZ_Realloc(eb->m.lmst_array[0],		eb->maxverts * sizeof(*eb->m.lmst_array[0]));
							eb->m.normals_array		= BZ_Realloc(eb->m.normals_array,		eb->maxverts * sizeof(*eb->m.normals_array));
							eb->m.snormals_array	= BZ_Realloc(eb->m.snormals_array,		eb->maxverts * sizeof(*eb->m.snormals_array));
							eb->m.tnormals_array	= BZ_Realloc(eb->m.tnormals_array,		eb->maxverts * sizeof(*eb->m.tnormals_array));
							eb->m.colors4f_array[0]	= BZ_Realloc(eb->m.colors4f_array[0],	eb->maxverts * sizeof(*eb->m.colors4f_array[0]));
						}

						memcpy(eb->m.xyz_array+eb->m.numvertexes,		mesh->xyz_array,		sizeof(*eb->m.xyz_array)*mesh->numvertexes);
						memcpy(eb->m.st_array+eb->m.numvertexes,		mesh->st_array,			sizeof(*eb->m.st_array)*mesh->numvertexes);
						memcpy(eb->m.lmst_array[0]+eb->m.numvertexes,	mesh->lmst_array[0],	sizeof(*eb->m.lmst_array[0])*mesh->numvertexes);
						memcpy(eb->m.normals_array+eb->m.numvertexes,	mesh->normals_array,	sizeof(*eb->m.normals_array)*mesh->numvertexes);
						memcpy(eb->m.snormals_array+eb->m.numvertexes,	mesh->snormals_array,	sizeof(*eb->m.snormals_array)*mesh->numvertexes);
						memcpy(eb->m.tnormals_array+eb->m.numvertexes,	mesh->tnormals_array,	sizeof(*eb->m.tnormals_array)*mesh->numvertexes);
						memcpy(eb->m.colors4f_array[0]+eb->m.numvertexes,mesh->colors4f_array[0],sizeof(*eb->m.colors4f_array[0])*mesh->numvertexes);

						for (i = 0; i < mesh->numindexes; i++)
							eb->idxbuffer[eb->numidx+i] = mesh->indexes[i] + eb->m.numvertexes;
						eb->m.numvertexes+=mesh->numvertexes;
					}
					else
					{
						for (i = 0; i < mesh->numindexes; i++)
							eb->idxbuffer[eb->numidx+i] = mesh->indexes[i] + mesh->vbofirstvert;
					}
					eb->numidx += mesh->numindexes;
				}
			}
		}
	}

	for (s = 1; s < wmodel->numsubmodels; s++)
	{
//		if (!es->bakedsubmodels[s])
//			continue;	//not baking this one (not currently visible or something)
		//FIXME: pvscull it here?
		lastface = wmodel->submodels[s].firstface + wmodel->submodels[s].numfaces;
		for (f = wmodel->submodels[s].firstface; f < lastface; f++)
		{
			surf = wmodel->surfaces+f;

			Surf_RenderDynamicLightmaps_Worker (wmodel, surf, es->lightstylevalues);
/*
			mesh = surf->mesh;
			eb = &es->batches[surf->sbatch->webobatch];
			if (eb->maxidx < eb->numidx + mesh->numindexes)
			{
				//FIXME: pre-allocate
				eb->maxidx = eb->numidx + surf->mesh->numindexes + 512;
				eb->idxbuffer = BZ_Realloc(eb->idxbuffer, eb->maxidx * sizeof(index_t));
			}
			for (i = 0; i < mesh->numindexes; i++)
				eb->idxbuffer[eb->numidx+i] = mesh->indexes[i] + mesh->vbofirstvert;
			eb->numidx += mesh->numindexes;*/
		}
	}
}
#endif
#if defined(Q2BSPS) || defined(Q3BSPS)
static void Surf_SimpleWorld_Q3BSP(struct webostate_s *es, qbyte *pvs)
{
	mleaf_t		*leaf;
	msurface_t	*surf, **mark, **end;
	mesh_t		*mesh;
	model_t *wmodel = es->wmodel;
	int l = wmodel->numleafs;	//is this doing submodels too?
	int c;
	int fc = es->framecount;
	for (leaf = wmodel->leafs; l --> 0; leaf++)
	{
		c = leaf->cluster;
		if (c < 0 || !leaf->parent)
			continue;	//o.O
		if ((pvs[c>>3] & (1u<<(c&7))) && leaf->nummarksurfaces && (((unsigned)leaf->area>=MAX_Q2MAP_AREAS)||es->areamask[leaf->area>>3]&1<<(leaf->area&7)))
		{
			mark = leaf->firstmarksurface;
			end = mark+leaf->nummarksurfaces;
			while(mark < end)
			{
				surf = *mark++;
				if (surf->visframe != fc)
				{
					int i;
					struct wesbatch_s *eb;
					surf->visframe = fc;

					mesh = surf->mesh;
					eb = &es->batches[surf->sbatch->user.bmodel.ebobatch];
					if (eb->maxidx < eb->numidx + mesh->numindexes)
					{
						//FIXME: pre-allocate
						eb->maxidx = eb->numidx + mesh->numindexes + 512;
						eb->idxbuffer = BZ_Realloc(eb->idxbuffer, eb->maxidx * sizeof(index_t));
					}
					if (eb->inefficient)
					{	//slow path that needs to create a single ram-backed mesh

						//FIXME: for portal/refract surfaces, track surfaces for refract pvs info
						if (eb->maxverts < eb->m.numvertexes + mesh->numvertexes)
						{
							//FIXME: pre-allocate
							eb->maxverts = eb->m.numvertexes + mesh->numvertexes + 512;
							eb->m.xyz_array		= BZ_Realloc(eb->m.xyz_array,		eb->maxverts * sizeof(*eb->m.xyz_array));
							eb->m.st_array		= BZ_Realloc(eb->m.st_array,		eb->maxverts * sizeof(*eb->m.st_array));
							eb->m.lmst_array[0]	= BZ_Realloc(eb->m.lmst_array[0],	eb->maxverts * sizeof(*eb->m.lmst_array[0]));
							eb->m.normals_array	= BZ_Realloc(eb->m.normals_array,	eb->maxverts * sizeof(*eb->m.normals_array));
							eb->m.snormals_array= BZ_Realloc(eb->m.snormals_array,	eb->maxverts * sizeof(*eb->m.snormals_array));
							eb->m.tnormals_array= BZ_Realloc(eb->m.tnormals_array,	eb->maxverts * sizeof(*eb->m.tnormals_array));
							eb->m.colors4f_array[0]= BZ_Realloc(eb->m.colors4f_array[0],eb->maxverts * sizeof(*eb->m.colors4f_array[0]));
						}
						memcpy(eb->m.numvertexes+eb->m.xyz_array,		mesh->xyz_array,		sizeof(*eb->m.xyz_array)*mesh->numvertexes);
						memcpy(eb->m.numvertexes+eb->m.st_array,		mesh->st_array,			sizeof(*eb->m.st_array)*mesh->numvertexes);
						memcpy(eb->m.numvertexes+eb->m.lmst_array[0],	mesh->lmst_array[0],	sizeof(*eb->m.lmst_array[0])*mesh->numvertexes);
						memcpy(eb->m.numvertexes+eb->m.normals_array,	mesh->normals_array,	sizeof(*eb->m.normals_array)*mesh->numvertexes);
						memcpy(eb->m.numvertexes+eb->m.snormals_array,	mesh->snormals_array,	sizeof(*eb->m.snormals_array)*mesh->numvertexes);
						memcpy(eb->m.numvertexes+eb->m.tnormals_array,	mesh->tnormals_array,	sizeof(*eb->m.tnormals_array)*mesh->numvertexes);
						memcpy(eb->m.numvertexes+eb->m.colors4f_array[0],mesh->colors4f_array[0],sizeof(*eb->m.colors4f_array[0])*mesh->numvertexes);

						for (i = 0; i < mesh->numindexes; i++)
							eb->idxbuffer[eb->numidx+i] = mesh->indexes[i] + eb->m.numvertexes;
						eb->m.numvertexes+=mesh->numvertexes;
					}
					else
					{	//using the general prebaked entire-batch vbos
						for (i = 0; i < mesh->numindexes; i++)
							eb->idxbuffer[eb->numidx+i] = mesh->indexes[i] + mesh->vbofirstvert;
					}
					eb->numidx += mesh->numindexes;
				}
			}
		}
	}
}
#endif
void R_GenWorldEBO(void *ctx, void *data, size_t a, size_t b)
{
	int i;
	struct webostate_s *es = ctx;
	qbyte *pvs;

	int sortid;
	batch_t *batch;
	qboolean inefficient;

	if (!es->numbatches)
	{
		es->numbatches = es->wmodel->numbatches;

		for (i = 0; i < es->numbatches; i++)
		{
			es->batches[i].firstidx = 0;
			es->batches[i].numidx = 0;
			es->batches[i].maxidx = 0;
			es->batches[i].idxbuffer = NULL;
			es->batches[i].inefficient = false;

			es->batches[i].maxverts = 0;
			memset(&es->batches[i].m, 0, sizeof(es->batches[i].m));
			memset(&es->batches[i].vbo, 0, sizeof(es->batches[i].vbo));
		}
	}
	else
	{
		for (i = 0; i < es->numbatches; i++)
		{
			es->batches[i].firstidx = 0;
			es->batches[i].numidx = 0;
			es->batches[i].m.numvertexes = 0;
		}
	}

	//set to 2 to reveal the inefficient surfaces...
	for (sortid = 0; sortid < SHADER_SORT_COUNT; sortid++)
		for (batch = es->wmodel->batches[sortid]; batch != NULL; batch = batch->next)
		{
			inefficient = false;
			if (r_temporalscenecache.ival < 2)
			{
#if MAXRLIGHTMAPS > 1
				if (batch->lmlightstyle[1] != INVALID_LIGHTSTYLE || batch->vtlightstyle[1] != INVALID_VLIGHTSTYLE)
					continue;	//not supported here, show fallback shader instead (would work but with screwed lighting, we prefer a better-defined result).
#endif
				if (!batch->shader)
					inefficient = true;
				else if (batch->shader->flags & SHADER_NEEDSARRAYS)
					inefficient = true;
			}
			if (es->batches[batch->user.bmodel.ebobatch].inefficient != inefficient)
			{
				es->batches[batch->user.bmodel.ebobatch].inefficient = inefficient;
				if (!inefficient)
				{
					if (es->batches[i].inefficient)
					{
						BZ_Free(es->batches[i].m.xyz_array);
						BZ_Free(es->batches[i].m.st_array);
						BZ_Free(es->batches[i].m.lmst_array[0]);
						BZ_Free(es->batches[i].m.normals_array);
						BZ_Free(es->batches[i].m.snormals_array);
						BZ_Free(es->batches[i].m.tnormals_array);
					}
					BZ_Free(es->batches[i].idxbuffer);

					memset(&es->batches[i], 0, sizeof(es->batches[i]));
				}
			}
		}

	//maybe we should just use fatpvs instead, and wait for completion when outside?
	if (r_novis.ival)
	{
		if (es->pvs.buffersize < es->wmodel->pvsbytes)
			es->pvs.buffer = BZ_Realloc(es->pvs.buffer, es->pvs.buffersize=es->wmodel->pvsbytes);
		memset(es->pvs.buffer, 0xff, es->pvs.buffersize);
		pvs = es->pvs.buffer;
	}
	else if (es->cluster[1] != -1 && es->cluster[0] != es->cluster[1])
	{	//view is near to a water boundary. this implies the water crosses the near clip plane. we need both leafs.
		pvs = es->wmodel->funcs.ClusterPVS(es->wmodel, es->cluster[0], &es->pvs, PVM_REPLACE);
		pvs = es->wmodel->funcs.ClusterPVS(es->wmodel, es->cluster[1], &es->pvs, PVM_MERGE);
	}
	else
		pvs = es->wmodel->funcs.ClusterPVS(es->wmodel, es->cluster[0], &es->pvs, PVM_REPLACE);

#if defined(Q2BSPS) || defined(Q3BSPS)
	if (es->wmodel->fromgame == fg_quake2 || es->wmodel->fromgame == fg_quake3)
		Surf_SimpleWorld_Q3BSP(es, pvs);
	else
#endif
#ifdef Q1BSPS
	if (es->wmodel->fromgame == fg_quake || es->wmodel->fromgame == fg_halflife)
		Surf_SimpleWorld_Q1BSP(es, pvs);
	else
#endif
	{
		//panic
	}

	COM_AddWork(WG_MAIN, R_GeneratedWorldEBO, es, NULL, 0, 0);
}
cvar_t r_temporalscenecache					= CVARAFD ("r_temporalscenecache", "", "r_scenecache", CVAR_ARCHIVE, "Controls whether to generate+reuse a scene cache over multiple frames. This is generated on a separate thread to avoid any associated costs. This can significantly boost framerates on complex maps, but can also stress the gpu more (performance tradeoff that varies per map). An outdated cache may be used if the cache takes too long to build (eg: lightmap animations), which could cause the odd glitch when moving fast (but retain more consistent framerates - another tradeoff).\n0: Tranditional quake rendering.\n1: Generate+Use the scene cache.");
#else
cvar_t r_temporalscenecache					= CVARAFD ("r_temporalscenecache", "", "r_scenecache", CVAR_NOSET, "Controls whether to generate+reuse a scene cache over multiple frames. This is generated on a separate thread to avoid any associated costs. This can significantly boost framerates on complex maps, but can also stress the gpu more (performance tradeoff that varies per map). An outdated cache may be used if the cache takes too long to build (eg: lightmap animations), which could cause the odd glitch when moving fast (but retain more consistent framerates - another tradeoff).\n0: Tranditional quake rendering.\n1: Generate+Use the scene cache.");
#endif

//nettest: what the scene-cache decision in Surf_DrawWorld actually came to on
//the last frame, for `r_waterinfo` below.  Latched rather than recomputed
//because the decision depends on state (loadstate, Media_Capturing, the
//per-style cvars) that is only meaningful mid-frame.
static int nettest_sc_wanted = -1;	//what the heuristic/cvar asked for
static int nettest_sc_forced = -1;	//...and whether the wateralpha override overrode it

/*
=============
R_WaterInfo_f

nettest: ONE COMMAND THAT SAYS WHY THE WATER LOOKS WRONG.

Transparent GoldSrc water has an unreasonable number of independent off
switches, none of which announces itself, and every previous round of this has
been spent guessing which one was closed:

  - r_wateralpha left at 1 (its default) by a config or a map cfg, which alone
    makes both the blend AND r_wateralpha_extendpvs no-ops.
  - cls.allow_watervis, a SERVER permission (the `watervis` serverinfo key). If
    the server says no, Shader_DefaultBSPWater forces alpha to 1 whatever the
    client asked for - gl_shader.c:7110-7113.
  - the temporal scene cache, which returns before Q1BSP_MarkLeaves and so
    silently disables extendpvs entirely; it auto-enables above 6000 leafs, i.e.
    on exactly the maps big enough to want translucent water.
  - the fluid merge only ever walks WORLDSPAWN leafs. Water built as a func_
    brush entity has no leafs in the world's cluster range at all, so no amount
    of extendpvs can do anything for it. On th_ep1_01 that is most of the water.
  - r_hlwater_hidesides, which suppresses the underside/sides of HL water.
=============
*/
void R_WaterInfo_f(void)
{
	extern cvar_t r_hlwater_hidesides, r_waterripple, r_waterstyle;
	extern float q1bsp_marktime;
	extern int q1bsp_wantmerge, q1bsp_fluidtotal, q1bsp_fluidmerged;
	extern int q1bsp_fluidshoreents, q1bsp_fluidnovis, q1bsp_fluidbodies;
	model_t *m = cl.worldmodel;
	int i, worldliquid = 0, entliquid = 0, hidden = 0, kept = 0;
	float alpha;

	//Say WHICH of the two it is.  "no world loaded" covers both "you are at the
	//menu" and "the map is still loading", and on the headless client - where
	//this is driven from a deferred console command with no way to watch the
	//load - those need telling apart or the run silently measures nothing.
	if (!m)
	{
		Con_Printf("r_waterinfo: no world model yet (not connected, or still receiving the map)\n");
		return;
	}
	if (m->loadstate != MLS_LOADED)
	{
		Con_Printf("r_waterinfo: world \"%s\" is still loading (loadstate %i) - try again in a moment\n",
				m->name, m->loadstate);
		return;
	}

	Con_Printf("^2world^7: %s (%s), %i leafs, %i clusters\n", m->name,
			(m->fromgame==fg_halflife)?"halflife":((m->fromgame==fg_quake)?"quake":"other"),
			m->numleafs, m->numclusters);

	//the alpha the water shader will actually have resolved to.
	if (cls.allow_watervis)
		alpha = *r_wateralpha.string?r_wateralpha.value:1;
	else
		alpha = 1;
	Con_Printf("^2alpha^7: r_wateralpha %s -> effective %g   (server allow_watervis %s)\n",
			*r_wateralpha.string?r_wateralpha.string:"<empty>", alpha,
			cls.allow_watervis?"YES":"^1NO - water is forced OPAQUE^7");
	if (!cls.allow_watervis)
		Con_Printf("        ^3the server has not set the `watervis` serverinfo key; nothing client-side can override this\n");
	else if (alpha >= 1)
		Con_Printf("        ^3alpha is 1, so water is opaque and extendpvs is a no-op whatever it is set to\n");

	//liquid faces, split by owner - the distinction that decides whether the
	//PVS extension can possibly help.
	for (i = 0; i < m->numsurfaces; i++)
	{
		if (!(m->surfaces[i].flags & SURF_DRAWTURB))
			continue;
		if (i >= m->submodels[0].firstface && i < m->submodels[0].firstface+m->submodels[0].numfaces)
			worldliquid++;
		else
			entliquid++;
		if (m->surfaces[i].flags & SURF_NODRAW)
			hidden++;
		else
			kept++;
	}
	Con_Printf("^2liquid faces^7: %i worldspawn, %i brush-entity   (%i drawn, %i hidden by r_hlwater_hidesides %s)\n",
			worldliquid, entliquid, kept, hidden, r_hlwater_hidesides.string);
	if (entliquid > worldliquid)
		Con_Printf("        ^3most of this map's water is brush-ENTITY water; r_wateralpha_extendpvs only ever\n"
				   "        ^3merges WORLDSPAWN fluid leafs, so it cannot affect those pools at all\n");

	//nettest: WHAT SHADER DOES A HIDDEN FACE ACTUALLY GET?
	//"the sides and bottom of the func_water are solid bright orange" is a
	//SHADER question, not a texture question - bspguy is right that every face
	//carries the water texture.  Mod_Batches (gl_model.c) throws that texture
	//away for any SURF_NODRAW face and hands the surface this shader instead,
	//so if this one ever resolves with a pass it PAINTS the sides rather than
	//hiding them, in whatever the fallback texture happens to look like.
	{
		shader_t *nd = R_RegisterShader("nodraw", SUF_NONE, "{\nsurfaceparm nodraw\n}");
		Con_Printf("^2nodraw shader^7: \"%s\" passes=%i sort=%i flags=%#x -> %s\n",
				nd->name, nd->numpasses, nd->sort, nd->flags,
				(nd->flags & SHADER_NODRAW)
					?"^2not drawn^7"
					:"^1DRAWN - every hidden liquid side is being painted^7");
		//Only when it is broken: register the IDENTICAL body under a name nothing
		//can claim.  If the control comes out nodraw and "nodraw" does not, the
		//NAME has been taken - by a plugin material loader or a shader script -
		//rather than the body being at fault.  That is the exact shape of the two
		//hijacks this engine has already hit ("black", then "nodraw").
		if (!(nd->flags & SHADER_NODRAW))
		{
			shader_t *pr = R_RegisterShader("\1waterinfo_nodraw_control", SUF_NONE|SUR_FORCEFALLBACK, "{\nsurfaceparm nodraw\n}");
			Con_Printf("        genargs=%s\n", nd->genargs?nd->genargs:"^1NULL^7");
			Con_Printf("        control (same body, name nothing can claim): passes=%i flags=%#x -> %s\n",
					pr->numpasses, pr->flags,
					(pr->flags & SHADER_NODRAW)
						?"^3nodraw, so the BODY is fine and the NAME was taken^7"
						:"^3also drawn, so the body itself is not parsing^7");
		}
	}

	//nettest: COINCIDENT LIQUID SURFACES, per submodel.
	//A pool that renders as TWO wavy sheets is two faces on one plane, both
	//still drawn.  The compiler emits the top boundary in both facings and
	//gl_model.c deliberately keeps the down-facing twin so you can see the
	//surface from underneath - which is right for worldspawn, where GL backface
	//culling shows exactly one of the pair.  It is wrong for a liquid brush
	//ENTITY, because Surf_DrawBrushModel forces those two-sided (skinnum < 0),
	//so both copies rasterize; and with r_waterripple they deform along
	//OPPOSITE normals and visibly separate into two sheets.
	//Counted as "down-facing liquid faces that are still drawn AND share their
	//plane with an up-facing one that is also still drawn", i.e. the number of
	//SURFACES you see twice - not the number of (up,down) pairs, which on a
	//tessellated pool is the product of the two counts and reads as nonsense.
	{
		int sm, a, b, dbl, entdbl = 0;
		for (sm = 0; sm < m->numsubmodels; sm++)
		{
			int first = m->submodels[sm].firstface;
			int last  = first + m->submodels[sm].numfaces;
			dbl = 0;
			for (a = first; a < last; a++)
			{
				if (!(m->surfaces[a].flags & SURF_DRAWTURB) || (m->surfaces[a].flags & SURF_NODRAW))
					continue;
				if (((m->surfaces[a].flags & SURF_PLANEBACK)?-1:1) * m->surfaces[a].plane->normal[2] >= -0.5)
					continue;	//only the down-facing half is the redundant copy
				for (b = first; b < last; b++)
				{
					if (b == a || m->surfaces[b].plane != m->surfaces[a].plane)
						continue;
					if (!(m->surfaces[b].flags & SURF_DRAWTURB) || (m->surfaces[b].flags & SURF_NODRAW))
						continue;
					if (((m->surfaces[b].flags & SURF_PLANEBACK)?-1:1) * m->surfaces[b].plane->normal[2] > 0.5)
					{
						dbl++;
						break;
					}
				}
			}
			if (dbl && sm)
			{
				entdbl += dbl;
				Con_Printf("^2coincident^7: submodel *%i draws %i liquid surface(s) TWICE (one plane, both facings)\n", sm, dbl);
			}
			else if (dbl)
				Con_Printf("^2coincident^7: worldspawn has %i down-facing liquid face(s) over an up-facing twin"
						   "   ^2(expected - nothing draws the world two-sided, so GL culling picks one)^7\n", dbl);
		}
		if (entdbl)
			Con_Printf("        ^1a liquid brush ENTITY is drawn two-sided, so BOTH facings rasterize -"
					   " that is the doubled surface^7\n");
	}

	Con_Printf("^2scene cache^7: r_temporalscenecache \"%s\" -> wanted %i, in use %i%s\n",
			r_temporalscenecache.string, nettest_sc_wanted, r_temporalscenecache.ival,
			(nettest_sc_forced>0)?"   ^2(forced off by r_wateralpha_extendpvs)^7":"");

	Con_Printf("^2extendpvs^7: r_wateralpha_extendpvs %s\n", r_wateralpha_extendpvs.string);
	if (q1bsp_marktime < 0)
		Con_Printf("        ^1Q1BSP_MarkLeaves has NEVER run^7 - on a real renderer that means the scene\n"
				   "        cache is returning before it and the extension is dead whatever the cvars say.\n"
				   "        (On vid_renderer headless it means only that nothing draws the world.)\n");
	else if (realtime - q1bsp_marktime > 1)
		Con_Printf("        ^1MarkLeaves last ran %.1f seconds ago^7 - it is not running per-frame, so the\n"
				   "        scene cache is returning before it and the extension is inert\n",
				realtime - q1bsp_marktime);
	else
	{
		Con_Printf("        MarkLeaves ran %.2fs ago (i.e. per-frame), merge wanted: %s\n",
				realtime - q1bsp_marktime, q1bsp_wantmerge?"yes":"^3no^7");
		if (q1bsp_fluidtotal >= 0)
		{
			Con_Printf("        merging %i of %i worldspawn fluid leafs%s\n",
					q1bsp_fluidmerged, q1bsp_fluidtotal,
					(q1bsp_fluidtotal==0)?"   ^3(none: this map has no worldspawn water)^7":"");
			//The shore table is what makes the merge possible at all on a map whose
			//vis treated water as opaque - see Q1BSP_BuildFluidAdjacency.  Zero
			//entries with a non-zero fluid count means it could not be built and the
			//gate has fallen back to the direct PVS test, which on such a map is
			//always false.
			Con_Printf("        shore table: %i adjacency entries across %i connected water bodies%s\n",
					q1bsp_fluidshoreents, q1bsp_fluidbodies,
					(q1bsp_fluidtotal>0 && q1bsp_fluidshoreents<=0)?"   ^1(EMPTY - merge cannot fire)^7":"");
			if (q1bsp_fluidnovis > 0)
				Con_Printf("        %i visible pool(s) skipped for having no vis data of their own\n"
						   "        (merging one would set every bit and turn the frame into r_novis)\n",
						q1bsp_fluidnovis);
		}
	}

	//The amplifier for every "I can see things that should be culled" report on
	//a HL map: the sky writes no depth, so anything the PVS lets through is
	//drawn straight over it.  Widening the PVS and having a depth-less sky are
	//individually defensible and together are what a player reads as "faces
	//flickering in the distance".
	Con_Printf("^2sky^7: allow_unmaskedskyboxes %s%s\n",
			cls.allow_unmaskedskyboxes?"1":"0",
			cls.allow_unmaskedskyboxes?"   ^3(sky writes NO depth: anything the PVS admits draws through it)^7":"");
	Con_Printf("^2ripple^7: r_waterripple %s, r_waterstyle %s\n", r_waterripple.string, r_waterstyle.string);
}

/*
=============
R_DrawWorld
=============
*/

static pvsbuffer_t surf_frustumvis[R_MAX_RECURSE];
void Surf_DrawWorld (void)
{
	//surfvis vs entvis - the key difference is that surfvis is surfaces while entvis is volume. though surfvis should be frustum culled also for lighting. entvis doesn't care.
	qbyte *surfvis, *entvis;
	int areas[2];
	RSpeedLocals();

	if (r_refdef.flags & RDF_NOWORLDMODEL)
	{
		r_refdef.flags |= RDF_NOWORLDMODEL;
		r_refdef.scenevis = NULL;
		BE_DrawWorld(NULL);
		return;
	}
	if (!cl.worldmodel || cl.worldmodel->loadstate != MLS_LOADED)
	{
		/*Don't act as a wallhack*/
		return;
	}

	if (!r_refdef.areabitsknown && cl.worldmodel->funcs.WriteAreaBits)
	{	//generate the info each frame, as the gamecode didn't tell us what to use.
		cl.worldmodel->funcs.WriteAreaBits(cl.worldmodel, r_refdef.areabits, sizeof(r_refdef.areabits), r_viewarea, false);
		r_refdef.areabitsknown = true;
	}

	currentmodel = cl.worldmodel;
	currententity = &r_worldentity;

	r_dlightlightmaps = !!r_dynamic.ival;

	{
#ifdef THREADEDWORLD
		int sc = r_temporalscenecache.ival;
#endif
		RSpeedRemark();

		Surf_LightmapShift(currentmodel);

#ifdef THREADEDWORLD
		if (!*r_temporalscenecache.string && cl.worldmodel && cl.worldmodel->loadstate == MLS_LOADED && (cl.worldmodel->fromgame == fg_quake || cl.worldmodel->fromgame == fg_halflife))
		{	//when empty, pick a suitable default.
			//at what point is it a win? should we consider batch counts? probability of offscreen-only surfaces?
			if (cl.worldmodel->fromgame == fg_quake || cl.worldmodel->fromgame == fg_halflife)
				sc = ((r_novis.ival==1)||(cl.worldmodel->numleafs > 6000)) && r_waterstyle.ival<=1 && r_telestyle.ival<=1 && r_slimestyle.ival<=1 && r_lavastyle.ival<=1 && Media_Capturing()<2;
		}
		//nettest: THE WATERALPHA PVS EXTENSION AND THE SCENE CACHE ARE MUTUALLY EXCLUSIVE.
		//
		//r_wateralpha_extendpvs is implemented in Q1BSP_MarkLeaves (common/q1bsp.c), which is
		//only reachable via model->funcs.PrepareFrame further down this function - and the
		//scene-cache branch RETURNS before it, handing BE_DrawWorld a PVS that R_GenWorldEBO
		//built straight out of ClusterPVS with no fluid merge at all.  So wherever the cache is
		//active the cvar does literally nothing, AND the leafs behind the water are missing
		//while the water is still drawn `sort underwater` with a blendfunc - so at
		//r_wateralpha 0.5 you blend water over an unpainted framebuffer and see straight out of
		//the level.  Both halves of the reported bug, from one cause.
		//
		//The auto-default above is what decides it, and it is leaf-count driven: th_ep1_00 has
		//2452 leafs so the feature works there, th_ep1_01 has 7328 and trips the >6000 rule, so
		//it is dead on exactly the maps big enough to want translucent water.  q1bsp.c:2079
		//already documents this trap and says it has to be fixed HERE.
		//
		//Forced AFTER the auto-default so it also overrides an explicit
		//"r_temporalscenecache 1"; .value is re-read first because .ival is the field this code
		//clobbers, so the override lifts cleanly when extendpvs is turned back off.
		//COST: the cache is a real FPS win on big maps.  This trades it back for correct water,
		//and only for someone who has actually asked for both translucent water AND the PVS
		//extension - set r_wateralpha_extendpvs 0 to get the cache back.
		if (*r_temporalscenecache.string)
			sc = (int)r_temporalscenecache.value;
		nettest_sc_wanted = sc;
		if (r_wateralpha_extendpvs.ival && r_wateralpha.value < 1.0f && cl.worldmodel &&
			(cl.worldmodel->fromgame == fg_quake || cl.worldmodel->fromgame == fg_halflife))
			sc = 0;
		nettest_sc_forced = (sc != nettest_sc_wanted);

		if (sc != r_temporalscenecache.ival)
		{
			r_temporalscenecache.ival = sc;
			r_temporalscenecache.modified = true;
		}

		if (r_temporalscenecache.modified || r_dynamic.modified)
		{
			r_dynamic.modified = false;
			r_temporalscenecache.modified = false;
#ifdef RTLIGHTS
//			Sh_CheckSettings(); //fiddle with r_dynamic vs r_shadow_realtime_dlight.
#endif
			COM_WorkerPartialSync(webogenerating, &webogeneratingstate, true);
			while (webostates)
			{
				void *webostate = webostates;
				webostates = webostates->next;
				R_DestroyWorldEBO(webostate);
			}
			webo_blocklightmapupdates = false;
		}

		if (!r_temporalscenecache.ival)
			;
		else if (!r_refdef.recurse && currentmodel->type == mod_brush)
		{
			struct webostate_s *webostate, *best = NULL, *kill, **link;
			vec_t bestdist = FLT_MAX;
			for (webostate = webostates; webostate; webostate = webostate->next)
			{
				if (webostate->wmodel != currentmodel)
					continue;

//				kill = webostate->next;
//				if (kill && kill->lastvalid < cls.framecount-5)
//				{
//					webostate->next = kill->next;
//					R_DestroyWorldEBO(kill);
//				}

				if (webostate->cluster[0] == r_viewcluster && webostate->cluster[1] == r_viewcluster2)
				{
					VectorCopy(r_refdef.vieworg, webostate->lastpos);
					if (!r_refdef.areabitsknown || !memcmp(webostate->areamask, r_refdef.areabits, MAX_MAP_AREA_BYTES))
					{
						best = webostate;
						bestdist = 0;
						break;
					}
					else if (bestdist)
					{
						best = webostate;
						bestdist = 0;
					}
				}
				else
				{
					vec3_t m;
					float d;
					VectorSubtract(webostate->lastpos, r_refdef.vieworg, m);
					d = DotProduct(m,m);
					if (bestdist > d)
					{
						bestdist = d;
						best = webostate;
					}
				}
			}
			webostate = best;

			if (qrenderer != QR_OPENGL && qrenderer != QR_VULKAN)
				;
#ifdef Q1BSPS
			else if (currentmodel->fromgame == fg_quake || currentmodel->fromgame == fg_halflife || currentmodel->fromgame == fg_quake3)
			{
				if (!webogenerating)
				{
					qboolean gennew = false;
					if (!webostate)
						gennew = true;	//generate an initial one, if we can.
					else
					{
						if (!gennew && !currentmodel->lightmaps.prebaked)
						{
							int i = cl_max_lightstyles;
							for (i = 0; i < cl_max_lightstyles; i++)
							{
								if (webostate->lightstylevalues[i] != d_lightstylevalue[i])
								{	//a lightstyle changed. something needs to be rebuilt. FIXME: should probably have a bitmask for whether the lightstyle is relevant...
									gennew = true;
									break;
								}
							}
						}

						if (!gennew && r_refdef.areabitsknown && memcmp(webostate->areamask, r_refdef.areabits, MAX_MAP_AREA_BYTES))
							gennew = true;

						if (!gennew && (webostate->cluster[0] != r_viewcluster || webostate->cluster[1] != r_viewcluster2))
						{
							if (webostate->pvs.buffersize != currentmodel->pvsbytes || r_viewcluster2 < 0)
								gennew = true;	//o.O
							else if (memcmp(webostate->pvs.buffer, webostate->wmodel->funcs.ClusterPVS(webostate->wmodel, r_viewcluster, NULL, PVM_FAST), currentmodel->pvsbytes))
								gennew = true;
							else
							{	//okay, so the pvs didn't change despite the clusters changing. this happens when using unvised maps or lots of func_detail
								//just hack the cluster numbers so we don't have to do the memcmp above repeatedly for no reason.
								webostate->cluster[0] = r_viewcluster;
								webostate->cluster[1] = r_viewcluster2;
							}
						}
					}

					if (gennew)
					{
						int i;
						static int ebogensequence;
						if (!currentmodel->numbatches)
						{
							int sortid;
							batch_t *batch;
							currentmodel->numbatches = 0;
							for (sortid = 0; sortid < SHADER_SORT_COUNT; sortid++)
								for (batch = currentmodel->batches[sortid]; batch != NULL; batch = batch->next)
								{
									batch->user.bmodel.ebobatch = currentmodel->numbatches;
									currentmodel->numbatches++;
								}
							/*TODO submodels too*/
						}

						webogeneratingstate = true;

						webogenerating = NULL;
						if (webostate)
							webostate->lastvalid = cls.framecount;
						for (link = &webostates; (kill=*link); )
						{
							if (kill->lastvalid < cls.framecount-5 && kill->wmodel == currentmodel && kill != webostate)
							{	//this one looks old... kill it.
								if (webogenerating)
									R_DestroyWorldEBO(webogenerating);	//can't use more than one, tidy up stale ones
								webogenerating = kill;
								*link = kill->next;
							}
							else
								link = &(*link)->next;
						}
						if (!webogenerating)
						{
							webogenerating = BZ_Malloc(sizeof(*webogenerating) + sizeof(webogenerating->batches[0]) * (currentmodel->numbatches-1) + currentmodel->pvsbytes);
							memset(&webogenerating->vbo, 0, sizeof(webogenerating->vbo));
							memset(&webogenerating->ebo, 0, sizeof(webogenerating->ebo));
							webogenerating->ebomem = NULL;
							webogenerating->numbatches = 0;
						}
						VectorCopy(r_refdef.vieworg, webogenerating->lastpos);
						webogenerating->wmodel = currentmodel;
						webogenerating->framecount = --ebogensequence;
						webogenerating->cluster[0] = r_viewcluster;
						webogenerating->cluster[1] = r_viewcluster2;
						webogenerating->pvs.buffer = (qbyte*)(webogenerating+1) + sizeof(webogenerating->batches[0])*(currentmodel->numbatches-1);
						webogenerating->pvs.buffersize = currentmodel->pvsbytes;
						memcpy(webogenerating->areamask, r_refdef.areabits, MAX_MAP_AREA_BYTES);
						for (i = 0; i < cl_max_lightstyles; i++)
							webogenerating->lightstylevalues[i] = d_lightstylevalue[i];
						Q_strncpyz(webogenerating->dbgid, "webostate", sizeof(webogenerating->dbgid));
						COM_AddWork(WG_LOADER, R_GenWorldEBO, webogenerating, NULL, 0, 0);
					}
				}
			}
#endif

			//if they teleported, don't show something ugly - like obvious wallhacks.
			if (webogenerating && !r_novis.ival && cl.splitclients<=1 && webostate && (webostate->cluster[0] != r_viewcluster || webostate->cluster[1] != r_viewcluster2))
			{
				vec3_t m;
				float d;
				VectorSubtract(webostate->lastpos, r_refdef.vieworg, m);
				d = sqrt(DotProduct(m,m));
				if (d > 40 && memcmp(webostate->pvs.buffer, webogenerating->wmodel->funcs.ClusterPVS(webogenerating->wmodel, webogenerating->cluster[0], NULL, PVM_FAST), webostate->pvs.buffersize))
				{
					Con_DLPrintf(2, "Blocking for scenecache generation (distance = %g)\n", d);
					webostate = webogenerating;
					COM_WorkerPartialSync(webogenerating, &webogeneratingstate, true);
				}
			}
			else if (webogenerating && !webostate)
			{	//block the first time around to avoid possible race conditions.
				webostate = webogenerating;
				COM_WorkerPartialSync(webogenerating, &webogeneratingstate, true);
			}

			if (webostate)
			{
				entvis = surfvis = webostate->pvs.buffer;

				webostate->lastvalid = cls.framecount;

				if (webostate->cluster[0] == r_viewcluster && webostate->cluster[1] == r_viewcluster2)
					VectorCopy(r_refdef.vieworg, webostate->lastpos);

				r_dlightlightmaps = false;	//don't waste time on dlighting bmodels.

				RSpeedEnd(RSPEED_WORLDNODE);

				areas[0] = 1;
				areas[1] = r_viewarea;
				CL_LinkStaticEntities(entvis, areas);
				TRACE(("dbg: calling R_DrawParticles\n"));
				if (!r_refdef.recurse && !(r_refdef.flags & RDF_DISABLEPARTICLES))
					P_DrawParticles ();
				if (!r_refdef.recurse)
					CL_EmitPersistentDecals ();	//nettest: persistent lit decals (before BE_DrawWorld consumes cl_stris)

				TRACE(("dbg: calling BE_DrawWorld\n"));
				r_refdef.scenevis = surfvis;
				BE_DrawWorld(webostate->rbatches);

				/*FIXME: move this away*/
				if (currentmodel->fromgame == fg_quake || currentmodel->fromgame == fg_halflife)
					Surf_LessenStains();
				return;
			}
		}
#endif

#ifdef RTLIGHTS
		if (r_shadow_realtime_dlight.ival || currentmodel->type != mod_brush || !(currentmodel->fromgame == fg_quake || currentmodel->fromgame == fg_halflife) || !currentmodel->funcs.MarkLights)
			r_dlightlightmaps = false; //don't do double lighting.
#endif

		Surf_PushChains(currentmodel->batches);

		if (currentmodel->funcs.PrepareFrame)
		{
			int clusters[2] = {r_viewcluster, r_viewcluster2};
			currentmodel->funcs.PrepareFrame(currentmodel, &r_refdef, r_viewarea, clusters, &surf_frustumvis[r_refdef.recurse], &entvis, &surfvis);
		}
		else if (currentmodel->type != mod_brush)
			entvis = surfvis = NULL;
#ifdef MAP_DOOM
		else if (currentmodel->fromgame == fg_doom)
		{
			entvis = surfvis = NULL;
			R_DoomWorld();
		}
#endif
		else
			entvis = surfvis = NULL;

		RSpeedEnd(RSPEED_WORLDNODE);

		/* FTESurf: HOW MUCH WORLD DID *THIS* VIEW EMIT?

		   r_speeds aggregates every view in the frame into one set of numbers, so on
		   a map with portals it cannot answer "did the recursed view draw anything",
		   which is the only question that separates a culling bug from a shading one.
		   Counted over the current view's own range (firstmesh..meshes) and reported
		   with the recursion level, area and cluster that produced it. */
		{
			static int worldreports = 0;
			if (worldreports < 16 && !(r_refdef.flags & RDF_NOWORLDMODEL))
			{
				batch_t *b;
				int i, nm = 0, nb = 0;
				for (i = 0; i < SHADER_SORT_COUNT; i++)
					for (b = cl.worldmodel->batches[i]; b; b = b->next)
						if (b->meshes > b->firstmesh)
						{
							nb++;
							nm += b->meshes - b->firstmesh;
						}
				worldreports++;
				Con_DPrintf("[world] recurse %i: area %i cluster %i vis %s -> %i meshes in %i batches\n",
					r_refdef.recurse, r_viewarea, r_viewcluster,
					surfvis?"yes":"NONE", nm, nb);
			}
		}

		areas[0] = 1;
		areas[1] = r_viewarea;
		r_refdef.sceneareas = areas;
		if (!(r_refdef.flags & RDF_NOWORLDMODEL))
		{
			CL_LinkStaticEntities(entvis, r_refdef.sceneareas);
			TRACE(("dbg: calling R_DrawParticles\n"));
			if (!r_refdef.recurse && !(r_refdef.flags & RDF_DISABLEPARTICLES))
				P_DrawParticles ();
			if (!r_refdef.recurse)
				CL_EmitPersistentDecals ();	//nettest: persistent lit decals
		}

		//FTESurf Patch 218: the world walk emitted these near-to-far, which is
		//backwards for alpha.  Must be after the walk has filled the batches and
		//before they are submitted.
		Surf_SortBlendedChains(cl.worldmodel->batches);

		TRACE(("dbg: calling BE_DrawWorld\n"));
		r_refdef.scenevis = surfvis;
		BE_DrawWorld(cl.worldmodel->batches);

		Surf_PopChains(cl.worldmodel->batches);

		/*FIXME: move this away*/
		if (cl.worldmodel->fromgame == fg_quake || cl.worldmodel->fromgame == fg_halflife)
			Surf_LessenStains();

		r_refdef.sceneareas = NULL;
	}
}

unsigned int Surf_CalcMemSize(msurface_t *surf)
{
	if (surf->mesh)
		return 0;

	if (!surf->numedges)
		return 0;

	//figure out how much space this surface needs
	return sizeof(mesh_t) + 
	sizeof(index_t)*(surf->numedges-2)*3 +
	(sizeof(vecV_t)+sizeof(vec2_t)*2+sizeof(vec3_t)*3+sizeof(vec4_t))*surf->numedges;
}

void Surf_DeInit(void)
{
	int i;
	extern void R_VertLightBuffers_Flush(void);	//FTESurf Patch 262, gl_alias.c

	//FTESurf Patch 262: the per-instance static prop colour buffers.  This runs on
	//vid_restart AND -- because Surf_NewMap calls it -- on every map load, which is the one
	//that matters: those buffers are keyed on CPU pointers that the world model's memgroup
	//is about to free, so a recycled address would otherwise hit a stale entry.
	R_VertLightBuffers_Flush();

#ifdef THREADEDWORLD
	webo_blocklightmapupdates = 0;
	while(webogenerating)
		COM_WorkerPartialSync(webogenerating, &webogeneratingstate, true);
	while (webostates)
	{
		void *webostate = webostates;
		webostates = webostates->next;
		R_DestroyWorldEBO(webostate);
	}
#endif

	for (i = 0; i < numlightmaps; i++)
	{
		Surf_FreeLightmap(lightmap[i]);
		lightmap[i] = NULL;
	}

	if (lightmap)
		BZ_Free(lightmap);

	for (i = 0; i < R_MAX_RECURSE; i++)
		Z_Free(surf_frustumvis[i].buffer);
	memset(surf_frustumvis, 0, sizeof(surf_frustumvis));

	CL_FreeDlights();

	lightmap=NULL;
	numlightmaps=0;

	Alias_Shutdown();
	Shader_ResetRemaps();
}

void Surf_Clear(model_t *mod)
{
	int i;
	vbo_t *vbo;
//	if (mod->fromgame == fg_doom3)
//		return;/*they're on the hunk*/

#ifdef THREADEDWORLD
	struct webostate_s **link, *t;
	while(webogenerating)
		COM_WorkerPartialSync(webogenerating, &webogeneratingstate, true);

	for (link = &webostates; (t=*link); )
	{
		if (t->wmodel == mod)
		{
			*link = t->next;
			R_DestroyWorldEBO(t);
		}
		else
			link = &(*link)->next;
	}
#endif

	while(mod->vbos)
	{
		vbo = mod->vbos;
		mod->vbos = vbo->next;
		BE_ClearVBO(vbo, false);
	}

	if (!mod->submodelof)
	{
		for (i = 0; i < mod->numtextures; i++)
		{
			R_UnloadShader(mod->textures[i]->shader);
			mod->textures[i]->shader = NULL;
		}
	}
	mod->numtextures = 0;

	BZ_Free(mod->shadowbatches);
	mod->numshadowbatches = 0;
	mod->shadowbatches = NULL;
#ifdef RTLIGHTS
	Sh_PurgeShadowMeshes();
#endif

	BZ_Free(blocklights);
	BZ_Free(blocknormals);
	blocklights = NULL;
	blocknormals = NULL;
	maxblocksize = 0;
}

uploadfmt_t Surf_NameToFormat(const char *nam)
{
	static uploadfmt_t tab[] = {PTI_L8, PTI_RGB8, PTI_BGRA8, PTI_A2BGR10, PTI_E5BGR9, PTI_RGBA16F, PTI_RGBA32F, PTI_RGB565, PTI_RGBA4444, PTI_RGBA5551};
	int idx = atoi(nam)-1;
	if (idx>=0 && idx < countof(tab))
		return tab[idx];

	if (!Q_strcasecmp(nam, "e5bgr9") || !Q_strcasecmp(nam, "rgb9e5"))
		return PTI_E5BGR9;	//prefered hdr format, for some reason.
	if (!Q_strcasecmp(nam, "a2bgr10") || !Q_strcasecmp(nam, "rgb10a2") || !Q_strcasecmp(nam, "rgb10"))
		return PTI_A2BGR10;	//prefered ldr format. hurrah for 10 bits.
	if (!Q_strcasecmp(nam, "rgba32f"))
		return PTI_RGBA32F;	//big bulky hdr format
	if (!Q_strcasecmp(nam, "rgba16f"))
		return PTI_RGBA16F;	//tolerable hdr format
//	if (!Q_strcasecmp(nam, "rgba8s"))
//		return PTI_RGBA8_SIGNED;
	if (!Q_strcasecmp(nam, "rgb565") || !Q_strcasecmp(nam, "rgb5"))
		return PTI_RGB565;	//boo hiss
	if (!Q_strcasecmp(nam, "rgba4444") || !Q_strcasecmp(nam, "rgba4"))
		return PTI_RGBA4444;	//erk
	if (!Q_strcasecmp(nam, "rgba5551") || !Q_strcasecmp(nam, "rgba51") || !Q_strcasecmp(nam, "rgb5a1"))
		return PTI_RGBA5551;
	if (!Q_strcasecmp(nam, "argb4444"))
		return PTI_ARGB4444;
	if (!Q_strcasecmp(nam, "argb1555"))
		return PTI_ARGB1555;
	if (!Q_strcasecmp(nam, "rgbx8") || !Q_strcasecmp(nam, "bgrx8") || !Q_strcasecmp(nam, "rgba8") || !Q_strcasecmp(nam, "bgra8"))
	{	//most common format(s) for lightmaps in various engines...
		if (sh_config.texfmt[PTI_BGRX8])
			return PTI_BGRX8;	//probably fastest
		if (sh_config.texfmt[PTI_RGBX8])
			return PTI_RGBX8;	//no bgr? odd...
		if (sh_config.texfmt[PTI_BGRA8])
			return PTI_BGRA8;	//no padded formats at all? erk!
		return PTI_RGBA8;	//probably the slowest for pc hardware.
	}
	if (!Q_strcasecmp(nam, "rgb8") || !Q_strcasecmp(nam, "bgr8"))
		return PTI_RGB8;	//generally not recommended (misaligned so the gpu has to compensate)
	if (!Q_strcasecmp(nam, "l8"))
		return PTI_L8;
	if (*nam)
		Con_Printf("Unknown lightmap format: %s\n", nam);
	return PTI_INVALID;
}

//pick fastest mode for lightmap data
uploadfmt_t Surf_LightmapMode(model_t *model)
{
	uploadfmt_t fmt = Surf_NameToFormat(r_lightmap_format.string);
	if (model && model->lightmaps.prebaked && model->lightmaps.fmt!=LM_RGB8)
		fmt = PTI_INVALID;	//don't let them force it away if we can't support it. this sucks.
	if (!sh_config.texfmt[fmt])
	{
		qboolean hdr = (vid.flags&VID_SRGBAWARE), rgb = false;

		if (fmt != PTI_INVALID)
			Con_Printf("lightmap format %s not supported by renderer\n", r_lightmap_format.string);

		if (model)
		{
			switch (model->lightmaps.fmt)
			{
			case LM_E5BGR9:
				hdr = rgb = true;
				break;
			case LM_RGB8:
				rgb = true;
				break;
			case LM_L8:
				break;
			}
			if (model->deluxdata)
				rgb = true;

			if (model->terrain)	//the terrain code requires rgba8.
				hdr = false;
		}

		if (sh_config.texfmt[PTI_E5BGR9] && hdr)
			fmt = PTI_E5BGR9;
		else if (sh_config.texfmt[PTI_RGBA16F] && hdr)
			fmt = PTI_RGBA16F;
		else if (sh_config.texfmt[PTI_RGBA32F] && hdr)
			fmt = PTI_RGBA32F;
		else if (sh_config.texfmt[PTI_A2BGR10] && rgb)
			fmt = PTI_A2BGR10;
		else if (sh_config.texfmt[PTI_L8] && !rgb && !r_deluxemapping && r_dynamic.ival<=0)
			fmt = PTI_L8;
		else if (sh_config.texfmt[PTI_BGRX8])
			fmt = PTI_BGRX8;
		else if (sh_config.texfmt[PTI_RGB8])
			fmt = PTI_RGB8;
		else
			fmt = PTI_RGBX8;

	}

	if (!model->submodelof)
		Con_DPrintf("%s: Using lightmap format %s\n", model->name, Image_FormatName(fmt));

	return fmt;
}

static void Surf_FreeLightmap(lightmapinfo_t *lm)
{
	if (lm)
	{
#ifdef GLQUAKE
		if (lm->pbo_handle)
		{
			qglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, lm->pbo_handle);
			qglUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
			qglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
			qglDeleteBuffersARB(1, &lm->pbo_handle);
		}
#endif
		if (!lm->external)
			Image_DestroyTexture(lm->lightmap_texture);
		BZ_Free(lm);
	}
}

//needs to be followed by a BE_UploadAllLightmaps at some point
int Surf_NewLightmaps(int count, int width, int height, uploadfmt_t fmt, qboolean deluxe, qboolean sunvis)
{
	int first = numlightmaps;
	int i;

	unsigned int pixbytes, pixw, pixh, pixd;
	unsigned int dpixbytes, dpixw, dpixh, dpixd;
	uploadfmt_t dfmt;
#ifdef THREADEDWORLD
	extern int webo_blocklightmapupdates;
	webo_blocklightmapupdates = 0;
#endif

	if (!count)
		return -1;

	if (deluxe && (count & 1))
	{
		deluxe = false;
//		count+=1;
		Con_Print("WARNING: Deluxemapping with odd number of lightmaps\n");
	}

	Image_BlockSizeForEncoding(fmt, &pixbytes, &pixw, &pixh, &pixd);
	if (pixw != 1 || pixh != 1 || pixd != 1)
		return -1;	//compressed formats are unsupported
	dfmt = PTI_A2BGR10;	//favour this one, because it tends to be slightly faster.
	if (!sh_config.texfmt[dfmt])
		dfmt = PTI_BGRX8;
	if (!sh_config.texfmt[dfmt])
		dfmt = PTI_RGBX8;
	if (!sh_config.texfmt[dfmt])
		dfmt = PTI_RGB8;
	Image_BlockSizeForEncoding(dfmt, &dpixbytes, &dpixw, &dpixh, &dpixd);
	if (dpixw != 1 || dpixh != 1 || dpixd != 1)
		return -1;	//compressed formats are unsupported

	Sys_LockMutex(com_resourcemutex);

	i = numlightmaps + count;
	lightmap = BZ_Realloc(lightmap, sizeof(*lightmap)*(i));
	while(i --> first)
	{
#ifdef GLQUAKE
		extern cvar_t gl_pbolightmaps;
		//we might as well use a pbo for our staging memory.
		if (qrenderer == QR_OPENGL && qglBufferStorage && qglMapBufferRange && gl_pbolightmaps.ival && Sys_IsMainThread())
		{	//glBufferStorage and GL_MAP_PERSISTENT_BIT generally means gl4.4+ (we need persistent for scenecache)
			//pbos are 2.1
			if (deluxe && ((i - numlightmaps)&1))
			{
				lightmap[i] = Z_Malloc(sizeof(*lightmap[i]));
				lightmap[i]->width = width;
				lightmap[i]->height = height;
				lightmap[i]->lightmaps = NULL;
				lightmap[i]->stainmaps = NULL;
				lightmap[i]->hasdeluxe = false;
				lightmap[i]->pixbytes = dpixbytes;
				lightmap[i]->fmt = dfmt;
			}
			else
			{
				lightmap[i] = Z_Malloc(sizeof(*lightmap[i]) + (sizeof(stmap)*3)*width*height);
				lightmap[i]->width = width;
				lightmap[i]->height = height;
				lightmap[i]->lightmaps = NULL;
				lightmap[i]->stainmaps = (qbyte*)(lightmap[i]+1);
				lightmap[i]->hasdeluxe = deluxe;
				lightmap[i]->pixbytes = pixbytes;
				lightmap[i]->fmt = fmt;
			}

			qglGenBuffersARB(1, &lightmap[i]->pbo_handle);
			qglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, lightmap[i]->pbo_handle);
			//note: we only write the memory. the pbo would normally be in system memory anyway so there shouldn't be too much cost from coherent mappings.
			qglBufferStorage(GL_PIXEL_UNPACK_BUFFER_ARB, lightmap[i]->pixbytes*width*height, NULL, GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT);
			lightmap[i]->lightmaps = qglMapBufferRange(GL_PIXEL_UNPACK_BUFFER_ARB, 0, lightmap[i]->pixbytes*width*height, GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT);
			qglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
		}
		else
#endif
		{
			if (deluxe && ((i - numlightmaps)&1))
			{	//deluxemaps always use a specific format.
				lightmap[i] = Z_Malloc(sizeof(*lightmap[i]) + (sizeof(qbyte)*dpixbytes)*width*height);
				lightmap[i]->width = width;
				lightmap[i]->height = height;
				lightmap[i]->lightmaps = (qbyte*)(lightmap[i]+1);
				lightmap[i]->stainmaps = NULL;
				lightmap[i]->hasdeluxe = false;
				lightmap[i]->pixbytes = dpixbytes;
				lightmap[i]->fmt = dfmt;
			}
			else
			{
				lightmap[i] = Z_Malloc(sizeof(*lightmap[i]) + (sizeof(qbyte)*pixbytes + sizeof(stmap)*3)*width*height);
				lightmap[i]->width = width;
				lightmap[i]->height = height;
				lightmap[i]->lightmaps = (qbyte*)(lightmap[i]+1);
				lightmap[i]->stainmaps = (stmap*)(lightmap[i]->lightmaps+pixbytes*width*height);
				lightmap[i]->hasdeluxe = deluxe;
				lightmap[i]->pixbytes = pixbytes;
				lightmap[i]->fmt = fmt;
			}
		}

		lightmap[i]->rectchange.l = 0;
		lightmap[i]->rectchange.t = 0;
		lightmap[i]->rectchange.b = lightmap[i]->height;
		lightmap[i]->rectchange.r = lightmap[i]->width;

		//nettest (SUNVIS): a single-channel page mirroring this lightmap page's atlas coords.
		//Allocated ONLY when the map actually shipped a SUNVIS lump - otherwise every page
		//would cost width*height bytes for data that is uniformly "fully lit" anyway.
		//Pre-filled 255 so any luxel the fill pass never reaches reads as fully sunlit, which
		//is the fail-safe direction (dynamic shadow behaves exactly as it does today).
		lightmap[i]->sunvis_texture = r_nulltex;
		lightmap[i]->sunvis_modified = false;
		if (sunvis)
		{
			lightmap[i]->sunvis_pixels = Z_Malloc(width*height);
			//0 = no occlusion = fully sunlit = dynamic shadow at full strength. Any luxel the
			//fill pass never reaches therefore behaves exactly as it did before SUNVIS existed.
			memset(lightmap[i]->sunvis_pixels, 0, width*height);
			lightmap[i]->sunvis_modified = true;
		}
		else
			lightmap[i]->sunvis_pixels = NULL;


		lightmap[i]->lightmap_texture = r_nulltex;
		lightmap[i]->modified = true;
//			lightmap[i]->shader = NULL;
		lightmap[i]->external = false;
		// reset stainmap since it now starts at 255
		if (lightmap[i]->stainmaps)
			memset(lightmap[i]->stainmaps, 255, width*height*3*sizeof(stmap));
	}

	numlightmaps += count;

	Sys_UnlockMutex(com_resourcemutex);

	return first;
}
int Surf_NewExternalLightmaps(int count, char *filepattern, qboolean deluxe)
{
	unsigned int nulllight = 0xffffffff;
	unsigned int nulldeluxe = 0xffff7f7f;
	int first = numlightmaps;
	int i;
	char nname[MAX_QPATH];
	qboolean odd = (count & 1) && deluxe;

#ifdef THREADEDWORLD
	extern int webo_blocklightmapupdates;
	webo_blocklightmapupdates = 0;
#endif

	if (!count)
		return -1;

	if (odd)
		count++;

	i = numlightmaps + count;
	lightmap = BZ_Realloc(lightmap, sizeof(*lightmap)*(i));
	while(i > first)
	{
		i--;

		lightmap[i] = Z_Malloc(sizeof(*lightmap[i]));
		lightmap[i]->width = 0;
		lightmap[i]->height = 0;
		lightmap[i]->lightmaps = NULL;
		lightmap[i]->stainmaps = NULL;

		lightmap[i]->modified = false;
		lightmap[i]->external = true;
		lightmap[i]->hasdeluxe = (deluxe && !((i - numlightmaps)&1));

		Q_snprintfz(nname, sizeof(nname), filepattern, i - numlightmaps);

		TEXASSIGN(lightmap[i]->lightmap_texture, R_LoadHiResTexture(nname, NULL, (r_lightmap_nearest.ival?IF_NEAREST:IF_LINEAR)|IF_NOMIPMAP));
		if (lightmap[i]->lightmap_texture->status == TEX_LOADING)
			COM_WorkerPartialSync(lightmap[i]->lightmap_texture, &lightmap[i]->lightmap_texture->status, TEX_LOADING);
		if (lightmap[i]->lightmap_texture->status == TEX_FAILED)
		{
			if ((i&1) && deluxe)
				lightmap[i]->lightmap_texture = R_LoadReplacementTexture("*nulldeluxe", NULL, IF_LOADNOW, &nulldeluxe, 1, 1, TF_RGBX32);
			else
				lightmap[i]->lightmap_texture = R_LoadReplacementTexture("*nulllight", NULL, IF_LOADNOW, &nulllight, 1, 1, TF_RGBX32);
		}
		lightmap[i]->width = lightmap[i]->lightmap_texture->width;
		lightmap[i]->height = lightmap[i]->lightmap_texture->height;
		lightmap[i]->fmt = lightmap[i]->lightmap_texture->format;
	}

	if (odd)
	{
		i = numlightmaps+count-1;
		if (!TEXVALID(lightmap[i]->lightmap_texture))
		{	//FIXME: no deluxemaps after all...
			Z_Free(lightmap[i]);
			lightmap[i] = NULL;
			count--;
		}
	}

	numlightmaps += count;

	return first;
}

void Surf_BuildModelLightmaps (model_t *m)
{
	int		i;
	int shift;
	msurface_t *surf;
	batch_t *batch;
	int sortid;
	int newfirst;
	uploadfmt_t fmt;

	if (m->loadstate != MLS_LOADED)
		return;

#ifdef TERRAIN
	//easiest way to deal with heightmap lightmaps is to just purge the entire thing.
	if (m->terrain)
		Terr_PurgeTerrainModel(m, false, false);	//FIXME: cop out. middle arg should be 'true'.
#endif

	if (m->type != mod_brush)
		return;

	if (!m->lightmaps.count)
	{
		//FTESurf build 11: a brush model with no lightmap PAGES leaves here
		//before any of the fixup below, so its surfaces are never painted and
		//it draws flat.  Counted rather than listed -- on a Source map there
		//can be thousands.
		if (r_texdiag.ival && m->nummodelsurfaces)
		{
			//Most of these are trigger volumes, which are invisible and are
			//SUPPOSED to be unlit.  What matters is whether any DRAWN surface is
			//in here, so count those separately and name the texture -- an
			//unlit drawn surface is the pale one.
			int k, drawn = 0;
			const char *tn = "?";
			for (k = 0; k < m->nummodelsurfaces; k++)
			{
				msurface_t *s = m->surfaces + k + m->firstmodelsurface;
				if (s->flags & (SURF_NODRAW|SURF_DRAWSKY))
					continue;
				if (s->texinfo && (s->texinfo->flags & TEX_SPECIAL))
					continue;
				if (!drawn && s->texinfo && s->texinfo->texture)
					tn = s->texinfo->texture->name;
				drawn++;
			}
			if (drawn)
				Con_Printf("[texdiag] LM0 %-20s NO LIGHTMAP PAGES, %i surfaces, %i DRAWN  first=%s\n",
					m->name, m->nummodelsurfaces, drawn, tn);
		}
		return;
	}

	currentmodel = m;
	shift = Surf_LightmapShift(currentmodel);

	fmt = Surf_LightmapMode(m);

#ifdef THREADEDWORLD
	//make sure nothing is poking the lightmaps while we're rewriting them
	while(webogenerating)
		COM_WorkerPartialSync(webogenerating, &webogeneratingstate, true);
#endif

	R_BumpLightstyles(m->lightmaps.maxstyle);	//should only really happen with lazy loading

	if (m->submodelof && m->lightmaps.prebaked)	//FIXME: should be all bsp formats
	{
		if (m->submodelof->loadstate != MLS_LOADED)
			return;
		newfirst = m->submodelof->lightmaps.first;
	}
	else
	{
		if (!m->lightdata && m->lightmaps.count && m->lightmaps.prebaked)
		{
			char pattern[MAX_QPATH];
			COM_StripAllExtensions(m->name, pattern, sizeof(pattern));
			Q_strncatz(pattern, "/lm_%04u.tga", sizeof(pattern));
			newfirst = Surf_NewExternalLightmaps(m->lightmaps.count, pattern, m->lightmaps.deluxemapping);
			m->lightmaps.count = numlightmaps - newfirst;
		}
		else
			newfirst = Surf_NewLightmaps(m->lightmaps.count, m->lightmaps.width, m->lightmaps.height, fmt, m->lightmaps.deluxemapping, m->sunvisdata != NULL);
	}

	//fixup batch lightmaps
	for (sortid = 0; sortid < SHADER_SORT_COUNT; sortid++)
	for (batch = m->batches[sortid]; batch != NULL; batch = batch->next)
	{
		for (i = 0; i < MAXRLIGHTMAPS; i++)
		{
			if (batch->lightmap[i] < 0)
				continue;
			batch->lightmap[i] = batch->lightmap[i] - m->lightmaps.first + newfirst;
		}
	}

	if (m->lightmaps.prebaked)
	{
		int j;
		unsigned char *src, *stop;
		unsigned char *dst;


		//fixup surface lightmaps, and paint
		for (i=0; i<m->nummodelsurfaces; i++)
		{
			surf = m->surfaces + i + m->firstmodelsurface;
			for (j = 0; j < MAXRLIGHTMAPS; j++)
			{
				if (surf->lightmaptexturenums[j] < m->lightmaps.first)
				{
					surf->lightmaptexturenums[j] = -1;
					continue;
				}
				if (surf->lightmaptexturenums[j] >= m->lightmaps.first+m->lightmaps.count)
				{
					surf->lightmaptexturenums[j] = -1;
					continue;
				}
				surf->lightmaptexturenums[j] = surf->lightmaptexturenums[0] - m->lightmaps.first + newfirst;
			}
		}

		if (!m->submodelof)
		for (i = 0; i < m->lightmaps.count; i++)
		{
			if (lightmap[newfirst+i]->external || !m->lightdata)
				continue;

			if (lightmap[newfirst+i]->fmt == m->lightmaps.prebaked)
			{
				unsigned int bb,bw,bh,bd;
				Image_BlockSizeForEncoding(m->lightmaps.prebaked, &bb,&bw,&bh,&bd);

				dst = lightmap[newfirst+i]->lightmaps;
				src = m->lightdata + i*m->lightmaps.width*m->lightmaps.height*bb;
				stop = m->lightdata + (i+1)*m->lightmaps.width*m->lightmaps.height*bb;
				if (stop-m->lightdata > m->lightdatasize)
					stop = m->lightdata + m->lightdatasize;
				memcpy(dst, src, stop-src);
			}
			//FIXME: replace with Image_ChangeFormat here. but the data may be partial for the last mip.
			else switch(m->lightmaps.fmt)
			{
			case LM_RGB8:
				dst = lightmap[newfirst+i]->lightmaps;
				src = m->lightdata + i*m->lightmaps.width*m->lightmaps.height*3;
				stop = m->lightdata + (i+1)*m->lightmaps.width*m->lightmaps.height*3;
				if (stop-m->lightdata > m->lightdatasize)
					stop = m->lightdata + m->lightdatasize;
				switch(lightmap[newfirst+i]->fmt)
				{
				default:
					Sys_Error("Surf_BuildModelLightmaps: Bad format - %s\n", Image_FormatName(lightmap[newfirst+i]->fmt));
					break;
				case PTI_A2BGR10:
					for (; src < stop; dst += 4, src += 3)
						*(unsigned int*)dst = (0x3u<<30) | (src[2]<<22) | (src[1]<<12) | (src[0]<<2);
					break;
				case PTI_E5BGR9:
					for (; src < stop; dst += 4, src += 3)
						*(unsigned int*)dst = Surf_PackE5BRG9(src[0], src[1], src[2], 8);
					break;
				case PTI_BGRA8:
				case PTI_BGRX8:
					for (; src < stop; dst += 4, src += 3)
					{
						dst[0] = src[2];
						dst[1] = src[1];
						dst[2] = src[0];
						dst[3] = 255;
					}
					break;
				case PTI_RGBA8:
				case PTI_RGBX8:
					for (; src < stop; dst += 4, src += 3)
					{
						dst[0] = src[0];
						dst[1] = src[1];
						dst[2] = src[2];
						dst[3] = 255;
					}
					break;
				case PTI_BGR8:
					for (; src < stop; dst += 3, src += 3)
					{
						dst[0] = src[2];
						dst[1] = src[1];
						dst[2] = src[0];
					}
					break;
				case PTI_RGB8:
					for (; src < stop; dst += 3, src += 3)
					{
						dst[0] = src[0];
						dst[1] = src[1];
						dst[2] = src[2];
					}
					break;
				case PTI_RGB565:
					for (; src < stop; dst += 2, src += 3)
						*(unsigned short*)dst = ((src[0]>>3)<<11)|((src[1]>>2)<<5)|((src[2]>>3)<<0);
					break;
				case PTI_L8:
					for (; src < stop; dst += 1, src += 3)
					{
						dst[0] = max(max(src[0], src[1]), src[2]);
					}
					break;
				}
				break;

			case LM_E5BGR9:
				dst = lightmap[newfirst+i]->lightmaps;
				src = m->lightdata + i*m->lightmaps.width*m->lightmaps.height*4;
				stop = m->lightdata + (i+1)*m->lightmaps.width*m->lightmaps.height*4;
				if (stop-m->lightdata > m->lightdatasize)
					stop = m->lightdata + m->lightdatasize;

				if (lightmap[newfirst+i]->fmt == PTI_E5BGR9)
					memcpy(dst, src, stop-src);
				else	//this can happen on older gpus...
					Con_Printf(CON_WARNING"Unsupported lightmap format. set ^[/r_lightmap_format e5bgr9^]\n");
				break;
			default:
				Con_Printf(CON_WARNING"Unsupported input lightmap format\n");
				break;
			}
		}
	}
	else
	{
		int j;

//		if (*m->name == '*')
//		{
//			if (!cl.worldmodel || cl.worldmodel->loadstate != MLS_LOADED)
//				return;
//		}
		//fixup surface lightmaps, and paint
		for (i=0; i<m->nummodelsurfaces; i++)
		{
			surf = m->surfaces + i + m->firstmodelsurface;
			for (j = 0; j < MAXRLIGHTMAPS; j++)
			{
				if (surf->lightmaptexturenums[j] < m->lightmaps.first)
				{
					surf->lightmaptexturenums[j] = -1;
					continue;
				}
				if (surf->lightmaptexturenums[j] >= m->lightmaps.first+m->lightmaps.count)
				{
					surf->lightmaptexturenums[j] = -1;
					continue;
				}
				surf->lightmaptexturenums[j] = surf->lightmaptexturenums[j] - m->lightmaps.first + newfirst;

				Surf_BuildLightMap (m, surf, j, shift, r_ambient.value*255, d_lightstylevalue);
			}
		}
	}
	m->lightmaps.first = newfirst;

	/*
	FTESurf build 11: did this model's surfaces actually END UP lightmapped?

	A surface can be allocated a lightmap by Mod_Batches_AllocLightmaps and then
	quietly lose it here -- the two range checks above stamp -1 on anything
	outside [lightmaps.first, first+count) -- and a surface with
	lightmaptexturenums[0] == -1 draws against a default white lightmap.  That is
	not black and it is not obviously broken; it is FLAT AND NEUTRAL, which on a
	warm map reads as "pale", and it was reported as a texture problem.

	`samples` is the other half: no luxels means nothing to paint whatever the
	allocation says.  Printed per model so the world and the brush entities can
	be compared side by side, which is the whole question.
	*/
	if (r_texdiag.ival && m->lightmaps.count)
	{
		int lit = 0, unlit = 0, nosamples = 0, k;
		double lr = 0, lg = 0, lb = 0;
		size_t nlux = 0;
		for (k = 0; k < m->nummodelsurfaces; k++)
		{
			surf = m->surfaces + k + m->firstmodelsurface;
			if (surf->lightmaptexturenums[0] < 0)
				unlit++;
			else
				lit++;
			if (!surf->samples)
				nosamples++;

			/*
			THE LUXELS THEMSELVES, averaged.  Everything above says only that a
			lightmap was allocated and painted; this says what was painted WITH.
			If a brush entity's average luxel is near-white while the world's is
			warm and dark, then the renderer is doing exactly as it was told and
			the wrong data is being read out of the lighting lump -- which is a
			different bug in a different file from anything the counts can show.
			*/
			else if (m->lightmaps.fmt == LM_E5BGR9 && surf->lightmaptexturenums[0] >= 0)
			{
				int smax = (surf->extents[0]>>surf->lmshift)+1;
				int tmax = (surf->extents[1]>>surf->lmshift)+1;
				unsigned int *lx = (unsigned int*)surf->samples;
				int n = smax*tmax, q;
				if (n > 256) n = 256;	//a sample, not a survey
				for (q = 0; q < n; q++)
				{
					unsigned int v = lx[q];
					double sc = pow(2.0, (double)((v>>27)&0x1f) - 15 - 9);
					lr += ((v>> 0)&0x1ff)*sc;
					lg += ((v>> 9)&0x1ff)*sc;
					lb += ((v>>18)&0x1ff)*sc;
					nlux++;
				}
			}
		}
		if (nlux)
			Con_Printf("[texdiag] LUX %-22s mean luxel %.3f %.3f %.3f  (R/B %.2f) over %u\n",
				m->name, lr/nlux, lg/nlux, lb/nlux,
				lb?(lr/lb):0.0, (unsigned)nlux);

		/*
		THE LAST LINK.  The surfaces can hold a perfectly good lightmap index and
		still draw unlit, because the BACKEND binds per batch, not per surface --
		batch->lightmap[0] is what actually reaches the shader.  A batch left at
		-1 whose surfaces are lit is invisible to every check above it.
		*/
		{
			int bl = 0, bunlit = 0, sid;
			batch_t *b;
			for (sid = 0; sid < SHADER_SORT_COUNT; sid++)
				for (b = m->batches[sid]; b; b = b->next)
				{
					if (b->lightmap[0] < 0)
						bunlit++;
					else
						bl++;
				}
			Con_Printf("[texdiag] BAT %-22s batches lit=%i unlit=%i\n", m->name, bl, bunlit);
		}
		/*
		`shift` and `overbright` are printed together because they have to
		AGREE ACROSS MODELS or the same lightmap comes out at a different
		brightness on a brush entity than on the world.  Surf_LightmapShift
		bakes the luxels darker by `shift` when MDLF_NEEDOVERBRIGHT is set, and
		the backend multiplies them back up per ENTITY on the same flag
		(gl_backend.c:3884).  One of the two disagreeing is a factor of four.
		*/
		Con_Printf("[texdiag] LM %-24s first=%-4i count=%-3i %ix%i  surfs lit=%i unlit=%i nosamples=%i  shift=%i overbright=%i fmt=%i\n",
			m->name, m->lightmaps.first, m->lightmaps.count,
			m->lightmaps.width, m->lightmaps.height, lit, unlit, nosamples,
			shift, (m->engineflags & MDLF_NEEDOVERBRIGHT)?1:0, (int)m->lightmaps.fmt);
	}
}

void Surf_ClearSceneCache(void)
{
#ifdef THREADEDWORLD
	while(webogenerating)
		COM_WorkerPartialSync(webogenerating, &webogeneratingstate, true);
	while (webostates)
	{
		void *webostate = webostates;
		webostates = webostates->next;
		R_DestroyWorldEBO(webostate);
	}
#endif
}

/*
==================
GL_BuildLightmaps

Builds the lightmap texture
with all the surfaces from all brush models
Groups surfaces into their respective batches (based on the lightmap number).
==================
*/
void Surf_BuildLightmaps (void)
{
	unsigned int		i, j;
	model_t	*m;

	extern model_t	*mod_known;
	extern int		mod_numknown;

	int maxstyle;

	//make sure the lightstyle values are correct (and be sure that the sizes cover all models).
	for (i = 0, maxstyle=0; i < mod_numknown; i++)
	{
		m = &mod_known[i];
		if (m->loadstate == MLS_LOADED)
			if (maxstyle < m->lightmaps.maxstyle)
				maxstyle = m->lightmaps.maxstyle;
	}
	R_BumpLightstyles(maxstyle);	//should only really happen with lazy loading
	R_AnimateLight();

	while(numlightmaps > 0)
	{
		numlightmaps--;
		Surf_FreeLightmap(lightmap[numlightmaps]);
		lightmap[numlightmaps] = NULL;
	}

	//FIXME: unload stuff that's no longer relevant somehow.
	for (i = 0; i < mod_numknown; i++)
	{
		m = &mod_known[i];
		if (m->loadstate != MLS_LOADED)
			continue;
		Surf_BuildModelLightmaps(m);

		for (j = 0; j < m->numenvmaps; j++)
			if (m->envmaps[j].image)
				m->envmaps[j].image->regsequence = r_regsequence;
	}
	BE_UploadAllLightmaps();
}



/*
===============
Surf_NewMap
===============
*/
void Surf_NewMap (model_t *worldmodel)
{
	char namebuf[MAX_QPATH];
	extern cvar_t host_mapname;
#ifdef BEF_PUSHDEPTH
	extern cvar_t r_polygonoffset_submodel_maps;
	char *s;
#endif
	int		i;

	cl.worldmodel = worldmodel;

	/*Patch 105: every cached model-light sample belongs to the OLD world's lightdata.*/
	if (!++r_modellight_seq)
		r_modellight_seq++;	//0 means "empty slot" in the cache

	//evil haxx
	r_dynamic.ival = r_dynamic.value;
	if (r_dynamic.ival > 0 && (!cl.worldmodel || cl.worldmodel->lightmaps.prebaked)) //quake3 has no lightmaps, disable r_dynamic
		r_dynamic.ival = 0;

	memset (&r_worldentity, 0, sizeof(r_worldentity));
	AngleVectors(r_worldentity.angles, r_worldentity.axis[0], r_worldentity.axis[1], r_worldentity.axis[2]);
	VectorInverse(r_worldentity.axis[1]);
	r_worldentity.model = cl.worldmodel;
	Vector4Set(r_worldentity.shaderRGBAf, 1, 1, 1, 1);
	VectorSet(r_worldentity.light_avg, 1, 1, 1);


	if (cl.worldmodel)
		COM_FileBase(cl.worldmodel->name, namebuf, sizeof(namebuf));
	else
		*namebuf = '\0';
	Cvar_Set(&host_mapname, namebuf);

	Surf_DeInit();

	r_viewcluster = -1;
	r_viewcluster2 = -1;
	surf_lastgoodworld = NULL;	//FTESurf P138: drop the void-PVS cache with the old world.
#ifdef BEF_PUSHDEPTH
	r_pushdepth = false;
	for (s = r_polygonoffset_submodel_maps.string; s && *s; )
	{
		s = COM_Parse(s);
		if (*com_token)
			if (wildcmp(com_token, namebuf))
			{
				r_pushdepth = true;
				break;
			}
	}
#endif

	TRACE(("dbg: Surf_NewMap: clear particles\n"));
	P_ClearParticles ();
	CL_RegisterParticles();

	/*
	  nettest Patch 141.  There was a second Shader_DoReload() immediately above
	  this block, with only the worldmodel sync and Mod_ParseInfoFromEntityLump
	  between the two.  It is gone, but NOT for the reason it first looked like.

	  I removed it expecting to halve the map-load shader cost, on the theory
	  that the entity parse sets `skyname` -> the r_skybox cvar -> a second
	  reload.  Measuring with the call-site labels showed that was wrong: BOTH
	  reloads during a map load come from elsewhere entirely (CL_MakeActive and
	  then SCR_UpdateScreen's per-frame call), because Shader_DoReload early-outs
	  while cls.state < ca_active -- which is the whole of a map load.  Both of
	  the calls in this file are no-ops at that point, and always were.

	  So this is tidying, not a fix, and it is written down as such: the real
	  cost is measured at the two sites that actually pay it.  Keeping one call
	  here rather than two costs nothing either way; keeping the one AFTER the
	  entity parse is simply the correct order if the guard ever changes.
	*/
	if (cl.worldmodel)
	{
		if (cl.worldmodel->loadstate == MLS_LOADING)
			COM_WorkerPartialSync(cl.worldmodel, &cl.worldmodel->loadstate, MLS_LOADING);
		Mod_ParseInfoFromEntityLump(cl.worldmodel);
	}
	shader_reload_why = "Surf_NewMap";
	Shader_DoReload();

#ifdef THREADEDWORLD
	Cvar_ForceCallback(&r_temporalscenecache);
#endif

	if (!pe)
		Cvar_ForceCallback(&r_particlesystem);
	R_Clutter_Purge();
TRACE(("dbg: Surf_NewMap: wiping them stains (getting the cloth out)\n"));
	Surf_WipeStains();
TRACE(("dbg: Surf_NewMap: building lightmaps\n"));
	Surf_BuildLightmaps ();


TRACE(("dbg: Surf_NewMap: ui\n"));
#ifdef VM_UI
	if (q3)
		q3->ui.Reset();
#endif
TRACE(("dbg: Surf_NewMap: tp\n"));
	TP_NewMap();

	for (i = 0; i < cl.num_statics; i++)
	{
		vec3_t mins, maxs;
		//fixme: no rotation
		if (!cl_static_entities[i].ent.model && cl_static_entities[i].mdlidx > 0 && cl_static_entities[i].mdlidx < countof(cl.model_precache))
			cl_static_entities[i].ent.model = cl.model_precache[cl_static_entities[i].mdlidx];
		else if (!cl_static_entities[i].ent.model && cl_static_entities[i].mdlidx < 0 && (-cl_static_entities[i].mdlidx) < countof(cl.model_csqcprecache))
			cl_static_entities[i].ent.model = cl.model_csqcprecache[-cl_static_entities[i].mdlidx];
		if (cl_static_entities[i].ent.model)
		{
			//unfortunately, we need to know the actual size so that we can get this right. bum.
			if (cl_static_entities[i].ent.model->loadstate == MLS_NOTLOADED)
				Mod_LoadModel(cl_static_entities[i].ent.model, MLV_WARNSYNC);
			if (cl_static_entities[i].ent.model->loadstate == MLS_LOADING)
				COM_WorkerPartialSync(cl_static_entities[i].ent.model, &cl_static_entities[i].ent.model->loadstate, MLS_LOADING);
			VectorAdd(cl_static_entities[i].ent.origin, cl_static_entities[i].ent.model->mins, mins);
			VectorAdd(cl_static_entities[i].ent.origin, cl_static_entities[i].ent.model->maxs, maxs);
		}
		else
		{
			VectorCopy(mins, cl_static_entities[i].ent.origin);
			VectorCopy(maxs, cl_static_entities[i].ent.origin);
		}
		if (cl.worldmodel && cl.worldmodel->loadstate == MLS_LOADED)
			cl.worldmodel->funcs.FindTouchedLeafs(cl.worldmodel, &cl_static_entities[i].ent.pvscache, mins, maxs);
		cl_static_entities[i].emit = trailkey_null;
	}

	CL_InitDlights();
#ifdef RTLIGHTS
	Sh_PreGenerateLights();
#endif
}

void Surf_PreNewMap(void)
{
	extern cvar_t gl_specular;

	r_loadbumpmapping = r_deluxemapping || r_glsl_offsetmapping.ival;
	r_loadbumpmapping |= gl_specular.value>0;
#ifdef RTLIGHTS
	r_loadbumpmapping |= r_shadow_realtime_world.ival || r_shadow_realtime_dlight.ival;
#endif
	r_viewcluster = -1;
	r_viewcluster2 = -1;
	surf_lastgoodworld = NULL;	//FTESurf P138: drop the void-PVS cache with the old world.

	shader_reload_why = "Surf_PreNewMap";
	Shader_DoReload();
}



static float sgn(float a)
{
    if (a > 0.0F) return (1.0F);
    if (a < 0.0F) return (-1.0F);
    return (0.0F);
}
void R_ObliqueNearClip(float *viewmat, mplane_t *wplane)
{
	float f;
	vec4_t q, c;
	vec3_t ping, pong;
	vec4_t vplane;

	//convert world plane into view space
	Matrix4x4_CM_Transform3x3(viewmat, wplane->normal, vplane);
	VectorScale(wplane->normal, wplane->dist, ping);
	Matrix4x4_CM_Transform3(viewmat, ping, pong);
	vplane[3] = -DotProduct(pong, vplane);

	// Calculate the clip-space corner point opposite the clipping plane
	// as (sgn(clipPlane.x), sgn(clipPlane.y), 1, 1) and
	// transform it into camera space by multiplying it
	// by the inverse of the projection matrix

	q[0] = (sgn(vplane[0]) + r_refdef.m_projection_std[8]) / r_refdef.m_projection_std[0];
	q[1] = (sgn(vplane[1]) + fabs(r_refdef.m_projection_std[9])) / fabs(r_refdef.m_projection_std[5]);
	q[2] = -1.0F;
	q[3] = (1.0F + r_refdef.m_projection_std[10]) / r_refdef.m_projection_std[14];

	// Calculate the scaled plane vector
	f = 2.0F / DotProduct4(vplane, q);
	Vector4Scale(vplane, f, c);

	// Replace the third row of the projection matrix
	r_refdef.m_projection_std[2] = c[0];
	r_refdef.m_projection_std[6] = c[1];
	r_refdef.m_projection_std[10] = c[2] + 1.0F;
	r_refdef.m_projection_std[14] = c[3];
}


#endif
