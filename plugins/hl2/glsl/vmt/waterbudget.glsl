!!ver 110
!!permu FOG
!!permu BUMP
!!permu REFLECTCUBEMASK
!!samps =BUMP normalmap
!!samps =REFLECTCUBEMASK reflectcube
!!samps =CHEAPANIM bumpanim:2DArray=0

#include "sys/defs.h"

// Capture-free approximation: no scene/depth samplers, no screen-space warp.
// FTESurf Patch 879: CHEAPOPAQUE ($forcecheap) also moves as Source's cheap water does: $bumptransform's
// scroll on the primary layer, and P867's AnimatedTexture frames in vmt/waterbudgetanim (this file under
// GLSL 130, built by the Makefile) -- a frame array that did not load falls back to the normal map.
#ifndef BUMPSCROLL
#define BUMPSCROLL 0.0,0.0
#endif
#ifdef CHEAPANIM
#define CHEAPBUMP(uv) (bumplayers > 0 ? texture2D(s_bumpanim, vec3(uv, bumplayer)) : texture2D(s_normalmap, uv))
#else
#define CHEAPBUMP(uv) texture2D(s_normalmap, uv)
#endif
#ifndef FOGTINT
#define FOGTINT 0.1,0.2,0.3
#endif
#ifndef TINT_REFL
#define TINT_REFL 1.0,1.0,1.0
#endif
#ifndef TXSCALE1
#define TXSCALE1 1.0
#endif
#ifndef SCROLL1
#define SCROLL1 0.0,0.0
#endif
#ifndef SCROLL2
#define SCROLL2 0.0,0.0
#endif
#ifndef ALPHA
#define ALPHA 0.65
#endif

varying vec2 tc;
varying vec3 eye;
varying vec3 geometricnormal;
#ifdef BUMP
varying mat3 tangenttoworld;
#endif

#ifdef VERTEX_SHADER
void main ()
{
	gl_Position = ftetransform();
	tc = v_texcoord.st;
	eye = (m_model * vec4(e_eyepos - v_position.xyz, 0.0)).xyz;
	geometricnormal = normalize((m_model * vec4(v_normal, 0.0)).xyz);
#ifdef BUMP
	// FTE stores the opposite Source T direction; preserve normal-map handedness.
	tangenttoworld = mat3(
		normalize((m_model * vec4(v_svector, 0.0)).xyz),
		-normalize((m_model * vec4(v_tvector, 0.0)).xyz),
		geometricnormal);
#endif
}
#endif

#ifdef FRAGMENT_SHADER
#include "sys/fog.h"
void main ()
{
	vec3 normal = normalize(geometricnormal);
#ifdef BUMP
	vec2 ntc = tc * float(TXSCALE1);
#ifdef CHEAPANIM
	int bumplayers = textureSize(s_bumpanim, 0).z;
	float bumplayer = bumplayers > 0 ? floor(mod(e_time * float(CHEAPANIM), float(bumplayers))) : 0.0;
#endif
#ifdef CHEAPOPAQUE
	vec3 n = CHEAPBUMP(ntc + e_time * vec2(BUMPSCROLL)).xyz * 2.0 - 1.0;
#else
	vec3 n = texture2D(s_normalmap, ntc).xyz * 2.0 - 1.0;
#endif
#ifdef MULTITEXTURE
	vec2 tc1 = vec2(ntc.x + ntc.y, ntc.y - ntc.x) * 0.1;
	vec3 n1 = CHEAPBUMP(tc1 + e_time * vec2(SCROLL1)).xyz * 2.0 - 1.0;
	vec3 n2 = CHEAPBUMP(ntc.yx * 0.45 + e_time * vec2(SCROLL2)).xyz * 2.0 - 1.0;
	n = (n + n1 + n2) / 3.0;
#endif
	normal = normalize(tangenttoworld * normalize(n));
#endif
	vec3 viewdir = normalize(eye);
	if (dot(normal, viewdir) < 0.0)
		normal = -normal;
#ifdef CHEAPOPAQUE
	//$forcecheap: watercheap_ps20b with BLEND 0 -- opaque, fog colour to the cubemap by (1-n.e)^5
	//(its FRESNEL combo), no refraction.
	vec3 cheap = vec3(FOGTINT);
#ifdef REFLECTCUBEMASK
#ifdef NOFRESNEL
	float f = float(REFLECTBLEND);	//$nofresnel: the FRESNEL combo off mixes by $reflectblendfactor
#else
	float f = pow(1.0 - clamp(dot(normal, viewdir), 0.0, 1.0), 5.0);
#endif
	cheap = mix(cheap, textureCube(s_reflectcube, reflect(-viewdir, normal)).rgb * vec3(TINT_REFL), f);
#endif
	gl_FragColor = vec4(fog3(cheap), 1.0);
	return;
#endif
	float fresnel = 0.05 + 0.95 * pow(1.0 - clamp(dot(normal, viewdir), 0.0, 1.0), 5.0);
	// Mild surface shading keeps normals visible even without a loaded cubemap.
	float shade = 0.9 + 0.1 * max(dot(normal, normalize(vec3(0.3,-0.4,0.85))), 0.0);
	vec3 colour = vec3(FOGTINT) * shade;
	float alpha = float(ALPHA);
#ifdef REFLECTCUBEMASK
	vec3 reflected = textureCube(s_reflectcube, reflect(-viewdir, normal)).rgb * vec3(TINT_REFL);
	//A baked cube can have dark faces unrelated to the live sky. Retain
	//water body colour at grazing angles instead of becoming a black rim.
	colour = mix(colour, reflected, 0.65 * fresnel);
	alpha = mix(alpha, 0.90, fresnel);
#endif
	// Scene fog affects colour, not the authored/translucent coverage.
	gl_FragColor = vec4(fog3(colour), alpha);
}
#endif
