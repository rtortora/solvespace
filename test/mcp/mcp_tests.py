#!/usr/bin/env python3
"""End-to-end tests for the SolveSpace MCP tools (milestones 0-5).

Start SolveSpace with MCP enabled (e.g. SOLVESPACE_MCP_PORT=47475), then:

    python3 test/mcp/mcp_tests.py --url http://127.0.0.1:47475/mcp --out DIR

The tests start new documents (discarding unsaved changes) and write files
and screenshots to DIR.
"""
import argparse
import base64
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_client import Client, ToolFailed  # noqa: E402

C = None
OUT = None


def close(a, b, tol=1e-6):
    if isinstance(a, (list, tuple)):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return abs(a - b) <= tol


def fresh():
    C.call("new_document", discard_unsaved=True)


def entities(group=None, kind="all"):
    args = {"kind": kind}
    if group:
        args["group"] = group
    return C.call("list_entities", **args)


def points_of(group):
    return [e for e in entities(group, "points")["entities"]]


def origin_point():
    for e in points_of("g:0001"):
        if close(e["xyz"], [0, 0, 0]):
            return e["handle"]
    raise AssertionError("no origin point")


def rect(c1, c2):
    return C.call("sketch_rectangle", corner1=c1, corner2=c2)


def line_along(r, axis):
    """The rectangle line whose direction is along u (0) or v (1)."""
    for ln in r["lines"]:
        a, b = ln["points"][0]["uv"], ln["points"][1]["uv"]
        if abs(a[1 - axis] - b[1 - axis]) < 1e-9:
            return ln
    raise AssertionError("no such line")


def save_png(name, result_raw):
    img = next(c for c in result_raw["content"] if c["type"] == "image")
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(base64.b64decode(img["data"]))
    return path


# ---------------------------------------------------------------------------
# Milestone 2: documents, undo, groups
# ---------------------------------------------------------------------------
def test_document_and_undo():
    fresh()
    s = C.call("get_model_summary")
    assert [g["handle"] for g in s["groups"]] == ["g:0001", "g:0002"], s["groups"]
    assert not s["unsaved"]
    C.call("create_workplane_sketch", plane="XY")
    rect([0, 0], [40, 20])
    assert C.call("get_group")["requests"] == 4
    # One undo removes the whole rectangle; another removes the sketch group.
    C.call("undo")
    assert C.call("get_group")["requests"] == 0
    C.call("redo")
    assert C.call("get_group")["requests"] == 4
    assert "unsaved" in C.fails("new_document")

    path = os.path.join(OUT, "doc.slvs")
    r = C.call("save_file", path=path)
    assert r["file"] == path and os.path.exists(path)
    assert not C.call("get_model_summary")["unsaved"]
    fresh()
    assert len(C.call("get_model_summary")["groups"]) == 2
    r = C.call("open_file", path=path)
    assert len(r["groups"]) == 3, r
    assert any(g["requests"] == 4 for g in r["groups"])
    assert "absolute" in C.fails("open_file", path="relative.slvs", discard_unsaved=True)


def test_groups():
    fresh()
    g = C.call("create_workplane_sketch", plane="XY", name="base")
    assert g["name"] == "base"
    hg = g["group"]
    C.call("set_group_visibility", group=hg, visible=False)
    assert not C.call("get_group", group=hg)["visible"]
    C.call("set_group_visibility", group=hg, visible=True)
    C.call("set_group_properties", group=hg, name="renamed")
    assert C.call("get_group", group=hg)["name"] == "renamed"
    C.call("activate_group", group="g:0002")
    assert C.call("get_model_summary")["active_group"] == "g:0002"
    assert "references" in C.fails("activate_group", group="g:0001")
    C.call("delete_group", group=hg)
    assert hg not in [x["handle"] for x in C.call("get_model_summary")["groups"]]
    C.call("undo")
    assert hg in [x["handle"] for x in C.call("get_model_summary")["groups"]]


def test_delete():
    fresh()
    C.call("create_workplane_sketch")
    r = rect([0, 0], [10, 10])
    n_constraints = len(C.call("list_constraints")["constraints"])
    assert n_constraints == 8
    C.call("delete", handles=[r["requests"][0]])
    g = C.call("get_group")
    assert g["requests"] == 3, g
    # The constraints on the deleted line went too.
    assert len(C.call("list_constraints")["constraints"]) < n_constraints
    assert "Reference" in C.fails("delete", handles=["e:00010000"])


