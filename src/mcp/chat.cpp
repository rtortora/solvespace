//-----------------------------------------------------------------------------
// The chat with Claude in the text window. Runs the Claude Code CLI as a
// child process in streaming JSON mode, connected to SolveSpace's own MCP
// server, and folds its output into the transcript in SS.chat, which the
// text window's chat screen (chatscreen.cpp) shows.
//
// Threading: a launcher thread finds and starts `claude`, then reads its
// stdout; another thread reads its stderr. Both queue lines in the Session.
// A timer on the GUI thread takes them and updates the transcript. The
// session belongs to this file, not to the screen, so it carries on while
// the user looks at other screens.
//-----------------------------------------------------------------------------
#include "solvespace.h"
#include "mcp/mcp.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#if !defined(WIN32)
#   include <errno.h>
#   include <fcntl.h>
#   include <signal.h>
#   include <sys/stat.h>
#   include <sys/wait.h>
#   include <unistd.h>
extern char **environ;
#endif

namespace SolveSpace {
namespace Mcp {

namespace {

static const int CHAT_PUMP_PERIOD_MS = 30;

// Appended to Claude Code's system prompt.
static const char *ChatSystemPrompt =
    "You are running inside SolveSpace, the parametric 2D/3D CAD program, in "
    "the chat panel of its property browser (the text window beside the "
    "model). The user is looking at the SolveSpace window and talks to you "
    "here. Use the solvespace MCP tools to inspect and change the open model; "
    "the user sees every change live in the window, and can undo it with "
    "Ctrl+Z. You have no shell or file tools, only SolveSpace's. Your replies "
    "are shown as plain monospace text in a narrow panel (roughly 45 to 90 "
    "columns), so keep them short, don't use Markdown tables, headings or "
    "wide ASCII art, and don't repeat the tool results back to the user.";

struct Session {
    enum class Kind { OUT, ERR, EXITED };

    std::mutex                                  mutex;
    std::deque<std::pair<Kind, std::string>>    queue;
    // Messages sent before the process was running; written once it is.
    std::vector<std::string>                    pendingInput;
    bool                                        started = false;
    bool                                        stopped = false;
    bool                                        exited  = false;
#if !defined(WIN32)
    pid_t                                       pid     = -1;
#endif
    int                                         stdinFd = -1;

