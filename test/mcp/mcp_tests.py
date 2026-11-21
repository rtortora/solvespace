#!/usr/bin/env python3
"""End-to-end tests for the SolveSpace MCP tools (milestones 0-6).

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
    """The handle of a rectangle edge along u (0) or v (1)."""
    return r["edges"]["bottom" if axis == 0 else "left"]


def req(result, i=0):
    """The i-th (compact) request of a sketch tool's result."""
    return result["requests"][i]


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
    c = req(circ)
    assert c["type"] == "circle", c
    assert close(c["radius"], 7) and close(c["center"]["uv"], [50, 50])
    # Verbose lists every generated entity, with [x, y, z] too.
    v = C.call("sketch_circle", center=[60, 50], radius=1, verbose=True)
    assert any(x["type"] == "point" and "xyz" in x for x in req(v)["entities"]), v

    arc = C.call("sketch_arc", center=[0, 0], start=[10, 0], end=[0, 25])
    a = req(arc)
    assert close(a["radius"], 10) and close(a["end"]["uv"], [0, 10]), a
    assert close(a["sweep_deg"], 90), a
    arc2 = C.call("sketch_arc", center=[0, 0], start=[10, 0], end=[0, 10], clockwise=True)
    assert close(req(arc2)["sweep_deg"], 270), arc2

    cub = C.call("sketch_cubic", points=[[0, -10], [10, -5], [20, -10], [30, -5]])
    cp = req(cub)
    uvs = [p["uv"] for p in cp["control_points"]]
    assert close(uvs[0], [0, -10]) and close(uvs[-1], [30, -5]), uvs
    assert close(uvs[2], [10, -5]) and close(uvs[3], [20, -10]), uvs
    per = C.call("sketch_cubic", points=[[0, 60], [10, 70], [0, 80]], periodic=True)
    assert len(req(per)["control_points"]) == 3

    pt = C.call("sketch_point", at=[5, 5])
    assert close(req(pt)["uv"], [5, 5])

    txt = C.call("sketch_text", text="Hi", origin=[0, 100], height=10)
    t = req(txt)
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
    assert close(req(ln)["length"], math.sqrt(300))
    assert close(req(ln)["points"][1]["xyz"], [10, 10, 10])
    circ = C.call("sketch_circle", center=[0, 0, 5], radius=3, normal=[1, 0, 0])
    c = req(circ)
    assert close(c["normal"], [1, 0, 0]), c
    wp = C.call("sketch_workplane", origin=[0, 0, 20], normal=[0, 1, 0])
    w = req(wp)
    w["handle"] = w["entity"]
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
    arcs = [q for q in f["requests"] if q["type"] == "arc"]
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
    horiz = line_along(r, 0)
    vert = line_along(r, 1)
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
    d = C.call("add_constraint", type="distance", entities=[r["edges"]["top"]])
    assert d["constraints"][0]["reference"], d
    # Bad selection gives SolveSpace's own explanation, without a dialog.
    err = C.fails("add_constraint", type="diameter", entities=[horiz, vert])
    assert "Bad selection" in err, err


def test_dimension_values_and_params():
    fresh()
    C.call("create_workplane_sketch")
    circ = C.call("sketch_circle", center=[0, 0], radius=5)
    ch = req(circ)["entity"]
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
    h1, h2 = req(l1), req(l2)
    C.call("add_constraint", type="coincident",
           points=[h1["points"][0]["handle"], h2["points"][0]["handle"]])
    a = C.call("add_constraint", type="angle", entities=[h1["entity"], h2["entity"]], value=30)
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
    return {"handle": req(ln)["entity"]}


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


