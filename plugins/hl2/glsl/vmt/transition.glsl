!!ver 110
!!permu FOG
!!permu BUMP
!!permu LIGHTSTYLED
!!permu REFLECTCUBEMASK
!!permu UPPERLOWER
!!samps diffuse
//ftesurf (P187): the second base texture is declared only when the material HAS
//one.  mat_vmt.c used to emit `uppermap "materials/.vtf"` unconditionally, and
//because the blend factor is vertex alpha -- which mod_vbsp.c:2384 leaves at 1.0
//on every non-displacement surface -- mix() then returned that missing texture
//in full and the material's own basetexture was never sampled at all.
!!samps !NOBLEND upper
//$blendmodulatetexture, carried on the free s_lower slot.  g = where the two
//textures cross over, r = how wide the crossover is.
!!samps =BLENDMOD lower

!!samps lightmap
!!samps =LIGHTSTYLED lightmap1 lightmap2 lightmap3

!!samps =BUMP normalmap
//ftesurf (P251): $bumpmap2, the SECOND normal map, riding the free s_specular
//slot.  Same reasoning as $blendmodulatetexture on s_lower above: `specularmap`
//(gl_shader.c:3531) is a real sampler keyword that mat_vmt.c emits for nothing
//else, so this costs no engine change and no new interface.  s_fullbright is NOT
//free -- mat_vmt.c:2463-2466 hands it to any material with $selfillum.
!!samps =BUMP2 specular

// envmaps only
!!samps =REFLECTCUBEMASK reflectmask reflectcube

!!permu FAKESHADOWS
!!cvardf r_glsl_pcf
!!samps =FAKESHADOWS shadowmap

#include "sys/defs.h"

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


//ftesurf (P187): $alpha, as a compile-time constant.  A Source material's
//opacity is a scalar on the material, not on the entity, so e_colourident (the
//entity's colormod/alpha) cannot carry it -- and this generator emits a
//top-level `program` with no pass, so `alphagen const` has nowhere to live.
//The #define is the same mechanism ENVTINT/ENVSAT already use.
#ifndef ALPHA
#define ALPHA 1.0
#endif

varying vec2 tex_c;
varying vec4 vex_color;

//ftesurf (P251): $seamless_scale, the same triplanar projection lightmapped.glsl
//gained in this patch and for the same reason -- 12 of the library's 26
//$seamless_scale materials are WorldVertexTransition, so implementing it only in
//the other shader would have covered less than half of them.  Valve's PC math,
//from lightmappedgeneric_vs20.fxc:117/:212 and
//common_lightmappedgeneric_fxc.h:103-130; the full derivation is written out in
//lightmapped.glsl rather than repeated here.
#ifdef SEAMLESS
varying vec3 seam_c;	//world position * $seamless_scale
varying vec3 seam_w;	//normal*normal, summing to 1
#endif

varying vec2 lm0;

#ifdef LIGHTSTYLED
varying vec2 lm1, lm2, lm3;
#endif

#ifdef FAKESHADOWS
	varying vec4 vtexprojcoord;
#endif

/* CUBEMAPS ONLY */
#ifdef REFLECTCUBEMASK
	varying vec3 eyevector;
	varying mat3 invsurface;
#endif

#ifdef VERTEX_SHADER
	void lightmapped_init(void)
	{
		lm0 = v_lmcoord;
	#ifdef LIGHTSTYLED
		lm1 = v_lmcoord2;
		lm2 = v_lmcoord3;
		lm3 = v_lmcoord4;
	#endif
	}

	void main ()
	{
		lightmapped_init();
		tex_c = v_texcoord + e_time * vec2(SCROLL);
		gl_Position = ftetransform();
		vex_color = v_colour;

	#ifdef SEAMLESS
		seam_c = (m_model * vec4(v_position.xyz, 1.0)).xyz * float(SEAMLESS);
		{
			vec3 wn = (m_model * vec4(v_normal, 0.0)).xyz;
			seam_w = wn * wn;
		}
	#endif

	/* CUBEMAPS ONLY */
	#ifdef REFLECTCUBEMASK
		invsurface = mat3(v_svector, v_tvector, v_normal);

		vec3 eyeminusvertex = e_eyepos - v_position.xyz;
		eyevector.x = dot(eyeminusvertex, v_svector.xyz);
		eyevector.y = dot(eyeminusvertex, v_tvector.xyz);
		eyevector.z = dot(eyeminusvertex, v_normal.xyz);
	#endif

	#ifdef FAKESHADOWS
		vtexprojcoord = (l_cubematrix*vec4(v_position.xyz, 1.0));
	#endif
	}
