//-----------------------------------------------------------------------------
// The MCP tools. Every handler runs on the GUI thread, inside the scope set
// up by CallToolOnGuiThread() in mcpserver.cpp (messages captured, UI
// refreshed afterwards).
//
// Conventions: lengths in mm, angles in degrees; handles are strings with a
// type prefix ("g:0002", "r:0004", "e:00040001", "c:0007").
//-----------------------------------------------------------------------------
#include "solvespace.h"
#include "mcp.h"

namespace SolveSpace {
namespace Mcp {

const char *ServerInstructions =
R"(This server controls a running SolveSpace instance (a parametric, constraint-based 2D/3D CAD program).

Model structure:
- A model is an ordered list of groups. Each group is either a sketch ("sketch-in-plane" in a workplane, or "sketch-in-3d") or an operation on earlier groups (extrude, lathe, revolve, helix, rotate, translate, link). There is no editable feature history beyond this ordered group list.
- New geometry and constraints always go into the ACTIVE group. Creating a group makes it active.
- Sketch entities (lines, circles, arcs, ...) are created by "requests"; each request generates entities, e.g. a line segment request r:0004 generates the line e:00040000 and its endpoints e:00040001 and e:00040002. An entity handle encodes its request in its upper 16 bits.
- Geometry is positioned by a constraint solver. "dof" is the remaining degrees of freedom of the active group; 0 means fully constrained.

Conventions:
- All lengths are in millimetres and angles in degrees, regardless of the units shown in the UI.
- In a workplane sketch, 2D coordinates [u, v] are in the workplane's frame (see u_axis/v_axis in results). The XY plane has u=X, v=Y; YZ has u=Y, v=Z; ZX has u=Z, v=X.
- Handles are strings: g:<hex> group, r:<hex> request, e:<hex> entity, c:<hex> constraint.

Typical workflow: create_workplane_sketch, then sketch_* tools, then inspect with list_entities / get_model_summary, and check the result visually with screenshot. Each modifying call is a single undo step in the GUI.)";

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static std::string Hex(const char *prefix, uint32_t v, int digits) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%0*x", prefix, digits, v);
    return buf;
}
static std::string H(hGroup h)      { return Hex("g:", h.v, 4); }
static std::string H(hRequest h)    { return Hex("r:", h.v, 4); }
static std::string H(hEntity h)     { return Hex("e:", h.v, 8); }
static std::string H(hConstraint h) { return Hex("c:", h.v, 4); }

static uint32_t ParseHandle(const json &j, const char *prefix, const char *what) {
    if(!j.is_string()) throw ToolError(std::string(what) + " handle must be a string");
    std::string s = j;
    size_t n = strlen(prefix);
    if(s.compare(0, n, prefix) != 0 || s.size() == n) {
        throw ToolError(std::string("Expected a ") + what + " handle like '" + prefix +
                        "0002', got '" + s + "'");
    }
    char *end;
    unsigned long v = strtoul(s.c_str() + n, &end, 16);
    if(*end != '\0') throw ToolError("Malformed handle '" + s + "'");
    return (uint32_t)v;
}

static hGroup ParseGroup(const json &j) {
    hGroup hg = { ParseHandle(j, "g:", "group") };
    if(!SK.group.FindByIdNoOops(hg)) throw ToolError("No such group: " + j.get<std::string>());
    return hg;
}

static json Vec(Vector v) { return json::array({v.x, v.y, v.z}); }

static double Num(const json &j, const char *what) {
    if(!j.is_number()) throw ToolError(std::string(what) + " must be a number");
    return j.get<double>();
}

static void Point2(const json &j, const char *what, double *u, double *v) {
    if(!j.is_array() || j.size() != 2) {
        throw ToolError(std::string(what) + " must be an array [u, v]");
    }
    *u = Num(j[0], what);
    *v = Num(j[1], what);
}

// The frame of a workplane entity: origin and the in-plane axes.
struct Frame {
    Vector origin, u, v, n;
    Vector To3d(double pu, double pv) const {
        return origin.Plus(u.ScaledBy(pu)).Plus(v.ScaledBy(pv));
    }
    json To2d(Vector p) const {
        Vector d = p.Minus(origin);
        return json::array({d.Dot(u), d.Dot(v)});
    }
};

static Frame FrameOf(hEntity wrkpl) {
    Entity *wp = SK.GetEntity(wrkpl);
    EntityBase *normal = wp->Normal();
    Frame f;
    f.origin = wp->WorkplaneGetOffset();
    f.u = normal->NormalU();
    f.v = normal->NormalV();
    f.n = normal->NormalN();
    return f;
}

