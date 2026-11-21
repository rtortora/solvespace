//-----------------------------------------------------------------------------
// MCP tools: the GUI's view (camera), display options, units and styles.
//
// These change what the user sees in the window. View and display settings
// are not part of the model, so they aren't undo steps; style changes are.
//-----------------------------------------------------------------------------
#include "mcptools.h"

namespace SolveSpace {
namespace Mcp {

static double Rad(double deg) { return deg * PI / 180.0; }

static const char *UnitName(Unit u) {
    switch(u) {
        case Unit::MM:          return "mm";
        case Unit::METERS:      return "m";
        case Unit::INCHES:      return "in";
        case Unit::FEET_INCHES: return "ft_in";
    }
    return "mm";
}

// Stops an animated view change in progress (e.g. onto a new workplane),
// which would otherwise overwrite a view set now.
static void StopAnimation() {
    if(SS.GW.animateTimer) SS.GW.animateTimer->onTimeout = [] {};
}

static json ViewJson() {
    GraphicsWindow &gw = SS.GW;
    json j = {
        {"offset", Vec(gw.offset)},
        {"center", Vec(gw.offset.ScaledBy(-1))},
        {"proj_right", Vec(gw.projRight)},
        {"proj_up", Vec(gw.projUp)},
        {"toward_viewer", Vec(gw.projRight.Cross(gw.projUp))},
        {"scale", gw.scale},
        {"perspective", SS.usePerspectiveProj},
        {"perspective_factor", Round(SS.cameraTangent * 1000)},
        {"units", UnitName(SS.viewUnits)},
        {"explode", SS.explode},
    };
    if(gw.window) {
        double w, h;
        gw.window->GetContentSize(&w, &h);
        j["window_size"] = {(int)w, (int)h};
    }
    return j;
}

static void ViewChanged(ToolResult *res) {
    SS.GW.NormalizeProjectionVectors();
    SS.GW.Invalidate();
    SS.ScheduleShowTW();
    res->structured = ViewJson();
}

// Fits the model (or the given handles) in the window.
static void ZoomToFitHandles(const json &args) {
    if(!SS.GW.window) return;
    bool useSelection = args.contains("handles");
    if(useSelection) SelectOnly(ParseHandles(args["handles"]));
    SS.GW.ZoomToFit(args.value("include_invisible", false), useSelection);
    if(useSelection) SS.GW.ClearSelection();
}

//-----------------------------------------------------------------------------
// get_view / set_view / orient_view / zoom / zoom_to_fit / pan / rotate_view
// / center_on
//-----------------------------------------------------------------------------
static void GetView(const json &, ToolResult *res) {
    res->structured = ViewJson();
}

static void SetView(const json &args, ToolResult *res) {
    StopAnimation();
    GraphicsWindow &gw = SS.GW;
    if(args.contains("offset") && args.contains("center")) {
        throw ToolError("Give offset or center, not both (offset = -center)");
    }
    Vector right = gw.projRight, up = gw.projUp;
    if(args.contains("proj_right")) right = Vec3(args["proj_right"], "proj_right");
    if(args.contains("proj_up"))    up    = Vec3(args["proj_up"], "proj_up");
    if(right.Magnitude() < LENGTH_EPS || up.Magnitude() < LENGTH_EPS ||
       right.Cross(up).Magnitude() < LENGTH_EPS * right.Magnitude() * up.Magnitude()) {
        throw ToolError("proj_right and proj_up must be non-zero and not parallel");
    }
    if(args.contains("scale")) {
        double scale = Num(args["scale"], "scale");
        if(scale <= 0) throw ToolError("scale must be positive (pixels per mm)");
        gw.scale = scale;
    }
    gw.projRight = right;
    gw.projUp    = up;
    if(args.contains("offset")) gw.offset = Vec3(args["offset"], "offset");
    if(args.contains("center")) gw.offset = Vec3(args["center"], "center").ScaledBy(-1);
    ViewChanged(res);
}

static void OrientView(const json &args, ToolResult *res) {
    std::string preset = args.value("preset", "iso");
    GraphicsWindow &gw = SS.GW;
    StopAnimation();
    if(preset == "workplane") {
        if(!gw.LockedInWorkplane()) throw ToolError("No workplane is active");
        Entity *w = SK.GetEntity(gw.ActiveWorkplane());
        Quaternion q = w->Normal()->NormalGetNum();
        gw.projRight = q.RotationU();
        gw.projUp    = q.RotationV();
        gw.offset    = SK.GetEntity(w->point[0])->PointGetNum().ScaledBy(-1);
    } else if(preset == "nearest_ortho" || preset == "nearest_iso") {
        // The GUI's View menu commands; they animate.
        GraphicsWindow::MenuView(preset == "nearest_ortho" ? Command::NEAREST_ORTHO
                                                           : Command::NEAREST_ISO);
        res->structured = ViewJson();
        res->structured["note"] = "The view is animating to this orientation; get_view "
                                  "shows the final view once it has finished.";
        return;
    } else {
        Vector right, up;
        ViewPreset(preset, &right, &up);
        gw.projRight = right;
        gw.projUp    = up;
    }
    if(args.value("zoom_to_fit", true)) {
        gw.NormalizeProjectionVectors();
        ZoomToFitHandles(json::object());
    }
    ViewChanged(res);
}

static void Zoom(const json &args, ToolResult *res) {
    double factor = Num(args.value("factor", json()), "factor");
    if(factor <= 0) throw ToolError("factor must be positive (2 zooms in 2x, 0.5 out)");
    StopAnimation();
    SS.GW.scale = max(0.003, min(300.0, SS.GW.scale * factor));
    ViewChanged(res);
}

static void ZoomToFit(const json &args, ToolResult *res) {
    StopAnimation();
    if(!SS.GW.window) throw ToolError("There is no window");
    ZoomToFitHandles(args);
    ViewChanged(res);
}

static void Pan(const json &args, ToolResult *res) {
    StopAnimation();
    GraphicsWindow &gw = SS.GW;
    Vector d;
    if(args.contains("dx") || args.contains("dy")) {
        // Screen pixels: the model moves right by dx and up by dy.
        double dx = args.contains("dx") ? Num(args["dx"], "dx") : 0;
        double dy = args.contains("dy") ? Num(args["dy"], "dy") : 0;
        d = gw.projRight.ScaledBy(dx / gw.scale).Plus(gw.projUp.ScaledBy(dy / gw.scale));
    } else if(args.contains("by")) {
        // Model space, mm: the view centre moves by this vector.
        d = Vec3(args["by"], "by").ScaledBy(-1);
    } else {
        throw ToolError("Give dx/dy (pixels) or by [x, y, z] (mm)");
    }
    gw.offset = gw.offset.Plus(d);
    ViewChanged(res);
}

static void RotateView(const json &args, ToolResult *res) {
    GraphicsWindow &gw = SS.GW;
    double angle = Rad(Num(args.value("angle_deg", json()), "angle_deg"));
    Vector axis;
    const json &a = args.value("axis", json("vertical"));
    if(a.is_string() && a == "vertical") {
        axis = gw.projUp;                               // turn left/right
    } else if(a.is_string() && a == "horizontal") {
        axis = gw.projRight;                            // tilt
    } else if(a.is_string() && a == "view") {
        axis = gw.projRight.Cross(gw.projUp);           // roll
    } else {
        axis = AxisArg(a, "axis");
    }
    StopAnimation();
    // Rotate the model about the axis by angle: the camera turns the other way.
    gw.projRight = gw.projRight.RotatedAbout(axis, -angle);
    gw.projUp    = gw.projUp.RotatedAbout(axis, -angle);
    ViewChanged(res);
}

static void CenterOn(const json &args, ToolResult *res) {
    if(!args.contains("point")) throw ToolError("point is required");
    const json &p = args["point"];
    Vector at;
    if(p.is_string()) {
        Entity *e = SK.GetEntity(ParseEntity(p));
        if(!e->IsPoint()) throw ToolError(H(e->h) + " isn't a point");
        at = e->PointGetNum();
    } else {
        at = Vec3(p, "point");
    }
    StopAnimation();
    SS.GW.offset = at.ScaledBy(-1);
    ViewChanged(res);
}

//-----------------------------------------------------------------------------
// set_perspective / set_explode_sketch / set_units
//-----------------------------------------------------------------------------
static void SetPerspective(const json &args, ToolResult *res) {
    if(args.contains("factor")) {
        double f = Num(args["factor"], "factor");
        if(f < 0 || f > 2) throw ToolError("factor must be between 0 and 2");
        // As the configuration screen stores it.
        SS.cameraTangent = f / 1000.0;
    }
    if(args.contains("enabled")) SS.usePerspectiveProj = args["enabled"].get<bool>();
    SS.GW.EnsureValidActives();
    SS.GW.Invalidate();
    res->structured = ViewJson();
    if(SS.usePerspectiveProj && SS.cameraTangent < 1e-6) {
        res->structured["note"] = "The perspective factor is 0, so the view is still a "
                                  "parallel projection; give factor (about 0.3 is typical).";
    }
}

static void SetExplodeSketch(const json &args, ToolResult *res) {
    if(args.contains("distance")) {
        double d = Num(args["distance"], "distance");
        if(d == 0) throw ToolError("distance can't be zero");
        SS.explodeDistance = d;
    }
    SS.explode = args.value("enabled", true);
    SS.GW.EnsureValidActives();
    SS.MarkGroupDirty(SS.GW.activeGroup, true);
    Regenerate();
    res->structured = {{"explode", SS.explode}, {"distance", SS.explodeDistance}};
}

static void SetUnits(const json &args, ToolResult *res) {
    std::string u = args.value("units", "");
    Command cmd;
    if(u == "mm")         cmd = Command::UNITS_MM;
    else if(u == "m")     cmd = Command::UNITS_METERS;
    else if(u == "in")    cmd = Command::UNITS_INCHES;
    else if(u == "ft_in") cmd = Command::UNITS_FEET_INCHES;
    else throw ToolError("units must be mm, m, in or ft_in");
    GraphicsWindow::MenuView(cmd);
    res->structured = {{"units", UnitName(SS.viewUnits)},
                       {"note", "Display units only; tools still take and return mm."}};
}

//-----------------------------------------------------------------------------
// set_display_options
//-----------------------------------------------------------------------------
static const char *ConstraintModeName(GraphicsWindow::ShowConstraintMode m) {
    switch(m) {
        case GraphicsWindow::ShowConstraintMode::SCM_NOSHOW:   return "none";
        case GraphicsWindow::ShowConstraintMode::SCM_SHOW_ALL: return "all";
        case GraphicsWindow::ShowConstraintMode::SCM_SHOW_DIM: return "dimensions";
    }
    return "all";
}

static const char *OccludedName(GraphicsWindow::DrawOccludedAs d) {
    switch(d) {
        case GraphicsWindow::DrawOccludedAs::INVISIBLE: return "invisible";
        case GraphicsWindow::DrawOccludedAs::STIPPLED:  return "stippled";
        case GraphicsWindow::DrawOccludedAs::VISIBLE:   return "visible";
    }
    return "stippled";
}

struct BoolOption { const char *name; bool GraphicsWindow::*field; };
static const BoolOption BOOL_OPTIONS[] = {
    { "workplanes",   &GraphicsWindow::showWorkplanes },
    { "normals",      &GraphicsWindow::showNormals },
    { "points",       &GraphicsWindow::showPoints },
    { "construction", &GraphicsWindow::showConstruction },
    { "shaded",       &GraphicsWindow::showShaded },
    { "faces",        &GraphicsWindow::showFaces },
    { "edges",        &GraphicsWindow::showEdges },
    { "outlines",     &GraphicsWindow::showOutlines },
    { "mesh",         &GraphicsWindow::showMesh },
    { "snap_grid",    &GraphicsWindow::showSnapGrid },
    { "dim_solid",    &GraphicsWindow::dimSolidModel },
};

static json DisplayOptionsJson() {
    json j;
    for(const BoolOption &o : BOOL_OPTIONS) j[o.name] = SS.GW.*o.field;
    j["constraints"]  = ConstraintModeName(SS.GW.showConstraints);
    j["hidden_lines"] = OccludedName(SS.GW.drawOccludedAs);
    return j;
}

static void SetDisplayOptions(const json &args, ToolResult *res) {
    GraphicsWindow &gw = SS.GW;
    for(auto &it : args.items()) {
        bool known = it.key() == "constraints" || it.key() == "hidden_lines";
        for(const BoolOption &o : BOOL_OPTIONS) known = known || it.key() == o.name;
        if(!known) throw ToolError("Unknown display option '" + it.key() + "'");
    }
    for(const BoolOption &o : BOOL_OPTIONS) {
        if(!args.contains(o.name)) continue;
        bool want = args[o.name].get<bool>();
        // ToggleBool also does what the GUI does around it (generating edges
        // that weren't, keeping faces and shading consistent).
        if(gw.*o.field != want) gw.ToggleBool(&(gw.*o.field));
    }
    if(args.contains("constraints")) {
        std::string m = args["constraints"];
        if(m == "none")            gw.showConstraints = GraphicsWindow::ShowConstraintMode::SCM_NOSHOW;
        else if(m == "all")        gw.showConstraints = GraphicsWindow::ShowConstraintMode::SCM_SHOW_ALL;
        else if(m == "dimensions") gw.showConstraints = GraphicsWindow::ShowConstraintMode::SCM_SHOW_DIM;
        else throw ToolError("constraints must be none, all or dimensions");
    }
    if(args.contains("hidden_lines")) gw.drawOccludedAs = HiddenLinesArg(args["hidden_lines"]);
    gw.EnsureValidActives();
    gw.Invalidate(/*clearPersistent=*/true);
    SS.ScheduleShowTW();
    res->structured = DisplayOptionsJson();
}

//-----------------------------------------------------------------------------
// Styles
//-----------------------------------------------------------------------------
static hStyle ParseStyle(const json &j) {
    if(!j.is_string()) throw ToolError("style must be a handle like 's:0100' or a name");
    std::string s = j;
    if(s == "default" || s == "none") return hStyle{0};
    if(s.compare(0, 2, "s:") == 0) {
        char *end;
        unsigned long v = strtoul(s.c_str() + 2, &end, 16);
        if(*end != '\0' || s.size() == 2) throw ToolError("Malformed style handle '" + s + "'");
        hStyle hs = { (uint32_t)v };
        if(hs.v < Style::FIRST_CUSTOM && hs.v > Style::OUTLINE) {
            throw ToolError("No such style: " + s);
        }
        if(hs.v >= Style::FIRST_CUSTOM && !SK.style.FindByIdNoOops(hs)) {
            throw ToolError("No such style: " + s);
        }
        return hs;
    }
    for(Style &st : SK.style) {
        if(st.name == s) return st.h;
    }
    for(uint32_t v = Style::ACTIVE_GRP; v <= Style::OUTLINE; v++) {
        hStyle hs = { v };
        if(Style::Get(hs)->name == s) return hs;
    }
    throw ToolError("No style named '" + s + "' (list_styles lists them)");
}

static json StyleJson(hStyle hs) {
    Style *s = Style::Get(hs);
    json j = {{"handle", H(hs)},
              {"name", hs.v >= Style::FIRST_CUSTOM ? s->name : s->DescriptionString()},
              {"custom", hs.v >= Style::FIRST_CUSTOM},
              {"color", ColorHex(s->color)}, {"width", Round(s->width)},
              {"width_units", s->widthAs == Style::UnitsAs::MM ? "mm" : "pixels"},
              {"visible", s->visible},
              {"stipple", Style::StipplePatternName(s->stippleType)}};
    if(s->filled) j["fill_color"] = ColorHex(s->fillColor);
    return j;
}

static void ListStyles(const json &args, ToolResult *res) {
    json out = json::array();
    if(args.value("include_builtin", true)) {
        for(uint32_t v = Style::ACTIVE_GRP; v <= Style::OUTLINE; v++) {
            out.push_back(StyleJson(hStyle{v}));
        }
    }
    for(Style &s : SK.style) {
        if(s.h.v >= Style::FIRST_CUSTOM) out.push_back(StyleJson(s.h));
    }
    res->structured = {{"styles", out}};
}

static RgbaColor ColorArg(const json &c, RgbaColor old) {
    if(c.is_string()) {
        std::string s = c;
        unsigned r, g, b;
        if(s.size() == 7 && s[0] == '#' && sscanf(s.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            return RgbaColor::From((int)r, (int)g, (int)b, old.alpha);
        }
        throw ToolError("color must be '#rrggbb' or [r, g, b] with 0..1 components");
    }
    if(!c.is_array() || c.size() != 3) {
        throw ToolError("color must be '#rrggbb' or [r, g, b] with 0..1 components");
    }
    Vector rgb = Vector::From(Num(c[0], "color"), Num(c[1], "color"), Num(c[2], "color"));
    rgb = rgb.ClampWithin(0, 1);
    return RgbaColor::FromFloat((float)rgb.x, (float)rgb.y, (float)rgb.z, old.alphaF());
}

static void ApplyStyleProperties(Style *s, const json &args) {
    if(args.contains("name")) {
        std::string name = args["name"];
        if(name.empty()) throw ToolError("name can't be empty");
        if(s->h.v < Style::FIRST_CUSTOM) throw ToolError("Built-in styles can't be renamed");
        s->name = name;
    }
    if(args.contains("color")) s->color = ColorArg(args["color"], s->color);
    if(args.contains("width")) {
        double w = Num(args["width"], "width");
        if(w <= 0) throw ToolError("width must be positive");
        s->width = w;
    }
    if(args.contains("width_units")) {
        std::string u = args["width_units"];
        if(u == "mm")          s->widthAs = Style::UnitsAs::MM;
        else if(u == "pixels") s->widthAs = Style::UnitsAs::PIXELS;
        else throw ToolError("width_units must be mm or pixels");
    }
    if(args.contains("visible")) s->visible = args["visible"].get<bool>();
    if(args.contains("stipple")) {
        std::string st = args["stipple"];
        static const char *names[] = { "Continuous", "ShortDash", "Dash", "LongDash",
                                       "DashDot", "DashDotDot", "Dot", "FreeHand", "ZigZag" };
        bool ok = false;
        for(const char *n : names) ok = ok || st == n;
        if(!ok) {
            throw ToolError("stipple must be one of Continuous, ShortDash, Dash, LongDash, "
                            "DashDot, DashDotDot, Dot, FreeHand, ZigZag");
        }
        s->stippleType = Style::StipplePatternFromString(st);
    }
}

static void CreateStyle(const json &args, ToolResult *res) {
    Transaction t;
    hStyle hs = { Style::CreateCustomStyle(/*rememberForUndo=*/false) };
    Style *s = Style::Get(hs);
    if(!args.contains("name")) s->name = "style-" + std::to_string(hs.v - Style::FIRST_CUSTOM + 1);
    ApplyStyleProperties(s, args);
    SS.GW.Invalidate(/*clearPersistent=*/true);
    t.Commit();
    res->structured = {{"style", StyleJson(hs)}};
}

static void SetStyleProperties(const json &args, ToolResult *res) {
    hStyle hs = ParseStyle(args.value("style", json()));
    if(hs.v == 0) throw ToolError("Give a style; 'default' has no properties of its own");
    Transaction t;
    ApplyStyleProperties(Style::Get(hs), args);
    SS.GW.persistentDirty = true;
    SK.GetGroup(SS.GW.activeGroup)->displayDirty = true;
    for(Group &g : SK.group) g.displayDirty = true;
    SS.GW.Invalidate(/*clearPersistent=*/true);
    t.Commit();
    res->structured = {{"style", StyleJson(hs)}};
}

static void SetStyle(const json &args, ToolResult *res) {
    Handles h = ParseHandles(args.value("handles", json::array()));
    for(hRequest hr : h.requests) h.entities.push_back(hr.entity(0));
    if(h.entities.empty() && h.constraints.empty()) {
        throw ToolError("Give entity, request or constraint handles");
    }
    for(hEntity he : h.entities) {
        if(!he.isFromRequest()) {
            throw ToolError(H(he) + " is generated by a group, not drawn; styles can only be "
                            "assigned to sketch geometry (its request) and constraints");
        }
    }
    hStyle hs = ParseStyle(args.value("style", json()));
    Transaction t;
    SelectOnly(h);
    Style::AssignSelectionToStyle(hs.v);
    // It opens the style's screen in the property browser; go back.
    SS.TW.GoToScreen(TextWindow::Screen::LIST_OF_GROUPS);
    ThrowIfErrors();
    Regenerate();
    t.Commit();
    json changed = json::array();
    for(hEntity he : h.entities) changed.push_back(H(he.request()));
    for(hConstraint hc : h.constraints) changed.push_back(H(hc));
    res->structured = {{"changed", changed}, {"style", hs.v ? StyleJson(hs) : json("default")}};
}

void AddViewTools(std::vector<Tool> *tools) {
    json none = {{"type", "object"}, {"properties", json::object()}};
    tools->push_back({
        "get_view",
        "The GUI's view: center (the model point at the middle of the window; "
        "offset = -center), proj_right / proj_up (screen directions in model space), "
        "scale (pixels per mm), perspective, display units, exploded sketch.",
        none, GetView,
    });
    tools->push_back({
        "set_view",
        "Set the GUI's camera directly: any of center (or offset), proj_right, "
        "proj_up and scale (pixels per mm). Use orient_view for standard views.",
        {{"type", "object"}, {"properties", {
            {"center", Vec3Schema("Model point at the middle of the window [x,y,z] mm.")},
            {"offset", Vec3Schema("The view offset (= -center).")},
            {"proj_right", Vec3Schema("Model direction pointing right on screen.")},
            {"proj_up", Vec3Schema("Model direction pointing up on screen.")},
            {"scale", NumProp("Zoom, in pixels per mm.")},
        }}},
        SetView,
    });
    tools->push_back({
        "orient_view",
        "Turn the GUI's view to a standard orientation (Z up: front looks from -Y, "
        "right from +X, top from +Z, iso from +X-Y+Z), onto the active workplane, or "
        "to the nearest orthographic/isometric view (as the View menu does). Fits the "
        "model in the window by default.",
        {{"type", "object"}, {"properties", {
            {"preset", {{"type", "string"}, {"enum", {"iso", "front", "back", "left", "right",
                "top", "bottom", "workplane", "nearest_ortho", "nearest_iso"}},
                {"default", "iso"}}},
            {"zoom_to_fit", BoolProp(true, "Also fit the model in the window.")},
        }}},
        OrientView,
    });
    tools->push_back({
        "zoom",
        "Zoom the GUI's view about the window's centre: factor 2 zooms in 2x, 0.5 out.",
        {{"type", "object"}, {"properties", {{"factor", NumProp("Zoom factor.")}}},
         {"required", {"factor"}}},
        Zoom,
    });
    tools->push_back({
        "zoom_to_fit",
        "Fit the model, or the given entities/constraints, in the GUI's window.",
        {{"type", "object"}, {"properties", {
            {"handles", HandleListSchema("Fit these instead of the whole model.")},
            {"include_invisible", BoolProp(false, "Also fit hidden geometry.")},
        }}},
        ZoomToFit,
    });
    tools->push_back({
        "pan",
        "Pan the GUI's view: by dx/dy screen pixels (the model moves right/up), or "
        "move the view's centre by a model vector 'by' [x, y, z] mm.",
        {{"type", "object"}, {"properties", {
            {"dx", NumProp("Pixels to the right.")},
            {"dy", NumProp("Pixels up.")},
            {"by", Vec3Schema("Move the view centre by this, mm.")},
        }}},
        Pan,
    });
    tools->push_back({
        "rotate_view",
        "Rotate the model on screen by angle_deg about an axis through the view "
        "centre: 'vertical' (the screen's up axis: turn left/right), 'horizontal' "
        "(tilt), 'view' (roll), or a model direction ('+z', [x,y,z]). Positive angles "
        "turn the model counter-clockwise, seen from the axis's positive end.",
        {{"type", "object"}, {"properties", {
            {"axis", {{"description", "vertical, horizontal, view, or a model axis."},
                      {"default", "vertical"}}},
            {"angle_deg", NumProp("Angle in degrees.")},
        }}, {"required", {"angle_deg"}}},
        RotateView,
    });
    tools->push_back({
        "center_on",
        "Move the GUI's view so a point (a point entity, or [x, y, z]) is at the "
        "middle of the window, keeping the orientation and zoom.",
        {{"type", "object"}, {"properties", {
            {"point", {{"description", "Point entity handle or [x, y, z] mm."}}},
        }}, {"required", {"point"}}},
        CenterOn,
    });
    tools->push_back({
        "set_perspective",
        "Switch the GUI between parallel and perspective projection; factor is the "
        "configuration screen's perspective factor (about 0.3 is typical). Both "
        "are saved in SolveSpace's settings.",
        {{"type", "object"}, {"properties", {
            {"enabled", {{"type", "boolean"}}},
            {"factor", NumProp("Perspective factor, 0..2.")},
        }}},
        SetPerspective,
    });
    tools->push_back({
        "set_explode_sketch",
        "Show the active sketch exploded (each entity moved apart along the "
        "workplane normal, to untangle overlapping geometry), or not.",
        {{"type", "object"}, {"properties", {
            {"enabled", BoolProp(true, "Explode the sketch.")},
            {"distance", NumProp("Distance between entities, mm.")},
        }}},
        SetExplodeSketch,
    });
    tools->push_back({
        "set_units",
        "Set the units the GUI displays (mm, m, in, ft_in); a user setting, saved in "
        "SolveSpace's settings. Tools always use mm.",
        {{"type", "object"}, {"properties", {
            {"units", {{"type", "string"}, {"enum", {"mm", "m", "in", "ft_in"}}}},
        }}, {"required", {"units"}}},
        SetUnits,
    });
    json opts = json::object();
    for(const BoolOption &o : BOOL_OPTIONS) opts[o.name] = {{"type", "boolean"}};
    opts["constraints"]  = {{"type", "string"}, {"enum", {"none", "all", "dimensions"}}};
    opts["hidden_lines"] = {{"type", "string"}, {"enum", {"invisible", "stippled", "visible"}},
                            {"description", "Edges behind the solid."}};
    tools->push_back({
        "set_display_options",
        "Show or hide things in the GUI (the property browser's display toggles): "
        "workplanes, normals, points, construction geometry, constraints "
        "(none/all/dimensions), shaded solid, faces, edges, outlines, mesh, snap grid, "
        "dimmed solid, and how hidden lines are drawn. Returns all of them; with no "
        "arguments, just returns them.",
        {{"type", "object"}, {"properties", opts}},
        SetDisplayOptions,
    });
    tools->push_back({
        "list_styles",
        "List line styles: the built-in ones (active group, construction, "
        "inactive group, datum, solid edge, constraint, ...) and custom ones, with "
        "colour, width and visibility. Hiding a style hides everything drawn with it.",
        {{"type", "object"}, {"properties", {
            {"include_builtin", BoolProp(true, "Include the built-in styles.")},
        }}},
        ListStyles,
    });
    json styleProps = {
        {"name", StrProp("Style name.")},
        {"color", {{"description", "'#rrggbb', or [r, g, b] 0..1."}}},
        {"width", NumProp("Line width.")},
        {"width_units", {{"type", "string"}, {"enum", {"pixels", "mm"}}}},
        {"visible", {{"type", "boolean"}}},
        {"stipple", {{"type", "string"}, {"enum", {"Continuous", "ShortDash", "Dash",
            "LongDash", "DashDot", "DashDotDot", "Dot", "FreeHand", "ZigZag"}}}},
    };
    tools->push_back({
        "create_style",
        "Create a custom line style (e.g. to colour or hide a set of curves, like a "
        "layer); assign it with set_style.",
        {{"type", "object"}, {"properties", styleProps}},
        CreateStyle,
    });
    json setProps = styleProps;
    setProps["style"] = StrProp("Style handle ('s:0100') or name.");
    tools->push_back({
        "set_style_properties",
        "Change a style's colour, width, line pattern or visibility (visible=false "
        "hides everything drawn in it), or rename a custom style.",
        {{"type", "object"}, {"properties", setProps}, {"required", {"style"}}},
        SetStyleProperties,
    });
    tools->push_back({
        "set_style",
        "Assign a style to sketch geometry (entity or request handles) and "
        "constraints; style 'default' removes it.",
        {{"type", "object"}, {"properties", {
            {"handles", HandleListSchema("Entities, requests or constraints.")},
            {"style", StrProp("Style handle, name, or 'default'.")},
        }}, {"required", {"handles", "style"}}},
        SetStyle,
    });
}

} // namespace Mcp
} // namespace SolveSpace
