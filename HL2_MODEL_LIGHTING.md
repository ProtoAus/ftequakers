# Source model lighting: fixes and remaining approximation

Assessment made 2026-10-06 UTC, Patches 508–509. Subject: `surf_tensor2`,
Momentum's installed VBSP v20 map; 232 static props, no embedded `.vhv` bakes.
The apparently brush-built window walls are architectural **models**, not
`func_brush` faces. World lightmaps and model lighting are different paths.

## Implemented

* **Ambient-only minimum (508).** `VBSP_FloorModelAmbient` lifts the shader base,
  not the directional amplitude or direction. Both folds use the base. For
  nonzero luminance the additive lift equals the old base scale/desaturation;
  zero bases become neutral grey. Baked vertex lighting stays exempt.
* **World occlusion (509).** PVS remains a cheap rejection step, followed by a
  real point trace to every contributing local light. A dedicated map-owned BIH
  contains Source `MASK_OPAQUE` world brushes and opaque displacements with ray
  collision enabled. The contents test uses the original Source bits, before
  movement remapping. Window/grate-only brushes are not opaque under that mask.
* The light tree excludes receiving props, other prop collision hulls and inline
  movers. It does not depend on `hl2_propcollision`, `hl2_dispcollision`, or a
  displacement's NOHULL flag. NORAY still excludes the displacement from rays.
  The player-movement tree is retained unchanged.
* Surface-light endpoints are moved 0.125 units along the emitter's outward
  normal to avoid blocking on the emitting brush itself. Point/spot endpoints
  and the receiving sample are not moved. Startsolid/allsolid/hit rays are
  blocked. Missing trace infrastructure fails closed, not silently PVS-only.
* `hl2_lt_min` and `hl2_lt_occlusion` changes invalidate static-prop lighting
  without another cvar toggle. `hl2_lt_occlusion 0` is the PVS-only control,
  not the default. `hl2_lt_occlusion 1` is the default.
* `hl2_lightprobe x y z` reports each candidate local light's incident linear
  RGB, direction and blocked/visible result, plus the resulting six-face direct
  cube. It neither changes player position nor writes player data. Worldlights
  must be enabled and a Source map must have rendered a frame first.

## What was measured

The first saved camera's stage1_detail02 wall ROI, `(400,260)-(800,345)`, fell
from 191.96 mean 8-bit luminance before 508 to **53.90** with the default floor.
Minimum-off is 49.15, restoring 16 returns 53.90, bounce-only is 26.82. Its
adjacent lightmapped ceiling stays 40.14. The floor changes base RGB from
2.0/5.0/6.0 to 13.8/16.8/17.7 while directional RGB stays exactly
94.8/114.3/127.3. Occlusion leaves this sample unchanged: its candidate rays are
clear. That is the visible-light control, not an assumed failure to act.

At the second saved camera, a left architectural-frame ROI,
`(495,430)-(570,670)`, is 58.95 with PVS-only, **37.35** with world occlusion,
and 28.27 with bounce-only. Restoring occlusion reproduces 37.35. A world-ceiling
ROI `(1000,140)-(1050,170)` stays 38.66 with occlusion on/off, bounce-only and
models hidden. Disabling all direct light would make this scene darker still;
that is an incomplete solution, not the desired reference.

Production-helper harnesses compile extracted C with GCC `-Wall -Wextra
-Werror`: 68 floor checks and 21 light-tree/trace checks, zero failures. Runtime
logs contain 18 probe cubes and acted-on visible/blocked/PVS-only controls.
Every cube reconstructs from **visible lights only**, with unchanged candidate
incident RGB/directions between same-point on/off arms. An independent convex
world-brush oracle matches **301 rays: 121 blocked, 180 clear, zero
disagreements**. This map's graded rays do not independently demonstrate a
shadow caused by a displacement; displacement selection/NORAY/NOHULL behavior
is covered by the production-helper harness instead.

## Against Source: what the current approximation loses

Reference code is the local, unversioned Momentum/Source SDK snapshot:

* `mp/src/materialsystem/stdshaders/common_vs_fxc.h`, `AmbientLight`,
  `VertexAttenInternal`, `CosineTermInternal`, `DoLighting` (lines 674–812).
* `mp/src/materialsystem/stdshaders/vertexlitgeneric_dx9_helper.cpp`:
  `MATERIAL_VAR_HALFLAMBERT` and shader/light-state selection.
* `mp/src/utils/vrad/vradstaticprops.cpp`, vertex-lighting loop around 1390:
  transform each vertex position/normal, then compute direct and indirect light.
* `mp/src/game/client/momentum/worldlight.cpp`, `GetLightAtPoint`: PVS is
  followed by an opaque ray, not accepted as visibility. This is corroborating
  client-side worldlight code, **not** the closed engine's model light cache.