static const char *GroupTypeName(Group::Type t) {
    switch(t) {
        case Group::Type::DRAWING_3D:        return "sketch_3d";
        case Group::Type::DRAWING_WORKPLANE: return "sketch_in_workplane";
        case Group::Type::EXTRUDE:           return "extrude";
        case Group::Type::LATHE:             return "lathe";
        case Group::Type::REVOLVE:           return "revolve";
        case Group::Type::HELIX:             return "helix";
        case Group::Type::ROTATE:            return "rotate";
        case Group::Type::TRANSLATE:         return "translate";
        case Group::Type::LINKED:            return "linked";
    }
    return "unknown";
}

static const char *SolveResultName(SolveResult how) {
    switch(how) {
        case SolveResult::OKAY:                     return "ok";
        case SolveResult::DIDNT_CONVERGE:           return "didnt_converge";
        case SolveResult::REDUNDANT_OKAY:           return "redundant_ok";
        case SolveResult::REDUNDANT_DIDNT_CONVERGE: return "redundant_didnt_converge";
        case SolveResult::TOO_MANY_UNKNOWNS:        return "too_many_unknowns";
        case SolveResult::UNKNOWN_NAME:             return "unknown_name";
    }
    return "unknown";
}

static const char *EntityTypeName(Entity::Type t) {
    switch(t) {
        case Entity::Type::POINT_IN_3D:
        case Entity::Type::POINT_IN_2D:
        case Entity::Type::POINT_N_TRANS:
        case Entity::Type::POINT_N_ROT_TRANS:
        case Entity::Type::POINT_N_COPY:
        case Entity::Type::POINT_N_ROT_AA:
        case Entity::Type::POINT_N_ROT_AXIS_TRANS: return "point";
        case Entity::Type::NORMAL_IN_3D:
        case Entity::Type::NORMAL_IN_2D:
        case Entity::Type::NORMAL_N_COPY:
        case Entity::Type::NORMAL_N_ROT:
        case Entity::Type::NORMAL_N_ROT_AA:        return "normal";
        case Entity::Type::DISTANCE:
        case Entity::Type::DISTANCE_N_COPY:        return "distance";
        case Entity::Type::FACE_NORMAL_PT:
        case Entity::Type::FACE_XPROD:
        case Entity::Type::FACE_N_ROT_TRANS:
        case Entity::Type::FACE_N_TRANS:
        case Entity::Type::FACE_N_ROT_AA:
        case Entity::Type::FACE_ROT_NORMAL_PT:
        case Entity::Type::FACE_N_ROT_AXIS_TRANS:  return "face";
        case Entity::Type::WORKPLANE:              return "workplane";
        case Entity::Type::LINE_SEGMENT:           return "line_segment";
        case Entity::Type::CUBIC:                  return "cubic";
        case Entity::Type::CUBIC_PERIODIC:         return "cubic_periodic";
        case Entity::Type::CIRCLE:                 return "circle";
        case Entity::Type::ARC_OF_CIRCLE:          return "arc";
        case Entity::Type::TTF_TEXT:               return "text";
        case Entity::Type::IMAGE:                  return "image";
    }
    return "unknown";
}

