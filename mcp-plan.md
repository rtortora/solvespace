# SolveSpace MCP Server — Plan and Handoff

Goal: the SolveSpace GUI runs an MCP server, so that Claude (Claude Code, Claude
Desktop, or another MCP client on the same machine) can connect to it and build,
inspect and view models: workplane and 3D sketches, every entity type,
constraints, extrude/lathe/revolve/helix/rotate/translate, reading the model
back, screenshots, camera and display control.

## Start here (for a fresh session)

1. Read §0 (state, how to run and test) and §3 (lessons; several were bugs).
2. Check the working tree is clean (`git status`). See "Commits so far" in
   §0 for what is committed.
3. Build and run the tests (§0) to confirm the baseline: 31 MCP tests and
   SolveSpace's 300 tests should pass.
4. **Current focus: the in-app chat's UI** (§1.7 describes it, §5 "Chat UI"
   lists what to do next). The user is iterating on it by hand: they run the
   build, try it and report back; the agent changes the code, rebuilds and
   restarts a test instance (§0, "Trying the chat UI"). Expect small,
   user-driven steps rather than a fixed plan.
5. After that: **Milestone 8, headless mode, CI and MCP resources** (§5).
   §1 explains the architecture; §1.4 has the patterns for writing tools.

The optional 5b/6 follow-ups in §5 are small and can be done any time.
Milestone 1's configuration screen still needs a look by hand (§5).

---

## 0. Status at a glance

| | |
|---|---|
| Branch | `mcp-server` (from `main`; the upstream main branch is `master`) |
| Done | Milestone 0 (vertical slice), 1 (settings, token, clean shutdown), 2–5 (documents/groups, sketching, constraints, 3D operations), 5b (modelling ergonomics), 6 (view, navigation, display, styles), 7 (selection, picking, measurement, mesh checks, export, run_command). 72 tools. **In-app chat with Claude** (§1.7): pinned multi-line input, typing while Claude replies, model chooser. |
| In progress | The chat's UI, iterated by hand with the user (§5 "Chat UI"). |
| Not done | 8 (headless/CI) |
| Tests | `test/mcp/mcp_tests.py`: 31 end-to-end tests against a live GUI, all passing, including the chair as an acceptance test. SolveSpace's own suite (`cmake --build build --target test_solvespace`): 300 cases passing. |
| Field test | Claude modelled a kitchen chair over MCP: 16 calls and three ~70-point listings. With 5b, the same chair (`test_chair_acceptance`) takes 3 calls (one `batch`, a summary, a 3-view screenshot) and ~11 KB of text results, every group at 0 DOF. |
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

MCP starts if the configuration screen's "enable MCP server" is on (saved
settings `McpEnabled`, `McpPort`, `McpToken`), or for one run with `--mcp`,
`--mcp=PORT` or `SOLVESPACE_MCP_PORT`, which override the saved port.
`SOLVESPACE_MCP_TOKEN` (if set, even empty) overrides the saved token.
`run-mcp.sh` uses the environment variable. Both overrides last until the
user changes the corresponding setting in the configuration screen.

Connect Claude Code once with:

```sh
claude mcp add --transport http solvespace http://127.0.0.1:47474/mcp
# with a token:  ... --header "Authorization: Bearer TOKEN"
```

