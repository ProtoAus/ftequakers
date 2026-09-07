!!ver 100 150
!!permu FRAMEBLEND
!!permu BUMP
!!permu FOG
!!permu NOFOG
!!permu SKELETAL
!!permu FULLBRIGHT
!!permu AMBIENTCUBE
!!permu VC			// FTESurf Patch 259: VRAD's own per-vertex bake for THIS prop instance (sp_N.vhv), as a multiplier over `light`
!!permu REFLECTCUBEMASK
!!samps diffuse
!!samps =BUMP normalmap
!!samps =FULLBRIGHT fullbright
!!samps rtenvsphere:2D=0
!!permu FAKESHADOWS
!!cvardf r_glsl_pcf
!!cvardf r_glsl_rtenvsphere
!!samps =FAKESHADOWS shadowmap

// envmaps only
!!samps =REFLECTCUBEMASK reflectmask reflectcube
!!cvardf r_skipDiffuse

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


varying vec2 tex_c;
varying vec3 norm;
varying vec4 light;

/* CUBEMAPS ONLY */
#ifdef REFLECTCUBEMASK
	varying vec3 eyevector;
	varying mat3 invsurface;

#endif

#ifndef ENVTINT
#define ENVTINT 1.0,1.0,1.0
#endif

#ifndef ENVSAT
#define ENVSAT 1.0
#endif


#ifdef FAKESHADOWS
	varying vec4 vtexprojcoord;
#endif

#ifdef VERTEX_SHADER
	#include "sys/skeletal.h"

	float lambert(vec3 normal, vec3 dir)
	{
		return dot(normal, dir);
	}

	float halflambert(vec3 normal, vec3 dir)
	{
		return (dot(normal, dir) * 0.5) + 0.5;
	}

	void main (void)
	{
		vec3 n, s, t, w;
		tex_c = v_texcoord + e_time * vec2(SCROLL);
		gl_Position = skeletaltransform_wnst(w,n,s,t);
		norm = n = normalize(n);
		s = normalize(s);
		t = normalize(t);
		light.rgba = vec4(e_light_ambient, 1.0);

	#ifdef AMBIENTCUBE
		//no specular effect here. use rtlights for that.
		vec3 nn = norm*norm; //FIXME: should be worldspace normal.
		light.rgb = nn.x * e_light_ambientcube[(norm.x<0.0)?1:0] +
				nn.y * e_light_ambientcube[(norm.y<0.0)?3:2] +
				nn.z * e_light_ambientcube[(norm.z<0.0)?5:4];
	#else
		#ifdef HALFLAMBERT
			light.rgb += max(0.0,halflambert(n,e_light_dir)) * e_light_mul;
		#else
			light.rgb += max(0.0,dot(n,e_light_dir)) * e_light_mul;
		#endif
	#endif

	//FTESurf Patch 259.  v_colour carries VRAD's OWN per-vertex answer for this
	//exact prop instance, read out of the map's sp_N.vhv and normalised so its
	//brightest vertex is exactly 1.0 -- the gain that normalisation removed was
	//folded back into e_light_ambient, so this multiply restores the absolute
	//value rather than darkening the model.
	//
	//Greyscale on purpose (see VBSP_LoadPropBakedLight): light.rgb already
	//carries the prop's colour from the same bake's mean, so a per-channel
	//multiplier here would tint it twice.  This carries only where the light
	//falls -- which is the whole difference between a hedge and a green blob.
	//
	//Multiplied over the WHOLE of light, ambient and directional alike, the same
	//way defaultskin.glsl:115 does it.  In Patch 259's mode the directional half
	//is zero anyway: VRAD already integrated N.L into these numbers, so applying
	//ours on top would shade the prop twice.
	//
	//And the comment is // rather than /* */ deliberately: generatebuiltinsl
	//emits a line beginning /* raw and then QUOTES the lines after it, so a
	//multi-line block comment produces an unterminated C comment in
	//mat_vmt_progs.h and the plugin will not compile.  Every other comment in
	//these files is either // or a /* */ that opens and closes on one line.
	#ifdef VC
		light.rgb *= v_colour.rgb;
	#endif

/* CUBEMAPS ONLY */
#ifdef REFLECTCUBEMASK
		invsurface = mat3(s, t, n);

		vec3 eyeminusvertex = e_eyepos - w.xyz;
		eyevector.x = dot(eyeminusvertex, s.xyz);
		eyevector.y = dot(eyeminusvertex, t.xyz);
		eyevector.z = dot(eyeminusvertex, n.xyz);
#endif
		
		#ifdef FAKESHADOWS
		vtexprojcoord = (l_cubematrix*vec4(w.xyz, 1.0));
		#endif
	}
