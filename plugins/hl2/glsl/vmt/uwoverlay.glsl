!!ver 110
!!samps screen=0 bump=1

#include "sys/defs.h"

// FTESurf Patch 1013: Source's $underwateroverlay, a Refract material over the whole screen while
// the eye is in that water, drawn as the post-process in place of underwaterwarp. refract.glsl's
// warp, blur and tint (Patch 866), sampling the scene ($sourcecolour) instead of a screen copy.

#ifndef REFRTINT
#define REFRTINT 1.0,1.0,1.0
#endif
#ifndef REFRAMOUNT
#define REFRAMOUNT 2.0
#endif
#ifndef SCROLL
#define SCROLL 0.0,0.0
#endif

varying vec2 stc;
varying vec2 btc;

#ifdef VERTEX_SHADER
void main()
{
	stc = vec2(v_texcoord.x, 1.0 - v_texcoord.y);	// $sourcecolour is an FBO: other way up (ccorrect.glsl)
	btc = v_texcoord + e_time * vec2(SCROLL);
	gl_Position = ftetransform();
}
#endif

#ifdef FRAGMENT_SHADER
uniform vec4 e_rendertexturescale;
void main()
{
	vec4 nrm = texture2D(s_bump, btc);
	vec3 tn = vec3(nrm.r, 1.0 - nrm.g, nrm.b) * 2.0 - 1.0;	// DX normal map: green flipped
#ifdef REFRSINE
	vec3 rs = vec3(REFRSINE);
	float amount = mix(rs.x, rs.y, sin(6.2831853 * e_time / rs.z) * 0.5 + 0.5);
#else
	float amount = float(REFRAMOUNT);
#endif
	vec2 rc = (stc + tn.xy * (nrm.a * amount)) * e_rendertexturescale.st;
#ifdef BLUR
	const float bf = 1.0 / 512.0, hf = 0.5 / 512.0;
	vec3 rgb = texture2D(s_screen, rc - vec2(hf, hf)).rgb * 0.4444444
	         + texture2D(s_screen, rc + vec2(bf, -hf)).rgb * 0.2222222
	         + texture2D(s_screen, rc + vec2(-hf, bf)).rgb * 0.2222222
	         + texture2D(s_screen, rc + vec2(bf, bf)).rgb * 0.1111111;
#else
	vec3 rgb = texture2D(s_screen, rc).rgb;
#endif
	gl_FragColor = vec4(rgb * vec3(REFRTINT), 1.0);
}
#endif
