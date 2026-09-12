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

//ftesurf (P300): $vertexcolor / $vertexalpha.  Until now these were the reason
//an additive UnlitGeneric or Sprite had to stay on a PASS -- `rgbGen vertex` is
//a pass keyword and this program had nowhere to put it.  Same idiom as
//lightmapped.glsl:113-115,149-151,240-245, which is the arm that proved it.
//
//NOT the same thing as a pass's `rgbGen vertex`, strictly: that is
//RGB_GEN_VERTEX_LIGHTING, which GenerateColourMods scales by
//shaderstate.identitylighting, where v_colour here is the raw array.  They agree
//at identitylighting 1, which is the case that GenerateColourMods short-circuits
//to the raw VBO anyway (gl_backend.c:2430-2488).  The draws this arm exists for
//are CSQC R_PolygonVertex sprites, which supply their colour per vertex and are
//not lit at all.

#include "sys/fog.h"

varying vec2 tex_c;
#if defined(VERTEXCOL) || defined(VERTEXALPHA)
varying vec4 vex_color;
#endif

#ifdef VERTEX_SHADER
void main ()
{
	tex_c = v_texcoord + e_time * vec2(SCROLL);
#if defined(VERTEXCOL) || defined(VERTEXALPHA)
	vex_color = v_colour;
#endif
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

#ifdef VERTEXCOL
	diffuse_f.rgb *= vex_color.rgb;
#endif
#ifdef VERTEXALPHA
	diffuse_f.a *= vex_color.a;
#endif

	diffuse_f.a *= float(ALPHA);

#ifdef NOFOG
	//FTESurf Patch 300: $nofog, which these arms have been computing and then
	//throwing away.  mat_vmt.c:2346-2347 appends #NOFOG to progargs for EVERY
	//class, but the UnlitGeneric and Sprite arms emitted no program to carry it,
	//so a material that said "do not fog me" was fogged regardless.  Every
	//sibling program (animated, lightmapped, twotexture, vertexlit, transition)
	//already guards exactly this way; this one was the gap.
	gl_FragColor = diffuse_f;
#elif defined(ADDITIVE)
	//FTESurf Patch 300: AN ADDITIVE SURFACE FADES TOWARD BLACK, NEVER TOWARD THE
	//FOG COLOUR.  This is the whole reason the patch exists: under gl_one/gl_one
	//a texel that should contribute nothing is black, and mixing black toward a
	//fog colour turns it into a value that is then ADDED -- so the mask stops
	//masking and the quad shows as a flat translucent rectangle.  fog4additive
	//is `c * vec4(fac,fac,fac,1.0)`.
	//
	//Note it leaves alpha alone, so the Patch 299 question below does not arise
	//on this branch: fog4additive never multiplied by alpha in the first place.
	gl_FragColor = fog4additive( diffuse_f );
#else
	//FTESurf Patch 299: fog the colour, leave the alpha to the blender.
	//fog4() multiplies by regularcolour.a, which on a Source material is a
	//MASK ($basealphaenvmapmask and friends), not opacity.  Reasoning and
	//measurements in vertexlit.glsl.  hl2_fog_alphamul 1 restores fog4().
	#if #include "cvar/hl2_fog_alphamul"
		gl_FragColor = fog4( diffuse_f );
	#else
		gl_FragColor = vec4(fog3(diffuse_f.rgb), diffuse_f.a);
	#endif
#endif
}
#endif