# ---------------------------------------------------------------------------
# Milestone 5b: modelling ergonomics
# ---------------------------------------------------------------------------
def test_offset_sketch():
    fresh()
    g = C.call("create_workplane_sketch", plane="XY", offset=420, name="seat")
    wp = g["workplane"]
    assert close(wp["origin"], [0, 0, 420]) and close(wp["u_axis"], [1, 0, 0]), g
    assert g["solve"]["dof"] == 0 and "datum_group" in g, g
    datum = C.call("get_group", group=g["datum_group"])
    assert not datum["visible"] and datum["solve"]["dof"] == 0, datum
    r = C.call("sketch_rectangle", corner1=[0, 0], corner2=[10, 10])
    assert close(r["corners"][2]["uv"], [10, 10]), r
    # The offset is a dimension: changing it moves the sketch.
    C.call("set_constraint_value", constraint=g["offset_constraint"], value=500)
    pts = [p["xyz"] for p in points_of(g["group"])
           if "xyz" in p] or [C.call("list_entities", group=g["group"], kind="points",
                                     verbose=True)["entities"][0]["xyz"]]
    assert all(close(p[2], 500) for p in pts), pts
    # origin + plane: YZ through (10, 20, 30); offset moves along +X.
    g2 = C.call("create_workplane_sketch", plane="YZ", origin=[10, 20, 30], offset=5)
    assert close(g2["workplane"]["origin"], [15, 20, 30]), g2
    assert close(g2["workplane"]["normal"], [1, 0, 0]), g2
    # No offset and no origin: just the reference plane, no datum group.
    g3 = C.call("create_workplane_sketch", plane="ZX", offset=0)
    assert "datum_group" not in g3 and close(g3["workplane"]["origin"], [0, 0, 0]), g3
    assert "combined" in C.fails("create_workplane_sketch", offset=3, point="e:00010001")


def test_face_sketch_and_find_faces():
    fresh()
    C.call("create_workplane_sketch")
    C.call("sketch_rectangle", corner1=[0, 0], corner2=[40, 20], constrain=True)
    ex = C.call("extrude", depth=10)
    top, bottom = ex["faces"]["top"], ex["faces"]["bottom"]
    assert close(top["normal"], [0, 0, 1]) and close(bottom["normal"], [0, 0, -1]), ex
    assert close(top["center"], [20, 10, 10]) and close(top["area"], 800), top
    assert ex["depth_constraint"].startswith("c:"), ex

    tops = C.call("find_faces", normal="+z")["faces"]
    assert [f["handle"] for f in tops] == [top["handle"]], tops
    front = C.call("find_faces", near=[20, -50, 5], limit=1)["faces"][0]
    assert close(front["normal"], [0, -1, 0]), front
    assert len(C.call("find_faces", limit=10)["faces"]) == 6

    # On the top face, [u, v] are model X, Y.
    g = C.call("create_workplane_sketch", face=top["handle"])
    assert close(g["workplane"]["origin"], [0, 0, 10]), g
    assert close(g["workplane"]["normal"], [0, 0, 1]) and g["solve"]["dof"] == 0, g
    C.call("sketch_circle", center=[20, 10], radius=4)
    cut = C.call("extrude", depth=-5, combine="difference")
    assert "warning" not in cut, cut
    assert close(cut["faces"]["top"]["center"], [20, 10, 5]), cut

    # On the front face (normal -Y): u = X, v = Z, and a positive depth
    # builds outward (towards -Y).
    g = C.call("create_workplane_sketch", face=front["handle"])
    assert close(g["workplane"]["u_axis"], [1, 0, 0]), g
    assert close(g["workplane"]["v_axis"], [0, 0, 1]), g
    C.call("sketch_rectangle", corner1=[5, 2], corner2=[15, 8])
    C.call("extrude", depth=3)
    b = C.call("get_model_summary")["solid_bounds"]
    assert close(b["min"], [0, -3, 0]) and close(b["max"], [40, 20, 10]), b

    # The face sketch follows the solid: make the block taller.
    C.call("set_constraint_value", constraint=ex["depth_constraint"], value=30)
    hole = C.call("find_faces", group=cut["handle"], normal="+z")["faces"][0]
    assert close(hole["center"][2], 25), hole
    assert "isn't a face" in C.fails("create_workplane_sketch", face=top["vertex"])