# ---------------------------------------------------------------------------
# Milestone 3: sketch entities
# ---------------------------------------------------------------------------
def test_sketch_entities():
    fresh()
    C.call("create_workplane_sketch")
    tri = C.call("sketch_line", points=[[0, 0], [30, 0], [0, 20]], closed=True)
    assert len(tri["requests"]) == 3
    assert C.call("get_solve_status")["status"] == "ok"
    coinc = [c for c in C.call("list_constraints")["constraints"] if c["type"] == "coincident"]
    assert len(coinc) == 3, coinc

    circ = C.call("sketch_circle", center=[50, 50], radius=7)
    e = circ["requests"][0]["entities"]
    c = next(x for x in e if x["type"] == "circle")
    assert close(c["radius"], 7) and close(c["center"]["uv"], [50, 50])

    arc = C.call("sketch_arc", center=[0, 0], start=[10, 0], end=[0, 25])
    a = next(x for x in arc["requests"][0]["entities"] if x["type"] == "arc")
    assert close(a["radius"], 10) and close(a["end"]["uv"], [0, 10]), a
    assert close(a["sweep_deg"], 90), a
    arc2 = C.call("sketch_arc", center=[0, 0], start=[10, 0], end=[0, 10], clockwise=True)
    a2 = next(x for x in arc2["requests"][0]["entities"] if x["type"] == "arc")
    assert close(a2["sweep_deg"], 270), a2

    cub = C.call("sketch_cubic", points=[[0, -10], [10, -5], [20, -10], [30, -5]])
    cp = next(x for x in cub["requests"][0]["entities"] if x["type"] == "cubic")
    uvs = [p["uv"] for p in cp["control_points"]]
    assert close(uvs[0], [0, -10]) and close(uvs[-1], [30, -5]), uvs
    assert close(uvs[2], [10, -5]) and close(uvs[3], [20, -10]), uvs
    per = C.call("sketch_cubic", points=[[0, 60], [10, 70], [0, 80]], periodic=True)
    pp = next(x for x in per["requests"][0]["entities"] if x["type"] == "cubic_periodic")
    assert len(pp["control_points"]) == 3

    pt = C.call("sketch_point", at=[5, 5])
    assert close(pt["requests"][0]["entities"][0]["uv"], [5, 5])

    txt = C.call("sketch_text", text="Hi", origin=[0, 100], height=10)
    t = next(x for x in txt["requests"][0]["entities"] if x["type"] == "text")
    assert close(t["corners"]["bottom_left"]["uv"], [0, 100])
    assert close(t["corners"]["top_left"]["uv"], [0, 110])
    assert t["corners"]["bottom_right"]["uv"][0] > 5

    C.call("set_construction", handles=[circ["requests"][0]["request"]])
    assert C.call("list_entities", kind="curves")
    status = C.call("get_solve_status")
    assert status["status"] == "ok", status
    assert "2D" in C.fails("sketch_point", at=[1, 2, 3])


def test_sketch_3d():
    fresh()
    C.call("create_3d_sketch", name="wire")
    ln = C.call("sketch_line", points=[[0, 0, 0], [10, 10, 10]])
    p = ln["requests"][0]["entities"]
    line = next(x for x in p if x["type"] == "line_segment")
    assert close(line["length"], math.sqrt(300))
    circ = C.call("sketch_circle", center=[0, 0, 5], radius=3, normal=[1, 0, 0])
    c = next(x for x in circ["requests"][0]["entities"] if x["type"] == "circle")
    assert close(c["normal"], [1, 0, 0]), c
    wp = C.call("sketch_workplane", origin=[0, 0, 20], normal=[0, 1, 0])
    w = next(x for x in wp["requests"][0]["entities"] if x["type"] == "workplane")
    assert close(w["normal"], [0, 1, 0]) and close(w["origin"], [0, 0, 20]), w
    assert "workplane" in C.fails("sketch_arc", center=[0, 0], start=[1, 0], end=[0, 1])
    assert "workplane" in C.fails("sketch_rectangle", corner1=[0, 0], corner2=[1, 1])
    # Sketch in the datum workplane.
    g = C.call("create_workplane_sketch", workplane=w["handle"])
    assert close(g["workplane"]["origin"], [0, 0, 20])


