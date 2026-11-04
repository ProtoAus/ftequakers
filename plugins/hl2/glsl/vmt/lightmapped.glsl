!!ver 110
!!permu FOG
!!permu BUMP
!!permu LIGHTSTYLED
!!permu FULLBRIGHT
!!permu REFLECTCUBEMASK
!!permu NOFOG
!!samps diffuse

!!samps lightmap
!!samps =LIGHTSTYLED lightmap1 lightmap2 lightmap3

!!samps =BUMP normalmap
!!samps =DELUXE deluxemap
!!samps =FULLBRIGHT fullbright

// envmaps only
!!samps =REFLECTCUBEMASK reflectmask reflectcube

!!permu FAKESHADOWS
!!cvardf r_glsl_pcf
!!cvardf r_lightmap_bicubic_world=0
!!cvarf r_renderscale
!!samps =FAKESHADOWS shadowmap

#ifndef ENVTINT
#define ENVTINT 1.0,1.0,1.0
#endif

#ifndef ENVSAT
#define ENVSAT 1.0,1.0,1.0
#endif

//FTESurf Patch 268 B: $envmapcontrast.  mat_vmt.c has always parsed this key and
//then dropped it.  Source: specularLighting = lerp(s, s*s, g_EnvmapContrast)
//(lightmappedgeneric_ps2_3_x.h:558-559), so 0.0 is "no contrast", which is today's
//picture -- an absent #ENVCONTRAST must default to exactly that.
#ifndef ENVCONTRAST
#define ENVCONTRAST 0.0
#endif

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


//ftesurf (P251): $seamless_scale -- Source's triplanar projection.
//
//The material stops using its UVs at all and projects the base texture three
//times, down each world axis, blending by the surface normal.  That is what lets
//a mapper drape one cliff texture over a displacement without a seam anywhere.
//
//The math is Valve's, exactly, from the PC branch (the X360 branch additionally
//does max(w-0.3,0) and renormalises; the PC one does not, and does not need to,
//because the weights already sum to 1):
//
//  lightmappedgeneric_vs20.fxc:117   o.SeamlessTexCoord = SEAMLESS_SCALE*worldPos;
//  lightmappedgeneric_vs20.fxc:212   o.vertexColor.xyz  = vNormal * vNormal;
//  common_lightmappedgeneric_fxc.h:103-130
//        vResultBase  = w.x*tex2D(base, coords.zy)
//                     + w.y*tex2D(base, coords.xz)
//                     + w.z*tex2D(base, coords.xy);
//
//Note which coordinate PAIR goes with which weight -- zy, xz, xy, in that order.
//Getting that wrong is invisible on a wall facing an axis and only shows up on a
//slope, which is the only place this material type is ever used.
//
//WORLD space, via m_model, not model space: Source projects from worldPos, so a
//brush entity's texture must stay put when the brush moves rather than travelling
//with it.  On the world model m_model is identity and the multiply folds away.
//
//One thing here is deliberately NOT Valve's.  lightmappedgeneric_ps2_3_x.h:426-428
//skips `albedo *= i.vertexColor` when SEAMLESS, because it has spent the colour
//register on the weights.  This shader carries the weights in their own varying,
//so the material's real $vertexcolor is still its own and the VERTEXCOL multiply
//below stays.
//
//Declared inside the guard: every lightmapped world surface in the game compiles
//this file, and an unguarded pair of varyings would spend two interpolators and a
//matrix multiply on all of them to carry something only four maps ask for.
#ifdef SEAMLESS
varying vec3 seam_c;	//world position * $seamless_scale
varying vec3 seam_w;	//normal*normal -- the three projection weights, summing to 1
#endif


//ftesurf (P188): $color / $color2, the per-material tint.  Source multiplies the
//albedo by it; e_colourident cannot carry it because that is the entity's
//colormod, one value for a whole entity, and this is per-material.  Not clamped
//-- values above 1 are authored deliberately.
#ifndef COLOR
#define COLOR 1.0,1.0,1.0
#endif


varying vec2 tex_c;

