//-----------------------------------------------------------------------------
// A minimal HTTP/1.1 server for the MCP endpoint. It listens on the loopback
// interface only, reads Content-Length bodies, and calls a handler on a
// per-connection thread. It is deliberately small: it only has to serve
// JSON-RPC POSTs from local MCP clients.
//-----------------------------------------------------------------------------
#ifndef SOLVESPACE_MCP_HTTPSERVER_H
#define SOLVESPACE_MCP_HTTPSERVER_H

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace SolveSpace {
namespace Mcp {

struct HttpRequest {
    std::string method;
    std::string path;
    // Header names are lowercased.
    std::map<std::string, std::string> headers;
    std::string body;

    std::string Header(const std::string &name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::map<std::string, std::string> headers;
    std::string body;
};

class HttpServer {
public:
    typedef std::function<HttpResponse(const HttpRequest &)> Handler;

    HttpServer();
    ~HttpServer();

    // Binds 127.0.0.1:port and starts the accept thread. On failure, returns
    // false and sets *error.
    bool Start(uint16_t port, Handler handler, std::string *error);
    void Stop();

private:
    void AcceptLoop();
    static void ServeConnection(intptr_t fd, std::shared_ptr<Handler> handler);

    intptr_t                    listenFd;
    std::atomic<bool>           running;
    std::thread                 acceptThread;
    std::shared_ptr<Handler>    handler;
};

} // namespace Mcp
} // namespace SolveSpace

#endif