def test_fillet_and_move():
    fresh()
    C.call("create_workplane_sketch")
    r = rect([0, 0], [40, 20])
    corner = r["corners"][2]["handle"]  # (40, 20)
    f = C.call("fillet", point=corner, radius=3)
    arcs = [e for q in f["requests"] for e in q["entities"] if e["type"] == "arc"]
    # SolveSpace's tangent arc tool is approximate, not constrained.
    assert arcs and close(arcs[0]["radius"], 3, 0.01), f
    fresh()
    C.call("create_workplane_sketch")
    r = rect([0, 0], [40, 20])
    m = C.call("move_point", point=r["corners"][2]["handle"], to=[50, 30])
    assert m["reached"], m
    assert close(m["point"]["uv"], [50, 30])
    # The rectangle stays a rectangle.
    lines = [e for e in entities(kind="curves")["entities"]]
    lengths = sorted(round(x["length"], 6) for x in lines)
    assert lengths == [30, 30, 50, 50], lengths


# ---------------------------------------------------------------------------
# Milestone 4: constraints
# ---------------------------------------------------------------------------
def test_constraints():
    fresh()
    C.call("create_workplane_sketch")
    r = rect([3, 4], [43, 24])
    horiz = line_along(r, 0)["handle"]
    vert = line_along(r, 1)["handle"]
    c = C.call("add_constraint", type="distance", entities=[horiz], value=50)
    assert close(c["constraints"][0]["value"], 50)
    assert c["solve"]["dof"] == 3, c
    C.call("add_constraint", type="distance", entities=[vert], value=25)
    C.call("add_constraint", type="coincident",
           points=[r["corners"][0]["handle"], origin_point()])
    st = C.call("get_solve_status")
    assert st["dof"] == 0 and st["free_points"] == [], st
    pts = sorted(tuple(round(v, 6) for v in p["uv"]) for p in points_of(None) if "uv" in p)
    assert (50, 25) in pts and (0, 0) in pts, pts

    # Conflicting constraint is rejected and rolled back.
    before = len(C.call("list_constraints")["constraints"])
    err = C.fails("add_constraint", type="horizontal", entities=[vert])
    assert "no longer solves" in err or "redundant" in err.lower(), err
    assert len(C.call("list_constraints")["constraints"]) == before
    # Duplicate.
    assert "already exists" in C.fails("add_constraint", type="vertical", entities=[vert])
    # A redundant dimension (on the opposite side) becomes a reference.
    other = next(ln for ln in r["lines"] if ln["handle"] != horiz and
                 close(ln["points"][0]["uv"][1], ln["points"][1]["uv"][1]))
    d = C.call("add_constraint", type="distance", entities=[other["handle"]])
    assert d["constraints"][0]["reference"], d
    # Bad selection gives SolveSpace's own explanation, without a dialog.
    err = C.fails("add_constraint", type="diameter", entities=[horiz, vert])
    assert "Bad selection" in err, err


def test_dimension_values_and_params():
    fresh()
    C.call("create_workplane_sketch")
    circ = C.call("sketch_circle", center=[0, 0], radius=5)
    ch = next(e for e in circ["requests"][0]["entities"] if e["type"] == "circle")["handle"]
    d = C.call("add_constraint", type="diameter", entities=[ch], value=30)
    hc = d["constraints"][0]["handle"]
    assert close(next(e for e in entities(kind="curves")["entities"]
                      if e["type"] == "circle")["radius"], 15)
    # Expression with a new named parameter.
    C.call("set_parameter", name="dia", value=40)
    C.call("set_constraint_value", constraint=hc, value="dia")
    assert close(next(e for e in entities(kind="curves")["entities"]
                      if e["type"] == "circle")["radius"], 20)
    C.call("set_parameter", name="dia", value=12)
    assert close(next(e for e in entities(kind="curves")["entities"]
                      if e["type"] == "circle")["radius"], 6)
    ps = C.call("list_parameters")["parameters"]
    assert any(p["name"] == "dia" and close(p["value"], 12) for p in ps), ps
    C.call("set_constraint_reference", constraint=hc, reference=True)
    assert C.call("list_constraints")["constraints"][0]["reference"]

    # Angle between two lines.
    l1 = C.call("sketch_line", points=[[50, 0], [80, 0]])
    l2 = C.call("sketch_line", points=[[50, 0], [70, 20]])
    h1 = next(e for e in l1["requests"][0]["entities"] if e["type"] == "line_segment")
    h2 = next(e for e in l2["requests"][0]["entities"] if e["type"] == "line_segment")
    C.call("add_constraint", type="coincident",
           points=[h1["points"][0]["handle"], h2["points"][0]["handle"]])
    a = C.call("add_constraint", type="angle", entities=[h1["handle"], h2["handle"]], value=30)
    assert close(a["constraints"][0]["value"], 30), a
    C.call("add_comment", text="hello", at=[0, 30])