The configuration screen's "copy connect command" puts the right command
(port and token) on the clipboard.

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
# token test: start SolveSpace with SOLVESPACE_MCP_TOKEN=xyz too, and run the
# tests with the same variable (or --token xyz). Without one, test_token only
# checks that no token is required.
python3 test/mcp/mcp_smoke.py --url ...              # milestone 0 smoke test
```

`--out` collects saved `.slvs` files and screenshots (`extrude_cut.png`,
`revolve.png`, `helix.png`, `translate.png`, `view_*.png`, `chair_*.png`,
`window.png`, `box.*` exports, ...). Look at them: a test that passes can
still render wrongly.

**Quitting a test instance:** quit it by PID, e.g.
`osascript -l JavaScript -e "ObjC.import('AppKit'); $.NSRunningApplication.runningApplicationWithProcessIdentifier(PID).terminate"`.
Call `new_document {discard_unsaved: true}` first: the tests leave an unsaved
document, and quitting then opens a "save changes?" prompt on the screen.
`tell application "SolveSpace" to quit` picks an app by name and may quit the
user's own copy instead.

**Settings warning.** The test instance shares SolveSpace's saved settings
with the user's own copy, and some tools change settings that SolveSpace
writes back at exit (`set_units` → `ViewUnits`, `set_perspective` →
`CameraTangent`). `test_display_and_units` restores them; if you try these
tools by hand on a test instance, put them back before quitting it.

`test/mcp/mcp_client.py` is a small client that uses only the standard
library. Its methods are:
* `call(tool, **args)`: returns `structuredContent`, and raises `ToolFailed`
  on `isError`.
* `fails(tool, **args)`: expects an error and returns its text.

The tool name is the positional parameter `_tool`, so that tools taking a
`name` argument still work.

### Trying the chat UI

The chat can't be exercised by `mcp_tests.py` (it's text-window UI driven by
the mouse and keyboard), so it's tried by hand:

* Build with `cmake --build build --target solvespace -j8`, then start a
  test instance with `SOLVESPACE_MCP_PORT=47475
  build/bin/SolveSpace.app/Contents/MacOS/SolveSpace &` and tell the user
  its pid; restart it after each rebuild (`pkill -f
  "^build/bin/SolveSpace.app/Contents/MacOS/SolveSpace"` matches only an
  instance started with that relative path, not the user's own).
* **The agent can see the window but not drive it.** `screencapture -x -o
  -l <windowID> out.png` captures one window (find the ID with
  `CGWindowListCopyWindowInfo` in a small Swift script; downscale with
  `sips -Z 1300` before reading). Posting clicks or keys with `CGEvent`
  needs Accessibility permission for VS Code, which wasn't granted, so the
  user does the clicking and reports back (with a crash report if it
  crashes; see §3 for finding the line).
* The MCP `screenshot` tool shows only the graphics window, never the text
  window.
* To check how the Claude Code CLI behaves (queued messages, interrupts,
  `set_model`), drive `claude -p --input-format stream-json
  --output-format stream-json --verbose --tools "" --model haiku
  --strict-mcp-config` from a short Python script (run with `CLAUDECODE`
  cleared) and print the `result`, `system/init` and `control_response`
  lines. That's how the facts in §1.7 were checked.

### Commits so far

1. `4dc31fed` Milestone 0: HTTP server, job pump, message capture, 5 tools.
2. `96cfa87f` Milestones 2–5, plus fixes from an independent code review.
3. `1bd9040a` Handoff update.
4. `223da4ae` Milestones 5b and 6: compact results, offset/face sketches,
   `find_faces`, `constrain`, `batch`, screenshot views; view, display and
   style tools.
5. `b0cd6c83` Milestone 1: settings, configuration screen, token, clean
   shutdown.
6. `e6b7d1f6` Handoff update.
7. `917fc7b2` Milestone 7: selection, picking, measurement, mesh checks,
   export, `run_command`.
8. `10d0e855` In-app chat with Claude in the text window.
9. `7ea6eedd` Chat input pinned to the bottom, multi-line, usable while
   Claude replies.
10. `dd087032` Chat model chooser; empty-chat explainer removed.
11. Handoff update (this document).

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
  * If a token is set, every request to `/mcp` needs
    `Authorization: Bearer <token>` (scheme case-insensitive, constant-time
    comparison), else `401` with `WWW-Authenticate: Bearer`. The token is
    stored in plain text in SolveSpace's settings.
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
* State shared with HTTP threads (job queue, token, session id, last client)
  is in `SharedState`, which is allocated once and never destroyed, so a
  straggling thread at exit can't touch destroyed statics.
* **Lifecycle (M1).** `Mcp::Setup` (from `entrygui.cpp`) installs hooks in
  `SS.mcp` (`applyServer`, `applyToken`, `status`, `connectCommand`,
  `shutdown`) and starts the server. The core library (configuration screen,
  `SS.Exit`) only calls these hooks, because `HAVE_MCP` is defined for the
  GUI target only; with no hooks the screen hides the MCP section.
  `StopServer` refuses new calls, fails queued ones ("server stopped"), drops
  a pending window-screenshot hook, shuts down every open connection
  (`HttpServer` tracks them) and waits up to 5 s for their threads.
  `SS.Exit` calls it, because on macOS `ExitGui()` calls `exit()` and the
  code after `RunGui()` never runs.

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
5. **Result.** `structuredContent` plus the same JSON (compact, not
   pretty-printed) as `text` content, and any `image` content. Errors are a
   single text item.
6. **Finishing off the GUI thread.** A tool can set
   `ToolResult::finishOffGuiThread`; the HTTP thread calls it after the GUI
   part returns, and it may block. `screenshot {source:"window"}` uses this:
   the GUI part sets `GraphicsWindow::onFrameCaptured` (a one-shot hook in
   `Paint`, `draw.cpp`) and invalidates the window; the HTTP thread waits up
   to 10 s for the PNG. The GUI part of a call is in `CallToolOnGuiThread`, the
   wire result is built by `CallResultJson` on the HTTP thread.

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
* **Results are compact by default** (`verbose: true` for the full form):
  `EntityJson(e, verbose)` gives points in a workplane as `uv` only, drops
  `construction: false`; `RequestJson(hr, verbose)` gives the main entity with
  its points; numbers go through `Round()` (1e-6) and colours are `#rrggbb`.
