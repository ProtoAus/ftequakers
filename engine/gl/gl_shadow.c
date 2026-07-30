#include "quakedef.h"

/*
room for improvement:
There is no screen-space culling of lit surfaces.
model meshes are interpolated multiple times per frame
*/


#if defined(RTLIGHTS) && !defined(SERVERONLY)

#ifdef VKQUAKE
#include "../vk/vkrenderer.h"
#endif
#include "glquake.h"
#include "shader.h"

#ifdef D3D9QUAKE
#include "shader.h"
#if !defined(HMONITOR_DECLARED) && (WINVER < 0x0500)
    #define HMONITOR_DECLARED
    DECLARE_HANDLE(HMONITOR);
#endif
#include <d3d9.h>
extern LPDIRECT3DDEVICE9 pD3DDev9;
void D3D9BE_Cull(unsigned int sflags);
void D3D9BE_RenderShadowBuffer(unsigned int numverts, IDirect3DVertexBuffer9 *vbuf, unsigned int numindicies, IDirect3DIndexBuffer9 *ibuf);
#endif
#ifdef D3D11QUAKE
void D3D11BE_GenerateShadowBuffer(void **vbuf, vecV_t *verts, int numverts, void **ibuf, index_t *indicies, int numindicies);
void D3D11BE_RenderShadowBuffer(unsigned int numverts, void *vbuf, unsigned int numindicies, void *ibuf);
void D3D11_DestroyShadowBuffer(void *vbuf, void *ibuf);
void D3D11BE_DoneShadows(void);
#endif
#ifdef VKQUAKE
#endif
void GLBE_RenderShadowBuffer(unsigned int numverts, int vbo, vecV_t *verts, unsigned numindicies, int ibo, index_t *indicies);

static void SHM_Shutdown(void);

#define SHADOWMAP_SIZE 512

#define PROJECTION_DISTANCE (float)(sh_shmesh->radius*2)//0x7fffffff

#ifdef BEF_PUSHDEPTH
extern qboolean r_pushdepth;
#endif

texid_t crepuscular_texture_id;
fbostate_t crepuscular_fbo;
shader_t *crepuscular_shader;

cvar_t r_shadow_shadowmapping_nearclip = CVAR("r_shadow_shadowmapping_nearclip", "1");
cvar_t r_shadow_shadowmapping_bias = CVAR("r_shadow_shadowmapping_bias", "0.03");
cvar_t r_shadow_scissor = CVARD("r_shadow_scissor", "1", "constrains stencil shadows to the onscreen box that contains the maxmium extents of the light. This avoids unnecessary work.");

cvar_t r_shadow_realtime_world				= CVARFD ("r_shadow_realtime_world", "0", CVAR_ARCHIVE, "Enables the use of static/world realtime lights.");
cvar_t r_shadow_realtime_world_shadows		= CVARF ("r_shadow_realtime_world_shadows", "1", CVAR_ARCHIVE);
cvar_t r_shadow_realtime_world_lightmaps	= CVARFD ("r_shadow_realtime_world_lightmaps", "0", 0, "Specifies how much of the map's normal lightmap to retain when using world realtime lights. 0 completely replaces lighting.");
float r_shadow_realtime_world_lightmaps_force;
cvar_t r_shadow_realtime_world_importlightentitiesfrommap = CVARFD ("r_shadow_realtime_world_importlightentitiesfrommap", "0", CVAR_ARCHIVE, "Controls default loading of map-based realtime lights.\n0: Load explicit .rtlight files only.\n1: Load explicit lights then try fallback to parsing the entities lump.\n2: Load only the entities lump.");
cvar_t r_shadow_realtime_dlight				= CVARAFD ("r_shadow_realtime_dlight", "1", "r_shadow_realtime_dynamic", CVAR_ARCHIVE, "Enables the use of dynamic realtime lights, allowing explosions to use bumpmaps etc properly.");
cvar_t r_shadow_realtime_dlight_shadows		= CVARFD ("r_shadow_realtime_dlight_shadows", "1", CVAR_ARCHIVE, "Allows dynamic realtime lights to cast shadows as they move.");
cvar_t r_shadow_realtime_dlight_ambient		= CVAR ("r_shadow_realtime_dlight_ambient", "0");
cvar_t r_shadow_realtime_dlight_diffuse		= CVAR ("r_shadow_realtime_dlight_diffuse", "1");
cvar_t r_shadow_realtime_dlight_specular	= CVAR ("r_shadow_realtime_dlight_specular", "4");	//excessive, but noticable. its called stylized, okay? shiesh, some people
cvar_t r_shadow_playershadows				= CVARD ("r_shadow_playershadows", "1", "Controls the presence of shadows on the local player.");
//nettest: the first-person VIEWMODEL sits INSIDE the local player's body, and with
//r_shadow_playershadows 1 that body IS rendered into the fake-sun depth map — so the gun
//visibly receives its own owner's shadow.  FAKESHADOWS is a GLOBAL compile-time define
//(gl_shader.c), so the shader cannot tell it is the viewmodel; this cvar instead drives the
//per-entity `e_noshadowrecv` uniform (uploaded in gl_backend.c, consumed by the gamedir
//glsl/defaultskin.glsl self-shadow term).  Runtime — no vid_reload needed (unlike the cvardf knobs).
cvar_t r_shadows_viewmodel					= CVARD ("r_shadows_viewmodel", "0", "Whether the first-person viewmodel RECEIVES the r_shadows 2 fake-sun shadow map. 0 = no (default; stops your own hidden first-person body from casting a shadow onto your gun). 1 = yes (legacy behaviour).");
cvar_t r_shadow_raytrace					= CVARFD ("r_shadow_raytrace", "0", CVAR_ARCHIVE, "Enables use of hardware raytracing for shadows. Consider also using with r_halfrate.");
cvar_t r_shadow_shadowmapping				= CVARFD ("r_shadow_shadowmapping", "1", CVAR_ARCHIVE, "Enables soft shadows instead of stencil shadows.");
cvar_t r_shadow_shadowmapping_precision		= CVARD ("r_shadow_shadowmapping_precision", "1", "Scales the shadowmap detail level up or down.");
static cvar_t r_shadow_shadowmapping_depthbits		= CVARD ("r_shadow_shadowmapping_depthbits", "16", "Shadowmap depth bits. 16, 24, or 32.");
//nettest: default is the ericw-tools LIGHT default sun (worldspawn "_sun_mangle" "230 -65 0"
//-> toward-sun), so the r_shadows 2 fake shadows fall the same way as the baked lightmap on
//maps without an env_sun (env_sun stuffs this per map when present).
//nettest: CVAR_SHADERSYSTEM so a per-map sun change (env_sun stuff) flushes shaders and re-injects
//the e_fakesundir #define used by the model sun-shade in defaultskin.glsl (no staleness across maps).
//nettest: the ericw-tools LIGHT default sun, _sun_mangle "230 -65 0" (= yaw 230 / pitch -65), so the
//dynamic sun agrees with the baked lightmap out of the box.  Mangle is YAW-PITCH-ROLL with +sin(pitch)
//and points the way the light TRAVELS, so toward-sun = -(cos y cos p, sin y cos p, sin p) =
//(0.271654, 0.323744, 0.906308) = azimuth 50, elevation 65.  (Was "-0.271654 0.582563 0.766045" =
//azimuth 115/elev 50 — the mangle wrongly read as Quake pitch=230/yaw=-65 through AngleVectors.)
cvar_t r_sun_dir							= CVARFD("r_sun_dir", "0.271654 0.323744 0.906308", CVAR_SHADERSYSTEM, "Direction toward the sun (the r_shadows 2 fake shadows are thrown opposite this unless r_shadows_throwdirection is set; also drives the model sun-shade).");
cvar_t r_sun_colour							= CVARFD ("r_sun_colour", "0 0 0", CVAR_ARCHIVE, "Specifies the colour of sunlight that appears in the form of crepuscular rays.");
cvar_t r_sun_occludedepth					= CVARFD ("r_sun_occludedepth", "0.65", CVAR_ARCHIVE, "Crepuscular god rays are depth-occluded by geometry NEARER than this window-depth (0..1), so the near first-person viewmodel blocks the rays while the farther scene still glows. Raise if the gun still shows rays; lower if near walls stop glowing. 0 = rays over everything.");

//nettest: console names de-"fake"d (r_shadows_distance/res/bias); the old fake* names
//survive as silent aliases via CVARAFD so stuffed cfgs / muscle memory keep working.
static cvar_t r_shadows_distance			= CVARAFD("r_shadows_distance", "1024", "r_shadows_fakedistance", 0, "Coverage radius (qu) of the r_shadows 2 shadow volume around the view. Smaller = sharper (denser texels), larger = shadows reach further.");
//nettest: default is the NEGATED r_sun_dir default (angled, not straight down) so sunless maps
//get a natural sun that doesn't graze vertical walls (top-down light shimmered coplanar func_ brushes).
static cvar_t r_shadows_throwdirection		= CVARD("r_shadows_throwdirection", "-0.271654 -0.323744 -0.906308", "The direction to throw the r_shadows 2 fake shadows in (opposite r_sun_dir). Empty = use -r_sun_dir.");
static cvar_t r_shadows_focus				= CVARD("r_shadows_focus", "0 0 0", "Offset for the center of the fake-shadows volume.");
//nettest: shadowmap texture resolution (the stock path hardcoded SHADOWMAP_SIZE*4=2048).
//Higher = crisper shadows at the same r_shadows_distance coverage (16bit depth: 4096 ~ 32MB).
static cvar_t r_shadows_res					= CVARAFD("r_shadows_res", "2048", "r_shadows_fakeres", 0, "r_shadows 2 depth map resolution. Higher = sharper at the same r_shadows_distance; 2048 = classic.");
//nettest: world-constant contact bias (qu) baked into the shadow projection (gl_backend.c
//ortho branch).  Replaces pcf.h's old radius-SCALED 0.015 NDC bias (~15qu at distance 1024),
//which cut shadows off well before their caster touched the ground.  Raise if shadow acne
//appears on steep surfaces; lower for even tighter contact.
cvar_t r_shadows_bias						= CVARD("r_shadows_bias", "2", "r_shadows 2 shadow depth bias in world units. Lower = shadows hug contact points tighter; higher = kills acne on steep surfaces.");
//nettest: contact-shadow gap fade (consumed in glsl sys/pcf.h FAKESHADOWS path).  The single
//global ortho shadow map has no world occluder, so a caster on an upper floor leaks its shadow
//onto the floor below.  This keeps a receiver shadowed only where its occluder is within this
//fraction of the ortho depth ([0,1] = 2*r_shadows_distance qu; 0.06 ~= 123 qu at distance 1024).
//Higher = shadows reach further (more leak); lower = tighter contact-only; 0 disables the fade.
cvar_t r_shadows_throwfade					= CVARD("r_shadows_throwfade", "0.06", "r_shadows 2 contact-shadow gap fade: fraction of the ortho shadow depth beyond which a caster stops shadowing (stops shadows leaking through floors). Higher = longer reach; 0 = off.");
//nettest Phase-0 (caster sun-visibility gate) -------------------------------------------------------
//The fake-SUN pass renders EVERY caster into the sun map regardless of whether the sun actually lights
//it, so a prop/player standing in baked shade still stamps a hard sun shadow onto nearby props (and
//double-darkens baked shade).  This gate drops a caster from the sun map when its dominant BAKED light
//direction (the same per-entity deluxemap dir Patch 108 uses to shade it) disagrees with the sun by more
//than the cutoff -- i.e. it is lit by a lamp / in shade, so the sun is not its shadow-caster.  Compares
//dot(toward-dominant-light, toward-sun): ~1 = sunlit (cast), low = lamp-lit/shade (skip).  Fail-safe: no
//deluxemap -> no dir -> treated as sunlit (still casts).  0 = off (classic: everyone casts).
cvar_t r_shadows_caster_sunvis				= CVARD("r_shadows_caster_sunvis", "0", "r_shadows 2: a caster whose dominant baked light is more than this far (dot cutoff, 0..1) from the sun is treated as lamp-lit/in-shade and does NOT cast a sun shadow onto others. 0 = off (DEFAULT since Patch 120: it deletes a shadow and puts nothing in its place, and its single origin-relative probe flipped as a player walked or ducked). Higher = more casters excluded; needs a deluxemap (light -bspxlux).");
//nettest P110 (multi-direction fake shadows) --------------------------------------------------------
//One ortho projection carries ONE parallel cast direction, so plain r_shadows 2 throws every prop's
//shadow along the sun even when a nearby lamp is what actually lights it.  These split the depth map
//into N cells, each rendered from a different DOMINANT LIGHT direction (the same per-entity direction
//Patch 108 already uses to SHADE the prop), and assign every caster to exactly one cell.
//CVAR_SHADERSYSTEM: the receiving shaders size uniform/varying arrays from FAKESHADOWS_COUNT, so a
//count change has to recompile them.
cvar_t r_shadows_slots						= CVARFD("r_shadows_slots", "1", CVAR_SHADERSYSTEM, "r_shadows 2 cast directions. 1 = classic single sun shadow. 2-8 = props also shadow away from nearby lamps. Non-sun cells are FITTED to the props in them, so they are SHARPER than the sun cell despite being a third of its width, and their box test early-outs for most pixels.");
static cvar_t r_shadows_slots_hyst			= CVARD("r_shadows_slots_hyst", "2.5", "How much more popular a light direction must be before it steals an occupied r_shadows_slots slot. Higher = stickier (less shadow popping), lower = more responsive.");
//A slot's RENDERED direction is the running mean of the props actually in it, eased over time -- not the
//quantised lattice centre used to ASSIGN them.  That split is the point: the lattice keeps membership
//stable (a static prop's bucket is bit-constant, which is what killed Patch 93 when it wasn't), while the
//direction stays continuous so shadows glide the way the sunshade does instead of snapping between the
//64 lattice directions.
static cvar_t r_shadows_slots_smooth		= CVARD("r_shadows_slots_smooth", "0.25", "Seconds for a cast direction to ease onto its target. 0 = snap instantly (quantised, poppy); higher = smoother but laggier transitions.");
//Non-sun cells cover only their own props, so their ortho box is fitted to those props' bounds plus this
//margin -- the room the shadow needs to actually reach the floor.  Small box over few casters = far more
//texels per prop than the shared view-sized box, AND the shader's box test can early-out for most pixels.
static cvar_t r_shadows_slots_margin		= CVARD("r_shadows_slots_margin", "256", "World units of room added around a fitted cast-shadow cell for the shadow to land in. Too low = shadows cut off short; too high = wastes the cell's resolution.");
static cvar_t r_shadows_slots_pcf			= CVARFD("r_shadows_slots_pcf", "4", CVAR_SHADERSYSTEM, "PCF taps for the non-sun r_shadows_slots directions (the sun keeps 9). 4 = cheaper secondary shadows; 9 = uniform quality. The single biggest cost dial when r_shadows_slots > 1.");
static cvar_t r_shadows_slots_debug			= CVARD("r_shadows_slots_debug", "0", "Print the chosen r_shadows_slots directions and their caster counts each frame. Use this to confirm slot assignment is STABLE (static props must never migrate between slots).");
//---------------------------------------------------------------------------------------------------
//nettest P114 (sun cascades) -----------------------------------------------------------------------
//The single r_shadows 2 sun map spends its whole resolution on ONE view-sized box, so a player 30qu
//away and the skyline 1500qu away share the same texel density -- 1 texel/qu at the defaults, ~32
//texels across a whole player.  Cascades split the SUN (not, like slots, into other directions but)
//into N nested boxes fitted to successive view-depth slices: the near cascade covers only the first
//few hundred units at a FRACTION of a texel per qu, the far one reaches to r_shadows_cascade_dist.
//That is the CS2 "sharp up close AND long range" trade the single map can't make.
//
//Shares ALL of P110's atlas plumbing (per-cell projection matrix, cell rect, the shader's per-cell
//sample loop); only the cell CONTENTS differ -- every cascade is the same sun direction, and every
//caster renders into every cascade (no per-caster slot filter).  The shader picks the TIGHTEST
//cascade that contains each pixel, so nested boxes never double-darken (which is why this is a
//distinct FAKESHADOWS_CASCADE shader path, not P110's additive one).
//
//Mutually exclusive with r_shadows_slots: slots re-key the atlas by DIRECTION, cascades by DEPTH,
//and they can't both own the cells.  slots > 1 wins (lamp shadows were the opt-in feature); cascades
//apply only at slots == 1.  cascades == 1 is the legacy single map, bit-identical.
//CVAR_SHADERSYSTEM for the same reason as r_shadows_slots: it sets FAKESHADOWS_COUNT (array sizes).
#define SH_MAX_CASCADES 4	/*the sun-cascade cell grid (Sh_CascadeCellRect) is 2x2 = four cells*/
cvar_t r_shadows_cascades					= CVARFD("r_shadows_cascades", "1", CVAR_SHADERSYSTEM, "r_shadows 2 sun cascade count (only when r_shadows_slots is 1). 1 = classic single map. 2-4 = sharp near shadows AND long range; the near cascade packs many more texels per qu than the single map. Ignored while r_shadows_slots > 1.");
static cvar_t r_shadows_cascade_dist		= CVARD("r_shadows_cascade_dist", "2048", "Radius (qu) of the OUTERMOST sun cascade around the camera. Inner cascades are r_shadows_cascade_ratio smaller each; bigger = longer shadow range but each cascade covers more ground (less dense).");
static cvar_t r_shadows_cascade_ratio		= CVARD("r_shadows_cascade_ratio", "3", "Size step between adjacent sun cascades: each inner cascade is 1/this the radius of the next one out. Higher = a tighter/denser near cascade but a bigger jump in resolution between cascades.");
static cvar_t r_shadows_cascade_debug		= CVARD("r_shadows_cascade_debug", "0", "Print each sun cascade's radius and texel density (texels/qu) once per second. Use it to confirm the near cascade is actually denser than the single map.");
//---------------------------------------------------------------------------------------------------
//nettest Phase-1 (per-prop PERSPECTIVE shadows) ----------------------------------------------------
//The Source/CS2 "dominant local light" model.  A prop lit by a room lamp casts ONE shadow FROM that lamp,
//rendered as a spotlight PERSPECTIVE projection through the prop -- so it swings to any angle AND grows as
//the prop nears the lamp (the two things the ortho slots/cascades physically cannot do), while darkening
//only (all baked lighting is preserved).  Each qualifying prop gets its own atlas cell.  slots==1 only.
//CVAR_SHADERSYSTEM: r_shadows_propshadows(_max) set FAKESHADOWS_COUNT / FAKESHADOWS_PERSP_FIRST.
cvar_t r_shadows_propshadows				= CVARFD("r_shadows_propshadows", "1", CVAR_SHADERSYSTEM, "r_shadows 2: props cast a PERSPECTIVE shadow from their nearest map light -- swings 360 and grows as it nears the lamp, darkening only. Needs a deluxemap (light -bspxlux) + point-light entities. Requires r_shadows_slots 1. 0 = off. (nettest ships this ON; the whole feature is inert on maps with no point lights.)");
cvar_t r_shadows_propshadows_cone			= CVARD("r_shadows_propshadows_cone", "120", "Prop shadows: cone angle (degrees, full width) used by a light that does NOT set its own \"angle\"/\"_cone\" spot key. The cone is FIXED per light -- aimed by \"mangle\"/\"target\", or straight down when the light declares neither -- so projections never re-aim or re-zoom as props move through them. Narrower = sharper (the cell's pixels cover less ground) but a prop outside the cone gets no shadow.");
cvar_t r_shadows_propshadows_switch			= CVARD("r_shadows_propshadows_switch", "2", "Prop shadows: how much brighter-per-distance a lamp must be than the one already shadowing a prop before it takes over (1 = no stickiness, switch on every tie; 2 = the challenger must score twice as high). Stops a prop's shadow snapping to a different lamp as you walk across two lamps' iso-surface.");
//nettest: default was 13, which is actively counter-productive.  sh_propsub_budget is sized from
//THIS value (not the live cell count -- Patch 120, deliberately, so resolution never steps), and
//Sh_PropSubCellRect picks quarter-size subcells only while `nsub <= (4-nc)*4`.  At the shipped
//r_shadows_cascades 2 that threshold is 8, so a budget of 13 fails it and EVERY lamp shadow is
//allocated an eighth-size cell -- 256px of a 2048 atlas instead of 512px, a quarter of the pixel
//area.  Measured on fy_killzone: 8 vs 13 costs nothing (442.18 vs 453.16us Shadow generation,
//identical draw calls) and quadruples lamp shadow resolution.  8 is also >= any live cell count
//seen on the mod's maps (6 map lights -> 5 cells), so no cell is ever lost to the lower budget.
//NOTE: with r_shadows_cascades 3 the threshold drops to 4, so this would need to be <=4 there.
cvar_t r_shadows_propshadows_max			= CVARFD("r_shadows_propshadows_max", "8", CVAR_SHADERSYSTEM, "Max simultaneous LIGHT cells for prop shadows (each cell = one lamp cone covering every prop near it; on-screen-nearest lamps win, with hysteresis). Shares the 16-slot atlas with the sun cells: cascades 3 leaves up to 13, cascades 1 up to 15. More live cells = smaller per-cell resolution once past 4 per free quadrant (raise r_shadows_res to compensate).");
static cvar_t r_shadows_propshadows_range	= CVARD("r_shadows_propshadows_range", "1.5", "How far a map light reaches to own a prop's shadow, as a multiple of its brightness value (matches the light-swing/blob reach). Higher = distant lamps still shadow props.");
static cvar_t r_shadows_propshadows_bias	= CVARD("r_shadows_propshadows_bias", "0.0015", "Depth bias for the per-prop perspective shadow (clip-z units, nudged toward the lamp). Raise to kill self-shadow acne on the caster; lower to hug contact tighter.");
static cvar_t r_shadows_propshadows_debug	= CVARD("r_shadows_propshadows_debug", "0", "Print the per-prop perspective shadow assignments (lamp count, prop->lamp distance/fov) -- confirm assignment is stable. 2 = FORCE mode: bypass the sun-vs-lamp classification so any prop near a lamp casts (to verify the projection).");
static cvar_t r_shadows_propshadows_showatlas = CVARD("r_shadows_propshadows_showatlas", "0", "Draw the fake-shadow depth atlas as an on-screen overlay: see the sun cascade quadrants, the lamp subcells, and what each cone actually captured. 1 = corner thumbnail, 2 = big. Depth shown as brightness (empty/far = black).");
//Sunlight-aware lamp shadows: where a lamp's shadow lands on a SUN-VISIBLE surface the sun re-lights it,
//so the shadow should wash out -- by the lamp-vs-sun brightness ratio (a floodlight keeps its shadow on
//sunlit ground; a candle loses it).  Engine computes suppress = Bsun/(Bsun+Blamp) per LIGHT CELL and the
//receiver multiplies it by the pixel's own baked sun visibility -- so the shadow fades exactly across the
//sun/shade boundary.  Needs a SUNVIS bake (without one the factor is forced 0 = feature inert).
static cvar_t r_shadows_sunbrightness		= CVARD("r_shadows_sunbrightness", "250", "The map sun's brightness in the same units as point-light `light` values (worldspawn _sunlight). env_sun stuffs the real per-map value; this is the fallback. Used to wash lamp shadows out on sunlit surfaces (r_shadows_propshadows_sunmask).");
static cvar_t r_shadows_propshadows_sunmask	= CVARD("r_shadows_propshadows_sunmask", "1", "How strongly SUNLIT surfaces wash out lamp shadows: 0 = off (lamp shadows stamp onto sunlit ground at full strength), 1 = physical lamp-vs-sun brightness ratio, >1 = harsher washout. Needs a SUNVIS bake (light -sunvis).");
/*non-static (Patch 120): gl_shader.c mirrors this cvar's halving of the lamp-cell budget into
  FAKESHADOWS_COUNT -- the two clamps MUST agree or the shader loops over cells the engine cannot fill.
  Patch 120b: hence CVAR_SHADERSYSTEM.  It was a plain CVARD, so toggling it changed the ENGINE's live
  budget immediately while leaving the compiled loop bounds stale until the next vid_reload -- which
  silently invalidated every A/B test of the mask itself.*/
cvar_t r_shadows_propshadows_worldmask = CVARFD("r_shadows_propshadows_worldmask", "1", CVAR_SHADERSYSTEM, "Lamp shadows stop at walls: each lamp cell carries a paired WORLD-depth render and a pixel only receives the shadow when no world surface sits between it and the lamp (first-surface-only, the modern-Source projected-texture fix). Costs half the lamp-cell budget + one cached depth draw per lamp. 0 = old behaviour (shadows project through walls; full budget).");
//In-shade classification for prop shadows: a prop whose BAKED sun visibility at its origin is >= this is
//treated as sunlit (the sun already shadows it) and does NOT get a lamp shadow; below it the prop is in the
//sun's shadow and is a candidate.  Uses the SUNVIS lump (true occlusion) -- far better than the sun-biased
//deluxemap DIRECTION, which stays sun-leaning even in shade.  No SUNVIS bake => falls back to the deluxemap
//dot (r_shadows_caster_sunvis), so un-baked maps are unchanged.
static cvar_t r_shadows_propshadows_sunvis	= CVARD("r_shadows_propshadows_sunvis", "0.5", "r_shadows 2 propshadows: a prop whose baked sun visibility at its origin is >= this (0..1, 1=fully sunlit) is treated as sunlit and gets NO lamp shadow; below it the prop is in shade and casts from its lamp. Needs a SUNVIS bake (light -sunvis); without one it falls back to r_shadows_caster_sunvis.");
//Patch 120b: in-shade classification WITHOUT any bake.  Both of the gates above need a lump the map may
//not carry (SUNVIS, or a deluxemap for the r_shadows_caster_sunvis dot), and on a map with neither the
//verdict stayed "unknown" -- so a player standing under a roof kept a full sun shadow while ALSO getting
//its lamp shadow, which is exactly the "the sun atlas still masks me indoors" report.  A trace along the
//sun direction answers the same question exactly, from geometry, on every map: if it stops on a solid
//world surface there is a roof overhead.  Sky brushes are deliberately NOT solid to this trace (q1bsp
//maps CONTENTS_SKY to FTECONTENTS_SKY, which MASK_WORLDSOLID does not include) so open air reads sunlit.
//Patch 120c: how long the model's SUN form-shade and self-shadow take to cross-fade as a caster moves
//between sun and shade.  Everything else in this system is anti-chatter hysteresis, which only DELAYS
//a flip -- this is the only thing that makes one gradual.
static cvar_t r_shadows_sunfade				= CVARD("r_shadows_sunfade", "0.25", "Seconds for a model's SUN form-shade and sun self-shadow to cross-fade as it moves between sunlight and shade (time constant, not a hard duration). 0 = instant, the pre-Patch-120c behaviour. Does not affect lamp shadows, which stay at full strength indoors.");
//Patch 120c: with the fade above in place, an in-shade caster no longer has to LEAVE the sun cascades
//to look right -- its sun shading is faded on the receive side instead -- so it can keep casting its
//sun shadow onto the world indoors.  On a map with a SUNVIS bake the receiving floor erases that
//shadow by itself; on a map without one it will not, which is the accepted trade for this being on.
static cvar_t r_shadows_propshadows_suncast	= CVARD("r_shadows_propshadows_suncast", "1", "r_shadows 2 propshadows: 1 = a caster stays in the sun cascades even when it is indoors, so its sun shadow keeps landing on the world (its own sun form-shade and self-shadow fade out via r_shadows_sunfade instead). 0 = the Patch 120a behaviour, where an in-shade caster is dropped from the sun cascades entirely. NOTE: on a map with no SUNVIS bake nothing erases an indoor sun shadow, so 1 will stamp one on indoor floors.");
static cvar_t r_shadows_propshadows_suntrace	= CVARD("r_shadows_propshadows_suntrace", "1", "r_shadows 2 propshadows: classify a caster as in-shade by TRACING toward the sun when the map has no SUNVIS/deluxemap bake to answer with. Costs one extra world trace per caster per frame and needs no compile flags. 0 = old behaviour (an unbaked map cannot classify, so casters keep both a sun shadow and a lamp shadow).");
//---------------------------------------------------------------------------------------------------

static void Sh_DrawEntLighting(dlight_t *light, vec3_t colour, qbyte *pvs);

static pvsbuffer_t	lvisb, lvisb2;

/*
called on framebuffer resize.
flushes textures so they can be regenerated at the real size
*/
void Sh_Reset(void)
{
#ifdef GLQUAKE
	if (crepuscular_texture_id)
	{
		Image_DestroyTexture(crepuscular_texture_id);
		crepuscular_texture_id = r_nulltex;
	}
	GLBE_FBO_Destroy(&crepuscular_fbo);
#endif
}
void Sh_Shutdown(void)
{
	Sh_Reset();
	SHM_Shutdown();
}



typedef struct {
	unsigned int count;
	unsigned int faceidxcount;
	unsigned int faceidxfirst;
	unsigned int max;
	texture_t *tex;
	vbo_t *vbo;
	mesh_t **s;
} shadowmeshbatch_t;
typedef struct shadowmesh_s
{
	vec3_t	origin;
	float	radius;
	enum
	{
		SMT_STENCILVOLUME,	//build edges mesh (and surface list)
		SMT_SHADOWMAP,		//build front faces mesh (and surface list)
		SMT_ORTHO,			//bounded by a box and with a single direction rather than an origin.
		SMT_SHADOWLESS,		//build vis+surface list only
		SMT_DEFERRED		//build vis without caring about any surfaces at all.
	} type;
	unsigned int numindicies;
	unsigned int maxindicies;
	index_t *indicies;

	unsigned int numverts;
	unsigned int maxverts;
	vecV_t *verts;

	//we also have a list of all the surfaces that this light lights.
	unsigned int numbatches;
	shadowmeshbatch_t *batches;

	unsigned int leafbytes;
	unsigned char *litleaves;

#ifdef VKQUAKE
	struct vk_shadowbuffer *vkbuffer;
#endif
#ifdef GLQUAKE
	GLuint vefbo[3];
	qboolean havefaceebo;
#endif
#ifdef D3D9QUAKE
	IDirect3DVertexBuffer9	*d3d9_vbuffer;
	IDirect3DIndexBuffer9	*d3d9_ibuffer;
#endif
#ifdef D3D11QUAKE
	void	*d3d11_vbuffer;
	void	*d3d11_ibuffer;
#endif
} shadowmesh_t;

/*state of the current shadow mesh*/
#define inc 128
int sh_shadowframe;
static int sh_firstindex;
static int sh_vertnum;		//vertex number (set to 0 at SH_Begin)
static shadowmesh_t *sh_shmesh, sh_tempshmesh;

/* functions to add geometry to the shadow mesh */
static void SHM_BeginQuads (void)
{
	sh_firstindex = sh_shmesh->numverts;
}
static void SHM_End (void)
{
	int i;
	i = (sh_shmesh->numindicies+(sh_vertnum/4)*6+inc+5)&~(inc-1);	//and a bit of padding
	if (sh_shmesh->maxindicies != i)
	{
		sh_shmesh->maxindicies = i;
		sh_shmesh->indicies = BZ_Realloc(sh_shmesh->indicies, i * sizeof(*sh_shmesh->indicies));
	}
	//add the extra triangles
	for (i = 0; i < sh_vertnum; i+=4)
	{
		sh_shmesh->indicies[sh_shmesh->numindicies++] = sh_firstindex + i+0;
		sh_shmesh->indicies[sh_shmesh->numindicies++] = sh_firstindex + i+1;
		sh_shmesh->indicies[sh_shmesh->numindicies++] = sh_firstindex + i+2;

		sh_shmesh->indicies[sh_shmesh->numindicies++] = sh_firstindex + i+0;
		sh_shmesh->indicies[sh_shmesh->numindicies++] = sh_firstindex + i+2;
		sh_shmesh->indicies[sh_shmesh->numindicies++] = sh_firstindex + i+3;
	}
	sh_vertnum = 0;
}
static void SHM_Vertex3fv (const float *v)
{
	int i;

//add the verts as we go
	i = (sh_shmesh->numverts+inc+5)&~(inc-1);	//and a bit of padding
	if (sh_shmesh->maxverts < i)
	{
		sh_shmesh->maxverts = i;
		sh_shmesh->verts = BZ_Realloc(sh_shmesh->verts, i * sizeof(*sh_shmesh->verts));
	}

	sh_shmesh->verts[sh_shmesh->numverts][0] = v[0];
	sh_shmesh->verts[sh_shmesh->numverts][1] = v[1];
	sh_shmesh->verts[sh_shmesh->numverts][2] = v[2];

	sh_vertnum++;
	sh_shmesh->numverts++;


	if (sh_vertnum == 4)
	{
		SHM_End();
		sh_firstindex = sh_shmesh->numverts;
	}
}

static void SHM_MeshFrontOnly(int numverts, vecV_t *verts, int numidx, index_t *idx)
{
	int first = sh_shmesh->numverts;
	int v, i;
	vecV_t *outv;
	index_t *outi;

	/*make sure there's space*/
	v = (sh_shmesh->numverts+numverts + inc)&~(inc-1);	//and a bit of padding
	if (sh_shmesh->maxverts < v)
	{
		v *= 2;
		v += 1024;
		sh_shmesh->maxverts = v;
		sh_shmesh->verts = BZ_Realloc(sh_shmesh->verts, v * sizeof(*sh_shmesh->verts));
	}

	outv = sh_shmesh->verts + sh_shmesh->numverts;
	for (v = 0; v < numverts; v++)
	{
		VectorCopy(verts[v], outv[v]);
	}

	v = (sh_shmesh->numindicies+numidx + inc)&~(inc-1);	//and a bit of padding
	if (sh_shmesh->maxindicies < v)
	{
		v *= 2;
		v += 1024;
		sh_shmesh->maxindicies = v;
		sh_shmesh->indicies = BZ_Realloc(sh_shmesh->indicies, v * sizeof(*sh_shmesh->indicies));
	}
	outi = sh_shmesh->indicies + sh_shmesh->numindicies;
	for (i = 0; i < numidx; i++)
	{
		outi[i] = first + idx[i];
	}

	sh_shmesh->numverts += numverts;
	sh_shmesh->numindicies += numidx;
}
#if 0
static void SHM_MeshBackOnly(int numverts, vecV_t *verts, int numidx, index_t *idx)
{
	int first = sh_shmesh->numverts;
	int v, i;
	vecV_t *outv;
	index_t *outi;

	/*make sure there's space*/
	v = (sh_shmesh->numverts+numverts + inc)&~(inc-1);	//and a bit of padding
	if (sh_shmesh->maxverts < v)
	{
		v += 1024;
		sh_shmesh->maxverts = v;
		sh_shmesh->verts = BZ_Realloc(sh_shmesh->verts, v * sizeof(*sh_shmesh->verts));
	}

	outv = sh_shmesh->verts + sh_shmesh->numverts;
	for (v = 0; v < numverts; v++)
	{
		VectorCopy(verts[v], outv[v]);
	}

	v = (sh_shmesh->numindicies+numidx + inc)&~(inc-1);	//and a bit of padding
	if (sh_shmesh->maxindicies < v)
	{
		v += 1024;
		sh_shmesh->maxindicies = v;
		sh_shmesh->indicies = BZ_Realloc(sh_shmesh->indicies, v * sizeof(*sh_shmesh->indicies));
	}
	outi = sh_shmesh->indicies + sh_shmesh->numindicies;
	for (i = 0; i < numidx; i+=3)
	{
		outi[i+0] = first + idx[i+2];
		outi[i+1] = first + idx[i+1];
		outi[i+2] = first + idx[i+0];
	}

	sh_shmesh->numverts += numverts;
	sh_shmesh->numindicies += numidx;
}
#endif
static void SHM_TriangleFan(int numverts, vecV_t *verts, vec3_t lightorg, float pd)
{
	int v, i, idxs;
	float *v1;
	vec3_t v3;
	vecV_t *outv;
	index_t *outi;

	/*make sure there's space*/
	v = (sh_shmesh->numverts+numverts*2 + inc)&~(inc-1);	//and a bit of padding
	if (sh_shmesh->maxverts < v)
	{
		v += 1024;
		sh_shmesh->maxverts = v;
		sh_shmesh->verts = BZ_Realloc(sh_shmesh->verts, v * sizeof(*sh_shmesh->verts));
	}
	outv = sh_shmesh->verts + sh_shmesh->numverts;

	for (v = 0; v < numverts; v++)
	{
		v1 = verts[v];
		VectorCopy(v1, outv[v]);

		v3[0] = ( v1[0]-lightorg[0] )*pd;
		v3[1] = ( v1[1]-lightorg[1] )*pd;
		v3[2] = ( v1[2]-lightorg[2] )*pd;

		outv[v+numverts][0] = v1[0]+v3[0];
		outv[v+numverts][1] = v1[1]+v3[1];
		outv[v+numverts][2] = v1[2]+v3[2];
	}

	idxs = (numverts-2)*3;
	/*now add the verts in a fan*/
	v = (sh_shmesh->numindicies+idxs*2+inc)&~(inc-1);	//and a bit of padding
	if (sh_shmesh->maxindicies < v)
	{
		v += 1024;
		sh_shmesh->maxindicies = v;
		sh_shmesh->indicies = BZ_Realloc(sh_shmesh->indicies, v * sizeof(*sh_shmesh->indicies));
	}
	outi = sh_shmesh->indicies + sh_shmesh->numindicies;

	for (v = 2, i = 0; v < numverts; v++, i+=3)
	{
		outi[i+0] = sh_shmesh->numverts;
		outi[i+1] = sh_shmesh->numverts+v-1;
		outi[i+2] = sh_shmesh->numverts+v;

		outi[i+0+idxs] = sh_shmesh->numverts+numverts+v;
		outi[i+1+idxs] = sh_shmesh->numverts+numverts+v-1;
		outi[i+2+idxs] = sh_shmesh->numverts+numverts;
	}

	/*we added this many*/
	sh_shmesh->numverts += numverts*2;
	sh_shmesh->numindicies += i*2;
}

