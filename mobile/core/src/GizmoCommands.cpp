// The engine side of the gizmos: committing a drag (GLCanvas3D::do_move / do_rotate / do_scale), Lay on face, and
// the values / inputs of the manipulation panels (GizmoObjectManipulation).
#include <cmath>

#include "Core.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/AABBMesh.hpp"
#include "Host.hpp"
#include "Gizmos.hpp"
#include "TextGizmo.hpp"
#include "SvgGizmo.hpp"
#include "Plater.hpp"
#include "Viewport.hpp"
#include "Edit.hpp"
#include "GroovePlane.hpp"
#include "Painter.hpp"
#include "Simplify.hpp"
#include "BrimEars.hpp"
#include "MeasureGizmo.hpp"
#include "LayersEditing.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/LayOnFace.hpp"

using namespace Slic3r;

namespace orca {

void texture_disp_open(bool open);   // TextureDisp.cpp

namespace {

// the selected (object, instance) pairs and (object, instance, volume) parts
struct Targets
{
    std::set<std::pair<int, int>>  instances;
    std::vector<SelItem>           volumes;
};

Targets targets()
{
    Plater &p = Plater::get();
    Model &m = p.model();
    Targets t;
    for (const SelItem &s : p.selection) {
        if (s.obj < 0 || s.obj >= int(m.objects.size())) continue;
        const ModelObject *mo = m.objects[s.obj];
        if (s.vol >= 0) { t.volumes.push_back({s.obj, std::max(0, s.inst), s.vol}); continue; }
        for (int k = 0; k < int(mo->instances.size()); ++k)
            if (s.inst < 0 || s.inst == k) t.instances.insert({s.obj, k});
    }
    return t;
}

} // namespace

static BoundingBoxf3 selection_box(const Targets &t)
{
    Model &m = Plater::get().model();
    BoundingBoxf3 b;
    for (auto &[o, i] : t.instances) b.merge(m.objects[o]->instance_bounding_box(i));
    for (const SelItem &v : t.volumes) {
        const ModelObject *mo = m.objects[v.obj];
        b.merge(mo->volumes[v.vol]->mesh().transformed_bounding_box(mo->instances[v.inst]->get_matrix() * mo->volumes[v.vol]->get_matrix()));
    }
    return b;
}

BoundingBoxf3 selection_world_box() { return selection_box(targets()); }

// GLCanvas3D::do_move / do_rotate / do_scale: the snapshot, the change, then "Fixes sinking/flying instances"
json apply_world(const Transform3d &T, const std::string &name)
{
    Targets t = targets();
    if (t.instances.empty() && t.volumes.empty()) throw CommandError("nothing selected");
    return apply_world_to(T, name, t.instances, t.volumes);
}

json apply_world_to(const Transform3d &T, const std::string &name, const std::set<std::pair<int, int>> &instances, const std::vector<SelItem> &volumes)
{
    Plater &p = Plater::get();
    Model &m = p.model();
    Targets t{instances, volumes};
    if (!name.empty()) p.take_snapshot(name);
    std::map<std::pair<int, int>, double> min_zs;
    std::set<std::pair<int, int>> done;
    for (auto &[o, i] : t.instances) {
        ModelObject *mo = m.objects[o];
        min_zs[{o, i}] = mo->get_instance_min_z(i);   // sinking stays sinking
        ModelInstance *mi = mo->instances[i];
        // GLCanvas3D::do_move / do_rotate in the Assembly view write the assemble transformation
        if (p.assemble_view) mi->set_assemble_from_transform(T * mi->get_assemble_transformation().get_matrix());
        else mi->set_transformation(Geometry::Transformation(T * mi->get_matrix()));
        done.insert({o, i});
    }
    for (const SelItem &v : t.volumes) {
        ModelObject *mo = m.objects[v.obj];
        const Transform3d inst = p.shown_instance_matrix(*mo->instances[v.inst]);
        ModelVolume *mv = mo->volumes[v.vol];
        if (!min_zs.count({v.obj, v.inst})) min_zs[{v.obj, v.inst}] = mo->get_instance_min_z(v.inst);
        mv->set_transformation(Geometry::Transformation(inst.inverse() * T * inst * mv->get_matrix()));
        done.insert({v.obj, v.inst});
    }
    for (auto &[o, i] : done) {
        ModelObject *mo = m.objects[o];
        mo->invalidate_bounding_box();
        if (!mo->auto_drop || p.assemble_view) continue;   // no snapping to the bed in the Assembly view
        const double shift_z = mo->get_instance_min_z(i);
        if ((min_zs[{o, i}] >= SINKING_Z_THRESHOLD || shift_z > SINKING_Z_THRESHOLD) && shift_z != 0.0) {
            mo->translate_instance(i, Vec3d(0.0, 0.0, -shift_z));
            mo->invalidate_bounding_box();
        }
    }
    p.changed();
    return p.state();
}

namespace {

// the single instance / part the panel edits, if there is one
struct Single
{
    ModelObject *mo{nullptr};
    int          inst{0}, vol{-1};
};

Single single()
{
    Plater &p = Plater::get();
    Targets t = targets();
    Single s;
    if (t.instances.size() == 1 && t.volumes.empty()) {
        s.mo = p.model().objects[t.instances.begin()->first];
        s.inst = t.instances.begin()->second;
    } else if (t.instances.empty() && t.volumes.size() == 1) {
        s.mo = p.model().objects[t.volumes[0].obj];
        s.inst = t.volumes[0].inst;
        s.vol = t.volumes[0].vol;
    }
    return s;
}

json vec(const Vec3d &v) { return {v.x(), v.y(), v.z()}; }

// GizmoObjectManipulation::update_settings_value
json info(const json &a)
{
    std::string coords = a.value("coords", std::string("world"));
    Targets t = targets();
    json r = {{"empty", t.instances.empty() && t.volumes.empty()}};
    if (r["empty"]) return r;
    BoundingBoxf3 box = selection_box(t);
    Single s = single();
    r["single"] = s.mo != nullptr;
    r["part"] = s.vol >= 0;
    r["size"] = vec(box.size());
    if (s.mo == nullptr || coords == "world") {
        // world: absolute position (instance offset or box centre), relative rotation, 100 % scale
        Vec3d pos = s.mo != nullptr ? (s.vol >= 0 ? Vec3d(s.mo->instances[s.inst]->get_matrix() * s.mo->volumes[s.vol]->get_offset()) : Vec3d(s.mo->instances[s.inst]->get_offset()))
                                    : box.center();
        r["position"] = vec(pos);
        r["rotation"] = vec(Vec3d::Zero());
        r["scale"] = vec(Vec3d::Constant(100.));
        if (s.mo != nullptr) {
            Geometry::Transformation tr = s.vol >= 0 ? s.mo->volumes[s.vol]->get_transformation() : s.mo->instances[s.inst]->get_transformation();
            r["absolute_rotation"] = vec(tr.get_rotation() * (180. / M_PI));
        }
        return r;
    }
    // object / part coordinates
    Geometry::Transformation tr = s.vol >= 0 ? s.mo->volumes[s.vol]->get_transformation() : s.mo->instances[s.inst]->get_transformation();
    r["position"] = vec(Vec3d::Zero());
    r["rotation"] = vec(Vec3d::Zero());
    r["absolute_rotation"] = vec(tr.get_rotation() * (180. / M_PI));
    r["scale"] = vec(tr.get_scaling_factor() * 100.);
    // the size along the object's (part's) own axes
    BoundingBoxf3 raw = s.vol >= 0 ? s.mo->volumes[s.vol]->mesh().bounding_box() : s.mo->raw_mesh_bounding_box();
    r["size"] = vec(raw.size().cwiseProduct(tr.get_scaling_factor()));
    return r;
}

// GizmoObjectManipulation::on_change (position / rotation / scale / size typed into the panel)
json input(const json &a)
{
    const std::string field = str_arg(a, "field"), coords = a.value("coords", std::string("world"));
    const int axis = a.value("axis", 0);
    const double value = a.at("value");
    const bool uniform = a.value("uniform", true);
    Targets t = targets();
    if (t.instances.empty() && t.volumes.empty()) throw CommandError("nothing selected");
    BoundingBoxf3 box = selection_box(t);
    Single s = single();
    json cur = info({{"coords", coords}});
    Transform3d local = Transform3d::Identity();   // instance / part frame of a single selection
    if (s.mo != nullptr) local = s.vol >= 0 ? s.mo->instances[s.inst]->get_matrix() * s.mo->volumes[s.vol]->get_matrix() : s.mo->instances[s.inst]->get_matrix();
    auto in_local = [&](const Transform3d &L) {   // a transformation along the object's own axes
        Transform3d rot = Transform3d::Identity();
        rot.linear() = Geometry::Transformation(local).get_rotation_matrix().linear();
        rot.translation() = box.center();
        return rot * L * rot.inverse();
    };
    if (field == "position") {
        Vec3d d = Vec3d::Zero();
        if (coords == "world" || s.mo == nullptr) d[axis] = value - cur["position"][axis].get<double>();
        else d = Geometry::Transformation(local).get_rotation_matrix() * (Vec3d::Unit(axis) * value);
        return apply_world(Geometry::translation_transform(d), "Set Position");
    }
    if (field == "rotation") {   // relative, around the selection centre
        Vec3d n = Vec3d::Unit(axis);
        Transform3d R(Eigen::AngleAxisd(value * M_PI / 180., n));
        Transform3d T = (coords == "world" || s.mo == nullptr)
                            ? Transform3d(Geometry::translation_transform(box.center()) * R * Geometry::translation_transform(-box.center()))
                            : in_local(R);
        return apply_world(T, "Set Orientation");
    }
    if (field == "absolute_rotation" || field == "reset_rotation") {
        if (s.mo == nullptr) throw CommandError("select a single object");
        Geometry::Transformation tr = s.vol >= 0 ? s.mo->volumes[s.vol]->get_transformation() : s.mo->instances[s.inst]->get_transformation();
        Vec3d r = tr.get_rotation();
        if (field == "reset_rotation") r = Vec3d::Zero(); else r[axis] = value * M_PI / 180.;
        Geometry::Transformation target = tr;
        target.set_rotation(r);
        // as a world change around the centre, so the drop rule applies like in do_rotate()
        Transform3d parent = s.vol >= 0 ? s.mo->instances[s.inst]->get_matrix() : Transform3d::Identity();
        Transform3d T = parent * target.get_matrix() * tr.get_matrix().inverse() * parent.inverse();
        Vec3d c = box.center(), moved = T * c;
        return apply_world(Geometry::translation_transform(c - moved) * T, "Set Orientation");
    }
    if (field == "scale" || field == "size" || field == "reset_scale") {
        Vec3d ratio = Vec3d::Ones();
        if (field == "reset_scale") {
            if (s.mo == nullptr) throw CommandError("select a single object");
            Geometry::Transformation tr = s.vol >= 0 ? s.mo->volumes[s.vol]->get_transformation() : s.mo->instances[s.inst]->get_transformation();
            ratio = tr.get_scaling_factor().cwiseInverse();
        } else {
            double old = field == "scale" ? cur["scale"][axis].get<double>() : cur["size"][axis].get<double>();
            if (old <= 0 || value <= 0) throw CommandError("invalid value");
            double k = value / old;
            if (uniform) ratio = Vec3d::Constant(k); else ratio[axis] = k;
        }
        Vec3d pivot(box.center().x(), box.center().y(), box.min.z());
        Transform3d S = Geometry::scale_transform(ratio);
        Transform3d T;
        if (coords == "world" || s.mo == nullptr) T = Geometry::translation_transform(pivot) * S * Geometry::translation_transform(-pivot);
        else {
            T = in_local(S);
            Vec3d c = box.center(), moved = T * c;
            T = Geometry::translation_transform(c - moved) * T;
        }
        return apply_world(T, "Set Scale");
    }
    if (field == "drop") {   // "Drop": the lowest point of the selection onto the bed
        return apply_world(Geometry::translation_transform(Vec3d(0, 0, -box.min.z())), "Drop");
    }
    throw CommandError("unknown field " + field);
}

json transform_world(const json &a)
{
    Transform3d T = Transform3d::Identity();
    const json &m = a.at("matrix");
    if (m.size() != 16) throw CommandError("matrix needs 16 values");
    for (int i = 0; i < 16; ++i) T.matrix().data()[i] = m[i].get<double>();
    return apply_world(T, a.value("name", std::string("Gizmo-Move")));
}

// GLCanvas3D::apply_selection_move: the arrow keys move the selection 10 mm (1 mm with Shift), along the bed axes or,
// with Ctrl, along the camera's screen directions flattened onto the bed
json key_move(const json &a)
{
    Vec3d dir(a.at("dir")[0].get<double>(), a.at("dir")[1].get<double>(), 0.0);
    const double multiplier = a.value("slow", false) ? 1.0 : 10.0;
    Vec3d displacement;
    if (a.value("camera", false)) {
        const Eigen::Matrix3d inv_view_3x3 = Viewport::get().camera().view().inverse().block<3, 3>(0, 0);
        displacement     = multiplier * (inv_view_3x3 * dir);
        displacement.z() = 0.0;
    } else
        displacement = multiplier * dir;
    return apply_world(Geometry::translation_transform(displacement), "Move Object");
}

// GLCanvas3D::apply_selection_rotate: Page Up / Down turn the selection 45 degrees about Z around its centre
json key_rotate(const json &a)
{
    const BoundingBoxf3 box = selection_world_box();
    const Vec3d         c   = box.center();
    const Transform3d   T   = Geometry::translation_transform(c) * Geometry::rotation_transform(Vec3d(0, 0, a.at("angle").get<double>())) *
                          Geometry::translation_transform(-c);
    return apply_world(T, "Rotate Object");
}

// GLGizmoFlatten::on_mouse: Selection::flattening_rotate + do_rotate("Gizmo-Place on Face")
json flatten(const json &a)
{
    Plater &p = Plater::get();
    Model &m = p.model();
    int o = a.at("obj"), i = a.value("inst", 0);
    if (o < 0 || o >= int(m.objects.size()) || i >= int(m.objects[o]->instances.size())) throw CommandError("bad object");
    Vec3d n(a.at("normal")[0], a.at("normal")[1], a.at("normal")[2]);
    p.take_snapshot("Gizmo-Place on Face");
    lay_on_face(*m.objects[o], size_t(i), n.normalized());
    m.objects[o]->invalidate_bounding_box();
    p.changed();
    return p.state();
}


// ---------------------------------------------------------------------------------------------------------------
// Cut (GLGizmoCut3D, planar mode)

json conn_params_json();
json groove_json();
void update_groove();
void reset_cut_by_contours();
bool cut_parts_stale(const Gizmos::CutPlane &c);
json cut_parts_json();

json cut_info(const json &)
{
    update_groove();   // a dragged plane: the groove mesh and its validity follow
    Gizmos::CutPlane c = Gizmos::get().cut_plane();
    if (cut_parts_stale(c)) reset_cut_by_contours();
    if (!c.valid) return {{"valid", false}};
    Vec3d rot = Geometry::Transformation(c.rotation).get_rotation() * (180. / M_PI);
    const bool initial = c.rotation.isApprox(Transform3d::Identity()) && c.center.isApprox(c.bb_center);
    Gizmos::CutEdit e = Gizmos::get().cut_edit();
    int selected = 0;
    for (bool b : e.selected) selected += b;
    return {{"valid", true}, {"z", c.center.z()}, {"min_z", c.box.min.z()}, {"max_z", c.box.max.z()},
            {"rotation", {rot.x(), rot.y(), rot.z()}}, {"initial", initial}, {"size", vec(c.box.size())},
            {"editing", e.editing}, {"connectors", e.connectors.size()}, {"selected", selected}, {"params", conn_params_json()},
            {"groove", groove_json()}, {"cut_line", Gizmos::get().cut_line_mode()},
            {"cut_line_label", _u8L("Cut by line")}, {"cut_line_tip", _u8L("Draw cut line")},
            {"parts", cut_parts_json()}};
}

json cut_set(const json &a)
{
    Gizmos &g = Gizmos::get();
    if (a.value("reset", false)) g.reset_cut_plane();
    if (a.contains("cut_line")) g.set_cut_line_mode(a["cut_line"]);
    else {
        Gizmos::CutPlane c = g.cut_plane();
        Vec3d center = c.center;
        if (a.contains("z")) center.z() = a["z"].get<double>();   // render_move_center_input(Z)
        g.set_cut_plane(center, c.rotation);
    }
    update_groove();
    Viewport::get().request_redraw();
    return cut_info(a);
}


// --- connectors (GLGizmoCut3D connectors editing) ---------------------------------------------------------------

struct ConnectorParams   // the panel's values (m_connector_*), applied to new and to selected connectors
{
    int   type{0}, style{0}, shape{3};
    float depth{3.f}, depth_tol{0.1f}, size{2.5f}, size_tol{0.f}, angle{0.f}, bulge{0.15f}, space{0.3f};
};
ConnectorParams g_conn;

json conn_params_json()
{
    return {{"type", g_conn.type}, {"style", g_conn.style}, {"shape", g_conn.shape}, {"depth", g_conn.depth}, {"depth_tol", g_conn.depth_tol},
            {"size", g_conn.size}, {"size_tol", g_conn.size_tol}, {"angle", g_conn.angle}, {"bulge", g_conn.bulge}, {"space", g_conn.space}};
}

ModelObject *cut_object(Gizmos::CutPlane &c)
{
    c = Gizmos::get().cut_plane();
    Model &m = Plater::get().model();
    if (!c.valid || c.obj < 0 || c.obj >= int(m.objects.size())) throw CommandError("nothing to cut");
    return m.objects[c.obj];
}

// keeps the drawn copy in step with ModelObject::cut_connectors
void sync_edit(ModelObject *mo, bool editing, std::vector<bool> selected = {})
{
    Gizmos::CutEdit e = Gizmos::get().cut_edit();
    e.editing = editing;
    e.connectors = mo->cut_connectors;
    if (!selected.empty()) e.selected = selected;
    e.selected.resize(e.connectors.size(), false);
    e.snap_space = g_conn.space;
    e.snap_bulge = g_conn.bulge;
    Gizmos::get().set_cut_edit(std::move(e));
    Viewport::get().request_redraw();
}

json cut_connectors_edit(const json &a)
{
    Gizmos::CutPlane c;
    ModelObject *mo = cut_object(c);
    const bool edit = a.value("edit", true);
    Gizmos::CutEdit e = Gizmos::get().cut_edit();
    if (edit) {
        // the cross section of the model parts in the plane frame (ObjectClipper / MeshClipper)
        const Transform3d plane = Geometry::translation_transform(c.center) * c.rotation;
        e.to_plane = plane.inverse();
        e.offset = mo->instances[c.inst]->get_offset();
        ExPolygons section;
        for (const ModelVolume *v : mo->volumes) {
            if (!v->is_model_part()) continue;
            MeshSlicingParams params;
            params.trafo = e.to_plane * mo->instances[c.inst]->get_matrix() * v->get_matrix();
            Polygons polys = slice_mesh(v->mesh().its, 0.f, params);
            append(section, union_ex(polys));
        }
        e.section = union_ex(section);
    }
    Gizmos::get().set_cut_edit(e);
    if (a.value("cancel", false)) mo->cut_connectors.clear();   // Cancel = reset_connectors
    sync_edit(mo, edit);
    return cut_info(a);
}

// GLGizmoCut3D::add_connector / select / delete, from a tap on the 3D view
json cut_connector_tap(const json &a)
{
    Gizmos::CutPlane c;
    ModelObject *mo = cut_object(c);
    Gizmos::CutEdit e = Gizmos::get().cut_edit();
    if (!e.editing) return nullptr;
    const Camera cam = Viewport::get().camera();
    const double px = a.at("x"), py = a.at("y");
    // a connector under the finger: select it (long press: delete it)
    Eigen::Matrix4d vp = cam.projection(1, 1000) * cam.view();
    auto screen = [&](const Vec3d &w) {
        Eigen::Vector4d q = vp * Eigen::Vector4d(w.x(), w.y(), w.z(), 1.0);
        return Vec2d((q.x() / q.w() * 0.5 + 0.5) * cam.width, (0.5 - q.y() / q.w() * 0.5) * cam.height);
    };
    int hit = -1;
    double best = 1e30;
    for (size_t i = 0; i < mo->cut_connectors.size(); ++i) {
        double d = (screen(mo->cut_connectors[i].pos + e.offset) - Vec2d(px, py)).norm();
        if (d < best) { best = d; hit = int(i); }
    }
    const double tol = 40.0;
    if (hit >= 0 && best < tol) {
        Plater &p = Plater::get();
        if (a.value("delete", false)) {
            p.take_snapshot(_u8L("Delete connector"));
            mo->cut_connectors.erase(mo->cut_connectors.begin() + hit);
            std::vector<bool> sel = e.selected;
            if (hit < int(sel.size())) sel.erase(sel.begin() + hit);
            sync_edit(mo, true, sel);
        } else {
            std::vector<bool> sel(mo->cut_connectors.size(), false);
            sel[hit] = true;
            sync_edit(mo, true, sel);
        }
        return cut_info(a);
    }
    if (a.value("delete", false)) return cut_info(a);
    // unproject_on_cut_plane + is_projection_inside_cut
    Vec3d o, d;
    cam.ray(px, py, o, d);
    Vec3d n = (c.rotation.linear() * Vec3d::UnitZ()).normalized();
    double den = n.dot(d);
    if (std::abs(den) < 1e-9) return cut_info(a);
    Vec3d w = o + d * ((c.center - o).dot(n) / den);
    Vec3d local = e.to_plane * w;
    Point pt(scale_(local.x()), scale_(local.y()));
    if (std::none_of(e.section.begin(), e.section.end(), [&](const ExPolygon &ex) { return ex.contains(pt); })) return cut_info(a);
    Plater::get().take_snapshot(_u8L("Add connector"));
    mo->cut_connectors.emplace_back(w - e.offset, c.rotation, g_conn.size * 0.5f, g_conn.depth, g_conn.size_tol * 0.5f, g_conn.depth_tol, g_conn.angle,
                                    CutConnectorAttributes(CutConnectorType(g_conn.type), CutConnectorStyle(g_conn.style), CutConnectorShape(g_conn.shape)));
    std::vector<bool> sel(mo->cut_connectors.size(), false);
    sel.back() = true;
    sync_edit(mo, true, sel);
    return cut_info(a);
}

// GLGizmoCut3D::on_stop_dragging for a connector: its new position into the object
json cut_connector_move(const json &a)
{
    Gizmos::CutPlane c;
    ModelObject *mo = cut_object(c);
    const int i = a.at("index");
    if (i < 0 || i >= int(mo->cut_connectors.size())) return cut_info(a);
    Plater::get().take_snapshot(_u8L("Move connector"));
    const auto &p = a.at("pos");
    mo->cut_connectors[i].pos = Vec3d(p[0], p[1], p[2]);
    std::vector<bool> sel(mo->cut_connectors.size(), false);
    sel[i] = true;
    sync_edit(mo, true, sel);
    return cut_info(a);
}

// the panel: new values for the next connectors and the selected ones (apply_selected_connectors)
json cut_connector_params(const json &a)
{
    Gizmos::CutPlane c;
    ModelObject *mo = cut_object(c);
    if (a.contains("type")) {
        g_conn.type = a["type"];
        if (g_conn.type == int(CutConnectorType::Dowel)) g_conn.style = int(CutConnectorStyle::Prism);
        if (g_conn.type == int(CutConnectorType::Snap)) g_conn.shape = int(CutConnectorShape::Circle);
    }
    if (a.contains("style")) g_conn.style = a["style"];
    if (a.contains("shape")) g_conn.shape = a["shape"];
    for (auto [k, f] : std::initializer_list<std::pair<const char *, float *>>{{"depth", &g_conn.depth}, {"depth_tol", &g_conn.depth_tol}, {"size", &g_conn.size},
                                                                              {"size_tol", &g_conn.size_tol}, {"angle", &g_conn.angle}, {"bulge", &g_conn.bulge},
                                                                              {"space", &g_conn.space}})
        if (a.contains(k)) *f = a[k].get<float>();
    Gizmos::CutEdit e = Gizmos::get().cut_edit();
    for (size_t i = 0; i < mo->cut_connectors.size() && i < e.selected.size(); ++i) {
        if (!e.selected[i]) continue;
        CutConnector &cc = mo->cut_connectors[i];
        cc.attribs = CutConnectorAttributes(CutConnectorType(g_conn.type), CutConnectorStyle(g_conn.style), CutConnectorShape(g_conn.shape));
        if (g_conn.depth > 0) cc.height = g_conn.depth;
        if (g_conn.depth_tol >= 0) cc.height_tolerance = g_conn.depth_tol;
        if (g_conn.size > 0) cc.radius = 0.5f * g_conn.size;
        if (g_conn.size_tol >= 0) cc.radius_tolerance = 0.5f * g_conn.size_tol;
        cc.z_angle = g_conn.angle;
    }
    sync_edit(mo, e.editing);
    return cut_info(a);
}

json cut_connectors_clear(const json &a)
{
    Gizmos::CutPlane c;
    ModelObject *mo = cut_object(c);
    Plater::get().take_snapshot(_u8L("Remove connectors"));
    mo->cut_connectors.clear();
    sync_edit(mo, Gizmos::get().cut_edit().editing);
    return cut_info(a);
}


// --- dovetail (cutTongueAndGroove) ------------------------------------------------------------------------------

GroovePlane g_groove;
bool        g_groove_mode = false;
int         g_groove_obj = -1;
BoundingBoxf3 g_groove_box;

// GLGizmoCut3D::update_bb: the groove's initial values for the selected object
void init_groove(const Gizmos::CutPlane &c)
{
    if (g_groove_obj == c.obj && g_groove_box.min.isApprox(c.box.min) && g_groove_box.max.isApprox(c.box.max)) return;
    g_groove_obj = c.obj;
    g_groove_box = c.box;
    g_groove.m_radius = c.radius;
    g_groove.m_bounding_box = c.box;
    const Camera cam = Viewport::get().camera();
    const double inv_zoom = 2 * cam.distance * std::tan(cam.fov * M_PI / 360.) / std::max(1, cam.height);
    const float mean = float(32. * inv_zoom);   // get_grabber_mean_size (ENABLE_FIXED_GRABBER)
    g_groove.m_groove.depth = g_groove.m_groove.depth_init = std::max(1.f, 0.5f * mean);
    g_groove.m_groove.width = g_groove.m_groove.width_init = 4.0f * g_groove.m_groove.depth;
    g_groove.m_groove.flaps_angle = g_groove.m_groove.flaps_angle_init = float(PI) / 3.f;
    g_groove.m_groove.angle = g_groove.m_groove.angle_init = 0.f;
}

// GLGizmoCut3D::has_valid_groove
bool groove_valid(const Gizmos::CutPlane &c)
{
    const Cut::Groove &g = g_groove.m_groove;
    if (-2.f * g.depth / std::tan(g.flaps_angle) > g.width) return false;
    const ModelObject *mo = Plater::get().model().objects[c.obj];
    const Transform3d cp = Geometry::translation_transform(c.center) * c.rotation;
    for (size_t id = 0; id + 1 < g_groove.m_groove_vertices.size(); id += 2) {
        const Vec3d beg = cp * g_groove.m_groove_vertices[id], end = cp * g_groove.m_groove_vertices[id + 1];
        bool hit = false;
        for (const ModelVolume *v : mo->volumes) {
            if (v->is_modifier()) continue;
            const Transform3d m = mo->instances[c.inst]->get_matrix() * v->get_matrix();
            const Transform3d inv = m.inverse();
            const Vec3d o = inv * beg, d = inv * end - o;
            const auto &its = v->mesh().its;
            for (const auto &f : its.indices) {   // segment / triangle (MeshRaycaster::intersects_line)
                const Vec3d a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(), cc = its.vertices[f[2]].cast<double>();
                Vec3d e1 = b - a, e2 = cc - a, pv = d.cross(e2);
                double det = e1.dot(pv);
                if (std::abs(det) < 1e-12) continue;
                Vec3d tv = o - a;
                double u = tv.dot(pv) / det;
                if (u < 0 || u > 1) continue;
                Vec3d q = tv.cross(e1);
                double vv = d.dot(q) / det;
                if (vv < 0 || u + vv > 1) continue;
                double t = e2.dot(q) / det;
                if (t >= 0 && t <= 1) { hit = true; break; }
            }
            if (hit) break;
        }
        if (!hit) return false;
    }
    return true;
}

void update_groove()
{
    Gizmos::CutPlane c = Gizmos::get().cut_plane();
    if (!c.valid || !g_groove_mode) { Gizmos::get().set_cut_groove(false, nullptr, true); return; }
    init_groove(c);
    auto mesh = std::make_shared<const TriangleMesh>(g_groove.its_make_groove_plane());
    Gizmos::get().set_cut_groove(true, mesh, groove_valid(c));
    Viewport::get().request_redraw();
}

json groove_json()
{
    const Cut::Groove &g = g_groove.m_groove;
    return {{"mode", g_groove_mode ? 1 : 0}, {"depth", g.depth}, {"width", g.width}, {"depth_tol", g.depth_tolerance}, {"width_tol", g.width_tolerance},
            {"flaps_angle", g.flaps_angle * 180. / PI}, {"angle", g.angle * 180. / PI}, {"count", g_groove.m_groove_count}, {"gap", g_groove.m_groove_gap},
            {"spacing", g_groove.m_groove_gap + Cut::calculate_groove_width(g, float(g_groove.m_radius))},
            {"valid", Gizmos::get().cut_plane().valid_groove}};
}

json cut_groove(const json &a)
{
    if (a.contains("mode")) g_groove_mode = a["mode"].get<int>() == 1;
    Cut::Groove &g = g_groove.m_groove;
    // render_groove_two_float_input / render_groove_angle_input (flap 30..120, groove 0..15 degrees) / int / gap
    if (a.contains("depth")) g.depth = std::max(0.1f, a["depth"].get<float>());
    if (a.contains("width")) g.width = std::max(0.1f, a["width"].get<float>());
    if (a.contains("depth_tol")) g.depth_tolerance = std::max(0.f, a["depth_tol"].get<float>());
    if (a.contains("width_tol")) g.width_tolerance = std::max(0.f, a["width_tol"].get<float>());
    if (a.contains("flaps_angle")) g.flaps_angle = float(std::clamp(a["flaps_angle"].get<double>(), 30., 120.) * PI / 180.);
    if (a.contains("angle")) g.angle = float(std::clamp(a["angle"].get<double>(), 0., 15.) * PI / 180.);
    if (a.contains("count")) g_groove.m_groove_count = std::clamp(a["count"].get<int>(), 1, 100);
    if (a.contains("gap")) g_groove.m_groove_gap = std::max(0.f, a["gap"].get<float>());
    if (a.value("reset", false)) {
        g.depth = g.depth_init; g.width = g.width_init; g.flaps_angle = g.flaps_angle_init; g.angle = g.angle_init;
        g_groove.m_groove_count = 1; g_groove.m_groove_gap = 10.f;
    }
    update_groove();
    return cut_info(a);
}

// update_object_cut_id
void update_object_cut_id(CutObjectBase &cut_id, ModelObjectCutAttributes attributes, const int dowels_count)
{
    if (!attributes.has(ModelObjectCutAttribute::KeepUpper) || !attributes.has(ModelObjectCutAttribute::KeepLower) ||
        attributes.has(ModelObjectCutAttribute::InvalidateCutInfo))
        return;
    if (cut_id.id().invalid()) cut_id.init();
    int cut_obj_cnt = -1;
    if (attributes.has(ModelObjectCutAttribute::KeepUpper)) cut_obj_cnt++;
    if (attributes.has(ModelObjectCutAttribute::KeepLower)) cut_obj_cnt++;
    if (attributes.has(ModelObjectCutAttribute::CreateDowels)) cut_obj_cnt += dowels_count;
    if (cut_obj_cnt > 0) cut_id.increase_check_sum(size_t(cut_obj_cnt));
}

// --- GLGizmoCut3D::PartSelection: which side each part of the cut goes to (the desktop's right click on a part) ----

struct PartSelection
{
    bool valid{false};
    Model model;
    int obj{-1}, inst{0};
    Vec3d center{0, 0, 0};
    Transform3d rotation{Transform3d::Identity()};
    struct Part { std::shared_ptr<const TriangleMesh> mesh; std::unique_ptr<AABBMesh> aabb; bool selected{true}, is_modifier{false}; };
    std::vector<Part> parts;
    std::vector<std::pair<std::vector<size_t>, std::vector<size_t>>> contour_to_parts;
    std::vector<size_t> ignored_contours;
    ModelObject *model_object() { return model.objects.front(); }
    Transform3d part_matrix(size_t id) { return Geometry::translation_transform(model_object()->instances[inst]->get_offset()) * model_object()->volumes[id]->get_matrix(); }
    bool is_one_object() const
    {
        if (parts.size() < 2) return true;
        return std::all_of(parts.begin(), parts.end(), [this](const Part &part) { return part.is_modifier || part.selected == parts.front().selected; });
    }
    std::vector<Cut::Part> get_cut_parts()
    {
        std::vector<Cut::Part> out;
        for (const auto &part : parts) out.push_back({part.selected, part.is_modifier});
        return out;
    }
};
PartSelection g_parts;

bool cut_parts_stale(const Gizmos::CutPlane &c)
{
    return g_parts.valid && (g_parts.obj != c.obj || !g_parts.center.isApprox(c.center) || !g_parts.rotation.isApprox(c.rotation));
}

json cut_parts_json() { return {{"valid", g_parts.valid}, {"one_object", g_parts.valid && g_parts.is_one_object()}}; }

void publish_parts()
{
    std::vector<Gizmos::CutPart> out;
    if (g_parts.valid)
        for (size_t i = 0; i < g_parts.parts.size(); ++i)
            out.push_back({g_parts.parts[i].mesh, g_parts.part_matrix(i), g_parts.parts[i].selected, g_parts.parts[i].is_modifier});
    Gizmos::get().set_cut_parts(g_parts.obj, std::move(out));
    Viewport::get().request_redraw();
}

void reset_cut_by_contours()
{
    if (!g_parts.valid) return;
    g_parts.valid = false;
    g_parts.parts.clear();
    g_parts.model.clear_objects();
    publish_parts();
}

// MeshClipper::point_per_contour: a point inside each island of the section (plane frame), not in a hole
std::vector<Vec3d> point_per_contour(const ExPolygons &islands, const Transform3d &from_plane)
{
    std::vector<Vec3d> out;
    for (const ExPolygon &isl : islands) {
        bool done = false;
        Vec2d p;
        size_t i = 1;
        while (i < isl.contour.size()) {
            const Vec2d a = unscale(isl.contour.points[i - 1]);
            const Vec2d b = unscale(isl.contour.points[i]);
            Vec2d n = (b - a).normalized();
            std::swap(n.x(), n.y());
            n.x() = -1 * n.x();
            double f = 10.;
            while (f > 0.05) {
                p = (0.5 * (b + a)) + f * n;
                if (isl.contains(Point::new_scale(p))) { done = true; break; }
                f = f / 10.;
            }
            if (done) break;
            i += std::max(size_t(2), isl.contour.size() / 5);
        }
        if (!done) p = unscale(isl.contour.points.front());   // should not happen
        out.emplace_back(from_plane * Vec3d(p.x(), p.y(), 0.));
    }
    return out;
}

// GLGizmoCut3D::process_contours + the PartSelection constructor
void process_contours()
{
    Gizmos::CutPlane c;
    ModelObject *mo = cut_object(c);
    g_parts = PartSelection();
    g_parts.obj = c.obj;
    g_parts.inst = c.inst;
    g_parts.center = c.center;
    g_parts.rotation = c.rotation;
    const Vec3d normal = (c.rotation.linear() * Vec3d::UnitZ()).normalized();
    const Transform3d cut_matrix = Geometry::translation_transform(c.center - mo->instances[c.inst]->get_offset()) * c.rotation;
    Cut cut(mo, c.inst, cut_matrix);
    g_parts.model.add_object(*cut.perform_with_plane().front());
    const ModelVolumePtrs &volumes = g_parts.model_object()->volumes;
    for (int id = int(volumes.size()) - 1; id >= 0; id--)
        if (volumes[id]->is_splittable()) volumes[id]->split(1, false);
    for (const ModelVolume *volume : volumes) {
        PartSelection::Part part;
        part.mesh = volume->get_mesh_shared_ptr();
        part.aabb = std::make_unique<AABBMesh>(volume->mesh());
        part.is_modifier = !volume->is_model_part();
        // whether this part is below or above the plane
        const Transform3d tr = (g_parts.model_object()->instances[c.inst]->get_matrix() * volume->get_matrix()).inverse();
        const Vec3f pos = (tr * c.center).cast<float>();
        const Vec3f norm = (tr.linear().inverse().transpose() * normal).cast<float>();
        for (const Vec3f &v : volume->mesh().its.vertices) {
            const double p = (v - pos).dot(norm);
            if (std::abs(p) > EPSILON) { part.selected = p > 0.; break; }
        }
        g_parts.parts.push_back(std::move(part));
    }
    // the contours of the section (the model parts cut by the plane) and which parts lie on each side of them
    const Transform3d plane = Geometry::translation_transform(c.center) * c.rotation;
    ExPolygons section;
    for (const ModelVolume *v : mo->volumes) {
        if (!v->is_model_part()) continue;
        MeshSlicingParams params;
        params.trafo = plane.inverse() * mo->instances[c.inst]->get_matrix() * v->get_matrix();
        append(section, union_ex(slice_mesh(v->mesh().its, 0.f, params)));
    }
    const std::vector<Vec3d> pts = point_per_contour(union_ex(section), plane);
    g_parts.contour_to_parts.resize(pts.size());
    for (size_t pt_idx = 0; pt_idx < pts.size(); ++pt_idx) {
        const Vec3d contour_point = (c.center - pts[pt_idx]).dot(normal) * normal + pts[pt_idx];
        for (size_t part_id = 0; part_id < g_parts.parts.size(); ++part_id) {
            const Transform3d tr = (Geometry::translation_transform(g_parts.model_object()->instances[c.inst]->get_offset()) *
                                    Geometry::translation_transform(g_parts.model_object()->volumes[part_id]->get_offset())).inverse();
            for (double d : {-1., 1.}) {
                const Vec3d dir_mesh = d * tr.linear().inverse().transpose() * normal;
                const Vec3d src = tr * (contour_point + d * 0.01 * normal);
                AABBMesh::hit_result hit = g_parts.parts[part_id].aabb->query_ray_hit(src, dir_mesh);
                if (hit.is_inside()) {
                    if (d == 1.) g_parts.contour_to_parts[pt_idx].first.emplace_back(part_id);
                    else g_parts.contour_to_parts[pt_idx].second.emplace_back(part_id);
                }
            }
        }
    }
    g_parts.valid = true;
}

// PartSelection::toggle_selection at a finger
json cut_part_toggle(const json &a)
{
    Gizmos::CutPlane c = Gizmos::get().cut_plane();
    if (!c.valid || g_groove_mode || Gizmos::get().cut_edit().editing) return cut_info(a);
    if (!g_parts.valid || g_parts.obj != c.obj || !g_parts.center.isApprox(c.center) || !g_parts.rotation.isApprox(c.rotation)) process_contours();
    const Camera cam = Viewport::get().camera();
    Vec3d o, d;
    cam.ray(a.at("x").get<double>(), a.at("y").get<double>(), o, d);
    int best = -1;
    double best_sq = DBL_MAX;
    for (size_t id = 0; id < g_parts.parts.size(); ++id) {
        const Transform3d tr = g_parts.part_matrix(id), inv = tr.inverse();
        for (const AABBMesh::hit_result &h : g_parts.parts[id].aabb->query_ray_hits(inv * o, (inv.linear() * d).normalized())) {
            if (!h.is_hit() || h.is_inside()) continue;
            const double sq = (o - tr * h.position()).squaredNorm();
            if (sq < best_sq) { best_sq = sq; best = int(id); }
            break;
        }
    }
    if (best >= 0) {
        g_parts.parts[best].selected = !g_parts.parts[best].selected;
        // the contours whose both sides now belong to the same object
        g_parts.ignored_contours.clear();
        size_t cont_id = 0;
        for (const auto &[parts_above, parts_below] : g_parts.contour_to_parts) {
            for (size_t upper : parts_above) {
                const bool upper_sel = g_parts.parts[upper].selected;
                if (std::find_if(parts_below.begin(), parts_below.end(), [&upper_sel](const size_t &i) { return g_parts.parts[i].selected == upper_sel; }) != parts_below.end()) {
                    g_parts.ignored_contours.emplace_back(cont_id);
                    break;
                }
            }
            ++cont_id;
        }
    }
    publish_parts();
    return cut_info(a);
}

// GLGizmoCut3D::perform_cut (planar, no connectors yet)
json cut_perform(const json &a)
{
    Plater &p = Plater::get();
    Model &m = p.model();
    Gizmos::CutPlane c = Gizmos::get().cut_plane();
    if (!c.valid || c.obj < 0 || c.obj >= int(m.objects.size())) throw CommandError("nothing to cut");
    ModelObject *mo = m.objects[c.obj];
    const bool has_connectors = !mo->cut_connectors.empty();
    const bool keep_upper = has_connectors || a.value("keep_upper", true), keep_lower = has_connectors || a.value("keep_lower", true),
               as_parts = !has_connectors && a.value("keep_as_parts", false);
    if (!keep_upper && !keep_lower) throw CommandError(_u8L("Select at least one object to keep after cutting."));
    if (g_groove_mode && !c.valid_groove) throw CommandError(_u8L("Cut plane with groove is invalid"));
    p.take_snapshot(_u8L("Cut by Plane"));
    // by contour: the cut of the part selection (its parts flipped between the sides) is cut again
    const bool cut_by_contour = !g_groove_mode && g_parts.valid && g_parts.obj == c.obj && !cut_parts_stale(c);
    ModelObject *cut_mo = cut_by_contour ? g_parts.model_object() : mo;
    if (cut_by_contour) cut_mo->cut_connectors = mo->cut_connectors;
    // apply_connectors_in_model: plugs move half their height along the normal, dowels double, then negative volumes
    int dowels_count = 0;
    {
        const Vec3d normal = (c.rotation.linear() * Vec3d::UnitZ()).normalized();
        for (CutConnector &cc : cut_mo->cut_connectors) {
            cc.rotation_m = c.rotation;
            if (cc.attribs.type == CutConnectorType::Dowel) {
                if (cc.attribs.style == CutConnectorStyle::Prism) cc.height *= 2;
                dowels_count++;
            } else
                cc.pos += normal * 0.5 * double(cc.height);
        }
        // GLGizmoCut3D::apply_cut_connectors
        if (!cut_mo->cut_connectors.empty()) {
            size_t connector_id = cut_mo->cut_id.connectors_cnt();
            for (const CutConnector &cc : cut_mo->cut_connectors) {
                ModelVolume *nv = cut_mo->add_volume(TriangleMesh(cut_connector_mesh(cc.attribs, g_conn.space, g_conn.bulge)), ModelVolumeType::NEGATIVE_VOLUME);
                nv->set_transformation(Geometry::translation_transform(cc.pos) * cc.rotation_m * Geometry::rotation_transform(-cc.z_angle * Vec3d::UnitZ()) *
                                       Geometry::scale_transform(Vec3f(cc.radius, cc.radius, cc.height).cast<double>()));
                nv->cut_info = {cc.attribs.type, cc.radius_tolerance, cc.height_tolerance};
                nv->name = _u8L("Connector") + "-" + std::to_string(++connector_id);
            }
            cut_mo->cut_id.increase_connectors_cnt(cut_mo->cut_connectors.size());
            cut_mo->cut_connectors.clear();
            if (cut_by_contour) mo->cut_connectors.clear();
        }
    }
    const bool keep_painting = Core::get().app_config->get_bool("keep_painting");
    ModelObjectCutAttributes attributes = only_if(keep_upper, ModelObjectCutAttribute::KeepUpper) |
                                          only_if(keep_lower, ModelObjectCutAttribute::KeepLower) |
                                          only_if(as_parts && !cut_by_contour, ModelObjectCutAttribute::KeepAsParts) |
                                          only_if(a.value("place_upper", true), ModelObjectCutAttribute::PlaceOnCutUpper) |
                                          only_if(a.value("place_lower", false), ModelObjectCutAttribute::PlaceOnCutLower) |
                                          only_if(a.value("flip_upper", false), ModelObjectCutAttribute::FlipUpper) |
                                          only_if(a.value("flip_lower", false), ModelObjectCutAttribute::FlipLower) |
                                          only_if(dowels_count > 0, ModelObjectCutAttribute::CreateDowels) |
                                          only_if(!has_connectors && !g_groove_mode && cut_mo->cut_id.id().invalid(), ModelObjectCutAttribute::InvalidateCutInfo) |
                                          only_if(keep_painting, ModelObjectCutAttribute::KeepPaint);
    update_object_cut_id(cut_mo->cut_id, attributes, dowels_count);
    // get_cut_matrix: the plane centre relative to the instance offset, then the plane rotation
    const Vec3d cut_center_offset = c.center - mo->instances[c.inst]->get_offset();
    const Transform3d cut_matrix = Geometry::translation_transform(cut_center_offset) * c.rotation;
    Cut cut(cut_mo, c.inst, cut_matrix, attributes);
    const ModelObjectPtrs &new_objects = cut_by_contour ? cut.perform_by_contour(mo, g_parts.get_cut_parts(), dowels_count) : g_groove_mode ? cut.perform_with_groove(g_groove.m_groove, c.rotation, g_groove.m_groove_count, g_groove.m_groove_gap,
                                                                                float(g_groove.m_radius))
                                                       : cut.perform_with_plane();
    // fix_non_manifold_edges
    bool asked = false, fix = false;
    for (ModelObject *no : new_objects)
        for (size_t j = 0; j < no->volumes.size(); ++j)
            if (its_num_open_edges(no->volumes[j]->mesh().its) > 0) {
                if (!asked) {
                    asked = true;
                    json r = Host::ask({{"kind", "message"}, {"message", _u8L("Non-manifold edges be caused by cut tool: do you want to fix now?")}, {"caption", ""},
                                        {"buttons", json::array({{{"id", 5103}, {"label", _u8L("Yes")}}, {{"id", 5101}, {"label", _u8L("Cancel")}}})}});
                    fix = r.value("id", 0) == 5103;
                }
                if (!fix) break;
                MeshRepairStats stats;
                std::string error;
                no->volumes[j]->repair(keep_painting, &stats, &error, [](int) {});
            }
    const CutObjectBase cut_id = cut_mo->cut_id;
    // Plater::apply_cut_object_to_model
    m.delete_object(size_t(c.obj));
    std::vector<size_t> idxs = load_model_objects(new_objects, false, true);
    // synchronize_model_after_cut
    for (ModelObject *obj : m.objects)
        if (obj->is_cut() && obj->cut_id.has_same_id(cut_id) && !obj->cut_id.is_equal(cut_id)) obj->cut_id.copy(cut_id);
    p.selection.clear();
    for (size_t i : idxs) p.selection.insert({int(i), -1, -1});
    reset_cut_by_contours();
    Gizmos::get().set_active("");
    Viewport::get().request_snapshot();
    p.changed();
    json st = p.state();
    st["close_gizmo"] = true;
    return st;
}

json set_gizmo(const json &a)
{
    static const std::map<std::string, std::string> names = {{"Move", "move"}, {"Rotate", "rotate"}, {"Scale", "scale"}, {"Lay on face", "flatten"}, {"Cut", "cut"}};
    // the painting gizmos are the Painter's (GLGizmoFdmSupports, GLGizmoSeam, GLGizmoMmuSegmentation, GLGizmoFuzzySkin)
    static const std::map<std::string, std::string> paints = {{"Support painting", "support"}, {"Seam painting", "seam"},
                                                              {"Color painting", "mmu"}, {"Fuzzy skin painting", "fuzzy"},
                                                              {"Texture displacement", "texture"}};
    std::string n = a.value("name", std::string());
    auto it = names.find(n);
    Gizmos::get().set_active(it != names.end() ? it->second : std::string());
    auto pit = paints.find(n);
    if ((pit != paints.end() ? pit->second : std::string()) != Painter::get().kind()) Painter::get().set_kind(pit != paints.end() ? pit->second : std::string());
    texture_disp_open(n == "Texture displacement");   // GLGizmoTextureDisplacement: its layer stack and preview (TextureDisp.cpp)
    if (n != "Text shape") text_gizmo_close();
    if (n != "Cut") reset_cut_by_contours();
    if (n != "SVG") svg_gizmo_close();
    if (n == "Simplify") Simplify::get().open(); else Simplify::get().close();
    Core::get().call(n == "Mesh boolean" ? "boolean.open" : "boolean.close", json::object());
    if (n == "Brim ears") BrimEarsGizmo::get().open(); else BrimEarsGizmo::get().close();
    if (n == "Measure" || n == "Assemble") MeasureGizmo::get().open(n == "Assemble"); else MeasureGizmo::get().close();
    if ((n == "Variable layer height") != LayersEditing::get().enabled()) LayersEditing::get().set_enabled(n == "Variable layer height");
    Gizmos::get().update_planes();   // the cut plane / flatten planes right away (we hold the core lock)
    Viewport::get().request_snapshot();
    if (n == "Text shape") {   // GLGizmoEmboss: edit the selected text, else create one (a menu passes the volume type)
        json st = Core::get().call("text.open", a);
        Viewport::get().request_snapshot();
        return st;
    }
    if (n == "SVG") {   // GLGizmoSVG: edit the selected SVG volume, or create one from the file the app picked
        json st = Core::get().call("svg.open", a);
        Viewport::get().request_snapshot();
        return st;
    }
    return nullptr;
}

} // namespace

static Commands reg({
    {"gizmo.set", set_gizmo},
    {"gizmo.info", info},
    {"gizmo.input", input},
    {"gizmo.cut_info", cut_info},
    {"gizmo.cut_set", cut_set},
    {"gizmo.cut_perform", cut_perform},
    {"gizmo.cut_connectors_edit", cut_connectors_edit},
    {"gizmo.cut_connector_tap", cut_connector_tap},
    {"gizmo.cut_connector_params", cut_connector_params},
    {"gizmo.cut_connector_move", cut_connector_move},
    {"gizmo.cut_part_toggle", cut_part_toggle},
    {"gizmo.cut_connectors_clear", cut_connectors_clear},
    {"gizmo.cut_groove", cut_groove},
    {"selection.transform_world", transform_world},
    {"selection.key_move", key_move},
    {"selection.key_rotate", key_rotate},
    {"object.lay_on_face", flatten},
});

} // namespace orca
