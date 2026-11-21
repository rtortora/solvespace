# SolveSpace MCP Server — Plan and Handoff

Goal: the SolveSpace GUI runs an MCP server, so that Claude (Claude Code, Claude
Desktop, or another MCP client on the same machine) can connect to it and build,
inspect and view models. That covers workplane and 3D sketches, every entity
type, constraints, extrude/lathe/revolve/helix/rotate/translate, reading the
model back, screenshots, camera control, and group and display visibility.

This file is the handoff document. §0 is the current state, and §5 is what to
do next.

---

## 0. Status at a glance

| | |
|---|---|
| Branch | `mcp-server` (from `main`; the upstream main branch is `master`) |
| Done | Milestone 0 (vertical slice) and milestones 2–5 (documents/groups, sketching, constraints, 3D operations). 45 tools. |
| Not done | Milestone 1 (settings UI, token), **5b (modelling ergonomics; next, see §5)**, 6 (camera/display), 7 (selection/analysis/export), 8 (headless/CI) |
| Tests | `test/mcp/mcp_tests.py`: 13 end-to-end tests against a live GUI, all passing. SolveSpace's own suite (`cmake --build build --target test_solvespace`): 300 cases passing. |
| Field test | Claude modelled a kitchen chair over MCP: legs, seat, back posts, backrest; 9 groups, all fully constrained. The lessons from that session are in §5. |
| Platforms | Only the macOS build has been compiled and run. The GTK, Windows and Qt changes are small and mechanical but have not been compiled. |

### How to run it

```sh
./run-mcp.sh            # incremental Release build, launch with MCP on port 47474,
                        # wait for ping, print the `claude mcp add` command
./run-mcp.sh --port N   # other port; --debug / --clean are passed to run.sh
```

`run.sh` and `run-mcp.sh` are local scripts. They are deliberately **not**
committed (they are listed in `.git/info/exclude`). `run-mcp.sh` calls
`run.sh --no-run` to build, then starts
`build/bin/SolveSpace.app/Contents/MacOS/SolveSpace` with `SOLVESPACE_MCP_PORT`
set.

MCP is off unless one of these is given: `--mcp`, `--mcp=PORT`, or the
`SOLVESPACE_MCP_PORT` environment variable. `run-mcp.sh` uses the environment
variable.

Connect Claude Code once with:

```sh
claude mcp add --transport http solvespace http://127.0.0.1:47474/mcp
```

**After a rebuild that adds or changes tools, reconnect with `/mcp`.**
Claude Code caches the tool list and schemas. A stale session only sees the
old tools, or old schemas for tools it already knew. Extra arguments still
reach the server, because the schemas don't set `additionalProperties: false`.

### How to test

Run tests against a **separate instance on a spare port**, so they don't
disturb a model the user has open. The tests start new documents and discard
unsaved changes.

```sh
SOLVESPACE_MCP_PORT=47475 build/bin/SolveSpace.app/Contents/MacOS/SolveSpace &
python3 test/mcp/mcp_tests.py --url http://127.0.0.1:47475/mcp --out /tmp/mcp-out
python3 test/mcp/mcp_tests.py ... -k constraints     # run a subset
python3 test/mcp/mcp_smoke.py --url ...              # milestone 0 smoke test
```

`--out` collects saved `.slvs` files and screenshots (`extrude_cut.png`,
`revolve.png`, `helix.png`, `translate.png`). Look at them: a test that passes
can still render wrongly.

`test/mcp/mcp_client.py` is a small client that uses only the standard
library. Its methods are:
* `call(tool, **args)`: returns `structuredContent`, and raises `ToolFailed`
  on `isError`.
* `fails(tool, **args)`: expects an error and returns its text.

The tool name is the positional parameter `_tool`, so that tools taking a
`name` argument still work.

### Commits so far

1. `4dc31fed` Milestone 0: HTTP server, job pump, message capture, 5 tools.
2. `96cfa87f` Milestones 2–5, plus fixes from an independent code review.
3. This handoff update.

---