static void SHM_Shadow_Cache_Surface(msurface_t *surf)
{
	int i;

	i = surf->sbatch->user.bmodel.shadowbatch;
	if (i < 0)
		return;

	if (sh_shmesh->batches[i].count == sh_shmesh->batches[i].max)
	{
		sh_shmesh->batches[i].max += 64;
		sh_shmesh->batches[i].s = BZ_Realloc(sh_shmesh->batches[i].s, sizeof(void*)*(sh_shmesh->batches[i].max));
	}
	sh_shmesh->batches[i].s[sh_shmesh->batches[i].count] = surf->mesh;
	sh_shmesh->batches[i].count++;
	sh_shmesh->batches[i].faceidxcount += surf->mesh->numindexes;
}

static void SHM_Shadow_Cache_Leaf(mleaf_t *leaf)
{
	int i;

	i = (leaf - cl.worldmodel->leafs)-1;
	sh_shmesh->litleaves[i>>3] |= 1<<(i&7);
}

static void SH_FreeShadowMesh_(shadowmesh_t *sm)
{
	unsigned int i;
	for (i = 0; i < sm->numbatches; i++)
		Z_Free(sm->batches[i].s);
	sm->numbatches = 0;
	Z_Free(sm->batches);
	sm->batches = NULL;
	Z_Free(sm->indicies);
	sm->indicies = NULL;
	Z_Free(sm->verts);
	sm->verts = NULL;
	sm->numindicies = 0;
	sm->numverts = 0;

	switch (qrenderer)
	{
	case QR_NONE:
	case QR_SOFTWARE:
	default:
		break;

#ifdef GLQUAKE
	case QR_OPENGL:
		if (qglDeleteBuffersARB)
			qglDeleteBuffersARB(3, sm->vefbo);
		sm->vefbo[0] = 0;
		sm->vefbo[1] = 0;
		sm->vefbo[2] = 0;
		sm->havefaceebo = false;
		break;
#endif
#ifdef VKQUAKE
	case QR_VULKAN:
		VKBE_DestroyShadowBuffer(sm->vkbuffer);
		sm->vkbuffer = NULL;
		break;
#endif
#ifdef D3D9QUAKE
	case QR_DIRECT3D9:
		if (sm->d3d9_ibuffer)
			IDirect3DIndexBuffer9_Release(sm->d3d9_ibuffer);
		sm->d3d9_ibuffer = NULL;
		if (sm->d3d9_vbuffer)
			IDirect3DVertexBuffer9_Release(sm->d3d9_vbuffer);
		sm->d3d9_vbuffer = NULL;
		break;
#endif
#ifdef D3D11QUAKE
	case QR_DIRECT3D11:
		D3D11_DestroyShadowBuffer(sm->d3d11_vbuffer, sm->d3d11_ibuffer);
		sm->d3d11_vbuffer = NULL;
		sm->d3d11_ibuffer = NULL;
		break;
#endif
	}
}
void SH_FreeShadowMesh(shadowmesh_t *sm)
{
	SH_FreeShadowMesh_(sm);
	Z_Free(sm);
}

static void SH_CalcShadowBatches(model_t *mod)
{
	int s;
	batch_t *b;
	batch_t *l = NULL;
	int sb;

	l = NULL;
	for (s = 0; s < SHADER_SORT_COUNT; s++)
	{
		for (b = mod->batches[s]; b; b = b->next)
		{
			if (!l || l->vbo != b->vbo || l->texture != b->texture)
			{
				b->user.bmodel.shadowbatch = mod->numshadowbatches++;
				l = b;
			}
			else
				b->user.bmodel.shadowbatch = l->user.bmodel.shadowbatch;
		}
	}

	if (!mod->numshadowbatches)
		mod->shadowbatches = NULL;
	else
	{
		l = NULL;
		sb = 0;
		mod->shadowbatches = BZ_Malloc(sizeof(*mod->shadowbatches)*mod->numshadowbatches);
		for (s = 0; s < SHADER_SORT_COUNT; s++)
		{
			for (b = mod->batches[s]; b; b = b->next)
			{
				if (!l || l->vbo != b->vbo || l->texture != b->texture)
				{
					mod->shadowbatches[sb].tex = b->texture;
					mod->shadowbatches[sb].vbo = b->vbo;
					sb++;
					l = b;
				}
			}
		}
	}
}

static void SHM_BeginShadowMesh(dlight_t *dl, int type)
{
	unsigned int i;
	unsigned int lb;
	sh_vertnum = 0;

	lb = (cl.worldmodel->numclusters+7)/8;
	if (!dl->die || !dl->key || (dl->flags&LFLAG_FORCECACHE))
	{
		sh_shmesh = dl->worldshadowmesh;
		if (!sh_shmesh || sh_shmesh->leafbytes != lb)
		{
			/*this shouldn't happen too often*/
			if (sh_shmesh)
			{	//FIXME: if the light is the same light, reuse the memory allocations where possible...
				SH_FreeShadowMesh(sh_shmesh);
			}

			/*Create a new shadowmesh for this light*/
			sh_shmesh = Z_Malloc(sizeof(*sh_shmesh) + lb);
			sh_shmesh->leafbytes = lb;
			sh_shmesh->litleaves = (unsigned char*)(sh_shmesh+1);

			dl->worldshadowmesh = sh_shmesh;
		}
		memset(sh_shmesh->litleaves, 0, sh_shmesh->leafbytes);
		dl->rebuildcache = false;
	}
	else
	{
		sh_shmesh = &sh_tempshmesh;
		if (sh_shmesh->leafbytes != lb)
		{
			/*this happens on map changes*/
			sh_shmesh->leafbytes = lb;
			Z_Free(sh_shmesh->litleaves);
			sh_shmesh->litleaves = Z_Malloc(lb);
		}
	}
#ifdef GLQUAKE
	sh_shmesh->havefaceebo = false;
#endif
	sh_shmesh->maxverts = 0;
	sh_shmesh->numverts = 0;
	sh_shmesh->maxindicies = 0;
	sh_shmesh->numindicies = 0;
	sh_shmesh->type = type;
	VectorCopy(dl->origin, sh_shmesh->origin);
	sh_shmesh->radius = dl->radius;

	if (!cl.worldmodel->numshadowbatches)
	{
		SH_CalcShadowBatches(cl.worldmodel);
	}

	if (sh_shmesh->numbatches != cl.worldmodel->numshadowbatches)
	{
		if (sh_shmesh->batches)
		{
			for (i = 0; i < sh_shmesh->numbatches; i++)
				Z_Free(sh_shmesh->batches[i].s);
			Z_Free(sh_shmesh->batches);
		}
		sh_shmesh->batches = Z_Malloc(sizeof(shadowmeshbatch_t)*cl.worldmodel->numshadowbatches);
		sh_shmesh->numbatches=cl.worldmodel->numshadowbatches;
	}

	for (i = 0; i < sh_shmesh->numbatches; i++)
	{
		sh_shmesh->batches[i].count = 0;
		sh_shmesh->batches[i].faceidxcount = 0;
	}
}
#ifdef GLQUAKE
static size_t SHM_GenWorldFaceIndexes(index_t **outindexes)
{
	size_t count = 0, b, m, i;
	index_t *out = *outindexes = NULL;
	mesh_t *surf;
	shadowmeshbatch_t *batch;
	size_t tmp;

	if (sh_shmesh == &sh_tempshmesh)
	{
		for (b = 0; b < sh_shmesh->numbatches; b++)
			sh_shmesh->batches[b].faceidxcount = 0;

		*outindexes = 0;
		return 0;
	}

	for (b = 0; b < sh_shmesh->numbatches; b++)
		count+= sh_shmesh->batches[b].faceidxcount;
	out = *outindexes = (void*fte_restrict)BZ_Malloc(count * sizeof(*out));

	for (b = 0, batch = sh_shmesh->batches; b < sh_shmesh->numbatches; b++, batch++)
	{
		batch->faceidxfirst = out-*outindexes;
		for (m = 0; m < batch->count; m++)
		{
			surf = batch->s[m];
			for (i = 0; i < surf->numindexes; i++)
			{
				tmp = surf->vbofirstvert + surf->indexes[i];
				if (tmp > MAX_INDICIES)
					Sys_Error("Too many indexes\n");
				*out++ = surf->vbofirstvert + surf->indexes[i];
			}
		}
	}
	return count;
}
#endif
static struct shadowmesh_s *SHM_FinishShadowMesh(dlight_t *dl)
{
	if (sh_shmesh != &sh_tempshmesh || 1)
	{
		switch (qrenderer)
		{
		case QR_NONE:
		case QR_SOFTWARE:
		default:
			break;

#ifdef GLQUAKE
		case QR_OPENGL:
			if (!qglGenBuffersARB)
				return sh_shmesh;
			{	//generate a per-face buffer.
				index_t *faceindexes;
				size_t faceindexcount = SHM_GenWorldFaceIndexes(&faceindexes);

				if (!sh_shmesh->vefbo[0])
					qglGenBuffersARB(3, sh_shmesh->vefbo);

				GL_DeselectVAO();
				GL_SelectVBO(sh_shmesh->vefbo[0]);
				qglBufferDataARB(GL_ARRAY_BUFFER_ARB, sizeof(*sh_shmesh->verts) * sh_shmesh->numverts, sh_shmesh->verts, GL_STATIC_DRAW_ARB);

				if (faceindexes)
				{
					GL_SelectEBO(sh_shmesh->vefbo[2]);
					qglBufferDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, sizeof(*faceindexes) * faceindexcount, faceindexes, GL_STATIC_DRAW_ARB);
					BZ_Free(faceindexes);
					sh_shmesh->havefaceebo = true;
				}

				GL_SelectEBO(sh_shmesh->vefbo[1]);
				qglBufferDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, sizeof(*sh_shmesh->indicies) * sh_shmesh->numindicies, sh_shmesh->indicies, GL_STATIC_DRAW_ARB);
			}
			break;
#endif
#ifdef VKQUAKE
		case QR_VULKAN:
			VKBE_DestroyShadowBuffer(sh_shmesh->vkbuffer);
			sh_shmesh->vkbuffer = VKBE_GenerateShadowBuffer(sh_shmesh->verts, sh_shmesh->numverts, sh_shmesh->indicies, sh_shmesh->numindicies, sh_shmesh == &sh_tempshmesh);
			break;
#endif
#ifdef D3D9QUAKE
		case QR_DIRECT3D9:
			if (sh_shmesh->numindicies && sh_shmesh->numverts)
			{
				void *map;
				IDirect3DDevice9_CreateIndexBuffer(pD3DDev9, sizeof(index_t) * sh_shmesh->numindicies, 0, D3DFMT_QINDEX, D3DPOOL_MANAGED, &sh_shmesh->d3d9_ibuffer, NULL);
				IDirect3DIndexBuffer9_Lock(sh_shmesh->d3d9_ibuffer, 0, sizeof(index_t) * sh_shmesh->numindicies, &map, D3DLOCK_DISCARD);
				memcpy(map, sh_shmesh->indicies, sizeof(index_t) * sh_shmesh->numindicies);
				IDirect3DIndexBuffer9_Unlock(sh_shmesh->d3d9_ibuffer);

				IDirect3DDevice9_CreateVertexBuffer(pD3DDev9, sizeof(vecV_t) * sh_shmesh->numverts, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &sh_shmesh->d3d9_vbuffer, NULL);
				IDirect3DVertexBuffer9_Lock(sh_shmesh->d3d9_vbuffer, 0, sizeof(vecV_t) * sh_shmesh->numverts, &map, D3DLOCK_DISCARD);
				memcpy(map, sh_shmesh->verts, sizeof(vecV_t) * sh_shmesh->numverts);
				IDirect3DVertexBuffer9_Unlock(sh_shmesh->d3d9_vbuffer);

			}
			break;
#endif
#ifdef D3D11QUAKE	
		case QR_DIRECT3D11:
			D3D11BE_GenerateShadowBuffer(&sh_shmesh->d3d11_vbuffer, sh_shmesh->verts, sh_shmesh->numverts, &sh_shmesh->d3d11_ibuffer, sh_shmesh->indicies, sh_shmesh->numindicies);
			break;
#endif
		}

		Z_Free(sh_shmesh->verts);
		sh_shmesh->verts = NULL;

		Z_Free(sh_shmesh->indicies);
		sh_shmesh->indicies = NULL;
	}
	return sh_shmesh;
}


/*state of the world that is still to compile*/
static struct {
	short count;
	short count2;
	int next;
	int prev;
} *edge;
static int firstedge;
static int maxedge;
static void (*genshadowmapcallback) (msurface_t *mesh);

static void SHM_RecursiveWorldNodeQ1_r (dlight_t *dl, mnode_t *node)
{
	int			c, side;
	mplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	double		dot;

	float		l, maxdist;
	int			j, s, t;
	vec3_t		impact;

	if (node->shadowframe != sh_shadowframe)
		return;

	if (node->contents == Q1CONTENTS_SOLID)
		return;		// solid


	//if light areabox is outside node, ignore node + children
	for (c = 0; c < 3; c++)
	{
		if (dl->origin[c] + dl->radius < node->minmaxs[c])
			return;
		if (dl->origin[c] - dl->radius > node->minmaxs[3+c])
			return;
	}

// if a leaf node, draw stuff
	if (node->contents < 0)
	{
		pleaf = (mleaf_t *)node;
		SHM_Shadow_Cache_Leaf(pleaf);

		if (sh_shmesh->type == SMT_DEFERRED)	//such rtlights don't need ANY surface info, just a tight pvs
			return;

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				(*mark++)->shadowframe = sh_shadowframe;
			} while (--c);
		}
		return;
	}

// node is just a decision point, so go down the apropriate sides

// find which side of the node we are on
	plane = node->plane;

	switch (plane->type)
	{
	case PLANE_X:
		dot = dl->origin[0] - plane->dist;
		break;
	case PLANE_Y:
		dot = dl->origin[1] - plane->dist;
		break;
	case PLANE_Z:
		dot = dl->origin[2] - plane->dist;
		break;
	default:
		dot = DotProduct (dl->origin, plane->normal) - plane->dist;
		break;
	}

	if (dot >= 0)
		side = 0;
	else
		side = 1;

// recurse down the children, front side first
	SHM_RecursiveWorldNodeQ1_r (dl, node->children[side]);

// draw stuff
  	c = node->numsurfaces;

	if (c)
	{
		surf = cl.worldmodel->surfaces + node->firstsurface;

		{

			maxdist = dl->radius*dl->radius;

			for ( ; c ; c--, surf++)
			{
				if (surf->shadowframe != sh_shadowframe)
					continue;

//				if ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK))
//					continue;		// wrong side

//				if (surf->flags & SURF_PLANEBACK)
//					continue;

				if (surf->flags & (SURF_DRAWALPHA | SURF_DRAWTILED))
				{	// no shadows
					continue;
				}

				//is the light on the right side?
				if (surf->flags & SURF_PLANEBACK)
				{//inverted normal.
					if (-DotProduct(surf->plane->normal, dl->origin)+surf->plane->dist >= dl->radius)
						continue;
				}
				else
				{
					if (DotProduct(surf->plane->normal, dl->origin)-surf->plane->dist >= dl->radius)
						continue;
				}

				//Yeah, you can blame LordHavoc for this alternate code here.
				for (j=0 ; j<3 ; j++)
					impact[j] = dl->origin[j] - surf->plane->normal[j]*dot;

				// clamp center of light to corner and check brightness
				l = DotProduct (impact, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3] - surf->texturemins[0];
				s = l+0.5;if (s < 0) s = 0;else if (s > surf->extents[0]) s = surf->extents[0];
				s = (l - s)*surf->texinfo->vecscale[0];
				l = DotProduct (impact, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3] - surf->texturemins[1];
				t = l+0.5;if (t < 0) t = 0;else if (t > surf->extents[1]) t = surf->extents[1];
				t = (l - t)*surf->texinfo->vecscale[1];
				// compare to minimum light
				if ((s*s+t*t+dot*dot) < maxdist)
					genshadowmapcallback(surf);
			}
		}
	}

// recurse down the back side
	SHM_RecursiveWorldNodeQ1_r (dl, node->children[!side]);
}

#ifdef Q1BSPS
void CategorizePlane ( mplane_t *plane );
static void SHM_OrthoWorldLeafsQ1 (dlight_t *dl)
{
	int			c, i;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf, *plastleaf;
	float dot;

	mplane_t orthoplanes[5];

	sh_shadowframe++;

	VectorCopy(dl->axis[0], orthoplanes[0].normal);
	VectorNegate(dl->axis[0], orthoplanes[1].normal);
	VectorCopy(dl->axis[1], orthoplanes[2].normal);
	VectorNegate(dl->axis[1], orthoplanes[3].normal);
	VectorNegate(dl->axis[0], orthoplanes[4].normal);

	for (i = 0; i < countof(orthoplanes); i++)
	{
		orthoplanes[i].dist = DotProduct(dl->origin, orthoplanes[i].normal) - dl->radius;
		CategorizePlane(&orthoplanes[i]);
	}

	for (pleaf = cl.worldmodel->leafs+1, plastleaf = cl.worldmodel->leafs+cl.worldmodel->submodels[0].visleafs; pleaf <= plastleaf; pleaf++)
	{
		for (i = 0; i < countof(orthoplanes); i++)
			if (BOX_ON_PLANE_SIDE (pleaf->minmaxs, pleaf->minmaxs+3, &orthoplanes[i]) == 2)
				goto next;

		SHM_Shadow_Cache_Leaf(pleaf);

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		while (c --> 0)
		{
			surf = *mark++;

			if (surf->flags & (SURF_DRAWALPHA | SURF_DRAWTILED | SURF_DRAWSKY))
				continue;

			if (surf->shadowframe != sh_shadowframe)
			{
				surf->shadowframe = sh_shadowframe;

				dot = DotProduct(surf->plane->normal, dl->axis[0]);
				if (surf->flags & SURF_PLANEBACK)
					dot = -dot;
			
				if (dot < 0)
				{
					SHM_Shadow_Cache_Surface(surf);
				}
				SHM_MeshFrontOnly(surf->mesh->numvertexes, surf->mesh->xyz_array, surf->mesh->numindexes, surf->mesh->indexes);
			}
		}

next:;
	}
}

static void SHM_MarkLeavesQ1(dlight_t *dl, const qbyte *lvis)
{
	mnode_t *node;
	int i;
	sh_shadowframe++;

	if (!lvis)
		return;

	//variation on mark leaves
	for (i=0 ; i<cl.worldmodel->numclusters ; i++)
	{
		if (lvis[i>>3] & (1<<(i&7)))
		{
			node = (mnode_t *)&cl.worldmodel->leafs[i+1];
			do
			{
				if (node->shadowframe == sh_shadowframe)
					break;
				node->shadowframe = sh_shadowframe;
				node = node->parent;
			} while (node);
		}
	}
}

void Q1BSP_GenerateShadowMesh(model_t *model, dlight_t *dl, const qbyte *lightvis, qbyte *litvis, void (*callback)(msurface_t *surf))
{
	genshadowmapcallback = callback;
	if (sh_shmesh->type == SMT_ORTHO)
		SHM_OrthoWorldLeafsQ1(dl);
	else
	{
		SHM_MarkLeavesQ1(dl, lightvis);
		SHM_RecursiveWorldNodeQ1_r(dl, cl.worldmodel->nodes);
	}
}
#endif

#ifdef Q3BSPS
static void SHM_OrthoWorldLeafsQ3 (dlight_t *dl)
{
	int			c, i;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf, *plastleaf;

	mplane_t orthoplanes[5];

	sh_shadowframe++;

	VectorCopy(dl->axis[0], orthoplanes[0].normal);
	VectorNegate(dl->axis[0], orthoplanes[1].normal);
	VectorCopy(dl->axis[1], orthoplanes[2].normal);
	VectorNegate(dl->axis[1], orthoplanes[3].normal);
	VectorNegate(dl->axis[0], orthoplanes[4].normal);

	for (i = 0; i < countof(orthoplanes); i++)
	{
		orthoplanes[i].dist = DotProduct(dl->origin, orthoplanes[i].normal) - dl->radius;
		CategorizePlane(&orthoplanes[i]);
	}

	for (pleaf = cl.worldmodel->leafs+1, plastleaf = cl.worldmodel->leafs+cl.worldmodel->numleafs; pleaf <= plastleaf; pleaf++)
	{
		for (i = 0; i < countof(orthoplanes); i++)
			if (BOX_ON_PLANE_SIDE (pleaf->minmaxs, pleaf->minmaxs+3, &orthoplanes[i]) == 2)
				goto next;

		SHM_Shadow_Cache_Leaf(pleaf);

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		while (c --> 0)
		{
			surf = *mark++;

			if (surf->flags & (SURF_DRAWALPHA | SURF_DRAWTILED | SURF_DRAWSKY))
				continue;

			if (surf->shadowframe != sh_shadowframe)
			{
				surf->shadowframe = sh_shadowframe;
			
//				if (dot < 0)
				{
					SHM_Shadow_Cache_Surface(surf);
				}
//				else
//				SHM_MeshBackOnly(surf->mesh->numvertexes, surf->mesh->xyz_array, surf->mesh->numindexes, surf->mesh->indexes);
					SHM_MeshFrontOnly(surf->mesh->numvertexes, surf->mesh->xyz_array, surf->mesh->numindexes, surf->mesh->indexes);
			}
		}

next:;
	}
}
#endif

#ifdef Q2BSPS
void CategorizePlane ( mplane_t *plane );
static void SHM_OrthoWorldLeafsQ2 (dlight_t *dl)
{
	int			c, i;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf, *plastleaf;

	mplane_t orthoplanes[5];

	sh_shadowframe++;

	VectorCopy(dl->axis[0], orthoplanes[0].normal);
	VectorNegate(dl->axis[0], orthoplanes[1].normal);
	VectorCopy(dl->axis[1], orthoplanes[2].normal);
	VectorNegate(dl->axis[1], orthoplanes[3].normal);
	VectorNegate(dl->axis[0], orthoplanes[4].normal);

	for (i = 0; i < countof(orthoplanes); i++)
	{
		orthoplanes[i].dist = DotProduct(dl->origin, orthoplanes[i].normal) - dl->radius;
		CategorizePlane(&orthoplanes[i]);
	}

	for (pleaf = cl.worldmodel->leafs+1, plastleaf = cl.worldmodel->leafs+cl.worldmodel->numleafs; pleaf <= plastleaf; pleaf++)
	{
		for (i = 0; i < countof(orthoplanes); i++)
			if (BOX_ON_PLANE_SIDE (pleaf->minmaxs, pleaf->minmaxs+3, &orthoplanes[i]) == 2)
				goto next;

		SHM_Shadow_Cache_Leaf(pleaf);

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		while (c --> 0)
		{
			surf = *mark++;

			if (surf->flags & (SURF_DRAWALPHA | SURF_DRAWTILED | SURF_DRAWSKY))
				continue;

			if (surf->shadowframe != sh_shadowframe)
			{
				surf->shadowframe = sh_shadowframe;

//				if (dot < 0)
				{
					SHM_Shadow_Cache_Surface(surf);
				}
//				else
//				SHM_MeshBackOnly(surf->mesh->numvertexes, surf->mesh->xyz_array, surf->mesh->numindexes, surf->mesh->indexes);
					SHM_MeshFrontOnly(surf->mesh->numvertexes, surf->mesh->xyz_array, surf->mesh->numindexes, surf->mesh->indexes);
			}
		}

next:;
	}
}

static void SHM_RecursiveWorldNodeQ2_r (dlight_t *dl, mnode_t *node)
{
	int			c, side;
	mplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	double		dot;
	int v;

	float		l, maxdist;
	int			j, s, t;
	vec3_t		impact;
	vec4_t		*lmvecs;
	float		*lmvecscale;

	if (node->shadowframe != sh_shadowframe)
		return;

	if (node->contents == Q2CONTENTS_SOLID)
		return;		// solid


	//if light areabox is outside node, ignore node + children
	for (c = 0; c < 3; c++)
	{
		if (dl->origin[c] + dl->radius < node->minmaxs[c])
			return;
		if (dl->origin[c] - dl->radius > node->minmaxs[3+c])
			return;
	}

// if a leaf node, draw stuff
	if (node->contents != -1)
	{
		pleaf = (mleaf_t *)node;

		if (pleaf->cluster >= 0)
			sh_shmesh->litleaves[pleaf->cluster>>3] |= 1<<(pleaf->cluster&7);

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				(*mark++)->shadowframe = sh_shadowframe;
			} while (--c);
		}
		return;
	}

// node is just a decision point, so go down the apropriate sides

// find which side of the node we are on
	plane = node->plane;

	switch (plane->type)
	{
	case PLANE_X:
		dot = dl->origin[0] - plane->dist;
		break;
	case PLANE_Y:
		dot = dl->origin[1] - plane->dist;
		break;
	case PLANE_Z:
		dot = dl->origin[2] - plane->dist;
		break;
	default:
		dot = DotProduct (dl->origin, plane->normal) - plane->dist;
		break;
	}

	if (dot >= 0)
		side = 0;
	else
		side = 1;

// recurse down the children, front side first
	SHM_RecursiveWorldNodeQ2_r (dl, node->children[side]);

// draw stuff
  	c = node->numsurfaces;

	if (c)
	{
		surf = cl.worldmodel->surfaces + node->firstsurface;

		{

			maxdist = dl->radius*dl->radius;

			for ( ; c ; c--, surf++)
			{
				if (surf->shadowframe != sh_shadowframe)
					continue;

//				if ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK))
//					continue;		// wrong side

//				if (surf->flags & SURF_PLANEBACK)
//					continue;

				if (surf->flags & (SURF_DRAWALPHA | SURF_DRAWTILED))
				{	// no shadows
					continue;
				}

				//is the light on the right side?
				if (surf->flags & SURF_PLANEBACK)
				{//inverted normal.
					if (-DotProduct(surf->plane->normal, dl->origin)+surf->plane->dist >= dl->radius)
						continue;
				}
				else
				{
					if (DotProduct(surf->plane->normal, dl->origin)-surf->plane->dist >= dl->radius)
						continue;
				}

				//Yeah, you can blame LordHavoc for this alternate code here.
				for (j=0 ; j<3 ; j++)
					impact[j] = dl->origin[j] - surf->plane->normal[j]*dot;

				if (currentmodel->facelmvecs)
					lmvecs = currentmodel->facelmvecs[surf-currentmodel->surfaces].lmvecs, lmvecscale = currentmodel->facelmvecs[surf-currentmodel->surfaces].lmvecscale;
				else
					lmvecs = surf->texinfo->vecs, lmvecscale = surf->texinfo->vecscale;
				// clamp center of light to corner and check brightness
				l = DotProduct (impact, lmvecs[0]) + lmvecs[0][3] - surf->texturemins[0];
				s = l;if (s < 0) s = 0;else if (s > surf->extents[0]) s = surf->extents[0];
				s = (l - s)*lmvecscale[0];
				l = DotProduct (impact, lmvecs[1]) + lmvecs[1][3] - surf->texturemins[1];
				t = l;if (t < 0) t = 0;else if (t > surf->extents[1]) t = surf->extents[1];
				t = (l - t)*lmvecscale[1];
				// compare to minimum light
				if ((s*s+t*t+dot*dot) < maxdist)
				{
					SHM_Shadow_Cache_Surface(surf);
					if (sh_shmesh->type == SMT_SHADOWMAP)
					{
						SHM_MeshFrontOnly(surf->mesh->numvertexes, surf->mesh->xyz_array, surf->mesh->numindexes, surf->mesh->indexes);
						continue;
					}
					if (sh_shmesh->type != SMT_STENCILVOLUME)
						continue;

					//build a list of the edges that are to be drawn.
					for (v = 0; v < surf->numedges; v++)
					{
						int e, delta;
						e = cl.worldmodel->surfedges[surf->firstedge+v];
						//negative edge means backwards edge.
						if (e < 0)
						{
							e=-e;
							delta = -1;
						}
						else
						{
							delta = 1;
						}

						if (!edge[e].count)
						{
							if (firstedge)
								edge[firstedge].prev = e;
							edge[e].next = firstedge;
							edge[e].prev = 0;
							firstedge = e;
							edge[e].count = delta;
						}
						else
						{
							edge[e].count += delta;

							if (!edge[e].count)	//unlink
							{
								if (edge[e].next)
								{
									edge[edge[e].next].prev = edge[e].prev;
								}
								if (edge[e].prev)
									edge[edge[e].prev].next = edge[e].next;
								else
									firstedge = edge[e].next;
							}
						}
					}

					SHM_TriangleFan(surf->mesh->numvertexes, surf->mesh->xyz_array, dl->origin, PROJECTION_DISTANCE);
				}
			}
		}
	}

// recurse down the back side
	SHM_RecursiveWorldNodeQ2_r (dl, node->children[!side]);
}

static void SHM_MarkLeavesQ2(dlight_t *dl, const unsigned char *lvis)
{
	mnode_t *node;
	int i;
	mleaf_t *leaf;
	int cluster;
	sh_shadowframe++;

	if (!dl->die)
	{
		//static
		//variation on mark leaves
		for (i=0,leaf=cl.worldmodel->leafs ; i<cl.worldmodel->numleafs ; i++, leaf++)
		{
			cluster = leaf->cluster;
			if (cluster == -1)
				continue;
			if (lvis[cluster>>3] & (1<<(cluster&7)))
			{
				node = (mnode_t *)leaf;
				do
				{
					if (node->shadowframe == sh_shadowframe)
						break;
					node->shadowframe = sh_shadowframe;
					node = node->parent;
				} while (node);
			}
		}
	}
	else
	{
		//dynamic lights will be discarded after this frame anyway, so only include leafs that are visible
		//variation on mark leaves
		for (i=0,leaf=cl.worldmodel->leafs ; i<cl.worldmodel->numleafs ; i++, leaf++)
		{
			cluster = leaf->cluster;
			if (cluster == -1)
				continue;
			if (lvis[cluster>>3] & (1<<(cluster&7)))
			{
				node = (mnode_t *)leaf;
				do
				{
					if (node->shadowframe == sh_shadowframe)
						break;
					node->shadowframe = sh_shadowframe;
					node = node->parent;
				} while (node);
			}
		}
	}
}
void Q2BSP_GenerateShadowMesh(model_t *model, dlight_t *dl, const qbyte *lightvis, qbyte *litvis, void (*callback)(msurface_t *surf))
{
	genshadowmapcallback = callback;
	if (sh_shmesh->type == SMT_ORTHO)
		SHM_OrthoWorldLeafsQ2(dl);
	else
	{
		SHM_MarkLeavesQ2(dl, lightvis);
		SHM_RecursiveWorldNodeQ2_r(dl, model->nodes);
	}
}
#endif

#ifdef Q3BSPS
static void SHM_RecursiveWorldNodeQ3_r (dlight_t *dl, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	**msurf;
	msurface_t	*surf;
	mleaf_t		*leaf;
	int			i;

	if (node->contents != -1)
	{
		leaf = (mleaf_t *)node;
		if (leaf->cluster >= 0)
			sh_shmesh->litleaves[leaf->cluster>>3] |= 1<<(leaf->cluster&7);

	// mark the polygons
		msurf = leaf->firstmarksurface;
		for (i=0 ; i<leaf->nummarksurfaces ; i++, msurf++)
		{
			surf = *msurf;

			//only check each surface once. it can appear in multiple leafs.
			if (surf->shadowframe == sh_shadowframe)
				continue;
			surf->shadowframe = sh_shadowframe;

			//FIXME: radius check
			SHM_Shadow_Cache_Surface(surf);
			if (sh_shmesh->type == SMT_SHADOWMAP && !(surf->texinfo->texture->shader->flags & SHADER_NOSHADOWS))
				SHM_MeshFrontOnly(surf->mesh->numvertexes, surf->mesh->xyz_array, surf->mesh->numindexes, surf->mesh->indexes);
		}
		return;
	}

	splitplane = node->plane;
	dist = DotProduct (dl->origin, splitplane->normal) - splitplane->dist;

	if (dist > dl->radius)
	{
		SHM_RecursiveWorldNodeQ3_r (dl, node->children[0]);
		return;
	}
	if (dist < -dl->radius)
	{
		SHM_RecursiveWorldNodeQ3_r (dl, node->children[1]);
		return;
	}
	SHM_RecursiveWorldNodeQ3_r (dl, node->children[0]);
	SHM_RecursiveWorldNodeQ3_r (dl, node->children[1]);
}
#endif

static struct {
	unsigned int numtris;
	unsigned int maxtris;
	struct {
		signed int edge[3];
	} *tris; /*negative for reverse edge*/

	unsigned int numedges;
	unsigned int maxedges;
	struct {
		unsigned int vert[2];
	} *edges;

	unsigned int numpoints;
	unsigned int maxpoints;
	vec3_t *points;

	unsigned int maxedgeuses;
	int *edgeuses;	/*negative for back sides, so 0 means unused or used equally on both sides*/
} cv;

static void SHM_Shutdown(void)
{
	SH_FreeShadowMesh_(&sh_tempshmesh);
	BZ_Free(sh_tempshmesh.litleaves);
	sh_tempshmesh.litleaves = NULL;
	sh_tempshmesh.leafbytes = 0;
	free(cv.tris);
	free(cv.edges);
	free(cv.points);
	memset(&cv, 0, sizeof(cv));
}

#ifdef Q3BSPS
#define VERT_POS_EPSILON (1.0f/32)
static int SHM_ComposeVolume_FindVert(float *vert)
{
	int i;
	for (i = 0; i < cv.numpoints; i++)
	{
#if 1
		if (cv.points[i][0] == vert[0] &&
			cv.points[i][1] == vert[1] &&
			cv.points[i][2] == vert[2])
#else
		vec3_t d;
		d[0] = cv.points[i][0]-vert[0];
		d[1] = cv.points[i][1]-vert[1];
		d[2] = cv.points[i][2]-vert[2];
		if (d[0]*d[0] < VERT_POS_EPSILON &&
			d[1]*d[1] < VERT_POS_EPSILON &&
			d[2]*d[2] < VERT_POS_EPSILON)
#endif
			return i;
	}
	VectorCopy(vert, cv.points[i]);
	cv.numpoints++;
	return i;
}
static int SHM_ComposeVolume_FindEdge(int v1, int v2)
{
	int i;
	for (i = 0; i < cv.numedges; i++)
	{
		if (cv.edges[i].vert[0] == v1 && cv.edges[i].vert[1] == v2)
			return i;
		if (cv.edges[i].vert[0] == v2 && cv.edges[i].vert[1] == v1)
			return -(i+1);
	}
	cv.edges[i].vert[0] = v1;
	cv.edges[i].vert[1] = v2;
	cv.numedges++;
	return i;
}

/*each triangle is coplanar, and all face the light, and its a triangle fan. this is a special case that provides a slight speedup*/
static void SHM_ComposeVolume_Fan(vecV_t *points, int numpoints)
{
	int newmax;
	int lastedge;
	int i;

	#define MAX_ARRAY_VERTS 65535
	static index_t pointidx[MAX_ARRAY_VERTS];

	/*make sure there's space*/
	newmax = (cv.numpoints+numpoints + inc)&~(inc-1);
	if (cv.maxpoints < newmax)
	{
		cv.maxpoints = newmax;
		cv.points = BZ_Realloc(cv.points, newmax * sizeof(*cv.points));
	}
	newmax = (cv.numedges+(numpoints-2)*3 + inc)&~(inc-1);
	if (cv.maxedges < newmax)
	{
		cv.maxedges = newmax;
		cv.edges = BZ_Realloc(cv.edges, newmax * sizeof(*cv.edges));
	}
	newmax = (cv.numtris+(numpoints-2) + inc)&~(inc-1);
	if (cv.maxtris < newmax)
	{
		cv.maxtris = newmax;
		cv.tris = BZ_Realloc(cv.tris, newmax * sizeof(*cv.tris));
	}

	for (i = 0; i < numpoints; i++)
	{
		pointidx[i] = SHM_ComposeVolume_FindVert(points[i]);
	}
	lastedge = SHM_ComposeVolume_FindEdge(pointidx[0], pointidx[1]);
	for (i = 2; i < numpoints; i++)
	{
		cv.tris[cv.numtris].edge[0] = lastedge;
		cv.tris[cv.numtris].edge[1] = SHM_ComposeVolume_FindEdge(pointidx[i-1], pointidx[i]);
		lastedge = SHM_ComposeVolume_FindEdge(pointidx[i], pointidx[0]);
		cv.tris[cv.numtris].edge[2] = lastedge;
		lastedge = -(lastedge+1);
		cv.numtris++;
	}
}
static void SHM_ComposeVolume_Soup(vecV_t *points, int numpoints, index_t *idx, int numidx)
{
	int newmax;
	int i;

	#define MAX_ARRAY_VERTS 65535
	static index_t pointidx[MAX_ARRAY_VERTS];

	/*make sure there's space*/
	newmax = (cv.numpoints+numpoints + inc)&~(inc-1);
	if (cv.maxpoints < newmax)
	{
		cv.maxpoints = newmax;
		cv.points = BZ_Realloc(cv.points, newmax * sizeof(*cv.points));
	}
	newmax = (cv.numedges+numidx + inc)&~(inc-1);
	if (cv.maxedges < newmax)
	{
		cv.maxedges = newmax;
		cv.edges = BZ_Realloc(cv.edges, newmax * sizeof(*cv.edges));
	}
	newmax = (cv.numtris+numidx/3 + inc)&~(inc-1);
	if (cv.maxtris < newmax)
	{
		cv.maxtris = newmax;
		cv.tris = BZ_Realloc(cv.tris, newmax * sizeof(*cv.tris));
	}

	for (i = 0; i < numpoints; i++)
	{
		pointidx[i] = SHM_ComposeVolume_FindVert(points[i]);
	}

	for (i = 0; i < numidx; i+=3, idx+=3)
	{
		cv.tris[cv.numtris].edge[0] = SHM_ComposeVolume_FindEdge(pointidx[idx[0]], pointidx[idx[1]]);
		cv.tris[cv.numtris].edge[1] = SHM_ComposeVolume_FindEdge(pointidx[idx[1]], pointidx[idx[2]]);
		cv.tris[cv.numtris].edge[2] = SHM_ComposeVolume_FindEdge(pointidx[idx[2]], pointidx[idx[0]]);
		cv.numtris++;
	}
}

