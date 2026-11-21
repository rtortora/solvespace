//-----------------------------------------------------------------------------
// MCP tools: the GUI's selection, picking on screenshots, measurement, mesh
// checks, export to files, and running menu commands (milestone 7).
//-----------------------------------------------------------------------------
#include "mcptools.h"

namespace SolveSpace {
namespace Mcp {

//-----------------------------------------------------------------------------
// Selection: select / clear_selection / get_selection
//-----------------------------------------------------------------------------
static json SelectionJson(bool verbose) {
    GraphicsWindow &gw = SS.GW;
    json items = json::array();
    for(GraphicsWindow::Selection &s : gw.selection) {
        if(s.entity.v) {
            Entity *e = SK.entity.FindByIdNoOops(s.entity);
            if(e == NULL) continue;
            json j = EntityJson(e, verbose);
            if(e->IsFace()) {
                FaceInfo fi = GetFaceInfo(e->h);
                if(fi.onSurface) j["face"] = fi.Json();
            }
            items.push_back(j);
        } else if(s.constraint.v) {
            Constraint *c = SK.constraint.FindByIdNoOops(s.constraint);
            if(c != NULL) items.push_back(ConstraintJson(c));
        }
    }
    gw.GroupSelection();
    auto const &gs = gw.gs;
    json counts = json::object();
    auto count = [&](const char *name, int n) { if(n > 0) counts[name] = n; };
    count("points", gs.points);
    count("line_segments", gs.lineSegments);
    count("circles_or_arcs", gs.circlesOrArcs);
    count("arcs", gs.arcs);
    count("cubics", gs.cubics + gs.periodicCubics);
    count("workplanes", gs.workplanes);
    count("normals", gs.anyNormals);
    count("faces", gs.faces);
    count("other_entities", gs.entities - gs.lineSegments - gs.circlesOrArcs -
                            gs.cubics - gs.periodicCubics - gs.workplanes);
    count("constraints", gs.constraints);
    return {{"count", gs.n}, {"items", items}, {"by_kind", counts}};
}

static void GetSelection(const json &args, ToolResult *res) {
    res->structured = SelectionJson(Verbose(args));
}

static void Select(const json &args, ToolResult *res) {
    if(!args.contains("handles")) throw ToolError("Give the handles to select");
    Handles h = ParseHandles(args["handles"]);
    if(!h.groups.empty()) {
        throw ToolError("Groups can't be selected; activate_group or list_entities instead");
    }
    for(hRequest hr : h.requests) h.entities.push_back(hr.entity(0));
    for(hEntity he : h.entities) {
        Entity *e = SK.GetEntity(he);
        if(!e->IsVisible() && !e->IsFace()) {
            throw ToolError(H(he) + " isn't visible, so it can't be selected (is its "
                            "group hidden, or are workplanes/normals hidden?)");
        }
    }
    std::string mode = args.value("mode", "replace");
    GraphicsWindow &gw = SS.GW;
    if(mode == "replace") {
        gw.ClearSelection();
    } else if(mode != "add" && mode != "remove") {
        throw ToolError("mode must be replace, add or remove");
    }
    for(hEntity he : h.entities) {
        GraphicsWindow::Selection s = {};
        s.entity = he;
        if(mode == "remove") {
            gw.MakeUnselected(&s, /*coincidentPointTrick=*/false);
        } else {
            gw.MakeSelected(&s);
        }
    }
    for(hConstraint hc : h.constraints) {
        GraphicsWindow::Selection s = {};
        s.constraint = hc;
        if(mode == "remove") {
            gw.MakeUnselected(&s, /*coincidentPointTrick=*/false);
        } else {
            gw.MakeSelected(&s);
        }
    }
    res->structured = SelectionJson(Verbose(args));
}

static void ClearSelection(const json &, ToolResult *res) {
    SS.GW.ClearSelection();
    res->structured = SelectionJson(false);
}

//-----------------------------------------------------------------------------
// pick_at: what is at a pixel of the last screenshot (or the window)
//-----------------------------------------------------------------------------
static json HitJson(const GraphicsWindow::Selection &s) {
    json j;
    if(s.entity.v) {
        Entity *e = SK.GetEntity(s.entity);
        j = EntityJson(e, false);
        if(e->IsFace()) {
            FaceInfo fi = GetFaceInfo(e->h);
            if(fi.onSurface) j["face"] = fi.Json();
        }
    } else if(s.constraint.v) {
        j = ConstraintJson(SK.GetConstraint(s.constraint));
    }
    return j;
}

static void PickAt(const json &args, ToolResult *res) {
    double x = Num(args.value("x", json()), "x");
    double y = Num(args.value("y", json()), "y");
    std::string source = args.value("source", "screenshot");
    GraphicsWindow &gw = SS.GW;

    ShotView shot = {};
    if(source == "screenshot") {
        if(lastScreenshot.empty()) {
            throw ToolError("No screenshot has been taken yet; take one, or use "
                            "source='window' for window coordinates");
        }
        int index = args.value("view_index", 0);
        if(index < 0 || index >= (int)lastScreenshot.size()) {
            throw ToolError("view_index must be 0.." +
                            std::to_string(lastScreenshot.size() - 1));
        }
        shot = lastScreenshot[index];
    } else if(source == "window") {
        if(!gw.window) throw ToolError("There is no window");
        shot.camera     = gw.GetCamera();
        shot.pixelRatio = 1;    // window coordinates are logical pixels
        shot.width      = (int)shot.camera.width;
        shot.height     = (int)shot.camera.height;
        shot.shaded     = gw.showShaded;
    } else {
        throw ToolError("source must be screenshot or window");
    }
    if(x < 0 || y < 0 || x > shot.width || y > shot.height) {
        throw ToolError("(" + std::to_string((int)x) + ", " + std::to_string((int)y) +
                        ") is outside the " + std::to_string(shot.width) + "x" +
                        std::to_string(shot.height) + " image");
    }
    // The GUI's mouse coordinates: logical pixels from the centre, y up.
    Point2d mp = { (x - shot.width / 2.0) / shot.pixelRatio,
                   (shot.height / 2.0 - y) / shot.pixelRatio };

    Regenerate();

    // Hit-test exactly as the mouse does, from the screenshot's camera.
    Vector savedOffset = gw.offset, savedRight = gw.projRight, savedUp = gw.projUp;
    double savedScale  = gw.scale;
    bool   savedShaded = gw.showShaded;
    GraphicsWindow::Selection savedHover = gw.hover;
    gw.offset     = shot.camera.offset;
    gw.projRight  = shot.camera.projRight;
    gw.projUp     = shot.camera.projUp;
    gw.scale      = shot.camera.scale;
    gw.showShaded = shot.shaded;

    gw.HitTestMakeSelection(mp);
    GraphicsWindow::Selection chosen = gw.hover;
    std::vector<GraphicsWindow::Hover> hovers;
    for(GraphicsWindow::Hover &h : gw.hoverList) hovers.push_back(h);

    // The solid's surface under the cursor, whether or not faces are
    // selectable in the GUI.
    Vector rayPoint = gw.UnProjectPoint3(Vector::From(mp.x, mp.y, 0.0));
    Vector rayDir   = gw.UnProjectPoint3(Vector::From(mp.x, mp.y, 1.0)).Minus(rayPoint);
    Group *ag = SK.GetGroup(gw.activeGroup);
    ag->GenerateDisplayItems();
    double bestT = VERY_NEGATIVE;
    uint32_t face = 0;
    Vector surfacePoint = {};
    for(const STriangle &tr : ag->displayMesh.l) {
        double t;
        Vector inters;
        if(!tr.Raytrace(rayPoint, rayDir, &t, &inters)) continue;
        if(t > bestT) {
            bestT = t;
            face = tr.meta.face;
            surfacePoint = inters;
        }
    }

    gw.offset     = savedOffset;
    gw.projRight  = savedRight;
    gw.projUp     = savedUp;
    gw.scale      = savedScale;
    gw.showShaded = savedShaded;
    gw.hover      = savedHover;
    gw.hoverList.Clear();
    // The entities' cached screen boxes are for the screenshot's view now.
    for(Entity &e : SK.entity) e.screenBBoxValid = false;

    json &r = res->structured;
    r["pixel"] = {x, y};
    r["view_plane_point"] = Vec(rayPoint);
    r["hit"] = chosen.IsEmpty() ? json() : HitJson(chosen);
    json candidates = json::array();
    std::sort(hovers.begin(), hovers.end(),
              [](const GraphicsWindow::Hover &a, const GraphicsWindow::Hover &b) {
                  return a.distance < b.distance;
              });
    for(GraphicsWindow::Hover &h : hovers) {
        if(candidates.size() >= 8) break;
        json c = h.selection.entity.v ? json(H(h.selection.entity))
                                      : json(H(h.selection.constraint));
        json cj = {{"handle", c}, {"distance_px", Round(h.distance)}};
        if(h.selection.entity.v) {
            Entity *e = SK.GetEntity(h.selection.entity);
            cj["type"]  = EntityTypeName(e->type);
            cj["group"] = H(e->group);
        } else {
            cj["type"] = ConstraintTypeName(SK.GetConstraint(h.selection.constraint)->type);
        }
        candidates.push_back(cj);
    }
    r["candidates"] = candidates;
    if(bestT > VERY_NEGATIVE) {
        json s = {{"point", Vec(surfacePoint)}};
        if(face) {
            s["face"] = H(hEntity{face});
            FaceInfo fi = GetFaceInfo(hEntity{face});
            if(fi.onSurface) s["normal"] = Vec(fi.normal);
        }
        r["surface"] = s;
    } else {
        r["surface"] = json();
    }

    std::string select = args.value("select", "none");
    if(select != "none") {
        GraphicsWindow::Selection s = chosen;
        if(s.IsEmpty() && face) s.entity = hEntity{face};
        if(s.IsEmpty()) throw ToolError("Nothing there to select");
        if(select == "replace") {
            gw.ClearSelection();
        } else if(select != "add") {
            throw ToolError("select must be none, replace or add");
        }
        gw.MakeSelected(&s);
        r["selection"] = SelectionJson(false);
    }
}

//-----------------------------------------------------------------------------
// measure
//-----------------------------------------------------------------------------
// A group whose solid has been generated: the active group or an earlier one.
static Group *GeneratedGroup(const json &args) {
    hGroup hg = args.contains("group") ? ParseGroup(args["group"]) : SS.GW.activeGroup;
    Group *g = SK.GetGroup(hg);
    if(g->order > SK.GetGroup(SS.GW.activeGroup)->order) {
        throw ToolError(H(hg) + " comes after the active group, so it isn't generated; "
                        "activate it (or a later group) first");
    }
    g->GenerateDisplayItems();
    return g;
}

static double MeshArea(const SMesh &m) {
    double a = 0;
    for(const STriangle &t : m.l) a += t.Area();
    return a;
}

static double EdgesLength(const SEdgeList &el) {
    double len = 0;
    for(const SEdge &e : el.l) len += e.b.Minus(e.a).Magnitude();
    return len;
}

static void Measure(const json &args, ToolResult *res) {
    Regenerate();
    Group *g = GeneratedGroup(args);
    json &r = res->structured;
    r["group"] = H(g->h);

    // The solid as of this group (what the GUI shows when it is active).
    SMesh *m = &g->displayMesh;
    if(!m->IsEmpty()) {
        double vol = m->CalculateVolume();
        json solid = {{"volume_mm3", Round(vol)},
                      {"surface_area_mm2", Round(MeshArea(*m))},
                      {"triangles", m->l.n}};
        if(fabs(vol) > LENGTH_EPS) solid["center_of_mass"] = Vec(m->GetCenterOfMass());
        Vector bmin = Vector::From(VERY_POSITIVE, VERY_POSITIVE, VERY_POSITIVE),
               bmax = Vector::From(VERY_NEGATIVE, VERY_NEGATIVE, VERY_NEGATIVE);
        for(const STriangle &t : m->l) {
            for(Vector p : {t.a, t.b, t.c}) {
                bmin = Vector::From(min(bmin.x, p.x), min(bmin.y, p.y), min(bmin.z, p.z));
                bmax = Vector::From(max(bmax.x, p.x), max(bmax.y, p.y), max(bmax.z, p.z));
            }
        }
        solid["bounds"] = {{"min", Vec(bmin)}, {"max", Vec(bmax)},
                           {"size", Vec(bmax.Minus(bmin))}};
        r["solid"] = solid;

        // This group's own contribution, before it is combined.
        SMesh cur = {};
        g->thisShell.TriangulateInto(&cur);
        if(cur.IsEmpty()) {
            for(const STriangle &t : g->thisMesh.l) cur.AddTriangle(&t);
        }
        double curVol = cur.CalculateVolume();
        if(curVol > 0) r["group_volume_mm3"] = Round(curVol);
        cur.Clear();
    }

    // The closed region sketched in this group.
    if(g->type == Group::Type::DRAWING_WORKPLANE) {
        if(g->polyError.how == PolyError::GOOD && !g->polyLoops.l.IsEmpty()) {
            SEdgeList sel = {};
            g->polyLoops.MakeEdgesInto(&sel);
            SPolygon sp = {};
            sel.AssemblePolygon(&sp, NULL, /*keepDir=*/true);
            sp.normal = sp.ComputeNormal();
            sp.FixContourDirections();
            r["sketch_region"] = {{"area_mm2", Round(fabs(sp.SignedArea()))},
                                  {"perimeter_mm", Round(EdgesLength(sel))},
                                  {"loops", sp.l.n}};
            sel.Clear();
            sp.Clear();
        } else {
            r["sketch_region"] = json();
            r["sketch_region_note"] = "This sketch has no correctly-formed closed region "
                                      "(it is open, not coplanar, or self-intersecting).";
        }
    }

    if(args.contains("faces")) {
        Handles h = ParseHandles(args["faces"]);
        std::vector<uint32_t> faces;
        json list = json::array();
        for(hEntity he : h.entities) {
            if(!SK.GetEntity(he)->IsFace()) throw ToolError(H(he) + " isn't a face");
            double a = m->CalculateSurfaceArea({he.v});
            list.push_back({{"face", H(he)}, {"area_mm2", Round(a)}});
            faces.push_back(he.v);
        }
        r["faces"] = list;
        r["faces_total_area_mm2"] = Round(m->CalculateSurfaceArea(faces));
    }

    if(args.contains("entities")) {
        Handles h = ParseHandles(args["entities"]);
        for(hRequest hr : h.requests) h.entities.push_back(hr.entity(0));
        json list = json::array();
        double total = 0;
        for(hEntity he : h.entities) {
            Entity *e = SK.GetEntity(he);
            if(e->IsPoint() || e->IsNormal() || e->IsWorkplane() || e->IsFace() ||
               e->IsDistance()) {
                throw ToolError(H(he) + " isn't a curve (line, arc, circle, cubic)");
            }
            double len = EdgesLength(*e->GetOrGenerateEdges());
            total += len;
            list.push_back({{"entity", H(he)}, {"length_mm", Round(len)}});
        }
        r["entities"] = list;
        r["entities_total_length_mm"] = Round(total);
    }
    r["note"] = "Curved surfaces and edges are approximated by triangles and line "
                "segments (about 1% error at the default chord tolerance).";
}

//-----------------------------------------------------------------------------
// check_naked_edges / check_interference
//-----------------------------------------------------------------------------
static void MeshCheck(const json &args, ToolResult *res, bool interference) {
    Regenerate();
    Group *g = GeneratedGroup(args);
    SMesh *m = &g->displayMesh;
    if(m->IsEmpty()) throw ToolError("There is no solid as of " + H(g->h));

    SEdgeList edges = {};
    SKdNode *root = SKdNode::From(m);
    bool inters = false, leaks = false;
    root->MakeCertainEdgesInto(&edges,
        interference ? EdgeKind::SELF_INTER : EdgeKind::NAKED_OR_SELF_INTER,
        /*coplanarIsInter=*/!interference, &inters, &leaks);

    json &r = res->structured;
    r["group"] = H(g->h);
    r["triangles"] = m->l.n;
    if(interference) {
        r["interferes"] = inters;
    } else {
        r["watertight"] = !leaks;
        r["self_intersecting"] = inters;
        r["surfaces"] = g->runningShell.surface.n;
    }
    r["problem_edges"] = edges.l.n;
    r["ok"] = edges.l.IsEmpty();
    int limit = args.value("limit", 20);
    json list = json::array();
    for(const SEdge &e : edges.l) {
        if((int)list.size() >= limit) break;
        list.push_back(json::array({Vec(e.a), Vec(e.b)}));
    }
    if(!list.empty()) r["edges"] = list;

    if(args.value("show", false)) {
        // Draw them in the window, as the Analyze menu does (Esc clears them).
        SS.nakedEdges.Clear();
        for(const SEdge &e : edges.l) SS.nakedEdges.AddEdge(e.a, e.b);
        r["shown"] = true;
    }
    edges.Clear();
}

static void CheckNakedEdges(const json &args, ToolResult *res) {
    MeshCheck(args, res, /*interference=*/false);
}

static void CheckInterference(const json &args, ToolResult *res) {
    MeshCheck(args, res, /*interference=*/true);
}

//-----------------------------------------------------------------------------
// export
//-----------------------------------------------------------------------------
static std::string Lower(std::string s) {
    for(char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static long FileSize(const Platform::Path &path) {
    FILE *f = OpenFile(path, "rb");
    if(!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return size;
}

// The solid's mesh, in the formats File > Export Triangle Mesh writes; as
// ExportMeshTo, but reporting naked edges in the result, not as an error.
static void ExportMesh(const Platform::Path &path, const std::string &ext, json *r) {
    Group *g = SK.GetGroup(SS.GW.activeGroup);
    g->GenerateDisplayItems();
    SMesh *m = &g->displayMesh;
    if(m->IsEmpty()) throw ToolError("The solid (as of the active group) is empty; "
                                     "nothing to export");
    SEdgeList edges = {};
    bool inters = false, leaks = false;
    SKdNode::From(m)->MakeCertainEdgesInto(&edges, EdgeKind::NAKED_OR_SELF_INTER,
                                           /*coplanarIsInter=*/true, &inters, &leaks);
    if(!edges.l.IsEmpty()) {
        (*r)["warning"] = "The mesh has " + std::to_string(edges.l.n) + " problem edges (" +
                          std::string(leaks ? "not watertight" : "watertight") + ", " +
                          (inters ? "self-intersecting" : "not self-intersecting") +
                          "); see check_naked_edges.";
    }
    edges.Clear();
    (*r)["triangles"] = m->l.n;

    FILE *f = OpenFile(path, "wb");
    if(!f) throw ToolError("Couldn't write to '" + path.raw + "'");
    if(ext == "stl") {
        SS.ExportMeshAsStlTo(f, m);
    } else if(ext == "obj") {
        Platform::Path mtl = path.WithExtension("mtl");
        FILE *fMtl = OpenFile(mtl, "wb");
        if(!fMtl) {
            fclose(f);
            throw ToolError("Couldn't write to '" + mtl.raw + "'");
        }
        fprintf(f, "mtllib %s\n", mtl.FileName().c_str());
        SS.ExportMeshAsObjTo(f, fMtl, m);
        fclose(fMtl);
        (*r)["material_file"] = mtl.raw;
    } else if(ext == "js" || ext == "html") {
        SS.ExportMeshAsThreeJsTo(f, path, m, &g->displayOutlines);
    } else if(ext == "wrl") {
        SS.ExportMeshAsVrmlTo(f, path, m);
    }
    fclose(f);
}

static void Export(const json &args, ToolResult *res) {
    std::string p = args.value("path", "");
    if(p.empty()) throw ToolError("Give the path to write");
    Platform::Path path = Platform::Path::From(p);
    if(!path.IsAbsolute()) throw ToolError("path must be absolute");
    std::string ext = Lower(path.Extension());
    if(ext.empty()) throw ToolError("path needs an extension, which picks the format");

    static const std::vector<std::string> meshExts = {"stl", "obj", "html", "js", "wrl"},
                                          viewExts = {"pdf", "eps", "ps", "svg", "dxf",
                                                      "step", "stp", "plt", "hpgl",
                                                      "ngc", "txt"};
    auto has = [&](const std::vector<std::string> &v) {
        return std::find(v.begin(), v.end(), ext) != v.end();
    };
    std::string kind = args.value("kind", "");
    if(kind.empty()) {
        if(ext == "png")               kind = "image";
        else if(ext == "step" || ext == "stp") kind = "solid";
        else if(has(meshExts))         kind = "mesh";
        else if(has(viewExts))         kind = "view";
        else throw ToolError("Unknown file type '." + ext + "'; use step/stp, stl, obj, "
                             "html, js, wrl, svg, pdf, eps, ps, dxf, plt, hpgl, ngc, txt or png");
    }
    bool ok;
    if(kind == "solid")          ok = (ext == "step" || ext == "stp");
    else if(kind == "mesh")      ok = has(meshExts);
    else if(kind == "view" || kind == "section") ok = has(viewExts);
    else if(kind == "wireframe") ok = (ext == "step" || ext == "stp" || ext == "dxf");
    else if(kind == "image")     ok = (ext == "png");
    else throw ToolError("kind must be solid, mesh, view, section, wireframe or image");
    if(!ok) throw ToolError("A ." + ext + " file can't hold a '" + kind + "' export");

    if(!args.value("overwrite", false) && FileSize(path) >= 0) {
        throw ToolError("'" + path.raw + "' exists; pass overwrite=true to replace it");
    }

    std::string view = args.value("view", "current");
    if(view != "current" && kind != "view" && kind != "section" && kind != "image") {
        throw ToolError("view only applies to view, section and image exports");
    }
    if(view != "current") {
        Vector r, u;
        ViewPreset(view, &r, &u);   // validates
    }

    json &r = res->structured;
    r["path"] = path.raw;
    r["kind"] = kind;
    r["group"] = H(SS.GW.activeGroup);

    Regenerate();
    if(kind == "image") {
        RenderOptions o = {};
        o.width  = args.value("width", 1600);
        o.height = args.value("height", 1200);
        if(o.width < 16 || o.width > 8192 || o.height < 16 || o.height > 8192) {
            throw ToolError("width and height must be between 16 and 8192");
        }
        o.zoomToFit = args.value("zoom_to_fit", view != "current");
        o.mmUnits   = args.value("units", "mm") == "mm";
        json vj;
        std::shared_ptr<Pixmap> pixmap = RenderOffscreen(view, o, &vj);
        if(!pixmap->WritePng(path, /*flip=*/true)) {
            throw ToolError("Couldn't write to '" + path.raw + "'");
        }
        r["view"] = vj;
        r["width"] = o.width;
        r["height"] = o.height;
        r["bytes"] = FileSize(path);
        return;
    }

    GraphicsWindow &gw = SS.GW;
    Vector savedOffset = gw.offset, savedRight = gw.projRight, savedUp = gw.projUp;
    auto savedConstraints = gw.showConstraints;
    std::vector<GraphicsWindow::Selection> savedSelection;
    for(GraphicsWindow::Selection &s : gw.selection) savedSelection.push_back(s);
    bool wasExportMode = SS.exportMode;

    auto restore = [&]() {
        gw.offset = savedOffset;
        gw.projRight = savedRight;
        gw.projUp = savedUp;
        gw.showConstraints = savedConstraints;
        gw.ClearSelection();
        for(GraphicsWindow::Selection &s : savedSelection) gw.MakeSelected(&s);
        SS.justExportedInfo.draw = false;
        if(!wasExportMode && SS.exportMode) {
            // Export regenerates at the export chord tolerance; go back to
            // the display tolerance, as Esc does after a GUI export.
            SS.exportMode = false;
            SS.GenerateAll(SolveSpaceUI::Generate::ALL);
        }
        gw.persistentDirty = true;
    };

    try {
        if(view != "current") {
            Vector right, up;
            ViewPreset(view, &right, &up);
            gw.projRight = right;
            gw.projUp    = up;
        }
        if(!args.value("constraints", false)) {
            gw.showConstraints = GraphicsWindow::ShowConstraintMode::SCM_NOSHOW;
        }
        gw.ClearSelection();

        if(kind == "solid") {
            StepFileWriter sfw = {};
            sfw.ExportSurfacesTo(path);
        } else if(kind == "mesh") {
            SS.exportMode = true;
            SS.GenerateAll(SolveSpaceUI::Generate::ALL);
            ExportMesh(path, ext, &r);
        } else if(kind == "view") {
            SS.ExportViewOrWireframeTo(path, /*exportWireframe=*/false);
        } else if(kind == "wireframe") {
            SS.ExportViewOrWireframeTo(path, /*exportWireframe=*/true);
        } else if(kind == "section") {
            if(args.contains("face")) {
                hEntity face = ParseEntity(args["face"]);
                if(!SK.GetEntity(face)->IsFace()) throw ToolError(H(face) + " isn't a face");
                gw.MakeSelected(face);
            } else if(!gw.LockedInWorkplane()) {
                throw ToolError("A section is taken in the active workplane, or in a "
                                "face's plane; give face, or activate a workplane sketch");
            }
            SS.ExportSectionTo(path);
        }
    } catch(...) {
        restore();
        throw;
    }
    restore();
    ThrowIfErrors();

    long size = FileSize(path);
    if(size < 0) throw ToolError("Nothing was written to '" + path.raw + "'");
    r["bytes"] = size;
    if(kind == "view" || kind == "section") {
        r["view"] = view;
        r["note"] = "2D exports are projections onto the view plane (the GUI's camera "
                    "unless view is given), scaled by the export scale in the "
                    "configuration screen.";
    }
}

//-----------------------------------------------------------------------------
// run_command: menu commands that don't open dialogs or wait for the mouse
//-----------------------------------------------------------------------------
struct CommandEntry {
    const char *name;
    Command     cmd;
    bool        modifies;
    const char *desc;
};

static const CommandEntry COMMANDS[] = {
    // Edit
    { "regenerate_all",     Command::REGEN_ALL,        false, "Edit > Regenerate All" },
    { "snap_to_grid",       Command::SNAP_TO_GRID,     true,  "snap the selected points to the grid" },
    { "rotate_imported_90", Command::ROTATE_90,        true,  "rotate the selected linked group 90 degrees" },
    { "delete",             Command::DELETE,           true,  "delete the selection" },
    { "select_edge_chain",  Command::SELECT_CHAIN,     false, "select the chain of edges joined to the selection" },
    { "select_all",         Command::SELECT_ALL,       false, "select every entity in the active group" },
    { "unselect_all",       Command::UNSELECT_ALL,     false, "clear the selection (as Esc)" },
    // View
    { "zoom_in",            Command::ZOOM_IN,          false, "" },
    { "zoom_out",           Command::ZOOM_OUT,         false, "" },
    { "zoom_to_fit",        Command::ZOOM_TO_FIT,      false, "" },
    { "align_view_to_workplane", Command::ONTO_WORKPLANE, false, "" },
    { "nearest_ortho_view", Command::NEAREST_ORTHO,    false, "" },
    { "nearest_iso_view",   Command::NEAREST_ISO,      false, "" },
    { "center_view_at_point", Command::CENTER_VIEW,    false, "needs one selected point" },
    { "toggle_snap_grid",   Command::SHOW_GRID,        false, "" },
    { "toggle_dim_solid",   Command::DIM_SOLID_MODEL,  false, "" },
    { "toggle_perspective", Command::PERSPECTIVE_PROJ, false, "" },
    { "toggle_exploded_view", Command::EXPLODE_SKETCH, false, "" },
    { "toggle_toolbar",     Command::SHOW_TOOLBAR,     false, "" },
    { "toggle_property_browser", Command::SHOW_TEXT_WND, false, "" },
    // New group (using the selection, as the menu does)
    { "group_sketch_3d",    Command::GROUP_3D,         true,  "" },
    { "group_sketch_workplane", Command::GROUP_WRKPL,  true,  "workplane from the selection" },
    { "group_step_translating", Command::GROUP_TRANS,  true,  "" },
    { "group_step_rotating", Command::GROUP_ROT,       true,  "needs an axis or point+normal selected" },
    { "group_extrude",      Command::GROUP_EXTRUDE,    true,  "" },
    { "group_helix",        Command::GROUP_HELIX,      true,  "needs an axis selected" },
    { "group_lathe",        Command::GROUP_LATHE,      true,  "needs an axis selected" },
    { "group_revolve",      Command::GROUP_REVOLVE,    true,  "needs an axis selected" },
    // Sketch
    { "sketch_in_workplane", Command::SEL_WORKPLANE,   true,  "draw in the active group's workplane" },
    { "sketch_in_3d",       Command::FREE_IN_3D,       true,  "draw anywhere in 3D" },
    { "toggle_construction", Command::CONSTRUCTION,    true,  "on the selected entities" },
    { "tangent_arc_at_point", Command::TANGENT_ARC,    true,  "needs a point joining two curves selected" },
    { "split_curves",       Command::SPLIT_CURVES,     true,  "at the selected intersection" },
    // Constrain (all use the selection)
    { "distance_diameter",  Command::DISTANCE_DIA,     true,  "" },
    { "reference_dimension", Command::REF_DISTANCE,    true,  "" },
    { "angle",              Command::ANGLE,            true,  "" },
    { "reference_angle",    Command::REF_ANGLE,        true,  "" },
    { "other_supplementary_angle", Command::OTHER_ANGLE, true, "" },
    { "toggle_reference_dimension", Command::REFERENCE, true, "" },
    { "horizontal",         Command::HORIZONTAL,       true,  "" },
    { "vertical",           Command::VERTICAL,         true,  "" },
    { "on_point_curve_plane", Command::ON_ENTITY,      true,  "" },
    { "equal",              Command::EQUAL,            true,  "" },
    { "ratio",              Command::RATIO,            true,  "" },
    { "difference",         Command::DIFFERENCE,       true,  "" },
    { "at_midpoint",        Command::AT_MIDPOINT,      true,  "" },
    { "symmetric",          Command::SYMMETRIC,        true,  "" },
    { "parallel_tangent",   Command::PARALLEL,         true,  "" },
    { "perpendicular",      Command::PERPENDICULAR,    true,  "" },
    { "same_orientation",   Command::ORIENTED_SAME,    true,  "" },
    { "lock_point",         Command::WHERE_DRAGGED,    true,  "" },
    // Analyze (results come back as messages)
    { "measure_volume",     Command::VOLUME,           false, "" },
    { "measure_area",       Command::AREA,             false, "" },
    { "measure_perimeter",  Command::PERIMETER,        false, "" },
    { "show_interfering_parts", Command::INTERFERENCE, false, "" },
    { "show_naked_edges",   Command::NAKED_EDGES,      false, "" },
    { "show_center_of_mass", Command::CENTER_OF_MASS,  false, "" },
    { "show_underconstrained_points", Command::SHOW_DOF, false, "" },
    { "trace_point",        Command::TRACE_PT,         false, "needs one selected point" },
};

static std::set<uint32_t> GroupSet() {
    std::set<uint32_t> s;
    for(Group &g : SK.group) s.insert(g.h.v);
    return s;
}
static std::set<uint32_t> RequestSet() {
    std::set<uint32_t> s;
    for(Request &r : SK.request) s.insert(r.h.v);
    return s;
}
static std::set<uint32_t> ConstraintSet() {
    std::set<uint32_t> s;
    for(Constraint &c : SK.constraint) s.insert(c.h.v);
    return s;
}

static void RunCommand(const json &args, ToolResult *res) {
    std::string name = args.value("name", "");
    const CommandEntry *ce = NULL;
    for(const CommandEntry &c : COMMANDS) {
        if(name == c.name) ce = &c;
    }
    if(ce == NULL) {
        throw ToolError("Unknown or unsupported command '" + name + "'. Commands that open "
                        "dialogs or wait for mouse clicks aren't available; see the "
                        "tool's description for the list.");
    }
    GraphicsWindow &gw = SS.GW;
    if(args.contains("handles")) {
        Handles h = ParseHandles(args["handles"]);
        for(hRequest hr : h.requests) h.entities.push_back(hr.entity(0));
        if(!h.groups.empty()) throw ToolError("Groups can't be selected");
        SelectOnly(h);
    }

    std::set<uint32_t> groupsBefore = GroupSet(), requestsBefore = RequestSet(),
                       constraintsBefore = ConstraintSet();
    bool savedImmediate = SS.immediatelyEditDimension;
    SS.immediatelyEditDimension = false;
    {
        std::unique_ptr<Transaction> t;
        if(ce->modifies) t.reset(new Transaction());
        try {
            gw.ActivateCommand(ce->cmd);
        } catch(...) {
            SS.immediatelyEditDimension = savedImmediate;
            throw;
        }
        SS.immediatelyEditDimension = savedImmediate;
        // Stop anything that would wait for the mouse (e.g. a pending
        // comment); tools can't click.
        if(gw.pending.operation != GraphicsWindow::Pending::NONE) gw.ClearPending();
        ThrowIfErrors();
        if(ce->modifies) {
            Regenerate();
            t->Commit();
        }
    }

    json &r = res->structured;
    r["command"] = ce->name;
    json added = json::object();
    json gs = json::array(), rs = json::array(), cs = json::array();
    for(Group &g : SK.group) if(!groupsBefore.count(g.h.v)) gs.push_back(H(g.h));
    for(Request &q : SK.request) if(!requestsBefore.count(q.h.v)) rs.push_back(H(q.h));
    for(Constraint &c : SK.constraint) {
        if(!constraintsBefore.count(c.h.v)) cs.push_back(ConstraintJson(&c));
    }
    if(!gs.empty()) added["groups"] = gs;
    if(!rs.empty()) added["requests"] = rs;
    if(!cs.empty()) added["constraints"] = cs;
    if(!added.empty()) r["added"] = added;
    r["active_group"] = H(gw.activeGroup);
    r["active_solve"] = SolveStatus(gw.activeGroup);
    r["selection"] = SelectionJson(false);
}

void AddAnalysisTools(std::vector<Tool> *tools) {
    tools->push_back({
        "get_selection",
        "What is selected in SolveSpace's window (by the user, or by select/pick_at): "
        "each entity, face or constraint, and counts by kind. Use it when the user "
        "says \"this\" or \"what I've selected\"; add_constraint {use_selection:true} "
        "and run_command act on the selection. Other tools that drive the GUI's "
        "menus may replace the selection.",
        {{"type", "object"}, {"properties", {
            {"verbose", BoolProp(false, "Full entity detail.")},
        }}},
        GetSelection,
    });
    tools->push_back({
        "select",
        "Select entities (points, curves, faces, workplanes, normals; a request "
        "handle selects its main entity) and constraints in SolveSpace's window, so "
        "the user can see them or a run_command acts on them.",
        {{"type", "object"}, {"properties", {
            {"handles", HandleListSchema("Entity, request or constraint handles.")},
            {"mode", {{"type", "string"}, {"enum", {"replace", "add", "remove"}},
                      {"default", "replace"}}},
            {"verbose", BoolProp(false, "Full entity detail.")},
        }}, {"required", {"handles"}}},
        Select,
    });
    tools->push_back({
        "clear_selection",
        "Clear the selection in SolveSpace's window.",
        {{"type", "object"}, {"properties", json::object()}},
        ClearSelection,
    });
    tools->push_back({
        "pick_at",
        "What is at pixel (x, y) of the last screenshot (view_index picks the image "
        "of a multi-view one), or of the window (source='window', logical pixels "
        "from the top left): the entity or constraint a mouse click there would "
        "pick (hit), other nearby candidates, and the solid's surface under it (the "
        "3D point and the face, if any). select='replace'/'add' also selects the hit.",
        {{"type", "object"}, {"properties", {
            {"x", NumProp("Pixels from the left edge.")},
            {"y", NumProp("Pixels from the top edge.")},
            {"view_index", IntProp(0, 5, 0, "Which image of the last screenshot.")},
            {"source", {{"type", "string"}, {"enum", {"screenshot", "window"}},
                        {"default", "screenshot"}}},
            {"select", {{"type", "string"}, {"enum", {"none", "replace", "add"}},
                        {"default", "none"}}},
        }}, {"required", {"x", "y"}}},
        PickAt,
    });
    tools->push_back({
        "measure",
        "Measure the model as of a group (default: the active group): the solid's "
        "volume, surface area, centre of mass and bounds, the group's own volume, "
        "the area and perimeter of a closed sketch region, and optionally the areas "
        "of faces and lengths of curves. mm, mm², mm³.",
        {{"type", "object"}, {"properties", {
            {"group", StrProp("Group handle; must be the active group or an earlier one.")},
            {"faces", HandleListSchema("Face entities to measure the area of.")},
            {"entities", HandleListSchema("Curves (or requests) to measure the length of.")},
        }}},
        Measure,
    });
    json checkProps = {
        {"group", StrProp("Group handle (default: active); the active group or an earlier one.")},
        {"limit", IntProp(0, 1000, 20, "How many problem edges to list.")},
        {"show", BoolProp(false, "Also highlight the problem edges in the window "
                                 "(until the user presses Esc).")},
    };
    tools->push_back({
        "check_naked_edges",
        "Check the solid's mesh (as of a group, default the active one) is watertight "
        "and not self-intersecting, as Analyze > Show Naked Edges does; lists the "
        "problem edges. A valid solid is needed for STEP/STL export and booleans.",
        {{"type", "object"}, {"properties", checkProps}},
        CheckNakedEdges,
    });
    tools->push_back({
        "check_interference",
        "Check whether the parts of an assembly (groups combined with 'assemble') "
        "overlap, as Analyze > Show Interfering Parts does; lists interfering edges.",
        {{"type", "object"}, {"properties", checkProps}},
        CheckInterference,
    });
    tools->push_back({
        "export",
        "Write the model to a file, without any dialog. The format comes from the "
        "extension; kind chooses what is exported: 'solid' (STEP surfaces: .step/.stp), "
        "'mesh' (.stl, .obj, .html, .js, .wrl), 'view' (2D projection of the view: "
        ".svg, .pdf, .eps, .ps, .dxf, .step, .plt/.hpgl, .ngc/.txt G-code), 'section' "
        "(the outline of the solid's faces lying in the active workplane or a face's "
        "plane, in that plane's coordinates; not a cut through the solid), "
        "'wireframe' (3D curves: .dxf, .step), "
        "'image' (.png, rendered offscreen). Defaults: .step/.stp solid, mesh formats "
        "mesh, vector formats view, .png image. Exports the solid as of the active "
        "group. Refuses to replace an existing file unless overwrite=true.",
        {{"type", "object"}, {"properties", {
            {"path", StrProp("Absolute path of the file to write.")},
            {"kind", {{"type", "string"}, {"enum", {"solid", "mesh", "view", "section",
                                                     "wireframe", "image"}}}},
            {"overwrite", BoolProp(false, "Replace an existing file.")},
            {"view", StrProp("For view/section/image: 'current' (the GUI's camera, "
                             "default) or a preset: iso, front, back, left, right, top, "
                             "bottom.")},
            {"constraints", BoolProp(false, "Include constraints/dimensions in a "
                                            "view export (as shown in the window).")},
            {"face", StrProp("For section: the face whose plane to export.")},
            {"width", IntProp(16, 8192, 1600, "Image width, pixels.")},
            {"height", IntProp(16, 8192, 1200, "Image height, pixels.")},
            {"zoom_to_fit", BoolProp(true, "Image: fit the model (default for presets).")},
        }}, {"required", {"path"}}},
        Export,
    });
    std::string list;
    for(const CommandEntry &c : COMMANDS) {
        if(!list.empty()) list += ", ";
        list += c.name;
        if(*c.desc) list += std::string(" (") + c.desc + ")";
    }
    json names = json::array();
    for(const CommandEntry &c : COMMANDS) names.push_back(c.name);
    tools->push_back({
        "run_command",
        "Run one of SolveSpace's menu commands, as if chosen from the menu, on the "
        "current selection (or first select the given handles). An escape hatch for "
        "what no other tool does; commands that open dialogs or wait for mouse "
        "clicks are not available. Analyze results come back as messages. "
        "Commands: " + list + ".",
        {{"type", "object"}, {"properties", {
            {"name", {{"type", "string"}, {"enum", names}}},
            {"handles", HandleListSchema("Select exactly these first.")},
        }}, {"required", {"name"}}},
        RunCommand,
    });
}

} // namespace Mcp
} // namespace SolveSpace