# ---------------------------------------------------------------------------
# Milestone 5: 3D operations
# ---------------------------------------------------------------------------
def test_extrude_and_cut():
    fresh()
    C.call("create_workplane_sketch")
    rect([0, 0], [40, 20])
    ex = C.call("extrude", depth=10)
    assert close(ex["depth"], 10), ex
    assert ex["solve"]["dof"] == 0, ex
    assert "warning" not in ex, ex
    faces = entities(ex["handle"], "faces")["entities"]
    zs = sorted(round(f["point"][2], 6) for f in faces if close(f["normal"], [0, 0, 1])
                or close(f["normal"], [0, 0, -1]))
    assert zs and zs[-1] == 10, faces
    # Sketch on the top face, through a top vertex, and cut a hole.
    top = next(p for p in points_of(ex["handle"]) if close(p["xyz"][2], 10))
    g = C.call("create_workplane_sketch", point=top["handle"], plane="XY")
    assert close(g["workplane"]["origin"][2], 10), g
    C.call("sketch_circle", center=[20 - g["workplane"]["origin"][0],
                                    10 - g["workplane"]["origin"][1]], radius=4)
    cut = C.call("extrude", depth=-5, combine="difference")
    assert cut["combine"] == "difference" and close(cut["depth"], 5), cut
    shot = C.call_raw("screenshot", width=640, height=480, zoom_to_fit=True)
    save_png("extrude_cut.png", shot)
    # Change the depth through its constraint.
    dim = C.call("list_constraints", group=ex["handle"])["constraints"][0]
    C.call("set_constraint_value", constraint=dim["handle"], value=15)
    assert close(C.call("get_group", group=ex["handle"])["depth"], 15)


def axis_line(a, b):
    ln = C.call("sketch_line", points=[a, b], construction=True)
    return next(e for e in ln["requests"][0]["entities"] if e["type"] == "line_segment")


def angles_at_radius(group, radius, axis=1):
    """Angles (deg) of points of a group lying at `radius` from the axis."""
    out = set()
    for p in points_of(group):
        x, y, z = p["xyz"]
        if axis == 1:
            r, ang = math.hypot(x, z), math.degrees(math.atan2(z, x))
        else:
            r, ang = math.hypot(x, y), math.degrees(math.atan2(y, x))
        if abs(r - radius) < 1e-6:
            out.add(round(ang, 4) % 360)
    return out


def test_revolve_lathe_helix():
    fresh()
    C.call("create_workplane_sketch")
    rect([10, 0], [20, 5])
    ax = axis_line([0, 0], [0, 10])
    rv = C.call("revolve", axis=ax["handle"], angle_deg=90)
    assert close(rv["angle_deg"], 90), rv
    angs = angles_at_radius(rv["handle"], 10)
    assert 0 in angs and (90 in angs or 270 in angs), angs
    save_png("revolve.png", C.call_raw("screenshot", zoom_to_fit=True))

    fresh()
    C.call("create_workplane_sketch")
    rect([10, 0], [20, 5])
    ax = axis_line([0, 0], [0, 10])
    la = C.call("lathe", axis=ax["handle"])
    assert la["type"] == "lathe" and "warning" not in la, la

    fresh()
    C.call("create_workplane_sketch")
    C.call("sketch_circle", center=[15, 0], radius=2)
    ax = axis_line([0, 0], [0, 10])
    hx = C.call("helix", axis=ax["handle"], angle_deg=720, pitch=10)
    assert close(hx["angle_deg"], 720) and close(hx["axial_travel"], 20), hx
    save_png("helix.png", C.call_raw("screenshot", zoom_to_fit=True))