#endif


#ifdef FRAGMENT_SHADER
	#include "sys/fog.h"
	#include "sys/pcf.h"

	vec3 env_saturation(vec3 rgb, float adjustment) {
	    vec3 intensity = vec3(dot(rgb, vec3(0.2126,0.7152,0.0722)));
	    return mix(intensity, rgb, adjustment);
	}

	void main (void)
	{
		vec4 diffuse_f = texture2D(s_diffuse, tex_c);
		diffuse_f.rgb *= vec3(COLOR);

#ifdef MASKLT
		if (diffuse_f.a < float(MASK))
			discard;
#endif

/* Normal/Bumpmap Shenanigans */
#ifdef BUMP
		/* Source's normalmaps are in the DX format where the green channel is flipped */
		vec3 normal_f = texture2D(s_normalmap, tex_c).rgb;
		normal_f.g = 1.0 - normal_f.g;
		normal_f = normalize(normal_f.rgb - 0.5);
#else
		vec3 normal_f = vec3(0.0,0.0,1.0);
#endif

/* CUBEMAPS ONLY */
#ifdef REFLECTCUBEMASK

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
				#define refl texture2D(s_normalmap, tex_c).a
			#else
				#define refl 1.0 - texture2D(s_normalmap, tex_c).a
			#endif
		#endif
	#endif

		vec3 cube_tint = vec3(ENVTINT);
		vec3 cube_sat = vec3(ENVSAT);

	#if r_glsl_rtenvsphere == 1
		vec3 r = reflect(normalize(-eyevector), normal_f.rgb);
		vec2 sphereCoord = 0.5 + r.xy * 0.5;
		vec3 cube_t = env_saturation(texture2D(s_rtenvsphere, sphereCoord).rgb * 0.5, cube_sat.r);
	#else
		vec3 cube_c = reflect(-eyevector, normal_f.rgb);
		cube_c = cube_c.x * invsurface[0] + cube_c.y * invsurface[1] + cube_c.z * invsurface[2];
		cube_c = (m_model * vec4(cube_c.xyz, 0.0)).xyz;
		vec3 cube_t = env_saturation(textureCube(s_reflectcube, cube_c).rgb, cube_sat.r);
	#endif

		cube_t.r *= cube_tint.r;
		cube_t.g *= cube_tint.g;
		cube_t.b *= cube_tint.b;
		
		diffuse_f.rgb += (cube_t * vec3(refl,refl,refl));
#endif

		diffuse_f.rgb *= light.rgb * e_colourident.rgb;

	#ifdef FAKESHADOWS
		diffuse_f.rgb *= ShadowmapFilter(s_shadowmap, vtexprojcoord);
	#endif

	#ifdef FULLBRIGHT
		diffuse_f.rgb += texture2D(s_fullbright, tex_c).rgb * texture2D(s_fullbright, tex_c).a;
	#endif


		diffuse_f.a *= float(ALPHA);

	//FTESurf Patch 255: was #if 1, so the fog branch was dead and EVERY Source
	//prop drew unfogged. NOFOG was declared above and never used; animated,
	//lightmapped and transition all spell this test the same way. See P255.
	#ifdef NOFOG
		gl_FragColor = diffuse_f;
	#else
		gl_FragColor = fog4(diffuse_f);
	#endif
	}
#endif