The reference snapshot has no Git metadata. For reproducibility, SHA256:
`common_vs_fxc.h` =
`4a56acc0c4ff3d15ef5658045968f07eaf6f73e255ed2893bc6a7a738e2d1f20`;
`vradstaticprops.cpp` =
`81383a5a4ff40f9c1989be104892e3cbde08bb29b509583cc6ba88f9c208c2df`;
`momentum/worldlight.cpp` =
`6b4cf371624408f0dfd36aea09ecacc30a9c9c4d4dbd6123fb9fd8b0c72a0c7f`.

These are source-contract comparisons and measured FTE controls. They are **not
matched Source runtime screenshots**, nor proof of Source's exact light-cache
selection, material permutations, exposure or tone mapping.

### 1. Ambient cube versus two slots

Source's ambient shader sums `n.x² * faceX + n.y² * faceY + n.z² * faceZ`,
choosing each face by the sign of the world normal. It retains six RGB faces.
FTE's default split folds the cube into one base, one amplitude and one dominant
direction. That cannot preserve arbitrary six-face colour/angular structure.
Opposite-coloured lights or multiple perpendicular lights expose the loss.
`hl2_cubelight 1` offers a six-face shader path, but it is not a blanket Source
parity switch: it still uses the sampled cube and the existing plugin direct
scatter, and its normal-space/permutation handling needs separate controls.

### 2. Direct lighting varies across the model

Source's shader computes each selected local light's direction and distance
attenuation from **the vertex world position**, not only the instance's
lighting-origin point. The ambient cube and selected light set may still be
cached per instance; this does NOT mean Source reselects lights or traces a
shadow ray at every hardware-lit vertex. A VRAD `.vhv` bake, by contrast,
computes direct/indirect lighting per vertex with its bake-time shadow scene.

FTE first scatters local direct lights into six faces **at one sample point**,
then usually folds that combined cube to two slots for the whole model. It
therefore loses separate local-light positions and spatial attenuation, not
just the extra ambient faces. The author-specified lighting origin is honored;
replacing it with a bounds centre is not an evidenced cure.

This matters at architectural scale. Prop 46, stage3_detail01, has local bounds
`(-4448,-1728,-1152)..(4704,1728,1152)` and yaw 90: a 9152-unit dimension.
World-only probes within those transformed bounds, at the same X/Z and Y
-800 versus 5344, give +Y direct linear luminance **0.148378 versus 0.038567**
(3.85x), and +Z **0.064837 versus 0.019327** (3.35x). A single sample cannot
represent this variation. These re-evaluated point probes demonstrate spatial
variation, not Source's exact per-instance cached light set or mesh-vertex
irradiance.

Even smaller prop 187 (local 1088×192×1536, yaw 90) changes its +Z/-Z direct
luminance from **0.019459/0** at Z=5632 to **0/0.029446** at Z=6912, both within
its transformed bounds. Occlusion fixes the origin's leaked rays; it cannot
make one origin's lighting describe both ends.

### 3. Half-Lambert is also not identical

Source chooses half-Lambert via the material flag; when selected,
`CosineTermInternal` squares `(N·L * 0.5 + 0.5)`. FTE's split-fold shader uses
an **unsquared** half-Lambert ramp and enables it for the split fold even without
the material flag. At N·L=0, the directional weights are 0.25 versus 0.5; the
ambient bases and fitted amplitudes differ as well, so this is not a global
"halve the brightness" correction. It is a separately falsifiable mismatch.

## Conclusion and remaining work

The two diagnosed brightness defects are fixed without disabling legitimate
local lights. The unbaked large-model path remains an approximation of Source,
and a colour-cube switch alone does not fix its spatial direct-light loss.

For closer parity, retain separate local-light positions/colours/attenuation
and evaluate at vertices or pixels while preserving the six-face ambient cube,
material-selected cosine/half-Lambert, and normal transforms. Alternatively,
honor a real per-instance VRAD bake when supplied. Both need rotated-model,
multiple-coloured-light, spatial-attenuation and matched-camera Source controls.

Not implemented by 509: prop/mover shadow meshes, displaced shadow-ray exactness
against Source, skylight tracing, styled lights, a new per-vertex/pixel local
light pipeline, or a Source screenshot oracle. Do not call the default path
Source-equivalent or normalize it to an adjacent brush texture by eye.

Re-run:

```
python tools/p508ambient.py --engine <fteqw>
python tools/p509lighting.py --engine <fteqw> --log <probe.log>
python tools/p509brushoracle.py <surf_tensor2.bsp> <probe.log>
```

Harness logs/screenshots are kept outside public Git. Camera ROI results are
from isolated DLL runs; owner saves/configs and the shipped executable/progs
were not modified by these tests.
