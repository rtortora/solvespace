//-----------------------------------------------------------------------------
// Shared helpers for the MCP tools; see mcptools.h.
//-----------------------------------------------------------------------------
#include "mcptools.h"

namespace SolveSpace {
namespace Mcp {

//-----------------------------------------------------------------------------
// Handles
//-----------------------------------------------------------------------------
static std::string Hex(const char *prefix, uint32_t v, int digits) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%0*x", prefix, digits, v);
    return buf;
}
std::string H(hGroup h)      { return Hex("g:", h.v, 4); }
std::string H(hRequest h)    { return Hex("r:", h.v, 4); }
std::string H(hEntity h)     { return Hex("e:", h.v, 8); }
std::string H(hConstraint h) { return Hex("c:", h.v, 4); }
std::string H(hStyle h)      { return Hex("s:", h.v, 4); }

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

hGroup ParseGroup(const json &j) {
    hGroup h = { ParseHandle(j, "g:", "group") };
    if(!SK.group.FindByIdNoOops(h)) throw ToolError("No such group: " + j.get<std::string>());
    return h;
}
hEntity ParseEntity(const json &j) {
    hEntity h = { ParseHandle(j, "e:", "entity") };
    Entity *e = SK.entity.FindByIdNoOops(h);
    if(!e) {
        throw ToolError("No such entity: " + j.get<std::string>() +
                        " (entities are regenerated; list_entities shows current handles)");
    }
    // Geometry of groups after the active one is hidden, and can't be used
    // by the active group: constraints or groups referring to it would be
    // removed when the model is regenerated.
    Group *g = SK.group.FindByIdNoOops(e->group);
    if(g && g->order > SK.GetGroup(SS.GW.activeGroup)->order) {
        throw ToolError(H(h) + " belongs to group " + H(e->group) + " ('" + g->name +
                        "'), which comes after the active group; it can't be used "
                        "from the active group. Activate a later group first.");
    }
    return h;
}
hRequest ParseRequest(const json &j) {
    hRequest h = { ParseHandle(j, "r:", "request") };
    if(!SK.request.FindByIdNoOops(h)) throw ToolError("No such request: " + j.get<std::string>());
    return h;
}
hConstraint ParseConstraint(const json &j) {
    hConstraint h = { ParseHandle(j, "c:", "constraint") };
    if(!SK.constraint.FindByIdNoOops(h)) {
        throw ToolError("No such constraint: " + j.get<std::string>());
    }
    return h;
}

Handles ParseHandles(const json &j) {
    if(!j.is_array()) throw ToolError("Expected an array of handles");
    Handles h;
    for(const json &item : j) {
        if(!item.is_string()) throw ToolError("Handles must be strings");
        std::string s = item;
        if(s.compare(0, 2, "e:") == 0)      h.entities.push_back(ParseEntity(item));
        else if(s.compare(0, 2, "c:") == 0) h.constraints.push_back(ParseConstraint(item));
        else if(s.compare(0, 2, "r:") == 0) h.requests.push_back(ParseRequest(item));
        else if(s.compare(0, 2, "g:") == 0) h.groups.push_back(ParseGroup(item));
        else throw ToolError("Unknown handle type: '" + s + "'");
    }
    return h;
}

//-----------------------------------------------------------------------------
// Arguments
//-----------------------------------------------------------------------------
double Num(const json &j, const char *what) {
    if(!j.is_number()) throw ToolError(std::string(what) + " must be a number");
    return j.get<double>();
}

void Point2(const json &j, const char *what, double *u, double *v) {
    if(!j.is_array() || j.size() != 2) {
        throw ToolError(std::string(what) + " must be an array [u, v]");
    }
    *u = Num(j[0], what);
    *v = Num(j[1], what);
}

Vector Vec3(const json &j, const char *what) {
    if(!j.is_array() || j.size() != 3) {
        throw ToolError(std::string(what) + " must be an array [x, y, z]");
    }
    return Vector::From(Num(j[0], what), Num(j[1], what), Num(j[2], what));
}

json Vec(Vector v) { return json::array({v.x, v.y, v.z}); }

Frame FrameOf(hEntity wrkpl) {
    Entity *wp = SK.GetEntity(wrkpl);
    EntityBase *normal = wp->Normal();
    Frame f;
    f.origin = wp->WorkplaneGetOffset();
    f.u = normal->NormalU();
    f.v = normal->NormalV();
    f.n = normal->NormalN();
    return f;
}

Vector SketchPoint(const json &j, const char *what) {
    if(SS.GW.LockedInWorkplane()) {
        double u, v;
        if(j.is_array() && j.size() == 3) {
            throw ToolError(std::string(what) + ": the active group sketches in a workplane, "
                            "so give 2D workplane coordinates [u, v]");
        }
        Point2(j, what, &u, &v);
        return FrameOf(SS.GW.ActiveWorkplane()).To3d(u, v);
    } else {
        if(j.is_array() && j.size() == 2) {
            throw ToolError(std::string(what) + ": the active group sketches in 3D, "
                            "so give 3D coordinates [x, y, z]");
        }
        return Vec3(j, what);
    }
}

//-----------------------------------------------------------------------------
// Names
//-----------------------------------------------------------------------------
const char *GroupTypeName(Group::Type t) {
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

const char *EntityTypeName(Entity::Type t) {
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

std::string ConstraintTypeName(Constraint::Type t) {
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
        case Constraint::Type::EQ_LEN_PT_LINE_D:   return "equal_length_point_line_distance";
        case Constraint::Type::EQ_PT_LN_DISTANCES: return "equal_point_line_distances";
        case Constraint::Type::EQUAL_ANGLE:        return "equal_angle";
        case Constraint::Type::EQUAL_LINE_ARC_LEN: return "equal_line_arc_length";
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
        case Constraint::Type::CUBIC_LINE_TANGENT: return "cubic_line_tangent";
        case Constraint::Type::CURVE_CURVE_TANGENT:return "curve_curve_tangent";
        case Constraint::Type::EQUAL_RADIUS:       return "equal_radius";
        case Constraint::Type::WHERE_DRAGGED:      return "where_dragged";
        case Constraint::Type::COMMENT:            return "comment";
        case Constraint::Type::RELATION:           return "relation";
        default: return "type_" + std::to_string((uint32_t)t);
    }
}

static const char *CombineName(Group::CombineAs c) {
    switch(c) {
        case Group::CombineAs::UNION:        return "union";
        case Group::CombineAs::DIFFERENCE:   return "difference";
        case Group::CombineAs::ASSEMBLE:     return "assemble";
        case Group::CombineAs::INTERSECTION: return "intersection";
    }
    return "unknown";
}

static const char *SubtypeName(Group::Subtype s) {
    switch(s) {
        case Group::Subtype::ONE_SIDED:  return "one_sided";
        case Group::Subtype::TWO_SIDED:  return "two_sided";
        case Group::Subtype::ONE_SKEWED: return "one_sided_skewed";
        case Group::Subtype::TWO_SKEWED: return "two_sided_skewed";
        default: return NULL;
    }
}

//-----------------------------------------------------------------------------
// Results
//-----------------------------------------------------------------------------
json SolveStatus(hGroup hg) {
    Group *g = SK.GetGroup(hg);
    json j = {{"status", SolveResultName(g->solved.how)}, {"dof", g->solved.dof}};
    if(g->solved.how != SolveResult::OKAY && !g->solved.remove.IsEmpty()) {
        json bad = json::array();
        for(hConstraint hc : g->solved.remove) bad.push_back(H(hc));
        j["problem_constraints"] = bad;
    }
    return j;
}

static double Deg(double rad) { return rad * 180.0 / PI; }

json GroupJson(Group *g) {
    hGroup hg = g->h;
    int requests = 0, constraints = 0;
    for(Request &r : SK.request)       if(r.group == hg) requests++;
    for(Constraint &c : SK.constraint) if(c.group == hg) constraints++;
    json j = {
        {"handle", H(hg)}, {"name", g->name}, {"type", GroupTypeName(g->type)},
        {"visible", g->visible}, {"active", hg == SS.GW.activeGroup},
        {"requests", requests}, {"constraints", constraints},
    };
    if(hg == Group::HGROUP_REFERENCES) {
        j["note"] = "Built-in reference group: the origin and the XY, YZ and ZX planes.";
        return j;
    }
    j["solve"] = SolveStatus(hg);
    if(g->opA.v) j["source_group"] = H(g->opA);
    if(g->activeWorkplane.v && SK.entity.FindByIdNoOops(g->activeWorkplane)) {
        j["workplane"] = H(g->activeWorkplane);
    }
    if(g->suppress)         j["suppressed"] = true;
    if(g->relaxConstraints) j["relax_constraints"] = true;
    if(g->allowRedundant)   j["allow_redundant"] = true;
    if(g->IsMeshGroup() || g->type == Group::Type::DRAWING_WORKPLANE) {
        j["combine"] = CombineName(g->meshCombine);
        j["color"]   = {g->color.redF(), g->color.greenF(), g->color.blueF()};
        j["opacity"] = g->color.alphaF();
    }
    const char *sub = SubtypeName(g->subtype);
    switch(g->type) {
        case Group::Type::EXTRUDE: {
            Vector v = g->ExtrusionGetVector();
            if(sub) j["sides"] = sub;
            // The top copy is translated by 2*vector (one-sided); a two-sided
            // extrusion goes from -vector to +vector. Either way the total
            // length is 2*|vector|.
            j["depth"] = 2 * v.Magnitude();
            j["direction"] = Vec(v.WithMagnitude(1));
            break;
        }
        case Group::Type::REVOLVE:
        case Group::Type::HELIX: {
            if(sub) j["sides"] = sub;
            double theta = SK.GetParam(hg.param(3))->val;
            j["angle_deg"] = Deg(4 * theta);
            j["axis_point"] = Vec(SK.VectorFrom(hg.param(0), hg.param(1), hg.param(2)));
            j["axis_direction"] = Vec(SK.VectorFrom(hg.param(4), hg.param(5), hg.param(6)));
            if(g->type == Group::Type::HELIX) {
                j["axial_travel"] = 2 * SK.GetParam(hg.param(7))->val;
                if(g->valB != 0.0) j["pitch"] = g->valB;
            }
            break;
        }
        case Group::Type::ROTATE: {
            if(sub) j["sides"] = sub;
            j["count"] = (int)g->valA;
            j["angle_step_deg"] = Deg(4 * SK.GetParam(hg.param(3))->val);
            j["axis_point"] = Vec(SK.VectorFrom(hg.param(0), hg.param(1), hg.param(2)));
            j["axis_direction"] = Vec(SK.VectorFrom(hg.param(4), hg.param(5), hg.param(6)));
            j["skip_first"] = g->skipFirst;
            break;
        }
        case Group::Type::TRANSLATE:
            if(sub) j["sides"] = sub;
            j["count"] = (int)g->valA;
            j["step"] = Vec(SK.VectorFrom(hg.param(0), hg.param(1), hg.param(2)).ScaledBy(2));
            j["skip_first"] = g->skipFirst;
            break;
        case Group::Type::LINKED:
            j["file"] = g->linkFile.raw;
            break;
        default:
            break;
    }
    return j;
}

json ActiveWorkplaneJson() {
    if(!SS.GW.LockedInWorkplane()) return json();
    hEntity wp = SS.GW.ActiveWorkplane();
    Frame f = FrameOf(wp);
    return {{"handle", H(wp)}, {"origin", Vec(f.origin)},
            {"u_axis", Vec(f.u)}, {"v_axis", Vec(f.v)}, {"normal", Vec(f.n)}};
}

json EntityJson(Entity *e) {
    json j;
    j["handle"] = H(e->h);
    j["type"]   = EntityTypeName(e->type);
    if(e->h.isFromRequest()) {
        j["request"] = H(e->h.request());
        Request *r = SK.request.FindByIdNoOops(e->h.request());
        if(r) j["construction"] = r->construction;
    } else if(e->construction) {
        j["construction"] = true;
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
    } else if(e->IsDistance()) {
        j["value"] = e->DistanceGetNum();
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
            case Entity::Type::ARC_OF_CIRCLE: {
                double thetaa, thetab, dtheta;
                e->ArcGetAngles(&thetaa, &thetab, &dtheta);
                j["center"] = PointJson(e->point[0]);
                j["start"]  = PointJson(e->point[1]);
                j["end"]    = PointJson(e->point[2]);
                j["radius"] = e->CircleGetRadiusNum();
                j["sweep_deg"] = Deg(dtheta);
                j["normal"] = Vec(e->Normal()->NormalN());
                break;
            }
            case Entity::Type::CUBIC:
            case Entity::Type::CUBIC_PERIODIC: {
                int n = (e->type == Entity::Type::CUBIC ? 4 : 3) + e->extraPoints;
                json pts = json::array();
                for(int i = 0; i < n; i++) pts.push_back(PointJson(e->point[i]));
                j["control_points"] = pts;
                break;
            }
            case Entity::Type::TTF_TEXT:
            case Entity::Type::IMAGE: {
                if(e->type == Entity::Type::TTF_TEXT) {
                    j["text"] = e->str;
                    j["font"] = e->font;
                } else {
                    j["file"] = e->file.raw;
                }
                // point[1] is the bottom-left (origin), point[0] the top-left.
                j["corners"] = {
                    {"bottom_left",  PointJson(e->point[1])},
                    {"top_left",     PointJson(e->point[0])},
                    {"bottom_right", PointJson(e->point[2])},
                    {"top_right",    PointJson(e->point[3])},
                };
                break;
            }
            default:
                break;
        }
    }
    return j;
}

json ConstraintJson(Constraint *c) {
    json j = {{"handle", H(c->h)}, {"type", ConstraintTypeName(c->type)}};
    if(c->ptA.v)     j["ptA"]     = H(c->ptA);
    if(c->ptB.v)     j["ptB"]     = H(c->ptB);
    if(c->entityA.v) j["entityA"] = H(c->entityA);
    if(c->entityB.v) j["entityB"] = H(c->entityB);
    if(c->entityC.v) j["entityC"] = H(c->entityC);
    if(c->entityD.v) j["entityD"] = H(c->entityD);
    if(c->HasLabel()) {
        double v = c->valA;
        if(c->type == Constraint::Type::DIAMETER && c->other) {
            j["shown_as"] = "radius";
        }
        j["value"]     = v;
        j["reference"] = c->reference;
        if(!c->expression.empty()) j["expression"] = c->expression;
    }
    if(c->type == Constraint::Type::RELATION) j["expression"] = c->expression;
    if(!c->comment.empty()) j["comment"] = c->comment;
    return j;
}

json RequestJson(hRequest hr) {
    Request *r = SK.GetRequest(hr);
    json ents = json::array();
    for(Entity &e : SK.entity) {
        if(!e.h.isFromRequest() || e.h.request() != hr) continue;
        if(e.IsNormal() || e.IsDistance()) continue;
        ents.push_back(EntityJson(&e));
    }
    return {{"request", H(hr)}, {"type", r->DescriptionString()},
            {"construction", r->construction}, {"entities", ents}};
}

json IntProp(int lo, int hi, int dflt, const char *desc) {
    return {{"type", "integer"}, {"minimum", lo}, {"maximum", hi},
            {"default", dflt}, {"description", desc}};
}
json BoolProp(bool dflt, const char *desc) {
    return {{"type", "boolean"}, {"default", dflt}, {"description", desc}};
}
json StrProp(const char *desc) {
    return {{"type", "string"}, {"description", desc}};
}
json NumProp(const char *desc) {
    return {{"type", "number"}, {"description", desc}};
}
json Point2Schema(const char *desc) {
    return {{"type", "array"}, {"items", {{"type", "number"}}},
            {"minItems", 2}, {"maxItems", 2}, {"description", desc}};
}
json PointSchema(const char *desc) {
    return {{"type", "array"}, {"items", {{"type", "number"}}},
            {"minItems", 2}, {"maxItems", 3}, {"description", desc}};
}
json Vec3Schema(const char *desc) {
    return {{"type", "array"}, {"items", {{"type", "number"}}},
            {"minItems", 3}, {"maxItems", 3}, {"description", desc}};
}
json HandleListSchema(const char *desc) {
    return {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", desc}};
}

//-----------------------------------------------------------------------------
// Model changes
//-----------------------------------------------------------------------------
void Regenerate() {
    SS.GenerateAll(SolveSpaceUI::Generate::DIRTY);
    SS.GW.persistentDirty = true;
}

void RegenerateFor(hGroup hg) {
    // Generate::DIRTY only solves up to the active group.
    if(SK.GetGroup(hg)->order > SK.GetGroup(SS.GW.activeGroup)->order) {
        SS.GenerateAll(SolveSpaceUI::Generate::ALL);
        SS.GW.persistentDirty = true;
    } else {
        Regenerate();
    }
}

static int UndoPushesSince(int writeBefore) {
    return (SS.undo.write - writeBefore + SolveSpaceUI::MAX_UNDO) % SolveSpaceUI::MAX_UNDO;
}

Transaction::Transaction() : committed(false) {
    undoDepth = SS.undo.write;
    wasUnsaved = SS.unsaved;
    SS.UndoRemember();
}

Transaction::~Transaction() {
    int pushes = UndoPushesSince(undoDepth);
    if(committed) {
        // Collapse any extra undo steps recorded by GUI functions we called
        // into the first one, so the whole tool call is a single undo step.
        while(pushes > 1 && SS.undo.cnt > 1) {
            SS.undo.write = (SS.undo.write - 1 + SolveSpaceUI::MAX_UNDO) %
                            SolveSpaceUI::MAX_UNDO;
            SS.UndoClearState(&SS.undo.d[SS.undo.write]);
            SS.undo.cnt--;
            pushes--;
        }
    } else {
        // Roll back to the state before the tool.
        for(int i = 0; i < pushes && SS.undo.cnt > 0; i++) {
            SS.UndoUndo();
        }
        SS.UndoClearStack(&SS.redo);
        SS.UndoEnableMenus();
        SS.GW.ClearSelection();
        SS.unsaved = wasUnsaved;
        Regenerate();
    }
}

void SelectOnly(const Handles &h) {
    SS.GW.ClearSelection();
    for(hEntity he : h.entities)       SS.GW.MakeSelected(he);
    for(hConstraint hc : h.constraints) SS.GW.MakeSelected(hc);
}

void ThrowIfErrors() {
    if(activeMessageCapture == NULL || activeMessageCapture->errors.empty()) return;
    std::string text;
    for(const std::string &e : activeMessageCapture->errors) {
        if(!text.empty()) text += "\n";
        text += e;
    }
    activeMessageCapture->errors.clear();
    throw ToolError(text);
}

void RequireSketchGroup() {
    Group *g = SK.GetGroup(SS.GW.activeGroup);
    if(g->type != Group::Type::DRAWING_WORKPLANE && g->type != Group::Type::DRAWING_3D) {
        throw ToolError("The active group '" + g->name + "' is a " + GroupTypeName(g->type) +
                        " group. Geometry drawn there is construction-only; create or "
                        "activate a sketch group (create_workplane_sketch / "
                        "create_3d_sketch / activate_group) first.");
    }
}

void RequireWorkplane(const char *what) {
    if(!SS.GW.LockedInWorkplane()) {
        throw ToolError(std::string(what) + " can only be drawn in a workplane, but the "
                        "active group sketches in 3D. Use create_workplane_sketch first.");
    }
}

//-----------------------------------------------------------------------------
// Registry
//-----------------------------------------------------------------------------
const std::vector<Tool> &GetTools() {
    static std::vector<Tool> tools;
    if(tools.empty()) {
        AddModelTools(&tools);
        AddDocumentTools(&tools);
        AddSketchTools(&tools);
        AddConstraintTools(&tools);
        AddOperationTools(&tools);
    }
    return tools;
}

} // namespace Mcp
} // namespace SolveSpace
