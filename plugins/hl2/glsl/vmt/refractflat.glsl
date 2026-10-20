!!ver 110
!!permu FOG
!!permu FRAMEBLEND
!!permu SKELETAL
!!permu REFLECTCUBEMASK
!!samps bump=0 tint=1
!!samps =ENVPASS reflectcube

#include "sys/defs.h"

// FTESurf Patch 859: hl2_refract 0, Source's Refract (refract_ps20b.fxc) without the
// framebuffer copy, so without the warp.  Two passes:
//   default: screen *= mix(1, REFRTINT * tint, normal.a), drawn with GL_DST_COLOR
//            GL_SRC_COLOR (2*src*dst), so the output is half the factor.
//   ENVPASS: Source's cubemap term, added.  Gamma-space, as lightmapped.glsl's.  Only
//            under REFLECTCUBEMASK, the engine's gate (r_reflectcube, r_envcubemap).
// #TINTTEX: Source's 2*tex is in linear light; 2^(1/2.2) is the same factor here.
// #ADDITIVE: $additive without $envmap adds the result to the screen: 1 + f*a, in linear light.
// #NOCOVER: Source does not blend this pane by the alpha (opaque, or additive ONE ONE; VMT_RefractBlend).

#ifndef REFRTINT
#define REFRTINT 1.0,1.0,1.0
#endif
#ifndef SCROLL
#define SCROLL 0.0,0.0
#endif
#ifndef ENVTINT
#define ENVTINT 1.0,1.0,1.0
#endif
#ifndef ENVSAT
#define ENVSAT 1.0,1.0,1.0
#endif
#ifndef ENVCONTRAST
#define ENVCONTRAST 0.0
#endif

varying vec2 tex_c;
#ifdef ENVPASS
varying vec3 incident;
varying mat3 invsurface;
#endif

#ifdef VERTEX_SHADER
#include "sys/skeletal.h"
void main()
{
	vec3 w, n, s, t;
	gl_Position = skeletaltransform_wnst(w, n, s, t);
	tex_c = v_texcoord + e_time * vec2(SCROLL);
#ifdef ENVPASS
	incident = w - e_eyepos;
	invsurface = mat3(s, t, n);
#endif
}
#endif

#ifdef FRAGMENT_SHADER
#include "sys/fog.h"
void main()
{
	vec4 nrm = texture2D(s_bump, tex_c);
#ifdef ALPHATEST
	if (nrm.a < 0.5)
		discard;
#endif
#ifdef ENVPASS
#ifndef REFLECTCUBEMASK
	discard;
#endif
	// Source's normal maps are DX: green is flipped (lightmapped.glsl).
	vec3 tn = normalize(vec3(nrm.r, 1.0 - nrm.g, nrm.b) - 0.5);
	vec3 ray = reflect(incident, normalize(invsurface * tn));
	ray = (m_model * vec4(ray, 0.0)).xyz;
	vec3 spec = textureCube(s_reflectcube, ray).rgb * nrm.a * vec3(ENVTINT);
	spec = mix(spec, spec * spec, float(ENVCONTRAST));
	spec = mix(vec3(dot(spec, vec3(0.299, 0.587, 0.114))), spec, vec3(ENVSAT));
#ifndef NOFOG
	spec = fog3additive(spec);
#endif
	gl_FragColor = vec4(spec, 1.0);
#else
	vec3 f = vec3(REFRTINT);
#ifdef TINTTEX
	f *= 1.3703 * texture2D(s_tint, tex_c).rgb;
#endif
	float cov = nrm.a;	// no $envmap: the alpha is coverage (refract_dx9_helper.cpp:143)
#if defined(ENVMAPPED) || defined(NOCOVER)
	cov = 1.0;
#endif
#ifdef ADDITIVE
	f = pow(vec3(1.0) + pow(f, vec3(2.2)) * cov, vec3(1.0 / 2.2));	// Source adds in linear light
#else
	f = mix(vec3(1.0), f, cov);
#endif
#ifndef NOFOG
	f = mix(vec3(1.0), f, fog3additive(vec3(1.0)).r);
#endif
	gl_FragColor = vec4(f * 0.5, 1.0);
#endif
}
#endif
