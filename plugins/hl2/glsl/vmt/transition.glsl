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
!!cvardf r_lightmap_bicubic_world=0
!!samps =FAKESHADOWS shadowmap
!!samps sunvis
!!cvar3f r_shadows_color=0.5 0.5 0.5
!!cvar3f r_shadows_sunfloor=0 0 0
!!cvar3f r_shadows_sunlm=0 0 0
!!cvarf r_shadows_worldcast=1
!!cvar3f r_shadows_sundir=0 0 -1
!!cvarf gl_overbright
!!cvarf r_autoexposure_sky=1

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

//FTESurf Patch 268 B: $envmaptint / $envmapsaturation / $envmapcontrast.  This
//shader is the one VMT program that never applied any of them -- its envmap block
//added the raw cube sample -- so it never needed the defaults either.  They live
//here now, in the same spelling lightmapped.glsl uses (mat_vmt.c emits
//"#ENVTINT=%f,%f,%f" and "#ENVSAT=%f,%f,%f"), and each default is the identity:
//tint 1, saturation 1, contrast 0 reproduce today's picture exactly.
#ifndef ENVTINT
#define ENVTINT 1.0,1.0,1.0
#endif

#ifndef ENVSAT
#define ENVSAT 1.0,1.0,1.0
#endif

#ifndef ENVCONTRAST
#define ENVCONTRAST 0.0
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
	varying vec3 shsunt;	//FTESurf Patch 1781: toward the sun in the surface's (s, t, n), as lightmapped.glsl
	uniform vec3 cvar_r_shadows_sundir;	//the sun VRAD baked with (the plugin's; shadow_control may aim the shadows elsewhere)
	varying vec4 vtexprojcoord;
	#ifdef FAKESHADOWS_CASCADE
	varying vec3 shwpos;	//FTESurf Patch 1762: world position for the sun cascades (sys/pcf.h CascadeShadowFilter)
	#endif
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
		#ifdef FAKESHADOWS_CASCADE
		shwpos = (m_model*vec4(v_position.xyz, 1.0)).xyz;
		#endif
		{
			vec3 tosun = -cvar_r_shadows_sundir;
			shsunt = vec3(dot(tosun, (m_model*vec4(v_svector, 0.0)).xyz), dot(tosun, (m_model*vec4(v_tvector, 0.0)).xyz),
				dot(tosun, (m_model*vec4(v_normal, 0.0)).xyz));
		}
	#endif
	}
#endif

#ifdef FRAGMENT_SHADER
	#include "sys/fog.h"
	
#ifdef FAKESHADOWS
	#include "sys/pcf.h"
#endif

	//FTESurf Patch 1275: a cubic B-spline over the lightmap in four bilinear taps, as HL2's 20th-anniversary
	//update filters it. Each face's block has a one-texel copy of its edge around it (r_surf.c SURF_LMPAD).
	#if r_lightmap_bicubic_world
	vec3 lm_bicubic(sampler2D s, vec2 c)
	{
		vec2 p = c * e_lmsize.xy - 0.5;
		vec2 f = fract(p);
		vec2 f2 = f * f, f3 = f2 * f;
		vec2 w0 = (1.0 - 3.0*f + 3.0*f2 - f3) / 6.0;
		vec2 w1 = (4.0 - 6.0*f2 + 3.0*f3) / 6.0;
		vec2 w3 = f3 / 6.0;
		vec2 g0 = w0 + w1, g1 = 1.0 - g0;
		vec2 b = p - f + 0.5;
		vec2 h0 = (b - 1.0 + w1 / g0) * e_lmsize.zw, h1 = (b + 1.0 + w3 / g1) * e_lmsize.zw;
		return g0.y * (g0.x * texture2D(s, h0).rgb + g1.x * texture2D(s, vec2(h1.x, h0.y)).rgb) +
		       g1.y * (g0.x * texture2D(s, vec2(h0.x, h1.y)).rgb + g1.x * texture2D(s, h1).rgb);
	}
	#define LMSAMPLE(s,c) lm_bicubic(s, c)
	#else
	#define LMSAMPLE(s,c) texture2D(s, c).rgb
	#endif
	#ifdef LIGHTSTYLED
		#define LIGHTMAP0 LMSAMPLE(s_lightmap0, lm0)
		#define LIGHTMAP1 LMSAMPLE(s_lightmap1, lm1)
		#define LIGHTMAP2 LMSAMPLE(s_lightmap2, lm2)
		#define LIGHTMAP3 LMSAMPLE(s_lightmap3, lm3)
	#else
		#define LIGHTMAP LMSAMPLE(s_lightmap, lm0)
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