## 1. Architecture (as built)

### 1.1 Transport

* The server uses **Streamable HTTP with JSON responses only**, on
  `127.0.0.1:<port>`.
  * `POST /mcp` takes one JSON-RPC message. Batches are rejected.
  * Notifications get `202`.
  * `GET` returns `405`, so there is no SSE.
  * `DELETE` returns `200`.
* A random `Mcp-Session-Id` is returned on `initialize` and not enforced.
* The supported protocol versions are `2025-11-25`, `2025-06-18`, `2025-03-26`
  and `2024-11-05`. The server echoes the client's version if it supports it.
* Security:
  * The server binds the loopback address only, and the accept loop checks
    the peer is 127/8 as well.
  * An `Origin` header, if present, must be `http(s)://localhost` or
    `http(s)://127.0.0.1` (DNS-rebinding protection).
  * **There is no auth token yet** (Milestone 1).
* `initialize` returns `instructions`: a primer on SolveSpace's model (ordered
  groups, the active group, requests vs. entities, DOF, units, handles, the
  typical workflow). It lives in `tools_model.cpp`, `ServerInstructions`.

### 1.2 Threading

```
[HTTP threads]   accept (one detached thread per connection, keep-alive)
                 → parse → JSON-RPC → tools/call → push Job onto queue (mutex)
[GUI thread]     Platform::Timer pump, re-armed every 15 ms
                 → if Platform::modalDepth > 0: skip (a modal dialog is open)
                 → pop Job, CAS state QUEUED→RUNNING, run tool, fulfil promise
[HTTP thread]    wait 30 s. If still QUEUED → CAS to ABANDONED, return "UI busy,
                 not executed". If RUNNING → wait up to 300 s more, then return
                 "outcome unknown".
```

* `Platform::modalDepth` (declared in `gui.h`, defined in `gui.cpp`) is
  incremented by a `ModalScope` in **every** platform's `RunModal`/`ShowModal`.
  Without this, the pump would run tools *inside* the GUI's modal loops,
  because macOS timers are in `NSRunLoopCommonModes` and Win32/GTK modal loops
  dispatch timers. That runs a tool while the code that opened the dialog
  holds pointers into the sketch. The Qt message box has not been audited.
* `initialize`, `ping` and `tools/list` are answered on the HTTP thread. The
  tool registry is built once, on the GUI thread, before the listener starts.

### 1.3 A tool call (`CallToolOnGuiThread`, mcpserver.cpp)

1. **Message capture.** `activeMessageCapture` (util.h/util.cpp) is set, so
   `Error()`/`Message()` append text instead of opening a dialog. Captured
   errors make the result `isError`; captured messages are returned as
   `messages`. `LocateImportedFile` (file.cpp) also refuses to prompt while a
   capture is active, and says the linked file is missing instead.
2. **The user's pending drawing operation is cancelled** the way Esc does it,
   including the undo for half-drawn lines, rectangles, circles and arcs.
3. **The handler runs.** Tools throw `ToolError` to fail. `json::exception`
   becomes "Invalid arguments: ...".
4. **The UI is refreshed.** The call ends with `SS.GW.Invalidate()` and
   `SS.ScheduleShowTW()`.
5. **Result.** `structuredContent` plus the same JSON as `text` content, and any
   `image` content. Errors are a single text item.

### 1.4 Writing a tool (patterns in `mcptools.h` / `tools_common.cpp`)

* **`Transaction t;` … `t.Commit();`**
  * Records one undo step.
  * On commit, it merges any extra undo steps that GUI functions pushed
    (`MenuGroup`, `MenuConstrain`, `ScreenChangeGroupOption`,
    `EditControlDone` all call `UndoRemember`), so one tool call is one Ctrl+Z.
  * If destroyed without a commit (an exception), it undoes back to the
    starting state, clears redo, and restores `SS.unsaved`.
