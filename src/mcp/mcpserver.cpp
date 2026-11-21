//-----------------------------------------------------------------------------
// MCP server: Streamable HTTP transport (JSON responses only), JSON-RPC
// routing, and marshalling of tool calls onto the GUI thread.
//-----------------------------------------------------------------------------
#include "solvespace.h"
#include "config.h"
#include "mcp.h"
#include "httpserver.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <future>
#include <mutex>
#include <random>

namespace SolveSpace {
namespace Mcp {

static const char *SERVER_NAME    = "solvespace";
static const int   CALL_TIMEOUT_S = 30;
static const int   PUMP_PERIOD_MS = 15;

static const char *SUPPORTED_VERSIONS[] = {
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05",
};

//-----------------------------------------------------------------------------
// Queue of jobs to run on the GUI thread.
//-----------------------------------------------------------------------------
enum JobState { QUEUED, RUNNING, ABANDONED };

// What a tool call produced on the GUI thread.
struct CallOutcome {
    ToolResult               result;
    std::vector<std::string> errors;
    std::vector<std::string> messages;
    bool                     cancelledPending = false;
};

struct Job {
    std::function<CallOutcome()> work;
    std::promise<CallOutcome>    result;
    std::atomic<int>             state{QUEUED};
};

// State shared by the GUI thread and the HTTP threads, guarded by `mutex`.
// It is allocated once and never destroyed, so that a connection thread
// still running at exit can't touch it during static destruction.
struct SharedState {
    std::mutex                          mutex;
    std::deque<std::shared_ptr<Job>>    queue;
    // False while the server is stopped or stopping: no new jobs.
    bool                                accepting = false;
    std::string                         sessionId;
    // If not empty, requests need "Authorization: Bearer <token>".
    std::string                         token;
    std::string                         lastClient;
    std::time_t                         lastRequest = 0;
    // Set when the status shown in the configuration screen has changed.
    std::atomic<bool>                   statusChanged{false};
};

static SharedState &Shared() {
    static SharedState *state = new SharedState();
    return *state;
}

// GUI-thread state.
static Platform::TimerRef                   pumpTimer;
static std::unique_ptr<HttpServer>          httpServer;
static uint16_t                             runningPort  = 0;
// From --mcp[=PORT] / SOLVESPACE_MCP_PORT; overrides the saved settings
// until the user changes them in the configuration screen.
static uint16_t                             overridePort = 0;
// From SOLVESPACE_MCP_TOKEN; overrides the saved token likewise.
static bool                                 tokenFromEnv = false;
static std::string                          envToken;
// Why the server last failed to start, if it did.
static std::string                          startError;

static void Pump() {
    if(Shared().statusChanged.exchange(false) &&
       SS.TW.shown.screen == TextWindow::Screen::CONFIGURATION) {
        SS.ScheduleShowTW();
    }
    // Don't run tools inside a modal dialog's nested event loop: the code
    // that opened the dialog may hold pointers into the sketch that a tool
    // would invalidate. Try again once the dialog is closed.
    if(Platform::modalDepth > 0) {
        pumpTimer->RunAfter(PUMP_PERIOD_MS);
        return;
    }
    for(;;) {
        std::shared_ptr<Job> job;
        {
            SharedState &shared = Shared();
            std::lock_guard<std::mutex> lock(shared.mutex);
            if(shared.queue.empty()) break;
            job = shared.queue.front();
            shared.queue.pop_front();
        }
        // If the client already gave up waiting, don't run it: it would
        // change the model behind the client's back.
        int expected = QUEUED;
        if(!job->state.compare_exchange_strong(expected, RUNNING)) continue;
        try {
            job->result.set_value(job->work());
        } catch(...) {
            job->result.set_exception(std::current_exception());
        }
    }
    pumpTimer->RunAfter(PUMP_PERIOD_MS);
}

// Called on an HTTP thread; blocks until the GUI thread has run `work`.
static CallOutcome RunOnGuiThread(std::function<CallOutcome()> work) {
    std::shared_ptr<Job> job = std::make_shared<Job>();
    job->work = work;
    std::future<CallOutcome> future = job->result.get_future();
    {
        SharedState &shared = Shared();
        std::lock_guard<std::mutex> lock(shared.mutex);
        if(!shared.accepting) {
            throw ToolError("SolveSpace's MCP server is stopping; the call was not "
                            "executed.");
        }
        shared.queue.push_back(job);
    }
    if(future.wait_for(std::chrono::seconds(CALL_TIMEOUT_S)) != std::future_status::ready) {
        int expected = QUEUED;
        if(job->state.compare_exchange_strong(expected, ABANDONED)) {
            throw ToolError("SolveSpace UI is busy (is a modal dialog open?); "
                            "the call was not executed. Close the dialog and retry.");
        }
        // It started running; it's just slow. Wait for it to finish.
        if(future.wait_for(std::chrono::seconds(10 * CALL_TIMEOUT_S)) !=
           std::future_status::ready) {
            throw ToolError("The call is still running in SolveSpace; its outcome is "
                            "unknown. Check the model with get_model_summary before "
                            "retrying.");
        }
    }
    return future.get();
}

//-----------------------------------------------------------------------------
// Tool execution, on the GUI thread.
//-----------------------------------------------------------------------------
static const Tool *FindTool(const std::string &name) {
    for(const Tool &t : GetTools()) {
        if(t.name == name) return &t;
    }
    return NULL;
}

static CallOutcome CallToolOnGuiThread(const Tool *tool, const json &args) {
    MessageCapture capture;
    activeMessageCapture = &capture;

    CallOutcome out;
    // If the user is half-way through drawing something, abandon it; the
    // pending operation would otherwise interfere with the tool.
    using Pending = GraphicsWindow::Pending;
    Pending op = SS.GW.pending.operation;
    if(op != Pending::NONE) {
        out.cancelledPending = true;
        // As pressing Esc does: these operations have already added the
        // geometry being drawn, which an undo removes.
        SS.GW.ClearSuper();
        if(op == Pending::DRAGGING_NEW_POINT || op == Pending::DRAGGING_NEW_LINE_POINT ||
           op == Pending::DRAGGING_NEW_ARC_POINT || op == Pending::DRAGGING_NEW_RADIUS) {
            SS.UndoUndo();
            SS.UndoClearStack(&SS.redo);
            SS.UndoEnableMenus();
        }
    }

    std::string failure;
    try {
        tool->handler(args, &out.result);
    } catch(ToolError &e) {
        failure = e.what();
    } catch(json::exception &e) {
        failure = std::string("Invalid arguments: ") + e.what();
    } catch(std::exception &e) {
        failure = std::string("Internal error: ") + e.what();
    }
    activeMessageCapture = NULL;

    SS.GW.Invalidate();
    SS.ScheduleShowTW();

    out.errors = capture.errors;
    if(!failure.empty()) out.errors.insert(out.errors.begin(), failure);
    out.messages = capture.messages;
    if(!out.errors.empty()) out.result.finishOffGuiThread = nullptr;
    return out;
}

// Turns the outcome into a tools/call result; on the HTTP thread.
static json CallResultJson(CallOutcome &out) {
    if(out.errors.empty() && out.result.finishOffGuiThread) {
        try {
            out.result.finishOffGuiThread(&out.result);
        } catch(ToolError &e) {
            out.errors.push_back(e.what());
        } catch(std::exception &e) {
            out.errors.push_back(std::string("Internal error: ") + e.what());
        }
    }
    json wire;
    json content = json::array();
    if(!out.errors.empty()) {
        std::string text;
        for(const std::string &e : out.errors) {
            if(!text.empty()) text += "\n";
            text += e;
        }
        content.push_back({{"type", "text"}, {"text", text}});
        wire["isError"] = true;
    } else {
        ToolResult &result = out.result;
        if(!out.messages.empty()) {
            result.structured["messages"] = out.messages;
        }
        if(out.cancelledPending) {
            result.structured["note"] =
                "An in-progress drawing operation in the GUI was cancelled.";
        }
        content.push_back({{"type", "text"}, {"text", result.structured.dump()}});
        for(const json &c : result.content) content.push_back(c);
        wire["structuredContent"] = result.structured;
        wire["isError"] = false;
    }
    wire["content"] = content;
    return wire;
}

//-----------------------------------------------------------------------------
// JSON-RPC handling, on an HTTP thread.
//-----------------------------------------------------------------------------
static json RpcError(const json &id, int code, const std::string &message) {
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", code}, {"message", message}}}};
}

static json RpcResult(const json &id, const json &result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

static json HandleInitialize(const json &params) {
    std::string requested = params.value("protocolVersion", "");
    std::string version = SUPPORTED_VERSIONS[0];
    for(const char *v : SUPPORTED_VERSIONS) {
        if(requested == v) version = v;
    }
    std::string clientName = "unknown client";
    if(params.contains("clientInfo") && params["clientInfo"].is_object()) {
        clientName = params["clientInfo"].value("name", clientName);
    }
    dbp("MCP: client connected: %s (protocol %s)", clientName.c_str(), version.c_str());
    {
        SharedState &shared = Shared();
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.lastClient = clientName;
    }
    Shared().statusChanged = true;

    return {
        {"protocolVersion", version},
        {"capabilities", {{"tools", {{"listChanged", false}}}}},
        {"serverInfo", {{"name", SERVER_NAME}, {"title", "SolveSpace"},
                        {"version", PACKAGE_VERSION}}},
        {"instructions", ServerInstructions},
    };
}

static json HandleToolsList() {
    json tools = json::array();
    for(const Tool &t : GetTools()) {
        tools.push_back({{"name", t.name}, {"description", t.description},
                         {"inputSchema", t.inputSchema}});
    }
    return {{"tools", tools}};
}

// Returns a JSON-RPC response, or null for notifications.
static json HandleMessage(const json &msg) {
    if(!msg.is_object() || !msg.contains("jsonrpc") || msg["jsonrpc"] != "2.0" ||
       !msg.contains("method") || !msg["method"].is_string()) {
        return RpcError(msg.is_object() && msg.contains("id") ? msg["id"] : json(),
                        -32600, "Invalid Request");
    }
    std::string method = msg["method"];
    json params = msg.contains("params") ? msg["params"] : json::object();
    bool isNotification = !msg.contains("id");
    json id = isNotification ? json() : msg["id"];

    if(isNotification) {
        // notifications/initialized, notifications/cancelled, ...: nothing to do.
        return json();
    }

    try {
        if(method == "initialize") {
            return RpcResult(id, HandleInitialize(params));
        } else if(method == "ping") {
            return RpcResult(id, json::object());
        } else if(method == "tools/list") {
            return RpcResult(id, HandleToolsList());
        } else if(method == "tools/call") {
            std::string name = params.value("name", "");
            json args = params.contains("arguments") && params["arguments"].is_object()
                ? params["arguments"] : json::object();
            const Tool *tool = FindTool(name);
            if(tool == NULL) return RpcError(id, -32602, "Unknown tool: " + name);
            try {
                CallOutcome out = RunOnGuiThread([=] { return CallToolOnGuiThread(tool, args); });
                return RpcResult(id, CallResultJson(out));
            } catch(ToolError &e) {
                return RpcResult(id, {{"isError", true},
                    {"content", {{{"type", "text"}, {"text", e.what()}}}}});
            }
        } else {
            return RpcError(id, -32601, "Method not found: " + method);
        }
    } catch(std::exception &e) {
        return RpcError(id, -32603, std::string("Internal error: ") + e.what());
    }
}

static bool OriginAllowed(const std::string &origin) {
    if(origin.empty()) return true;   // Non-browser clients send no Origin.
    for(const char *prefix : {"http://127.0.0.1", "http://localhost",
                              "https://127.0.0.1", "https://localhost"}) {
        size_t n = strlen(prefix);
        if(origin.compare(0, n, prefix) == 0 &&
           (origin.size() == n || origin[n] == ':')) {
            return true;
        }
    }
    return false;
}

// Compares in time that depends only on the lengths, so a client can't
// find the token a character at a time by timing.
static bool ConstantTimeEquals(const std::string &a, const std::string &b) {
    if(a.size() != b.size()) return false;
    volatile unsigned char diff = 0;
    for(size_t i = 0; i < a.size(); i++) {
        diff = diff | (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static bool Authorized(const HttpRequest &req) {
    std::string token;
    {
        SharedState &shared = Shared();
        std::lock_guard<std::mutex> lock(shared.mutex);
        token = shared.token;
    }
    if(token.empty()) return true;
    std::string auth = req.Header("authorization");
    // The scheme is case-insensitive (RFC 7235).
    static const char *SCHEME = "bearer ";
    if(auth.size() < strlen(SCHEME)) return false;
    for(size_t i = 0; i < strlen(SCHEME); i++) {
        if(tolower((unsigned char)auth[i]) != SCHEME[i]) return false;
    }
    std::string given = auth.substr(strlen(SCHEME));
    given.erase(0, given.find_first_not_of(' '));
    return ConstantTimeEquals(given, token);
}

static HttpResponse HandleHttp(const HttpRequest &req) {
    HttpResponse resp;
    if(req.path != "/mcp") {
        resp.status = 404;
        resp.body = R"({"error":"not found; the MCP endpoint is /mcp"})";
        return resp;
    }
    if(!OriginAllowed(req.Header("origin"))) {
        resp.status = 403;
        resp.body = R"({"error":"origin not allowed"})";
        return resp;
    }
    if(!Authorized(req)) {
        resp.status = 401;
        resp.headers["WWW-Authenticate"] = "Bearer realm=\"solvespace\"";
        resp.body = R"({"error":"this SolveSpace MCP server requires a token: send )"
                    R"(\"Authorization: Bearer <token>\". The token is in SolveSpace's )"
                    R"(configuration screen."})";
        return resp;
    }
    {
        SharedState &shared = Shared();
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.lastRequest = std::time(NULL);
    }
    if(req.method == "GET") {
        // We don't offer a server-to-client SSE stream.
        resp.status = 405;
        resp.headers["Allow"] = "POST, DELETE";
        return resp;
    }
    if(req.method == "DELETE") {
        resp.status = 200;
        resp.body = "{}";
        return resp;
    }
    if(req.method != "POST") {
        resp.status = 405;
        resp.headers["Allow"] = "POST, DELETE";
        return resp;
    }

    json msg;
    try {
        msg = json::parse(req.body);
    } catch(json::exception &) {
        resp.status = 400;
        resp.body = RpcError(json(), -32700, "Parse error").dump();
        return resp;
    }
    if(msg.is_array()) {
        resp.status = 400;
        resp.body = RpcError(json(), -32600, "Batch requests are not supported").dump();
        return resp;
    }

    json reply = HandleMessage(msg);
    if(msg.is_object() && msg.contains("method") && msg["method"] == "initialize") {
        SharedState &shared = Shared();
        std::lock_guard<std::mutex> lock(shared.mutex);
        resp.headers["Mcp-Session-Id"] = shared.sessionId;
    }
    if(reply.is_null()) {
        resp.status = 202;
        return resp;
    }
    resp.body = reply.dump();
    return resp;
}

//-----------------------------------------------------------------------------
// Lifecycle, on the GUI thread.
//-----------------------------------------------------------------------------
static std::string EffectiveToken() {
    return tokenFromEnv ? envToken : SS.mcp.token;
}

bool StartServer(uint16_t port) {
    StopServer();
    startError.clear();

    std::random_device rd;
    char buf[33];
    snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
    {
        SharedState &shared = Shared();
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.sessionId = buf;
        shared.token     = EffectiveToken();
        shared.accepting = true;
    }

    // Build the tool registry now, on the GUI thread, before any HTTP thread
    // can read it.
    GetTools();

    httpServer.reset(new HttpServer());
    std::string error;
    if(!httpServer->Start(port, HandleHttp, &error)) {
        fprintf(stderr, "MCP server could not start: %s\n", error.c_str());
        startError = error;
        httpServer.reset();
        std::lock_guard<std::mutex> lock(Shared().mutex);
        Shared().accepting = false;
        return false;
    }
    runningPort = port;

    pumpTimer = Platform::CreateTimer();
    pumpTimer->onTimeout = Pump;
    pumpTimer->RunAfter(PUMP_PERIOD_MS);

    fprintf(stderr, "MCP server listening on http://127.0.0.1:%u/mcp\n", port);
    return true;
}

void StopServer() {
    // Fail the calls that are waiting to run, so their connection threads
    // return; new calls are refused.
    std::deque<std::shared_ptr<Job>> pending;
    {
        SharedState &shared = Shared();
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.accepting = false;
        pending.swap(shared.queue);
    }
    for(std::shared_ptr<Job> &job : pending) {
        int expected = QUEUED;
        if(job->state.compare_exchange_strong(expected, ABANDONED)) {
            job->result.set_exception(std::make_exception_ptr(ToolError(
                "SolveSpace's MCP server stopped; the call was not executed.")));
        }
    }
    // A window screenshot waiting for the next frame: dropping the hook
    // breaks its promise, which wakes it up.
    SS.GW.onFrameCaptured = nullptr;

    if(httpServer) {
        if(!httpServer->Stop()) {
            fprintf(stderr, "MCP server: some connections did not close in time\n");
        }
        httpServer.reset();
        fprintf(stderr, "MCP server stopped\n");
    }
    if(pumpTimer) {
        pumpTimer->onTimeout = [] {};
        pumpTimer.reset();
    }
    runningPort = 0;
}

static SolveSpaceUI::McpStatus Status() {
    SolveSpaceUI::McpStatus status = {};
    std::string token = EffectiveToken();
    if(httpServer) {
        status.running = true;
        status.text = ssprintf("listening on http://127.0.0.1:%u/mcp", runningPort);
        if(overridePort != 0) status.text += " (port from command line)";
        if(tokenFromEnv) status.text += " (token from SOLVESPACE_MCP_TOKEN)";
        else if(token.empty()) status.text += ", no token";
    } else if(!startError.empty()) {
        status.error = true;
        status.text = "could not start: " + startError;
    } else {
        status.text = "off";
    }

    SharedState &shared = Shared();
    std::lock_guard<std::mutex> lock(shared.mutex);
    if(!shared.lastClient.empty()) {
        status.lastClient = shared.lastClient;
        if(shared.lastRequest != 0) {
            char when[32];
            std::strftime(when, sizeof(when), "%H:%M:%S",
                          std::localtime(&shared.lastRequest));
            status.lastClient += std::string(", last request at ") + when;
        }
    }
    return status;
}

static std::string ConnectCommand() {
    unsigned port = runningPort != 0 ? runningPort : (unsigned)SS.mcp.port;
    std::string cmd = ssprintf(
        "claude mcp add --transport http solvespace http://127.0.0.1:%u/mcp", port);
    std::string token = EffectiveToken();
    if(!token.empty()) {
        cmd += " --header \"Authorization: Bearer " + token + "\"";
    }
    return cmd;
}

// After the user changed "enabled" or the port in the configuration screen.
static void ApplyServerSettings() {
    overridePort = 0;
    uint16_t want = SS.mcp.enabled ? (uint16_t)SS.mcp.port : 0;
    if(want == runningPort && httpServer) return;
    StopServer();
    startError.clear();
    if(want != 0) StartServer(want);
}

// After the user changed the token in the configuration screen.
static void ApplyToken() {
    tokenFromEnv = false;
    SharedState &shared = Shared();
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.token = SS.mcp.token;
}

void Setup(uint16_t portFromArgs) {
    overridePort = portFromArgs;
    const char *env = getenv("SOLVESPACE_MCP_TOKEN");
    tokenFromEnv = (env != NULL);
    envToken     = env ? env : "";

    SS.mcp.applyServer    = ApplyServerSettings;
    SS.mcp.applyToken     = ApplyToken;
    SS.mcp.status         = Status;
    SS.mcp.connectCommand = ConnectCommand;
    SS.mcp.shutdown       = StopServer;

    if(overridePort != 0) {
        StartServer(overridePort);
    } else if(SS.mcp.enabled) {
        StartServer((uint16_t)SS.mcp.port);
    }
}

static uint16_t ParsePort(const std::string &s) {
    char *end;
    unsigned long v = strtoul(s.c_str(), &end, 10);
    if(*end != '\0' || v == 0 || v > 65535) {
        fprintf(stderr, "MCP: invalid port '%s'; using %u\n", s.c_str(), DEFAULT_PORT);
        return DEFAULT_PORT;
    }
    return (uint16_t)v;
}

uint16_t PortFromArgs(std::vector<std::string> *args) {
    uint16_t port = 0;
    for(auto it = args->begin(); it != args->end();) {
        if(*it == "--mcp") {
            port = DEFAULT_PORT;
            it = args->erase(it);
        } else if(it->compare(0, 6, "--mcp=") == 0) {
            port = ParsePort(it->substr(6));
            it = args->erase(it);
        } else {
            ++it;
        }
    }
    if(port == 0) {
        const char *env = getenv("SOLVESPACE_MCP_PORT");
        if(env && *env) port = ParsePort(env);
    }
    return port;
}

} // namespace Mcp
} // namespace SolveSpace
