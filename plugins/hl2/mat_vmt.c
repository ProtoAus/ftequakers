#include "../plugin.h"
#include "shader.h"

//FTESurf: owned by mod_vbsp.c, which registers it alongside the other map
//cvars.  Both files decide "is this face a tool brush" and they must agree.
extern cvar_t *hl2_hidetools;

/*
FTESurf Build 10 -- what a Source water or glass surface actually costs.

A "Water" VMT was compiled into a shader carrying `map $refraction` and
`map $reflection`.  Those are not textures; they are RENDER TARGETS, and the
backend fills them by rendering the world again.  gl_backend.c:5732 and :5781
test SHADER_HASREFLECT / SHADER_HASREFRACT and call GLR_DrawPortal for each,
once per batch of that shader -- so ONE water plane in view is up to THREE
scene renders instead of one, and two visible planes on different materials is
five.  "Refract" (Source's glass) is cheaper but not free: `map $currentrender`
is a full framebuffer copy per batch.

That is the whole of "water and windows are costing me frames", and none of it
was reachable from a cvar.  Now:

  hl2_water    0  flat translucent -- a $fogcolor sheet, no portal at all
               1  refraction only, no reflection
               2  refraction + reflection, which is what shipped
               3  DITHERED FLAT  (DEFAULT since build 12)

  hl2_refract  0  glass draws as plain translucency, no framebuffer copy
               1  Source's refraction
               2  DITHERED FLAT  (DEFAULT since build 12)

  hl2_translucent 0  $translucent world materials are drawn OPAQUE
                  1  as authored  (DEFAULT)

  hl2_imposter 0  WindowImposter draws nodraw -- whatever is behind it shows
                  through, which on a fake-sky slab is the map's REAL sky
               1  Source's behaviour: the $envmap cubemap, sampled by view
                  direction, opaque  (DEFAULT)  -- FTESurf Patch 263

BUILD 12: WHY DITHER IS THE CHEAP ONE, not a compromise.  Mode 0 removed the
render target but left a BLENDED surface, and a blend is not free: it cannot
write depth, so it has to be sorted back-to-front and everything behind it is
drawn whether you can see it or not.  An ordered dither is opaque geometry with
a discard -- no blend, no sort, no framebuffer copy, and it WRITES DEPTH, so the
geometry behind is z-rejected instead of overdrawn.  Strictly cheaper than the
mode it replaces as the default.  The colour and the coverage come from the
material's own keys ($fogcolor for water, $refracttint/$basetexture and $alpha
for glass), fed to glsl/vmt/flatdither.glsl through the pass's rgbgen/alphagen
const, so each material still looks like itself and no shader permutation is
spent on carrying them.

The numbering is ADDITIVE on purpose: 0/1/2 keep the meanings they shipped with,
so no config written before build 12 changes behaviour when it is read after.

All three are MAPLATCH: the shader is compiled once when the material is first
loaded, so changing any of them needs a map reload to mean anything -- and
MAPLATCH is the flag that actually delivers one.  (They were RENDERERLATCH until
build 11, which needs a vid_reload, so setting them and reloading the map did
nothing at all.  See the note beside MAPOPTIONS in mod_vbsp.c.)
*/
cvar_t *hl2_water;
cvar_t *hl2_refract;
cvar_t *hl2_translucent;
cvar_t *hl2_dither_alpha;	//FTESurf Build 13
cvar_t *hl2_dither_force;	//FTESurf Patch 174, see VMT_DitherScale
cvar_t *hl2_bumpmap;
cvar_t *hl2_envmap;
//FTESurf Patch 268 B: the literal $envmap "env_cubemap" -- emit the $envcubemap
//sentinel so the engine's REFLECTCUBEMASK gate fires and T_GEN_REFLECTCUBE falls
//through to the batch's nearest baked cubemap; and the Source-parity bits for the
//envmap term itself.  Registered in mod_vbsp.c beside hl2_bumpmap/hl2_envmap.
cvar_t *hl2_envcubemap;
cvar_t *hl2_envmap_source;
//FTESurf Patch 268 C: #BUMPCUBE (per-pixel ambient-cube relief on a bumped VertexLitGeneric)
//and #BUMPGREENFIX (no green flip on that normal map).  Registered in mod_vbsp.c like the above.
cvar_t *hl2_cubelight;
cvar_t *hl2_bumpgreenfix;
cvar_t *hl2_animated;	//FTESurf Patch 195, the AnimatedTexture proxy
cvar_t *hl2_vertexcolor;	//FTESurf Patch 232, $vertexcolor/$vertexalpha on lit materials
cvar_t *hl2_decallit;	//FTESurf Patch 238, whether a $decal material may keep its lightmap
cvar_t *hl2_vmtkeys;	//FTESurf Patch 250, how loudly to report keys this file does not implement
cvar_t *hl2_emissive;	//FTESurf Patch 250, the $emissiveblend* scrolling self-illum pass
cvar_t *hl2_seamless;	//FTESurf Patch 251, $seamless_scale triplanar projection
cvar_t *hl2_bumpmap2;	//FTESurf Patch 251, WorldVertexTransition's second normal map
cvar_t *hl2_imposter;	//FTESurf Patch 263, WindowImposter -- Source's fake-second-skybox shader
cvar_t *hl2_twotexture;	//FTESurf: UnlitTwoTexture's second layer + its proxy chain
cvar_t *hl2_twoframes;	//FTESurf Patch 286: ...and its distance-driven flipbook
cvar_t *hl2_additivefog;	//FTESurf Patch 300, additive materials onto a program so they fog toward black

//How many of each this map actually loaded, for the census mod_vbsp.c prints.
int vmt_stat_water, vmt_stat_refract, vmt_stat_translucent;
int vmt_stat_imposter;	//FTESurf Patch 263
int vmt_stat_hdrenvmap;	//FTESurf Patch 264, $envmap resolved via the .hdr.vtf fallback
int vmt_stat_ignorez;	//FTESurf Patch 290, $ignorez materials that could not be honoured
int vmt_stat_addprog;	//FTESurf Patch 300, additive materials moved from a pass onto vmt/unlit
int vmt_stat_addpass;	//...and those that had to stay on a pass, so the census says which
/*
FTESurf Patch 263: materials whose SHADER CLASS this file does not implement.

Kept beside the others because it is the one census that would have found the
bug that prompted it.  Shader_GenerateFromVMT dispatches on the first token of
the VMT through an if/else-if chain, and until now the terminal else was silent
-- no warning, no count -- so an unimplemented class was indistinguishable from
an implemented one that happened to look wrong.

Censused across the library's 1310 embedded pakfiles, 171,076 VMTs: 790
materials (0.46%) name one of 39 classes this file does not handle, and they
touch 209 maps -- ONE MAP IN SIX.  The two biggest by FILE COUNT are SpriteCard
(335 across 53 maps) and WindowImposter (155 across 59); then ShatteredGlass 58,
Grass 30, Wireframe 28, ParticleSphere 26, Cable 23, Eyes 16, Refract_DX90 15,
PBR 14.

THOSE TWO ARE NOT COMPARABLE PRIZES, and this comment used to imply they were.
A .vmt packed in a pakfile is not a .vmt that is drawn.  Measured on the same
1310 maps (FTESurf Patch 264):

  WindowImposter   16,042 world faces across 60 maps -- which is why Patch 263
                   was worth doing.
  SpriteCard       ZERO.  Not one face in 23,735,419.  Zero of 9,702 baked
                   overlays, zero of 58,137 model skins parsed out of studiohdr
                   texture tables, zero entity material keys, zero
                   $bottommaterial.  Exactly ONE of the 334 reaches a texdata
                   string table -- strafe/cig_smoke on kz_bhop_yonkoma -- and no
                   face uses it, so it is registered here (a shader is generated
                   per TEXINFO, not per face: mod_vbsp.c VBSP_GenerateMaterials
                   loops mod->numtextures) and then draws nothing.

SpriteCard is not a surface shader.  It is the vertex-format contract between
Source's CPU particle system and the GPU: its vertex shader reads sheet bounding
UVs for two cross-faded frames, a blend factor, rotation, radius and a corner id,
all written per particle from a CSheet built out of the VTF's sheet resource --
which img_vtf.c does not read, and which is keyed 0x10, not an 'SHT' fourCC.  No
brush face carries any of that.  257 of the 334 are named only inside the maps'
own .pcf files and nothing in this plugin or the engine reads .pcf.

So implementing the class alone would change no pixel.  The missing piece is a
.pcf reader and info_particle_system (4,105 of those entities across 52 of the
53 maps), and that is a particle-system project, not a shader-class one.  Do not
pick the largest number in this list without measuring what it is on first.

Only WindowImposter is implemented here; the rest are counted so that the next
one to matter is visible before it is reported.

vmt_stat_unknown_name keeps the first few distinct class names, the way
vmt_stat_missing_name already does for unresolved materials -- a bare count says
something is wrong, a name says what to go and read.
*/
int vmt_stat_unknown;
char vmt_stat_unknown_name[4][64];
int vmt_stat_animated;	//FTESurf Patch 195
int vmt_stat_animunlit;	//FTESurf Patch 253: of those, how many are UnlitGeneric
int vmt_stat_emissive;	//FTESurf Patch 250
int vmt_stat_seamless, vmt_stat_bumpmap2;	//FTESurf Patch 251
/*
FTESurf: UnlitTwoTexture and its proxies, counted separately.

Three numbers rather than one, because they answer three different questions
and a single "twotexture N" would hide the interesting failure.  `twotexture`
is how many materials took the two-layer path at all; `proxfade` and
`proxframe` are how many of those had a proxy chain this could actually
resolve.  A map where twotexture is 4 and proxfade is 0 means the graph walk
declined every chain, which is a bug in the walk -- and it is invisible from
the screen, because the shield still draws, just without its distance
behaviour.
*/
int vmt_stat_twotexture, vmt_stat_proxfade, vmt_stat_proxframe;
/*
FTESurf Patch 286: of the ones with a distance-driven frame, how many could
actually SAMPLE it, and how many were declined.

`proxframe` counts a resolved proxy chain, which was already true before any
frame was ever drawn -- Patch 279 resolved the coefficient and then threw it
away.  So it cannot answer "is the flipbook on screen".  These two can:
`twoframes` is the array path taken, `twoframesdecl` is a material that has a
frame ramp and two DIFFERENT textures, which needs a second array sampler and
is deliberately not attempted (see the arm).  On surf_tensor2 the numbers are
1 and 1 -- comshieldwall (42 faces) and comshieldwall2 (2 faces).
*/
int vmt_stat_twoframes, vmt_stat_twoframesdecl;
int vmt_stat_spritecard;	//FTESurf: Source's particle shader, see the arm
/*
FTESurf Patch 251: the permutation string of the last Water material generated.

shader_here cannot answer "did the water keys reach the program" from a normal
vantage: the water surface does not stop CL_TraceShaderUnderCrosshair, so a probe
aimed down at a pool reports the $bottommaterial face BEHIND it -- measured, on
both surf_garden and surf_boreas, as juxxy/water_canals03_beneath#warp with
prog=-.  Standing under it reports the same thing for the opposite reason.

So the generator says so itself, once per map, beside the water census that
already prints the mode.  Cheaper than a probe and it cannot be aimed wrong.
*/
/*
256, not 192, and the first run of this diagnostic is why: surf_boreas' water
writes all six keys and came out at

  #HQWATER#STRENGTH_REFR=0.500000#STRENGTH_REFL=1.000000
  #TINT_REFR=0.862745,0.949020,0.937255#TINT_REFL=1.000000,1.000000,1.000000
  #FOGTINT=0.039216,0.152941,0.262745#TXSCALE1=1.000000#TXSCALE2=1.000000

which is 199 characters and was silently truncated to "#TXSCALE2=" with no value.
That is the worst case for a Water material -- all six keys, three of them vec3 --
and it fits progargs[256] with 57 bytes to spare, so the SHADER is not truncated;
only this copy of it was.  A diagnostic that lies about the thing it exists to
show is worse than no diagnostic, hence the exact size and this note.
*/
char vmt_stat_waterargs[256];

/*
FTESurf Patch 254: the map's underwater fog, for the engine's FOGTYPE_WATER slot.

Source takes the fog you see while SUBMERGED from the water material's own
$fogcolor/$fogstart/$fogend -- not from env_fog_controller -- and this parser has
been reading $fogcolor and $fogend all along, spending them only on the hl2_water
dither coverage and the #FOGTINT shader arg.  Published to the gamecode through
the same cvar channel hl2_unresolved already uses (mod_vbsp.c), because there is
no other plugin->QC route and writing a second VMT parser in QC to re-read data
this one already has correct would be absurd.

FIRST water material wins, deliberately, unlike vmt_stat_waterargs beside it,
which is last-wins because it is only a census line.  This one drives what the
screen looks like underwater, so it wants to be stable across a load rather than
whichever material happened to be parsed last.
*/
char  vmt_stat_waterfog[64];		//"r g b", 0..1
float vmt_stat_waterfogstart, vmt_stat_waterfogend;
int   vmt_stat_waterfog_set;

/*
And how many would not resolve at all -- see Shader_LoadVMT.

FTESurf Patch 174: the NAMES, not just the first one.

Build 11 kept one name because one name is enough to identify the asset pack,
which is what the message is for.  It is not enough for the other question the
same message raises -- "which of the things I am looking at are broken" -- and
answering that by hand meant standing in front of each surface and asking the
engine what it was.  A map that fails to resolve thirteen materials can just say
which thirteen; the cap exists so a catastrophically unmounted map cannot spam
the console with four hundred lines, and the tail is counted rather than listed.

FTESurf Patch 231: STORE 256, SHOW 24.

Those were one number, and the console's need was the one that set it -- so the
tail was not merely unlisted, it was never recorded, and there was no way to ask
for it later.  That is exactly the shape of the question surf_demise poses: the
HUD says 41 unresolved, tools/mapdeps.py --map surf_demise resolves everything
but one, and adjudicating between them needs the other forty NAMED.  A capped
console line cannot do it and neither can a count.

So the array holds 256 (32KB of static across both lists, which is nothing) and
`hl2_missing` prints all of them on demand; the warning still shows 24, because
its job is to be read, not to be complete.
*/
#define VMT_MISSING_LIST 256
int vmt_stat_missing;
char vmt_stat_missing_name[VMT_MISSING_LIST][MAX_QPATH];
//FTESurf Patch 181: found on disk, but the parse gave up.  Kept apart from the
//missing list because the two have opposite fixes -- one wants an asset pack
//mounted, the other wants this plugin to understand a key it does not.
int vmt_stat_unparsed;
char vmt_stat_unparsed_name[VMT_MISSING_LIST][MAX_QPATH];

/*
FTESurf Patch 231: has this name already been counted?

"%i material(s) did not resolve" was never a count of MATERIALS.  It counted failed
LOOKUPS, and the two are wildly different numbers because a failed shader is not
cached: R_LoadShader frees it and returns NULL when the generator path is not taken
(gl_shader.c:8570-8578), so every model that references a missing skin re-attempts it
from scratch.  Measured on surf_demise: `models/elly/bones/Bone_Pearl.vmt` was counted
81 times and `models/props_forest/firbranch02_singlemat_snowy.vmt` 36, turning about
two dozen genuinely absent materials into a reported 206.

That number is the one the HUD banner shows and the one a report quotes, and being
off by 8x in the alarming direction is how "surf_demise is missing 41 materials"
becomes a hunt for an asset pack that was never the problem.  So the counter now
counts distinct names, which is what its own message claims.

Linear, but only ever on the failure path and only over what has been recorded, so
the worst case is VMT_MISSING_LIST compares for a map that is already broken.  Past
the cap the list is full, dedupe stops, and the count starts inflating again -- fine:
a map with 256 distinct missing materials does not need a precise total.
*/
static qboolean VMT_CensusSeen(char names[][MAX_QPATH], int count, const char *name)
{
	int i;
	if (count > VMT_MISSING_LIST)
		count = VMT_MISSING_LIST;
	for (i = 0; i < count; i++)
		if (!Q_strcasecmp(names[i], name))
			return true;
	return false;
}

/*
FTESurf Patch 250: THE KEYS THIS FILE DOES NOT IMPLEMENT, COUNTED ONCE EACH.

The old behaviour was Con_DPrintf per key per parse, and there are four separate
multipliers on that: the plugin's Con_DPrintf gates on `developer` being merely
non-zero, a replace/insert block is walked twice (see the bottom of
VMT_ParseBlock), an include chain re-enters the parser per file, and a material
that fails to resolve is re-attempted per REFERENCE rather than cached -- the
essay above measured one models/props_forest VMT at 36 lookups.  Sixteen
$treeSway keys on one material is 576 lines before anything else has spoken.

So keys are recorded, not printed, and split by whether Source itself would
recognise them (VMT_IsKnownSourceParam).  That split is the useful one: the
first list is work we could do, the second is mostly proxy variables and typos
we could not act on if we wanted to.  `vmt_keys` prints both with counts, which
is the durable version of the offline census that produced this patch -- the
next person does not have to decompress 90 pakfiles to ask the same question.

Per map, like the rest of the stats: cleared by Mat_VMT_ResetStats.  128 each is
well past the 45 distinct keys the three worst maps in the library manage
between them, and going over only costs precision in a summary line.
*/
#define VMT_KEY_LIST 128
typedef struct
{
	char name[64];
	char example[MAX_QPATH];	//the first material that used it -- where to go and look
	unsigned int uses;
} vmtkey_t;
static vmtkey_t vmt_key_param[VMT_KEY_LIST];	//real Source shader params, unimplemented here
static int vmt_key_param_n, vmt_key_param_over;
static vmtkey_t vmt_key_alien[VMT_KEY_LIST];	//not Source params at all: proxy vars, extensions, typos
static int vmt_key_alien_n, vmt_key_alien_over;

static void VMT_RecordKey(vmtkey_t *list, int *count, int *over, const char *key, const char *fname, const char *what)
{
	int i;
	int mode = hl2_vmtkeys ? hl2_vmtkeys->ival : 0;
	qboolean isnew = true;

	for (i = 0; i < *count; i++)
		if (!Q_strcasecmp(list[i].name, key))
		{
			list[i].uses++;
			isnew = false;
			break;
		}
	if (isnew)
	{
		if (*count >= VMT_KEY_LIST)
			(*over)++;
		else
		{
			Q_strlcpy(list[*count].name, key, sizeof(list[0].name));
			Q_strlcpy(list[*count].example, fname, sizeof(list[0].example));
			list[*count].uses = 1;
			(*count)++;
		}
	}
	if (mode >= 2 || (mode == 1 && isnew))
		Con_DPrintf("%s: %s field \"%s\"\n", fname, what, key);
}

/*
FTESurf Patch 233: WHICH WATER A MISSING BOTTOM MATERIAL BELONGS TO.

A Source water VMT may name a $bottommaterial: the material drawn on the UNDERSIDE
of the water.  VBSP does not treat that as a property of the water -- it writes the
string into the map's texdata string table as an ordinary world material and gives
the underwater faces a texinfo pointing at it.  So the underside is normal geometry,
and it goes missing like any other material when the mapper does not ship it.

Two of the three maps this was reported on are exactly that, and both are DRAWN:

  surf_aesthetic  nature/water_epic_clear.vmt  $bottommaterial "water/clear_beneath"
                  325 faces, SURF_WARP, no SURF_NODRAW.
  surf_demise     liquidpack/water/unique/water_tar.vmt
                  $bottommaterial "liquidpack/water/unique/water_tar_beneath.vmf"
                  7 faces.  The .vmf is a mapper typo for .vmt, baked into the VMT.

Neither bottom material exists in the map's own pakfile, in CS:S, CS:GO, TF2, HL2 or
Momentum, in either spelling.  Nothing can be mounted to bring them back -- but the
material that DEFINES them is right there in the map, and the engine was reading it
and throwing the link away.

WHY A TABLE AND NOT A NAME RULE.  Stripping "_beneath" finds surf_demise's parent
(water_tar_beneath -> water_tar) and completely misses surf_aesthetic's, whose parent
is nature/water_epic_clear -- a different directory AND a different stem.  There is no
lexical relationship to exploit; the link only exists inside the VMT, so that is where
it has to be taken from.

Normalised on both sides because the two real values disagree about spelling: one
carries ".vmf", the other carries no extension at all, and a texdata name may or may
not have the "materials/" prefix.  Extension tolerance here is load-bearing, not
defensive.

Per map: cleared by Mat_VMT_ResetStats, which Mod_LoadVBSP calls on every load.  32 is
generous -- surf_demise has one $bottommaterial among 171 VMTs, surf_aesthetic one
among 44 -- and overflowing costs a fallback, not a fault.
*/
#define VMT_BOTTOM_MAX 32
static struct
{
	char bottom[MAX_QPATH];	//normalised: no "materials/" prefix, no extension
	char parent[MAX_QPATH];	//the water material to draw instead, as Shader_LoadVMT saw it
} vmt_bottom[VMT_BOTTOM_MAX];
static int vmt_bottom_num;

//"materials/liquidpack/water/unique/water_tar_beneath.vmf" -> "liquidpack/water/unique/water_tar_beneath"
static void VMT_BottomNormalise(const char *in, char *out, size_t outsize)
{
	char *c, *sl, *dot;
	if (!Q_strncasecmp(in, "materials/", 10))
		in += 10;
	else if (!Q_strncasecmp(in, "materials\\", 10))
		in += 10;
	Q_strlcpy(out, in, outsize);
	for (c = out; *c; c++)
		if (*c == '\\')
			*c = '/';
	sl = strrchr(out, '/');
	dot = strrchr(sl?sl:out, '.');
	if (dot)
		*dot = 0;
}

//Called from Shader_LoadVMT for every VMT that LOADED and named a $bottommaterial.
static void VMT_BottomRecord(const char *parent, const char *bottom)
{
	char norm[MAX_QPATH];
	int i;

	if (!*bottom)
		return;
	VMT_BottomNormalise(bottom, norm, sizeof(norm));
	if (!*norm)
		return;

	for (i = 0; i < vmt_bottom_num; i++)
		if (!Q_strcasecmp(vmt_bottom[i].bottom, norm))
			return;	//first water material to claim it wins; two claiming one bottom is a map fault, not ours to arbitrate
	if (vmt_bottom_num >= VMT_BOTTOM_MAX)
		return;
	Q_strlcpy(vmt_bottom[vmt_bottom_num].bottom, norm, MAX_QPATH);
	Q_strlcpy(vmt_bottom[vmt_bottom_num].parent, parent, MAX_QPATH);
	vmt_bottom_num++;
}

//mod_vbsp.c asks this about every name that did not resolve.  NULL = not a bottom material.
const char *Mat_VMT_BottomParent(const char *name)
{
	char norm[MAX_QPATH];
	int i;

	VMT_BottomNormalise(name, norm, sizeof(norm));
	for (i = 0; i < vmt_bottom_num; i++)
		if (!Q_strcasecmp(vmt_bottom[i].bottom, norm))
			return vmt_bottom[i].parent;
	return NULL;
}

/*
FTESurf Patch 233: a name that was reported missing and then resolved after all.

The census fires from inside the shader loader, which cannot know that the caller is
about to find another way to draw the surface.  Rather than deferring the count (and
losing it for every caller that has no second chance), the one caller that DOES have
one takes its name back out.

Only decrements when the name is actually in the stored list: past VMT_MISSING_LIST
the count runs ahead of the names, and guessing which of the unstored ones this was
would make the total wrong in the other direction.
*/
qboolean Mat_VMT_CensusMissing(const char *name)
{
	return VMT_CensusSeen(vmt_stat_missing_name, vmt_stat_missing, name);
}

void Mat_VMT_CensusForget(const char *name)
{
	int i, n = vmt_stat_missing;
	if (n > VMT_MISSING_LIST)
		n = VMT_MISSING_LIST;
	for (i = 0; i < n; i++)
	{
		if (Q_strcasecmp(vmt_stat_missing_name[i], name))
			continue;
		if (i < n-1)	//i+1 must stay a real element: n can be VMT_MISSING_LIST
			memmove(vmt_stat_missing_name[i], vmt_stat_missing_name[i+1],
				sizeof(vmt_stat_missing_name[0]) * (n-1 - i));
		*vmt_stat_missing_name[n-1] = 0;
		vmt_stat_missing--;
		return;
	}
}

void Mat_VMT_ResetStats(void)
{
	vmt_stat_water = vmt_stat_refract = vmt_stat_translucent = 0;
	vmt_stat_animated = vmt_stat_animunlit = 0;
	vmt_stat_emissive = 0;
	vmt_stat_seamless = vmt_stat_bumpmap2 = 0;
	vmt_stat_twotexture = vmt_stat_proxfade = vmt_stat_proxframe = 0;	//FTESurf
	vmt_stat_twoframes = vmt_stat_twoframesdecl = 0;	//FTESurf Patch 286
	vmt_stat_spritecard = 0;	//FTESurf
	//FTESurf Patch 263
	vmt_stat_imposter = 0;
	vmt_stat_hdrenvmap = 0;	//FTESurf Patch 264
	vmt_stat_ignorez = 0;	//FTESurf Patch 290
	vmt_stat_addprog = vmt_stat_addpass = 0;	//FTESurf Patch 300
	vmt_stat_unknown = 0;
	*vmt_stat_unknown_name[0] = 0;
	*vmt_stat_waterargs = 0;
	*vmt_stat_waterfog = 0;	//FTESurf Patch 254
	vmt_stat_waterfogstart = vmt_stat_waterfogend = 0;
	vmt_stat_waterfog_set = 0;
	vmt_stat_missing = 0;
	*vmt_stat_missing_name[0] = 0;
	vmt_stat_unparsed = 0;
	*vmt_stat_unparsed_name[0] = 0;
	//FTESurf Patch 250: the key census is per-map like everything else here.
	vmt_key_param_n = vmt_key_param_over = 0;
	vmt_key_alien_n = vmt_key_alien_over = 0;
}

/*
FTESurf Patch 250: `vmt_keys` -- what this map asked for that we do not do.

Two lists, because they have different answers.  A key in the first is declared
by a real Source shader, so it is a feature with a specification we could go and
read; a key in the second is not, and is overwhelmingly a material-proxy result
variable (Source lets a mapper declare $anything at the top level for a proxy to
write to), with the occasional branch extension or typo -- $surcefaceprop and
$moidel are both in the shipped library.  Sorting the second list into "should
have been implemented" and "was never a thing" is not possible from the name.

Counts are uses, not materials, and the example is the first material seen with
that key: enough to go and open it, which is the next thing you want.
*/
void Mat_VMT_KeyCensus_f (void)
{
	int i;

	if (!vmt_key_param_n && !vmt_key_alien_n)
	{
		Con_Printf("vmt_keys: every .vmt key this map used is either implemented or deliberately ignored.\n");
		return;
	}

	if (vmt_key_param_n)
	{
		Con_Printf("^3Real Source shader parameters, not implemented here (%i distinct%s):\n",
			vmt_key_param_n, vmt_key_param_over ? ", list full -- more were seen" : "");
		for (i = 0; i < vmt_key_param_n; i++)
			Con_Printf("  %-34s %5u use(s)   e.g. %s\n",
				vmt_key_param[i].name, vmt_key_param[i].uses, vmt_key_param[i].example);
	}
	if (vmt_key_alien_n)
	{
		Con_Printf("^3Names Source does not declare either -- proxy variables, extensions, typos (%i distinct%s):\n",
			vmt_key_alien_n, vmt_key_alien_over ? ", list full -- more were seen" : "");
		for (i = 0; i < vmt_key_alien_n; i++)
			Con_Printf("  %-34s %5u use(s)   e.g. %s\n",
				vmt_key_alien[i].name, vmt_key_alien[i].uses, vmt_key_alien[i].example);
	}
	vmt_bottom_num = 0;	//FTESurf Patch 233: the link table is a fact about ONE map
}

/*
Source's $fogcolor, as three 0..1 floats for `rgbgen const`.

Hammer writes it either way round: "[.05 .09 .12]" is already 0..1 and
"{40 60 80}" is 0..255.  The brackets ARE the type tag, which is why this
reads them rather than guessing from the magnitudes -- "{1 1 1}" is a
legitimate near-black under the integer convention and pure white under the
float one, and no heuristic can tell those apart.

An absent or unparseable key falls back to a dark blue-grey, which is what
water looks like from above when it is not reflecting anything.
*/
/*
FTESurf Patch 188: the same convention reads $color and $color2, so it is one
function now instead of being inlined in the water path.

NOT CLAMPED, deliberately.  "{400 380 370}" and "{450 350 250}" are real values
on surf_unrequited and they are meant to blow out -- clamping them to white
would throw away the only thing distinguishing a lamp from a wall on a map whose
entire palette is $color2 over a white texture.
*/
static qboolean VMT_ParseColour(const char *in, float *c)
{
	float scale = 1;
	const char *s = in;

	if (!s || !*s)
		return false;
	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '{')
		scale = 1/255.0f;
	if (*s == '{' || *s == '[')
		s++;
	if (sscanf(s, "%f %f %f", &c[0], &c[1], &c[2]) != 3)
		return false;
	c[0] *= scale; c[1] *= scale; c[2] *= scale;
	return true;
}

