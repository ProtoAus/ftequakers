# P1836 dedicated PHY-first candidate (default OFF)

From the repository root run:

```sh
bash plugins/hl2/tests/run_dedicated_phy.sh
```

This compiles an isolated test translation unit including the **production**
`mod_hl2.c` and `mod_phy.c`, with engine-interface callbacks substituted. It does
not build/load the plugin or run the engine. Outputs go under `rig/lobbyfast/`.

The positive fixture is a synthetic one-triangle supported PHY and a static,
one-bone MDL with no animation/sequences/includes. VVD/VTX companions are absent.
The test observes five render-file reads with the switch off, zero with it on,
and compares exact collision vertices, indices, contents, bounds, radius and
bone matrices. The P1812 callback is stubbed, but calls the production convex-piece
parser and checks one piece and its triangle range. The BIH callback captures its
input; this is **not** an acting BIH trace or brush-construction test. Matrix
construction is a test callback, not the engine's real quaternion implementation.

Fallback arms compare on/off collision arrays and observe the full render-file
lookup path: missing, truncated, unsupported version, animated, rendered client,
jointed, nonfinite PHY, sequences, and collision modes 0/2/3. No test here proves
real rendered geometry/animation or a wrapped large-hull fallback. Model-group
allocation leaks in this small fixture are intentional; sanitizer runs disable
leak detection, not address/undefined-behavior checks.

## Parent qualification before changing the default

Use a parent-built, hash-bound dedicated executable/plugin in the independently
owned fixture, under the parent-held game lock. Run **fresh processes** per arm:

```sh
"$SERVER" -nohome -basedir "$FIXTURE" -game ftesurf -dedicated \
  +set cfg_save_auto 0 +set developer 1 +set hl2_propcollision 1 \
  +set hl2_phywinding 1 +set hl2_dedicatedphy 0 +map "$MAP"
# Repeat identically with +set hl2_dedicatedphy 1 in a fresh process.
```

`SERVER`, `FIXTURE` and `MAP` must be supplied by the parent after checking actual
binary CLI and asset mounts. Keep ports/config/logs isolated as in the parent rig.
The on arm must print `dedicated validated PHY (render skipped)` for each model
claimed optimized; no such line is not a performance sample of this feature.
Do not count a map reload with cached models as A/B. Verify collision-shape census
matches, but **census alone is not collision equivalence**.

There is no existing console command here that dumps all model collision arrays.
For actual-model acceptance use a debugger/test driver at `BIH_BuildAlias` to
capture exact input vertices/indices/contents; capture model bounds/radius and
bone base/inverse transforms, the mover9 phyfixed model, and P1812 vphysalt brush
planes/contents/bounds. Compare off/on, then run identical player/box traces using
both mover9 and mover17. Include accepted static PHY, no PHY, malformed/unsupported
PHY, real animated and large/wrapped hull controls, and a rendered-client control.
The worker did not run these engine checks. The candidate intentionally declines
all sequences (including ordinary static sequences), so corpus eligibility may
be small or zero. No speedup has been measured.