/*call this function after generating litsurfs meshes*/
static void SHM_ComposeVolume_BruteForce(dlight_t *dl)
{
	shadowmeshbatch_t *sms;
	unsigned int tno;
	unsigned int sno;
	int i, e;
	mesh_t *sm;
	vec3_t ext;
	float sc;
	cv.numedges = 0;
	cv.numpoints = 0;
	cv.numtris = 0;

	for (tno = 0; tno < sh_shmesh->numbatches; tno++)
	{
		sms = &sh_shmesh->batches[tno];
		if (!sms->count)
			continue;
		if ((cl.worldmodel->shadowbatches[tno].tex->shader->flags & (SHADER_BLEND|SHADER_NODRAW|SHADER_NOSHADOWS)))
			continue;

		for (sno = 0; sno < sms->count; sno++)
		{
			sm = sms->s[sno];

			if (sm->istrifan)
				SHM_ComposeVolume_Fan(sm->xyz_array, sm->numvertexes);
			else
				SHM_ComposeVolume_Soup(sm->xyz_array, sm->numvertexes, sm->indexes, sm->numindexes);
		}
	}

	/*FIXME: clip away overlapping triangles*/

	if (cv.maxedgeuses < cv.numedges)
	{
		BZ_Free(cv.edgeuses);
		cv.maxedgeuses = cv.numedges;
		cv.edgeuses = Z_Malloc(cv.maxedgeuses * sizeof(*cv.edgeuses));
	}
	else
		memset(cv.edgeuses, 0, cv.numedges * sizeof(*cv.edgeuses));
	
	i = (sh_shmesh->numverts+cv.numpoints*6+inc+5)&~(inc-1);	//and a bit of padding
	if (sh_shmesh->maxverts < i)
	{
		sh_shmesh->maxverts = i;
		sh_shmesh->verts = BZ_Realloc(sh_shmesh->verts, i * sizeof(*sh_shmesh->verts));
	}

	for (i = 0; i < cv.numpoints; i++)
	{
		/*front face*/
		sh_shmesh->verts[(i * 2) + 0][0] = cv.points[i][0];
		sh_shmesh->verts[(i * 2) + 0][1] = cv.points[i][1];
		sh_shmesh->verts[(i * 2) + 0][2] = cv.points[i][2];

		/*shadow direction*/
		ext[0] = cv.points[i][0]-dl->origin[0];
		ext[1] = cv.points[i][1]-dl->origin[1];
		ext[2] = cv.points[i][2]-dl->origin[2];
		
		sc = dl->radius * VectorNormalize(ext);

		/*back face*/
		sh_shmesh->verts[(i * 2) + 1][0] = cv.points[i][0] + ext[0] * sc;
		sh_shmesh->verts[(i * 2) + 1][1] = cv.points[i][1] + ext[1] * sc;
		sh_shmesh->verts[(i * 2) + 1][2] = cv.points[i][2] + ext[2] * sc;
	}
	sh_shmesh->numverts = i*2;

	i = (sh_shmesh->numindicies+cv.numtris*6+cv.numedges*6+inc+5)&~(inc-1);	//and a bit of padding
	if (sh_shmesh->maxindicies < i)
	{
		sh_shmesh->maxindicies = i;
		sh_shmesh->indicies = BZ_Realloc(sh_shmesh->indicies, i * sizeof(*sh_shmesh->indicies));
	}

	for (tno = 0; tno < cv.numtris; tno++)
	{
		for (i = 0; i < 3; i++)
		{
			e = cv.tris[tno].edge[i];
			if (e < 0)
			{
				e = -(e+1);
				cv.edgeuses[e]--;
				e = cv.edges[e].vert[1];
			}
			else
			{
				cv.edgeuses[e]++;
				e = cv.edges[e].vert[0];
			}

			sh_shmesh->indicies[sh_shmesh->numindicies+i] = e*2;
			sh_shmesh->indicies[sh_shmesh->numindicies+5-i] = e*2 + 1;
		}
		sh_shmesh->numindicies += 6;
	}

	for (i = 0; i < cv.numedges; i++)
	{
		if (cv.edgeuses[i] > 0)
		{
			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[1]*2 + 0;
			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[0]*2 + 0;
			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[0]*2 + 1;

			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[0]*2 + 1;
			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[1]*2 + 1;
			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[1]*2 + 0;
		}
		else if (cv.edgeuses[i] < 0)
		{
			//generally should not happen...
			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[1]*2 + 0;
			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[0]*2 + 1;
			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[0]*2 + 0;

			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[0]*2 + 1;
			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[1]*2 + 0;
			sh_shmesh->indicies[sh_shmesh->numindicies++] = cv.edges[i].vert[1]*2 + 1;
		}
	}
}

void Q3BSP_GenerateShadowMesh(model_t *model, dlight_t *dl, const qbyte *lightvis, qbyte *litvis, void (*callback)(msurface_t *surf))
{
	/*q3 doesn't have edge info*/
	if (sh_shmesh->type == SMT_ORTHO)
		SHM_OrthoWorldLeafsQ3(dl);
	else
	{
		sh_shadowframe++;
		SHM_RecursiveWorldNodeQ3_r(dl, model->nodes);
	}
	if (sh_shmesh->type == SMT_STENCILVOLUME)
		SHM_ComposeVolume_BruteForce(dl);
}
#endif

static void SHM_Shadow_Surface_Shadowmap (msurface_t *surf)
{
	SHM_Shadow_Cache_Surface(surf);
	if (surf->texinfo->texture->shader->flags & SHADER_NOSHADOWS)
		return;
	SHM_MeshFrontOnly(surf->mesh->numvertexes, surf->mesh->xyz_array, surf->mesh->numindexes, surf->mesh->indexes);
}
static void SHM_Shadow_Surface_StencilVolume (msurface_t *surf)
{
	int v;
	SHM_Shadow_Cache_Surface(surf);
	if (surf->texinfo->texture->shader->flags & SHADER_NOSHADOWS)
		return;
	if (!surf->mesh->istrifan)
		return;

	//build a list of the edges that are to be drawn.
	for (v = 0; v < surf->numedges; v++)
	{
		int e, delta;
		e = cl.worldmodel->surfedges[surf->firstedge+v];
		//negative edge means backwards edge.
		if (e < 0)
		{
			e=-e;
			delta = -1;
		}
		else
		{
			delta = 1;
		}

		if (!edge[e].count)
		{
			if (firstedge)
				edge[firstedge].prev = e;
			edge[e].next = firstedge;
			edge[e].prev = 0;
			firstedge = e;
			edge[e].count = delta;
		}
		else
		{
			edge[e].count += delta;

			if (!edge[e].count)	//unlink
			{
				if (edge[e].next)
				{
					edge[edge[e].next].prev = edge[e].prev;
				}
				if (edge[e].prev)
					edge[edge[e].prev].next = edge[e].next;
				else
					firstedge = edge[e].next;
			}
		}
	}

	SHM_TriangleFan(surf->mesh->numvertexes, surf->mesh->xyz_array, sh_shmesh->origin, PROJECTION_DISTANCE);
}

static struct shadowmesh_s *SHM_BuildShadowMesh(dlight_t *dl, unsigned char *lvis, int type)
{
	float *v1, *v2;
	vec3_t v3, v4;

	if (dl->worldshadowmesh && !dl->rebuildcache && dl->worldshadowmesh->type == type)
		return dl->worldshadowmesh;

	if (!lvis)
	{
		int clus;
		if (type == SMT_ORTHO)
			;
		else if ((type == SMT_SHADOWLESS || dl->lightcolourscales[0]) && cl.worldmodel->funcs.ClustersInSphere)
			//shadowless lights don't cast shadows, so they're seen through everything - their vis must reflect that.
			lvis = cl.worldmodel->funcs.ClustersInSphere(cl.worldmodel, dl->origin, dl->radius, &lvisb, NULL);
		else
		{
			clus = cl.worldmodel->funcs.ClusterForPoint(cl.worldmodel, dl->origin, NULL);	//FIXME: track the lights area
			lvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, clus, &lvisb, PVM_FAST);

			if (cl.worldmodel->funcs.ClustersInSphere)
				lvis = cl.worldmodel->funcs.ClustersInSphere(cl.worldmodel, dl->origin, dl->radius, &lvisb2, lvis);
		}
	}

	firstedge=0;
	if (maxedge < cl.worldmodel->numedges)
	{
		maxedge = cl.worldmodel->numedges;
		Z_Free(edge);
		edge = Z_Malloc(sizeof(*edge) * maxedge);
	}

	SHM_BeginShadowMesh(dl, type);
	if (cl.worldmodel->funcs.GenerateShadowMesh)
	{
		switch(type)
		{
		case SMT_SHADOWMAP:
			cl.worldmodel->funcs.GenerateShadowMesh(cl.worldmodel, dl, lvis, sh_shmesh->litleaves, SHM_Shadow_Surface_Shadowmap);
			break;
		case SMT_STENCILVOLUME:
			cl.worldmodel->funcs.GenerateShadowMesh(cl.worldmodel, dl, lvis, sh_shmesh->litleaves, SHM_Shadow_Surface_StencilVolume);
			break;
		default:
			cl.worldmodel->funcs.GenerateShadowMesh(cl.worldmodel, dl, lvis, sh_shmesh->litleaves, SHM_Shadow_Cache_Surface);
			break;
		}
	}
	else if (cl.worldmodel->type == mod_brush)
	{
		switch(cl.worldmodel->fromgame)
		{
#ifdef Q1BSPS
		case fg_quake:
		case fg_halflife:
			if (type == SMT_ORTHO)
				SHM_OrthoWorldLeafsQ1(dl);
			else
			{
				SHM_MarkLeavesQ1(dl, lvis);
				SHM_RecursiveWorldNodeQ1_r(dl, cl.worldmodel->nodes);
			}
			break;
#endif
		default:
			sh_shadowframe++;

			{
				int cluster = cl.worldmodel->funcs.ClusterForPoint(cl.worldmodel, dl->origin, NULL);
				if (cluster >= 0)
					sh_shmesh->litleaves[cluster>>3] |= 1<<(cluster&7);
			}
			break;
		}
	}
	else
	{
		SHM_BeginShadowMesh(dl, type);
		sh_shadowframe++;
	}

	/*generate edge polys for map types that need it (q1/q2)*/
	switch (type)
	{
	case SMT_STENCILVOLUME:
		SHM_BeginQuads();
		while(firstedge)
		{
			//border
			v1 = cl.worldmodel->vertexes[cl.worldmodel->edges[firstedge].v[0]].position;
			v2 = cl.worldmodel->vertexes[cl.worldmodel->edges[firstedge].v[1]].position;

			//get positions of v3 and v4 based on the light position
			v3[0] = v1[0] + ( v1[0]-dl->origin[0] )*PROJECTION_DISTANCE;
			v3[1] = v1[1] + ( v1[1]-dl->origin[1] )*PROJECTION_DISTANCE;
			v3[2] = v1[2] + ( v1[2]-dl->origin[2] )*PROJECTION_DISTANCE;

			v4[0] = v2[0] + ( v2[0]-dl->origin[0] )*PROJECTION_DISTANCE;
			v4[1] = v2[1] + ( v2[1]-dl->origin[1] )*PROJECTION_DISTANCE;
			v4[2] = v2[2] + ( v2[2]-dl->origin[2] )*PROJECTION_DISTANCE;

			if (edge[firstedge].count > 0)
			{
				SHM_Vertex3fv(v3);
				SHM_Vertex3fv(v4);
				SHM_Vertex3fv(v2);
				SHM_Vertex3fv(v1);
			}
			else
			{
				SHM_Vertex3fv(v1);
				SHM_Vertex3fv(v2);
				SHM_Vertex3fv(v4);
				SHM_Vertex3fv(v3);
			}
			edge[firstedge].count=0;

			firstedge = edge[firstedge].next;
		}
		SHM_End();
		break;
	}

	return SHM_FinishShadowMesh(dl);
}













static qboolean Sh_VisOverlaps(qbyte *v1, qbyte *v2)
{
	int i, m;
	if (!v2 || !v1)
		return true;
	m = (cl.worldmodel->numclusters+7)>>3;

	for (i=(m&~3) ; i<m ; i++)
	{
		if (v1[i] & v2[i])
			return true;
	}
	m>>=2;
	for (i=0 ; i<m ; i++)
	{
		if (((unsigned int*)v1)[i] & ((unsigned int*)v2)[i])
			return true;
	}
	return false;
}

#define Sh_LeafInView Sh_VisOverlaps


/*
static void Sh_Scissor (srect_t *r)
{
	//float xs = vid.pixelwidth / (float)vid.width, ys = vid.pixelheight / (float)vid.height;
	switch(qrenderer)
	{
	case QR_NONE:
	case QR_SOFTWARE:
	case QR_DIRECT3D11:
	default:
		break;

	case QR_OPENGL:
#ifdef GLQUAKE
		qglScissor(
			floor(r_refdef.pxrect.x + r->x*r_refdef.pxrect.width),
			floor((r_refdef.pxrect.y + r->y*r_refdef.pxrect.height) - r_refdef.pxrect.height),
			ceil(r->width * r_refdef.pxrect.width),
			ceil(r->height * r_refdef.pxrect.height));
		qglEnable(GL_SCISSOR_TEST);

		if (qglDepthBoundsEXT)
		{
			qglDepthBoundsEXT(r->dmin, r->dmax);
			qglEnable(GL_DEPTH_BOUNDS_TEST_EXT);
		}
#endif
		break;
	case QR_DIRECT3D9:
#ifdef D3D9QUAKE
		{
			RECT rect;
			rect.left = r->x;
			rect.right = r->x + r->width;
			rect.top = r->y;
			rect.bottom = r->y + r->height;
			IDirect3DDevice9_SetScissorRect(pD3DDev9, &rect);
		}
#endif
		break;
	}
}
static void Sh_ScissorOff (void)
{
	switch(qrenderer)
	{
	default:
		break;
	case QR_OPENGL:
#ifdef GLQUAKE
		qglDisable(GL_SCISSOR_TEST);
		if (qglDepthBoundsEXT)
			qglDisable(GL_DEPTH_BOUNDS_TEST_EXT);
#endif
		break;
	case QR_DIRECT3D9:
#ifdef D3D9QUAKE
#endif
		break;
	}
}
*/
#if 0
static qboolean Sh_ScissorForSphere(vec3_t center, float radius, vrect_t *rect)
{
	/*return false to say that its fully offscreen*/

	float v[4], tempv[4];
	int i;
	vrect_t r;

	rect->x = 0;
	rect->y = 0;
	rect->width = vid.pixelwidth;
	rect->height = vid.pixelheight;


/*
	for (i = 0; i < 4; i++)
	{
		v[3] = 1;
		VectorMA(center, radius, frustum[i].normal, v);

		tempv[0] = r_refdef.m_view[0]*v[0] + r_refdef.m_view[4]*v[1] + r_refdef.m_view[8]*v[2] + r_refdef.m_view[12]*v[3];
		tempv[1] = r_refdef.m_view[1]*v[0] + r_refdef.m_view[5]*v[1] + r_refdef.m_view[9]*v[2] + r_refdef.m_view[13]*v[3];
		tempv[2] = r_refdef.m_view[2]*v[0] + r_refdef.m_view[6]*v[1] + r_refdef.m_view[10]*v[2] + r_refdef.m_view[14]*v[3];
		tempv[3] = r_refdef.m_view[3]*v[0] + r_refdef.m_view[7]*v[1] + r_refdef.m_view[11]*v[2] + r_refdef.m_view[15]*v[3];

		product[0] = r_refdef.m_projection[0]*tempv[0] + r_refdef.m_projection[4]*tempv[1] + r_refdef.m_projection[8]*tempv[2] + r_refdef.m_projection[12]*tempv[3];
		product[1] = r_refdef.m_projection[1]*tempv[0] + r_refdef.m_projection[5]*tempv[1] + r_refdef.m_projection[9]*tempv[2] + r_refdef.m_projection[13]*tempv[3];
		product[2] = r_refdef.m_projection[2]*tempv[0] + r_refdef.m_projection[6]*tempv[1] + r_refdef.m_projection[10]*tempv[2] + r_refdef.m_projection[14]*tempv[3];
		product[3] = r_refdef.m_projection[3]*tempv[0] + r_refdef.m_projection[7]*tempv[1] + r_refdef.m_projection[11]*tempv[2] + r_refdef.m_projection[15]*tempv[3];

		v[0] /= v[3];
		v[1] /= v[3];
		v[2] /= v[3];

		out[0] = (1+v[0])/2;
		out[1] = (1+v[1])/2;
		out[2] = (1+v[2])/2;

		r.x 
	}
*/
	return false;
}
#endif

#define BoxesOverlap(a,b,c,d) ((a)[0] <= (d)[0] && (b)[0] >= (c)[0] && (a)[1] <= (d)[1] && (b)[1] >= (c)[1] && (a)[2] <= (d)[2] && (b)[2] >= (c)[2])
static qboolean Sh_ScissorForBox(vec3_t mins, vec3_t maxs, srect_t *r)
{
	static const int edge[12][2] =
	{
		{0, 1}, {0, 2}, {1, 3}, {2, 3},
		{4, 5}, {4, 6}, {5, 7}, {6, 7},
		{0, 4}, {1, 5}, {2, 6}, {3, 7}
	};
	//the box is a simple cube.
	//clip each vert to the near clip plane
	//insert a replacement vertex for edges that cross the nearclip plane where it crosses
	//calc the scissor rect from projecting the verts that survived, plus the clipped edge ones.
	float ncpdist;
	float dist[8];
	int sign[8];
	vec4_t vert[20];
	vec3_t p[8];
	int numverts = 0, i, v1, v2;
	vec4_t v,tv;
	float frac;
	float x,x1,x2,y,y1,y2;
	double z, z1, z2;

	r->x = 0;
	r->y = 0;
	r->width = 1;
	r->height = 1;
	r->dmin = 0;
	r->dmax = 1;
	if (!r_shadow_scissor.ival)
	{
		r->x = 0;
		r->y = 0;
		r->width  = 1;
		r->height = 1;
		return false;
	}
	/*if view is inside the box, then skip this maths*/
//	if (BoxesOverlap(r_refdef.vieworg, r_refdef.vieworg, mins, maxs))
//	{
//		return false;
//	}

	ncpdist = DotProduct(r_refdef.vieworg, vpn) + r_refdef.mindist;

	for (i = 0; i < 8; i++)
	{
		p[i][0] = (i & 1) ? mins[0] : maxs[0];
		p[i][1] = (i & 2) ? mins[1] : maxs[1];
		p[i][2] = (i & 4) ? mins[2] : maxs[2];
		dist[i] = ncpdist - DotProduct(p[i], vpn);
		sign[i] = (dist[i] > 0);
		if (!sign[i])
		{
			VectorCopy(p[i], vert[numverts]);
			numverts++;
		}
	}

	/*fully clipped by near plane*/
	if (!numverts)
		return true;

	if (numverts != 8)
	{
		/*crosses near clip plane somewhere*/
		for (i = 0; i < 12; i++)
		{
			v1 = edge[i][0];
			v2 = edge[i][1];
			if (sign[v1] != sign[v2])
			{
				frac = dist[v1] / (dist[v1] - dist[v2]);
				VectorInterpolate(p[v1], frac, p[v2], vert[numverts]);
				numverts++;
			}
		}
	}
	x1 = y1 = z1 = 1;
	x2 = y2 = z2 = -1;
	/*transform each vert to get the screen pos*/
	for (i = 0; i < numverts; i++)
	{
		vert[i][3] = 1;
		Matrix4x4_CM_Transform4(r_refdef.m_view, vert[i], tv); 
		Matrix4x4_CM_Transform4(r_refdef.m_projection_std, tv, v);

		x = v[0] / v[3];
		y = v[1] / v[3];
		z = (double)v[2] / v[3];
		if (x < x1) x1 = x;
		if (x > x2) x2 = x;
		if (y < y1) y1 = y;
		if (y > y2) y2 = y;
		if (z < z1) z1 = z;
		if (z > z2) z2 = z;
	}
	x1 = (1+x1) / 2;
	x2 = (1+x2) / 2;
	y1 = (1+y1) / 2;
	y2 = (1+y2) / 2;
	z1 = (1+z1) / 2;
	z2 = (1+z2) / 2;

	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 < 0)
		x2 = 0;
	if (y2 < 0)
		y2 = 0;
	if (x1 > 1)
		x1 = 1;
	if (y1 > 1)
		y1 = 1;
	if (x2 > 1)
		x2 = 1;
	if (y2 > 1)
		y2 = 1;
	r->x = x1;
	r->y = y1;
	r->width  = x2 - r->x;
	r->height = y2 - r->y;
	if (r->width == 0 || r->height == 0)
		return true;	//meh

	r->dmin = z1;
	r->dmax = z2;
	return false;
}

#if 0
static qboolean Sh_ScissorForBox(vec3_t mins, vec3_t maxs, vrect_t *r)
{
	int i, ix1, iy1, ix2, iy2;
	float x1, y1, x2, y2, x, y, f;
	vec3_t smins, smaxs;
	vec4_t v, v2;

	r->x = 0;
	r->y = 0;
	r->width = vid.pixelwidth;
	r->height = vid.pixelheight;
	if (0)//!r_shadow_scissor.integer)
	{
		return false;
	}
	// if view is inside the box, just say yes it's fully visible
	if (BoxesOverlap(r_refdef.vieworg, r_refdef.vieworg, mins, maxs))
	{
		return false;
	}
	for (i = 0;i < 3;i++)
	{
		if (vpn[i] >= 0)
		{
			v[i] = mins[i];
			v2[i] = maxs[i];
		}
		else
		{
			v[i] = maxs[i];
			v2[i] = mins[i];
		}
	}
	f = DotProduct(vpn, r_refdef.vieworg);
	if (DotProduct(vpn, v2) <= f)
	{
		// entirely behind nearclip plane, entirely obscured
		return true;
	}
	if (DotProduct(vpn, v) >= f)
	{
		// entirely infront of nearclip plane
		x1 = y1 = x2 = y2 = 0;
		for (i = 0;i < 8;i++)
		{
			v[0] = (i & 1) ? mins[0] : maxs[0];
			v[1] = (i & 2) ? mins[1] : maxs[1];
			v[2] = (i & 4) ? mins[2] : maxs[2];
			v[3] = 1.0f;
			Matrix4x4_CM_Project(v, v2, r_refdef.viewangles, r_refdef.vieworg, r_refdef.fov_x, r_refdef.fov_y);
			v2[0]*=vid.pixelwidth;
			v2[1]*=vid.pixelheight;
//			GL_TransformToScreen(v, v2);
			//Con_Printf("%.3f %.3f %.3f %.3f transformed to %.3f %.3f %.3f %.3f\n", v[0], v[1], v[2], v[3], v2[0], v2[1], v2[2], v2[3]);
			x = v2[0];
			y = v2[1];
			if (i)
			{
				if (x1 > x) x1 = x;
				if (x2 < x) x2 = x;
				if (y1 > y) y1 = y;
				if (y2 < y) y2 = y;
			}
			else
			{
				x1 = x2 = x;
				y1 = y2 = y;
			}
		}
	}
	else
	{
		// clipped by nearclip plane
		// this is nasty and crude...
		// create viewspace bbox
		i = 0;
		/*unrolled the first iteration to avoid warnings*/
		v[0] = ((i & 1) ? mins[0] : maxs[0]) - r_refdef.vieworg[0];
		v[1] = ((i & 2) ? mins[1] : maxs[1]) - r_refdef.vieworg[1];
		v[2] = ((i & 4) ? mins[2] : maxs[2]) - r_refdef.vieworg[2];
		v2[0] = DotProduct(v, vright);
		v2[1] = DotProduct(v, vup);
		v2[2] = DotProduct(v, vpn);
		smins[0] = smaxs[0] = v2[0];
		smins[1] = smaxs[1] = v2[1];
		smins[2] = smaxs[2] = v2[2];
		for (i = 1;i < 8;i++)
		{
			v[0] = ((i & 1) ? mins[0] : maxs[0]) - r_refdef.vieworg[0];
			v[1] = ((i & 2) ? mins[1] : maxs[1]) - r_refdef.vieworg[1];
			v[2] = ((i & 4) ? mins[2] : maxs[2]) - r_refdef.vieworg[2];
			v2[0] = DotProduct(v, vright);
			v2[1] = DotProduct(v, vup);
			v2[2] = DotProduct(v, vpn);
			if (smins[0] > v2[0]) smins[0] = v2[0];
			if (smaxs[0] < v2[0]) smaxs[0] = v2[0];
			if (smins[1] > v2[1]) smins[1] = v2[1];
			if (smaxs[1] < v2[1]) smaxs[1] = v2[1];
			if (smins[2] > v2[2]) smins[2] = v2[2];
			if (smaxs[2] < v2[2]) smaxs[2] = v2[2];
		}
		// now we have a bbox in viewspace
		// clip it to the view plane
		if (smins[2] < 1)
			smins[2] = 1;
		// return true if that culled the box
		if (smins[2] >= smaxs[2])
			return true;
		// ok some of it is infront of the view, transform each corner back to
		// worldspace and then to screenspace and make screen rect
		// initialize these variables just to avoid compiler warnings
		x1 = y1 = x2 = y2 = 0;
		for (i = 0;i < 8;i++)
		{
			v2[0] = (i & 1) ? smins[0] : smaxs[0];
			v2[1] = (i & 2) ? smins[1] : smaxs[1];
			v2[2] = (i & 4) ? smins[2] : smaxs[2];
			v[0] = v2[0] * vright[0] + v2[1] * vup[0] + v2[2] * vpn[0] + r_refdef.vieworg[0];
			v[1] = v2[0] * vright[1] + v2[1] * vup[1] + v2[2] * vpn[1] + r_refdef.vieworg[1];
			v[2] = v2[0] * vright[2] + v2[1] * vup[2] + v2[2] * vpn[2] + r_refdef.vieworg[2];
			v[3] = 1.0f;
			Matrix4x4_CM_Project(v, v2, r_refdef.viewangles, r_refdef.vieworg, r_refdef.fov_x, r_refdef.fov_y);
			v2[0]*=vid.pixelwidth;
			v2[1]*=vid.pixelheight;
			//Con_Printf("%.3f %.3f %.3f %.3f transformed to %.3f %.3f %.3f %.3f\n", v[0], v[1], v[2], v[3], v2[0], v2[1], v2[2], v2[3]);
			x = v2[0];
			y = v2[1];
			if (i)
			{
				if (x1 > x) x1 = x;
				if (x2 < x) x2 = x;
				if (y1 > y) y1 = y;
				if (y2 < y) y2 = y;
			}
			else
			{
				x1 = x2 = x;
				y1 = y2 = y;
			}
		}
#if 1
		// this code doesn't handle boxes with any points behind view properly
		x1 = 1000;x2 = -1000;
		y1 = 1000;y2 = -1000;
		for (i = 0;i < 8;i++)
		{
			v[0] = (i & 1) ? mins[0] : maxs[0];
			v[1] = (i & 2) ? mins[1] : maxs[1];
			v[2] = (i & 4) ? mins[2] : maxs[2];
			v[3] = 1.0f;
			Matrix4x4_CM_Project(v, v2, r_refdef.viewangles, r_refdef.vieworg, r_refdef.fov_x, r_refdef.fov_y);
			v2[0]*=vid.pixelwidth;
			v2[1]*=vid.pixelheight;
			//Con_Printf("%.3f %.3f %.3f %.3f transformed to %.3f %.3f %.3f %.3f\n", v[0], v[1], v[2], v[3], v2[0], v2[1], v2[2], v2[3]);
			if (v2[2] > 0)
			{
				x = v2[0];
				y = v2[1];

				if (x1 > x) x1 = x;
				if (x2 < x) x2 = x;
				if (y1 > y) y1 = y;
				if (y2 < y) y2 = y;
			}
		}
#endif
	}
	ix1 = x1 - 1.0f;
	iy1 = y1 - 1.0f;
	ix2 = x2 + 1.0f;
	iy2 = y2 + 1.0f;
	//Con_Printf("%f %f %f %f\n", x1, y1, x2, y2);
	if (ix1 < r->x) ix1 = r->x;
	if (iy1 < r->y) iy1 = r->y;
	if (ix2 > r->x + r->width) ix2 = r->x + r->width;
	if (iy2 > r->y + r->height) iy2 = r->y + r->height;
	if (ix2 <= ix1 || iy2 <= iy1)
		return true;
	// set up the scissor rectangle
	
	r->x = ix1;
	r->y = iy1;
	r->width = ix2 - ix1;
	r->height = iy2 - iy1;
	return false;
}
#endif

void D3D11BE_BeginShadowmapFace(void);

//determine the 5 bounding points of a shadowmap light projection side
//needs to match Sh_GenShadowFace
static void Sh_LightFrustumPlanes(dlight_t *l, vec3_t axis[3], vec4_t *planes, int face)
{
	vec3_t tmp;
	int axis0, axis1, axis2;
	int dir;
	int i;
	//+x,+y,+z,-x,-y,-z
	axis0 = (face+0)%3;	//our major axis
	axis1 = (face+1)%3;
	axis2 = (face+2)%3;
	dir = (face >= 3)?-1:1;

	//center point is always the same
	VectorCopy(l->origin, planes[4]);
	VectorScale(axis[axis0], dir, planes[4]);
	VectorNormalize(planes[4]);
	planes[4][3] = (l->nearclip?l->nearclip:r_shadow_shadowmapping_nearclip.value) + DotProduct(planes[4], l->origin);

	for (i = 0; i < 4; i++)
	{
		VectorScale(axis[axis0], dir, tmp);
		VectorMA(tmp,		((i&1)?1:-1), axis[axis1], tmp);
		VectorMA(tmp,		((i&2)?1:-1), axis[axis2], planes[i]);
		VectorNormalize(planes[i]);
		planes[i][3] = DotProduct(planes[i], l->origin);
	}
}

//culling for the face happens in the caller.
//these faces should thus match Sh_LightFrustumPlanes
//nettest P110: when set, Sh_GenShadowFace renders into this CELL of the shadow texture instead of the
//centred full-texture region.  Statics rather than parameters so the stock call sites are untouched.
static qboolean sh_fakecell_active;
static int sh_fakecell_x, sh_fakecell_y;

static void Sh_GenShadowFace(dlight_t *l, vec3_t axis[3], int lighttype, shadowmesh_t *smesh, int face, int smsize, int txsize, float proj[16], const qbyte *lightpvs)
{
	vec3_t t1,t2,t3;
	texture_t *tex;
	int tno;

/*	if (face >= 3)
		face -= 3;
	else
		face += 3;
*/
	switch(face)
	{
	case 0:
		//down
		VectorCopy(axis[0], t1);
		VectorCopy(axis[1], t2);
		VectorCopy(axis[2], t3);
		Matrix4x4_CM_LightMatrixFromAxis(r_refdef.m_view, t1, t2, t3, l->origin);
		r_refdef.flipcull = 0;
		break;
	case 1:
		//back
		VectorCopy(axis[2], t1);
		VectorCopy(axis[1], t2);
		VectorCopy(axis[0], t3);
		Matrix4x4_CM_LightMatrixFromAxis(r_refdef.m_view, t1, t2, t3, l->origin);
		r_refdef.flipcull = SHADER_CULL_FLIP;
		break;
	case 2:
		//right
		VectorCopy(axis[0], t1);
		VectorCopy(axis[2], t2);
		VectorCopy(axis[1], t3);
		Matrix4x4_CM_LightMatrixFromAxis(r_refdef.m_view, t1, t2, t3, l->origin);
		r_refdef.flipcull = SHADER_CULL_FLIP;
		break;
	case 3:
		//up
		VectorCopy(axis[0], t1);
		VectorCopy(axis[1], t2);
		VectorCopy(axis[2], t3);
		VectorNegate(t3, t3);
		Matrix4x4_CM_LightMatrixFromAxis(r_refdef.m_view, t1, t2, t3, l->origin);
		r_refdef.flipcull = SHADER_CULL_FLIP;
		break;
	case 4:
		//forward
		VectorCopy(axis[2], t1);
		VectorCopy(axis[1], t2);
		VectorCopy(axis[0], t3);
		VectorNegate(t3, t3);
		Matrix4x4_CM_LightMatrixFromAxis(r_refdef.m_view, t1, t2, t3, l->origin);
		r_refdef.flipcull = 0;
		break;
	case 5:
		//left
		VectorCopy(axis[0], t1);
		VectorCopy(axis[2], t2);
		VectorCopy(axis[1], t3);
		VectorNegate(t3, t3);
		Matrix4x4_CM_LightMatrixFromAxis(r_refdef.m_view, t1, t2, t3, l->origin);
		r_refdef.flipcull = 0;
		break;
	}

	if (sh_fakecell_active)
	{	//nettest P110: this face is one CELL of the multi-direction fake-shadow atlas rather than the
		//whole texture.  Top-origin here; GL_ViewportUpdate flips it to GL's bottom-origin below, and
		//the matching cell UNIFORM is computed flipped in Sh_GenerateFakeShadowsAtlas (see the note
		//there -- an asymmetric rect that ignores the flip samples the empty half and yields NO shadows
		//at all, which cost Patch 93 a whole debugging session).
		r_refdef.pxrect.x = sh_fakecell_x;
		r_refdef.pxrect.y = sh_fakecell_y;
		r_refdef.pxrect.width = smsize;
		r_refdef.pxrect.height = smsize;
		r_refdef.pxrect.maxheight = txsize;
	}
	else if (lighttype & (LSHADER_SPOT|LSHADER_ORTHO))
	{
		r_refdef.pxrect.x = (txsize-smsize)/2;
		r_refdef.pxrect.width = smsize;
		r_refdef.pxrect.height = smsize;
		r_refdef.pxrect.y = (txsize-smsize)/2;
		r_refdef.pxrect.maxheight = txsize;
	}
	else
	{
		r_refdef.pxrect.x = (face%3 * txsize) + (txsize-smsize)/2;
		r_refdef.pxrect.width = smsize;
		r_refdef.pxrect.height = smsize;
		r_refdef.pxrect.y = (((face<3)*txsize) + (txsize-smsize)/2);
		r_refdef.pxrect.maxheight = txsize*2;
	}

	R_SetFrustum(proj, r_refdef.m_view);

	if (lighttype & LSHADER_ORTHO)
		r_refdef.frustum_numplanes = 4;	//kill the near clip plane - we allow ANYTHING nearer through.

	if (lighttype & LSHADER_FAKESHADOWS)
		r_refdef.flipcull ^= SHADER_CULL_FLIP;
	r_refdef.colourmask = 0u;

#ifdef SHADOWDBG_COLOURNOTDEPTH
	BE_SelectMode(BEM_STANDARD);
#else
	BE_SelectMode(BEM_DEPTHONLY);
#endif
	BE_SelectEntity(&r_worldentity);

	switch(qrenderer)
	{
#ifdef GLQUAKE
	case QR_OPENGL:
		GL_ViewportUpdate();
		if (lighttype & LSHADER_ORTHO)
			qglEnable(GL_DEPTH_CLAMP_ARB);
		GL_CullFace(SHADER_CULL_FRONT);
		if (smesh)
			GLBE_RenderShadowBuffer(smesh->numverts, smesh->vefbo[0], smesh->verts, smesh->numindicies, smesh->vefbo[1], smesh->indicies);
		break;
#endif
#ifdef VKQUAKE
	case QR_VULKAN:
		//FIXME: generate a single commandbuffer (requires full separation of viewprojection matrix)
		VKBE_BeginShadowmapFace();
		if (smesh)
			VKBE_RenderShadowBuffer(smesh->vkbuffer);
		break;
#endif
#ifdef D3D11QUAKE
	case QR_DIRECT3D11:
		//opengl render targets are upside down - our code kinda assumes gl
		r_refdef.pxrect.y = r_refdef.pxrect.maxheight -(r_refdef.pxrect.y+r_refdef.pxrect.height);
		D3D11BE_BeginShadowmapFace();
		if (smesh)
			D3D11BE_RenderShadowBuffer(smesh->numverts, smesh->d3d11_vbuffer, smesh->numindicies, smesh->d3d11_ibuffer);
		break;
#endif
	default:
		//FIXME: should be able to merge batches between textures+lightmaps.
		if (smesh)
		for (tno = 0; tno < smesh->numbatches; tno++)
		{
			if (!smesh->batches[tno].count)
				continue;
			tex = cl.worldmodel->shadowbatches[tno].tex;
			if (tex->shader->flags & (SHADER_NOSHADOWS|SHADER_NODRAW))	//FIXME: shadows not lights
				continue;
			BE_DrawMesh_List(tex->shader, smesh->batches[tno].count, smesh->batches[tno].s, cl.worldmodel->shadowbatches[tno].vbo, NULL, 0);
		}
		break;
	}

	//fixme: this walks through the entity lists up to 6 times per frame per entity.
	switch(qrenderer)
	{
	default:
		break;
#ifdef GLQUAKE
	case QR_OPENGL:
		{
		//nettest: ENTDRAW.  This is the "walks the entity lists up to 6 times per frame per entity"
		//admitted just above -- one full pass per shadow face, 7 faces at the shipped cvar defaults.
		//Bracketed so we know whether the 429us Shadow generation bucket is dominated by these
		//repeated walks or by the classification loop, instead of assuming.
		RSpeedMark();
		GLBE_BaseEntTextures(lightpvs, NULL);
		RSpeedEnd(RSPEED_SHADOW_ENTDRAW);
		}

		if (lighttype & LSHADER_ORTHO)
			qglDisable(GL_DEPTH_CLAMP_ARB);
		break;
#endif
#ifdef D3D9QUAKE
	case QR_DIRECT3D9:
		D3D9BE_BaseEntTextures(lightpvs, NULL);
		break;
#endif
#ifdef D3D11QUAKE
	case QR_DIRECT3D11:
		D3D11BE_BaseEntTextures(lightpvs, NULL);
		break;
#endif
#ifdef VKQUAKE
	case QR_VULKAN:
		VKBE_BaseEntTextures(lightpvs, NULL);
		break;
#endif
	}

/*
	{
		int i;
		static float depth[SHADOWMAP_SIZE*SHADOWMAP_SIZE];
		qglReadPixels(0, 0, smsize, smsize,
			GL_DEPTH_COMPONENT, GL_FLOAT, depth);
		for (i = SHADOWMAP_SIZE*SHADOWMAP_SIZE; i --> 0; )
		{
			if (depth[i] == 1)
				*((unsigned int*)depth+i) = 0;
			else
				*((unsigned int*)depth+i) = 0xff000000|((((unsigned char)(int)(depth[i]*128)))*0x10101);
		}

		qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
			smsize, smsize, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, depth);

		qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	}
*/
}

