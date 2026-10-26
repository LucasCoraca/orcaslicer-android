// The Mesh Boolean gizmo (GLGizmoMeshBoolean): pick a source and a tool part of the selected instance by tapping them,
// union / difference / intersection with libslic3r's MeshBoolean::mcut, the result replaces the source part.
#include <GLES3/gl3.h>
#include <optional>

#include "Core.hpp"
#include "Plater.hpp"
#include "Viewport.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;

namespace orca {

namespace {

const std::string warning_text = _u8L("Unable to perform boolean operation on selected parts");

enum class Operation { Undef, Union, Difference, Intersection };
enum class Selecting { Undef, SelectSource, SelectTool };

struct VolumeInfo
{
    ModelVolume *mv{nullptr};
    int          volume_idx{-1};
    Transform3d  trafo;
    void reset() { mv = nullptr; volume_idx = -1; trafo = Transform3d::Identity(); }
    std::optional<TriangleSelector::SavedPainting> save_painting() const
    {
        if (Core::get().app_config->get_bool("keep_painting")) {
            std::optional<TriangleSelector::SavedPainting> saved_painting = mv->save_painting();
            if (saved_painting) saved_painting->mesh.transform(trafo);
            return saved_painting;
        }
        return {};
    }
};

struct State
{
    bool        active{false};
    int         obj{-1}, inst{0};
    Operation   mode{Operation::Undef};
    Selecting   selecting{Selecting::Undef};
    bool        diff_delete_input{false}, inter_delete_input{false};
    VolumeInfo  src, tool;
    // what the GL thread draws (render_bounding_box of both parts)
    std::mutex  mutex;
    BoundingBoxf3 src_bb, tool_bb;
} g;

// on_is_activable: a single full instance with more than one volume
bool single_instance(int &obj, int &inst)
{
    Plater &p = Plater::get();
    const Model &m = p.model();
    std::set<std::pair<int, int>> insts;
    for (const SelItem &s : p.selection) {
        if (s.vol >= 0 || s.obj < 0 || s.obj >= int(m.objects.size())) return false;
        if (s.inst < 0) for (int i = 0; i < int(m.objects[s.obj]->instances.size()); ++i) insts.insert({s.obj, i});
        else insts.insert({s.obj, s.inst});
    }
    if (insts.size() != 1) return false;
    std::tie(obj, inst) = *insts.begin();
    return m.objects[obj]->volumes.size() > 1;
}

// the boxes the GL thread draws, after any change (core lock held)
void update_boxes()
{
    std::lock_guard<std::mutex> l(g.mutex);
    g.src_bb = g.tool_bb = BoundingBoxf3();
    const Model &m = Plater::get().model();
    if (!g.active || g.obj < 0 || g.obj >= int(m.objects.size())) return;
    const ModelObject *mo = m.objects[g.obj];
    const Transform3d inst = mo->instances[g.inst]->get_matrix();
    // GLVolume::transformed_convex_hull_bounding_box
    if (g.src.mv) g.src_bb = g.src.mv->get_convex_hull().transformed_bounding_box(inst * g.src.mv->get_matrix());
    if (g.tool.mv) g.tool_bb = g.tool.mv->get_convex_hull().transformed_bounding_box(inst * g.tool.mv->get_matrix());
}

void reset_all(Operation mode, Selecting selecting)
{
    g.src.reset();
    g.tool.reset();
    g.mode = mode;
    g.selecting = selecting;
}

json state()
{
    Plater &p = Plater::get();
    if (!g.active) return {{"active", false}};
    const bool diff = g.mode == Operation::Difference;
    json st = {{"active", true}, {"mode", g.mode == Operation::Union ? "union" : diff ? "difference" : "intersection"},
               {"selecting", g.selecting == Selecting::SelectSource ? "source" : g.selecting == Selecting::SelectTool ? "tool" : ""},
               {"src", g.src.mv ? g.src.mv->name : ""}, {"tool", g.tool.mv ? g.tool.mv->name : ""},
               {"delete_input", g.mode == Operation::Intersection ? g.inter_delete_input : g.diff_delete_input},
               {"labels", {{"union", _u8L("Union")}, {"difference", _u8L("Difference")}, {"intersection", _u8L("Intersection")},
                           {"cap1", diff ? _u8L("Subtract from") : _u8L("Part 1")}, {"cap2", diff ? _u8L("Subtract with") : _u8L("Part 2")},
                           {"selected", _u8L("1 selected")}, {"select", _u8L("Select")}, {"delete_input", _u8L("Delete input")}}}};
    (void) p;
    return st;
}

// GLGizmoMeshBoolean::on_set_state / on_render's object change
json open(const json &)
{
    int obj, inst;
    g.active = single_instance(obj, inst);
    if (g.active) {
        g.obj = obj;
        g.inst = inst;
        reset_all(Operation::Union, Selecting::SelectSource);
    } else
        reset_all(Operation::Undef, Selecting::Undef);
    g.diff_delete_input = g.inter_delete_input = false;
    update_boxes();
    Viewport::get().request_redraw();
    return state();
}

json close(const json &)
{
    g.active = false;
    reset_all(Operation::Undef, Selecting::Undef);
    update_boxes();
    Viewport::get().request_redraw();
    return state();
}

json set(const json &a)
{
    if (!g.active) return state();
    if (a.contains("mode")) {
        const std::string m = a["mode"];
        g.mode = m == "difference" ? Operation::Difference : m == "intersection" ? Operation::Intersection : Operation::Union;
    }
    if (a.contains("selecting")) g.selecting = a["selecting"] == "tool" ? Selecting::SelectTool : Selecting::SelectSource;
    if (a.contains("delete_input")) (g.mode == Operation::Intersection ? g.inter_delete_input : g.diff_delete_input) = a["delete_input"];
    if (a.value("reset_src", false)) g.src.reset();
    if (a.value("reset_tool", false)) g.tool.reset();
    update_boxes();
    Viewport::get().request_redraw();
    return state();
}

// gizmo_event(LeftDown): the closest volume of the instance under the finger (modifiers too)
json tap(const json &a)
{
    if (!g.active) return state();
    const Model &m = Plater::get().model();
    if (g.obj >= int(m.objects.size())) return state();
    ModelObject *mo = m.objects[g.obj];
    const ModelInstance *mi = mo->instances[g.inst];
    Vec3d o, d;
    Viewport::get().camera().ray(a.at("x").get<double>(), a.at("y").get<double>(), o, d);
    double best = std::numeric_limits<double>::max();
    int hit = -1;
    for (int vi = 0; vi < int(mo->volumes.size()); ++vi) {
        const Transform3d t = mi->get_transformation().get_matrix() * mo->volumes[vi]->get_matrix();
        const Transform3d inv = t.inverse();
        const Vec3d lo = inv * o, ld = inv.linear() * d;
        const indexed_triangle_set &its = mo->volumes[vi]->mesh().its;
        for (const Vec3i32 &f : its.indices) {
            const Vec3d p0 = its.vertices[f[0]].cast<double>(), e1 = its.vertices[f[1]].cast<double>() - p0, e2 = its.vertices[f[2]].cast<double>() - p0;
            const Vec3d pv = ld.cross(e2);
            const double det = e1.dot(pv);
            if (std::abs(det) < 1e-12) continue;
            const Vec3d tv = lo - p0;
            const double u = tv.dot(pv) / det;
            if (u < 0 || u > 1) continue;
            const Vec3d q = tv.cross(e1);
            const double v = ld.dot(q) / det;
            if (v < 0 || u + v > 1) continue;
            const double tt = e2.dot(q) / det;
            if (tt <= 0) continue;
            const double dist = (t * Vec3d(lo + tt * ld) - o).squaredNorm();
            if (dist < best) { best = dist; hit = vi; }
        }
    }
    if (hit < 0) return state();
    if (g.selecting == Selecting::SelectTool) {
        g.tool.trafo = mo->volumes[hit]->get_matrix();
        g.tool.volume_idx = hit;
        g.tool.mv = mo->volumes[hit];
    } else if (g.selecting == Selecting::SelectSource) {
        g.src.trafo = mo->volumes[hit]->get_matrix();
        g.src.volume_idx = hit;
        g.src.mv = mo->volumes[hit];
        g.selecting = Selecting::SelectTool;
    }
    update_boxes();
    Viewport::get().request_redraw();
    return state();
}

// generate_new_volume
void generate_new_volume(bool delete_input, TriangleMesh &mesh_result, const std::vector<std::optional<TriangleSelector::SavedPainting>> &saved_paintings)
{
    Plater &p = Plater::get();
    p.take_snapshot("Mesh Boolean");
    ModelObject *curr_model_object = p.model().objects[g.obj];
    ModelVolume *new_volume = curr_model_object->add_volume(std::move(mesh_result));
    for (const auto &saved_painting : saved_paintings) new_volume->restore_painting(saved_painting, true);
    ModelVolume *old_volume = g.src.mv;
    std::string suffix;
    switch (g.mode) {
    case Operation::Union: suffix = "union"; break;
    case Operation::Difference: suffix = "difference"; break;
    case Operation::Intersection: suffix = "intersection"; break;
    default: break;
    }
    new_volume->name = old_volume->name + " - " + suffix;
    new_volume->set_new_unique_id();
    new_volume->config.apply(old_volume->config);
    new_volume->set_type(old_volume->type());
    new_volume->set_material_id(old_volume->material_id());
    new_volume->set_offset(old_volume->get_transformation().get_offset());
    // delete old_volume
    std::swap(curr_model_object->volumes[g.src.volume_idx], curr_model_object->volumes.back());
    curr_model_object->delete_volume(curr_model_object->volumes.size() - 1);
    if (delete_input) {   // ObjectList::delete_from_model_and_list of the tool part
        int parts = 0;
        for (auto *mv : curr_model_object->volumes) parts += mv->is_model_part();
        ModelVolume *tv = curr_model_object->volumes[g.tool.volume_idx];
        if (!(tv->is_model_part() && parts <= 1)) curr_model_object->delete_volume(g.tool.volume_idx);
    }
    // reorder_volumes_and_get_selection: sort, select the new part
    if (curr_model_object->volumes.size() > 1) curr_model_object->sort_volumes(true);
    p.selection.clear();
    for (int i = 0; i < int(curr_model_object->volumes.size()); ++i)
        if (curr_model_object->volumes[i] == new_volume) p.selection.insert({g.obj, g.inst, i});
    curr_model_object->invalidate_bounding_box();
    g.src.reset();
    g.tool.reset();
    g.selecting = Selecting::SelectSource;
}

json apply(const json &)
{
    if (!g.active || !g.src.mv || !g.tool.mv) return state();
    TriangleMesh temp_src_mesh = g.src.mv->mesh();
    temp_src_mesh.transform(g.src.trafo);
    TriangleMesh temp_tool_mesh = g.tool.mv->mesh();
    temp_tool_mesh.transform(g.tool.trafo);
    std::vector<TriangleMesh> temp_mesh_resuls;
    const char *op = g.mode == Operation::Union ? "UNION" : g.mode == Operation::Difference ? "A_NOT_B" : "INTERSECTION";
    MeshBoolean::mcut::make_boolean(temp_src_mesh, temp_tool_mesh, temp_mesh_resuls, op);
    json st;
    if (!temp_mesh_resuls.empty()) {
        std::vector<std::optional<TriangleSelector::SavedPainting>> saved_paintings{g.src.save_painting()};
        if (g.mode != Operation::Difference) saved_paintings.push_back(g.tool.save_painting());   // diff only keeps the source's paint
        const bool delete_input = g.mode == Operation::Union ? true : g.mode == Operation::Difference ? g.diff_delete_input : g.inter_delete_input;
        generate_new_volume(delete_input, *temp_mesh_resuls.begin(), saved_paintings);
        Plater::get().changed();
        st = Plater::get().state();
    } else {
        st = Plater::get().state();
        st["warning"] = warning_text;   // push_plater_warning_notification
    }
    update_boxes();
    st["boolean"] = state();
    return st;
}

} // namespace

// GLGizmoMeshBoolean::on_render: Selection::render_bounding_box of the source (white) and the tool (teal)
void render_mesh_boolean(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    BoundingBoxf3 boxes[2];
    {
        std::lock_guard<std::mutex> l(g.mutex);
        boxes[0] = g.src_bb;
        boxes[1] = g.tool_bb;
    }
    const ColorRGBA colors[2] = {{1.f, 1.f, 1.f, 1.f}, {0.f, 150.f / 255.f, 136.f / 255.f, 1.f}};
    for (int k = 0; k < 2; ++k) {
        const BoundingBoxf3 &box = boxes[k];
        if (!box.defined) continue;
        const Vec3f b_min = box.min.cast<float>(), b_max = box.max.cast<float>(), size = 0.2f * box.size().cast<float>();
        std::vector<float> xyz;
        auto seg = [&](const Vec3f &a, const Vec3f &b) { xyz.insert(xyz.end(), {a.x(), a.y(), a.z(), b.x(), b.y(), b.z()}); };
        for (int c = 0; c < 8; ++c) {
            const Vec3f corner((c & 1) ? b_max.x() : b_min.x(), (c & 2) ? b_max.y() : b_min.y(), (c & 4) ? b_max.z() : b_min.z());
            const Vec3f dir((c & 1) ? -1.f : 1.f, (c & 2) ? -1.f : 1.f, (c & 4) ? -1.f : 1.f);
            seg(corner, corner + Vec3f(dir.x() * size.x(), 0, 0));
            seg(corner, corner + Vec3f(0, dir.y() * size.y(), 0));
            seg(corner, corner + Vec3f(0, 0, dir.z() * size.z()));
        }
        glDisable(GL_DEPTH_TEST);
        gl_draw_flat(xyz, GL_LINES, proj * view, colors[k], 2.f);
        glEnable(GL_DEPTH_TEST);
    }
}

bool mesh_boolean_active() { return g.active; }

static Commands reg({
    {"boolean.open", open},
    {"boolean.close", close},
    {"boolean.state", [](const json &) { return state(); }},
    {"boolean.set", set},
    {"boolean.tap", tap},
    {"boolean.apply", apply},
});

} // namespace orca