#endif

#ifdef FRAGMENT_SHADER
	#include "sys/fog.h"
	
#ifdef FAKESHADOWS
	#include "sys/pcf.h"
#endif

	#ifdef LIGHTSTYLED
		#define LIGHTMAP0 texture2D(s_lightmap0, lm0).rgb
		#define LIGHTMAP1 texture2D(s_lightmap1, lm1).rgb
		#define LIGHTMAP2 texture2D(s_lightmap2, lm2).rgb
		#define LIGHTMAP3 texture2D(s_lightmap3, lm3).rgb
	#else
		#define LIGHTMAP texture2D(s_lightmap, lm0).rgb 
	#endif

	//$seamless_scale sampling -- see lightmapped.glsl for the derivation.  Applied
	//to the base, the SECOND base and both normal maps, because Source projects
	//all of them through the same GetBaseTextureAndNormal call.  NOT applied to
	//$blendmodulatetexture: Source samples that from lightmapTexCoord3.zw, a
	//separate coordinate set it never puts through the projection
	//(lightmappedgeneric_ps2_3_x.h:321).
#ifdef SEAMLESS
	#define SEAMSAMP(s) (seam_w.x*texture2D(s, seam_c.zy) + seam_w.y*texture2D(s, seam_c.xz) + seam_w.z*texture2D(s, seam_c.xy))