static std::string ConstraintTypeName(Constraint::Type t) {
    switch(t) {
        case Constraint::Type::POINTS_COINCIDENT:  return "coincident";
        case Constraint::Type::PT_PT_DISTANCE:     return "distance";
        case Constraint::Type::PT_PLANE_DISTANCE:  return "point_plane_distance";
        case Constraint::Type::PT_LINE_DISTANCE:   return "point_line_distance";
        case Constraint::Type::PT_FACE_DISTANCE:   return "point_face_distance";
        case Constraint::Type::PROJ_PT_DISTANCE:   return "projected_distance";
        case Constraint::Type::PT_IN_PLANE:        return "point_in_plane";
        case Constraint::Type::PT_ON_LINE:         return "point_on_line";
        case Constraint::Type::PT_ON_FACE:         return "point_on_face";
        case Constraint::Type::EQUAL_LENGTH_LINES: return "equal_length";
        case Constraint::Type::LENGTH_RATIO:       return "length_ratio";
        case Constraint::Type::LENGTH_DIFFERENCE:  return "length_difference";
        case Constraint::Type::SYMMETRIC:          return "symmetric";
        case Constraint::Type::SYMMETRIC_HORIZ:    return "symmetric_horizontal";
        case Constraint::Type::SYMMETRIC_VERT:     return "symmetric_vertical";
        case Constraint::Type::SYMMETRIC_LINE:     return "symmetric_line";
        case Constraint::Type::AT_MIDPOINT:        return "at_midpoint";
        case Constraint::Type::HORIZONTAL:         return "horizontal";
        case Constraint::Type::VERTICAL:           return "vertical";
        case Constraint::Type::DIAMETER:           return "diameter";
        case Constraint::Type::PT_ON_CIRCLE:       return "point_on_circle";
        case Constraint::Type::SAME_ORIENTATION:   return "same_orientation";
        case Constraint::Type::ANGLE:              return "angle";
        case Constraint::Type::PARALLEL:           return "parallel";
        case Constraint::Type::PERPENDICULAR:      return "perpendicular";
        case Constraint::Type::ARC_LINE_TANGENT:   return "arc_line_tangent";
        case Constraint::Type::EQUAL_RADIUS:       return "equal_radius";
        case Constraint::Type::WHERE_DRAGGED:      return "where_dragged";
        case Constraint::Type::COMMENT:            return "comment";
        default: return "type_" + std::to_string((uint32_t)t);
    }
}

static const char *UnitName(Unit u) {
    switch(u) {
        case Unit::MM:          return "mm";
        case Unit::METERS:      return "m";
        case Unit::INCHES:      return "in";
        case Unit::FEET_INCHES: return "ft_in";
    }
    return "mm";
}

static json SolveStatus(hGroup hg) {
    Group *g = SK.GetGroup(hg);
    return {{"status", SolveResultName(g->solved.how)}, {"dof", g->solved.dof}};
}

static json ActiveWorkplaneJson() {
    if(!SS.GW.LockedInWorkplane()) return json();
    hEntity wp = SS.GW.ActiveWorkplane();
    Frame f = FrameOf(wp);
    return {{"handle", H(wp)}, {"origin", Vec(f.origin)},
            {"u_axis", Vec(f.u)}, {"v_axis", Vec(f.v)}, {"normal", Vec(f.n)}};
}

// Describes one entity; null for those not worth reporting (e.g. the
// normal of a circle, which is reported with the circle).
static json EntityJson(Entity *e) {
    json j;
    j["handle"] = H(e->h);
    j["type"]   = EntityTypeName(e->type);
    if(e->h.isFromRequest()) {
        j["request"] = H(e->h.request());
        Request *r = SK.request.FindByIdNoOops(e->h.request());
        if(r) j["construction"] = r->construction;
    }
    bool inWorkplane = e->workplane != Entity::FREE_IN_3D &&
                       SK.entity.FindByIdNoOops(e->workplane) != NULL;
    Frame f;
    if(inWorkplane) {
        f = FrameOf(e->workplane);
        j["workplane"] = H(e->workplane);
    }
    auto PointJson = [&](hEntity hp) {
        Vector p = SK.GetEntity(hp)->PointGetNum();
        json pj = {{"handle", H(hp)}, {"xyz", Vec(p)}};
        if(inWorkplane) pj["uv"] = f.To2d(p);
        return pj;
    };

    if(e->IsPoint()) {
        Vector p = e->PointGetNum();
        j["xyz"] = Vec(p);
        if(inWorkplane) j["uv"] = f.To2d(p);
    } else if(e->IsNormal()) {
        j["direction"] = Vec(e->NormalN());
    } else if(e->IsFace()) {
        j["normal"] = Vec(e->FaceGetNormalNum());
        j["point"]  = Vec(e->FaceGetPointNum());
    } else {
        switch(e->type) {
            case Entity::Type::WORKPLANE: {
                Frame wf = FrameOf(e->h);
                j["origin"] = Vec(wf.origin);
                j["u_axis"] = Vec(wf.u);
                j["v_axis"] = Vec(wf.v);
                j["normal"] = Vec(wf.n);
                break;
            }
            case Entity::Type::LINE_SEGMENT: {
                Vector a = SK.GetEntity(e->point[0])->PointGetNum(),
                       b = SK.GetEntity(e->point[1])->PointGetNum();
                j["points"] = {PointJson(e->point[0]), PointJson(e->point[1])};
                j["length"] = a.Minus(b).Magnitude();
                break;
            }
            case Entity::Type::CIRCLE:
                j["center"] = PointJson(e->point[0]);
                j["radius"] = e->CircleGetRadiusNum();
                j["normal"] = Vec(e->Normal()->NormalN());
                break;
            case Entity::Type::ARC_OF_CIRCLE:
                j["center"] = PointJson(e->point[0]);
                j["start"]  = PointJson(e->point[1]);
                j["end"]    = PointJson(e->point[2]);
                j["radius"] = e->CircleGetRadiusNum();
                j["normal"] = Vec(e->Normal()->NormalN());
                break;
            case Entity::Type::CUBIC:
            case Entity::Type::CUBIC_PERIODIC: {
                json pts = json::array();
                for(int i = 0; i < 4 + e->extraPoints; i++) pts.push_back(PointJson(e->point[i]));
                j["control_points"] = pts;
                break;
            }
            case Entity::Type::TTF_TEXT:
                j["text"] = e->str;
                j["font"] = e->font;
                break;
            default:
                break;
        }
    }
    return j;
}

