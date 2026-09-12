!!ver 130-450
!!permu FOG
!!permu NOFOG
!!cvarf hl2_twoframes
!!samps !FRAMES diffuse upper
!!samps =FRAMES shield:2DArray=0

// ftesurf: UnlitTwoTexture, and the material-proxy chain that animates it.
//
// Reported as: "the portals in the main spawn area are a solid glow web
// texture. but in counter-strike they are not visible until you move closer to
// them, and visually it looks like a mask moving diagonally, and maybe mipmap
// coming in and showing you more as you get closer."
//
// Every one of those observations is a separate feature of one material,
// effects/combineshield/comshieldwall.vmt, and mat_vmt.c's UnlitTwoTexture arm
// implemented none of them -- it emitted a single pass with $basetexture and a
// blendfunc, which is exactly "a solid glow web".
//
// WHAT THE MATERIAL ACTUALLY ASKS FOR, read out of the VMT rather than guessed:
//
//   $basetexture / $texture2   two layers, MULTIPLIED.  That is what
//                              UnlitTwoTexture means, and drawing only the
//                              first is why the web never moved.
//   TextureScroll on
//   $texture2transform         rate .1, angle -45 -> the second layer slides
//                              diagonally across the first.  "a mask moving
//                              diagonally", precisely.
//   PlayerProximity 0.0009
//     -> Subtract from $gnoise
//     -> Sine -> $alpha         alpha ~= 0.9 - dist*0.0009, so it reaches zero
//                              at about 1000 units.  "not visible until you
//                              move closer".
//   PlayerProximity 0.2
//     -> Subtract 24 -> Clamp
//     -> $frame                 comshieldwall.vtf has THIRTY-ONE frames, and
//                              the frame index is chosen by DISTANCE, not by
//                              time: frame 0 inside 120 units, frame 30 beyond
//                              270.  That is the "mipmap coming in and showing
//                              you more as you get closer" -- a good guess at
//                              the effect from a wrong mechanism.
//
// THE FRAME RAMP, AND #FRAMES (Patch 286).  This was the one item on that list
// that Patch 279 resolved and could not apply.  Sampling a flipbook needs a
// sampler2DArray, which has to be filled by `map "$2darray:…"` -- a PASS
// keyword -- and every arrangement of that with a SECOND texture was tried and
// failed:
//
//   program inside the pass + top-level `uppermap`   both samplers came out
//                                                    black (a red/green debug
//                                                    build showed neither)
//   top-level program + one pass per sampler         `prog 1 passes 2`, and the
//                                                    surface stopped reaching
//                                                    the rasteriser entirely --
//                                                    a shader forced to output
//                                                    opaque red drew nothing
//
// The way past that is to stop needing two samplers.  comshieldwall.vmt points
// BOTH $basetexture and $texture2 at the same file, and it drives BOTH $frame
// and $frame2 from the same Clamp proxy -- two identical Clamp blocks, one per
// var, read out of the VMT.  So the material is one array read twice, at two
// texcoords and one layer, and that is exactly vmt/animated's proven shape:
// one pass, one `map "$2darray:…"`, `!!samps =FRAMES shield:2DArray=0`.
//
// 7 of the library's 48 UnlitTwoTexture materials are same-texture like this.
// The rest -- including comshieldwall2, which pairs this 31-frame base with a
// different single-frame texture -- keep the two-sampler, no-flipbook draw and
// are counted as declined in the census line.  On surf_tensor2 that is 42 of
// the 44 shield faces covered and 2 not.
//
// Without #FRAMES this draws frame 0, which is the CLOSE-UP frame and therefore
// the right one to be stuck on.
//
// WHY THE PROXIES ARE COMPILED IN RATHER THAN EVALUATED.  A general Source
// material-proxy interpreter would need a per-material scalar that the CPU
// updates every frame and the shader can read, which FTE has no channel for.
// But this whole chain is a pure function of two things the shader already has:
// the distance from the eye, and time.  So mat_vmt.c walks the proxy graph once
// at load, extracts the two coefficients, and bakes them in as #defines.  What
// cannot be expressed that way is not attempted -- see the honest list at the
// bottom of the UnlitTwoTexture arm in mat_vmt.c.
//
// PER-PIXEL, NOT PER-ENTITY, and that is a deliberate difference.  Source's
// PlayerProximity is the distance from the player to the ENTITY, so a large
// shield fades as one unit.  e_eyepos is the eye in model space, so
// length(v_position - e_eyepos) is the distance to this fragment, and a shield
// you stand beside fades in across its own surface instead of all at once.  It
// is a better-looking answer to the same question and it costs nothing, but it
// is not a transcription and should not be described as one.
//
// THE ARRAY SAMPLER IS THE SAME IDIOM vmt/animated USES (Patch 195): the frames
// of one mip level are contiguous in a VTF, which is the 2D-array layout, and
// `map "$2darray:name"` is what asks the loader for all of them.  Reused rather
// than reinvented, including the textureSize() frame count so no permutation is
// spent per frame count.  A single-frame VTF loads as a one-layer array and the
// clamp below keeps the index at 0, so the same shader serves both.
//
// WHY THE NON-FRAMES PATH USES NO PASSES.  The first version of this shader
// declared `base:2DArray=0` beside a bare `upper`, expecting the second layer to
// arrive through the top-level `uppermap` default-texture slot while the program
// sat in a pass -- and nothing arrived at all: the shield drew black, and a
// debug build that painted the two samples into red and green came out black in
// both channels.  The obvious repair, one pass per sampler with the program at
// the top level, was worse: `prog 1 passes 2` and no rasterised fragments at
// all.  What ships instead is a top-level program with `diffusemap` and
// `uppermap` and NO passes -- default textures are shader-scoped, so the program
// reads both without a pass list existing.  The samplers below are declared as
// bare default names for that reason, not as `name:type=idx`.
//
// #FRAMES is the exception and it is a one-sampler shader, which is the only
// arrangement that has ever bound an array here.  Its `shield:2DArray=0` pairs
// with the single `{ program … map "$2darray:…" }` pass in mat_vmt.c.
// vmt/animated has been running that exact shape on 927 materials.

