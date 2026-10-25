!!ver 110
!!permu FOG
!!permu FRAMEBLEND
!!permu SKELETAL
!!samps reflectcube
!!samps =BASE diffuse
!!samps =DETAILMASK upper
!!samps =ENVFROMMASK reflectmask

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
#ifndef SCROLL
#define SCROLL 0.0,0.0
#endif
#ifndef DETAILSCALE
#define DETAILSCALE 4.0
#endif
#ifndef DETAILSCROLL
#define DETAILSCROLL 0.0,0.0
#endif

// UnlitGeneric with an $envmap: base x $color plus the reflection (vertexlit_and_unlit_generic_ps2x.fxc
// :361-467). Without #BASE the base is Source's TEXTURE_BLACK (vertexlitgeneric_dx9_helper.cpp:938-941).
// $color never scales the reflection (Patch 990: bhop_ceaseless's $color2 {0 0 0} props drew black).
varying vec3 incident;
varying vec3 normal;
#ifdef BASE
varying vec2 tex_c;
#endif
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
#ifdef BASE
	tex_c = v_texcoord;
#endif
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
	float alpha = float(ALPHA);
#ifdef BASE
	vec4 base = texture2D(s_diffuse, tex_c + e_time * vec2(SCROLL));
#ifdef DETAILMASK
	//$detailblendmode 9, TCOMBINE_MASK_BASE_BY_DETAIL_ALPHA (common_ps_fxc.h:730)
	base.a *= texture2D(s_upper, (tex_c + e_time * vec2(DETAILSCROLL)) * float(DETAILSCALE)).a;	//BaseVSShader.cpp:351-357 scales the scroll too
#endif
#ifdef MASKLT
	if (base.a < float(MASK))
		discard;
#endif
#ifdef ENVFROMMASK
	rgb *= texture2D(s_reflectmask, tex_c).rgb;
#endif
#ifdef ENVFROMBASE
	rgb *= 1.0 - base.a;	//"this blows!" -- Source's own comment on the inverted mask
#endif
#endif
	vec3 grey = vec3(dot(rgb, vec3(0.2126, 0.7152, 0.0722)));
	rgb = mix(grey, rgb, vec3(ENVSAT));
	rgb = mix(rgb, rgb * rgb, float(ENVCONTRAST));
	rgb *= vec3(ENVTINT) * e_colourident.rgb;
#ifdef BASE
	base.rgb *= vec3(COLOR);
#ifdef VERTEXCOL
	base.rgb *= vertex_colour.rgb;
#endif
	rgb += base.rgb;
#ifndef ENVFROMBASE
	alpha *= base.a;
#endif
#elif defined(VERTEXCOL)
	rgb *= vertex_colour.rgb;
#endif
	vec4 result = vec4(rgb, alpha * e_colourident.a);
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