static json ConstraintJson(Constraint *c) {
    json j = {{"handle", H(c->h)}, {"type", ConstraintTypeName(c->type)}};
    if(c->ptA.v)     j["ptA"]     = H(c->ptA);
    if(c->ptB.v)     j["ptB"]     = H(c->ptB);
    if(c->entityA.v) j["entityA"] = H(c->entityA);
    if(c->entityB.v) j["entityB"] = H(c->entityB);
    if(c->HasLabel()) {
        j["value"]     = c->valA;
        j["reference"] = c->reference;
    }
    if(!c->comment.empty()) j["comment"] = c->comment;
    return j;
}

static json IntProp(int lo, int hi, int dflt, const char *desc) {
    return {{"type", "integer"}, {"minimum", lo}, {"maximum", hi},
            {"default", dflt}, {"description", desc}};
}

static const json POINT2_SCHEMA = {
    {"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2},
};

//-----------------------------------------------------------------------------
// get_model_summary
//-----------------------------------------------------------------------------
static void GetModelSummary(const json &, ToolResult *res) {
    json groups = json::array();
    for(hGroup hg : SK.groupOrder) {
        Group *g = SK.GetGroup(hg);
        int requests = 0, constraints = 0;
        for(Request &r : SK.request)       if(r.group == hg) requests++;
        for(Constraint &c : SK.constraint) if(c.group == hg) constraints++;
        json gj = {
            {"handle", H(hg)}, {"name", g->name}, {"type", GroupTypeName(g->type)},
            {"visible", g->visible}, {"active", hg == SS.GW.activeGroup},
            {"requests", requests}, {"constraints", constraints},
        };
        if(hg != Group::HGROUP_REFERENCES) gj["solve"] = SolveStatus(hg);
        groups.push_back(gj);
    }
    json &s = res->structured;
    s["file"] = SS.saveFile.IsEmpty() ? json() : json(SS.saveFile.raw);
    s["unsaved"] = SS.unsaved;
    s["display_units"] = UnitName(SS.viewUnits);
    s["groups"] = groups;
    s["active_group"] = H(SS.GW.activeGroup);
    s["active_workplane"] = ActiveWorkplaneJson();
    s["active_solve"] = SolveStatus(SS.GW.activeGroup);
}

//-----------------------------------------------------------------------------
// list_entities
//-----------------------------------------------------------------------------
static void ListEntities(const json &args, ToolResult *res) {
    hGroup hg = args.contains("group") ? ParseGroup(args["group"]) : SS.GW.activeGroup;
    bool includeAll = args.value("include_normals", false);

    json entities = json::array();
    for(Entity &e : SK.entity) {
        if(e.group != hg) continue;
        if(!includeAll && (e.IsNormal() || e.IsDistance())) continue;
        entities.push_back(EntityJson(&e));
    }
    json constraints = json::array();
    for(Constraint &c : SK.constraint) {
        if(c.group != hg) continue;
        constraints.push_back(ConstraintJson(&c));
    }
    Group *g = SK.GetGroup(hg);
    res->structured = {
        {"group", H(hg)}, {"group_name", g->name}, {"group_type", GroupTypeName(g->type)},
        {"entities", entities}, {"constraints", constraints},
    };
    if(hg != Group::HGROUP_REFERENCES) res->structured["solve"] = SolveStatus(hg);
}

//-----------------------------------------------------------------------------
// create_workplane_sketch
//-----------------------------------------------------------------------------
static void CreateWorkplaneSketch(const json &args, ToolResult *res) {
    std::string plane = args.value("plane", "XY");
    hRequest ref;
    if(plane == "XY" || plane == "xy")      ref = Request::HREQUEST_REFERENCE_XY;
    else if(plane == "YZ" || plane == "yz") ref = Request::HREQUEST_REFERENCE_YZ;
    else if(plane == "ZX" || plane == "zx") ref = Request::HREQUEST_REFERENCE_ZX;
    else throw ToolError("plane must be one of XY, YZ, ZX");

    hGroup before = SS.GW.activeGroup;
    SS.GW.ClearSelection();
    SS.GW.MakeSelected(ref.entity(0));
    // This is exactly what Sketch -> In Workplane does with a workplane
    // selected; it records its own undo step.
    Group::MenuGroup(Command::GROUP_WRKPL);
    SS.GW.ClearSelection();

    if(SS.GW.activeGroup == before) {
        throw ToolError("The workplane sketch was not created.");
    }
    Group *g = SK.GetGroup(SS.GW.activeGroup);
    if(args.contains("name") && args["name"].is_string()) {
        g->name = args["name"].get<std::string>();
    }
    res->structured = {
        {"group", H(g->h)}, {"name", g->name},
        {"workplane", ActiveWorkplaneJson()},
        {"solve", SolveStatus(g->h)},
    };
}

//-----------------------------------------------------------------------------
// sketch_rectangle
//-----------------------------------------------------------------------------
static void SketchRectangle(const json &args, ToolResult *res) {
    if(!SS.GW.LockedInWorkplane()) {
        throw ToolError("No active workplane. Rectangles can only be drawn in a "
                        "workplane; call create_workplane_sketch first.");
    }
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
    // Corners: c1 and c2 as given, plus the other two.
    Vector c1  = f.To3d(u1, v1), c2  = f.To3d(u2, v2),
           c21 = f.To3d(u2, v1), c12 = f.To3d(u1, v2);

    // The same construction as the interactive rectangle tool in mouse.cpp:
    // line i's start is coincident with line i+1's end; lines 0 and 2 are
    // vertical, 1 and 3 horizontal. That puts the corners at:
    //   line 0: c2  -> c21     line 1: c12 -> c2
    //   line 2: c1  -> c12     line 3: c21 -> c1
    const Vector starts[4] = { c2,  c12, c1,  c21 };
    const Vector ends[4]   = { c21, c2,  c12, c1  };

    SS.UndoRemember();
    hRequest lns[4];
    for(int i = 0; i < 4; i++) {
        lns[i] = SS.GW.AddRequest(Request::Type::LINE_SEGMENT, /*rememberForUndo=*/false);
        if(construction) SK.GetRequest(lns[i])->construction = true;
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
    SS.GenerateAll(SolveSpaceUI::Generate::DIRTY);

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
// screenshot
//-----------------------------------------------------------------------------
static std::string Base64(const std::vector<uint8_t> &data) {
    static const char *tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for(; i + 2 < data.size(); i += 3) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];  out += tbl[n & 63];
    }
    if(i + 1 == data.size()) {
        uint32_t n = data[i] << 16;
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += "==";
    } else if(i + 2 == data.size()) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];  out += '=';
    }
    return out;
}