#else
	#define SEAMSAMP(s) texture2D(s, tex_c)
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

		/* the light we're getting is always too bright */
		lightmaps *= 0.75;

		/* clamp at 1.5 */
		if (lightmaps.r > 1.5)
			lightmaps.r = 1.5;
		if (lightmaps.g > 1.5)
			lightmaps.g = 1.5;
		if (lightmaps.b > 1.5)
			lightmaps.b = 1.5;

		return lightmaps;
	}

	void main (void)
	{
		vec4 diffuse_f;

		// ftesurf (P187): this used to be
		//     diffuse_f.rgb = mix(s_diffuse, s_upper, vex_color.a);
		//     diffuse_f.a = 1.0;
		// and both lines were wrong.
		//
		// THE ALPHA.  Forcing it to 1.0 meant that mat_vmt.c's
		// `progblendfunc src_alpha one_minus_src_alpha` -- which it emits for
		// every $translucent material -- always blended with src_alpha == 1,
		// i.e. fully opaque.  The surface therefore looked solid while still
		// paying the entire cost of transparency: a blend cannot write depth,
		// so it is sorted back-to-front and everything behind it is drawn
		// anyway.  That is both "the smoke is solid" and part of "the map runs
		// at 30fps", from one line.  lightmapped.glsl never did this, which is
		// why an ordinary $translucent LightmappedGeneric has always blended
		// correctly and only this shader was reported broken.
		//
		// THE MIX.  See the !!samps note above: with no second texture the mix
		// returned a missing image in full.  #NOBLEND is now emitted for
		// exactly that case and the sampler is not even declared.
		vec4 base_f = SEAMSAMP(s_diffuse);

#ifdef NOBLEND
		diffuse_f = base_f;
#else
		float blend = vex_color.a;
	#ifdef BLENDMOD
		// Source's shaped transition.  The modulate map's green channel is the
		// midpoint and its red channel the half-width, so the crossover happens
		// over [g-r, g+r] instead of linearly across the whole range -- which is
		// what makes a blend read as gravel through tarmac rather than as a fade.
		// smoothstep is undefined when its edges are equal, and r == 0 is a
		// legitimate authoring choice meaning "hard edge", so that case is a step.
		vec4 mod_f = texture2D(s_lower, tex_c);
		float lo = clamp(mod_f.g - mod_f.r, 0.0, 1.0);
		float hi = clamp(mod_f.g + mod_f.r, 0.0, 1.0);
		blend = (hi > lo) ? smoothstep(lo, hi, blend) : step(lo, blend);
	#endif
		diffuse_f = mix(base_f, SEAMSAMP(s_upper), blend);
#endif

		//$alphatest.  mat_vmt.c has always emitted #MASK/#MASKLT for this
		//material type and this shader has never read it, so an alpha-masked
		//transition surface came out as a solid rectangle.
#ifdef MASKLT
		if (diffuse_f.a < float(MASK))
			discard;
#endif

		//ftesurf (P232): $vertexcolor / $vertexalpha, the per-vertex tint and opacity.
		//
		//AFTER the $alphatest discard on purpose.  Source's alpha test reads the
		//TEXTURE's alpha, so a region the mapper painted to fade has to fade -- if the
		//vertex value were folded in first, the faded end of the ramp would fall under
		//MASK and be cut out instead, which is a hole rather than a gradient.
		//
		//In the blended path vex_color.a is ALSO the mix factor, so a material asking
		//for both would be spending one channel twice.  No such material exists: see
		//the census in mat_vmt.c beside the #VERTEXALPHA emit.
#ifdef VERTEXCOL
		diffuse_f.rgb *= vex_color.rgb;
#endif
#ifdef VERTEXALPHA
		diffuse_f.a *= vex_color.a;
#endif

		diffuse_f.rgb *= vec3(COLOR);

		/* deluxemapping isn't working on Source BSP yet, FIXME */
		diffuse_f.rgb *= lightmap_fragment();

#ifdef FAKESHADOWS
		diffuse_f.rgb *= ShadowmapFilter(s_shadowmap, vtexprojcoord);
#endif

/* CUBEMAPS ONLY */
#ifdef REFLECTCUBEMASK
	/* We currently only use the normal/bumpmap for cubemap warping. move this block out once we do proper radiosity normalmapping */
	#ifdef BUMP
		/* Source's normalmaps are in the DX format where the green channel is flipped */
		vec4 normal_f = SEAMSAMP(s_normalmap);
		//ftesurf (P251): $bumpmap2 -- the second normal map, blended by the SAME
		//factor as the two base textures.  Valve, lightmappedgeneric_ps2_3_x.h:378-382:
		//
		//	vNormal.xyz = lerp( vNormal.xyz, vNormal2.xyz, blendfactor );
		//
		//Three details worth having right, all of them from those lines:
		//  - the factor is `blendfactor`, the one the diffuse already used, INCLUDING
		//    the $blendmodulatetexture reshaping at :321-331.  So this must sit after
		//    `blend` has been through the BLENDMOD smoothstep, and it does.
		//  - it is a lerp of the two normals, NOT of the raw texels and NOT
		//    renormalised afterwards.  Doing it before the DX green flip and the
		//    -0.5 unpack keeps that true, because both are affine in the texel.
		//  - same texture coordinates as $bumpmap (the separate-coords case is
		//    BUMPMASK, which Source itself excludes from this path).
		//
		//#BUMP2 is only emitted when the material has a $basetexture2, so `blend`
		//is always in scope here -- with #NOBLEND there is nothing to blend toward
		//and mat_vmt.c does not ask for this permutation.
		#ifdef BUMP2
		normal_f = mix(normal_f, SEAMSAMP(s_specular), blend);
		#endif
		normal_f.g = 1.0 - normal_f.g;
		normal_f.rgb = normalize(normal_f.rgb - 0.5);
	#else
		vec4 normal_f = vec4(0.0,0.0,1.0,0.0);
	#endif

	#if defined(ENVFROMMASK)
		/* We have a dedicated reflectmask */
		#define refl texture2D(s_reflectmask, tex_c).r
	#else
		/* when ENVFROMBASE is set or a normal isn't present, we're getting the reflectivity info from the diffusemap's alpha channel */
		#if defined(ENVFROMBASE) || !defined(BUMP)
			#define refl 1.0 - diffuse_f.a
		#else
			/* when ENVFROMNORM is set, we don't invert the refl */
			#if defined(ENVFROMNORM)
				//ftesurf (P251): normal_f.a rather than a second fetch of the same
				//sampler -- identical before this patch, and now the only version
				//that stays correct, since normal_f may be a $seamless_scale
				//projection and/or a $bumpmap2 blend that a plain tex_c re-fetch
				//would not reproduce.
				#define refl normal_f.a
			#else
				#define refl 1.0 - normal_f.a
			#endif
		#endif
	#endif

		vec3 cube_c = reflect(normalize(-eyevector), normal_f.rgb);
		cube_c = cube_c.x * invsurface[0] + cube_c.y * invsurface[1] + cube_c.z * invsurface[2];
		cube_c = (m_model * vec4(cube_c.xyz, 0.0)).xyz;
		diffuse_f.rgb += (textureCube(s_reflectcube, cube_c).rgb * vec3(refl,refl,refl));
#endif

		//AFTER the envmap block on purpose: `refl` above is 1.0 - diffuse_f.a,
		//and Source's $basealphaenvmapmask reads the TEXTURE's alpha, not the
		//material's opacity.  (Note that with the old hardcoded a = 1.0 that
		//refl was a constant zero, so #ENVFROMBASE on a transition material has
		//never reflected anything until now.)
		diffuse_f.a *= float(ALPHA);

#ifdef NOFOG
		gl_FragColor = diffuse_f;
#else
		gl_FragColor = fog4(diffuse_f);
#endif
	}
#endif