#include "sys/defs.h"

#ifndef SCROLL
#define SCROLL 0.0,0.0
#endif
#ifndef SCROLL2
#define SCROLL2 0.0,0.0
#endif
#ifndef COLOR
#define COLOR 1.0,1.0,1.0
#endif
#ifndef ALPHA
#define ALPHA 1.0
#endif

//alpha = clamp(PROXBASE - PROXFADE*dist, 0, 1).  PROXFADE 0 disables it.
#ifndef PROXFADE
#define PROXFADE 0.0
#endif
#ifndef PROXBASE
#define PROXBASE 1.0
#endif

//layer = clamp(PROXFRAME.x*dist - PROXFRAME.y, PROXFRAME.z, PROXFRAME.w)
#ifndef PROXFRAME
#define PROXFRAME 0.0,0.0,0.0,0.0
#endif

//the Sine proxy: amplitude, period in seconds.  0 amplitude disables it.
#ifndef FLICKER
#define FLICKER 0.0,1.0
#endif

varying vec2 tex_c;
varying vec2 tex2_c;
varying float eyedist;

#ifdef VERTEX_SHADER
	void main ()
	{
		tex_c  = v_texcoord + e_time * vec2(SCROLL);
		tex2_c = v_texcoord + e_time * vec2(SCROLL2);
		//e_eyepos is the eye in MODEL space, so this is a world-unit distance
		//for a brush and for a moving entity alike, with no matrix of our own.
		eyedist = length(v_position.xyz - e_eyepos);
		gl_Position = ftetransform();
	}
#endif