static void Screenshot(const json &args, ToolResult *res) {
    int width  = args.value("width", 800);
    int height = args.value("height", 600);
    if(width < 16 || width > 4096 || height < 16 || height > 4096) {
        throw ToolError("width and height must be between 16 and 4096");
    }
    bool zoomToFit = args.value("zoom_to_fit", false);

    // Make sure the model is up to date before drawing it.
    SS.GenerateAll(SolveSpaceUI::Generate::DIRTY);

    Camera camera     = SS.GW.GetCamera();
    camera.width      = width;
    camera.height     = height;
    camera.pixelRatio = 1;
    camera.gridFit    = true;

    Vector savedOffset = SS.GW.offset;
    double savedScale  = SS.GW.scale;
    if(zoomToFit) {
        camera.scale  = SS.GW.ZoomToFit(camera);
        camera.offset = SS.GW.offset;
    }

    // The GUI's persistent canvas is an OpenGL batch; it can't be drawn into
    // a Cairo surface, so draw everything directly for this frame.
    std::shared_ptr<BatchCanvas> savedPersistent = SS.GW.persistentCanvas;
    SS.GW.persistentCanvas = NULL;

    CairoPixmapRenderer canvas;
    canvas.antialias = true;
    canvas.SetLighting(SS.GW.GetLighting());
    canvas.SetCamera(camera);
    canvas.Init();
    canvas.StartFrame();
    SS.GW.Draw(&canvas);
    canvas.FlushFrame();
    canvas.FinishFrame();
    std::shared_ptr<Pixmap> pixmap = canvas.ReadFrame();
    canvas.Clear();

    SS.GW.persistentCanvas = savedPersistent;
    SS.GW.persistentDirty  = true;
    SS.GW.offset = savedOffset;
    SS.GW.scale  = savedScale;

    // Encode to PNG via a temporary file; Pixmap only writes PNGs to files.
    std::vector<uint8_t> png;
    FILE *tmp = tmpfile();
    if(tmp == NULL) throw ToolError("Couldn't create a temporary file for the PNG");
    bool ok = pixmap->WritePng(tmp, /*flip=*/true);
    if(ok) {
        fflush(tmp);
        long size = ftell(tmp);
        rewind(tmp);
        png.resize((size_t)size);
        ok = fread(png.data(), 1, png.size(), tmp) == png.size();
    }
    fclose(tmp);
    if(!ok) throw ToolError("Couldn't encode the screenshot as PNG");

    res->content.push_back({{"type", "image"}, {"mimeType", "image/png"},
                            {"data", Base64(png)}});
    res->structured = {
        {"width", width}, {"height", height},
        {"view", {{"proj_right", Vec(camera.projRight)}, {"proj_up", Vec(camera.projUp)},
                  {"offset", Vec(camera.offset)}, {"scale", camera.scale}}},
    };
}