def test_compact_results():
    fresh()
    C.call("create_workplane_sketch")
    raw = C.call_raw("sketch_rectangle", corner1=[0, 0], corner2=[40, 20])
    text = raw["content"][0]["text"]
    assert len(text) < 700, len(text)
    r = raw["structuredContent"]
    assert set(r["edges"]) == {"bottom", "right", "top", "left"}, r
    assert "lines" not in r and "xyz" not in r["corners"][0], r
    v = C.call("sketch_rectangle", corner1=[50, 0], corner2=[60, 10], verbose=True)
    assert len(v["lines"]) == 4 and "xyz" in v["corners"][0] and v["constraints"], v
    # Coincident corners are listed once (the workplane's origin point is
    # at a corner too).
    pts = entities(kind="points")
    assert len(pts["entities"]) == 8 and pts["coincident_points_omitted"] == 9, pts
    assert len(C.call("list_entities", kind="points", unique_points=False)["entities"]) == 17
    ex = C.call("extrude", depth=5)
    assert len(points_of(ex["handle"])) == 16, points_of(ex["handle"])
    assert C.call("get_group")["color"].startswith("#")
    # Numbers are rounded.
    assert all(len(repr(x)) < 12 for p in points_of(ex["handle"]) for x in p["xyz"])


def test_constrained_primitives():
    fresh()
    C.call("create_workplane_sketch")
    # A corner at the origin: anchored with a coincident constraint.
    r = C.call("sketch_rectangle", corner1=[0, 0], corner2=[40, 20], constrain=True)
    assert r["solve"]["dof"] == 0 and "at_origin" in r["dimensions"], r
    C.call("set_constraint_value", constraint=r["dimensions"]["width"], value=50)
    lines = {e["handle"]: e for e in entities(kind="curves")["entities"]}
    assert close(lines[r["edges"]["bottom"]]["length"], 50)
    # Elsewhere: distances from the origin.
    r = C.call("sketch_rectangle", corner1=[70, 40], corner2=[60, 30], constrain=True)
    assert r["solve"]["dof"] == 0 and {"u", "v"} <= set(r["dimensions"]), r
    # Center form, centered on the origin and off it.
    r = C.call("sketch_rectangle", center=[0, 0], width=10, height=6, constrain=True)
    assert r["solve"]["dof"] == 0 and "at_origin" in r["dimensions"], r
    assert close(r["corners"][0]["uv"], [-5, -3]), r
    r = C.call("sketch_rectangle", center=[100, 50], width=10, height=6, constrain=True)
    assert r["solve"]["dof"] == 0 and r["center"], r
    C.call("set_constraint_value", constraint=r["dimensions"]["width"], value=20)
    st = C.call("get_solve_status")
    c = next(p for p in points_of(None) if p["handle"] == r["corners"][0]["handle"])
    assert close(c["uv"], [90, 47]), c          # grew about the centre
    # Circles.
    c = C.call("sketch_circle", center=[-30, 40], diameter=12, constrain=True)
    assert c["solve"]["dof"] == 0 and {"diameter", "u", "v"} <= set(c["dimensions"]), c
    assert close(c["requests"][0]["radius"], 6)
    # A closed polyline with an angled first segment, and an open one.
    t = C.call("sketch_line", points=[[200, 0], [230, 10], [230, 40], [200, 40]],
               closed=True, constrain=True)
    assert t["solve"]["dof"] == 0, t
    o = C.call("sketch_line", points=[[300, 0], [320, 0], [330, 20]], constrain=True)
    assert o["solve"]["dof"] == 0, o
    # Segments in line are made parallel rather than given a 0 degree angle.
    o = C.call("sketch_line", points=[[400, 0], [410, 5], [420, 10]], constrain=True)
    assert o["solve"]["dof"] == 0, o
    st = C.call("get_solve_status")
    assert st["dof"] == 0 and st["status"] == "ok", st
    # In a workplane at an angle to the reference axes, a circle's size is
    # still constrained, but not its position.
    C.call("create_3d_sketch")
    wp = C.call("sketch_workplane", origin=[0, 0, 0], normal=[1, 1, 0])["requests"][0]
    C.call("create_workplane_sketch", workplane=wp["entity"])
    c = C.call("sketch_circle", center=[5, 5], radius=2, constrain=True)
    assert c["solve"]["dof"] == 2 and "note" in c, c
    assert "height" in C.fails("sketch_rectangle", center=[0, 0], width=1)
    C.call("create_3d_sketch")
    assert "workplane" in C.fails("sketch_line", points=[[0, 0, 0], [1, 1, 1]], constrain=True)


