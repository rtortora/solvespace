//-----------------------------------------------------------------------------
// A minimal HTTP/1.1 server for the MCP endpoint; see httpserver.h.
//-----------------------------------------------------------------------------
#include "httpserver.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#   include <winsock2.h>
#   include <ws2tcpip.h>
    typedef int socklen_t;
#   define CLOSE_SOCKET closesocket
#   pragma comment(lib, "ws2_32.lib")
#else
#   include <arpa/inet.h>
#   include <netinet/in.h>
#   include <sys/socket.h>
#   include <sys/time.h>
#   include <unistd.h>
#   define CLOSE_SOCKET ::close
#endif

namespace SolveSpace {
namespace Mcp {

static const size_t MAX_HEADER_BYTES = 64 * 1024;
static const size_t MAX_BODY_BYTES   = 16 * 1024 * 1024;
// Idle keep-alive connections are closed after this long.
static const int    IDLE_TIMEOUT_SEC = 60;

static const char *StatusText(int status) {
    switch(status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string Trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if(a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool SendAll(intptr_t fd, const std::string &data) {
    size_t sent = 0;
    while(sent < data.size()) {
#if defined(_WIN32)
        int n = send((SOCKET)fd, data.data() + sent, (int)(data.size() - sent), 0);
#elif defined(MSG_NOSIGNAL)
        ssize_t n = send((int)fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
        ssize_t n = send((int)fd, data.data() + sent, data.size() - sent, 0);
#endif
        if(n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static void SendResponse(intptr_t fd, const HttpResponse &resp, bool keepAlive) {
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " +
                      StatusText(resp.status) + "\r\n";
    if(!resp.body.empty() || resp.status == 200) {
        out += "Content-Type: " + resp.contentType + "\r\n";
    }
    out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    for(const auto &h : resp.headers) {
        out += h.first + ": " + h.second + "\r\n";
    }
    out += "\r\n";
    out += resp.body;
    SendAll(fd, out);
}

HttpServer::HttpServer() : listenFd(-1), running(false) {}

HttpServer::~HttpServer() {
    Stop();
}

bool HttpServer::Start(uint16_t port, Handler h, std::string *error) {
#if defined(_WIN32)
    WSADATA wsaData;
    if(WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        *error = "WSAStartup failed";
        return false;
    }
#endif
    intptr_t fd = (intptr_t)socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        *error = "socket() failed";
        return false;
    }
    int yes = 1;
#if defined(_WIN32)
    // On Windows SO_REUSEADDR would let a second instance share the port.
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof(yes));
#else
    setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#endif

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(bind((int)fd, (sockaddr *)&addr, sizeof(addr)) != 0) {
        *error = "port " + std::to_string(port) + " is already in use";
        CLOSE_SOCKET((int)fd);
        return false;
    }
    if(listen((int)fd, 16) != 0) {
        *error = "listen() failed";
        CLOSE_SOCKET((int)fd);
        return false;
    }

    listenFd = fd;
    handler  = std::make_shared<Handler>(h);
    running  = true;
    acceptThread = std::thread([this] { AcceptLoop(); });
    return true;
}

void HttpServer::Stop() {
    if(!running) return;
    running = false;
#if defined(_WIN32)
    closesocket((SOCKET)listenFd);
#else
    // shutdown() wakes up a blocked accept() on Linux; close() does on macOS.
    shutdown((int)listenFd, SHUT_RDWR);
    CLOSE_SOCKET((int)listenFd);
#endif
    listenFd = -1;
    if(acceptThread.joinable()) acceptThread.join();
}

void HttpServer::AcceptLoop() {
    while(running) {
        sockaddr_in peer = {};
        socklen_t len = sizeof(peer);
        intptr_t client = (intptr_t)accept((int)listenFd, (sockaddr *)&peer, &len);
        if(client < 0) {
            if(!running) break;
            continue;
        }
        // Defence in depth: we only bound loopback, but check anyway.
        if(ntohl(peer.sin_addr.s_addr) >> 24 != 127) {
            CLOSE_SOCKET((int)client);
            continue;
        }
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
        int one = 1;
        setsockopt((int)client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        std::thread(ServeConnection, client, handler).detach();
    }
}

void HttpServer::ServeConnection(intptr_t fd, std::shared_ptr<Handler> handler) {
#if defined(_WIN32)
    DWORD tv = IDLE_TIMEOUT_SEC * 1000;
#else
    timeval tv = {};
    tv.tv_sec = IDLE_TIMEOUT_SEC;
#endif
    setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    std::string buf;
    char chunk[16384];
    auto ReadMore = [&]() -> bool {
        int n = (int)recv((int)fd, chunk, sizeof(chunk), 0);
        if(n <= 0) return false;
        buf.append(chunk, (size_t)n);
        return true;
    };

    for(;;) {
        // Read the request head.
        size_t headEnd;
        while((headEnd = buf.find("\r\n\r\n")) == std::string::npos) {
            if(buf.size() > MAX_HEADER_BYTES || !ReadMore()) {
                CLOSE_SOCKET((int)fd);
                return;
            }
        }

        HttpRequest req;
        std::string head = buf.substr(0, headEnd);
        buf.erase(0, headEnd + 4);

        size_t lineEnd = head.find("\r\n");
        std::string requestLine = head.substr(0, lineEnd);
        size_t sp1 = requestLine.find(' ');
        size_t sp2 = requestLine.find(' ', sp1 + 1);
        if(sp1 == std::string::npos || sp2 == std::string::npos) {
            HttpResponse bad;
            bad.status = 400;
            SendResponse(fd, bad, false);
            CLOSE_SOCKET((int)fd);
            return;
        }
        req.method = requestLine.substr(0, sp1);
        req.path   = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
        size_t q = req.path.find('?');
        if(q != std::string::npos) req.path.erase(q);

        size_t pos = (lineEnd == std::string::npos) ? head.size() : lineEnd + 2;
        while(pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            if(e == std::string::npos) e = head.size();
            std::string line = head.substr(pos, e - pos);
            size_t colon = line.find(':');
            if(colon != std::string::npos) {
                req.headers[ToLower(Trim(line.substr(0, colon)))] =
                    Trim(line.substr(colon + 1));
            }
            pos = e + 2;
        }

        size_t contentLength = 0;
        std::string cl = req.Header("content-length");
        if(!cl.empty()) contentLength = (size_t)strtoull(cl.c_str(), NULL, 10);
        if(contentLength > MAX_BODY_BYTES) {
            HttpResponse tooBig;
            tooBig.status = 413;
            SendResponse(fd, tooBig, false);
            CLOSE_SOCKET((int)fd);
            return;
        }
        while(buf.size() < contentLength) {
            if(!ReadMore()) {
                CLOSE_SOCKET((int)fd);
                return;
            }
        }
        req.body = buf.substr(0, contentLength);
        buf.erase(0, contentLength);

        bool keepAlive = ToLower(req.Header("connection")) != "close";

        HttpResponse resp;
        try {
            resp = (*handler)(req);
        } catch(...) {
            resp = HttpResponse();
            resp.status = 500;
        }
        SendResponse(fd, resp, keepAlive);
        if(!keepAlive) break;
    }
    CLOSE_SOCKET((int)fd);
}

} // namespace Mcp
} // namespace SolveSpace