qboolean Sh_GenShadowMap (dlight_t *l, int lighttype, vec3_t axis[3], qbyte *lvis, int smsize, int txsize)
{
	int restorefbo = 0;
	int f,lf;
	float oprojs[16], oprojv[16], oview[16];
	pxrect_t oprect;
	shadowmesh_t *smesh;
	int sidevisible;
	int oldflip = r_refdef.flipcull;
	unsigned int oldcolourmask = r_refdef.colourmask;
	int oldexternalview = r_refdef.externalview;
	int twidth;
	int theight;
	int smapidx;
	uploadfmt_t fmt;

	if (r_shadow_shadowmapping_depthbits.ival >= 32 && sh_config.texfmt[PTI_DEPTH32])
		fmt = PTI_DEPTH32;
	else if (r_shadow_shadowmapping_depthbits.ival >= 24 && sh_config.texfmt[PTI_DEPTH24])
		fmt = PTI_DEPTH24;
	else if (r_shadow_shadowmapping_depthbits.ival >= 24 && sh_config.texfmt[PTI_DEPTH24_8])
		fmt = PTI_DEPTH24_8;
	else
		fmt = PTI_DEPTH16;
	(void)fmt;

	if (lighttype & (LSHADER_SPOT|LSHADER_ORTHO))
	{	//spotlights only face forwards. which is side 4. which is annoying.
		f = 4;
		lf = f+1;
		sidevisible = 1<<f;
		twidth = theight = txsize;

		if (lighttype & LSHADER_FAKESHADOWS)
			smapidx = 2;
		else
			smapidx = 1;
	}
	else
	{
		f = 0;
		lf = 6;
		sidevisible = (1<<6)-1;
		twidth = txsize*3;
		theight = txsize*2;
		smapidx = 0;
	}
	if (R_CullSphere(l->origin, 0))
	{	//if the light's center isn't onscreen, cull individual faces
		//FIXME: if the fov is < 90, we need to clip by the near lightplane first
		for (; f < lf; f++)
		{
			vec4_t planes[5];
			float dist;
			int fp,lp;
			Sh_LightFrustumPlanes(l, axis, planes, f);
			for (fp = 0; fp < r_refdef.frustum_numplanes; fp++)
			{
				vec3_t nearest;
				//make a guess based upon the frustum plane
				VectorMA(l->origin, l->radius, r_refdef.frustum[fp].normal, nearest);
				//clip that point to the various planes

				for(lp = 0; lp < 5; lp++)
				{
					dist = DotProduct(nearest, planes[lp]) - planes[lp][3];
					if (dist < 0)
						VectorMA(nearest, dist, planes[lp], nearest);
				}

//				P_RunParticleEffect(nearest, vec3_origin, 15, 1);
				//give up if the best point for any frustum plane is offscreen
				dist = DotProduct(r_refdef.frustum[fp].normal, nearest) - r_refdef.frustum[fp].dist;
				if (dist <= 0)
					break;		
			}
			if (fp != r_refdef.frustum_numplanes)
				sidevisible &= ~(1u<<f);
		}
	}

	//if nothing is visible, then there's no point generating any shadowmaps at all...
	if (!sidevisible)
		return false;

	memcpy(oprojs, r_refdef.m_projection_std, sizeof(oprojs));
	memcpy(oprojv, r_refdef.m_projection_view, sizeof(oprojv));
	memcpy(oview, r_refdef.m_view, sizeof(oview));
	oprect = r_refdef.pxrect;
	if (lighttype & LSHADER_FAKESHADOWS)
		smesh = NULL;
	else
		smesh = SHM_BuildShadowMesh(l, lvis, (lighttype & LSHADER_ORTHO)?SMT_ORTHO:SMT_SHADOWMAP);

	if (lighttype & LSHADER_SPOT)
		Matrix4x4_CM_Projection_Far(r_refdef.m_projection_std, l->fov, l->fov, l->nearclip?l->nearclip:r_shadow_shadowmapping_nearclip.value, l->radius, false);
	else if (lighttype & LSHADER_ORTHO)
	{
		float xmin = -l->radius;
		float ymin = -l->radius;
		float znear = -l->radius;
		float xmax = l->radius;
		float ymax = l->radius;
		float zfar = l->radius;
		Matrix4x4_CM_Orthographic(r_refdef.m_projection_std, xmin, xmax, ymax, ymin, znear, zfar);
	}
	else
		Matrix4x4_CM_Projection_Far(r_refdef.m_projection_std, 90, 90, l->nearclip?l->nearclip:r_shadow_shadowmapping_nearclip.value, l->radius, false);

	memcpy(r_refdef.m_projection_view, r_refdef.m_projection_std, sizeof(r_refdef.m_projection_view));

	switch(qrenderer)
	{
	default:
		return false;
#ifdef GLQUAKE
	case QR_OPENGL:
		if (!GLBE_BeginShadowMap(smapidx, twidth, theight, fmt, &restorefbo))
			return false;
		break;
#endif
#ifdef D3D11QUAKE
	case QR_DIRECT3D11:
		if (!D3D11_BeginShadowMap(smapidx, twidth, theight))
			return false;
		break;
#endif

#ifdef VKQUAKE
	case QR_VULKAN:
		if (!VKBE_BeginShadowmap(smapidx, twidth, theight))
			return false;
		break;
#endif
	}

	r_refdef.externalview = true;	//never any viewmodels

	/*generate faces*/
	for (f = 0; f < 6; f++)
	{
		if (sidevisible & (1u<<f))
		{
			RQuantAdd(RQUANT_SHADOWSIDES, 1);
			Sh_GenShadowFace(l, axis, lighttype, smesh, f, smsize, txsize, r_refdef.m_projection_std, lvis);
		}
	}

	memcpy(r_refdef.m_view, oview, sizeof(r_refdef.m_view));
	memcpy(r_refdef.m_projection_std, oprojs, sizeof(r_refdef.m_projection_std));
	memcpy(r_refdef.m_projection_view, oprojv, sizeof(r_refdef.m_projection_view));

	r_refdef.pxrect = oprect;

	r_refdef.flipcull = oldflip;
	r_refdef.colourmask = oldcolourmask;
	r_refdef.externalview = oldexternalview;
	R_SetFrustum(r_refdef.m_projection_std, r_refdef.m_view);

	switch(qrenderer)
	{
#ifdef GLQUAKE
	case QR_OPENGL:
		/*end framebuffer*/
		GLBE_EndShadowMap(restorefbo);
		GL_ViewportUpdate();
		break;
#endif
#ifdef D3D11QUAKE
	case QR_DIRECT3D11:
		D3D11_EndShadowMap();
		D3D11BE_DoneShadows();
		break;
#endif
#ifdef VKQUAKE
	case QR_VULKAN:
		VKBE_DoneShadows();
		break;
#endif
	default:
		(void)restorefbo;
		break;
	}

	return true;
}

qboolean Sh_GenerateShadowMap(dlight_t *l, int lighttype)
{
	int smsize;
	qbyte *vvis = r_refdef.scenevis;
	qbyte *lvis;
	int texwidth, texheight;
	
/*	if (Sh_ScissorForBox(mins, maxs, &rect))
	{
		RQuantAdd(RQUANT_RTLIGHT_CULL_SCISSOR, 1);
		return;
	}*/

	if (vvis)
	{
		if (!l->rebuildcache && l->worldshadowmesh)
		{
			lvis = l->worldshadowmesh->litleaves;
			//fixme: check head node first?
			if (!Sh_LeafInView(l->worldshadowmesh->litleaves, vvis))
			{
				RQuantAdd(RQUANT_RTLIGHT_CULL_PVS, 1);
				return false;
			}
		}
		else
		{
			int clus;
			clus = cl.worldmodel->funcs.ClusterForPoint(cl.worldmodel, l->origin, NULL);
			lvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, clus, &lvisb, PVM_FAST);
			//FIXME: surely we can use the phs for this?

			if (!Sh_VisOverlaps(lvis, vvis))	//The two viewing areas do not intersect.
			{
				RQuantAdd(RQUANT_RTLIGHT_CULL_PVS, 1);
				return false;
			}
		}
	}
	else
		lvis = NULL;


	if (lighttype & (LSHADER_SPOT | LSHADER_ORTHO))
		texwidth = texheight = smsize = SHADOWMAP_SIZE;
	else
	{
		//Stolen from DP. Actually, LH pasted it to me in IRC.
		vec3_t nearestpoint;
		vec3_t d;
		float distance, lodlinear;
		nearestpoint[0] = bound(l->origin[0]-l->radius, r_origin[0], l->origin[0]+l->radius);
		nearestpoint[1] = bound(l->origin[1]-l->radius, r_origin[1], l->origin[1]+l->radius);
		nearestpoint[2] = bound(l->origin[2]-l->radius, r_origin[2], l->origin[2]+l->radius);
		VectorSubtract(nearestpoint, r_origin, d);
		distance = VectorLength(d);
		lodlinear = (l->radius * r_shadow_shadowmapping_precision.value) / sqrt(max(1.0f, distance / l->radius));
		smsize = bound(16, lodlinear, SHADOWMAP_SIZE);
		texwidth = smsize*3;
		texheight = smsize*2;
	}

	switch(qrenderer)
	{
#ifdef GLQUAKE
	case QR_OPENGL:
		GLBE_SetupForShadowMap(l, texwidth, texheight, (smsize-4) / (float)SHADOWMAP_SIZE);
		break;
#endif
#ifdef D3D11QUAKE
	case QR_DIRECT3D11:
		D3D11BE_SetupForShadowMap(l, texwidth, texheight, (smsize-4) / (float)SHADOWMAP_SIZE);
		break;
#endif
#ifdef VKQUAKE
	case QR_VULKAN:
		VKBE_SetupForShadowMap(l, texwidth, texheight, (smsize-4) / (float)SHADOWMAP_SIZE);
		break;
#endif
	default:
		(void)texwidth;
		(void)texheight;
		break;
	}

	//fixme: light rotation
	if (!Sh_GenShadowMap(l, lighttype, l->axis, lvis, smsize, SHADOWMAP_SIZE))
		return false;	//didn't need to do anything
	return true;
}

#ifdef _MSC_VER
#define round(a) floor((a)+0.5)
#endif

void Sh_OrthoAlignToFrustum(dlight_t *dl, int smsize)
{
	vec3_t neworg;
	double dot;
	double scale;
	int i;
	//fixme: fit to frustum
	VectorMA(r_origin, dl->radius/3, vpn, neworg);
	VectorMA(neworg, -r_shadows_focus.vec4[2], vpn, neworg);
	VectorMA(neworg, -r_shadows_focus.vec4[0], vright, neworg);
	VectorMA(neworg, -r_shadows_focus.vec4[1], vup, neworg);
	//nettest: WHOLE-TEXEL snap.  The ortho projection spans -radius..+radius = 2*radius world units
	//over smsize texels (gl_backend.c LSHADER_ORTHO: xmin=-radius,xmax=+radius), so ONE texel is
	//exactly 2*radius/smsize.  The old `dl->radius/(smsize*2)` snapped neworg to a QUARTER-texel
	//lattice (it's texel/4), which does NOT stabilise the sampling phase: the frustum centre rides
	//r_origin + (radius/3)*vpn, so it slides sub-texel amounts every frame as the CAMERA ROTATES,
	//and a quarter-texel grid leaves a world point's fractional texel position free to flip between
	//{0,¼,½,¾} → the depth-compare boundary steps → shadow EDGES CRAWL (worse at higher
	//r_shadows_distance: coarser texels AND a bigger radius/3 swing).  Snapping to a WHOLE texel makes
	//every world point hash to the same texel every frame regardless of the swing → the crawl is gone,
	//with zero quality loss (the depth-axis snap is a no-op: a translation along the light dir cancels
	//in the light-space depth compare).
	scale = 2.0*dl->radius/smsize;
	for (i = 0; i < 3; i++)
	{
		dot = DotProduct_Double(neworg, dl->axis[i]);
		dot /= scale;
		dot = round(dot)-dot;
		dot *= scale;
		VectorMA(neworg, dot, dl->axis[i], neworg); //realign it on this axis.
	}
	VectorCopy(neworg, dl->origin);
}

qboolean r_fakeshadows;
static dlight_t r_fakelight;

//nettest P110: multi-direction fake shadows =========================================================
//
//An ortho projection has exactly ONE parallel direction, so the classic r_shadows 2 map throws every
//prop's shadow along the sun.  Here the depth map is split into N cells; each cell is rendered from a
//different DOMINANT LIGHT direction and holds only the casters whose own light points that way.  So a
//barrel beside a lamp shadows away from the lamp while one out in the open still shadows along the sun.
//
//WHY THIS CAN WORK NOW.  Patch 93 built this same atlas and it was retired (Patch 95) because slot
//assignment came from a QC registry fed by BlobShadow_Emit: prop_static scenery has no CSQC predraw and
//settled physics props unhook theirs, so static props never registered and got rendered into whatever
//MOVING caster's box happened to cover them -- inheriting the player's angle.  Patch 108's
//R_EntityDominantLightDir removes the registry entirely: the direction comes from the map's deluxemap,
//per entity, for engine-drawn prop_static as much as anything else.
//
//STABILITY IS THE WHOLE GAME.  A prop whose slot changes between frames visibly POPS its shadow, so:
//  - directions quantise to a FIXED angular lattice, and a bucket's representative is the lattice cell
//    CENTRE, never the mean of its members (a mean drifts as members join/leave and props chase it);
//  - a prop_static never moves -> the Patch-105 origin-keyed light cache never invalidates -> its
//    light_dir is bit-identical every frame -> its bucket id is literally constant;
//  - which buckets OWN slots is sticky (hysteresis + an idle timer), so a prop briefly hidden behind a
//    corner doesn't surrender its slot for one frame and snap back.
#define SH_AZ 16	/*azimuth sectors*/
#define SH_EL 4		/*elevation bands, equal-AREA in z so the poles don't crowd*/
#define SH_BUCKETS (SH_AZ*SH_EL)	/*64 ids, ~13 degree mean cell half-angle*/

static qbyte  *fs_entbucket;		/*slot index per visedict; parallel to cl_visedicts*/
static int     fs_entbucket_max;
static int     fs_slotbucket[MAX_FAKESHADOW_SLOTS];	/*bucket id each slot owns; -1 = free. PERSISTS across frames (hysteresis)*/
static int     fs_slotcount;		/*slots live THIS frame; 1 = legacy single path*/
static int     fs_curslot = -1;		/*cell currently rendering; -1 = not inside the atlas pass*/
static qboolean fs_sunpass;			/*Phase-0: true ONLY while the fake-SUN depth pass renders (distinguishes it from real rtlight depth passes, which also reach Sh_FakeShadowFilter with fs_curslot<0)*/
static qboolean fs_propshadowpass;	/*Patch 120: true while the PROPSHADOW atlas renders (as opposed to the r_shadows_slots direction atlas).  The two paths disagree about what fs_entbucket means for the sun cell -- see Sh_FakeShadowFilter.*/
static vec3_t  fs_sundir;			/*Phase-0: direction TOWARD the sun, for the caster sun-visibility gate*/
//BY-CONE lamp casting: during a LAMP cell's caster depth pass, admit ANY visedict whose origin sits
//inside that lamp's cone -- not just the one prop the clusterer assigned it to.  So a caster near two
//lamps casts into both (multiple projections), and any model that walks into an existing cone casts
//without depending on the (fragile) per-caster in-shade classification -- the PLAYER included.  Set
//from the lamp-cell loop right before its caster render; consumed by Sh_FakeShadowFilter.
#define FS_WORLDSLOT 0x7f			/*fs_curslot sentinel for the world-occlusion cell: admit NO entity*/
static vec3_t  fs_cone_org;			/*current lamp cell cone: apex (lamp origin)...*/
static vec3_t  fs_cone_axis;		/*...unit aim...*/
static float   fs_cone_rad2;		/*...(reach + pad)^2...*/
static float   fs_cone_coscos;		/*...cos^2(half-fov+pad), the angular gate*/
static vec3_t  fs_slotdir[MAX_FAKESHADOW_SLOTS];	/*eased THROW direction actually rendered*/
static vec3_t  fs_slotorg[MAX_FAKESHADOW_SLOTS];	/*eased box centre (non-sun slots are FITTED to their props)*/
static float   fs_slotrad[MAX_FAKESHADOW_SLOTS];	/*eased box half-extent*/
static qboolean fs_slotsmooth[MAX_FAKESHADOW_SLOTS];/*false = no previous value to ease from, snap instead*/

//Sh_OrthoAlignToFrustum centres the box on the VIEW.  A fitted per-prop-cluster cell needs to centre on
//the cluster instead -- and must stay view-INDEPENDENT, or the whole cell's shadows would swing as the
//player walks (the exact flaw that sank Patch 93's per-light boxes).  Same whole-texel snap, which is what
//keeps shadow edges from crawling: a world point must hash to the same texel every frame.
static void Sh_OrthoAlignToPoint(dlight_t *dl, const vec3_t centre, int smsize)
{
	vec3_t neworg;
	double dot, scale;
	int i;
	VectorCopy(centre, neworg);
	scale = 2.0*dl->radius/smsize;
	for (i = 0; i < 3; i++)
	{
		dot = DotProduct_Double(neworg, dl->axis[i]);
		dot /= scale;
		dot = round(dot)-dot;
		dot *= scale;
		VectorMA(neworg, dot, dl->axis[i], neworg);
	}
	VectorCopy(neworg, dl->origin);
}

static int Sh_DirBucketId(const vec3_t d)
{
	int el = (int)((d[2]*0.5+0.5) * SH_EL);
	int az = (int)((atan2(d[1], d[0]) + M_PI) * (SH_AZ/(2*M_PI)));
	el = bound(0, el, SH_EL-1);
	az &= (SH_AZ-1);
	return el*SH_AZ + az;
}

static void Sh_BucketIdToDir(int id, vec3_t out)
{
	int el = id / SH_AZ, az = id % SH_AZ;
	float z  = ((el+0.5f) / SH_EL) * 2.0f - 1.0f;
	float a  = ((az+0.5f) / SH_AZ) * (2*M_PI) - M_PI;
	float r  = sqrt(max(0, 1.0f - z*z));
	out[0] = cos(a)*r;
	out[1] = sin(a)*r;
	out[2] = z;
}

//Called from BE_GenModelBatches' BEM_DEPTHONLY filter: does this visedict belong in the cell we are
//rendering right now?  The fs_curslot guard is LOAD-BEARING -- without it every real rtlight shadow map
//in the game (which also renders BEM_DEPTHONLY) would get filtered by our bucket table.
int Sh_FakeShadowFilter(int visedictindex)
{
	int base;
	if (fs_curslot < 0 || fs_slotcount <= 1)
		base = 1;	/*not our pass (rtlight/cascade/single), or legacy single-direction: everything casts*/
	else if (fs_curslot == FS_WORLDSLOT)
		return 0;	/*the world-occlusion cell renders WORLD depth only -- no entities*/
	else if (fs_curslot > 0)
	{	//a LAMP cell: admit any caster geometrically inside this lamp's cone (by-cone, see fs_cone_*),
		//regardless of which lamp the clusterer assigned it to -- so the player (and any model) casts
		//into every nearby lamp instead of at most one, and casting no longer hinges on the per-caster
		//in-shade classification.  fs_entbucket is now consulted only by the SUN branch (below).
		vec3_t d;
		float dist2, proj;
		if ((unsigned)visedictindex >= (unsigned)cl_numvisedicts)
			return 0;
		VectorSubtract(cl_visedicts[visedictindex].origin, fs_cone_org, d);
		dist2 = DotProduct(d, d);
		if (dist2 > fs_cone_rad2)
			return 0;			//past the lamp's reach
		proj = DotProduct(d, fs_cone_axis);
		if (proj <= 0)
			return 0;			//behind the lamp
		if (proj*proj < fs_cone_coscos * dist2)
			return 0;			//outside the cone half-angle
		return 1;
	}
	else if ((unsigned)visedictindex >= (unsigned)fs_entbucket_max)
		base = 1;	/*sun cell (fs_curslot==0), unbucketed -> the sun owns it*/
	else
		base = (fs_entbucket[visedictindex] == 0);
			/*Sun cell: admit only casters NOT owned by a lamp.
			  For the PROPSHADOW path (Patch 120a) that means "not classified in shade" -- a prop under a
			  roof should be lit and shadowed by the lamp above it, and a hard sun shadow from something
			  standing indoors makes no sense.  Sunlit props (and every prop on a map with no bake to
			  classify with) stay in bucket 0 and keep their sun shadow.
			  For the r_shadows_slots direction atlas it means the histogram gave the caster to a
			  non-sun direction slot, which really is exclusive.
			  Patch 120's blanket bypass here was WRONG: it also gave in-shade props a sun shadow, which
			  is what "the shadows don't make any sense indoors" was.  What actually needed fixing was
			  the CHATTER -- see Sh_PropShadeState's debounce.*/
	if (!base)
		return 0;

	//nettest Phase-0: caster sun-visibility gate.  ONLY during the fake-SUN pass (fs_sunpass) and only for
	//the sun cell(s) (fs_curslot<=0: -1 = cascade/single, 0 = the slots-path sun cell): drop a caster whose
	//dominant baked light disagrees with the sun beyond the cutoff -- it is lamp-lit / in baked shade, so a
	//hard sun shadow from it reads wrong and double-darkens the bake.  Real rtlight depth passes have
	//fs_sunpass=0; non-sun lamp slots have fs_curslot>0 -- both untouched.  Fail-safe: no deluxemap dir ->
	//R_EntityDominantLightDir returns false -> still casts (classic behaviour).
	//Patch 120c: r_shadows_propshadows_suncast makes this the SAME decision as the fs_entbucket stamp --
	//"may an in-shade caster stay in the sun cascades" -- so one cvar has to govern both, or turning it
	//on would still see deluxemapped maps silently evicting casters here.
	if (fs_sunpass && fs_curslot <= 0 && r_shadows_caster_sunvis.value > 0 &&
		!(fs_propshadowpass && r_shadows_propshadows_suncast.ival))
	{
		vec3_t dir;
		if (R_EntityDominantLightDir(&cl_visedicts[visedictindex], dir))
			if (DotProduct(dir, fs_sundir) < r_shadows_caster_sunvis.value)
				return 0;
	}
	return base;
}

//Histogram the visible casters by light direction and hand the N-1 most popular directions a slot.
//Returns the slot count to use this frame (1 = nothing worth splitting, take the legacy path).
static int Sh_FakeShadowChooseSlots(dlight_t *l)
{
	int counts[SH_BUCKETS];
	int i, s, b, want, claimed;
	int slotcount = bound(1, r_shadows_slots.ival, 8);	/*the direction-atlas L-layout holds 8 cells max, regardless of MAX_FAKESHADOW_SLOTS*/
	vec3_t sundir, dir, bdir;
	float maxdist;
	static int fs_slotidle[MAX_FAKESHADOW_SLOTS];
	static qboolean fs_init;

	if (!fs_init)
	{
		for (i = 0; i < MAX_FAKESHADOW_SLOTS; i++)
			fs_slotbucket[i] = -1;
		fs_init = true;
	}

	if (slotcount <= 1)
		return 1;

	//grow the side table with the visedict array. cl_visedicts is only reallocated in
	//CL_ClearEntityLists, once per client frame BEFORE rendering, so indices are stable for the
	//whole render frame (including portal/mirror recursion, which only appends).
	if (fs_entbucket_max < cl_maxvisedicts)
	{
		Z_Free(fs_entbucket);
		fs_entbucket_max = cl_maxvisedicts;
		fs_entbucket = Z_Malloc(fs_entbucket_max);
	}
	if (fs_entbucket)
		memset(fs_entbucket, 0, fs_entbucket_max);	/*default = slot 0 = the sun*/

	memset(counts, 0, sizeof(counts));
	VectorNegate(l->axis[0], sundir);	/*axis[0] is the THROW dir; buckets are TOWARD the light*/
	maxdist = l->radius;

	for (i = r_refdef.firstvisedict; i < cl_numvisedicts; i++)
	{
		entity_t *ent = &cl_visedicts[i];
		vec3_t ofs;
		float rad;

		//mirror BE_GenModelBatches' BEM_DEPTHONLY rejects: anything that won't be rendered as a
		//caster must not sway the histogram.
		if (ent->flags & (RF_NOSHADOW|RF_ADDITIVE|RF_NODEPTHTEST|RF_TRANSLUCENT))
			continue;
		if ((ent->flags & RF_EXTERNALMODEL) && !r_shadow_playershadows.ival)
			continue;
		if (!ent->model || ent->model->type != mod_alias)
			continue;
		if (ent->model->engineflags & MDLF_FLAME)
			continue;

		//too far to land in ANY cell (every slot shares this radius and centre)
		rad = ent->model->radius;
		VectorSubtract(ent->origin, l->origin, ofs);
		if (DotProduct(ofs, ofs) > (maxdist+rad)*(maxdist+rad))
			continue;

		if (!R_EntityDominantLightDir(ent, dir))
			continue;	/*no per-entity info -> stays on the sun slot*/

		b = Sh_DirBucketId(dir);
		//already essentially the sun? slot 0 serves it, and splitting it out would only cost a cell.
		Sh_BucketIdToDir(b, bdir);
		if (DotProduct(bdir, sundir) > 0.93969f)	/*within 20 degrees*/
			continue;
		counts[b]++;
	}

	//--- slot ownership: incumbents are sticky, challengers must clearly beat them ---
	claimed = 0;
	for (s = 1; s < slotcount; s++)
	{
		b = fs_slotbucket[s];
		if (b >= 0 && counts[b] > 0)
		{
			fs_slotidle[s] = 0;
			claimed |= 1<<s;
		}
		else if (b >= 0 && ++fs_slotidle[s] < 30)
			claimed |= 1<<s;	/*briefly occluded -- hold the slot rather than pop the shadow*/
		else
			fs_slotbucket[s] = -1;
	}
	for (s = 1; s < slotcount; s++)
	{
		int best = -1, bestcount = 0;
		if (claimed & (1<<s))
			continue;
		for (b = 0; b < SH_BUCKETS; b++)
		{
			int taken = 0, s2;
			for (s2 = 1; s2 < slotcount; s2++)
				if (fs_slotbucket[s2] == b) { taken = 1; break; }
			if (taken)
				continue;
			if (counts[b] > bestcount)
				{ bestcount = counts[b]; best = b; }
		}
		if (best < 0)
			break;
		fs_slotbucket[s] = best;
		fs_slotidle[s] = 0;
	}
	//an incumbent only loses its slot to a CLEARLY more popular direction
	for (s = 1; s < slotcount; s++)
	{
		int own = fs_slotbucket[s];	/*NB: not "inc" -- gl_shadow.c has a `#define inc 128` above*/
		float thresh;
		if (own < 0)
			continue;
		thresh = counts[own] * max(1.0f, r_shadows_slots_hyst.value);
		for (b = 0; b < SH_BUCKETS; b++)
		{
			int taken = 0, s2;
			for (s2 = 1; s2 < slotcount; s2++)
				if (fs_slotbucket[s2] == b) { taken = 1; break; }
			if (!taken && counts[b] > thresh)
				{ fs_slotbucket[s] = b; fs_slotidle[s] = 0; break; }
		}
	}

	//--- how many slots carry casters at all ---
	want = 1;
	for (s = 1; s < slotcount; s++)
		if (fs_slotbucket[s] >= 0 && counts[fs_slotbucket[s]])
			want = s+1;

	//--- second pass: stamp each caster's slot, and gather that slot's TRUE mean direction + bounds ---
	//The lattice bucket decided MEMBERSHIP (stable), but it is far too coarse to render with: its cell
	//centre can be ~13 degrees off, so a prop crossing a lattice boundary would jump ~26 degrees.  The
	//mean of the members' own directions is continuous, so it tracks the way the sunshade does.
	{
		vec3_t dirsum[MAX_FAKESHADOW_SLOTS], bmin[MAX_FAKESHADOW_SLOTS], bmax[MAX_FAKESHADOW_SLOTS];
		int    nmemb[MAX_FAKESHADOW_SLOTS];

		for (s = 0; s < MAX_FAKESHADOW_SLOTS; s++)
		{
			VectorClear(dirsum[s]);
			nmemb[s] = 0;
			bmin[s][0] = bmin[s][1] = bmin[s][2] =  FLT_MAX;
			bmax[s][0] = bmax[s][1] = bmax[s][2] = -FLT_MAX;
		}

		if (want > 1 && fs_entbucket)
		{
			for (i = r_refdef.firstvisedict; i < cl_numvisedicts; i++)
			{
				entity_t *ent = &cl_visedicts[i];
				float rad;
				if (!ent->model || ent->model->type != mod_alias)
					continue;
				if (!R_EntityDominantLightDir(ent, dir))
					continue;
				b = Sh_DirBucketId(dir);
				for (s = 1; s < want; s++)
				{
					if (fs_slotbucket[s] != b)
						continue;
					fs_entbucket[i] = s;
					VectorAdd(dirsum[s], dir, dirsum[s]);
					nmemb[s]++;
					rad = ent->model->radius;
					for (b = 0; b < 3; b++)
					{
						bmin[s][b] = min(bmin[s][b], ent->origin[b] - rad);
						bmax[s][b] = max(bmax[s][b], ent->origin[b] + rad);
					}
					break;
				}
			}
		}

		//--- publish, easing onto the targets so transitions glide instead of snapping ---
		//slot 0 is the sun: view-centred, full radius, exactly as the legacy single path.
		VectorCopy(l->axis[0], fs_slotdir[0]);
		VectorCopy(l->origin,  fs_slotorg[0]);
		fs_slotrad[0] = l->radius;
		fs_slotsmooth[0] = true;

		for (s = 1; s < slotcount; s++)
		{
			vec3_t tdir, torg;
			float trad, lerp;

			if (s >= want || !nmemb[s])
			{	//owns a direction with nothing visible right now: keep ownership (the idle timer decides
				//when to release it) but render nothing into the cell, and forget the eased state so the
				//slot snaps cleanly when it next picks up casters instead of sweeping in from stale values.
				VectorCopy(l->axis[0], fs_slotdir[s]);
				VectorCopy(l->origin,  fs_slotorg[s]);
				fs_slotrad[s] = l->radius;
				fs_slotsmooth[s] = false;
				continue;
			}

			VectorCopy(dirsum[s], tdir);
			if (!VectorNormalize(tdir))
				{ Sh_BucketIdToDir(fs_slotbucket[s], tdir); }	/*members cancelled out - fall back to the lattice*/
			VectorNegate(tdir, tdir);	/*directions point TOWARD the light; we render the THROW dir*/

			//fit the box to these props plus the room their shadows need to reach the floor.  This is
			//why a non-sun cell is SHARPER than the sun cell despite being a quarter of the texture: it
			//covers a few hundred units instead of the whole view volume.
			for (b = 0; b < 3; b++)
				torg[b] = (bmin[s][b] + bmax[s][b]) * 0.5f;
			trad = 0;
			for (b = 0; b < 3; b++)
				trad = max(trad, (bmax[s][b] - bmin[s][b]) * 0.5f);
			trad += max(0, r_shadows_slots_margin.value);
			trad = bound(64, trad, l->radius);

			if (!fs_slotsmooth[s] || r_shadows_slots_smooth.value <= 0)
			{	//no previous value to ease from (or easing disabled) -> take the target outright
				VectorCopy(tdir, fs_slotdir[s]);
				VectorCopy(torg, fs_slotorg[s]);
				fs_slotrad[s] = trad;
				fs_slotsmooth[s] = true;
			}
			else
			{	//exponential ease, frame-rate independent
				lerp = 1.0f - exp(-host_frametime / r_shadows_slots_smooth.value);
				lerp = bound(0, lerp, 1);
				VectorInterpolate(fs_slotdir[s], lerp, tdir, fs_slotdir[s]);
				if (!VectorNormalize(fs_slotdir[s]))
					VectorCopy(tdir, fs_slotdir[s]);
				VectorInterpolate(fs_slotorg[s], lerp, torg, fs_slotorg[s]);
				fs_slotrad[s] += (trad - fs_slotrad[s]) * lerp;
			}
		}
	}

	if (r_shadows_slots_debug.ival)
	{
		int percell[MAX_FAKESHADOW_SLOTS];
		memset(percell, 0, sizeof(percell));
		if (fs_entbucket)
			for (i = r_refdef.firstvisedict; i < cl_numvisedicts; i++)
				if (fs_entbucket[i] < MAX_FAKESHADOW_SLOTS)
					percell[fs_entbucket[i]]++;
		Con_Printf("^3fakeshadow slots %i:", want);
		for (s = 0; s < want; s++)
			Con_Printf(" [%i]b%i n%i r%.0f (%.2f %.2f %.2f)", s, s?fs_slotbucket[s]:-1, percell[s],
						fs_slotrad[s], fs_slotdir[s][0], fs_slotdir[s][1], fs_slotdir[s][2]);
		Con_Printf("\n");
	}

	return want;
}
//====================================================================================================

#ifdef GLQUAKE
//nettest P110 atlas layout.  NOT an even grid -- that would spend as many texels on a cell holding three
//props as on the one holding fifty, which is exactly why the first cut of this patch looked lower-res than
//the single map it replaced.
//
//Slot 0 is the sun.  It carries the overwhelming majority of casters AND must cover the whole view volume,
//so it takes a 3/4 x 3/4 cell -- NINE times the area of the others.  The per-light cells are FITTED to
//their own props (a few hundred units across instead of the view volume), so at a quarter of the width
//they still end up SHARPER per world unit than the sun cell, and they tile the remaining L:
//
//      +---------------+---+     u = txsize/4
//      |               | 1 |     slot 0 : 3u x 3u   (sun, view-sized box, ~3/4 of a full-texture map)
//      |               +---+     slots 1+: u x u    (fitted boxes, far denser per world unit)
//      |       0       | 2 |
//      |     (3u)      +---+
//      |               | 3 |
//      +---+---+---+---+---+
//      | 4 | 5 | 6 | 7 |
//      +---+---+---+---+
static void Sh_FakeShadowCellRect(int slot, int slots, int txsize, int *ox, int *oy, int *osize)
{
	int u = txsize/4;
	if (slots <= 1)
		{ *ox = 0; *oy = 0; *osize = txsize; return; }	/*legacy: the whole texture*/
	if (slot == 0)
		{ *ox = 0; *oy = 0; *osize = 3*u; return; }
	switch (slot)
	{
	case 1:  *ox = 3*u; *oy = 0;   break;
	case 2:  *ox = 3*u; *oy = u;   break;
	case 3:  *ox = 3*u; *oy = 2*u; break;
	case 4:  *ox = 0;   *oy = 3*u; break;
	case 5:  *ox = u;   *oy = 3*u; break;
	case 6:  *ox = 2*u; *oy = 3*u; break;
	default: *ox = 3*u; *oy = 3*u; break;	/*slot 7 -- the layout holds 8 in total*/
	}
	*osize = u;
}

