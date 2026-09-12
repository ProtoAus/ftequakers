!!ver 130-450
!!cvarf hl2_colourcorrection
!!samps screen=0
!!samps lut0:3D=1
!!samps =LUT1 lut1:3D=2
!!samps =LUT2 lut2:3D=3
!!samps =LUT3 lut3:3D=4

// ftesurf Patch 288: Source's colour_correction, which FTE has never applied.
//
// Reported as part of "[the portal particles] are slightly to small and wayy to
// blue.  maybe the hue is actually colourcorrection, maybe they actually white."
//
// The hunch was right and it is worth being precise about why.  The PCF really
// is blue -- portal_blue's Color Random runs [51,51,80] to [0,96,255] -- and
// particle_glow_01.vtf is provably neutral (its reflectivity is R=G=B exactly).
// So the particles are not wrong.  What is wrong is everything AROUND them: in
// Source, surf_tensor2 runs two color_correction entities over the whole frame
// and the entire scene is graded cool, so the particles do not stand out.  In
// FTE the scene was ungraded and they did.  Fixing the particles' colour would
// have been fixing the wrong thing twice.
//
// WHAT SOURCE DOES, and it is a short list:
//
//     out = (1 - W) * in + sum( w[i] * LUT[i](in) ),  W = min(sum(w[i]), 1)
//
// with each LUT a 32x32x32 RGB lattice in a headerless .raw.  See img_ccraw.c
// for the file, and its essay for how the axis order was established (a corner
// probe gets it wrong; only a whole-volume correlation separates the six
// possible orders).
//
// THE HALF-TEXEL INSET IS NOT OPTIONAL.  A 32-cube stores its values AT the grid
// points, so input 0.0 must land on the centre of texel 0 and input 1.0 on the
// centre of texel 31 -- (c*(N-1) + 0.5)/N.  Sampling with the raw colour instead
// puts 1.0 half a texel past the last sample, where GL_CLAMP_TO_EDGE flattens
// it: highlights lose their top end and read as "crushed", which is exactly the
// artefact that would be blamed on the LUT rather than on the sampler.
//
// WHY THE WEIGHTS ARE #defineS AND NOT UNIFORMS.  color_correction has a
// fadeInDuration and can be Enabled and Disabled by entity I/O, so in Source the
// weights are live.  Nothing in this build implements that: the entities this
// applies to are enabled at map spawn by a logic_auto and never touched again,
// which is the common idiom and both of tensor2's.  Baking them means no uniform
// plumbing between the plugin and the renderer at all, and a map that really
// does animate its grade will hold the wrong weight rather than crash -- a
// visible-but-small error, and counted in the map's census line.
//
// SAMPLER 0 IS THE SCREEN AND THE REST ARE PASSES.  A post-process shader with
// more than one sampler is not a shape that has to be guessed at here:
// scenepp_waterwarp (gl_rmain.c:110-126) is a shipped top-level program with
// THREE passes and its underwaterwarp.glsl declares `!!samps screen=0 warp=1
// edge=2`.  That is the precedent this follows exactly.  It is worth writing
// down because the same shape FAILS for a world surface -- see the essay in
// mat_vmt.c's UnlitTwoTexture arm, where it produced `prog 1 passes 2` and no
// rasterised fragments at all.  Post-process and world are not the same case.
//
// NOTE FOR ANYONE EDITING THIS FILE: line comments only.  A /* */ block breaks
// engine/shaders/generatebuiltinsl, which wraps the whole shader in a C comment
// when it bakes it into mat_vmt_progs.h.

#include "sys/defs.h"

// the weight of each LUT.  0 is "not present", and with the defaults the whole
// thing folds to `out = in`.
#ifndef W0
#define W0 0.0
#endif
#ifndef W1
#define W1 0.0
#endif
#ifndef W2
#define W2 0.0
#endif
#ifndef W3
#define W3 0.0
#endif

varying vec2 texcoord;

#ifdef VERTEX_SHADER
void main ()
{
	// same flip fxaa.glsl and underwaterwarp.glsl use: $sourcecolour is an FBO
	// and its origin is the other way up from the screen's.
	texcoord = vec2(v_texcoord.x, 1.0 - v_texcoord.y);
	gl_Position = ftetransform();
}
#endif

#ifdef FRAGMENT_SHADER
// !!cvarf gives a LIVE uniform, re-read every frame, not a #define that would
// recompile.  That is what makes the A/B one command with the map already
// loaded: hl2_colourcorrection 0 / 1 / 0 at a fixed vantage, with nothing else
// changing between the three pictures.  Fractional values work and are useful --
// .5 is "half the grade", which is how you tell a wrong LUT from a right LUT
// applied too strongly.
//
// The same cvar ALSO gates generation at map load (mod_vbsp.c), so 0 at launch
// costs nothing at all: no LUT textures, no post-process pass, no FBO round
// trip.  Setting it to 1 afterwards needs a map load to build the shader; the
// live path exists for a map that was loaded with it on.
uniform float cvar_hl2_colourcorrection;

void main (void)
{
	vec4 scene = texture2D(s_screen, texcoord);
	vec3 c = clamp(scene.rgb, 0.0, 1.0);
	vec3 uvw;
	vec3 graded = vec3(0.0);
	float n, w = 0.0;

	// the lattice size is read from the texture rather than assumed, so the
	// shader cannot disagree with the loader about it.  Same idiom vmt/animated
	// uses for its frame count.
	n = float(textureSize(s_lut0, 0).x);
	uvw = (c * (n - 1.0) + 0.5) / n;

	graded += float(W0) * texture2D(s_lut0, uvw).rgb;
	w += float(W0);
#ifdef LUT1
	graded += float(W1) * texture2D(s_lut1, uvw).rgb;
	w += float(W1);
#endif
#ifdef LUT2
	graded += float(W2) * texture2D(s_lut2, uvw).rgb;
	w += float(W2);
#endif
#ifdef LUT3
	graded += float(W3) * texture2D(s_lut3, uvw).rgb;
	w += float(W3);
#endif

	// THE TOTAL WEIGHT IS WHAT SATURATES, not each one, and the normalise below
	// is the difference between saturating and over-brightening.  Three
	// corrections at .5 each are not 1.5x the grade: with w clamped to 1 the
	// scene term vanishes and the sum of the LUT terms would still be 1.5, so
	// the screen washes out.  Scaling the accumulator by 1/w when w > 1 keeps
	// this a convex combination for every possible set of weights.
	if (w > 1.0)
	{
		graded /= w;
		w = 1.0;
	}

	// the live switch, applied to the blend and not to the result, so 0 is
	// bit-for-bit the ungraded scene rather than "the scene, times something
	// that happens to be one".
	{
		float on = clamp(cvar_hl2_colourcorrection, 0.0, 1.0);
		graded *= on;
		w *= on;
	}

	gl_FragColor = vec4((1.0 - w) * scene.rgb + graded, scene.a);
}
#endif
