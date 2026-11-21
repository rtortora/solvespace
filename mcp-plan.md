# SolveSpace MCP Server — Implementation Plan

Goal: the SolveSpace GUI app runs an MCP server, so that Claude (Claude Code, Claude
Desktop, or another MCP client on the same machine) can connect to it and build,
inspect and view models: create workplane and 3D sketches, draw every kind of
entity, add constraints, extrude/lathe/revolve/helix/rotate/translate, read the
model back, take screenshots, move the camera, and switch group and display
visibility.

Delivery is staged. **Milestone 0 is a vertical slice** that shows the whole path
working end to end: Claude connects, creates a workplane sketch, draws a
rectangle, reads it back and takes a screenshot of it. Work stops after Milestone
0 so it can be tested by hand before anything else is built.

---

## 1. Architecture

### 1.1 Transport: Streamable HTTP on localhost

* The server listens on `127.0.0.1:<port>` and nowhere else. The default port is
  `47474`.
* It uses the MCP **Streamable HTTP** transport with plain JSON replies:
  * `POST /mcp` takes one JSON-RPC message and returns `application/json`, or
    `202 Accepted` for notifications.
  * `GET /mcp` returns `405 Method Not Allowed`. The spec allows this, and it
    means no SSE stream is needed.
  * The server sets an `Mcp-Session-Id` on `initialize`. It is checked loosely,
    since there is a single user on the machine.
* Why this transport: a GUI process can't be launched by the client over stdio,
  because the user has already started it. HTTP lets Claude attach to the window
  that's already running:
  `claude mcp add --transport http solvespace http://127.0.0.1:47474/mcp`
* Security. This matters, because any local web page can reach localhost.
  * Bind only to the loopback address.
  * Reject requests whose `Origin` header is present but isn't
    `http://127.0.0.1` or `http://localhost`. The spec requires this check to
    stop DNS rebinding.
  * An optional bearer token can be required, set as `mcp.token`.
  * The server is **off by default**. It is switched on with the `--mcp[=PORT]`
    command-line flag, the `SOLVESPACE_MCP_PORT` environment variable, or a
    checkbox in the Configuration screen. The checkbox setting is saved, which
    matters on macOS: an app launched from Finder gets no command-line
    arguments.

### 1.2 Threading model

SolveSpace isn't thread-safe. `SK`, `SS` and `SS.GW` are globals that only the
GUI thread may touch. So:

```
[listener thread]  accept → parse HTTP → JSON-RPC → push Job{request, promise}
                                                      │  (mutex + deque)
[GUI main thread]  Platform::Timer pump (re-armed every ~15 ms)
                   drains the queue → dispatches the tool → fulfills the promise
[listener thread]  future.wait_for(30 s) → writes the HTTP response
```

