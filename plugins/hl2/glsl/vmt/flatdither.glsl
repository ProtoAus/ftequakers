!!ver 110
!!fixed
!!permu FOG
!!samps flat=0

// FTESurf Patch 151 -- ordered-dither stand-in for Source's expensive surfaces.
//
// WHY A DITHER RATHER THAN A BLEND.  Source water compiles to $refraction +
// $reflection and Source glass to $currentrender; all three are RENDER TARGETS,
// and the backend fills them by rendering the scene again, per batch.  Turning
// them off still left a translucent sheet, which is cheap to fill but is NOT
// free: a blended surface cannot write depth, so it has to be sorted back to
// front and everything behind it is drawn whether or not you can see it.
//
// An ordered dither is opaque geometry with a discard.  No blend, no sort, no
// framebuffer copy, no render target, and it WRITES DEPTH -- so the geometry
// behind it is z-rejected instead of overdrawn.  It is strictly cheaper than
// the translucent version it replaces, and it is a look you choose rather than
// a degradation you tolerate.
//
// The colour and the coverage are the material's own: mat_vmt.c feeds them in
// through the pass's rgbgen/alphagen const, which arrive here as v_colour, so
// each material still looks like itself and no shader permutation is spent on
// them.
//
// FTESurf Patch 159 -- `!!fixed` IS LOAD-BEARING AND BUILD 12 SHIPPED WITHOUT IT.
//
// GenerateColourMods -- the only thing that ever evaluates an rgbgen or an
// alphagen -- runs from gl_backend.c:4354 under `if (p->calcgens)`, and
// calcgens is set by exactly two things: `!!fixed` at the top of the GLSL, or
// `#usemods` in the program name (gl_shader.c:1831-1834, 2247-2248).  This had
// neither, so v_colour was never written for this program: the pass inherited
// whatever colour array the previous draw happened to leave bound.
//
// That is the whole of "hl2_water 3 makes the water a white dithered surface"
// and "hl2_dither_alpha does nothing at hl2_water 3".  The colour came out
// near-white because it was not $fogcolor, and the coverage varied because it
// was not $alpha either.  hl2_water 0 looked right the entire time for the one
// reason that matters here: mode 0 emits no program at all, so it takes the
// legacy path where colourgen always runs.
//
// Same trap as Patch 132's `#CHROME` (see engine/gl/model_hl.h): a generated
// shader asking for a gen that nothing was ever going to compute.  It costs one
// CPU pass over the vertices and it is what makes the mode work at all.

#include "sys/defs.h"
#include "sys/fog.h"

varying vec2 tex_c;
varying vec4 vc;

#ifdef VERTEX_SHADER
void main ()
{
	tex_c = v_texcoord;
	vc = v_colour;
	gl_Position = ftetransform();
}
#endif

#ifdef FRAGMENT_SHADER

// The 2x2 Bayer cell, [[0,2],[3,1]], written as arithmetic rather than as an
// array: GLSL 1.10 does not guarantee dynamic indexing of arrays in a fragment
// shader, and this has to run on whatever the driver is.
float b2 (float x, float y)
{
	return mod(2.0*x + 3.0*y, 4.0);
}

// The standard 4x4 from the recursion M2n = [[4Mn, 4Mn+2],[4Mn+3, 4Mn+1]],
// returned as a threshold in (0,1).
float bayer4 (vec2 p)
{
	float lo = b2(mod(p.x, 2.0), mod(p.y, 2.0));
	float hi = b2(mod(floor(p.x*0.5), 2.0), mod(floor(p.y*0.5), 2.0));
	return (4.0*lo + hi + 0.5) * (1.0/16.0);
}

void main ()
{
	// s_flat is the pass's own map -- $whiteimage for water (so the colour is
	// entirely $fogcolor) and the real base texture for glass, which has a frame
	// and a pattern worth keeping.
	vec4 col = texture2D(s_flat, tex_c) * vc;

	// Screen space, deliberately: a dither locked to the surface's own UVs
	// swims and moires as you move.  gl_FragCoord makes the pattern stand
	// still on the screen, which is what reads as a stipple rather than as a
	// broken texture.
	if (col.a < bayer4(gl_FragCoord.xy))
		discard;

	gl_FragColor = fog4(vec4(col.rgb, 1.0));
}
#endif
