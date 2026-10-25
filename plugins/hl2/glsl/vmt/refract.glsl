!!ver 110
!!permu FOG
!!permu FRAMEBLEND
!!permu SKELETAL
!!permu REFLECTCUBEMASK
!!samps refraction=0 tint=2
!!samps !ANIMBUMP bump=1
!!samps =ANIMBUMP bumpanim:2DArray=1
!!samps =REFLECTCUBEMASK reflectcube

#include "sys/defs.h"

// FTESurf Patch 866: hl2_refract 1, Source's refract_ps20b.fxc on the screen copy:
//   warp = normal.xy * normal.a * $refractamount (the old shader offset by the whole
//   normalized normal, so surf_garden's fountain, $refractamount .02, smeared the screen);
//   result = screen(warp) * $refracttint (* 2 tint texture) [+ $envmap term], alpha normal.a.
// Gamma-space like refractflat.glsl: 2x linear is 2^(1/2.2) here.

#ifndef REFRTINT
#define REFRTINT 1.0,1.0,1.0
#endif
#ifndef REFRAMOUNT
#define REFRAMOUNT 2.0
#endif
#ifndef SCROLL
#define SCROLL 0.0,0.0
#endif
//Patch 990: Source's Sine proxy on $refractamount (min, max, period), proxies.cpp CSineProxy
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
varying vec4 tf_c;
#ifdef REFLECTCUBEMASK
varying vec3 incident;
varying mat3 invsurface;
#endif

#ifdef VERTEX_SHADER
#include "sys/skeletal.h"
void main()
{
	vec3 w, n, s, t;
	gl_Position = tf_c = skeletaltransform_wnst(w, n, s, t);
	tex_c = v_texcoord + e_time * vec2(SCROLL);
#ifdef REFLECTCUBEMASK
	incident = w - e_eyepos;
	invsurface = mat3(s, t, n);
#endif
}
#endif

#ifdef FRAGMENT_SHADER
#include "sys/fog.h"
void main()
{
#ifdef ANIMBUMP
	// Patch 990: AnimatedTexture frames, whole frames at ANIMBUMP a second (vmt/refractanim, GLSL 130).
	int bumplayers = textureSize(s_bumpanim, 0).z;
	vec4 nrm = vec4(0.5, 0.5, 1.0, 1.0);	// flat while the array is absent: an incomplete texture reads 0 = full warp
	if (bumplayers > 0)
		nrm = texture2D(s_bumpanim, vec3(tex_c, floor(mod(e_time * float(ANIMBUMP), float(bumplayers)))));
#else
	vec4 nrm = texture2D(s_bump, tex_c);
#endif
#ifdef ALPHATEST
	if (nrm.a < 0.5)
		discard;
#endif
	// Source's normal maps are DX: green is flipped (lightmapped.glsl).
	vec3 tn = vec3(nrm.r, 1.0 - nrm.g, nrm.b) * 2.0 - 1.0;
	vec2 sc = (1.0 + tf_c.xy / tf_c.w) * 0.5;
#ifdef NOWARP
	vec2 rc = sc;	// no $normalmap: Source's DX9 Refract has nothing to warp by
#else
#ifdef REFRSINE
	vec3 rs = vec3(REFRSINE);
	float amount = mix(rs.x, rs.y, sin(6.2831853 * e_time / rs.z) * 0.5 + 0.5);
#else
	float amount = float(REFRAMOUNT);
#endif
	vec2 rc = sc + tn.xy * (nrm.a * amount);
#endif
#ifdef BLUR
	// refract_ps20b's BLUR 1: nine taps from four bilinear ones, 1/512 apart.
	const float bf = 1.0 / 512.0, hf = 0.5 / 512.0;
	vec3 rgb = texture2D(s_refraction, rc - vec2(hf, hf)).rgb * 0.4444444
	         + texture2D(s_refraction, rc + vec2(bf, -hf)).rgb * 0.2222222
	         + texture2D(s_refraction, rc + vec2(-hf, bf)).rgb * 0.2222222
	         + texture2D(s_refraction, rc + vec2(bf, bf)).rgb * 0.1111111;
#else
	vec3 rgb = texture2D(s_refraction, rc).rgb;
#endif
	vec3 tint = vec3(REFRTINT);
#ifdef TINTTEX
	tint *= 1.3703 * texture2D(s_tint, tex_c).rgb;
#endif
	rgb *= tint;
#ifdef REFLECTCUBEMASK
	vec3 ray = reflect(incident, normalize(invsurface * normalize(tn)));
	ray = (m_model * vec4(ray, 0.0)).xyz;
	vec3 spec = textureCube(s_reflectcube, ray).rgb * nrm.a * vec3(ENVTINT);
	spec = mix(spec, spec * spec, float(ENVCONTRAST));
	spec = mix(vec3(dot(spec, vec3(0.299, 0.587, 0.114))), spec, vec3(ENVSAT));
	rgb += spec;
#endif
#if defined(NOFOG)
	gl_FragColor = vec4(rgb, nrm.a);
#elif defined(ADDITIVE)
	gl_FragColor = vec4(fog3additive(rgb), nrm.a);	// additive fogs to black (BaseShader.cpp:775)
#else
	gl_FragColor = vec4(fog3(rgb), nrm.a);
#endif
#ifdef OPAQUE
	gl_FragColor.a = 1.0;
#endif
}
#endif
