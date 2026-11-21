//-----------------------------------------------------------------------------
// The MCP (Model Context Protocol) server, which lets a local MCP client
// such as Claude Code drive the running SolveSpace GUI.
//
// Threading: the HTTP server runs on background threads; every tool call is
// queued and executed on the GUI thread by a timer pump, because the sketch
// and UI state are not thread-safe.
//-----------------------------------------------------------------------------
#ifndef SOLVESPACE_MCP_H
#define SOLVESPACE_MCP_H

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace SolveSpace {
namespace Mcp {

using json = nlohmann::json;

// Thrown by tool handlers to report a tool error (an `isError` result).
class ToolError : public std::runtime_error {
public:
    explicit ToolError(const std::string &msg) : std::runtime_error(msg) {}
};

struct ToolResult {
    // The structured result; also sent as JSON text content.
    json                structured = json::object();
    // Extra content items, e.g. {"type":"image", ...}.
    std::vector<json>   content;
    // If set, called on the HTTP thread after the handler has returned on
    // the GUI thread, to finish the result; it may block (e.g. waiting for
    // the window to be redrawn) and throw ToolError. It must not touch the
    // sketch or UI state.
    std::function<void(ToolResult *)> finishOffGuiThread;
};

struct Tool {
    std::string name;
    std::string description;
    json        inputSchema;
    // Runs on the GUI thread. Throws ToolError on failure.
    std::function<void(const json &args, ToolResult *result)> handler;
};

// The registry of tools (defined in mcptools.cpp).
const std::vector<Tool> &GetTools();

// The text sent as `instructions` in the initialize result.
extern const char *ServerInstructions;

// Installs the server's hooks into SS.mcp (the configuration screen uses
// them), and starts the server: on portFromArgs if it isn't 0 (see
// PortFromArgs), else on the saved port if the saved settings enable it.
// The SOLVESPACE_MCP_TOKEN environment variable, if set, overrides the saved
// token. Must be called on the GUI thread, after SS.Init().
void Setup(uint16_t portFromArgs);

// Starts the server on 127.0.0.1:port, stopping it first if it's running.
// Returns false (with a message on stderr, and in the status shown in the
// configuration screen) on failure. On the GUI thread.
bool StartServer(uint16_t port);
// Fails queued calls, closes the connections and waits for their threads.
// On the GUI thread, and not from inside a tool call.
void StopServer();

// Determines the port to use from the command line (--mcp or --mcp=PORT,
// which are removed from args) and the SOLVESPACE_MCP_PORT environment
// variable. Returns 0 if MCP was not requested.
uint16_t PortFromArgs(std::vector<std::string> *args);

static const uint16_t DEFAULT_PORT = 47474;

} // namespace Mcp
} // namespace SolveSpace

#endif