//nettest P110: render N model-only depth passes, one per cast direction, into cells of ONE texture.
//
//Deliberately NOT built on Sh_GenShadowMap: that function does its own BeginShadowMap/EndShadowMap pair
//and BeginShadowMap CLEARS THE WHOLE TEXTURE, so calling it per slot would wipe every cell rendered
//before it.  Hence one Begin here, then a manual per-cell viewport loop.
static void Sh_GenerateFakeShadowsAtlas(dlight_t *l, int slots, int txsize)
{
	int inset    = 16;	/*Patch 93's proven margin: wider than the PCF tap radius + edge-fade slack, so
						  neighbouring cells cannot bleed into each other*/
	int s, restorefbo = 0;
	int cx, cy, csize, smsize;
	float oprojs[16], oprojv[16], oview[16];
	pxrect_t oprect;
	unsigned int oldflip, oldcolourmask;
	qboolean oldexternalview;
	uploadfmt_t fmt;
	vec4_t cell;

	Sh_FakeShadowCellRect(0, slots, txsize, &cx, &cy, &csize);
	smsize = csize - 2*inset;	/*slot 0's cell; the loop recomputes this per slot*/
	if (smsize < 64)
		{ GLBE_SetFakeShadowCount(1); return; }	/*r_shadows_res too small to subdivide sensibly*/

	if (r_shadow_shadowmapping_depthbits.ival >= 32 && sh_config.texfmt[PTI_DEPTH32])
		fmt = PTI_DEPTH32;
	else if (r_shadow_shadowmapping_depthbits.ival >= 24 && sh_config.texfmt[PTI_DEPTH24])
		fmt = PTI_DEPTH24;
	else if (r_shadow_shadowmapping_depthbits.ival >= 24 && sh_config.texfmt[PTI_DEPTH24_8])
		fmt = PTI_DEPTH24_8;
	else
		fmt = PTI_DEPTH16;

	memcpy(oprojs, r_refdef.m_projection_std,  sizeof(oprojs));
	memcpy(oprojv, r_refdef.m_projection_view, sizeof(oprojv));
	memcpy(oview,  r_refdef.m_view,            sizeof(oview));
	oprect          = r_refdef.pxrect;
	oldflip         = r_refdef.flipcull;
	oldcolourmask   = r_refdef.colourmask;
	oldexternalview = r_refdef.externalview;

	//Each slot gets its OWN ortho projection, built inside the loop: non-sun cells are FITTED to the props
	//they contain (a few hundred units instead of the whole view volume), which is where their sharpness
	//comes from and why the shader's box test can early-out for most pixels.  Trap #4 from Patch 93 still
	//holds though -- a perspective/spot projection never lines up with the ortho-tuned consumption shader
	//(it came out mirrored and tiny), so every slot stays ORTHOGRAPHIC, only the extent and axes differ.

	//smapidx 2 = the fake-shadow texture, same slot the single path uses.  ONE Begin (it clears all of it).
	if (!GLBE_BeginShadowMap(2, txsize, txsize, fmt, &restorefbo))
		{ GLBE_SetFakeShadowCount(1); return; }

	r_refdef.externalview = true;	//never any viewmodels
	//PCF tap offsets are in ATLAS uv space (the shader remaps cell-local coords into the atlas before
	//sampling), so the scale is 1/txsize -- the whole texture, not the cell.
	GLBE_SetupForShadowMap(l, txsize, txsize, smsize/(float)txsize);

	for (s = 0; s < slots; s++)
	{
		Sh_FakeShadowCellRect(s, slots, txsize, &cx, &cy, &csize);
		smsize = csize - 2*inset;
		if (smsize < 16)
			continue;

		VectorCopy(fs_slotdir[s], l->axis[0]);
		if (!VectorNormalize(l->axis[0]))
			VectorNegate(r_sun_dir.vec4, l->axis[0]);
		VectorVectors(l->axis[0], l->axis[1], l->axis[2]);
		VectorNegate(l->axis[1], l->axis[1]);

		//re-snap the box for THIS direction and extent: the whole-texel lattice is per-axis and its pitch
		//is 2*radius/smsize -- the CELL size, not the texture size.  Getting that wrong reintroduces
		//exactly the shadow-edge crawl the snap exists to kill.
		l->radius = fs_slotrad[s];
		if (s == 0)
			Sh_OrthoAlignToFrustum(l, smsize);	//the sun covers the view, as it always has
		else
			Sh_OrthoAlignToPoint(l, fs_slotorg[s], smsize);	//fitted cells centre on their own props

		//this slot's projection (extent varies per slot now, so it cannot be hoisted out of the loop)
		Matrix4x4_CM_Orthographic(r_refdef.m_projection_std, -l->radius, l->radius, l->radius, -l->radius, -l->radius, l->radius);
		memcpy(r_refdef.m_projection_view, r_refdef.m_projection_std, sizeof(r_refdef.m_projection_view));

		//viewport in TOP-origin coords; GL_ViewportUpdate flips it for GL.
		sh_fakecell_active = true;
		sh_fakecell_x = cx + inset;
		sh_fakecell_y = cy + inset;

		//...and the matching UNIFORM must therefore use the cell's BOTTOM edge (trap #2).  The legacy
		//single path centres its region symmetrically, which makes the flip invisible; asymmetric atlas
		//cells sampled the EMPTY half of the texture and produced zero shadows at any count > 1.
		cell[0] = (cx + inset)                     / (float)txsize;
		cell[1] = (txsize - (cy + inset + smsize)) / (float)txsize;
		cell[2] = cell[3] = smsize / (float)txsize;

		//trap #1: BE_SelectDLight sets shaderstate.curdlight, which the BEM_DEPTHONLY entity batcher
		//dereferences (ent->keynum == dl->key).  Skipping it per slot is a NULL deref the first frame a
		//client spawns with this active.  It also builds the very matrix we snapshot next.
		if (!BE_SelectDLight(l, vec3_origin, l->axis, LSHADER_SMAP|LSHADER_ORTHO))
			continue;
		GLBE_CaptureFakeShadowSlot(s, cell);

		fs_curslot = s;	/*Sh_FakeShadowFilter now admits only this slot's casters*/
		RQuantAdd(RQUANT_SHADOWSIDES, 1);
		//P114b: this cell's ortho differs from the previous cell's, but GLBE_SelectEntity only refreshes
		//the cached render projection when entity flags change (never for world/props).  Force it, or the
		//depth here renders with cell 0's projection while the shader samples with this cell's -> shadows
		//magnified by radius(s)/radius(0).
		GLBE_FlushProjection();
		Sh_GenShadowFace(l, l->axis, LSHADER_SMAP|LSHADER_ORTHO|LSHADER_FAKESHADOWS, NULL, 4, smsize, txsize, r_refdef.m_projection_std, NULL);
	}
	fs_curslot = -1;
	sh_fakecell_active = false;

	//stale matrices in unused slots would project receivers into a cell that now holds a different
	//direction's depth, so neutralise every slot the shader will still loop over.
	for (s = slots; s < MAX_FAKESHADOW_SLOTS; s++)
	{
		Vector4Set(cell, 0, 0, 0, 0);
		GLBE_ClearFakeShadowSlot(s, cell);
	}
	GLBE_SetFakeShadowCount(slots);

	memcpy(r_refdef.m_view,            oview,  sizeof(r_refdef.m_view));
	memcpy(r_refdef.m_projection_std,  oprojs, sizeof(r_refdef.m_projection_std));
	memcpy(r_refdef.m_projection_view, oprojv, sizeof(r_refdef.m_projection_view));
	r_refdef.pxrect       = oprect;
	r_refdef.flipcull     = oldflip;
	r_refdef.colourmask   = oldcolourmask;
	r_refdef.externalview = oldexternalview;
	R_SetFrustum(r_refdef.m_projection_std, r_refdef.m_view);

	GLBE_EndShadowMap(restorefbo);
	GL_ViewportUpdate();

	//leave SLOT 0 selected so the forward pass sees sun-slot backend state: defaultskin's legacy
	//l_cubematrix branch and any other single-map consumer keep working unchanged.
	//NB restore the radius too -- the loop above overwrote l->radius with each fitted cell's extent.
	l->radius = fs_slotrad[0];
	VectorCopy(fs_slotdir[0], l->axis[0]);
	VectorNormalize(l->axis[0]);
	VectorVectors(l->axis[0], l->axis[1], l->axis[2]);
	VectorNegate(l->axis[1], l->axis[1]);
	Sh_FakeShadowCellRect(0, slots, txsize, &cx, &cy, &csize);
	Sh_OrthoAlignToFrustum(l, csize - 2*inset);
	BE_SelectDLight(l, vec3_origin, l->axis, LSHADER_SMAP|LSHADER_ORTHO);
}

//nettest P114: sun cascades =========================================================================
//Equal cells in a 2x2 grid (each half the texture).  Cascades get EQUAL texels on purpose: the near
//cascade's density comes from its ortho box SHRINKING to a few hundred units, not from a bigger cell.
static void Sh_CascadeCellRect(int idx, int n, int txsize, int *ox, int *oy, int *osize)
{
	int h = txsize/2;
	if (n <= 1)
		{ *ox = 0; *oy = 0; *osize = txsize; return; }
	*osize = h;
	*ox = (idx & 1) ? h : 0;
	*oy = (idx & 2) ? h : 0;
}

//Render N nested SUN cascades into cells of ONE texture.  Built on the same one-Begin, per-cell-viewport
//skeleton as Sh_GenerateFakeShadowsAtlas (BeginShadowMap clears the whole texture, so it must be called
//once).  The differences from the direction atlas are the whole point:
//  - every cell is the SAME sun direction (l->axis unchanged across the loop);
//  - fs_curslot stays -1, so Sh_FakeShadowFilter admits EVERY caster into EVERY cascade -- a caster near
//    the view straddles cascades and must appear in each, unlike a direction slot it belongs to just one.
//
//CONCENTRIC, CAMERA-CENTRED (not view-frustum fitted).  An earlier version centred each cascade AHEAD of
//the camera along vpn and fitted it to a frustum slice.  That made the cascade boxes -- and so the boundary
//between them -- SWING with the view DIRECTION: rotating the camera (not even moving) slid the shadows and
//the far, huge cascade whipped around, which read as "shadows move when I look around, corrupt at 3+".  The
//fix is to centre every cascade on the camera POSITION with geometrically growing radii.  Rotating no longer
//moves anything (r_origin is rotation-independent); only walking does, and the whole-texel snap keeps that
//shimmer-free.  The near box is small = dense; each outer box is `ratio`x bigger, out to r_shadows_cascade_dist.
static void Sh_GenerateCascadeAtlas(dlight_t *l, int cascades, int txsize)
{
	int inset = 16;	/*same proven margin as the direction atlas: wider than the PCF tap radius so cells can't bleed*/
	int s, restorefbo = 0;
	int cx, cy, csize, smsize;
	float oprojs[16], oprojv[16], oview[16];
	pxrect_t oprect;
	unsigned int oldflip, oldcolourmask;
	qboolean oldexternalview;
	uploadfmt_t fmt;
	vec4_t cell;
	vec3_t sundir;
	float outer, ratio;
	static int dbgframe;
	qboolean dbg = r_shadows_cascade_debug.ival && ((dbgframe++ % 60) == 0);

	Sh_CascadeCellRect(0, cascades, txsize, &cx, &cy, &csize);
	if (csize - 2*inset < 64)
		{ GLBE_SetFakeShadowCount(1); return; }	/*r_shadows_res too small to subdivide*/

	if (r_shadow_shadowmapping_depthbits.ival >= 32 && sh_config.texfmt[PTI_DEPTH32])
		fmt = PTI_DEPTH32;
	else if (r_shadow_shadowmapping_depthbits.ival >= 24 && sh_config.texfmt[PTI_DEPTH24])
		fmt = PTI_DEPTH24;
	else if (r_shadow_shadowmapping_depthbits.ival >= 24 && sh_config.texfmt[PTI_DEPTH24_8])
		fmt = PTI_DEPTH24_8;
	else
		fmt = PTI_DEPTH16;

	VectorCopy(l->axis[0], sundir);	/*the throw direction the caller set up; every cascade uses it*/
	outer = max(64.0f, r_shadows_cascade_dist.value);	/*radius of the OUTERMOST (last) cascade*/
	ratio = bound(1.5f, r_shadows_cascade_ratio.value, 8.0f);	/*each inner cascade is 1/ratio the next one out*/

	memcpy(oprojs, r_refdef.m_projection_std,  sizeof(oprojs));
	memcpy(oprojv, r_refdef.m_projection_view, sizeof(oprojv));
	memcpy(oview,  r_refdef.m_view,            sizeof(oview));
	oprect          = r_refdef.pxrect;
	oldflip         = r_refdef.flipcull;
	oldcolourmask   = r_refdef.colourmask;
	oldexternalview = r_refdef.externalview;

	if (!GLBE_BeginShadowMap(2, txsize, txsize, fmt, &restorefbo))
		{ GLBE_SetFakeShadowCount(1); return; }

	r_refdef.externalview = true;	//never any viewmodels
	GLBE_SetupForShadowMap(l, txsize, txsize, (csize-2*inset)/(float)txsize);

	//sun axis is constant for the whole loop -- set it once.
	VectorCopy(sundir, l->axis[0]);
	if (!VectorNormalize(l->axis[0]))
		VectorNegate(r_sun_dir.vec4, l->axis[0]);
	VectorVectors(l->axis[0], l->axis[1], l->axis[2]);
	VectorNegate(l->axis[1], l->axis[1]);

	for (s = 0; s < cascades; s++)
	{
		float radius;

		Sh_CascadeCellRect(s, cascades, txsize, &cx, &cy, &csize);
		smsize = csize - 2*inset;
		if (smsize < 16)
			continue;

		//concentric radius: the last cascade is `outer`, each inner one 1/ratio of the next.  So cascade
		//s has radius outer / ratio^(cascades-1-s) -- geometric from a tight near box to the full reach.
		radius = outer / (float)pow(ratio, (double)(cascades-1-s));
		if (radius < 16.0f) radius = 16.0f;

		l->radius = radius;
		//CENTRE ON THE CAMERA, not ahead of it: the box then does not move when the view rotates (only when
		//it translates), which is what kills the "shadows swing when I look around" the frustum fit caused.
		//Whole-texel snap on r_origin -- same crawl-killer as the fitted lamp cells.  The lattice pitch is
		//2*radius/smsize (this CELL's extent), so it MUST use smsize not txsize.
		Sh_OrthoAlignToPoint(l, r_origin, smsize);

		Matrix4x4_CM_Orthographic(r_refdef.m_projection_std, -l->radius, l->radius, l->radius, -l->radius, -l->radius, l->radius);
		memcpy(r_refdef.m_projection_view, r_refdef.m_projection_std, sizeof(r_refdef.m_projection_view));

		sh_fakecell_active = true;
		sh_fakecell_x = cx + inset;
		sh_fakecell_y = cy + inset;

		//cell rect in the SAME bottom-origin convention the direction atlas uses (trap #2 there).
		cell[0] = (cx + inset)                     / (float)txsize;
		cell[1] = (txsize - (cy + inset + smsize)) / (float)txsize;
		cell[2] = cell[3] = smsize / (float)txsize;

		if (!BE_SelectDLight(l, vec3_origin, l->axis, LSHADER_SMAP|LSHADER_ORTHO))
			continue;
		GLBE_CaptureFakeShadowSlot(s, cell);

		//fs_curslot intentionally left at -1: Sh_FakeShadowFilter passes ALL casters into this cascade.
		RQuantAdd(RQUANT_SHADOWSIDES, 1);
		//P114b: force the cached render projection to re-read THIS cascade's ortho.  Without it, cascades
		//1+ render caster depth with cascade 0's projection while sampling with their own -> a single caster
		//casts 3 shadows magnified by radius(s)/radius(0) (the "3 copies at 3 sizes" bug).
		GLBE_FlushProjection();
		Sh_GenShadowFace(l, l->axis, LSHADER_SMAP|LSHADER_ORTHO|LSHADER_FAKESHADOWS, NULL, 4, smsize, txsize, r_refdef.m_projection_std, NULL);

		if (dbg)
			Con_Printf("^5cascade %i: radius %.0f qu (concentric)  %.2f texels/qu\n",
				s, radius, smsize/(2.0f*radius));
	}
	sh_fakecell_active = false;

	//neutralise unused cells so the shader's fixed loop never projects into a stale cascade.
	for (s = cascades; s < MAX_FAKESHADOW_SLOTS; s++)
	{
		Vector4Set(cell, 0, 0, 0, 0);
		GLBE_ClearFakeShadowSlot(s, cell);
	}
	GLBE_SetFakeShadowCount(cascades);

	memcpy(r_refdef.m_view,            oview,  sizeof(r_refdef.m_view));
	memcpy(r_refdef.m_projection_std,  oprojs, sizeof(r_refdef.m_projection_std));
	memcpy(r_refdef.m_projection_view, oprojv, sizeof(r_refdef.m_projection_view));
	r_refdef.pxrect       = oprect;
	r_refdef.flipcull     = oldflip;
	r_refdef.colourmask   = oldcolourmask;
	r_refdef.externalview = oldexternalview;
	R_SetFrustum(r_refdef.m_projection_std, r_refdef.m_view);

	GLBE_EndShadowMap(restorefbo);
	GL_ViewportUpdate();

	//leave a sane sun slot selected for the forward pass (matches the direction atlas epilogue).
	l->radius = r_shadows_distance.value;
	VectorCopy(sundir, l->axis[0]);
	VectorNormalize(l->axis[0]);
	VectorVectors(l->axis[0], l->axis[1], l->axis[2]);
	VectorNegate(l->axis[1], l->axis[1]);
	Sh_OrthoAlignToFrustum(l, txsize);
	BE_SelectDLight(l, vec3_origin, l->axis, LSHADER_SMAP|LSHADER_ORTHO);
}
#endif

//nettest Phase-1: map-light table (lamp POSITIONS for the perspective prop shadows) ================
//A perspective spot needs a lamp ORIGIN, but R_EntityDominantLightDir only gives a DIRECTION and the
//engine's rtlight array is empty unless realtime world lighting is on.  So parse the BSP entity lump the
//same way R_ImportRTLights (gl_rlight.c) does -- classname light / light_* (not light_environment = the
//sun) -- keeping only {origin, brightness}.  Rebuilt whenever the worldmodel changes.
#define SH_MAXMAPLIGHTS 1024
typedef struct
{
	vec3_t org;
	float bright;
	//Patch 120a: the lamp's FIXED shadow cone, resolved once at load from the map entity.  Previously the
	//cone was re-fitted every frame to whichever props were currently inside it -- so its aim was the
	//running MEAN of their directions and its fov was the widest member's angle, both recomputed from
	//scratch each frame.  That is why the atlas tile visibly panned and zoomed with the player, and why
	//two props entering or leaving a cell shifted every shadow in it.  A fixed cone is stable by
	//construction: nothing a prop does can move it.
	//  aim  = ericw "mangle" (yaw pitch roll) or the direction to "target"; STRAIGHT DOWN when the light
	//         declares neither, which is the overwhelmingly common case for a ceiling lamp.
	//  fov  = ericw "angle" (spot cone DIAMETER in degrees) or "_cone"*2 (Q2 radius); a wide default
	//         otherwise, since an unaimed omni lamp still has to cover the floor beneath it.
	vec3_t aim;
	float  fov;
	qboolean shadowflag;	//map explicitly opted this light in with "_shadow" / "_shadowcast"
	//WORLD-OCCLUSION mask: the world-surface depth mesh as seen from this lamp (SHM_BuildShadowMesh,
	//SMT_SHADOWMAP).  Lamps are static, so this builds ONCE (lazily, the first frame the lamp wins a
	//cell) and then every frame is a single cached-VBO depth draw -- same lifecycle as the rtlight
	//prebuild in Sh_PreGenerateLights.  Freed on map change (Sh_LoadMapLights).
	struct shadowmesh_s *mesh;
} sh_maplight_t;
static sh_maplight_t sh_maplights[SH_MAXMAPLIGHTS];
static int sh_nummaplights;
static model_t *sh_maplights_model;
static qboolean sh_anyshadowflag;	//this map flagged at least one light -> those lights are a WHITELIST
//HYSTERESIS: the lamps that held atlas cells LAST frame.  Without stickiness the nearest-N cell ranking
//churns as the camera walks and lamps at the budget boundary flick their shadows on/off.
static int sh_heldlamp[MAX_FAKESHADOW_SLOTS];
static int sh_heldlamp_n;

static qboolean Sh_LampWasHeld(int lamp)
{
	int i;
	for (i = 0; i < sh_heldlamp_n; i++)
		if (sh_heldlamp[i] == lamp)
			return true;
	return false;
}

//"mangle" is ericw's "yaw pitch roll" and yields the direction the light TRAVELS:
//  (cos p cos y, cos p sin y, sin p)   -- note +sin(pitch), NOT Quake's makevectors convention.
//Same decode sv_env_sun.qc uses for the sun; getting it wrong tilts every spot cone.
static void Sh_MangleToVec(const vec3_t mangle, vec3_t out)
{
	float y = mangle[0] * (M_PI/180.0f);
	float pt = mangle[1] * (M_PI/180.0f);
	float cp = cos(pt);
	out[0] = cos(y) * cp;
	out[1] = sin(y) * cp;
	out[2] = sin(pt);
}

//Resolve "target" -> the origin of the entity with that "targetname".  Spotlights are aimed this way at
//least as often as with mangle, so ignoring it would silently point half the mapper's lights at the floor.
#define SH_MAXTARGETS 512
typedef struct { char name[64]; vec3_t org; } sh_target_t;
static qboolean Sh_FindTarget(const sh_target_t *tbl, int n, const char *name, vec3_t out)
{
	int i;
	for (i = 0; i < n; i++)
		if (!strcmp(tbl[i].name, name))
			{ VectorCopy(tbl[i].org, out); return true; }
	return false;
}

