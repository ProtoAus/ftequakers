!!ver 110
!!permu FOG
!!permu BUMP
!!permu REFLECTCUBEMASK
!!samps =BUMP normalmap
!!samps =REFLECTCUBEMASK reflectcube

#include "sys/defs.h"

// Capture-free approximation: no scene/depth samplers, no screen-space warp.
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
	vec3 n = texture2D(s_normalmap, ntc).xyz * 2.0 - 1.0;
#ifdef MULTITEXTURE
	vec2 tc1 = vec2(ntc.x + ntc.y, ntc.y - ntc.x) * 0.1;
	vec3 n1 = texture2D(s_normalmap, tc1 + e_time * vec2(SCROLL1)).xyz * 2.0 - 1.0;
	vec3 n2 = texture2D(s_normalmap, ntc.yx * 0.45 + e_time * vec2(SCROLL2)).xyz * 2.0 - 1.0;
	n = (n + n1 + n2) / 3.0;
#endif
	normal = normalize(tangenttoworld * normalize(n));
#endif
	vec3 viewdir = normalize(eye);
	if (dot(normal, viewdir) < 0.0)
		normal = -normal;
	float fresnel = 0.05 + 0.95 * pow(1.0 - clamp(dot(normal, viewdir), 0.0, 1.0), 5.0);
	// Mild surface shading keeps normals visible even without a loaded cubemap.
	float shade = 0.9 + 0.1 * max(dot(normal, normalize(vec3(0.3,-0.4,0.85))), 0.0);
	vec3 colour = vec3(FOGTINT) * shade;
	float alpha = float(ALPHA);
#ifdef REFLECTCUBEMASK
	vec3 reflected = textureCube(s_reflectcube, reflect(-viewdir, normal)).rgb * vec3(TINT_REFL);
	colour = mix(colour, reflected, fresnel);
	alpha = mix(alpha, 1.0, fresnel);
#endif
	// Scene fog affects colour, not the authored/translucent coverage.
	gl_FragColor = vec4(fog3(colour), alpha);
}
#endif
