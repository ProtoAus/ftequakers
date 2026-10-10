# Optional explicit-draw Dear ImGui service

Pinned upstream: **Dear ImGui 1.91.9b**, Git tag `v1.91.9b`, commit
`f5befd2d29e66809cd1110a152e375a7f1981f06`.
`vendor/` contains unmodified upstream core/headers and its MIT `LICENSE.txt`;
`vendor/SHA256.json` pins every vendored file. No runtime code/font downloads.
Since Patch 614 it also holds **ImPlot 1.0** (`epezent/implot` tag `v1.0`, commit
`524f9fcd48d76c13fdf94c5ffbba8787a1ff7e39`): `implot.h`, `implot.cpp`,
`implot_internal.h` and its MIT `LICENSE-implot.txt`, byte-identical to that
commit's blobs and pinned the same way. See "Run graphs" below.

This began as a **diagnostic gallery / explicit transport prerequisite**. Two
FTESurf panels are now drawn through it, each with a QC panel behind it that
draws when anything here is missing or refuses: owner 205, the scoreboard's
ranked table (Patch 605, opt-in), and owner 206, the run graphs (Patch 614).
The editor and the menus are not. Passive owners 101 (MQC) and
202 (CSQC) use the unchanged `NativeUI/1` host; optional interactive owners
103/204 require additive `NativeUIInput/1`; optional `NativeUIModel/1` supplies
counted diagnostic snapshots. No Sbar/Menu/Tick drawing hook. Loaded but
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

## Run graphs: counted sample series and owner 206 (Patch 614)

