"""A tiny MCP client (JSON-RPC over Streamable HTTP, JSON responses only)
for testing the SolveSpace MCP server. Standard library only."""
import json
import urllib.request


class ToolFailed(Exception):
    pass


class Client:
    def __init__(self, url="http://127.0.0.1:47474/mcp"):
        self.url = url
        self.next_id = 0

    def rpc(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notify:
            self.next_id += 1
            msg["id"] = self.next_id
        req = urllib.request.Request(
            self.url, data=json.dumps(msg).encode(),
            headers={"Content-Type": "application/json",
                     "Accept": "application/json, text/event-stream"})
        with urllib.request.urlopen(req, timeout=60) as resp:
            body = resp.read()
            if notify:
                return None
        reply = json.loads(body)
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply["result"]

    def initialize(self, name="mcp-test"):
        result = self.rpc("initialize", {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": name, "version": "0"}})
        self.rpc("notifications/initialized", notify=True)
        return result

    def tools(self):
        return self.rpc("tools/list")["tools"]

    def call_raw(self, _tool, **args):
        return self.rpc("tools/call", {"name": _tool, "arguments": args})

    def call(self, _tool, **args):
        """Calls a tool; returns structuredContent, raises ToolFailed on isError."""
        result = self.call_raw(_tool, **args)
        if result.get("isError"):
            raise ToolFailed(f"{_tool}: {result['content'][0]['text']}")
        return result.get("structuredContent", {})

    def fails(self, _tool, **args):
        """Calls a tool that should fail; returns the error text."""
        result = self.call_raw(_tool, **args)
        if not result.get("isError"):
            raise AssertionError(f"{_tool} unexpectedly succeeded: "
                                 f"{result.get('structuredContent')}")
        return result["content"][0]["text"]
