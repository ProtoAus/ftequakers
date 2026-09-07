!!ver 110
!!permu FOG
!!samps diffuse lower fullbright

// ftesurf (P250): Source's $emissiveblend pass -- a self-illumination layer whose
// lookup is displaced by a flow map and then scrolled, which is what makes a rune
// circle appear to churn rather than merely slide.
//
// This is emissive_scroll_blended_pass_ps20b.fxc:31-48 exactly:
//
//     float4 cBaseColor  = tex2D( g_tBaseSampler,     i.vTexCoord0.xy );
//     float4 vFlowValue  = tex2D( g_tFlowSampler,     i.vTexCoord0.xy );
//     float2 vEmissiveTexCoord = vFlowValue.xy + ( g_vEmissiveScrollVector.xy * g_flTime );
//     float4 cEmissiveColor = tex2D( g_tSelfIllumSampler, vEmissiveTexCoord.xy );
//     result.rgb = cBaseColor.rgb * cEmissiveColor.rgb * g_cSelfIllumTint.rgb;
//     result.rgb *= g_flBlendStrength;
//     result.a = 0.0f;
//
// Note the flow map REPLACES the texture coordinate rather than offsetting it --
// vFlowValue.xy, not i.vTexCoord0.xy + vFlowValue.xy.  It is a lookup table into
// the emissive texture, not a perturbation of the surface's own UV, so the
// emissive texture is sampled in the flow map's space and the surface's own
// mapping does not tile it.  Getting that backwards is a plausible-looking
// mistake that produces a plausible-looking result, so it is written down.
//
// alpha 0 with blendFunc add is Source's own choice (the comment there reads "so
// it doesn't change dest alpha"); mat_vmt.c forces st->additive for this arm, so
// the progblendfunc in the shared tail supplies the ONE ONE.

// s_diffuse  = $emissiveblendbasetexture -- the shape, e.g. the circle artwork
// s_lower    = $emissiveblendflowtexture -- the dudv/flow lookup
// s_fullbright = $emissiveblendtexture   -- the thing that scrolls through it

#include "sys/defs.h"
#include "sys/fog.h"

#ifndef SCROLL
#define SCROLL 0.11,0.124	// kDefaultEmissiveScrollVector
#endif

#ifndef TINT
#define TINT 1.0,1.0,1.0	// kDefaultEmissiveTint. Not clamped: [10 0 0] is authored.
#endif

#ifndef STRENGTH
#define STRENGTH 1.0		// kDefaultEmissiveBlendStrength
#endif

varying vec2 tex_c;

#ifdef VERTEX_SHADER
void main ()
{
	tex_c = v_texcoord;
	gl_Position = ftetransform();
}
#endif

#ifdef FRAGMENT_SHADER
void main ()
{
	vec4 base_f = texture2D(s_diffuse, tex_c);
	vec4 flow_f = texture2D(s_lower, tex_c);

	vec2 emis_c = flow_f.xy + vec2(SCROLL) * e_time;
	vec4 emis_f = texture2D(s_fullbright, emis_c);

	vec4 result;
	result.rgb = base_f.rgb * emis_f.rgb * vec3(TINT) * float(STRENGTH);
	result.a = 0.0;

	gl_FragColor = fog4additive(result);
}
#endif
