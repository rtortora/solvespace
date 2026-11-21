//-----------------------------------------------------------------------------
// MCP tools: documents (new/open/save), undo/redo, and group management.
//-----------------------------------------------------------------------------
#include "mcptools.h"

namespace SolveSpace {
namespace Mcp {

static void RequireNotUnsaved(const json &args) {
    if(SS.unsaved && !args.value("discard_unsaved", false)) {
        throw ToolError("The current model has unsaved changes. Save it with save_file, "
                        "or pass discard_unsaved=true to discard them.");
    }
}

static Platform::Path AbsolutePath(const json &j, const char *what) {
    if(!j.is_string() || j.get<std::string>().empty()) {
        throw ToolError(std::string(what) + " must be a non-empty string");
    }
    Platform::Path path = Platform::Path::From(j.get<std::string>());
    if(!path.IsAbsolute()) {
        throw ToolError(std::string(what) + " must be an absolute path");
    }
    return path;
}

static void SummaryAfterLoad(ToolResult *res) {
    json groups = json::array();
    for(hGroup hg : SK.groupOrder) groups.push_back(GroupJson(SK.GetGroup(hg)));
    res->structured["file"] = SS.saveFile.IsEmpty() ? json() : json(SS.saveFile.raw);
    res->structured["groups"] = groups;
    res->structured["active_group"] = H(SS.GW.activeGroup);
}

//-----------------------------------------------------------------------------
// new_document / open_file / save_file
//-----------------------------------------------------------------------------
static void NewDocument(const json &args, ToolResult *res) {
    RequireNotUnsaved(args);
    if(!SS.saveFile.IsEmpty()) SS.RemoveAutosave();
    SS.saveFile.Clear();
    SS.NewFile();
    SS.AfterNewFile();
    SummaryAfterLoad(res);
}

static void OpenFileTool(const json &args, ToolResult *res) {
    Platform::Path path = AbsolutePath(args.value("path", json()), "path");
    RequireNotUnsaved(args);
    FILE *f = OpenFile(path, "rb");
    if(!f) throw ToolError("Can't open '" + path.raw + "': no such file");
    fclose(f);

    // Like SolveSpaceUI::Load(), but never offers to load an autosave file
    // (that would open a dialog).
    if(SS.LoadFromFile(path, /*canCancel=*/true)) {
        SS.saveFile = path;
        SS.AddToRecentList(path);
        SS.AfterNewFile();
        SS.unsaved = false;
    } else {
        SS.saveFile.Clear();
        SS.NewFile();
        SS.AfterNewFile();
        ThrowIfErrors();
        throw ToolError("Couldn't load '" + path.raw + "'; started a new model instead.");
    }
    SummaryAfterLoad(res);
}

static void SaveFile(const json &args, ToolResult *res) {
    Platform::Path path;
    if(args.contains("path")) {
        path = AbsolutePath(args["path"], "path");
        if(path.Extension().empty()) path = path.WithExtension("slvs");
    } else if(!SS.saveFile.IsEmpty()) {
        path = SS.saveFile;
    } else {
        throw ToolError("This model has never been saved; give a path.");
    }
    if(!SS.SaveToFile(path)) {
        ThrowIfErrors();
        throw ToolError("Couldn't save to '" + path.raw + "'");
    }
    SS.AddToRecentList(path);
    SS.saveFile = path;
    SS.RemoveAutosave();
    SS.unsaved = false;
    SS.UpdateWindowTitles();
    res->structured = {{"file", path.raw}};
}

//-----------------------------------------------------------------------------
// undo / redo
//-----------------------------------------------------------------------------
static void Undo(const json &args, ToolResult *res) {
    int steps = args.value("steps", 1);
    int done = 0;
    for(; done < steps && SS.undo.cnt > 0; done++) SS.UndoUndo();
    if(done == 0) throw ToolError("Nothing to undo.");
    Regenerate();
    res->structured = {{"undone", done}, {"can_undo", SS.undo.cnt > 0},
                       {"can_redo", SS.redo.cnt > 0}, {"active_group", H(SS.GW.activeGroup)}};
}

static void Redo(const json &args, ToolResult *res) {
    int steps = args.value("steps", 1);
    int done = 0;
    for(; done < steps && SS.redo.cnt > 0; done++) SS.UndoRedo();
    if(done == 0) throw ToolError("Nothing to redo.");
    Regenerate();
    res->structured = {{"redone", done}, {"can_undo", SS.undo.cnt > 0},
                       {"can_redo", SS.redo.cnt > 0}, {"active_group", H(SS.GW.activeGroup)}};
}

//-----------------------------------------------------------------------------
// Groups
//-----------------------------------------------------------------------------
static hGroup ParseUserGroup(const json &args) {
    if(!args.contains("group")) throw ToolError("group is required");
    hGroup hg = ParseGroup(args["group"]);
    if(hg == Group::HGROUP_REFERENCES) {
        throw ToolError("The #references group can't be changed.");
    }
    return hg;
}

static void ActivateGroup(const json &args, ToolResult *res) {
    hGroup hg = ParseUserGroup(args);
    TextWindow::ScreenActivateGroup(0, hg.v);
    Regenerate();
    res->structured = GroupJson(SK.GetGroup(hg));
    res->structured["active_workplane"] = ActiveWorkplaneJson();
}

static void SetGroupVisibility(const json &args, ToolResult *res) {
    if(!args.contains("visible")) throw ToolError("visible is required");
    bool visible = args["visible"].get<bool>();
    json results = json::array();
    std::vector<hGroup> groups;
    if(args.contains("groups")) {
        for(const json &j : args["groups"]) groups.push_back(ParseGroup(j));
    } else if(args.contains("group")) {
        groups.push_back(ParseGroup(args["group"]));
    } else {
        throw ToolError("group or groups is required");
    }
    for(hGroup hg : groups) {
        SK.GetGroup(hg)->visible = visible;
    }
    // A group that was just shown might not have been generated yet.
    SS.GenerateAll();
    SS.GW.persistentDirty = true;
    for(hGroup hg : groups) {
        Group *g = SK.GetGroup(hg);
        results.push_back({{"group", H(hg)}, {"name", g->name}, {"visible", g->visible}});
    }
    res->structured = {{"groups", results}};
}

// Applies one of the text window's group options ('s', 'S', 'w', 'c', ...),
// exactly as clicking it would.
static void GroupOption(hGroup hg, int link, uint32_t v = 0) {
    SS.TW.shown.group = hg;
    TextWindow::ScreenChangeGroupOption(link, v);
}

static void SetGroupProperties(const json &args, ToolResult *res) {
    hGroup hg = ParseUserGroup(args);
    Transaction t;
    Group *g = SK.GetGroup(hg);
    json changed = json::array();

    if(args.contains("name")) {
        std::string name = args["name"];
        if(name.empty()) throw ToolError("name can't be empty");
        g->name = name;
        changed.push_back("name");
    }
    if(args.contains("color")) {
        const json &c = args["color"];
        if(!c.is_array() || c.size() != 3) throw ToolError("color must be [r, g, b], 0..1");
        Vector rgb = Vector::From(Num(c[0], "color"), Num(c[1], "color"), Num(c[2], "color"));
        rgb = rgb.ClampWithin(0, 1);
        g->color = RgbaColor::FromFloat((float)rgb.x, (float)rgb.y, (float)rgb.z,
                                        g->color.alphaF());
        changed.push_back("color");
    }
    if(args.contains("opacity")) {
        double a = Num(args["opacity"], "opacity");
        if(a < 0 || a > 1) throw ToolError("opacity must be between 0 and 1");
        g->color.alpha = (int)(255.1f * a);
        changed.push_back("opacity");
    }
    if(args.contains("combine")) {
        if(!g->IsMeshGroup()) {
            throw ToolError("combine only applies to groups that make solids "
                            "(extrude, lathe, revolve, helix, rotate, translate, linked)");
        }
        std::string c = args["combine"];
        Group::CombineAs ca;
        if(c == "union")             ca = Group::CombineAs::UNION;
        else if(c == "difference")   ca = Group::CombineAs::DIFFERENCE;
        else if(c == "intersection") ca = Group::CombineAs::INTERSECTION;
        else if(c == "assemble")     ca = Group::CombineAs::ASSEMBLE;
        else throw ToolError("combine must be union, difference, intersection or assemble");
        GroupOption(hg, 'c', (uint32_t)ca);
        changed.push_back("combine");
    }
    g = SK.GetGroup(hg);
    if(args.contains("sides")) {
        std::string s = args["sides"];
        bool skewed = (g->subtype == Group::Subtype::ONE_SKEWED ||
                       g->subtype == Group::Subtype::TWO_SKEWED);
        bool hasSides = skewed || g->subtype == Group::Subtype::ONE_SIDED ||
                        g->subtype == Group::Subtype::TWO_SIDED;
        if(!hasSides) throw ToolError("This group has no one/two-sided option");
        if(s == "one_sided")      GroupOption(hg, 's');
        else if(s == "two_sided") GroupOption(hg, 'S');
        else throw ToolError("sides must be one_sided or two_sided");
        changed.push_back("sides");
    }
    g = SK.GetGroup(hg);
    if(args.contains("skewed")) {
        if(g->type != Group::Type::EXTRUDE) throw ToolError("skewed only applies to extrusions");
        bool want = args["skewed"].get<bool>();
        bool is = (g->subtype == Group::Subtype::ONE_SKEWED ||
                   g->subtype == Group::Subtype::TWO_SKEWED);
        if(want != is) GroupOption(hg, 'w');
        changed.push_back("skewed");
    }
    auto Toggle = [&](const char *key, bool current, int link) {
        if(!args.contains(key)) return;
        if(args[key].get<bool>() != current) GroupOption(hg, link);
        changed.push_back(key);
    };
    g = SK.GetGroup(hg);
    Toggle("suppress", g->suppress, 'P');
    g = SK.GetGroup(hg);
    Toggle("relax_constraints", g->relaxConstraints, 'r');
    g = SK.GetGroup(hg);
    Toggle("allow_redundant", g->allowRedundant, 'e');
    g = SK.GetGroup(hg);
    if(args.contains("skip_first")) {
        if(g->type != Group::Type::ROTATE && g->type != Group::Type::TRANSLATE) {
            throw ToolError("skip_first only applies to rotate and translate groups");
        }
        GroupOption(hg, args["skip_first"].get<bool>() ? 'k' : 'K');
        changed.push_back("skip_first");
    }
    g = SK.GetGroup(hg);
    if(args.contains("count")) {
        if(g->type != Group::Type::ROTATE && g->type != Group::Type::TRANSLATE) {
            throw ToolError("count only applies to rotate and translate groups");
        }
        int n = args["count"].get<int>();
        if(n < 1 || n > 999) throw ToolError("count must be between 1 and 999");
        g->valA = n;
        if(g->type == Group::Type::ROTATE && g->GetNumConstraints() == 0) {
            // As the GUI does: space the copies evenly over a full turn,
            // unless constraints already set the angle.
            int copies = g->skipFirst ? n + 1 : n;
            SK.GetParam(hg.param(3))->val = PI / (2 * copies);
        }
        changed.push_back("count");
    }
    ThrowIfErrors();
    SS.MarkGroupDirty(hg);
    RegenerateFor(hg);
    t.Commit();
    res->structured = GroupJson(SK.GetGroup(hg));
    res->structured["changed"] = changed;
}

static void DeleteGroup(const json &args, ToolResult *res) {
    hGroup hg = ParseUserGroup(args);
    std::string name = SK.GetGroup(hg)->name;
    Transaction t;
    SS.TW.shown.group = hg;
    TextWindow::ScreenDeleteGroup(0, hg.v);
    Regenerate();
    t.Commit();
    res->structured = {{"deleted", H(hg)}, {"name", name},
                       {"active_group", H(SS.GW.activeGroup)}};
}

//-----------------------------------------------------------------------------
// delete (entities, requests, constraints)
//-----------------------------------------------------------------------------
static void Delete(const json &args, ToolResult *res) {
    if(!args.contains("handles")) throw ToolError("handles is required");
    Handles h = ParseHandles(args["handles"]);
    if(!h.groups.empty()) throw ToolError("Use delete_group to delete groups.");

    Transaction t;
    SK.request.ClearTags();
    SK.constraint.ClearTags();
    json deleted = json::array();
    auto TagRequest = [&](hRequest hr) {
        if(hr.IsFromReferences()) throw ToolError("Reference geometry can't be deleted.");
        SK.request.Tag(hr, 1);
        deleted.push_back(H(hr));
    };
    for(hRequest hr : h.requests) TagRequest(hr);
    for(hEntity he : h.entities) {
        if(!he.isFromRequest()) {
            throw ToolError(H(he) + " is generated by its group, not drawn; "
                            "delete the group, or the sketch entity it came from.");
        }
        TagRequest(he.request());
    }
    for(hConstraint hc : h.constraints) {
        SK.constraint.Tag(hc, 1);
        deleted.push_back(H(hc));
    }
    SK.constraint.RemoveTagged();
    SS.GW.DeleteTaggedRequests();
    Regenerate();
    t.Commit();
    res->structured = {{"deleted", deleted}, {"solve", SolveStatus(SS.GW.activeGroup)}};
}

void AddDocumentTools(std::vector<Tool> *tools) {
    json discard = BoolProp(false, "Discard unsaved changes to the current model.");
    tools->push_back({
        "new_document",
        "Start a new, empty model (like File > New). Refuses if there are unsaved "
        "changes, unless discard_unsaved is true. Never shows a save prompt.",
        {{"type", "object"}, {"properties", {{"discard_unsaved", discard}}}},
        NewDocument,
    });
    tools->push_back({
        "open_file",
        "Open a .slvs model from an absolute path. Refuses if there are unsaved "
        "changes, unless discard_unsaved is true.",
        {{"type", "object"}, {"properties", {
            {"path", StrProp("Absolute path of the .slvs file.")},
            {"discard_unsaved", discard},
        }}, {"required", {"path"}}},
        OpenFileTool,
    });
    tools->push_back({
        "save_file",
        "Save the model. With path, saves to that absolute path (adding .slvs if "
        "there's no extension) and makes it the current file; otherwise saves to "
        "the current file.",
        {{"type", "object"}, {"properties", {
            {"path", StrProp("Absolute path to save to.")},
        }}},
        SaveFile,
    });
    tools->push_back({
        "undo",
        "Undo the last change(s), as Edit > Undo does. Each modifying tool call is "
        "one undo step.",
        {{"type", "object"}, {"properties", {{"steps", IntProp(1, 100, 1, "How many steps.")}}}},
        Undo,
    });
    tools->push_back({
        "redo",
        "Redo changes undone by undo.",
        {{"type", "object"}, {"properties", {{"steps", IntProp(1, 100, 1, "How many steps.")}}}},
        Redo,
    });
    tools->push_back({
        "activate_group",
        "Make a group the active group, so that new geometry and constraints go "
        "into it. Groups after the active one are hidden in the view while it is "
        "active.",
        {{"type", "object"}, {"properties", {{"group", StrProp("Group handle.")}}},
         {"required", {"group"}}},
        ActivateGroup,
    });
    tools->push_back({
        "set_group_visibility",
        "Show or hide groups (SolveSpace has no layers; group visibility is the "
        "equivalent). Give group or groups.",
        {{"type", "object"}, {"properties", {
            {"group", StrProp("Group handle.")},
            {"groups", HandleListSchema("Several group handles.")},
            {"visible", BoolProp(true, "Whether the groups are shown.")},
        }}, {"required", {"visible"}}},
        SetGroupVisibility,
    });
    tools->push_back({
        "set_group_properties",
        "Change a group's properties. Only the given properties change. combine "
        "applies to solid-making groups; sides to extrude/revolve/helix/rotate/"
        "translate; skewed to extrude; count and skip_first to rotate/translate.",
        {{"type", "object"}, {"properties", {
            {"group", StrProp("Group handle.")},
            {"name", StrProp("New name.")},
            {"color", Vec3Schema("Solid colour [r, g, b], each 0..1.")},
            {"opacity", NumProp("Solid opacity, 0..1.")},
            {"combine", {{"type", "string"},
                         {"enum", {"union", "difference", "intersection", "assemble"}},
                         {"description", "How the solid combines with earlier solids."}}},
            {"sides", {{"type", "string"}, {"enum", {"one_sided", "two_sided"}}}},
            {"skewed", BoolProp(false, "Allow the extrusion direction to be non-normal.")},
            {"suppress", BoolProp(false, "Suppress this group's solid.")},
            {"relax_constraints", BoolProp(false, "Relax constraints while dragging.")},
            {"allow_redundant", BoolProp(false, "Allow redundant constraints.")},
            {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 999},
                       {"description", "Number of copies (rotate/translate)."}}},
            {"skip_first", BoolProp(false, "Skip the first copy (rotate/translate).")},
        }}, {"required", {"group"}}},
        SetGroupProperties,
    });
    tools->push_back({
        "delete_group",
        "Delete a group and everything in it. Later groups that depend on it are "
        "removed or broken, as in the GUI.",
        {{"type", "object"}, {"properties", {{"group", StrProp("Group handle.")}}},
         {"required", {"group"}}},
        DeleteGroup,
    });
    tools->push_back({
        "delete",
        "Delete sketch geometry and/or constraints. Give request handles (r:...), "
        "entity handles (e:...; deletes the whole request that drew the entity, e.g. "
        "a line's endpoint deletes the line) and constraint handles (c:...). "
        "Constraints on deleted geometry are deleted too.",
        {{"type", "object"}, {"properties", {
            {"handles", HandleListSchema("Handles to delete.")},
        }}, {"required", {"handles"}}},
        Delete,
    });
}

} // namespace Mcp
} // namespace SolveSpace