* **Driving through the selection:** use this for constraints and groups.
  `SelectOnly(handles)`, then call `Constraint::MenuConstrain(cmd)` or
  `Group::MenuGroup(cmd)`, then `ThrowIfErrors()`, then find what was added by
  comparing before and after. This reuses SolveSpace's own rules about what a
  selection means, and its error messages, which list the valid selections.
* **Direct construction:** use this for sketch entities.
  `SS.GW.AddRequest(type, false)`, then `PointForceTo` and similar, then the
  same automatic constraints the mouse code adds. The point layouts are
  documented in the code (rectangle, arc, cubic, text/image boxes).
* **Values** are applied through the GUI's own label editor
  (`GraphicsWindow::EditControlDone`, `TextWindow::EditControlDone`), with
  `SS.viewUnits` temporarily set to mm. Expressions and new named parameters
  therefore behave exactly as typed in the GUI. Numbers are formatted without
  exponents, because the expression lexer rejects `5e-05`.
* **`ParseEntity`** rejects entities in groups *after* the active group. Those
  are hidden, and a constraint or group that refers to them gets pruned on
  regeneration, which used to crash on stale pointers.
* **Regenerating.** `Regenerate()` uses `Generate::DIRTY`, which only solves
  up to the active group. When editing a later group, use `RegenerateFor(hg)`.
* **After `GenerateAll`, `MenuGroup` or `AddRequest`, re-fetch `Group*` /
  `Entity*` / `Request*`.** The underlying lists reallocate.

### 1.5 Wire conventions

* **Handles:** `g:0002` group, `r:0004` request, `e:00040001` entity,
  `c:0007` constraint. Entity = `request<<16 | index`. Entities generated by
  a group look like `e:8005002a`.
* **Units:** lengths in **mm** and angles in **degrees**, always.
* **Coordinates:** `[u, v]` in the active workplane, or `[x, y, z]` in 3D
  sketches. Results give `xyz`, plus `uv` for workplane entities.

### 1.6 Code layout

```
extlib/nlohmann/json.hpp        vendored, MIT, v3.11.3 (in THIRD_PARTIES.txt)
src/mcp/httpserver.{h,cpp}      loopback HTTP/1.1 server (POSIX + Winsock)
src/mcp/mcp.h, mcpserver.cpp    JSON-RPC, lifecycle, job queue, pump, call scope
src/mcp/mcptools.h              shared helper declarations, Transaction
src/mcp/tools_common.cpp        handles, args, JSON for groups/entities/constraints,
                                Transaction, tool registry (GetTools)
src/mcp/tools_model.cpp         get_model_summary, get_group, list_entities,
                                screenshot, ServerInstructions
src/mcp/tools_document.cpp      new/open/save, undo/redo, group management, delete
src/mcp/tools_sketch.cpp        sketch groups and all entity types, fillet, split,
                                construction, move_point
src/mcp/tools_constraint.cpp    constraints, solve status, parameters, relations
src/mcp/tools_ops.cpp           extrude, lathe, revolve, helix, rotate/translate
                                copies, link_file
src/util.{h,cpp}                MessageCapture hook in MessageBox()
src/file.cpp                    LocateImportedFile: no prompt while capturing
src/platform/gui.{h,cpp}        Platform::modalDepth / ModalScope
src/platform/gui*.{cpp,mm}      ModalScope in each RunModal/ShowModal
src/platform/entrygui.cpp       --mcp / env parsing, StartServer/StopServer
CMakeLists.txt, src/CMakeLists  ENABLE_MCP option; GUI target only (not Qt or
                                Emscripten); adds render/rendercairo.cpp for
                                screenshots
test/mcp/                       mcp_client.py, mcp_smoke.py, mcp_tests.py
```

---

## 2. Tool reference (45 tools)