static void Sh_LoadMapLights(void)
{
	const char *lump, *p;
	char key[256], value[1024];
	int nest;
	vec3_t org, mangle, aimat;
	float bright, cone;
	qboolean islight, hasorg, hasmangle, hasaim, shadowflag;
	char targetname[64];
	static sh_target_t targets[SH_MAXTARGETS];
	int numtargets = 0;
	qboolean anyflagged = false;

	{	//free the per-lamp world-occlusion meshes (they reference the OLD map's surfaces/leafs)
		int i;
		for (i = 0; i < sh_nummaplights; i++)
			if (sh_maplights[i].mesh)
				{ SH_FreeShadowMesh(sh_maplights[i].mesh); sh_maplights[i].mesh = NULL; }
	}
	sh_nummaplights = 0;
	sh_heldlamp_n = 0;	//lamp indices are per-map; forget the held set
	sh_maplights_model = cl.worldmodel;
	sh_anyshadowflag = false;
	if (!cl.worldmodel)
		return;
	lump = Mod_GetEntitiesString(cl.worldmodel);
	if (!lump)
		return;

	//PASS 1: every targetname -> origin, so a spotlight's "target" can be resolved below.
	for (p = lump; ;)
	{
		qboolean gotname = false, gotorg = false;
		p = COM_Parse(p);
		if (com_token[0] != '{')
			break;
		targetname[0] = 0;
		VectorClear(org);
		nest = 1;
		while (p)
		{
			p = COM_ParseOut(p, key, sizeof(key));
			if (!p) break;
			if (key[0] == '{') { nest++; continue; }
			if (key[0] == '}') { if (!--nest) break; continue; }
			if (nest != 1) continue;
			p = COM_ParseOut(p, value, sizeof(value));
			if (!p) break;
			if (!strcmp(key, "targetname"))
				{ Q_strncpyz(targetname, value, sizeof(targetname)); gotname = true; }
			else if (!strcmp(key, "origin"))
			{
				org[0] = org[1] = org[2] = 0;
				sscanf(value, "%f %f %f", &org[0], &org[1], &org[2]);
				gotorg = true;
			}
		}
		if (gotname && gotorg && numtargets < SH_MAXTARGETS)
		{
			Q_strncpyz(targets[numtargets].name, targetname, sizeof(targets[numtargets].name));
			VectorCopy(org, targets[numtargets].org);
			numtargets++;
		}
	}

	//PASS 2: the lights themselves.
	for (p = lump; ;)
	{
		p = COM_Parse(p);
		if (com_token[0] != '{')
			break;
		islight = hasorg = hasmangle = hasaim = shadowflag = false;
		bright = 0;
		cone = 0;
		VectorClear(org);
		VectorClear(mangle);
		VectorClear(aimat);
		nest = 1;
		while (p)
		{
			p = COM_ParseOut(p, key, sizeof(key));
			if (!p)
				break;
			if (key[0] == '{') { nest++; continue; }
			if (key[0] == '}') { if (!--nest) break; continue; }
			if (nest != 1)
				continue;
			if (key[0] == '_')
				memmove(key, key+1, strlen(key));	//_light -> light, _color -> color, ...
			p = COM_ParseOut(p, value, sizeof(value));
			if (!p)
				break;
			if (!strcmp(key, "classname"))
			{
				if (!strcmp(value, "light"))
					islight = true;
				else if (!strncmp(value, "light_", 6) && strcmp(value, "light_environment"))
					islight = true;	//light_torch/globe/... presets (NOT the sun)
			}
			else if (!strcmp(key, "origin"))
			{
				org[0] = org[1] = org[2] = 0;
				sscanf(value, "%f %f %f", &org[0], &org[1], &org[2]);
				hasorg = true;
			}
			else if (!strcmp(key, "light"))
			{
				float v[4] = {0,0,0,0};
				int n = sscanf(value, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
				bright = (n >= 4) ? v[3] : v[0];	//HL/Source `r g b i` -> i; Quake `n` -> n
			}
			//--- Patch 120a: the shadow cone, straight off the ericw-tools spotlight keys ---
			else if (!strcmp(key, "mangle"))
			{
				mangle[0] = mangle[1] = mangle[2] = 0;
				sscanf(value, "%f %f %f", &mangle[0], &mangle[1], &mangle[2]);
				hasmangle = true;
			}
			else if (!strcmp(key, "target"))
			{
				if (Sh_FindTarget(targets, numtargets, value, aimat))
					hasaim = true;
			}
			else if (!strcmp(key, "angle"))
				cone = atof(value);			//q1 style: cone DIAMETER in degrees
			else if (!strcmp(key, "cone"))
				cone = atof(value) * 2.0f;	//q2 style ("_cone"): cone RADIUS -> diameter
			else if (!strcmp(key, "shadow") || !strcmp(key, "shadowcast"))
				shadowflag = !!atoi(value);
		}
		if (islight && hasorg && sh_nummaplights < SH_MAXMAPLIGHTS)
		{
			sh_maplight_t *ml = &sh_maplights[sh_nummaplights];
			if (bright <= 0)
				bright = 200;	//Quake default `light` value
			VectorCopy(org, ml->org);
			ml->bright = bright;
			ml->shadowflag = shadowflag;
			if (shadowflag)
				anyflagged = true;

			//AIM: "target" beats "mangle" (that is ericw's precedence too); neither = STRAIGHT DOWN.
			if (hasaim)
			{
				VectorSubtract(aimat, org, ml->aim);
				if (!VectorNormalize(ml->aim))
					VectorSet(ml->aim, 0, 0, -1);
			}
			else if (hasmangle)
			{
				Sh_MangleToVec(mangle, ml->aim);
				if (!VectorNormalize(ml->aim))
					VectorSet(ml->aim, 0, 0, -1);
			}
			else
				VectorSet(ml->aim, 0, 0, -1);	//an unaimed lamp lights the floor under it

			//FOV: honour "angle"/"_cone" whenever the mapper set one, even on a lamp that ericw would
			//not treat as a spotlight -- it is the natural knob for "how wide should this thing cast".
			//Otherwise r_shadows_propshadows_cone, which has to be wide enough to cover the floor below.
			ml->fov = (cone > 0) ? cone : r_shadows_propshadows_cone.value;
			ml->fov = bound(20.0f, ml->fov, 150.0f);
			sh_nummaplights++;
		}
	}

	//OPT-IN, but only if the mapper actually opted in: a map with at least one "_shadow" light casts from
	//THOSE ONLY; a map with none casts from all of them, exactly as before.  So no existing map needs
	//editing, and flagging one light immediately becomes a whitelist for that map.
	sh_anyshadowflag = anyflagged;

	if (r_shadows_propshadows_debug.ival)
	{
		int spots = 0, flagged = 0, i;
		for (i = 0; i < sh_nummaplights; i++)
		{
			if (sh_maplights[i].aim[2] > -0.999f) spots++;
			if (sh_maplights[i].shadowflag) flagged++;
		}
		Con_Printf("Sh_LoadMapLights: %i point lamps (%i aimed, %i flagged _shadow%s)\n",
			sh_nummaplights, spots, flagged, anyflagged ? " -- WHITELIST ACTIVE" : "");
	}
}

//The map lamp that best "owns" a prop at `org` (its shadow-probe point, origin+24z).  Ranked by physical
//falloff bright^2/dist^2 so the nearest bright lamp wins.  `minalign` optionally gates lamps against the
//prop's baked dominant-light direction (`lightdir`, toward-light) -- only meaningful on maps with NO
//SUNVIS bake; on baked maps the deluxemap direction stays sun-biased even in shade, so the gate rejected
//every lamp on the DOWNSUN side of a prop => shadows appeared in one direction and not the other.
//`lostrace` instead requires WORLD line-of-sight from the probe point: the top-scoring lamps are traced
//in order and the first VISIBLE one wins -- direction no longer matters, and a lamp behind a wall can't
//own a prop.  A trace that stops within 32qu of the lamp still counts as seen (lights sit embedded in
//fixture brushes; they must not occlude themselves).  Returns lamp index or -1.
//Patch 120: 8, was 3.  With only 3 traced, a prop with three occluded lamps in front of it returned -1
//and lost its shadow ENTIRELY even though a fourth visible lamp was in range.
#define SH_LAMPCANDS 8

//Is `lamp` visible from the probe point?  A trace that stops within 32qu of the lamp still counts as
//seen -- lights sit embedded in their fixture brushes and must not occlude themselves.
static qboolean Sh_PropLampVisible(int lamp, const vec3_t org)
{
	trace_t tr;
	vec3_t left;
	if (!cl.worldmodel || !cl.worldmodel->funcs.NativeTrace)
		return true;
	cl.worldmodel->funcs.NativeTrace(cl.worldmodel, 0, NULLFRAMESTATE, NULL, org, sh_maplights[lamp].org, vec3_origin, vec3_origin, false, MASK_WORLDSOLID, &tr);
	if (tr.fraction >= 1)
		return true;
	VectorSubtract(sh_maplights[lamp].org, tr.endpos, left);
	return DotProduct(left, left) < 32*32;
}

//Patch 120b: is there a ROOF between this caster and the sun?  Returns 1 = in shade, 0 = sunlit,
//-1 = cannot tell (no world model, or the probe point is buried in solid).
//
//`towardsun` must point TOWARD the sun.  The trace mask is MASK_WORLDSOLID *plus* FTECONTENTS_SKY, and
//the two outcomes are then distinguished by what it stopped in:
//  - nothing hit                  -> open air (or out through the void): SUNLIT.
//  - stopped in a SKY leaf        -> the ray reached the skybox: SUNLIT.  This case is the whole reason
//                                    sky has to be IN the mask: a q1 sky brush is a thin shell with the
//                                    solid void behind it, so a sky-blind trace sails through the shell
//                                    and stops on that void, reporting "roof" for everything outdoors.
//  - stopped in anything else     -> real geometry overhead: IN SHADE.
//The q2/q3 TI_SKY surface flag is accepted too, for map formats where sky brushes are solid.
static int Sh_PropSunOccluded(const vec3_t org, const vec3_t towardsun)
{
	trace_t tr;
	vec3_t end;
	unsigned int mask = MASK_WORLDSOLID;
	if (!cl.worldmodel || !cl.worldmodel->funcs.NativeTrace)
		return -1;
	//FTECONTENTS_SKY is 0x80000000, which Q3CONTENTS_NODROP aliases onto (bspfile.h).  Only q1/hl BSPs
	//have non-solid sky leafs and need the bit; adding it on a q3 map would stop the trace on any nodrop
	//volume (they ring pits and lava) and then read that as "sky" = sunlit.  q2/q3 sky brushes ARE solid
	//and carry TI_SKY on the surface, which the surface test below picks up instead.
	if (cl.worldmodel->fromgame == fg_quake || cl.worldmodel->fromgame == fg_halflife)
		mask |= FTECONTENTS_SKY;
	VectorMA(org, 16384.0f, towardsun, end);	//well past any map's bounds; a roof is a roof regardless of r_shadows_distance
	cl.worldmodel->funcs.NativeTrace(cl.worldmodel, 0, NULLFRAMESTATE, NULL, org, end, vec3_origin, vec3_origin, false, mask, &tr);
	if (tr.startsolid || tr.allsolid)
		return -1;	//probe buried in world geometry: no honest answer, leave the caster as it was
	if (tr.fraction >= 1)
		return 0;
	if (tr.contents & FTECONTENTS_SKY)
		return 0;
	if (tr.surface && (tr.surface->flags & TI_SKY))
		return 0;
	return 1;
}

//Falloff score of ONE lamp at a probe point, 0 when the lamp cannot shadow that point at all.  Same
//metric as the ranking below, so the two are directly comparable (that is what the stickiness needs).
//Patch 120a adds two rejections that the ranking MUST share, or a prop gets assigned to a lamp whose
//cell can never contain it and ends up with no shadow:
//  - the "_shadow" whitelist, when the map uses one;
//  - the lamp's FIXED cone.  Cones no longer stretch to fit their members, so a prop outside the cone
//    is simply not shadowed by that lamp -- the next-best lamp should get it instead.
static float Sh_PropLampScore(int lamp, const vec3_t org)
{
	vec3_t d;
	float dist2, reach, dist, proj, coshalf;
	const sh_maplight_t *ml;
	if (lamp < 0 || lamp >= sh_nummaplights)
		return 0;
	ml = &sh_maplights[lamp];
	if (sh_anyshadowflag && !ml->shadowflag)
		return 0;			//map declares its shadow casters and this is not one
	VectorSubtract(ml->org, org, d);
	dist2 = DotProduct(d, d);
	if (dist2 < 1.0f)
		return 0;
	reach = ml->bright * r_shadows_propshadows_range.value;
	if (dist2 > reach*reach)
		return 0;
	//inside the fixed cone?  d is prop->lamp, so test against the REVERSED aim.
	dist = sqrt(dist2);
	proj = -DotProduct(d, ml->aim) / dist;
	coshalf = cos(ml->fov * 0.5f * (M_PI/180.0f));
	if (proj < coshalf)
		return 0;
	return ml->bright * ml->bright / dist2;
}

static int Sh_PropDominantLamp(const vec3_t org, const vec3_t lightdir, float minalign, qboolean lostrace)
{
	int i, j, k;
	int   top[SH_LAMPCANDS];		//best N by score, so a blocked lamp falls back to the next
	float topscore[SH_LAMPCANDS];
	for (j = 0; j < SH_LAMPCANDS; j++)
		{ top[j] = -1; topscore[j] = 0; }
	for (i = 0; i < sh_nummaplights; i++)
	{
		vec3_t d;
		float dist2, score;
		//Patch 120a: ONE definition of "can this lamp shadow this point" -- whitelist, reach and the
		//lamp's fixed cone all live in Sh_PropLampScore, so the ranking here and the stickiness
		//comparison in Sh_PropStickyLamp can never disagree about which lamps are eligible.
		score = Sh_PropLampScore(i, org);
		if (score <= 0)
			continue;
		VectorSubtract(sh_maplights[i].org, org, d);	//prop -> lamp
		dist2 = DotProduct(d, d);
		if (minalign > -1 && DotProduct(d, lightdir)/sqrt(dist2) < minalign)
			continue;	//legacy deluxemap gate (un-baked maps only; Patch 120 passes -2 = disabled)
		for (j = 0; j < SH_LAMPCANDS; j++)
			if (score > topscore[j])
			{
				for (k = SH_LAMPCANDS-1; k > j; k--)
					{ top[k] = top[k-1]; topscore[k] = topscore[k-1]; }
				top[j] = i;  topscore[j] = score;
				break;
			}
	}
	if (!lostrace || !cl.worldmodel || !cl.worldmodel->funcs.NativeTrace)
		return top[0];
	for (j = 0; j < SH_LAMPCANDS && top[j] >= 0; j++)
	{	//the nearest bright lamp the prop can actually SEE
		if (Sh_PropLampVisible(top[j], org))
			return top[j];
	}
	//Patch 120: every candidate occluded -> fall back to the best-scoring one rather than returning -1.
	//A shadow from a slightly wrong lamp is far less jarring than one that blinks out for a frame as you
	//walk past a railing or a doorframe.
	return top[0];
}

//nettest Phase-1: sun in cell 0 (single ortho) + up to N per-prop PERSPECTIVE shadows in cells 1..N of
//Patch 120: PER-ENTITY LAMP STICKINESS.
//Sh_PropDominantLamp is stateless and has no dead-band, so two comparable lamps flip the instant a prop
//crosses their bright^2/dist^2 iso-surface -- the shadow snapped to the other side of the prop on a
//single frame as you walked.  The LOS trace made it worse: one frame of occlusion behind a railing
//handed the prop to a different lamp entirely.  This holds the incumbent lamp across frames and only
//switches when a challenger is decisively better, or when the incumbent has been unusable for several
//frames running.  Bucketed by keynum and VALIDATED on it (every visedict has one: pr_csqc.c sets
//keynum = entnum for csqc ents, and players get their slot index), mirroring the Patch-105 model-light
//cache.  Entries expire so the table self-cleans across map changes and entity churn.
#define SH_STICKYLAMP_BUCKETS 4096	//power of two
#define SH_STICKYLAMP_EXPIRE  60	//frames an untouched entry survives
#define SH_STICKYLAMP_MISSES  3		//frames of no line-of-sight tolerated before dropping the incumbent
#define SH_STICKYLAMP_SHADE   6		//consecutive frames the in-shade verdict must hold before it flips
typedef struct
{
	int          keynum;	//owner; 0 = free.  Validated, so bucket collisions just lose stickiness.
	int          lamp;
	unsigned int frame;
	int          misses;
	//Patch 120a: the in-shade verdict, DEBOUNCED.  The raw test is a hard threshold against a single
	//probe luxel, so a prop straddling it (or walking a lightmap gradient, or stepping over a luxel the
	//sample misses) answered differently frame to frame -- and since the verdict decides whether the
	//prop is in the sun cascades at all, that swapped its entire shadow. `pending` must agree with
	//itself for SH_STICKYLAMP_SHADE frames running before `inshade` is allowed to follow it.
	signed char  inshade;	//-1 = not yet classified
	signed char  pending;
	signed char  pendcount;
	//Patch 120c: the same verdict as a CONTINUOUS 0..1 (0 sunlit, 1 fully in shade), time-smoothed.
	//`inshade` above still decides the discrete question "is this caster in the sun cascades"; this one
	//drives the model's SUN form-shade and self-shadow, which have to cross-fade or the model visibly
	//snaps between two shading regimes as you walk through a doorway.  Every hysteresis in this file
	//DELAYS a flip; none of them makes one gradual, and that is what the transition needed.
	float        shade;
	qboolean     shadeinit;	//false = never classified; the accessor then reports 0 (= unchanged)
} sh_stickylamp_t;
static sh_stickylamp_t sh_stickylamp[SH_STICKYLAMP_BUCKETS];
static unsigned int    sh_stickyframe;
static int             sh_dbg_held, sh_dbg_swap;	//per-frame counters for the debug line

static int Sh_PropStickyLamp(const entity_t *ent, const vec3_t org, const vec3_t dir, qboolean sticky)
{
	sh_stickylamp_t *e;
	int best;
	float bestscore, heldscore, ratio;

	best = Sh_PropDominantLamp(org, dir, -2.0f, sticky);
	if (!sticky || !ent || !ent->keynum)
		return best;	//FORCE mode (debug 2), or an entity with no stable key: no stickiness

	e = &sh_stickylamp[(unsigned int)ent->keynum & (SH_STICKYLAMP_BUCKETS-1)];
	if (e->keynum != ent->keynum || sh_stickyframe - e->frame > SH_STICKYLAMP_EXPIRE)
	{	//new owner, or this entity has been out of sight long enough that its old lamp is meaningless
		e->keynum = ent->keynum;
		e->lamp   = best;
		e->misses = 0;
		e->frame  = sh_stickyframe;
		return best;
	}
	e->frame = sh_stickyframe;

	if (e->lamp == best)
		{ e->misses = 0; return best; }

	//Is the incumbent still usable?  Out of reach is immediate; merely occluded is tolerated briefly so
	//a doorframe or a passing player cannot strobe the shadow.
	heldscore = Sh_PropLampScore(e->lamp, org);
	if (heldscore > 0 && !Sh_PropLampVisible(e->lamp, org))
	{
		if (++e->misses > SH_STICKYLAMP_MISSES)
			heldscore = 0;
	}
	else
		e->misses = 0;

	if (heldscore <= 0)
		{ e->lamp = best; sh_dbg_swap++; return best; }	//incumbent gone -> take the new one

	//Both usable: the challenger must be decisively brighter-per-distance to take over.
	bestscore = Sh_PropLampScore(best, org);
	ratio = r_shadows_propshadows_switch.value;
	if (ratio < 1.0f)
		ratio = 1.0f;
	if (bestscore > heldscore * ratio)
		{ e->lamp = best; sh_dbg_swap++; return best; }

	sh_dbg_held++;
	return e->lamp;
}

//Debounced in-shade verdict for one caster.  `raw` is this frame's answer from the bake (1 in shade,
//0 sunlit, -1 = the map has no data to answer with).  Returns the STABLE verdict; -1 propagates.
//Without this the classification is a per-frame coin flip at every sun/shade boundary, and because an
//in-shade prop is excluded from the sun cascades entirely, each flip swapped its whole shadow.
static int Sh_PropShadeState(const entity_t *ent, int raw)
{
	sh_stickylamp_t *e;
	if (!ent || !ent->keynum)
		return raw;
	e = &sh_stickylamp[(unsigned int)ent->keynum & (SH_STICKYLAMP_BUCKETS-1)];
	if (e->keynum != ent->keynum)
		{ e->keynum = ent->keynum; e->lamp = -1; e->misses = 0; e->inshade = -1; e->shadeinit = false; }
	e->frame = sh_stickyframe;
	//Patch 120b: a DROPOUT (raw < 0) holds the last stable verdict instead of returning -1.  It used to
	//bypass the debounce entirely, so a single frame the classifier could not answer -- a SUNVIS sample
	//miss, or Sh_PropSunOccluded's startsolid guard when a prop clips into geometry -- popped a settled
	//in-shade caster straight back into the sun cascades with a full sun shadow, one frame on, one off.
	//The 6-frame hysteresis below only ever protected 0<->1; this covers the third answer too.  A caster
	//that has NEVER been classified still returns -1 (e->inshade starts at -1), so a map with no bake and
	//no sun trace behaves exactly as before.
	if (raw < 0)
		return e->inshade;
	if (e->inshade < 0)
		{ e->inshade = (signed char)raw; e->pending = (signed char)raw; e->pendcount = 0; return raw; }
	if (raw == e->inshade)
		{ e->pendcount = 0; return e->inshade; }
	if (raw != e->pending)
		{ e->pending = (signed char)raw; e->pendcount = 1; return e->inshade; }
	if (++e->pendcount >= SH_STICKYLAMP_SHADE)
		{ e->inshade = (signed char)raw; e->pendcount = 0; }
	return e->inshade;
}

//Patch 120c: the same in-shade question as a CONTINUOUS 0..1, smoothed in TIME rather than debounced
//in frames.  `target` is this frame's answer (0 sunlit .. 1 in shade, <0 = could not tell).
//
//Exponential approach with a real time constant, so the fade is framerate-independent: over dt the
//value covers 1 - exp(-dt/tau) of the remaining gap.  r_shadows_sunfade IS tau (seconds); 0 snaps.
//
//The dropout rule from Patch 120b applies here too -- a frame that cannot answer holds the current
//value rather than dragging it toward either end.
static float Sh_PropShadeFraction(const entity_t *ent, float target, float dt)
{
	sh_stickylamp_t *e;
	float tau, k;
	if (!ent || !ent->keynum)
		return (target < 0) ? 0.0f : target;
	e = &sh_stickylamp[(unsigned int)ent->keynum & (SH_STICKYLAMP_BUCKETS-1)];
	if (e->keynum != ent->keynum)
		{ e->keynum = ent->keynum; e->lamp = -1; e->misses = 0; e->inshade = -1; e->shadeinit = false; }
	e->frame = sh_stickyframe;
	if (target < 0)
		return e->shadeinit ? e->shade : 0.0f;	//no answer this frame: hold
	if (!e->shadeinit)
	{	//first ever classification: adopt it outright.  Ramping up from 0 would make every prop fade
		//INTO shade the moment it comes into view, which reads as a light turning off.
		e->shade = target;
		e->shadeinit = true;
		return e->shade;
	}
	tau = r_shadows_sunfade.value;
	if (dt <= 0)
		k = 0.0f;			//paused / same frame twice: hold, don't snap
	else if (tau <= 0)
		k = 1.0f;			//fade disabled: the old instant behaviour
	else
	{
		k = 1.0f - (float)exp(-dt / tau);
		if (k > 1.0f) k = 1.0f;
		if (k < 0.0f) k = 0.0f;
	}
	e->shade += (target - e->shade) * k;
	if (e->shade < 0.0f) e->shade = 0.0f;
	if (e->shade > 1.0f) e->shade = 1.0f;
	return e->shade;
}

//Patch 120c: how far into shade this entity is, for the FORWARD pass (gl_backend.c, SP_E_SUNSHADE and
//SP_E_SUNDIR).  0 = fully sunlit, which is also what an entity we have never classified reports --
//that is the fail-safe, and it matches the shader's unbound-uniform value, so anything this system
//never saw renders exactly as it did before Patch 120c.
//
//Unlike Sh_EntityLampDir this does NOT bail on !sh_nummaplights: a lamp-less map still has a sun, and
//a caster under a roof there still needs its sun form-shade faded out.
float Sh_EntitySunShade(const entity_t *ent)
{
	const sh_stickylamp_t *e;
	if (!ent || !ent->keynum)
		return 0;
	e = &sh_stickylamp[(unsigned int)ent->keynum & (SH_STICKYLAMP_BUCKETS-1)];
	if (e->keynum != ent->keynum || !e->shadeinit)
		return 0;
	if (sh_stickyframe - e->frame > SH_STICKYLAMP_EXPIRE)
		return 0;	//stale: the atlas pass has not seen this entity for a while (it may not even run)
	return e->shade;
}

//The lamp direction (TOWARD the lamp) a caster is currently shadowed by, for the model form-shade.
//Returns false when the caster has no lamp -- the shader then keeps the global sun direction.
qboolean Sh_EntityLampDir(const entity_t *ent, vec3_t out)
{
	const sh_stickylamp_t *e;
	if (!ent || !ent->keynum || !sh_nummaplights)
		return false;
	e = &sh_stickylamp[(unsigned int)ent->keynum & (SH_STICKYLAMP_BUCKETS-1)];
	if (e->keynum != ent->keynum || e->lamp < 0 || e->lamp >= sh_nummaplights)
		return false;
	if (sh_stickyframe - e->frame > SH_STICKYLAMP_EXPIRE)
		return false;
	VectorSubtract(sh_maplights[e->lamp].org, ent->origin, out);
	return VectorNormalize(out) != 0;
}

//ONE texture.  Modelled on Sh_GenerateFakeShadowsAtlas; the perspective cells are the whole point.
//nettest Phase-2: atlas layout for sun CASCADES + perspective PROP cells sharing ONE texture.
//  nc==1 : the single-sun layout (cell 0 = the 3/4 sun cell, prop cells in the 7 small L-cells) -- the
//          bit-identical Phase-1 layout, so single-sun + props is unchanged.
//  nc>1  : each of the nc sun cascades takes a full QUADRANT (txsize/2, same as Sh_CascadeCellRect), and
//          the prop cells pack into the (4-nc) FREE quadrants as quarter-size (txsize/4) subcells, 4 per
//          quadrant.  Cascades keep their resolution; props ride the leftover corner.  Fits nc+ceil(np/4)<=4.
//worldmask: each prop cell gets a same-size sibling subcell at +1 holding the WORLD's depth from the
//lamp's view, so lamp shadows can't project through walls.  Halves the subcell budget when set.
static qboolean sh_propcell_paired;

//Patch 120: the subcell tier is chosen from the BUDGET, not the live count.  It used to key off how many
//cells were live this frame, so at cascades 3 + worldmask 1 the tier flipped at 2 lamps: a third lamp
//coming into view instantly QUARTERED the pixel area of every lamp shadow on screen, and walking back out
//of the room quadrupled it again.  Sizing from the budget makes every lamp shadow the same resolution
//always -- the atlas simply reserves space it may not use, which costs nothing (unused cells are cleared
//past the far plane and never sampled).  Set once per frame beside propmax.
static int sh_propsub_budget;

//place the s-th SUBCELL of the free quadrants: quarter-size while the BUDGET fits, else eighth-size
//(4x4 of 16 per quadrant).  Shared by the prop cells and their world siblings.
static void Sh_PropSubCellRect(int s, int nc, int nsub, int txsize, int *ox, int *oy, int *osize)
{
	int h = txsize/2, q = txsize/4;
	if (sh_propsub_budget > nsub)
		nsub = sh_propsub_budget;
	if (nsub <= (4-nc)*4)
	{	//quarter cells, 4 per free quadrant
		int quad = nc + (s >> 2), sub = s & 3;
		*osize = q;
		*ox = ((quad & 1) ? h : 0) + ((sub & 1) ? q : 0);
		*oy = ((quad & 2) ? h : 0) + ((sub & 2) ? q : 0);
	}
	else
	{	//eighth cells, a 4x4 grid of 16 per free quadrant
		int e = txsize/8;
		int quad = nc + (s >> 4), sub = s & 15;
		*osize = e;
		*ox = ((quad & 1) ? h : 0) + (sub & 3)*e;
		*oy = ((quad & 2) ? h : 0) + ((sub >> 2) & 3)*e;
	}
}

static void Sh_PropComboCellRect(int idx, int nc, int np, int txsize, int *ox, int *oy, int *osize)
{
	int h = txsize/2;
	if (nc <= 1 && np <= 7 && !sh_propcell_paired)
	{	//single sun + few UNPAIRED props: reuse the direction-atlas layout (big 3/4 sun cell + L cells)
		Sh_FakeShadowCellRect(idx, 1 + np, txsize, ox, oy, osize);
		return;
	}
	if (idx < nc)
	{	//cascade cell = whole quadrant idx (nc==1 past 7 props: the sun drops to one quadrant too)
		*osize = h;
		*ox = (idx & 1) ? h : 0;
		*oy = (idx & 2) ? h : 0;
	}
	else
	{	//prop cell p: subcell p (unpaired) or 2p (paired -- its WORLD-occlusion sibling sits at 2p+1)
		int p = idx - nc;
		if (sh_propcell_paired)
			Sh_PropSubCellRect(p*2, nc, np*2, txsize, ox, oy, osize);
		else
			Sh_PropSubCellRect(p, nc, np, txsize, ox, oy, osize);
	}
}

//the WORLD-occlusion sibling of prop cell p (subcell 2p+1, same size).  Only meaningful when paired.
static void Sh_PropWorldCellRect(int p, int nc, int np, int txsize, int *ox, int *oy, int *osize)
{
	Sh_PropSubCellRect(p*2 + 1, nc, np*2, txsize, ox, oy, osize);
}

static void Sh_GeneratePropShadowsAtlas(dlight_t *l, int fulltexsize)
{
	int inset = 16;
	int s, restorefbo = 0, propcells, propmax, nc;
	int cx, cy, csize, smsize, txsize = fulltexsize;
	vec3_t sundir, tosun;
	float outer, ratio;
	float oprojs[16], oprojv[16], oview[16];
	pxrect_t oprect;
	unsigned int oldflip, oldcolourmask;
	qboolean oldexternalview;
	uploadfmt_t fmt;
	vec4_t cell;
	int i, npc;
	RSpeedLocals();	//nettest: splits Shadow generation into classify vs entdraw
	//PER-LIGHT cells: each atlas cell is a shadowmap attached to a LIGHT, and EVERY in-shade prop near that
	//light renders into it -- so the cell budget limits active LIGHTS, not props.  A room full of props
	//under one lamp costs ONE cell.
	#define SH_MAXPROPCANDS 256
	//Patch 120a removed the direction CLUSTERING (and with it SH_CELLJOINANG, pc_dir and pc_angrad): a
	//lamp now has exactly one cell with the lamp's own fixed cone, so there is nothing to group by and
	//nothing that re-aims when a prop joins or leaves.
	int   pc_ve[SH_MAXPROPCANDS];		//lamp-lit props (visedict index)...
	int   pc_lamp[SH_MAXPROPCANDS];		//...each one's dominant lamp...
	float pc_camdist[SH_MAXPROPCANDS];	//...its camera distance (for ranking its light's cell)...
	int   pc_cell[SH_MAXPROPCANDS];		//...and its final cell (-1 = none this frame)
	int   pc_shade[SH_MAXPROPCANDS];	//...debounced in-shade verdict: 1 in shade, 0 sunlit, -1 unknown
	//eviction hysteresis: a lamp that held a cell LAST frame counts as SH_HELDBIAS x its real (squared)
	//camera distance -- both as an incumbent and as a challenger.  0.25 in dist^2 space = a challenger
	//must be within HALF the held lamp's distance to displace it, so ranking flutter can't strobe cells.
	#define SH_HELDBIAS 0.25f
	int   lc_lamp[MAX_FAKESHADOW_SLOTS];	//the lamp each chosen LIGHT CELL wraps (its cone comes from the lamp)
	float lc_camdist[MAX_FAKESHADOW_SLOTS];	//nearest assigned prop's camera distance (cell ranking)
	qboolean lc_held[MAX_FAKESHADOW_SLOTS];	//this cell's lamp held a cell last frame (hysteresis)
	//diagnostics (r_shadows_propshadows_debug): where do props fall out of candidacy?
	int   dbg_alias = 0, dbg_nodir = 0, dbg_nolamp = 0, dbg_inshade = 0;
	float dbg_nearcam = 1e30f;
	float dbg_nearsunvis = -1.0f, dbg_nearsunvis_cam = 1e30f;
	static unsigned int dbg_frame = 0;
	//SUNVIS bake present?  Drives BOTH the in-shade classification (exact sun occlusion beats the
	//deluxemap dot, which stays sun-biased even in shade) and the sunmask term further down.
	qboolean hassunvis = (cl.worldmodel && cl.worldmodel->sunvisdata != NULL);

	if (sh_maplights_model != cl.worldmodel)
	{
		Sh_LoadMapLights();
		memset(sh_stickylamp, 0, sizeof(sh_stickylamp));	//lamp INDICES just changed meaning
	}
	sh_stickyframe++;
	sh_dbg_held = sh_dbg_swap = 0;

	//nc = number of SUN cells = cascades (Phase-2: cascades now COEXIST with prop cells).  The prop-cell
	//budget must match the atlas layout AND gl_shader.c's FAKESHADOWS_COUNT / FAKESHADOWS_PERSP_FIRST=nc.
	nc = bound(1, r_shadows_cascades.ival, SH_MAX_CASCADES);
	{
		//geometric capacity: each free quadrant packs up to 16 eighth-size subcells (the layout keeps the
		//roomier quarter/L cells while the live count allows), bounded by the slot arrays.  The
		//FAKESHADOWS_COUNT clamp in gl_shader.c is the matching UPPER bound (worldmask halves the live
		//capacity below it -- the paired world cells consume subcells but no matrix slots).
		int cap;
		sh_propcell_paired = !!r_shadows_propshadows_worldmask.ival;
		cap = (4-nc)*16;
		if (sh_propcell_paired)
			cap /= 2;	//each lamp also gets a same-size WORLD-occlusion subcell
		if (cap > MAX_FAKESHADOW_SLOTS - nc) cap = MAX_FAKESHADOW_SLOTS - nc;
		if (cap < 0) cap = 0;
		propmax = bound(0, r_shadows_propshadows_max.ival, cap);
		//Patch 120: freeze the subcell tier at the worst case so cell resolution never steps (see
		//Sh_PropSubCellRect).  Paired cells consume two subcells each.
		sh_propsub_budget = sh_propcell_paired ? propmax*2 : propmax;
	}
	//the sun throw direction (shared by every sun cell), + cascade geometry (used only when nc>1)
	if (*r_shadows_throwdirection.string)
		VectorCopy(r_shadows_throwdirection.vec4, sundir);
	else
		VectorNegate(r_sun_dir.vec4, sundir);
	outer = max(64.0f, r_shadows_cascade_dist.value);
	ratio = bound(1.5f, r_shadows_cascade_ratio.value, 8.0f);
	//Patch 120b: the same direction REVERSED and normalised, for the in-shade sun trace below.  Derived
	//from the local sundir (not fs_sundir) so it always matches the direction the sun cells actually
	//render with, r_shadows_throwdirection override included.
	VectorNegate(sundir, tosun);
	if (!VectorNormalize(tosun))
		VectorSet(tosun, 0, 0, 1);	//degenerate sun: straight up, so the trace still asks a sane question

	//nettest: CLASSIFY phase begins.  Everything to the GLBE_SetupForShadowMap below is CPU-side
	//caster bookkeeping -- no GL calls, no geometry.  Bracketed separately from the face rendering
	//because the two need different fixes: this half wants an early distance/frustum reject (there
	//is none today; camdist is computed and then only used for ranking ~100 lines later), while the
	//draw half wants the 7 repeated entity-list walks collapsed or cached.
	RSpeedRemark();

	//--- collect EVERY in-shade, lamp-lit prop (they get grouped by lamp below) ---
	npc = 0;
	//Patch 120c: the `&& sh_nummaplights` that used to sit here moved DOWN to just before the lamp
	//range gate.  The loop body now also produces the per-caster sun-shade fraction, which a map with
	//no lamps at all still needs -- with the guard up here that map classified nothing and every model
	//on it fell back to "fully sunlit" even standing under a roof.
	for (i = r_refdef.firstvisedict; i < cl_numvisedicts; i++)
	{
		entity_t *ent = &cl_visedicts[i];
		vec3_t dir, d, sp;
		int lamp, shade;
		float camdist;
		qboolean hasdir;
		if (ent->flags & (RF_NOSHADOW|RF_ADDITIVE|RF_NODEPTHTEST|RF_TRANSLUCENT))
			continue;
		if ((ent->flags & RF_EXTERNALMODEL) && !r_shadow_playershadows.ival)
			continue;
		if (!ent->model || ent->model->type != mod_alias)
			continue;
		if (ent->model->engineflags & MDLF_FLAME)
			continue;
		dbg_alias++;
		VectorSubtract(ent->origin, r_origin, d);
		camdist = DotProduct(d, d);
		//Patch 120: the deluxemap direction is now DIAGNOSTIC ONLY.  It used to be a hard requirement --
		//no dir, no lamp shadow -- which silently killed the whole feature on every map built without
		//`light -bspxlux` (22 of the mod's 29 maps), even though on a SUNVIS map the direction was already
		//unused (minalign was forced to -2).  The LOS trace in Sh_PropDominantLamp is what actually decides
		//lamp ownership, and it needs no bake at all.
		hasdir = R_EntityDominantLightDir(ent, dir);
		if (!hasdir)
		{
			dbg_nodir++;
			VectorClear(dir);	//unused by the lamp pick (minalign -2 skips the alignment test)
		}
		else if (camdist < dbg_nearcam)
			dbg_nearcam = camdist;
		//The shadow probe point = the caster's GROUND CONTACT (its model's lowest extent + a little), NOT
		//origin+24.  Origin conventions differ: a prop's origin is at its base (so +24 = mid-body), but a
		//PLAYER's origin is the physics CENTRE, so +24 lands at the HEAD.  model->mins[2] normalises that:
		//prop base+8, player shin -- both sample the floor the shadow actually lands on.  (Patch 120: this
		//point no longer CLASSIFIES anything, it is only the origin of the lamp LOS trace, so the 18qu the
		//player's origin drops on duck can no longer change whether a shadow exists.)
		VectorCopy(ent->origin, sp);
		sp[2] += (ent->model ? ent->model->mins[2] : 0) + 8;
		//--- IN-SHADE CLASSIFICATION (Patch 120a: restored, debounced) ---
		//Indoors a prop should be lit and shadowed by the LAMP, not the sun: the sun form-shade and the
		//sun self-shadow on something standing under a roof make no sense.
		//
		//What Patch 120 got wrong was not HAVING this test -- it was that the test was a hard threshold
		//on one probe luxel evaluated fresh every frame, so it chattered.  Sh_PropShadeState debounces
		//it: the answer has to hold for SH_STICKYLAMP_SHADE frames before the prop actually switches.
		//
		//Patch 120c: this block moved ABOVE the lamp range gate below, and the loop no longer requires
		//sh_nummaplights.  It has to run for EVERY caster, not just ones standing near a map lamp,
		//because the continuous fraction it produces now drives the model's sun shading in the forward
		//pass -- and a player out in the open, or on a map with no lamps at all, needs that value just
		//as much.  Under the old order they bailed at the range gate and were never classified.
		{
			int rawshade = -1;			//discrete: 1 in shade, 0 sunlit, -1 no answer
			float shadetarget = -1.0f;	//continuous: same question, 0..1, -1 no answer
			if (r_shadows_propshadows_debug.ival < 2)	//debug 2 = FORCE mode: everything is a lamp caster
			{
				if (hassunvis)
				{
					float sv = R_PointSunVis(cl.worldmodel, sp);
					if (sv >= 0.0f)
					{
						float t = r_shadows_propshadows_sunvis.value;
						if (camdist < dbg_nearsunvis_cam) { dbg_nearsunvis_cam = camdist; dbg_nearsunvis = sv; }
						rawshade = (sv < t);
						//Patch 120c: keep the FLOAT.  R_PointSunVis returns a real 0..1 and this used to
						//throw it away on the very next line.  Remapped piecewise so the cvar threshold
						//stays exactly the half-way point -- sv==t gives 0.5 -- which means the discrete
						//verdict above and the fraction cross over together and cannot disagree.
						if (sv <= t)  shadetarget = (t > 0.0f)  ? 0.5f + 0.5f*(t - sv)/t          : 1.0f;
						else          shadetarget = (t < 1.0f)  ? 0.5f * (1.0f - sv)/(1.0f - t)   : 0.0f;
					}
				}
				else if (hasdir && r_shadows_caster_sunvis.value > 0)
					rawshade = (DotProduct(dir, fs_sundir) < r_shadows_caster_sunvis.value);
				//Patch 120b: neither bake could answer -> ask the WORLD.  This is the case for every map
				//built without `light -bspxlux -sunvis`, which is most of them, and leaving it unanswered
				//is what kept a full sun shadow on a player standing indoors.  The probe is the caster's
				//MID-HEIGHT, not the ground-contact point the lamp traces use: sp sits 8qu above the model's
				//lowest extent, close enough to the floor that a prop resting flush reads startsolid.
				if (rawshade < 0 && r_shadows_propshadows_suntrace.ival)
				{
					vec3_t sunsp;
					VectorCopy(ent->origin, sunsp);
					if (ent->model)
						sunsp[2] += (ent->model->mins[2] + ent->model->maxs[2]) * 0.5f;
					rawshade = Sh_PropSunOccluded(sunsp, tosun);
				}
				//no continuous source (no SUNVIS bake): fall back to the discrete answer as the target.
				//The time-smoothing in Sh_PropShadeFraction is what makes it gradual on those maps.
				if (shadetarget < 0 && rawshade >= 0)
					shadetarget = (float)rawshade;
			}
			//Order matters: Sh_PropShadeFraction is the first toucher of the sticky bucket now, so it
			//owns the new-owner reset (it clears shadeinit, which Sh_PropShadeState alone would not).
			Sh_PropShadeFraction(ent, shadetarget, (float)host_frametime);
			shade = Sh_PropShadeState(ent, rawshade);
			if (shade > 0)
				dbg_inshade++;
		}
		//Cheap range gate (plain lamp scan, no trace): a prop with no lamp in range and in-cone at
		//all can never get a lamp shadow, so don't pay a BSP walk or a trace on it.  Everything from
		//here down is about picking a LAMP, so it is also where the sh_nummaplights guard belongs.
		//minalign is always -2 (= disabled): the deluxemap alignment gate rejected lamps DOWNSUN of a
		//prop ("casts one way but not the other"), and the LOS trace supersedes it.
		if (!sh_nummaplights)
			continue;
		if (Sh_PropDominantLamp(sp, dir, -2.0f, false) < 0)
			{ dbg_nolamp++; continue; }
		//The prop's lamp: the nearest bright in-range lamp it has LINE OF SIGHT to, held STICKY across
		//frames (Sh_PropStickyLamp) so it cannot swap as the player walks across two lamps' iso-surface.
		//FORCE mode (debug 2): nearest bright, no trace, no stickiness.
		lamp = Sh_PropStickyLamp(ent, sp, dir, r_shadows_propshadows_debug.ival < 2);
		if (lamp < 0)
			{ dbg_nolamp++; continue; }
		if (npc < SH_MAXPROPCANDS)
		{	//accepted.  The cone is the lamp's own, so there is nothing per-prop to record beyond which
			//lamp owns it and how far away it is (that ranks its lamp's cell).
			float propr = ent->model->radius > 0 ? ent->model->radius : 16.0f;
			pc_ve[npc] = i;  pc_lamp[npc] = lamp;  pc_camdist[npc] = camdist;
			pc_shade[npc] = shade;
			//cell-ranking bias: props OUTSIDE the view frustum rank 4x as far away, so the cells go to
			//the shadows the player can actually SEE (standing among off-screen props must not evict
			//the lamps they are looking at).
			if (R_CullSphere(ent->origin, propr + 32))
				pc_camdist[npc] *= 16.0f;	//squared-distance space: 16 = 4x the distance
			npc++;
		}
	}

	//--- ONE CELL PER LAMP (Patch 120a).  The cone is the LAMP's, fixed at map load, so there is nothing
	//    to cluster: every prop that picked lamp L shares L's single cell.  This replaces the old
	//    (lamp, direction-cluster) grouping, whose cone aim was the running MEAN of its members' directions
	//    and whose fov was refitted to the widest member -- both recomputed from scratch every frame, so
	//    the projection panned and zoomed with the player and any prop joining or leaving shifted every
	//    other shadow in the cell.  Cells still rank by their nearest prop's (frustum-biased) camera
	//    distance, and the nearest `propmax` win the atlas. ---
	propcells = 0;
	for (i = 0; i < npc; i++)
	{
		int c, cbest = -1;
		for (c = 0; c < propcells; c++)
			if (lc_lamp[c] == pc_lamp[i])
				{ cbest = c; break; }
		if (cbest >= 0)
		{	//joins its lamp's cell -- the cone does not move to accommodate it
			if (pc_camdist[i] < lc_camdist[cbest])
				lc_camdist[cbest] = pc_camdist[i];
		}
		else if (propcells < propmax)
		{
			lc_lamp[propcells] = pc_lamp[i];
			lc_camdist[propcells] = pc_camdist[i];
			lc_held[propcells] = Sh_LampWasHeld(pc_lamp[i]);
			propcells++;
		}
		else
		{	//cell budget full: evict the EFFECTIVELY-farthest cell -- held lamps count as much closer
			//than they are (and a held challenger presses harder), so cells don't strobe as ranking
			//flutters with camera movement.
			int worst = 0, k;
			float weff, ceff;
			for (k = 1; k < propcells; k++)
				if (lc_camdist[k]*(lc_held[k]?SH_HELDBIAS:1.0f) > lc_camdist[worst]*(lc_held[worst]?SH_HELDBIAS:1.0f))
					worst = k;
			weff = lc_camdist[worst] * (lc_held[worst] ? SH_HELDBIAS : 1.0f);
			ceff = pc_camdist[i] * (Sh_LampWasHeld(pc_lamp[i]) ? SH_HELDBIAS : 1.0f);
			if (ceff < weff)
			{
				lc_lamp[worst] = pc_lamp[i];
				lc_camdist[worst] = pc_camdist[i];
				lc_held[worst] = Sh_LampWasHeld(pc_lamp[i]);
			}
		}
	}
	//remember this frame's winners for next frame's hysteresis
	sh_heldlamp_n = 0;
	for (i = 0; i < propcells; i++)
	{
		int k;
		for (k = 0; k < sh_heldlamp_n; k++)
			if (sh_heldlamp[k] == lc_lamp[i])
				break;
		if (k == sh_heldlamp_n && sh_heldlamp_n < MAX_FAKESHADOW_SLOTS)
			sh_heldlamp[sh_heldlamp_n++] = lc_lamp[i];
	}
	//FINAL membership against the surviving cells (eviction above may have orphaned props whose lamp lost
	//its cell; they must not render into whatever replaced it).  A prop belongs to its lamp's cell, full
	//stop -- Sh_PropLampScore already guaranteed it is inside that lamp's fixed cone.
	for (i = 0; i < npc; i++)
	{
		int c, cbest = -1;
		for (c = 0; c < propcells; c++)
			if (lc_lamp[c] == pc_lamp[i])
				{ cbest = c; break; }
		pc_cell[i] = cbest;
	}

	if (r_shadows_propshadows_debug.ival && ((dbg_frame++ & 63) == 0))
		Con_Printf("propshadows: lamps=%i alias=%i nodir=%i nolamp=%i inshade=%i -> props=%i lightcells=%i (held=%i swapped=%i; nearest sunvis=%.2f vs %.2f; suncast=%i shadefade=%.2fs)\n",
			sh_nummaplights, dbg_alias, dbg_nodir, dbg_nolamp, dbg_inshade, npc, propcells,
			sh_dbg_held, sh_dbg_swap, dbg_nearsunvis, r_shadows_propshadows_sunvis.value,
			r_shadows_propshadows_suncast.ival, r_shadows_sunfade.value);

	//--- stamp the caster filter table.  Everyone defaults to the sun cell(s) (bucket 0); an IN-SHADE prop
	//    whose lamp won a cell is stamped with that cell instead, which Sh_FakeShadowFilter reads as "keep
	//    this one out of the sun cascades" -- so indoors a prop's cast shadow, self-shadow and form-shade
	//    all come from its lamp and nothing from the sun.  A prop that is SUNLIT (or on a map with no bake
	//    to tell) stays in bucket 0 and keeps its sun shadow while still rendering into its lamp's cone by
	//    geometry, so a sunlit prop near a lamp is not silently robbed of its sun shadow the way it was
	//    before Patch 120. ---
	if (fs_entbucket_max < cl_maxvisedicts)
	{
		Z_Free(fs_entbucket);
		fs_entbucket_max = cl_maxvisedicts;
		fs_entbucket = Z_Malloc(fs_entbucket_max);
	}
	if (fs_entbucket)
		memset(fs_entbucket, 0, fs_entbucket_max);	//default 0 = the sun cell(s); cascades all filter as slot 0
	//Patch 120c: with r_shadows_propshadows_suncast set (the default) NOTHING is stamped -- every caster
	//stays in the sun cascades and keeps throwing a sun shadow onto the world, indoors included.  That is
	//only safe because the model's own SUN form-shade and self-shadow now fade out via e_sunshade
	//instead; left in the cascades WITHOUT that fade, an indoor model self-shadows itself from a sun it
	//cannot see, since defaultskin.glsl samples the very cells the caster renders into.  The two halves
	//are one change.  0 restores the Patch 120a exclusivity exactly.
	if (!r_shadows_propshadows_suncast.ival)
		for (i = 0; i < npc; i++)
			if (pc_cell[i] >= 0 && pc_shade[i] > 0 && fs_entbucket && (unsigned)pc_ve[i] < (unsigned)fs_entbucket_max)
				fs_entbucket[pc_ve[i]] = nc + pc_cell[i];	//in shade -> its lamp owns it, not the sun
	fs_slotcount = nc + propcells;

	if (r_shadow_shadowmapping_depthbits.ival >= 32 && sh_config.texfmt[PTI_DEPTH32])       fmt = PTI_DEPTH32;
	else if (r_shadow_shadowmapping_depthbits.ival >= 24 && sh_config.texfmt[PTI_DEPTH24])   fmt = PTI_DEPTH24;
	else if (r_shadow_shadowmapping_depthbits.ival >= 24 && sh_config.texfmt[PTI_DEPTH24_8]) fmt = PTI_DEPTH24_8;
	else                                                                                     fmt = PTI_DEPTH16;

	memcpy(oprojs, r_refdef.m_projection_std,  sizeof(oprojs));
	memcpy(oprojv, r_refdef.m_projection_view, sizeof(oprojv));
	memcpy(oview,  r_refdef.m_view,            sizeof(oview));
	oprect          = r_refdef.pxrect;
	oldflip         = r_refdef.flipcull;
	oldcolourmask   = r_refdef.colourmask;
	oldexternalview = r_refdef.externalview;

	if (!GLBE_BeginShadowMap(2, txsize, txsize, fmt, &restorefbo))
	{	//no atlas this frame: neutralise EVERY extra cell (the uniforms upload all slots now, so
		//anything left un-cleared would replay last frame's capture)
		for (s = 1; s < MAX_FAKESHADOW_SLOTS; s++)
		{
			Vector4Set(cell, 0, 0, 0, 0);
			GLBE_ClearFakeShadowSlot(s, cell);
		}
		GLBE_SetFakeShadowCount(1);
		fs_slotcount = 1;
		return;
	}

	RSpeedEnd(RSPEED_SHADOW_CLASSIFY);	//nettest: end of the CPU-side caster classification

	r_refdef.externalview = true;
	Sh_PropComboCellRect(0, nc, propcells, txsize, &cx, &cy, &csize);
	smsize = csize - 2*inset;
	GLBE_SetupForShadowMap(l, txsize, txsize, smsize/(float)txsize);

	//--- sun cells [0, nc): a single ortho fitted to the view (nc==1) OR nc concentric camera-centred
	//    cascades (nc>1, geometry matching Sh_GenerateCascadeAtlas).  ALL filter as slot 0 (fs_curslot=0),
	//    so every non-prop caster lands in every sun cell while the prop-assigned casters (entbucket=nc+p)
	//    are excluded -- an in-shade prop casts ONLY its lamp shadow, never a sun/cascade one. ---
	for (s = 0; s < nc; s++)
	{
		Sh_PropComboCellRect(s, nc, propcells, txsize, &cx, &cy, &csize);
		smsize = csize - 2*inset;
		if (smsize < 16)
			continue;
		VectorCopy(sundir, l->axis[0]);
		if (!VectorNormalize(l->axis[0]))
			VectorNegate(r_sun_dir.vec4, l->axis[0]);
		VectorVectors(l->axis[0], l->axis[1], l->axis[2]);
		VectorNegate(l->axis[1], l->axis[1]);
		l->flags = LFLAG_SHADOWMAP|LFLAG_ORTHO;
		if (nc == 1)
		{
			l->radius = r_shadows_distance.value;
			Sh_OrthoAlignToFrustum(l, smsize);
		}
		else
		{	//concentric: cascade s has radius outer/ratio^(nc-1-s), centred on the CAMERA (rotation-stable)
			float radius = outer / (float)pow((double)ratio, (double)(nc-1-s));
			if (radius < 16.0f) radius = 16.0f;
			l->radius = radius;
			Sh_OrthoAlignToPoint(l, r_origin, smsize);
		}
		Matrix4x4_CM_Orthographic(r_refdef.m_projection_std, -l->radius, l->radius, l->radius, -l->radius, -l->radius, l->radius);
		memcpy(r_refdef.m_projection_view, r_refdef.m_projection_std, sizeof(r_refdef.m_projection_view));
		sh_fakecell_active = true;
		sh_fakecell_x = cx + inset;
		sh_fakecell_y = cy + inset;
		cell[0] = (cx + inset)                     / (float)txsize;
		cell[1] = (txsize - (cy + inset + smsize)) / (float)txsize;
		cell[2] = cell[3] = smsize / (float)txsize;
		if (!BE_SelectDLight(l, vec3_origin, l->axis, LSHADER_SMAP|LSHADER_ORTHO))
			continue;
		GLBE_CaptureFakeShadowSlot(s, cell);
		fs_curslot = 0;	//sun: admit entbucket==0 casters (Phase-0 sun-vis gate still applies)
		RQuantAdd(RQUANT_SHADOWSIDES, 1);
		GLBE_FlushProjection();
		Sh_GenShadowFace(l, l->axis, LSHADER_SMAP|LSHADER_ORTHO|LSHADER_FAKESHADOWS, NULL, 4, smsize, txsize, r_refdef.m_projection_std, NULL);
	}

	//--- cells nc..nc+N: ONE spot shadowmap per LIGHT, containing EVERY prop assigned to that light.  The
	//    cone aims at its props' centroid and widens to contain them all, so a whole room's props cast from
	//    a single lamp cell -- the budget limits LIGHTS, not props. ---
	for (s = 0; s < propcells; s++)
	{
		vec3_t lamp, aim;
		float rad, fov, nearclip;
		int nprops;
		float slotmat[16];
		int cellidx = nc + s;

		Sh_PropComboCellRect(cellidx, nc, propcells, txsize, &cx, &cy, &csize);
		smsize = csize - 2*inset;
		if (smsize < 16)
			continue;

		VectorCopy(sh_maplights[lc_lamp[s]].org, lamp);
		VectorCopy(sh_maplights[lc_lamp[s]].aim, aim);	//THE LAMP'S OWN cone axis -- fixed at map load

		//Patch 120a: the projection is now a property of the LAMP, not of whatever happens to be standing
		//under it this frame.  aim / fov / near / far are all constant, so the captured matrix is
		//bit-identical frame to frame while the camera moves -- which is what makes the shadow stop
		//swimming, and what stopped the atlas tile panning around like a free camera.
		nprops = 0;
		for (i = 0; i < npc; i++)
			if (pc_cell[i] == s)
				nprops++;
		if (!nprops)
		{	//cell lost all its members: neutralise it so LAST frame's capture can't linger in this atlas
			//slot (the receiver still loops over it).
			Vector4Set(cell, 0, 0, 0, 0);
			GLBE_ClearFakeShadowSlot(cellidx, cell);
			continue;
		}
		fov = bound(20.0f, sh_maplights[lc_lamp[s]].fov, 150.0f);
		rad = sh_maplights[lc_lamp[s]].bright * r_shadows_propshadows_range.value;	//zfar = lamp reach
		if (rad < 64.0f)
			rad = 64.0f;

		//by-cone caster admission: hand Sh_FakeShadowFilter this lamp's cone so ANY model inside it casts
		//into this cell (see fs_cone_*).  Widen the half-angle and reach a touch so a caster whose ORIGIN
		//is just outside but whose body pokes in still renders.
		VectorCopy(lamp, fs_cone_org);
		VectorCopy(aim, fs_cone_axis);
		fs_cone_rad2 = (rad + 48.0f) * (rad + 48.0f);
		{
			float halfrad = (0.5f*fov + 8.0f) * (float)(M_PI/180.0);	//half-fov + 8deg pad
			if (halfrad > (float)(M_PI*0.5 - 0.01)) halfrad = (float)(M_PI*0.5 - 0.01);
			fs_cone_coscos = (float)cos(halfrad); fs_cone_coscos *= fs_cone_coscos;
		}
		nearclip = 8.0f;	//fixed, like everything else about this frustum

		VectorCopy(lamp, l->origin);
		VectorCopy(aim, l->axis[0]);
		VectorVectors(l->axis[0], l->axis[1], l->axis[2]);
		l->radius = rad;
		l->fov = fov;
		l->nearclip = nearclip;
		l->flags = LFLAG_SHADOWMAP;

		Matrix4x4_CM_Projection_Far(r_refdef.m_projection_std, fov, fov, nearclip, rad, false);
		memcpy(r_refdef.m_projection_view, r_refdef.m_projection_std, sizeof(r_refdef.m_projection_view));

		sh_fakecell_active = true;
		sh_fakecell_x = cx + inset;
		sh_fakecell_y = cy + inset;
		cell[0] = (cx + inset)                     / (float)txsize;
		cell[1] = (txsize - (cy + inset + smsize)) / (float)txsize;
		cell[2] = cell[3] = smsize / (float)txsize;

		if (!BE_SelectDLight(l, vec3_origin, l->axis, LSHADER_SMAP|LSHADER_SPOT))
		{
			Vector4Set(cell, 0, 0, 0, 0);
			GLBE_ClearFakeShadowSlot(cellidx, cell);
			continue;
		}

		fs_curslot = cellidx;	//Sh_FakeShadowFilter now admits any caster inside fs_cone_* (this lamp's cone)
		RQuantAdd(RQUANT_SHADOWSIDES, 1);
		GLBE_FlushProjection();
		Sh_GenShadowFace(l, l->axis, LSHADER_SMAP|LSHADER_SPOT|LSHADER_FAKESHADOWS, NULL, 4, smsize, txsize, r_refdef.m_projection_std, NULL);

		//ROBUST capture: snapshot the transform ACTUALLY rendered (proj * m_view, the view Sh_GenShadowFace
		//just built for face 4), NOT GLBE_SelectDLight's spot matrix -- that uses a different axis
		//convention (xy transposed vs face 4) and would mismatch the depth (the P93 "mirrored/tiny" trap).
		//Copying the real render transform is correct for ANY projection by construction.
		Matrix4_Multiply(r_refdef.m_projection_std, r_refdef.m_view, slotmat);
		slotmat[14] -= r_shadows_propshadows_bias.value;	//nudge clip-z toward the lamp = self-shadow acne guard
		//normalise the WHOLE matrix by the lamp reach: xyz/w is a ratio so sampling is bit-identical, but
		//the shader's raw pc.w becomes DISTANCE/REACH (0..1) -- a free per-pixel POWER FALLOFF term with no
		//extra uniform.  The shader fades the darkening to zero as pc.w -> 1, so a shadow can never throw
		//further than its lamp's power (r_shadows_propshadows_range * brightness).
		for (i = 0; i < 16; i++)
			slotmat[i] *= 1.0f / rad;
		GLBE_CaptureFakeShadowSlotMatrix(cellidx, slotmat, cell);

		//SUNLIGHT-AWARE washout (info.x): where this lamp's shadow lands on a SUN-VISIBLE pixel, the sun
		//re-lights the area, so the receiver scales the darkening down by the lamp-vs-sun brightness
		//ratio x the pixel's own baked sun visibility.  Bsun comes from env_sun (r_shadows_sunbrightness,
		//same units as point-light `light` values).  MUST be forced 0 on maps with no SUNVIS bake: their
		//sampler fallback reads "fully sunlit" everywhere and would erase every lamp shadow.
		{
			vec4_t info = {0, 0, 0, 0};
			if (hassunvis && r_shadows_propshadows_sunmask.value > 0)
			{
				float bsun = max(0.0f, r_shadows_sunbrightness.value);
				float blamp = max(1.0f, sh_maplights[lc_lamp[s]].bright);
				info[0] = bound(0.0f, (bsun / (bsun + blamp)) * r_shadows_propshadows_sunmask.value, 1.0f);
			}
			//WORLD-OCCLUSION cell (info.y/z = its atlas uv origin, info.w = live flag): render the
			//WORLD's depth through the SAME cone into the paired subcell, so the receiver can reject any
			//pixel with a wall between it and the lamp -- the shadow lands on the FIRST surface only and
			//can never project through a wall into the next room (the modern-Source fix).  Same captured
			//matrix, same cell scale; only the uv origin differs, so it costs no uniform/varying slots.
			if (sh_propcell_paired)
			{
				int wx, wy, wsize, wsm;
				shadowmesh_t *smesh;
				Sh_PropWorldCellRect(s, nc, propcells, txsize, &wx, &wy, &wsize);
				wsm = wsize - 2*inset;
				//adopt this LAMP's cached world mesh: SHM_BuildShadowMesh caches on the dlight and is NOT
				//keyed on origin, so plant the lamp's own mesh (rebuild only when absent -- map lamps are
				//static, so each builds ONCE ever) and steal the pointer back afterwards (Begin can free
				//and replace it on leafbytes mismatch; the steal-back self-heals the table).
				l->worldshadowmesh = sh_maplights[lc_lamp[s]].mesh;
				l->rebuildcache = !l->worldshadowmesh;
				smesh = SHM_BuildShadowMesh(l, NULL, SMT_SHADOWMAP);
				sh_maplights[lc_lamp[s]].mesh = l->worldshadowmesh;
				if (smesh && wsm >= 16)
				{
					sh_fakecell_active = true;
					sh_fakecell_x = wx + inset;
					sh_fakecell_y = wy + inset;
					fs_curslot = FS_WORLDSLOT;	//Sh_FakeShadowFilter rejects EVERY visedict, so only the world mesh draws
					RQuantAdd(RQUANT_SHADOWSIDES, 1);
					GLBE_FlushProjection();
					Sh_GenShadowFace(l, l->axis, LSHADER_SMAP|LSHADER_SPOT|LSHADER_FAKESHADOWS, smesh, 4, wsm, txsize, r_refdef.m_projection_std, NULL);
					info[1] = (wx + inset)                  / (float)txsize;	//world cell uv origin
					info[2] = (txsize - (wy + inset + wsm)) / (float)txsize;	//(bottom-origin, like cell[1])
					info[3] = 1;	//world mask live for this cell
				}
			}
			GLBE_SetFakeShadowSlotInfo(cellidx, info);
		}
	}
	fs_curslot = -1;
	sh_fakecell_active = false;
	//un-plant the last lamp's adopted mesh from the shared r_fakelight: the table owns these meshes;
	//leaving the pointer here would let some other path free or clobber a lamp's cache.
	l->worldshadowmesh = NULL;
	l->rebuildcache = true;

	//neutralise every atlas cell the compile-time FAKESHADOWS_COUNT loop will still read past our live count.
	for (s = nc+propcells; s < MAX_FAKESHADOW_SLOTS; s++)
	{
		Vector4Set(cell, 0, 0, 0, 0);
		GLBE_ClearFakeShadowSlot(s, cell);
	}
	GLBE_SetFakeShadowCount(nc+propcells);

	memcpy(r_refdef.m_view,            oview,  sizeof(r_refdef.m_view));
	memcpy(r_refdef.m_projection_std,  oprojs, sizeof(r_refdef.m_projection_std));
	memcpy(r_refdef.m_projection_view, oprojv, sizeof(r_refdef.m_projection_view));
	r_refdef.pxrect       = oprect;
	r_refdef.flipcull     = oldflip;
	r_refdef.colourmask   = oldcolourmask;
	r_refdef.externalview = oldexternalview;
	R_SetFrustum(r_refdef.m_projection_std, r_refdef.m_view);

	GLBE_EndShadowMap(restorefbo);
	GL_ViewportUpdate();

	//leave the SUN slot selected for the forward pass (as the other atlas paths do).
	if (*r_shadows_throwdirection.string)
		VectorCopy(r_shadows_throwdirection.vec4, l->axis[0]);
	else
		VectorNegate(r_sun_dir.vec4, l->axis[0]);
	VectorNormalize(l->axis[0]);
	VectorVectors(l->axis[0], l->axis[1], l->axis[2]);
	VectorNegate(l->axis[1], l->axis[1]);
	l->radius = r_shadows_distance.value;
	l->flags = LFLAG_SHADOWMAP|LFLAG_ORTHO;
	Sh_PropComboCellRect(0, nc, propcells, txsize, &cx, &cy, &csize);
	Sh_OrthoAlignToFrustum(l, csize - 2*inset);
	BE_SelectDLight(l, vec3_origin, l->axis, LSHADER_SMAP|LSHADER_ORTHO);
}

//nettest: r_shadows_propshadows_showatlas -- draw the fake-shadow depth atlas as a 2d overlay so the
//cell layout (cascade quadrants, lamp subcells, insets, what each cone captured) can be SEEN.  Called
//from SCR_DrawTwoDimensional every 2d frame; no-ops unless the cvar is set.
//The atlas is a DEPTH texture with ARB compare mode enabled, so a plain texture2D read of it is
//undefined -- the glsl (nettest glsl/atlasdebug.glsl) instead takes N shadow-COMPARE taps at swept
//reference depths and the pass fraction reconstructs the stored depth as grayscale.
void Sh_DrawFakeShadowAtlasOverlay(void)
{
	shader_t *sh;
	texid_t tex;
	float size, x, y;
	if (!r_shadows_propshadows_showatlas.ival)
		return;
	if (qrenderer != QR_OPENGL || !gl_config.arb_shadow)
		return;
	tex = GLBE_GetFakeShadowAtlasTexture();
	if (!TEXVALID(tex) || tex->status != TEX_LOADED)
		return;
	sh = R_RegisterShader("fakeshadow_atlasdebug", 0, "{\nprogram atlasdebug\n{\nmap $diffuse\n}\n}");
	if (!sh)
		return;
	sh->defaulttextures->base = tex;
	if (r_shadows_propshadows_showatlas.ival >= 2)
		size = min(vid.width, vid.height) - 16;			//big: as much of the screen as stays square
	else
		size = min(vid.width, vid.height) * 0.35;		//corner thumbnail
	x = vid.width - size - 8;
	y = vid.height - size - 8;
	R2D_ImageColours(1, 1, 1, 1);
	//t flipped: the atlas viewports use GL's bottom-left origin, so t1=1 puts cell row 0 at the top.
	R2D_Image(x, y, size, size, 0, 1, 1, 0, sh);
}

void Sh_GenerateFakeShadows(void)	//generates shadowmaps and selects the dlight, but does not actually render any lighting. the lightmapped-wall etc glsl must filter by itself if it wants to accept shadows.
{
	dlight_t *l = &r_fakelight;
	vec3_t mins, maxs;
	srect_t rect;
	int smsize;
	int texwidth, texheight;

	if (*r_shadows_throwdirection.string)
		VectorCopy(r_shadows_throwdirection.vec4, l->axis[0]);
	else
		VectorNegate(r_sun_dir.vec4, l->axis[0]);
	VectorNormalize(l->axis[0]);
	VectorVectors(l->axis[0], l->axis[1], l->axis[2]);
	VectorNegate(l->axis[1], l->axis[1]);

	smsize = bound(256, r_shadows_res.ival, 8192);	//nettest r_shadows_res (stock hardcoded SHADOWMAP_SIZE*4 = 2048)
	//nettest P114: radius MUST be set before the align.  Sh_OrthoAlignToFrustum divides by
	//scale = 2*radius/smsize, so on the very first frame (r_fakelight is static-zeroed, radius 0)
	//the old order gave scale 0 -> a div-by-zero NaN origin that then defeated the cull test.
	l->radius = r_shadows_distance.value;
	l->flags = LFLAG_SHADOWMAP|LFLAG_ORTHO;
	Sh_OrthoAlignToFrustum(l, smsize);
	l->rebuildcache = true;

	if (R_CullSphere(l->origin, l->radius))
	{
		RQuantAdd(RQUANT_RTLIGHT_CULL_FRUSTUM, 1);
		return;	//this should be the more common case
	}

	mins[0] = l->origin[0] - l->radius;
	mins[1] = l->origin[1] - l->radius;
	mins[2] = l->origin[2] - l->radius;

	maxs[0] = l->origin[0] + l->radius;
	maxs[1] = l->origin[1] + l->radius;
	maxs[2] = l->origin[2] + l->radius;

	if (Sh_ScissorForBox(mins, maxs, &rect))
	{
		RQuantAdd(RQUANT_RTLIGHT_CULL_SCISSOR, 1);
		return;
	}

	//nettest P110: pick this frame's cast directions.  Runs here, AFTER the slot-0 axis/origin/radius
	//setup and the frustum+scissor culls, because the bucketer needs the box centre to range-cull
	//casters -- and because a culled frame should cost nothing at all.
	fs_slotcount = Sh_FakeShadowChooseSlots(l);

	texwidth = smsize;
	texheight = smsize;

	switch(qrenderer)
	{
#ifdef GLQUAKE
	case QR_OPENGL:
		GLBE_SetupForShadowMap(l, texwidth, texheight, (smsize) / (float)texwidth);
		break;
#endif
#ifdef D3D11QUAKE
	case QR_DIRECT3D11:
		D3D11BE_SetupForShadowMap(l, texwidth, texheight, (smsize) / (float)texwidth);
		break;
#endif
#ifdef VKQUAKE
	case QR_VULKAN:
		VKBE_SetupForShadowMap(l, texwidth, texheight, (smsize) / (float)texwidth);
		break;
#endif
	default:
		(void)texwidth;
		(void)texheight;
		break;
	}

	//nettest Phase-0: mark the fake-SUN depth pass so Sh_FakeShadowFilter's caster sun-visibility gate
	//applies HERE (cascade / single / slots-sun-cell) and NOT to the real rtlight shadow maps that reach
	//the same filter later.  fs_sundir points TOWARD the sun (dominant-light dirs are toward-light too, so
	//the dot in the gate compares like with like).  Reset before every exit so it never leaks.
	fs_sunpass = true;
	VectorNegate(l->axis[0], fs_sundir);

	//nettest P110: more than one cast direction this frame -> render the atlas instead of the single map.
	if (fs_slotcount > 1 && qrenderer == QR_OPENGL)
	{
		Sh_GenerateFakeShadowsAtlas(l, fs_slotcount, smsize);
		fs_sunpass = false;
		return;
	}
	//nettest Phase-1: per-prop PERSPECTIVE shadows (only reachable with slots==1, since slots>1 returned
	//above).  Cell 0 = the sun, cells 1..N = one spot shadow per prop from its dominant lamp.  For now this
	//replaces the cascades (Phase 2 merges them); gl_shader.c injects the matching FAKESHADOWS_PERSP_FIRST.
	if (r_shadows_propshadows.ival && qrenderer == QR_OPENGL)
	{
		fs_propshadowpass = true;	//Patch 120: tells Sh_FakeShadowFilter that lamp cells are ADDITIVE,
									//so the sun cells must admit every caster (see the filter's sun branch)
		Sh_GeneratePropShadowsAtlas(l, smsize);
		fs_propshadowpass = false;
		fs_sunpass = false;
		return;
	}
	//nettest P114: single cast direction -> optionally split the SUN into view-depth cascades.  Mutually
	//exclusive with slots (which already claimed the cells above); fs_slotcount is 1 here so the caster
	//filter passes everything into every cascade.
	if (qrenderer == QR_OPENGL)
	{
		int cascades = bound(1, r_shadows_cascades.ival, SH_MAX_CASCADES);
		if (cascades > 1)
		{
			Sh_GenerateCascadeAtlas(l, cascades, smsize);
			fs_sunpass = false;
			return;
		}
	}
	GLBE_SetFakeShadowCount(1);

	if (BE_SelectDLight(l, vec3_origin, l->axis, LSHADER_SMAP|LSHADER_ORTHO))
		Sh_GenShadowMap(l, LSHADER_SMAP|LSHADER_ORTHO|LSHADER_FAKESHADOWS, l->axis, NULL, smsize, texwidth);
	fs_sunpass = false;
}

static void Sh_DrawShadowMapLight(dlight_t *l, vec3_t colour, vec3_t axis[3], qbyte *vvis)
{
	vec3_t mins, maxs;
	qbyte *lvis;
	srect_t rect;
	int smsize;
	int lighttype;
	int texwidth, texheight;

	if (l->fov != 0)
		lighttype = LSHADER_SMAP|LSHADER_SPOT;
#ifdef LFLAG_ORTHO
	else if (l->flags & LFLAG_ORTHO)
		lighttype = LSHADER_SMAP|LSHADER_ORTHO;
#endif
	else
		lighttype = LSHADER_SMAP;

	if (R_CullSphere(l->origin, l->radius))
	{
		RQuantAdd(RQUANT_RTLIGHT_CULL_FRUSTUM, 1);
		return;	//this should be the more common case
	}

	mins[0] = l->origin[0] - l->radius;
	mins[1] = l->origin[1] - l->radius;
	mins[2] = l->origin[2] - l->radius;

	maxs[0] = l->origin[0] + l->radius;
	maxs[1] = l->origin[1] + l->radius;
	maxs[2] = l->origin[2] + l->radius;

	if (Sh_ScissorForBox(mins, maxs, &rect))
	{
		RQuantAdd(RQUANT_RTLIGHT_CULL_SCISSOR, 1);
		return;
	}

	if (vvis)
	{
		if (!l->rebuildcache && l->worldshadowmesh)
		{
			lvis = l->worldshadowmesh->litleaves;
			//fixme: check head node first?
			if (!Sh_LeafInView(l->worldshadowmesh->litleaves, vvis))
			{
				RQuantAdd(RQUANT_RTLIGHT_CULL_PVS, 1);
				return;
			}
		}
		else
		{
			int clus;
			clus = cl.worldmodel->funcs.ClusterForPoint(cl.worldmodel, l->origin, NULL);
			lvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, clus, &lvisb, PVM_FAST);
			//FIXME: surely we can use the phs for this?
			if (cl.worldmodel->funcs.ClustersInSphere)
				lvis = cl.worldmodel->funcs.ClustersInSphere(cl.worldmodel, l->origin, l->radius, &lvisb2, lvis);
			//FIXME: check areas
			if (!Sh_VisOverlaps(lvis, vvis))	//The two viewing areas do not intersect.
			{
				RQuantAdd(RQUANT_RTLIGHT_CULL_PVS, 1);
				return;
			}
		}
	}
	else
		lvis = NULL;

	if (lighttype & LSHADER_SPOT)
	{
		smsize = SHADOWMAP_SIZE;	//spot lights or ortho lights can just use the full thing.
		texwidth = smsize;
		texheight = smsize;
	}
	else if (lighttype & LSHADER_ORTHO)
	{
		smsize = SHADOWMAP_SIZE;	//spot lights or ortho lights can just use the full thing.
		texwidth = smsize;
		texheight = smsize;
	}
	else
	{
		//Stolen from DP. Actually, LH pasted it to me in IRC.
		vec3_t nearestpoint;
		vec3_t d;
		float distance, lodlinear;
		nearestpoint[0] = bound(l->origin[0]-l->radius, r_origin[0], l->origin[0]+l->radius);
		nearestpoint[1] = bound(l->origin[1]-l->radius, r_origin[1], l->origin[1]+l->radius);
		nearestpoint[2] = bound(l->origin[2]-l->radius, r_origin[2], l->origin[2]+l->radius);
		VectorSubtract(nearestpoint, r_origin, d);
		distance = VectorLength(d);
		lodlinear = (l->radius * r_shadow_shadowmapping_precision.value) / sqrt(max(1.0f, distance / l->radius));
		smsize = bound(16, lodlinear, SHADOWMAP_SIZE);
		texwidth = smsize*3;
		texheight = smsize*2;
	}

	switch(qrenderer)
	{
#ifdef GLQUAKE
	case QR_OPENGL:
		GLBE_SetupForShadowMap(l, texwidth, texheight, (smsize-4) / (float)SHADOWMAP_SIZE);
		break;
#endif
#ifdef D3D11QUAKE
	case QR_DIRECT3D11:
		D3D11BE_SetupForShadowMap(l, texwidth, texheight, (smsize-4) / (float)SHADOWMAP_SIZE);
		break;
#endif
#ifdef VKQUAKE
	case QR_VULKAN:
		VKBE_SetupForShadowMap(l, texwidth, texheight, (smsize-4) / (float)SHADOWMAP_SIZE);
		break;
#endif
	default:
		(void)texwidth;
		(void)texheight;
		break;
	}

	if (!BE_SelectDLight(l, colour, axis, lighttype))
		return;
	if (!Sh_GenShadowMap(l, lighttype, axis, lvis, smsize, SHADOWMAP_SIZE))
		return;

	RQuantAdd(RQUANT_RTLIGHT_DRAWN, 1);

	//may as well use scissors
	BE_Scissor(&rect);

	BE_SelectEntity(&r_worldentity);

	BE_SelectMode(BEM_LIGHT);
	Sh_DrawEntLighting(l, colour, vvis);

}


