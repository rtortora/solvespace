//-----------------------------------------------------------------------------
// Shared helpers for the MCP tool implementations (tools_*.cpp). Everything
// here runs on the GUI thread.
//
// Conventions: lengths in mm, angles in degrees; handles are strings with a
// type prefix ("g:0002", "r:0004", "e:00040001", "c:0007").
//-----------------------------------------------------------------------------
#ifndef SOLVESPACE_MCP_TOOLS_H
#define SOLVESPACE_MCP_TOOLS_H

#include "solvespace.h"
#include "mcp.h"

namespace SolveSpace {
namespace Mcp {

// Each tools_*.cpp file adds its tools to the registry.
void AddModelTools(std::vector<Tool> *tools);
void AddDocumentTools(std::vector<Tool> *tools);
void AddSketchTools(std::vector<Tool> *tools);
void AddConstraintTools(std::vector<Tool> *tools);
void AddOperationTools(std::vector<Tool> *tools);
void AddViewTools(std::vector<Tool> *tools);
void AddAnalysisTools(std::vector<Tool> *tools);

//-----------------------------------------------------------------------------
// Handles
//-----------------------------------------------------------------------------
std::string H(hGroup h);
std::string H(hRequest h);
std::string H(hEntity h);
std::string H(hConstraint h);
std::string H(hStyle h);
// A model value rounded to 1e-6 (mm or degrees), for results.
double Round(double v);
std::string ColorHex(RgbaColor c);

// Parse a handle string, checking that it exists.
hGroup      ParseGroup(const json &j);
hEntity     ParseEntity(const json &j);
hRequest    ParseRequest(const json &j);
hConstraint ParseConstraint(const json &j);
// Parse a list of handles of any kind into a selection.
struct Handles {
    std::vector<hEntity>     entities;
    std::vector<hConstraint> constraints;
    std::vector<hRequest>    requests;
    std::vector<hGroup>      groups;
};
Handles ParseHandles(const json &j);

//-----------------------------------------------------------------------------
// Arguments
//-----------------------------------------------------------------------------
double Num(const json &j, const char *what);
void   Point2(const json &j, const char *what, double *u, double *v);
Vector Vec3(const json &j, const char *what);
json   Vec(Vector v);   // rounded
// The "verbose" argument: full results instead of compact ones.
bool   Verbose(const json &args);
// An axis direction: "+z", "-x", ... or a vector [x, y, z]; unit length.
Vector AxisArg(const json &j, const char *what);

// The frame of a workplane entity: origin and in-plane axes.
struct Frame {
    Vector origin, u, v, n;
    Vector To3d(double pu, double pv) const {
        return origin.Plus(u.ScaledBy(pu)).Plus(v.ScaledBy(pv));
    }
    json To2d(Vector p) const {
        Vector d = p.Minus(origin);
        return json::array({d.Dot(u), d.Dot(v)});
    }
    double U(Vector p) const { return p.Minus(origin).Dot(u); }
    double V(Vector p) const { return p.Minus(origin).Dot(v); }
};
inline json Uv(const Frame &f, Vector p) { return json::array({Round(f.U(p)), Round(f.V(p))}); }
Frame FrameOf(hEntity wrkpl);

// A point given by a tool caller, in the coordinates of the active group:
// [u, v] in the active workplane, or [x, y, z] when sketching in 3D.
Vector SketchPoint(const json &j, const char *what);

//-----------------------------------------------------------------------------
// Results
//-----------------------------------------------------------------------------
const char *GroupTypeName(Group::Type t);
const char *EntityTypeName(Entity::Type t);
std::string ConstraintTypeName(Constraint::Type t);
json SolveStatus(hGroup hg);
json GroupJson(Group *g);
json ActiveWorkplaneJson();
// Compact by default: points give [u, v] if in a workplane, else [x, y, z].
json EntityJson(Entity *e, bool verbose = false);
json ConstraintJson(Constraint *c);
// A request: compact, its main entity and points; verbose, every entity
// it generates (skipping normals and distances).
json RequestJson(hRequest hr, bool verbose = false);

// Where a face is on the solid (as of the active group), from the mesh.
struct FaceInfo {
    hEntity face;
    hGroup  group;
    Vector  normal;         // out of the solid, if onSurface
    Vector  point;          // a point on the face's plane
    bool    onSurface = false;
    Vector  center, bmin, bmax;
    double  area = 0;
    json Json() const;
};
FaceInfo GetFaceInfo(hEntity face);
std::vector<FaceInfo> GetFaceInfos(const std::vector<hEntity> &faces);

json IntProp(int lo, int hi, int dflt, const char *desc);
json BoolProp(bool dflt, const char *desc);
json StrProp(const char *desc);
json NumProp(const char *desc);
json Point2Schema(const char *desc);
json PointSchema(const char *desc);
json Vec3Schema(const char *desc);
json HandleListSchema(const char *desc);

//-----------------------------------------------------------------------------
// Model changes
//-----------------------------------------------------------------------------
// Solve, and refresh everything that depends on the sketch.
void Regenerate();
// The same, but also solving the given group if it comes after the active one.
void RegenerateFor(hGroup hg);

// Records an undo step and, unless Commit() is called, rolls it back on
// destruction, so that a tool that fails part-way leaves the model as it was.
class Transaction {
public:
    Transaction();
    ~Transaction();
    void Commit() { committed = true; }
private:
    int  undoDepth;
    bool wasUnsaved;
    bool committed;
};

// Selects exactly the given items in the graphics window.
void SelectOnly(const Handles &h);

// The camera directions of a named view: iso, front, back, left, right,
// top, bottom (Z up; front looks from -Y). Throws for other names.
void ViewPreset(const std::string &name, Vector *right, Vector *up);
GraphicsWindow::DrawOccludedAs HiddenLinesArg(const json &j);

// Offscreen rendering (tools_model.cpp), used by screenshot and export.
struct RenderOptions {
    int         width, height;
    bool        zoomToFit;
    bool        mmUnits;
    bool        setShaded;
    bool        shaded;
    bool        setOccluded;
    GraphicsWindow::DrawOccludedAs occluded;
    Handles     highlight;
};
// Renders from a view preset, or "current" for the GUI's camera, leaving the
// GUI's view, display settings and selection as they were.
std::shared_ptr<Pixmap> RenderOffscreen(const std::string &view, const RenderOptions &o,
                                        json *viewJson, Camera *usedCamera = NULL);
std::vector<uint8_t> EncodePng(const std::shared_ptr<Pixmap> &pixmap);

// The cameras of the last screenshot's images, for pick_at.
struct ShotView {
    Camera camera;          // logical pixels; image pixels = logical * pixelRatio
    int    width, height;   // of the image, in pixels
    double pixelRatio;
    bool   shaded;
};
extern std::vector<ShotView> lastScreenshot;

// Throws a ToolError if anything was captured by Error() since the tool
// started; used after calling into GUI menu functions.
void ThrowIfErrors();

// Throws unless the active group is a sketch (workplane or 3D).
void RequireSketchGroup();
void RequireWorkplane(const char *what);

} // namespace Mcp
} // namespace SolveSpace

#endif
