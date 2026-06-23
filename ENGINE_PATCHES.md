# Engine patches (fteqw `fteqw64.exe`)

Custom modifications to the FTEQW engine for the nettest mod. These live in the
engine **source tree** at `C:\msys64\home\Lex\fteqw\engine` and must be
**re-applied + rebuilt** whenever you pull a new upstream fteqw, because they
fork the engine. The deployed binary is `C:\FTEQuake\fteqw64.exe` (the previous
build is kept as `fteqw64.exe.prev` for rollback).

## Build / deploy

```powershell
# ucrt64 toolchain on PATH; incremental (only changed .c recompiles + relink, ~10-30s)
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"
& "C:\msys64\usr\bin\make.exe" -C "C:\msys64\home\Lex\fteqw\engine" m-rel FTE_TARGET=win64
# output: engine\release\fteqw64.exe  -> copy over C:\FTEQuake\fteqw64.exe (back up the old one to .prev first)
```
`m-rel` = the merged GL+VK+SW+server win64 client (the deployed target). A
harmless `pattern recipe did not update peer target p_script.d` warning may
print; the `.o` and `.exe` still build.

---

## Patch 1 — `watercliptype` (rain splashes on water surfaces)

**File:** `engine/client/p_script.c`  ·  search the source for `nettest`

**Why:** particle collision (the `cliptype` splash) traces against
`MASK_WORLDSOLID` (= `FTECONTENTS_SOLID | FTECONTENTS_WINDOW`), which **excludes
water** (`FTECONTENTS_WATER` is a fluid). So a clipping rain drop falls straight
*through* a water surface and only splashes on the solid floor beneath. There is
no cfg-only fix — the engine never reports a water "hit" for collision particles.

**What it adds:** a new opt-in per-particle-type field `watercliptype` (mirrors
`cliptype`). When set, a clipping particle that crosses the air→water boundary
spawns that effect at the refined surface point (via a cheap `PointContents`
test — no extra trace) and dies, instead of falling through. Lets the rain throw
a **separate, bigger** splash on water (`weather.cfg` `r_part splashbig`) vs the
land splash.

**The 5 edit sites** (all tagged `//nettest`):

1. **Struct field** — in `part_type_t`, after `int cliptype;`:
   ```c
   int watercliptype;	//nettest: ... P_INVALID = off.
   ```
2. **Init A** — in the type-reset (after `ptype->cliptype = P_INVALID;` ~line 517):
   ```c
   ptype->watercliptype = P_INVALID;	//nettest
   ```
3. **Init B** — in `P_NewParticleType` (after the other `ptype->cliptype = P_INVALID;`):
   ```c
   ptype->watercliptype = P_INVALID;	//nettest
   ```
4. **Parser** — after the `else if (!strcmp(var, "cliptype"))` block:
   ```c
   else if (!strcmp(var, "watercliptype"))	//nettest
   {
       assoc = P_AllocateParticleType(config, value);
       ptype = &part_type[pnum];
       ptype->watercliptype = assoc;
   }
   ```
5. **Detection** — in the collision block `if (type->cliptype>=0 && r_bouncysparks.ival)`,
   immediately after `int e;` and **before** the solid `CL_TraceLine`: a
   `PointContents`-based air→water crossing check that binary-refines the surface,
   sets `p->die = -1`, and `P_RunParticleEffectType(surface, '0 0 1',
   clipcount/watercliptype.count, watercliptype)` then `continue;`. Gated on
   `type->watercliptype != P_INVALID` so snow / other clippers are untouched.
   Full block is in the source with a `WATER-SURFACE SPLASH` comment.

**Mod side (`particles/weather.cfg`):** `r_part rain` gets `watercliptype splashbig`;
`r_part splashbig` is the bigger/higher water splash. On an **unpatched** engine
the `watercliptype` line just warns and is ignored (harmless), and water rain
falls through as before.

**Cost:** +1 `PointContents` per clipping drop per frame (much cheaper than the
`CL_TraceLine` already running); the binary search only runs for drops actually
crossing a surface.

---

## Patch 2 — `waterringtype` (per-drop water ripple ring)

**File:** `engine/client/p_script.c` · search `waterringtype` (all sites tagged `//nettest`)

**Why:** the engine-path water *ring* used to be a QC random sampler (`Rain_WaterRingTick`) that fired ~dozens of downward `traceline`s per frame to *guess* where water was — so the rings didn't land on actual drops. With Patch 1, the engine already knows the exact air↔water point each drop crosses. This patch reuses that point to spawn a **second** effect (the flat ripple ring) right there, so rings land on real drops like the CSQC pool does — and the QC sampler is deleted.

**What it adds:** a per-type field `waterringtype` (sibling of `watercliptype`). In the same water-crossing branch as Patch 1, if `waterringtype` is set, spawn it at the refined surface point. **No extra trace/search** — it reuses Patch 1's computed point.

**The 5 edit sites** (mirror Patch 1, tagged `//nettest`):
1. **Struct** — `int waterringtype;` after `int watercliptype;`.
2/3. **Inits** — `ptype->waterringtype = P_INVALID;` after each `ptype->watercliptype = P_INVALID;`.
4. **Parser** — a `waterringtype` case after the `watercliptype` case (same `P_AllocateParticleType` body).
5. **Detection** — in Patch 1's crossing branch, before the `watercliptype` spawn:
   ```c
   if (type->waterringtype != P_INVALID)
       P_RunParticleEffectType(wlo, wnormal, 1, type->waterringtype);
   ```
   Spawn count `1` → the ring effect's own `count` IS the density.

**Mod side:** `r_part rain` + `rain_cheapsplash` get `waterringtype ringsplash`; `r_part ringsplash` is `type udecal` (flat) with **`count 0.5`** = ~50% of water hits spawn a ring (the density/speed dial; lower = fewer/faster). The QC `Rain_WaterRingTick` sampler was **removed** from `client/cl_rain.qc`, so `cl_weather_waterring` / `cl_weather_waterring_rate` no longer affect the engine path — tune via the ringsplash `count` instead. Unpatched engine: the `waterringtype` line warns + is ignored (no engine-path rings).