/*
draws faces facing the light
Note: Backend mode must have been selected in advance, as must the light to light from
*/
static void Sh_DrawEntLighting(dlight_t *light, vec3_t colour, qbyte *pvs)
{
	int tno;
	texture_t *tex;
	shader_t *shader;
	shadowmesh_t *sm;

	sm = light->worldshadowmesh;
	if (light->rebuildcache)
		sm = &sh_tempshmesh;
	if (sm)
	{
		for (tno = 0; tno < sm->numbatches; tno++)
		{
			if (!sm->batches[tno].count)
				continue;
			tex = cl.worldmodel->shadowbatches[tno].tex;
			if (cl.worldmodel->fromgame == fg_quake2)
				shader = R_TextureAnimation_Q2(tex)->shader;
			else
				shader = R_TextureAnimation(false, tex)->shader;
			if (shader->flags & (SHADER_NODLIGHT|SHADER_NODRAW|SHADER_SKY))
				continue;
			//FIXME: it should be worth building a dedicated ebo, for static ones
#ifdef GLQUAKE
			if (qrenderer == QR_OPENGL && sm->batches[tno].faceidxcount && !(shader->flags & SHADER_NEEDSARRAYS) && sm->havefaceebo)
			{
				mesh_t unimesh = {0};
				mesh_t *unimeshptr = &unimesh;
				vboarray_t oldidx = cl.worldmodel->shadowbatches[tno].vbo->indicies;
				unimesh.numindexes = sm->batches[tno].faceidxcount;
				unimesh.numvertexes = cl.worldmodel->shadowbatches[tno].vbo->vertcount;
				unimesh.vbofirstelement = sm->batches[tno].faceidxfirst;
				cl.worldmodel->shadowbatches[tno].vbo->indicies.gl.vbo = sm->vefbo[2];
				cl.worldmodel->shadowbatches[tno].vbo->indicies.gl.addr = NULL;

				BE_DrawMesh_List(shader, 1, &unimeshptr, cl.worldmodel->shadowbatches[tno].vbo, NULL, 0);
				cl.worldmodel->shadowbatches[tno].vbo->indicies = oldidx;
			}
			else
#endif
				BE_DrawMesh_List(shader, sm->batches[tno].count, sm->batches[tno].s, cl.worldmodel->shadowbatches[tno].vbo, NULL, 0);
			RQuantAdd(RQUANT_LITFACES, sm->batches[tno].count);
		}

		switch(qrenderer)
		{
		default:
			break;
#ifdef GLQUAKE
		case QR_OPENGL:
			GLBE_BaseEntTextures(pvs, NULL);
			break;
#endif
#ifdef VKQUAKE
		case QR_VULKAN:
			VKBE_BaseEntTextures(pvs, NULL);
			break;
#endif
#ifdef D3D9QUAKE
		case QR_DIRECT3D9:
			D3D9BE_BaseEntTextures(pvs, NULL);
			break;
#endif
#ifdef D3D11QUAKE
		case QR_DIRECT3D11:
			D3D11BE_BaseEntTextures(pvs, NULL);
			break;
#endif
		}
	}
}

#ifdef GLQUAKE
/*Fixme: this is brute forced*/
#ifdef warningmsg
#pragma warningmsg("brush shadows are bruteforced")
#endif
static void Sh_DrawBrushModelShadow(dlight_t *dl, entity_t *e)
{
	int v;
	float *v1, *v2;
	vec3_t v3, v4;
	vec3_t lightorg;

	int i;
	model_t *model;
	msurface_t *surf;

	if (qrenderer != QR_OPENGL)
		return;

	if (BE_LightCullModel(e->origin, e->model))
		return;

	RotateLightVector((void *)e->axis, e->origin, dl->origin, lightorg);

	BE_SelectEntity(e);

	GL_DeselectVAO();
	GL_SelectVBO(0);
	GL_SelectEBO(0);
	qglEnableClientState(GL_VERTEX_ARRAY);

#ifdef BEF_PUSHDEPTH
	GLBE_PolyOffsetStencilShadow(r_pushdepth);
#else
	GLBE_PolyOffsetStencilShadow();
#endif

	model = e->model;
	surf = model->surfaces+model->firstmodelsurface;
	for (i = 0; i < model->nummodelsurfaces; i++, surf++)
	{
		if (surf->flags & SURF_PLANEBACK)
		{//inverted normal.
			if (DotProduct(surf->plane->normal, lightorg)-surf->plane->dist >= -0.1)
				continue;
		}
		else
		{
			if (DotProduct(surf->plane->normal, lightorg)-surf->plane->dist <= 0.1)
				continue;
		}

		if (surf->flags & (SURF_DRAWALPHA | SURF_DRAWTILED))
		{	// no shadows
			continue;
		}

		if (!surf->mesh)
			continue;

		//front face
		qglVertexPointer(3, GL_FLOAT, sizeof(vecV_t), surf->mesh->xyz_array);
		qglDrawArrays(GL_POLYGON, 0, surf->mesh->numvertexes);
//		qglDrawRangeElements(GL_TRIANGLES, 0, surf->mesh->numvertexes-1, surf->mesh->numindexes, GL_INDEX_TYPE, surf->mesh->indexes);
		RQuantAdd(RQUANT_SHADOWINDICIES, surf->mesh->numvertexes);

		for (v = 0; v < surf->mesh->numvertexes; v++)
		{
		//border
			v1 = surf->mesh->xyz_array[v];
			v2 = surf->mesh->xyz_array[( v+1 )%surf->mesh->numvertexes];

			//get positions of v3 and v4 based on the light position
			v3[0] = ( v1[0]-lightorg[0] );
			v3[1] = ( v1[1]-lightorg[1] );
			v3[2] = ( v1[2]-lightorg[2] );
			VectorNormalizeFast(v3);
			VectorScale(v3, PROJECTION_DISTANCE, v3);

			v4[0] = ( v2[0]-lightorg[0] );
			v4[1] = ( v2[1]-lightorg[1] );
			v4[2] = ( v2[2]-lightorg[2] );
			VectorNormalizeFast(v4);
			VectorScale(v4, PROJECTION_DISTANCE, v4);

			//Now draw the quad from the two verts to the projected light
			//verts
			qglBegin( GL_QUAD_STRIP );
				qglVertex3fv(v1);
				qglVertex3f	(v1[0]+v3[0], v1[1]+v3[1], v1[2]+v3[2]);
				qglVertex3fv(v2);
				qglVertex3f (v2[0]+v4[0], v2[1]+v4[1], v2[2]+v4[2]);
			qglEnd();
		}

//back
			//the same applies as earlier
		qglBegin(GL_POLYGON);
		for (v = surf->mesh->numvertexes-1; v >=0; v--)
		{
			v1 = surf->mesh->xyz_array[v];
			v3[0] = (v1[0]-lightorg[0]);
			v3[1] = (v1[1]-lightorg[1]);
			v3[2] = (v1[2]-lightorg[2]);
			VectorNormalizeFast(v3);
			VectorScale(v3, PROJECTION_DISTANCE, v3);

			qglVertex3f(v1[0]+v3[0], v1[1]+v3[1], v1[2]+v3[2]);
		}
		qglEnd();
	}

#ifdef BEF_PUSHDEPTH
	GLBE_PolyOffsetStencilShadow(false);
#else
	GLBE_PolyOffsetStencilShadow();
#endif
}
#endif

#if defined(GLQUAKE) || defined(D3D9QUAKE)
/*when this is called, the gl state has been set up to draw the stencil volumes using whatever extensions we have
if secondside is set, then the gpu sucks and we're drawing stuff the slow 2-pass way, and this is the second pass.
*/
static void Sh_DrawStencilLightShadows(dlight_t *dl, qbyte *lvis, qbyte *vvis, qboolean secondside)
{
	struct shadowmesh_s *sm;
#ifdef GLQUAKE
	extern cvar_t gl_part_flame;
	int		i;
	entity_t *ent;
	model_t *emodel;
#endif

	sm = SHM_BuildShadowMesh(dl, lvis, SMT_STENCILVOLUME);
	if (!sm)
	{
#ifdef GLQUAKE
		Sh_DrawBrushModelShadow(dl, &r_worldentity);
#endif
	}
	else
	{
		switch (qrenderer)
		{
		case QR_NONE:
		case QR_SOFTWARE:
		default:
			break;

#ifdef D3D11QUAKE
//		case QR_DIRECT3D11:
//			D3D11BE_RenderShadowBuffer(sm->numverts, sm->d3d11_vbuffer, sm->numindicies, sm->d3d11_ibuffer);
//			break;
#endif
#ifdef D3D9QUAKE
		case QR_DIRECT3D9:
			D3D9BE_RenderShadowBuffer(sm->numverts, sm->d3d9_vbuffer, sm->numindicies, sm->d3d9_ibuffer);
			break;
#endif
#ifdef GLQUAKE
		case QR_OPENGL:
			GLBE_RenderShadowBuffer(sm->numverts, sm->vefbo[0], sm->verts, sm->numindicies, sm->vefbo[1], sm->indicies);
			break;
#endif
#ifdef VKQUAKE
//		case QR_VULKAN:
//			VKBE_RenderShadowBuffer(sm->numverts, sm->vebo[0], sm->verts, sm->numindicies, sm->vebo[1], sm->indicies);
//			break;
#endif
		}
	}
	if (!r_drawentities.value)
		return;

#ifdef GLQUAKE
	if (qrenderer != QR_OPENGL)
		return;	//FIXME: still uses glBegin specifics.
	if (gl_config_nofixedfunc)
		return;	/*too lazy to use shaders*/
	if (gl_config_gles)
		return;	//FIXME: uses glBegin

	// draw sprites seperately, because of alpha blending
	for (i=r_refdef.firstvisedict ; i<cl_numvisedicts ; i++)
	{
		ent = &cl_visedicts[i];

		if (ent->rtype != RT_MODEL)
			continue;

		if (ent->flags & (RF_NOSHADOW|Q2RF_BEAM))
			continue;

		if (ent->keynum == dl->key && ent->keynum)
			continue;

		emodel = ent->model;
		if (!emodel)
			continue;

		if (cls.allow_anyparticles)	//allowed or static
		{
			if (emodel->engineflags & MDLF_EMITREPLACE)
			{
				if (gl_part_flame.value)
					continue;
			}
		}

		if (emodel->loadstate == MLS_NOTLOADED)
		{
			if (!Mod_LoadModel(emodel, MLV_WARN))
				continue;
		}
		if (emodel->loadstate != MLS_LOADED)
			continue;

		switch (emodel->type)
		{
		case mod_alias:
			if (r_drawentities.ival == 3)
				continue;
			R_DrawGAliasShadowVolume (ent, dl->origin, dl->radius);
			break;

		case mod_brush:
			if (r_drawentities.ival == 2)
				continue;
			Sh_DrawBrushModelShadow (dl, ent);
			break;

		case mod_sprite:	//never any shadows on sprites, it doesn't really make sense.
			break;

		default:
			break;
		}
	}
	BE_SelectEntity(&r_worldentity);
#endif
}

//draws a light using stencil shadows.
//redraws world geometry up to 3 times per light...
static qboolean Sh_DrawStencilLight(dlight_t *dl, vec3_t colour, vec3_t axis[3], qbyte *vvis)
{
	int sref;
	int clus;
	qbyte *lvis;
	srect_t rect;

	vec3_t mins;
	vec3_t maxs;

	if (R_CullSphere(dl->origin, dl->radius))
	{
		RQuantAdd(RQUANT_RTLIGHT_CULL_FRUSTUM, 1);
		return false;	//this should be the more common case
	}

	mins[0] = dl->origin[0] - dl->radius;
	mins[1] = dl->origin[1] - dl->radius;
	mins[2] = dl->origin[2] - dl->radius;

	maxs[0] = dl->origin[0] + dl->radius;
	maxs[1] = dl->origin[1] + dl->radius;
	maxs[2] = dl->origin[2] + dl->radius;

	if (!dl->rebuildcache)
	{
		//fixme: check head node first?
		if (!Sh_LeafInView(dl->worldshadowmesh->litleaves, vvis))
		{
			RQuantAdd(RQUANT_RTLIGHT_CULL_PVS, 1);
			return false;
		}
		lvis = NULL;
	}
	else
	{
		clus = cl.worldmodel->funcs.ClusterForPoint(cl.worldmodel, dl->origin, NULL);	//FIXME: check areas
		lvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, clus, &lvisb, PVM_FAST);
//		if (cl.worldmodel->funcs.ClustersInSphere)
//			lvis = cl.worldmodel->funcs.ClustersInSphere(cl.worldmodel, dl->origin, dl->radius, &lvisb2, lvis);

		if (!Sh_VisOverlaps(lvis, vvis))	//The two viewing areas do not intersect.
		{
			RQuantAdd(RQUANT_RTLIGHT_CULL_PVS, 1);
			return false;
		}
	}

	//sets up the gl scissor (and culls to view)
	if (Sh_ScissorForBox(mins, maxs, &rect))
	{
		RQuantAdd(RQUANT_RTLIGHT_CULL_SCISSOR, 1);
		return false;	//this doesn't cull often.
	}
	RQuantAdd(RQUANT_RTLIGHT_DRAWN, 1);

	BE_SelectDLight(dl, colour, axis, LSHADER_STANDARD);
	BE_SelectMode(BEM_STENCIL);

	//The backend doesn't maintain scissor state.
	//The backend doesn't maintain stencil test state either - it needs to be active for more than just stencils, or disabled. its awkward.
	BE_Scissor(&rect);


	switch(qrenderer)
	{
	default:
		(void)sref;
		break;
#ifdef GLQUAKE
	case QR_OPENGL:
		{
			int sfrontfail;
			int sbackfail;
			qglEnable(GL_STENCIL_TEST);

			//FIXME: is it practical to test to see if scissors allow not clearing the stencil buffer?

			/*we don't need all that much stencil buffer depth, and if we don't get enough or have dodgy volumes, wrap if we can*/
		#ifdef I_LIVE_IN_A_FREE_COUNTRY
			sref = 0;
			sbackfail = GL_INCR;
			sfrontfail = GL_DECR;
			if (gl_config.ext_stencil_wrap)
			{	//minimise damage...
				sbackfail = GL_INCR_WRAP_EXT;
				sfrontfail = GL_DECR_WRAP_EXT;
			}
		#else
			sref = (1<<sh_config.stencilbits)-1; /*this is halved for two-sided stencil support, just in case there's no wrap support*/
			sbackfail = GL_DECR;
			sfrontfail = GL_INCR;
			if (gl_config.ext_stencil_wrap)
			{	//minimise damage...
				sbackfail = GL_DECR_WRAP_EXT;
				sfrontfail = GL_INCR_WRAP_EXT;
			}
		#endif
			//our stencil writes.
			if (gl_config.arb_depth_clamp && r_refdef.maxdist != 0)
				qglEnable(GL_DEPTH_CLAMP_ARB);

		#if 0 //def _DEBUG
		//	if (r_shadows.value == 666)	//testing (visible shadow volumes)
			{
				qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				qglColor4f(dl->color[0], dl->color[1], dl->color[2], 1);
				qglDisable(GL_STENCIL_TEST);
//				qglEnable(GL_POLYGON_OFFSET_FILL);
//				qglPolygonOffset(-1, -1);
			//	qglPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
				Sh_DrawStencilLightShadows(dl, lvis, vvis, false);
//				qglDisable(GL_POLYGON_OFFSET_FILL);
//				qglPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			}
		#endif

			if (qglStencilOpSeparate)
			{
				//ATI/GLES/ARB method
				sref/=2;
				qglClearStencil(sref);
				qglClear(GL_STENCIL_BUFFER_BIT);
				GL_CullFace(0);

				qglStencilFunc(GL_ALWAYS, 0, ~0);

				qglStencilOpSeparate(GL_BACK, GL_KEEP, sbackfail, GL_KEEP);
				qglStencilOpSeparate(GL_FRONT, GL_KEEP, sfrontfail, GL_KEEP);

				Sh_DrawStencilLightShadows(dl, lvis, vvis, false);
				qglStencilOpSeparate(GL_FRONT_AND_BACK, GL_KEEP, GL_KEEP, GL_KEEP);

				GL_CullFace(SHADER_CULL_FRONT);

				qglStencilFunc(GL_EQUAL, sref, ~0);
			}
			else if (qglActiveStencilFaceEXT)
			{
				//Nvidia-specific method.
				sref/=2;
				qglClearStencil(sref);
				qglClear(GL_STENCIL_BUFFER_BIT);
				GL_CullFace(0);

				qglEnable(GL_STENCIL_TEST_TWO_SIDE_EXT);

				qglActiveStencilFaceEXT(GL_BACK);	
				qglStencilOp(GL_KEEP, sbackfail, GL_KEEP);
				qglStencilFunc(GL_ALWAYS, 0, ~0 );

				qglActiveStencilFaceEXT(GL_FRONT);
				qglStencilOp(GL_KEEP, sfrontfail, GL_KEEP);
				qglStencilFunc(GL_ALWAYS, 0, ~0 );

				Sh_DrawStencilLightShadows(dl, lvis, vvis, false);

				qglActiveStencilFaceEXT(GL_BACK);
				qglStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
				qglStencilFunc(GL_ALWAYS, 0, ~0 );

				qglActiveStencilFaceEXT(GL_FRONT);
				qglStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
				qglStencilFunc(GL_EQUAL, sref, ~0 );

				qglDisable(GL_STENCIL_TEST_TWO_SIDE_EXT);
			}
			else //your graphics card sucks and lacks efficient stencil shadow techniques.
			{	//centered around 0. Will only be increased then decreased less.
				qglClearStencil(sref);
				qglClear(GL_STENCIL_BUFFER_BIT);

				qglStencilFunc(GL_ALWAYS, 0, ~0);

				GL_CullFace(SHADER_CULL_BACK);
				qglStencilOp(GL_KEEP, sbackfail, GL_KEEP);
				Sh_DrawStencilLightShadows(dl, lvis, vvis, false);

				GL_CullFace(SHADER_CULL_FRONT);
				qglStencilOp(GL_KEEP, sfrontfail, GL_KEEP);
				Sh_DrawStencilLightShadows(dl, lvis, vvis, true);

				qglStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
				qglStencilFunc(GL_EQUAL, sref, ~0);
			}
			if (gl_config.arb_depth_clamp)
				qglDisable(GL_DEPTH_CLAMP_ARB);
			//end stencil writing.

			BE_SelectMode(BEM_LIGHT);
			Sh_DrawEntLighting(dl, colour, vvis);
			qglDisable(GL_STENCIL_TEST);
			qglStencilFunc( GL_ALWAYS, 0, ~0 );
		}
		break;
#endif
#ifdef D3D9QUAKE
	case QR_DIRECT3D9:
		sref = (1<<8)-1;
		sref/=2;

		/*clear the stencil buffer*/
		IDirect3DDevice9_Clear(pD3DDev9, 0, NULL, D3DCLEAR_STENCIL, D3DCOLOR_XRGB(0, 0, 0), 1.0f, sref);

		/*set up 2-sided stenciling*/
		D3D9BE_Cull(0);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILENABLE, true);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_TWOSIDEDSTENCILMODE, true);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILZFAIL, D3DSTENCILOP_DECR);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_INCR);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_CCW_STENCILPASS, D3DSTENCILOP_KEEP);

		/*draw the shadows*/
		Sh_DrawStencilLightShadows(dl, lvis, vvis, false);

		//disable stencil writing
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_TWOSIDEDSTENCILMODE, false);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILREF, sref);
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILMASK, ~0);

		/*draw the light*/
		BE_SelectMode(BEM_LIGHT);
		Sh_DrawEntLighting(dl, colour, vvis);

		/*okay, no more stencil stuff*/
		IDirect3DDevice9_SetRenderState(pD3DDev9, D3DRS_STENCILENABLE, false);
		break;
