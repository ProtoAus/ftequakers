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
//FTESurf Patch 304: the ramp between the two lighting slots, and the OTHER HALF
//of mod_vbsp.c's hl2_lt_fold -- one cvar drives both so they cannot disagree.
//The "=1" is only the fallback for a build where the hl2 plugin never registered
//it; in FTESurf VBSP_Init has already done so by the time this compiles.
!!cvardf hl2_lt_fold=1
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
#endif
//FTESurf Patch 268 C: #BUMPCUBE needs invsurface too, to carry the normal map's
//tangent-space normal back to model space -- and it has to work on a bumped prop with
//no $envmap at all, which is exactly the prop that has no REFLECTCUBEMASK.
#if defined(REFLECTCUBEMASK) || defined(BUMPCUBE)
	varying mat3 invsurface;
#endif

#ifndef ENVTINT
#define ENVTINT 1.0,1.0,1.0
#endif

#ifndef ENVSAT
#define ENVSAT 1.0
#endif

//FTESurf Patch 268 B: $envmapcontrast.  mat_vmt.c has always parsed this key and
//then dropped it.  Source: specularLighting = lerp(s, s*s, g_EnvmapContrast)
//(vertexlit_and_unlit_generic_bump_ps20b.fxc:305-306), so 0.0 is "no contrast",
//which is today's picture -- an absent #ENVCONTRAST must default to exactly that.
#ifndef ENVCONTRAST
#define ENVCONTRAST 0.0
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
		//FTESurf Patch 304: half-lambert, because e_light_ambient is now the level
		//on the UNLIT side rather than the lit one (mod_vbsp.c, the split fold).
		//A plain lambert clamps flat at that base across the whole shadow
		//hemisphere while the true irradiance there keeps falling, so the two
		//choices are not interchangeable -- the fold and the ramp are one change
		//and are driven by one cvar.  Source uses half-lambert on models for the
		//same reason.  An undefined hl2_lt_fold evaluates to 0 here, which is the
		//legacy pair, so a missing define degrades to today rather than to a mix.
		#if defined(HALFLAMBERT) || hl2_lt_fold
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
//FTESurf Patch 268 C: invsurface for #BUMPCUBE as well; see its varying.
#if defined(REFLECTCUBEMASK) || defined(BUMPCUBE)
		invsurface = mat3(s, t, n);
#endif
#ifdef REFLECTCUBEMASK

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
	//FTESurf Patch 268 C: #BUMPGREENFIX drops this flip.  Source never flips G -- its bump
	//shaders take normalTexel*2-1 as it is and carry handedness on the binormal sign,
	//cross(N,T)*T.w -- and mod_hl2.c builds FTE's tangent basis from the same VVD tangent in
	//the same way, so on a model this flip mirrors the relief along V relative to Source.
	#ifndef BUMPGREENFIX
		normal_f.g = 1.0 - normal_f.g;
	#endif
		normal_f = normalize(normal_f.rgb - 0.5);
#else
		vec3 normal_f = vec3(0.0,0.0,1.0);
#endif

		//FTESurf Patch 268 B: the reflection contribution now lives in `spec`.
		//Declared OUT HERE, not inside the block below, so that the ENVSRCPOST add
		//site under the lighting multiply still compiles when REFLECTCUBEMASK is
		//off -- there it stays vec3(0.0) and the add is a no-op.
		vec3 spec = vec3(0.0);

