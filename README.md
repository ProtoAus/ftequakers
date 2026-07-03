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
- **Box3D physics plugin (`fteplug_box3d`)** — a second rigid-body backend
  alongside ODE, using [Box3D](https://box2d.org/posts/2026/06/announcing-box3d/)
  (Erin Catto's C fork of Box2D). It has a **multicore** internal solver,
  handles **concave props** by attaching one convex hull per convex-decomposition
  piece to a body (`.acd` sidecars or the runtime decomposition), and supports the
  gravity-gun "black hole" force. Load it with `plug_load box3d` (it registers as
  the physics engine like ODE). Tuned by `physics_box3d_*` cvars: `unitscale`
  (Quake-units-per-metre, so Box3D's metre-tuned tolerances fit — this is required),
  `threads` (worker count), `substeps`, `decomp` / `maxpieces` (concave detail vs
  cost), `debug`. Builds against the prebuilt pure-C `libbox3d.a` — no extra runtime
  DLLs. Skeletal ragdolls and prop `.touch` events are not yet wired (ODE covers those).
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
- **Crepuscular god-ray fixes** — the sun-shaft (crepuscular) pass now
  aligns correctly at `r_renderscale` > 1, excludes the first-person
  viewmodel from the occluder mask (so the gun's silhouette no longer
  smears rays from screen-centre), and depth-gates the additive composite
  so near geometry — including the gun — cleanly blocks the rays instead of
  letting them bleed over it.

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
- **`func_fogvolume` fog volumes** — bounded, per-brush-entity fog for Q1
  (idBSP) and Half-Life maps, which have no Q3 fog lump. A mapper places a
  `func_fogvolume` brush and the render path applies its fog only inside that
  volume.

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
touched files recompile, then it relinks).

```sh
# Plugins: cod + hl2 asset loaders AND the physics plugins (ode + box3d). Build
# from THIS tree so the ABI matches the exe. The physics entries and `-k` are
# both required — see the notes below.
make plugins-rel FTE_TARGET=win64 NATIVE_PLUGINS="cod hl2 ode box3d" CC=gcc CXX=g++ -k
```

Two gotchas this command works around:

- **List the physics plugins explicitly.** `ode` and `box3d` are both commented
  out of the Makefile's default plugin set, so `NATIVE_PLUGINS="cod hl2"` builds
  *no physics plugin* and phys props silently break. (The old `PLUGINS_STATIC="ode"`
  token this README used to show did nothing — it is not a real Makefile variable.)
- **Keep `-k`.** Each plugin's last build step embeds a metadata zip via the
  `zip` tool, which isn't in the UCRT64 shell, so every plugin ends with
  `zip: command not found` / `Error 127`. That step is **harmless** — the DLL is
  fully linked *before* it, and the metazip is only plugin-manager cosmetics the
  engine never reads — but **without `-k` it aborts the make after the first
  plugin** (that is why `"cod hl2"` only ever produced `cod`). With `-k`, make
  keeps going and builds them all despite the expected non-zero exit. ODE links
  the prebuilt static `libode.a` under `engine/libs-x86_64-w64-mingw32/`; box3d
  links the prebuilt pure-C `libbox3d.a` (`BOX3D_BASE` in the Makefile) — no
  libstdc++, no runtime DLLs.

The DLLs land in **`engine/release/`**. Copy all four — `fteplug_cod_x64.dll`,
`fteplug_hl2_x64.dll`, `fteplug_ode_x64.dll`, `fteplug_box3d_x64.dll` — next to the
executable, and **redeploy them every time you rebuild the engine**: a plugin built
against an older exe fails to load with `Couldn't load plugin <name>`. The ODE and
box3d plugins are statically linked, so they need no `libwinpthread-1.dll` /
`libgcc_s_seh-1.dll` / `libstdc++-6.dll` beside them — but a *non-static* build will
fail to load once those runtime DLLs are gone. See the `documentation` folder for more.

## Based on FTEQW — credits & license

All credit for the engine goes to the FTE team and contributors. FTEQW
is licensed under the GNU General Public License v2.

    Copyright (c) 2004-2025 FTE's team and its contributors
    Quake source (c) 1999 id Software

See `LICENSE` for the full terms and `Credits.md` for contributors. The
original upstream README — highlights, contact links, issue-reporting
guidance — is preserved in this repository's git history and at
[fteqw.org](https://fteqw.org).
