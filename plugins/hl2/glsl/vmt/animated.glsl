!!ver 130-450
!!permu FOG
!!permu LIGHTSTYLED
!!permu NOFOG
!!samps anim:2DArray=0

!!samps !UNLIT lightmap
!!samps !UNLIT =LIGHTSTYLED lightmap1 lightmap2 lightmap3

// ftesurf (P195): the AnimatedTexture proxy -- a flipbook whose frames all live
// in ONE VTF.  3,342 materials across 741 maps drive one, and every one of them
// has always rendered as frame 0, because img_vtf.c read
// `frames = 1;//vtf->numframes;`.
//
// WHY THIS IS ITS OWN SHADER rather than a permutation of vmt/lightmapped.
// Sampling a flipbook needs sampler2DArray, and that needs GLSL 130; the four
// vmt shaders are `!!ver 110` and raising them would move every permutation of
// every Source material onto a newer context to serve one feature.  A separate
// file is selected per material by mat_vmt.c, exactly the way vmt/transition is,
// so a map with no animated materials compiles none of this.
//
// THE FRAME COUNT is read from the texture with textureSize rather than passed
// in as a define, so no permutation is spent per frame count and the plugin does
// not have to open the VTF to write the shader.  Same idiom as the engine's own
// default2danim.glsl, which is the shipped precedent for an array sampler here.
//
// The frames of one mip level are contiguous in a VTF, which is exactly the
// PTI_2D_ARRAY layout, so the loader change is a reinterpretation and not a
// decode.  It is opt-in on IF_TEXTYPE_2D_ARRAY, which the shader script spells
// `map "$2darray:name"` -- so this shader asking is what makes the frames appear.
//
// Deliberately NARROW: no cubemaps, no bumpmapping, no fake shadows.  A flipbook
// is smoke, fire, a screen or water, and none of those want an envmap.  Keeping
// the permutation set small is also what keeps this from being a second copy of
// vmt/lightmapped that has to be maintained beside it.
//
// #UNLIT, and why this shader cannot just always sample the lightmap.
// UnlitGeneric is 456 of the 927 materials a `hl2_animated 1` actually reroutes
// -- 49% of them -- and mat_vmt.c's own UnlitGeneric arm (:1979) emits a plain
// PASS with no program and no lightmap, because unlit means unlit.  Sampling one
// here would not merely be wasted work: `!!samps lightmap` sets
// prog->defaulttextures |= S_LIGHTMAP0, Shader_Finish then sets
// SHADER_HASLIGHTMAP, and Mod_LightmapAllocSurf gates on exactly that -- so the
// surface is GIVEN a real lightmap page with the map's real lighting in it, and
// every animated sign, screen and light panel in the library would be multiplied
// by the room's shadows and go dark.  It would have looked like the flipbook was
// "too dim" rather than like a lighting bug.
//
// So the lightmap is permutation-gated at the DECLARATION with `!UNLIT`
// (gl_shader.c:2137-2143 -- a leading '!' means "only when this permutation is
// not set"), not merely skipped in the fragment: that is what keeps
// SHADER_HASLIGHTMAP off and stops the page being allocated at all.

#include "sys/defs.h"

#ifndef ANIMRATE
#define ANIMRATE 10.0
#endif

#ifndef ALPHA
#define ALPHA 1.0
#endif

#ifndef SCROLL
#define SCROLL 0.0,0.0
#endif

#ifndef COLOR
#define COLOR 1.0,1.0,1.0
#endif

varying vec2 tex_c;

#ifndef UNLIT
varying vec2 lm0;
#ifdef LIGHTSTYLED
varying vec2 lm1, lm2, lm3;
#endif
#endif

#ifdef VERTEX_SHADER
	void main ()
	{
	#ifndef UNLIT
		lm0 = v_lmcoord;
	#ifdef LIGHTSTYLED
		lm1 = v_lmcoord2;
		lm2 = v_lmcoord3;
		lm3 = v_lmcoord4;
	#endif
	#endif
		tex_c = v_texcoord + e_time * vec2(SCROLL);
		gl_Position = ftetransform();
	}
#endif

#ifdef FRAGMENT_SHADER
	#include "sys/fog.h"

	#ifndef UNLIT
	#ifdef LIGHTSTYLED
		#define LIGHTMAP0 texture2D(s_lightmap0, lm0).rgb
		#define LIGHTMAP1 texture2D(s_lightmap1, lm1).rgb
		#define LIGHTMAP2 texture2D(s_lightmap2, lm2).rgb
		#define LIGHTMAP3 texture2D(s_lightmap3, lm3).rgb
	#else
		#define LIGHTMAP texture2D(s_lightmap, lm0).rgb
	#endif

	vec3 lightmap_fragment()
	{
		vec3 lightmaps;
#ifdef LIGHTSTYLED
		lightmaps  = LIGHTMAP0 * e_lmscale[0].rgb;
		lightmaps += LIGHTMAP1 * e_lmscale[1].rgb;
		lightmaps += LIGHTMAP2 * e_lmscale[2].rgb;
		lightmaps += LIGHTMAP3 * e_lmscale[3].rgb;
#else
		lightmaps  = LIGHTMAP * e_lmscale.rgb;
#endif
		return lightmaps;
	}
	#endif

	void main (void)
	{
		vec4 diffuse_f;

		// which frame.  sz.z is the layer count, so the modulo keeps the index
		// inside [0, frames) and the flipbook wraps on its own without the CPU
		// having to know how long the loop is.
		ivec3 sz = textureSize(s_anim, 0);
		float layer = mod(e_time * float(ANIMRATE), float(sz.z));

		diffuse_f = texture2D(s_anim, vec3(tex_c, layer));
		diffuse_f.rgb *= e_colourident.rgb * vec3(COLOR);

#ifdef MASKLT
		if (diffuse_f.a < float(MASK))
			discard;
#endif

	#ifndef UNLIT
		diffuse_f.rgb *= lightmap_fragment();
	#endif
		diffuse_f.a *= float(ALPHA);

	#ifdef NOFOG
		gl_FragColor = diffuse_f;
	#else
		gl_FragColor = fog4(diffuse_f);
	#endif
	}
#endif