* **`constrain: true`** helpers are in `tools_sketch.cpp`: `AddDimension`
  (adds a dimension at its current value via `ModifyToSatisfy`),
  `AnchorPoint` (coincident with the workplane origin, else two
  `PROJ_PT_DISTANCE`s along vector entities: the shape's own horizontal and
  vertical lines, or the reference planes' normals when the workplane is
  axis-aligned), `CheckConstrained`.
* **Datum groups.** `create_workplane_sketch {offset|origin|face}` inserts a
  hidden 3D sketch holding one datum point, fixed by `PT_PLANE_DISTANCE`s to
  the reference planes (`PT_IN_PLANE` where 0) and, for faces, `PT_ON_FACE`;
  then a `WORKPLANE_BY_POINT_ORTHO` group through that point with
  `predef.q` set explicitly (`MakeDatumPoint`, `MakeWorkplaneGroup`).
* **Faces.** `GetFaceInfos` finds each face's triangles in the active group's
  `displayMesh` (`STriangle::meta.face`) for its centre, bounds, area and
  **outward** normal. Face entities don't know which way is out: an
  extrusion's top and bottom faces carry the same normal.
* **Initialise handles.** `hEntity x;` is uninitialised (a POD). Write
  `hEntity x = Entity::NO_ENTITY;`. This caused a crash (§3).
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
  sketches. Compact results give `uv` for points in a workplane and `xyz`
  otherwise; `verbose: true` gives both.

### 1.6 Code layout

```
extlib/nlohmann/json.hpp        vendored, MIT, v3.11.3 (in THIRD_PARTIES.txt)
src/mcp/httpserver.{h,cpp}      loopback HTTP/1.1 server (POSIX + Winsock)
src/mcp/mcp.h, mcpserver.cpp    JSON-RPC, lifecycle, job queue, pump, call scope
src/mcp/mcptools.h              shared helper declarations, Transaction
src/mcp/tools_common.cpp        handles, args, JSON for groups/entities/constraints,
                                Transaction, tool registry (GetTools)
src/mcp/tools_model.cpp         get_model_summary, get_group, list_entities,
                                find_faces, screenshot, ServerInstructions
src/mcp/tools_document.cpp      new/open/save, undo/redo, group management, delete,
                                batch
src/mcp/tools_sketch.cpp        sketch groups and all entity types, fillet, split,
                                construction, move_point
src/mcp/tools_constraint.cpp    constraints, solve status, parameters, relations
src/mcp/tools_ops.cpp           extrude (with end faces), lathe, revolve, helix,
                                rotate/translate copies, link_file
src/mcp/tools_view.cpp          view/camera, display options, units, perspective,
                                exploded sketch, styles (milestone 6)
src/mcp/tools_analysis.cpp      selection, pick_at, measure, mesh checks, export,
                                run_command (milestone 7)
src/util.{h,cpp}                MessageCapture hook in MessageBox()
src/ui.h, src/draw.cpp          GraphicsWindow::onFrameCaptured (window screenshots)
src/file.cpp                    LocateImportedFile: no prompt while capturing
src/platform/gui.{h,cpp}        Platform::modalDepth / ModalScope
src/platform/gui*.{cpp,mm}      ModalScope in each RunModal/ShowModal
src/platform/entrygui.cpp       --mcp / env parsing, Mcp::Setup/StopServer
src/solvespace.{h,cpp}          SS.mcp settings and hooks, FreezeMcpSettings,
                                shutdown hook in Exit()
src/confscreen.cpp, src/ui.h    MCP section of the configuration screen
src/platform/gui*.{cpp,mm}      Platform::SetClipboardText (all backends)
src/mcp/chat.cpp                the in-app chat's backend (runs `claude`, see §1.7)
res/mcp/chat-system-prompt.md   the chat's system prompt (a bundled resource)
src/chatscreen.cpp, src/ui.h    the text window's chat screen (Screen::CHAT), its
                                footer, input (TW.chatInput) and model chooser
src/textwin.cpp                 the text window's pinned footer (footerRow, RowY,
                                ScrollBottom/ScrollPage/FooterHeight)
src/platform/gui.h, guimac.mm   multi-line editor: ShowMultilineEditor, MoveEditor,
                                onEditorChanged (macOS NSTextView; others fall
                                back to the single-line editor)
CMakeLists.txt, src/CMakeLists  ENABLE_MCP option; GUI target only (not Qt or
                                Emscripten); adds render/rendercairo.cpp for
                                screenshots
test/mcp/                       mcp_client.py, mcp_smoke.py, mcp_tests.py
```

