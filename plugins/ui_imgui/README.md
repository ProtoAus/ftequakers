# Optional explicit-draw Dear ImGui service

Pinned upstream: **Dear ImGui 1.91.9b**, Git tag `v1.91.9b`, commit
`f5befd2d29e66809cd1110a152e375a7f1981f06`.
`vendor/` contains unmodified upstream core/headers and its MIT `LICENSE.txt`;
`vendor/SHA256.json` pins every vendored file. No runtime code/font downloads.

This is a **diagnostic gallery / command-list renderer prerequisite**, not the
scoreboard, graph, editor, main menu or input/widget/model bridge. It accepts
only owner 101 in MQC and owner 202 in CSQC, explicitly dispatched via the
already-versioned `NativeUI/1` host. No Sbar/Menu/Tick drawing hook. Loaded but
closed runs no ImGui frames or atlas uploads. `ui_imgui_status` is an explicit,
quiet-by-default diagnostic command. Implicit ini/log writes are disabled.

- One context and one embedded-default-font RGBA atlas per opened VM; physical
  13px font/layout with framebuffer scale 1, independent of QC virtual scale.
  No claim of advanced font quality, Unicode/input transport or DPI selection.
- Main-thread synchronous `2DMesh/1` only. No graphics API hooks; no engine
  C++/dedicated-server link dependency. C ABI version/size remains unchanged.
- Preflight all draw lists, finite transforms/clips/UVs, atlas IDs, counts,
  offsets and consumed indices before any Submit. Unknown callbacks reject;
  `ImDrawCallback_ResetRenderState` is a no-op because each host batch restores
  renderer state. This is trusted native code, not a hostile-pointer sandbox.
- 64 lists / 262,144 source vertices / 786,432 stored AND consumed indices /
  4,096 commands per frame. Reused fixed scratch buffers expand triangles into
  <=16,383 vertices/indices per host batch and <=128 commands, supporting both
  16/32-bit source indices and VtxOffset. Expansion is a deliberate prototype
  tradeoff; CPU/GPU cost is NOT accepted. Do not switch a real panel default.
- A backend Submit failure returns false for immediate QC fallback. Validating
  malformed data is all-or-nothing, but multiple successful host batches cannot
  be rolled back if a later backend submission fails. The QC caller redraws its
  legacy equivalent; it must cover its own content rather than rely on erasing.
- Explicit close, host VM/plugin/renderer/failure cleanup destroys textures
  before renderer teardown and disposes contexts. No frame/QC pointers retained.

Build with the engine's `plugins-rel NATIVE_PLUGINS="... ui_imgui"` or the
plugin Makefile's exact `fteplug_ui_imgui` target. Adapter compiles at -O2;
unmodified vendor compiles at -O1 to avoid GCC 16's upstream mouse-array
assert/range diagnostics. Both use -Wall -Wextra -Werror, no warning suppression.
Windows links libgcc/libstdc++ statically in the plugin; neither runtime becomes
an engine/server dependency. Linux/device/non-GL packaging remains unverified.
Normal development `build.ps1 -Engine` includes/copies this optional plugin to
both Windows installs; loading remains explicit. Release ship-set changes are
not part of this prerequisite and must be gated before wider adoption.

Falsifiers and retained results: FTESurf `tools/p595imgui.md`,
`tools/p595imgui.py`, `tools/p595build.py`, `tools/test_p595imgui_unit.py` and
`tools/fixtures/p595imgui_host.cpp` (real service/adapter/ImGui source).