    void Push(Kind kind, std::string text) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.emplace_back(kind, std::move(text));
    }
};

struct LaunchParams {
    std::vector<std::string> args;      // without the program
    std::string              cwd;
};

// GUI-thread state.
static std::shared_ptr<Session>     session;
static Platform::TimerRef           pumpTimer;
static std::string                  configPath;     // temporary MCP config
// The transcript entry receiving streamed text, or -1.
static int                          streamingEntry = -1;
// Whether the current assistant message's text arrived as deltas.
static bool                         sawTextDelta = false;
static bool                         interrupted = false;
static int                          requestCount = 0;
static std::deque<std::string>      stderrTail;
// The status to show between replies, e.g. "claude-opus-5-5, connected ...".
static std::string                  readyStatus;

static void AddEntry(SolveSpaceUI::ChatEntry::Kind kind, const std::string &text) {
    SS.chat.entries.push_back({ kind, text });
}

static void Note(const std::string &text) {
    AddEntry(SolveSpaceUI::ChatEntry::Kind::NOTE, text);
}

static void Failure(const std::string &text) {
    AddEntry(SolveSpaceUI::ChatEntry::Kind::FAILURE, text);
}

static void RemoveConfigFile() {
    if(!configPath.empty()) {
        remove(configPath.c_str());
        configPath.clear();
    }
}

static void ShowChanges() {
    if(SS.TW.shown.screen == TextWindow::Screen::CHAT) {
        SS.ScheduleShowTW();
    }
}

//-----------------------------------------------------------------------------
// The child process (POSIX).
//-----------------------------------------------------------------------------
#if !defined(WIN32)

static bool WriteAll(int fd, const std::string &data) {
    size_t done = 0;
    while(done < data.size()) {
        ssize_t n = write(fd, data.data() + done, data.size() - done);
        if(n < 0) {
            if(errno == EINTR) continue;
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

// GUI apps on macOS start with a minimal PATH, which usually lacks `claude`
// (and `node`, which an npm-installed `claude` needs). Ask the user's shell
// for its PATH, once, and add the places the installers use.
static std::string UserPath() {
    static std::mutex   mutex;
    static bool         done = false;
    static std::string  path;
    std::lock_guard<std::mutex> lock(mutex);
    if(done) return path;
    done = true;

    const char *envPath = getenv("PATH");
    path = (envPath && *envPath) ? envPath : "/usr/bin:/bin:/usr/sbin:/sbin";

    const char *shell = getenv("SHELL");
    if(shell && *shell && strchr(shell, '\'') == NULL) {
        std::string cmd = std::string("'") + shell + "' -ilc "
            "'printf \"\\n__SOLVESPACE_PATH__=%s\\n\" \"$PATH\"' </dev/null 2>/dev/null";
        if(FILE *f = popen(cmd.c_str(), "r")) {
            std::string output;
            char buf[4096];
            size_t n;
            while((n = fread(buf, 1, sizeof(buf), f)) > 0) output.append(buf, n);
            pclose(f);
            const std::string marker = "\n__SOLVESPACE_PATH__=";
            size_t at = output.rfind(marker);
            if(at != std::string::npos) {
                at += marker.size();
                std::string shellPath = output.substr(at, output.find('\n', at) - at);
                if(!shellPath.empty()) path = shellPath + ":" + path;
            }
        }
    }

    const char *home = getenv("HOME");
    if(home && *home) {
        path += std::string(":") + home + "/.local/bin";
        path += std::string(":") + home + "/.claude/local";
    }
    path += ":/opt/homebrew/bin:/usr/local/bin";
    return path;
}

static std::string FindInPath(const std::string &name, const std::string &path) {
    size_t start = 0;
    while(start <= path.size()) {
        size_t end = path.find(':', start);
        if(end == std::string::npos) end = path.size();
        std::string dir = path.substr(start, end - start);
        if(!dir.empty()) {
            std::string candidate = dir + "/" + name;
            struct stat st;
            if(stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
               access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
        }
        start = end + 1;
    }
    return "";
}

// On the launcher thread: starts the process, then reads its stdout until it
// exits.
static void RunSession(std::shared_ptr<Session> s, LaunchParams params) {
    std::string path = UserPath();
    std::string program;
    const char *override = getenv("SOLVESPACE_CLAUDE");
    if(override && *override) {
        program = override;
    } else {
        program = FindInPath("claude", path);
    }
    if(program.empty()) {
        s->Push(Session::Kind::EXITED,
            "Couldn't find the `claude` command (Claude Code). Install it, or "
            "set SOLVESPACE_CLAUDE to its path before starting SolveSpace.");
        return;
    }

    // Everything the child needs is built before fork(): after it, only
    // async-signal-safe calls are allowed.
    std::vector<std::string> argStrings;
    argStrings.push_back(program);
    argStrings.insert(argStrings.end(), params.args.begin(), params.args.end());
    std::vector<char *> argv;
    for(std::string &a : argStrings) argv.push_back(&a[0]);
    argv.push_back(NULL);

    std::vector<std::string> envStrings;
    for(char **e = environ; *e; e++) {
        std::string var = *e;
        // Not nested in another Claude Code session, as far as it knows.
        if(var.compare(0, 5, "PATH=") == 0 ||
           var.compare(0, 11, "CLAUDECODE=") == 0 ||
           var.compare(0, 23, "CLAUDE_CODE_ENTRYPOINT=") == 0) continue;
        envStrings.push_back(var);
    }
    envStrings.push_back("PATH=" + path);
    std::vector<char *> envp;
    for(std::string &e : envStrings) envp.push_back(&e[0]);
    envp.push_back(NULL);

    int inPipe[2], outPipe[2], errPipe[2];
    if(pipe(inPipe) != 0) {
        s->Push(Session::Kind::EXITED, "Couldn't create a pipe.");
        return;
    }
    if(pipe(outPipe) != 0) {
        close(inPipe[0]); close(inPipe[1]);
        s->Push(Session::Kind::EXITED, "Couldn't create a pipe.");
        return;
    }
    if(pipe(errPipe) != 0) {
        close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
        s->Push(Session::Kind::EXITED, "Couldn't create a pipe.");
        return;
    }
    long maxFd = sysconf(_SC_OPEN_MAX);
    if(maxFd < 0 || maxFd > 65536) maxFd = 65536;
    const char *cwd = params.cwd.c_str();

    std::unique_lock<std::mutex> lock(s->mutex);
    if(s->stopped) {
        lock.unlock();
        for(int fd : { inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1] }) {
            close(fd);
        }
        return;
    }
    pid_t pid = fork();
    if(pid == 0) {
        // Its own process group, so that stopping it stops its children too.
        setpgid(0, 0);
        dup2(inPipe[0], 0);
        dup2(outPipe[1], 1);
        dup2(errPipe[1], 2);
        // Don't pass on the MCP server's sockets or anything else.
        for(long fd = 3; fd < maxFd; fd++) close((int)fd);
        if(chdir(cwd) != 0) { /* stay where we are */ }
        execve(argv[0], argv.data(), envp.data());
        const char msg[] = "exec failed\n";
        if(write(2, msg, sizeof(msg) - 1) < 0) { /* nothing to do */ }
        _exit(127);
    }
    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);
    if(pid < 0) {
        close(inPipe[1]); close(outPipe[0]); close(errPipe[0]);
        lock.unlock();
        s->Push(Session::Kind::EXITED, "Couldn't start `claude`: fork failed.");
        return;
    }
    for(int fd : { inPipe[1], outPipe[0], errPipe[0] }) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    s->pid     = pid;
    s->stdinFd = inPipe[1];
    s->started = true;
    for(const std::string &line : s->pendingInput) WriteAll(s->stdinFd, line);
    s->pendingInput.clear();
    lock.unlock();

    int errFd = errPipe[0];
    std::thread([s, errFd] {
        std::string partial;
        char buf[4096];
        for(;;) {
            ssize_t n = read(errFd, buf, sizeof(buf));
            if(n < 0 && errno == EINTR) continue;
            if(n <= 0) break;
            partial.append(buf, (size_t)n);
            size_t nl;
            while((nl = partial.find('\n')) != std::string::npos) {
                s->Push(Session::Kind::ERR, partial.substr(0, nl));
                partial.erase(0, nl + 1);
            }
        }
        if(!partial.empty()) s->Push(Session::Kind::ERR, partial);
        close(errFd);
    }).detach();

    int outFd = outPipe[0];
    std::string partial;
    char buf[16384];
    for(;;) {
        ssize_t n = read(outFd, buf, sizeof(buf));
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) break;
        partial.append(buf, (size_t)n);
        size_t nl;
        while((nl = partial.find('\n')) != std::string::npos) {
            s->Push(Session::Kind::OUT, partial.substr(0, nl));
            partial.erase(0, nl + 1);
        }
    }
    close(outFd);

    int status = 0;
    while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    std::string how;
    if(WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        how = (code == 127) ? "Couldn't run " + program + "."
                            : ssprintf("Claude exited with status %d.", code);
    } else if(WIFSIGNALED(status)) {
        how = ssprintf("Claude was stopped by signal %d.", WTERMSIG(status));
    }
    {
        std::lock_guard<std::mutex> lock2(s->mutex);
        s->exited = true;
        if(s->stdinFd >= 0) {
            close(s->stdinFd);
            s->stdinFd = -1;
        }
    }
    s->Push(Session::Kind::EXITED, how);
}

// Ends the process: closing its input ends a print-mode session, and
// SIGTERM covers a turn in progress; SIGKILL if it's still there in 3 s.
static void StopSession(std::shared_ptr<Session> s) {
    std::lock_guard<std::mutex> lock(s->mutex);
    s->stopped = true;
    if(s->stdinFd >= 0) {
        close(s->stdinFd);
        s->stdinFd = -1;
    }
    if(s->started && !s->exited && s->pid > 0) {
        kill(-s->pid, SIGTERM);
        std::thread([s] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::lock_guard<std::mutex> lock(s->mutex);
            if(!s->exited) kill(-s->pid, SIGKILL);
        }).detach();
    }
}

static void WriteLine(std::shared_ptr<Session> s, const std::string &line) {
    std::lock_guard<std::mutex> lock(s->mutex);
    if(!s->started) {
        s->pendingInput.push_back(line);
    } else if(s->stdinFd >= 0) {
        WriteAll(s->stdinFd, line);
    }
}

#else // WIN32

static void RunSession(std::shared_ptr<Session> s, LaunchParams params) {
    s->Push(Session::Kind::EXITED,
            "The chat with Claude isn't available on Windows yet.");
}
static void StopSession(std::shared_ptr<Session> s) {
    std::lock_guard<std::mutex> lock(s->mutex);
    s->stopped = true;
}
static void WriteLine(std::shared_ptr<Session> s, const std::string &line) {}

#endif

//-----------------------------------------------------------------------------
// Claude's output, on the GUI thread.
//-----------------------------------------------------------------------------
static std::string Shorten(std::string s, size_t max) {
    for(char &c : s) {
        if(c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    if(s.size() > max) {
        size_t cut = max;
        // Don't cut a UTF-8 sequence in half.
        while(cut > 0 && (s[cut] & 0xC0) == 0x80) cut--;
        s = s.substr(0, cut) + "...";
    }
    return s;
}

static std::string ToolName(const std::string &name) {
    const std::string prefix = "mcp__solvespace__";
    if(name.compare(0, prefix.size(), prefix) == 0) return name.substr(prefix.size());
    return name;
}

static std::string ToolResultText(const json &content) {
    if(content.is_string()) return content.get<std::string>();
    std::string text;
    if(content.is_array()) {
        for(const json &item : content) {
            if(item.value("type", "") == "text") {
                if(!text.empty()) text += " ";
                text += item.value("text", "");
            }
        }
    }
    return text;
}

static void HandleMessage(const json &msg) {
    using Kind = SolveSpaceUI::ChatEntry::Kind;
    std::string type = msg.value("type", "");

    if(type == "system" && msg.value("subtype", "") == "init") {
        // Claude Code has read the MCP configuration by now.
        RemoveConfigFile();
        std::string model = msg.value("model", "");
        std::string mcpStatus = "not connected";
        if(msg.contains("mcp_servers") && msg["mcp_servers"].is_array()) {
            for(const json &server : msg["mcp_servers"]) {
                if(server.value("name", "") == "solvespace") {
                    mcpStatus = server.value("status", "unknown");
                }
            }
        }
        readyStatus = model + (mcpStatus == "connected" ? ", connected to SolveSpace"
                                                        : ", SolveSpace " + mcpStatus);
        SS.chat.status = readyStatus;
        if(mcpStatus != "connected") {
            Failure("Claude couldn't connect to SolveSpace's MCP server (" +
                    mcpStatus + "); it can't change the model.");
        }
    } else if(type == "stream_event" && msg.contains("event")) {
        const json &event = msg["event"];
        std::string eventType = event.value("type", "");
        if(eventType == "message_start") {
            sawTextDelta = false;
            streamingEntry = -1;
        } else if(eventType == "content_block_start") {
            streamingEntry = -1;
        } else if(eventType == "content_block_delta" && event.contains("delta")) {
            const json &delta = event["delta"];
            if(delta.value("type", "") == "text_delta") {
                if(streamingEntry < 0) {
                    AddEntry(Kind::CLAUDE, "");
                    streamingEntry = (int)SS.chat.entries.size() - 1;
                }
                SS.chat.entries[streamingEntry].text += delta.value("text", "");
                sawTextDelta = true;
            }
        } else if(eventType == "content_block_stop") {
            streamingEntry = -1;
        }
    } else if(type == "assistant" && msg.contains("message")) {
        const json &content = msg["message"].value("content", json::array());
        for(const json &block : content) {
            std::string blockType = block.value("type", "");
            if(blockType == "tool_use") {
                std::string name = ToolName(block.value("name", ""));
                const json &input = block.contains("input") ? block["input"] : json();
                std::string args;
                if(name == "batch" && input.is_object() && input.contains("calls") &&
                   input["calls"].is_array()) {
                    // The calls in it, which say more than its arguments.
                    for(const json &call : input["calls"]) {
                        args += (args.empty() ? ": " : ", ") +
                                (call.is_object() ? call.value("tool", "?") : "?");
                    }
                } else if(input.is_object() && !input.empty()) {
                    args = " " + input.dump();
                }
                AddEntry(Kind::TOOL, Shorten(name + args, 100));
            } else if(blockType == "text" && !sawTextDelta) {
                // Not streamed (no partial messages): the whole text at once.
                AddEntry(Kind::CLAUDE, block.value("text", ""));
            }
        }
    } else if(type == "user" && msg.contains("message")) {
        const json &content = msg["message"].value("content", json::array());
        if(content.is_array()) {
            for(const json &block : content) {
                if(block.value("type", "") == "tool_result" &&
                   block.value("is_error", false)) {
                    std::string text = block.contains("content")
                                       ? ToolResultText(block["content"]) : "";
                    Failure(Shorten("tool failed: " + text, 400));
                }
            }
        }
    } else if(type == "result") {
        // One for each message sent (Claude answers queued ones in turn).
        SS.chat.pending = std::max(0, SS.chat.pending - 1);
        if(!readyStatus.empty()) SS.chat.status = readyStatus;
        streamingEntry = -1;
        std::string subtype = msg.value("subtype", "");
        if(interrupted) {
            Note("stopped");
        } else if(msg.value("is_error", false) || subtype != "success") {
            std::string text = msg.value("result", "");
            if(text.empty() && msg.contains("errors") && msg["errors"].is_array() &&
               !msg["errors"].empty() && msg["errors"][0].is_string()) {
                text = msg["errors"][0].get<std::string>();
            }
            Failure(text.empty() ? "Claude stopped with an error (" + subtype + ")."
                                 : text);
        } else {
            double ms = msg.value("duration_ms", 0.0);
            double cost = msg.value("total_cost_usd", 0.0);
            std::string note = ssprintf("%.1f s", ms / 1000.0);
            if(cost > 0) note += ssprintf(", $%.3f", cost);
            Note(note);
        }
        interrupted = false;
    }
}

static void Pump() {
    std::shared_ptr<Session> s = session;
    if(!s) return;

    std::deque<std::pair<Session::Kind, std::string>> items;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        items.swap(s->queue);
    }

    bool changed = !items.empty();
    for(auto &item : items) {
        switch(item.first) {
            case Session::Kind::OUT: {
                json msg = json::parse(item.second, nullptr, /*allow_exceptions=*/false);
                if(msg.is_object()) {
                    try {
                        HandleMessage(msg);
                    } catch(const json::exception &e) {
                        dbp("chat: unexpected message from claude: %s", e.what());
                    }
                }
                break;
            }
            case Session::Kind::ERR:
                stderrTail.push_back(item.second);
                if(stderrTail.size() > 8) stderrTail.pop_front();
                break;

            case Session::Kind::EXITED: {
                std::string text = item.second;
                for(const std::string &line : stderrTail) {
                    if(!line.empty()) text += "\n" + line;
                }
                if(!text.empty()) Failure(text);
                session.reset();
                RemoveConfigFile();
                SS.chat.running = false;
                SS.chat.pending = 0;
                SS.chat.status  = "not running";
                streamingEntry  = -1;
                break;
            }
        }
        if(!session) break;
    }

    if(changed) ShowChanges();
    if(session) pumpTimer->RunAfter(CHAT_PUMP_PERIOD_MS);
}

//-----------------------------------------------------------------------------
// The hooks in SS.chat.
//-----------------------------------------------------------------------------
static bool Start() {
    // Claude talks to us through the MCP server; start it for the session if
    // the user hasn't switched it on, trying a few ports if the usual one is
    // taken (e.g. by another SolveSpace).
    uint16_t port = RunningPort();
    if(port == 0) {
        for(int i = 0; i < 10 && port == 0; i++) {
            int candidate = SS.mcp.port + i;
            if(candidate > 65535) break;
            if(StartServer((uint16_t)candidate)) port = (uint16_t)candidate;
        }
        if(port == 0) {
            Failure("Couldn't start SolveSpace's MCP server, which Claude needs: "
                    "the ports from " + std::to_string(SS.mcp.port) + " are in use.");
            return false;
        }
        Note(ssprintf("started SolveSpace's MCP server on port %u for the chat", port));
    }

    json server = {
        { "type", "http" },
        { "url",  ssprintf("http://127.0.0.1:%u/mcp", port) },
    };
    std::string token = CurrentToken();
    if(!token.empty()) {
        server["headers"] = { { "Authorization", "Bearer " + token } };
    }
    json config = { { "mcpServers", { { "solvespace", server } } } };

    // The token goes in a file only we can read (mkstemp makes it 0600), not
    // on the command line, where other users could see it.
    RemoveConfigFile();
#if !defined(WIN32)
    const char *tmp = getenv("TMPDIR");
    std::string dir = (tmp && *tmp) ? tmp : "/tmp";
    if(dir.back() != '/') dir += "/";
    std::string templ = dir + "solvespace-chat-XXXXXX";
    std::vector<char> name(templ.begin(), templ.end());
    name.push_back('\0');
    int fd = mkstemp(name.data());
    if(fd < 0) {
        Failure("Couldn't create a temporary file in " + dir + ".");
        return false;
    }
    std::string text = config.dump();
    bool ok = WriteAll(fd, text);
    close(fd);
    configPath = name.data();
    if(!ok) {
        RemoveConfigFile();
        Failure("Couldn't write a temporary file in " + dir + ".");
        return false;
    }
#endif

    LaunchParams params;
    params.args = {
        "-p",
        "--input-format",  "stream-json",
        "--output-format", "stream-json",
        "--verbose",
        "--include-partial-messages",
        "--strict-mcp-config",
        "--mcp-config",    configPath,
        // Only SolveSpace's tools: no shell, no file access.
        "--tools",         "",
        "--allowedTools",  "mcp__solvespace",
        "--permission-mode", "dontAsk",
        "--append-system-prompt", ChatSystemPrompt,
    };
    // Claude Code keeps its session history per directory: use the open
    // file's, else the home directory.
    if(!SS.saveFile.IsEmpty()) {
        params.cwd = SS.saveFile.Parent().raw;
    } else {
        const char *home = getenv("HOME");
        params.cwd = (home && *home) ? home : "/";
    }

    session = std::make_shared<Session>();
    stderrTail.clear();
    readyStatus.clear();
    streamingEntry = -1;
    interrupted = false;
    SS.chat.running = true;
    SS.chat.status = "starting Claude...";

    std::shared_ptr<Session> s = session;
    std::thread([s, params] { RunSession(s, params); }).detach();
    pumpTimer->RunAfter(CHAT_PUMP_PERIOD_MS);
    return true;
}

// May be called during a reply: Claude queues the message and answers it
// after the reply (stopping the reply doesn't drop it).
static void Send(const std::string &text) {
    AddEntry(SolveSpaceUI::ChatEntry::Kind::USER, text);
    if(!session && !Start()) {
        ShowChanges();
        return;
    }
    json msg = {
        { "type", "user" },
        { "message", { { "role", "user" }, { "content", text } } },
    };
    WriteLine(session, msg.dump() + "\n");
    SS.chat.pending++;
    ShowChanges();
}

static void Interrupt() {
    if(!session || SS.chat.pending == 0) return;
    interrupted = true;
    json msg = {
        { "type", "control_request" },
        { "request_id", ssprintf("solvespace_%d", ++requestCount) },
        { "request", { { "subtype", "interrupt" } } },
    };
    WriteLine(session, msg.dump() + "\n");
    SS.chat.status = "stopping...";
    ShowChanges();
}

static void EndSession() {
    if(session) {
        StopSession(session);
        session.reset();
    }
    RemoveConfigFile();
    SS.chat.running = false;
    SS.chat.pending = 0;
    streamingEntry  = -1;
}

static void Reset() {
    EndSession();
    SS.chat.entries.clear();
    SS.chat.status.clear();
    ShowChanges();
}

} // namespace

void SetupChat() {
#if !defined(WIN32)
    // Writing to a Claude that has exited must not kill SolveSpace.
    signal(SIGPIPE, SIG_IGN);
#endif
    pumpTimer = Platform::CreateTimer();
    pumpTimer->onTimeout = Pump;

    SS.chat.send      = Send;
    SS.chat.interrupt = Interrupt;
    SS.chat.reset     = Reset;
    SS.chat.shutdown  = EndSession;
}

} // namespace Mcp
} // namespace SolveSpace
