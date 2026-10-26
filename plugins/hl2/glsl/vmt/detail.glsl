!!ver 110
!!permu FOG
!!samps diffuse

#include "sys/defs.h"

//FTESurf Patch 1038: Source detail props (detailobjectsystem.cpp), one rigid mesh per cell built by
//mod_vbsp.c VBSP_DetailBuild.  v_normal carries the billboard: x along the viewer's horizontal right,
//y up, z 1 when fully screen-aligned (orientation 1); 0,0,0 for a sprite or shape the map fixed in
//place, whose corner is already in v_position.
//MAXD/FADE are cl_detaildist/cl_detailfade after env_detail_controller: alpha falls off in squared
//distance from (MAXD-FADE)^2 to MAXD^2 (SortSpritesBackToFront).  Cut by a dither instead of blended,
//so the cells need no back-to-front sort; the texture's own alpha is tested at MASK.
#ifndef MAXD
#define MAXD 1200.0
#endif
#ifndef FADE
#define FADE 400.0
#endif
#ifndef MASK
#define MASK 0.5
#endif

#include "sys/fog.h"

varying vec2 tex_c;
varying vec4 vex_color;

#ifdef VERTEX_SHADER
void main ()
{
	vec3 pos = v_position.xyz;
	vec3 d = e_eyepos - pos;	//the cell entity sits at the origin, so model space is world space
	if (v_normal.x != 0.0 || v_normal.y != 0.0)
	{
		float hl = length(d.xy);
		vec3 right = (hl > 0.001) ? vec3(-d.y, d.x, 0.0) / hl : vec3(0.0, 1.0, 0.0);
		vec3 up = vec3(0.0, 0.0, 1.0);
		if (v_normal.z > 0.5)
			up = normalize(cross(normalize(d), right));
		pos += right * v_normal.x + up * v_normal.y;
	}
	float far2 = float(MAXD) * float(MAXD);
	float near = max(float(MAXD) - float(FADE), 0.0);
	vex_color = v_colour;
	vex_color.a = clamp((far2 - dot(d, d)) / max(far2 - near * near, 1.0), 0.0, 1.0);
	tex_c = v_texcoord;
	gl_Position = m_modelviewprojection * vec4(pos, 1.0);
}
#endif

#ifdef FRAGMENT_SHADER
void main ()
{
	vec4 c = texture2D(s_diffuse, tex_c);
	if (c.a < float(MASK))
		discard;
	//interleaved-gradient noise as the fade's threshold
	if (vex_color.a < 1.0 && vex_color.a <= fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))))
		discard;
	gl_FragColor = fog4(vec4(c.rgb * vex_color.rgb, 1.0));
}
#endif