#ifdef FRAGMENT_SHADER
	#include "sys/fog.h"

	//from !!cvarf at the top -- live, re-read every frame.  Declared
	//unconditionally rather than under #ifdef FRAMES because the !! directive
	//is scanned out of the whole file and the uniform exists in every
	//permutation; an unused uniform costs nothing.
	uniform float cvar_hl2_twoframes;

	void main (void)
	{
		vec4 diffuse_f;

		//THE MULTIPLY IS THE WHOLE POINT OF UnlitTwoTexture.  Source's
		//UnlitTwoTexture combines $basetexture and $texture2 and then modulates
		//by the vertex colour; with the second layer scrolling, the product is
		//an interference pattern that moves across a stationary web.  Drawing
		//layer one alone is a still image of the same texture, which is what
		//was on screen.
	#ifdef FRAMES
		//WHICH FRAME, and it is chosen by DISTANCE rather than by time -- the
		//one thing about this material that is not like every other flipbook.
		//frame 0 inside 120 units, frame 30 beyond 270, so the web gets DENSER
		//as you approach.  "mipmap coming in and showing you more as you get
		//closer" was a good description of the effect from a wrong mechanism.
		//
		//The layer count comes from textureSize rather than a define, the same
		//way vmt/animated does it, so no permutation is spent per frame count
		//and mat_vmt.c never has to open the VTF.  The second clamp is not
		//redundant with the material's own: Source's Clamp says 0..30 because
		//the author knew there were 31 frames, and a material whose proxy
		//outruns its texture would sample past the end of the array.
		//
		//ONE LAYER FOR BOTH READS, because the VMT drives $frame and $frame2
		//from two identical Clamp proxies.  That is transcription, not an
		//economy -- see the essay at the top.
		vec4 pf = vec4(PROXFRAME);
		ivec3 sz = textureSize(s_shield, 0);
		float layer = clamp(pf.x * eyedist - pf.y, pf.z, pf.w);
		layer = clamp(layer, 0.0, float(sz.z - 1));

		//THE A/B SWITCH, AND WHY IT HAD TO BE A LIVE UNIFORM.
		//
		//hl2_twoframes is read at MAP LOAD to choose this arm at all, and the
		//first attempt to verify the flipbook toggled it in the console and
		//compared screenshots.  It changed nothing: a plugin cvar's flags are
		//masked to `flags&1` on the way through Plug_Cvar_GetNVFDG, so
		//CVAR_SHADERSYSTEM never reaches Cvar_Get2 and no shader reload
		//happens.  Both arms of that A/B were the same arm, and the 12.5% of
		//the frame that differed was this material's own scroll and its 1.08s
		//alpha sine between the two shots.
		//
		//!!cvarf is re-read every frame, so 0 pins the layer at 0 -- exactly
		//what the load-time gate produces -- with the map still loaded and
		//nothing else changing between the pictures.  Fractional values are
		//useful too: .5 walks the ramp at half rate.
		layer = layer * clamp(cvar_hl2_twoframes, 0.0, 1.0);

		diffuse_f  = texture2D(s_shield, vec3(tex_c,  layer));
		diffuse_f *= texture2D(s_shield, vec3(tex2_c, layer));
	#else
		diffuse_f  = texture2D(s_diffuse, tex_c);
		diffuse_f *= texture2D(s_upper,   tex2_c);
	#endif

		diffuse_f.rgb *= e_colourident.rgb * vec3(COLOR);

		//THE DISTANCE FADE, and the reason the shield is invisible across the
		//room.  Clamped at both ends: Source's chain can produce a value above
		//1 when the noise term is high and the player is close.
		//
		//Unconditional rather than #ifdef'd on purpose -- the GLSL preprocessor
		//compares INTEGERS, so `#if PROXFADE != 0.0` is a syntax error and not
		//a switch.  With the defaults (fade 0, base 1) this is clamp(1,0,1),
		//which the compiler folds away, so the permutation is not worth its
		//risk of being written wrong.
		float a = float(ALPHA);
		a *= clamp(float(PROXBASE) - float(PROXFADE) * eyedist, 0.0, 1.0);
		{
			vec2 fl = vec2(FLICKER);
			if (fl.x > 0.0)
				a *= 1.0 + fl.x * sin(e_time * 6.2831853 / max(fl.y, 0.001));
		}

		//WHICH CHANNEL THE FADE GOES INTO DEPENDS ON THE BLEND, and writing it
		//into alpha alone was wrong for exactly the material this shader
		//exists for.
		//
		//Reported as "the scrolling effect doesn't really diminish over
		//distance", with the scroll itself visibly working -- so the two
		//layers and their transform had arrived and only the fade had not.
		//The reason is that comshieldwall is $additive 1, which becomes
		//blendFunc add, which is GL_ONE GL_ONE: the destination factor never
		//involves the source alpha, so `diffuse_f.a *= a` is discarded by the
		//blender and the shield is exactly as bright at 900 units as at 90.
		//
		//An additive surface fades by getting DARKER, an alpha-blended one by
		//getting more transparent.  The permutation comes from the material's
		//own $additive rather than a guess, because a material may write both
		//$additive and $translucent and Source resolves that the way
		//mat_vmt.c's blend chain does -- additive wins.
		//
		//NOTE FOR ANYONE EDITING THIS FILE: line comments only.  A /* */ block
		//breaks engine/shaders/generatebuiltinsl, which wraps the whole shader
		//in a C comment when it bakes it into mat_vmt_progs.h -- the first */
		//inside ends that comment early and the build dies on stray tokens
		//three lines later.  Every other vmt/*.glsl uses // for this reason.
	#ifdef ADDITIVE
		diffuse_f.rgb *= a;
	#else
		diffuse_f.a *= a;
	#endif

	#ifdef NOFOG
		gl_FragColor = diffuse_f;
	#else
		//FTESurf Patch 299: fog the colour, leave the alpha to the blender.
		//fog4() multiplies by regularcolour.a, which on a Source material is a
		//MASK ($basealphaenvmapmask and friends), not opacity.  Reasoning and
		//measurements in vertexlit.glsl.  hl2_fog_alphamul 1 restores fog4().
		#if #include "cvar/hl2_fog_alphamul"
			gl_FragColor = fog4(diffuse_f);
		#else
			gl_FragColor = vec4(fog3(diffuse_f.rgb), diffuse_f.a);
		#endif
	#endif
	}
#endif