def test_batch():
    fresh()
    r = C.call("batch", calls=[
        {"tool": "create_workplane_sketch", "args": {"plane": "XY"}},
        {"tool": "sketch_rectangle", "args": {"corner1": [0, 0], "corner2": [30, 30],
                                              "constrain": True}},
        {"tool": "extrude", "args": {"depth": 10}},
        {"tool": "create_workplane_sketch", "args": {"face": "$2.faces.top.handle"}},
        {"tool": "sketch_circle", "args": {"center": [15, 15], "diameter": 8,
                                           "constrain": True}},
        {"tool": "extrude", "args": {"depth": 20}},
    ])
    assert len(r["results"]) == 6 and r["solve"]["dof"] == 0, r
    b = C.call("get_model_summary")["solid_bounds"]
    assert close(b["max"], [30, 30, 30]), b
    n = len(C.call("get_model_summary")["groups"])
    # All or nothing.
    err = C.fails("batch", calls=[
        {"tool": "create_workplane_sketch", "args": {"plane": "XY", "offset": 5}},
        {"tool": "sketch_circle", "args": {"center": [0, 0], "radius": -1}},
    ])
    assert "calls[1]" in err and "undone" in err, err
    assert len(C.call("get_model_summary")["groups"]) == n
    assert "hasn't run" in C.fails("batch", calls=[
        {"tool": "sketch_point", "args": {"at": "$0.x"}}])
    assert "batch" in C.fails("batch", calls=[{"tool": "undo"}])
    # One undo step.
    C.call("undo")
    assert len(C.call("get_model_summary")["groups"]) == 2


def images(raw):
    return [c for c in raw["content"] if c["type"] == "image"]


def test_screenshot_views():
    fresh()
    C.call("create_workplane_sketch")
    r = C.call("sketch_rectangle", corner1=[0, 0], corner2=[40, 20], constrain=True)
    C.call("extrude", depth=30)
    before = C.call("get_view")
    raw = C.call_raw("screenshot", views=["iso", "front", "right"], width=320, height=240)
    assert len(images(raw)) == 3, raw["structuredContent"]
    vs = raw["structuredContent"]["views"]
    assert close(vs[1]["proj_up"], [0, 0, 1]) and close(vs[1]["proj_right"], [1, 0, 0]), vs
    for i, name in enumerate(["iso", "front", "right"]):
        save_png(f"view_{name}.png", {"content": [images(raw)[i]]})
    # The GUI's view didn't move.
    after = C.call("get_view")
    assert close(before["offset"], after["offset"]) and close(before["scale"], after["scale"])
    assert close(before["proj_right"], after["proj_right"])
    # Options.
    C.call("activate_group", group="g:0003")
    raw = C.call_raw("screenshot", view="top", units="display", shaded=False,
                     hidden_lines="visible", highlight=[r["edges"]["top"]])
    assert len(images(raw)) == 1
    save_png("options.png", raw)
    assert "Unknown view" in C.fails("screenshot", view="sideways")
    # The real window, as drawn (needs the window on screen).
    raw = C.call_raw("screenshot", source="window")
    if raw.get("isError"):
        assert "redrawn" in raw["content"][0]["text"], raw
        print("  (window capture skipped: window not drawn)")
    else:
        assert raw["structuredContent"]["source"] == "window" and images(raw)
        save_png("window.png", raw)
    assert "window" in C.fails("screenshot", source="window", view="iso")