static void VMT_FogColorString(const char *in, char *out, size_t outsize)
{
	float c[3];
	if (!VMT_ParseColour(in, c))
	{	//water from above, when it is not reflecting anything
		c[0] = 0.06f; c[1] = 0.11f; c[2] = 0.15f;
	}
	Q_snprintfz(out, outsize, "%f %f %f", c[0], c[1], c[2]);
}

/*
FTESurf build 12: how opaque a Water material's dither should be.

$fogend is how far you can see INTO the water before it goes solid, which is the
closest thing a Water VMT carries to an opacity -- Source uses it for exactly
that, just continuously with depth rather than as one coverage number.  Clear
water (a long fogend) dithers sparse; murky water dithers dense.

The mapping below is a PRESENTATION CHOICE, not physics: a dither has one
coverage for the whole surface and the thing it is standing in for varies with
view depth, so no formula here can be "correct".  It is monotonic, it is bounded
so nothing ever becomes invisible or fully solid, and 0.7 -- what the flat
translucent mode used -- is what a material with no $fogend gets.
*/
/*
FTESurf Build 13: hl2_dither_alpha scales all of this.

Build 12 shipped these coverages raw and they read as a flat painted sheet
rather than as something you look through -- correct as a mapping, too strong as
a picture.  The multiplier is applied at the END, after the per-material
mapping, so the relative ordering the $fogend curve produces is preserved and
only the overall strength moves.  Clamped so nothing becomes invisible (which
would look like a hole) or fully solid (which would look like a wall).
*/
/*
FTESurf Patch 174: hl2_dither_force overrides the whole derivation.

hl2_dither_alpha is a MULTIPLIER, and a multiplier cannot rescue a pane the
material itself authored as nearly invisible.  surf_spectra has windows whose
$alpha is low enough that they read as an empty frame -- "too much alpha, I can
barely see it" -- and the per-material path is exactly what puts them there:
the Refract fallback is 0.25, water's $fogend curve bottoms out at 0.35, and
they were then multiplied down again.  Scaling that by anything still leaves the
faintest surfaces the faintest, which is the complaint.

So this is a FLOOR-AND-CEILING REPLACEMENT, not another factor: at any value
above 0 every dithered surface in the map gets exactly that coverage, whatever
its VMT says.  It is the one setting that can be reasoned about without knowing
what any individual material author wrote, which is the point of having it.

0 (the default) keeps the per-material derivation and hl2_dither_alpha's scale,
so nothing changes for anyone who has not asked for this.
*/
static float VMT_DitherScale(float a)
{
	if (hl2_dither_force && hl2_dither_force->value > 0)
		a = hl2_dither_force->value;
	else if (hl2_dither_alpha)
		a *= hl2_dither_alpha->value;
	//Never invisible (which reads as a hole) and never fully solid (which reads
	//as a wall).  0.98 clears the 4x4 Bayer's highest threshold, 15.5/16, so the
	//top of the range is opaque in practice without ever being opaque in fact.
	if (a < 0.03f) a = 0.03f;
	if (a > 0.98f) a = 0.98f;
	return a;
}

static float VMT_WaterDitherAlpha(float fogend)
{
	float a;
	if (fogend <= 0)
		a = 0.7f;
	else
	{
		a = 1.0f - (fogend / 1000.0f);
		if (a < 0.35f) a = 0.35f;
		if (a > 0.90f) a = 0.90f;
	}
	return VMT_DitherScale(a);
}

static plugfsfuncs_t *fsfuncs;

typedef struct shaderparsestate_s parsestate_t;

typedef struct
{
	char **savefile;
	char *sourcefile;
	char type[MAX_QPATH];
	char normalmap[MAX_QPATH];
	struct
	{
		char name[MAX_QPATH];
	} tex[5];
	char hdrcompressedtex[MAX_QPATH];	//nettest: $hdrcompressedtexture — compressed-HDR sky face (RGBS in BGRA8), decoded via the $hdr: prefix
	char hdrbasetex[MAX_QPATH];			//nettest: $hdrbasetexture — uncompressed HDR sky face (RGBA16161616F), native float
	char fullbrightmap[MAX_QPATH];
	char envmap[MAX_QPATH];
	char envmapmask[MAX_QPATH];
	char dudvmap[MAX_QPATH];
	char refracttinttexture[MAX_QPATH];
	/*
	FTESurf Patch 187: $blendmodulatetexture, which was parsed and thrown away.

	It is not a colour.  Its GREEN channel is WHERE the two base textures cross
	over and its RED channel is how WIDE that crossover is, so Source shapes the
	transition with smoothstep(g-r, g+r, vertexalpha) instead of fading linearly.
	Discarding it is why a displacement blend read as a flat gradient rather than
	as gravel poking through tarmac.  722 of the library's 1337
	WorldVertexTransition materials write it.
	*/
	char blendmod[MAX_QPATH];
	/*
	FTESurf Patch 251: $bumpmap2, WorldVertexTransition's SECOND normal map.

	Blended by the same factor as the two base textures, including the
	$blendmodulatetexture reshaping (lightmappedgeneric_ps2_3_x.h:378-382).  The
	old note here said "there's also bumpmap2, but rtlight glsl doesn't respect it
	anyway", which was true of the RTLIGHT path and is beside the point for the
	world one -- transition.glsl reads the normal for cubemap warping, and that
	reflection is now blended instead of taking the first map everywhere.

	HOW MUCH THIS IS WORTH, measured before building it rather than after.  Across
	all 1310 maps: 481 materials in 175 maps carry $bumpmap2, and 130 of them
	(27%) also carry $envmap.  Only those 130 look any different today, because
	the normal is consulted only inside #ifdef REFLECTCUBEMASK; the other 351 are
	correct and invisible until this shader lights from the normal for something
	other than a reflection.  Stated here so nobody later measures it and concludes
	the feature is broken.

	(A 200-map sample first put the visible fraction at 6 of 42.  It was not
	representative -- the maps that lean on $bumpmap2 are exactly the ones that
	also bake cubemaps.  surf_demise is 6 of 6.)
	*/
	char bumpmap2[MAX_QPATH];
	/*
	FTESurf Patch 251: $seamless_scale -- Source's triplanar projection.  The
	material stops using its UVs and projects the base map three times down the
	world axes, blended by normal*normal, so one texture can be draped over a
	displacement with no seam.  Kept as a float plus a "was it written" flag for
	the same reason $envmaptint needed one (Patch 237): 0 is a legal value the
	mapper can write and it means something different from silence.

	367 materials across 89 of the library's 1310 maps.  In a 200-map sample the
	split was 11 LightmappedGeneric / 12 WorldVertexTransition / 3 VertexlitGeneric
	-- roughly half on each world type, which is why BOTH world shaders get the
	projection and not just the one the plan named.  Values run 0.0005 to 0.005,
	i.e. one texture repeat every 200 to 2000 world units.
	*/
	float seamless;
	qboolean seamless_set;
	/*
	FTESurf Patch 250: the $emissiveblend* family -- Source's scrolling
	self-illumination pass (emissive_scroll_blended_pass_helper.cpp).

	It is an EXTRA additive pass in Source, drawn after the material's own, and
	its whole content is six lines of pixel shader
	(emissive_scroll_blended_pass_ps20b.fxc:31-48):

		base = tex2D(base, uv);  flow = tex2D(flow, uv);
		emis = tex2D(selfillum, flow.xy + scroll*time);
		rgb  = base.rgb * emis.rgb * tint * strength;  a = 0;

	with EnableAlphaBlending(SHADER_BLEND_ONE, SHADER_BLEND_ONE) at helper:174.

	Every one of the six materials that carries it on surf_demise has its
	$basetexture COMMENTED OUT and sets $additive 1, so the base pass contributes
	nothing and the emissive pass IS the material.  That is what makes this one
	arm rather than a second pass bolted onto another: there is nothing to bolt
	it to.  A material that had both would need the pass; none in the library
	does, and the census in vmt_keys is how the next one would be noticed.
	*/
	char emisbase[MAX_QPATH];	//$emissiveblendbasetexture -> s_diffuse
	char emisflow[MAX_QPATH];	//$emissiveblendflowtexture -> s_lower, a dudv map that displaces the lookup
	char emistex[MAX_QPATH];	//$emissiveblendtexture -> s_fullbright, the thing that scrolls
	float emistint[3];
	float emisstrength;
	float emisscroll[2];
	qboolean emisenabled, emistint_set, emisstrength_set, emisscroll_set;
	/*
	FTESurf Patch 189: the TextureScroll proxy.

	Proxies were skipped wholesale, and this one is the single most widespread
	Source material feature FTE does not implement: 4,697 materials across 1,007
	maps scroll a texture, which is most of what "wavy" means on a smoke sheet,
	a waterfall or a conveyor.  It is also nearly free -- defaultwall.glsl:83
	already does `tc.st += e_time * vec2(FLOWV)` from a define.

	scrollvar says WHICH transform the proxy drives; we only model
	$basetextureTransform, so a proxy pointed at the bump transform is read and
	then declined rather than silently applied to the wrong map.
	*/
	float scrollrate;
	float scrollangle;
	char scrollvar[64];
	/*
	FTESurf Patch 195: the AnimatedTexture proxy -- the other half of "wavy".

	3,342 materials across 741 maps flip a texture through the frames of a
	single VTF.  img_vtf.c had `frames = 1;//vtf->numframes;`, so every one of
	them has always been frame 0; surf_demise's smoke drifts (Patch 189) and does
	not cycle.  animatedtexturevar names the texture var to animate, the same way
	texturescrollvar names a transform, and is declined the same way when it
	points at something other than the base texture.
	*/
	float animrate;
	char animvar[64];
	char animpass;	//the program went into a pass, so progblendfunc must not also be emitted
	/*
	FTESurf: the SECOND layer's scroll, and the proxy graph that animates a
	UnlitTwoTexture.

	Patch 189 modelled one TextureScroll and only when it drove
	$basetextureTransform, because that is the one a lightmapped world surface
	can use.  effects/combineshield/comshieldwall.vmt drives
	$texture2Transform instead -- the second layer slides diagonally across a
	stationary first one, and multiplying the two is what makes the pattern
	move.  Declining it left the shield as a still image, which is exactly the
	report ("a solid glow web").  So the two transforms now have separate slots
	rather than one that is taken or declined.

	The prox* fields are the compiled form of the material's proxy chain; see
	VMT_ResolveProxies and the essay on the UnlitTwoTexture arm.  They are
	COEFFICIENTS, not values: everything Source's chain computes here is a
	function of eye distance and time, so the shader evaluates it per pixel and
	nothing has to be updated per frame.
	*/
	float tex2scrollrate;
	float tex2scrollangle;
	char  tex2scrollvar[64];

	float proxalpha;	//alpha falls by this much per world unit of eye distance
	float proxalphabase;	//...starting from this, before the fade
	float proxframe;	//frame index rises by this much per world unit...
	float proxframebias;	//...less this
	float proxframemin, proxframemax;
	float flickeramp, flickerperiod;	//the Sine proxy on $alpha
	char fogcolor[MAX_QPATH];	//FTESurf: what the water looks like at hl2_water 0
	char color[MAX_QPATH];
	char color2[MAX_QPATH];		//FTESurf Patch 188: $color2, which wins over $color where both are written
	char envfrombase;
	char envfromnorm;
	char halflambert;

	float envmaptint_r;
	float envmaptint_g;
	float envmaptint_b;
	float envmapsat_r;
	float envmapsat_g;
	float envmapsat_b;
	qboolean envmaptint_set;	//Patch 237: the key was PRESENT, as distinct from non-zero
	qboolean envmapsat_set;
	//FTESurf Patch 268 B: $envmapcontrast, which was recognised-and-dropped.  Same
	//_set rule as the two above -- 0 is a legal value ("no contrast") and is not the
	//same claim as silence, which the shader's own #ifndef default answers.
	float envmapcontrast;
	qboolean envmapcontrast_set;
	float alphatestref;
	float alphaval;		//FTESurf build 12: $alpha, 0 = not specified. The dither pass IS a pass, so a fractional one finally has somewhere to live.
	float fogend;		//FTESurf build 12: $fogend, 0 = not specified. How far you can see INTO the water, which is the closest thing a Water VMT has to an opacity.
	float fogstart;		//FTESurf Patch 254: $fogstart, parsed at last. Was recognised-and-discarded; it is the near end of the UNDERWATER fog, which the engine's FOGTYPE_WATER slot wants.
	/*
	FTESurf Patch 251: the Water keys that vmt/water.glsl already had #defines for
	and was never handed.  Every one of these was an explicit "recognised but
	ignored" arm; the shader has honoured STRENGTH_REFR/REFL, TINT_REFR/REFL,
	FOGTINT and TXSCALE1/2 since it was written, and mat_vmt.c only ever emitted
	#LQWATER or #HQWATER, so every water material in the library drew with the
	shader's built-in defaults regardless of what its author wrote.

	Counted over the packed VMTs of all 1310 maps: 679 Water materials in 256 maps,
	of which $fogcolor 679, $refractamount 677, $scale 639, $reflectamount 600,
	$refracttint 540, $reflecttint 527.  Practically every water material in the
	library writes keys that were being thrown away -- which makes this the widest
	of the three features in this patch by a long way, and the plan's estimate of
	"$waterdepth, 34 of 90 maps" both the wrong key and the wrong number
	($waterdepth itself turns up on ONE material in the entire library).

	Each carries a "_set" flag for the Patch 237 reason: 0 is a legal value with a
	meaning ("no distortion", "black") that is not the same as silence, and the
	shader's own #ifndef default is what silence should get.
	*/
	float refractamount, reflectamount;
	qboolean refractamount_set, reflectamount_set;
	float refracttint[3], reflecttint[3];
	qboolean refracttint_set, reflecttint_set;
	float fogcolor_f[3];
	qboolean fogcolor_set;
	float waterscale;		//$scale, the normal-map tiling
	qboolean waterscale_set;
	char *blendfunc;
	qboolean alphatest;
	qboolean culldisable;
	qboolean ignorez;
	//FTESurf Patch 268 B: this material asked for the map's nearest baked cubemap
	//($envmap "env_cubemap") on a class whose program can sample one, and
	//hl2_envcubemap is on -- so the shared tail emits reflectcube "$envcubemap".
	qboolean wantenvcube;
	char *replaceblock;

	char vertexcolor;
	char vertexalpha;
	char nodraw;
	char additive;
	char translucent;
	char selfillum;
	char decal;
	char nofog;
	char mod2x; /* modulate only */
	char water_cheap; /* water only */
	char water_expensive; /* water only */
	/*
	FTESurf Patch 181: did the FILE turn up, regardless of whether it parsed.

	VMT_ReadVMT returns false for two completely different things -- "no such
	file" and "found it, but VMT_ParseBlock gave up" (it ends `return !!line`).
	Shader_LoadVMT counted both as "did not resolve" and told the player to mount
	an asset pack, which for the second case is advice to fix a problem they do
	not have: surf_tensor2 reported two unresolved materials ONLY once CS:GO was
	mounted, because CS:GO's copy of concrete/tunnel_concretewall_01b is a richer
	material than the stub in the map's own pakfile and something in it stops the
	parse.  Mounting the pack made the message appear, so the message could not
	possibly be asking for the right thing.
	*/
	char filefound;
	char nobasetex;	//FTESurf Patch 190: the material named no $basetexture and there is nothing sane to guess at
	/*
	FTESurf Patch 233: $bottommaterial, the material Source draws on the UNDERSIDE
	of this water.  Kept rather than discarded because it is the only place the link
	between the two exists -- see VMT_BottomRecord.
	*/
	char bottommaterial[MAX_QPATH];

	/*
	FTESurf: the material's proxy graph, as parsed, before it is resolved into
	the prox* coefficients above.

	Kept as a flat list of "this result variable is produced by this operation
	on these inputs" rather than as a tree, because that is exactly the shape a
	Proxies block has -- a sequence of assignments to $-variables -- and because
	resolving it means walking BACKWARDS from $alpha and $frame, which a list
	does as well as a tree and with no allocation.

	The same list holds the material's NUMERIC $var declarations, as
	VMTPRX_CONST, because a proxy chain reads its constants out of them
	($frameminusten = $playerdistance2 - $ten, with "$ten" "24" declared at the
	top level) and a resolver that cannot see them would produce a coefficient
	with a term missing.

	FORTY IS NOT ARBITRARY.  The largest chain in the mounted library is
	comshieldwall.vmt's: nine proxies and thirteen numeric declarations.
	Overflow is counted and reported rather than silently truncated, because a
	truncated graph resolves to a plausible wrong coefficient instead of to
	nothing at all.
	*/
#define VMT_MAXPROX 40
	struct
	{
		char kind;		//VMTPRX_*
		char result[48];
		char src1[48];
		char src2[48];
		float a, b, c;	//scale / min / max / period, per kind
	} prox[VMT_MAXPROX];
	int numprox;
	int proxoverflow;
} vmtstate_t;

//The proxy operations this understands.  Everything else is parsed for its
//result variable alone, so a chain that passes through one is broken at that
//point rather than resolved through a shape we cannot model.
enum
{
	VMTPRX_OTHER = 0,
	VMTPRX_PLAYERPROXIMITY,	//a = scale;  result = dist * scale
	VMTPRX_SUBTRACT,		//result = src1 - src2
	VMTPRX_ADD,				//result = src1 + src2
	VMTPRX_MULTIPLY,		//result = src1 * src2
	VMTPRX_CLAMP,			//a = min, b = max;  result = clamp(src1, a, b)
	VMTPRX_SINE,			//c = period;  result oscillates between src1 and src2
	VMTPRX_GAUSSIANNOISE,	//a = minVal, b = maxVal;  result is noise in that range
	VMTPRX_EQUALS,			//result = src1
	VMTPRX_CONST			//a = the material's own numeric declaration of `result`
};


static void VARGS Q_strlcatfz (char *dest, size_t *offset, size_t size, const char *fmt, ...) LIKEPRINTF(4);
static void VARGS Q_strlcatfz (char *dest, size_t *offset, size_t size, const char *fmt, ...)
{
	va_list		argptr;

	dest += *offset;
	size -= *offset;

	va_start (argptr, fmt);
	Q_vsnprintfz(dest, size, fmt, argptr);
	va_end (argptr);
	*offset += strlen(dest);
}
static void Q_StrCat(char **ptr, const char *append)
{
	size_t oldlen = *ptr?strlen(*ptr):0;
	size_t newlen = strlen(append);
	char *newptr = plugfuncs->Malloc(oldlen+newlen+1);
	memcpy(newptr, *ptr, oldlen);
	memcpy(newptr+oldlen, append, newlen);
	newptr[oldlen+newlen] = 0;
	plugfuncs->Free(*ptr);
	*ptr = newptr;
}

static qboolean VMT_ReadVMT(const char *materialname, vmtstate_t *st);	//this is made more complicated on account of includes allowing recursion
//nettest: Source .vmt keys/blocks FTE intentionally ignores. Recognise-and-skip them so developer 1 isn't flooded,
//while a genuinely novel/malformed key or block still reaches the warning below.
static qboolean VMT_IsKnownIgnoredField(const char *key)
{
	static const char *ig[] = {"$nolod", "$model", "$FlashlightNoLambert",
		"$parallaxmap", "$parallaxmapscale",	//nettest: Source parallax-occlusion hints, FTE has no POM path
		"$cheapwaterstartdistance", "$cheapwaterenddistance",	//nettest: Source water LOD distance cutoffs, unused by FTE water
		"$multipass", "$fresnelreflection", "$modelmaterial", "$texture2", "$no_fullbright",	//nettest: more benign Source-only fields on HL2 glass/metal/combine props
		"$gnoise", "$playerdistance", "$playerdistance2", "$alpharesult", "$alpharesultmin", "$alpharesultmax",	//nettest: COMBINESHIELD proxy result-vars (top-level $-var declarations the proxy system writes)
		"$smallamount", "$largeamount", "$hundred", "$ten", "$frameminusten",
		//FTESurf Patch 150: the remaining developer-1 field spam on surf_666 and
		//the HL2 canals maps, each ignored for a reason rather than in bulk.
		"$reflectivity",		//VRAD COMPILE-TIME input: how much light a surface bounces. Consumed when the map was built; meaningless to a renderer reading the finished bsp.
		"$bluramount",			//Source water/refract blur radius; FTE's water has no blur stage to give it to
		"$phong", "$phongexponent", "$phongexponenttexture", "$phongboost",	//Source's per-pixel Phong model. FTE has specular but not this parameterisation, and half-applying it looks worse than not.
		"$phongfresnelranges", "$phongalbedotint", "$phongdisablehalflambert",
		"$phongwarptexture", "$phongtint",
		"$rimlight", "$rimlightexponent", "$rimlightboost",		//the Phong rim term, same story
		"$halflambert",			//lighting warp for the same model
		"$ambientocclusion", "$ambientocclusiontexture",
		//FTESurf Patch 188: the rest of the developer-1 field spam on
		//surf_unrequited and surf_demise, each ignored for a reason.  This list
		//is what makes a genuinely unrecognised key -- $color2 was one, until
		//this patch -- visible instead of buried.
		"$basemapalphaphongmask", "$lightwarptexture",	//more of the Phong family above
		"$disablecsmlookup",	//Source 2013 cascaded-shadow-map opt-out; FTE's shadows are not CSMs
		"$spriteorigin", "$spriteorientation",	//sprite billboarding, decided by the entity here
		"$center", "$angle", "$translate",		//TextureTransform proxy result-vars, declared at the top level
		NULL};
	const char **i;
	for (i = ig; *i; i++)
		if (!Q_strcasecmp(key, *i))
			return true;
	return false;
}
/*
FTESurf Patch 250: a bracketed vector, the way Source writes them.

"[10 0 0]" is float, "{255 0 0}" is 0-255 integer -- the brace form is what the
material system means by "this is a colour I authored in a paint program", and
Source divides it by 255 (IMaterialVar's SetVecValue path).  Values above 1 in
the bracket form are deliberate and are NOT clamped: elly/demon_circle asks for
a tint of [10 0 0], which is how it gets a red that survives being multiplied by
a strength of 0.1.
*/
static void VMT_ParseVec3(const char *value, float *out, int count)
{
	int i;
	qboolean bytes = false;
	for (i = 0; i < count; i++)
		out[i] = 0;
	while (*value == ' ' || *value == '\t' || *value == '"')
		value++;
	if (*value == '{')
		bytes = true;
	if (*value == '[' || *value == '{')
		value++;
	for (i = 0; i < count && *value; i++)
	{
		while (*value == ' ' || *value == '\t')
			value++;
		if (!*value || *value == ']' || *value == '}')
			break;
		out[i] = atof(value);
		if (bytes)
			out[i] /= 255.0f;
		while (*value && *value != ' ' && *value != '\t' && *value != ']' && *value != '}')
			value++;
	}
}

