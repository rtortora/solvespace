//-----------------------------------------------------------------------------
// MCP tools: sketch groups, and drawing sketch entities.
//-----------------------------------------------------------------------------
#include "mcptools.h"

namespace SolveSpace {
namespace Mcp {

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static std::set<uint32_t> RequestSet() {
    std::set<uint32_t> s;
    for(Request &r : SK.request) s.insert(r.h.v);
    return s;
}

static json NewRequestsSince(const std::set<uint32_t> &before) {
    json out = json::array();
    for(Request &r : SK.request) {
        if(before.count(r.h.v) == 0) out.push_back(RequestJson(r.h));
    }
    return out;
}

static hRequest NewRequest(Request::Type type, bool construction) {
    hRequest hr = SS.GW.AddRequest(type, /*rememberForUndo=*/false);
    // AddRequest makes geometry in non-sketch groups construction anyway.
    if(construction) SK.GetRequest(hr)->construction = true;
    return hr;
}

// A unit quaternion whose normal (N) is the given vector.
static Quaternion QuaternionForNormal(Vector n) {
    n = n.WithMagnitude(1);
    Vector u = n.Normal(0).WithMagnitude(1);
    Vector v = n.Cross(u).WithMagnitude(1);
    return Quaternion::From(u, v);
}

static Vector NormalArg(const json &args, const char *key) {
    if(!args.contains(key)) return Vector::From(0, 0, 1);
    Vector n = Vec3(args[key], key);
    if(n.Magnitude() < LENGTH_EPS) throw ToolError(std::string(key) + " can't be zero");
    return n;
}

static void FinishSketchResult(ToolResult *res, const json &requests) {
    res->structured["group"] = H(SS.GW.activeGroup);
    res->structured["requests"] = requests;
    res->structured["solve"] = SolveStatus(SS.GW.activeGroup);
}

//-----------------------------------------------------------------------------
// create_workplane_sketch / create_3d_sketch
//-----------------------------------------------------------------------------
static void NameNewGroup(const json &args) {
    if(args.contains("name") && args["name"].is_string() &&
       !args["name"].get<std::string>().empty()) {
        SK.GetGroup(SS.GW.activeGroup)->name = args["name"].get<std::string>();
    }
}

static hRequest ReferencePlane(const std::string &plane) {
    if(plane == "XY" || plane == "xy") return Request::HREQUEST_REFERENCE_XY;
    if(plane == "YZ" || plane == "yz") return Request::HREQUEST_REFERENCE_YZ;
    if(plane == "ZX" || plane == "zx") return Request::HREQUEST_REFERENCE_ZX;
    throw ToolError("plane must be one of XY, YZ, ZX");
}

static void CreateWorkplaneSketch(const json &args, ToolResult *res) {
    Handles sel;
    std::string plane = args.value("plane", "XY");
    // While creating a workplane through a point, Group::MenuGroup picks the
    // orientation from the view, so temporarily look straight at the plane.
    bool setView = false;
    if(args.contains("workplane")) {
        hEntity wp = ParseEntity(args["workplane"]);
        if(!SK.GetEntity(wp)->IsWorkplane()) throw ToolError(H(wp) + " isn't a workplane");
        sel.entities.push_back(wp);
    } else if(args.contains("point")) {
        hEntity pt = ParseEntity(args["point"]);
        if(!SK.GetEntity(pt)->IsPoint()) throw ToolError(H(pt) + " isn't a point");
        sel.entities.push_back(pt);
        if(args.contains("line_a") || args.contains("line_b")) {
            sel.entities.push_back(ParseEntity(args.value("line_a", json())));
            sel.entities.push_back(ParseEntity(args.value("line_b", json())));
        } else if(args.contains("normal")) {
            sel.entities.push_back(ParseEntity(args["normal"]));
        } else {
            setView = true;
        }
    } else {
        sel.entities.push_back(ReferencePlane(plane).entity(0));
    }

    Vector savedRight = SS.GW.projRight, savedUp = SS.GW.projUp;
    if(setView) {
        Frame f = FrameOf(ReferencePlane(plane).entity(0));
        SS.GW.projRight = f.u;
        SS.GW.projUp    = f.v;
    }
    hGroup before = SS.GW.activeGroup;
    Transaction t;
    SelectOnly(sel);
    // Exactly what Sketch > In Workplane does with this selection.
    Group::MenuGroup(Command::GROUP_WRKPL);
    SS.GW.ClearSelection();
    if(setView) {
        SS.GW.projRight = savedRight;
        SS.GW.projUp    = savedUp;
    }
    ThrowIfErrors();
    if(SS.GW.activeGroup == before) throw ToolError("The workplane sketch was not created.");
    NameNewGroup(args);
    Regenerate();
    t.Commit();

    Group *g = SK.GetGroup(SS.GW.activeGroup);
    res->structured = {
        {"group", H(g->h)}, {"name", g->name},
        {"workplane", ActiveWorkplaneJson()},
        {"solve", SolveStatus(g->h)},
    };
}

static void Create3dSketch(const json &args, ToolResult *res) {
    hGroup before = SS.GW.activeGroup;
    Transaction t;
    SS.GW.ClearSelection();
    Group::MenuGroup(Command::GROUP_3D);
    ThrowIfErrors();
    if(SS.GW.activeGroup == before) throw ToolError("The 3D sketch was not created.");
    NameNewGroup(args);
    Regenerate();
    t.Commit();
    Group *g = SK.GetGroup(SS.GW.activeGroup);
    res->structured = {{"group", H(g->h)}, {"name", g->name}, {"solve", SolveStatus(g->h)}};
}

//-----------------------------------------------------------------------------
// sketch_point
//-----------------------------------------------------------------------------
static void SketchPointTool(const json &args, ToolResult *res) {
    RequireSketchGroup();
    Vector p = SketchPoint(args.value("at", json()), "at");
    Transaction t;
    hRequest hr = NewRequest(Request::Type::DATUM_POINT, args.value("construction", false));
    SK.GetEntity(hr.entity(0))->PointForceTo(p);
    Regenerate();
    t.Commit();
    FinishSketchResult(res, json::array({RequestJson(hr)}));
}

//-----------------------------------------------------------------------------
// sketch_line (polyline)
//-----------------------------------------------------------------------------
static void SketchLine(const json &args, ToolResult *res) {
    RequireSketchGroup();
    const json &pts = args.value("points", json());
    if(!pts.is_array() || pts.size() < 2) throw ToolError("points needs at least 2 points");
    bool closed = args.value("closed", false);
    if(closed && pts.size() < 3) throw ToolError("A closed polyline needs at least 3 points");
    bool construction = args.value("construction", false);

    std::vector<Vector> p;
    for(size_t i = 0; i < pts.size(); i++) {
        p.push_back(SketchPoint(pts[i], ("points[" + std::to_string(i) + "]").c_str()));
    }
    size_t n = closed ? p.size() : p.size() - 1;
    for(size_t i = 0; i < n; i++) {
        if(p[i].Equals(p[(i + 1) % p.size()])) {
            throw ToolError("points " + std::to_string(i) + " and " +
                            std::to_string((i + 1) % p.size()) +
                            " are the same; that would make a zero-length line");
        }
    }

    Transaction t;
    std::vector<hRequest> lines;
    for(size_t i = 0; i < n; i++) {
        hRequest hr = NewRequest(Request::Type::LINE_SEGMENT, construction);
        SK.GetEntity(hr.entity(1))->PointForceTo(p[i]);
        SK.GetEntity(hr.entity(2))->PointForceTo(p[(i + 1) % p.size()]);
        lines.push_back(hr);
    }
    // Join consecutive segments, as drawing a polyline in the GUI does.
    for(size_t i = 0; i + 1 < lines.size(); i++) {
        Constraint::ConstrainCoincident(lines[i].entity(2), lines[i + 1].entity(1));
    }
    if(closed) {
        Constraint::ConstrainCoincident(lines.back().entity(2), lines.front().entity(1));
    }
    Regenerate();
    t.Commit();

    json reqs = json::array();
    for(hRequest hr : lines) reqs.push_back(RequestJson(hr));
    FinishSketchResult(res, reqs);
}

//-----------------------------------------------------------------------------
// sketch_rectangle
//-----------------------------------------------------------------------------
static void SketchRectangle(const json &args, ToolResult *res) {
    RequireSketchGroup();
    RequireWorkplane("Rectangles");
    if(!args.contains("corner1") || !args.contains("corner2")) {
        throw ToolError("corner1 and corner2 are required");
    }
    double u1, v1, u2, v2;
    Point2(args["corner1"], "corner1", &u1, &v1);
    Point2(args["corner2"], "corner2", &u2, &v2);
    if(fabs(u1 - u2) < LENGTH_EPS || fabs(v1 - v2) < LENGTH_EPS) {
        throw ToolError("The rectangle has zero width or height.");
    }
    bool construction = args.value("construction", false);

    Frame f = FrameOf(SS.GW.ActiveWorkplane());
    Vector c1  = f.To3d(u1, v1), c2  = f.To3d(u2, v2),
           c21 = f.To3d(u2, v1), c12 = f.To3d(u1, v2);

    // The same construction as the interactive rectangle tool in mouse.cpp:
    // line i's start is coincident with line i+1's end; lines 0 and 2 are
    // vertical, 1 and 3 horizontal. That puts the corners at:
    //   line 0: c2  -> c21     line 1: c12 -> c2
    //   line 2: c1  -> c12     line 3: c21 -> c1
    const Vector starts[4] = { c2,  c12, c1,  c21 };
    const Vector ends[4]   = { c21, c2,  c12, c1  };

    Transaction t;
    hRequest lns[4];
    for(int i = 0; i < 4; i++) {
        lns[i] = NewRequest(Request::Type::LINE_SEGMENT, construction);
    }
    for(int i = 0; i < 4; i++) {
        SK.GetEntity(lns[i].entity(1))->PointForceTo(starts[i]);
        SK.GetEntity(lns[i].entity(2))->PointForceTo(ends[i]);
    }
    for(int i = 0; i < 4; i++) {
        Constraint::ConstrainCoincident(lns[i].entity(1), lns[(i + 1) % 4].entity(2));
    }
    for(int i = 0; i < 4; i++) {
        Constraint::Constrain(
            (i % 2) ? Constraint::Type::HORIZONTAL : Constraint::Type::VERTICAL,
            Entity::NO_ENTITY, Entity::NO_ENTITY, lns[i].entity(0));
    }
    Regenerate();
    t.Commit();

    json lines = json::array();
    for(int i = 0; i < 4; i++) lines.push_back(EntityJson(SK.GetEntity(lns[i].entity(0))));
    // One point per corner, in the order c1, (u2,v1), c2, (u1,v2).
    json corners = json::array();
    for(hEntity hp : { lns[2].entity(1), lns[3].entity(1), lns[0].entity(1), lns[1].entity(1) }) {
        Vector p = SK.GetEntity(hp)->PointGetNum();
        corners.push_back({{"handle", H(hp)}, {"uv", f.To2d(p)}, {"xyz", Vec(p)}});
    }
    json constraints = json::array();
    for(Constraint &c : SK.constraint) {
        for(int i = 0; i < 4; i++) {
            if(c.entityA == lns[i].entity(0) || c.ptA.request() == lns[i]) {
                constraints.push_back(ConstraintJson(&c));
                break;
            }
        }
    }
    res->structured = {
        {"group", H(SS.GW.activeGroup)},
        {"requests", {H(lns[0]), H(lns[1]), H(lns[2]), H(lns[3])}},
        {"lines", lines},
        {"corners", corners},
        {"constraints", constraints},
        {"solve", SolveStatus(SS.GW.activeGroup)},
    };
}

//-----------------------------------------------------------------------------
// sketch_circle / sketch_arc
//-----------------------------------------------------------------------------
static void SketchCircle(const json &args, ToolResult *res) {
    RequireSketchGroup();
    Vector c = SketchPoint(args.value("center", json()), "center");
    double r = Num(args.value("radius", json()), "radius");
    if(r <= 0) throw ToolError("radius must be positive");
    bool inWorkplane = SS.GW.LockedInWorkplane();
    if(inWorkplane && args.contains("normal")) {
        throw ToolError("normal only applies in a 3D sketch; in a workplane the circle "
                        "lies in the workplane");
    }
    Transaction t;
    hRequest hr = NewRequest(Request::Type::CIRCLE, args.value("construction", false));
    SK.GetEntity(hr.entity(1))->PointForceTo(c);
    if(!inWorkplane) {
        SK.GetEntity(hr.entity(32))->NormalForceTo(QuaternionForNormal(NormalArg(args, "normal")));
    }
    SK.GetEntity(hr.entity(64))->DistanceForceTo(r);
    Regenerate();
    t.Commit();
    FinishSketchResult(res, json::array({RequestJson(hr)}));
}

static void SketchArc(const json &args, ToolResult *res) {
    RequireSketchGroup();
    RequireWorkplane("Arcs");
    Vector c = SketchPoint(args.value("center", json()), "center");
    Vector a = SketchPoint(args.value("start", json()), "start");
    Vector b = SketchPoint(args.value("end", json()), "end");
    double r = a.Minus(c).Magnitude();
    if(r < LENGTH_EPS) throw ToolError("start can't be at the center");
    if(b.Minus(c).Magnitude() < LENGTH_EPS) throw ToolError("end can't be at the center");
    // The end point lies on the circle through start; take its direction only.
    b = c.Plus(b.Minus(c).WithMagnitude(r));
    if(args.value("clockwise", false)) swap(a, b);
    if(a.Equals(b)) throw ToolError("start and end are the same point; use sketch_circle");

    Transaction t;
    hRequest hr = NewRequest(Request::Type::ARC_OF_CIRCLE, args.value("construction", false));
    SK.GetEntity(hr.entity(1))->PointForceTo(c);
    SK.GetEntity(hr.entity(2))->PointForceTo(a);
    SK.GetEntity(hr.entity(3))->PointForceTo(b);
    Regenerate();
    t.Commit();
    FinishSketchResult(res, json::array({RequestJson(hr)}));
}

//-----------------------------------------------------------------------------
// sketch_cubic
//-----------------------------------------------------------------------------
static void SketchCubic(const json &args, ToolResult *res) {
    RequireSketchGroup();
    const json &pts = args.value("points", json());
    bool periodic = args.value("periodic", false);
    if(!pts.is_array()) throw ToolError("points is required");
    size_t n = pts.size();
    if(!periodic && (n < 2 || n > 10)) {
        throw ToolError("points needs 2 to 10 on-curve points");
    }
    if(periodic && (n < 3 || n > 11)) {
        throw ToolError("a periodic (closed) spline needs 3 to 11 points");
    }
    std::vector<Vector> p;
    for(size_t i = 0; i < n; i++) {
        p.push_back(SketchPoint(pts[i], ("points[" + std::to_string(i) + "]").c_str()));
    }
    Vector ctrlS, ctrlF;
    if(!periodic) {
        ctrlS = args.contains("start_control")
            ? SketchPoint(args["start_control"], "start_control")
            : p[0].Plus(p[1].Minus(p[0]).ScaledBy(1.0 / 3));
        ctrlF = args.contains("end_control")
            ? SketchPoint(args["end_control"], "end_control")
            : p[n - 1].Minus(p[n - 1].Minus(p[n - 2]).ScaledBy(1.0 / 3));
    }

    Transaction t;
    hRequest hr = NewRequest(periodic ? Request::Type::CUBIC_PERIODIC : Request::Type::CUBIC,
                             args.value("construction", false));
    // The request's point layout depends on extraPoints, so regenerate the
    // entities once it's set, as the GUI does while adding points.
    SK.GetRequest(hr)->extraPoints = periodic ? (int)n - 3 : (int)n - 2;
    SS.GenerateAll(SolveSpaceUI::Generate::REGEN);
    if(periodic) {
        for(size_t i = 0; i < n; i++) SK.GetEntity(hr.entity((int)i + 1))->PointForceTo(p[i]);
    } else {
        // [0] start, [1] start tangent control, [2..n-1] interior on-curve
        // points, [n] end tangent control, [n+1] end.
        SK.GetEntity(hr.entity(1))->PointForceTo(p[0]);
        SK.GetEntity(hr.entity(2))->PointForceTo(ctrlS);
        for(size_t i = 1; i + 1 < n; i++) {
            SK.GetEntity(hr.entity((int)i + 2))->PointForceTo(p[i]);
        }
        SK.GetEntity(hr.entity((int)n + 1))->PointForceTo(ctrlF);
        SK.GetEntity(hr.entity((int)n + 2))->PointForceTo(p[n - 1]);
    }
    SS.MarkGroupDirty(SS.GW.activeGroup);
    Regenerate();
    t.Commit();
    FinishSketchResult(res, json::array({RequestJson(hr)}));
}

//-----------------------------------------------------------------------------
// sketch_text / sketch_image
//-----------------------------------------------------------------------------
// Places the four corners of a text or image request: point[0] (entity 1)
// is the top-left, point[1] the bottom-left origin, point[2] bottom-right,
// point[3] top-right.
static void PlaceBox(hRequest hr, double u, double v, double width, double height) {
    Frame f = FrameOf(SS.GW.ActiveWorkplane());
    SK.GetEntity(hr.entity(1))->PointForceTo(f.To3d(u, v + height));
    SK.GetEntity(hr.entity(2))->PointForceTo(f.To3d(u, v));
    SK.GetEntity(hr.entity(3))->PointForceTo(f.To3d(u + width, v));
    SK.GetEntity(hr.entity(4))->PointForceTo(f.To3d(u + width, v + height));
}

static void SketchText(const json &args, ToolResult *res) {
    RequireSketchGroup();
    RequireWorkplane("Text");
    std::string text = args.value("text", "");
    if(text.empty()) throw ToolError("text can't be empty");
    double u, v;
    Point2(args.value("origin", json::array({0, 0})), "origin", &u, &v);
    double height = Num(args.value("height", json(10.0)), "height");
    if(height <= 0) throw ToolError("height must be positive");
    std::string font = args.value("font", Platform::embeddedFont);

    Transaction t;
    hRequest hr = NewRequest(Request::Type::TTF_TEXT, args.value("construction", false));
    Request *r = SK.GetRequest(hr);
    r->str  = text;
    r->font = font;
    SS.GenerateAll(SolveSpaceUI::Generate::REGEN);
    double aspect = SK.GetRequest(hr)->aspectRatio;
    if(aspect <= 0) aspect = 1;
    PlaceBox(hr, u, v, height * aspect, height);
    SS.MarkGroupDirty(SS.GW.activeGroup);
    Regenerate();
    t.Commit();
    FinishSketchResult(res, json::array({RequestJson(hr)}));
}

static void SketchImage(const json &args, ToolResult *res) {
    RequireSketchGroup();
    RequireWorkplane("Images");
    std::string file = args.value("path", "");
    Platform::Path path = Platform::Path::From(file);
    if(file.empty() || !path.IsAbsolute()) throw ToolError("path must be an absolute path");
    FILE *f = OpenFile(path, "rb");
    if(!f) throw ToolError("Can't open '" + file + "'");
    fclose(f);
    double u, v;
    Point2(args.value("origin", json::array({0, 0})), "origin", &u, &v);
    double height = Num(args.value("height", json(50.0)), "height");
    if(height <= 0) throw ToolError("height must be positive");

    // Only PNGs can be loaded; check first, since a file that doesn't load
    // makes SolveSpace ask where the file is.
    if(Pixmap::ReadPng(path) == NULL) {
        throw ToolError("Couldn't load '" + file + "' as a PNG image (only PNG is supported)");
    }
    if(!SS.ReloadLinkedImage(SS.saveFile, &path, /*canCancel=*/false)) {
        ThrowIfErrors();
        throw ToolError("Couldn't load the image '" + file + "'");
    }
    Transaction t;
    hRequest hr = NewRequest(Request::Type::IMAGE, true);
    SK.GetRequest(hr)->file = path;
    SS.GenerateAll(SolveSpaceUI::Generate::REGEN);
    double aspect = SK.GetRequest(hr)->aspectRatio;
    if(aspect <= 0) aspect = 1;
    PlaceBox(hr, u, v, height * aspect, height);
    SS.MarkGroupDirty(SS.GW.activeGroup);
    Regenerate();
    t.Commit();
    FinishSketchResult(res, json::array({RequestJson(hr)}));
}

//-----------------------------------------------------------------------------
// sketch_workplane (a datum workplane, in a 3D sketch)
//-----------------------------------------------------------------------------
static void SketchWorkplane(const json &args, ToolResult *res) {
    RequireSketchGroup();
    if(SS.GW.LockedInWorkplane()) {
        throw ToolError("Datum workplanes can only be drawn in a 3D sketch "
                        "(create_3d_sketch). To sketch in a new plane, use "
                        "create_workplane_sketch instead.");
    }
    Vector origin = Vec3(args.value("origin", json::array({0, 0, 0})), "origin");
    Vector n = NormalArg(args, "normal");
    Quaternion q;
    if(args.contains("u_axis")) {
        Vector u = Vec3(args["u_axis"], "u_axis");
        u = u.Minus(n.WithMagnitude(1).ScaledBy(u.Dot(n.WithMagnitude(1))));
        if(u.Magnitude() < LENGTH_EPS) throw ToolError("u_axis can't be parallel to normal");
        u = u.WithMagnitude(1);
        q = Quaternion::From(u, n.WithMagnitude(1).Cross(u));
    } else {
        q = QuaternionForNormal(n);
    }
    Transaction t;
    hRequest hr = NewRequest(Request::Type::WORKPLANE, args.value("construction", false));
    SK.GetEntity(hr.entity(1))->PointForceTo(origin);
    SK.GetEntity(hr.entity(32))->NormalForceTo(q);
    Regenerate();
    t.Commit();
    FinishSketchResult(res, json::array({RequestJson(hr)}));
}

//-----------------------------------------------------------------------------
// fillet / split_curves
//-----------------------------------------------------------------------------
static void Fillet(const json &args, ToolResult *res) {
    RequireSketchGroup();
    RequireWorkplane("Fillets");
    hEntity pt = ParseEntity(args.value("point", json()));
    if(!SK.GetEntity(pt)->IsPoint()) throw ToolError(H(pt) + " isn't a point");
    double radius = args.contains("radius") ? Num(args["radius"], "radius") : 0;
    if(args.contains("radius") && radius <= 0) throw ToolError("radius must be positive");

    std::set<uint32_t> before = RequestSet();
    double savedRadius = SS.tangentArcRadius;
    bool savedManual = SS.tangentArcManual;
    SS.tangentArcManual = args.contains("radius");
    if(SS.tangentArcManual) SS.tangentArcRadius = radius;

    Transaction t;
    SelectOnly(Handles{{pt}, {}, {}, {}});
    GraphicsWindow::MenuRequest(Command::TANGENT_ARC);
    SS.tangentArcRadius = savedRadius;
    SS.tangentArcManual = savedManual;
    ThrowIfErrors();
    Regenerate();
    json created = NewRequestsSince(before);
    if(created.empty()) throw ToolError("No fillet was made.");
    t.Commit();
    FinishSketchResult(res, created);
    res->structured["note"] = "The two curves were trimmed; their old handles may have "
                              "changed. Use list_entities to see the result.";
}

static void SplitCurves(const json &args, ToolResult *res) {
    RequireSketchGroup();
    RequireWorkplane("Splitting");
    Handles h = ParseHandles(args.value("handles", json::array()));
    std::set<uint32_t> before = RequestSet();
    Transaction t;
    SelectOnly(h);
    GraphicsWindow::MenuRequest(Command::SPLIT_CURVES);
    ThrowIfErrors();
    Regenerate();
    t.Commit();
    FinishSketchResult(res, NewRequestsSince(before));
}

//-----------------------------------------------------------------------------
// set_construction
//-----------------------------------------------------------------------------
static void SetConstruction(const json &args, ToolResult *res) {
    Handles h = ParseHandles(args.value("handles", json::array()));
    bool construction = args.value("construction", true);
    std::set<uint32_t> reqs;
    for(hRequest hr : h.requests) reqs.insert(hr.v);
    for(hEntity he : h.entities) {
        if(!he.isFromRequest()) throw ToolError(H(he) + " is not sketch geometry");
        reqs.insert(he.request().v);
    }
    if(reqs.empty()) throw ToolError("Give some request or entity handles");
    Transaction t;
    json changed = json::array();
    for(uint32_t v : reqs) {
        hRequest hr = { v };
        if(hr.IsFromReferences()) throw ToolError("Reference geometry can't be changed");
        Request *r = SK.GetRequest(hr);
        r->construction = construction;
        SS.MarkGroupDirty(r->group);
        changed.push_back(H(hr));
    }
    Regenerate();
    t.Commit();
    res->structured = {{"changed", changed}, {"construction", construction}};
}

//-----------------------------------------------------------------------------
// move_point
//-----------------------------------------------------------------------------
static void MovePoint(const json &args, ToolResult *res) {
    hEntity hp = ParseEntity(args.value("point", json()));
    Entity *e = SK.GetEntity(hp);
    if(!e->IsPoint()) throw ToolError(H(hp) + " isn't a point");
    if(e->group != SS.GW.activeGroup) {
        throw ToolError(H(hp) + " belongs to group " + H(e->group) +
                        "; only points in the active group can be moved");
    }
    Vector to;
    bool inWorkplane = e->workplane != Entity::FREE_IN_3D;
    if(inWorkplane) {
        double u, v;
        Point2(args.value("to", json()), "to", &u, &v);
        to = FrameOf(e->workplane).To3d(u, v);
    } else {
        to = Vec3(args.value("to", json()), "to");
    }

    Transaction t;
    // Solve as if the user dragged the point there: the solver keeps the
    // dragged point where it was put if it can.
    SS.GW.pending.points.Add(&hp);
    e->PointForceTo(to);
    SS.MarkGroupDirty(e->group);
    Regenerate();
    SS.GW.ClearPending(/*scheduleShowTW=*/true);
    Regenerate();
    t.Commit();
    Entity *moved = SK.GetEntity(hp);
    res->structured = {{"point", EntityJson(moved)},
                       {"reached", moved->PointGetNum().Equals(to, 1e-6)},
                       {"solve", SolveStatus(SS.GW.activeGroup)}};
}

void AddSketchTools(std::vector<Tool> *tools) {
    json construction = BoolProp(false,
        "Construction geometry: used for constraints, but not part of the profile.");
    json point = PointSchema("[u, v] in the active workplane, or [x, y, z] in a 3D sketch (mm).");
    tools->push_back({
        "create_workplane_sketch",
        "Create a new 'sketch in workplane' group and make it active; it is inserted "
        "after the active group. Ways to place the workplane:\n"
        "- plane only: on a reference plane through the origin. XY: u=X, v=Y; YZ: "
        "u=Y, v=Z; ZX: u=Z, v=X.\n"
        "- point (+ plane): through an existing point, parallel to that reference "
        "plane. Use this to sketch on a face of a solid: pick a vertex on the face "
        "and the plane the face is parallel to.\n"
        "- point + normal: through the point, perpendicular to a normal entity.\n"
        "- point + line_a + line_b: through the point, parallel to two lines.\n"
        "- workplane: a copy of an existing workplane entity.",
        {{"type", "object"}, {"properties", {
            {"plane", {{"type", "string"}, {"enum", {"XY", "YZ", "ZX"}}, {"default", "XY"}}},
            {"point", StrProp("Point entity handle the workplane passes through.")},
            {"normal", StrProp("Normal entity handle (with point).")},
            {"line_a", StrProp("Line entity handle (with point and line_b).")},
            {"line_b", StrProp("Line entity handle (with point and line_a).")},
            {"workplane", StrProp("Workplane entity handle to copy.")},
            {"name", StrProp("Optional group name.")},
        }}},
        CreateWorkplaneSketch,
    });
    tools->push_back({
        "create_3d_sketch",
        "Create a new 'sketch in 3D' group and make it active. In it, points are "
        "[x, y, z]; lines, points, circles (with a normal), cubics and datum "
        "workplanes can be drawn; arcs, rectangles, text and images need a workplane.",
        {{"type", "object"}, {"properties", {{"name", StrProp("Optional group name.")}}}},
        Create3dSketch,
    });
    tools->push_back({
        "sketch_point",
        "Draw a datum point in the active sketch.",
        {{"type", "object"}, {"properties", {{"at", point}, {"construction", construction}}},
         {"required", {"at"}}},
        SketchPointTool,
    });
    tools->push_back({
        "sketch_line",
        "Draw a line segment, or a polyline through several points, in the active "
        "sketch. Consecutive segments are joined with coincident constraints; with "
        "closed=true the last point is joined back to the first, making a closed "
        "profile that can be extruded.",
        {{"type", "object"}, {"properties", {
            {"points", {{"type", "array"}, {"items", point}, {"minItems", 2},
                        {"description", "The vertices, in order."}}},
            {"closed", BoolProp(false, "Join the last point back to the first.")},
            {"construction", construction},
        }}, {"required", {"points"}}},
        SketchLine,
    });
    tools->push_back({
        "sketch_rectangle",
        "Draw an axis-aligned rectangle in the active workplane, given two opposite "
        "corners in workplane coordinates (mm). Like the GUI's rectangle tool, it "
        "creates 4 line segments with coincident corners and horizontal/vertical "
        "constraints, leaving 4 degrees of freedom (position and size).",
        {{"type", "object"}, {"properties", {
            {"corner1", Point2Schema("[u, v]")},
            {"corner2", Point2Schema("[u, v]")},
            {"construction", construction},
        }}, {"required", {"corner1", "corner2"}}},
        SketchRectangle,
    });
    tools->push_back({
        "sketch_circle",
        "Draw a circle in the active sketch. In a 3D sketch, normal sets its plane "
        "(default [0,0,1]).",
        {{"type", "object"}, {"properties", {
            {"center", point},
            {"radius", NumProp("Radius in mm.")},
            {"normal", Vec3Schema("3D sketches only: the circle's axis.")},
            {"construction", construction},
        }}, {"required", {"center", "radius"}}},
        SketchCircle,
    });
    tools->push_back({
        "sketch_arc",
        "Draw an arc of a circle in the active workplane. It runs counter-clockwise "
        "(about the workplane normal) from start to end, or clockwise with "
        "clockwise=true. The radius is |start - center|; end only gives the direction.",
        {{"type", "object"}, {"properties", {
            {"center", Point2Schema("[u, v]")},
            {"start", Point2Schema("[u, v]")},
            {"end", Point2Schema("[u, v]")},
            {"clockwise", BoolProp(false, "Go clockwise from start to end.")},
            {"construction", construction},
        }}, {"required", {"center", "start", "end"}}},
        SketchArc,
    });
    tools->push_back({
        "sketch_cubic",
        "Draw a cubic spline through the given on-curve points (2 to 10). With 2 "
        "points it is a single Bezier segment. start_control and end_control are "
        "the tangent control points at the ends (default: a third of the way to "
        "the neighbouring point). periodic=true makes a smooth closed loop through "
        "3 to 11 points.",
        {{"type", "object"}, {"properties", {
            {"points", {{"type", "array"}, {"items", point}, {"minItems", 2}}},
            {"start_control", point},
            {"end_control", point},
            {"periodic", BoolProp(false, "Closed, periodic spline.")},
            {"construction", construction},
        }}, {"required", {"points"}}},
        SketchCubic,
    });
    tools->push_back({
        "sketch_text",
        "Draw TrueType text in the active workplane. origin is the bottom-left "
        "corner; the width follows from the height and the font.",
        {{"type", "object"}, {"properties", {
            {"text", StrProp("The text.")},
            {"origin", Point2Schema("Bottom-left corner [u, v]; default [0, 0].")},
            {"height", NumProp("Text height in mm (default 10).")},
            {"font", StrProp("Font file name (default: the built-in Bitstream Vera Sans).")},
            {"construction", construction},
        }}, {"required", {"text"}}},
        SketchText,
    });
    tools->push_back({
        "sketch_image",
        "Place a PNG image (construction geometry, e.g. for tracing) in the "
        "active workplane, bottom-left corner at origin.",
        {{"type", "object"}, {"properties", {
            {"path", StrProp("Absolute path of the image file.")},
            {"origin", Point2Schema("Bottom-left corner [u, v]; default [0, 0].")},
            {"height", NumProp("Height in mm (default 50).")},
        }}, {"required", {"path"}}},
        SketchImage,
    });
    tools->push_back({
        "sketch_workplane",
        "Draw a datum workplane in the active 3D sketch, e.g. to sketch on later "
        "with create_workplane_sketch(workplane=...).",
        {{"type", "object"}, {"properties", {
            {"origin", Vec3Schema("[x, y, z]; default the origin.")},
            {"normal", Vec3Schema("Plane normal; default [0, 0, 1].")},
            {"u_axis", Vec3Schema("Optional in-plane u direction.")},
            {"construction", construction},
        }}},
        SketchWorkplane,
    });
    tools->push_back({
        "fillet",
        "Round the corner where two lines/arcs meet at a point, with a tangent arc "
        "(Sketch > Tangent Arc at Point). The curves are trimmed. Without radius, "
        "SolveSpace picks one.",
        {{"type", "object"}, {"properties", {
            {"point", StrProp("The shared endpoint (point entity handle) to round.")},
            {"radius", NumProp("Fillet radius in mm.")},
        }}, {"required", {"point"}}},
        Fillet,
    });
    tools->push_back({
        "split_curves",
        "Split curves where they intersect (Sketch > Split Curves at Intersection): "
        "give two intersecting curves, or one curve and a point on it.",
        {{"type", "object"}, {"properties", {
            {"handles", HandleListSchema("Two curve entity handles, or a curve and a point.")},
        }}, {"required", {"handles"}}},
        SplitCurves,
    });
    tools->push_back({
        "set_construction",
        "Mark sketch geometry as construction (or normal) geometry. Construction "
        "geometry is not part of profiles that get extruded.",
        {{"type", "object"}, {"properties", {
            {"handles", HandleListSchema("Request or entity handles.")},
            {"construction", BoolProp(true, "true for construction, false for normal.")},
        }}, {"required", {"handles"}}},
        SetConstruction,
    });
    tools->push_back({
        "move_point",
        "Move a point of the active group, like dragging it in the GUI: the solver "
        "moves the rest of the sketch to keep the constraints, and the point ends "
        "where requested if the constraints allow ('reached').",
        {{"type", "object"}, {"properties", {
            {"point", StrProp("Point entity handle.")},
            {"to", point},
        }}, {"required", {"point", "to"}}},
        MovePoint,
    });
}

} // namespace Mcp
} // namespace SolveSpace
