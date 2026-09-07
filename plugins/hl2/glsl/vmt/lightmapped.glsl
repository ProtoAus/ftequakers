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
!!samps =FULLBRIGHT fullbright

// envmaps only
!!samps =REFLECTCUBEMASK reflectmask reflectcube

!!permu FAKESHADOWS
!!cvardf r_glsl_pcf
!!samps =FAKESHADOWS shadowmap

#ifndef ENVTINT
#define ENVTINT 1.0,1.0,1.0
#endif

#ifndef ENVSAT
#define ENVSAT 1.0,1.0,1.0
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

	vec3 env_saturation(vec3 rgb, float adjustment) {
	    vec3 intensity = vec3(dot(rgb, vec3(0.2126,0.7152,0.0722)));
	    return mix(intensity, rgb, adjustment);
	}

	void main (void)
	{
		vec4 diffuse_f;

		diffuse_f = SEAMSAMP(s_diffuse);
		diffuse_f.rgb *= e_colourident.rgb * vec3(COLOR);

#ifdef MASKLT
		if (diffuse_f.a < float(MASK))
			discard;
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

		/* deluxemapping isn't working on Source BSP yet */
		diffuse_f.rgb *= lightmap_fragment();

/* CUBEMAPS ONLY */
#ifdef REFLECTCUBEMASK
	/* We currently only use the normal/bumpmap for cubemap warping. move this block out once we do proper radiosity normalmapping */
	#ifdef BUMP
		/* Source's normalmaps are in the DX format where the green channel is flipped */
		vec4 normal_f = SEAMSAMP(s_normalmap);
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
	

		vec3 cube_c = reflect(-eyevector, normal_f.rgb);
		vec3 cube_tint = vec3(ENVTINT);
		vec3 cube_sat = vec3(ENVSAT);
		cube_c = cube_c.x * invsurface[0] + cube_c.y * invsurface[1] + cube_c.z * invsurface[2];
		cube_c = (m_model * vec4(cube_c.xyz, 0.0)).xyz;
		vec3 cube_t = env_saturation(textureCube(s_reflectcube, cube_c).rgb, cube_sat.r);
		cube_t.r *= cube_tint.r;
		cube_t.g *= cube_tint.g;
		cube_t.b *= cube_tint.b;
		diffuse_f.rgb += (cube_t * vec3(refl,refl,refl));
#endif

	#ifdef FULLBRIGHT
		diffuse_f.rgb += texture2D(s_fullbright, tex_c).rgb * texture2D(s_fullbright, tex_c).a;
	#endif

		diffuse_f.a *= float(ALPHA);

	#ifdef NOFOG
		gl_FragColor = diffuse_f;
	#else
		gl_FragColor = fog4(diffuse_f);
	#endif
	}
#endif