#endif
	}

	return true;
}
#else
#define Sh_DrawStencilLight(dl,rgb,axis,vvis) Sh_DrawShadowlessLight(dl,rgb,axis,vvis,LSHADER_STANDARD)
#endif

qboolean Sh_CullLight(dlight_t *dl, qbyte *vvis)
{
	if (R_CullSphere(dl->origin, dl->radius))
	{
		RQuantAdd(RQUANT_RTLIGHT_CULL_FRUSTUM, 1);
		return true;	//this should be the more common case
	}

	if (!dl->rebuildcache)
	{
		//fixme: check head node first?
		if (!Sh_LeafInView(dl->worldshadowmesh->litleaves, vvis))
		{
			RQuantAdd(RQUANT_RTLIGHT_CULL_PVS, 1);
			return true;
		}
	}
	else
	{
		int clus;
		qbyte *lvis;

		clus = cl.worldmodel->funcs.ClusterForPoint(cl.worldmodel, dl->origin, NULL);
		lvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, clus, &lvisb, PVM_FAST);
//		if (cl.worldmodel->funcs.ClustersInSphere)
//			lvis = cl.worldmodel->funcs.ClustersInSphere(cl.worldmodel, dl->origin, dl->radius, &lvisb2, lvis);

		SHM_BuildShadowMesh(dl, lvis, SMT_DEFERRED);

		if (!Sh_VisOverlaps(lvis, vvis))	//The two viewing areas do not intersect.
		{
			RQuantAdd(RQUANT_RTLIGHT_CULL_PVS, 1);
			return true;
		}
	}
	return false;	//please draw this...
}

static void Sh_DrawShadowlessLight(dlight_t *dl, vec3_t colour, vec3_t axis[3], qbyte *vvis, unsigned int lshaderflags)
{
	vec3_t mins, maxs;
	srect_t rect;

	if (R_CullSphere(dl->origin, dl->radius))
	{
		RQuantAdd(RQUANT_RTLIGHT_CULL_FRUSTUM, 1);
		return;	//this should be the more common case
	}

	if (!dl->rebuildcache)
	{
		//fixme: check head node first?
		if (!Sh_LeafInView(dl->worldshadowmesh->litleaves, vvis))
		{
			RQuantAdd(RQUANT_RTLIGHT_CULL_PVS, 1);
			return;
		}
	}
	else
	{
		int clus;
		qbyte *lvis;

		if (cl.worldmodel->funcs.ClustersInSphere)
			lvis = cl.worldmodel->funcs.ClustersInSphere(cl.worldmodel, dl->origin, dl->radius, &lvisb2, NULL);
		else
		{
			clus = cl.worldmodel->funcs.ClusterForPoint(cl.worldmodel, dl->origin, NULL);
			lvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, clus, &lvisb, PVM_FAST);
		}

		SHM_BuildShadowMesh(dl, lvis, SMT_SHADOWLESS);
		//FIXME: check areas
		if (!Sh_VisOverlaps(lvis, vvis))	//The two viewing areas do not intersect.
		{
			RQuantAdd(RQUANT_RTLIGHT_CULL_PVS, 1);
			return;
		}
	}

	mins[0] = dl->origin[0] - dl->radius;
	mins[1] = dl->origin[1] - dl->radius;
	mins[2] = dl->origin[2] - dl->radius;

	maxs[0] = dl->origin[0] + dl->radius;
	maxs[1] = dl->origin[1] + dl->radius;
	maxs[2] = dl->origin[2] + dl->radius;


//sets up the gl scissor (actually just culls to view)
	if (Sh_ScissorForBox(mins, maxs, &rect))
	{
		RQuantAdd(RQUANT_RTLIGHT_CULL_SCISSOR, 1);
		return;	//was culled.
	}

	//should we actually scissor here? there's not really much point I suppose.
	BE_Scissor(NULL);

	RQuantAdd(RQUANT_RTLIGHT_DRAWN, 1);

	if (dl->fov)
		lshaderflags |= LSHADER_SPOT;
	BE_SelectDLight(dl, colour, axis, lshaderflags);
	BE_SelectMode(BEM_LIGHT);
	Sh_DrawEntLighting(dl, colour, vvis);
}

void Sh_DrawCrepuscularLight(dlight_t *dl, float *colours)
{
#ifdef GLQUAKE
	int oldfbo;
	int cwidth, cheight;
	static int crep_w = 0, crep_h = 0;
	static mesh_t mesh;
	image_t *oldsrccol;
	static vecV_t xyz[4] =
	{	//nettest: clip-space fullscreen quad.  The z is OVERWRITTEN each call (below) from
		//r_sun_occludedepth so the composite's forced depth test occludes the rays on near
		//geometry (the first-person viewmodel).  See ENGINE_PATCHES Patch 76.
		{-1,-1,-1},
		{-1,1,-1},
		{1,1,-1},
		{1,-1,-1}
	};
	static vec2_t tc[4] =
	{
		{0,0},
		{0,1},
		{1,1},
		{1,0}
	};
	static index_t idx[6] =
	{
		0,1,2,
		0,2,3
	};
	if (qrenderer != QR_OPENGL)
		return;

	mesh.numindexes = 6;
	mesh.numvertexes = 4;
	mesh.xyz_array = xyz;
	mesh.st_array = tc;
	mesh.indexes = idx;

	/*
	a crepuscular light (seriously, that's the correct spelling) is one that gives 'god rays', rather than regular light.
	our implementation doesn't cast shadows. this allows it to actually be outside the map, and to shine through cloud layers in the sky.
	we could cast shadows if the light was actually inside, I suppose.
	Anyway, its done using an FBO, where everything but the sky is black (stuff that occludes the sky is black too).
	which is then blitted onto the screen in 2d-space.
	*/

	/*requires an FBO, as stated above*/
	if (!gl_config.ext_framebuffer_objects)
		return;

	//fixme: we should add an extra few pixels each side to the fbo, to avoid too much weirdness at screen edges.

	//size the mask FBO to the SCENE render target (r_refdef.pxrect), NOT the window
	//(vid.pixelwidth).  With r_renderscale>1 the scene is rendered larger, so a
	//window-sized mask lands offset/scaled from the geometry ("god rays way off").
	cwidth  = r_refdef.pxrect.width;
	cheight = r_refdef.pxrect.height;
	if (cwidth  < 1) cwidth  = 1;
	if (cheight < 1) cheight = 1;

	if (!crepuscular_texture_id)
	{
		/*FIXME: requires npot*/
		crepuscular_shader = R_RegisterShader("crepuscular_screen", SUF_NONE,
			"{\n"
				"program crepuscular_rays\n"
				"{\n"
					"map $sourcecolour\n"
					"blend add\n"
				"}\n"
			"}\n"
			);

		crepuscular_texture_id = Image_CreateTexture("***crepusculartexture***", NULL, IF_LINEAR|IF_NOMIPMAP|IF_CLAMP|IF_NOGAMMA);
		crep_w = crep_h = 0;	//force the (re)upload below
	}
	//(re)size the mask texture to match the scene render target whenever it changes.
	if (crep_w != cwidth || crep_h != cheight)
	{
		Image_Upload(crepuscular_texture_id, TF_RGBA32, NULL, NULL, cwidth, cheight, 1, IF_LINEAR|IF_NOMIPMAP|IF_CLAMP|IF_NOGAMMA);
		crep_w = cwidth;
		crep_h = cheight;
	}

	BE_Scissor(NULL);

	oldfbo = GLBE_FBO_Update(&crepuscular_fbo, FBO_RB_DEPTH, &crepuscular_texture_id, 1, r_nulltex, cwidth, cheight, 0);

	GL_ForceDepthWritable();
//	qglClearColor(0, 0, 0, 1);
	qglClear(GL_DEPTH_BUFFER_BIT);

	BE_SelectMode(BEM_CREPUSCULAR);
	BE_SelectDLight(dl, colours, dl->axis, LSHADER_STANDARD);
	GLBE_SubmitMeshes(cl.worldmodel->batches, SHADER_SORT_PORTAL, SHADER_SORT_BLEND);

	GLBE_FBO_Pop(oldfbo);

	oldsrccol = NULL;//shaderstate.tex_sourcecol;
	GLBE_FBO_Sources(crepuscular_texture_id, NULL);
//	crepuscular_shader->defaulttextures.base = crepuscular_texture_id;
	//shaderstate.tex_sourcecol = oldsrccol;

	BE_SelectMode(BEM_STANDARD);

	//nettest: depth-occlude the additive rays against the scene depth so NEAR geometry (the
	//first-person viewmodel) blocks them (otherwise the additive composite glows over the
	//already-drawn gun).  Set the quad's clip-z from r_sun_occludedepth (window depth 0..1 ->
	//NDC -1..1) and FORCE the depth test on (BEF_FORCEDEPTHTEST) -- the blend-add composite
	//otherwise leaves depth-test in an indeterminate state (was drawing over everything).  GL_LEQUAL
	//then rejects the rays where the scene is NEARER than the boundary (the gun) and passes over the
	//farther scene (glow preserved).  Depth-WRITE stays off (blend-add), so the buffer is untouched.
	{
		float zt = r_sun_occludedepth.value * 2.0 - 1.0;
		xyz[0][2] = xyz[1][2] = xyz[2][2] = xyz[3][2] = zt;
	}
	BE_DrawMesh_Single(crepuscular_shader, &mesh, NULL, BEF_FORCEDEPTHTEST);

	GLBE_FBO_Sources(oldsrccol, NULL);
#endif
}

void Sh_PurgeShadowMeshes(void)
{
	dlight_t *dl;
	size_t i;
	for (dl = cl_dlights, i=0; i<cl_maxdlights; i++, dl++)
	{
		if (dl->worldshadowmesh)
		{
			SH_FreeShadowMesh(dl->worldshadowmesh);
			dl->worldshadowmesh = NULL;
			dl->rebuildcache = true;
		}
	}
	Z_Free(edge);
	edge = NULL;
	maxedge = 0;
}

void R_StaticEntityToRTLight(int i);
void Sh_PreGenerateLights(void)
{
	unsigned int ignoreflags;
	dlight_t *dl;
	int shadowtype;
	int leaf;
	qbyte *lvis;
	int i;

	if (r_shadow_realtime_world_importlightentitiesfrommap.modified)
	{
		rtlights_first = RTL_FIRST;
		rtlights_max = RTL_FIRST;
	}

	r_shadow_realtime_world_importlightentitiesfrommap.modified = false;
	r_shadow_realtime_world_lightmaps.value = atof(r_shadow_realtime_world_lightmaps.string);
	if (!cl.worldmodel)
		return;
	if ((r_shadow_realtime_dlight.ival || r_shadow_realtime_world.ival) && rtlights_max == RTL_FIRST)
	{
		qboolean okay = false;
		r_shadow_realtime_world_lightmaps_force = -1;
		if (!okay && r_shadow_realtime_world_importlightentitiesfrommap.ival <= 1)
			okay |= R_LoadRTLights();
		if (!okay)
		{
			for (i = 0; i < cl.num_statics; i++)
				R_StaticEntityToRTLight(i);
			okay |= rtlights_max != RTL_FIRST;
		}
		if (!okay)
			okay |= R_ImportRTLights(Mod_GetEntitiesString(cl.worldmodel), r_shadow_realtime_world_importlightentitiesfrommap.ival);
		if (!okay && r_shadow_realtime_world.ival && r_shadow_realtime_world_lightmaps.value < 0.5)
		{
			r_shadow_realtime_world_lightmaps.value = 1;
			if (!r_shadow_realtime_world_importlightentitiesfrommap.ival)
				Con_Printf(CON_WARNING "No lights detected in map, ^[[and importing is disabled]\\type\\r_shadow_realtime_world_importlightentitiesfrommap 1^].\n");
			else
				Con_DPrintf("No lights detected in map.\n");
		}

		for (i = 0; i < cl.num_statics; i++)
		{
			R_StaticEntityToRTLight(i);
		}
	}

	if (r_shadow_realtime_world_lightmaps_force >= 0)
		r_shadow_realtime_world_lightmaps.value = r_shadow_realtime_world_lightmaps_force;

	ignoreflags = (r_shadow_realtime_world.ival?LFLAG_REALTIMEMODE:0)
			| (r_shadow_realtime_dlight.ival?LFLAG_NORMALMODE:0);

	for (dl = cl_dlights+rtlights_first, i=rtlights_first; i<rtlights_max; i++, dl++)
	{
		dl->rebuildcache = true;

		if (dl->radius)
		{
			if (dl->flags & ignoreflags)
			{
				if (dl->flags & LFLAG_CREPUSCULAR)
					continue;

				if (((!dl->die)?!r_shadow_realtime_world_shadows.ival:!r_shadow_realtime_dlight_shadows.ival) || (dl->flags & LFLAG_NOSHADOWS))
					shadowtype = SMT_SHADOWLESS;
				else if (r_shadow_raytrace.ival)
					shadowtype = SMT_SHADOWLESS;	//shadows are done via acceleration structures set up by the backend. don't need to worry about them here, they're effectively shadowless.
				else if (dl->flags & LFLAG_SHADOWMAP || r_shadow_shadowmapping.ival)
					shadowtype = SMT_SHADOWMAP;
				else
					shadowtype = SMT_STENCILVOLUME;

				//shadowless and lights with an ambient term pass through walls, so need to affect EVERY leaf withing the sphere.
				if ((shadowtype == SMT_SHADOWLESS || dl->lightcolourscales[0]) && cl.worldmodel->funcs.ClustersInSphere)
					lvis = cl.worldmodel->funcs.ClustersInSphere(cl.worldmodel, dl->origin, dl->radius, &lvisb2, NULL);
				else
				{	//other lights only want to use the source leaf's pvs (clamped by the sphere)
					leaf = cl.worldmodel->funcs.ClusterForPoint(cl.worldmodel, dl->origin, NULL);
					lvis = cl.worldmodel->funcs.ClusterPVS(cl.worldmodel, leaf, &lvisb, PVM_FAST);
					if (cl.worldmodel->funcs.ClustersInSphere)
						lvis = cl.worldmodel->funcs.ClustersInSphere(cl.worldmodel, dl->origin, dl->radius, &lvisb2, lvis);
				}

				SHM_BuildShadowMesh(dl, lvis, shadowtype);
				continue;
			}
		}

		if (dl->worldshadowmesh)
		{
			SH_FreeShadowMesh(dl->worldshadowmesh);
			dl->worldshadowmesh = NULL;
			dl->rebuildcache = true;
		}
	}
}

void Com_ParseVector(char *str, vec3_t out)
{
	str = COM_Parse(str);
	out[0] = atof(com_token);
	str = COM_Parse(str);
	out[1] = atof(com_token);
	str = COM_Parse(str);
	out[2] = atof(com_token);
}

void Sh_CheckSettings(void)
{
	extern cvar_t r_shadows;
	qboolean canstencil = false, cansmap = false, canshadowless = false, canraytrace = false;
	r_shadow_raytrace.ival = r_shadow_raytrace.value;
	r_shadow_shadowmapping.ival = r_shadow_shadowmapping.value;
	r_shadow_realtime_world.ival = r_shadow_realtime_world.value;
	r_shadow_realtime_dlight.ival = r_shadow_realtime_dlight.value;
	r_shadow_realtime_world_shadows.ival = r_shadow_realtime_world_shadows.value;
	r_shadow_realtime_dlight_shadows.ival = r_shadow_realtime_dlight_shadows.value;

	switch(qrenderer)
	{
#ifdef VKQUAKE
	case QR_VULKAN:
		canshadowless = true;
		cansmap = vk.multisamplebits==VK_SAMPLE_COUNT_1_BIT; //FIXME - we need to render shadowmaps without needing to restart the current scene.
		canraytrace = vk.khr_ray_query;
		canstencil = false;
		break;
#endif
#ifdef GLQUAKE
	case QR_OPENGL:
		canshadowless = gl_config.arb_shader_objects || !gl_config_nofixedfunc; //falls back to crappy texture env
		if (gl_config.arb_shader_objects && gl_config.ext_framebuffer_objects && gl_config.arb_depth_texture)// && gl_config.arb_shadow)
			cansmap = true;
		else if ((r_shadow_realtime_world_shadows.ival || r_shadow_realtime_dlight_shadows.ival) && r_shadow_shadowmapping.ival)
		{
			if (!gl_config.arb_shader_objects)
				Con_DPrintf("Shadowmapping unsupported: No arb_shader_objects\n");
			else if (!gl_config.ext_framebuffer_objects)
				Con_DPrintf("Shadowmapping unsupported: No ext_framebuffer_objects\n");
			else if (!gl_config.arb_depth_texture)
				Con_DPrintf("Shadowmapping unsupported: No arb_depth_texture\n");
		}
		if (sh_config.stencilbits)
			canstencil = true;
		break;
#endif
#ifdef D3D9QUAKE
	case QR_DIRECT3D9:
//		canshadowless = true;
		//the code still has a lot of ifdefs, so will crash if you try it in a merged build.
		//its not really usable in d3d-only builds either, so no great loss.
//		canstencil = true;
		break;
#endif
#ifdef D3D11QUAKE
	case QR_DIRECT3D11:
		canshadowless = true;	//all feature levels
/* shadows are buggy right now. tbh they've always been buggy... rendering seems fine, its just the shadowmaps that are bad
		if (D3D11_BeginShadowMap(0, SHADOWMAP_SIZE*3, SHADOWMAP_SIZE*2))
		{
			D3D11_EndShadowMap();
			cansmap = true;		//tends to not work properly until feature level 10 for one error or another.
		}
*/
		break;
#endif
	default:
		break;
	}

	if (!canstencil && !cansmap && !canshadowless)
	{
		//can't even do lighting
		if (r_shadow_realtime_world.ival || r_shadow_realtime_dlight.ival)
			Con_Printf("Missing rendering features: realtime %s lighting is not possible.\n", r_shadow_realtime_world.ival?"world":"dynamic");
		r_shadow_realtime_world.ival = 0;
		r_shadow_realtime_dlight.ival = 0;
		r_shadow_raytrace.ival = 0;
	}
	else if (!canstencil && !cansmap)
	{
		if (canraytrace)	//just silently force raytrace on if we're not allowed stencil nor shadowmaps, but can use rt...
			r_shadow_raytrace.ival = true;
		else
		{
			//no shadow methods available at all.
			if ((r_shadow_realtime_world.ival&&r_shadow_realtime_world_shadows.ival)||(r_shadow_realtime_dlight.ival&&r_shadow_realtime_dlight_shadows.ival))
				Con_Printf("Missing rendering features: realtime shadows are not possible.\n");
			r_shadow_realtime_world_shadows.ival = 0;
			r_shadow_realtime_dlight_shadows.ival = 0;
			r_shadow_raytrace.ival = 0;
		}
	}
	else
	{
		r_shadow_raytrace.ival = r_shadow_raytrace.ival && canraytrace;
		if (!canstencil || !cansmap)
		{
			//only one shadow method
			if (!!r_shadow_shadowmapping.ival != cansmap)
			{
				if (!r_shadow_raytrace.ival && r_shadow_shadowmapping.ival && ((r_shadow_realtime_world.ival&&r_shadow_realtime_world_shadows.ival)||(r_shadow_realtime_dlight.ival&&r_shadow_realtime_dlight_shadows.ival)))
					Con_Printf("Missing rendering features: forcing shadowmapping %s.\n", cansmap?"on":"off");
				r_shadow_shadowmapping.ival = cansmap;
			}
		}
	}

	cansmap = cansmap && (r_shadows.ival==2);
	if (r_fakeshadows != cansmap)
	{
		r_fakeshadows = cansmap;
		Shader_NeedReload(false);
	}
	r_blobshadows = r_fakeshadows?0:r_shadows.value;	//force it off the hacky way.
}

void Sh_CalcPointLight(vec3_t point, vec3_t light)
{
	vec3_t colour;
	dlight_t *dl;
	vec3_t disp;
	float dist;
	float frac;
	int i;
	unsigned int ignoreflags;

	vec3_t norm, impact;
	ignoreflags = (r_shadow_realtime_world.ival?LFLAG_REALTIMEMODE:0)
			| (r_shadow_realtime_dlight.ival?LFLAG_NORMALMODE:0);

	VectorClear(light);
	if (ignoreflags)
	for (dl = cl_dlights+rtlights_first, i=rtlights_first; i<rtlights_max; i++, dl++)
	{
		if (!(dl->flags & ignoreflags))
			continue;

		if (dl->key == cl.playerview[0].viewentity)	//ignore the light if its emitting from the player. generally the player can't *SEE* that light so it still counts.
			continue;								//disable this check if this function gets used for anything other than iris adaptation

		colour[0] = dl->color[0];
		colour[1] = dl->color[1];
		colour[2] = dl->color[2];
		if (dl->style>=0 && dl->style<cl_max_lightstyles)
		{
			colour[0] *= cl_lightstyle[dl->style].colours[0] * d_lightstylevalue[dl->style]/255.0f;
			colour[1] *= cl_lightstyle[dl->style].colours[1] * d_lightstylevalue[dl->style]/255.0f;
			colour[2] *= cl_lightstyle[dl->style].colours[2] * d_lightstylevalue[dl->style]/255.0f;
		}
		else
		{
			colour[0] *= r_lightstylescale.value;
			colour[1] *= r_lightstylescale.value;
			colour[2] *= r_lightstylescale.value;
		}

		if (colour[0] < 0.001 && colour[1] < 0.001 && colour[2] < 0.001)
			continue;	//just switch these off.

		VectorSubtract(dl->origin, point, disp);
		dist = VectorLength(disp);
		frac = dist / dl->radius;
		if (frac >= 1)
			continue;
		//FIXME: this should be affected by the direction.
		if (CL_TraceLine(point, dl->origin, impact, norm, NULL)>=1)
			VectorMA(light, 1-frac, colour, light);
	}
}

void Sh_DrawLights(qbyte *vis)
{
	vec3_t rotated[3];
	vec3_t *axis;
	vec3_t colour;
	dlight_t *dl;
	int i;
	unsigned int ignoreflags;
	extern cvar_t r_shadows;

	if (r_shadow_realtime_world.modified ||
		r_shadow_realtime_world_shadows.modified ||
		r_shadow_realtime_world_importlightentitiesfrommap.modified ||
		r_shadow_realtime_dlight.modified ||
		r_shadow_realtime_dlight_shadows.modified ||
		r_shadow_shadowmapping.modified || r_shadows.modified ||
		r_shadow_raytrace.modified)
	{
		r_shadow_realtime_world.modified =
		r_shadow_realtime_world_shadows.modified =
		r_shadow_realtime_dlight.modified =
		r_shadow_realtime_dlight_shadows.modified =
		r_shadow_shadowmapping.modified =
		r_shadows.modified =
		r_shadow_raytrace.modified =
				false;
		Sh_CheckSettings();
		//make sure the lighting is reloaded
		Sh_PreGenerateLights();
	}

	if (r_lightprepass)
		return;

	ignoreflags = (r_shadow_realtime_world.ival?LFLAG_REALTIMEMODE:0)
				| (r_shadow_realtime_dlight.ival?LFLAG_NORMALMODE:0)
				| ((r_dynamic.ival&&!r_dlightlightmaps)?LFLAG_LIGHTMAP:0);	//if we're using scenecache and won't be updating lightmaps then we should still respect r_dynamic.
	if (!ignoreflags)
		return;

//	if (r_refdef.recurse)
	for (dl = cl_dlights+rtlights_first, i=rtlights_first; i<rtlights_max; i++, dl++)
	{
		if (!dl->radius)
			continue;	//dead

		if (!(dl->flags & ignoreflags))
			continue;

		colour[0] = dl->color[0];
		colour[1] = dl->color[1];
		colour[2] = dl->color[2];
		if (dl->customstyle)
		{
			const char *map = dl->customstyle;
			int maplen = strlen(map);

			int idx, v1, v2, vd;
			float frac, strength;

			if (!maplen)
			{
				strength = ('m'-'a')*22 * r_lightstylescale.value/255.0;
			}
			else if (map[0] == '=')
			{
				strength = atof(map+1)*r_lightstylescale.value;
			}
			else
			{
				frac = (cl.time*r_lightstylespeed.value);
				if (*map == '?' && maplen>1)
				{
					map++;
					maplen--;
					frac += i*M_PI;
				}
				frac += i*M_PI;
				if (frac < 0)
					frac = 0;
				idx = (int)frac;
				frac -= idx;	//this can require updates at 1000 times a second.. Depends on your framerate of course

				v1 = idx % maplen;
				v1 = map[v1] - 'a';

				v2 = (idx+1) % maplen;
				v2 = map[v2] - 'a';

				vd = v1 - v2;
				if (/*!r_lightstylesmooth.ival ||*/ vd < -r_lightstylesmooth_limit.ival || vd > r_lightstylesmooth_limit.ival)
					strength = v1*(22/255.0)*r_lightstylescale.value;
				else
					strength = (v1*(1-frac) + v2*(frac))*(22/255.0)*r_lightstylescale.value;
			}
			strength *= d_lightstylevalue[0]/256.0f;	//a lot of QW mods use lightstyle 0 for a global darkening fade-in thing, so be sure to respect that.
			colour[0] *= strength;
			colour[1] *= strength;
			colour[2] *= strength;
		}
		if (dl->style>=0 && dl->style < cl_max_lightstyles)
		{
			colour[0] *= cl_lightstyle[dl->style].colours[0] * d_lightstylevalue[dl->style]/255.0f;
			colour[1] *= cl_lightstyle[dl->style].colours[1] * d_lightstylevalue[dl->style]/255.0f;
			colour[2] *= cl_lightstyle[dl->style].colours[2] * d_lightstylevalue[dl->style]/255.0f;
		}
		else
		{
			colour[0] *= r_lightstylescale.value;
			colour[1] *= r_lightstylescale.value;
			colour[2] *= r_lightstylescale.value;
		}
		if (dl->fade[1])
		{
			vec3_t dir;
			float dist;
			VectorSubtract(dl->origin, r_origin, dir);
			dist = VectorLength(dir);
			if (dist > dl->fade[1])
				continue;
			if (dist > dl->fade[0])
			{
				dist = 1-((dist-dl->fade[0]) / (dl->fade[1]-dl->fade[0]));
				VectorScale(colour, dist, colour);
			}
		}
		colour[0] *= r_refdef.hdr_value;
		colour[1] *= r_refdef.hdr_value;
		colour[2] *= r_refdef.hdr_value;

		if (colour[0] < 0.001 && colour[1] < 0.001 && colour[2] < 0.001)
			continue;	//just switch these off.

		if (!dl->lightcolourscales[0] && !dl->lightcolourscales[1] && !dl->lightcolourscales[2])
			continue;	//these lights are just coronas.

		if (dl->rotation[0] || dl->rotation[1] || dl->rotation[2])
		{	//auto-rotating (static) rtlights
			vec3_t rot;
			vec3_t rotationaxis[3];
			VectorScale(dl->rotation, cl.time, rot);
			AngleVectorsFLU(rot, rotationaxis[0], rotationaxis[1], rotationaxis[2]);
			Matrix3_Multiply(dl->axis, rotationaxis, rotated);
			axis = rotated;
		}
		else
			axis = dl->axis;

		if (dl->flags & LFLAG_ORTHO)
		{
			vec3_t saveorg = {dl->origin[0], dl->origin[1], dl->origin[2]};
			vec3_t saveaxis[3];
			memcpy(saveaxis, dl->axis, sizeof(saveaxis));
			memcpy(dl->axis, axis, sizeof(saveaxis));
			Sh_OrthoAlignToFrustum(dl, SHADOWMAP_SIZE);
			dl->rebuildcache = true;
			Sh_DrawShadowMapLight(dl, colour, axis, NULL);
			VectorCopy(saveorg, dl->origin);
			memcpy(dl->axis, saveaxis, sizeof(saveaxis));
		}
		else if (dl->flags & LFLAG_CREPUSCULAR)
			Sh_DrawCrepuscularLight(dl, colour);
		else if (dl->flags & LFLAG_NOSHADOWS ||
			((i >= RTL_FIRST)?!r_shadow_realtime_world_shadows.ival:!r_shadow_realtime_dlight_shadows.ival) || //force shadowless when configured that way...
			ignoreflags==LFLAG_LIGHTMAP)	//scenecache fallback...
		{
			Sh_DrawShadowlessLight(dl, colour, axis, vis, LSHADER_STANDARD);
		}
		else if (r_shadow_raytrace.ival)
			Sh_DrawShadowlessLight(dl, colour, axis, vis, LSHADER_RAYQUERY);
		else if ((dl->flags & LFLAG_SHADOWMAP) || r_shadow_shadowmapping.ival)
		{
			Sh_DrawShadowMapLight(dl, colour, axis, vis);
		}
		else
		{
			Sh_DrawStencilLight(dl, colour, axis, vis);
		}
	}

#ifdef GLQUAKE
	if (gl_config.arb_shader_objects)
	{
		dlight_t sun = {0};
		vec3_t sundir;
		float dot;
		Com_ParseVector(r_sun_dir.string, sundir);
		Com_ParseVector(r_sun_colour.string, colour);

		//fade it out if we're looking at an angle parallel to it (to avoid nasty visible graduations or backwards rays!)
		dot = DotProduct(vpn, sundir);
		dot = 1-dot;
		dot *= dot;
		dot = 1-dot;
		VectorScale(colour, dot, colour);

		if (colour[0] > 0.001 || colour[1] > 0.001 || colour[2] > 0.001)
		{
			//only do this if we can see some sky surfaces. pointless otherwise
			batch_t *b;
			for (b = cl.worldmodel->batches[SHADER_SORT_SKY]; b; b = b->next)
			{
				if (b->meshes)
					break;
			}
			if (b)
			{
				VectorNormalize(sundir);
				VectorMA(r_origin, 1000, sundir, sun.origin);
				Sh_DrawCrepuscularLight(&sun, colour);
			}
		}
	}
#endif

	BE_Scissor(NULL);

	BE_SelectMode(BEM_STANDARD);

//	if (developer.value)
//	Con_Printf("%i lights drawn, %i frustum culled, %i pvs culled, %i scissor culled\n", bench.numlights, bench.numfrustumculled, bench.numpvsculled, bench.numscissorculled);
//	memset(&bench, 0, sizeof(bench));
}
#endif

//stencil shadows generally require that the farclip distance is really really far away
//so this little function is used to check if its needed or not.
qboolean Sh_StencilShadowsActive(void)
{
#if defined(RTLIGHTS) && !defined(SERVERONLY)
	//if shadowmapping is forced on all lights then we don't need special depth stuff
	if (r_shadow_shadowmapping.ival)
		return false;
	if (isDedicated)
		return false;
	return	(r_shadow_realtime_dlight.ival && r_shadow_realtime_dlight_shadows.ival) ||
			(r_shadow_realtime_world.ival && r_shadow_realtime_world_shadows.ival);
#else
	return false;
#endif
}

void Sh_RegisterCvars(void)
{
#if defined(RTLIGHTS) && !defined(SERVERONLY)
#define REALTIMELIGHTING "Realtime Lighting"
	Cvar_Register (&r_shadow_scissor,					REALTIMELIGHTING);
	Cvar_Register (&r_shadow_realtime_world,			REALTIMELIGHTING);
	Cvar_Register (&r_shadow_realtime_world_shadows,	REALTIMELIGHTING);
	Cvar_Register (&r_shadow_realtime_dlight,			REALTIMELIGHTING);
	Cvar_Register (&r_shadow_realtime_dlight_ambient,	REALTIMELIGHTING);
	Cvar_Register (&r_shadow_realtime_dlight_diffuse,	REALTIMELIGHTING);
	Cvar_Register (&r_shadow_realtime_dlight_specular,	REALTIMELIGHTING);
	Cvar_Register (&r_shadow_realtime_dlight_shadows,	REALTIMELIGHTING);
	Cvar_Register (&r_shadow_realtime_world_lightmaps,	REALTIMELIGHTING);
	Cvar_Register (&r_shadow_playershadows,				REALTIMELIGHTING);
	Cvar_Register (&r_shadows_viewmodel,				REALTIMELIGHTING);
	Cvar_Register (&r_shadow_raytrace,					REALTIMELIGHTING);
	Cvar_Register (&r_shadow_shadowmapping,				REALTIMELIGHTING);
	Cvar_Register (&r_shadow_shadowmapping_precision,	REALTIMELIGHTING);
	Cvar_Register (&r_shadow_shadowmapping_nearclip,	REALTIMELIGHTING);
	Cvar_Register (&r_shadow_shadowmapping_bias,		REALTIMELIGHTING);
	Cvar_Register (&r_sun_dir,							REALTIMELIGHTING);
	Cvar_Register (&r_sun_colour,						REALTIMELIGHTING);
	Cvar_Register (&r_sun_occludedepth,					REALTIMELIGHTING);
	Cvar_Register (&r_shadows_distance,					REALTIMELIGHTING);
	Cvar_Register (&r_shadows_bias,						REALTIMELIGHTING);
	Cvar_Register (&r_shadows_throwfade,				REALTIMELIGHTING);
	Cvar_Register (&r_shadows_caster_sunvis,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_throwdirection,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_focus,					REALTIMELIGHTING);
	Cvar_Register (&r_shadows_res,						REALTIMELIGHTING);
	Cvar_Register (&r_shadows_slots,					REALTIMELIGHTING);
	Cvar_Register (&r_shadows_slots_hyst,				REALTIMELIGHTING);
	Cvar_Register (&r_shadows_slots_smooth,				REALTIMELIGHTING);
	Cvar_Register (&r_shadows_slots_margin,				REALTIMELIGHTING);
	Cvar_Register (&r_shadows_slots_pcf,				REALTIMELIGHTING);
	Cvar_Register (&r_shadows_slots_debug,				REALTIMELIGHTING);
	Cvar_Register (&r_shadows_cascades,					REALTIMELIGHTING);
	Cvar_Register (&r_shadows_cascade_dist,				REALTIMELIGHTING);
	Cvar_Register (&r_shadows_cascade_ratio,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_cascade_debug,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows,				REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_max,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_switch,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_cone,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_range,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_bias,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_sunvis,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_suntrace,		REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_suncast,		REALTIMELIGHTING);
	Cvar_Register (&r_shadows_sunfade,					REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_debug,			REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_showatlas,		REALTIMELIGHTING);
	Cvar_Register (&r_shadows_sunbrightness,				REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_sunmask,		REALTIMELIGHTING);
	Cvar_Register (&r_shadows_propshadows_worldmask,		REALTIMELIGHTING);
	Cvar_Register (&r_shadow_shadowmapping_depthbits,	REALTIMELIGHTING);
#endif
}