/*
FTESurf Patch 250: IS THIS A REAL SOURCE SHADER PARAMETER?

The list below is every SHADER_PARAM name declared by Source's own shaders
(materialsystem/stdshaders, all .cpp and .h -- 441 of them) MINUS the 62 this file already
implements or deliberately ignores by name above.  It is not a guess at what
mappers write; it is the authoritative set of keys a Source material is allowed
to carry, so anything in it is a key we UNDERSTAND and have not built, and
warning about it once per material forever is noise rather than information.

WHY A CLASS TEST AND NOT MORE ENTRIES IN THE ig[] LIST ABOVE.  A census of 11096
VMTs across 90 maps found 274 distinct keys this file does not handle.  Only 101
of them are real shader parameters; the other 173 are names a mapper INVENTED --
material-proxy result variables ($distance, $distdiff, $original, $i, $temp),
engine-branch extensions Source 2013 never had ($envmaplightscale,
$envmapanisotropy, $bumpscale), and typos ($surcefaceprop, $moidel).  An
allow-list can enumerate the first group exactly and can never finish the second,
which is why the two are separated here and counted separately below.

Kept sorted so a diff against a newer SDK drop is readable.
*/
static qboolean VMT_IsKnownSourceParam(const char *key)
{
	static const char *sp[] = {
		"$aaenable", "$aainternal1", "$aainternal2", "$aainternal3", "$addbasetexture2", "$addoverblend",
		"$addself", "$albedo", "$allowlocalcontrast", "$allownoise", "$allowvignette", "$alpha2",
		"$alpha_blend", "$alpha_blend_color_overlay", "$alphadepth", "$alphasharpenfactor",
		"$alphatested", "$ambientocclcolor", "$ambientoccltexture", "$ambientonly", "$autoexpose_max",
		"$autoexpose_min", "$basetexture2noenvmap", "$basetexture3", "$basetexturenoenvmap",
		"$basetextureoffset", "$basetexturescale", "$blendframes", "$blendmasktransform",
		"$blendtintbybasealpha", "$blendtintcoloroverbase", "$blobbyshadows", "$bloomamount",
		"$bloomenable", "$bloomexp", "$bloomexponent", "$bloomsaturation", "$bloomtexture",
		"$bloomtintenable", "$bloomtype", "$blurredvignetteenable", "$blurredvignettescale",
		"$blurrefract", "$blurtexture", "$bumpcompress", "$bumpframe2", "$bumpmap2", "$bumpmask",
		"$bumpstretch", "$bumptransform", "$bumptransform2", "$c0_w", "$c0_x", "$c0_y", "$c0_z", "$c1_w",
		"$c1_x", "$c1_y", "$c1_z", "$c2_w", "$c2_x", "$c2_y", "$c2_z", "$c3_w", "$c3_x", "$c3_y",
		"$c3_z", "$channel_select", "$clearalpha", "$clearcolor", "$cleardepth", "$cloakcolortint",
		"$cloakfactor", "$cloakpassenabled", "$cloudalphatexture", "$cloudscale", "$color_00",
		"$color_01", "$color_10", "$color_11", "$color_depth", "$colortint", "$compress", "$contrast",
		"$corneabumpstrength", "$corneatexture", "$depthblend", "$depthblendscale", "$depthblurenable",
		"$depthblurfocaldistance", "$depthblurstrength", "$depthtexture", "$desaturateenable",
		"$desaturation", "$detail_alpha_mask_base_texture", "$detailframe", "$detailtexturetransform",
		"$dilation", "$disable_color_writes", "$displacementmap", "$displacementwrinkle",
		"$distancealpha", "$distancealphafromdetail", "$distortbounds", "$distortmap", "$dualsequence",
		"$dust_texture", "$edge_softness", "$edgesoftnessend", "$edgesoftnessstart", "$emissionscale",
		"$emissionscale2", "$emissiontexture", "$emissiontexture2", "$emissiveblendbasetexture",
		"$emissiveblendenabled", "$emissiveblendflowtexture", "$emissiveblendscrollvector",
		"$emissiveblendstrength", "$emissiveblendtexture", "$emissiveblendtint", "$enableclearcolor",
		"$endfadesize", "$entityorigin", "$envmapframe", "$envmapfresnel", "$envmapmaskframe",
		"$envmapmasktransform", "$envmaporigin", "$envmapparallaxobb1", "$envmapparallaxobb2",
		"$envmapparallaxobb3", "$exposure_texture", "$extractgreenalpha", "$eyeballradius", "$eyeorigin",
		"$eyeup", "$fade", "$fadecolor", "$fadeoutonsilhouette", "$fadetoblackscale", "$falloffamount",
		"$falloffdistance", "$falloffoffset", "$farblurdepth", "$farblurradius", "$farfadeinterval",
		"$farfocusdepth", "$farplane", "$fb_texture", "$fbtexture", "$fleshbordernoisescale",
		"$fleshbordersoftness", "$fleshbordertexture1d", "$fleshbordertint", "$fleshborderwidth",
		"$fleshcubetexture", "$fleshdebugforcefleshon", "$flesheffectcenterradius1",
		"$flesheffectcenterradius2", "$flesheffectcenterradius3", "$flesheffectcenterradius4",
		"$fleshglobalopacity", "$fleshglossbrightness", "$fleshinteriorenabled",
		"$fleshinteriornoisetexture", "$fleshinteriortexture", "$fleshnormaltexture",
		"$fleshscrollspeed", "$fleshsubsurfacetexture", "$fleshsubsurfacetint", "$flowmap",
		"$fogexponent", "$fogscale", "$forcealphawrite", "$forward", "$fow", "$frame2", "$frametexture",
		"$gammacolorread", "$glint", "$glintu", "$glintv", "$glossiness", "$glow", "$glowalpha",
		"$glowcolor", "$glowend", "$glowstart", "$glowx", "$glowy", "$grain_texture", "$groundmax",
		"$groundmin", "$hdrcolorscale", "$hdrcompressedtexture0", "$hdrcompressedtexture1",
		"$hdrcompressedtexture2", "$hsv", "$hsv_blend", "$hudtranslucent", "$hudundistort",
		"$ignorevertexcolors", "$illumfactor", "$input_texture", "$internal_vignettetexture", "$intro",
		"$invertphongmask", "$iris", "$irisframe", "$irisu", "$irisv", "$kernel", "$leafcenter",
		"$light_color", "$light_position", "$lightmap", "$lights", "$linearread_basetexture",
		"$linearread_texture1", "$linearread_texture2", "$linearread_texture3", "$linearwrite",
		"$localcontrastedgescale", "$localcontrastenable", "$localcontrastmidtonemask",
		"$localcontrastscale", "$localcontrastvignetteend", "$localcontrastvignettestart", "$masked",
		"$maskedblending", "$maskscale", "$maxdistance", "$maxlight", "$maxlumframeblend1",
		"$maxlumframeblend2", "$maxsize", "$minlight", "$minsize", "$motionblurinternal",
		"$motionblurviewportinternal", "$mraoscale", "$mraoscale2", "$mraotexture", "$mraotexture2",
		"$mrtindex", "$nearblurdepth", "$nearblurradius", "$nearfocusdepth", "$nearplane",
		"$nodiffusebumplighting", "$nofresnel", "$noiseenable", "$noisescale", "$noisetexture",
		"$nolowendlightmap", "$normalmap2", "$normaltexture", "$noscale", "$nosrgb", "$nowritez",
		"$num_lookups", "$orientation", "$outline", "$outlinealpha", "$outlinecolor", "$outlineend0",
		"$outlineend1", "$outlinestart0", "$outlinestart1", "$overbrightfactor", "$parallax",
		"$parallaxcenter", "$parallaxdepth", "$parallaxstrength", "$passcount", "$phongexponentfactor",
		"$pixshader", "$quality", "$ramptexture", "$raytracesphere", "$receiveflashlight",
		"$reflectance", "$reflectblendfactor", "$refracttinttextureframe", "$rimmask", "$saturation",
		"$scaleedgesoftnessbasedonscreenres", "$scaleoutlinesoftnessbasedonscreenres",
		"$screenblurstrength", "$screeneffecttexture", "$seamless_base", "$seamless_detail",
		"$seamless_scale", "$selfillum_envmapmask_alpha", "$selfillumfresnel",
		"$selfillumfresnelminmaxexp", "$selfillummap", "$separatedetailuvs", "$sequence_blend_mode",
		"$shadersrgbread360", "$shadowdepth", "$sharpness", "$sheenindex", "$sheenmap", "$sheenmapmask",
		"$sheenmapmaskdirection", "$sheenmapmaskframe", "$sheenmapmaskoffsetx", "$sheenmapmaskoffsety",
		"$sheenmapmaskscalex", "$sheenmapmaskscaley", "$sheenmaptint", "$sheenpassenabled", "$showalpha",
		"$silhouettecolor", "$silhouettethickness", "$smallfb", "$softedges", "$sourcemrtrendertarget",
		"$spectexture", "$spheretexkillcombo", "$splinetype", "$spriterendermode", "$startfadesize",
		"$stretch", "$texture1", "$texture2transform", "$texture3", "$time", "$tint",
		"$toolcolorcorrection", "$toolmode", "$tooltime", "$translucent_material", "$treesway",
		"$treeswayfalloffexp", "$treeswayheight", "$treeswayradius", "$treeswayscrumblefalloffexp",
		"$treeswayscrumblefrequency", "$treeswayscrumblespeed", "$treeswayscrumblestrength",
		"$treeswayspeed", "$treeswayspeedhighwindmultiplier", "$treeswayspeedlerpend",
		"$treeswayspeedlerpstart", "$treeswaystartheight", "$treeswaystartradius", "$treeswaystrength",
		"$tv_gamma", "$unlit", "$unlitfactor", "$use_fb_texture", "$useinstancing", "$userendertarget",
		"$usingpixelshader", "$velocity_texture", "$vertexalphatest", "$vertexcolormodulate",
		"$vignette_min_bright", "$vignette_power", "$vignetteenable", "$vomitcolor1", "$vomitcolor2",
		"$vomitenable", "$vomitrefractscale", "$warpparam", "$waterdepth", "$wave", "$weight0",
		"$weight1", "$weight2", "$weight3", "$weight_default", "$woodcut", "$writez", "$x360appchooser",
		"$zoomanimateseq2",
		NULL};
	const char **i;
	for (i = sp; *i; i++)
		if (!Q_strcasecmp(key, *i))
			return true;
	return false;
}
static qboolean VMT_IsKnownIgnoredBlock(const char *key)
{	//Source ships per-DX-level / HDR shader-variant sub-blocks + a Proxies block
	static const char *ig[] = {"vertexlitgeneric", "lightmappedgeneric", "unlitgeneric", "worldvertextransition", "proxies",
		"animatedtexture", "texturescroll", "waterlod",	//nettest: named animation/scroll/LOD sub-blocks FTE doesn't interpret
		"water_",	//nettest: per-DX water fallbacks Water_DX81/DX80/DX60 (prefix, case-insensitive)
		NULL};
	const char **i;
	//nettest: Source DX-version conditional blocks (">=dx90", "<DX90", ">=DX90", "=dx9") begin with an operator,
	//not a letter; they are never real material blocks FTE handles, so skip any block whose name starts with > < or =.
	if (*key == '>' || *key == '<' || *key == '=')
		return true;
	for (i = ig; *i; i++)
		if (!Q_strncasecmp(key, *i, strlen(*i)))
			return true;
	return false;
}
static char *VMT_ParseBlock(const char *fname, vmtstate_t *st, char *line);

//FTESurf Patch 189: does this proxy drive the base texture's transform?  An
//absent texturescrollvar means the material never said, and every observed one
//that omits it means $basetextureTransform.
static qboolean VMT_ScrollsBase(const char *var)
{
	const char *s;
	if (!*var)
		return true;
	for (s = var; *s; s++)
		if (!Q_strncasecmp(s, "basetexture", 11))
			return true;
	return false;
}

//The keys inside one `TextureScroll { }` proxy.  A sub-block here would be an
//arithmetic proxy feeding a value in; we do not evaluate those, so it is
//skipped exactly as before.
static char *VMT_ParseTextureScroll(const char *fname, vmtstate_t *st, char *line)
{
	com_tokentype_t ttype;
	char key[MAX_OSPATH];
	char value[MAX_OSPATH];
	/*
	FTESurf: buffered and then ROUTED, rather than written straight through.

	A material may carry more than one TextureScroll, and which transform each
	drives is decided by a key that can appear anywhere in the block -- so the
	rate and angle cannot be assigned to a slot until the block is closed.
	comshieldwall.vmt is the case: its single scroll drives $texture2Transform,
	and Patch 189's code wrote the rate into the BASE slot regardless and then
	let VMT_ScrollsBase decline it at emit time.  Declining is right for a
	lightmapped surface with one texture; for UnlitTwoTexture the second layer
	sliding across the first IS the effect, and it was being thrown away.
	*/
	char var[64] = "";
	float rate = 0, angle = 0;

	for(;line;)
	{
		line = cmdfuncs->ParseToken(line, key, sizeof(key), &ttype);
		if (ttype == TTP_RAWTOKEN && !strcmp(key, "}"))
			break;
		line = cmdfuncs->ParseToken(line, value, sizeof(value), &ttype);
		if (ttype == TTP_RAWTOKEN && !strcmp(value, "{"))
		{
			line = VMT_ParseBlock(fname, NULL, line);
			continue;
		}
		if (!Q_strcasecmp(key, "texturescrollvar"))
			Q_strlcpy(var, value, sizeof(var));
		else if (!Q_strcasecmp(key, "texturescrollrate"))
			rate = atof(value);
		else if (!Q_strcasecmp(key, "texturescrollangle"))
			angle = atof(value);
	}

	if (st)
	{
		//"texture2transform" and "texture2Transform" both appear; the test is a
		//substring one for the same reason VMT_ScrollsBase' is.
		if (!Q_strncasecmp(var, "$texture2", 9))
		{
			Q_strlcpy(st->tex2scrollvar, var, sizeof(st->tex2scrollvar));
			st->tex2scrollrate = rate;
			st->tex2scrollangle = angle;
		}
		else
		{
			Q_strlcpy(st->scrollvar, var, sizeof(st->scrollvar));
			st->scrollrate = rate;
			st->scrollangle = angle;
		}
	}
	return line;
}

//FTESurf Patch 195: the keys inside one `AnimatedTexture { }` proxy.  Same shape
//as TextureScroll above, and the same rule about a var that names something
//other than the base texture: read it, then decline it.
static char *VMT_ParseAnimatedTexture(const char *fname, vmtstate_t *st, char *line)
{
	com_tokentype_t ttype;
	char key[MAX_OSPATH];
	char value[MAX_OSPATH];
	for(;line;)
	{
		line = cmdfuncs->ParseToken(line, key, sizeof(key), &ttype);
		if (ttype == TTP_RAWTOKEN && !strcmp(key, "}"))
			break;
		line = cmdfuncs->ParseToken(line, value, sizeof(value), &ttype);
		if (ttype == TTP_RAWTOKEN && !strcmp(value, "{"))
		{
			line = VMT_ParseBlock(fname, NULL, line);
			continue;
		}
		if (!Q_strcasecmp(key, "animatedtexturevar"))
			Q_strlcpy(st->animvar, value, sizeof(st->animvar));
		else if (!Q_strcasecmp(key, "animatedtextureframerate"))
			st->animrate = atof(value);
	}
	return line;
}

/*
FTESurf: one ARITHMETIC proxy -- PlayerProximity, Subtract, Add, Multiply,
Clamp, Sine, GaussianNoise, Equals.

These were skipped wholesale, which was the right call while nothing could use
them: they compute per-frame scalars and FTE has no per-material scalar channel
for the CPU to write into.  What makes them reachable now is that the ones the
shield uses are pure functions of EYE DISTANCE and TIME, both of which a
fragment shader already has -- so the graph is read once at load and compiled
into shader constants rather than evaluated per frame.  See VMT_ResolveProxies.

Every proxy is recorded, including the kinds not understood, because a chain
that passes through an unmodelled operation must BREAK there rather than be
resolved through a link that does not exist.  Recording it with kind
VMTPRX_OTHER is what makes the backward walk stop instead of guessing.
*/
static char *VMT_ParseArithProxy(const char *fname, vmtstate_t *st, int kind, char *line)
{
	com_tokentype_t ttype;
	char key[MAX_OSPATH];
	char value[MAX_OSPATH];
	int idx = -1;

	if (st)
	{
		if (st->numprox < VMT_MAXPROX)
		{
			idx = st->numprox++;
			memset(&st->prox[idx], 0, sizeof(st->prox[idx]));
			st->prox[idx].kind = kind;
		}
		else
			st->proxoverflow++;
	}

	for(;line;)
	{
		line = cmdfuncs->ParseToken(line, key, sizeof(key), &ttype);
		if (ttype == TTP_RAWTOKEN && !strcmp(key, "}"))
			break;
		line = cmdfuncs->ParseToken(line, value, sizeof(value), &ttype);
		if (ttype == TTP_RAWTOKEN && !strcmp(value, "{"))
		{	//a proxy feeding another proxy inline; not a shape Source writes
			//here, and not one we could name a result variable for.
			line = VMT_ParseBlock(fname, NULL, line);
			continue;
		}
		if (idx < 0)
			continue;

		if (!Q_strcasecmp(key, "resultVar"))
			Q_strlcpy(st->prox[idx].result, value, sizeof(st->prox[idx].result));
		else if (!Q_strcasecmp(key, "srcVar1"))
			Q_strlcpy(st->prox[idx].src1, value, sizeof(st->prox[idx].src1));
		else if (!Q_strcasecmp(key, "srcVar2"))
			Q_strlcpy(st->prox[idx].src2, value, sizeof(st->prox[idx].src2));
		else if (!Q_strcasecmp(key, "sinemin"))
			Q_strlcpy(st->prox[idx].src1, value, sizeof(st->prox[idx].src1));
		else if (!Q_strcasecmp(key, "sinemax"))
			Q_strlcpy(st->prox[idx].src2, value, sizeof(st->prox[idx].src2));
		else if (!Q_strcasecmp(key, "sineperiod"))
			st->prox[idx].c = atof(value);
		else if (!Q_strcasecmp(key, "scale"))
			st->prox[idx].a = atof(value);
		else if (!Q_strcasecmp(key, "min") || !Q_strcasecmp(key, "minVal"))
			st->prox[idx].a = atof(value);
		else if (!Q_strcasecmp(key, "max") || !Q_strcasecmp(key, "maxVal"))
			st->prox[idx].b = atof(value);
	}
	return line;
}

//The Proxies block itself.  Descended into far enough to find TextureScroll,
//AnimatedTexture, and the arithmetic chain; anything genuinely unrecognised is
//still skipped with st==NULL, so nothing starts warning that did not before.
static char *VMT_ParseProxies(const char *fname, vmtstate_t *st, char *line)
{
	com_tokentype_t ttype;
	char key[MAX_OSPATH];
	char value[MAX_OSPATH];
	for(;line;)
	{
		line = cmdfuncs->ParseToken(line, key, sizeof(key), &ttype);
		if (ttype == TTP_RAWTOKEN && !strcmp(key, "}"))
			break;
		line = cmdfuncs->ParseToken(line, value, sizeof(value), &ttype);
		if (ttype == TTP_RAWTOKEN && !strcmp(value, "{"))
		{
			if (!Q_strcasecmp(key, "texturescroll"))
				line = VMT_ParseTextureScroll(fname, st, line);
			else if (!Q_strcasecmp(key, "animatedtexture"))
				line = VMT_ParseAnimatedTexture(fname, st, line);
			else if (!Q_strcasecmp(key, "playerproximity"))
				line = VMT_ParseArithProxy(fname, st, VMTPRX_PLAYERPROXIMITY, line);
			else if (!Q_strcasecmp(key, "subtract"))
				line = VMT_ParseArithProxy(fname, st, VMTPRX_SUBTRACT, line);
			else if (!Q_strcasecmp(key, "add"))
				line = VMT_ParseArithProxy(fname, st, VMTPRX_ADD, line);
			else if (!Q_strcasecmp(key, "multiply"))
				line = VMT_ParseArithProxy(fname, st, VMTPRX_MULTIPLY, line);
			else if (!Q_strcasecmp(key, "clamp"))
				line = VMT_ParseArithProxy(fname, st, VMTPRX_CLAMP, line);
			else if (!Q_strcasecmp(key, "sine"))
				line = VMT_ParseArithProxy(fname, st, VMTPRX_SINE, line);
			else if (!Q_strcasecmp(key, "gaussiannoise") || !Q_strcasecmp(key, "uniformnoise"))
				line = VMT_ParseArithProxy(fname, st, VMTPRX_GAUSSIANNOISE, line);
			else if (!Q_strcasecmp(key, "equals"))
				line = VMT_ParseArithProxy(fname, st, VMTPRX_EQUALS, line);
			else
				line = VMT_ParseArithProxy(fname, st, VMTPRX_OTHER, line);
			continue;
		}
	}
	return line;
}

/*
FTESurf: compile the proxy graph into shader coefficients.

WHY THIS IS A BACKWARD WALK.  A Proxies block is a sequence of assignments and
most of them are dead ends -- comshieldwall.vmt computes $largeamount and
$envmaptint[1] and never uses either.  Walking forward means evaluating
everything and hoping something lands on a variable that matters.  Walking back
from the two variables that DO matter -- $alpha and $frame -- touches only the
chain that feeds them, and stops the moment it meets an operation this does not
model, which is the behaviour we want: an unrecognised link means "decline",
not "assume".

WHAT IT CAN RESOLVE, stated as the two shapes rather than as a general claim:

    $alpha  <- Sine(min=X, max=Y)   or   X directly
       X    <- Subtract(A, P)       where P is a PlayerProximity
                                    => alpha = value(A) - dist*P.scale

    $frame  <- Clamp(X, lo, hi)
       X    <- Subtract(P, K)       where P is a PlayerProximity
                                    => frame = dist*P.scale - value(K),
                                       clamped to [lo, hi]

Anything else leaves the coefficients at zero and the shader's defaults make
them no-ops.  That is a narrow claim and it is meant to be: eleven materials in
the mounted library carry a PlayerProximity at all, and all of them are the
combine shield family, so a general evaluator would be built to serve one shape
and then be trusted for shapes nobody checked.
*/
static int VMT_FindProx(vmtstate_t *st, const char *var, int notidx)
{	//the LAST assignment wins, which is how a sequence of assignments works
	int i, found = -1;
	if (!var || !*var || *var != '$')
		return -1;
	for (i = 0; i < st->numprox; i++)
	{
		if (i == notidx)
			continue;
		//a CONST is only a fallback: a $var that is both declared at the top
		//level and written by a proxy is a placeholder for the proxy's output.
		if (!Q_strcasecmp(st->prox[i].result, var))
		{
			if (st->prox[i].kind == VMTPRX_CONST && found >= 0)
				continue;
			if (found >= 0 && st->prox[found].kind != VMTPRX_CONST &&
			    st->prox[i].kind == VMTPRX_CONST)
				continue;
			found = i;
		}
	}
	return found;
}

//The scalar value of a variable, for the constant side of a Subtract.  Only the
//shapes that can be a constant: a literal number, a declaration, or a noise
//proxy (whose mean is the only constant it has).
static qboolean VMT_ProxValue(vmtstate_t *st, const char *var, float *out)
{
	int i;
	char *end;
	double v;

	if (!var || !*var)
		return false;
	if (*var != '$')
	{	//written inline as a number
		v = strtod(var, &end);
		if (end != var && !*end)
		{
			*out = v;
			return true;
		}
		return false;
	}
	i = VMT_FindProx(st, var, -1);
	if (i < 0)
		return false;
	if (st->prox[i].kind == VMTPRX_CONST)
	{
		*out = st->prox[i].a;
		return true;
	}
	if (st->prox[i].kind == VMTPRX_GAUSSIANNOISE)
	{	//a flicker: its mean is the part a baked coefficient can carry, and the
		//Sine below supplies the wobble the noise would otherwise have given.
		*out = (st->prox[i].a + st->prox[i].b) * 0.5f;
		return true;
	}
	return false;
}

//"$X = something - dist*scale": the alpha shape.  Returns the scale, and the
//constant term through *base.
static qboolean VMT_ProxDistFade(vmtstate_t *st, const char *var, float *scale, float *base)
{
	int sub = VMT_FindProx(st, var, -1);
	int pp;
	if (sub < 0 || st->prox[sub].kind != VMTPRX_SUBTRACT)
		return false;
	pp = VMT_FindProx(st, st->prox[sub].src2, -1);
	if (pp < 0 || st->prox[pp].kind != VMTPRX_PLAYERPROXIMITY)
		return false;
	if (st->prox[pp].a == 0)
		return false;
	if (!VMT_ProxValue(st, st->prox[sub].src1, base))
		*base = 1.0f;
	*scale = st->prox[pp].a;
	return true;
}

static void VMT_ResolveProxies(vmtstate_t *st)
{
	int i, sub, pp;
	float f;

	if (!st->numprox)
		return;

	//---- $alpha
	i = VMT_FindProx(st, "$alpha", -1);
	if (i >= 0 && st->prox[i].kind == VMTPRX_SINE)
	{
		float lo = 0, hi = 0;
		//The Sine oscillates between two variables.  Its LOW end carries the
		//distance fade; the gap between them is the flicker's amplitude, and
		//on every material in this family that gap is a small constant added by
		//an Add proxy, so it is read as a value rather than followed.
		if (VMT_ProxDistFade(st, st->prox[i].src1, &st->proxalpha, &st->proxalphabase))
		{
			st->flickerperiod = st->prox[i].c;
			//amplitude: sinemax - sinemin, when both resolve to numbers.  An
			//Add of a small constant is the only shape observed, so the
			//amplitude is that constant and it is HALVED -- a sine spends its
			//time centred between the two, not at the top.
			sub = VMT_FindProx(st, st->prox[i].src2, -1);
			if (sub >= 0 && st->prox[sub].kind == VMTPRX_ADD &&
			    VMT_ProxValue(st, st->prox[sub].src2, &f))
				st->flickeramp = fabs(f) * 0.5f;
			(void)lo; (void)hi;
		}
	}
	else if (i >= 0)
		VMT_ProxDistFade(st, st->prox[i].result, &st->proxalpha, &st->proxalphabase);
	if (st->proxalpha == 0)
		VMT_ProxDistFade(st, "$alpha", &st->proxalpha, &st->proxalphabase);

	//---- $frame
	i = VMT_FindProx(st, "$frame", -1);
	if (i >= 0 && st->prox[i].kind == VMTPRX_CLAMP)
	{
		sub = VMT_FindProx(st, st->prox[i].src1, -1);
		if (sub >= 0 && st->prox[sub].kind == VMTPRX_SUBTRACT)
		{
			pp = VMT_FindProx(st, st->prox[sub].src1, -1);
			if (pp >= 0 && st->prox[pp].kind == VMTPRX_PLAYERPROXIMITY &&
			    st->prox[pp].a != 0)
			{
				if (!VMT_ProxValue(st, st->prox[sub].src2, &f))
					f = 0;
				st->proxframe     = st->prox[pp].a;
				st->proxframebias = f;
				st->proxframemin  = st->prox[i].a;
				st->proxframemax  = st->prox[i].b;
			}
		}
	}
}