### 1.7 In-app chat with Claude

The text window's header has a **chat** link left of **home**. It opens
`Screen::CHAT`, a conversation with the Claude Code CLI that pilots the
running SolveSpace through its own MCP server.

* **Backend (`src/mcp/chat.cpp`, GUI target only).** The first message
  starts `claude -p --input-format stream-json --output-format stream-json
  --verbose --include-partial-messages` in its own process group, with
  `--strict-mcp-config --mcp-config <temp file>` (0600, holds the token,
  deleted once Claude's `system/init` arrives), `--tools ""` (no shell or
  file tools), `--allowedTools mcp__solvespace`, `--permission-mode dontAsk`
  and `--system-prompt` (see "System prompt" below). The working
  directory is the open file's folder, else `$HOME` (Claude keeps its session
  history per directory). If the MCP server is off, it is started for the
  chat on `SS.mcp.port` (or one of the next 9 ports) without changing the
  saved setting.
* **System prompt.** `res/mcp/chat-system-prompt.md` is a bundled resource
  (listed in `res/CMakeLists.txt`) loaded with `LoadString` and passed as
  `--system-prompt`. It **replaces** Claude Code's default prompt, which is
  about coding with shell/file tools the chat doesn't have. Checked: the MCP
  server's `instructions` and all tools still reach Claude with it. It was
  written from solvespace.com's tutorials, reference, features and
  technology pages plus the tool list, and covers modelling strategy, solver
  and DOF behaviour, missing features and workarounds, reading screenshots,
  units, care with the user's work, and plain-text replies. To try edits
  without rebuilding, set `SOLVESPACE_CHAT_PROMPT=/path/to/file.md` (read
  each time a chat starts). Note that on macOS a rebuild after changing
  only a resource updates `build/Resources` but not the `.app`: the copy
  into the bundle is a POST_BUILD step that runs only when the executable
  relinks. Run `cp -r build/Resources build/bin/SolveSpace.app/Contents/`
  (a pre-existing quirk that affects every resource). To test outside the
  GUI, run the same flags
  from a shell: `claude -p --strict-mcp-config --mcp-config <json>
  --tools "" --allowedTools mcp__solvespace --permission-mode dontAsk
  --system-prompt "$(cat res/mcp/chat-system-prompt.md)" "<request>"`.
* **One editor, two users.** The text window's single editor serves both
  the chat input and value edits. The text window's `onEditingDone` callback
  sends the text to `ChatInputDone` when `chatInput.open`, and otherwise to
  `EditControlDone`. The check used to be inside `EditControlDone`, so while
  the chat was open, `set_parameter` (which calls `SS.TW.EditControlDone`
  directly) sent its value into the chat input instead of setting the
  parameter.
* **The macOS multi-line editor's layout** (`layoutMultilineEditor`,
  guimac.mm) reports height changes, and the chat responds by moving the
  editor, which lays it out again. Two rules keep that from recursing:
  - Only the outermost call reports a change.
  - The text wraps at a fixed width that always leaves room for a vertical
    scroller (`widthTracksTextView = NO`).

  Before this, a long draft near the maximum height crashed with a stack
  overflow. When scroll bars take up width ("always show", or with a mouse
  connected), the auto-hiding scroller narrowed and widened the text, so its
  measured height flipped on every pass.
* **Finding `claude`.** GUI apps on macOS get a minimal PATH, so the user's
  shell is asked for its PATH once (`$SHELL -ilc`), plus `~/.local/bin`,
  `~/.claude/local`, `/opt/homebrew/bin`, `/usr/local/bin`.
  `SOLVESPACE_CLAUDE` overrides the path. `CLAUDECODE` is removed from the
  child's environment.
