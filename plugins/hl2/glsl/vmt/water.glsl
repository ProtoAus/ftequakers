!!cvardf r_glsl_turbscale_reflect=1	//simpler scaler
!!cvardf r_glsl_turbscale_refract=1	//simpler scaler
!!permu REFLECTCUBEMASK
!!samps diffuse normalmap
!!samps	refract=0	//always present
!!samps reflect=1
!!samps =DEPTH refractdepth=2
!!samps =REFLECTCUBEMASK reflectcube
!!permu FOG

#include "sys/defs.h"

//modifier: REFLECT		(s_t2 is a reflection instead of diffusemap)
//modifier: STRENGTH_REFL	(distortion strength - 0.1 = fairly gentle, 0.2 = big waves)
//modifier: STRENGTH_REFL	(distortion strength - 0.1 = fairly gentle, 0.2 = big waves)
//modifier: FRESNEL_EXP	(5=water)
//modifier: TXSCALE1		(normal UV tiling, not animation speed)
//modifier: MULTITEXTURE	(Source's extra rotated/scaled normal layers)
//modifier: SCROLL1/SCROLL2	(signed UV units per second)
//modifier: RIPPLEMAP		(s_t3 contains a ripplemap
//modifier: TINT_REFR		(some colour value)
//modifier: TINT_REFL		(some colour value)
//modifier: ALPHA		(mix in the normal water texture over the top)
//modifier: USEMODS		(use single-texture scrolling via tcmods - note, also forces the engine to actually use tcmod etc)

//a few notes on DP compat:
//'dpwater' makes numerous assumptions about DP internals
//by default there is a single pass that uses the pass's normal tcmods
//the fresnel has a user-supplied min+max rather than an exponent
//both parts are tinted individually
//if alpha is enabled, the regular water texture is blended over the top, again using the same crappy tcmods...

//legacy crap
#ifndef FRESNEL
#define FRESNEL 5.0
#endif
#ifndef TINT
#define TINT 0.7,0.8,0.7
#endif
#ifndef STRENGTH
#define STRENGTH 0.25
#endif
#ifndef TXSCALE
#define TXSCALE 1
#endif

//current values (referring to legacy defaults where needed)
#ifndef FRESNEL_EXP
#define FRESNEL_EXP 5.0
#endif
#ifndef FRESNEL_MIN
#define FRESNEL_MIN 0.0
#endif
#ifndef FRESNEL_RANGE
#define FRESNEL_RANGE 1.0
#endif
#ifndef STRENGTH_REFL
#define STRENGTH_REFL STRENGTH
#endif
#ifndef STRENGTH_REFR
#define STRENGTH_REFR STRENGTH
#endif
#ifndef TXSCALE1
#define TXSCALE1 TXSCALE
#endif
#ifndef SCROLL1
#define SCROLL1 0.0,0.0
#endif
#ifndef SCROLL2
#define SCROLL2 0.0,0.0
#endif
#ifndef TINT_REFR
#define TINT_REFR TINT
#endif
#ifndef TINT_REFL
#define TINT_REFL 1.0,1.0,1.0
#endif
#ifndef FOGTINT
#define FOGTINT 0.2,0.3,0.2
#endif
#ifndef FOGSTART
#define FOGSTART 0.0
#endif
#ifndef FOGRANGE
#define FOGRANGE 1024.0
#endif

varying vec2 tc;
varying vec4 tf;
varying vec3 norm;
varying vec3 eye;
varying mat3 tangenttoworld;

#ifdef VERTEX_SHADER
void main (void)
{
	tc = v_texcoord.st;
	tf = ftetransform();
	norm = normalize((m_model * vec4(v_normal, 0.0)).xyz);
	eye = (m_model * vec4(e_eyepos - v_position.xyz, 0.0)).xyz;
	// VBSP negates the texture V axis when building its T attribute.
	// Undo that convention for Source tangent-space RGB normals.
	tangenttoworld = mat3(
		normalize((m_model * vec4(v_svector, 0.0)).xyz),
		normalize((m_model * vec4(-v_tvector, 0.0)).xyz),
		norm);
	gl_Position = tf;
}
#endif

#ifdef FRAGMENT_SHADER
#include "sys/fog.h"

#ifdef DEPTH
// The depth capture uses the same non-oblique projection as the surface.
// Use the matrix, not assumed near/far cvars (including infinite far planes).
float eyeDepth(float z)
{
	float ndc = z * 2.0 - 1.0;
	return abs(m_projection[3][2] / max(ndc * m_projection[2][3] - m_projection[2][2], 0.000001));
}