/* CUBEMAPS ONLY */
#ifdef REFLECTCUBEMASK

	//FTESurf Patch 268 B: Source's own mask defaults, under #ENVSRCMASK.
	//  no mask key at all      -> specularFactor stays 1.0
	//                             (vertexlit_and_unlit_generic_bump_ps20b.fxc:195;
	//                              the non-bump and world shaders reach the same
	//                              value through vNormal's float4(0,0,1,1) default,
	//                              lightmappedgeneric_ps2_3_x.h:205)
	//  $basealphaenvmapmask    -> 1.0 - baseColor.a.  Source inverts THIS one and
	//                             only this one (vertexlit_and_unlit_generic_ps20b
	//                             .fxc:332, "this blows!")
	//  $normalmapalphaenvmapmask -> the normalmap's alpha, uninverted, and only when
	//                             there is a normalmap to read it from
	#ifdef ENVSRCMASK
		#if defined(ENVFROMMASK) && !defined(BUMP)
			//A dedicated reflectmask -- and Source honours one only on an UNBUMPED
			//VertexLitGeneric: vertexlitgeneric_dx9_helper.cpp:187-199 SetUndefined()s
			//$envmapmask the moment a bumpmap is present, and the bumped fxc declares
			//no ENVMAPMASK combo at all.  A bumped material falls through below.
			#define refl texture2D(s_reflectmask, tex_c).r
		#else
			#if defined(ENVFROMBASE) && !defined(BUMP)
				//Likewise $basealphaenvmapmask, which the same helper CLEAR_FLAGS()es
				//when a bumpmap is present -- and which is the ONE mask Source inverts
				//(vertexlit_and_unlit_generic_ps20b.fxc:332, "this blows!").
				#define refl 1.0 - diffuse_f.a
			#else
				#if defined(ENVFROMNORM) && defined(BUMP)
					#define refl texture2D(s_normalmap, tex_c).a
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
				#define refl texture2D(s_normalmap, tex_c).a
			#else
				#define refl 1.0 - texture2D(s_normalmap, tex_c).a
			#endif
		#endif
	#endif

	//FTESurf Patch 268 B: end of the ENVSRCMASK wrapper.
	#endif

		vec3 cube_tint = vec3(ENVTINT);
		vec3 cube_sat = vec3(ENVSAT);

	//FTESurf Patch 268 B: Source's envmap term, under #ENVSRCSPEC --
	//    spec  = cube * mask;
	//    spec *= tint;
	//    spec  = lerp(spec, spec*spec, contrast);
	//    spec  = lerp(luma(0.299,0.587,0.114), spec, saturation);
	//(vertexlit_and_unlit_generic_bump_ps20b.fxc:302-308).  Today's order is the
	//other way round -- saturation first, with Rec.709 weights, then the tint, and
	//no contrast at all.  The rtenvsphere sample takes the same post-processing.
	//`refl` is a macro that may expand to `1.0 - diffuse_f.a`, so it is always
	//spelled vec3(refl,refl,refl) here and never used as a bare factor.
	#ifdef ENVSRCSPEC
		#if r_glsl_rtenvsphere == 1
		vec3 r = reflect(normalize(-eyevector), normal_f.rgb);
		vec2 sphereCoord = 0.5 + r.xy * 0.5;
		spec = texture2D(s_rtenvsphere, sphereCoord).rgb * 0.5;
		#else
		vec3 cube_c = reflect(-eyevector, normal_f.rgb);
		cube_c = cube_c.x * invsurface[0] + cube_c.y * invsurface[1] + cube_c.z * invsurface[2];
		cube_c = (m_model * vec4(cube_c.xyz, 0.0)).xyz;
		spec = textureCube(s_reflectcube, cube_c).rgb;
		#endif
		spec *= vec3(refl,refl,refl);
		spec *= cube_tint;
		spec = mix(spec, spec*spec, float(ENVCONTRAST));
		spec = mix(vec3(dot(spec, vec3(0.299,0.587,0.114))), spec, cube_sat.r);
	//FTESurf Patch 268 B: without #ENVSRCSPEC, today's term, verbatim.
	#else

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
		
		spec = cube_t * vec3(refl,refl,refl);

	//FTESurf Patch 268 B: end of the ENVSRCSPEC wrapper.
	#endif

	//FTESurf Patch 268 B: without #ENVSRCPOST the reflection lands exactly where it
	//always has -- before the lighting multiply, so the prop's own light dims it.
	#ifndef ENVSRCPOST
		diffuse_f.rgb += spec;
	#endif
