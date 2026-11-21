#!/usr/bin/env python3
"""Smoke test for the SolveSpace MCP server (Milestone 0).

Start SolveSpace with MCP enabled (./run-mcp.sh), preferably with a new
document, then run:

    python3 test/mcp/mcp_smoke.py [--url http://127.0.0.1:47474/mcp] [--png out.png]

It speaks raw JSON-RPC over HTTP (standard library only), creates an XY
workplane sketch with a 40x20 mm rectangle, checks the solved corners, and
saves a screenshot.
"""
import argparse
import base64
import json
import sys
import urllib.request

_next_id = 0


def rpc(url, method, params=None, notify=False):
    global _next_id
    msg = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        msg["params"] = params
    if not notify:
        _next_id += 1
        msg["id"] = _next_id
    req = urllib.request.Request(
        url, data=json.dumps(msg).encode(),
        headers={"Content-Type": "application/json",
                 "Accept": "application/json, text/event-stream"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        body = resp.read()
        if notify:
            assert resp.status == 202, resp.status
            return None
    reply = json.loads(body)
    if "error" in reply:
        raise RuntimeError(f"{method}: {reply['error']}")
    return reply["result"]


def call(url, name, **args):
    result = rpc(url, "tools/call", {"name": name, "arguments": args})
    if result.get("isError"):
        raise RuntimeError(f"{name} failed: {result['content'][0]['text']}")
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:47474/mcp")
    ap.add_argument("--png", default=None, help="where to save the screenshot")
    a = ap.parse_args()

    init = rpc(a.url, "initialize", {
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "mcp_smoke", "version": "0"}})
    print("server:", init["serverInfo"], "protocol:", init["protocolVersion"])
    rpc(a.url, "notifications/initialized", notify=True)

    tools = [t["name"] for t in rpc(a.url, "tools/list")["tools"]]
    print("tools:", tools)

    g = call(a.url, "create_workplane_sketch", plane="XY")["structuredContent"]
    print("group:", g["group"], g["name"])

    r = call(a.url, "sketch_rectangle", corner1=[0, 0], corner2=[40, 20])
    s = r["structuredContent"]
    corners = sorted(tuple(round(x, 6) for x in c["uv"]) for c in s["corners"])
    print("corners:", corners, "solve:", s["solve"])
    assert corners == [(0, 0), (0, 20), (40, 0), (40, 20)], corners
    assert s["solve"]["status"] == "ok", s["solve"]
    assert s["solve"]["dof"] == 4, s["solve"]

    ents = call(a.url, "list_entities")["structuredContent"]
    lines = [e for e in ents["entities"] if e["type"] == "line_segment"]
    assert len(lines) == 4, lines
    print("lines:", sorted(round(l["length"], 6) for l in lines),
          "constraints:", len(ents["constraints"]))

    summary = call(a.url, "get_model_summary")["structuredContent"]
    print("groups:", [(x["handle"], x["type"]) for x in summary["groups"]])

    shot = call(a.url, "screenshot", width=640, height=480, zoom_to_fit=True)
    img = next(c for c in shot["content"] if c["type"] == "image")
    png = base64.b64decode(img["data"])
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    print(f"screenshot: {len(png)} bytes")
    if a.png:
        with open(a.png, "wb") as f:
            f.write(png)
        print("saved", a.png)

    print("OK")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001
        print("FAIL:", e, file=sys.stderr)
        sys.exit(1)
