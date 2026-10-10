# Optional explicit-draw Dear ImGui service

Pinned upstream: **Dear ImGui 1.91.9b**, Git tag `v1.91.9b`, commit
`f5befd2d29e66809cd1110a152e375a7f1981f06`.
`vendor/` contains unmodified upstream core/headers and its MIT `LICENSE.txt`;
`vendor/SHA256.json` pins every vendored file. No runtime code/font downloads.

This is a **diagnostic gallery / explicit transport prerequisite**, not a
migrated scoreboard, graph, editor or main menu. Passive owners 101 (MQC) and
202 (CSQC) use the unchanged `NativeUI/1` host; optional interactive owners
103/204 require additive `NativeUIInput/1`; optional `NativeUIModel/1` supplies
counted diagnostic snapshots. No production panel has migrated. No Sbar/Menu/Tick drawing hook. Loaded but
closed runs no ImGui frames or atlas uploads. `ui_imgui_status` is an explicit,
quiet-by-default diagnostic command. Implicit ini/log writes are disabled.

- One context and one embedded-default-font RGBA atlas per opened VM; physical
  13px font/layout with framebuffer scale 1, independent of QC virtual scale.
  No claim of advanced font quality, non-Latin glyph coverage or DPI selection.
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

## Optional scalar input/actions

`NativeUIInput/1` is an exact-size copied table from the SAME provider as
`NativeUI/1`. An older host may reject it without preventing passive drawing;
interactive owners then refuse open. Optional MQC/CSQC named builtins are
`ui_native_input_status()`, `ui_native_input(handle,type,a,b)` and
`ui_native_poll(handle)`; QC wrappers gate every optional builtin.

Event types and bounds (all scalars finite):

| Type | a | b |
|---|---|---|
| 1 mouse position | physical X, -32768..32768 | physical Y, same bound |
| 2 button | integer 0..4 | pressed 0/1 |
| 3 wheel | horizontal -10..10 | vertical -10..10 |
| 4 key | compact PLUGUI_KEY_* id 1..18 | pressed 0/1 |
| 5 text | Unicode scalar 1..0x10ffff, excluding surrogates | 0 |
| 6 reset | 0 | 0 |

QC MUST already own input/focus/cursor routing for its live VM-local handle.
There is no native input hook, engine bind/scancode interpretation, forwarding
of console/server commands, automatic capture or engine cursor claimant.
Mouse coordinates are physical; QC callers must convert their virtual units.
Compact key ids are in `plugin.h`, not platform/engine key numbers. Reset clears
queued/held keys/mouse/text, active focus and pending actions; close/restart also
destroys widget state. Clipboard and OS IME callbacks are disabled.

The host admits at most 128 non-reset events per host frame even across reopen;
the plugin bounds both accepted-since-Draw and actual trickled queued events at
128. Reset remains callable at the limit. Overflow/callback failure releases
the host owner for covering QC fallback. All core/adapter compilation units use
IMGUI_USE_WCHAR32: supplementary text keeps its exact UTF-8 bytes, though default
atlas glyph coverage remains limited. The gallery text buffer is 128 bytes.

Poll returns vector `(id, owner_generation, scalar_value)` or zero, at most 16
calls after a successful Draw in the SAME VM draw bracket/frame. Malformed action
records release the owner; pre-reset and previous unread frame actions expire.
Gallery ids 1/2/3 witness click count, checkbox state and UTF-8 text byte count;
no text contents or engine commands cross this interface. Owner generation is
NOT a row/model snapshot generation. Real browser/entity actions require a
separate counted model/identity protocol before use. Synthetic QC event tests
are not device-routing/minus/modifier/cursor or real-panel acceptance.

## Optional counted widget/model snapshots

