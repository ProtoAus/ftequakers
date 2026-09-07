!!ver 110
!!permu FOG
!!samps diffuse

#include "sys/defs.h"

//ftesurf (P187): $alpha as a compile-time constant.  e_colourident cannot carry
//it -- that is the entity's alpha, one value per entity -- and these materials
//emit a top-level `program` with no pass, so `alphagen const` has nowhere to
//go.  Applied LAST, after any envmap block, because $basealphaenvmapmask reads
//the texture's own alpha and not the material's opacity.
#ifndef ALPHA
#define ALPHA 1.0
#endif


//ftesurf (P189): the TextureScroll proxy, as a constant velocity in texture
//units per second.  Same idiom as defaultwall.glsl's FLOWV.
#ifndef SCROLL
#define SCROLL 0.0,0.0
#endif


//ftesurf (P188): $color / $color2, the per-material tint.  Source multiplies the
//albedo by it; e_colourident cannot carry it because that is the entity's
//colormod, one value for a whole entity, and this is per-material.  Not clamped
//-- values above 1 are authored deliberately.
#ifndef COLOR
#define COLOR 1.0,1.0,1.0
#endif

#include "sys/fog.h"

varying vec2 tex_c;

#ifdef VERTEX_SHADER
void main ()
{
	tex_c = v_texcoord + e_time * vec2(SCROLL);
	gl_Position = ftetransform();
}
#endif

#ifdef FRAGMENT_SHADER
void main ()
{
	vec4 diffuse_f = texture2D( s_diffuse, tex_c );
	diffuse_f.rgb *= vec3(COLOR);

#ifdef MASKLT
		if (diffuse_f.a < float(MASK))
			discard;
#endif

	diffuse_f.a *= float(ALPHA);
	gl_FragColor = fog4( diffuse_f );
}
#endif