| Area | Tools |
|---|---|
| Read | `get_model_summary`, `get_group`, `list_entities {group?, kind: all/curves/points/faces/workplanes}`, `list_constraints`, `get_solve_status` (with free points), `list_parameters`, `screenshot {width, height, zoom_to_fit}` |
| Document | `new_document {discard_unsaved}`, `open_file {path}`, `save_file {path?}`, `undo {steps}`, `redo {steps}` |
| Groups | `activate_group`, `set_group_visibility {group \| groups, visible}`, `set_group_properties {name, color, opacity, combine, sides, skewed, suppress, relax_constraints, allow_redundant, count, skip_first}`, `delete_group` |
| Sketch groups | `create_workplane_sketch {plane \| point+plane \| point+normal \| point+line_a+line_b \| workplane}`, `create_3d_sketch` |
| Entities | `sketch_point`, `sketch_line {points, closed}`, `sketch_rectangle`, `sketch_circle {normal in 3D}`, `sketch_arc {clockwise}`, `sketch_cubic {points, start/end_control, periodic}`, `sketch_text`, `sketch_image` (PNG only), `sketch_workplane` (3D only), `fillet {point, radius}`, `split_curves`, `set_construction`, `move_point`, `delete {handles}` |
| Constraints | `add_constraint {type, points, entities, value, reference, supplementary}` (types: distance, diameter, angle, coincident, on_entity, equal, ratio, difference, horizontal, vertical, parallel, tangent, perpendicular, symmetric, midpoint, same_orientation, lock), `set_constraint_value`, `set_constraint_reference`, `add_comment`, `set_parameter`, `add_relation` |
| 3D ops | `extrude {depth, sides, combine, constrain_depth}`, `lathe {axis, axis_point}`, `revolve {angle_deg}`, `helix {angle_deg, pitch \| axial_travel}`, `rotate_copy {center, axis, count, angle_deg}`, `translate_copy {step, count}`, `link_file {path}` |

Behaviour worth knowing:
* A change that makes a previously solving sketch fail is rolled back. The
  error names the conflicting constraints.
* A redundant dimension is turned into a reference dimension, as the GUI does.
* `extrude` constrains its depth by default, so the group has 0 DOF.
* `revolve` and `helix` leave 1 DOF, because the angle is not constrained.
* `fillet` uses SolveSpace's approximate tangent-arc tool, so the radius is
  within about 0.1%.
* `rotate_copy` and `translate_copy` inherit `combine` from their source group.
  SolveSpace forces this.
* Parameter scaling, verified against `Group::Generate` and by measurement:
  * extrude depth = 2·|vector|
  * translate step = 2·params
  * rotate and revolve angle = 4·param3
  * helix travel = 2·param7

---

## 3. Lessons learned (read before changing code)

* **nlohmann/json gotcha:** `json x = {someJsonObject};` produces the *object*,
  not a one-element array. Use `json::array({...})`. This caused a real bug.
* **Every GUI path that can show a dialog** is a hazard: file dialogs, the
  save/autosave prompts (`OkayToStartNewFile`, `LoadAutosaveFor`),
  `LocateImportedFile`, `ReloadLinkedImage` with non-PNG files, and
  `TANGENT_ARC` with nothing selected (it opens a text-window screen, which is
  harmless). The tools avoid these on purpose:
  * `open_file` calls `LoadFromFile` directly.
  * `sketch_image` checks with `Pixmap::ReadPng` first.
* **Group order matters.** Anything a tool references must be in the active
  group or an earlier one. `UseSourceSketch` (ops) activates the source sketch
  *before* parsing the axis handles.
* **The camera follows the user, not the tools.** `MenuGroup` animates the view
  onto each new workplane (asynchronously, through a timer). The `screenshot`
  tool uses the current GUI camera, so after sketching on XY planes every
  screenshot is a top-down view. Milestone 6 fixes this. In the chair session
  the workaround was to create a YZ sketch to turn the view, then `undo`.
* **The client caches tool schemas.** See the reconnect note in §0.

---

## 4. Completed milestones (summary of what was asked for and what changed)

* **M0, vertical slice.** As planned.
* **M2, documents and groups.** `list_groups` and `get_document_info` were
  folded into `get_model_summary`. `delete` was added here.