`NativeUIPlot/1` is a third additive table from the same provider (`plugin.h`):
`SetPlot` takes a revision of up to 16 series, each a run of rows `x, a, b` and a
break flag (65,536 rows a series, 589,824 a revision); `SetView` takes what moves
between revisions (two 16-bit series masks, hidden and emphasis; a marker's x; the
caller's caption size in physical pixels; a serial that asks for an x range once).
Numbers only: no string crosses this interface. An older host rejects the table
and owner 206 then refuses to open; an older provider leaves the builtins saying
no. Either way QC draws its own plots.

QC stages through six named CSQC/MQC builtins inside its draw callback:
`ui_native_plot_status()`, `_begin(handle, revision, series)`,
`_series(handle, index, [flags, gap, 0], rgb)`, `_rows(handle, index, x*, a*, b*,
brk*, count, xoffset)`, `_commit(handle)` and `_view(handle, [x, shown, px],
[hidden, emphasis, 0], [serial, x0, x1])`. `_rows` is the first native UI builtin
handed QC POINTERS. The host resolves each with the VM's own bounds check
(`PR_PointerToNative_MoInvalidate`, `count` floats at offset 0), reads through
`memcpy` (a QC pointer need not be aligned), copies into its own arrays and runs
`PlugUI_PlotValid` on the copy before the provider is called; the provider runs
it again. Every number must be finite and within `PLUGUI_PLOT_MAX_VALUE` (1e9
either way; x is judged less its offset, so a late clock with a short run
passes), and x must not decrease. Finite alone was the first cut: at 1e30 a
flat view cannot be padded and a hover failed the frame. A bad call poisons its
transaction, exactly as the model's does; a VM may begin two a frame.

Owner 206 (CSQC) is FTESurf's run-graph panel: two ImPlot plots on one linked x
axis (a = speed above, b = energy below) inside the inherited clip, drawn in
FTESurf's `Dusk` palette. FTESurf keeps the panel, the legend chips, the cursor's
readout and every number in them; this owner draws curves, axes and the view,
and reports back through `NativeUIInput/1` actions after each Draw: 1 and 2 the
visible x range, 3 the x under the cursor (absent off the plots), 4 the points it
drew, 5 the plots cut afresh. The player's wheel zooms about the cursor, a drag pans, a right-drag
selects a range, the right button alone or a double click shows everything, and
the bar between the plots drags. A clip under 160x120 returns false.

- What comes back is bounded to what the host takes. It closes an owner over an
  action outside 1e6 either way, so every report is clamped to that; and a
  range request is cut to the run before ImPlot sees it, because ImPlot's own
  limits took two frames to bring one from past the end back inside (the
  fixture's mutant fails on both) and what was shown meanwhile was reported.
  A vertex is bounded before it
  is a float (the host refuses a mesh past 1e7 pixels, and the row beside a
  zoomed view can map a billion seconds away), and a marker outside the view is
  not drawn.
- The plugin is built with asserts live. For this owner ImGui's recoverable-error
  assert, log and tooltip are off and `PlotsError` fails the frame instead, so
  a usage error costs the native plots and not the process. Every other owner
  still asserts.
- ImPlot zooms one step for a frame's wheel whatever its size (`implot.cpp:2026`),
  and ImGui takes a wheel queued after a move a frame later. A caller sends one
  event a frame, a notch at a time.
- Curves are this owner's own strips on the plot's draw list, not ImPlot items:
  ImPlot's line renderer wants `AntiAliasedLinesUseTex`, which this backend's
  atlas filtering cannot promise, and its item templates (`implot_items.cpp`,
  6.9 MB of objects) are neither vendored nor built. `ImPlot::BustItemCache`,
  the one symbol `implot.cpp` needs from that file, is an empty function in
  `ui_imgui.cpp`.
- A point costs: 4 vertices and 18 indices as an anti-aliased strip, each index
  a vertex again after the backend's expansion, each triangle through the
  host's clipper. Drawn two to a pixel column, three 50 s runs were 11,752
  points and took a 1.6 ms frame to 9-16 ms on the desktop PC. So a series is
  cut by a swing filter to the fewest vertices that stay within 0.25 px,
  vertically, of every row they replace (`PlotSwing`; measured at 0.250 px by
  the fixture), and the strips are kept per plot until the revision, the series
  shown, either axis or the plot's rectangle changes: a view that is only being
  read is not cut again (action 5 reports how many of the two plots were). The
  y axis is fitted to the rows in view rather than to the surviving vertices,
  or the fit would move with the cut and nothing could be kept. A plot's pool
  is 6,000 points and 1,024 strips; a series that would take more than its
  share (noise, or far more rows than pixels) is cut to each pixel column's low
  and high first. Nine full series of noise draw 12,000 points.
- Axis labels are `fonts/roboto_ascii.h`: Roboto Regular 3.015 (SIL OFL 1.1,
  `fonts/OFL.txt`; no Reserved Font Name) cut to its default instance and
  U+0020..U+007E, 10,596 bytes, baked at eight sizes at Open. It is a Modified
  Version in the licence's terms and keeps the font's own copyright and licence
  records. Made with fontTools 4.53.1 from FTESurf `ftesurf/gfx/fonts/Roboto.ttf`
  (sha256 0fe599ab...340fc5) by `instancer.instantiateVariableFont` at the axis
  defaults, then `subset` with no layout features, no hinting, every name record
  and unchanged timestamps; the header carries both hashes and the output repeats
  byte for byte. The scoreboard owner's font is unchanged.
- This owner's ImGui clock is real time, bounded to 0.5..250 ms a frame, because a
  double click and the splitter's hover delay are measured in it. Every other
  owner still advances 10 ms a Draw.

The host's mesh path (`engine/client/cl_plugin_mesh.inc`) changed with this
owner, for every owner: a triangle wholly inside its clip skips the four
clipping passes (which would return it unchanged), and a backend draw carries up
to 4,096 vertices where it carried 512.

Falsifiers: FTESurf `tools/p614plots.py` (the panel through a live client, on
its own status lines and on pixels; hostile transactions through the real
builtins; an older plugin, an older engine and no plugin),
`tools/test_p614plot_unit.py` (the host bridge over a small VM, the shared
grammar against a table of malformed revisions, and this provider under a fake
host at both index widths) and `tools/p614mutants.py` (one edit to a copy of
this tree a mutant, 106 of them; each fixture must fail on its own, and a
mutant that does not build is not a catch). The older suites build against this tree with their
fixtures taught the new table (`tools/fixtures/p590bridge_host.c`,
`p598imgui_host.cpp`).

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
