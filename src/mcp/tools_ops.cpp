//-----------------------------------------------------------------------------
// MCP tools: groups that operate on a sketch (extrude, lathe, revolve, helix,
// rotate, translate) and linked files.
//
// These drive Group::MenuGroup() through the selection, then set the new
// group's parameters numerically. Their parameters are scaled by how
// SolveSpace applies them (see Group::Generate): copy i of a translation is
// at i*params, and a rotation by axis-angle param theta applied i times
// rotates by 2*i*theta. The end copies of an extrude/revolve/helix are
// applied twice (one-sided: 0 and 2; two-sided: -1 and 1).
//-----------------------------------------------------------------------------
#include "mcptools.h"

namespace SolveSpace {
namespace Mcp {

static double Rad(double deg) { return deg * PI / 180.0; }

// Activates the sketch group given as `sketch`, if any, and checks that the
// active group can be the source of the operation.
static void UseSourceSketch(const json &args) {
    if(args.contains("sketch")) {
        hGroup hg = ParseGroup(args["sketch"]);
        if(hg == Group::HGROUP_REFERENCES) throw ToolError("Can't use #references here");
        if(hg != SS.GW.activeGroup) {
            TextWindow::ScreenActivateGroup(0, hg.v);
            Regenerate();
        }
    }
}

static Group::CombineAs ParseCombine(const json &args, Group::CombineAs dflt) {
    if(!args.contains("combine")) return dflt;
    std::string c = args["combine"];
    if(c == "union")        return Group::CombineAs::UNION;
    if(c == "difference")   return Group::CombineAs::DIFFERENCE;
    if(c == "intersection") return Group::CombineAs::INTERSECTION;
    if(c == "assemble")     return Group::CombineAs::ASSEMBLE;
    throw ToolError("combine must be union, difference, intersection or assemble");
}

static bool TwoSided(const json &args) {
    std::string s = args.value("sides", "one_sided");
    if(s == "one_sided") return false;
    if(s == "two_sided") return true;
    throw ToolError("sides must be one_sided or two_sided");
}

// Creates a group with Group::MenuGroup and the given selection; returns it.
static Group *MakeGroup(Command cmd, const Handles &sel) {
    hGroup before = SS.GW.activeGroup;
    SelectOnly(sel);
    Group::MenuGroup(cmd);
    SS.GW.ClearSelection();
    ThrowIfErrors();
    if(SS.GW.activeGroup == before) throw ToolError("The group was not created.");
    return SK.GetGroup(SS.GW.activeGroup);
}

static void NameGroup(Group *g, const json &args) {
    if(args.contains("name") && args["name"].is_string() &&
       !args["name"].get<std::string>().empty()) {
        g->name = args["name"].get<std::string>();
    }
}

// The selection for an axis: a line segment, or a point plus a line/normal.
static Handles AxisSelection(const json &args) {
    Handles sel;
    if(!args.contains("axis")) {
        throw ToolError("axis is required: a line segment, or with axis_point, a line "
                        "or normal giving the direction");
    }
    hEntity axis = ParseEntity(args["axis"]);
    Entity *e = SK.GetEntity(axis);
    if(args.contains("axis_point")) {
        hEntity pt = ParseEntity(args["axis_point"]);
        if(!SK.GetEntity(pt)->IsPoint()) throw ToolError(H(pt) + " isn't a point");
        sel.entities.push_back(pt);
    } else if(e->type != Entity::Type::LINE_SEGMENT) {
        throw ToolError("Without axis_point, axis must be a line segment");
    }
    sel.entities.push_back(axis);
    return sel;
}

static void Finish(Group *g, ToolResult *res) {
    Regenerate();
    g = SK.GetGroup(g->h);
    res->structured = GroupJson(g);
    if(g->runningMesh.IsEmpty() && g->runningShell.IsEmpty() && g->IsMeshGroup()) {
        res->structured["warning"] = "The group made no solid. Is the source sketch a "
                                     "closed profile of non-construction curves?";
    }
    Group *src = SK.group.FindByIdNoOops(g->opA);
    if(src && src->polyError.how != PolyError::GOOD) {
        res->structured["warning"] = "The source sketch is not a valid closed profile "
                                     "(open, self-intersecting or not coplanar).";
    }
}

//-----------------------------------------------------------------------------
// extrude
//-----------------------------------------------------------------------------
static void Extrude(const json &args, ToolResult *res) {
    UseSourceSketch(args);
    double depth = args.contains("depth") ? Num(args["depth"], "depth") : 10;
    if(fabs(depth) < LENGTH_EPS) throw ToolError("depth can't be zero");
    bool twoSided = TwoSided(args);
    Group::CombineAs combine = ParseCombine(args, Group::CombineAs::UNION);
    bool constrain = args.value("constrain_depth", true);

    Transaction t;
    Group *g = MakeGroup(Command::GROUP_EXTRUDE, Handles());
    hGroup hg = g->h;
    NameGroup(g, args);
    g->subtype = twoSided ? Group::Subtype::TWO_SIDED : Group::Subtype::ONE_SIDED;
    g->meshCombine = combine;
    Vector n = FrameOf(g->predef.entityB).n;
    // The end copy is translated by 2*vector (or from -vector to +vector).
    g->ExtrusionForceVectorTo(n.ScaledBy(depth / 2));
    SS.MarkGroupDirty(hg);
    Regenerate();

    if(constrain) {
        // Fix the depth with a distance constraint along one of the extrusion
        // edges (a line between a point's bottom and top copies), so the
        // group is fully constrained.
        for(Entity &e : SK.entity) {
            if(e.group != hg || e.type != Entity::Type::LINE_SEGMENT) continue;
            Vector a = SK.GetEntity(e.point[0])->PointGetNum(),
                   b = SK.GetEntity(e.point[1])->PointGetNum();
            Vector d = a.Minus(b);
            if(fabs(d.Magnitude() - fabs(depth)) > 1e-6) continue;
            if(fabs(fabs(d.WithMagnitude(1).Dot(n)) - 1) > 1e-9) continue;
            Constraint c = {};
            c.group     = hg;
            c.workplane = Entity::FREE_IN_3D;
            c.type      = Constraint::Type::PT_PT_DISTANCE;
            c.ptA       = e.point[0];
            c.ptB       = e.point[1];
            c.valA      = fabs(depth);
            Constraint::AddConstraint(&c, /*rememberForUndo=*/false);
            break;
        }
    }
    Finish(SK.GetGroup(hg), res);
    t.Commit();
}

//-----------------------------------------------------------------------------
// lathe / revolve / helix
//-----------------------------------------------------------------------------
static void Lathe(const json &args, ToolResult *res) {
    UseSourceSketch(args);
    Handles sel = AxisSelection(args);
    Group::CombineAs combine = ParseCombine(args, Group::CombineAs::UNION);
    Transaction t;
    Group *g = MakeGroup(Command::GROUP_LATHE, sel);
    NameGroup(g, args);
    g->meshCombine = combine;
    SS.MarkGroupDirty(g->h);
    Finish(g, res);
    t.Commit();
}

static void Revolve(const json &args, ToolResult *res) {
    UseSourceSketch(args);
    Handles sel = AxisSelection(args);
    double angle = args.contains("angle_deg") ? Num(args["angle_deg"], "angle_deg") : 90;
    if(fabs(angle) < 1e-6 || fabs(angle) >= 360) {
        throw ToolError("angle_deg must be non-zero and less than 360; use lathe for a full turn");
    }
    bool twoSided = TwoSided(args);
    Group::CombineAs combine = ParseCombine(args, Group::CombineAs::UNION);
    Transaction t;
    Group *g = MakeGroup(Command::GROUP_REVOLVE, sel);
    hGroup hg = g->h;
    NameGroup(g, args);
    g->subtype = twoSided ? Group::Subtype::TWO_SIDED : Group::Subtype::ONE_SIDED;
    g->meshCombine = combine;
    SS.MarkGroupDirty(hg);
    Regenerate();
    SK.GetParam(hg.param(3))->val = Rad(angle) / 4;
    SS.MarkGroupDirty(hg);
    Finish(SK.GetGroup(hg), res);
    t.Commit();
}

static void Helix(const json &args, ToolResult *res) {
    UseSourceSketch(args);
    Handles sel = AxisSelection(args);
    double angle = args.contains("angle_deg") ? Num(args["angle_deg"], "angle_deg") : 360;
    if(fabs(angle) < 1e-6) throw ToolError("angle_deg can't be zero");
    if(args.contains("pitch") && args.contains("axial_travel")) {
        throw ToolError("Give pitch or axial_travel, not both");
    }
    bool twoSided = TwoSided(args);
    Group::CombineAs combine = ParseCombine(args, Group::CombineAs::UNION);
    Transaction t;
    Group *g = MakeGroup(Command::GROUP_HELIX, sel);
    hGroup hg = g->h;
    NameGroup(g, args);
    g->subtype = twoSided ? Group::Subtype::TWO_SIDED : Group::Subtype::ONE_SIDED;
    g->meshCombine = combine;
    SS.MarkGroupDirty(hg);
    Regenerate();
    g = SK.GetGroup(hg);
    double theta = Rad(angle) / 4;
    SK.GetParam(hg.param(3))->val = theta;
    if(args.contains("pitch")) {
        double pitch = Num(args["pitch"], "pitch");
        // The group's equation: param7 * pi = param3 * pitch.
        g->valB = pitch;
        SK.GetParam(hg.param(7))->val = theta * pitch / PI;
    } else {
        double travel = args.contains("axial_travel")
            ? Num(args["axial_travel"], "axial_travel") : 20;
        g->valB = 0;
        SK.GetParam(hg.param(7))->val = travel / 2;
    }
    SS.MarkGroupDirty(hg);
    Finish(SK.GetGroup(hg), res);
    t.Commit();
}

//-----------------------------------------------------------------------------
// rotate_copy / translate_copy
//-----------------------------------------------------------------------------
static void RotateCopy(const json &args, ToolResult *res) {
    UseSourceSketch(args);
    Handles sel;
    if(!args.contains("center")) throw ToolError("center (a point entity) is required");
    hEntity center = ParseEntity(args["center"]);
    if(!SK.GetEntity(center)->IsPoint()) throw ToolError(H(center) + " isn't a point");
    sel.entities.push_back(center);
    if(args.contains("axis")) sel.entities.push_back(ParseEntity(args["axis"]));
    int count = args.value("count", 3);
    if(count < 1 || count > 999) throw ToolError("count must be between 1 and 999");
    bool twoSided = TwoSided(args);
    bool skipFirst = args.value("skip_first", false);

    Transaction t;
    Group *g = MakeGroup(Command::GROUP_ROT, sel);
    hGroup hg = g->h;
    NameGroup(g, args);
    g->valA = count;
    g->subtype = twoSided ? Group::Subtype::TWO_SIDED : Group::Subtype::ONE_SIDED;
    g->skipFirst = skipFirst;
    SS.MarkGroupDirty(hg);
    Regenerate();
    double step;
    if(args.contains("angle_deg")) {
        step = Rad(Num(args["angle_deg"], "angle_deg"));
    } else {
        // Evenly around a full turn, as the GUI does.
        int copies = skipFirst ? count + 1 : count;
        step = 2 * PI / copies;
    }
    // Copy i is rotated by i * 2 * (2 * param3)... see the file comment; the
    // copies are applied with timesApplied = 2*i.
    SK.GetParam(hg.param(3))->val = step / 4;
    SS.MarkGroupDirty(hg);
    Finish(SK.GetGroup(hg), res);
    t.Commit();
}

static void TranslateCopy(const json &args, ToolResult *res) {
    UseSourceSketch(args);
    if(!args.contains("step")) throw ToolError("step [x, y, z] is required");
    Vector step = Vec3(args["step"], "step");
    if(step.Magnitude() < LENGTH_EPS) throw ToolError("step can't be zero");
    int count = args.value("count", 3);
    if(count < 1 || count > 999) throw ToolError("count must be between 1 and 999");
    if(SS.GW.LockedInWorkplane()) {
        Vector n = FrameOf(SS.GW.ActiveWorkplane()).n;
        if(fabs(step.Dot(n)) > 1e-6) {
            throw ToolError("While a workplane is active, step must lie in the workplane. "
                            "To translate out of the plane, activate a solid group (e.g. an "
                            "extrude) first.");
        }
    }
    bool twoSided = TwoSided(args);
    bool skipFirst = args.value("skip_first", false);

    Transaction t;
    Group *g = MakeGroup(Command::GROUP_TRANS, Handles());
    hGroup hg = g->h;
    NameGroup(g, args);
    g->valA = count;
    g->subtype = twoSided ? Group::Subtype::TWO_SIDED : Group::Subtype::ONE_SIDED;
    g->skipFirst = skipFirst;
    SS.MarkGroupDirty(hg);
    Regenerate();
    // Copy i is translated by 2*i*params (one-sided).
    Vector half = step.ScaledBy(0.5);
    SK.GetParam(hg.param(0))->val = half.x;
    SK.GetParam(hg.param(1))->val = half.y;
    SK.GetParam(hg.param(2))->val = half.z;
    SS.MarkGroupDirty(hg);
    Finish(SK.GetGroup(hg), res);
    t.Commit();
}

//-----------------------------------------------------------------------------
// link_file
//-----------------------------------------------------------------------------
static void LinkFile(const json &args, ToolResult *res) {
    std::string file = args.value("path", "");
    Platform::Path path = Platform::Path::From(file);
    if(file.empty() || !path.IsAbsolute()) throw ToolError("path must be an absolute path");
    FILE *f = OpenFile(path, "rb");
    if(!f) throw ToolError("Can't open '" + file + "'");
    fclose(f);

    hGroup before = SS.GW.activeGroup;
    Transaction t;
    SS.GW.ClearSelection();
    Group::MenuGroup(Command::GROUP_LINK, path);
    ThrowIfErrors();
    if(SS.GW.activeGroup == before) throw ToolError("The file was not linked.");
    Group *g = SK.GetGroup(SS.GW.activeGroup);
    NameGroup(g, args);
    Finish(g, res);
    t.Commit();
}

void AddOperationTools(std::vector<Tool> *tools) {
    json sketch = StrProp("Source sketch group; default the active group. It is activated.");
    json combine = {{"type", "string"},
                    {"enum", {"union", "difference", "intersection", "assemble"}},
                    {"description", "How the solid combines with earlier solids "
                                    "(default union; difference cuts)."}};
    json sides = {{"type", "string"}, {"enum", {"one_sided", "two_sided"}},
                  {"default", "one_sided"},
                  {"description", "two_sided: symmetric about the sketch."}};
    json name = StrProp("Optional group name.");
    std::string axisDesc =
        " The axis is a line segment (axis), or a point (axis_point) plus a line or "
        "normal (axis) giving the direction. A construction line in the sketch makes a "
        "good axis.";

    tools->push_back({
        "extrude",
        "Extrude the active (or given) workplane sketch's closed profiles normal to "
        "its workplane into a solid, as a new group. depth in mm; negative extrudes "
        "the other way. By default the depth is fixed by a distance constraint, so "
        "the group is fully constrained (change it with set_constraint_value).",
        {{"type", "object"}, {"properties", {
            {"sketch", sketch},
            {"depth", NumProp("Depth in mm (default 10).")},
            {"sides", sides},
            {"combine", combine},
            {"constrain_depth", BoolProp(true, "Add a distance constraint for the depth.")},
            {"name", name},
        }}},
        Extrude,
    });
    tools->push_back({
        "lathe",
        "Revolve the active (or given) workplane sketch a full turn about an axis "
        "into a solid, as a new group." + axisDesc,
        {{"type", "object"}, {"properties", {
            {"sketch", sketch},
            {"axis", StrProp("Line segment, or with axis_point a line/normal.")},
            {"axis_point", StrProp("Point the axis passes through.")},
            {"combine", combine},
            {"name", name},
        }}, {"required", {"axis"}}},
        Lathe,
    });
    tools->push_back({
        "revolve",
        "Revolve the active (or given) workplane sketch part of a turn about an axis "
        "into a solid, as a new group." + axisDesc,
        {{"type", "object"}, {"properties", {
            {"sketch", sketch},
            {"axis", StrProp("Line segment, or with axis_point a line/normal.")},
            {"axis_point", StrProp("Point the axis passes through.")},
            {"angle_deg", NumProp("Angle in degrees (default 90).")},
            {"sides", sides},
            {"combine", combine},
            {"name", name},
        }}, {"required", {"axis"}}},
        Revolve,
    });
    tools->push_back({
        "helix",
        "Sweep the active (or given) workplane sketch along a helix about an axis, "
        "as a new group: it turns angle_deg while moving along the axis, set by "
        "pitch (travel per full turn) or axial_travel (total)." + axisDesc,
        {{"type", "object"}, {"properties", {
            {"sketch", sketch},
            {"axis", StrProp("Line segment, or with axis_point a line/normal.")},
            {"axis_point", StrProp("Point the axis passes through.")},
            {"angle_deg", NumProp("Total turn in degrees (default 360).")},
            {"pitch", NumProp("Axial travel per full turn, mm.")},
            {"axial_travel", NumProp("Total axial travel, mm (default 20).")},
            {"sides", sides},
            {"combine", combine},
            {"name", name},
        }}, {"required", {"axis"}}},
        Helix,
    });
    tools->push_back({
        "rotate_copy",
        "Make rotated copies of the active (or given) group (a sketch or a solid), as "
        "a new 'rotate' group (step and repeat). In a workplane sketch, center alone "
        "rotates in the plane; otherwise give axis (a line or normal) too. "
        "angle_deg is the step between copies (default: evenly around a full turn). "
        "The copies combine like the source group.",
        {{"type", "object"}, {"properties", {
            {"sketch", StrProp("Source group; default the active group.")},
            {"center", StrProp("Point entity at the centre of rotation.")},
            {"axis", StrProp("Line or normal entity giving the axis direction.")},
            {"count", IntProp(1, 999, 3, "Number of copies (including the original "
                                         "unless skip_first).")},
            {"angle_deg", NumProp("Angle between copies, degrees.")},
            {"sides", sides},
            {"skip_first", BoolProp(false, "Don't include the original position.")},
            {"name", name},
        }}, {"required", {"center"}}},
        RotateCopy,
    });
    tools->push_back({
        "translate_copy",
        "Make translated copies of the active (or given) group (a sketch or a solid), "
        "as a new 'translate' group (step and repeat). step is the offset between "
        "copies in mm; while a workplane sketch is active it must lie in the plane. "
        "The copies combine like the source group.",
        {{"type", "object"}, {"properties", {
            {"sketch", StrProp("Source group; default the active group.")},
            {"step", Vec3Schema("Offset between copies [x, y, z] mm.")},
            {"count", IntProp(1, 999, 3, "Number of copies (including the original "
                                         "unless skip_first).")},
            {"sides", sides},
            {"skip_first", BoolProp(false, "Don't include the original position.")},
            {"name", name},
        }}, {"required", {"step"}}},
        TranslateCopy,
    });
    tools->push_back({
        "link_file",
        "Insert another .slvs model as a linked group (for assemblies). It is "
        "combined as 'assemble' and can be positioned with constraints.",
        {{"type", "object"}, {"properties", {
            {"path", StrProp("Absolute path of the .slvs file.")},
            {"name", name},
        }}, {"required", {"path"}}},
        LinkFile,
    });
}

} // namespace Mcp
} // namespace SolveSpace