* **M3, sketch entities.** The mouse code was **not** refactored; the tools
  repeat its point placement instead, which carries no regression risk for the
  GUI. The tangent arc tool is called `fillet`. Sketching on a face means a
  vertex of the face plus the reference plane it is parallel to.
* **M4, constraints.** `where_dragged` became `lock`, and `other_angle` became
  the `supplementary` flag. Comments have their own tool. Constraints are
  deleted with `delete`.
* **M5, 3D operations.** Faces are listed with `list_entities kind=faces`.
  Skewed extrusions are switched on with `set_group_properties`.

---

## 5. Next steps (in priority order)

### Milestone 5b — Modelling ergonomics (do this first)

These come from Claude actually modelling a chair through the tools. Each one
removed friction or wasted context in that session. All of them are additions
inside `src/mcp/` and need no changes to SolveSpace itself.

1. **Sketch at a height or offset, without hunting for a vertex.** For the
   seat and the backrest, Claude had to list about 70 points (`list_entities
   kind=points`) to find one at z=420 or z=900, just to anchor a workplane.
   * Add `create_workplane_sketch {plane: "XY", offset: 420}` (the reference
     plane moved along its normal) and `{origin: [x,y,z], plane}`.
   * Implementation: a `WORKPLANE_BY_POINT_ORTHO` group needs an origin
     *point entity* (`predef.origin`). Either build the group directly (the
     `predef.q` from the reference plane, plus an origin point), or first
     create a small hidden "datums" 3D sketch group holding a datum point, then
     select that point plus the plane orientation, as is done today.
   * Choose the option that keeps the result parametric (the offset as a
     dimension that can be edited later).
2. **Sketch on a face, by face.**
   * Add `create_workplane_sketch {face: "e:..."}`. Compute the frame from the
     face normal and point; for the origin, use a vertex on the face or the
     point the face entity carries (`FaceGetPointNum`).
   * Add `find_faces {group?, normal?: "+z"|[x,y,z], near?: [x,y,z]}`. It
     returns a few faces (handle, normal, point, rough bounds), not every
     entity.
   * The common request "sketch on the top of the seat" should take one call.
3. **Compact results by default.**
   * A `sketch_rectangle` result was about 2 KB, mostly repeated point JSON.
     Return request handles, key point handles with their `uv`, and the solve
     status. Keep the full form behind `verbose: true`.
   * Do the same for `list_entities`. Extruded groups list each vertex
     several times (once per edge copy), so deduplicate by position, or add
     `unique_points: true`.
   * Add an `extrude` result section with the useful generated handles: the
     top and bottom face handles, and a top-face vertex. That lets the next
     sketch chain on without a lookup.
4. **Fully constrained primitives in one call.** Add `constrain: true` to
   `sketch_rectangle`, `sketch_circle` and `sketch_line`. It adds the
   width/height, diameter or length dimensions, and anchors the shape to the
   workplane origin when a corner or centre is on it. The result is 0 DOF
   without a series of `add_constraint` calls. Also add a `center` +
   `width`/`height` form for `sketch_rectangle`.
5. **Several shapes per call.** Four legs took four `sketch_rectangle` calls.
   Choose between:
   * a `shapes: [...]` array on the sketch tools, or
   * a general `batch {calls: [{tool, args}, ...]}` tool that runs all the
     calls in **one Transaction** (all or nothing, one undo step).

   `batch` is more general, and also covers "sketch + extrude" in one round
   trip.
6. **Views for screenshots without moving the user's camera** (the most urgent
   part of Milestone 6). Add a `view` argument to `screenshot`, one of
   `"iso"|"front"|"back"|"left"|"right"|"top"|"bottom"|"current"`.
   * Build a `Camera` from the preset, call `ZoomToFit(camera)`, and render
     offscreen.
   * Save and restore `SS.GW.offset`/`scale`/`projRight`/`projUp`, as
     `zoom_to_fit` already does.
   * Add `views: [...]` to return several images in one call (for example iso,
     front and side), so one check covers the whole model.
7. **Units in screenshots.** Dimension labels show the user's display units:
   the chair's 160 mm label read "6.299", in inches. Add a
   `units: "mm"|"display"` option to `screenshot`, which forces mm while
   rendering.