* The pump uses the existing cross-platform `Platform::CreateTimer()`
  ([gui.h:142](src/platform/gui.h#L142)), so the same code works on macOS,
  GTK, Windows and Qt, with no per-platform main-queue code. Polling every
  15 ms costs nothing noticeable.
* If a modal dialog is open, the pump may not fire, because macOS timers don't
  run in modal run-loop mode. The worker then times out and returns
  `"SolveSpace UI is busy (modal dialog open)"`. Nothing deadlocks.
* Requests are handled one at a time on the main thread. MCP clients don't
  depend on parallel tool calls.

### 1.3 Executing a tool on the main thread

Every tool call runs inside a `McpCallScope`, which takes care of the
following:

1. **Capturing errors instead of showing dialogs.** `MessageBox()` in
   [util.cpp](src/util.cpp) gets a hook: while a scope is active, the text of
   `Error()` and `Message()` is added to the scope instead of opening a modal
   dialog. Errors are returned as `isError: true` tool results. Messages are
   returned as warnings. Without this hook, SolveSpace's own checks (such as
   "Can't draw rectangle in 3d…") would freeze the session behind a dialog that
   only a human can dismiss.
2. **Undo.** `SS.UndoRemember()` is called once per mutating call, so a whole
   tool call can be undone with a single Ctrl+Z in the GUI.
3. **Solving synchronously.** After a mutation the scope calls
   `SS.GenerateAll(Generate::DIRTY)` directly. It doesn't rely on
   `ScheduleGenerateAll()`. That way the result contains solved coordinates,
   the solve status and the remaining DOF, and the next read sees the same
   state.
4. **Refreshing the UI.** The scope calls `SS.GW.Invalidate()`,
   `SS.TW.Show()` and `SS.ScheduleShowTW()`, so the user sees each change as it
   happens.

### 1.4 How mutations are implemented

There are two patterns. Each tool uses whichever one is safer for it.

* **Direct construction**, for sketch entities. The interactive drawing code in
  [mouse.cpp](src/mouse.cpp) is driven by mouse state, so it can't be called
  directly. The tools repeat its steps instead: `AddRequest(type, false)`, then
  `PointForceTo`/`DistanceForceTo`/`NormalForceTo`, then the automatic
  constraints the GUI adds (for example the 4×coincident + H/V set for a
  rectangle, [mouse.cpp:1016](src/mouse.cpp#L1016)). The steps are pulled out
  into shared helpers where possible, so the mouse path and the MCP path call
  the same code.
* **Driving through the selection**, for constraints and groups.
  `Constraint::MenuConstrain(Command)` and `Group::MenuGroup(Command)` already
  contain the rules about what a given selection means: point-point distance
  vs. point-line distance vs. diameter, workplane-by-point vs. by-two-lines,
  extrude-along-normal, and so on. Rewriting those rules would be a copy that
  could drift from the original. The tool clears the selection, calls
  `SS.GW.MakeSelected(...)` on each handle it was given, calls the menu
  function, and works out the new constraint or group by comparing what existed
  before and after. Any error the menu function raises is captured as in §1.3.

### 1.5 Wire conventions

* **Handles** are strings with a type prefix: `"g:0002"` (group), `"r:0004"`
  (request), `"e:00040001"` (entity), `"c:0007"` (constraint), `"s:0001"`
  (style), `"p:..."` (param). The prefix catches the common mistake of passing
  a constraint handle where an entity was expected. Entity handles encode their
  request as `request<<16 | index`; the docs explain this so the model can tell
  that `e:00040001` and `e:00040002` are the two endpoints of line `r:0004`.
* **Units**: all lengths are in **mm** and all angles in **degrees**, whatever
  unit the UI shows. Read results also include `display_units`.
* **Coordinates**: points in workplane tools are `[u, v]` in that workplane's
  2D frame. Points in 3D tools are `[x, y, z]`. Read results include both where
  both apply.
* **Results**: each result carries a JSON payload in `structuredContent`, and
  the same JSON as `text` content for clients that don't read structured
  content. Screenshots come back as `image` content (base64 PNG).
* **Server `instructions`**: the reply to `initialize` includes a short guide
  to SolveSpace's model: groups are applied in order; you sketch in the active
  group; workplane vs. 3D; how DOF works; the handle format. Without it, an LLM
  will usually assume a feature tree with editable history, which SolveSpace
  isn't.

### 1.6 Code layout

```
extlib/nlohmann/json.hpp           vendored single header (MIT), C++11-compatible;
                                   added to THIRD_PARTIES.txt
src/mcp/httpserver.{h,cpp}         minimal HTTP/1.1 server: POSIX sockets + Winsock,
                                   Content-Length bodies only, keep-alive optional
src/mcp/mcpserver.{h,cpp}          JSON-RPC routing, initialize/ping/tools/*,
                                   session id, Origin check, job queue + pump
src/mcp/mcpcontext.{h,cpp}         McpCallScope, handle encode/decode, error helpers,
                                   JSON <-> Vector/Quaternion
src/mcp/tools.h                    ToolRegistry: name, description, input JSON schema,
                                   handler(json args, McpResult&)
src/mcp/tools_model.cpp            read-back tools
src/mcp/tools_sketch.cpp           entity creation
src/mcp/tools_constraint.cpp
src/mcp/tools_group.cpp            workplanes, 3D sketches, extrude, lathe, ...
src/mcp/tools_view.cpp             camera, screenshot, visibility
src/mcp/tools_file.cpp             new/open/save/export
test/mcp/mcp_smoke.py              stdlib-only client used by the test plan
```

The code is linked **only into the `solvespace` GUI target**
([src/CMakeLists.txt:262](src/CMakeLists.txt#L262)), behind a CMake option
`ENABLE_MCP` (default `ON` for native builds, `OFF` for Emscripten). The server
is started in `entrygui.cpp` after `SS.Init()` and stopped before `SS.Clear()`.
Milestone 8 adds a headless stdio mode to `solvespace-cli`.

The HTTP layer is written by hand, about 300 lines. It only needs to handle
single POSTs on loopback, and pulling in cpp-httplib would mean dealing with
its compiler requirements and a large header. If the hand-written version turns
out to be fragile, it can be replaced later without touching anything above it.

---

## 2. Milestone 0 — Vertical slice (stop here for manual testing)

**Status: implemented and awaiting manual testing.** Code is in `src/mcp/` and
the hooks are in `src/util.cpp` (message capture) and
`src/platform/entrygui.cpp` (startup). `test/mcp/mcp_smoke.py` passes
against a live GUI.

**Scope: the infrastructure from §1, kept minimal, plus five tools.**

| Tool | Input | Behaviour |
|---|---|---|
| `get_model_summary` | – | Groups in order (handle, name, type, visible, active). Active group and workplane. Entity and constraint counts per group. Solve status and DOF of the active group. Document file name and unsaved flag. |
| `list_entities` | `group?` (default: active) | Requests and entities. For each: handle, type, construction flag, workplane. Point coordinates as 3D and, if in a workplane, 2D. Line endpoints and length. Circle centre and radius. |
| `create_workplane_sketch` | `plane: "XY"\|"YZ"\|"ZX"`, `name?` | Selects the reference workplane (`Request::HREQUEST_REFERENCE_*`) and calls `Group::MenuGroup(Command::GROUP_WRKPL)`, which creates a `DRAWING_WORKPLANE` group and makes it active. Returns the group handle and its workplane entity. |
| `sketch_rectangle` | `corner1: [u,v]`, `corner2: [u,v]` (mm), `construction?` | Requires an active workplane; otherwise returns a captured error. Same construction as [mouse.cpp:1016](src/mouse.cpp#L1016): 4 line segments, 4 coincident, 2 horizontal + 2 vertical, corners placed exactly. Then solves. Returns the 4 line handles, the 4 corner point handles and the resulting DOF (expected: 4). |
| `screenshot` | `width?=800`, `height?=600`, `zoom_to_fit?=false` | Draws offscreen with `CairoPixmapRenderer` using the current GUI camera. This is the same path as `solvespace-cli thumbnail` ([entrycli.cpp:236](src/platform/entrycli.cpp#L236)). Returns a PNG `image`. Needs a small `Pixmap::WritePng` to memory (or a temp file plus a read) and a base64 helper. The GUI's own `screenshotFile` path isn't used, because it only fires on the next redraw. |

**Infrastructure included:** `--mcp[=PORT]` flag and env var (no settings UI
yet); HTTP server; `initialize`/`notifications/initialized`/`ping`/`tools/list`/
`tools/call`; job queue + timer pump; `McpCallScope` with error capture,
undo and synchronous solve; handle encoding; Origin check.

**Not included yet:** settings UI, auth token, every other tool, automated tests.

### Acceptance test (manual)

1. `./run-mcp.sh` builds incrementally and launches with MCP turned on. It
   waits for the server to answer a `ping`, then prints the `claude mcp add`
   command. `--debug`, `--clean` and `--port N` are passed through to the build
   and launch.
2. `claude mcp add --transport http solvespace http://127.0.0.1:47474/mcp`,
   then `/mcp` in Claude Code shows `solvespace` connected with 5 tools.
3. Ask Claude: *"Create a sketch on the XY plane and draw a 40×20 mm rectangle
   with one corner at the origin, then show me a screenshot."*
   * The GUI shows a new `sketch-in-plane` group with the rectangle in it.
   * `list_entities` reports corner points at (0,0), (40,0), (40,20), (0,20).
   * The screenshot shows the rectangle.
   * One Ctrl+Z in the GUI removes the rectangle.
4. Ask Claude to draw a rectangle **before** creating a workplane, starting
   from a fresh document. It should get back a tool error saying a workplane
   is needed, and no modal dialog should appear in the GUI.
5. `python3 test/mcp/mcp_smoke.py` runs the same sequence without Claude:
   it sends raw JSON-RPC and saves the PNG to a path given as an argument.
6. Leave the GUI on a modal dialog (e.g. File → Open) and call a tool. It
   should time out with the "UI busy" error, and the app should keep working
   once the dialog is closed.

---

## 3. Later milestones

Each milestone ships on its own and gets a short manual check plus smoke-script
coverage. The numbering is a suggested order, not a strict dependency chain,
except where noted.

### Milestone 1 — Hardening and settings

* A Configuration screen section: enable MCP, port, optional token, and a
  "copy `claude mcp add …` command" button. The settings persist.
* Bearer-token check. Clear error when the port is already in use, without
  crashing.
* Request size limits, timeouts, and graceful shutdown of the listener on exit.
* A status bar or text-window indicator showing "MCP: connected (Claude Code)",
  using `clientInfo` from `initialize`.
* `notifications/tools/list_changed` isn't needed, because the tool set is
  fixed.

### Milestone 2 — Document and group management (the "layers" story)

SolveSpace has **no layers**. The nearest equivalents are: (a) group visibility,
(b) per-style visibility, and (c) the global display toggles. Milestone 2
covers (a). Milestone 6 covers (b) and (c).

* `new_document {discard_unsaved: bool}`: refuses if there are unsaved changes,
  unless `discard_unsaved` is true. It never shows the save prompt.
* `open_file {path}`, `save_file {path?}`, `get_document_info`.
* `list_groups`, `get_group {group}`: full parameters, including the
  `solid model as` mode, colour, opacity, suppress flag, skip-first and so on.
* `activate_group {group}`: also makes its workplane active.
* `set_group_visibility {group, visible}`, `set_group_properties {group,
  name?, color?, opacity?, combine?: "union"|"difference"|"intersection"|"assemble",
  suppress?, relax_constraints?, ...}`
* `delete_group {group}`, `undo`, `redo`.

### Milestone 3 — Sketch entities (all request types)

All tools take an optional `construction` flag and return the new request and
entity handles. Workplane tools take `[u,v]`; when the active group is a 3D
sketch, they take `[x,y,z]` instead.

* `sketch_point`
* `sketch_line {points: [[u,v], ...], closed?}`: polyline, with the joints made
  coincident as in the GUI.
* `sketch_circle {center, radius}`
* `sketch_arc {center, start, end}`: same point layout and fudge factor as
  [mouse.cpp](src/mouse.cpp).
* `sketch_cubic {points: [...], periodic?}`: Bézier or spline.
* `sketch_text {text, font?, origin, height}` (TTF) and `sketch_image {path, ...}`.
* `sketch_workplane {origin, normal}`: a datum workplane request.
* `sketch_tangent_arc {point}` (fillet at a shared vertex), `split_curves
  {entities}`
* `set_construction {entities, construction}`, `delete {handles}`.
* `move_point {point, to}`: equivalent to dragging, followed by a re-solve.
* **3D sketching**: `create_3d_sketch` (`GROUP_3D`). The tools above then take
  3D coordinates. Line, point, cubic and workplane requests are allowed here.
  Arc and rectangle return the same error the GUI gives.
* `create_workplane_sketch` is extended to accept `{point}`, `{point, line_a,
  line_b}`, `{workplane}` or `{face, point}`, matching every case in
  `Group::MenuGroup` ([group.cpp:92](src/group.cpp#L92)).

Refactoring prerequisite: move the entity-placement steps out of
`GraphicsWindow::MouseLeftDown` into helpers (e.g. `CreateRectangle(Vector a,
Vector b)`) that both the mouse path and MCP call. This is the change most
likely to cause regressions in this plan. It is tested against the existing
`test/request/*` fixtures.

### Milestone 4 — Constraints

* `add_constraint {type, points?: [...], entities?: [...], value?, reference?}`.
  `type` is an enum that matches the Constrain menu: `distance`, `diameter`,
  `angle`, `other_angle`, `equal`, `ratio`, `difference`, `on_entity`,
  `symmetric`, `midpoint`, `horizontal`, `vertical`, `parallel`,
  `perpendicular`, `tangent`, `same_orientation`, `where_dragged`, `comment`,
  `coincident`. It runs through the selection (§1.4) using
  `Constraint::MenuConstrain`. When a value is given, it is applied
  afterwards, the same way as editing the label in the GUI. The tool
  description lists the allowed selections for each type; this is the main
  documentation the model relies on.
* `list_constraints {group?}`: type, handles involved, value, whether it's a
  reference, and comment.
* `set_constraint_value {constraint, value}`,
  `set_constraint_reference {constraint, reference}`, `delete_constraint`.
* `get_solve_status`: OK, inconsistent or failed to converge, the conflicting
  constraints (`SS.sys` failed list, as shown in the text window), DOF, and
  which points are still free (the equivalent of `SHOW_DOF`).
* Named parameters and relations. This fork has `NAMED_PARAMETER` requests and
  `Command::RELATION`. Tools: `list_parameters`, `set_parameter`,
  `add_relation {expression}`.

### Milestone 5 — 3D operations

All of these create groups by driving `Group::MenuGroup` through the selection,
then apply their numeric parameters through the group's own parameters or
through constraints.

* `extrude {sketch_group?, distance?, direction?: "normal"|[x,y,z],
  two_sided?, combine?}`
* `lathe {axis: line | {point, direction}}`
* `revolve {axis, angle_deg, combine?}`
* `helix {axis, angle_deg, pitch?, combine?}`
* `rotate_copy {axis, angle_deg, count, one_sided?, combine?}`
* `translate_copy {vector, count, one_sided?, combine?}`
* `link_file {path, ...}` (assemblies)
* `list_faces {group?}`: face entities with centroid and normal, so a
  later tool can say "sketch on the top face".

### Milestone 6 — View, navigation, screenshots and display visibility

* `get_view` / `set_view {offset?, proj_right?, proj_up?, scale?}`: the
  camera, read or written directly.
* `orient_view {preset: "front"|"back"|"top"|"bottom"|"left"|"right"|"iso"|
  "nearest_ortho"|"nearest_iso"|"onto_workplane"}`
* `zoom {factor}`, `zoom_to_fit {selection_only?}`, `pan {du, dv}`,
  `rotate_view {axis, angle_deg}`, `center_on {point}`
* `set_perspective {enabled, factor?}`, `set_explode_sketch`.
* `screenshot` gets more options: `source: "offscreen"|"window"`, where the
  `window` source reads the real GL frame on the next redraw and so includes
  overlays; `include_toolbar`; `highlight: [handles]`; `background`; and
  `view_preset` for a one-off view without moving the user's camera.
* `set_display_options {workplanes?, normals?, points?, construction?,
  constraints?: "none"|"all"|"dimensions", faces?, edges?, outlines?, mesh?,
  shaded?, hidden_lines?: ..., snap_grid?}`. These map to the `show*` fields in
  [ui.h:860](src/ui.h#L860).
* Styles: `list_styles`, `set_style {entities, style}`,
  `set_style_visibility {style, visible}`. This is the closest thing to
  per-layer visibility.

### Milestone 7 — Selection, picking, analysis and export

* `select {handles}`, `clear_selection`, `get_selection`. The user's selection
  is shown as well, so they can say "constrain what I've selected".
* `pick_at {x, y}` (screen pixels from the last screenshot): returns the
  entity or constraint under that point, so Claude can point at things it sees.
* `measure {kind: "volume"|"area"|"perimeter"|"center_of_mass"}`,
  `check_interference`, `check_naked_edges`, `get_bounding_box`,
  `get_mesh_stats`.
* `export {format: "step"|"stl"|"obj"|"dxf"|"svg"|"pdf"|"png", path, ...}`.
  This uses the existing `Export*` functions with an explicit path, so no file
  dialog opens.
* `run_command {name}`: an escape hatch for any `Command` enum entry, with a
  blocklist for commands that open dialogs.

### Milestone 8 — Headless mode, automated tests and MCP resources

* `solvespace-cli mcp [file.slvs]`: the same tool registry over **stdio**, with
  no window. Screenshots work, because they already use Cairo. This lets Claude
  model with no GUI open and lets CI test the tools.
* Automated tests: add MCP cases to the existing `test/` harness. They run
  JSON tool calls against the CLI and compare against `.slvs` fixtures and
  reference PNGs, the same pattern as `test/request/*`.
* MCP resources: `solvespace://model` (the live JSON summary) and
  `solvespace://file` (the current `.slvs` text). An MCP prompt called
  `model-a-part` gives guidance on modelling workflow.

---

## 4. Risks and open questions

* **Error capture coverage.** Some code paths open dialogs directly rather than
  through `Error()`: file dialogs, the save prompt, the text-window editor. MCP
  tools must never call into them. The `run_command` blocklist in Milestone 7
  exists for this reason.
* **Interaction with the user at the same time.** If the user is halfway
  through drawing something when a tool call arrives, `pending` state could be
  corrupted. Plan: each tool call first calls `SS.GW.ClearPending()`, the
  equivalent of pressing Esc, and reports that it did.
* **The solver can blow up on initial positions.** The mouse path places
  points carefully before solving, for example with the arc fudge factor. MCP
  tools must place every point exactly where the caller asked before solving,
  and must report solver failure in the result instead of leaving a broken
  sketch behind. When a solve fails, the tool undoes its own change.
* **Inferring constraints from the selection.** Comparing which constraints
  existed before and after a `MenuConstrain` call is reliable for a single
  call. Because calls run one at a time on the main thread, there is no race.
* **Screenshot fidelity.** Cairo output looks a little different from the GL
  window. That is acceptable for Milestone 0; Milestone 6 adds the `window`
  source.
* **Protocol version.** The server supports `2025-03-26`, `2025-06-18` and
  `2025-11-25`. It returns the client's requested version if it supports it,
  and otherwise its own latest.