static char *VMT_ParseBlock(const char *fname, vmtstate_t *st, char *line)
{	//assumes the open { was already parsed, but will parse the close.
	char *replace = NULL;
	com_tokentype_t ttype;
	char key[MAX_OSPATH];
	char value[MAX_OSPATH];
	char *qmark;
	qboolean cond;
	for(;line;)
	{
		line = cmdfuncs->ParseToken(line, key, sizeof(key), &ttype);
		if (ttype == TTP_RAWTOKEN && !strcmp(key, "}"))
			break;	//end-of-block
		line = cmdfuncs->ParseToken(line, value, sizeof(value), &ttype);
		if (ttype == TTP_RAWTOKEN && !strcmp(value, "{"))
		{	//sub block. we don't go into details here.
			//insert and replace blocks do the same thing in practice 
			if (!Q_strcasecmp(key, "replace") || !Q_strcasecmp(key, "insert"))
				replace = line;
			else if (st && !Q_strcasecmp(key, "proxies"))
			{	//FTESurf Patch 189: the one proxy we model. Only at the material
				//top level, where st exists -- a Proxies block nested inside
				//something we are already skipping is not ours to act on.
				line = VMT_ParseProxies(fname, st, line);
				continue;
			}
			else if (VMT_IsKnownIgnoredBlock(key))
				;	//nettest: Source per-DX/HDR shader-variant sub-block (vertexlitgeneric_dx9, Proxies, …) — benign
			else if (st)	//nettest: only warn about UNKNOWN blocks at the material TOP level. Nested sub-blocks recurse with st==NULL — e.g. every proxy entry inside "Proxies" (GaussianNoise/PlayerProximity/Add/Subtract/Multiply/Equals/Sine/Clamp/…). FTE handles no nested blocks, so they're all benign noise.
				Con_DPrintf("%s: Unknown block \"%s\"\n", fname, key);
			line = VMT_ParseBlock(fname, NULL, line);
			continue;
		}

		while ((qmark = strchr(key, '?')))
		{
			*qmark++ = 0;
			if (!Q_strcasecmp(key, "srgb"))
				cond = false;//!!(vid.flags & VID_SRGBAWARE);
			else
			{
				Con_DPrintf("%s: Unknown vmt conditional \"%s\"\n", fname, key);
				cond = false;
			}
			if (!cond)
			{
				*key = 0;
				break;
			}
			else
				memmove(key, qmark, strlen(qmark)+1);
		}

		/*
		FTESurf: every NUMERIC $var, recorded for the proxy resolver.

		A proxy chain reads its constants out of the material's own top-level
		declarations: comshieldwall.vmt writes `"$ten" "24"` and then subtracts
		$ten from a distance.  Without these the chain resolves to a coefficient
		with a missing term, which is worse than not resolving at all -- it
		would put the frame ramp in the wrong place rather than leave it off.

		Recorded BEFORE the key dispatch, not in its unknown-key tail, because
		some of these names are real Source parameters ($alpha, $frame) that the
		dispatch claims; the resolver prefers a proxy result over a constant of
		the same name, so a declaration that is only a placeholder for a proxy's
		output cannot win.  Costs one strtod per key on materials with no
		proxies at all, and nothing else.
		*/
		if (*key == '$' && st && st->numprox < VMT_MAXPROX)
		{
			char *end;
			double v = strtod(value, &end);
			while (*end == ' ' || *end == '\t')
				end++;
			if (end != value && !*end)
			{
				int c = st->numprox++;
				memset(&st->prox[c], 0, sizeof(st->prox[c]));
				st->prox[c].kind = VMTPRX_CONST;
				Q_strlcpy(st->prox[c].result, key, sizeof(st->prox[c].result));
				st->prox[c].a = v;
			}
		}

		if (!*key || !st)
			;
		else if (!Q_strcasecmp(key, "include"))
		{
			if (!VMT_ReadVMT(value, st))
				return NULL;
		}
		else if (!Q_strcasecmp(key, "$basetexture"))	//nettest: $basetexture is the LDR face — make it authoritative
			Q_strlcpy(st->tex[0].name, value, sizeof(st->tex[0].name));
		else if (!Q_strcasecmp(key, "$hdrbasetexture"))	//nettest: uncompressed HDR sky face (RGBA16161616F) — native float HDR, used in preference to the LDR $basetexture to kill banding
			Q_strlcpy(st->hdrbasetex, value, sizeof(st->hdrbasetex));
		else if (!Q_strcasecmp(key, "$hdrcompressedtexture"))	//nettest: compressed HDR sky face (RGBS in BGRA8) — decoded rgb*alpha*8 by img_vtf via the $hdr: name prefix
			Q_strlcpy(st->hdrcompressedtex, value, sizeof(st->hdrcompressedtex));
		else if (!Q_strcasecmp(key, "$basetexturetransform"))
			;
		else if (!Q_strcasecmp(key, "$bumpmap")) // same as normalmap ~eukara
			Q_strlcpy(st->normalmap, value, sizeof(st->normalmap));
		//FTESurf Patch 251: the second normal map, for WorldVertexTransition.
		else if (!Q_strcasecmp(key, "$bumpmap2"))
			Q_strlcpy(st->bumpmap2, value, sizeof(st->bumpmap2));
		/*
		FTESurf Patch 251: $seamless_scale, and the two keys that ride with it.

		$seamless_base and $seamless_detail are accepted and ignored ON PURPOSE
		rather than left to the unknown-key census.  $seamless_detail is not a real
		Source parameter on a LightmappedGeneric at all -- only VertexlitGeneric
		declares it (vertexlitgeneric_dx9.cpp:78-80) -- and $seamless_base selects
		which of the base maps is projected, which is moot here because both are.
		Silencing them beside the key they belong to keeps the family in one place.
		*/
		else if (!Q_strcasecmp(key, "$seamless_scale"))
		{
			st->seamless = atof(value);
			st->seamless_set = true;
		}
		else if (!Q_strcasecmp(key, "$seamless_base") || !Q_strcasecmp(key, "$seamless_detail"))
			;
		else if (!Q_strcasecmp(key, "$dudvmap")) // refractions only
		{
			Q_strlcpy(st->dudvmap, value, sizeof(st->dudvmap));
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		}
		else if (!Q_strcasecmp(key, "$ssbump"))
			;
		else if (!Q_strcasecmp(key, "$ssbumpmathfix"))
			;
		else if (!Q_strcasecmp(key, "$basetexture2") || !strcmp(key, "$texture2"))
			Q_strlcpy(st->tex[1].name, value, sizeof(st->tex[1].name));
		else if (!Q_strcasecmp(key, "$basetexturetransform2"))
			;
		else if (!Q_strcasecmp(key, "$surfaceprop"))
			;

		else if (!Q_strcasecmp(key, "$ignorez"))
			st->ignorez = !!atoi(value);
		else if (!Q_strcasecmp(key, "$nocull") && (!strcmp(value, "1")||!strcmp(value, "0")))
			st->culldisable = atoi(value);
		else if (!Q_strcasecmp(key, "$alphatest") && (!strcmp(value, "1")||!strcmp(value, "0")))
			st->alphatest = atoi(value);
		else if (!Q_strcasecmp(key, "$alphatestreference"))
			st->alphatestref = atof(value);
		else if (!Q_strcasecmp(key, "$alphafunc"))
		{
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		}
		else if (!Q_strcasecmp(key, "$alpha"))
		{
			/*
			FTESurf: honour a ZERO $alpha.

			This was recognised-and-ignored, which meant a material whose author
			had made it invisible by turning its opacity off came out fully
			opaque -- the exact inverse of what the VMT asks for.

			surf_null is the case that found it.  Its ramp clip brushes carry a
			custom material, materials/rampcollision.vmt:

			    UnlitGeneric { $basetexture "tools/toolsplayerclip"
			                   $alpha 0  $translucent 1 }

			191 faces of it, painted right across the ramps.  Note it is NOT a
			TOOLS texture as far as the compiler is concerned and carries no
			SURF_NODRAW, so neither the BSP flag nor the %compile* keys above
			catch it -- there is nothing to notice except the alpha itself.

			Only the zero case is acted on, and it is exact: a surface at zero
			opacity can never contribute a pixel, so nodraw is both what it
			looks like and strictly cheaper than blending a transparent one.

			FTESurf Patch 187: a FRACTIONAL $alpha is no longer ignored.  The
			paragraph that used to sit here was right about the cause -- FTE
			takes alphagen only inside a pass (gl_shader.c:4369) and this
			generator emits a top-level program block -- and wrong to conclude
			there was nowhere to put the constant.  #ALPHA is a compile-time
			define on the program, the same mechanism ENVTINT/ENVSAT already
			used, so it needs no pass and no engine change.
			*/
			if (atof(value) <= 0 && hl2_hidetools && hl2_hidetools->ival)
			{
				st->nodraw = 1;
				Con_DPrintf("%s: $alpha 0 -- hidden\n", fname);
			}
			else
				st->alphaval = atof(value);	//FTESurf build 12: kept now; the dither pass can carry it (see hl2_translucent)
		}
		else if (!Q_strcasecmp(key, "$translucent"))
		{
			/*
			FTESurf Patch 217: READ THE VALUE.

			This ignored it, so `$translucent 0` -- a material saying outright that
			it is NOT translucent -- was made translucent, which is the exact
			inverse of what the VMT asks for.  The bug is visible three lines
			below, where $additive gets this right: `if (atoi(value))`.

			The consequence is not one wrong surface, it is a wrong RENDER MODE for
			the surface and everything behind it.  A blended surface cannot write
			depth, so it has to be sorted back-to-front and it stops occluding what
			is behind it; a map whose walls and floors have all been turned
			translucent therefore draws its whole world in the wrong order.  The
			user's words were "a lot of glass / alpha textures in this map ... and
			it seems to get the ordering of these alphas wrong, almost backwards".

			surf_kitsune is the case that found it.  Its nine GRIDS/GRID_* materials
			-- the floors and walls of every room -- are all of this shape:

			    UnlitGeneric { $basetexture "grids/grid_red"  $translucent 0
			                   $selfillum 1  $alpha 0.75 }

			$translucent 0 with a fractional $alpha is deliberate authoring, not a
			contradiction: Patch 187's #ALPHA carries the 0.75 into the program as a
			constant, and the material wants that constant WITHOUT being sorted as
			glass.  Reading the 0 is what lets it have that.

			Counted across the 2394 VBSP maps in the library that carry an embedded
			pakfile: 945 materials in 106 maps write $translucent 0 and were all
			being blended.  22740 write a nonzero value and are untouched by this,
			which is what makes the fix narrow -- it can only ever turn a surface
			the author marked opaque back into an opaque one.
			*/
			st->translucent = atoi(value) ? 1 : 0;
		}
		else if (!Q_strcasecmp(key, "$additive"))
		{
			if (atoi(value))
				st->additive = 1;
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		}
		else if (!Q_strcasecmp(key, "$halflambert"))
			st->halflambert = 1;
		/*
		FTESurf: the tool materials.

		Every "%..." key falls through to the "editor lines" arm further down and
		is discarded -- which was right for %keywords and %tooltexture, and wrong
		for the compile flags, because those say what the surface IS.  Only
		%compiletrigger was ever caught, which is exactly why TOOLSTRIGGER came out
		invisible and TOOLSCLIP came out as a lightmapped wall across the ramp.

		The BSP's own SURF_NODRAW is the primary fix (mod_vbsp.c); this is the
		backstop for faces the compiler did not flag, and it costs one strcmp.
		None of these affect collision -- they are render-side only.
		*/
		else if (!Q_strcasecmp(key, "%compiletrigger")   ||
		         !Q_strcasecmp(key, "%compilenodraw")    ||
		         !Q_strcasecmp(key, "%compileclip")      ||
		         !Q_strcasecmp(key, "%compileplayerclip")||
		         !Q_strcasecmp(key, "%compilenpcclip")   ||
		         !Q_strcasecmp(key, "%compileinvisible") ||
		         !Q_strcasecmp(key, "%compileskip")      ||
		         !Q_strcasecmp(key, "%compilehint")      ||
		         !Q_strcasecmp(key, "%compileareaportal")||
		         !Q_strcasecmp(key, "%compileblocklight"))
		{
			// %compiletrigger was always honoured; the rest are new, and
			// hl2_hidetools turns the lot off together.  Leaving trigger
			// unconditional would make the cvar a half-truth.
			if (hl2_hidetools && hl2_hidetools->ival)
				st->nodraw = 1;
		}
		else if (!Q_strcasecmp(key, "lampbeam"))
			st->additive = 1;
		else if (!Q_strcasecmp(key, "$color"))
			Q_strlcpy(st->color, value, sizeof(st->color));
		/*
		FTESurf Patch 188: $color2, which was not parsed at all.

		Source has both and $color2 is the modern one; where a material writes
		both, $color2 is what the game draws, so it wins here too.

		This is not a corner case.  surf_unrequited is a flat-shaded map whose
		entire palette is one white texture tinted per-material: 227 of its 275
		materials carry $color2, and dropping the key is the whole of "all
		textures are white".  Across the library it is 3,357 materials in 356
		maps.
		*/
		else if (!Q_strcasecmp(key, "$color2"))
			Q_strlcpy(st->color2, value, sizeof(st->color2));
		else if (!Q_strcasecmp(key, "$vertexcolor"))
			st->vertexcolor = 1;
		else if (!Q_strcasecmp(key, "$vertexalpha"))
			st->vertexalpha = 1;
		else if (!Q_strcasecmp(key, "$decal"))
			st->decal = atoi(value);
		else if (!Q_strcasecmp(key, "$decalscale"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$decalsize"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$envmap"))
			Q_strlcpy(st->envmap, value, sizeof(st->envmap));
		else if (!Q_strcasecmp(key, "$envmapmask"))
			Q_strlcpy(st->envmapmask, value, sizeof(st->envmapmask));
		//FTESurf Patch 268 B: kept rather than dropped.  Source applies it as
		//lerp(spec, spec*spec, contrast) between the tint and the saturation
		//(..._bump_ps20b.fxc:296-312), which is the second bit of hl2_envmap_source.
		else if (!Q_strcasecmp(key, "$envmapcontrast"))
		{
			st->envmapcontrast = atof(value);
			st->envmapcontrast_set = true;
		}
		else if (!Q_strcasecmp(key, "$envmaptint"))
		{
			char tok[64];
			char *tintline;
			tintline = cmdfuncs->ParsePunctuation(value, "[", tok, sizeof(tok), 0);

			if (!strcmp(tok, "[")) {
				tintline = cmdfuncs->ParseToken(tintline, tok, sizeof(tok), 0);
				st->envmaptint_r = strtof(tok, NULL);
				tintline = cmdfuncs->ParseToken(tintline, tok, sizeof(tok), 0);
				st->envmaptint_g = strtof(tok, NULL);
				tintline = cmdfuncs->ParsePunctuation(tintline, "]", tok, sizeof(tok), 0);
				st->envmaptint_b = strtof(tok, NULL);
			} else {
				st->envmaptint_r = st->envmaptint_g = st->envmaptint_b = atof(value);
			}
			st->envmaptint_set = 1;		//Patch 237: "was it written", not "is it non-zero"
		}
		else if (!Q_strcasecmp(key, "$envmapsaturation"))
		{
			char tok[64];
			char *tintline;
			tintline = cmdfuncs->ParsePunctuation(value, "[", tok, sizeof(tok), 0);

			if (!strcmp(tok, "[")) {
				tintline = cmdfuncs->ParseToken(tintline, tok, sizeof(tok), 0);
				st->envmapsat_r = strtof(tok, NULL);
				tintline = cmdfuncs->ParseToken(tintline, tok, sizeof(tok), 0);
				st->envmapsat_g = strtof(tok, NULL);
				tintline = cmdfuncs->ParsePunctuation(tintline, "]", tok, sizeof(tok), 0);
				st->envmapsat_b = strtof(tok, NULL);
			} else {
				st->envmapsat_r = st->envmapsat_g = st->envmapsat_b = atof(value);
			}
			st->envmapsat_set = 1;		//Patch 237: ditto -- $envmapsaturation 0 is a real request
		}
		else if (!Q_strcasecmp(key, "$basealphaenvmapmask"))
			st->envfrombase=1;
		else if (!Q_strcasecmp(key, "$normalmapalphaenvmapmask"))
			st->envfromnorm=1;
		else if (!Q_strcasecmp(key, "$crackmaterial"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$selfillum"))
			st->selfillum = 1;
		else if (!Q_strcasecmp(key, "$selfillummask"))
			Q_strlcpy(st->fullbrightmap, value, sizeof(st->fullbrightmap));
		else if (!Q_strcasecmp(key, "$selfillumtint"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$nofog"))
			st->nofog = 1;
		else if (!Q_strcasecmp(key, "$nomip"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$nodecal"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$detail"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$detailscale"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$detailtint"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$detailblendfactor"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$detailblendmode"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)

		else if (!Q_strcasecmp(key, "$surfaceprop2"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$AllowAlphaToCoverage"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$blendmodulatetexture"))
			Q_strlcpy(st->blendmod, value, sizeof(st->blendmod));	//FTESurf Patch 187, see vmtstate_t::blendmod

		//FTESurf Patch 250: the $emissiveblend* family. See vmtstate_t::emisbase.
		else if (!Q_strcasecmp(key, "$emissiveblendenabled"))
			st->emisenabled = !!atoi(value);
		else if (!Q_strcasecmp(key, "$emissiveblendbasetexture"))
			Q_strlcpy(st->emisbase, value, sizeof(st->emisbase));
		else if (!Q_strcasecmp(key, "$emissiveblendflowtexture"))
			Q_strlcpy(st->emisflow, value, sizeof(st->emisflow));
		else if (!Q_strcasecmp(key, "$emissiveblendtexture"))
			Q_strlcpy(st->emistex, value, sizeof(st->emistex));
		else if (!Q_strcasecmp(key, "$emissiveblendtint"))
		{	VMT_ParseVec3(value, st->emistint, 3); st->emistint_set = true; }
		else if (!Q_strcasecmp(key, "$emissiveblendstrength"))
		{	st->emisstrength = atof(value); st->emisstrength_set = true; }
		else if (!Q_strcasecmp(key, "$emissiveblendscrollvector"))
		{	VMT_ParseVec3(value, st->emisscroll, 2); st->emisscroll_set = true; }

		//water/reflection stuff
		else if (!Q_strcasecmp(key, "$REFRACTTINTTEXTURE"))
			Q_strlcpy(st->refracttinttexture, value, sizeof(st->refracttinttexture));
		else if (!Q_strcasecmp(key, "$refracttexture"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		//FTESurf Patch 251: these five stop being discarded -- water.glsl has had
		//the matching #defines all along. See the vmtstate_t note for the census.
		else if (!Q_strcasecmp(key, "$refractamount"))
		{
			st->refractamount = atof(value);
			st->refractamount_set = true;
		}
		else if (!Q_strcasecmp(key, "$refracttint"))
			st->refracttint_set = VMT_ParseColour(value, st->refracttint);
		else if (!Q_strcasecmp(key, "$reflecttexture"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$reflectamount"))
		{
			st->reflectamount = atof(value);
			st->reflectamount_set = true;
		}
		else if (!Q_strcasecmp(key, "$reflecttint"))
			st->reflecttint_set = VMT_ParseColour(value, st->reflecttint);
		else if (!Q_strcasecmp(key, "$fresnelpower"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$minreflectivity"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$maxreflectivity"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$mod2x"))
			st->mod2x = 1;
		else if (!Q_strcasecmp(key, "$forcecheap"))
			st->water_cheap = 1;
		else if (!Q_strcasecmp(key, "$forceexpensive"))
			st->water_expensive = 1;
		else if (!Q_strcasecmp(key, "$normalmap"))
		{
			Q_strlcpy(st->normalmap, value, sizeof(st->normalmap));
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		}
		else if (!Q_strcasecmp(key, "$bumpframe"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$fogenable"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$fogcolor"))
		{
			Q_strlcpy(st->fogcolor, value, sizeof(st->fogcolor));	//FTESurf: hl2_water 0 draws this
			//FTESurf Patch 251: and as floats, for water.glsl's #FOGTINT at
			//hl2_water 1/2. The string form is kept because modes 0 and 3 feed it
			//straight to `rgbgen const` and re-deriving it would be two spellings
			//of one value.
			st->fogcolor_set = VMT_ParseColour(value, st->fogcolor_f);
		}
		/*
		FTESurf Patch 251: $scale, the Water shader's normal-map tiling -> TXSCALE.

		Hammer writes it as "[1 1]", "[.5 .5]" or a bare number, and only the first
		component is ever used here: water.glsl applies TXSCALE1/TXSCALE2 as
		vec2(TXSCALE1)*tc, a single scalar per wave layer, so a material asking for
		different S and T tiling cannot be expressed and the first component is the
		closer of the two answers.

		Parsed inside the Water arm's neighbourhood rather than globally: $scale
		means something different on a VertexlitGeneric (a texture transform), and
		this must not start claiming to implement that.
		*/
		else if (!Q_strcasecmp(key, "$scale") && !Q_strcasecmp(st->type, "Water"))
		{
			const char *s = value;
			while (*s == '[' || *s == '{' || *s == ' ') s++;
			st->waterscale = atof(s);
			st->waterscale_set = (st->waterscale > 0);
		}
		else if (!Q_strcasecmp(key, "$fogstart"))
			st->fogstart = atof(value);	//FTESurf Patch 254: was recognised-and-discarded. It is the near end of the underwater fog.
		else if (!Q_strcasecmp(key, "$fogend"))
			st->fogend = atof(value);	//FTESurf build 12: drives the dither coverage at hl2_water 3
		else if (!Q_strcasecmp(key, "$abovewater"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$underwateroverlay"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$reflectentities"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$scale"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$bottommaterial"))
			Q_strlcpy(st->bottommaterial, value, sizeof(st->bottommaterial));	//FTESurf Patch 233, see VMT_BottomRecord
		else if (!Q_strcasecmp(key, "$scroll1"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)
		else if (!Q_strcasecmp(key, "$scroll2"))
			;	//nettest: recognised-but-ignored Source key; silenced (was per-field developer-1 spam)

		else if (*key == '%')
			;	//editor lines
		else if (VMT_IsKnownIgnoredField(key))
			;	//nettest: known Source-only field FTE intentionally ignores — don't warn
		//FTESurf Patch 250: recorded rather than printed, and split by whether
		//Source itself declares the key. See VMT_RecordKey and `vmt_keys`.
		else if (VMT_IsKnownSourceParam(key))
			VMT_RecordKey(vmt_key_param, &vmt_key_param_n, &vmt_key_param_over, key, fname, "Unimplemented Source");
		else
			VMT_RecordKey(vmt_key_alien, &vmt_key_alien_n, &vmt_key_alien_over, key, fname, "Unknown");
	}
	if (replace)
		VMT_ParseBlock(fname, st, replace);
	return line;
}
/*
FTESurf Patch 300: may this additive material be drawn by a top-level `program`
instead of a pass?

WHY IT WANTS TO BE.  A shader with no TOP-LEVEL program takes
BE_ProglessFogBegin (gl_backend.c), which enables fixed-function GL fog.  That
fog mixes toward GL_FOG_COLOR on RGB and knows nothing about the batch's blend.
On a gl_one/gl_one surface the only mask these textures have is BLACK --
light_glow02/03.vtf are BGR888, no alpha channel at all -- and a black texel
mixed toward the fog colour becomes fogcolour*(1-f), which is then ADDED.  So
the masked-out part of the quad stops being masked and the sprite draws as a
flat translucent rectangle.  Reported on surf_tensor2 as the light sprites
"rendering a big square of translucent color".

A top-level program skips that path entirely (two independent guards, both on
shaderstate.curshader->prog) and reaches sys/fog.h's fog4additive() instead,
which fades toward black -- the physically right answer for an emissive sprite,
and what every program-carrying pass in the engine has always done.

WHY IT IS GATED ON ADDITIVE AND NOT ON CLASS.  The control arm for this change
is surf_boreas's cloud layer, which Patch 266 exists for.  cloods.vmt is
UnlitGeneric, so a class-based gate would have moved it -- but its `$additive`
line is COMMENTED OUT in the VMT (`//"$additive" 1`); it is $translucent, i.e.
src_alpha/one_minus_src_alpha.  Keying on st->additive means those clouds never
enter this branch at all, which is a stronger guarantee than a screenshot.

THE TWO DECLINES are the only things a PASS can name that a top-level
`diffusemap` cannot, and both were counted before being written off rather than
assumed rare: across all 1,312 library maps plus the four shared packs, of 1,659
additive UnlitGeneric rows exactly ONE uses `map $whiteimage`
(df_precision2's sharq-liquids/water1) and ZERO use an HDR sky face.  They stay
on the pass, keep today's behaviour, and are counted in vmt_stat_addpass so the
census says so instead of the number quietly being wrong.
*/
static qboolean VMT_AdditiveProgramOK(vmtstate_t *st)
{
	if (!st->additive)
		return false;
	if (hl2_translucent && !hl2_translucent->ival)
		return false;	//forced opaque -- it is not an additive surface any more, and #ADDITIVE was not emitted either
	if (hl2_additivefog && !hl2_additivefog->ival)
		return false;
	if (st->nobasetex)
		return false;	//`map $whiteimage`
	if (*st->hdrbasetex || *st->hdrcompressedtex)
		return false;	//`map "$hdr:..."`
	return true;
}

/*
FTESurf Patch 300: the same question for a Sprite, which cannot be asked the same
way -- AND THIS IS THE ONE THAT DECIDES WHETHER THE PATCH REACHES THE REPORTED BUG.

A Sprite is additive in TWO ways, and the reported case is the second:

    if      (st->additive   && blendok)  blendFunc add
    else if (st->translucent && blendok)  blendFunc blend
    else if (!st->alphatest)              blendFunc add   <-- "no opinion in the
                                                              VMT: keep what shipped"

surf_tensor2's 233 lens flares are materials/sprites/light_glow03.vmt, whose
entire contents are $spriteorientation, $spriteorigin and $basetexture.  It sets
NO $additive.  So st->additive is 0, it is additive purely by falling to the
bottom of that chain, and a gate written as `if (st->additive)` -- which is the
correct gate for UnlitGeneric -- would have declined every single one of them and
shipped a fix that left the actual complaint on screen.

Worth being explicit about how close that was: the UnlitGeneric gate is right for
light_glow02_add_noz.vmt, which is the env_lightglow material and DOES say
$additive 1.  There are 166 of those and 263 of the others.  A patch gated only
on st->additive would have fixed 39% of the map's sprites and looked, from a
screenshot, like it had half worked.

The two DECLINES here are the branches that are genuinely not additive: a
$translucent sprite (alpha blend), and a pure cutout, which takes the
`!st->alphatest` test and so emits no blendFunc at all.
*/
static qboolean VMT_SpriteAdditiveProgramOK(vmtstate_t *st)
{
	if (hl2_translucent && !hl2_translucent->ival)
		return false;	//the tail declines to write progblendfunc, so a program would have no blend
	if (hl2_additivefog && !hl2_additivefog->ival)
		return false;
	if (st->translucent && !st->additive)
		return false;	//blendFunc blend
	if (st->alphatest && !st->additive)
		return false;	//cutout: no blendFunc at all
	if (st->nobasetex || *st->hdrbasetex || *st->hdrcompressedtex)
		return false;
	return true;
}

/*
FTESurf Patch 300: the vertex-colour arguments for a material moving from a pass
to vmt/unlit.

THIS IS THE SUBTLE HALF OF THE PATCH, and getting it wrong would have undone
build 58's sprite-brightness work in the least obvious way possible.

Both arms that this serves emit `rgbGen vertex` WITHOUT an `alphaGen` line -- the
Sprite arm unconditionally (:3136), UnlitGeneric whenever $vertexcolor is set.
That looks like "colour per vertex, alpha untouched".  It is not:
gl_shader.c:4199-4203 defaults alphagen to ALPHA_GEN_VERTEX whenever rgbgen is
vertex and alphagen is still unspecified.  So those passes have been consuming
per-vertex ALPHA all along, silently.

That alpha is not decoration.  It is how a sprite's brightness reaches the
rasteriser: the CSQC draws pass DRAWFLAG_ADD, BEF_FORCEADDITIVE rewrites the
blend to GL_SRC_ALPHA/GL_ONE (gl_backend.c:3289-3293), and the src factor IS the
per-vertex alpha that cl_sprites.qc computes from rendermode/renderamt and the
inverse-square fade.  Emit #VERTEXCOL without #VERTEXALPHA and every sprite on
the map draws at full brightness regardless of what the entity asked for.

Hence: if the pass would have said `rgbGen vertex`, the program gets BOTH.  Not
gated on hl2_vertexcolor either -- that cvar governs the painted displacement
colour on lit world materials (Patch 232), and switching it off must not silently
turn every additive sprite up to full.
*/
static void VMT_AddVertexColourArgs(char *progargs, size_t progargssize)
{
	Q_strlcat(progargs, "#VERTEXCOL", progargssize);
	Q_strlcat(progargs, "#VERTEXALPHA", progargssize);
}

static void Shader_GenerateFromVMT(parsestate_t *ps, vmtstate_t *st, const char *shortname, void (*LoadMaterialString)(parsestate_t *ps, const char *script))
{
	size_t offset = 0;
	char script[8192];
	char envmaptint[128];
	char envmapsat[128];
	char envmapcontrast[64];	//FTESurf Patch 268 B, filled beside the two above
	/*
	FTESurf Patch 187: progargs is a BUFFER now, not a pointer to a literal.

	It was `char *progargs` and every feature ASSIGNED to it, so the last one to
	run won and the others were silently dropped -- `$nofog` on an `$alphatest`
	material threw the alpha mask away, which is a hole a surface falls straight
	through.  Appending is what the callers always meant; the permutation string
	is `#`-separated and the shaders test each define independently.

	Water and Refract still REPLACE rather than append, exactly as before: their
	modes are whole-program selections and an alpha mask on a water plane means
	nothing.
	*/
	char progargs[256];

	*progargs = 0;

	/*
	FTESurf Patch 190: the default $basetexture must not carry the material's
	own extension, and that is where surf_demise's twelve console lines came
	from.

	    Unable to load file materials/models/surf_demise/ramp7/sky_demise_05.vmt
	                                                          (format unsupported)

	The material is `UnlitGeneric { $envmap … $model 1 $nofog 1 }` -- no
	$basetexture at all.  mod_hl2.c:491 names model skins WITH a .vmt extension
	and gl_shader.c:8263 passes that name through verbatim, so the fallback
	below produced "…/sky_demise_05.vmt", the emit appended ".vtf", and
	R_LoadHiResTexture stripped the extension it recognised (image.c:14461) and
	then probed the EMPTY extension last (image.c:14193) -- landing back on the
	.vmt itself, which exists, loads, and is not a picture.

	Strip whichever of the two we were handed.  A material name is never a
	texture name with an extension on it.
	*/
	if (!*st->tex[0].name)	//fill in a default...
	{
		size_t l;
		Q_strlcpy(st->tex[0].name, shortname, sizeof(st->tex[0].name));
		l = strlen(st->tex[0].name);
		if (l > 4 && (!Q_strcasecmp(st->tex[0].name+l-4, ".vmt") || !Q_strcasecmp(st->tex[0].name+l-4, ".vtf")))
			st->tex[0].name[l-4] = 0;
		/*
		And an envmap-only material never had a base texture to guess at.  The
		material name is not one either -- it just happens to resolve to the VMT
		we are reading.  $whiteimage is honest about having nothing: a flat
		surface tinted by whatever else the material says, rather than the
		missing-texture checkerboard.

		Tested BEFORE the hl2_envmap switch below clears st->envmap, deliberately:
		the question is what the material was AUTHORED as, not whether we are
		currently honouring its cubemap.  At hl2_envmap 0 that leaves a flat white
		surface, which is still a better answer than a checkerboard.

		KNOWN LIMITATION: what Source actually draws here is the cubemap.  This
		arm emits a PASS, and a pass cannot sample s_reflectcube -- only the
		program paths can -- so doing it properly means moving envmap-only
		UnlitGeneric onto a program, which changes lighting for every other
		material of that type.  Not done here.

		FTESurf Patch 263 drops the UnlitGeneric test that used to gate this.

		The gate was not protecting anything -- it was describing the ONE class
		the author had in front of him.  Read the condition on its own terms:
		"this material names an $envmap and no $basetexture, so the material's
		own path is not a texture name."  Nothing in that sentence is about
		UnlitGeneric, and the failure it prevents is the same for every class.

		WindowImposter is the proof.  materials/fakeskies/mpa45.vmt is

		    WindowImposter { $envmap "fakeskies/mpa45"  $nofog 1 }

		-- envmap, no basetexture, and NOT UnlitGeneric, so it walked straight
		past this test.  st->tex[0].name was filled with "fakeskies/mpa45", the
		unknown-class fallback emitted `diffusemap materials/fakeskies/mpa45.vtf`,
		and that file exists: it is the CUBEMAP, which img_vtf.c loads as
		PTI_CUBE (`vtf->flags & 0x4000`).  A cube texture is not a loaded 2D
		base, so gl_backend.c's T_GEN_DIFFUSE substituted missing_texture and
		surf_monolith's bonus 4 drew as the notexture checkerboard.

		The arm added below now handles that class properly, so this line is no
		longer what saves it -- but it is what saves the next one.  Widening it
		cannot regress a material that works today: it only fires where there is
		no $basetexture at all, and $whiteimage is strictly better than a name
		that was never a texture.
		*/
		if (*st->envmap)
			st->nobasetex = 1;
	}

	/*
	FTESurf Build 13: the feature switches, applied HERE rather than at each of
	the dozen emit sites.

	Clearing the parsed field is what actually turns the feature off, because
	every downstream decision -- which #ENVFROM* permutation to pick, whether to
	emit a reflectcube, whether to emit a normalmap -- is already written as
	`if (*st->envmap)` / `if (*st->normalmap)`.  One place to change, and no way
	for a shader to end up asking for a sampler that was never bound, which is
	precisely the failure this build spent its morning on.

	Water and Refract keep their normal map regardless: theirs is not a lighting
	detail, it is the geometry the refraction is perturbed BY, and those paths
	emit it unconditionally.  Turning it off there would write "materials/.vtf".
	*/
	if (hl2_envmap && !hl2_envmap->ival)
		st->envmap[0] = st->envmapmask[0] = 0;
	if (hl2_bumpmap && !hl2_bumpmap->ival &&
		Q_strcasecmp(st->type, "Water") && Q_strcasecmp(st->type, "Refract"))
		st->normalmap[0] = 0;

	/*
	FTESurf Patch 268 B: the Source-parity permutations, and the env_cubemap flag.

	HERE, after the hl2_envmap kill switch above -- that switch clears st->envmap,
	so hl2_envmap 0 emits none of this -- and BEFORE the type dispatch, where
	st->type is still the VMT's OWN shader class; every arm below overwrites it
	with a "vmt/..." program name.

	Restricted to the three LIT classes whose programs sample s_reflectcube:
	VertexlitGeneric (plus the decal materials its arm claims, tested exactly the
	way that arm tests them), LightmappedGeneric and WorldVertexTransition.
	Water, Refract and the unlit classes are deliberately excluded -- their
	REFLECTCUBEMASK blocks were not audited for this patch, and hl2_water /
	hl2_refract own those materials.

	These three are BARE permutation flags, so they must only reach a program that
	declares them (Patch 236's silent load failure).  The VALUE defines (#ENVTINT
	and friends) go on at the emit sites instead, where the program is known.
	*/
	{
		/*
		The three class NAMES only.

		The first draft also carried the `st->decal && ...` disjunct copied out of
		the VertexlitGeneric arm's own condition, on the theory that whatever that
		arm claims should be scoped the same way.  That is wrong here and three
		reviewers caught it: the arm is the LAST `else if` of the dispatch chain, so
		its decal clause only ever sees materials no earlier arm took, whereas this
		test runs BEFORE the chain and sees every class.  `Q_strcasecmp(type,
		"LightmappedGeneric")` is non-zero for `Water`, so a `Water` material with
		`$decal 1` and `$envmap "env_cubemap"` would have been handed the sentinel
		and the #ENVSRC flags -- and Water is the one excluded class whose program
		really does sample s_reflectcube.

		Cost of the narrower test: a $decal material of some OTHER class that would
		have landed on the vertexlit arm does not get the sentinel.  That is the
		safe direction, and decals carrying env_cubemap are not a population this
		patch was measured against.
		*/
		qboolean envlitclass =
			!Q_strcasecmp(st->type, "VertexlitGeneric") ||
			!Q_strcasecmp(st->type, "LightmappedGeneric") ||
			!Q_strcasecmp(st->type, "WorldVertexTransition");
		//...and of those three, only vertexlit.glsl implements the post-lighting add.
		qboolean envpostclass = !Q_strcasecmp(st->type, "VertexlitGeneric");

		if (*st->envmap && envlitclass)
		{
			if (hl2_envmap_source && (hl2_envmap_source->ival & 1))
				Q_strlcat(progargs, "#ENVSRCMASK", sizeof(progargs));
			if (hl2_envmap_source && (hl2_envmap_source->ival & 2))
				Q_strlcat(progargs, "#ENVSRCSPEC", sizeof(progargs));
			//Only where a shader tests it.  lightmapped.glsl and transition.glsl
			//already add the reflection after their lightmap multiply, so bit 4 has
			//nothing to do there -- appending it anyway would mint a second,
			//byte-identical program permutation for every world envmap material.
			if (envpostclass && hl2_envmap_source && (hl2_envmap_source->ival & 4))
				Q_strlcat(progargs, "#ENVSRCPOST", sizeof(progargs));

			//The literal "env_cubemap" means "the nearest baked one", which is
			//curbatch->envmap -- see the sentinel emit in the shared tail.
			if (!strcmp(st->envmap, "env_cubemap") && hl2_envcubemap && hl2_envcubemap->ival)
				st->wantenvcube = true;
		}
	}

	/*
	FTESurf Patch 268 C: per-pixel relief on a bumped VertexLitGeneric.

	Model lighting in vertexlit.glsl is per VERTEX, from the vertex normal, and the
	normal map is sampled only inside the REFLECTCUBEMASK block -- so a prop with a
	$bumpmap and no $envmap (surf_fantasy's island rocks: `$basetexture fan/moss
	$bumpmap fan/moss_n`, nothing else) samples no normal map at all and draws flat.
	Source lights that prop per PIXEL: the leaf's six-face ambient cube evaluated
	with the bumped world-space normal (common_vertexlitgeneric_dx9.h).

	#BUMPCUBE does that as a RATIO over the lighting FTE already has,
	    light *= cube(n_bumped) / cube(n_vertex)
	so VRAD's per-vertex bake keeps its level and its shadows and the cube adds only
	the per-pixel direction.  A flat normal makes the ratio exactly 1, and so does an
	empty cube -- which is what r_cubelight 0 uploads, and what every entity the
	plugin did not light carries.

	#BUMPGREENFIX drops the green flip.  Its own switch because it also bends the
	Patch 268 B reflection; see vertexlit.glsl.

	Appended only where it fits whole: a truncated define would silently compile a
	different program than its name claims (see vmt_stat_waterargs above).
	*/
	if (!Q_strcasecmp(st->type, "VertexlitGeneric") && *st->normalmap)
	{
		if (hl2_cubelight && hl2_cubelight->ival && strlen(progargs) + 9 < sizeof(progargs))
			Q_strlcat(progargs, "#BUMPCUBE", sizeof(progargs));
		if (hl2_bumpgreenfix && hl2_bumpgreenfix->ival && strlen(progargs) + 13 < sizeof(progargs))
			Q_strlcat(progargs, "#BUMPGREENFIX", sizeof(progargs));
	}

	/*
	FTESurf Patch 237: $alphatestreference, which was parsed and then never read.

	`st->alphatestref` is filled at :789-790 and had no other reference in the
	file -- the cutoff was hardcoded 0.5 here and GE128 in the UnlitGeneric pass
	arm.  Source's default is 0.5, so the constant was right for materials that
	stay silent and wrong for every material that bothers to say otherwise.

	surf_rise is why this shows: every fence and grate on it asks for 0.3 --

	    LightmappedGeneric { $basetexture "soniccolours/pla_metal_my1_fence01_dif"
	                         $alphatest 1   $alphatestreference 0.3   ... }

	-- so a wire authored to survive down to alpha 0.3 was being cut at 0.5.
	That erodes the thin parts of a chainlink and opens holes in the mesh, which
	is the "not masked correctly" half of the report on that map.

	Clamped rather than trusted: a material writing 0 would discard nothing and
	one writing >= 1 would discard everything, and neither is a thing a mapper
	means by an alpha-test reference.
	*/
	if (st->alphatest)
	{
		char maskarg[64];
		float mask = (st->alphatestref > 0.0f) ? st->alphatestref : 0.5f;
		if (mask < 0.01f) mask = 0.01f;
		if (mask > 0.99f) mask = 0.99f;
		//alphamask has to be handled by glsl (when glsl is used)
		Q_snprintfz(maskarg, sizeof(maskarg), "#MASK=%f#MASKLT", mask);
		Q_strlcat(progargs, maskarg, sizeof(progargs));
	}

	if (st->nofog)
		Q_strlcat(progargs, "#NOFOG", sizeof(progargs));

	/*
	FTESurf Patch 300: #ADDITIVE, so a program can fog itself correctly.

	sys/fog.h has had the right function all along -- fog4additive() is
	`c * vec4(fac,fac,fac,1.0)`, i.e. fade toward BLACK -- and every
	program-carrying pass in the ENGINE already calls the right variant.  What no
	vmt program could do until now is find out that it is additive, because the
	arms that know it emitted a pass and never a program.

	Gated on hl2_translucent exactly as the blendfunc is (:3913 and the pass arms
	above).  At hl2_translucent 0 an additive material is forced OPAQUE, and an
	opaque surface must fog toward the fog colour like any other -- fading it to
	black instead would make that switch darken the world rather than simplify it.
	Reading the same condition in both places is what keeps those two in step.
	*/
	if (st->additive && (!hl2_translucent || hl2_translucent->ival))
		Q_strlcat(progargs, "#ADDITIVE", sizeof(progargs));

	/*
	FTESurf Patch 188: $color/$color2 as a program define, because the emit this
	replaces has never done anything.

	The old code wrote `rgbGen const <value>` at the TOP LEVEL of the generated
	shader (see the bottom of this function, now deleted).  `rgbgen` is
	registered only in shaderpasskeywords (gl_shader.c:4615) and these materials
	emit a `program` with no pass at all, so it was parsed as an unknown
	top-level directive -- gl_shader.c:5586 has been listing "rgbgen" among the
	Source keywords that "leak to the top level of the generated shader
	(harmless no-ops there)" the whole time.  It also passed the value through
	VERBATIM, brackets included, so even inside a pass `rgbGen const {400 380
	370}` would not have parsed.

	e_colourident cannot carry it either: that is the ENTITY's colormod, one
	value for the whole entity, and this is a per-material constant.

	So it goes where ENVTINT/ENVSAT already go.  Cost is one shader permutation
	per distinct colour; measured on the worst map in the library
	(surf_unrequited) that is 52 across 275 materials.
	*/
	{
		const char *cval = *st->color2 ? st->color2 : st->color;
		float c[3] = {1,1,1};
		qboolean havecolour = (*cval && VMT_ParseColour(cval, c));
		qboolean havealpha = (st->alphaval > 0 && st->alphaval < 1);
		char t[128];

		/*
		FTESurf Patch 187: $alpha, which had nowhere to live on a program
		material (the note on the $alpha key above says so).

		An ADDITIVE surface has no use for it as an alpha -- src_one never reads
		the source alpha -- so there it scales the COLOUR instead, which is what
		Source does and what makes $color [0.2 0.2 0.2] + $alpha 0.8 come out as
		a dim additive smoke rather than a bright one.
		*/
		if (havealpha && st->additive && (!hl2_translucent || hl2_translucent->ival))
		{
			c[0] *= st->alphaval; c[1] *= st->alphaval; c[2] *= st->alphaval;
			havecolour = true;
			havealpha = false;
		}

		if (havecolour)
		{
			Q_snprintfz(t, sizeof(t), "#COLOR=%f,%f,%f", c[0], c[1], c[2]);
			Q_strlcat(progargs, t, sizeof(progargs));
		}
		if (havealpha)
		{
			Q_snprintfz(t, sizeof(t), "#ALPHA=%f", st->alphaval);
			Q_strlcat(progargs, t, sizeof(progargs));
		}
	}

	/*
	FTESurf Patch 189: TextureScroll -> #SCROLL=dx,dy.

	Source gives a RATE (texture units per second) and an ANGLE in degrees; the
	shader wants a velocity, so the trig happens once here rather than per
	fragment.  UnlitGeneric does not take this -- it emits a real pass and gets
	the engine's own `tcMod scroll`, which is the same thing without a
	permutation.
	*/
	if (st->scrollrate != 0 && VMT_ScrollsBase(st->scrollvar))
	{
		double a = st->scrollangle * (3.14159265358979323846/180.0);
		char t[128];
		Q_snprintfz(t, sizeof(t), "#SCROLL=%f,%f", st->scrollrate*cos(a), st->scrollrate*sin(a));
		Q_strlcat(progargs, t, sizeof(progargs));
	}

	/*
	FTESurf Patch 232: $vertexcolor / $vertexalpha -> #VERTEXCOL / #VERTEXALPHA.

	Both keys have been parsed since forever (see the $vertexcolor case above) and
	then USED by exactly one material type: UnlitGeneric, which emits a real pass and
	can say `rgbGen vertex` / `alphaGen vertex`.  Every other type emits a top-level
	`program` with no pass, where those keywords have nothing to attach to -- so for
	LightmappedGeneric and WorldVertexTransition the material asked for per-vertex
	colour and opacity and was silently given neither.  Same reason $alpha needed
	#ALPHA in Patch 187 and $color needed #COLOR in Patch 188; same remedy.

	This is a no-op everywhere except DISPLACEMENTS.  mod_vbsp.c writes the painted
	per-vertex alpha only for displacement surfaces (:2361); every other surface is
	handed a hardcoded white with alpha 1.0 (:2432), and multiplying by that changes
	nothing.  Which is precisely the reported symptom: surf_rise's ivy is a
	displacement whose material is `$translucent 1 $vertexcolor 1 $vertexalpha 1`,
	painted to fade from opaque to transparent across the surface, and the fade was
	being thrown away.  VBSP's own _wvt_patch clone of that material -- what Source
	itself draws on the brush faces -- is a LightmappedGeneric that keeps both keys,
	which is the confirmation that vertex alpha here means opacity.

	Restricted to these two types deliberately.  The other arms either consume the
	keys already (UnlitGeneric) or have no vertex stream worth reading, and widening
	it would spend permutations to multiply by 1.0.

	The one thing that could have made this ambiguous does not occur: in a transition
	material vertex alpha is ALSO the blend factor between $basetexture and
	$basetexture2, so a material wanting both would be asking one channel to do two
	jobs.  A census of the map-embedded VMTs in all 1362 maps finds 41 transition
	materials with $vertexalpha and no $basetexture2 (20 maps, surf_rise and
	surf_demise among them), 494 LightmappedGeneric ones (111 maps), and ZERO that
	carry $vertexalpha alongside a real $basetexture2.
	*/
	if ((st->vertexcolor || st->vertexalpha) &&
	    (!hl2_vertexcolor || hl2_vertexcolor->ival))
	{
		if (!Q_strcasecmp(st->type, "WorldVertexTransition") ||
		    !Q_strcasecmp(st->type, "LightmappedGeneric"))
		{
			if (st->vertexcolor)
				Q_strlcat(progargs, "#VERTEXCOL", sizeof(progargs));
			if (st->vertexalpha)
				Q_strlcat(progargs, "#VERTEXALPHA", sizeof(progargs));
		}
	}

	/*
	FTESurf Patch 251: $seamless_scale.

	Emitted HERE, beside #VERTEXCOL, because it is the same shape of decision --
	one key, two material types, one permutation each -- and because both
	lightmapped.glsl and transition.glsl now carry the projection.  Restricting it
	to those two is not a simplification: they are the only arms whose shader has
	the world position and normal to project from.  The 3 VertexlitGeneric
	materials in the census that also write the key are models, where Source's own
	SKIP list keeps SEAMLESS out of the vertexlit path anyway.

	> 0 rather than != 0: the scale multiplies world position, so zero would
	collapse every projection onto one texel of the texture -- a flat colour
	stretched over the whole surface.  A material that writes 0 means "off", and
	falling through to the material's UVs is what off looks like.
	*/
	if (st->seamless_set && st->seamless > 0 &&
	    (!hl2_seamless || hl2_seamless->ival))
	{
		if (!Q_strcasecmp(st->type, "WorldVertexTransition") ||
		    !Q_strcasecmp(st->type, "LightmappedGeneric"))
		{
			char t[64];
			Q_snprintfz(t, sizeof(t), "#SEAMLESS=%f", st->seamless);
			Q_strlcat(progargs, t, sizeof(progargs));
			vmt_stat_seamless++;
		}
	}

	/*
	FTESurf Patch 237: ask whether the key was WRITTEN, not whether it is non-zero.

	`envmaptint_b > 0.0f` cannot tell "the mapper said nothing" from "the mapper
	said none", and those are opposite instructions.  With no #ENVTINT emitted
	the shader falls back to its own `#ifndef ENVTINT #define ENVTINT 1.0,1.0,1.0`
	(lightmapped.glsl:62-64, vertexlit.glsl:62-64) and `diffuse_f.rgb += cube_t *
	refl` (lightmapped.glsl:227) adds the cubemap at FULL WHITE -- so a material
	asking for no reflection got the strongest possible one.

	surf_rise is built out of them:

	    soniccolours/sonicfence.vmt   $envmap "env_cubemap"  $envmaptint "[0 0 0]"
	    soniccolours/sonicfence2.vmt  $envmap "env_cubemap"  $envmaptint "[0 0 0]"

	and its cubemap is its own sky (skybox/surfrisesky2*), which is what makes
	the wash BLUE.  The material next door, sonicwallgratea, writes
	"[0.02 0.01 0.01]" -- b is non-zero, so it kept its tint and rendered
	correctly, which is exactly why this looked like "some surfaces and not
	others" rather than like a bug in one place.

	Same reasoning for $envmapsaturation 0, which means "fully desaturate the
	reflection" and was reading as "no saturation key, use 1.0".  It also used
	the RED component to make a decision about all three.
	*/
	if (st->envmaptint_set) {
		Q_snprintfz(envmaptint, sizeof(envmaptint), "#ENVTINT=%f,%f,%f", st->envmaptint_r, st->envmaptint_g, st->envmaptint_b);
	} else {
		Q_snprintfz(envmaptint, sizeof(envmaptint), "");
	}

	if (st->envmapsat_set) {
		Q_snprintfz(envmapsat, sizeof(envmapsat), "#ENVSAT=%f,%f,%f", st->envmapsat_r, st->envmapsat_g, st->envmapsat_b);
	} else {
		Q_snprintfz(envmapsat, sizeof(envmapsat), "");
	}

	/*
	FTESurf Patch 268 B: $envmapcontrast, emitted only under bit 2.

	The contrast only means anything inside Source's own tint->contrast->saturation
	order, which is what bit 2 installs; handing the shader a contrast while it is
	still doing FTE's saturation-then-tint would be a third arithmetic that is
	neither engine's.  With the bit off this stays empty and every program name is
	the string it is today.
	*/
	if (st->envmapcontrast_set && hl2_envmap_source && (hl2_envmap_source->ival & 2))
		Q_snprintfz(envmapcontrast, sizeof(envmapcontrast), "#ENVCONTRAST=%f", st->envmapcontrast);
	else
		*envmapcontrast = 0;

	Q_strlcatfz(script, &offset, sizeof(script), "\n");

	if (st->nodraw)
	{
		Q_strlcatfz(script, &offset, sizeof(script), "\tsurfaceparm nodraw\n");
	}
	/*
	FTESurf Patch 195: an AnimatedTexture flipbook, when it is switched on.

	FIRST in the chain, immediately after nodraw, and that position is the whole
	of the fix that made it work.  It was originally written next to the
	LightmappedGeneric arm, which is unreachable for the material that motivated
	it: surf_demise's elly/animated_smoke is a WorldVertexTransition, whose arm
	comes 400 lines earlier and had already claimed it.  The test dumped
	`program "vmt/transition#...#NOBLEND"` twice before that was obvious.

	Taken only for a proxy that names the base texture (or names nothing, which
	every observed one that omits the key means) -- the same rule VMT_ScrollsBase
	applies to the scroll, so a proxy pointed at some other var is read and then
	declined rather than applied to the wrong map.

	A PASS, not a top-level program, because `map "$2darray:..."` is what asks the
	loader for every frame (gl_shader.c:1022 -> IF_TEXTYPE_2D_ARRAY), and that is
	a pass keyword.  The pass fills sampler slot 0, which is what
	`!!samps anim:2DArray=0` declares -- the same binding vertexlit.glsl already
	uses for rtenvsphere.  s_lightmap is a default sampler and is bound whether
	there is a pass or not, so the lightmap survives.

	OFF BY DEFAULT.  3,342 materials across 741 maps carry this proxy, so turning
	it on moves a large fraction of the library's world materials onto a shader
	that has never rendered anything before, and onto a GLSL 130 context.  That
	is not a change to make silently on someone's behalf; see hl2_animated.
	*/
	else if (hl2_animated && hl2_animated->ival && st->animrate > 0 &&
	         VMT_ScrollsBase(st->animvar) && *st->tex[0].name &&
	         (!Q_strcasecmp(st->type, "LightmappedGeneric") || !Q_strcasecmp(st->type, "UnlitGeneric") ||
	          /*
	          A WorldVertexTransition with no $basetexture2 is a lightmapped
	          material wearing the wrong type name -- Patch 187 already draws it
	          with #NOBLEND, sampling s_diffuse alone.  So it can animate, and it
	          has to: elly/animated_smoke is exactly that shape (331 frames,
	          24fps, one texture).  A transition that DOES carry a second texture
	          is left alone, because vmt/animated has no blend and quietly
	          dropping the crossover would be worse than not animating.
	          */
	          (!Q_strcasecmp(st->type, "WorldVertexTransition") && !*st->tex[1].name)))
	{
		char anim[64];
		/*
		FTESurf Patch 253: read the type BEFORE it is overwritten below.

		UnlitGeneric is 456 of the 927 materials this arm actually claims across
		the library -- 49% -- and the arm two hundred lines down (:1979) draws an
		unclaimed one as a plain PASS with no program and no lightmap, because
		unlit means unlit.  Routing it here without saying so handed it to a shader
		that multiplied by a lightmap, and `!!samps lightmap` is not a passive
		declaration: it sets SHADER_HASLIGHTMAP, which is the flag
		Mod_LightmapAllocSurf gates on, so the surface was given a real page with
		the room's real lighting and every animated sign and screen in the library
		would have gone dark the moment the default moved.  That would have read as
		"the flipbook is too dim", not as a lighting bug, and it is exactly the
		class of thing a screenshot of the smoke could never show.
		*/
		qboolean animunlit = !Q_strcasecmp(st->type, "UnlitGeneric");
		Q_snprintfz(anim, sizeof(anim), "#ANIMRATE=%f", st->animrate);
		Q_strlcat(progargs, anim, sizeof(progargs));
		if (animunlit)
		{
			Q_strlcat(progargs, "#UNLIT", sizeof(progargs));
			vmt_stat_animunlit++;
		}
		Q_strlcpy(st->type, "vmt/animated", sizeof(st->type));
		Q_strlcatfz(script, &offset, sizeof(script),	"\t{\n");
		Q_strlcatfz(script, &offset, sizeof(script),	"\t\tprogram \"%s%s\"\n", st->type, progargs);
		Q_strlcatfz(script, &offset, sizeof(script),	"\t\tmap \"$2darray:%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
		/*
		THE BLEND HAS TO BE THE PASS'S, and getting this wrong is what the first
		screenshot showed.  `progblendfunc` -- emitted at the bottom of this
		function -- applies to a TOP-LEVEL program; with the program inside a pass
		there is nothing for it to attach to, so the surface came out opaque and
		surf_demise's smoke turned into a dark slab across the valley, hiding the
		mountains it is supposed to drift in front of.

		So the same decision is made here in the pass's own spelling, and
		animpass suppresses the top-level emit.  Deliberately mirrors the two
		blocks further down rather than reaching for their result, because
		st->blendfunc is not assigned until well after this dispatch runs; the
		coupling is noted at both ends.
		*/
		if (st->additive && (!hl2_translucent || hl2_translucent->ival))
			Q_strlcatfz(script, &offset, sizeof(script),	"\t\tblendFunc add\n");
		else if (st->translucent && (!hl2_translucent || hl2_translucent->ival))
			Q_strlcatfz(script, &offset, sizeof(script),	"\t\tblendFunc blend\n");
		Q_strlcatfz(script, &offset, sizeof(script),	"\t}\n");
		st->animpass = 1;
		vmt_stat_animated++;
	}
	/*
	FTESurf Patch 250: $emissiveblendenabled -- the scrolling emissive pass.

	Placed here, ahead of the type dispatch, because it REPLACES the material
	rather than decorating it: the six materials in the library that use it have
	no $basetexture at all (see vmtstate_t::emisbase), so routing them to the
	VertexlitGeneric arm below gets a material with nothing to sample.  That is
	what they have always done, and it is why surf_demise's summoning circles and
	its two credits banners have been invisible.

	The defaults are Source's own, from emissive_scroll_blended_pass_helper.cpp:
	scroll [0.11 0.124] (kDefaultEmissiveScrollVector), strength 1.0, tint white.
	Applied when the key is ABSENT rather than when it is zero -- a strength of 0
	is a legitimate authoring choice meaning "off this frame", and Source honours
	it by skipping the pass (helper:72).
	*/
	else if (hl2_emissive && hl2_emissive->ival && st->emisenabled &&
	         *st->emistex && *st->emisbase && *st->emisflow)
	{
		char t[192];
		float sx = st->emisscroll_set ? st->emisscroll[0] : 0.11f;
		float sy = st->emisscroll_set ? st->emisscroll[1] : 0.124f;
		float str = st->emisstrength_set ? st->emisstrength : 1.0f;
		float tr = st->emistint_set ? st->emistint[0] : 1.0f;
		float tg = st->emistint_set ? st->emistint[1] : 1.0f;
		float tb = st->emistint_set ? st->emistint[2] : 1.0f;

		Q_snprintfz(t, sizeof(t), "#SCROLL=%f,%f#TINT=%f,%f,%f#STRENGTH=%f", sx, sy, tr, tg, tb, str);
		Q_strlcat(progargs, t, sizeof(progargs));
		Q_strlcpy(st->type, "vmt/emissive", sizeof(st->type));

		Q_strlcatfz(script, &offset, sizeof(script),	"\tprogram \"%s%s\"\n", st->type, progargs);
		Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->emisbase, "materials/")?"materials/":"", st->emisbase);
		Q_strlcatfz(script, &offset, sizeof(script),	"\tlowermap \"%s%s.vtf\"\n", strcmp(st->emisflow, "materials/")?"materials/":"", st->emisflow);
		//The scrolling texture rides the fullbright slot, which is the self-illum
		//one and semantically exactly right.  Emitted by the shared tail below, so
		//it is set rather than written here -- these materials never set
		//$selfillummask, so there is nothing to overwrite.
		Q_strlcpy(st->fullbrightmap, st->emistex, sizeof(st->fullbrightmap));
		//Source's pass is unconditionally ONE ONE. These materials all say
		//$additive anyway, but the shader is additive whatever the material said.
		st->additive = 1;
		vmt_stat_emissive++;
	}
	/*
	FTESurf Patch 300: an ADDITIVE UnlitGeneric goes onto vmt/unlit rather than a
	pass, so that it fogs toward black instead of toward the fog colour.  See
	VMT_AdditiveProgramOK above for why, and for why the gate is st->additive and
	not the class name.

	Nothing is lost by leaving the pass, and three things are regained.  The pass
	arm's other keywords all have program spellings that mat_vmt.c HAS ALREADY
	BUILT by this point and was throwing away, because this arm emitted no
	program to carry them:
	    tcMod scroll   -> #SCROLL   (:2414, Patch 189)
	    rgbGen const   -> #COLOR    (:2371, Patch 188)
	    alphaFunc      -> #MASK/#MASKLT, and at the material's exact
	                      $alphatestreference rather than quantised to ge128/gt0
	    $nofog         -> #NOFOG    (:2346) -- dead on this arm until now
	    rgbGen vertex  -> #VERTEXCOL/#VERTEXALPHA (see VMT_AddVertexColourArgs)

	TWO BEHAVIOUR CHANGES RIDE ALONG, both of them the VMT finally being obeyed,
	and both stated here rather than discovered later:

	1. $nofog starts working.  It is appended to progargs for every class and
	   this arm never had a program to read it, so a material asking not to be
	   fogged was fogged anyway.  surf_boreas's alch_symbols_alpha is one.

	2. $alphatest starts working -- 108 rows across the library.  Today it is
	   unreachable on an additive material because the pass arm's chain is
	   `if additive ... else if translucent ... else if alphatest`, so the
	   discard never emitted.  The visible delta is bounded and small by
	   construction: on an ADDITIVE surface a discarded fragment and a black
	   fragment are the same pixel, so this can only change texels where the
	   author put visible colour inside a region they also marked transparent.

	$color and $vertexcolor both apply here, where the pass could only have one
	rgbgen and had to pick.  That is Source's own behaviour, not a liberty.
	*/
	else if (!Q_strcasecmp(st->type, "UnlitGeneric") && VMT_AdditiveProgramOK(st))
	{
		if (st->vertexcolor)
			VMT_AddVertexColourArgs(progargs, sizeof(progargs));
		Q_strlcpy(st->type, "vmt/unlit", sizeof(st->type));
		Q_strlcatfz(script, &offset, sizeof(script),	"\tprogram \"%s%s\"\n", st->type, progargs);
		Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
		//The blend itself still comes from the shared tail: st->additive makes it
		//write `progblendfunc add` (:3913), which is the top-level spelling and the
		//only pass-state directive a program with no pass block can use.
		vmt_stat_addprog++;
	}
	else if (!Q_strcasecmp(st->type, "UnlitGeneric"))
	{
		if (st->additive)
			vmt_stat_addpass++;	//additive but declined above -- $whiteimage or an HDR face
		Q_strlcatfz(script, &offset, sizeof(script), "{\n");
		//nettest: HDR sky — prefer an HDR face over the LDR $basetexture (8-bit, bands).
		//  $hdrbasetexture = RGBA16161616F (native float, used as-is); $hdrcompressedtexture
		//  = RGBS packed in BGRA8, decoded to linear float by img_vtf via the $hdr: name
		//  prefix (sets IF_HDRDECOMPRESS).  Prefer the uncompressed float face when both exist.
		if (*st->hdrbasetex || *st->hdrcompressedtex)
		{
			const char *hdrname = *st->hdrbasetex ? st->hdrbasetex : st->hdrcompressedtex;
			const char *hdrpfx  = *st->hdrbasetex ? "" : "$hdr:";
			Q_strlcatfz(script, &offset, sizeof(script),	"\tmap \"%s%s%s.vtf\"\n", hdrpfx, strcmp(hdrname, "materials/")?"materials/":"", hdrname);
		}
		else if (st->nobasetex)	//FTESurf Patch 190, see the fallback at the top of this function
			Q_strlcatfz(script, &offset, sizeof(script),	"\tmap $whiteimage\n");
		else
			Q_strlcatfz(script, &offset, sizeof(script),	"\tmap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);

		if (st->vertexcolor)
			Q_strlcatfz(script, &offset, sizeof(script),	"\rrgbGen vertex\n");
		/*
		FTESurf Patch 188: UnlitGeneric is the one arm that emits a real PASS
		rather than a program, so its $color/$color2 can use the pass keyword
		the engine actually has -- no permutation spent, and no shader change.
		$vertexcolor wins because a pass has exactly one rgbgen and the material
		asked for the per-vertex one explicitly.
		*/
		else
		{
			const char *cval = *st->color2 ? st->color2 : st->color;
			float c[3];
			if (*cval && VMT_ParseColour(cval, c))
				Q_strlcatfz(script, &offset, sizeof(script), "\trgbGen const %f %f %f\n", c[0], c[1], c[2]);
		}
		if (st->vertexalpha)
			Q_strlcatfz(script, &offset, sizeof(script),	"\ralphaGen vertex\n");
		//FTESurf Patch 189: this arm is a real pass, so the engine's own tcMod
		//does the scroll and no shader permutation is spent on it.
		if (st->scrollrate != 0 && VMT_ScrollsBase(st->scrollvar))
		{
			double a = st->scrollangle * (3.14159265358979323846/180.0);
			Q_strlcatfz(script, &offset, sizeof(script),	"\ttcMod scroll %f %f\n", st->scrollrate*cos(a), st->scrollrate*sin(a));
		}

		if (st->additive)
			Q_strlcatfz(script, &offset, sizeof(script),	"\tblendFunc add\n");
		else if (st->translucent)
			Q_strlcatfz(script, &offset, sizeof(script),	"\tblendFunc blend\n");
		else if (st->alphatest)
			Q_strlcatfz(script, &offset, sizeof(script),	"\talphaFunc GE128\n");


		Q_strlcatfz(script, &offset, sizeof(script), "}\n");
	}
	else if (!Q_strcasecmp(st->type, "WorldVertexTransition"))
	{
		if (*st->envmap && st->envfrombase)
			Q_strlcpy(st->type, "vmt/transition#ENVFROMBASE", sizeof(st->type));
		else if (*st->envmap && st->envfromnorm)
			Q_strlcpy(st->type, "vmt/transition#ENVFROMNORM", sizeof(st->type));
		else if (*st->envmap && *st->envmapmask) /* dedicated reflectmask */
			Q_strlcpy(st->type, "vmt/transition#ENVFROMMASK", sizeof(st->type));
		else /* take from normalmap */
			Q_strlcpy(st->type, "vmt/transition", sizeof(st->type));

		/*
		FTESurf Patch 187: a transition material with ONE texture was drawn as
		its missing second one.

		`uppermap` was emitted unconditionally, so a WorldVertexTransition with
		no $basetexture2 bound the literal path "materials/.vtf".  That alone
		would be a missing sampler; what makes it fatal is the mix factor.
		transition.glsl blends by vertex alpha, and mod_vbsp.c:2384 gives EVERY
		NON-DISPLACEMENT SURFACE an alpha of 1.0 (only displacements carry the
		painted value, at :2313).  mix(diffuse, upper, 1.0) is entirely upper --
		so on an ordinary brush face the material's own texture was never
		sampled and the surface came out as the missing-texture placeholder.
		surf_demise's smoke sheets are func_illusionary brushes, which is
		exactly why they read as solid slabs.

		93 materials across 49 maps are this shape (surf_torrential 9,
		surf_radiant 5, surf_agony 4, surf_demise 4, surf_rise 2).  #NOBLEND
		makes the shader sample s_diffuse alone, so the second sampler is never
		declared and cannot be wrong.

		Guarding the emit rather than binding a placeholder is the same rule
		Build 13 set for $envmap/$normalmap above: never write "materials/.vtf".
		*/
		if (!*st->tex[1].name)
			Q_strlcat(progargs, "#NOBLEND", sizeof(progargs));
		else if (*st->blendmod)	//shapes a crossover; with nothing to cross over TO it has no meaning, and binding it would load a texture the shader cannot reach
			Q_strlcat(progargs, "#BLENDMOD", sizeof(progargs));

		/*
		FTESurf Patch 251: $bumpmap2.

		All three conditions are load-bearing, and each corresponds to one of
		Source's own SKIP rules for the BUMPMAP2 combo:
		  tex[1]     -- "SKIP: !$BUMPMAP && $BUMPMAP2" in spirit: with #NOBLEND
		                there is no blend factor in scope in the shader at all,
		                because `blend` is declared inside the #else.
		  normalmap  -- the lerp needs a FIRST normal to start from, and the
		                shader only samples any normal inside #ifdef BUMP.
		  bumpmap2   -- never emit a `specularmap "materials/.vtf"` line; that is
		                the same rule Patch 187 set for uppermap and Build 13 for
		                envmap/normalmap.
		*/
		if (*st->tex[1].name && *st->normalmap && *st->bumpmap2 &&
		    (!hl2_bumpmap2 || hl2_bumpmap2->ival))
		{
			Q_strlcat(progargs, "#BUMP2", sizeof(progargs));
			vmt_stat_bumpmap2++;
		}
		/*
		FTESurf Patch 268 B: this arm has never been handed #ENVTINT/#ENVSAT at
		all, so a transition material's $envmaptint has always been ignored --
		transition.glsl falls back to its own defaults.  Fixing that is part of
		Source parity, not a free correctness win, so it rides on bit 2 with the
		contrast: with the bit off the three strings are empty and the program
		name is character-for-character the one emitted today.
		*/
		{
			qboolean envsrcspec = (hl2_envmap_source && (hl2_envmap_source->ival & 2));
			Q_strlcatfz(script, &offset, sizeof(script),	"\tprogram \"%s%s%s%s%s\"\n", st->type, progargs,
				envsrcspec?envmaptint:"", envsrcspec?envmapsat:"", envsrcspec?envmapcontrast:"");
		}
		Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
		if (*st->tex[1].name)
			Q_strlcatfz(script, &offset, sizeof(script),	"\tuppermap \"%s%s.vtf\"\n", strcmp(st->tex[1].name, "materials/")?"materials/":"", st->tex[1].name);
		//$blendmodulatetexture rides in on `lowermap`: S_LOWERMAP/s_lower
		//(gl_shader.c:1694, keyword at :3402) is a real sampler slot that
		//nothing on this material type uses, so the blend shape costs no engine
		//change and no new interface.
		if (*st->tex[1].name && *st->blendmod)
			Q_strlcatfz(script, &offset, sizeof(script),	"\tlowermap \"%s%s.vtf\"\n", strcmp(st->blendmod, "materials/")?"materials/":"", st->blendmod);

		if (*st->normalmap)
			Q_strlcatfz(script, &offset, sizeof(script),	"\tnormalmap \"%s%s.vtf\"\n", strcmp(st->normalmap, "materials/")?"materials/":"", st->normalmap);
		//$bumpmap2 rides in on `specularmap`: S_SPECULAR/s_specular
		//(gl_shader.c:1752, keyword at :3531) is a real sampler slot that nothing
		//on this material type uses.  The same trick $blendmodulatetexture plays
		//with s_lower above.  NOT s_fullbright -- mat_vmt.c hands that to any
		//material with $selfillum, transition materials included.
		//Emitted under the same three-way test that emitted #BUMP2, so the
		//permutation and the binding can never disagree.
		if (*st->tex[1].name && *st->normalmap && *st->bumpmap2 &&
		    (!hl2_bumpmap2 || hl2_bumpmap2->ival))
			Q_strlcatfz(script, &offset, sizeof(script),	"\tspecularmap \"%s%s.vtf\"\n", strcmp(st->bumpmap2, "materials/")?"materials/":"", st->bumpmap2);
	}
	else if (!Q_strcasecmp(st->type, "UnlitTwoTexture") && *st->tex[1].name &&
	         (!hl2_twotexture || hl2_twotexture->ival))
	{
		/*
		FTESurf: UnlitTwoTexture, with its second texture and its proxies.

		Reported as: "the portals in the main spawn area are a solid glow web
		texture. but in counter-strike they are not visible until you move
		closer to them, and visually it looks like a mask moving diagonally,
		and maybe mipmap coming in and showing you more as you get closer".

		The arm this replaces emitted ONE pass, bound $basetexture to it, chose
		a blendfunc and stopped.  $texture2 was parsed (:1436) and never used;
		the material's TextureScroll was read and then declined because it
		names $texture2Transform rather than the base (VMT_ScrollsBase); the
		proxy chain was skipped wholesale.  So the shield was a still image of
		one of its two layers, drawn at full brightness from any distance --
		"a solid glow web", exactly.

		SCOPE, MEASURED.  48 materials across the three mounted packs are
		UnlitTwoTexture.  Four of them are the combine shield family that
		surf_tensor2's portals use; the rest are HL2 monitors and displays
		(alyxmonitor, combinedisplay, tvscreen_noise, masterinterface), fog
		cards, the portal funnel -- and five of Momentum's OWN props:
		powerup_airjump, powerup_flight, powerup_haste, powerup_slick and
		mom_hologramfx.  Every one of them has been drawing one layer of two.

		11 materials in the library carry a PlayerProximity proxy and all 11
		are this family, so the distance behaviour is narrow; the two-layer
		multiply is not, and it is the part that matters on 48 materials.

		A PROGRAM IN A PASS, which is vmt/animated's shape (Patch 195) and for
		the same reason: `map "$2darray:..."` is what asks the loader for every
		frame of a multi-frame VTF, and `map` is a pass keyword.  The second
		layer rides on `uppermap`, a top-level default-texture slot that
		nothing on this material type uses -- the same trick Patch 251 plays
		with `specularmap` for $bumpmap2.  Default textures are shader-scoped,
		not pass-scoped, so a top-level uppermap and a program inside a pass
		see each other.

		THE FALLBACK IS THE OLD ARM, not a failure: a UnlitTwoTexture with no
		$texture2 has nothing to multiply, and hl2_twotexture 0 restores the
		single-layer draw for anyone who wants to compare.
		*/
		char t[256];
		double a;
		float sx = 0, sy = 0, s2x = 0, s2y = 0;
		/*
		FTESurf Patch 286: THE FLIPBOOK, at last.

		Reported as "I swear the portals do fade in and out over distance? and
		the masking effect also works, it doesn't MATCH source, but it's
		'there'" -- the fade and the scrolling mask had landed in Patch 279 and
		the third feature had not.  comshieldwall.vtf is 256x256 with THIRTY-ONE
		frames and the proxy chain picks one by DISTANCE:

		    $playerdistance2 = PlayerProximity * .2
		    $frameminusten   = $playerdistance2 - $ten     ($ten is "24")
		    $frame           = Clamp($frameminusten, 0, 30)

		so frame 0 inside 120 units and frame 30 beyond 270.  Drawing frame 0
		forever is why the web gets brighter as you approach but never denser.

		AND THE VMT SETS BOTH FRAMES FROM THE SAME CLAMP.  It carries two
		identical Clamp proxies, one writing $frame and one writing $frame2, so
		BOTH layers step together -- read out of the file rather than assumed,
		and it is why one array sampler read twice is not a shortcut but the
		transcription.  (VMT_ResolveProxies only looks for $frame; the $frame2
		twin is redundant by construction and is not parsed a second time.)

		TAKEN ONLY WHEN THE TWO TEXTURES ARE THE SAME FILE, and that is a real
		limit rather than a tidy special case.  A flipbook needs a 2DArray
		sampler, a 2DArray sampler needs `map "$2darray:..."`, `map` is a PASS
		keyword, and the shape that is PROVEN to work with a pass-level program
		is vmt/animated's -- exactly one pass, exactly one sampler.  Two
		different textures would need a second one, and both arrangements of
		that were tried in Patch 279 and failed silently (see the essay at the
		emit below, and the list at the top of twotexture.glsl).  7 of the
		library's 48 UnlitTwoTexture materials point both keys at one file.

		On surf_tensor2 that covers 42 of the 44 shield faces: comshieldwall is
		the same-texture one, comshieldwall2 pairs the same 31-frame base with a
		DIFFERENT single-frame texture and keeps the existing no-flipbook draw.
		It is counted as declined rather than passed over in silence, because
		"the portals still look wrong" needs to be answerable with a number.
		*/
		qboolean frames = (st->proxframe > 0 && *st->tex[0].name && *st->tex[1].name &&
		                   !Q_strcasecmp(st->tex[0].name, st->tex[1].name) &&
		                   (!hl2_twoframes || hl2_twoframes->ival));

		if (st->scrollrate != 0 && VMT_ScrollsBase(st->scrollvar))
		{
			a = st->scrollangle * (3.14159265358979323846/180.0);
			sx = st->scrollrate*cos(a);
			sy = st->scrollrate*sin(a);
		}
		if (st->tex2scrollrate != 0)
		{
			a = st->tex2scrollangle * (3.14159265358979323846/180.0);
			s2x = st->tex2scrollrate*cos(a);
			s2y = st->tex2scrollrate*sin(a);
		}
		Q_snprintfz(t, sizeof(t), "#SCROLL=%f,%f#SCROLL2=%f,%f", sx, sy, s2x, s2y);
		Q_strlcat(progargs, t, sizeof(progargs));

		if (st->proxalpha > 0)
		{
			Q_snprintfz(t, sizeof(t), "#PROXFADE=%f#PROXBASE=%f",
				st->proxalpha, st->proxalphabase);
			Q_strlcat(progargs, t, sizeof(progargs));
			vmt_stat_proxfade++;
		}
		if (st->proxframe > 0)
		{
			Q_snprintfz(t, sizeof(t), "#PROXFRAME=%f,%f,%f,%f",
				st->proxframe, st->proxframebias, st->proxframemin, st->proxframemax);
			Q_strlcat(progargs, t, sizeof(progargs));
			vmt_stat_proxframe++;
		}
		if (st->flickeramp > 0)
		{
			Q_snprintfz(t, sizeof(t), "#FLICKER=%f,%f",
				st->flickeramp, st->flickerperiod > 0 ? st->flickerperiod : 1.0f);
			Q_strlcat(progargs, t, sizeof(progargs));
		}
		if (st->alphaval > 0 && st->alphaval < 1)
		{
			Q_snprintfz(t, sizeof(t), "#ALPHA=%f", st->alphaval);
			Q_strlcat(progargs, t, sizeof(progargs));
		}
		/*
		WHICH CHANNEL THE DISTANCE FADE GOES INTO.  An additive surface fades by
		getting darker and an alpha-blended one by getting more transparent, and
		putting it in the wrong one is silently a no-op rather than an error:
		blendFunc add is GL_ONE GL_ONE, whose factors never touch the source
		alpha, so a fade written into alpha is simply discarded by the blender.
		That is what "the scrolling effect doesn't really diminish over
		distance" was -- the two layers and the scroll had arrived and only the
		fade had not.  Additive wins over translucent here for the same reason
		it does in the blend chain below: Source resolves it that way.
		*/
		if (st->additive)
			Q_strlcat(progargs, "#ADDITIVE", sizeof(progargs));

		if (frames)
			Q_strlcat(progargs, "#FRAMES", sizeof(progargs));
		else if (st->proxframe > 0)
			vmt_stat_twoframesdecl++;

		Q_strlcpy(st->type, "vmt/twotexture", sizeof(st->type));

		if (frames)
		{
			/*
			vmt/animated's SHAPE, and it is the only one of the three that has
			ever put pixels on the screen.

			The program goes INSIDE the pass that carries the array, because
			`map "$2darray:..."` (gl_shader.c:1022 -> IF_TEXTYPE_2D_ARRAY) is
			what asks the loader for all 31 frames and `map` is a pass keyword.
			One pass, one sampler, `!!samps =FRAMES shield:2DArray=0` -- the
			same binding Patch 195 has been running on 927 materials.

			The two arrangements that DID NOT work, both from Patch 279 and both
			silent, so that nobody re-derives them:

			  program in the pass + top-level `uppermap`   two samplers by two
			      different mechanisms.  A debug build that painted the samples
			      into red and green came out black in both channels.
			  top-level program + one pass per sampler     `prog 1 passes 2`,
			      and the surface stopped reaching the rasteriser at all -- a
			      shader forced to output opaque red drew nothing.

			The shipped no-flipbook path avoids both by using NO passes: a
			top-level program with `diffusemap`/`uppermap`, which are shader-
			scoped default-texture slots.  There is no default slot of array
			type, which is why this case has to be a pass and why it can only
			be one.

			THE BLEND HAS TO BE THE PASS'S.  `progblendfunc` attaches to a
			top-level program and there is none here; animpass suppresses that
			emit down in the tail (:3703) and this spells it instead.  Getting
			this wrong is what turned surf_demise's smoke into an opaque slab.
			Additive wins over translucent, which is how Source resolves a
			material that writes both -- and comshieldwall writes both.
			*/
			Q_strlcatfz(script, &offset, sizeof(script),	"\t{\n");
			Q_strlcatfz(script, &offset, sizeof(script),	"\t\tprogram \"%s%s\"\n", st->type, progargs);
			Q_strlcatfz(script, &offset, sizeof(script),	"\t\tmap \"$2darray:%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
			if (st->additive && (!hl2_translucent || hl2_translucent->ival))
				Q_strlcatfz(script, &offset, sizeof(script),	"\t\tblendFunc add\n");
			else if (st->translucent && (!hl2_translucent || hl2_translucent->ival))
				Q_strlcatfz(script, &offset, sizeof(script),	"\t\tblendFunc blend\n");
			Q_strlcatfz(script, &offset, sizeof(script),	"\t}\n");
			//animpass also gets the generated script dumped at developer 1
			//(:3744), which is the only way to read one of these back: the
			//shield is on func_brush *19 and shader_here traces the world.
			st->animpass = 1;
			vmt_stat_twoframes++;
			vmt_stat_twotexture++;
		}
		else
		{
		/*
		A TOP-LEVEL PROGRAM WITH NO PASSES AT ALL, and two attempts got this
		wrong in ways worth recording.

		HEADING CORRECTED IN PATCH 286: this block used to be titled "a
		top-level program with two passes", which is not what the three lines
		below emit and never was.  They emit a top-level program and two
		DEFAULT-TEXTURE slots; the engine synthesises the single pass.  The
		wrong title was a theory that survived next to code that had already
		disproved it, which is exactly the thing these essays exist to stop.

		Attempt 1 -- vmt/animated's shape, the program INSIDE the pass that
		carries `map "$2darray:..."`, with the second layer arriving on the
		top-level `uppermap` slot.  The generated script was exactly as
		intended, every proxy coefficient in it was correct, and the shield drew
		NOTHING: `[shader] effects/combineshield/comshieldwall sort 12 prog 0
		passes 2`.  A debug build that painted the two samples into red and
		green came out black, so neither sampler had anything in it.

		Attempt 2 -- a top-level program with one pass per sampler, reasoning
		from gl_shader.c:1956-1959 that "numsamplers and the SYNTHESISED PASS
		LIST are the same thing".  That got `prog 1 passes 2` and the surface
		stopped reaching the rasteriser entirely: a shader forced to output
		opaque red drew nothing.

		So neither route to TWO textures through a pass works, and what ships is
		the route that uses no pass: `diffusemap` and `uppermap` are
		shader-scoped default slots and the program reads both.  The cost is
		that there is no default slot of ARRAY type, which is why the flipbook
		above has to be a one-sampler special case.
		*/
		Q_strlcatfz(script, &offset, sizeof(script),	"\tprogram \"%s%s\"\n", st->type, progargs);
		Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
		//$texture2 rides on `uppermap`: S_UPPERMAP/s_upper is a real default
		//sampler slot that nothing on this material type uses.  The same trick
		//Patch 251 plays with s_specular for $bumpmap2 and Patch 187 with
		//s_lower for $blendmodulatetexture.
		Q_strlcatfz(script, &offset, sizeof(script),	"\tuppermap \"%s%s.vtf\"\n", strcmp(st->tex[1].name, "materials/")?"materials/":"", st->tex[1].name);
		//animpass stays 0: the program is TOP-LEVEL, so the tail's
		//`progblendfunc` has something to attach to and is the right way to set
		//the blend.  Shader_ProgBlendFunc rewrites pass 0's blend, which for a
		//program shader is the pass the engine synthesises for it.
		vmt_stat_twotexture++;
		}
	}
	else if (!Q_strcasecmp(st->type, "UnlitTwoTexture"))
	{

		Q_strlcatfz(script, &offset, sizeof(script), "{\n");
		Q_strlcatfz(script, &offset, sizeof(script),	"\tmap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);

		if (st->mod2x) {
			Q_strlcatfz(script, &offset, sizeof(script),"\t\tblendFunc gl_dst_color gl_src_color\n");
		} else if (st->additive) {
			Q_strlcatfz(script, &offset, sizeof(script),"\t\tblendFunc add\n");
		}

		Q_strlcatfz(script, &offset, sizeof(script), "}\n");
	}
	/*
	FTESurf Patch 300: an ADDITIVE Sprite onto vmt/unlit, for the same reason as
	the UnlitGeneric arm above -- see VMT_AdditiveProgramOK -- and this is the arm
	that carries the reported bug.

	surf_tensor2 draws 263 env_sprite through here (233 sprites/light_glow03.vmt,
	30 sprites/xen_ray_3.vmt) and every one is program-less today.  Their textures
	are BGR888 with NO ALPHA CHANNEL, so black is the only mask they have, and
	that is exactly the thing fixed-function fog destroys.

	A NOTE ON THE CLASS, because the tree had it wrong and so did I: these are
	shader class "Sprite", not UnlitGeneric.  The comment above
	BE_ProglessFogBegin in gl_backend.c said UnlitGeneric, Patch 297's writeup
	repeated it, and it survived because the CONCLUSION was right for the wrong
	reason -- both arms emit a program-less pass, so both show the artefact.  It
	only became load-bearing here, where the fix has to name an arm.  Verified
	against the packs the map actually mounts: mapdeps.txt has one line for
	surf_tensor2 (CS:GO), and the light_glow VMTs are byte-identical "Sprite" in
	the HL2 and CS:GO copies alike.

	THE BLEND HAS TO BE SET BY HAND HERE.  The shared tail writes `progblendfunc`
	only when st->blendfunc is set, and it sets that from st->additive (:3913) --
	but a default-additive Sprite has st->additive == 0 by definition.  So this
	arm asserts it, the way the emissive arm does at its own :2686.  #ADDITIVE has
	to be appended here too: the block that would normally emit it runs before
	this dispatch, when st->additive was still 0.
	*/
	else if (!Q_strcasecmp(st->type, "Sprite") && VMT_SpriteAdditiveProgramOK(st))
	{
		if (!st->additive)
		{
			st->additive = 1;
			Q_strlcat(progargs, "#ADDITIVE", sizeof(progargs));
		}
		//Unconditional, because this arm's pass says `rgbGen vertex`
		//unconditionally -- and that implies alphaGen vertex too.  See
		//VMT_AddVertexColourArgs; getting this wrong draws every sprite at full
		//brightness.
		VMT_AddVertexColourArgs(progargs, sizeof(progargs));
		Q_strlcpy(st->type, "vmt/unlit", sizeof(st->type));
		Q_strlcatfz(script, &offset, sizeof(script),	"\tprogram \"%s%s\"\n", st->type, progargs);
		Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
		vmt_stat_addprog++;
	}
	else if (!Q_strcasecmp(st->type, "Sprite"))
	{
		if (st->additive || (!st->translucent && !st->alphatest))
			vmt_stat_addpass++;	//additive but declined -- see VMT_SpriteAdditiveProgramOK
		/*
		FTESurf Patch 239: a Sprite that asks to be MASKED was drawn as an
		additive glow with no mask at all.

		This arm used to emit `map` / `rgbGen vertex` / `blendFunc add` with no
		branch of any kind.  That reads like "unconditionally additive", and it
		is worth being precise about why it mostly is not: the shared tail below
		emits `progblendfunc`, and Shader_ProgBlendFunc rewrites PASS 0's blend
		(gl_shader.c:3116 walks `ps->pass = ps->s->passes` and calls
		Shaderpass_BlendFunc).  So a Sprite writing $translucent or $additive
		already had its blend corrected on the way out, by accident.

		$alphatest is the case with nothing to correct it.  It sets no
		st->blendfunc, so no progblendfunc is emitted, the pass keeps
		`blendFunc add`, and the `alphatest ge128` the tail writes at :2060 is a
		TOP-LEVEL directive -- `alphatest` lives in shaderpasskeys, and
		gl_shader.c:5719 lists it by name among the Source keywords that leak to
		the top level as harmless no-ops.  A cutout therefore came out as an
		unmasked additive glow.  That is 219 of the 901 distinct Sprite
		materials in the library (24%), and 217 of the 413 that maps actually
		pack (53%).

		$vertexalpha was dropped too: `rgbGen vertex` was emitted unconditionally
		with no `alphaGen` beside it, so a sprite's per-vertex opacity -- which
		is how rendermode/renderamt reaches a sprite at all -- had nowhere to go.

		SCOPE, STATED HONESTLY: only 12 Sprite-typed materials in the whole
		1310-map library sit on a world face, so this changes almost nothing on
		screen TODAY.  It matters because it is the prerequisite for drawing
		env_sprite and friends at all -- every one of those 217 masked materials
		is referenced by a sprite ENTITY, and nothing spawns those yet.
		*/
		Q_strlcatfz(script, &offset, sizeof(script), "{\n");
		Q_strlcatfz(script, &offset, sizeof(script),	"\tmap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
		Q_strlcatfz(script, &offset, sizeof(script),	"\rrgbGen vertex\n");
		if (st->vertexalpha || st->translucent || st->alphaval)
			Q_strlcatfz(script, &offset, sizeof(script),	"\ralphaGen vertex\n");
		//The order is Source's own: additive wins, then translucency, and a
		//cutout is its own branch rather than the tail of that chain -- a
		//material is entitled to write $alphatest AND $translucent, and Source
		//masks first and blends what survives.
		if (st->additive && (!hl2_translucent || hl2_translucent->ival))
			Q_strlcatfz(script, &offset, sizeof(script),	"\tblendFunc add\n");
		else if (st->translucent && (!hl2_translucent || hl2_translucent->ival))
			Q_strlcatfz(script, &offset, sizeof(script),	"\tblendFunc blend\n");
		else if (!st->alphatest)
			Q_strlcatfz(script, &offset, sizeof(script),	"\tblendFunc add\n");	//no opinion in the VMT: keep what shipped
		if (st->alphatest)
		{
			/*
			In the PASS's own spelling, which is where alphaFunc is not a no-op.

			Shaderpass_AlphaFunc understands exactly three tokens -- gt0, lt128
			and ge128 -- and an unrecognised one is worse than useless: it clears
			SBITS_ATEST_BITS and then matches nothing, so the alpha test ends up
			DISABLED.  So the reference is quantised to the two thresholds that
			exist rather than written out as a number.  ge128 is 0.502, which is
			Source's own 0.5 default to within half a level, and that default is
			what all but a handful of these materials use; a material asking for
			materially less than half gets gt0, i.e. discard only what is fully
			transparent, which errs toward keeping the artist's pixels.
			(The vmt/* PROGRAMS take the exact value via #MASK -- see Patch 237.
			This arm emits a pass, not a program, so it only has these two.)
			*/
			float mask = (st->alphatestref > 0.0f) ? st->alphatestref : 0.5f;
			Q_strlcatfz(script, &offset, sizeof(script),	"\talphaFunc %s\n", (mask >= 0.25f)?"ge128":"gt0");
		}
		Q_strlcatfz(script, &offset, sizeof(script), "}\n");
	}
	else if (!Q_strcasecmp(st->type, "SpriteCard"))
	{
		/*
		FTESurf: SpriteCard -- Source's PARTICLE shader, and the reason
		surf_tensor2's portal particles were white.

		Reported as "they don't have a cool blue hue like in source", on
		particles whose baked r_part block asks for rgb 51 51 80 ramping to
		0 96 255.  The colour was not being dropped by the particle system: it
		was being dropped by THIS material, and the route is worth writing down
		because it is not visible from either end.

		p_script.c:743 gives BM_BLEND an EMPTY shader-name postfix, so
		P_LoadTexture's first move for `blend blendalpha` is
		R_RegisterCustom("particle/particle_glow_01") -- the bare texture name,
		with nothing appended.  A material of exactly that name exists, this
		plugin answers with it, and the particle system takes it in preference
		to its own template (:754, "try and load the shader, fail if we would
		need to generate one").  The template it skipped is the one carrying
		`rgbgen vertex` and `alphagen vertex` (:774-776).  So every Source
		particle material in the library has been overriding the per-particle
		colour with a flat white texture, and the effect looked like the
		translation had lost the colour.

		SpriteCard was reaching the unimplemented-class fallback, which emits
		vmt/unlit with no rgbgen at all -- so the flat white was guaranteed.
		180 materials across the three mounted packs are SpriteCard, and every
		one of them is a particle.

		$vertexcolor and $vertexalpha are LITERALLY IN THE VMT
		(particle_glow_01.vmt writes both), so this is not an inference about
		what a particle wants; it is the material saying so and nothing reading
		it.  They are still tested rather than assumed, because a SpriteCard
		that declines them is entitled to a flat colour.

		nodepth, because that is what the particle template it displaces does
		(p_script.c:777) and a particle that writes depth punches a hole in
		everything drawn after it.
		*/
		Q_strlcatfz(script, &offset, sizeof(script), "{\n");
		Q_strlcatfz(script, &offset, sizeof(script),	"\tmap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
		if (st->vertexcolor || !*st->color)
			Q_strlcatfz(script, &offset, sizeof(script),	"\trgbGen vertex\n");
		if (st->vertexalpha || st->translucent || st->additive)
			Q_strlcatfz(script, &offset, sizeof(script),	"\talphaGen vertex\n");
		//Source's own order: additive wins, then translucency.  A SpriteCard
		//with neither is still a particle, so it keeps an alpha blend rather
		//than becoming an opaque quad.
		if (st->additive)
			Q_strlcatfz(script, &offset, sizeof(script),	"\tblendFunc add\n");
		else
			Q_strlcatfz(script, &offset, sizeof(script),	"\tblendFunc blend\n");
		//INSIDE the pass.  `nodepth` is a pass keyword; at the top level the
		//parser prints "Unknown shader directive" and carries on, which is how
		//the first version of this arm shipped a depth-writing particle and a
		//console line nobody would connect to it.
		Q_strlcatfz(script, &offset, sizeof(script),	"\tnodepth\n");
		Q_strlcatfz(script, &offset, sizeof(script), "}\n");
		st->translucent = 0;	//the pass carries it; a progblendfunc would have no program to attach to
		vmt_stat_spritecard++;
	}
	else if (!Q_strcasecmp(st->type, "Decal"))
	{
		Q_strlcpy(st->type, "vmt/vertexlit", sizeof(st->type));
		Q_strlcatfz(script, &offset, sizeof(script),	"\tprogram \"%s%s\"\n", st->type, progargs);
		Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
		Q_strlcatfz(script, &offset, sizeof(script),	"\tpolygonOffset 1\n");
		st->decal = 0;
	}
	else if (!Q_strcasecmp(st->type, "DecalModulate"))
	{
		Q_strlcatfz(script, &offset, sizeof(script),	"\tpolygonOffset 1\n");
		Q_strlcatfz(script, &offset, sizeof(script),	"\t{\n");
		Q_strlcatfz(script, &offset, sizeof(script),	"\t\tmap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);

		if (!st->mod2x) {
			Q_strlcatfz(script, &offset, sizeof(script),"\t\tblendFunc gl_dst_color gl_src_color\n");
		} else {
			Q_strlcatfz(script, &offset, sizeof(script),"\t\tblendFunc gl_dst_color gl_one_minus_src_alpha\n");
		}

		Q_strlcatfz(script, &offset, sizeof(script),	"\t}\n");
		st->decal = 0;
		st->translucent = 0;
	}
	else if (!Q_strcasecmp(st->type, "Modulate"))
	{

		Q_strlcatfz(script, &offset, sizeof(script),	"\t{\n");
		Q_strlcatfz(script, &offset, sizeof(script),	"\t\tmap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);

		if (!st->mod2x) {
			Q_strlcatfz(script, &offset, sizeof(script),"\t\tblendFunc gl_dst_color gl_src_color\n");
		} else {
			Q_strlcatfz(script, &offset, sizeof(script),"\t\tblendFunc gl_dst_color gl_one_minus_src_alpha\n");
		}
		Q_strlcatfz(script, &offset, sizeof(script),	"\trgbGen vertex\n");

		Q_strlcatfz(script, &offset, sizeof(script),	"\t}\n");
		st->translucent = 0;
	}
	else if (!Q_strcasecmp(st->type, "Water"))
	{
		int wmode = hl2_water?hl2_water->ival:3;
		if (wmode < 0) wmode = 0;
		if (wmode > 3) wmode = 3;
		vmt_stat_water++;

		/*
		FTESurf Patch 254: the underwater fog, recorded once per map.

		$fogcolor is what Source paints when the camera is submerged, and
		$fogstart/$fogend are how far you can see through it.  The engine already
		swaps to cl.fog[FOGTYPE_WATER] on its own whenever RDF_UNDERWATER is set
		(gl_rmain.c), so the gamecode only has to hand it these numbers once at
		map load and never think about submersion again.

		First one wins: a map's water materials usually agree, and when they do
		not, the first is at least stable across loads.  A material with no
		$fogend is skipped rather than recorded as 0, because 0 would mean "fully
		fogged at the eye" once it reached the linear fog path.
		*/
		if (!vmt_stat_waterfog_set && st->fogcolor_set && st->fogend > 0)
		{
			vmt_stat_waterfog_set = 1;
			vmt_stat_waterfogstart = st->fogstart;
			vmt_stat_waterfogend   = st->fogend;
			Q_snprintfz(vmt_stat_waterfog, sizeof(vmt_stat_waterfog), "%f %f %f",
				st->fogcolor_f[0], st->fogcolor_f[1], st->fogcolor_f[2]);
		}

		if (st->water_cheap)
			Q_strlcpy(progargs, "#LQWATER", sizeof(progargs));
		if (st->water_expensive)
			Q_strlcpy(progargs, "#HQWATER", sizeof(progargs));

		/*
		FTESurf Patch 160: hl2_water 1 and 2 rendered IDENTICALLY, and the
		reason is that mode 1 was only half a mode.

		`map $reflection` was dropped at 1 and kept at 2, which does remove the
		reflection RENDER TARGET -- that is real, and it is the whole of Build
		10's 8.71 -> 390.36 fps.  But vmt/water.glsl declares `!!samps
		reflect=1` unconditionally and samples s_reflect unconditionally, so
		with no pass to bind it the shader read whatever was left in sampler 1
		and mixed that in by fresnel exactly as before.  Cheap and expensive
		water were the same picture and only one of them was honest about it.

		The shader already has the answer: #LQWATER takes the reflection from
		s_reflectcube -- a real cubemap -- instead of the render target.  So
		mode 1 forces it rather than leaving it to the material, which is what
		"cheap water" is supposed to mean, and which as of Patch 153 has an
		actual baked cubemap to read.  Mode 2 still honours the VMT's own
		$cheapwaterstartdistance/expensive hints.
		*/
		if (wmode == 1)
			Q_strlcpy(progargs, "#LQWATER", sizeof(progargs));

		/*
		FTESurf Patch 251: hand water.glsl the keys it has always been able to read.

		The shader declares #defines for FRESNEL_*, STRENGTH_REFR/REFL,
		TINT_REFR/REFL, TXSCALE1/2 and FOGTINT and falls back to a built-in default
		for each; mat_vmt.c only ever wrote #LQWATER or #HQWATER, so every water
		material in the library drew identically no matter what its author asked
		for.  $refractamount appears on all 61 Water materials in a 200-map sample,
		$reflectamount on 51, $fogcolor on 58, the two tints on 29 each.

		Appended, not assigned: the two lines above use Q_strlcpy and this must not
		undo them.

		WHAT IS DELIBERATELY NOT WIRED, so the next person does not assume it was
		missed.  #DEPTH is the interesting one -- it is what turns $fogstart/$fogend
		into a real depth-graded fade instead of a constant tint -- but its branch
		samples s_refractdepth, and water.glsl's `!!samps` line declares only
		refract and reflect.  Wiring #DEPTH without also declaring and binding that
		sampler would produce a shader that fails to compile, so $fogstart/$fogend
		stay where they are: driving the dither coverage at hl2_water 3, which is
		the default mode and the one most people see.

		AND THE HONEST HEADLINE: hl2_water defaults to 3, the dithered flat mode,
		which uses none of this.  Everything below changes hl2_water 1 and 2 only.
		*/
		{
			char t[192];
			if (st->refractamount_set)
			{
				Q_snprintfz(t, sizeof(t), "#STRENGTH_REFR=%f", st->refractamount);
				Q_strlcat(progargs, t, sizeof(progargs));
			}
			if (st->reflectamount_set)
			{
				Q_snprintfz(t, sizeof(t), "#STRENGTH_REFL=%f", st->reflectamount);
				Q_strlcat(progargs, t, sizeof(progargs));
			}
			if (st->refracttint_set)
			{
				Q_snprintfz(t, sizeof(t), "#TINT_REFR=%f,%f,%f",
					st->refracttint[0], st->refracttint[1], st->refracttint[2]);
				Q_strlcat(progargs, t, sizeof(progargs));
			}
			if (st->reflecttint_set)
			{
				Q_snprintfz(t, sizeof(t), "#TINT_REFL=%f,%f,%f",
					st->reflecttint[0], st->reflecttint[1], st->reflecttint[2]);
				Q_strlcat(progargs, t, sizeof(progargs));
			}
			if (st->fogcolor_set)
			{
				Q_snprintfz(t, sizeof(t), "#FOGTINT=%f,%f,%f",
					st->fogcolor_f[0], st->fogcolor_f[1], st->fogcolor_f[2]);
				Q_strlcat(progargs, t, sizeof(progargs));
			}
			if (st->waterscale_set)
			{
				//Both layers from the one key: Source has a single $scale and the
				//shader's two are its two wave layers, which are meant to tile
				//together and drift apart only in time (the +e_time offsets differ).
				Q_snprintfz(t, sizeof(t), "#TXSCALE1=%f#TXSCALE2=%f",
					st->waterscale, st->waterscale);
				Q_strlcat(progargs, t, sizeof(progargs));
			}
			//What the census will print. See the note beside vmt_stat_waterargs
			//for why this is recorded here rather than probed with shader_here.
			Q_strlcpy(vmt_stat_waterargs, *progargs ? progargs : "(none)",
				sizeof(vmt_stat_waterargs));
		}

		if (wmode == 3)
		{
			/*
			Dithered flat -- the default since build 12, and the cheapest of the
			four.  Opaque geometry with a discard: no blend, no sort, no render
			target, and it writes depth, so what is under the water is z-rejected
			instead of drawn and then covered up.

			$fogcolor is the colour, exactly as mode 0 established -- a Water
			VMT's $basetexture is an INPUT to the refraction program, not a
			picture of water, and drawing it gives an opaque white slab.
			*/
			char fog[64];
			VMT_FogColorString(st->fogcolor, fog, sizeof(fog));
			Q_strlcatfz(script, &offset, sizeof(script),
				"\t{\n"
					"\t\tprogram \"vmt/flatdither\"\n"
					"\t\tmap $whiteimage\n"
					"\t\trgbgen const %s\n"
					"\t\talphagen const %f\n"
				"\t}\n", fog, VMT_WaterDitherAlpha(st->fogend));
		}
		else if (!wmode)
		{
			/*
			Flat, and NOT the base texture.

			Drawing $basetexture was the obvious thing and it is wrong: a Water
			VMT's base map is an input to the refraction program, not a picture
			of water, and on d1_canals_02 it came out as an opaque white slab.
			What Source actually paints where the water is opaque is $fogcolor,
			so that is what this uses -- flat, translucent, and the same idiom
			the engine's own r_waterstyle 0 uses (gl_shader.c:7205).

			No program and no render target, so the backend never reaches the
			GLR_DrawPortal branches at all.
			*/
			char fog[64];
			VMT_FogColorString(st->fogcolor, fog, sizeof(fog));
			Q_strlcatfz(script, &offset, sizeof(script),
				"\t{\n"
					"\t\tmap $whiteimage\n"
					"\t\trgbgen const %s\n"
					"\t\talphagen const 0.7\n"
					"\t\tblendfunc blend\n"
				"\t}\n", fog);
		}
		else
		{
			/*
			Refraction always; reflection only at 2.  Dropping the reflection
			removes one whole scene render per batch and is what Source's own
			cheap water does.
			*/
			Q_strlcatfz(script, &offset, sizeof(script),
				"\t{\n"
					"\t\tprogram \"vmt/water%s\"\n"
					"\t\tmap $refraction\n", progargs);
			if (wmode >= 2)
				Q_strlcatfz(script, &offset, sizeof(script), "\t\tmap $reflection\n");
			Q_strlcatfz(script, &offset, sizeof(script), "\t}\n");
		}

		/*
		Mode 3 samples neither of these -- the dither pass carries its own
		$whiteimage and the colour comes from rgbgen -- so asking for them would
		load two VTFs per water material to never read them.  The point of the
		mode is that it is cheap.
		*/
		if (wmode != 3)
		{
			Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
			Q_strlcatfz(script, &offset, sizeof(script),	"\tnormalmap \"%s%s.vtf\"\n", strcmp(st->normalmap, "materials/")?"materials/":"", st->normalmap);
		}
		Q_strlcatfz(script, &offset, sizeof(script),	"\tsurfaceparm nodlight\n");
	}
	else if (!Q_strcasecmp(st->type, "Refract"))
	{
		int rmode = hl2_refract?hl2_refract->ival:2;
		if (rmode < 0) rmode = 0;
		if (rmode > 2) rmode = 2;
		vmt_stat_refract++;

		if (rmode == 2)
		{
			/*
			Dithered flat -- the default since build 12.  A pane keeps its own
			texture (glass has a frame, a tint and often dirt, and throwing that
			away for a flat colour loses the thing that makes it read as a
			window), and $alpha decides the coverage.  A Refract material rarely
			carries one, so the fallback is 0.25: mostly see-through, which is
			what refraction looked like once it stopped warping.
			*/
			const char *tex = *st->refracttinttexture ? st->refracttinttexture : st->tex[0].name;
			Q_strlcatfz(script, &offset, sizeof(script),
				"\t{\n"
					"\t\tprogram \"vmt/flatdither\"\n"
					"\t\tmap \"%s%s.vtf\"\n"
					"\t\talphagen const %f\n"
				"\t}\n",
				strcmp(tex, "materials/")?"materials/":"", tex,
				VMT_DitherScale((st->alphaval > 0 && st->alphaval < 1) ? st->alphaval : 0.25f));
		}
		else if (!rmode)
		{
			/*
			Glass without the framebuffer copy.  $currentrender forces the
			backend to snapshot the screen for every batch of this shader; at
			0 the pane becomes ordinary translucency, which is what you see
			through a window anyway once it is not warping.
			*/
			Q_strlcatfz(script, &offset, sizeof(script),
				"\t{\n"
					"\t\tmap \"%s%s.vtf\"\n"
					"\t\tblendfunc blend\n"
				"\t}\n",
				*st->refracttinttexture
					? (strcmp(st->refracttinttexture, "materials/")?"materials/":"")
					: (strcmp(st->tex[0].name, "materials/")?"materials/":""),
				*st->refracttinttexture ? st->refracttinttexture : st->tex[0].name);
		}
		else
		{
			if (*st->refracttinttexture)
				Q_strlcpy(progargs, "#TINTTEXTURE", sizeof(progargs));

			Q_strlcatfz(script, &offset, sizeof(script),
				"\t{\n"
					"\t\tprogram \"vmt/refract%s\"\n"
					"\t\tmap $currentrender\n"
					"\t\tmap \"%s%s.vtf\"\n"
				"\t}\n", progargs, strcmp(st->normalmap, "materials/")?"materials/":"", st->normalmap);
		}

		//rmode 2 has no normal to perturb and samples its texture through the
		//pass, so neither of these would ever be read.
		if (rmode != 2)
		{
			if (*st->refracttinttexture)
				Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->refracttinttexture, "materials/")?"materials/":"", st->refracttinttexture);

			Q_strlcatfz(script, &offset, sizeof(script),	"\tnormalmap \"%s%s.vtf\"\n", strcmp(st->normalmap, "materials/")?"materials/":"", st->normalmap);
		}
	}
	/*
	FTESurf Patch 263: WindowImposter -- the shader Source uses to fake a SECOND
	skybox, and the one class in this chain whose absence drew a checkerboard.

	Reported as "surf_monolith b4 ... missing sky texture".  Bonus 4 is roofed
	and walled by a 2-unit slab of

	    materials/fakeskies/mpa45.vmt
	    WindowImposter { $envmap "fakeskies/mpa45"  $nofog 1 }

	laid 4 units UNDER the map's real sky brush -- 111 faces, all worldspawn, one
	of them running the full height of the wall (z 12164..16300).  The map's own
	sky is sky138a and it is perfect: six 1024x1024 BGR888 faces, present, all
	blue.  It was simply hidden behind the slab.

	WHAT SOURCE DRAWS: the $envmap cubemap and nothing else, sampled by the VIEW
	DIRECTION, ignoring the face's orientation.  That last part is not a detail,
	it is the whole shader -- it is what lets a flat ceiling read as open sky, and
	it is why the trick works for "more than one skybox in one map".  Valve's
	docs: "The faces orientation isn't taken into account when reflecting the
	cubemap, which makes it very convenient for making windows/doors that hide
	everything behind them while looking like they aren't, as well as faking
	having multiple simultaneous skyboxes."  See glsl/vmt/imposter.glsl.

	WHAT WE DREW INSTEAD, and why nothing said so.  The class matched no arm, so
	it fell into the terminal else, which is silent.  That arm emits
	`program vmt/unlit` + a diffusemap, and the material has no $basetexture, so
	the default-fill at the top of this function had already back-filled
	st->tex[0].name with the material's OWN path.  The emitted diffusemap was
	therefore materials/fakeskies/mpa45.vtf -- which exists, and which img_vtf.c
	loads as PTI_CUBE because bit 0x4000 is set.  A cube texture is not a loaded
	2D base, so gl_backend.c's T_GEN_DIFFUSE substituted missing_texture, and
	missing_texture is R_InitTextures' 16x16 checkerboard of palette index 0 and
	index 255 -- and index 255 of default_quakepal is 159,91,83.  That dusty
	salmon, minified across a huge face, is the "flat salmon plane" in the
	screenshots.  Every step of that chain behaved as designed.

	SCOPE: 155 WindowImposter materials across 59 of the library's 1310 maps.
	surf_polytron 22, bhop_angst 13, surf_angst 11, surf_illusion 7, surf_agony
	6, surf_muerto 6; surf_monolith has exactly one, and it is this.

	NO $basetexture IS EMITTED HERE.  The shared tail below already emits
	`reflectcube` for any material carrying an $envmap, and deliberately skips the
	literal "env_cubemap" -- which is correct rather than a gap, because
	gl_backend.c's T_GEN_REFLECTCUBE then falls back to curbatch->envmap, the
	per-surface baked cubemap VBSP_LoadCubemaps assigns.  So both spellings of
	$envmap reach the sampler with no extra code here.

	progargs is passed through: #ALPHA/#COLOR/#SCROLL are value defines, not
	permutations, so an arg this shader does not read is an unused macro rather
	than the silent program-load failure Patch 236 documents for bare flags like
	#VERTEXCOL.  Those two are gated to LightmappedGeneric/WorldVertexTransition
	above and cannot reach here.

	hl2_imposter 0 emits nodraw rather than restoring the old behaviour: a
	checkerboard is not a fallback worth keeping, and an invisible imposter is
	also the one-cvar way to see what the slab is hiding (on monolith, sky138a).
	*/
	else if (!Q_strcasecmp(st->type, "WindowImposter"))
	{
		vmt_stat_imposter++;

		if (hl2_imposter && !hl2_imposter->ival)
			Q_strlcatfz(script, &offset, sizeof(script),	"\tsurfaceparm nodraw\n");
		else
			Q_strlcatfz(script, &offset, sizeof(script),	"\tprogram \"vmt/imposter%s\"\n", progargs);
	}
	/*
	FTESurf Patch 236: a LIGHTMAPPED material does not stop being lightmapped
	because it is also a decal.

	`|| st->decal` used to send any material carrying `$decal 1` down the
	VertexlitGeneric arm regardless of what the material actually said it was.
	Since LightmappedGeneric is tested BELOW this (:1801) and not above it, that
	captured every world decal in the game -- and bhop_futile's are all

	    LightmappedGeneric { $vertexcolor 1 $vertexalpha 1 $translucent 1
	                         $basetexture "Decals/ivy02" $decal 1 }

	which is where it turned into a rendering bug rather than a lighting nit.
	Patch 232 appends "#VERTEXCOL#VERTEXALPHA" to progargs for exactly two
	types, LightmappedGeneric and WorldVertexTransition (:1286-1296), and it
	does that test BEFORE this reroute -- so the type was still
	"LightmappedGeneric" when the args were chosen and "vmt/vertexlit" by the
	time they were used.  vertexlit.glsl declares neither permutation (`grep -c
	VERTEXCOL` is 0 there, against 3 in lightmapped.glsl), so the program failed
	to load, and failed SILENTLY: not one line in the log.  Measured, with
	r_shaderpasses, before this patch:

	    decals/ivy02                noLIGHTMAP  sort=6  passes=6  prog=<none>
	    concrete/concretefloor024a  HASLIGHTMAP sort=5  passes=6  prog=vmt/lightmapped

	prog=<none> means the surface lost its program, its lightmap AND its
	progblendfunc together and fell back to generated fixed-function passes --
	drawn as a flat opaque rectangle of the base texture's RGB.  That is the
	reported "decals are not masked and are tinted": olive for ivy02, black for
	the big func_illusionary panels, the colour being whatever the artist left
	in the fully-transparent texels.  It also explains why hl2_translucent 0 and
	1 were pixel-identical on those surfaces -- there was no blend to withhold.

	So route on what the material SAYS IT IS first.  A LightmappedGeneric decal
	keeps vmt/lightmapped, which is the shader that actually implements the
	permutations it was handed, and still gets its polygonOffset below (:1937).
	`st->decal` is kept as a fallback for materials whose type this chain does
	not otherwise recognise, which is what it was presumably for.

	WorldVertexTransition needs no exclusion here: its own arm is at :1443,
	above this one, so a transition decal never reaches this test.

	FTESurf Patch 238 adds the kill switch this should have shipped with.  The
	blast radius is the widest of any material change here -- it moves EVERY
	decal material in the library, 224 maps of infodecal plus every decal brush
	face, off model lighting and onto the lightmap.  hl2_decallit 0 restores the
	old routing exactly, so "did P236 do this" is answerable with one cvar and a
	map reload rather than by rebuilding the plugin.
	*/
	else if (!Q_strcasecmp(st->type, "VertexlitGeneric") ||
	         (st->decal && (!hl2_decallit || !hl2_decallit->ival ||
	                        Q_strcasecmp(st->type, "LightmappedGeneric"))))
	{

			if (*st->envmap && st->envfrombase)
			{
				if (st->halflambert)
					Q_strlcpy(st->type, "vmt/vertexlit#ENVFROMBASE#HALFLAMBERT", sizeof(st->type));
				else
					Q_strlcpy(st->type, "vmt/vertexlit#ENVFROMBASE", sizeof(st->type));
			}
			else if (*st->envmap && st->envfromnorm)
			{
				if (st->halflambert)
					Q_strlcpy(st->type, "vmt/vertexlit#ENVFROMNORM#HALFLAMBERT", sizeof(st->type));
				else
					Q_strlcpy(st->type, "vmt/vertexlit#ENVFROMNORM", sizeof(st->type));
			}
			/*
			FTESurf Patch 268 B: the dedicated $envmapmask arm this chain never had.

			The lightmapped and transition arms both pick #ENVFROMMASK when a
			material names an $envmapmask; the model arm falls through to the plain
			program and reads the mask out of the normalmap's alpha instead, so a
			VertexlitGeneric with a real mask texture has never used it.

			Only with NO $bumpmap, because that is Source's own rule: the bumped
			helper kills $envmapmask and $basealphaenvmapmask outright
			(vertexlitgeneric_dx9_helper.cpp:187-199), and the unbumped pixel shader
			is the one that samples the mask (_ps20b.fxc:327).  Under bit 1 only --
			it changes which program a shipped material gets.
			*/
			else if (*st->envmap && *st->envmapmask && !*st->normalmap &&
			         hl2_envmap_source && (hl2_envmap_source->ival & 1))
			{
				if (st->halflambert)
					Q_strlcpy(st->type, "vmt/vertexlit#ENVFROMMASK#HALFLAMBERT", sizeof(st->type));
				else
					Q_strlcpy(st->type, "vmt/vertexlit#ENVFROMMASK", sizeof(st->type));
			}
			else
			{
				if (st->halflambert)
					Q_strlcpy(st->type, "vmt/vertexlit#HALFLAMBERT", sizeof(st->type));
				else
					Q_strlcpy(st->type, "vmt/vertexlit", sizeof(st->type));
			}



		Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);

		if (*st->normalmap) {
			Q_strlcatfz(script, &offset, sizeof(script),	"\tnormalmap \"%s%s.vtf\"\n", strcmp(st->normalmap, "materials/")?"materials/":"", st->normalmap);
		}
#if 0
		if (st->additive)
			st->blendfunc = "src_one dst_one";
		else if (st->translucent)
			st->blendfunc = "src_alpha one_minus_src_alpha";
#endif

		/*
		FTESurf Build 13: this used to be an unconditional

		    reflectcube $cube:materials/skybox/sky_day03_06

		on EVERY VertexLitGeneric material -- every Source model in the game
		reflecting one hardcoded Half-Life 2 daytime sky, regardless of the map
		it was standing in and regardless of whether the material asked for a
		reflection at all.

		It also actively blocked the correct answer.  gl_backend.c:1408 prefers
		the shader's own reflectcube over the batch's, and gl_alias.c:2162
		already sets a model batch's envmap to Mod_CubemapForOrigin() -- the
		nearest env_cubemap to that entity.  So the moment VBSP_LoadCubemaps
		populates mod->envmaps, props get their map's real baked cubemap for
		free, but only if nothing has pinned a cube on the shader first.

		So it is simply deleted.  A material that names a REAL cubemap still
		gets one from the shared emitter further down (which already skips
		`env_cubemap`, correctly -- that name means "the nearest one", and as
		of this build something finally answers it).  See VBSP_LoadCubemaps in
		mod_vbsp.c for the measurement that started this.
		*/
		//FTESurf Patch 268 B: + envmapcontrast, which is empty unless the material
		//wrote $envmapcontrast AND hl2_envmap_source bit 2 is on.
		Q_strlcatfz(script, &offset, sizeof(script),	"\t{\n\t\tprogram \"%s%s%s%s%s\"\n\t\tmap $rt:$linear:rtenvsphere\n\t}\n", st->type, progargs, envmaptint, envmapsat, envmapcontrast);

	}
	else if (!Q_strcasecmp(st->type, "LightmappedGeneric"))
	{
		/* reflectmask from diffuse map alpha */

			if (*st->envmap && st->envfrombase)
				Q_strlcpy(st->type, "vmt/lightmapped#ENVFROMBASE", sizeof(st->type));
			else if (*st->envmap && st->envfromnorm)
				Q_strlcpy(st->type, "vmt/lightmapped#ENVFROMNORM", sizeof(st->type));
			else if (*st->envmap && *st->envmapmask) /* dedicated reflectmask */
				Q_strlcpy(st->type, "vmt/lightmapped#ENVFROMMASK", sizeof(st->type));
			else /* take from normalmap */
				Q_strlcpy(st->type, "vmt/lightmapped", sizeof(st->type));

			//FTESurf Patch 268 B: + envmapcontrast, empty unless bit 2 is on.
			Q_strlcatfz(script, &offset, sizeof(script),	"\tprogram \"%s%s%s%s%s\"\n", st->type, progargs, envmaptint, envmapsat, envmapcontrast);


		Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);

		if (*st->normalmap)
			Q_strlcatfz(script, &offset, sizeof(script),	"\tnormalmap \"%s%s.vtf\"\n", strcmp(st->normalmap, "materials/")?"materials/":"", st->normalmap);
	}
	else
	{
		/* render-target camera/monitor - eukara*/
		if (!Q_strcasecmp(st->tex[0].name, "_rt_Camera"))
			Q_strlcatfz(script, &offset, sizeof(script),
			"\t{\n"
				"\t\tprogram vmt/rt\n"
				"\t\tmap $rt:base\n"
			"\t}\n"/*, progargs*/);
		else
		{
			/*
			FTESurf Patch 263: SAY SO.

			This arm was silent, and that silence is the whole reason bonus 4 of
			surf_monolith drew as a notexture checkerboard for as long as this
			plugin has existed.  A material whose class is not implemented and a
			material that is implemented badly looked identical from the console:
			no warning, no count, nothing in hl2_missing (the class parsed fine --
			it is the class we do not know).  The WindowImposter arm above has the
			full chain.

			Not a rare shape.  Across the library's 1310 pakfiles, 171,076 VMTs:
			790 materials name one of 39 unimplemented classes, on 209 maps -- one
			map in six ships at least one.

			Con_DPrintf, not a warning: most of those 790 are cosmetic props and
			this belongs beside the other per-material diagnostics in the
			developer log, not in front of someone playing.  The count and the
			names go to the per-map census in mod_vbsp.c, which is where a number
			is actually read.

			st->type is overwritten on the very next line, so the class has to be
			captured BEFORE it rather than after.
			*/
			{
				int u;
				for (u = 0; u < 4 && *vmt_stat_unknown_name[u]; u++)
					if (!Q_strcasecmp(vmt_stat_unknown_name[u], st->type))
						break;
				if (u < 4 && !*vmt_stat_unknown_name[u])
					Q_strlcpy(vmt_stat_unknown_name[u], st->type, sizeof(vmt_stat_unknown_name[u]));
				vmt_stat_unknown++;
				Con_DPrintf("[vmt] %s: shader class \"%s\" is not implemented -- drawing it as vmt/unlit\n",
					shortname, st->type);
			}

			/* the default should just be unlit, let's not make any assumptions - eukara*/
			Q_strlcpy(st->type, "vmt/unlit", sizeof(st->type));
			Q_strlcatfz(script, &offset, sizeof(script),	"\tprogram \"%s%s\"\n", st->type, progargs);
			Q_strlcatfz(script, &offset, sizeof(script),	"\tdiffusemap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);
		}
	}

	/*
	FTESurf Patch 217, second half: A FRACTIONAL $alpha IS ITSELF TRANSLUCENCY.

	The first half of this patch made $translucent read its value, which is right
	and which on its own is WRONG, because it silently dropped the other half of
	Source's rule.  In Source, $alpha between 0 and 1 blends the material whether
	or not $translucent is set; $translucent 0 does not veto it.  Reading the 0 and
	stopping there turns surf_kitsune's grids -- authored as

	    UnlitGeneric { $basetexture "grids/grid_red"  $translucent 0
	                   $selfillum 1  $alpha 0.75 }

	-- from 75%-opaque glass into solid walls.  Reported straight off the first
	build of it: "the stage textures on kitsune are supposed to be transparent,
	they are like dark with coloured highlights which you can see through slightly,
	but in the current patch, they come back solid."  The $translucent 0 in that
	material is the author declining the TRANSLUCENT FLAG, not declining the alpha.

	Tested after the keys are parsed rather than inside the $alpha branch, because a
	VMT may write the two in either order and both orders are live: of 1633 packed
	materials that write both keys, 304 put $alpha first.

	Scope, measured across the 2394 VBSP maps in the library that carry an embedded
	pakfile (counted per map, so a material packed into several is counted several
	times): 2118 materials carry a fractional $alpha, and 39 of those -- across 4
	maps, surf_kitsune among them -- also say $translucent 0.  Those 39 are exactly
	what the first half of this patch turned solid, and exactly what this restores.
	The other 906 of the 945 $translucent-0 materials have no fractional alpha and
	stay opaque, which was the point of reading the value at all.

	Zero is excluded because it is already handled where $alpha is parsed -- a
	surface at zero opacity is turned into nodraw there, which is both cheaper and
	what it looks like.
	*/
	if (st->alphaval > 0 && st->alphaval < 1)
		st->translucent = 1;

	if (st->translucent) {
		vmt_stat_translucent++;
		/*
		FTESurf build 12: hl2_translucent 0 makes these OPAQUE.

		A blended surface cannot write depth, so it has to be sorted
		back-to-front and everything behind it is drawn whether it ends up
		visible or not.  On a map with a lot of glazing that ordering cost, not
		the fill, is what the surfaces are actually charging you.  Dropping the
		blendfunc keeps the material's own program and its lightmap -- these are
		world brushes, and replacing them with a flat colour would look far worse
		than they cost -- and turns them into ordinary opaque geometry.

		NOT DITHERED, deliberately, and this is the one place in build 12 that
		is not.  These materials emit a top-level `program`, and a dither has to
		be a PASS; adding one alongside would draw the surface twice, which is
		the opposite of the point.  Dithering these properly means replacing
		their program outright and losing the lightmap with it, so it is left as
		an honest binary until there is a reason to spend a shader on it.
		Source's actual expensive glass is the Refract type, and that dithers.
		*/
		if (!hl2_translucent || hl2_translucent->ival)
			st->blendfunc = "src_alpha one_minus_src_alpha";
	}

	/*
	FTESurf Patch 187: $additive on a PROGRAM material, which only the pass-based
	arms above have ever honoured.

	surf_demise's smoke is why this matters and the texture proves it:
	elly/smokeanimated is DXT1 with NO alpha channel at all, so there is nothing
	for src_alpha to read -- the author made it transparent with $additive, where
	black pixels contribute nothing.  Blending it by alpha instead draws an
	opaque grey rectangle no matter how the alpha is computed, which is what
	"the smoke is solid" looked like even after the alpha fix.

	Additive wins over $translucent where a material writes both, as it does in
	Source.  Gated on the same cvar so hl2_translucent 0 still forces every
	blended world surface opaque -- an additive surface costs exactly what a
	translucent one does, so exempting it would make that switch a half-truth.

	Applied whether or not the material also says $translucent.  Counted over
	the library's embedded pakfiles, 462 program-based materials in 220 maps
	write both -- demise's smoke among them -- and 232 more in 102 maps write
	$additive ALONE.  Those 232 are currently drawn fully opaque, which for a
	glow, a light shaft or a hologram means a solid box where the mapper put
	something you were meant to see through.  There is no reading of $additive
	under which opaque is right, so the narrower "only upgrade an already
	blended material" version was not worth the maps it would leave broken.
	*/
	if (st->additive && (!hl2_translucent || hl2_translucent->ival))
	{
		//"add" rather than spelling out the factors: Shaderpass_BlendFunc
		//(gl_shader.c:4205) turns it into SRCBLEND_ONE|DSTBLEND_ONE by name, and
		//it is the same word the pass-based arms above already use.
		st->blendfunc = "add";
		if (!st->translucent)
			vmt_stat_translucent++;	//it costs what a translucent surface costs; the census should say so
	}

	if (st->decal) {
		Q_strlcatfz(script, &offset, sizeof(script),	"\tpolygonOffset 1\n");
		Q_strlcatfz(script, &offset, sizeof(script),	"\trgbGen vertex\n");
	}

	if (*st->fullbrightmap)
		Q_strlcatfz(script, &offset, sizeof(script), "\tfullbrightmap \"%s%s.vtf\"\n", strcmp(st->fullbrightmap, "materials/")?"materials/":"", st->fullbrightmap);
	else if (st->selfillum == 1)
		Q_strlcatfz(script, &offset, sizeof(script), "\tfullbrightmap \"%s%s.vtf\"\n", strcmp(st->tex[0].name, "materials/")?"materials/":"", st->tex[0].name);

	if (*st->envmapmask)
		Q_strlcatfz(script, &offset, sizeof(script),	"\treflectmask \"%s%s.vtf\"\n", strcmp(st->envmapmask, "materials/")?"materials/":"", st->envmapmask);
	if (*st->envmap && strcmp(st->envmap, "env_cubemap"))
	{
		/*
		FTESurf Patch 264: a cubemap that ships only as "<name>.hdr.vtf".

		Source's convention is that the author writes the suffix into the VMT
		value -- $envmap "cubemaps/blur_nebula.hdr" -- and that already works
		here, because this line appends ".vtf" to whatever the value says.
		surf_ab spells it that way 82 times.  surf_agony names the SAME file 45
		times with the ".hdr" left off, and gets nothing at all: the plain .vtf
		does not exist, so the cubemap silently never loads.

		Measured over the 1310-map library: 85 $envmap references in 13 maps
		naming 3 distinct textures resolve ONLY as .hdr.vtf --
		cubemaps/blur_whitenebula (83 refs, 11 maps), fakeskies/bikini_bottom_night
		(surf_bikini_bottom's fake sky) and one baked ocean cube in kz_bhop_badg3s.
		Zero via $basetexture or any other texture-valued key, which is why this
		probe is on the envmap emission alone and not on the others.

		A FALLBACK, NEVER AN OVERRIDE, and the distinction is load-bearing:
		13,291 baked cubemaps ship BOTH cX_Y_Z.vtf and cX_Y_Z.hdr.vtf, 56,132
		references between them, and every one must keep taking the tone-mapped
		LDR bake.  The probe below runs only when the plain .vtf is absent, and
		when both are absent it emits the string it always emitted, so a miss
		still reads the same in the log.

		Not done in the engine's image loader instead, which was the obvious
		place: $envmap becomes `reflectcube`, which carries IF_TEXTYPE_CUBE
		(gl_shader.c:3129) and lands on the PTI_CUBE branch of
		Image_LoadHiResTextureWorker (image.c:14696).  That branch does not read
		tex_extensions[] at all -- it uses a hardcoded cubeexts[] = {"", ".ktx",
		".dds"} -- so adding "hdr.vtf" to r_imageextensions would fix 0 of the 85
		while costing an FS probe on every 2D texture load in the engine.  It
		would not even do that: tex_extensions[].name is `char[6]` (image.c:14175)
		and ".hdr.vtf" truncates to ".hdr." with no warning.

		Cost here is one FS_FLocateFile per material that names a real $envmap,
		and a second only when the first misses.
		*/
		char cube[MAX_QPATH], hdr[MAX_QPATH];
		const char *pfx = strcmp(st->envmap, "materials/")?"materials/":"";
		Q_snprintfz(cube, sizeof(cube), "%s%s.vtf", pfx, st->envmap);
		if (!fsfuncs->LocateFile(cube, FSLF_IFFOUND, NULL))
		{
			Q_snprintfz(hdr, sizeof(hdr), "%s%s.hdr.vtf", pfx, st->envmap);
			if (fsfuncs->LocateFile(hdr, FSLF_IFFOUND, NULL))
			{
				vmt_stat_hdrenvmap++;
				Con_DPrintf("[vmt] %s: $envmap \"%s\" has no %s -- using %s\n",
					shortname, st->envmap, cube, hdr);
				Q_strlcpy(cube, hdr, sizeof(cube));
			}
		}
		Q_strlcatfz(script, &offset, sizeof(script),	"\treflectcube \"%s\"\n", cube);
	}
	/*
	FTESurf Patch 268 B: the literal $envmap "env_cubemap" -- the sentinel.

	The block above deliberately skips this spelling, because "env_cubemap" is not
	a texture name: it means "the nearest baked cubemap", which is curbatch->envmap
	and which gl_backend.c's T_GEN_REFLECTCUBE already falls back to.  What that
	reasoning missed is the PERMUTATION GATE (gl_backend.c:4410), which tests only
	TEXLOADED(curtexnums->reflectcube || reflectmask) and never curbatch->envmap --
	so with no reflectcube line the whole REFLECTCUBEMASK block compiles out and
	the batch's cubemap is never sampled at all.

	"$envcubemap" is a real 1x1x6 mid-grey cube created by Shader_Init, so the gate
	sees a loaded texture and fires; T_GEN_REFLECTCUBE recognises the sentinel
	pointer and binds curbatch->envmap instead of it.  r_envcubemap is the live A/B
	(a plugin-side switch alone cannot turn it off until the next map load --
	Shader_Reset keeps defaulttextures across a regenerate).

	st->wantenvcube is set far above, before the type dispatch, and only for the
	three LIT classes: Water, Refract and the unlit classes are excluded because
	their programs' REFLECTCUBEMASK blocks were not audited for this patch and
	hl2_water / hl2_refract own those materials.  A material that already carries
	$envmapmask + env_cubemap reflects today and is unchanged -- this line only
	makes the gate fire, it does not alter the mask.
	*/
	else if (st->wantenvcube)
		Q_strlcatfz(script, &offset, sizeof(script),	"\treflectcube \"$envcubemap\"\n");
	if (st->alphatest)
		Q_strlcatfz(script, &offset, sizeof(script), "\talphatest ge128\n");
	if (st->culldisable)
		Q_strlcatfz(script, &offset, sizeof(script), "\tcull disable\n");
	/*
	FTESurf Patch 290: $ignorez IS NOT HONOURED HERE, AND SAYING SO IS THE FIX.

	This used to emit "\tnodepth\n" at the top level.  `nodepth` is a PASS keyword
	(gl_shader.c:4838) and its handler dereferences ps->pass, so at the top level
	the parser prints a developer-only "Unknown shader directive", skips the rest
	of the line and carries on -- the material rendered exactly as if the line had
	never been written, and every log carried

	    Unknown shader directive parsing sprites/light_glow02_add_noz: "nodepth"

	which reads like a spelling mistake rather than a dropped feature.  The
	SpriteCard arm in this same file (:3072-3076) already documents this trap and
	puts its `nodepth` inside the pass; it is the only arm that got it right.

	It is NOT simply moved into a pass here because this is the shared tail of the
	generic emit, after every per-type arm has finished: some arms emit an explicit
	pass, others emit a bare top-level program and let Shader_Finish back-fill one
	(gl_shader.c:6221), and there is no top-level depth key to fall back on.
	Emitting a pass just to carry this would change what those materials rasterise.

	So it is counted and declined, like every other declined Source feature in this
	plugin -- no behaviour change (it never worked), one honest census number
	instead of a misleading warning.  The one place it would matter most is already
	covered: the env_lightglow flares force DRAWFLAG_NODEPTHTEST from the CSQC draw
	regardless of what their material says.
	*/
	if (st->ignorez)
		vmt_stat_ignorez++;
	//Patch 195: animpass means the program lives inside a PASS, and progblendfunc
	//has no top-level program to attach to there -- the pass emitted its own
	//blendFunc instead.  Emitting this as well would be a second, ignored
	//blendfunc on a material that already has the right one.
	if (st->blendfunc && !st->animpass)
		Q_strlcatfz(script, &offset, sizeof(script), "\tprogblendfunc %s\n", st->blendfunc);
	//FTESurf Patch 188: a top-level `rgbGen const` used to be emitted here.  It
	//was a no-op at the top level and it passed the value through with its
	//brackets still on, so it could not have worked even in a pass.  $color and
	//$color2 are now handled where each material type can actually receive them
	//-- see the #COLOR block near the top of this function, and the UnlitGeneric
	//arm's pass rgbGen.

	Q_strlcatfz(script, &offset, sizeof(script), "}\n");

	/*
	FTESurf: SAY WHAT THE MATERIAL ACTUALLY CAME OUT AS.

	The translucency census counts materials whose `translucent` flag is set, which
	is not the same claim as "this surface will blend" -- the flag has to survive
	into a progblendfunc, and the program has to be handed an #ALPHA to blend WITH.
	surf_kitsune reported `11 translucent` while rendering entirely opaque, and
	there was no way to tell which of those three steps had dropped it.

	One line per material that carries any alpha or translucency at all, so the
	whole chain is legible next to the count: 11 on kitsune, and self-limiting
	everywhere -- an opaque material prints nothing.
	*/
	if (st->translucent || st->additive || st->alphaval)
		Con_DPrintf("[vmt] %s: alpha %.3f translucent %i additive %i animpass %i blendfunc %s\n",
			shortname, st->alphaval, st->translucent, st->additive, st->animpass,
			st->blendfunc ? st->blendfunc : "(NONE -- will not blend)");

	/*
	FTESurf: the generated script, verbatim, for the material types whose
	program lives INSIDE A PASS.

	There is otherwise no way to see what was emitted for one of these.
	`shader_here` prints a shader body, but it traces the WORLD model, and the
	materials that take this path are overwhelmingly on brush ENTITIES -- the
	combine shield that motivated it is func_brush *19 -- so the crosshair goes
	straight through and reports the wall behind.  Hunting that with screenshots
	cost an hour; printing eight lines costs nothing and is only emitted at
	`developer 1` for the handful of materials involved.
	*/
	if (st->animpass)
		Con_DPrintf("[vmt] %s generated:\n%s", shortname, script);

	LoadMaterialString(ps, script);

	if (st->sourcefile)
	{	//cat the original file on there...
		if (st->savefile)
		{
			char *winsucks;	//strip any '\r' chars in there that like to show as ugly glyphs.
			for (winsucks = st->sourcefile; *winsucks; winsucks++)
				if (*winsucks=='\r')
					*winsucks = ' ';

			Q_StrCat(st->savefile, "\n/*\n");
			Q_StrCat(st->savefile, st->sourcefile);
			Q_StrCat(st->savefile, "*/");
		}
		plugfuncs->Free(st->sourcefile);
	}
}
static qboolean VMT_ReadVMT(const char *fname, vmtstate_t *st)
{
	char *line, *file = NULL;
	com_tokentype_t ttype;
	char token[MAX_QPATH*2];
	char norm[MAX_QPATH*2];	//FTESurf Patch 261
	char *prefix="", *postfix="";
	char *c;

	if (strstr(fname, "://"))
		return false;	//don't try to handle urls.

	/*
	FTESurf Patch 261: a `patch` VMT that writes its include the Windows way
	resolves to nothing, and the surface it names is drawn as a pink/black
	checkerboard.

	surf_monolith ships materials/effects/tp_refract_pink.vmt:

	    patch
	    {
	        include "materials\effects\tp_refract_algetic.vmt"
	        replace { "$REFRACTTINT" "{204 0 102}" }
	    }

	The prefix test below was a raw strncmp against "materials/".  That compare
	runs to index 9, finds '\' where it wants '/', and concludes the name is
	un-prefixed -- so a SECOND "materials/" is glued on and the load asks for
	"materials/materials/effects/tp_refract_algetic.vmt".  LoadFile fails,
	VMT_ReadVMT returns false, the `include` arm in VMT_ParseBlock returns NULL,
	and Shader_LoadVMT reports the whole material as found-but-unparsed and
	falls back to no_texture.

	WHAT IS NOT BROKEN, because it decides how large this fix has to be.  An
	include of "materials/agency\glass\glass01_break.vmt" works today: the
	compare matches at index 0, no second prefix is added, and FS_GetCleanPath
	(fs.c:2868) treats '\' as a segment separator and rewrites it on the way in.
	Every backslash the FILESYSTEM sees is already handled.  Only the separator
	at index 9 matters, because this function decides before the filesystem is
	ever asked.  That distinction is the difference between a scary number and a
	true one: the first count of this defect flagged every backslash-bearing
	include and reported 41 across a 219-map sample, and most of those were
	names FS_GetCleanPath was already fixing.

	Measured properly, over all 1310 maps and all 72925 include directives:
	23 resolve differently once this is fixed, on 7 maps -- bhop_angst 7,
	surf_valpect 4, surf_doomsday 3, surf_sinsane2 3, surf_zen2 3, surf_angst 2,
	surf_monolith 1.  20 of the 23 name a target that is present in the map's
	own pakfile, so those are provably broken today rather than merely
	mis-spelt.  (The 3 that are not are surf_sinsane2's, whose GridBase_Diffuse
	is in neither the map pak nor this fix's reach -- that material may still
	fail afterwards, for an unrelated reason.)

	Normalising HERE rather than at the `include` call site because this is the
	single funnel -- Shader_LoadVMT's top-level call, the include recursion and
	any future caller all arrive through it -- and because VMT_BottomNormalise
	above already establishes the house idiom: fold '\' to '/', compare the
	prefix case-blind.

	Case is the same defect in a different hat, and it is NOT theoretical: 3 of
	the 23 are surf_doomsday writing `include "MATERIALS/PK02/PK02_SAND01.vmt"`.
	strncmp is case-SENSITIVE, so a capitalised prefix double-prefixes exactly
	as the backslash does.  The extension test alongside it has the same flaw
	(a name ending ".VMT" is asked for as "foo.VMT.vmt"); that one is fixed
	because the compare was wrong rather than because anything is failing --
	zero of the 72925 includes end in a capitalised extension.  Neither change
	can regress a name that resolves today: a case-insensitive match is a strict
	superset of the exact one it replaces, and every name it newly matches is
	one that gets a second prefix glued on today.
	*/
	Q_strlcpy(norm, fname, sizeof(norm));
	for (c = norm; *c; c++)
		if (*c == '\\')
			*c = '/';
	fname = norm;

	//don't dupe the mandatory materials/ prefix
	if (Q_strncasecmp(fname, "materials/", 10))
		prefix = "materials/";
	if (Q_strcasecmp(fsfuncs->GetExtension(fname, NULL), ".vmt"))
		postfix = ".vmt";
	Q_snprintfz(token, sizeof(token), "%s%s%s", prefix, fname, postfix);

	file = fsfuncs->LoadFile(token, NULL);
	if (file)
	{
		st->filefound = true;	//FTESurf Patch 181, see the field's comment
		if (st->savefile)
		{
			if (st->sourcefile)
			{
				Q_StrCat(&st->sourcefile, fname);
				Q_StrCat(&st->sourcefile, ":\n");
				Q_StrCat(&st->sourcefile, file);
			}
			else
				Q_StrCat(&st->sourcefile, file);
		}

		line = file;
		line = cmdfuncs->ParseToken(line, st->type, sizeof(st->type), &ttype);
		line = cmdfuncs->ParseToken(line, token, sizeof(token), &ttype);
		if (!strcmp(token, "{"))
		{
			line = VMT_ParseBlock(fname, st, line);
		}

		plugfuncs->Free(file);
		return !!line;
	}
	return false;
}
const char *VBSP_ColourCorrectScript(const char *name);	//FTESurf Patch 288, mod_vbsp.c

static qboolean Shader_LoadVMT(parsestate_t *ps, const char *filename, void (*LoadMaterialString)(parsestate_t *ps, const char *script))
{
	vmtstate_t st;

	/*
	FTESurf Patch 288: the colour-correction post-process, which is not a
	material and has no file.

	It has to arrive HERE because a plugin has no other way to define a shader by
	name -- Material_RegisterLoader's ReadMaterial hook is the only callback the
	engine makes when a shader name does not resolve, and LoadMaterialString is
	the only way back in.  mod_vbsp.c generates the script at map load from the
	map's color_correction entities and hands it over on request; the name is
	content-derived, so this can only ever match the grade it was built for.

	FIRST, before VMT_ReadVMT, for cost rather than correctness: the miss counter
	below deliberately does not count names with no '/' or with a "_glsl" suffix,
	and this name has a '/' and would otherwise be reported as a Source material
	the map needs and cannot find.
	*/
	{
		const char *cc = VBSP_ColourCorrectScript(filename);
		if (cc)
		{
			LoadMaterialString(ps, cc);
			return true;
		}
	}

	memset(&st, 0, sizeof(st));
	st.savefile = NULL;//ps->saveshaderbody;
	if (!VMT_ReadVMT(filename, &st))
	{
		/*
		FTESurf build 11: COUNT THE MISSES, and remember the first name.

		A material that will not resolve is the entire reason CS:GO and TF2 were
		mounted -- two maps out of 1308 reference them and go untextured
		otherwise.  Mounting them permanently to cover that costs 13 seconds of
		every launch (see ftesurf/fs_addons.txt for the measurement), so they are
		not mounted any more, and this is what replaces them: the map that
		actually needs one says so by name, and the fix is one command.

		Counted here rather than in VMT_ReadVMT because that recurses through
		patch/include chains whose intermediate lookups are allowed to fail.
		*/
		/*
		WHAT DOES NOT COUNT, and why each one had to be excluded by measurement
		rather than by guessing.  A counter that fires on every map means nothing
		on any map, and the first two versions of this did exactly that:

		  "<name>_glsl"   the shader system PROBING for a hand-written GLSL
		                  variant.  Supposed to miss.  surf_666 -- every material
		                  of which resolves -- produced 373 of these.
		  "tools/", "sky/" tool and sky materials, which the plugin synthesises
		                  rather than loads.  A further ~14 per map, on every map
		                  including the ones that are completely fine.
		  no '/' at all   engine-internal shader names -- "depthonly",
		                  "defaultwall" and friends.  A Source material path
		                  always has a directory; these are not paths and were
		                  never going to be found in a VPK.  Exactly 13 of them
		                  on surf_666, surf_null AND surf_sandtrap2, which is how
		                  they were spotted: a constant floor across unrelated
		                  maps is a property of the engine, not of the map.

		What is left is a real reference to a material that is not mounted, which
		is the only thing worth a warning.  Verified below.
		*/
		/*
		FTESurf Patch 231: a FOREIGN EXTENSION does not count either, because such a
		name is never the last word on the subject.

		R_LoadShader probes a name twice when it carries an extension: once as given,
		and then again with it stripped -- gl_shader.c:8561-8564,
		`if (strcmp(cleanname, shortname)) Shader_ParseShader(&ps, shortname)`.  The
		SECOND probe is the one with a chance: the first makes VMT_ReadVMT append .vmt
		to a name that already ends in one, asking for "<name>.vmf.vmt", which no pack
		will ever hold.

		Counting the first probe therefore reports one missing texture as two, and
		shows the wrong spelling while doing it.  surf_demise is the case: its water
		material says `$bottommaterial "liquidpack/water/unique/water_tar_beneath.vmf"`
		-- the mapper typed .vmf for .vmt -- VBSP baked that string into the texdata
		table as a real face, and the typo is what reached hl2_unresolved_first instead
		of the name the engine actually went looking for.

		Sound ONLY because that retry is unconditional.  If gl_shader.c ever stops
		retrying the stripped name, this quietly stops counting real misses -- so the
		two belong together.
		*/
		//FTESurf Patch 231: "gfx/" joins tools/ and sky/ for the same reason the
		//no-slash names were excluded in build 11 -- gfx/conback, gfx/backtile and
		//gfx/loading are QUAKE's own console and loading-screen images, probed on every
		//map by the engine's 2D code.  They are not Source materials, no VPK will ever
		//hold one, and they were 13 constant entries in the running list on every map.
		size_t fl = strlen(filename);
		const char *ext = fsfuncs->GetExtension(filename, NULL);
		if ((fl < 5 || strcmp(filename+fl-5, "_glsl")) &&
		    strchr(filename, '/') &&
		    strncmp(filename, "tools/", 6) &&
		    strncmp(filename, "sky/", 4) &&
		    strncmp(filename, "gfx/", 4) &&
		    !(*ext && strcmp(ext, ".vmt")))
		{
			//FTESurf Patch 181: a material that WAS found and would not parse is
			//a different fault with a different fix, and must not be reported as
			//a missing asset pack -- see vmtstate_t::filefound.
			if (st.filefound)
			{
				if (!VMT_CensusSeen(vmt_stat_unparsed_name, vmt_stat_unparsed, filename))
				{
					if (vmt_stat_unparsed < VMT_MISSING_LIST)
						Q_strlcpy(vmt_stat_unparsed_name[vmt_stat_unparsed], filename, MAX_QPATH);
					vmt_stat_unparsed++;
				}
			}
			else
			{
				if (!VMT_CensusSeen(vmt_stat_missing_name, vmt_stat_missing, filename))
				{
					if (vmt_stat_missing < VMT_MISSING_LIST)
						Q_strlcpy(vmt_stat_missing_name[vmt_stat_missing], filename, MAX_QPATH);
					vmt_stat_missing++;
				}
			}
		}

		if (st.sourcefile)
			plugfuncs->Free(st.sourcefile);
		return false;
	}

	//FTESurf Patch 233: this VMT loaded, and if it names an underside then it is the
	//only thing in the map that knows which water that underside belongs to.  Recorded
	//here rather than in VMT_ParseBlock because that recurses through patch/include
	//chains whose intermediate states are not materials in their own right.
	VMT_BottomRecord(filename, st.bottommaterial);

	//FTESurf: the proxy graph is complete only once every include and patch has
	//been folded in, so it is compiled into coefficients HERE and not inside
	//VMT_ParseBlock -- a chain whose PlayerProximity comes from a base material
	//and whose Subtract comes from the patch that includes it is a real shape.
	VMT_ResolveProxies(&st);

	Shader_GenerateFromVMT(ps, &st, filename, LoadMaterialString);
	return true;
}

static struct sbuiltin_s vmtprograms[] =
{
//we don't know what renderer the engine will need...
#ifdef FTEPLUGIN
	#ifndef GLQUAKE
		#define GLQUAKE
	#endif
	#ifndef VKQUAKE
		#define VKQUAKE
	#endif
	#ifndef D3DQUAKE
		#define D3DQUAKE
	#endif
#endif
#include "mat_vmt_progs.h"
	{QR_NONE}
};
static plugmaterialloaderfuncs_t vmtfuncs =
{
	"HL2 VMT",
	Shader_LoadVMT,

	vmtprograms,
};

qboolean VMT_Init(void)
{
	fsfuncs = plugfuncs->GetEngineInterface(plugfsfuncs_name, sizeof(*fsfuncs));
	if (!fsfuncs)
		return false;
	return plugfuncs->ExportInterface(plugmaterialloaderfuncs_name, &vmtfuncs, sizeof(vmtfuncs));
}