* **Threads.** A launcher thread resolves PATH, forks (closing every fd
  above 2, so the MCP sockets aren't inherited) and reads stdout; another
  reads stderr. Lines go into a queue; a 30 ms GUI timer folds them into
  `SS.chat.entries` (user / Claude / tool / note / failure). Text arrives as
  `content_block_delta`s; tool calls come from `assistant` messages (a
  `batch` shows its inner tools); failed tool results and the `result`
  (time, cost or error) are shown too.
* **Session lifetime.** The process and transcript belong to `SS.chat`, not
  the screen, so they survive navigating away. **stop** sends a
  `control_request` interrupt; **new chat** ends the process (stdin closed,
  SIGTERM to the group, SIGKILL after 3 s) and clears the transcript.
  `SS.Exit` ends it too.
* **Screen behaviour.** The chat isn't replaced by the selection
  description (Claude selects things through MCP). A tool call that starts
  in the chat, and `TW.ClearSuper()` (undo, new file), stay in the chat.
  The screen follows new text when scrolled to the bottom, wraps to the
  window width, and shows only the newest ~3960 lines (the text window's row
  limit).
* **Input, pinned to the bottom.** The text window has an optional footer
  (`TW.footerRow`): rows from it on are drawn at the bottom of the window,
  not scrolled (`RowY`, `ScrollBottom`, `ScrollPage`, `FooterHeight`). The
  chat's footer is a status line of constant height (`working... [stop]`,
  with the number queued, or a hint) and the `>` input line.
  The input is a multi-line editor (`Window::ShowMultilineEditor`,
  `MoveEditor`, `onEditorChanged`; on macOS an `NSTextView` in an
  `NSScrollView`): Enter sends, Shift/Option+Enter adds a line, it grows
  upwards to a third of the window and then scrolls. It stays open while the
  chat is shown (`TW.chatInput`; `HideEditControl()` leaves it alone, so
  clicks in the graphics window, undo, `delete` and `ClearSuper()` don't close
  it) and the transcript scrolls while it's open. Its text is kept in
  `SS.chat.draft` as it changes, so leaving the chat or a pending operation
  (which hide it) doesn't lose it. Other platforms fall back to the
  single-line editor, opened by clicking `>` (not compiled yet).