#ifdef FAKESHADOWS
	uniform vec3 cvar_r_shadows_color;		//FTESurf Patch 1732
	uniform vec3 cvar_r_shadows_sunfloor;
#endif
	uniform vec3 cvar_r_shadows_sunlm;		//FTESurf Patch 1781
	uniform float cvar_r_shadows_worldcast;	//the live sun needs the world in the cascades
	uniform float cvar_gl_overbright;
	uniform float cvar_r_autoexposure_sky;	//FTESurf Patch 1811: Source exposes the envmap with the rest of the pixel
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
		//Patch 1269: no 0.75 scale or 1.5 clamp, as lightmapped.glsl since e4c4938ef: a blended
		//displacement drew at 3/4 of the flat face beside it (surf_starvald's floor seam).
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
	#ifdef FTE_ALPHACOVERAGE	// FTESurf Patch 1262: under MSAA the test's edge is a pixel of coverage, not a cut
		float a2c = (diffuse_f.a - float(MASK)) / max(fwidth(diffuse_f.a), 0.0001) + 0.5;
		if (e_alphacoverage > 0.0 ? a2c <= 0.0 : diffuse_f.a < float(MASK))
			discard;
	#else
		if (diffuse_f.a < float(MASK))
			discard;
	#endif
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

		//FTESurf Patch 1288: $color in linear light, as LightmappedGeneric (see lightmapped.glsl)
		diffuse_f.rgb *= e_colourident.rgb * pow(vec3(COLOR), vec3(1.0/2.2));	//FTESurf Patch 1250: a brush entity's rendercolor

		/* deluxemapping isn't working on Source BSP yet, FIXME */
		vec3 lmv = lightmap_fragment();
		float rnmlin = -1.0;	//FTESurf Patch 1781: set once the radiosity weights are in lmv
		//the world's e_lmscale also carries 2^gl_overbright (its texels are stored that much smaller, Surf_LightmapShift --
		//MDLF_NEEDOVERBRIGHT), which a prop's light does not
		float shob = exp2(clamp(floor(cvar_gl_overbright), 0.0, 2.0));
#ifdef FAKESHADOWS
		//FTESurf Patch 1732: shlit 0 shadowed .. 1 lit (the filter's 0.5..1)
		#ifdef FAKESHADOWS_CASCADE
		float shlit = 2.0*CascadeShadowFilter(s_shadowmap, vtexprojcoord, shwpos) - 1.0;	//FTESurf Patch 1762
		#else
		float shlit = 2.0*ShadowmapFilter(s_shadowmap, vtexprojcoord) - 1.0;
		#endif
