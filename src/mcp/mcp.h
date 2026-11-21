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

// Starts the server on 127.0.0.1:port. Must be called on the GUI thread,
// after SS.Init(). Returns false (with a message on stderr) on failure.
bool StartServer(uint16_t port);
void StopServer();

// Determines the port to use from the command line (--mcp or --mcp=PORT,
// which are removed from args) and the SOLVESPACE_MCP_PORT environment
// variable. Returns 0 if MCP was not requested.
uint16_t PortFromArgs(std::vector<std::string> *args);

static const uint16_t DEFAULT_PORT = 47474;

} // namespace Mcp
} // namespace SolveSpace

#endif