* **Model.** Under the title: `model <name>  [change]`. The name is
  `SS.chat.activeModel` (what Claude's `system/init` reports, at the start of
  every turn) while a session runs, else `SS.chat.model` or "default".
  **[change]** lists default / fable / opus / sonnet / haiku (aliases; the
  CLI resolves them to the newest model) and other... (a single-line edit
  control, `Edit::CHAT_MODEL`). The choice is saved as `ChatModel` (with the
  MCP settings, `FreezeMcpSettings`), passed as `--model` at start, and sent
  to a running session as `control_request {subtype: "set_model", model}`
  (no `model` for the default). The `control_response` is matched by
  request id: success adds a "model: ..." note; an error (e.g. "Model 'x'
  not found"; the CLI keeps its model) is shown and the previous choice is
  restored. The new model applies from the next message.
* **Typing while Claude replies.** `SS.chat.pending` counts replies still to
  come. The CLI queues a message sent mid-reply and answers it as its own
  turn, with its own `result` (checked); an interrupt's response reports
  `still_queued`, so **stop** should only stop the current reply (not
  checked with a message actually queued).
* **Not done:** Windows (the backend reports "not available"; it needs
  `CreateProcess` with pipes); Markdown rendering; approving tools other than
  SolveSpace's. See §5 "Chat UI" for the open items.

---

## 2. Tool reference (72 tools)

| Area | Tools |
|---|---|
| Read | `get_model_summary` (with `solid_bounds`), `get_group`, `list_entities {group?, kind: all/curves/points/faces/workplanes, unique_points, verbose}`, `find_faces {group?, normal?, near?, limit, include_hidden}`, `list_constraints`, `get_solve_status` (with free points), `list_parameters`, `screenshot {view \| views, width, height, zoom_to_fit, units, highlight, shaded, hidden_lines, source: render/window}` |
| Document | `new_document {discard_unsaved}`, `open_file {path}`, `save_file {path?}`, `undo {steps}`, `redo {steps}`, `batch {calls: [{tool, args}]}` |
| Groups | `activate_group`, `set_group_visibility {group \| groups, visible}`, `set_group_properties {name, color, opacity, combine, sides, skewed, suppress, relax_constraints, allow_redundant, count, skip_first}`, `delete_group` |
| Sketch groups | `create_workplane_sketch {plane \| plane+offset/origin \| face \| point+plane \| point+normal \| point+line_a+line_b \| workplane}`, `create_3d_sketch` |
| Entities | `sketch_point`, `sketch_line {points, closed, constrain}`, `sketch_rectangle {corner1+corner2 \| center+width+height, constrain}`, `sketch_circle {radius \| diameter, normal in 3D, constrain}`, `sketch_arc {clockwise}`, `sketch_cubic {points, start/end_control, periodic}`, `sketch_text`, `sketch_image` (PNG only), `sketch_workplane` (3D only), `fillet {point, radius}`, `split_curves`, `set_construction`, `move_point`, `delete {handles}`. All take `verbose`. |
| Constraints | `add_constraint {type, points, entities, value, reference, supplementary}` (types: distance, diameter, angle, coincident, on_entity, equal, ratio, difference, horizontal, vertical, parallel, tangent, perpendicular, symmetric, midpoint, same_orientation, lock), `set_constraint_value`, `set_constraint_reference`, `add_comment`, `set_parameter`, `add_relation` |
| 3D ops | `extrude {depth, sides, combine, constrain_depth}` (returns `depth_constraint` and `faces.top/bottom`), `lathe {axis, axis_point}`, `revolve {angle_deg}`, `helix {angle_deg, pitch \| axial_travel}`, `rotate_copy {center, axis, count, angle_deg}`, `translate_copy {step, count}`, `link_file {path}` |
| View (M6) | `get_view`, `set_view {center \| offset, proj_right, proj_up, scale}`, `orient_view {preset: iso/front/back/left/right/top/bottom/workplane/nearest_ortho/nearest_iso, zoom_to_fit}`, `zoom {factor}`, `zoom_to_fit {handles?}`, `pan {dx, dy \| by}`, `rotate_view {axis, angle_deg}`, `center_on {point}`, `set_perspective {enabled, factor}`, `set_explode_sketch {enabled, distance}`, `set_units {mm/m/in/ft_in}` |
| Selection (M7) | `get_selection {verbose}` (items plus counts by kind), `select {handles, mode: replace/add/remove}`, `clear_selection`, `add_constraint {use_selection: true}`, `pick_at {x, y, view_index, source: screenshot/window, select: none/replace/add}` |
| Analysis (M7) | `measure {group?, faces?, entities?}` (solid volume, surface area, centre of mass, bounds; group's own volume; sketch region area/perimeter; face areas; curve lengths), `check_naked_edges {group?, limit, show}`, `check_interference {group?, limit, show}` |
| Export, commands (M7) | `export {path, kind: solid/mesh/view/section/wireframe/image, overwrite, view, constraints, face, width, height, zoom_to_fit}`, `run_command {name, handles?}` (an allowlist of 59 menu commands) |
| Display, styles (M6) | `set_display_options {workplanes, normals, points, construction, constraints: none/all/dimensions, shaded, faces, edges, outlines, mesh, snap_grid, dim_solid, hidden_lines}` (no arguments: read them), `list_styles`, `create_style {name, color, width, width_units, visible, stipple}`, `set_style_properties {style, ...}`, `set_style {handles, style}` |

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
* **Sketch placement (5b).** `{plane:"XY", offset:420}` and
  `{plane, origin:[x,y,z]}` keep the reference plane's orientation, so `[u, v]`
  stay model coordinates; `offset_constraint` edits the height later.
  `{face}` orients the workplane with the face's **outward** normal (positive
  extrude depth builds outward; negative with `difference` cuts in), Z up on
  vertical faces (front face: u=X, v=Z), and for axis-aligned faces puts the
  origin where the face plane meets the axis through the model origin, so
  `[u, v]` are model coordinates and the sketch follows the face. Other faces
  use a vertex of the face as origin (noted in the result).
* **`constrain: true` (5b)** adds dimensions at the drawn values:
  rectangles get width, height and a corner (or, in the centre form, the
  centre of a construction diagonal) anchored; circles a diameter and centre
  position; polylines H/V or an angle to the previous segment (parallel if in
  line), a length per segment (not the closing one), and the first point. In
  a workplane not aligned with the reference axes, a circle's or polyline's
  position can't be measured and is left free (with a `note`).
* **`batch` (5b)** runs up to 50 calls in one Transaction: one undo step, and
  any failure (including an `Error()` the tool didn't turn into an exception)
  undoes them all. Strings like `"$2.faces.top.handle"` are replaced by values
  from earlier results. `batch`, `new_document`, `open_file`, `save_file`,
  `undo`, `redo` and window screenshots aren't allowed inside.
* **Screenshot presets** use a Z-up convention: front looks from -Y, right from
  +X, top from +Z, iso from (+1, -1, +1). Presets are fitted to the model by
  default, and render without touching the GUI's camera, selection, units or
  display settings. Labels are in mm unless `units: "display"`.
* **View tools change the user's view** directly (no animation) and stop an
  animation in progress, except `orient_view nearest_ortho/nearest_iso`,
  which use the View menu and animate. `rotate_view`: positive angles turn the
  model counter-clockwise seen from the axis's positive end. `pan {dx, dy}`
  moves the model on screen by pixels; `pan {by}` moves the view centre.
* **Selection (M7)** is the GUI's own and is shared with the user. Tools that
  drive menus through the selection (`add_constraint`, groups, ...) replace
  it, then clear it, as the GUI does.
* **`pick_at` (M7)** hit-tests exactly as the mouse does
  (`HitTestMakeSelection`), using the camera saved by the last `screenshot`
  (each image of a multi-view one; `ShotView`/`lastScreenshot` in
  `mcptools.h`), then puts the GUI's camera and hover back. It also ray-casts
  the solid's mesh for the 3D point and face under the pixel, whether or not
  faces are selectable in the GUI.
* **`measure`, the checks and `export` (M7) use the solid as of the active
  group** (`measure` and the checks take `group`, which must not come after
  the active one).
* **`export` (M7)** never opens a dialog and refuses to replace a file without
  `overwrite`. Mesh export is a copy of `ExportMeshTo` that reports naked
  edges as a `warning` instead of an `Error()`. Exports switch on
  `exportMode` (finer chord tolerance) and regenerate; the tool switches it
  off and regenerates again afterwards, as Esc does in the GUI. View exports
  hide constraints unless `constraints: true`. `view` presets temporarily
  turn the camera. PNG export renders offscreen (like `screenshot`), not from
  the window as File > Export Image does. **A section is not a cut:**
  SolveSpace's Export 2d Section only outputs the solid's faces lying *in*
  the plane (`MakeEdgesInPlaneInto`, `MakeSectionEdgesInto`), so a plane
  through the middle of a solid gives an empty file.
* **`run_command` (M7)** is an allowlist, not the planned blocklist:
  commands that open dialogs (file, export, link, configuration, paste
  transformed, stop tracing), wait for clicks (drawing tools, comment,
  relation) or are covered by other tools (undo/redo, new/open/save) aren't
  in it. Model-changing commands run in a `Transaction`; the result lists
  added groups/requests/constraints, the solve status and the selection.
  `immediatelyEditDimension` is switched off so a dimension doesn't open the
  value editor.
* **Saved settings:** `set_units` and `set_perspective` change values
  SolveSpace saves at exit. The perspective factor is the configuration
  screen's (0.3 typical; stored as `cameraTangent = factor/1000`).

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
  onto each new workplane (asynchronously, through a timer). Use
  `screenshot {view|views}` (offscreen presets) to check the model; the view
  tools move the user's camera and should be used when the user asks.
* **Uninitialised handles.** `hEntity uVec, vVec;` holds garbage; using it
  as a constraint entity hit `ssassert` in `GetEntity` and aborted SolveSpace
  (crash reports are in `~/Library/Logs/DiagnosticReports/SolveSpace-*.ips`;
  lldb didn't get the app's server up, the `.ips` backtrace was enough).
* **Signed distances keep their sign.** `PT_PLANE_DISTANCE` and
  `PROJ_PT_DISTANCE` values are signed; `set_constraint_value` keeps the sign
  like the GUI does (typing 30 into a -50 dimension gives -30; give a negative
  value to flip).
* **The client caches tool schemas.** See the reconnect note in §0.
* **Text window `Printf` codes that take arguments.** Besides `%s %d %x %@
  %2 %3 %# %c`, these read an argument too: `%Fp`/`%Bp` (colour from an
  int), `%Bz` (an `RgbaColor *`), `%Lp` (link code from an int), `%D` (link
  data), `%f`/`%h` (link functions). So `p` and `z` can't be used as literal
  link or colour codes: `%Lp` as a link code shifted every later argument
  and crashed in `strlen` (the chat's model chooser; now `%Lk`).
* **Text window editors.** Each window has one native editor
  (`ShowEditor`); `ShowEditControl` takes it from the chat's input.
  `TW.ClearSuper()` rebuilds the whole `TextWindow` with placement new, so
  state that must survive it lives in `SS` (e.g. `SS.chat.draft`) or is
  carried across explicitly (`chatInput`, the CHAT screen).

---

## 4. Completed milestones (summary of what was asked for and what changed)

* **M0, vertical slice.** As planned.
* **M1, hardening and settings.** As planned:
  * Saved settings `McpEnabled`/`McpPort`/`McpToken`; command line and
    environment override them for one run. Settings are also saved as soon
    as they're changed in the screen (other settings only at exit).
  * Configuration screen section: enable, port, token (masked to its last 4
    characters; `[show/change]` opens the editor with the full token,
    `[generate]` makes 128 random bits as hex, `[clear]`), status, last client
    and time of its last request, and "copy connect command" (new
    `Platform::SetClipboardText`). Toggling or changing the port starts,
    stops or restarts the server at once; a port that can't be bound shows
    "could not start: port N is already in use" in red.
  * Bearer token, `test_token`; clean shutdown as described in §1.2.
    Verified by quitting with two idle keep-alive connections open: both
    closed within 0.15 s and the app exited without a crash report.
  * Not done: a status in the window title (optional in the plan).
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
* **M5b, modelling ergonomics.** All eight items, as planned, plus:
  * Offset/origin/face sketches use a hidden datum group (parametric: the
    offset is a dimension; face sketches follow the face).
  * `constrain` anchors off-origin shapes with projected distances from the
    workplane origin, not only at the origin. Rectangles have a centre form,
    circles take `diameter`.
  * `batch` was chosen over `shapes: [...]`, with `$N.path` references.
  * Compact results everywhere, rounding, hex colours, and the text content
    is no longer pretty-printed. `extrude` returns `depth_constraint` and its
    end faces.
  * Screenshot `views` (up to 6), `units`, and (from M6) `highlight`, `shaded`,
    `hidden_lines`, `source: "window"`.
* **M7, selection, analysis and export.** As planned, except:
  * `measure` returns everything that applies in one call rather than taking
    a `volume|area|...` choice.
  * `add_constraint {use_selection}` was added for "constrain what I've
    selected".
  * `run_command` uses an allowlist (§2).
  * Not done: the export settings (chord tolerance, export scale, offset,
    canvas size) can't be changed from MCP; they are the configuration
    screen's.
* **M6, view and display.** As planned, except:
  * `set_style_visibility` became `set_style_properties` (visibility,
    colour, width, stipple, name), and `create_style` was added, since custom
    styles otherwise can't be made.
  * `zoom_to_fit {selection_only}` became `zoom_to_fit {handles}`.
  * `source: "window"` needs SolveSpace changes (`onFrameCaptured` in
    `ui.h`/`draw.cpp`) and the deferred-result path in `mcpserver.cpp`.

## 5. Next steps (in priority order)

### Chat UI (current focus; user-driven)

Done and liked by the user: pinned footer, multi-line input, typing while
Claude replies, model chooser, no explainer text. Open items, roughly in the
order they'd matter, for the user to pick from:
* **Cmd+C / Cmd+V / Cmd+X / Cmd+A / Cmd+Z in the input** probably fire
  SolveSpace's menu commands (copy/paste sketch, select all, undo) rather
  than editing the text; the old single-line editor likely had this too.
  Fix: in `guimac.mm`, let the editor handle these key equivalents when it
  is first responder (e.g. `performKeyEquivalent:` in the text view, or
  have the menu items' handlers forward to the first responder).
* **Esc in the input** goes to the graphics window's Esc handler (clears the
  selection); handing the keyboard back to the model may be nicer.
* **stop with a queued message:** not checked that the queued message
  survives (the CLI's interrupt response has `still_queued`, which suggests
  it does).
* The transcript wraps to the width at the time it was shown; resizing the
  window doesn't rewrap until the next change.
* Markdown in Claude's replies is shown as plain text (`**bold**`, lists,
  code blocks).
* Other platforms: GTK/Qt/Windows fall back to the single-line editor,
  opened by clicking `>`; none of this has been compiled there. The Windows
  chat backend doesn't exist yet.

### Milestone 1 — check by hand

Done (see §4); what couldn't be checked from here:
* **The configuration screen** (⌘, or the text window's "configuration"
  link) wasn't seen: this session couldn't send key presses or capture a
  single window. Check the layout, the checkbox, `[change]` on port and token,
  `[generate]`, `[clear]`, the status line (green when listening, red with
  the reason when the port is taken), the "client" line updating when a
  client connects, and that "copy connect command" puts the command on the
  clipboard.
* GTK3/GTK4/Windows/Qt/HTML `SetClipboardText` haven't been compiled.

### Milestone 8 — Headless mode, CI and MCP resources (after the chat UI)

* `solvespace-cli mcp [file]`: the same registry over stdio, with no window.
  Screenshots already use Cairo. The `test/` harness could then run
  `mcp_tests.py` scenarios in CI.
* Resources: `solvespace://model` (JSON summary) and `solvespace://file`
  (`.slvs` text). A `model-a-part` prompt.

### Small follow-ups from 5b/6 (optional)

* `delete_group` on an offset/face sketch leaves its hidden datum group
  behind. Delete the datum group with it, or reuse one datum group per model.
* `batch` returns every call's full result (7 KB of the chair's 11 KB). An
  option to return only some results would trim it further.
* Polylines with an angled first segment in a non-aligned workplane, and
  circles there, are left with free position (noted in the result).
* `find_faces`, `solid_bounds`, `measure` and `export` look at the solid
  **as of the active group**; activate the last group to see the whole model.
* M7: export settings (chord tolerance, export scale, cutter offset, canvas
  size) as `export` arguments or a settings tool.

---

## 6. Known gaps and risks

* **Settings written by two instances:** each instance writes all its
  settings at exit, so a test instance started before the user changed the
  MCP settings in another instance writes the old values back when it quits.
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
  window, and has no toolbar. `source: "window"` is exact but is at the
  screen's pixel density (about 1440×2000 on a Retina display) and fails if
  the window isn't redrawn within 10 s (minimised or hidden).
* **Breaking change for existing clients:** results are compact now. Clients
  that read `requests[i].entities` or `sketch_rectangle`'s `lines` need
  `verbose: true`, or the new fields (`entity`, `points`, `edges`).
