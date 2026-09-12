!!ver 110
!!permu FOG
!!samps diffuse=0

#include "sys/defs.h"
#include "sys/fog.h"

varying vec2 tex_c;

#ifdef VERTEX_SHADER
void main ()
{
	tex_c = v_texcoord;
	gl_Position = ftetransform();
}
#endif

#ifdef FRAGMENT_SHADER
void main ()
{
	vec4 diffuse_f = texture2D( s_diffuse, fract(tex_c) );
	//FTESurf Patch 299: fog the colour, leave the alpha to the blender.
	//fog4() multiplies by regularcolour.a, which on a Source material is a
	//MASK ($basealphaenvmapmask and friends), not opacity.  Reasoning and
	//measurements in vertexlit.glsl.  hl2_fog_alphamul 1 restores fog4().
	#if #include "cvar/hl2_fog_alphamul"
		gl_FragColor = fog4( diffuse_f );
	#else
		gl_FragColor = vec4(fog3(diffuse_f.rgb), diffuse_f.a);
	#endif
}
#endif
