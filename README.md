# ftequakers

A personal fork of [FTEQW](https://fteqw.org) with a small set of engine
changes made for my own mod/project. It is standard FTEQW plus the
additions listed below — nothing has been removed.

- Upstream: [fte-team/fteqw](https://github.com/fte-team/fteqw)

## What's changed from upstream

The new cvars, render flags, and builtins are opt-in — they default to the
original engine behaviour, so a stock setup is unchanged. The added map-format
support and bug fixes don't alter stock Quake content either; they only apply
when you load those maps or would otherwise have hit those bugs.

### Gameplay & physics

- **`SOLID_PHYSICS_TRIMESH` collision** — players collide against a
  prop's actual triangle mesh instead of its bounding box, on both the
  server and the client-side predictor (alias/IQM/MD3 props that expose
  NativeTrace).
- **Half-Life model aim poses** — bone controllers (`.bonecontrol1..5`)
  and the aim subblend are fed into the server framestate, so HL models
  drive their torso/arm aim poses correctly.
- **`ED_ParseUnknownEpair`** — optional QC hook
  `void(string key, string value)` letting gamecode absorb arbitrary
  mapper keyvalues (e.g. `multi_manager` `<target>=<delay>` pairs)
  instead of warning about unknown fields.

### Rendering

- **`RF_XFLIP`** (CSQC render flag, 128) — horizontally mirrors an
  entity, intended for left-handed viewmodels; flips projection X and
  cull winding so backface culling stays correct.
- **`r_viewmodel_maxlight`** — per-channel ceiling on the first-person
  viewmodel's light so a bright floor (lava, white tile) doesn't blow
  the gun out. `0` = off (engine default); try `96..160`.
- **Half-Life `.mdl` normalmaps** — loads an optional `<model>_norm`
  texture as a whole-model bumpmap, used by the defaultskin GLSL `#BUMP`
  path when a per-pixel light direction is available.
- **`r_wateralpha_extendpvs`** — opt-in transparent-water PVS extension
  for legacy maps (vanilla GoldSrc / classic vis) so underwater geometry
  shows through transparent water at a distance. Leave `0` for maps
  compiled with modern transparent-water vis.

### System & performance (Windows)

- **`sys_framepacing`** — high-precision `cl_maxfps` pacing. The engine's
  own (drift-corrected) limiter still decides when each frame is due; this
  makes the wait land on that target accurately. `0` = vanilla `Sleep()`;
  `1` = NtSetTimerResolution + high-res waitable timer + spin; `2` = (1) +
  DXGI frame-latency wait on D3D11; `3` = (2) + absolute-grid anchor
  (renderer-agnostic). `sys_framepacing_stats` reports what it's doing.
- **`cl_debug_spikes`** — logs a per-stage timing breakdown whenever a
  client frame exceeds `cl_debug_spike_ms` (default 2 ms), to pin a
  hitch to a specific stage.

### Menu / UI

- **`localcmd_local`** (menu builtin) — injects a command at trusted
  (LOCAL) level rather than INSECURE, so menu sliders can set
  `NOTFROMSERVER` cvars (`sys_highpriority`, `sys_framepacing`, ...).
  Menu-only by design.

### Source / Half-Life 2 maps

- **Loads Valve Source (`.bsp`) maps** — Half-Life 2 and Counter-Strike: Source
  levels, with their skyboxes (including HDR skies), textures and materials,
  water, and props (which are distance- and visibility-culled). A large share of
  the work went into *not crashing* on big, complex maps such as `d1_canals`,
  and into silencing the flood of harmless warnings those maps print.

### Call of Duty maps

- **Loads Call of Duty 1 & 2 (`.bsp` / `.d3dbsp`) maps** — including the models
  placed around the level (rocks, foliage, props) at the correct scale. A bug
  that made large maps take minutes to load is fixed.

### Using content from your installed games

- **Mounts content straight from your installed Steam games** — Half-Life,
  Counter-Strike 1.6, Half-Life 2, Counter-Strike: Source, and Call of Duty —
  on demand and at low priority, so it fills in missing assets without ever
  overriding your own files. The menu lists maps from those games (from an
  offline index), and when two games share a map name (e.g. `de_dust2`) it loads
  the correct copy.

### Dedicated server & multiplayer

- **Dedicated (windowless) servers can host Source and CoD maps** — they used to
  crash the instant such a map loaded.
- **A batch of connection fixes:** no instant crash when a second player joins;
  no "map does not match" kick when you join a server hosting a map you also own
  under a different game; water renders correctly the moment you connect (instead
  of see-through until you change a setting); and `+connect` / `+map` launch
  options take you straight into the game instead of the menu backdrop.

### Weather & effects

- **Rain that splashes** on water surfaces and on physics props, with optional
  ripple rings and a per-frame cap so heavy weather stays cheap.

### Smaller fixes & cleanup

- A real **`flushshaders`** console command (reloads shaders without a full
  video restart); console history kept inside the game folder instead of the
  install root; comment-aware config parsing; and the bundled ODE physics plugin
  is statically linked, so it needs no loose runtime DLLs alongside it.

## Building

Built with MSYS2 **UCRT64** (gcc). Open the *MSYS2 UCRT64* shell and run from
`engine/`:

```sh
# Engine: client fteqw64.exe + dedicated server fteqwsv64.exe
make clean m-rel sv-rel FTE_TARGET=win64 \
    CFLAGS="-O3 -march=x86-64-v3 -flto=14" \
    LDFLAGS="-static -flto=14" \
    OPTIM_RELEASE="-O3" \
    CC=gcc CXX=g++ PKGCONFIG=pkg-config -j14
```

`-march=x86-64-v3` enables AVX2/FMA codegen engine-wide. **It requires a
Haswell-era (2013+) CPU and will SIGILL on anything older** — there is no
runtime fallback. Drop back to `-march=x86-64-v2` if you need to run on
pre-AVX2 hardware.

Drop `clean` for a fast incremental rebuild after a small change (only the
touched files recompile, then it relinks). Note: the old `PLUGINS_STATIC="ode"`
token did nothing — it is not a real Makefile variable — so it has been removed.

```sh
# Asset plugins (cod + hl2): build from THIS tree so the ABI matches the exe
make plugins-rel FTE_TARGET=win64 NATIVE_PLUGINS="cod hl2" CC=gcc CXX=g++
```

The plugin DLLs land in `engine/release/`; copy `fteplug_cod_x64.dll` and
`fteplug_hl2_x64.dll` next to the executable. Rebuild them whenever the
engine↔plugin ABI changes. See the `documentation` folder for more.

## Based on FTEQW — credits & license

All credit for the engine goes to the FTE team and contributors. FTEQW
is licensed under the GNU General Public License v2.

    Copyright (c) 2004-2025 FTE's team and its contributors
    Quake source (c) 1999 id Software

See `LICENSE` for the full terms and `Credits.md` for contributors. The
original upstream README — highlights, contact links, issue-reporting
guidance — is preserved in this repository's git history and at
[fteqw.org](https://fteqw.org).