#endif
		if (cvar_r_shadows_sunlm.r + cvar_r_shadows_sunlm.g + cvar_r_shadows_sunlm.b > 0.0)
		{	//FTESurf Patch 1781: Strata's world sun.  Style 0 is the indirect light and s_sunvis (1 - page) the sun VRAD baked
			//over it (VBSP_SunPageToIndirect): the bake goes back on, or under the cascades with the world in them the sun at
			//this pixel's N.L -- VRAD's 0.984 * N.L^1.4 at full sun -- shadowed by them, the bake again by 0.35 of the outer
			//cascade (its own fade starts there).  In linear light: lmv is the texel's sRGB times e_lmscale.
		#ifdef LIGHTSTYLED
			vec3 shk = max(e_lmscale[0].rgb / shob, vec3(1e-4));
		#else
			vec3 shk = max(e_lmscale.rgb / shob, vec3(1e-4));
		#endif
			vec3 lml = max(lmv, vec3(0.0)) / shk;
			lml = mix(lml / 12.92, pow((lml + 0.055) / 1.055, vec3(2.4)), step(0.04045, lml));
			vec3 sun = cvar_r_shadows_sunlm * (1.0 - LMSAMPLE(s_sunvis, lm0).r);	//filtered as the lightmap is (P1275 bicubic)
		#ifdef FAKESHADOWS
			sun *= clamp(shlit, 0.0, 1.0);	//a shadow takes the baked sun out where the live sun is not drawn (worldcast 0, one map)
		#endif
		#ifdef FAKESHADOWS_CASCADE
			if (cvar_r_shadows_worldcast > 0.0 && l_fakeshadowinfo[0].z > 0.5)
			{
				vec4 swc = l_fakeshadowworld[FAKESHADOWS_SUNCELLS-1] * vec4(shwpos, 1.0);
				vec3 swe = abs((swc.xyz/swc.w + vec3(1.0)) * 0.5 - vec3(0.5));
				float sunw = 1.0 - clamp((max(max(swe.x, swe.y), swe.z) - 0.30) / 0.05, 0.0, 1.0);
			#ifdef BUMP
				vec3 sunn = SEAMSAMP(s_normalmap).rgb;
				#if defined(BUMP2) && !defined(NOBLEND)
				sunn = mix(sunn, SEAMSAMP(s_specular).rgb, blend);
				#endif
				sunn = normalize(vec3(sunn.r, 1.0 - sunn.g, sunn.b) - 0.5);
			#else
				vec3 sunn = vec3(0.0, 0.0, 1.0);
			#endif
			#ifdef BUMP
				float ndl = max(dot(sunn, shsunt) * inversesqrt(max(dot(shsunt, shsunt), 1e-8)), 0.0);
			#else
				float ndl = max(shsunt.z, 0.0);	//the unit sun on the unit normal: no tangent (degenerate on a cliff) involved
			#endif
				sun = mix(sun, cvar_r_shadows_sunlm * (0.984 * pow(ndl, 1.4) * clamp(shlit, 0.0, 1.0)), sunw);
			}
		#endif
			lml += sun;
			lmv = shk * mix(lml * 12.92, 1.055 * pow(lml, vec3(1.0/2.4)) - 0.055, step(0.0031308, lml));
		}
#ifdef FAKESHADOWS
		else
		{	//over a lightmap that holds the sun's shadows (r_shadows_sunfloor, a Strata map without the page) a shadow only
			//takes the sun out, down to the sky's share; elsewhere it is shadow_control's colour (r_shadows_color)
		#ifdef LIGHTSTYLED
			vec3 shfloor = cvar_r_shadows_sunfloor * e_lmscale[0].rgb / shob;
		#else
			vec3 shfloor = cvar_r_shadows_sunfloor * e_lmscale.rgb / shob;
		#endif
			if (shfloor.r + shfloor.g + shfloor.b > 0.0)
			{	//FTESurf Patch 1774: down to the floor's LEVEL, the lightmap's own hue kept part-way
				float shll = dot(lmv, vec3(0.2126, 0.7152, 0.0722)), shlf = dot(shfloor, vec3(0.2126, 0.7152, 0.0722));
				vec3 shdark = lmv;
				if (shll > shlf)
				{
					float shr = shlf / shll;
					shdark = mix(lmv * shr, shfloor, (1.0 - shr) * 0.5);
				}
				lmv = mix(shdark, lmv, shlit);
			}
			else
				diffuse_f.rgb *= mix(cvar_r_shadows_color, vec3(1.0), shlit);
		}