//-----------------------------------------------------------------------------
// Registry
//-----------------------------------------------------------------------------
const std::vector<Tool> &GetTools() {
    static const std::vector<Tool> tools = {
        {
            "get_model_summary",
            "Summarize the open model: its groups in order (type, visibility, "
            "active flag, solve status and degrees of freedom), the active group "
            "and workplane, the file name and the unsaved flag.",
            {{"type", "object"}, {"properties", json::object()}},
            GetModelSummary,
        },
        {
            "list_entities",
            "List the entities and constraints of a group (default: the active "
            "group), with solved coordinates: 3D [x,y,z] and, for entities in a "
            "workplane, 2D [u,v]. Lengths in mm.",
            {{"type", "object"}, {"properties", {
                {"group", {{"type", "string"},
                           {"description", "Group handle, e.g. 'g:0002'. Default: active group."}}},
                {"include_normals", {{"type", "boolean"}, {"default", false},
                    {"description", "Also list normal and distance entities."}}},
            }}},
            ListEntities,
        },
        {
            "create_workplane_sketch",
            "Create a new 'sketch in workplane' group on one of the reference "
            "planes through the origin, and make it the active group. Its 2D "
            "frame: XY -> u=X, v=Y; YZ -> u=Y, v=Z; ZX -> u=Z, v=X.",
            {{"type", "object"}, {"properties", {
                {"plane", {{"type", "string"}, {"enum", {"XY", "YZ", "ZX"}},
                           {"default", "XY"}}},
                {"name", {{"type", "string"}, {"description", "Optional group name."}}},
            }}},
            CreateWorkplaneSketch,
        },
        {
            "sketch_rectangle",
            "Draw an axis-aligned rectangle in the active workplane, given two "
            "opposite corners in workplane coordinates (mm). Like the GUI's "
            "rectangle tool, it creates 4 line segments with coincident corners "
            "and horizontal/vertical constraints, leaving 4 degrees of freedom "
            "(position and size).",
            {{"type", "object"}, {"properties", {
                {"corner1", POINT2_SCHEMA},
                {"corner2", POINT2_SCHEMA},
                {"construction", {{"type", "boolean"}, {"default", false}}},
            }}, {"required", {"corner1", "corner2"}}},
            SketchRectangle,
        },
        {
            "screenshot",
            "Render the current 3D view (the same camera as the SolveSpace window) "
            "to a PNG image. Rendered offscreen, so the toolbar is not shown.",
            {{"type", "object"}, {"properties", {
                {"width",  IntProp(16, 4096, 800, "Image width in pixels.")},
                {"height", IntProp(16, 4096, 600, "Image height in pixels.")},
                {"zoom_to_fit", {{"type", "boolean"}, {"default", false},
                    {"description", "Fit the model in the image (without moving the GUI's view)."}}},
            }}},
            Screenshot,
        },
    };
    return tools;
}

} // namespace Mcp
} // namespace SolveSpace
