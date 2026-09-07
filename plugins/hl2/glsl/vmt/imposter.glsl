!!ver 110
!!permu FOG
!!samps reflectcube

#include "sys/defs.h"

//FTESurf Patch 263: Source's WindowImposter.
//
//A brush/displacement shader that draws ONE thing -- its $envmap cubemap,
//sampled by the VIEW DIRECTION -- and nothing else.  No basetexture, no
//lightmap, no lighting.  What separates it from every other envmap in this
//directory is one sentence of Valve's documentation: the face's orientation is
//not taken into account when reflecting the cubemap, which makes it convenient
//for windows and doors that hide what is behind them while looking like they do
//not -- and for faking more than one simultaneous skybox.
//
//So the single line that IS this shader is that the sample direction is the view
//ray itself and not reflect(view, normal).  Reflecting off the surface normal --
//which is what lightmapped.glsl and vertexlit.glsl correctly do for a real
//$envmap -- would make the pane behave like a mirror: the image would slide the
//wrong way as the camera moves, and a flat ceiling would smear one cubemap texel
//across the whole face.  Sampling the view ray makes the surface behave like a
//window onto a cube, which is exactly a second skybox.
//
//WHY THIS EXISTS: surf_monolith's bonus 4 is roofed and walled by a 2-unit slab
//of materials/fakeskies/mpa45.vmt, laid 4 units UNDER the map's real sky brush,
//so the room shows a red sky while the rest of the map shows sky138a.  With no
//WindowImposter arm in the parser that material fell through to vmt/unlit, whose
//diffusemap resolved to the cubemap file itself -- img_vtf.c loads it as
//PTI_CUBE, not a 2D base -- so the backend substituted missing_texture and the
//whole room drew as the notexture checkerboard.  155 materials, 59 maps.
//
//WORLD SPACE, computed straight from the position rather than through the
//tangent basis.  e_eyepos is the eye in MODEL space, so v_position - e_eyepos is
//a model-space direction and m_model takes it to world -- the same space
//lightmapped.glsl's cube_c ends up in, so a cubemap looks identical whichever
//material samples it.  Going via mat3(v_svector, v_tvector, v_normal) the way
//that shader does would be equivalent for an orthonormal basis and would need
//three vertex attributes this shader has no other use for; the direct form needs
//only v_position, which every surface has.
//
//Not normalised: textureCube takes a direction of any length.
//
//NOTE FOR THE NEXT PERSON ADDING A FILE HERE: plugins/hl2/Makefile carries a
//hand-maintained VMTPROGSBASE list, and a .glsl that is not in it is silently
//left out of the DLL while make reports success.  This file cost one build to
//rediscover that.  Essays go in // comments, never /* */: generatebuiltinsl
//passes // lines through as C comments but turns everything else into string
//literals, so a quotation mark inside a block comment breaks the generated
//header.  That cost the second build.

varying vec3 cubedir;

#ifdef VERTEX_SHADER
void main ()
{
	gl_Position = ftetransform();
	cubedir = (m_model * vec4(v_position.xyz - e_eyepos, 0.0)).xyz;
}
#endif

#ifdef FRAGMENT_SHADER
#include "sys/fog.h"

//$alpha and $color, which first-party Source does not accept on this shader and
//which the library uses anyway.  Censused over all 155 WindowImposter materials:
//$envmap 155 (100%), $nofog 150, $color 25, $alpha 22, $ignorez 18, $nocull 6.
//So roughly 30% of them say something about tint or opacity, and a version of
//this shader that ignored both would draw those at the wrong colour while
//looking like it worked.
//
//Both arrive through the generic progargs the parser has already built -- they
//are value defines rather than permutations, so they need no !!permu and an
//unused one costs nothing.  $ignorez and $nocull need nothing here either: the
//shared tail in mat_vmt.c emits nodepth and cull disable for them.
//
//Opaque by default: hiding what is behind it is the entire point of an imposter,
//and a blended one would have to sort.
#ifndef ALPHA
#define ALPHA 1.0
#endif

#ifndef COLOR
#define COLOR 1.0,1.0,1.0
#endif

void main ()
{
	vec4 imposter_f = vec4(textureCube(s_reflectcube, cubedir).rgb, float(ALPHA));
	imposter_f.rgb *= vec3(COLOR);
	gl_FragColor = fog4( imposter_f );
}
#endif