float waterDepth(vec2 uv, float surfaceDepth)
{
	return max(0.0, eyeDepth(texture2D(s_refractdepth, uv).r) - surfaceDepth);
}
#endif

void main (void)
{
	vec2 stc;	//screen tex coords
	vec2 ntc;	//normalmap/diffuse tex coords
	vec3 n, refr, refl;
	float fres;
	float depth = 0.0;
	stc = (1.0 + (tf.xy / tf.w)) * 0.5;

	ntc = tc * float(TXSCALE1);
	// Source water_vs20 / watercheap_ps20b: decode RGB normally, without
	// the old +4/256 Z/XY bias. A neutral normal must remain surface-normal.
	n = texture2D(s_normalmap, ntc).xyz * 2.0 - 1.0;
#ifdef MULTITEXTURE
	// Source water_vs20 / watercheap_vs20 extra layers, including scale/axis swap.
	vec2 tc1 = vec2(ntc.x + ntc.y, ntc.y - ntc.x) * 0.1;
	vec3 n1 = texture2D(s_normalmap, tc1 + e_time * vec2(SCROLL1)).xyz * 2.0 - 1.0;
	vec3 n2 = texture2D(s_normalmap, ntc.yx * 0.45 + e_time * vec2(SCROLL2)).xyz * 2.0 - 1.0;
	n = (n + n1 + n2) / 3.0;
#endif

#ifdef RIPPLEMAP
	n += texture2D(s_ripplemap, stc).rgb*3.0;
#endif
	n = normalize(n);

	// Fresnel and cube lookup must use the SAME world-space normal/view.
	vec3 worldnormal = normalize(tangenttoworld * n);
	vec3 viewdir = normalize(eye);
	fres = pow(1.0-clamp(dot(worldnormal, viewdir), 0.0, 1.0), float(FRESNEL_EXP)) * float(FRESNEL_RANGE) + float(FRESNEL_MIN);

#ifdef DEPTH
	float surfaceDepth = eyeDepth(gl_FragCoord.z);
	depth = waterDepth(stc, surfaceDepth);
	// Source attenuates distortion by the refraction capture's fog-depth.
	// Do not attenuate the unit normal used for Fresnel/cubemap reflection.
	float distortion = clamp(depth / float(FOGRANGE), 0.0, 1.0);
#else
	float distortion = 1.0;
#endif


	//refraction image (and water fog, if possible)
	vec2 refractUV = stc + n.st * float(STRENGTH_REFR) * float(r_glsl_turbscale_refract) * distortion;
#ifdef DEPTH
	// Reject foreground/above-water distortion instead of pulling pillars
	// across the shoreline. Capture and surface share screen depth.
	if (any(lessThan(refractUV, vec2(0.0))) || any(greaterThan(refractUV, vec2(1.0))) ||
		texture2D(s_refractdepth, clamp(refractUV, 0.0, 1.0)).r <= gl_FragCoord.z)
		refractUV = stc;
	depth = waterDepth(clamp(refractUV, 0.0, 1.0), surfaceDepth);
#endif
	refr = texture2D(s_refract, clamp(refractUV, 0.0, 1.0)).rgb * vec3(TINT_REFR);
#if defined(DEPTH) && !defined(NO_WATERFOG) && !defined(UNDERWATER)
	refr = mix(refr, vec3(FOGTINT), clamp((depth - float(FOGSTART)) / float(FOGRANGE), 0.0, 1.0));
#endif

#ifdef LQWATER
#ifdef REFLECTCUBEMASK
	refl = textureCube(s_reflectcube, reflect(-viewdir, worldnormal)).rgb * vec3(TINT_REFL);
#else
	refl = vec3(FOGTINT);
#endif
	//Baked reflection is only an approximation. Preserve water body colour
	//at the horizon; full Fresnel replacement is reserved for live reflection.
	fres *= 0.65;
#else
	refl = texture2D(s_reflect, clamp(stc - n.st * float(STRENGTH_REFL) * float(r_glsl_turbscale_reflect) * distortion, 0.0, 1.0)).rgb * vec3(TINT_REFL);
#endif

	//interplate by fresnel
	refr = mix(refr, refl, fres);

#ifdef ALPHA
	vec4 ts = texture2D(s_diffuse, ntc);
	vec4 surf = fog4blend(vec4(ts.rgb, float(ALPHA)*ts.a));
	refr = mix(refr, surf.rgb, surf.a);
#else
	refr = fog3(refr);	
#endif

	//done
	gl_FragColor = vec4(refr, 1.0);
}
#endif