#endif
		diffuse_f.rgb *= lmv;

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

	//FTESurf Patch 268 B: Source's own mask defaults, under #ENVSRCMASK.
	//  no mask key at all      -> specularFactor stays 1.0.  vNormal's default is
	//                             float4(0,0,1,1) (lightmappedgeneric_ps2_3_x.h:205),
	//                             so even the bNormalMapAlphaEnvmapMask-without-a-
	//                             bumpmap arm at :396-399 reads 1.0
	//  $basealphaenvmapmask    -> 1.0 - blendedAlpha, i.e. 1.0 - diffuse_f.a here --
	//                             and on THIS shader diffuse_f.a is already the
	//                             lerp of the two base alphas by `blend`, which is
	//                             exactly Source's blendedAlpha (:334, :408-411,
	//                             "Reversing alpha blows!").  Source inverts this
	//                             one and only this one
	//  $normalmapalphaenvmapmask -> the normalmap's alpha, uninverted (:391-394), and
	//                             only when there is a normalmap to read it from
	#ifdef ENVSRCMASK
		#if defined(ENVFROMMASK)
			//We have a dedicated reflectmask
			#define refl texture2D(s_reflectmask, tex_c).r
		#else
			#if defined(ENVFROMBASE)
				#define refl 1.0 - diffuse_f.a
			#else
				#if defined(ENVFROMNORM) && defined(BUMP)
					//ftesurf (P251): normal_f.a rather than a second fetch -- it may
					//be a $seamless_scale projection and/or a $bumpmap2 blend that a
					//plain tex_c re-fetch would not reproduce.
					#define refl normal_f.a
				#else
					#define refl 1.0
				#endif
			#endif
		#endif
	//FTESurf Patch 268 B: without #ENVSRCMASK the arms below are today's, verbatim.
	#else

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

	//FTESurf Patch 268 B: end of the ENVSRCMASK wrapper.
	#endif

		vec3 cube_c = reflect(normalize(-eyevector), normal_f.rgb);
		cube_c = cube_c.x * invsurface[0] + cube_c.y * invsurface[1] + cube_c.z * invsurface[2];
		cube_c = (m_model * vec4(cube_c.xyz, 0.0)).xyz;
	//FTESurf Patch 268 B: Source's envmap term, under #ENVSRCSPEC --
	//    spec  = cube * mask;
	//    spec *= tint;
	//    spec  = lerp(spec, spec*spec, contrast);
	//    spec  = lerp(luma(0.299,0.587,0.114), spec, saturation);
	//(lightmappedgeneric_ps2_3_x.h:553-561).  This shader applies NONE of the three
	//today -- it adds the raw cube sample -- so the whole chain, and the ENVTINT /
	//ENVSAT / ENVCONTRAST defaults it now carries, arrive together under this switch.
	//ENVSRCPOST is a no-op here: this shader already adds after the lightmap multiply
	//(:263), which is where Source adds it too.
	//`refl` is a macro that may expand to `1.0 - diffuse_f.a`, so it is always
	//spelled vec3(refl,refl,refl) here and never used as a bare factor.
	//FTESurf Patch 1811: in LINEAR light, as Source: cube * mask * tint, contrast, saturation, then the tonemap scale (the
	//exposure, r_autoexposure_sky^2.2) and the add onto the linear diffuse, encoded after (this cube is sRGB-encoded).
	#ifdef ENVSRCSPEC
		vec3 cube_tint = vec3(ENVTINT);
		vec3 cube_sat = vec3(ENVSAT);
		vec3 spec = pow(textureCube(s_reflectcube, cube_c).rgb, vec3(2.2)) * vec3(refl,refl,refl);
		spec *= cube_tint;
		spec = mix(spec, spec*spec, float(ENVCONTRAST));
		spec = mix(vec3(dot(spec, vec3(0.299,0.587,0.114))), spec, cube_sat.r);
		diffuse_f.rgb = pow(pow(max(diffuse_f.rgb, vec3(0.0)), vec3(2.2)) + spec * pow(cvar_r_autoexposure_sky, 2.2), vec3(1.0/2.2));
	//FTESurf Patch 268 B: without #ENVSRCSPEC, today's line, verbatim.
	#else
		diffuse_f.rgb = pow(pow(max(diffuse_f.rgb, vec3(0.0)), vec3(2.2)) + (pow(textureCube(s_reflectcube, cube_c).rgb, vec3(2.2)) * vec3(refl,refl,refl)) * pow(cvar_r_autoexposure_sky, 2.2), vec3(1.0/2.2));
	//FTESurf Patch 268 B: end of the ENVSRCSPEC wrapper.
	#endif
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
#if defined(MASKLT) && defined(FTE_ALPHACOVERAGE)
	if (e_alphacoverage > 0.0)
		gl_FragColor.a = min(a2c, 1.0);
#endif
	}
#endif