# ---------------------------------------------------------------------------
# Milestone 6: view, navigation and display
# ---------------------------------------------------------------------------
def test_view_tools():
    fresh()
    C.call("create_workplane_sketch")
    C.call("sketch_rectangle", corner1=[0, 0], corner2=[40, 20])
    C.call("extrude", depth=30)
    saved = C.call("get_view")
    v = C.call("orient_view", preset="front")
    assert close(v["proj_up"], [0, 0, 1]) and close(v["toward_viewer"], [0, -1, 0]), v
    # The model turns counter-clockwise seen from above, so its left side
    # comes to face the viewer.
    v = C.call("rotate_view", axis="vertical", angle_deg=90)
    assert close(v["toward_viewer"], [-1, 0, 0]), v
    s0 = v["scale"]
    assert close(C.call("zoom", factor=2)["scale"], 2 * s0)
    v = C.call("center_on", point=[1, 2, 3])
    assert close(v["center"], [1, 2, 3]), v
    v = C.call("pan", by=[0, 10, 0])
    assert close(v["center"], [1, 12, 3]), v
    # The model moves 2 mm to the right on screen (proj_right is -Y here),
    # so the view's centre moves 2 mm the other way.
    v = C.call("pan", dx=2 * v["scale"])
    assert close(v["center"], [1, 14, 3]), v
    v = C.call("set_view", center=[5, 5, 5], proj_right=[0, 1, 0], proj_up=[0, 0, 1],
               scale=3)
    assert close(v["offset"], [-5, -5, -5]) and close(v["scale"], 3), v
    assert "parallel" in C.fails("set_view", proj_right=[1, 0, 0], proj_up=[2, 0, 0])
    C.call("zoom_to_fit")
    C.call("activate_group", group="g:0003")
    v = C.call("orient_view", preset="workplane")
    assert close(v["toward_viewer"], [0, 0, 1]), v
    C.call("set_view", offset=saved["offset"], proj_right=saved["proj_right"],
           proj_up=saved["proj_up"], scale=saved["scale"])


def test_display_and_units():
    fresh()
    saved = C.call("set_display_options")
    d = C.call("set_display_options", workplanes=False, constraints="dimensions",
               hidden_lines="visible", shaded=False)
    assert not d["workplanes"] and d["constraints"] == "dimensions", d
    assert d["hidden_lines"] == "visible" and not d["shaded"], d
    assert "Unknown" in C.fails("set_display_options", bogus=True)
    C.call("set_display_options", **saved)
    assert C.call("set_display_options") == saved

    view = C.call("get_view")
    try:
        assert C.call("set_units", units="mm")["units"] == "mm"
        assert C.call("get_model_summary")["display_units"] == "mm"
        p = C.call("set_perspective", enabled=True, factor=0.5)
        assert p["perspective"] and close(p["perspective_factor"], 0.5), p
        e = C.call("set_explode_sketch", enabled=True, distance=7)
        assert e["explode"] and close(e["distance"], 7), e
    finally:
        # These are user settings that SolveSpace saves; put them back.
        C.call("set_units", units=view["units"])
        C.call("set_perspective", enabled=view["perspective"],
               factor=view["perspective_factor"])
        C.call("set_explode_sketch", enabled=False)


def test_styles():
    fresh()
    C.call("create_workplane_sketch")
    r = C.call("sketch_rectangle", corner1=[0, 0], corner2=[10, 10])
    st = C.call("create_style", name="hidden-ref", color="#ff0000", width=2)["style"]
    assert st["custom"] and st["color"] == "#ff0000" and st["name"] == "hidden-ref", st
    names = [s["name"] for s in C.call("list_styles")["styles"]]
    assert "hidden-ref" in names and len(names) > 10, names
    ch = C.call("set_style", handles=r["requests"][:2], style="hidden-ref")
    assert ch["changed"] == r["requests"][:2], ch
    s2 = C.call("set_style_properties", style=st["handle"], visible=False, stipple="Dash")
    assert not s2["style"]["visible"] and s2["style"]["stipple"] == "Dash", s2
    save_png("styles_hidden.png", C.call_raw("screenshot", view="top"))
    assert "derived" in C.fails("set_style", handles=["e:80020000"], style="hidden-ref") \
        or "generated" in C.fails("set_style", handles=["e:80020000"], style="hidden-ref")
    C.call("set_style", handles=r["requests"][:1], style="default")
    C.call("undo")   # style changes are undoable
    assert "No style" in C.fails("set_style", handles=r["requests"], style="nope")