**Cost:** essentially free — one extra cheap particle spawn on water-crossing drops only (reuses Patch 1's surface point); net *cheaper* than before since it removed the ~64-traceline/frame QC sampler.

---

## Patch 3 — oriented-box (`SOLID_PHYSICS_BOX`) player/bullet collision

**File:** `engine/server/world.c` · search `World_OBBTrace` and `FTE patch` (tagged in comments)

**Why:** FTE has no smooth *oriented* collision hull for movable props (the Source/VPhysics
convex-hull equivalent). Player movement and hitscan trace through `World_Move →
World_ClipMoveToEntity → World_TransformedTrace`, and that function's box-hull branch
(`model==NULL`) **ignores the entity's angles** — so `SOLID_PHYSICS_BOX` (and `SOLID_BBOX`) only ever
collide as an *axis-aligned* AABB. The two existing alternatives are both bad for a prop you stand
on: an axis-aligned box can't tilt, and `SOLID_PHYSICS_TRIMESH` (per-triangle `Mod_Trace`) is
oriented but the player wedges/stutters on triangle seams. FTE's only smooth-oriented collision is
BSP brush clip-hulls (`Q1BSP_RecursiveHullCheck`), which alias models can't use.

**What it adds:** `World_OBBTrace()` — the box analogue of the BSP/alias rotated trace. For a
`SOLID_PHYSICS_BOX` entity **with non-zero angles**, it rotates the trace into the box's local frame
(mirroring the alias `r_meshpitch`/`r_meshroll` convention so collision lines up with the rendered
model), clips against the player-expanded **axis-aligned** box hull, then rotates the result plane
normal back to world space. The cabinet is a box, so an oriented box == its true convex hull ==
Source-parity for that prop: smooth like a hull, no per-triangle bumpiness, follows any tilt. Same
path serves the player pmove (server + CSQC) **and** hitscan, so shooting a tumbling box hits its
real shape too.

**The 3 edit sites** (all tagged `FTE patch`):
0. **`World_LinkEdict`** — the "expand for rotation" condition (which enlarges `absmin/absmax` for
   tilted brushes) gains `|| solid == SOLID_PHYSICS_BOX`, so a rotated physics box's tilted corners
   aren't area-grid-culled before the oriented narrowphase runs. Only rotated boxes are affected;
   axis-aligned ones keep the tight AABB.
1. **`World_OBBTrace`** — new static fn placed right after `World_TransformedTrace`. Builds the
   rotation from the **raw** entity angles (`AngleVectors` + right-hand `VectorNegate`, NO
   `r_meshpitch`/`r_meshroll` flip — the IQM renders with raw angles, so applying the alias mesh-pitch
   pitched the collision box the wrong way vs the model), Minkowski-expands the prop box
   by the **rotation-projected** player extents (so the expansion is correct under rotation; a point
   trace = exact ray-vs-OBB), rotates start/end via the axis, runs `Q1BSP_RecursiveHullCheck` on the
   shared `box_hull`, then rotates `trace.plane.normal` back with `Matrix3x3_RM_Invert_Simple` +
   interpolates `endpos` (copied from the proven BSP path `q1bsp.c:1579-1608`).
2. **`World_ClipMoveToEntity`** — before the final `else World_TransformedTrace(...)`, a branch:
   `else if (solid == SOLID_PHYSICS_BOX && !model && (eang[0]||eang[1]||eang[2]))` →
   `World_OBBTrace(...)`. Scoped strictly to `SOLID_PHYSICS_BOX` so `SOLID_BBOX` (items, players)
   keeps its cheap axis-aligned behaviour.

**Critical correctness note:** the patch **rotates the TRACE, never the hull planes** — the shared
`static box_hull` (`world.c:68`) stays axis-aligned, so the per-entity clip loop is not corrupted.
(Rotating the global plane set would also break its axis-aligned `.type` fast-path and clobber the
next entity's trace.)

**Mod side:** the filing cabinet (`server/sv_physprop.qc`) is `SOLID_PHYSICS_BOX` always now
(velocity-carry works on a box; collision is oriented via this patch) — the TRIMESH/solid-swap layer
was removed. On an **unpatched** engine the cabinet simply degrades to an axis-aligned box (old
behaviour) — no crash.

**Cost:** a few dot products + one matrix invert per `SOLID_PHYSICS_BOX`-with-angles entity per
trace; nil for axis-aligned entities (skipped) and for resting (`angles≈0`) props.

---

## Patch 4 — engine-particle rain/weather splashing on phys props  *(APPLIED — see "As built" below)*

**File (when done):** `engine/client/r_part.c` (`CL_TraceLine`) + `engine/client/p_script.c` (the clip
collision near the `WATER-SURFACE SPLASH` block, ~line 7578).  Search `nettest` once applied.

**Why:** with `cl_weather_engine 1`, engine rain collides via `p_script.c` →
`CL_TraceLine(p->oldorg, p->org, …)` → `World_Move(&csqc_world, …, MOVE_NOMONSTERS, …)` (`r_part.c:891`).
`MOVE_NOMONSTERS` keeps `SOLID_BSP` (so rain splashes on brushwork) but **skips `SOLID_PHYSICS_BOX`/
`SOLID_PHYSICS_TRIMESH`**, so rain falls straight through the filing cabinet / barrels / cans.

**The catch (why it's not a 1-word change):** simply using `MOVE_NORMAL` would also collide rain with
**live other players** — they're `SOLID_BBOX` in `csqc_world` (`client/cl_player.qc:4121`) — and it
would break the carry wall-clamp, which relies on `MOVE_NOMONSTERS` skipping props so a carried prop
can *push* others (`server/sv_physprop.qc` Carry_Tick clip-and-slide).  So the clean version must hit
**phys props only**, not players, and must not change the global `MOVE_NOMONSTERS` semantics (several
QC traces depend on it).

**Planned approach (props-only, scoped):**
- Add a per-particle-type flag `clipprops` to `part_type_t` (mirror the `watercliptype` patch: struct
  field + 2 `P_INVALID`/0 inits + a parser case).
- Add a dedicated trace `CL_TraceLineProps()` in `r_part.c` (copy of `CL_TraceLine`) that traces
  `csqc_world` with `MOVE_NORMAL` **but ignores `SOLID_SLIDEBOX`/`SOLID_BBOX`/`SOLID_CORPSE`** (players/
  corpses) — i.e. keep only world + `SOLID_BSP` + `SOLID_PHYSICS_*`.  Cleanest is a tiny solid-class
  filter inside the trace, or give phys props a dedicated dimension bit and trace that.  NOTE the
  cabinet is a local csqc entity (`World_ClipToAllLinks`) while barrels/cans are packet entities
  (`World_ClipToNetwork`, encoded `ES_SOLID_BSP`) — both are reachable from the `csqc_world` move, so
  one trace covers all three.  The cabinet's oriented box is handled by Patch 3's `World_OBBTrace`.
- In `p_script.c`'s clip collision (~7578), when `type->clipprops` is set, run that trace (in addition
  to / instead of the world `CL_TraceLine`) and splash `cliptype` at the impact.
- Mod side: `weather.cfg` `r_part rain` gets `clipprops 1`.  Unpatched engine: the cvar warns + is
  ignored (rain falls through props as today — no crash).

**As built (APPLIED):** done with a scoped engine move flag (cleaner than the MOVE_NORMAL-minus-filter
sketch above). 5 files:
- `engine/common/world.h`: `MOVE_HITPROPS (1<<9)`.
- `engine/server/world.c`: a `SOLID_ISPHYSPROP()` macro + relaxed the `MOVE_NOMONSTERS` skip at the
  two compiled `World_ClipToLinks` loops (~1841 areagrid, ~2086 areanode) so `SOLID_PHYSICS_*` props
  pass **only** when `MOVE_HITPROPS` is also set. Plain `MOVE_NOMONSTERS` is unchanged (no existing QC
  trace sets the new bit).
- `engine/client/r_part.c` (+ proto in `client.h`): `CL_TraceLineProps()` — one
  `World_Move(&csqc_world, …, MOVE_NOMONSTERS|MOVE_HITPROPS, …)` sweep hits world BSP **and** props in a
  single trace (same trace budget). `World_OBBTrace` (Patch 3) already returns a **world-space** normal,
  so splashes aim correctly off the cabinet's tilted faces.
- `engine/client/p_script.c`: per-type `int clipprops` (field + 2 inits + parser, mirroring
  `watercliptype`); the cliptype collision (~7590) routes through `CL_TraceLineProps` when `clipprops` is set.
- `particles/weather.cfg`: `clipprops 1` on `rain`, `rain_cheapsplash`, and the three `snow*` types.

**Scope:** only the **cabinet** (`SOLID_PHYSICS_BOX`) is hit — correcting the planning note above:
barrels/cans are `SOLID_NOT` on the client (manual CSQC ray-vs-cylinder override in `cl_physprop.qc`,
to protect predicted pmove), so the engine trace doesn't reach them. The `SOLID_ISPHYSPROP` macro
includes `TRIMESH`, so they're covered automatically if ever made solid client-side. Graceful
degradation: on a stock engine the `clipprops` key hits the parser's `Con_DPrintf` fall-through
(developer-only) and is ignored — no crash, rain behaves as today.

---

## Patch 5 — `r_part_splashlimit` (per-frame splash-effect cap, decoupled from collision)  *(APPLIED)*

**Why:** the engine-weather collision couples "the drop dies/stops at a surface" with "a splash effect
spawns" — both happen together in the cliptype block of `engine/client/p_script.c`. So there was no way
to cap splash *spawns* independently of collision, and `r_particle_tracelimit` only bounds the expensive
land `CL_TraceLine` calls — the **water** path uses cheap `PointContents` (Patch 1), not a trace, so it
ignored the trace limit entirely. User wanted (a) a real per-frame **splash** cap shared across land +
water, decoupled from the die, and (b) `cl_rain_splash 0` to STOP rain at BSP/props without splashing.

**As built (2 files + cfg):**
- `engine/client/r_part.c`: new cvar `r_part_splashlimit` (default `0x7fffffff` = unlimited), declared
  next to `r_particle_tracelimit` and registered alongside it.
- `engine/client/p_script.c`: `extern` it; init `int splashes=r_part_splashlimit.ival;` next to
  `int traces` (~6995); wrap the impact-splash spawns with `if (splashes-->0){...}` at the **water**
  ring+splash spawns (~7579-7581) and the **land** cliptype spawns (~7680-7686). Only the
  `P_RunParticleEffectType` spawns are gated — every `p->die=-1` kill and every `continue` stays
  unconditional, so drops still STOP at the surface; only the splash is capped. The single `splashes`
  counter is shared land+water+ring (one decrement per impact). Decals (`clipbounce -2`) and snow's
  splashless death (`clipbounce -1`, no spawn) consume nothing.
- `particles/weather.cfg`: `rain_nosplash` (what `cl_rain_splash 0` selects) gained `clipbounce -1`
  (auto cliptype=self → die on contact via the `clipbounce<0` path, NO splash spawn) + `clipprops 1`
  (die on the rotated cabinet too). Its header comment was updated (it changed from "fall through" to
  "collide+die, no splash").

**Knobs after this:** `r_particle_tracelimit` = land trace COST cap; `r_part_splashlimit` = splash-effect
cap (land+water+ring); `r_bouncysparks 0` = whole-collision off switch. Stock engine ignores the new
cvar (just an unknown cvar) — harmless.

---

## Patch 6 — fix manifest `steam:` gamedir on Windows (mount Steam GoldSrc/HL dirs)  *(APPLIED, upstream bug)*

**Why:** a `.fmf` manifest line like `basegame steam:Half-Life/cstrike` / `steam:Half-Life/valve` (to mount a
Steam Half-Life/Counter-Strike install so GoldSrc maps find their `.wad`s + skyboxes without copying) **silently
did nothing on Windows** — the mounted dir never appeared and map textures stayed missing. This is an upstream FTE
bug, not a config error: the manifest `steam:Subdir/gamedir` resolver (`fs.c` ~5377) verifies the folder by calling
`Sys_SteamHasFile(..., dir, "")` with an **empty filename**, and the Windows `Sys_SteamDirsWithFile` (`fs.c` ~5737)
does that check with `VFSOS_Open(".../cstrike/", "rb")` — i.e. it tries to **open the directory as a file**, which
always fails on Windows (Linux uses `access()`, which works on dirs). It *also* passed the empty `fname` (not the
resolved path) to the result callback, so even a passing probe would have mounted nothing.

**As built (1 file, `engine/common/fs.c`):** in the Windows `Sys_SteamDirsWithFile`, when `fname` is empty, check
the GAMEDIR DIRECTORY's existence with the engine's own `GetFileAttributesU(basepath) != INVALID_FILE_ATTRIBUTES`
(forward-declared just above the function; it's defined further down) and pass the **resolved absolute `basepath`**
to the callback. The non-empty path (the normal Quake/Q2/Q3/Hexen2 Steam auto-detect) is byte-for-byte unchanged —
the new behaviour only fires for the manifest `steam:` empty-filename case. Result: `basegame steam:Half-Life/cstrike`
now resolves via `HKCU\SOFTWARE\Valve\Steam\SteamPath` + `/SteamApps/common/Half-Life/cstrike` and mounts it
read-only (`SPF_COPYPROTECTED`). GoldSrc wad lookup already strips the map's embedded wad path to the basename and
searches all mounted gamedirs (`wad.c` ~780-839), so the wads + `gfx/env` skyboxes are then found automatically.

**Known limitation:** the Windows resolver only checks Steam's MAIN install (registry `SteamPath`); it does not parse
extra Steam *library* folders (`libraryfolders.vdf`) the way the Linux path does. Half-Life in the main library works;
a second-drive library would need a junction or a `libraryfolders.vdf` parse (future). Verify a mount with the `path`
console command (the steam dirs should be listed).

---

## Patch 7 — `r_builtinpalette` (don't let a mounted game's palette/colormap override yours)  *(APPLIED)*

**Why (a direct consequence of Patch 6):** mounting a Steam Half-Life/CS dir for its TEXTURES also drags in its
`gfx/palette.lmp` (HL color palette) and `gfx/colormap.lmp` (HL fullbright/lighting ramp). FTE loads those
**globally** (`client/renderer.c` host_basepal load ~1561 + colormap ~1591), and this mod ships **neither** of its
own (it relies on the engine's built-in `default_quakepal` at renderer.c:1352, with `vid.fullbright` 0). So the
mounted HL palette+colormap WON the search and replaced ours for the whole game → **green/wrong colours on ALL
content (world BSP, models like the snowball, brush entities) and random fullbright pixels** (HL's colormap flips
`vid.fullbright` on). Not a func_illusionary/entity issue — a global palette swap. There's no FSLF flag to skip
copyprotected paths for a file load, so a cvar is the clean fix.

**As built (1 file, `client/renderer.c`):** new archived cvar `r_builtinpalette` (default 0). When set to 1, the
`gfx/palette.lmp` and `gfx/colormap.lmp` `FS_LoadMallocFile` calls are skipped (`r_builtinpalette.ival ? NULL : ...`),
so host_basepal falls back to the built-in `default_quakepal` and `vid.fullbright` stays 0 — exactly the pre-mount
state. HL **textures** are unaffected (WAD3 carries a per-texture embedded palette), so CS maps still render right.
Def + `Cvar_Register` near gl_conback (~795). Default 0 = no change for anyone not setting it.

**Use:** set it EARLY (before the renderer loads the palette) — add `set r_builtinpalette 1` to the `.fmf` manifest
(alongside the `basegame steam:` lines), or `r_builtinpalette 1` in config + `vid_restart`. General rule: whenever you
mount an external game for assets only, set this so its palette/colormap can't override yours.

---

## Patch 8 — `fs_load` (mount external games ON DEMAND at LOW priority)  *(APPLIED)*

**Files:** `engine/common/fs.h` (1 line) + `engine/common/fs.c` (5 sites)  ·  search for `nettest` / `P8`

**Why:** mounting Steam games via `basegame steam:Half-Life/cstrike` lines in the `.fmf` mounts them at the
**HIGHEST** search priority — each basegame is *prepended* to the head of `com_searchpaths` (`fs.c` `FS_AddPathHandle`
~4347), so the search order became `cstrike > valve > nettest` (the mod LAST/lowest). Consequences: the mounted
`valve/gfx/conback.lmp` overrode the mod's console image; FTE's config-write target (`FS_BASEGAMEONLY`) picked the
last/Steam basegame; and the higher-priority foreign `config.cfg` could flip `vid_fullscreen` (window→fullscreen).
They were also always mounted, not on demand.

**What it adds:** three console commands — **`fs_load <game>` / `fs_unload <game>` / `fs_loadlist`** — that mount an
external game **at the LOWEST priority** (appended to the *tail* of `com_searchpaths`, so its assets/maps only fill
gaps and the mod's own files always win — no conback/config/video override). `<game>` accepts **`steam:Game/dir`**
(reuses the Patch-6 `Sys_SteamHasFile` resolver), an **absolute path** (`C:\…` / `C:/…`), or a **relative dir**. The
mounted dir's sub-packages (`.wad`/`.vpk`/`.iwd`/`.pk3`/`.pak`) load too, so GoldSrc, Source (VBSP/.vpk) and CoD1
(.iwd) maps work after a load (CoD2 needs the `.ff` pre-extracted — see [[fte-cod-hl2-map-support]]). The set is
persisted to **`<gamedir>/fs_addons.txt`** (one entry per line) and **auto-remounted on every searchpath rebuild**, so
it survives `fs_restart`/`gamedir` and across launches.

**The 5 edit sites** (all tagged `//nettest` / `P8`):
1. `fs.h` — new flag `#define SPF_ADDON 4096` (append-at-tail, loads sub-packages, no pure/server semantics).
2. `fs.c` `FS_AddPathHandle` (~4319) — add `SPF_ADDON` to the `(SPF_TEMPORARY|SPF_SERVER)` append condition.
3. `fs.c` end of `FS_ReloadPackFilesFlags` (~5637) — `if (reloadflags) FS_RemountAddons(reloadflags);` (single
   rebuild path both startup and `fs_restart` hit). Forward-declared near the `FS_ReloadPackFilesFlags` proto.
4. `fs.c` before `COM_InitFilesystem` — the implementation block: `FS_Addon_Resolve` (steam:/absolute/relative →
   syspath), `FS_Addon_Mount` (dedup + `VFSOS_OpenPath` + `FS_AddPathHandle` with `SPF_ADDON|SPF_COPYPROTECTED|
   SPF_PRIVATE|SPF_ISDIR`), `FS_RemountAddons` (read `fs_addons.txt`, mount each), `FS_Addon_SaveList`
   (add/remove a line), and the `FS_Load_f`/`FS_Unload_f`/`FS_LoadList_f` command handlers.
5. `fs.c` `COM_InitFilesystem` (~8457) — `Cmd_AddCommandD` for `fs_load`, `fs_unload`, `fs_loadlist`.

**Mod side:** `C:\FTEQuake\default.fmf` — the two `basegame steam:…` lines were **removed** (replaced by a comment).
`C:\FTEQuake\nettest\fs_addons.txt` seeds the low-priority mounts (`steam:Half-Life/valve` + `steam:Half-Life/cstrike`)
so the mod still works out-of-the-box. Keep `set r_builtinpalette 1` in the manifest (Patch 7 still applies — the HL
palette is still mounted, just at low priority). With the mod now highest-priority, its `default.cfg` (`vid_fullscreen
0`) wins → **windowed**, and config writes go to `nettest/`.

**Console-image caveat:** the mod ships **no** `gfx/conback`, so the lookup still falls through to the low-priority
`valve/gfx/conback.lmp` (valve's image). To use your own, drop `nettest/gfx/conback.lmp` (or `.tga`/`.png`) — being
higher priority it then wins. (Mounting valve only for assets can't selectively hide its conback; providing your own
is the fix.)

**Verified:** windowed launch (log: `Setting windowed mode`), de_dust2 loads, `path` shows `nettest (e)(w)` above
`$system/valve (c)` + `$system/cstrike (c)` (mod wins), `fs_load C:/…` + `fs_loadlist` + persistence all work. Stock
upstream engine without this patch: `fs_load` is just an unknown command (the `.fmf` would need the old basegame
lines back).

**P8 follow-up (sub-package priority) — REQUIRED for correctness:** `FS_AddPathHandle` strips most flags from the
mask it passes to `FS_AddDataFiles` (fs.c ~4315), so an addon dir's *sub-packages* (`.wad`/`.iwd`/`.vpk`/`.pk3`/`.pak`)
were NOT inheriting `SPF_ADDON` → they mounted at HIGH priority (prepended) even though the dir itself was low.
Symptom: mounting CoD (whose `.iwd` packages carry their own `default.cfg`) made the engine exec CoD's `default.cfg`
instead of nettest's → **fullscreen + plug_load lines skipped**. Fix: add `SPF_ADDON` to the mask at fs.c ~4315 so
sub-packages also append at low priority. Also fixed the `fs_load` command to accept an **unquoted path with spaces**
(`fs_load C:\games\Call of Duty\Main`) via `Cmd_Args()` (new `FS_Addon_Arg` helper). Verified: CoD mounted → windowed
+ all 3 plugins load.

**P8 follow-up 2 (skip foreign configs):** when the mod has no `config.cfg`/`autoexec.cfg` of its own, the startup
config-exec (`engine/client/cl_main.c` ~7824/7826, `exec config.cfg` / `exec autoexec.cfg`) fell through to a mounted
game's (valve's GoldSrc `config.cfg` + `autoexec.cfg`→`violence.cfg`/`controller.cfg`) → dozens of `Unknown command
"cl_*/gl_*"` AND it silently ran valve's binds/cvars over the user's. `SPF_UNTRUSTED` does NOT help (it only lowers the
exec permission level, still finds+execs). Fix: new `qboolean FS_FileIsAddonOnly(const char *name)` in fs.c (FS_FLocateFile
+ check `loc.search->flags & SPF_ADDON`); cl_main.c skips `exec config.cfg`/`exec autoexec.cfg` when their top hit is in
an `SPF_ADDON` dir. `default.cfg` is NOT guarded (the mod ships its own, found first). Verified: foreign-config spam = 0.
(The empty `C:\FTEQuake\valve\` + `\cstrike\` `data\scripts\` folders were leftovers from the basegame era — `fs_load`
uses `FS_AddPathHandle` which never sets `gamedirfile`, so FS_GAMEONLY writes go to nettest; the stubs don't refill.)

---

## Patch 9 — CoD2 (.d3dbsp) BSP material-index bounds guards  *(APPLIED, plugin)*

**File:** `plugins/cod/codbsp.c` (the **cod plugin**, not the engine — rebuild with `make plugins-rel
NATIVE_PLUGINS=cod CC=gcc`, deploy `engine/release/fteplug_cod_x64.dll` → `C:\FTEQuake\fteplug_cod_x64.dll`).

**Why:** loading some CoD2 maps (e.g. mp_harbour) crashes once the CoD assets are mounted; with the game unmounted the
geometry loads fine. Cause: the CoD2 BSP loader reads a brush/patch **material index** straight out of the BSP and
indexes `prv->surfaces[mat]` / `mod->textures[in->mat]` with **no bounds check** (the code even comments *"is this
right? … feels wrong though"*). A bad/out-of-range index → out-of-bounds read → crash.

**Fix (4 sites, tagged `//nettest`):** clamp `mat` to `< mod->numtexinfo` (the surface/shader count = the size of
`prv->surfaces[]`), falling back to index 0, at `CODBSP_LoadBrushes` (~1195, ~1206/1225), `CODBSP_BuildBIH` patch-tri
(~1547), and made the unexpected-patch-mode `Con_Printf` (~1317) stop dereferencing `textures[in->mat]`. A bad index
now uses a default material instead of crashing. (CoD2 `.iwi` pixelformat 6 is separately unsupported but fails
*gracefully* in codiwi.c — not the crash.) NOTE: rebuilding the cod plugin needs `CC=gcc` (the plugin Makefile
defaults to `cc`, absent in ucrt64) and prints a harmless `Error 127` from the optional post-link `zip`/EMBEDMETA step
— the `.dll` is already built before that.

---

## Patch 10 — Source/HL2 skybox loading (find + decode `.vtf` faces)  *(APPLIED)*

**Files:** `engine/gl/gl_shader.c` (`Shader_ParseSkySides` patterns) + `engine/client/image.c` (`r_defaultimageextensions`)  ·  search `nettest`

**Why:** Source/HL2 (VBSP) maps rendered a BLACK skybox. The worldspawn `skyname` (e.g. `sky_day01_04_hdr`)
builds a sky shader `skybox_<name>`, but two things were missing: (1) the engine sky-side loader never tried the
Source layout `materials/skybox/<name><side>.vtf` (only `<name>_<side>`, `<name><side>`, `env/…`, `gfx/env/…`);
and (2) even with the right path, `R_LoadHiResTexture` never probed the `.vtf` extension — `r_defaultimageextensions`
listed only dds/ktx/tga/png/jpg/pcx, so the face file was never found/decoded. (The VTF decoder itself already lives
in the hl2 plugin `img_vtf.c` and runs the moment a `.vtf` is read into memory by name.) Net: every face missed →
`r_blackimage`; and because all 6 faces × 6 patterns × 2 suffix-sets always missed, changing `r_skybox` did a full
Source-`.vpk` search per combo = very slow (felt like a level reload).

**As built (2 sites, tagged `nettest`):**
- `gl_shader.c` `Shader_ParseSkySides` (~668) — added `"skybox/%s%s"` and `"materials/skybox/%s%s"` to
  `skyname_pattern[]`. Source side suffixes (`rt/bk/lf/ft/up/dn`) already matched the existing suffix set; only the
  directory prefix was missing.
- `image.c` `r_defaultimageextensions` (~99) — appended `" vtf"` **last** (native tga/png/dds keep priority; Source
  assets resolve to `.vtf` only when nothing else exists). `R_ImageExtensions_Callback` re-parses on registration.

**Result:** `materials/skybox/<name>rt.vtf` is now found, decoded by the plugin VTF loader, and the per-face loop
**breaks on first hit** → correct sky AND fast `r_skybox` changes. Set `r_skybox <skyname>`. Stock engine (no `.vtf`
extension): Source skyboxes stay black, no crash.

**Known follow-up:** Source "compressed HDR" faces store R8G8B8E8 (shared-exponent); `mat_vmt.c:133` doesn't
decompress that, so such a face decodes DARK (not black). If a specific `_hdr` sky looks dark, remap its skyname to
the LDR base (`sky_day01_04`) or extend `img_vtf.c` to transcode R8G8B8E8. True-float (RGBA16F) HDR VTF decodes fine.

---

## Patch 11 — quiet CoD Q3-style shader-directive spam  *(APPLIED)*

**File:** `engine/gl/gl_shader.c` (`Shader_Parsetok` + new `Shader_IsKnownIgnoredDirective`)  ·  search `nettest`

**Why:** CoD/CoD2 `.stype` materials (routed through FTE's shader parser by `plugins/cod/codmat.c`) use Q3-style
directives FTE doesn't implement (`nvTexShader`, `waterMap`, `perlight`, `sunfile`, `tessSize`, `radialNormals`).
Each hits `Con_DPrintf("Unknown shader directive …")` — already developer-gated, but with `developer 1` on (needed to
read real errors) hundreds per map flood the console. Demoting can't help (already DPrintf; user runs developer).

**As built (1 site):** a `Shader_IsKnownIgnoredDirective()` helper (the 6 known CoD keywords) above `Shader_Parsetok`;
the unknown-directive guard gains `&& !Shader_IsKnownIgnoredDirective(prefix?prefix:token)`. The line is still consumed
by the existing "Next Line" loop, so parsing is unchanged; **any other** unknown token (typos, broken Q3 shaders) still
warns. Extend the `ignored[]` array for more benign CoD keywords (but NOT real FTE keys like `nomipmaps`).

---

## Patch 12 — `Cbuf_GetNext` is `//`-comment-aware  *(APPLIED)*

**File:** `engine/common/cmd.c` (`Cbuf_GetNext`)  ·  search `nettest`

**Why:** `particles/weather.cfg` (and any r_part/menu script with inline `//` comments) printed spurious
`"<word> is not a recognised particle type field"` warnings (`the`, `true`, `dev-warn`). Cause: `Cbuf_GetNext` split
command lines on an unquoted `;` but was NOT `//`-comment-aware (unlike its sibling `Cbuf_ExecuteLevel`). A `;` *inside*
a `//` comment (e.g. `…floor); the…`, `…patch; dev-warn`) split the line, and the post-`;` fragment lost its `//`
prefix so its first word was parsed as a particle field.

**As built (1 site):** added a `comment` flag to the `Cbuf_GetNext` scan loop (mirrors `Cbuf_ExecuteLevel`): once `//`
is seen on a physical line, a subsequent `;` no longer splits it. `\n` is checked first so an unterminated comment
still ends at the newline; quote handling unchanged; non-comment lines tokenize identically. `common/` file → affects
client + dedicated server. The cfg needs no edits.

---

## Patch 13 (plugin) — CoD static-prop spawning · foliage non-solid · `modelscale` · xmodel robustness  *(APPLIED, cod plugin)*

**Files:** `plugins/cod/codbsp.c` + `plugins/cod/codmod.c` (cod plugin — rebuild `make plugins-rel NATIVE_PLUGINS=cod
CC=gcc`, deploy `engine/release/fteplug_cod_x64.dll` → `C:\FTEQuake\fteplug_cod_x64.dll`)  ·  search `nettest`

**What it adds (all tagged `nettest`):**
- **Placed-prop spawning** (`codbsp.c` `COD_LoadProps` + `CODBSP_PrepareFrame`): CoD has no static-prop lump — props
  are entity-lump `script_model`/`misc_model` entities with `model "xmodel/<name>"`. `COD_LoadProps` parses them
  (2-pass count+fill); `CODBSP_PrepareFrame` renders each via `NewSceneEntity` (threaded `GetModel`). Any other
  xmodel-bearing class is logged once under `developer 1`.
- **`modelscale`** (`COD_LoadProps`): parse the entity `modelscale` key → `ent->scale` (default 1). Big rocks authored
  at `modelscale >1` were rendering at base size (≈¼) because the key was ignored.
- **Foliage non-solid** (`codbsp.c` `CODBSP_LoadShaders` ~897): CoD foliage materials carry `SOLID` in the BSP
  material contents → player collides with every leaf. After reading `surfaces[mat].c.value`, clear
  `SOLID|WINDOW|PLAYERCLIP|MONSTERCLIP|BODY` when the material name contains **`foliage`** (all CoD foliage is named
  `foliage_masked@…`/`foliage_detail@…`, incl. grass-blades & bushes). **One** edit covers both CoD1 (mode0/mode1
  patch) and CoD2 (brush) collision — both derive contents from this single source. Rendering is independent of
  `c.value`, so foliage still draws. NOTE: an earlier version also matched bare `grass`/`bush`, which wrongly hit a
  WALKABLE grass-ground displacement on CoD1 dam and dropped the player through the terrain — match `foliage` ONLY.
- **xmodel loader robustness** (`codmod.c`): (a) the CoD1 strip-decoder no longer aborts the whole model on a
  `ntris != t` mismatch — it renders what decoded (degenerate-tri stripping legitimately yields t<ntris on big
  multi-strip rocks); (b) bound the per-LOD `tex[64]` write so material-rich models can't smash the stack; (c) name
  missing `xmodelparts/`/`xmodelsurfs/` sub-files under `developer 1` (was a silent skip).

**Graceful degradation:** all of the above are additive — a stock cod plugin just renders no props and keeps foliage
solid; no crash. NOTE the same `CC=gcc` + harmless `zip` `Error 127` caveat as Patch 9.

---

## Patch 14 — quiet Source `.vmt` console spam (5 categories) + collapse interior `//` in paths  *(APPLIED)*

**Files:** `plugins/hl2/mat_vmt.c` (A/B/C) + `engine/gl/gl_shader.c` (D) + `engine/common/common.c` (E)  ·  search `nettest`

**Why:** loading a Source/HL2 (VBSP) map flooded the `developer 1` console with hundreds of `.vmt` material lines,
burying real errors. The plugin's `Con_DPrintf` is a BINARY `developer!=0` gate (no level arg, and the plugin SDK
exposes only `Con_Printf`/`Con_DPrintf`), so "demote to developer 2" is impossible plugin-side — the fix is
recognise-and-skip (same pattern as Patch 11), except category E which is a real path bug we FIX.

**As built:**
- **A — per-field echo** (`mat_vmt.c` `VMT_ParseBlock`): ~41 recognise-but-ignore branches were pure
  `Con_DPrintf("%s: %s \"%s\"\n", …)` traces (`$nodecal`, `$detail`, `$AllowAlphaToCoverage`, …). Replaced the echo
  with `;` (keeps any store on the preceding line). These keys are deliberately handled, so nothing is hidden.
- **B — Unknown field** (`mat_vmt.c` final `else`): added `VMT_IsKnownIgnoredField()` (`$nolod`/`$model`/
  `$FlashlightNoLambert`) + an `else if (…) ;` before the warning. A genuinely novel/malformed key still warns.
- **C — Unknown block** (`mat_vmt.c` sub-block `else`): added `VMT_IsKnownIgnoredBlock()` (prefix match
  `vertexlitgeneric`/`lightmappedgeneric`/`unlitgeneric`/`worldvertextransition`/`proxies`) for Source's per-DX/HDR
  shader-variant sub-blocks + Proxies. Any other block name still warns.
- **D — leaked shader directives** (`engine/gl/gl_shader.c` `Shader_IsKnownIgnoredDirective`, the Patch-11 helper):
  the VMT→FTE-shader generator emits Q3 pass-only keywords (`rgbgen`/`alphagen`/`alphatest`) and blendfunc args
  (`src_alpha`/`one_minus_src_alpha`/`one`/`zero`/…) at the shader TOP LEVEL, where they fall through to "Unknown
  shader directive". Added those tokens to the ignore list. Collision-free: they aren't in the top-level
  `shaderkeys[]`, and inside a real `{ }` pass block `rgbgen`/etc. match `shaderpasskeys[]` FIRST.
- **E — `//` path bug** (`engine/common/common.c` `COM_CleanUpPath`): a stray interior `//` in a Source `.vmt`
  material ref (e.g. `props_wasteland//bridge_railing`) was never collapsed (the function strips leading `/` and
  resolves `..` but not interior `//`), producing `Error: empty directory name (…//…vmt_glsl.vmt)` (a `Con_Printf`,
  always shown) + a wasted lookup. Added an interior-`//`→`/` collapse pass after the leading-slash strip. URL `://`
  never reaches here (handled earlier). This is a FIX, not suppression.

**Net effect:** `developer 1` stays useful — a malformed `.vmt`, a real novel unknown field/block, or a genuine
unknown shader directive STILL warns; only the benign Source-asset flood is silenced. Two rebuilds: hl2 plugin
(A/B/C; `make plugins-rel NATIVE_PLUGINS=hl2 CC=gcc` → `fteplug_hl2_x64.dll`) + engine (D/E; `make m-rel`).

---

## Patch 15 — round-2 Source/CoD shader-spam quiet (GLSL permutations, conditional-operator leak, imagesize, skin) + extended VMT ignore-lists  *(APPLIED)*

**Files:** `engine/gl/gl_shader.c` (2 sites) + `engine/gl/gl_alias.c` (1) + `plugins/hl2/mat_vmt.c` (extends Patch 14's two allow-lists)  ·  search `nettest`

**Why:** after Patch 14, Source/HL2 + CoD maps still flooded `developer 1` with a second, larger wave of benign warnings — all confirmed cosmetic (rendering is correct):
- **GLSL `Unknown pemutation in glsl program vmt/vertexlit#…`** (the dominant one — hundreds of lines): the hl2 plugin's
  `vmt/vertexlit`/`vmt/lightmapped` GLSL programs declare `!!permu NOFOG` and `!!permu AMBIENTCUBE`, which aren't in the
  engine's 8-entry bitmask permutation table, so `Shader_LoadPermutations` (gl_shader.c:1998) warns once per *distinct*
  program name — and mat_vmt builds a unique name per material (env-tint/sat/mask combos), so the same 1–2 tokens re-fire
  as hundreds of differently-named lines. The features (envmap/tint/mask/nofog) come from `#define` injection, NOT the
  permu table, so the failed registration changes nothing. FIX: recognise + skip `NOFOG`/`AMBIENTCUBE` in the
  force-defined whitelist (mirrors `TESS`/`SPECULAR`/…), and demote the surviving generic print to `Con_DLPrintf(2,…)` so
  any *other* unknown permu only whispers at developer 2.
- **CoD `||`/`<` leak** (CoD1 `stalingrad` water, a burst at load): NOT codmat — CoD1 world textures resolve to CoD's own
  Q3-style text `.shader` files, and an `if(...)` conditional that continues an operator onto the next line leaves the
  leading `||`/`<` orphaned (the evaluator stops at the newline), read as a bogus top-level directive. FIX: added
  `||`/`&&`/`<`/`<=`/`>`/`>=`/`==`/`!=` to `Shader_IsKnownIgnoredDirective` (an operator is never a valid directive). (Did
  NOT add bare `s` — too generic.)
- **`console: "imagesize"`**: codmat.c emits a no-op `imagesize` metadata line into every CoD material; added to the ignore list.
- **`Skin number out of range (car003a.mdl)`**: a 1-skin Source `.mdl` asked for skin index 1; falls through to a valid
  skin (cosmetic). Demoted the throttled print (gl_alias.c) from developer 1 → 2.

**Plus** (extends Patch 14's hl2 `mat_vmt.c` allow-lists — applied in-source): `VMT_IsKnownIgnoredField` +=
`$parallaxmap`/`$parallaxmapscale`/`$cheapwaterstartdistance`/`$cheapwaterenddistance`; `VMT_IsKnownIgnoredBlock` +=
`animatedtexture`/`texturescroll`/`waterlod`/`water_` (prefix, covers `Water_DXxx`) + a leading-char test that skips any
block starting with `>`/`<`/`=` (the Source DX-version conditional blocks `>=dx90`/`<DX90`).

**Net:** `developer 1` on a Source/CoD map is now down to genuinely-useful lines (real missing-content `Couldn't load
sound`, the mod's own `SERVERINFO`). Real shader/material typos, novel unknown fields/blocks, and genuinely-unknown
permutations (now at developer 2) still surface. Two rebuilds: engine (gl_shader.c + gl_alias.c) + hl2 plugin.

---

## Patch 16 — guard the sprite renderer against a non-sprite / failed model (Source `.vmt` sprite crash)  *(APPLIED)*

**Files:** `engine/client/renderer.c` (R_GetSpriteFrame) + `engine/gl/gl_alias.c` (R_Sprite_GenerateTrisoup) +
`engine/gl/gl_model.c` (warning names the file)  ·  search `nettest`
**+ Mod QC (git-tracked, not an engine patch):** `server/sv_env_sprite.qc`, `server/sv_cycler_sprite.qc` (reject `.vmt`)

**Why:** loading a Source/HL2 map containing `env_sprite`/`cycler_sprite` entities (e.g. **d1_canals_01**) CRASHED on
load. Those entities' `model` key is a Source SPRITE MATERIAL (`.vmt`, content begins `"Sprite"`/`"SpriteCard"`). The
mod QC's `precache_model(self.model)` + `setmodel` feeds the `.vmt` to the engine MODEL loader; no loader matches a
`.vmt` so `gl_model.c` falls through to `mod_dummy`/MLS_FAILED (the `Unrecognised model format "Spr…` warning). One
frame later the sprite renderer derefs it: `R_GetSpriteFrame` (renderer.c:2538) did `psprite = model->meshinfo; …
psprite->numframes` with NO type/NULL check, and a dummy has `meshinfo == NULL` → segfault. Every OTHER model path
guards `mod_dummy`/MLS_FAILED; the sprite path was the one gap. No `.dmp` exists because FTE's minidump writer is
`#ifdef _MSC_VER` and this is a **mingw** build — the cause came from the flushed `-condebug` `qconsole.log` (the last
line before the crash was the model warning; see the crash-logging note below).

**As built (engine — the load-bearing guard):**
- `renderer.c` R_GetSpriteFrame: `if (currententity->model->type != mod_sprite || !psprite) return NULL;` before the deref.
- `gl_alias.c` R_Sprite_GenerateTrisoup (the sole LIVE caller; the other at :1964 is inside `#if 0`): added `if (!frame)
  return;` after the call (was `shader = frame->shader;`, which would itself NULL-deref on the new NULL return).
- `gl_model.c:1340`: the "Unrecognised model format %c%c%c%c" warning now also prints `mod->name` (was anonymous) — a
  permanent diagnostic win for any future unrecognised model.

**As built (mod QC — the root cause):** `env_sprite`/`cycler_sprite` now reject a `model` ending in `.vmt` (`dprint`
+ `remove(self)`), so the bad precache never happens (the Source glow is simply absent instead of a dummy). `.spr`/`.mdl`
sprites still work. Recompile: `fteqcc64_latest.exe progs.src` from `nettest/src` → `qwprogs.dat`.

**Result:** d1_canals_01 (and any Source map with sprite entities) loads instead of crashing. Either layer alone stops
the crash; both together also remove the warning + the useless dummy entity. Rebuilds: engine (`make m-rel`) + QC progs.

### Capturing an FTE crash in this build (mingw, no minidump)
`fteqw64.exe`'s `MiniDumpWriteDump` is `#ifdef _MSC_VER` → **no `.dmp` from the mingw build**. Instead use the flushed
console log: launch with **`-condebug +set developer 1`** (the `C:\FTEQuake\fte_debug.bat` launcher does this). The log
is written **per-line, open/append/close** (`log.c:210`, `"ab"`) to **`C:\FTEQuake\nettest\qconsole.log`** (`log_root =
FS_GAMEONLY` = the mod gamedir), so the tail survives a crash — the **last line before the crash usually names the
asset/lump** that killed it. (On a hard segfault FTE *also* pops a "KABOOM!" dialog with a DBGHELP stack-walk and copies
it to the clipboard — but the stripped exe + mingw/DWARF means it's mostly raw addresses; the `qconsole.log` tail +
`developer 1` is the practical tool.)

---

## Patch 17 — void green-tint, HL2 HDR-sky banding, and the d1_canals_01 crash guards  *(APPLIED)*

**Files:** `engine/client/view.c` (void tint) + `plugins/hl2/mat_vmt.c` (sky LDR) + `plugins/hl2/mod_vbsp.c` (2 crash
guards + 3 temporary breadcrumbs)  ·  search `nettest`

- **Void green tint** (`view.c:733` `V_SetContentsColor`): noclipping into the VOID resolved the eye contents to
  `FTECONTENTS_SOLID`, which was grouped with the SLIME case → the dark-green slime cshift (`0 25 5 150`) on every map.
  FIX: `else if (contents & FTECONTENTS_SLIME)` (was `& (FTECONTENTS_SLIME | FTECONTENTS_SOLID)`) — pure SOLID/void now
  falls through to `v_cshift_empty` (alpha 0 = no tint). Real slime/water/lava tints (each its own bit) unchanged.
- **HL2 HDR skybox banding** (`mat_vmt.c` ~163): HL2 campaign sky `.vmt`s ship a compressed-HDR face
  (`$hdrbasetexture`/`$hdrcompressedtexture`, R8G8B8E8-packed-in-BGRA8) that `img_vtf` reads as plain BGRA8 — the shared
  exponent is dropped → banded/dark. CS:S skies ship only an LDR `$basetexture` and look fine. FIX: split the combined
  key test so `$basetexture` (LDR) is authoritative and the two HDR variants are skipped. (Fallback if a map ships
  HDR-only sky: decode R8G8B8E8→E5BGR9 in `img_vtf.c` — not done, easy to get the exponent bias/channel order wrong.)
- **d1_canals_01 crash** (loads, then SIGSEGVs ~500ms in with audio, no log/dump — diagnosed via [[fte-crash-logging]]):
  two safe speculative guards in `mod_vbsp.c` `VBSP_PrepareFrame`/`VBSP_LightPointValues`:
  - **H1 (primary):** the displacement render loop appended `surf->sbatch->mesh[surf->sbatch->meshes++]` with NO
    `visframe` dedup, unlike the leaf walk — a displacement face present in both a leaf's marksurfaces AND the dispinfo
    list is emitted twice, overrunning the once-per-surface-sized batch mesh array → heap corruption → a *delayed*
    crash (fits the ~500ms). d1_canals beds are displacement-heavy; CS:S isn't. FIX: `if (surf->visframe ==
    vbsp_surfsequence) continue; surf->visframe = vbsp_surfsequence;` before the emit (mirrors the leaf walk; the
    sequence is bumped per-frame so it can only prevent the overrun, never wrongly cull).
  - **H2 (secondary):** `VBSP_LightPointValues` read `leafs[leafnum]`/`leaflight[leafnum].count` before bounding
    `leafnum` (a prop near the void can resolve out-of-range) → OOB read. FIX: clamp `leafnum` to `[0,numleafs)`.
  - **Breadcrumbs (TEMPORARY, `Con_DPrintf` = developer-1 only):** `[bc] VBSP_PrepareFrame: displacements/staticprops/done`
    bracket the two phases so if it still crashes, the `qconsole.log` tail (via `fte_debug.bat`) shows which phase died.
    **Remove these once the crash is confirmed fixed.**

Rebuilds: engine (`make m-rel`) + hl2 plugin (`make plugins-rel NATIVE_PLUGINS=hl2 CC=gcc`).

---

## Patch 18 — static-prop PVS culling + GL-backend NULL guards (d1_canals crash, 2nd cause)  *(APPLIED)*

**Files:** `plugins/hl2/mod_vbsp.c` (prop cull) + `engine/gl/gl_backend.c` (2 guards + temp breadcrumbs) +
`engine/client/r_surf.c` (temp breadcrumbs)  ·  search `nettest`

- **Static props never culled** (`mod_vbsp.c` `VBSP_PrepareFrame` static-prop loop ~3604): the loop only rejected by
  fade distance + model loadstate, then `NewSceneEntity()` unconditionally — so props drew through area portals (visible
  with `r_showtris 1`, world geo culled but props not). FIX: before the lighting/emit, (1) populate `src->pvscache`
  first (moved up), (2) `if (surfvis && !VBSP_EdictInFatPVS(mod, &src->pvscache, surfvis, areas)) continue;` — the same
  PVS/area cull the displacement loop already uses (pvscache.leafnums hold clusters, surfvis is the cluster-PVS →
  index-consistent), and (3) a cheap `VBSP_CullBox` frustum cull on the radius box. `surfvis==NULL`
  (portal-recursion/novis) still draws everything. See [[vbsp-staticprop-pvs-cull]].
- **d1_canals_01 crash — 2nd cause** (the env_sprite fix (P16) + the displacement/leaf guards (P17) got it loading, then
  it SIGSEGV'd in the first frame's render *backend* — breadcrumbs confirmed `VBSP_PrepareFrame` completes). Root cause:
  `gl_backend.c:1280` `case T_GEN_FULLBRIGHT: t = shaderstate.curtexnums->fullbright;` was the **only** `T_GEN_*` texture
  case that dereferenced `curtexnums` with NO null/loaded guard (every neighbor uses `curtexnums && TEXLOADED(...)`). On
  a Source map a prop/material whose texnums aren't resolved on frame 1 has `curtexnums == NULL` → fault. FIX: guard it
  like the neighbors (`… ? …->fullbright : r_nulltex`). Plus a defensive `if (!sh) return;` in `GLBE_SubmitBatch`
  (gl_backend.c:5190) for the portal/depthmask loops that call it directly past the sortlist `!bs` guard.
- **d1_canals_01 crash — ACTUAL 2nd cause** (the FULLBRIGHT/SubmitBatch guards above are good hardening but were NOT it —
  the breadcrumbs showed the crash hit BEFORE the backend, in the gap right after `VBSP_PrepareFrame: done`): the only
  call there is `CL_LinkStaticEntities` (`engine/client/cl_ents.c:3676`), whose guard was `if (!clmodel ||
  clmodel->loadstate == MLS_LOADING) continue;` — it skipped only `MLS_LOADING`, so a static entity with a **FAILED/dummy
  model** (a Source model that didn't load) passed through and crashed in the bounds/PVS/axis setup below. FIX: `!=
  MLS_LOADED` (skip anything not fully loaded, matching every other model-use site).
- **Breadcrumbs:** the temporary `[bc]` `Con_DPrintf`s (this patch's `r_surf.c`/`gl_backend.c` + P17's `mod_vbsp.c`
  `VBSP_PrepareFrame` ones) were **all removed** once the crash was localized — search confirms zero `[bc]` remain.

Rebuilds: engine (`make m-rel`) + hl2 plugin (`make plugins-rel NATIVE_PLUGINS=hl2 CC=gcc`).

---

## Patch 19 — d1_canals crash (3rd cause: NULL curtexnums) + Source model brightness + VMT proxy/field spam  *(APPLIED)*

**Files:** `engine/gl/gl_backend.c` (crash) + `plugins/hl2/mod_vbsp.c` (brightness) + `plugins/hl2/mat_vmt.c` (spam)  ·  search `nettest`

- **d1_canals_01 crash — 3rd cause** (the `CL_LinkStaticEntities` P18 fix held; the crash moved into the prop GLSL
  draw): a `combine_fenceglow` static prop (VertexLitGeneric → `program "vmt/vertexlit"` → GLSL path) whose material is
  registered-but-not-yet-generated has `defaulttextures == NULL`, so `GLBE_DrawMesh_List`/`GLBE_SubmitBatch` set
  `shaderstate.curtexnums = NULL` (the `else` branches at gl_backend.c:5133/5158/5203), and `BE_RenderMeshProgram`
  (gl_backend.c:3940) dereferences it with no guard (the GLSL sibling of the P18 `T_GEN_FULLBRIGHT` bug). FIX: a
  file-scope `static texnums_t r_nulltexnums;` (zeroed) and `… = X->defaulttextures ? X->defaulttextures : &r_nulltexnums;`
  at all 3 sites — `TEXLOADED()` on zeroed texids is false, so the prop draws untextured for a frame instead of faulting.
- **Source model brightness "all over the place"** (`mod_vbsp.c` `VBSP_LightPointValues`): the per-leaf ambient-cube is
  decoded correctly (`ColorRGBExp32`, exponent applied) but for HDR leaf-ambient it's *unbounded linear*, and it was
  pushed through `M_LinearToSRGB(x, mag=1)*256` with no exposure → bright leaves blew past 255 (white), dark stayed tiny
  → 20× spread straddling the clamp. FIX: cvar defaults `hl2_lt_srgb_mag 1→0` (disable the sRGB curve) and `hl2_lt_scale
  256→160` (decoded-linear × 160 stays in 0..255); both runtime-tunable. Also fixed a directional bug at
  mod_vbsp.c:4041/4044 (`res_dir[0]` used for all 3 axes → leaned every prop's shading toward +X; now `res_dir[j]`).
  (If bright leaves still clamp, the follow-up is a Reinhard cap before the scale; not applied yet.)
- **VMT proxy/field spam**: (1) the `else` that warns "Unknown block" became `else if (st)` — only the material TOP level
  warns; nested sub-blocks recurse with `st==NULL`, so every proxy entry inside `Proxies`
  (GaussianNoise/PlayerProximity/Add/Subtract/Sine/Clamp/…) is now silent. (2) extended `VMT_IsKnownIgnoredField` with the
  common benign top-level fields ($multipass/$fresnelreflection/$modelmaterial/$texture2/$no_fullbright) + the
  COMBINESHIELD proxy result-vars ($gnoise/$playerdistance*/$alpharesult*/$smallamount/$largeamount/$hundred/$ten/…).

**Still pending:** the proper **R8G8B8E8 HDR sky decode** (HL2 sky residual banding) — a dedicated 4-file change
(engine `render.h` `IF_HDRDECOMPRESS` flag + `gl_shader.c` `$hdr:` name-prefix; plugin `mat_vmt.c` signal + `img_vtf.c`
R8G8B8E8→RGBA32F decode). Tracked, not yet built.

Rebuilds: engine (`make m-rel`) + hl2 plugin (`make plugins-rel NATIVE_PLUGINS=hl2 CC=gcc`).

---

## Patch 20 — d1_canals SPAWN-IN crash: water-reflection NULL forcedvis + unguarded BIH NativeTrace  *(APPLIED)*

**Files:** `engine/gl/gl_rmain.c` + `engine/common/com_bih.c` + `engine/server/world.c` + `plugins/hl2/mod_vbsp.c`  ·  search `nettest`

After P16–P19 the map LOADS and renders the team-select overview for seconds, then SIGSEGVs the instant the player
SPAWNS IN to first-person. developer-2 QC trace ends at `[spawn] picked info_player_start at 892 2636 74.9 — CS-FALLBACK`.
The spawn point and the team-select camera are **different locations**, so the crash is in a subsystem only exercised by
rendering/colliding **from the spawn point**. Two candidate paths; all four guards are pure NULL/bounds checks (applied
together, none can change correct behavior):

- **Water reflection recursion** (top suspect — canal water enters frame at the spawn point but not at the overview
  camera): `GLR_DrawPortal`'s refraction loop (gl_rmain.c:1220-1239) sets `r_refdef.forcevis=true` as soon as a mesh has
  `xyz_array`, but `forcedvis` is only assigned by `ClusterPVS` (can stay NULL — empty mesh / div-by-zero centroid at
  :1229 when `numvertexes==0` / ClusterPVS miss). `VBSP_MarkLeaves` (mod_vbsp.c:3463-3495) then did `vis =
  refdef->forcedvis` and dereferenced it (`vis[cluster>>3]`) with no NULL check → SIGSEGV. FIX: (a) mod_vbsp.c:3466 —
  `if (!vis) return NULL;` (falls into the existing whole-model/all-surfaces fallback); (b) gl_rmain.c:1223 — skip
  degenerate meshes (`!mesh->numvertexes`) so the centroid can't div-by-zero and leave `forcedvis` NULL.
- **BIH collision trace** (the other candidate — the first predicted/real PMOVE box-traces the *separate* BIH collision
  tree `mod->cnodes`, never touched by the render PVS path): two `funcs.NativeTrace` calls were unguarded vs their
  siblings. FIX: (c) `com_bih.c` BIH_MODEL *Test* case (~1196) — `if (!model || loadstate!=MLS_LOADED || !NativeTrace)
  return;` (a flushed/non-collidable static prop; the sibling `BIH_RecursiveTrace` already guards loadstate); (d)
  `world.c:915` server worldmodel trace — add `&& model->funcs.NativeTrace` (the CSQC/hitmodel paths already null-check
  it; the box-hull `else` at world.c:941 is the safe fallback).

No breadcrumbs added (the guards cover both subsystems). If it STILL crashes, the next step is dev-2 `[bc]` breadcrumbs
at `GLR_DrawPortal`/`VBSP_MarkLeaves` (render) vs `World_TransformedTrace`/`BIH_Trace` (collision) to pick the path.

Rebuilds: engine (`make m-rel`) + hl2 plugin (`make plugins-rel NATIVE_PLUGINS=hl2 CC=gcc`).

---

## Patch 21 — compressed-HDR (RGBS) sky decode: kill the residual HL2 sky banding  *(APPLIED)*

**Files:** `engine/client/render.h` + `engine/gl/gl_shader.c` + `plugins/hl2/mat_vmt.c` + `plugins/hl2/img_vtf.c`  ·  search `nettest`

Supersedes the P17 stop-gap (skip the HDR face, show the LDR `$basetexture`) and closes P19's "Still pending"
R8G8B8E8 item. HL2 campaign sky `.vmt`s ship a compressed-HDR face the 8-bit LDR `$basetexture` can't match → the
sky gradient bands. The real Source format (verified against the SDK shader — NOT the power-of-2 "shared exponent"
the Valve wiki implies; see [[source-hdr-compressed-sky-decode]]) is **RGBS**: a plain `BGRA8888` VTF where
**alpha is a per-pixel LINEAR scale**, decoded `linear.rgb = srgb_to_linear(rgb/255) * (alpha/255) * 8`
(`sky_hdr_compressed_rgbs_ps2x.fxc`: `result.rgb = rgb*alpha; result *= InputScale`, with `InputScale = 8*$color`
and an sRGB read on rgb). Decoding to float kills banding — the alpha scale carries sub-8-bit gradient precision.

Threaded through the existing `$`-prefix texture-name mechanism (same plumbing as `$cube:`/`$rt:`/`$linear:`):

- **`render.h`** (~466): renamed the free `IF_UNUSED15` → `IF_HDRDECOMPRESS = 1<<15`.
- **`gl_shader.c`** `Shader_SetImageFlags` (~835, before the `else break`): a `$hdr:` name-prefix handler that
  strips `$hdr:` and sets `IF_HDRDECOMPRESS`, so the flag rides the texture name down to the image loader.
- **`mat_vmt.c`**: added `hdrcompressedtex`/`hdrbasetex` to `vmtstate_t`; `$hdrbasetexture` / `$hdrcompressedtexture`
  now STORE their value (were no-op skips). The `UnlitGeneric` sky emit (~420) prefers an HDR face over the LDR
  `$basetexture`: `$hdrbasetexture` (uncompressed `RGBA16161616F`, used as-is — already `PTI_RGBA16F` in img_vtf)
  when present, else `$hdrcompressedtexture` emitted as `map "$hdr:materials/<name>.vtf"` to trigger the decode.
  LDR `$basetexture` stays the fallback when no HDR face exists (CS:S skies → unchanged).
- **`img_vtf.c`** `Image_ReadVTFFile` (post-pass before `return mips`): when `(flags & IF_HDRDECOMPRESS) &&
  vmffmt == VMF_BGRA8`, allocate a per-mip `RGBA32F` buffer (`plugfuncs->Malloc`, `needfree=true`), decode every
  pixel `B,G,R,A → {srgblin[R]*a, srgblin[G]*a, srgblin[B]*a, 1}` with `a = A/255*8` (256-entry srgb→linear LUT
  built once per call; iterates `w*h*depth` so all cube faces are covered), and set `mips->encoding = PTI_RGBA32F`.
  The engine transcodes RGBA32F → RGBA16F/byte on upload (the plugin has no FloatToHalf, so it can't emit RGBA16F
  directly). The original BGRA8 bytes are freed via the existing `mips->extrafree`.

Caveats: `$color` tint ignored (defaults to white); the multi-exposure `sky_hdr_compressed_ps2x` variant (3 textures
@ 0.25/2/16) is NOT handled — only the single-VTF RGBS path. If the sky reads too dark/bright, the knob is the `*8`
InputScale or the sRGB linearization in `img_vtf.c`.

Rebuilds: engine (`make m-rel`) + hl2 plugin (`make plugins-rel NATIVE_PLUGINS=hl2 CC=gcc`). **Build note:** when
building from a sandboxed shell, export a writable `TMP`/`TEMP` first — a `C:\WINDOWS` TMP makes gcc fail with
"Cannot create temporary file" / Error 127 (the compiler never even runs).

---

## Patch 22 — HDR skybox via the ENGINE sky loader (Patch 21 patched the WRONG path)  *(APPLIED)*

**Files:** `engine/gl/gl_shader.c` (`Shader_ParseSkySides`)  ·  search `nettest`

Patch 21 added the `$hdr:` decode to `mat_vmt.c`'s UnlitGeneric path — but the HL2 **skybox** never goes through a
VMT material. worldspawn `skyname` (wad.c:1016) → `R_SetSky` (gl_warp.c) → the 6 faces load as RAW IMAGE FILES via
`R_LoadHiResTexture` in `Shader_ParseSkySides` (gl_shader.c:701) with `IF_NOALPHA|IF_CLAMP|IF_LOADNOW` — **no
`IF_HDRDECOMPRESS`** — so the (correct) img_vtf decode never fired for the sky and it kept banding. Verified the real
HL2 asset naming from the mounted VPK: each sky ships an LDR face `<skyname><side>` AND a compressed-HDR face
**`<skyname>_hdr<side>`** (e.g. `sky_day01_01rt` + `sky_day01_01_hdrrt`).

FIX: in `Shader_ParseSkySides`, wrap the existing pattern×suffix probe in a 2-pass loop — **pass 0** builds
`hdrname = "<texturename>_hdr"` and loads with `IF_HDRDECOMPRESS|IF_CLAMP|IF_LOADNOW` (alpha kept — it's the HDR scale);
**pass 1** is the original LDR face with `IF_NOALPHA|IF_CLAMP|IF_LOADNOW`. First hit wins, so an HDR face is preferred
and decoded (img_vtf rgb*alpha*8), falling back to LDR when no `_hdr` variant exists (CS:S / older skies → unchanged).
Flags flow straight through `image.c` `ReadImageFile(flags,…)` to `Image_ReadVTFFile`, so the Patch-21 decode now
actually runs. An `_hdr` face stored as RGBA16F (not RGBS/BGRA8) just loads as native float and ignores the flag (the
decode gate is `vmffmt==VMF_BGRA8`). The Patch-21 `mat_vmt.c` emit stays (harmless; covers genuine UnlitGeneric world
sky-brush materials).

---

## Patch 23 — VBSP world-surface emit overrun guard (d1_canals team-select RAM-explosion crash)  *(APPLIED)*

**Files:** `plugins/hl2/mod_vbsp.c` (`VBSP_RecursiveWorldNode` + `VBSP_PrepareFrame` + new `VBSP_EMIT_SURF` macro)  ·  search `nettest`

d1_canals loaded + rendered the team-select overview, then on certain camera angles **RAM ramped ~1GB→10GB in ~1s and
crashed**. That signature = a runaway allocation from **heap corruption**, not a normal segfault. Diagnosed (4-agent
workflow; the new prop/HDR/flag code was independently *exonerated*) to the VBSP world render: `mod->nummodelsurfaces`
is narrowed to `cmodels[0].numsurfaces` (mod_vbsp.c ~4305), so `Mod_Batches` assigns `surf->sbatch` + counts
`maxmeshes` only over THAT range — but the leaf/node walk (`VBSP_RecursiveWorldNode`) emits ANY leaf-marked surface
(validated only vs the LARGER `loadmodel->numsurfaces`). When a camera viewpoint brings an out-of-range surface into a
visible leaf, `surf->sbatch->mesh[surf->sbatch->meshes++]` writes through a NULL/unsized `sbatch` and overruns the
contiguous `bmeshes` block → smashes an adjacent allocation's size header → a later malloc reads it as multi-GB. It is
scene/camera-specific because it only fires when those surfaces enter that angle's PVS leaves.

FIX (crash stopper): a `VBSP_EMIT_SURF(s)` macro replaces all FOUR raw emit sites (leaf walk, node walk, surfvis==NULL
fallback, displacement loop). It writes only when `s->sbatch && s->sbatch->meshes < s->sbatch->maxmeshes*R_MAX_RECURSE`
(the real mesh[] capacity — engine + plugin resolve `R_MAX_RECURSE` from the same `#ifndef`-guarded headers, so they
agree), else drops the surface (never corrupts the heap) and warns the first 8 times at `developer 1`. Plus a load-time
`developer 1` print of `total vs model0(batched)` surface counts to confirm/quantify the gap. NOTE: this is the
definitive crash-stopper; if it drops VISIBLE world surfaces (holes), the follow-up ROOT fix is to widen the world
batch range so every walk-reachable surface gets a batch (set `mod->firstmodelsurface=0` / `nummodelsurfaces=numsurfaces`
for the Batches build) — not applied yet, pending the runtime gap/drop counts.

Rebuilds: engine (`make m-rel`) + hl2 plugin (`make plugins-rel NATIVE_PLUGINS=hl2 CC=gcc`).

---

## Patch 24 — ODE trimesh-data leak (the REAL d1_canals 12GB-OOM crash) + brush "no geometry" per-frame spam  *(APPLIED)*

**Files:** `engine/server/progdefs.h` (+1 field) + `engine/common/com_phys_ode.c`  ·  search `nettest`  ·  also `engine/common/zone.c` (`[bigalloc]` diagnostic)

The d1_canals_03 "RAM spikes to ~12GB super fast, then crash" was NOT props (`sv_props_physics 0` still crashed), NOT
the Patch-23 VBSP overrun (its guard never tripped), and produced NO `[bigalloc]` line — i.e. a flood of sub-256MB
allocations, a leak. Root cause: **ODE leaks the trimesh data of every brush-entity collision geom.** Every SOLID_BSP
entity maps to `GEOMTYPE_TRIMESH` (com_phys_ode.c:2090), so ODE builds a trimesh geom for each brush ent (and the
world). That geom rebuilds whenever mins/maxs/mass/modelindex change (~:2155) — **every frame** for a moving/rotating
brush ent (`func_door_rotating`) or one re-`setmodel`'d by brushsync. Each build does
`dataID = dGeomTriMeshDataCreate()` (:2200, holds the trimesh + BVH) but **`dGeomTriMeshDataDestroy` was never called
anywhere** — `dGeomDestroy` frees the geom, NOT its data — so a fresh dataID (1–50MB) leaked every frame → 12GB in
seconds → OOM. (Confirmed via the `[bigalloc]` tripwire's *silence*: a single >256MB alloc would have printed.)

- **FIX (leak):** added `void *geomdata` to `entityrbe_t` (progdefs.h, after `massbuf`); store it at create
  (`ed->rbe.geomdata = dataID;` after the `dCreateTriMesh`); and in `World_ODE_RemoveFromEntity` (after the
  `dGeomDestroy`) `dGeomTriMeshDataDestroy((dTriMeshDataID)ed->rbe.geomdata)` + null it.
  (`dGeomTriMeshGetTriMeshDataID` is commented-out/unavailable in FTE's ODE bindings, so the dataID must be tracked on
  the entity — can't be fetched back from the geom. `dGeomTriMeshDataDestroy` IS imported, so no new binding needed.)
- **FIX (spam):** the per-frame `entity N (classname func_door_rotating/func_breakable) has no geometry`
  (world.c:2973, dev-1) — these Source brush ents build no faces in FTE so `GenerateCollisionMesh` fails; the failure
  path called `World_ODE_RemoveFromEntity` which reset `rbe.physics=false`, so the `!physics` rebuild condition
  re-fired EVERY frame (re-attempt + re-spam + re-count surfaces). Fix: on no-geometry failure leave `physics=true` +
  `geom=NULL` (mins/maxs/modelindex were just recorded) so it only retries if those change → one attempt per ent.
- **Diagnostic (kept):** `zone.c` BZF_Malloc/Z_Malloc print `[bigalloc] N bytes` for any single alloc >256MB (logged
  BEFORE the malloc so it survives an OOM). Harmless; useful for the next single-giant-alloc.

**Build note:** progdefs.h changes `entityrbe_t`'s size → rebuild ENGINE + PLUGIN together (ABI). Rebuilds: engine
(`make m-rel`) + hl2 plugin (`make plugins-rel NATIVE_PLUGINS=hl2 CC=gcc`).

---

## Patch 25 — the ACTUAL d1_canals_01 OOM: unbounded CSQC entity-array realloc (remote-DoS) + the static-prop mirror flood that triggered it  *(APPLIED)*

**Files:** `engine/client/pr_csqc.c` (`CSQC_EntityCheck` + its 4 call sites — KEEP) · mod-side `nettest/src/server/sv_props.qc` (`Props_SpawnStatic`)  ·  search `nettest`

**This is the real cause.** Patches 17–20 / 23 / 24 each fixed a genuine but *different* d1_canals issue (NULL guards, PVS cull, the ODE trimesh leak) — **none** was the 12–13GB OOM the user kept hitting on **d1_canals_01** (canals_03 stayed ~1GB throughout). Decisive repro: set the menu-backdrop pool (`nettest/data/maps.txt`) to **only `d1_canals_01`**, launch → RAM ramps to ~12GB and crashes in ~25s, screen never renders a frame (blank).

**Diagnosis (the trail every earlier canals patch missed).** The 12GB was NON-zone AND NON-BZ — it bypassed the `[zmem]`/`[bigalloc]` tripwires entirely, because those hook `Z_Malloc`/`BZ_Malloc` but **`BZ_Realloc` is plain `realloc()` — untracked**. A `[realloc]` high-water tripwire added to `BZF_Realloc` then caught it: a **single `BZ_Realloc` of ~34GB** (`32767 MB`). `__builtin_return_address(0)` captured at the `BZ_Realloc` entry, resolved with `addr2line` on the unstripped `release/fteqw64.exe.db` (ASLR defeated via an anchor symbol delta), pinned the caller to **`CSQC_EntityCheck`** (pr_csqc.c) — the CSQC entity array.

**Root cause (engine).** `CSQC_EntityCheck(entnum)` grows the `csqcent[]` pointer array to `entnum × sizeof(ptr)` with **no upper bound**. The CSQC entity-delta wire format reads a 22-bit index as `(entnum & 0x3fff) | (MSG_ReadByte() << 14)`. When a packet is **truncated mid-header** (the 2-byte short reads, the extended-index byte hits EOF), `MSG_ReadByte()` returns **-1** and the `<<14` **sign-extends** into the high bits → `entnum ≈ 0xFFFFFFxx` (~4.29 billion) → `4.29e9 × 8 = ~34GB` `BZ_Realloc` + `memset` (the memset page-touch is what actually balloons RAM) → OOM. **Also a remote-DoS:** any malicious/buggy server can truncate a CSQC entity packet and OOM-crash a connected client.

**Root cause (why d1_canals_01 specifically — a regression from this session's Source-prop feature).** `sv_props.qc Props_SpawnStatic` networked **every `prop_static` as an always-on CSQC entity** (`SendEntity = phys_files_Send` + `pvsflags = PVSF_NOREMOVE` = sent to every client, ignoring PVS). HL2 d1_canals_01 has **thousands** of decorative props (CSQC entnums observed past 8000) → the CSQC entity datagram **overflowed**, truncating mid-entity → the corrupt index. canals_03 / surf maps have few props, so they never overflowed → never tripped it.

**FIX (engine, `pr_csqc.c`):**
- `CSQC_EntityCheck` now returns `qboolean` — **`false` for `entnum > 0x3fffff`** (beyond the 22-bit protocol max = a corrupt/truncated read), refusing the realloc.
- The 4 callers recover **gracefully** instead of `Host_EndGame`: the entity-delta loop (update + remove) `break`s — treats it as end-of-packet, keeping the connection AND the entities that parsed; the two CSQC sound paths `return false` (engine plays the sound normally). A prop-dense map that overflows the CSQC datagram now just renders the entities that fit, instead of OOM-ing or disconnecting to a black screen.

**FIX (mod, `sv_props.qc`):** `Props_SpawnStatic` no longer attaches a per-prop CSQC mirror. Static props don't move, so the engine's standard **PVS-culled** entity networking renders them (modelindex/angles/colormod/alpha all network) at near-zero ongoing cost and scales to thousands. Server `SOLID_PHYSICS_BOX` still blocks bullets/grenades/non-predicted moves; the only loss is local predicted-pmove collision on decorative clutter (minor rubber-band). `prop_physics` (few, interactive) keep their mirror. Recompile QC: `fteqcc64_latest.exe progs.src`.

**Cleanup:** the OOM-hunt diagnostics added across this and earlier canals patches were removed afterward — `[bigalloc]`/`[zmem]`/`[realloc]` (zone.c), the `[glwrap]` qglTexImage2D interposer (gl_vidcommon.c), `[gltex]` (gl_draw.c), `[glclamp]`/`[glbuf]` (gl_backend.c), `[lmdbg]` + the unrelated lightmap-atlas cap (gl_model.c), and the `[vtf-hdr]`/`[sky]`/`[vbsp]`-gap dev prints. *(Supersedes Patch 24's "diagnostic kept" note — `[bigalloc]` is gone too.)* The real feature/fix patches (21/22 HDR sky, 24 ODE leak, this one) stay.

Rebuilds: engine `make m-rel FTE_TARGET=win64 CC=gcc` (pr_csqc.c is engine-only; no plugin/ABI change). The mod fix is QC-only.

---

## Patch 26 — offline map index (fixes missing CS:S maps) + add-only lazy mounting (DEFAULT ON)  *(APPLIED)*

> **STATUS / OUTCOME:** both parts now ship. The **offline map index** lists CS:S/CoD maps correctly in the create-server menu. The **lazy on-demand mounting** is now **DEFAULT ON (`fs_lazyaddons 1`)** and crash-safe, after fixing the original FATAL FLAW. **The fix: `fs_useaddons` is now ADD-ONLY** — it calls `FS_Addon_Mount` per spec (which appends a searchpath at lowest priority and BuildHashes the new files, dup-skipping an already-mounted path) and **never calls `FS_ReloadPackFiles`**. The old crash was the rebuild: `FS_ReloadPackFiles` FREES all old searchpaths (`ClosePath`, fs.c:~5617) while a map/backdrop server holds that content → dangling pointers → access violation on Source-map load and on disconnect-to-backdrop. Add-only never frees, so it is safe to mount mid-map (the always-running backdrop server is fine). Trade-off: games **accumulate** for the session (never unmounted mid-session) — bounded by the 6 configured games; each game's one-time `BuildHash` is paid lazily on first selection. Boot mounts only the small GoldSrc base (valve+cstrike via the backdrop dep-mount); the heavy hl2/CSS `.vpk` + CoD `.iwd` stay lazy until their map is picked → faster boot. **Engine-only change** (no QC change — the menu already issues `fs_useaddons …` then `map …` in the same FIFO buffer; they now ADD instead of rebuild). KNOWN separate issue: same-named maps still shadow by mount priority (cstrike(1.6) outranks CS:S → `cs_assault` loads the GoldSrc version) — fixed by the **package-qualified map load** follow-up (Part 2 in the plan), plus the Source `.mdl` prop crash below.



**Files:** `engine/common/fs.c` (extends the P8 fs_load addon system) · mod-side `nettest/src/menu/m_createserver.qc` + `m_main.qc`  ·  search `nettest (P25)` / `nettest (P26)`

**Two problems, one subsystem (the multi-game filesystem):**
1. **Slow boot.** `fs_addons.txt` lists 6 external games (Half-Life/valve, Half-Life/cstrike=CS1.6, Half-Life 2/hl2, Counter-Strike Source/cstrike=CS:S, CoD1, CoD2). They were ALL mounted + hash-indexed (`BuildHash`, fs.c:4344) serially on the main thread at every searchpath rebuild (`FS_RemountAddons`, fs.c:5641) — a ~0.5–1s boot stall.
2. **Missing CS:S maps in the create-server menu.** CS:S and CS1.6 both mount as the leaf dir `cstrike` and share map names (`de_dust2.bsp`, different BSP formats). With both in one search path the same-named maps **shadow** each other, and the menu classified by `search_getpackagename` which returned the ambiguous `cstrike` for both → CS:S maps mis-bucketed as CS1.6 → absent from the CS:S tab.

**Approach: lazy-mount.** Don't mount the games at boot; cheaply *index* their maps (loose `.bsp`/`.d3dbsp` files → a directory read needs no mount); mount a game only when one of its maps is loaded. Because each game is enumerated separately, same-named maps become distinct, correctly-tagged entries — fixing #2 for free.

**Engine (`fs.c`):**
- **`fs_lazyaddons`** cvar (default `1`; `0` = old eager mount-all-at-boot, escape hatch).
- **`FS_IndexAddonMaps()` / `fs_indexmaps`** — writes `data/maps_index.txt`, one `"<sourcespec>\t<mapname>"` line per map. The mod's own maps come from `FS_EnumerateNonAddonFiles` (a `COM_EnumerateFiles` variant that SKIPS `SPF_ADDON` paths so an already-mounted game can't mis-tag them); each addon game's maps come from `Sys_EnumerateFiles("<FS_Addon_Resolve(spec)>/maps", "*.bsp"/"*.d3dbsp")` — OFFLINE, no mount, no BuildHash. Not installed → resolve fails → game silently skipped.
- **`fs_useaddons <spec> [<spec> …]`** — **ADD-ONLY**: for each spec, `FS_Addon_Mount(spec, ~0u)` (resolves steam:/abs/rel, dup-skips an already-mounted path, appends at LOWEST priority, BuildHashes the new files), under `fs_thread_mutex` (the hash add is the Unsafe/no-internal-lock variant). **No `fs_activeaddons`, no clear, no `FS_ReloadPackFiles`** — so it never frees searchpaths and is crash-safe while a map is loaded. Games accumulate for the session. Run right before `map` (same command buffer, FIFO). No-op in eager mode (`fs_lazyaddons 0`).
- **Gate** (`FS_ReloadPackFilesFlags`, fs.c:~5650): a genuine rebuild (`fs_restart`/gamedir switch) always calls `FS_RemountAddons()` — re-mounting every persisted `fs_addons.txt` game (add-only at tail) so the user's games survive a restart. There is NO lazy branch / `FS_MountActiveAddons` anymore; on-demand mounting is exclusively the add-only `FS_UseAddons_f` path. The mod's own gamedir + FTE base are unaffected (they load via the normal gamedir path, not fs_addons.txt).

**Menu:** `create_server_read_index(filter)` reads `data/maps_index.txt` (replacing the `search_begin` scan), classifying each map by its true source spec via the existing `create_server_map_game()` (now reliable: `"…Counter-Strike Source/cstrike"`→CS:S, `"…Half-Life/cstrike"`→CS1.6). `create_server_launch` calls `create_server_mount_for_map(map, filter)` → `fs_useaddons <deps>` before `map`. **Dep sets = an always-mounted BASE (`valve` + `cstrike`) + the map's own game added on top** — because the mod currently borrows gameplay/map assets from Half-Life + CS 1.6, so EVERY map (nettest's own included) needs them. CS:S → base + `hl2` + CS:S-cstrike; HL2 → base + `hl2`; CoD → base + that CoD dir; CS1.6/HL/nettest → base only. (An earlier EXCLUSIVE per-game mount was the bug: dropping valve/cstrike for a CS:S/HL2/CoD map stripped the mod itself → "maps that worked before stop working" + missing assets on nettest maps. Set `base` to `""` in `create_server_deps_for_spec` once nettest is fully self-contained → then it's truly lazy.) `m_main.qc m_init` issues `fs_indexmaps` at boot; the backdrop dep-mounts the base (so boot mounts only the small GoldSrc valve+cstrike, with the heavy hl2/CSS .vpk + CoD .iwd staying lazy until their maps are picked).

**SHADOWING (was a KNOWN LIMITATION, now FIXED by Part 2 below):** mount priority means a same-named map loads the highest-priority copy — nettest (the mod) outranks the base, and the base CS1.6 `cstrike` outranks CS:S. So `map de_dust2` loaded nettest's, and `map cs_assault` loaded the CS1.6 GoldSrc one, never the CS:S VBSP. **Part 2** adds a package-qualified map load so the create-server menu can pick a SPECIFIC game's copy.

**Tradeoff (intended):** the first selection of each game pays its one-time `BuildHash` (~0.5–1s), moved off boot onto first-use. With add-only mounting a game also stays mounted for the rest of the session (no unmount), so a second different-game selection just adds; memory is bounded by the 6 configured games.

Rebuilds: engine `make m-rel FTE_TARGET=win64 CC=gcc` (Part 1 is **engine-only**, no QC rebuild — the menu already emits `fs_useaddons … ` then `map …`). Verify: cold boot faster, `path` shows only valve+cstrike mounted; selecting a CS:S/HL2 map mounts+hashes that game then loads with **no access violation**; **disconnect back to the backdrop no longer crashes** (the rebuild that used to dangle content is gone); repeated cross-game selections accumulate without crashing; `fs_restart` remounts all games.

**Follow-ups (post-review + user testing):**
- **Archived maps (CoD).** `FS_IndexAddonMaps` first only readdir'd LOOSE `maps/` files, so CoD maps (which live inside `.iwd`/`.pk3` zips — `iw_00.iwd`…) went un-indexed (a regression vs the old eager mount that read inside them). Fixed with `FS_IndexArchive_Visit`: per game dir, `Sys_EnumerateFiles` the `*.iwd`/`*.pk3`/`*.pk4`/`*.pak` archives, `FS_OpenPackByExtension` each (central-directory read only, NO mount/BuildHash), enumerate its `maps/*.bsp`+`*.d3dbsp`, `ClosePath`. Now 33 CoD1 + 39 CoD2 maps index (gid 4/5). `.iwd` is the base zip loader (fs.c:8926), no plugin needed.
- **Add-only conversion (the crash fix, supersedes the old rebuild design).** The original `fs_useaddons` did `FS_ReloadPackFiles` (full rebuild: free all searchpaths + re-add) to make EXACTLY the requested set active — which dangled loaded content mid-map and crashed. Replaced with pure ADD-ONLY `FS_Addon_Mount` per spec (no free, no rebuild). Consequence: the old "clear the previous game" / "no-op when the set is unchanged" logic is **gone** (there is no active-set to swap; games just accumulate). The menu's bare-`fs_useaddons`-to-clear for nettest maps is now a harmless no-op (nothing to clear). Shadowing of same-named maps is no longer "fixed" by exclusivity (it never safely was) — it is handled by the package-qualified map load (Part 2).
- **KNOWN ISSUE (separate, NOT lazy-mount):** prop-heavy CS:S/HL2 maps (e.g. `cs_assault`) crash in the Source `.mdl` loader (`models/props_lab/pipesystem02a.mdl` "Unable to load / no collision info" → access violation). Pre-existing in the Source-prop feature, only newly *reachable* because those maps now list. Lighter CS:S maps load fine. Needs its own reproduce + `.db` addr2line crash-hunt.

---

### Patch 26 **Part 2** — package-qualified map load (load a SPECIFIC game's copy of a same-named map)  *(APPLIED)*

**Problem:** `cs_assault.bsp` / `de_dust2.bsp` exist in several mounted games; `FS_FLocateFile` (fs.c:~2176) takes the FIRST (highest-priority) searchpath hit, so the CS:S VBSP can never load while the CS1.6 GoldSrc copy (or nettest's) outranks it. **Fix:** a `map "@<gamespec>/<mapname>"` qualifier biases ONLY the worldmodel BSP locate to one game's resolved dir — no searchpath reorder, no rebuild (crash-safe, composes with Part 1).

**Files / search `nettest (P26 Part 2)`:**
- **fs.c** — global `char fs_preferhint[MAX_OSPATH]` (the resolved game dir to prefer; "" = off). In `FS_FLocateFile`: the `com_fs_cache` fast-path gains `&& !*fs_preferhint` (a set hint must bypass the cache, since the hash keeps only the highest-priority hit per name — the exact copy we want to override); a NEW **preferred pass** runs before the pure/main loops — it iterates `com_searchpaths` and `FindFile`s only in searchpaths whose `logicalpath` matches the hint **at a path boundary** (`Q_strcasestr` + the next char must be `/`,`\` or end — so `.../Call of Duty` can't match `.../Call of Duty 2`); on a miss it leaves `found==FF_NOTFOUND` so the normal loops run as the fallback (empty-hint path is byte-identical to before). `FS_SetPreferHint(spec)` resolves a game spec via the SAME `FS_Addon_Resolve` that produced each addon's `logicalpath` (so the match is exact); `FS_ClearPreferHint()`.
- **common.h** — declares `FS_SetPreferHint`/`FS_ClearPreferHint` (next to `FS_FLocateFile`; NOT fs.h — sv_init.c includes common.h, not fs.h).
- **sv_ccmds.c** `SV_Map_f` — global `char sv_mappreferhint[MAX_OSPATH]`, cleared at function entry (so `map_restart`/`changelevel`/savegame paths that skip the parse can't inherit a stale hint). The package-manager `:` parse skips args starting with `@`. The `@<spec>/<map>` arg (always QUOTED by the menu → one token) is split on the **LAST** `/` (the spec itself contains `/` and `:`): tag→`sv_mappreferhint`, bare mapname→`level` (so all downstream existence checks + `sv.modelname` use the bare name).
- **sv_init.c** `SV_SpawnServer` — brackets ONLY the worldmodel `Mod_ForName(sv.modelname, MLV_ERROR)` (which blocks until loaded, even on a loader thread): `FS_SetPreferHint(sv_mappreferhint)` then immediately `sv_mappreferhint[0]=0` (one-shot consume — can't leak to a later cache-hit/savegame spawn); `FS_ClearPreferHint()` after the load + the consistent mtime-reload. Scoped to the `.bsp` only, so precache + the mod's own same-named content are NOT biased.
- **m_createserver.qc** `create_server_launch` — on a specific game tab (`create_server_game_filter != 0`) with a known source spec, emits `map "@<spec>/<mapname>"`; the All tab + unqualified paths keep plain `map %s`. Mount step (`fs_useaddons`, Part 1) unchanged — both games may be mounted; the hint disambiguates.

**Adversarially reviewed** (multi-agent find→verify): fixed a `map_restart` stale-hint leak + a savegame-cache-hit leak (entry-clear + one-shot consume) and added the path-boundary match. Rejected a false "mtime-reload double-bias" finding — the reload intentionally reuses the same preferred copy.

**Verify:** on the CS:S tab, Start `cs_assault` (or `de_dust2`) → loads the Source VBSP, not the GoldSrc/nettest copy (confirm in-world or `flocate maps/cs_assault.bsp` shows the CS:S logicalpath). Console `map "@steam:Counter-Strike Source/cstrike/cs_assault"` works directly. All-tab + non-shared maps + `map_restart` behave exactly as before. Rebuild: engine `make m-rel FTE_TARGET=win64 CC=gcc` + menu `fteqcc64_latest.exe m_progs.src`.

---

## Patch 27 — multiplayer connect crash: flush the FS hash on EVERY pack rebuild  *(APPLIED, engine-only)*

**Symptom (the "scary" one):** a second client connecting to a listen server — or any client connecting to the dedicated server — **crashed instantly, no RAM spike, no error** (the server just logged `Client removed` / `timed out`). Pre-existing (not caused by the lazy-mount work; reproduced with `fs_lazyaddons 0` too).

**Root cause:** `FS_ReloadPackFilesFlags` ([fs.c](C:\msys64\home\Lex\fteqw\engine\common\fs.c) ~5683) only flushed the `filesystemhash` when the searchpath *order* changed (`if (next || i != orderkey)`). A connect triggers a `COM_Gamedir` reload that **closes + reopens the same packs without reordering them**, so the flush was skipped — but `FS_ReloadPackFilesFlags` had still freed the old `searchpath_t`s, leaving the hash full of **dangling pack-bucket pointers**. The next `FS_AddFileHashUnsafe` / lookup walked freed memory → access violation deep in the connect path (which is why it surfaced on the *connecting* client, not the server).

**Fix (one condition):** flush the hash whenever the pack set is rebuilt at all, not just on reorder:
```c
//nettest: was `if (next || i != orderkey)`.  A COM_Gamedir reload (e.g. on connect) re-opens the SAME packs
//without reordering, so the old test skipped the flush while the searchpaths were freed underneath the hash
//-> dangling pack buckets -> crash. Flush on ANY rebuild (reloadflags set) too.
if (next || i != orderkey || reloadflags)
```
**Verify (done):** two GL clients on a listen server, and a GL client → dedicated server, both connect and STAY connected; `crashaddr.txt` stays empty. Holds for `fs_lazyaddons` 1 (default) and 0.

---

## Patch 28 — `cl_launchintogame`: `+connect` / `+map` / demo launch options bypass the menu backdrop  *(APPLIED, engine + 1 QC line)*

**Why:** the menu loads a local "backdrop" listen server in `m_init` (`map <cl_menumap>`). That `map` ran from `m_init` and **clobbered any command-line `+connect <ip>` / `+map <name>`**, so launching the game with those options always dumped the user on the menu world instead of the server/map they asked for.

**Mechanism:** a new engine cvar tells the menu the launch intent.
- **[cl_main.c](C:\msys64\home\Lex\fteqw\engine\client\cl_main.c)** — `cvar_t cl_launchintogame = CVARFD("cl_launchintogame","0", CVAR_NOSAVE|CVAR_NORESET, …)`. In **`Host_Init`, immediately before `M_Init()`** (which is where the menu's `m_init` runs — and `M_Init` is *before* `CL_Init`), register it and `Cvar_ForceSet` it to `"1"` iff the command line has any of `+connect/+map/+spmap/+devmap/+gamemap/+changelevel/+playdemo/+demo/+qtvplay`.
- **[m_main.qc](c:\FTEQuake\nettest\src\menu\m_main.qc)** `m_init` — gate the backdrop on it: `if (menumap != "" && menumap != "none" && cvar("cl_launchintogame") == 0) Menu_StartBackdrop(menumap);` (backdrop logic was lifted into a `Menu_StartBackdrop(menumap)` helper).

**THE two traps that made this hard (both essential):**
1. **`CVAR_NORESET` is mandatory.** `Cvar_GamedirChange()` ([cvar.c:602](C:\msys64\home\Lex\fteqw\engine\common\cvar.c#L602)) runs as the fs mounts the game during boot and **resets every registered cvar to its engine default BEFORE `m_init` reads it** — so without `NORESET` the value was silently wiped back to `0` (the symptom: `m_init` always saw `0`). `NORESET` makes `Cvar_GamedirChange` skip it (line 610). Do **not** try to fix this via `Cvar_SetEngineDefault` — its `Z_Free(enginevalue)` corrupts the heap (0xC0000374) because a `CVARFD` cvar's `enginevalue` is the literal default string, not heap.
2. **Set it before `M_Init`, not in `CL_Init`.** The menu's `m_init` runs inside `M_Init()` (Host_Init:~8071), which is BEFORE `CL_Init()` (~8077). Registering the cvar in `CL_Init` is too late — `m_init` has already read it.

**Console shorthand still works** independently: `map @<spec>/<name>` and the `mapfrom`/`css`/`hl`/`cod` aliases (Patch 26 Part 2) are unaffected.

**Verify (done):** with the dedicated server up, `fteqw64.exe … +connect 127.0.0.1:27500` connects straight in (no `Server spawned` backdrop) across repeated runs; `+map 2fort` boots straight into 2fort; a plain launch still loads the backdrop. No crash, no debug output.

---

## Diagnostic: vectored crash-address logger (kept)  *(APPLIED, engine-only)*

The mingw release build has **no crash handler** (`CATCHCRASH` is debug-only; no minidump). `nettest_CrashAddrLogger` in [sys_win.c](C:\msys64\home\Lex\fteqw\engine\client\sys_win.c) (an `AddVectoredExceptionHandler`, registered unconditionally) writes `code/addr/base/rva` + a 28-frame `CaptureStackBackTrace` to `C:\FTEQuake\nettest\crashaddr.txt` via raw Win32 `CreateFileA` (bypassing the `fopen_nolink` sandbox) on a fatal exception, then `EXCEPTION_CONTINUE_SEARCH`. Resolve with `addr2line -e fteqw64.exe.db -f -C -i (0x140000000 + rva)`. NOTE: it catches access-violations etc. but NOT heap-corruption aborts (`0xC0000374`), which don't dispatch through the VEH.

---

## Patch 29 — don't advertise SPF_ADDON game-mounts to clients (the `dlcache absolute path` MP-join break)  *(APPLIED, engine-only)*

**Symptom:** a client connecting to a dedicated server that has external games mounted (HL2 / CS:S / CoD via `fs_addons.txt`) connects fine, then **loops forever re-mounting the addon vpkfiles and spamming `Error: absolute path in filename C:/dlcache/games/call.<hash>`** (fs.c:2648), never spawning into the map.

**Root cause:** `FS_GetPackHashes` ([fs.c](C:\msys64\home\Lex\fteqw\engine\common\fs.c) ~2511) and `FS_GetPackNames` (~2544) build the server's advertised package list (`//paks` / `//paknames`, sv_user.c) by iterating `com_searchpaths` with only a `crc_check` guard — **no `SPF_ADDON` filter**. The addon game-mounts carry **absolute** Steam/CoD `purepath`s (e.g. `C:/games/Call of Duty/Main/...`), so the client feeds them to `FS_GenCachedPakName` → `C:/dlcache/games/call.<hash>`, which `FS_GetCleanPath` rejects (absolute-path sandbox), and the `//paks` re-validation re-fires every message → infinite loop. `SPF_ADDON` ([fs.h:134](C:\msys64\home\Lex\fteqw\engine\common\fs.h#L134)) is explicitly documented "no pure/server semantics"; `FS_EnumerateNonAddonFiles` already filters them the same way.

**Fix (two one-liners):** in the non-pure `else` branches of both functions, change `if (search->crc_check)` → `if (search->crc_check && !(search->flags & SPF_ADDON))`. Addon mounts are now invisible to clients; the server still uses them locally for map content.

**Verify (done):** with the fix, the `absolute path` error is gone and the client downloads only the csprogs + real assets. **THIS is the fix that made MP work** — the user had been running the OLD server build which lacked it (see the two-install note in memory).

---

## Patch 30 — csprogs download deadlock → self-heal + clear error  *(APPLIED, engine-only)*

**Symptom (cost hours of confusion):** client connects, "downloads" csprogs (file written fully to disk), then **deadlocks forever with no map** — most often because the dedicated server was running a **different csprogs build** than the client (recompile-without-restart), or a **stale/bloated cached** `csprogsvers/<hash>.dat` (e.g. accumulated from earlier retry-appends).

**Root cause:** in `CL_LoadModels` ([cl_parse.c](C:\msys64\home\Lex\fteqw\engine\client\cl_parse.c) ~1294-1304), when `CSQC_CheckDownload` fails but the `csprogsvers/<hash>.dat` file is already on disk, `CL_CheckOrEnqueDownloadFile(..., DLLF_REQUIRED)` returns true (file present, won't re-pull without `DLLF_OVERWRITE`) → `return -1` every frame → `cl.contentstage` never advances → silent hang.

**Fix:** after `CL_IsDownloading` is false, if `CL_CheckDLFile(str)` shows the file present-but-still-invalid: force ONE clean re-download (`FS_Remove` the stale file + re-enqueue with `DLLF_REQUIRED|DLLF_OVERWRITE`); if the fresh copy STILL fails to validate (tracked by a hash-keyed static so it can't loop), `Host_EndGame("csprogs checksum mismatch … rebuild + RESTART the dedicated server …")`. Converts a silent deadlock into either auto-recovery or an actionable error. Failure-path only — never runs on a successful join.

**Note:** a single-machine "dedicated server + client from the SAME gamedir" self-test shows a csprogs self-download that doesn't finalize — that's a TEST-HARNESS artifact, NOT a real bug; real two-party joins work (verified by the user: connected, joined, shot, bought, chatted).

---

## Patch 31 — host Source/VBSP (+ CoD) maps on a DEDICATED server (headless plugin + ODE collision crash)  *(APPLIED, plugins + engine)*

**Symptom:** the dedicated server (`fteqwsv64.exe`) loaded GoldSrc (CS1.6) + nettest maps fine, but **crashed ~1s after** `+mapfrom css de_dust2` (`0xC0000005`), and would crash identically on CoD maps. The `hl2: TTH/VTF/VMT support unavailable` / `cod: Shader Types/IWI support unavailable` banners at boot are the SAME headless condition surfacing *gracefully* — only the map-load path forgot to check it.

There were **TWO independent crashes** stacked on the same map load:

**Crash #1 — the loader plugins' renderer-only material pass (plugin fix).** `fteplug_hl2_x64.dll` / `fteplug_cod_x64.dll` are single DLLs built WITH `HAVE_CLIENT` (`plugins/Makefile:512` passes neither `-DSERVERONLY` nor `-DCLIENTONLY`), so their material/lightmap pass is compiled in. They hardcode `qrenderer = QR_OPENGL` and never set `QR_NONE` headless, so `if (qrenderer != QR_NONE)` stayed true and the pass ran — calling `modfuncs->RegisterBasicShader` / `modfuncs->Batches_Build`, which a SERVERONLY engine leaves **NULL** (`engine/common/plugin.c:2424-2433`, inside `#ifdef HAVE_CLIENT … #else NULL,NULL`). Call-through-NULL → crash. (Collision is safe: `BIH_Build` is non-NULL on the server, `plugin.c:2421`, unconditional.)
- **[mod_vbsp.c](C:\msys64\home\Lex\fteqw\plugins\hl2\mod_vbsp.c)** `VBSP_Init` (~4413, after the interface fetches): `if (!plugfuncs->GetEngineInterface(plugimagefuncs_name, sizeof(plugimagefuncs_t))) qrenderer = QR_NONE;` — the exact NULL-interface probe `VTF_Init` already uses (`img_vtf.c:288`). Non-NULL on the client → keeps `QR_OPENGL`. The file-global `qrenderer` (~59) then makes every existing gate (`qrenderer != QR_NONE` at the world + submodel material pass ~4327/4381; `haverenderer` lighting) skip correctly. No other gating edits needed (the load funcs + BIH are server-safe and keep running).
- **[codbsp.c](C:\msys64\home\Lex\fteqw\plugins\cod\codbsp.c)** had NO `qrenderer` global and its gate was **commented out** (`:1845`), so `CODBSP_GenerateMaterials` ran unconditionally. Added `r_qrenderer_t qrenderer = QR_OPENGL;` (~13), the **identical probe** in `CODBSP_Init` (~2010), and **uncommented** `if (qrenderer != QR_NONE)` (1845).

**Crash #2 — the engine building the ODE world-collision mesh (engine fix).** With crash #1 gone, the map still crashed when `fteplug_ode_x64.dll` built the world collision mesh. `World_GenerateCollisionMesh` → `GenerateCollisionMesh_BSP` ([server/world.c](C:\msys64\home\Lex\fteqw\engine\server\world.c) ~2941) walks `mod->surfaces[].mesh`. On a **headless VBSP** world, `surf->mesh` is ALLOCATED but UNFILLED (`xyz_array == NULL`) because the renderer's `Batches_Build` that fills it is skipped on a dedicated server — so the fill loop dereferenced a NULL `xyz_array`. (Q1/GoldSrc don't hit this: their `surf->mesh` is NULL on a dedicated server, so they already take the edge-path `else`.)
- **Fix:** both the count loop (~2963) and the fill loop (~2991) guard changed `if (surf->mesh)` → **`if (surf->mesh && surf->mesh->xyz_array)`**, so headless VBSP falls through to the same edge/box-hull `else` branch Q1 uses on a dedicated server. No-op on the client/listen server (renderer present → `xyz_array` filled → uses the mesh exactly as before).

**Located via** the dedicated-server crash logger (the dedi `main` has no crash handler — added `AddVectoredExceptionHandler(1, nettest_CrashAddrLogger)` in [sv_sys_win.c](C:\msys64\home\Lex\fteqw\engine\server\sv_sys_win.c):1718, a server-side parallel to the client's kept `nettest_CrashAddrLogger` — `crashaddr.txt` gave `mod=fteqwsv64.exe+0x849d6` with `fteplug_ode_x64.dll` in the frames; `addr2line` → `GenerateCollisionMesh_BSP`). Kept (release dedi otherwise dies silently).

**Build / deploy:** plugins `release/fteplug_hl2_x64.dll` + `fteplug_cod_x64.dll` (built directly with the engine's `BASE_CFLAGS`; `make plugins-rel` can't be used — its `native:` target builds the ffmpeg plugin first, which needs `unzip`); engine `sv-rel` (the world.c fix). **Deploy** both DLLs + `fteqwsv64.exe` → `C:\FTEQuake\`. The client `fteqw64.exe` (`m-rel`) does NOT need rebuilding for the world.c guard (no-op with a renderer) but DOES use the new plugins.

**Verify (done):** `fteqwsv64.exe -game nettest +mapfrom css de_dust2` stays alive (no crash; `crashaddr.txt` empty); a client connecting confirms the server IS on de_dust2 (`Map model file does not match (maps/de_dust2.bsp), 0XE0B39589 != 0/0`). Before these fixes it exited instantly.

**Follow-on (FIXED by Patch 32 below):** a connecting client got `Map model file does not match … kicked due to the file … being modified` — the worldmodel-CRC consistency check. The server loaded the CS:S map (via the Patch-26-Part-2 `@spec` prefer-hint) but the connecting client resolved a *different* same-named copy (the GoldSrc one), so the CRCs differed. The prefer-hint was server-spawn-only; Patch 32 propagates it to the client.

---

## Patch 32 — propagate the map prefer-hint to the connecting client (same-named map kick on connect)  *(APPLIED, engine-only)*

**Symptom:** with Patch 31 (dedi hosts Source maps), a client connecting to a dedicated server hosting a **same-named** map — `cs_assault` / `de_dust2` exist in BOTH Counter-Strike:Source (VBSP) and CS1.6/Half-Life (GoldSrc) — was **kicked at the map-check** and bounced to a grey menu, retrying forever: `Map model file does not match (maps/cs_assault.bsp), 0XAD2FC871 != 0/0` … `kicked due to the file … being modfied, located at …/Half-Life/cstrike/maps/cs_assault.bsp`. A map *unique* to one game joined fine.

**Root cause:** the Patch-26-Part-2 prefer-hint (`map @<spec>/<map>` → `FS_SetPreferHint`) was **server-spawn-only**. The server loaded the CS:S copy; the connecting client had no hint, so its connect-time worldmodel load resolved the **highest-priority** same-named copy — the GoldSrc `Half-Life/cstrike` one. Decoding the kick (`SV_PreSpawn_f`, [sv_user.c:2013-2030](C:\msys64\home\Lex\fteqw\engine\server\sv_user.c#L2013), format is `clientCRC != serverCRC/CRC2`): client's GoldSrc worldmodel = `0xAD2FC871`, server's VBSP worldmodel = `0`. **Checksum insight:** the VBSP/CoD worldmodel checksum is computed identically on client AND server (`VBSP_ComputeChecksum`, [mod_vbsp.c:4400](C:\msys64\home\Lex\fteqw\plugins\hl2\mod_vbsp.c#L4400), *outside* the `qrenderer!=QR_NONE` gate; CoD sets none → 0), so loading the **same** physical copy yields matching checksums → `check != worldmodel->checksum` is `X==X` → **no kick**. The kick only fired because the client loaded a *different-format* (GoldSrc, engine-checksummed) copy. So the entire fix is "make the client load the same copy" — no checksum/security weakening.

**Fix (two ends, both reusing the existing `FS_SetPreferHint`/`FS_FLocateFile` preferred-pass):**
- **Server** — [sv_init.c](C:\msys64\home\Lex\fteqw\engine\server\sv_init.c) `SV_SpawnServer`: a function-scope `char mappref_spec[MAX_OSPATH]=""` captures `sv_mappreferhint` (via `Q_strncpyz`) **before** the existing prefer-hint block zeroes the one-shot global, then advertises it as a new `*mappref` serverinfo star-key — `InfoBuf_SetStarKey(&svs.info, "*mappref", mappref_spec)` — right after the `*bspversion`/`*startspot` keys. Set **unconditionally** (`""` for plain/non-`@` maps) so it can't carry a stale value across maps. `*`-keys ride the `fullserverinfo` stufftext to every client (sv_user.c PRESPAWN_SERVERINFO, `prioritykeys` includes `"*"`). The capture buffer is at **function scope**, NOT inside the worldmodel-load `else`/prefer-hint blocks (those close before the star-key block runs).
- **Client** — [cl_parse.c](C:\msys64\home\Lex\fteqw\engine\client\cl_parse.c) `CL_ParseModellist`: read `*mappref` from `cl.serverinfo` and `FS_SetPreferHint` it around the worldmodel load, `FS_ClearPreferHint` after.

**THE non-obvious gotcha (cost most of the debugging):** the worldmodel load is **asynchronous** — dispatched to a `WG_LOADER` worker thread (`Mod_LoadModel`, gl_model.c). The *first* load wins: the **"we might as well START loading them now"** pre-load loop (~cl_parse.c:4697, `Mod_ForName(…, MLV_SILENT)`) kicks off the worldmodel (index 1) load **before** the obvious load site lower down — and `MLV_SILENT` does **not** block, so a `FS_SetPreferHint`→`Mod_ForName`→`FS_ClearPreferHint` wrapper on the main thread **clears the global `fs_preferhint` before the worker thread performs the locate** (proven by a debug print: `FS_SetPreferHint` resolved correctly, but the `FS_FLocateFile` preferred-pass never saw a non-empty hint). A later hinted load is also too late — the model is already `MLS_LOADING`, so it just waits on the unhinted one. **Fix:** wrap the **index-1 pre-load** with the hint and use **`MLV_SILENTSYNC`** (blocks via `COM_WorkerPartialSync` until the loader thread finishes), so the global hint stays set across the whole worker locate; clear immediately after. The lower hinted load is kept as the **no-`WG_LOADER`-workers fallback** (then it's the worldmodel's first + only load). Since index 1 is the first model in the list, no other model load is in flight during its `SILENTSYNC` window, so the hint can't bleed onto sibling models; it *does* (correctly) cover the worldmodel's own embedded `materials/*`/`vmt/*` deps, which should come from the map's game.

**Timing (why the key is present in time):** serverinfo is sent in `PRESPAWN_SERVERINFO` (prespawn stage ~2) and processed **inline** on the client (`CL_ParseStuffCmd` → `InfoBuf_FromString(&cl.serverinfo,…)`, sets `cl.haveserverinfo`); the modellist is a **later** stage (`PRESPAWN_MODELLIST` ~6). Reliable, in-order delivery + inline processing ⇒ `cl.serverinfo["*mappref"]` is populated before the worldmodel loads. (A workflow adversarial-review lens flagged a theoretical packet-reorder race; ruled out — reliable messages are not reordered, and the stages are strictly sequential.)

**Build/deploy:** engine `m-rel` (client: cl_parse.c) + `sv-rel` (server: sv_init.c); `fs.c`/`sv_init.c` compile into both. Plugins **unchanged** (the checksum symmetry means no plugin rebuild). Deploy `fteqw64.exe` + `fteqwsv64.exe`.

**Verify (done):** dedicated server `+mapfrom css cs_assault`; a connecting client loads `maps/cs_assault.bsp` from the **CS:S** searchpath (the preferred-pass matches `…/Counter-Strike Source/cstrike` + its vpk, rejects `…/Half-Life/cstrike`), **no map-check kick** (was an instant kick→retry loop before), and proceeds to content download. Regression: a plain Q1 `2fort` connect is unaffected (empty `*mappref` ⇒ `FS_SetPreferHint` not called ⇒ byte-identical to before). The single-machine self-connect still stalls later on the csprogs self-download (harness artifact, see Patch 30) — but the kick is gone, which is the fix.

---

## Patch 33 — CoD map "hang on load": each submodel re-registered ALL the map's shaders  *(APPLIED, cod plugin)*

**Symptom:** connecting to a dedicated server hosting a **CoD1** map (e.g. `+cod dam`) — the client shows the map loading screen, then **hangs** for ~2-3 minutes (the dedicated server `times out` the still-loading client). CoD single-player maps like `dam` have ~150 inline brushmodel submodels (doors / script brushmodels / triggers).

**Root cause (O(textures × submodels) shader registration):** in [codbsp.c](C:\msys64\home\Lex\fteqw\plugins\cod\codbsp.c) `CODBSP_LoadInlineModels`, the per-model material pass is dispatched with a **hardcoded `0`** as the inline-model index:
```c
threadfuncs->AddWork(WG_MAIN, CODBSP_GenerateMaterials, mod, bd, 0, 0);   // <-- bug: 0, should be i
```
`CODBSP_GenerateMaterials` uses that index to gate the "register every one of the map's shaders" loop to the worldmodel only — `if (!a) for(a=0;a<mod->numtextures;a++) RegisterBasicShader(...)` — with the comment *"submodels share textures, so only do this if 'a' is 0 (inline index, 0 = world)."* But because the dispatch always passed `0`, `if (!a)` was true for **every** submodel; and since each submodel is `*mod = *wmod` (so `mod->numtextures` = the **whole map's** texture count), all ~150 submodels re-registered the **entire** shader set. For `dam` that's hundreds of shaders × ~150 submodels ≈ tens of thousands of `RegisterBasicShader` calls ≈ ~0.4s/submodel ≈ a minute-plus of redundant work on the main thread while the connect is blocked → the "hang". (The hl2/VBSP plugin gets this right — `VBSP_LoadModel` passes the real index `i` to `VBSP_GenerateMaterials`.)

**Fix (one char):** pass the loop index `i` instead of `0`, so only the worldmodel (`i==0`) registers shaders and the ~150 submodels skip it:
```c
threadfuncs->AddWork(WG_MAIN, CODBSP_GenerateMaterials, mod, bd, i, 0);
```

**Diagnosis path (kept as a lesson):** the client spun at 100% CPU with steadily growing RAM and a frozen log → instrumented `CODBSP_LoadInlineModels`/`CODBSP_BuildBIH` with sparse prints (every Con_Printf from a loader thread itself queues `WG_MAIN` work, so dense prints distort the timing — keep them sparse). Trace showed the worldmodel BIH is *instant* (36270 leaves), every submodel BIH is instant (1 leaf), but ~0.4s elapsed *between* submodels. Timed the engine per-model callbacks (`Surf_BuildModelLightmaps`, `P_LoadedModel` via `Sys_DoubleTime`) — both fast — leaving the plugin's material pass as the only remaining per-submodel `WG_MAIN` work, where the hardcoded-index bug was. gdb is unusable on this box (attach → exit 57; `gdb.exe` itself fails `0xC0000139` entrypoint-not-found), so all of this was print/timer instrumentation, since removed.

**Verify (done):** `+cod dam` then connect — the inline-model load drops from ~150s (hang, client timed out) to **submodels processing in <1s** (worldmodel shader pass ~10s one-time), RAM **plateaus** at ~1GB (same as any map on this DIAG csprogs build — NOT a CoD leak; a plain `2fort` connect hits the same ~1GB) instead of growing for minutes. Build: cod plugin only (`release/fteplug_cod_x64.dll` via the BASE_CFLAGS-extract method, deploy to `C:\FTEQuake`). The single-machine self-connect still can't confirm the final spawn (csprogs self-connect harness artifact, Patch 30) — but the load-time hang, which is the CoD-specific bug, is fixed.

---

## Patch 34 — connect-time see-through water + install polish (flushshaders, conhistory, ODE static)  *(APPLIED, engine + cod-unrelated)*

Four related fixes from a polish pass. The headline one:

### 34a — water renders see-through on a CONNECT until a manual cvar nudge  *(engine, client)*
**Symptom:** connecting to a server (esp. Source/CoD maps), water surfaces show their geometric ripple/warp but no opaque texture — you see straight through to the world below. On a LISTEN server the same map's water is fine. Changing `r_wateralpha` to ANY value (even 0), or typing the (now-real) `flushshaders`, makes it "pop in" correctly.

**Root cause:** `Shader_DoReload` ([gl_shader.c](C:\msys64\home\Lex\fteqw\engine\gl\gl_shader.c) ~8467) early-returns while `cls.state < ca_active` (`if (cls.state && cls.state < ca_active) return;`). On a CONNECT the client sits at `ca_onserver` while it loads — so the water shader, built during that phase, is finalized in its stale/fallback state and every `Shader_DoReload` (per-frame, `Surf_NewMap`, the mod's CSQC `flushshaders` in `CSQC_WorldLoaded`) is a no-op. A LISTEN server skips `ca_onserver` (straight to `ca_active`), so its reloads run and the water is correct. A `CVAR_SHADERSYSTEM` change (`r_wateralpha`, [cvar.c:996](C:\msys64\home\Lex\fteqw\engine\common\cvar.c#L996)) → `Shader_NeedReload(false)` → next-frame `Shader_DoReload` → `Shader_Regenerate`→`Shader_DefaultBSPWater` rebuilds it — which is why the manual nudge works, but only because the user does it AFTER the first frame (post-`ca_active`).

**Fix:** a one-shot in [cl_main.c](C:\msys64\home\Lex\fteqw\engine\client\cl_main.c) `CL_Frame`, right after `SCR_UpdateScreen()` draws a frame: when `cls.state==ca_active`, the worldmodel is `MLS_LOADED`, and we haven't already fired for this map, call `Shader_NeedReload(false)` once and record `cl.servercount`. The next frame's `Shader_DoReload` (top of `GLSCR_UpdateScreen`, before the world is drawn) rebuilds the water → corrected from that frame on (≤1 frame of stale water, imperceptible). The marker `cls.shader_reload_servercount` lives in `client_static_t` (persists across the per-signon `cl` wipe), is `-1`-initialized in `CL_Init`, reset in `CL_Disconnect` (so a reconnect to the same unchanged map re-fires), and re-arms automatically per map since `cl.servercount` is fresh per signon. One harmless extra reload per map on listen/demo (one-shot, not per-frame). Found via a multi-agent investigation; the adversarial pass confirmed timing (fires after the shader is built) and edge cases. **VERIFIED by the user.**

### 34b — real `flushshaders` command  *(engine, renderer.c)*
The mod's CSQC runs `flushshaders` on map load (client/cl_sprays.qc, to re-parse freshly-written custom-spray shader files) — but it was never an engine command (`Unknown command "flushshaders"`, a silent no-op). Added `R_FlushShaders_f` → `Shader_NeedReload(true)` and registered it in `Renderer_Init` (next to `vid_reload`). Fixes spray refresh; was also the first (insufficient, mistimed) attempt at the water fix — see 34a for why a map-load-time flush is too early.

### 34c — conhistory in the gamedir, no stray root file  *(engine, console.c + fs.c)*
`conhistory.txt` was written to the install ROOT (`FS_ROOT`); changed `Con_History_Load`/`Con_History_Save` ([console.c](C:\msys64\home\Lex\fteqw\engine\client\console.c) 553/590) to `FS_GAMEONLY` so it lives in `nettest/`. The 0-byte one that kept reappearing in the root was the home-vs-base **writability probe** ([fs.c](C:\msys64\home\Lex\fteqw\engine\common\fs.c) ~8383) which opened `conhistory.txt` in append mode (creating it); changed it to probe a throwaway `.fte_writeprobe` and `Sys_remove` it. Root stays clean.

### 34d — ODE plugin fully static (no loose runtime DLLs)  *(plugins/Makefile)*
The install shipped `libwinpthread-1.dll` (+ unused `libgcc_s_seh-1.dll`, `libstdc++-6.dll`) because `fteplug_ode_x64.dll` linked winpthread dynamically (`-lpthread`). Added `-static` to the ODE link recipe ([plugins/Makefile:265](C:\msys64\home\Lex\fteqw\plugins\Makefile)); objdump confirms the rebuilt DLL imports NONE of the three. All three runtime DLLs can be deleted from `C:\FTEQuake`. (Force-relink note: `make -B` rebuilds ODE from a tarball that's gone — instead `touch` a source like `engine/common/com_phys_ode.c` to relink only the DLL.)

**Build/deploy:** engine `m-rel` (client: cl_main.c/client.h/renderer.c/console.c/fs.c) + `sv-rel` (fs.c shared) + the ODE plugin. Deploy `fteqw64.exe`, `fteqwsv64.exe`, `fteplug_ode_x64.dll`. **Adding a field to client_static_t is ABI-safe (appended at the end) since cls is defined in cl_main.c which is recompiled.**

---

## Patch 35 — CoD `checkpvs` NULL-deref crash (CODBSP_ClusterPVS missing the NULL-buffer fallback)  *(APPLIED, cod plugin)*

**Symptom:** calling the `checkpvs(viewpos, ent)` builtin on a **CoD (IBSP) map** can crash (NULL-pointer segfault) when the viewpoint lands in a clusterless leaf — i.e. the eye is in the void / clipped into solid (spectator/noclip, camera pushed into a wall, a brief spawn frame). The other four BSP families the mod loads (Q1, Q2, Q3, HL2/VBSP) never crash here. This is a **latent crash in the existing `cl_bulletholes_pvs_check` feature** (default **on**, client/cl_bulletholes.qc) on CoD maps, independent of any new work.

**Root cause:** `PF_checkpvs` ([engine/common/pr_bgcmd.c](C:\msys64\home\Lex\fteqw\engine\common\pr_bgcmd.c) ~1406-1427) always passes `pvsbuffer = NULL` to `worldmodel->funcs.ClusterPVS(cluster, NULL, PVM_FAST)`, then dereferences the result in `EdictInFatPVS`. Every BSP format guards the NULL-buffer case with an `if (!buffer) buffer = &staticrow;` fallback so it never returns NULL — **except CoD.** `CODBSP_ClusterPVS` ([plugins/cod/codbsp.c](C:\msys64\home\Lex\fteqw\plugins\cod\codbsp.c) ~233) returned the packed pvs pointer only for a valid cluster; for an out-of-range/`-1` cluster (which `CODBSP_ClusterForPoint` can legitimately return for a void point) the `if (pvsbuffer)` fallback was skipped (buffer is NULL) and it hit `return NULL` (codbsp.c:262). `CODBSP_EdictInFatPVS` then derefs `pvs[l>>3]` (codbsp.c:479) → crash. CoD was the only one of the five missing the guard. The engine's own fail-safe (`!funcs.FatPVS → visible`) doesn't fire because the CoD plugin *does* register PVS funcs.

**Fix:** add the same fallback the other formats already have — a file-static `pvsbuffer_t codpvsrow;` and, at the top of `CODBSP_ClusterPVS`, `if (!pvsbuffer) pvsbuffer = &codpvsrow;`. Now an invalid/`-1` cluster falls through to the existing `if (pvsbuffer)` branch (realloc + zero) and returns a valid all-zero row (= "nothing visible", cull-in-void — same benign behaviour as VBSP/Q2/Q3) instead of NULL. The valid-cluster `PVM_FAST` path (what the renderer's per-frame scene cull uses) is **unchanged** — it still returns the direct pointer, so normal CoD rendering is unaffected. Mirrors [mod_vbsp.c:3039](C:\msys64\home\Lex\fteqw\plugins\hl2\mod_vbsp.c) / gl_q2bsp.c exactly.

**Why it was done:** a multi-agent investigation of `checkpvs` cross-format safety (before adding an opt-in PVS-based dropped-weapon cull, `r_physdrops_pvscull`, in the mod's CSQC) verified the fail-safe path of all five formats against source and found CoD to be the sole CRASHES verdict. This one-line guard makes `checkpvs` universally crash-safe, which (a) lets the new QC `r_physdrops_pvscull` run safely on any map, and (b) retroactively de-risks the existing `cl_bulletholes_pvs_check` on CoD maps.

**Build/deploy:** rebuild the cod plugin (`make -C plugins release/fteplug_cod_x64.dll FTE_TARGET=win64 CC=gcc OUT_DIR=release BASE_CFLAGS="…"`); the final `EMBEDMETA` step needs `zip` (absent here) and is non-fatal — the linked DLL before it is complete and the deployed DLL never carried the metazip anyway. Deploy `fteplug_cod_x64.dll`. Plugin compiles clean (pre-existing indentation warnings only) and autoloads without ABI issue. The `checkpvs`-NULL path itself is hard to stage headlessly, so the crash-fix is code-verified (a faithful copy of the VBSP pattern); normal CoD map load/render is unaffected by construction.

---

## Patch 36 — accurate 1:1 player hit detection on IQM models (engine spine bend + native per-bone hitboxes)  *(APPLIED, engine + QC)*

**Symptom:** player hit detection on the IQM player models was inaccurate and inconsistent between `sv_trust_clienthits` 0 (server-authoritative, lag-comped) and 1 (client-authoritative). The up/down view-pitch **lean** made you miss leaning targets; hitgroups were wrong (a hand raised to head height read as a *headshot*, "both legs read as left leg"); and the two trust modes disagreed. The goal: exact, forgiving, CS-style per-bone hitboxes that match the rendered silhouette **identically on both sides, regardless of trust setting.**

**Root cause (the saga):** the player models are **IQM** (`mod_alias`). (1) The third-person view-pitch lean + strafe twist were applied **only on the client**, as a QC `skel_set_bone` deform on the visual proxy, while the **server collision pose was always upright** — so server traces (and the client's own trace) never matched the leaning render. (2) FTE **cannot trace per-bone hitboxes on IQM at all** — `MOVE_HITMODEL` traces mesh triangles; only Half-Life `.mdl` (`HLMDL_Trace`) had engine hitboxes — so "use only hitboxes" was impossible in QC, and the gamecode fell back to a Z-height guess. Both required **engine** changes.

This is two engine features plus a chain of trust-1 client fixes. All engine edits in [common/com_mesh.c](C:\msys64\home\Lex\fteqw\engine\common\com_mesh.c) (+ `.h`), tagged `//nettest`.

### Part A — engine-native view-pitch/twist spine bend (so the SERVER pose leans)
- **`Alias_ApplySpineBend()`** (com_mesh.c): a bit-for-bit C port of the client QC `PlayerVis_LoadBoneBasis`/`TwistBone`/`PitchBone` (cl_player.qc). Rotates the named spine bones (`Bip01 Spine/Spine1/Spine2/Spine3/Neck`) in **parent-relative** (`SKEL_RELATIVE`) space — extract `(fwd,right,up)` via the engine bone↔qcvector convention (col0=fwd, col1=**−**right, col2=up; see pr_skelobj.c `bonemat_toqcvectors`), orthonormalize, twist (rot right/up about fwd) then pitch (rot fwd/right about up), same per-bone fractions, write back. Driven by `framestate->g[FS_REG].subblend2frac` (= v_angle_x/90, pitch) and `.subblendfrac` (= leg_twist/90, twist) — values the gamecode **already** set on both sides and that **lag-comp already rewinds** (sv_lagcomp.qc), so no new fields/networking and lag-comp is automatic. Bone indices cached on `galiasinfo_t.spinebend_bone[5]`.
- **`Alias_GetBoneInformation` wrapper** (renames the original to `_Raw`): when `Alias_SpineBendActive()` is true, fetch the blended pose as `SKEL_RELATIVE` into a **private** copy (the raw fast-path can return shared static frame data — must memcpy before mutating), bend it, then convert to the requested type. Covers the render (`Alias_BuildSkeletalMesh`) and `Mod_Trace`. `Mod_GetTag` routes through the bent `SKEL_ABSOLUTE` pose too (it has its own bone walk).
- **Gated `r_skel_spinebend`** (CVARFD, default 1, registered in `Alias_Register`). **CRUCIAL:** `Alias_SpineBendActive` returns false when `framestate->bonestate` is set — i.e. when a **QC skeletal object** drives the entity. So the engine bend applies to the **SERVER** player (its skeleton is cleared each tick) but NOT the **client visual proxy** (which keeps its own QC `skel_build` skeleton + QC spine deform). Applying the engine bend on top of the proxy's QC skeleton double-bent it and visibly **warped the gun attachment** — this split avoids that. Tuning reads the existing `cl_player_spine_pitch_*`/`_twist_*` cvars via cached `Cvar_FindVar`.

### Part B — native per-bone hitbox collision for IQM (mirrors `HLMDL_Trace`)
- **`galiasinfo_t`** gains `int numhitboxes; aliashitbox_t hitbox[MAX_ALIASHITBOXES(32)]` (`{int bone; int hitgroup; vec3 mins,maxs}`) — IQM has no native hitbox chunk.
- **`Mod_AddHitbox(model, bonename, hitgroup, mins, maxs)`**: resolves the bone name against the head `galiasinfo` and stores the box (idempotent on re-precache). Exposed to QC as the **`addmodelhitbox` builtin** (`PF_addmodelhitbox` in pr_skelobj.c — compiled into both progs like `PF_modelframecount`; prototype in pr_common.h; registered in the pr_cmds.c **and** pr_csqc.c builtin tables; name-bound `#0`).
- **`Mod_Trace_Hitbox()`**: a near-verbatim port of `HLMDL_Trace`'s 6-plane slab test, but in **model space** — the ray is already model-local (`start_l/end_l`) and the bones come from `Alias_GetBoneInformation(SKEL_ABSOLUTE)` (with the Part A bend / QC skeleton already applied), so no axis is baked into the bones. On hit it sets `trace->surface_id = box hitgroup` (HL's own convention) and `trace->bone_id = bone+1`. **Must zero `brush_face/bone_id/brush_id/surface_id` at function entry** (HL memsets the whole trace; `Mod_Trace` doesn't) or a MISS leaves stale fields → OOB `norm[]`/`bones[]` + a phantom hitgroup to QC.
- **`Mod_Trace` hook** (right after `trace->fraction = 1`): `if (mod->numhitboxes && framestate && (mod->contents & contentsmask)) return Mod_Trace_Hitbox(...)` — **replaces** the mesh trace for player models ("only hitboxes").
- **QC:** the 20-box `$hbox` table (shared/sh_weapons.qc) is registered per player model at precache via `Hitbox_RegisterModel(precache_model(...))` in `precache_everything` (both progs; no-ops on the sini `.mdl` and unknown bones, `checkbuiltin`-guarded). `W_HitgroupClassify` (sh_weapon_logic.qc) returns `trace_surface_id` directly when `bone_id>0 && surface_id∈1..7`, ahead of the legacy Z-height guess.

### THE trust-1 fix (single argument) + the client-prediction chain
With Parts A+B, **trust-0 worked perfectly** but **trust-1 (client) passed straight through players.** Causes, in order found:
1. **`MAX_BONES` vs `inf->numbones`** — `Mod_Trace_Hitbox`/`Mod_Trace` called `Alias_GetBoneInformation(..., MAX_BONES, ...)`, but a QC skeleton's `bonestate` is only honored when `framestate->bonecount >= numbones`. Player skeletons have ~50 bones; `50 >= 256` is false → **the engine silently discarded the posed skeleton and traced the bind pose** (offset from the render — a ray through the player's origin sailed past their boxes). The render asks for `inf->numbones` (~50 ≥ 50 ✓), which is why the model looked right while the trace missed. **Fix: `MAX_BONES` → `inf->numbones`/`mod->numbones`** in both trace calls (server no-skeleton path is unaffected — `Alias_FindRawSkelData` already clamps).
2. **Client proxy posed at render time but traced at prediction time** — the proxy's engine framestate is only written in predraw (render), which runs *after* the fire trace; with the QC skeleton re-enabled (Part A split) the persistent skeleton supplies a valid posed `bonestate` even a frame stale, so re-enabling it was what fixed the placement.
3. **Hitgroup clobber** — the FX between the trace and the hitclaim (`CSQC_BulletImpactSilent` re-traces for the impact decal) zero `trace_bone_id`/`trace_surface_id`, so `W_HitgroupClassify`/`CSQC_SendHitclaim`/`CSQC_TryPredictDeath`/`cl_debug_impacts` re-derived a Z-height guess (arm-at-head-height → **headshot**). **Fix:** capture the hitgroup at the instant of the hit and thread it through (`optional in_hitgroup` on `CSQC_SendHitclaim` + `CSQC_TryPredictDeath`). This also fixed a **phantom headshot kill** (client predicted a kill on an arm hit; victim alive on the server → "dead body you can re-shoot").
4. **Double damage flinch** — the attacker predicted the flinch AND the server multicast `CSQC_EVENT_PLAYER_FLINCH` re-stamped it ~RTT later, restarting the anim. **Fix:** client dedup latch `vis_flinch_pred_until` (cl_main.qc handler skips the re-stamp during the prediction window; witnesses still see it once).

**Why it was done:** the user wanted "perfect 1:1 hit groups regardless of trust settings… server-side skeleton AND accurate client-authoritative hitgroup, only if the engine supports it." Engine investigation confirmed it was feasible with no blockers. Verified in-game on a listen server: trust-0 and trust-1 both land box-accurate hits with correct hitgroups (arm-at-head-height reads as arm, not headshot) on every IQM player model (arctic/gign/gsg9/sas/guerilla/leet/terror/urban/chickenman/numberK/kilmer — each registers 20/20 boxes); the gun attaches correctly; the flinch fires once. Sini (`.mdl`, Half-Life) uses its own native `HLMDL_Trace` path and is unaffected by this work.

**Build/deploy:** rebuild the engine — `$env:TMP="C:\msys64\home\Lex\tmp"` (writable, **Windows-style** path or MinGW gcc fails "Cannot create temporary file in C:\WINDOWS"); ucrt64 PATH; `make -C C:\msys64\home\Lex\fteqw\engine m-rel FTE_TARGET=win64` → copy `release/fteqw64.exe` → `C:\FTEQuake`. Recompile QC: `fteqcc64_latest.exe sv_progs.src` + `cl_progs.src`. The `addmodelhitbox` registration is verified headlessly via the developer-gated `[hitbox] <model> idx=<n> registered N/20 boxes` print (all IQM players 20/20; sini 0/20 no-op).

**Follow-up polish (QC-only, after the core landed):**
- **Double damage flinch (shooter saw it twice):** the attacker predicts the flinch locally AND the server multicasts `CSQC_EVENT_PLAYER_FLINCH` to everyone. A client-side time-window latch failed at high ping (the window must beat RTT, impossible at `sv_minping 500`). Fixed timing-independently: the server tags the flinch event with the **attacker slot** (`num_for_edict(attacker)`, +1 wire byte, size 4→5) and the attacker's own client **skips** it (they already predicted it). Witnesses still see it once. Mirrors the footstep exclude pattern.
- **Hitgroup lost to FX clobber:** the impact-FX re-trace (`CSQC_BulletImpactSilent`) zeroed `trace_bone_id`/`trace_surface_id` before the hitclaim, so an arm-at-head-height Z-fell-back to HEAD — making the hitclaim mis-classify AND `CSQC_TryPredictDeath` predict a **phantom headshot kill** (victim alive on the server → "dead body you can re-shoot"). Fixed by capturing the hitgroup at the instant of the hit and threading it through (`optional in_hitgroup` on `CSQC_SendHitclaim` + `CSQC_TryPredictDeath`; also fixes the `cl_debug_impacts` label).
- **Debug overlays:** the drawn boxes were Y-mirrored ("backwards feet") because `gettaginfo` returns `v_right = -col1` while the engine trace uses raw `col1` — fixed by negating `v_right` in all three draws (`CSQC_DrawDebugHitboxes`/`CSQC_DrawServerHitboxes`/`CSQC_DrawContinuousHitboxes`). `sv_debug_hitboxes 1` (per-shot) snapshots the lag-comped pose; `2` (continuous, sv_main.qc) streams the live pose and the client extrapolates `frame1time` to animate — both correct once un-mirrored.
- **Wireframe draw — DON'T use `type beam` particles for it.** Briefly switched the per-edge draw from `R_BeginPolygon` to `trailparticles` beams; this looked like the upper-body boxes (head/arms) just stopped drawing. Root cause: FTE `type beam` segments come from a tiny **HARDCODED 2048-slot pool** (`MAX_BEAMSEGS`, `p_script.c:396` / `r_numbeams` `:3494`; `r_part_beams` is on/off only, NOT a size — separate from the 262144 `r_part_maxparticles` pool), and each box edge subdivides into `length/step` segments — so ~8 boxes (the lower body) filled 2048 and every box above the spine **silently dropped** (`free_beams==NULL` → break, `:5818`). It also shares that pool with weapon tracers/gauss/egon. **Reverted to `CSQC_DebugBeam` (immediate-mode `R_BeginPolygon` thin camera-facing quad per edge)** — no cap (32k verts/batch, ~12% at 4 targets), ~0.5ms for ~1000 edges, draws every frame so it tracks precisely. `particles/debug.cfg` deleted. (Investigated with a 4-agent workflow; the `v_right` feet fix + per-hitgroup colours were kept.)

---

## Patch 37 — `sv_allow_download_anything` (opt-in: let the server send copy-protected files + lift the per-type asset gates)  *(APPLIED, server-only)*

**Files:** `engine/server/sv_main.c` (2) + `engine/server/sv_user.c` (4)  ·  search `//nettest P37`

> **BUILD-AND-DEPLOY LESSON:** the dedicated server runs **`fteqwsv64.exe`** (`start_dedicated_server.bat`), which is the **`sv-rel`** target — NOT `fteqw64.exe` (`m-rel`). A server-side engine patch is NOT live until you **rebuild `sv-rel` AND redeploy `fteqwsv64.exe`** (and restart the server). After deploying only `m-rel`, the dedicated server still denied copy-protected `.wad`s — the `set sv_allow_download_anything 1` in ftesrv.cfg was a dead cvar in the stale `sv-rel` binary. Always rebuild **both** `m-rel` and `sv-rel` for server-side patches.

**Why:** the server denied clients downloading files that live inside a **copy-protected** mounted game folder, logging `<player> denied download of maps/de_dust2_css_2026.bsp - it is copyrighted` (also `models/skeleton.mdl`). The `mapfrom css/cs/hl/cod` addon mounts (Patch 8 / Steam dirs) are flagged `SPF_COPYPROTECTED`, and a **loose** file inside such a mount **inherits** that flag (`fs.c` ~3930/4202/4373). `SV_LocateDownload()` (`sv_user.c`) then hard-denies it. This fires **even with the correct installs** — the flag is about *where the file is mounted from*, not whether the player owns it. The loose-file branch (`else if (copyprotected)` → "it is copyrighted") had **no cvar override** (the pak branches at least honour `allow_download_pakmaps`/`allow_download_pakcontents` == 2).

**What it adds:** a server cvar **`sv_allow_download_anything`** (default `0` = byte-for-byte stock). When `1` it does **two** things so a client can pull *everything a mounted-game map needs* in one go: (a) `SV_LocateDownload` ignores `SPF_COPYPROTECTED` on the server's own searchpaths (loose *and* pak), and (b) `SV_AllowDownload` lifts the per-type asset gates (`allow_download_other`/`textures`/`wads`/etc.) so paths like `gfx/env/*` and `materials/skybox/*` aren't blocked. **Still NEVER** hands out **logs** or **configs** (`.cfg` / `config(s)/` — they leak the rcon password). Path/name safety (`..`, absolute, drive) is unchanged.

**The edits** (all tagged `//nettest P37`):
1. `sv_main.c` (~`:100`, after `allow_download_other`) — `cvar_t sv_allow_download_anything = CVARFD(... "0", CVAR_WARNONCHANGE, ...)`.
2. `sv_main.c` (~`:6012`) — `Cvar_Register(&sv_allow_download_anything, cvargroup_serverpermissions);`.
3. `sv_user.c` (~`:3471`) — added to the `extern cvar_t` list inside `SV_LocateDownload`.
4. `sv_user.c` (right after `copyprotected = …`, ~`:3630`) — `if (sv_allow_download_anything.ival) copyprotected = false;`. One line: both `else if (copyprotected)` branches fall through to "allowed" and the package-redirect path also works.
5. `sv_user.c` `SV_AllowDownload` (~`:3354`) — added to its `extern cvar_t` list.
6. `sv_user.c` `SV_AllowDownload` (right after the logs block, ~`:3388`) — `if (sv_allow_download_anything.ival) { if (cfg/config(s)) return false; return true; }`. Placed AFTER the logs check and BEFORE the per-type gates; blocks configs explicitly, allows every other asset. Without this, the skybox (`gfx/env/*`, `materials/skybox/*`) hit `allow_download_other` (default 0) and were denied "due to path/name rules" even with the copyright veto lifted.

**Mod side:** `C:\FTEQuake\nettest\ftesrv.cfg` sets `set sv_allow_download_anything 1` (dedicated server; `set` so it creates regardless of cvar-registration order). A **listen host** doesn't exec ftesrv.cfg — set it in console / autoexec there if needed. Stock engine without this patch: unknown cvar, denial stays.

**Verify:** with the cvar `0` the deny line still prints; with `1` it's gone and the transfer proceeds (maps, wads, skybox faces, misc assets). Server-only change (compiled into both `m-rel` and `sv-rel`).

---

## Patch 38 — `FS_GAMEDOWNLOADS`: route client downloads into `<gamedir>_downloads/`  *(APPLIED, client/common)*

**Files:** `engine/common/common.h` (1) + `engine/common/fs.c` (3) + `engine/client/cl_parse.c` (1)  ·  search `//nettest P38`

**Why:** the user wants downloaded content to land in a **sibling `nettest_downloads/` folder** instead of mixing into the active gamedir `nettest/`, "to keep the game files pure". The client previously wrote every loose download to `FS_PUBGAMEONLY` (the active gamedir). FTE already segregates *package* downloads (`package/` → `FS_ROOT`'s `downloads/`) and addon-game downloads (`<addongame>_downloads`, Patch 8's `FS_Addon_Mount`); this extends the same idea to the **active gamedir's loose downloads**.

**What it adds:** a new `fs_relative` value **`FS_GAMEDOWNLOADS`** that resolves to `<base-or-home>/<gamedir>_downloads/<file>` (sibling of the gamedir, derived from the `gamedirfile` global). Only the client download path uses it, so configs/screenshots/demos still write to `nettest/`. The folder is mounted as a normal read searchpath so the downloads load.

**The edits** (all tagged `//nettest P38`):
1. `common.h` (~`:690`) — enum member `FS_GAMEDOWNLOADS` after `FS_PUBBASEGAMEONLY` (sorts after `FS_GAME`, so `FS_RELATIVE_ISSPECIAL` stays false).
2. `fs.c` `FS_NativePath` (new `case`, ~`:2943`) — resolve to `<com_gamepath|com_homepath>/<gamedirfile>_downloads/<fname>` (mirrors `FS_GAMEONLY` + `_downloads`; returns false if no gamedir). `FS_SystemPath`/`FS_CreatePath`/`FS_Rename` all route through this.
3. `fs.c` `FS_OpenVFS` (~`:3154`) — add `case FS_GAMEDOWNLOADS:` to the shared `FS_PUBGAMEONLY`/`FS_BASEGAMEONLY`/`FS_PUBBASEGAMEONLY` block (FS_SystemPath + `COM_CreatePath` on write + `VFSOS_Open`). **Required** — that switch's `default:` is `Sys_Error`.
4. `cl_parse.c` `DL_Begun` (~`:2064`) — the final `else` fsroot `FS_PUBGAMEONLY` → `FS_GAMEDOWNLOADS`. `package/`(FS_ROOT) and `skins/`(FS_PUBBASEGAMEONLY) branches unchanged. The temp→final rename (`:2647`) uses the same fsroot both ends → same-dir rename.
5. `fs.c` `FS_ReloadPackFilesFlags` (~`:5503`, just before `FS_AddDownloadManifestPackages`) — mount `<gamedirfile>_downloads` as a searchpath via the **low-level** `FS_GetOldPath`/`VFSOS_OpenPath` + `FS_AddPathHandle` (NOT `FS_AddSingleGameDirectory`/`FS_AddGameDirectory`, which would clobber `gamedirfile`/`pubgamedirfile`/`gameonly_gamedir`). Base follows `com_homepathenabled` to match the write path. Flags **`SPF_ADDON|SPF_ISDIR`** — `SPF_ADDON` appends it at the **tail (lowest priority, BELOW the mod)**, mirroring the `cstrike_downloads` precedent in `FS_Addon_Mount`, so the mod's own files always win and a stale download can never shadow a mod asset; an addon path is also never a write target (so it can't steal config/general writes), and `FS_FileIsAddonOnly` correctly skips auto-exec of any downloaded `config.cfg`/`autoexec.cfg`. **NOT** `SPF_COPYPROTECTED` (our own files, keep them re-servable — the download-out gate keys on `SPF_COPYPROTECTED`, and the package-advertisement filter that *does* skip `SPF_ADDON` is CRC/package-only, irrelevant to loose files) / **NOT** `SPF_TEMPORARY` (would be purged at map change). `VFSOS_OpenPath` tolerates a not-yet-existing dir, so the first download is found without a remount.

**Result:** downloads land in `C:\FTEQuake\nettest_downloads\…`, load via `FS_FLocateFile` like gamedir files at LOW priority (so `nettest\` always wins on a name collision — a stale download can't shadow a mod update), and `nettest\` stays pure. On a stock engine without this patch downloads go to the gamedir as before. Pairs with Patch 37 (server sends, client files them away).

**Verified (headless boot, `-condebug +path`):** `path` lists `$basedir/nettest_downloads` BELOW `$basedir/nettest (e)(w)` and ABOVE the addon games; `nettest_downloads` is read-only (no `(w)`) and not `(c)` copyprotected; the `sv_allow_download_anything` cvar registers (default `0`); no FS/`Sys_Error`. End-to-end download test (real client↔server) is the user's: trigger a download → file appears under `nettest_downloads/` (not `nettest/`), `.tmp` renamed cleanly, `map <name>` renders it; `nettest/` did not gain the file.

---

## Patch 39 — auto-download a mounted-game map's skybox faces  *(APPLIED, client-only)*

**Files:** `engine/client/cl_parse.c` (1 function)  ·  search `//nettest P39`

**Why:** a client without the mounted game (CS/HL/Source) downloads the map + its wads fine (the wad list is already requested by `CL_CheckHLBspWads`), but the **skybox faces are never requested** — so the sky renders black with `Sky "skybox_<name>" missing texture: materials/skybox/<name>_rt` (the `Con_DPrintf` at `gl_shader.c:733`, the last fallback path the sky loader `Shader_ParseSkySides` tries). Nothing in the engine auto-downloads missing textures (no download calls in `image.c`), and the Q2 path even has a `FIXME: parse entity lump for sky name`. So the skybox needs an explicit download request, like the wads.

**What it adds:** `CL_CheckHLBspWads` (cl_parse.c) now also parses the worldspawn **`skyname`** (or Q1 `sky`) and, for each of the 6 faces, requests the two common conventions — GoldSrc `gfx/env/<name><side>.tga` and Source `materials/skybox/<name><side>.vtf` (sides `rt/bk/lf/ft/up/dn`) — with `DLLF_REQUIRED` so they arrive before the sky shader builds. The server serves whichever exists (it must permit them: the SV_AllowDownload bypass in **Patch 37**); the rest resolve as not-found (no hang — same as a missing wad). They land in `nettest_downloads/` (**Patch 38**) and the sky loader's existing patterns (`gfx/env/%s%s`, `materials/skybox/%s%s` — its own nettest skybox patch) then find them.

**The edit** (tagged `//nettest P39`): one rewrite of `CL_CheckHLBspWads`. **Critical detail:** the original used the *outer* entity-lump cursor `s` as its inner wad-list parser (`s = wads; while((s = COM_ParseToken(s,";")))`) and therefore `return`ed immediately after the `wad` key (the cursor was destroyed). To also reach `skyname` (which may appear before OR after `wad`), the inner loop now uses a **local cursor** `char *p = wads` so the outer `s` survives, the loop runs to `}`, then the skybox requests fire after it. Wad handling (request `textures/<wad>` `DLLF_REQUIRED`) is byte-for-byte unchanged; return value still `false`.

**Scope:** GoldSrc (`BSPVERSIONHL`) maps — the path that has the worldspawn wad+sky. Source/VBSP maps load via the plugin, not `CL_CheckHLBspWads`; if a VBSP map needs the same, mirror this in its loader. Requesting both conventions means ~6 of the 12 requests not-found on any given map (the wrong convention) — cheap and silent. Stock engine: no skybox download (black sky for clients lacking the game).

**Verify:** connect a client lacking the game to a server hosting a GoldSrc map with a skyname → the 6 face files appear under `nettest_downloads/gfx/env/` (or `nettest_downloads/materials/skybox/`) and the sky renders instead of black. Needs Patch 37 active on the server (incl. the `sv-rel` rebuild) so `gfx/env/*` / `materials/skybox/*` aren't denied.

---

## Patch 40 — in-game console: drop the spurious tab-complete `/` + arrow-navigable completion  *(APPLIED, client-only)*

**Files:** `engine/client/keys.c` (`CompleteCommand` + the K_UPARROW/K_DOWNARROW handlers)  ·  search `//nettest`

**Why:** (a) pressing Tab to complete inserted a leading `/` you didn't type (`r_drawviewmodel` → `/r_drawviewmodel`). Root cause: `CompleteCommand` did `if (cl_chatmode.ival) Key_ConsoleInsert("/")` unconditionally at **two** sites; `cl_chatmode` defaults to 2 so every completion prepended `/`. (b) FTE already renders the match list (highlighted via the global `con_commandmatch`) and **Tab/Shift-Tab cycle it**, but Up/Down only did command history — no Source-style arrow selection of the dropdown.

**What it does:** (a) capture whether the user's line **actually started** with `/`/`\\` (`qboolean had_slash` at the existing slash-skip in `CompleteCommand`) and gate both inserts: `if (cl_chatmode.ival && had_slash)`. Now bare→bare, `/cmd`→keeps the `/`; execution is unchanged (chatmode-2 runs bare recognised commands; the `/` was a redundant disambiguator). (b) at the top of the K_UPARROW/K_DOWNARROW handlers, if a completion is active (`con_commandmatch != 0` **and** `Cmd_Complete(line)->num > 1`), the arrow runs `CompleteCommand(false, ∓1)` (reusing the exact Tab-cycle path) and returns — i.e. **Tab opens the list, Up/Down move the highlight, Enter accepts** (Enter already honours `con_commandmatch`); otherwise it falls through to history as before. No new rendering — the highlighted list + inline greyed preview already exist.

**Scope:** client console only. Stock engine: the `/` reappears and Up/Down are history-only. A full *as-you-type* dropdown (vs Tab-driven) is deferred.

---

## Patch 41 — dedicated-server terminal: readline-style line editing  *(APPLIED, server-only / `sv-rel`)*

**Files:** `engine/server/sv_sys_win.c` (`Sys_ConsoleInput` + `Sys_Printf`)  ·  search `//nettest`

**Why:** the dedicated server's Windows cmd console (`fteqwsv64.exe`) had no line editing. `Sys_ConsoleInput` was a raw `_kbhit()`/`_getch()` loop handling only Enter/Backspace/Tab/printable; **arrow keys arrive as a `0x00`/`0xE0` prefix + a scan byte, both echoed verbatim → the `àKàHàP` garbage**, plus no history, no cursor movement (append-at-end only), so copy-paste-then-edit corrupted the line.

**What it adds (all in sv_sys_win.c, no threading — keeps the non-blocking `_getch` poll):**
- A **caret** (`coninput_cursor`) + a **command-history ring** (`SVCON_HIST 32`) with two static helpers `SVCon_SetLine` (rub-out + reprint a line) and `SVCon_HistAdd` (push on Enter, skip blanks/dupes).
- **Extended-key dispatch:** on `c==0||c==0xE0`, read the scan byte — **Left/Right** caret move, **Up/Down** history, **Home/End**, **Delete** — and **swallow any other extended key** (this alone kills the garbage echo). Other control chars (`c<32`) are swallowed too.
- **Insert/delete at the caret** (printable + Backspace + Delete) with proper tail-redraw (reprint the tail, trailing space to clear the old glyph, backspace back to the caret). Tab-complete sets caret to end.
- **`Sys_Printf` cursor-awareness:** before its existing erase it advances the terminal caret to end, and after reprinting `]<input>` it backspaces `(len - cursor)` to restore a mid-line caret. (No-op when nothing is being typed, so normal logging is unchanged.)

**Build/deploy:** server file → **rebuild `sv-rel` + redeploy `fteqwsv64.exe`** (and restart the server) — see [[engine-build-and-patches]]; the in-game console (Patch 40) is `m-rel`/`fteqw64.exe`. **Verify (interactive):** in the cmd window, Left/Right move the caret, Up/Down recall history, Home/End/Delete work, arrow keys no longer print `àK…`, and a log line printed mid-edit doesn't corrupt the input line. Stock engine: raw-mode garbage as before.

---

## Patch 42 — in-game console: cursor/completion fixes + Ctrl+L / Ctrl+W / cvar-default / Ctrl+F search  *(APPLIED, client-only / `m-rel`)*

**Files:** `engine/client/keys.c` + `engine/client/console.c` (+ 1 decl in `common/console.h`)  ·  search `//nettest`

Follow-up to Patch 40. Three bug fixes + four niceties, all in the in-game console. Client-only → `m-rel`/`fteqw64.exe`.

**Bug fixes:**
- **Right-arrow inserted spaces** — in the shared text-entry handler `Key_EntryLine`, the K_RIGHTARROW case at end-of-line fell to `else unicode = ' ';` (keys.c:1654) → the space got inserted by the printable-char path. Changed to `else return true;` (Right-at-EOL is a no-op everywhere, standard).
- **Spurious trailing space after a sole-match completion** ("invisible spacer" that blocked Left-arrow re-completion) — removed the `if (c->num == 1) Key_ConsoleInsert(" ");` in `CompleteCommand`.
- **`clear` while scrolled up stayed stuck** — `Con_ClearCon` reset `display`/`current` but not `con->displayscroll`; the stale value failed the auto-scroll-to-bottom gate in `Con_PrintCon` (`displayscroll==0`), so new prints didn't pin to the bottom + the `^^^^` indicator persisted. Added `con->displayscroll = 0;` in `Con_ClearCon` (console.c).

**Niceties (all in `Key_Console`/`Key_EntryLine` unless noted):**
- **Ctrl+L** = clear the console (`Con_ClearCon(con)`).
- **Ctrl+W** = delete the word before the caret (readline-style; reuses the existing Ctrl+←/utf_left word-walk).
- **Cvar value + default on Tab** — `Key_UpdateCompletionDesc` already showed the cvar's current value in the footer; added its `var->defaultstr` so the footer reads `name value (default X)`.
- **Ctrl+F search-in-scrollback** — new `Con_SearchText(con, text, dir)` (console.c, declared in console.h) walks `conline_t` from `con->display` in `dir`, decodes each line via `CON_CHARMASK`, case-insensitive substring match; on a hit it scrolls (`con->display = line`) and selects the span (`selstartline/offset` + `CONF_KEEPSELECTION`) so the existing selection-draw highlights it. `Key_Console` adds a `con_findmode` + `con_findtext[64]`: **Ctrl+F toggles** the `find:` bar (footer) on/off; while open, typing jumps to the newest match, **Enter/F3** = next older, **Shift+Enter** = newer, **Backspace** edits. **Esc closes the find bar** — but Escape is consumed in `Key_Event` (keys.c ~:3090, just before it removes `kdm_console`) *before* the per-console `Key_Console` dispatch, so the find-mode exit had to be hooked THERE, not in `Key_Console` (an in-`Key_Console` Esc handler is dead for Escape). Ctrl+F-toggle gives a second guaranteed exit.

**Follow-ups (same patch, later):**
- **Plain Home/End scroll the console** — were Ctrl+Home/Ctrl+End only (input-cursor home/end fell through to `Key_EntryLine`). Dropped the `&& ctrl` on the two handlers in `Key_Console` (keys.c ~:2096) so plain **Home → top** (`con->display = con->oldest`), **End → bottom** (`= con->current`); they now always `return true` (consume the key before the input-cursor handler runs). Word-jump (Ctrl+←/→) still reaches the input line.
- **Scrollbar on the right of the floating console** (`console.c` `Con_DrawConsole` `CONF_ISWINDOW` path): narrowed the content (scissor + `Con_DrawOneConsole` width `wnd_w-16` → `wnd_w-24`) to free an 8px strip on the right, and draw a track + thumb there (`R2D_FillBlock`/`SRGBA`). The thumb height = `visible/total` lines, position from counting `oldest→display`. **No new mouse code** — that strip is already the existing `CB_SCROLL` drag region (keys.c ~:1998), so dragging it scrolls and the thumb tracks `con->display`. (Window console only; the non-window drop-down is unchanged.)
  - **CRASH FIX (boot SIGSEGV):** the thumb-height math first used `Font_CharHeight()` — which returns the **global `curfont->charheight`** (gl_font.c:2880), and `curfont` is **NULL** at this point in `Con_DrawConsole` (it's only bound later, inside `Con_DrawOneConsole` via `Font_BeginString`). With `con_window 1` the console window draws on the first frame → NULL deref → `0xC0000005` on launch. Fixed by using the **explicit-font** `Font_CharVHeight(font_console)` (takes the font as an arg, no `curfont`). LESSON: in any draw code OUTSIDE a `Font_BeginString`/`Font_EndString` pair, never call the argless `Font_CharHeight()`/`Font_CharWidth()` — use the `Font_Char*Height(struct font_s*)` variants. (Also: a `+quit` headless smoke-test renders no frame, so it misses console-draw crashes — verify with a few seconds of real rendering.)
  - **FOLLOW-UP FIXES (after first use):** (1) the window content + my scrollbar draw *outside* the `if (Key_Dest_Has(kdm_cwindows))` focused-block (closes at console.c ~:3296), so a **closed** console kept drawing the scrollbar + the `^^^^` backscroll indicator (faded). Fixed: in the `else` (unfocused) branch, snap `w->display = w->current` + `displayscroll = 0` (kills the `^^^^` AND scrolls back to the bottom so it reopens at the live end — the user's "scroll back down on close" ask), and gate the scrollbar draw on `Key_Dest_Has(kdm_cwindows)` so it only shows when focused. (2) The bar reused the existing `CB_SCROLL` drag, which is a *relative content-grab* (drag down = pull content down = scroll UP) — wrong for a scrollbar. Added **`CB_SCROLLBAR`** (console.h enum): the window-scrollbar strip now sets `CB_SCROLLBAR` on mouse-down (keys.c hit-test) and `Key_GetConsoleSelectionBox` handles it as an **absolute** map — mouse-Y in the track → `con->display` position (top=oldest, bottom=newest), so dragging DOWN scrolls DOWN and the thumb follows the cursor. Release is the catch-all `CB_NONE` (keys.c ~:1288), no special handling.

**Build/deploy:** client-only → `make m-rel FTE_TARGET=win64` → `fteqw64.exe`. **Verify (interactive):** Right at EOL inserts nothing; complete a unique command → no trailing space + Left re-triggers completion; scroll up + `clear` + type → view snaps to bottom, no `^^^^`; Ctrl+L clears; Ctrl+W deletes a word; Tab a cvar → footer shows value + default; Ctrl+F + term → jumps to/highlights matches, Enter/Shift+Enter walk them, Esc closes; **Home/End jump to top/bottom**; a **scrollbar** shows on the right of the `con_window` console + drags. Stock engine: none of these.

---

## Patch 43 — IQM frame-blend bone renormalize (player-model "warp" fix)  *(APPLIED, client+server / `m-rel` + `sv-rel`)*

**File:** `engine/common/com_mesh.c` (`Alias_BlendBoneData` + new `Alias_RenormalizeBoneMatrix` + cvar `r_skel_blendnormalize`)  ·  search `//nettest warp fix`

**Why:** IQM player/bot models warped intermittently — torsos/arms (and the attached gun) scaled up massively for a frame. A QC detector (`PlayerVis_CheckWarp`) showed `Bip01 Head` and the root `Bip01` reporting an **inf** absolute bone scale on every player, plus the gun's clavicle/flash/Line02 bones at 15–26× during firing. Root cause: `Alias_BlendBoneData` blends animation frames with a **linear weighted sum of 3×4 matrices** (`pose = Σ frac·matrix`). A linear blend of rotation matrices is no longer orthonormal — the basis shrinks/skews — and on a **SKEL_RELATIVE** skeleton that per-bone scale **compounds geometrically down the parent chain** (root → pelvis → spine → neck → head), overflowing the deepest bones to a giant/`inf` absolute scale. The same upper-body scale fed the QC weapon-mirror (`gettaginfo` → `skel_set_bone_world`) and bent the gun. Render, `gettaginfo`, and `skel_get_boneabs` all use the same chain-walk, so the warp is real, not a measurement artifact.

**Fix:** after each **blended** bone (the single-frame `frac==1` path is an exact `memcpy`, untouched), Gram-Schmidt the 3×3 column basis back to a clean rotation — normalize col0, orthogonalize col1, `col2 = col0×col1` with the **original handedness preserved** (so it never mirrors), translation column untouched. Gated to `skeltype == SKEL_RELATIVE` (where compounding happens) and the new cvar **`r_skel_blendnormalize`** (default 1; set 0 only for models that bake intentional non-unit bone scale). This removes the compounding at its source for ALL models, so the QC per-bone orthonormalize (`PlayerVis_OrthonormalizeSkeleton`, ~0.2s/frame) can drop back to the cheap 5-spine-bone version.

**Build/deploy:** `com_mesh.c` is common → **rebuild BOTH** `make m-rel` (`fteqw64.exe`, render) **and** `make sv-rel` (`fteqwsv64.exe`, server hit-detection pose) — see [[engine-build-and-patches]]. **Verify:** spawn a bot match, hold any weapon (incl. HDAK), watch + switch weapons → no torso/arm/gun warps; with `cl_skel_warp_log 1` the `[skelwarp] BODY … Head … inf` spam is gone. Toggle `r_skel_blendnormalize 0` → the warp returns (confirms the lever). Stock engine: warps as before.

**Follow-up (same patch):** the first cut only renormalized the RENDER blend (`Alias_BlendBoneData`), but `skel_build` (the CSQC builtin that populates the player proxy's QC skeleton — what's actually rendered + read by `gettaginfo`/`skel_get_*`/the weapon mirror) fills its bonematrix via a **separate** blend path (`Mod_GetBoneRelations`, `pr_skelobj.c`), so the warp persisted. Fixed by exporting `Alias_RenormalizeBoneMatrix` (un-static) and renormalizing every (re)built bone at the end of `PF_skel_build` (same `r_skel_blendnormalize` + `SKEL_RELATIVE` gate). `pr_skelobj.c` is client-side → the `skel_build` half is `m-rel` only, but `com_mesh.c` (the export) is shared → rebuild **both**. With this, the QC-side `PlayerVis_OrthonormalizeSkeleton` (~0.2 s/frame) can drop back to the cheap 5-spine-bone cleanup.

---

## Patch 44 — `gl_line_width`: CSQC debug wireframe lines respect glLineWidth (scoped to debug lines)  *(APPLIED, client-only / `m-rel`)*

**Files:** `engine/client/renderer.c` (cvar def + register) · `engine/gl/gl_backend.c` (`BE_SubmitMeshChain` injection + `GLBE_Init` range query) · `engine/gl/gl_alias.c` (`R_DB_Poly` un-static)  ·  search `//nettest:` near each.

**Why:** the mod's CSQC debug overlays (nav graph, hitboxes, impacts, physprops, skeleton, …) now draw as true `GL_LINES` (cheap 1px wireframe via the `CSQC_DebugLine` → `R_BeginPolygon` 2-vertex path; engine sets `BEF_LINES` when `nv==2`, `pr_csqc.c:1635`). The user wanted those lines **thicker without reverting to camera-facing quads**. But `BE_SubmitMeshChain` (the GL_LINES draw, gl_backend.c:3139) **never called `qglLineWidth`**, so true lines were pinned at 1px. The QC-side `cl_debug_wire_thickness` only achieves thickness by switching to quads (2 tris + per-edge orientation math).

**What it adds:** a `gl_line_width` cvar (default 1) applied at the single GL_LINES injection point. **Scoped to debug lines only:** 2D `drawline` (the crosshair accents, `R2D_Line` → `BE_DrawMesh_Single` → `GLBE_DrawMesh_List`) and particle line-trails share the `BEF_LINES` path but draw through `shaderstate.dummybatch`, whereas the CSQC scene polys (the debug overlays) are the only batches whose `buildmeshes == R_DB_Poly` (set in `BE_GenPolyBatches`, gl_alias.c:2885). So the injection is gated on `shaderstate.curbatch->buildmeshes == R_DB_Poly` — debug wireframe thickens, crosshair + tracers stay 1px. `R_DB_Poly` was made non-static so gl_backend.c can name it.

**Driver caveat (handled):** `glLineWidth > 1` is clamped to the driver's `GL_ALIASED_LINE_WIDTH_RANGE`; **core** GL profiles cap it at 1. This build defaults to a **compatibility** profile (`vid_gl_context_forwardcompatible 0`), so wide lines work here. `GLBE_Init` now queries the range once into `gl_maxlinewidth` (clamps the cvar) and, if the driver max ≤ 1, prints a one-time note to use `cl_debug_wire_thickness` (quads) instead. `r_showtris`/outlines are unaffected — they use `glPolygonMode(GL_LINE)` on triangle batches, not the `BEF_LINES` batchtype.

**No QC change.** `gl_line_width N` (with `cl_debug_wire_thickness` at its default 1, so QC still emits lines) gives cheap thick lines. The two knobs stay complementary: `gl_line_width` = thick **lines** (cheap, driver-dependent); `cl_debug_wire_thickness` = thick **quads** (works on any driver, costs more — and since quads aren't `BEF_LINES`, `gl_line_width` is ignored when it's >1, so no double-apply).

**Build/deploy:** renderer/GL-backend only → **`make m-rel`** (`fteqw64.exe`); the dedicated server has no GL backend, so **no `sv-rel`**. Deployed to `C:\FTEQuake\fteqw64.exe`. **Verify:** `developer 1` → console shows `GL_ALIASED_LINE_WIDTH_RANGE: [1, N]`; `sv_debug_nav 1` + bots, `gl_line_width 1/4/8` → nav/hitbox/impact lines thicken (up to the driver cap) while the crosshair accents + tracers stay thin. Stock engine: lines stuck at 1px.

---

## Patch 45 — `gl_2dline_width`: thick 2D `drawline` strokes (hit-marker reticle) without the offset-stack hack  *(APPLIED, client + QC / `m-rel`)*

**Files:** `engine/client/renderer.c` (cvar) · `engine/gl/gl_backend.c` (`BE_SubmitMeshChain` else-branch) · QC `client/cl_hud.qc` (`HUD_DrawHitmarker`).  Sibling to Patch 44.

**Why:** the hit-marker (the diagonal-X reticle, `HUD_DrawHitmarker`) faked line thickness by drawing each of its strokes `HITMARKER_WIDTH` times at ±0.5px offsets ("Draw multiple offset lines to simulate thickness") — which read on screen as **several faint reticles layered on top of each other**. Now that the engine can apply `glLineWidth` to lines (Patch 44), the hit-marker can draw single clean strokes at a real width. The 2D `drawline` builtin (`PF_CL_drawline`, pr_menu.c:1063) even has a `width` arg, but it's **commented out** (`//float width = …`) — the engine has always ignored it.

**What it adds:** a `gl_2dline_width` cvar (default 2, matching the old `HITMARKER_WIDTH`) applied at the **same** GL_LINES injection point as Patch 44, but on the **2D path**: 2D `drawline` (`R2D_Line` → `GLBE_DrawMesh_List`) draws through `shaderstate.dummybatch`, so the branch is `else if (shaderstate.curbatch == &shaderstate.dummybatch) lw = bound(1, gl_2dline_width.value, gl_maxlinewidth)`. Fully separate from `gl_line_width` (which is gated on `buildmeshes == R_DB_Poly`, the 3D debug scene polys) — so debug-wireframe width and reticle width are independent. Everything else that hits the GL_LINES path (particle trails, etc.) falls through to `lw = 1`.

**QC:** `HUD_DrawHitmarker` drops the `for (i<HITMARKER_WIDTH)` offset loop and draws **4 single diagonal strokes** (one per corner of the X); thickness now comes from `gl_2dline_width`. `HITMARKER_WIDTH` const removed (unused). The main crosshair (`HUD_DrawCrosshair`) is unaffected — it's `drawfill` rectangles sized by `cl_crosshairthickness`, never `drawline`.

**Scope note:** `gl_2dline_width` controls ALL 2D `drawline` strokes; in this mod the hit-marker is the only `drawline` user (the crosshair is drawfill), so it's effectively the reticle width. Driver-clamped like `gl_line_width` (compat profile here → works; core caps at 1).

**Build/deploy:** `m-rel` (`fteqw64.exe`) + QC (`fteqcc64.exe progs.src`). No `sv-rel`. **Verify:** land a hit → the X reticle is a clean single-width X (not layered duplicates); `gl_2dline_width 1/4/8` changes its thickness; `gl_line_width` (debug) does NOT affect it and vice-versa.

---

## Patch 46 — renormalize `skel_set_bone_world` writes (the CSQC half of the Patch 43 warp fix; kills the QC per-bone scrubs)  *(APPLIED, client-only / `m-rel`)*

**File:** `engine/client/pr_skelobj.c` `PF_skel_set_bone_world` (~line 2461)  ·  search `//nettest warp fix (Patch 46)`.

**Why:** Patch 43 renormalized the bones `skel_build` fills, but **`skel_set_bone_world`** — the other QC path that writes a bone — does `Matrix3x4_Multiply(childworld, parentinv, bone)` and stores the result **verbatim, no renorm**. A non-unit `childworld` (gettaginfo chain-walks the deformed spine and compounds residual IQM frame-blend scale) or non-unit `parentinv` lands on the bone and, on a relative skeleton, compounds down the chain + feeds the gun mirror's R-Hand source (flash/gunmesh/Line02 balloon at render). The CSQC player code worked around this with two QC scrubs every frame per player: a **5-spine-bone** `PlayerVis_OrthonormalizeBone` pass after the spine deform, and a **whole-gun-skeleton** `PlayerVis_OrthonormalizeSkeleton` after the weapon-mirror loop. A profile showed those at ~0.105s (gun) + part of 0.088s (post-deform) — the largest remaining QC cost after the blood-drop + body-orthonormalize wins.

**Fix:** one line at the tail of `PF_skel_set_bone_world` — `if (skelobj->type == SKEL_RELATIVE && r_skel_blendnormalize.ival) Alias_RenormalizeBoneMatrix(bone);` — the exact mirror of Patch 43's `skel_build` renorm (same exported `Alias_RenormalizeBoneMatrix` from `com_mesh.c`, same `r_skel_blendnormalize`/`SKEL_RELATIVE` gate). Every `skel_set_bone_world` write now arrives unit, so `PlayerVis_YawBone`/`PitchBoneWorld` (the spine deform) and the gun-mirror loop produce clean bones with no QC follow-up. The render renorm cost moves from QC (skel_get/skel_set builtins + per-element NaN validation) into a cheap inline Gram-Schmidt in C — net cheaper, and the QC scrubs are deleted.

**Paired QC change** (`client/cl_player.qc`): removed the post-deform 5-bone `PlayerVis_OrthonormalizeBone` scrub and the gun `PlayerVis_OrthonormalizeSkeleton` scrub (both now redundant). `PlayerVis_OrthonormalizeSkeleton` is now unused (harmless); the pre-deform 7-bone scrub stays (covered by Patch 43, cheap belt-and-suspenders on the deform's inputs). `r_skel_blendnormalize` is now the single master switch for the whole warp-fix system — at 0, with the QC scrubs gone, warps return (its documented "models that bake non-unit bone scale" escape hatch).

**Build/deploy:** `pr_skelobj.c` is client-side (the server uses the Patch 36 engine spine path, not `skel_set_bone_world`) → **`m-rel`** (`fteqw64.exe`) + QC. No `sv-rel`. **Verify:** bot match, hold/switch weapons incl. HDAK, `cl_skel_warp_log 1` for a round → no torso/arm/gun warps, no `[skelwarp]` spam; toggle `r_skel_blendnormalize 0` → warps return (confirms the engine renorm is now the only thing holding them off). Re-profile: the gun `OrthonormalizeSkeleton` (0.105s) and the post-deform `OrthonormalizeBone` are gone.

---

## Patch 47 — `sys_framepacing` enhancements: present-cadence telemetry + non-GL fallback + ARB_sync fence  *(APPLIED, client-only / `m-rel`)*

Three refinements to the existing `sys_framepacing` system (the custom high-precision FPS-limiter / present-pacer; the core already mirrors SpecialK's timer+spin hybrid, `1.125×`/`2.875×` thresholds). Files: `client/sys_win.c`, `gl/gl_screen.c`, `gl/gl_vidcommon.c`, `gl/glquake.h`  ·  search `nettest`/`RecordPresent`/`FenceSync`.

**1. Present-cadence telemetry** (`sys_win.c` + `gl_screen.c`). The stats only measured *wait* error (requested vs actual sleep); added a parallel ring (`g_present_ring`) sampling the actual **present-to-present interval** via `Sys_FramePace_RecordPresent()` (called from `gl_screen.c` right after `VID_SwapBuffers`, every mode). `sys_framepacing_stats` now prints a "Present cadence" block: avg interval/fps, min/max, jitter (stddev), spread. This is the real VRR-smoothness metric — lets you SEE mode 4's flat cadence vs mode 2's render-variance jitter even when wait-accuracy looks identical.

**2. Mode-4 non-GL fallback** (`sys_win.c`). Mode 4 (present-pacing) only has a hook in `gl_screen.c`, so on D3D11/Vulkan it silently did nothing. Now `Sys_FramePacePresentActive()` returns true only when `qrenderer == QR_OPENGL`, and `Sys_FramePacingAnchor()` returns true for mode 3 **or** mode 4 off-GL — so mode 4 cleanly degrades to the mode-3 frame-START grid anchor on non-GL renderers instead of being a no-op. Help text + stats text updated to say so.

**3. ARB_sync fence instead of glFinish in mode 4** (`gl_screen.c` + `gl_vidcommon.c` + `glquake.h`). The mode-4 GPU drain was a full `qglFinish()`; now it prefers a per-frame `GL_ARB_sync` fence (`qglFenceSync` → `qglClientWaitSync(GL_SYNC_FLUSH_COMMANDS_BIT, ≤100ms)` → `qglDeleteSync`) — surgical "this frame done" with less collateral stall, marginally tighter hold. Added the three entry points to the GL loader (`getglext`, NULL-safe) + a `GLsync` typedef/enums to `glquake.h`; **falls back to `qglFinish()` if the context lacks ARB_sync** (pre-GL-3.2), so it's risk-free. (Honest: smallest practical gain of the three; the fallback makes it safe.)

**Build/deploy:** client/renderer only → **`make m-rel`** (`fteqw64.exe`); editing `glquake.h` triggers a broad GL recompile (expected). No `sv-rel`. **Verify:** `sys_framepacing 2` → `sys_framepacing_stats` (note present-cadence jitter); `sys_framepacing 4` → jitter drops sharply (flat cadence) while wait-error stays tight; visibly smoother on a VRR/G-Sync display at ~1 frame more latency. Mode 4 on a D3D11 build now behaves as the mode-3 anchor (not a no-op).

**Follow-up:** added a **"mode-4 GPU drain"** line to `sys_framepacing_stats` — reports `ARB_sync fence` vs `glFinish (ARB_sync absent)` vs the off-GL fallback, so you can confirm the fence (item 3) is actually live. Implemented via a `GLQUAKE`-guarded `GLVID_FramePaceDrainPath()` accessor (`gl_vidcommon.c`, returns `qglFenceSync?1:2`) called from the stats command — mirrors the `D3D11QUAKE`/`D3D11_GetFrameLatencyWaitHandle` extern pattern. **VALIDATED in fullscreen** (360Hz, `cl_maxfps 300`, vsync 0): mode 4 present-cadence stddev **67µs vs mode 2's 865µs (~13× flatter)**, avg locked 300fps. Mode 4 is **fullscreen-only** — windowed/DWM owns the present + G-Sync won't engage, so it reads worse there; see [[sys-framepacing-mode4-fullscreen]].

---

## Patch 48 — `skel_build` blend: two latent bugs = the REAL player-model "warp" root cause  *(APPLIED, client+server / `m-rel` + `sv-rel`)*

This is the actual root cause of the player/bot model warp that Patches 43 + 46 (basis renormalize) and a large QC `PlayerVis_GuardSkeletonTranslations` bind-snap workaround were *compensating the symptom of*. Patches 43/46 renormalize the **basis** (the linear blend de-orthonormalizes the 3×3) — real and still correct — but the visible warp was a **pure translation** corruption (limbs rendered several times their length, or flung across the map, with no thickness change), which renorm never touches (it preserves the translation column). Two distinct bugs in `skel_build`'s blend path produced it; both reproduce **headless** during a bot firefight (the transition frames), so they were pinned by instrumenting `Alias_BlendBoneData` directly. Files: `engine/common/com_mesh.c`, `engine/client/pr_skelobj.c`  ·  search `//nettest fix`.

**Bug A — cross-group pose read (the "across the map" garbage flings, bones to ~1e12 u).** `engine/common/com_mesh.c` `Alias_BlendBoneData` (~line 1263/1268). `Alias_FindRawSkelData` fills a **separate** `lerps[]` slot per bone-group (`lerps++` per group; with a basebone torso/leg split the torso group is `lerps[1]`). The blend loop iterates groups via `lerp`, and correctly uses `lerp->frac`/`lerp->skeltype`/`lerp->pose[0]` (in the `memcpy` fast-path) — **but the blend path read `lerps->pose[b]` (always group 0)** instead of `lerp->pose[b]`. For group 1, when its `lerpcount` exceeds group 0's, `lerps[0].pose[b]` dereferences an **uninitialised pose pointer** → garbage keyframe data → whole-limb translation blowup on a transition frame. **Fix:** `lerps->pose` → `lerp->pose` (both reads). Consistent with the `memcpy` fast-path right above it; for `numgroups==1` it's a no-op, so zero risk for non-split models.

**Bug B — uninitialised framestate → phantom frame-blend influences (the uniform ~7–18× "stretched limbs").** `engine/client/pr_skelobj.c` `PF_skel_build` (~line 1993, before `Get_FrameState`). The `framestate_t fstate;` is an **uninitialised stack local**, and `cs_getframestate` only fills frame-blend slots **[0] and [1]** (the 4-slot `FRAME_BLENDS>=4` path is `#if 0`'d out and never zeroes [2]/[3]). So slots [2]/[3] keep **stack garbage** — `Alias_BuildSkelLerps` then blends in two *phantom* poses (the correct ~11.4 u bind pose) with **garbage weights** (captured live: `f[2]=2.354`, `f[3]=14.219`; Σ≈17.6 → 11.4 u × 17.6 ≈ 200 u). Renorm fixes the basis but leaves the scaled translation → the limbs render stretched with a clean basis. **Fix:** one line — `memset(&fstate, 0, sizeof(fstate));` before `Get_FrameState`. Slots [2]/[3] are now zero-weight → dropped → only the real [0]/[1] influences blend.

**Paired QC change** (`client/cl_player.qc`, `shared/sh_cvar_table.qc`): with the engine producing correct skeletons, the entire QC warp-workaround stack is **deleted** — `PlayerVis_GuardSkeletonTranslations` + the per-model bind-translation cache (`PlayerVis_BindSlot`/`g_bind_*`), `PlayerVis_CheckWarp` + the BODY/GUN warp-catch resets, the dead `PlayerVis_OrthonormalizeSkeleton`, and the `cl_skel_warp_log` cvar. The spine-deform + its 7-bone `PlayerVis_OrthonormalizeBone` pass stay (those are for the aim/strafe lean, not the warp).

**Build/deploy:** `com_mesh.c` + `pr_skelobj.c` are shared (the server also `skel_build`s for hitbox/lagcomp skeletons) → rebuild **both** `m-rel` (`fteqw64.exe`) + `sv-rel` (`fteqwsv64.exe`) + QC. **Verify:** packed bot match + firefights, hold/switch weapons incl. HDAK → no stretched/flung limbs. Headless repro for regression: `+set sv_bot_minplayers 10 +map fy_iceworld` with the (former) `cl_skel_warp_log 1` detector showed `[skelfix]` firing every firefight frame before; **0 across 3×35 s boots** after. (Note: Patches 43/46 + `r_skel_blendnormalize` stay — they fix the orthogonal **basis** de-orthonormalization; Patch 48 fixes the **translation** corruption.)