//ftesurf (P232): $vertexcolor / $vertexalpha.  Declared INSIDE the guard, not
//unconditionally, because this shader draws every lightmapped world surface in the
//game and only a displacement painted by the mapper has anything to say here -- an
//always-on varying would spend an interpolator on all of them to carry white.
#if defined(VERTEXCOL) || defined(VERTEXALPHA)
varying vec4 vex_color;
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
	#if defined(VERTEXCOL) || defined(VERTEXALPHA)
		vex_color = v_colour;
	#endif

	#ifdef SEAMLESS
		seam_c = (m_model * vec4(v_position.xyz, 1.0)).xyz * float(SEAMLESS);
		{
			vec3 wn = (m_model * vec4(v_normal, 0.0)).xyz;
			seam_w = wn * wn;	//already sums to 1 for a unit normal; Valve does not normalise on PC
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
uniform float cvar_r_renderscale;
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

	//$seamless_scale sampling, as one macro so the base map and the bump map
	//cannot drift apart -- Source projects both through the same
	//GetBaseTextureAndNormal call (common_lightmappedgeneric_fxc.h:89-145).
	//The zy / xz / xy pairing is Valve's and is load-bearing: swap two of them and
	//a wall facing an axis still looks right while every slope goes wrong.
#ifdef SEAMLESS
	#define SEAMSAMP(s) (seam_w.x*texture2D(s, seam_c.zy) + seam_w.y*texture2D(s, seam_c.xz) + seam_w.z*texture2D(s, seam_c.xy))
#else
	#define SEAMSAMP(s) texture2D(s, tex_c)
#endif

#ifdef BUMP
	//FTESurf Patch 1264: Source's bump basis (common_fxc.h:36), the tangent space of its normal maps,
	//ssbumps and the deluxemap mod_vbsp.c VBSP_BuildRNM writes.
	const vec3 rnm_b0 = vec3(0.81649661, 0.0, 0.57735026);
	const vec3 rnm_b1 = vec3(-0.40824833, 0.70710677, 0.57735026);
	const vec3 rnm_b2 = vec3(-0.40824822, -0.70710683, 0.57735026);
#endif
#if defined(DELUXE) && defined(BUMP)
	//FTESurf Patch 1264: radiosity normal mapping (lightmappedgeneric_ps2_3_x.h:433-466).  The
	//deluxemap is the luxel's three basis-map luminances as one direction, so each map's share is
	//dot(dir, basis); the lightmap is their mean (VBSP_BuildRNM), so Source's weighted sum is the mean
	//times 3 * dot(weights, share) / sum(share).  A normal map weighs by its squared, clamped dots
	//with the basis, normalized; an ssbump IS the three weights, unnormalized.  That ratio is linear
	//light and the lightmap is sRGB-encoded (VBSP_ConvertLuxel), hence the 1/2.2.  A zero direction is
	//a luxel with no basis data (or no static light): left alone.
	float rnm_fragment()
	{
		vec3 share = max(vec3(dot(texture2D(s_deluxemap, lm0).rgb * 2.0 - 1.0, rnm_b0),
		                      dot(texture2D(s_deluxemap, lm0).rgb * 2.0 - 1.0, rnm_b1),
		                      dot(texture2D(s_deluxemap, lm0).rgb * 2.0 - 1.0, rnm_b2)), 0.0);
		vec3 w = SEAMSAMP(s_normalmap).rgb;
	#ifndef SSBUMP
		w = w * 2.0 - 1.0;	//Source's DX space, as the deluxemap: no green flip
		w = clamp(vec3(dot(w, rnm_b0), dot(w, rnm_b1), dot(w, rnm_b2)), 0.0, 1.0);
		w *= w;
		w /= max(dot(w, vec3(1.0)), 0.0001);
	#endif
		float total = dot(share, vec3(1.0));
		if (total < 0.01)
			return 1.0;
		return pow(3.0 * dot(w, share) / total, 1.0 / 2.2);
	}
#endif

	vec3 env_saturation(vec3 rgb, float adjustment) {
	    vec3 intensity = vec3(dot(rgb, vec3(0.2126,0.7152,0.0722)));
	    return mix(intensity, rgb, adjustment);
	}

	void main (void)
	{
		vec4 diffuse_f;

#ifdef MIPFADE
		//FTESurf Patch 1218 (mat_vmt.c VMT_MipFade): the level the screen would pick at r_renderscale 1;
		//a supersampled buffer picks finer ones and moves the fade out by that factor.
		diffuse_f = texture2D(s_diffuse, tex_c, log2(max(abs(cvar_r_renderscale), 1.0)));
#else
		diffuse_f = SEAMSAMP(s_diffuse);
#endif
		//FTESurf Patch 1288: LightmappedGeneric multiplies by $color raw between an sRGB read and an sRGB write
		//(lightmappedgeneric_dx9_helper.cpp: no GammaToLinear, unlike VertexLitGeneric), so in linear light; on
		//the gamma values here that is ^(1/2.2). surf_eternity's $color .103 walls drew 3.5x too dark.
		diffuse_f.rgb *= e_colourident.rgb * pow(vec3(COLOR), vec3(1.0/2.2));

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

		//ftesurf (P232): the per-vertex tint and opacity the material asked for.
		//After the $alphatest discard for the same reason transition.glsl does it
		//there -- Source's alpha test reads the texture's alpha, and folding a
		//painted fade in first turns the faded end of the ramp into a hole.
#ifdef VERTEXCOL
		diffuse_f.rgb *= vex_color.rgb;
#endif
#ifdef VERTEXALPHA
		diffuse_f.a *= vex_color.a;
#endif

#ifdef FAKESHADOWS
		diffuse_f.rgb *= ShadowmapFilter(s_shadowmap, vtexprojcoord);
#endif

		diffuse_f.rgb *= lightmap_fragment();
#if defined(DELUXE) && defined(BUMP)
		diffuse_f.rgb *= rnm_fragment();	//FTESurf Patch 1264
#endif

/* CUBEMAPS ONLY */
#ifdef REFLECTCUBEMASK
	/* We currently only use the normal/bumpmap for cubemap warping. move this block out once we do proper radiosity normalmapping */
	#ifdef BUMP
		vec4 normal_f = SEAMSAMP(s_normalmap);
		#ifdef SSBUMP
		{	//FTESurf Patch 1264: three basis weights, and Source reflects off their sum (ps2_3_x.h:447)
			vec3 n = normalize(rnm_b0 * normal_f.r + rnm_b1 * normal_f.g + rnm_b2 * normal_f.b);
			normal_f.rgb = vec3(n.x, -n.y, n.z);	//into the green-flipped space below
		}
		#else
		/* Source's normalmaps are in the DX format where the green channel is flipped */
		normal_f.g = 1.0 - normal_f.g;
		normal_f.rgb = normalize(normal_f.rgb - 0.5);
		#endif
	#else
		vec4 normal_f = vec4(0.0,0.0,1.0,0.0);
	#endif


	//FTESurf Patch 268 B: Source's own mask defaults, under #ENVSRCMASK.
	//  no mask key at all      -> specularFactor stays 1.0.  vNormal's default is
	//                             float4(0,0,1,1) (lightmappedgeneric_ps2_3_x.h:205),
	//                             so even the bNormalMapAlphaEnvmapMask-without-a-
	//                             bumpmap arm at :396-399 reads 1.0
	//  $basealphaenvmapmask    -> 1.0 - blendedAlpha, i.e. 1.0 - diffuse_f.a here
	//                             (:408-411, "Reversing alpha blows!").  Source
	//                             inverts THIS one and only this one
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
					//ftesurf (P251): normal_f.a rather than a second fetch -- see
					//the note in the verbatim arm below.
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
			//ftesurf (P251): normal_f.a, not a second texture2D of the same
			//sampler.  Identical without $seamless_scale, and with it the
			//re-fetch would have used the material's UVs while the normal
			//itself came from the triplanar projection -- two different
			//texels from one map.  One fetch fewer, too.
			#if defined(ENVFROMNORM)
				#define refl normal_f.a
			#else
				#define refl 1.0 - normal_f.a
			#endif
		#endif
	#endif

	//FTESurf Patch 268 B: end of the ENVSRCMASK wrapper.
	#endif


		vec3 cube_c = reflect(-eyevector, normal_f.rgb);
		vec3 cube_tint = vec3(ENVTINT);
		vec3 cube_sat = vec3(ENVSAT);
		cube_c = cube_c.x * invsurface[0] + cube_c.y * invsurface[1] + cube_c.z * invsurface[2];
		cube_c = (m_model * vec4(cube_c.xyz, 0.0)).xyz;
	//FTESurf Patch 268 B: Source's envmap term, under #ENVSRCSPEC --
	//    spec  = cube * mask;
	//    spec *= tint;
	//    spec  = lerp(spec, spec*spec, contrast);
	//    spec  = lerp(luma(0.299,0.587,0.114), spec, saturation);
	//(lightmappedgeneric_ps2_3_x.h:553-561).  Today's order is the other way round --
	//saturation first, with Rec.709 weights, then the tint, and no contrast at all.
	//ENVSRCPOST is a no-op here: this shader already adds after the lightmap multiply
	//(:244), which is where Source adds it too.
	//`refl` is a macro that may expand to `1.0 - diffuse_f.a`, so it is always
	//spelled vec3(refl,refl,refl) here and never used as a bare factor.
	#ifdef ENVSRCSPEC
		vec3 spec = textureCube(s_reflectcube, cube_c).rgb * vec3(refl,refl,refl);
		spec *= cube_tint;
		spec = mix(spec, spec*spec, float(ENVCONTRAST));
		spec = mix(vec3(dot(spec, vec3(0.299,0.587,0.114))), spec, cube_sat.r);
		diffuse_f.rgb += spec;
	//FTESurf Patch 268 B: without #ENVSRCSPEC, today's term, verbatim.
	#else
		vec3 cube_t = env_saturation(textureCube(s_reflectcube, cube_c).rgb, cube_sat.r);
		cube_t.r *= cube_tint.r;
		cube_t.g *= cube_tint.g;
		cube_t.b *= cube_tint.b;
		diffuse_f.rgb += (cube_t * vec3(refl,refl,refl));
	//FTESurf Patch 268 B: end of the ENVSRCSPEC wrapper.
	#endif
#endif

	#ifdef FULLBRIGHT
		diffuse_f.rgb += texture2D(s_fullbright, tex_c).rgb * texture2D(s_fullbright, tex_c).a;
	#endif

		//FTESurf Patch 844: and the entity's renderamt, as Source's g_flAlpha2
		//(lightmappedgeneric_ps2_3_x.h:413-429).  1 on the world and an opaque entity.
		//Under $basealphaenvmapmask the base alpha is the mask, so a faded entity
		//takes its renderamt alone, as Source does.
	#ifdef ENVFROMBASE
		if (e_colourident.a < 1.0)
			diffuse_f.a = 1.0;
	#endif
		diffuse_f.a *= float(ALPHA) * e_colourident.a;

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