8. **Small things noticed:**
   * `get_model_summary` could include the overall bounding box of the solid.
     That gives a cheap sanity check ("is it 900 mm tall?").
   * Group JSON shows the colour as 0.392… floats. Round them, or show hex.
   * Tool descriptions should mention the `offset` and `face` options once they
     exist, so the model reaches for them first.

Add tests to `mcp_tests.py` for each item, and rerun the chair as an
acceptance test: it should take about a third of the calls and much less
context than before. In that session, 16 calls modelled the chair, with three
long point listings.

### Milestone 6 — View, navigation and display (the rest of it)

* `get_view` / `set_view {offset, proj_right, proj_up, scale}`
* `orient_view {preset}`, `zoom`, `zoom_to_fit {selection_only}`, `pan`,
  `rotate_view {axis, angle_deg}`, `center_on {point}`
* `set_perspective`, `set_explode_sketch`, `set_units {mm|in|ft_in|m}`
* `screenshot` options:
  * `source: "window"`, which reads the real GL frame on the next redraw, with
    overlays;
  * `highlight: [handles]`;
  * `shaded`/`hidden_lines`.
* `set_display_options {workplanes, normals, points, construction,
  constraints: none|all|dimensions, faces, edges, outlines, mesh, shaded,
  snap_grid}`. These are the `show*` fields in `ui.h` (~line 860).
* Styles: `list_styles`, `set_style {entities, style}`,
  `set_style_visibility`. These are the closest thing to per-layer visibility,
  together with group visibility, which is already done.

### Milestone 1 — Hardening and settings

* A Configuration screen section:
  * enable MCP, choose the port, set an optional token;
  * a "copy `claude mcp add` command" button;
  * the settings persist (needed on macOS, where Finder launches get no
    arguments).
* Bearer-token check.
* A status indicator, such as "MCP: connected (Claude Code)", from
  `clientInfo`.
* Shutdown at exit: connection threads are detached and can still touch the
  static `queue`/`queueMutex` during static destruction. Join them, or give
  them a shared lifetime.

### Milestone 7 — Selection, picking, analysis and export

* `select`, `clear_selection`, `get_selection` (so the user can say
  "constrain what I've selected").
* `pick_at {x, y}` on the last screenshot.
* `measure {volume|area|perimeter|center_of_mass}`, `check_interference`,
  `check_naked_edges`, `get_bounding_box`.
* `export {step|stl|obj|dxf|svg|pdf|png, path}`, using the `Export*` functions
  with explicit paths, so no file dialogs open.
* `run_command {name}`: an escape hatch, with a blocklist for commands that
  open dialogs.

### Milestone 8 — Headless mode, CI and MCP resources

* `solvespace-cli mcp [file]`: the same registry over stdio, with no window.
  Screenshots already use Cairo. The `test/` harness could then run
  `mcp_tests.py` scenarios in CI.
* Resources: `solvespace://model` (JSON summary) and `solvespace://file`
  (`.slvs` text). A `model-a-part` prompt.

---

## 6. Known gaps and risks

* **Not tested by hand:** a tool call while the user has a modal dialog open.
  It should wait, then run when the dialog closes, or report "UI busy, not
  executed" after 30 s.
* **Other platforms:** only macOS has been built. The `ModalScope` additions in
  `guigtk*.cpp`, `guiwin.cpp`, `guiqt.cpp` and `guihtml.cpp` have not been
  compiled. The Qt `MessageDialog` path has not been checked.
* **Undo ring:** when it is full (`MAX_UNDO` = 100), a call whose GUI path
  pushes an extra undo step overwrites one more old history entry. That is
  minor.
* **Redo is cleared** by any modifying tool call, as by any GUI edit.
* **Error capture coverage:** new tools must not call code that opens file
  dialogs or save prompts. See §3.
* **Screenshot fidelity:** the Cairo render differs slightly from the GL
  window, and has no toolbar.