def test_step_and_repeat():
    fresh()
    C.call("create_workplane_sketch")
    C.call("sketch_circle", center=[20, 0], radius=3)
    o = origin_point()
    rot = C.call("rotate_copy", center=o, count=4)
    assert rot["count"] == 4 and close(rot["angle_step_deg"], 90), rot
    angs = angles_at_radius(rot["handle"], 20, axis=2)
    assert angs == {0, 90, 180, 270}, angs

    fresh()
    C.call("create_workplane_sketch")
    rect([0, 0], [10, 10])
    ex = C.call("extrude", depth=5)
    tr = C.call("translate_copy", sketch=ex["handle"], step=[0, 0, 8], count=3)
    assert close(tr["step"], [0, 0, 8]) and tr["count"] == 3, tr
    zs = sorted({round(p["xyz"][2], 6) for p in points_of(tr["handle"])})
    assert 16 in zs or 21 in zs, zs
    save_png("translate.png", C.call_raw("screenshot", zoom_to_fit=True))
    assert "workplane" in C.fails("translate_copy", sketch="g:0002", step=[0, 0, 5])


def test_link():
    fresh()
    C.call("create_workplane_sketch")
    rect([0, 0], [10, 10])
    C.call("extrude", depth=5)
    part = os.path.join(OUT, "part.slvs")
    C.call("save_file", path=part)
    fresh()
    lk = C.call("link_file", path=part)
    assert lk["type"] == "linked" and lk["file"] == part, lk


# ---------------------------------------------------------------------------
# Regressions from code review
# ---------------------------------------------------------------------------
def test_review_regressions():
    fresh()
    sk = C.call("create_workplane_sketch")
    r = rect([0, 0], [10, 10])
    # Free points are found even though the group was already solved.
    st = C.call("get_solve_status")
    assert st["dof"] == 4 and len(st["free_points"]) > 0, st
    ex = C.call("extrude", depth=5)
    later_pt = points_of(ex["handle"])[0]["handle"]
    C.call("activate_group", group=sk["group"])
    # Geometry of a later group is refused, rather than crashing SolveSpace.
    err = C.fails("add_constraint", type="coincident",
                  points=[r["corners"][0]["handle"], later_pt])
    assert "after the active group" in err, err
    err = C.fails("create_workplane_sketch", point=later_pt)
    assert "after the active group" in err, err
    # A failed call on a saved model doesn't mark it unsaved.
    path = os.path.join(OUT, "saved.slvs")
    C.call("save_file", path=path)
    C.fails("add_constraint", type="horizontal", entities=["e:00010000"])
    assert not C.call("get_model_summary")["unsaved"]
    # Tiny values are formatted so the expression parser accepts them.
    C.call("set_parameter", name="tiny", value=0.00005)
    ps = C.call("list_parameters")["parameters"]
    assert any(p["name"] == "tiny" and close(p["value"], 0.00005) for p in ps), ps
    # Editing a later group's dimension solves that group.
    dim = C.call("list_constraints", group=ex["handle"])["constraints"][0]
    C.call("set_constraint_value", constraint=dim["handle"], value=7)
    assert close(C.call("get_group", group=ex["handle"])["depth"], 7)
    # sketch_image refuses non-PNG files without opening a dialog.
    assert "PNG" in C.fails("sketch_image", path=path)


TESTS = [
    test_document_and_undo, test_groups, test_delete,
    test_sketch_entities, test_sketch_3d, test_fillet_and_move,
    test_constraints, test_dimension_values_and_params,
    test_extrude_and_cut, test_revolve_lathe_helix, test_step_and_repeat, test_link,
    test_review_regressions,
]


def main():
    global C, OUT
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:47474/mcp")
    ap.add_argument("--out", required=True, help="directory for files and screenshots")
    ap.add_argument("-k", default="", help="only run tests whose name contains this")
    a = ap.parse_args()
    OUT = os.path.abspath(a.out)
    os.makedirs(OUT, exist_ok=True)
    C = Client(a.url)
    C.initialize()
    failed = 0
    for t in TESTS:
        if a.k not in t.__name__:
            continue
        try:
            t()
            print(f"PASS {t.__name__}")
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
            traceback.print_exc()
    print("OK" if not failed else f"{failed} FAILED")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
