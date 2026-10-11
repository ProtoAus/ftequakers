!!permu FOG
!!samps 1
//used by both particles and sprites.
//note the fog blending mode is all that differs from defaultadditivesprite

#include "sys/defs.h"
#include "sys/fog.h"
varying vec2 tc;
varying vec4 vc;
#ifdef VERTEX_SHADER
void main ()
{
	tc = v_texcoord;
	vc = v_colour;
	gl_Position = ftetransform();
}
#endif
#ifdef FRAGMENT_SHADER
uniform vec4 e_vlscale;
void main ()
{
	vec4 col = texture2D(s_t0, tc);
#ifdef MASK
	if (col.a < float(MASK))
		discard;
#endif
	col = fog4blend(col * vc * e_colourident * e_vlscale);
//FTESurf Patch 658: the GL renderer drew the unlitdecal bucket twice a frame, and every effect
//in it was tuned by eye over that.  Drawn once, these write what two blends at col left:
//a*(2-a) is exact for an alpha factor, c*(2-c) for a colour one.
#ifdef TWICE
	col.a = clamp(col.a, 0.0, 1.0);
	col.a *= 2.0 - col.a;
#endif
#ifdef TWICEC
	col.rgb = clamp(col.rgb, 0.0, 1.0);
	col.rgb *= 2.0 - col.rgb;
#endif
	gl_FragColor = col;
}
#endif