def test_summary_bounds():
    fresh()
    assert "solid_bounds" not in C.call("get_model_summary")
    C.call("create_workplane_sketch")
    C.call("sketch_circle", center=[0, 0], radius=10)
    C.call("extrude", depth=-4)
    b = C.call("get_model_summary")["solid_bounds"]
    assert close(b["size"], [20, 20, 4], 1e-3) and close(b["min"][2], -4), b


# ---------------------------------------------------------------------------
# Acceptance: the kitchen chair from the first field test, which took 16
# calls and three long point listings.
# ---------------------------------------------------------------------------
def test_chair_acceptance():
    fresh()
    sizes = []

    def call(tool, **args):
        raw = C.call_raw(tool, **args)
        assert not raw.get("isError"), raw["content"][0]["text"]
        sizes.append(len(raw["content"][0]["text"]))
        return raw

    W, D, H, LEG, SEAT_T, BACK_H = 450, 420, 420, 40, 30, 900
    legs = [{"tool": "sketch_rectangle",
             "args": {"corner1": [x, y], "corner2": [x + LEG, y + LEG], "constrain": True}}
            for x in (0, W - LEG) for y in (0, D - LEG)]
    call("batch", calls=[
        {"tool": "create_workplane_sketch", "args": {"plane": "XY", "name": "legs"}},
        *legs,
        {"tool": "extrude", "args": {"depth": H, "name": "legs"}},
        {"tool": "create_workplane_sketch", "args": {"plane": "XY", "offset": H,
                                                     "name": "seat"}},
        {"tool": "sketch_rectangle", "args": {"corner1": [0, 0], "corner2": [W, D],
                                              "constrain": True}},
        {"tool": "extrude", "args": {"depth": SEAT_T, "name": "seat"}},
        # Back posts on the seat's top face, over the back legs.
        {"tool": "create_workplane_sketch", "args": {"face": "$8.faces.top.handle",
                                                     "name": "back posts"}},
        {"tool": "sketch_rectangle", "args": {"corner1": [0, D - LEG],
                                              "corner2": [LEG, D], "constrain": True}},
        {"tool": "sketch_rectangle", "args": {"corner1": [W - LEG, D - LEG],
                                              "corner2": [W, D], "constrain": True}},
        {"tool": "extrude", "args": {"depth": BACK_H - H - SEAT_T, "name": "back posts"}},
        # The backrest between the posts.
        {"tool": "create_workplane_sketch", "args": {"plane": "XY", "offset": 700,
                                                     "name": "backrest"}},
        {"tool": "sketch_rectangle", "args": {"corner1": [LEG, D - 25],
                                              "corner2": [W - LEG, D - 5],
                                              "constrain": True}},
        {"tool": "extrude", "args": {"depth": 180, "name": "backrest"}},
    ])
    s = call("get_model_summary")["structuredContent"]
    assert close(s["solid_bounds"]["size"], [W, D, BACK_H]), s["solid_bounds"]
    for g in s["groups"][1:]:
        assert g["solve"]["dof"] == 0 and g["solve"]["status"] == "ok", g
    raw = call("screenshot", views=["iso", "front", "right"], width=480, height=480)
    for i, v in enumerate(["iso", "front", "right"]):
        save_png(f"chair_{v}.png", {"content": [images(raw)[i]]})
    C.call("save_file", path=os.path.join(OUT, "chair.slvs"))
    print(f"  chair: {len(sizes)} calls, {sum(sizes)} bytes of text results "
          f"({', '.join(map(str, sizes))})")
    assert len(sizes) <= 5


TESTS = [
    test_document_and_undo, test_groups, test_delete,
    test_sketch_entities, test_sketch_3d, test_fillet_and_move,
    test_constraints, test_dimension_values_and_params,
    test_extrude_and_cut, test_revolve_lathe_helix, test_step_and_repeat, test_link,
    test_review_regressions,
    test_offset_sketch, test_face_sketch_and_find_faces, test_compact_results,
    test_constrained_primitives, test_batch, test_screenshot_views,
    test_view_tools, test_display_and_units, test_styles, test_summary_bounds,
    test_chair_acceptance,
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
