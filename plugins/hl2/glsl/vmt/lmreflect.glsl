!!ver 110
!!permu FOG
!!permu REFLECTCUBEMASK
!!samps refraction=0 reflection=1 bump=2
!!samps =REFLECTCUBEMASK reflectcube

#include "sys/defs.h"

// FTESurf Patch 1217: Source's LightmappedReflective (lightmappedreflective_ps20b.fxc) without its
// $basetexture combo: refraction * (1-F) + reflection * F, F = R + (1-R)(1-N.V)^5, both lookups
// moved by normal.xy * normal.a * amount.  Gamma space, like refract.glsl.
//   CUBEREFL  the nearest baked cubemap stands in for the reflection capture
//   BLENDED   no refraction capture either: blend the reflection over the framebuffer by F
// With CUBEREFL and no cubemap bound (hl2_envcubemap 0, a map with none) there is nothing to
// reflect: the refraction alone, or nothing at all when BLENDED, not a black mirror.

#ifndef REFRTINT
#define REFRTINT 1.0,1.0,1.0
#endif
#ifndef REFLTINT
#define REFLTINT 1.0,1.0,1.0
#endif
#ifndef REFLECTANCE
#define REFLECTANCE 0.25
#endif
#ifndef REFRAMOUNT
#define REFRAMOUNT 0.0
#endif
#ifndef REFLAMOUNT
#define REFLAMOUNT 0.8
#endif
#ifndef SCROLL
#define SCROLL 0.0,0.0
#endif

varying vec2 tex_c;
varying vec4 tf_c;
varying vec3 incident;
varying mat3 invsurface;

#ifdef VERTEX_SHADER
#include "sys/skeletal.h"
void main()
{
	vec3 w, n, s, t;
	gl_Position = tf_c = skeletaltransform_wnst(w, n, s, t);
	tex_c = v_texcoord + e_time * vec2(SCROLL);
	incident = w - e_eyepos;
	invsurface = mat3(s, t, n);
}
#endif

#ifdef FRAGMENT_SHADER
#include "sys/fog.h"
void main()
{
	vec4 nrm = texture2D(s_bump, tex_c);
	// Source's normal maps are DX: green is flipped (lightmapped.glsl).
	vec3 tn = normalize(vec3(nrm.r, 1.0 - nrm.g, nrm.b) * 2.0 - 1.0);
	vec3 wn = normalize(invsurface * tn);
#ifdef FTE_CAPTUREFIX
	vec4 ctf = m_capturefix * tf_c;	// FTESurf Patch 1245: where a reused capture saw this point
	if (ctf.w <= 0.5)
		ctf = tf_c;	// behind that camera: this frame's
	vec2 sc = (1.0 + ctf.xy / ctf.w) * 0.5;
#else
	vec2 sc = (1.0 + tf_c.xy / tf_c.w) * 0.5;
#endif
	float ndv = clamp(dot(normalize(-incident), wn), 0.0, 1.0);
	float f = float(REFLECTANCE) + (1.0 - float(REFLECTANCE)) * pow(1.0 - ndv, 5.0);
#if defined(CUBEREFL) && !defined(REFLECTCUBEMASK)
	f = 0.0;
	vec3 refl = vec3(0.0);
#elif defined(CUBEREFL)
	vec3 refl = textureCube(s_reflectcube, (m_model * vec4(reflect(incident, wn), 0.0)).xyz).rgb;
#else
	vec3 refl = texture2D(s_reflection, clamp(sc + tn.xy * (nrm.a * float(REFLAMOUNT)), 0.0, 1.0)).rgb;
#endif
	refl *= vec3(REFLTINT);
#ifdef BLENDED
	gl_FragColor = vec4(fog3(refl), f);
#else
	vec3 refr = texture2D(s_refraction, clamp(sc + tn.xy * (nrm.a * float(REFRAMOUNT)), 0.0, 1.0)).rgb * vec3(REFRTINT);
	gl_FragColor = vec4(fog3(mix(refr, refl, f)), 1.0);
#endif
}
#endif
