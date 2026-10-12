!!ver 110
!!permu FOG
!!permu FRAMEBLEND
!!permu SKELETAL
!!samps reflectcube

#include "sys/defs.h"

#ifndef ENVTINT
#define ENVTINT 1.0,1.0,1.0
#endif
#ifndef ENVSAT
#define ENVSAT 1.0,1.0,1.0
#endif
#ifndef ENVCONTRAST
#define ENVCONTRAST 0.0
#endif
#ifndef COLOR
#define COLOR 1.0,1.0,1.0
#endif
#ifndef ALPHA
#define ALPHA 1.0
#endif

// UnlitGeneric without a base texture: black albedo plus its reflection.
// Unlike WindowImposter, the surface normal determines the sample direction.
varying vec3 incident;
varying vec3 normal;
#if defined(VERTEXCOL) || defined(VERTEXALPHA)
varying vec4 vertex_colour;
#endif

#ifdef VERTEX_SHADER
#include "sys/skeletal.h"
void main()
{
	vec3 w, n, s, t;
	gl_Position = skeletaltransform_wnst(w, n, s, t);
	incident = w - e_eyepos;
	normal = n;
#if defined(VERTEXCOL) || defined(VERTEXALPHA)
	vertex_colour = v_colour;
#endif
}
#endif

#ifdef FRAGMENT_SHADER
#include "sys/fog.h"
void main()
{
	vec3 ray = reflect(incident, normalize(normal));
	ray = (m_model * vec4(ray, 0.0)).xyz;
	vec3 rgb = textureCube(s_reflectcube, ray).rgb;
	vec3 grey = vec3(dot(rgb, vec3(0.2126, 0.7152, 0.0722)));
	rgb = mix(grey, rgb, vec3(ENVSAT));
	rgb = mix(rgb, rgb * rgb, float(ENVCONTRAST));
	rgb *= vec3(ENVTINT) * vec3(COLOR) * e_colourident.rgb;
#ifdef VERTEXCOL
	rgb *= vertex_colour.rgb;
#endif
	vec4 result = vec4(rgb, float(ALPHA) * e_colourident.a);
#ifdef VERTEXALPHA
	result.a *= vertex_colour.a;
#endif
#ifdef NOFOG
	gl_FragColor = result;
#elif defined(ADDITIVE)
	gl_FragColor = fog4additive(result);
#else
	gl_FragColor = vec4(fog3(result.rgb), result.a);
#endif
}
#endif