`NativeUIModel/1` requires the same provider's existing draw AND input services.
Older hosts can reject only this extension; existing galleries remain usable.
QC wrappers `NUI_ModelAvailable/Begin/Widget/Commit/Poll` gate named builtins.
Stage within the VM draw bracket, before Draw: begin(handle,revision,count), then
widget(handle,index,identity,value,label) for ordered indices 0..count-1, then
commit(handle). Identity is vector(widgetID,rowID,type); type 1=text, 2=button,
3=checkbox. IDs/revisions are integers 1..16777215; widget IDs are unique. Rows
can contain multiple widgets. Count is 0..64; zero clears the model. Labels have
at most 95 valid UTF-8 bytes plus NUL, with ASCII control bytes rejected. All
labels are plain text, including literal ##/###. No string pointer is retained.
Text/button value is 0; checkbox value is 0 or 1. Snapshot values are immutable:
QC responds to an action by publishing a strictly newer revision, not mutating
native widget state. Revisions restart only with a fresh owner generation.

Invalid/incomplete/duplicate staging cannot publish; a bad row poisons its whole
transaction. Failed native publication releases once. Replacement cancels queued
and held input, active focus and unpolled actions, and requires a successful Draw
before polling. `poll(handle,currentRevision)` returns vector(widgetID,rowID,value)
(button=1, checkbox=0/1) exactly once, with generation/revision checked inside the
host. No offset, label hash or command string is action authority. Poll shares the
16-attempt host-frame budget with scalar polling. Scalar Poll returns empty while
an owner has an active model. No production input routing or panel is introduced.

## Optional dense snapshots and the scoreboard owner

`NativeUIModel/2` is a second exact-size table from the same provider: the /1
contract with 256 widgets (a 28684-byte snapshot against /1's 7180). /1 is
unchanged and stays 64; the host converts either way for an older peer, validating
first. `ui_native_model_limit()` returns 256, 64 or 0 so QC sizes a page before it
stages one. Open attempts are budgeted at 4 per VM per host frame; asking again
for the live owner is free.

Owner 205 is FTESurf's ranked scoreboard table (Patch 605, opt-in from QC): one
ImGui table inside the inherited clip, a metadata prefix (pin state, message,
Previous/Next, standing, optional font size) and nine cells per row, at most 24
rows. Four ProggyClean bakes (13/16/20/24 physical px) share one 512x256 atlas
made at Open. A clip under 150x80, non-finite, or larger than the framebuffer
returns false and QC draws its own table. Appearance, DPI and device acceptance
are not claimed. Falsifiers: FTESurf `tools/p603scores.md` and the `p603*` tools
it names (a historical harness number, not Patch 603).

Since Patch 606 the owner wears FTESurf's `Dusk` palette (`ScoresTheme` in
`scores.inc`): colours, two rounding values and `DisabledAlpha` 0.85, the same
numbers as `SUI_THEME_*` in FTESurf `src/shared/sh_ui.qc`. No padding, spacing
or border size is set there, because those gates click at fixed offsets inside
the table. The font is still ProggyClean.

Build with the engine's `plugins-rel NATIVE_PLUGINS="... ui_imgui"` or the
plugin Makefile's exact `fteplug_ui_imgui` target. Adapter compiles at -O2;
unmodified vendor compiles at -O1 to avoid GCC 16's upstream mouse-array
assert/range diagnostics. Both use -Wall -Wextra -Werror, no warning suppression.
Windows links libgcc/libstdc++ statically in the plugin; neither runtime becomes
an engine/server dependency. Linux/device/non-GL packaging remains unverified.
Normal development `build.ps1 -Engine` includes/copies this optional plugin to
both Windows installs; loading remains explicit. Release ship-set changes are
not part of this prerequisite and must be gated before wider adoption.

Falsifiers and retained results: FTESurf `tools/p598imgui.md`,
`tools/p598imgui.py`, `tools/p598build.py`, `tools/test_p598imgui_unit.py` and
`tools/fixtures/p598imgui_host.cpp` (real service/adapter/ImGui source).
The provisional native P595 claim collided with surfd-only P595-597. Canonical
native feature is P598; immutable historical patch-595 tag and p595-prefixed
fixture commands/cvars/logs are retained, not rewritten or panel defaults.
