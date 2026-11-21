//-----------------------------------------------------------------------------
// MCP tools: constraints, solve status, named parameters and relations.
//-----------------------------------------------------------------------------
#include "mcptools.h"

namespace SolveSpace {
namespace Mcp {

static std::set<uint32_t> ConstraintSet() {
    std::set<uint32_t> s;
    for(Constraint &c : SK.constraint) s.insert(c.h.v);
    return s;
}

// Runs `f` with the display units forced to mm, so that text typed into the
// GUI's edit handlers is read as mm (and angles as degrees).
template<class F>
static void WithMmUnits(F f) {
    Unit saved = SS.viewUnits;
    SS.viewUnits = Unit::MM;
    try {
        f();
    } catch(...) {
        SS.viewUnits = saved;
        throw;
    }
    SS.viewUnits = saved;
}

static std::string ValueText(const json &value) {
    if(value.is_number()) {
        // Fixed notation: the expression parser doesn't read exponents
        // like 5e-05.
        char buf[400];
        snprintf(buf, sizeof(buf), "%.12f", value.get<double>());
        std::string s = buf;
        size_t dot = s.find('.');
        if(dot != std::string::npos) {
            s.erase(s.find_last_not_of('0') + 1);
            if(s.back() == '.') s.pop_back();
        }
        return s;
    }
    if(value.is_string()) return value.get<std::string>();
    throw ToolError("value must be a number, or an expression string");
}

// Sets a dimension's value the way typing it into the GUI's label editor
// does: numbers in mm or degrees, or an expression using named parameters
// (undefined names become new parameters).
static void SetValue(hConstraint hc, const json &value) {
    Constraint *c = SK.GetConstraint(hc);
    if(c->type == Constraint::Type::COMMENT) {
        c->comment = ValueText(value);
        return;
    }
    if(!c->HasLabel() && c->type != Constraint::Type::RELATION) {
        throw ToolError(H(hc) + " (" + ConstraintTypeName(c->type) + ") has no value");
    }
    std::string text = ValueText(value);
    // A diameter shown as a radius reads its text as a radius; we always take
    // the diameter.
    bool radius = (c->type == Constraint::Type::DIAMETER && c->other);
    if(radius) c->other = false;
    WithMmUnits([&] {
        SS.GW.constraintBeingEdited = hc;
        SS.GW.EditControlDone(text);
    });
    if(radius) SK.GetConstraint(hc)->other = true;
    ThrowIfErrors();
}

// After a change, fail (rolling back) if a sketch that solved before no
// longer does.
static void CheckStillSolves(hGroup hg, bool solvedBefore) {
    Group *g = SK.GetGroup(hg);
    if(!solvedBefore || g->solved.how == SolveResult::OKAY) return;
    json status = SolveStatus(hg);
    std::string msg = "The sketch no longer solves (" + status["status"].get<std::string>() + ")";
    if(status.contains("problem_constraints")) {
        msg += "; conflicting or redundant constraints: " + status["problem_constraints"].dump();
    }
    msg += ". The change was undone. Remove a conflicting constraint, or make the new "
           "dimension a reference (reference=true).";
    throw ToolError(msg);
}

static bool Solves(hGroup hg) {
    return SK.GetGroup(hg)->solved.how == SolveResult::OKAY;
}

//-----------------------------------------------------------------------------
// add_constraint
//-----------------------------------------------------------------------------
struct ConstraintKind {
    const char *name;
    Command     command;
    Command     refCommand;
    const char *usage;
};

static const ConstraintKind KINDS[] = {
    { "distance", Command::DISTANCE_DIA, Command::REF_DISTANCE,
      "2 points; a line segment (its length); a point and a line (point-line distance); "
      "a point and a workplane or face (point-plane distance); a line and a point with "
      "a workplane... (projected distance)" },
    { "diameter", Command::DISTANCE_DIA, Command::REF_DISTANCE,
      "a circle or arc (value is the diameter)" },
    { "angle", Command::ANGLE, Command::REF_ANGLE,
      "2 line segments (or normals); value in degrees" },
    { "coincident", Command::ON_ENTITY, Command::NONE,
      "2 points (coincident); a point and a line, circle, arc, cubic, workplane or face "
      "(point on entity)" },
    { "on_entity", Command::ON_ENTITY, Command::NONE,
      "same as coincident" },
    { "equal", Command::EQUAL, Command::NONE,
      "2 or more line segments (equal length); 2 circles/arcs (equal radius); a line "
      "and an arc (equal length); 4 lines (equal angle); a line and a point-line "
      "distance ..." },
    { "ratio", Command::RATIO, Command::NONE,
      "2 line segments or arcs (length ratio A/B)" },
    { "difference", Command::DIFFERENCE, Command::NONE,
      "2 line segments or arcs (length difference A-B)" },
    { "horizontal", Command::HORIZONTAL, Command::NONE,
      "a line segment; or 2 points (horizontal relative to each other). Needs a workplane." },
    { "vertical", Command::VERTICAL, Command::NONE,
      "a line segment; or 2 points. Needs a workplane." },
    { "parallel", Command::PARALLEL, Command::NONE,
      "2 line segments or normals; or a line and an arc/cubic sharing an endpoint "
      "(tangent)" },
    { "tangent", Command::PARALLEL, Command::NONE,
      "a line and an arc or cubic, or two curves, sharing an endpoint (select the "
      "shared point too if they share more than one)" },
    { "perpendicular", Command::PERPENDICULAR, Command::NONE,
      "2 line segments or normals" },
    { "symmetric", Command::SYMMETRIC, Command::NONE,
      "2 points and a line (symmetric about the line); 2 points (symmetric about "
      "the workplane's horizontal/vertical axis); a line segment's endpoints ..." },
    { "midpoint", Command::AT_MIDPOINT, Command::NONE,
      "a point and a line segment (point at the line's midpoint); a line and a workplane" },
    { "same_orientation", Command::ORIENTED_SAME, Command::NONE,
      "2 normals" },
    { "lock", Command::WHERE_DRAGGED, Command::NONE,
      "a point (lock it where it is)" },
};

static void AddConstraint(const json &args, ToolResult *res) {
    std::string type = args.value("type", "");
    const ConstraintKind *kind = NULL;
    for(const ConstraintKind &k : KINDS) {
        if(type == k.name) kind = &k;
    }
    if(kind == NULL) throw ToolError("Unknown constraint type '" + type + "'");
    bool reference = args.value("reference", false);
    if(reference && kind->refCommand == Command::NONE) {
        throw ToolError("Only distance, diameter and angle constraints can be references");
    }
    Handles sel;
    if(args.contains("points")) {
        for(const json &j : args["points"]) {
            hEntity he = ParseEntity(j);
            if(!SK.GetEntity(he)->IsPoint()) throw ToolError(H(he) + " isn't a point");
            sel.entities.push_back(he);
        }
    }
    if(args.contains("entities")) {
        for(const json &j : args["entities"]) sel.entities.push_back(ParseEntity(j));
    }
    if(args.value("use_selection", false)) {
        if(!sel.entities.empty()) {
            throw ToolError("Give points/entities or use_selection, not both");
        }
        // What the user has selected in the GUI (see get_selection).
        for(GraphicsWindow::Selection &s : SS.GW.selection) {
            if(s.entity.v) sel.entities.push_back(s.entity);
        }
        if(sel.entities.empty()) {
            throw ToolError("Nothing is selected in SolveSpace (selected constraints "
                            "don't count)");
        }
    }
    if(sel.entities.empty()) throw ToolError("Give the points and/or entities to constrain");

    hGroup hg = SS.GW.activeGroup;
    bool solvedBefore = Solves(hg);
    std::set<uint32_t> before = ConstraintSet();
    bool savedImmediate = SS.immediatelyEditDimension;
    SS.immediatelyEditDimension = false;

    Transaction t;
    SelectOnly(sel);
    Constraint::MenuConstrain(reference ? kind->refCommand : kind->command);
    SS.immediatelyEditDimension = savedImmediate;
    ThrowIfErrors();

    std::vector<hConstraint> created;
    for(Constraint &c : SK.constraint) {
        if(before.count(c.h.v) == 0) created.push_back(c.h);
    }
    if(created.empty()) {
        std::string msg = "No constraint was added.";
        for(GraphicsWindow::Selection &s : SS.GW.selection) {
            if(s.constraint.v) {
                msg = "That constraint already exists: " + H(s.constraint) + ".";
            }
        }
        SS.GW.ClearSelection();
        throw ToolError(msg);
    }
    SS.GW.ClearSelection();

    if(args.contains("supplementary") && args["supplementary"].get<bool>()) {
        for(hConstraint hc : created) {
            Constraint *c = SK.GetConstraint(hc);
            if(c->type != Constraint::Type::ANGLE) continue;
            // As Constrain > Other Supplementary Angle does.
            c->other = !c->other;
            c->ModifyToSatisfy(&SK);
            SS.MarkGroupDirty(c->group);
        }
    }
    if(args.contains("value")) {
        int labeled = 0;
        for(hConstraint hc : created) {
            if(SK.GetConstraint(hc)->HasLabel()) labeled++;
        }
        if(labeled != 1) {
            throw ToolError("value given, but this constraint type has no value");
        }
        for(hConstraint hc : created) {
            if(SK.GetConstraint(hc)->HasLabel()) SetValue(hc, args["value"]);
        }
    }
    Regenerate();
    CheckStillSolves(hg, solvedBefore);
    t.Commit();

    json cs = json::array();
    bool madeReference = false;
    for(hConstraint hc : created) {
        Constraint *c = SK.GetConstraint(hc);
        if(c->reference && !reference) madeReference = true;
        cs.push_back(ConstraintJson(c));
    }
    res->structured = {{"constraints", cs}, {"solve", SolveStatus(hg)}};
    if(madeReference) {
        res->structured["note"] = "The dimension would have been redundant, so SolveSpace "
                                  "made it a reference dimension (it measures, but doesn't "
                                  "constrain).";
    }
}

//-----------------------------------------------------------------------------
// set_constraint_value / set_constraint_reference / list_constraints
//-----------------------------------------------------------------------------
static void SetConstraintValue(const json &args, ToolResult *res) {
    hConstraint hc = ParseConstraint(args.value("constraint", json()));
    if(!args.contains("value")) throw ToolError("value is required");
    hGroup hg = SK.GetConstraint(hc)->group;
    bool solvedBefore = Solves(hg);
    Transaction t;
    SetValue(hc, args["value"]);
    RegenerateFor(hg);
    CheckStillSolves(hg, solvedBefore);
    t.Commit();
    res->structured = {{"constraint", ConstraintJson(SK.GetConstraint(hc))},
                       {"solve", SolveStatus(hg)}};
}

static void SetConstraintReference(const json &args, ToolResult *res) {
    hConstraint hc = ParseConstraint(args.value("constraint", json()));
    bool reference = args.value("reference", true);
    Constraint *c = SK.GetConstraint(hc);
    if(!c->HasLabel()) throw ToolError("Only dimensions can be reference dimensions");
    hGroup hg = c->group;
    bool solvedBefore = Solves(hg);
    Transaction t;
    c->reference = reference;
    SS.MarkGroupDirty(hg);
    SK.GetGroup(hg)->dofCheckOk = false;
    RegenerateFor(hg);
    CheckStillSolves(hg, solvedBefore);
    t.Commit();
    res->structured = {{"constraint", ConstraintJson(SK.GetConstraint(hc))},
                       {"solve", SolveStatus(hg)}};
}

static void ListConstraints(const json &args, ToolResult *res) {
    hGroup hg = args.contains("group") ? ParseGroup(args["group"]) : SS.GW.activeGroup;
    json cs = json::array();
    for(Constraint &c : SK.constraint) {
        if(c.group == hg) cs.push_back(ConstraintJson(&c));
    }
    res->structured = {{"group", H(hg)}, {"constraints", cs}};
    if(hg != Group::HGROUP_REFERENCES) res->structured["solve"] = SolveStatus(hg);
}

//-----------------------------------------------------------------------------
// get_solve_status
//-----------------------------------------------------------------------------
static void GetSolveStatus(const json &args, ToolResult *res) {
    hGroup hg = args.contains("group") ? ParseGroup(args["group"]) : SS.GW.activeGroup;
    if(hg != SS.GW.activeGroup) {
        throw ToolError("Free points can only be found for the active group; "
                        "activate it first, or use get_group for its status.");
    }
    // Like Analyze > Show Degrees of Freedom: mark the free parameters. The
    // group must be solved again for that, even if it's clean.
    SS.MarkGroupDirty(hg);
    SS.GenerateAll(SolveSpaceUI::Generate::DIRTY, /*andFindFree=*/true);
    json free = json::array();
    for(Entity &e : SK.entity) {
        if(e.group != hg || !e.IsPoint()) continue;
        bool isFree = false;
        for(int i = 0; i < 3; i++) {
            if(!e.param[i].v) continue;
            Param *p = SK.param.FindByIdNoOops(e.param[i]);
            if(p && p->free) isFree = true;
        }
        if(isFree) free.push_back(EntityJson(&e));
    }
    res->structured = SolveStatus(hg);
    res->structured["group"] = H(hg);
    res->structured["free_points"] = free;
    // Don't leave the free-point markers drawn in the GUI.
    for(Param &p : SK.param) p.free = false;
}

//-----------------------------------------------------------------------------
// add_comment
//-----------------------------------------------------------------------------
static void AddComment(const json &args, ToolResult *res) {
    std::string text = args.value("text", "");
    if(text.empty()) throw ToolError("text can't be empty");
    Vector at = args.contains("at") ? SketchPoint(args["at"], "at") : Vector::From(0, 0, 0);
    Transaction t;
    Constraint c = {};
    c.group       = SS.GW.activeGroup;
    c.workplane   = SS.GW.ActiveWorkplane();
    c.type        = Constraint::Type::COMMENT;
    c.disp.offset = at;
    c.comment     = text;
    hConstraint hc = Constraint::AddConstraint(&c, /*rememberForUndo=*/false);
    Regenerate();
    t.Commit();
    res->structured = {{"constraint", ConstraintJson(SK.GetConstraint(hc))}};
}

//-----------------------------------------------------------------------------
// Named parameters and relations
//-----------------------------------------------------------------------------
static json ParamJson(Request *r) {
    Param *p = SK.param.FindByIdNoOops(r->NamedParam());
    json j = {{"name", r->str}, {"request", H(r->h)}, {"group", H(r->group)},
              {"locked", r->type == Request::Type::NAMED_CONST_PARAM}};
    if(p) j["value"] = p->val;
    if(r->HasExpression()) j["expression"] = r->expression;
    return j;
}

static void ListParameters(const json &, ToolResult *res) {
    json ps = json::array();
    for(Request &r : SK.request) {
        if(r.IsNamedParam()) ps.push_back(ParamJson(&r));
    }
    res->structured = {{"parameters", ps}};
}

static void SetParameter(const json &args, ToolResult *res) {
    std::string name = args.value("name", "");
    if(name.empty()) throw ToolError("name is required");
    if(!args.contains("value")) throw ToolError("value is required");
    hGroup hg = SS.GW.activeGroup;
    bool solvedBefore = Solves(hg);

    Transaction t;
    Request *r = NULL;
    for(Request &rr : SK.request) {
        if(rr.IsNamedParam() && rr.str == name && rr.group == hg) r = &rr;
    }
    if(r == NULL) {
        for(Request &rr : SK.request) {
            if(rr.IsNamedParam() && rr.str == name) {
                throw ToolError("Parameter '" + name + "' belongs to group " + H(rr.group) +
                                "; activate that group to change it.");
            }
        }
        Constraint::CreateNamedParams(hg, {name}, Request::NAMED_PARAM_DEFAULT);
        Regenerate();
        for(Request &rr : SK.request) {
            if(rr.IsNamedParam() && rr.str == name && rr.group == hg) r = &rr;
        }
        if(r == NULL) {
            ThrowIfErrors();
            throw ToolError("Couldn't create parameter '" + name + "'");
        }
    }
    // As if the value was typed into the property browser.
    SS.TW.edit.meaning = TextWindow::Edit::PARAM_VALUE;
    SS.TW.edit.request = r->h;
    hRequest hr = r->h;
    SS.TW.EditControlDone(ValueText(args["value"]));
    ThrowIfErrors();
    SS.MarkGroupDirty(hg);
    Regenerate();
    CheckStillSolves(hg, solvedBefore);
    t.Commit();
    res->structured = {{"parameter", ParamJson(SK.GetRequest(hr))}, {"solve", SolveStatus(hg)}};
}

static void AddRelation(const json &args, ToolResult *res) {
    std::string expr = args.value("expression", "");
    if(expr.empty()) throw ToolError("expression is required");
    hGroup hg = SS.GW.activeGroup;
    bool solvedBefore = Solves(hg);
    Vector at = args.contains("at") ? SketchPoint(args["at"], "at") : Vector::From(0, 0, 0);
    Transaction t;
    Constraint c = {};
    c.group       = hg;
    c.workplane   = SS.GW.ActiveWorkplane();
    c.type        = Constraint::Type::RELATION;
    c.disp.offset = at;
    hConstraint hc = Constraint::AddConstraint(&c, /*rememberForUndo=*/false);
    SetValue(hc, expr);
    Regenerate();
    CheckStillSolves(hg, solvedBefore);
    t.Commit();
    res->structured = {{"constraint", ConstraintJson(SK.GetConstraint(hc))},
                       {"solve", SolveStatus(hg)}};
}

void AddConstraintTools(std::vector<Tool> *tools) {
    std::string desc =
        "Add a constraint between existing points/entities of the active group (and "
        "earlier groups' geometry, which stays fixed), like the Constrain menu. "
        "The constraint is placed in the active workplane, if any. For dimensions "
        "(distance, diameter, angle, ratio, difference) value sets the dimension: a "
        "number in mm or degrees, or an expression using named parameters (e.g. "
        "'width/2'; unknown names become new parameters). reference=true makes a "
        "reference dimension that only measures. If the sketch stops solving, the "
        "constraint is not added. Types and what to pass (points and entities are "
        "combined, in order):\n";
    for(const ConstraintKind &k : KINDS) {
        desc += std::string("- ") + k.name + ": " + k.usage + "\n";
    }
    std::vector<std::string> names;
    for(const ConstraintKind &k : KINDS) names.push_back(k.name);

    tools->push_back({
        "add_constraint", desc,
        {{"type", "object"}, {"properties", {
            {"type", {{"type", "string"}, {"enum", names}}},
            {"points", HandleListSchema("Point entity handles.")},
            {"entities", HandleListSchema("Other entity handles (lines, circles, ...).")},
            {"use_selection", BoolProp(false,
                "Constrain what is selected in SolveSpace's window instead of points/entities.")},
            {"value", {{"description", "Dimension value (mm or degrees), or an expression."}}},
            {"reference", BoolProp(false, "Reference dimension (measures only).")},
            {"supplementary", BoolProp(false,
                "For angles: constrain the supplementary angle (180 - value).")},
        }}, {"required", {"type"}}},
        AddConstraint,
    });
    tools->push_back({
        "set_constraint_value",
        "Change a dimension's value: a number in mm (or degrees for angles; a "
        "diameter for diameter constraints), or an expression using named "
        "parameters. Also sets the text of a comment or the expression of a "
        "relation. Fails, without changing anything, if the sketch would stop solving.",
        {{"type", "object"}, {"properties", {
            {"constraint", StrProp("Constraint handle.")},
            {"value", {{"description", "Number or expression string."}}},
        }}, {"required", {"constraint", "value"}}},
        SetConstraintValue,
    });
    tools->push_back({
        "set_constraint_reference",
        "Make a dimension a reference dimension (measures only) or a driving one.",
        {{"type", "object"}, {"properties", {
            {"constraint", StrProp("Constraint handle.")},
            {"reference", BoolProp(true, "true: reference; false: driving.")},
        }}, {"required", {"constraint"}}},
        SetConstraintReference,
    });
    tools->push_back({
        "list_constraints",
        "List the constraints of a group (default: the active group), with values.",
        {{"type", "object"}, {"properties", {
            {"group", StrProp("Group handle. Default: active group.")},
        }}},
        ListConstraints,
    });
    tools->push_back({
        "get_solve_status",
        "Solve status of the active group: ok or the failure, the conflicting "
        "constraints, the remaining degrees of freedom, and which points are still "
        "free to move (what still needs constraining).",
        {{"type", "object"}, {"properties", json::object()}},
        GetSolveStatus,
    });
    tools->push_back({
        "add_comment",
        "Add a text comment to the active group, shown in the view at 'at'.",
        {{"type", "object"}, {"properties", {
            {"text", StrProp("The comment.")},
            {"at", PointSchema("Where to show it; default the origin.")},
        }}, {"required", {"text"}}},
        AddComment,
    });
    tools->push_back({
        "list_parameters",
        "List the named parameters (variables usable in dimension expressions) and "
        "their values.",
        {{"type", "object"}, {"properties", json::object()}},
        ListParameters,
    });
    tools->push_back({
        "set_parameter",
        "Set a named parameter of the active group to a value or expression (of "
        "earlier groups' parameters), creating it if needed. The parameter is then "
        "locked to that value. Dimensions whose expressions use it follow.",
        {{"type", "object"}, {"properties", {
            {"name", StrProp("Parameter name.")},
            {"value", {{"description", "Number, or expression string."}}},
        }}, {"required", {"name", "value"}}},
        SetParameter,
    });
    tools->push_back({
        "add_relation",
        "Add a relation (an equation between named parameters, e.g. 'w = 2*h') to "
        "the active group. Unknown names become new parameters.",
        {{"type", "object"}, {"properties", {
            {"expression", StrProp("The equation.")},
            {"at", PointSchema("Where to show it; default the origin.")},
        }}, {"required", {"expression"}}},
        AddRelation,
    });
}

} // namespace Mcp
} // namespace SolveSpace