#endif

	//FTESurf Patch 268 C: per-pixel relief, as a ratio over the lighting the prop already has.
	//Source lights a bumped VertexLitGeneric per pixel, from the leaf's six-face ambient cube
	//and the BUMPED world-space normal (common_vertexlitgeneric_dx9.h).  FTE lights it per
	//vertex, and where the prop has VRAD's .vhv bake that vertex light carries the level and
	//the shadowing a cube cannot -- so keep it, and multiply in only what the bump changes,
	//    cube(n_bumped) / cube(n_vertex)
	//in LINEAR light (e_light_ambientcube is linear), taken to display space with the ~2.2
	//gamma the vertex light already went through.  A flat normal gives exactly 1.0, and so
	//does an empty cube (eps/eps) -- which r_cubelight 0 uploads and every entity without a
	//cube carries -- so off changes nothing to the bit.  Clamped, so a face the cube calls
	//black cannot divide the ratio into a blow-out.
		vec3 lightrgb = light.rgb;
	#if defined(BUMPCUBE) && defined(BUMP)
		#ifndef CUBEEPS
		#define CUBEEPS 0.0005
		#endif
		vec3 nb = normalize(normal_f.x * invsurface[0] + normal_f.y * invsurface[1] + normal_f.z * invsurface[2]);
		nb = normalize((m_model * vec4(nb, 0.0)).xyz);
		vec3 ng = normalize((m_model * vec4(normalize(norm), 0.0)).xyz);
		vec3 nb2 = nb * nb;
		vec3 ng2 = ng * ng;
		vec3 cb = nb2.x * ((nb.x < 0.0) ? e_light_ambientcube[1] : e_light_ambientcube[0]) +
		          nb2.y * ((nb.y < 0.0) ? e_light_ambientcube[3] : e_light_ambientcube[2]) +
		          nb2.z * ((nb.z < 0.0) ? e_light_ambientcube[5] : e_light_ambientcube[4]);
		vec3 cg = ng2.x * ((ng.x < 0.0) ? e_light_ambientcube[1] : e_light_ambientcube[0]) +
		          ng2.y * ((ng.y < 0.0) ? e_light_ambientcube[3] : e_light_ambientcube[2]) +
		          ng2.z * ((ng.z < 0.0) ? e_light_ambientcube[5] : e_light_ambientcube[4]);
		lightrgb *= clamp(pow((cb + vec3(CUBEEPS)) / (cg + vec3(CUBEEPS)), vec3(1.0/2.2)), 0.0, 4.0);
	#endif
		diffuse_f.rgb *= lightrgb * e_colourident.rgb;

	#ifdef FAKESHADOWS
		diffuse_f.rgb *= ShadowmapFilter(s_shadowmap, vtexprojcoord);
	#endif

	//FTESurf Patch 268 B: Source adds the envmap AFTER lighting and shadowing --
	//`result = diffuseComponent + specularLighting`, diffuseComponent being
	//albedo*lighting (vertexlit_and_unlit_generic_bump_ps20b.fxc:268,312).  No
	//e_colourident on it: the entity's colormod modulates the albedo, not the
	//reflection.  With REFLECTCUBEMASK off `spec` is vec3(0.0) and this is a no-op.
	#ifdef ENVSRCPOST
		diffuse_f.rgb += spec;
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
		//FTESurf Patch 299: fog the COLOUR, and leave the alpha to the blender.
		//
		//fog4() is `vec4(fog3(rgb), 1.0) * regularcolour.a` (gl_vidcommon.c:1887)
		//-- it multiplies the fogged colour by the surface's own alpha.  That is
		//the correct arithmetic for a PREMULTIPLIED blend and the wrong one for
		//every material this plugin emits.
		//
		//Source does not treat base-texture alpha as opacity unless the material
		//says so.  On an opaque VertexLitGeneric it is a MASK -- $basealphaenvmapmask,
		//$selfillummask, $basemapalphaphongmask -- and the surface is fully solid.
		//With no fog nothing shows, because an opaque blend discards the alpha
		//entirely; the moment fog is on, that mask multiplies every pixel.
		//
		//Measured on surf_tensor2's 3D skybox at Lex's own save011 window vantage,
		//mean base-texture alpha of the eight building materials in frame:
		//   hill_cluster 0.030   gov_upscale 0.028   antenna 0.094
		//   long_building001a 0.920 (min 0.035)   buildingsheet_03a 0.963 (min 0.000)
		//   project_building02 0.984   project_building03 0.969   industrial 0.960
		//A clean bimodal split, and it matches the screen object for object: the
		//0.03 props drew at 3% of the fog colour -- a black cutout against a sky
		//that was itself correctly fogged, which is the reported "bright black"
		//buildings -- and the low-alpha WINDOW PANES inside the 0.92-0.96 textures
		//are the "black windows" half of the same report.  Under a white fog forced
		//to end at 1 unit, where saturation is 255, those props measured
		//137.7 147.4 163.3 with std 38.5; that std IS the alpha mask showing through.
		//
		//There is no premultiplied path here to protect.  mat_vmt.c:3719 and :3885
		//emit `src_alpha one_minus_src_alpha`, so GL already applies the alpha and
		//fog4() was applying it a SECOND time -- translucent Source surfaces have
		//been doubly darkened by their own alpha on every fogged map as well.
		//$alphatest is a discard, so its kept fragments were darkened by whatever
		//alpha survived the mask rather than drawn at full strength.  Additive keeps
		//fog4additive and water keeps fog4blend; neither ever multiplied, and
		//neither is touched.
		//
		//hl2_fog_alphamul 1 restores fog4() exactly, bit-for-bit.
		#if #include "cvar/hl2_fog_alphamul"
			gl_FragColor = fog4(diffuse_f);
		#else
			gl_FragColor = vec4(fog3(diffuse_f.rgb), diffuse_f.a);
		#endif
	#endif
	}
#endif
