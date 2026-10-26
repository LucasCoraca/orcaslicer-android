#include "MeasureGizmo.hpp"

#include <GLES3/gl3.h>
#include <cfloat>
#include <cmath>

#include "Core.hpp"
#include "Edit.hpp"
#include "Gizmos.hpp"
#include "Plater.hpp"
#include "Viewport.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/MeasureUtils.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;
using namespace Slic3r::Measure;

namespace orca {

// GLGizmoMeasure.hpp
static const ColorRGBA SELECTED_1ST_COLOR = {0.25f, 0.75f, 0.75f, 1.0f};
static const ColorRGBA SELECTED_2ND_COLOR = {0.75f, 0.25f, 0.75f, 1.0f};
static const ColorRGBA NEUTRAL_COLOR      = {0.5f, 0.5f, 0.5f, 1.0f};
static const float     TRIANGLE_BASE      = 10.0f;
static const float     TRIANGLE_HEIGHT    = TRIANGLE_BASE * 1.618033f;
static const float     MEASURE_PLNE_NORMAL_OFFSET = 0.05f;
static const double    mm_to_in           = 1. / 25.4;   // GizmoObjectManipulation::mm_to_in

static void *vol_key(int vi) { return reinterpret_cast<void *>(intptr_t(vi + 1)); }
static int   vol_index(const void *key) { return int(reinterpret_cast<intptr_t>(key)) - 1; }

static std::string format_double(double value)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%.3f", value);
    return buf;
}

static std::string format_vec3(const Vec3d &v)
{
    char buf[128];
    snprintf(buf, sizeof buf, "X: %.3f, Y: %.3f, Z: %.3f", v.x(), v.y(), v.z());
    return buf;
}

static std::string surface_feature_type_as_string(SurfaceFeatureType type)
{
    switch (type) {
    default:
    case SurfaceFeatureType::Undef: return _u8L("No feature");
    case SurfaceFeatureType::Point: return _u8L("Vertex");
    case SurfaceFeatureType::Edge: return _u8L("Edge");
    case SurfaceFeatureType::Circle: return _u8L("Circle");
    case SurfaceFeatureType::Plane: return _u8L("Plane");
    }
}

static std::string point_on_feature_type_as_string(SurfaceFeatureType type)
{
    switch (type) {
    case SurfaceFeatureType::Point: return _u8L("Vertex");
    case SurfaceFeatureType::Edge: return _u8L("Point on edge");
    case SurfaceFeatureType::Circle: return _u8L("Point on circle");
    case SurfaceFeatureType::Plane: return _u8L("Point on plane");
    default: return "";
    }
}

static std::string center_on_feature_type_as_string(SurfaceFeatureType type)
{
    switch (type) {
    case SurfaceFeatureType::Edge: return _u8L("Center of edge");
    case SurfaceFeatureType::Circle: return _u8L("Center of circle");
    default: return "";
    }
}

static bool is_feature_with_center(const SurfaceFeature &feature)
{
    const SurfaceFeatureType type = feature.get_type();
    return (type == SurfaceFeatureType::Circle || (type == SurfaceFeatureType::Edge && feature.get_extra_point().has_value()));
}

static Vec3d get_feature_offset(const SurfaceFeature &feature)
{
    switch (feature.get_type()) {
    case SurfaceFeatureType::Circle: return std::get<0>(feature.get_circle());
    case SurfaceFeatureType::Edge: {
        const std::optional<Vec3d> extra = feature.get_extra_point();
        if (extra.has_value()) return *extra;
        const auto [pt1, pt2] = feature.get_edge();
        return 0.5 * (pt1 + pt2);
    }
    case SurfaceFeatureType::Point: return feature.get_point();
    default: return Vec3d::Zero();
    }
}

MeasureGizmo &MeasureGizmo::get()
{
    static MeasureGizmo g;
    return g;
}

bool MeasureGizmo::active()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    return m_active;
}

// register_single_mesh_pick: every selected part (GLVolume) with its world matrix and a Measuring of its mesh
void MeasureGizmo::update_volumes()
{
    Plater &p = Plater::get();
    const Model &m = p.model();
    std::vector<Vol> vols;
    for (const SelItem &s : p.selection) {
        if (s.obj < 0 || s.obj >= int(m.objects.size())) continue;
        const ModelObject *mo = m.objects[s.obj];
        for (int i = 0; i < int(mo->instances.size()); ++i) {
            if (s.inst >= 0 && s.inst != i) continue;
            for (int v = 0; v < int(mo->volumes.size()); ++v) {
                if (s.vol >= 0 && s.vol != v) continue;
                Vol vol{s.obj, i, v, mo->instances[i]->get_matrix() * mo->volumes[v]->get_matrix(), nullptr, mo->volumes[v]->mesh_ptr()};
                for (const Vol &old : m_vols)   // keep the Measuring of an unchanged mesh
                    if (old.mesh == vol.mesh) vol.measuring = old.measuring;
                if (!vol.measuring) vol.measuring = std::make_shared<Measuring>(vol.mesh->its);
                vols.push_back(std::move(vol));
            }
        }
    }
    m_vols = std::move(vols);
}

void MeasureGizmo::open(bool assembly)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    m_active = true;
    m_assembly = assembly;
    m_point_selection = false;   // m_mode = EMode::FeatureSelection
    m_vols.clear();
    update_volumes();
    reset_all_feature();
    m_known_revision = Plater::get().revision;
    Viewport::get().request_redraw();
}

void MeasureGizmo::close()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active) return;
    m_active = false;
    reset_all_feature();
    m_vols.clear();
    Viewport::get().request_redraw();
}

// data_changed: the scene changed outside the gizmo (undo, another tool): start over
void MeasureGizmo::check_model_changed()
{
    if (Plater::get().revision == m_known_revision) return;
    m_known_revision = Plater::get().revision;
    update_volumes();
    reset_all_feature();
}

void MeasureGizmo::reset_all_feature()
{
    reset_feature2();
    reset_feature1();
    m_show_reset_first_tip = false;
    m_hit_different_volumes.clear();
    m_hit_order_volumes.clear();
    m_last_hit_volume = -1;
}

void MeasureGizmo::reset_feature1()
{
    m_selected_wrong_feature_waring_tip = false;
    if (m_second.feature.has_value()) {
        if (m_hit_different_volumes.size() == 2) m_hit_different_volumes[0] = m_hit_different_volumes[1];
        if (m_hit_order_volumes.size() == 2) m_hit_order_volumes[0] = m_hit_order_volumes[1];
        m_first = m_second;
        m_sphere1 = m_sphere2;
        reset_feature2();
        m_show_reset_first_tip = true;
    } else {
        m_sphere1 = false;
        m_first.feature.reset();
        m_show_reset_first_tip = false;
        if (m_hit_different_volumes.size() == 1) m_hit_different_volumes.clear();
        if (m_hit_order_volumes.size() == 1) m_hit_order_volumes.clear();
    }
    update_measurement_result();
}

void MeasureGizmo::reset_feature2()
{
    if (m_hit_different_volumes.size() == 2) m_hit_different_volumes.erase(m_hit_different_volumes.begin() + 1);
    if (m_hit_order_volumes.size() == 2) m_hit_order_volumes.erase(m_hit_order_volumes.begin() + 1);
    m_sphere2 = false;
    m_second.reset();
    m_show_reset_first_tip = false;
    m_selected_wrong_feature_waring_tip = false;
    update_measurement_result();
}

void MeasureGizmo::update_measurement_result()
{
    if (!m_first.feature.has_value()) {
        m_measurement_result = MeasurementResult();
        m_assembly_action = AssemblyAction();
    } else if (m_second.feature.has_value()) {
        m_measurement_result = get_measurement(*m_first.feature, *m_second.feature, true);
        m_assembly_action = get_assembly_action(*m_first.feature, *m_second.feature);
        m_can_set_xyz_distance = can_set_xyz_distance(*m_first.feature, *m_second.feature);
        const MeasurementResult &measure = m_measurement_result;
        m_distance = Vec3d::Zero();
        if (measure.distance_xyz.has_value() && measure.distance_xyz->norm() > EPSILON)
            m_distance = *measure.distance_xyz;
        else if (measure.distance_infinite.has_value())
            m_distance = measure.distance_infinite->to - measure.distance_infinite->from;
        else if (measure.distance_strict.has_value())
            m_distance = measure.distance_strict->to - measure.distance_strict->from;
        if (Core::get().app_config->get_bool("use_inches")) m_distance = mm_to_in * m_distance;
    } else if (m_first.feature->get_type() == SurfaceFeatureType::Circle)
        m_measurement_result = get_measurement(*m_first.feature, SurfaceFeature(std::get<0>(m_first.feature->get_circle())));
}

bool MeasureGizmo::is_pick_meet_assembly_mode(const Item &item) const
{
    if (!m_assembly) return true;
    if (m_assembly_mode == AssemblyMode::FACE_FACE && item.feature->get_type() == SurfaceFeatureType::Plane) return true;
    if (m_assembly_mode == AssemblyMode::POINT_POINT &&
        (item.feature->get_type() == SurfaceFeatureType::Point || item.feature->get_type() == SurfaceFeatureType::Circle))
        return true;
    return false;
}

bool MeasureGizmo::same_model_object() const
{
    return m_hit_different_volumes.size() == 2 && m_vols[m_hit_different_volumes[0]].obj == m_vols[m_hit_different_volumes[1]].obj;
}

void MeasureGizmo::update_world_plane_features(Measuring *cur_measuring, SurfaceFeature &feautre)
{
    if (!cur_measuring) return;
    const auto &[idx, normal, pt] = feautre.get_plane();
    feautre.plane_indices = const_cast<std::vector<int> *>(&cur_measuring->get_plane_triangle_indices(idx));
    auto cur_plane_features = &cur_measuring->get_plane_features(idx);
    if (!feautre.world_plane_features) feautre.world_plane_features = std::make_shared<std::vector<SurfaceFeature>>();
    feautre.world_plane_features->clear();
    for (size_t i = 0; i < cur_plane_features->size(); i++) {
        SurfaceFeature temp(cur_plane_features->at(i));
        temp.translate(feautre.world_tran);
        feautre.world_plane_features->push_back(std::move(temp));
    }
}

// update_feature_by_tran: the feature again from its local original and the part's new matrix
void MeasureGizmo::update_feature_by_tran(SurfaceFeature &feature)
{
    if (!feature.volume || !feature.origin_surface_feature) return;
    const int vi = vol_index(feature.volume);
    if (vi < 0 || vi >= int(m_vols.size())) return;
    feature.world_tran = m_vols[vi].world;
    feature.clone(*feature.origin_surface_feature);
    feature.translate(feature.world_tran);
    if (feature.get_type() == SurfaceFeatureType::Plane) update_world_plane_features(m_vols[vi].measuring.get(), feature);
}

nlohmann::json MeasureGizmo::tap(const Camera &cam, double px, double py)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active) return state();
    check_model_changed();
    const double iz = Gizmos::get().inv_zoom(cam);
    const double touch = Gizmos::get().touch_px();
    const Eigen::Matrix4d vp = cam.projection(1., 10000.) * cam.view();
    auto to_ss = [&](const Vec3d &w) {
        Eigen::Vector4d c = vp * Eigen::Vector4d(w.x(), w.y(), w.z(), 1.);
        return Vec2d((c.x() / c.w() * 0.5 + 0.5) * cam.width, (0.5 - c.y() / c.w() * 0.5) * cam.height);
    };
    (void) iz;
    // the selected spheres (SEL_SPHERE_1 / 2) first
    enum { NONE = -1, FEATURE = 1, SEL_SPHERE_1 = 10, SEL_SPHERE_2 = 11 };
    int hover_id = NONE;
    if (m_sphere1 && m_first.feature && (to_ss(get_feature_offset(*m_first.feature)) - Vec2d(px, py)).norm() < touch) hover_id = SEL_SPHERE_1;
    else if (m_sphere2 && m_second.feature && (to_ss(get_feature_offset(*m_second.feature)) - Vec2d(px, py)).norm() < touch) hover_id = SEL_SPHERE_2;
    // the closest part under the finger
    Vec3d o, d;
    cam.ray(px, py, o, d);
    double best = DBL_MAX;
    int hit_vol = -1;
    size_t hit_facet = 0;
    Vec3d hit_local = Vec3d::Zero();
    for (int vi = 0; vi < int(m_vols.size()); ++vi) {
        const Vol &v = m_vols[vi];
        const Transform3d inv = v.world.inverse();
        const Vec3d lo = inv * o, ld = inv.linear() * d;
        const indexed_triangle_set &its = v.mesh->its;
        for (size_t f = 0; f < its.indices.size(); ++f) {
            const Vec3d p0 = its.vertices[its.indices[f][0]].cast<double>(), e1 = its.vertices[its.indices[f][1]].cast<double>() - p0,
                        e2 = its.vertices[its.indices[f][2]].cast<double>() - p0;
            const Vec3d pv = ld.cross(e2);
            const double det = e1.dot(pv);
            if (std::abs(det) < 1e-12) continue;
            const Vec3d tv = lo - p0;
            const double u = tv.dot(pv) / det;
            if (u < 0 || u > 1) continue;
            const Vec3d q = tv.cross(e1);
            const double vv = ld.dot(q) / det;
            if (vv < 0 || u + vv > 1) continue;
            const double t = e2.dot(q) / det;
            if (t <= 0) continue;
            const Vec3d local = lo + t * ld;
            const double dist = (v.world * local - o).norm();
            if (dist < best) { best = dist; hit_vol = vi; hit_facet = f; hit_local = local; }
        }
    }
    // the hovered feature (on_render): Measuring::get_feature at the hit, filtered by the assembly mode
    std::optional<SurfaceFeature> curr_feature;
    if (hover_id == NONE && hit_vol >= 0) {
        const bool only_select_plane = m_assembly && m_assembly_mode == AssemblyMode::FACE_FACE;
        curr_feature = m_vols[hit_vol].measuring->get_feature(hit_facet, hit_local, m_vols[hit_vol].world, only_select_plane);
        if (m_assembly && curr_feature.has_value()) {
            if (m_assembly_mode == AssemblyMode::FACE_FACE) {
                if (curr_feature->get_type() != SurfaceFeatureType::Plane) curr_feature.reset();
            } else if (!(curr_feature->get_type() == SurfaceFeatureType::Point || curr_feature->get_type() == SurfaceFeatureType::Circle ||
                         (m_point_selection && (curr_feature->get_type() == SurfaceFeatureType::Plane || curr_feature->get_type() == SurfaceFeatureType::Edge))))
                curr_feature.reset();
        }
        if (curr_feature.has_value()) {
            curr_feature->volume = vol_key(hit_vol);
            curr_feature->world_tran = m_vols[hit_vol].world;
            if (curr_feature->get_type() == SurfaceFeatureType::Plane) update_world_plane_features(m_vols[hit_vol].measuring.get(), *curr_feature);
            hover_id = FEATURE;
        }
    }
    if (hit_vol >= 0) m_last_hit_volume = hit_vol;
    if (hover_id == NONE) return state();   // a tap outside any feature does nothing

    // the point on the hovered feature (PointSelection: Shift on the desktop)
    std::optional<Vec3d> point_on_feature;
    if (m_point_selection && hover_id == FEATURE) {
        const Vec3d hit_world = m_vols[hit_vol].world * hit_local;
        switch (curr_feature->get_type()) {
        case SurfaceFeatureType::Point: point_on_feature = curr_feature->get_point(); break;
        case SurfaceFeatureType::Edge: {
            const auto [a, b] = curr_feature->get_edge();
            const Vec3d ab = b - a;
            const double t = std::clamp((hit_world - a).dot(ab) / std::max(ab.squaredNorm(), 1e-12), 0., 1.);
            point_on_feature = a + t * ab;
            break;
        }
        case SurfaceFeatureType::Plane: point_on_feature = hit_world; break;
        case SurfaceFeatureType::Circle: {
            const auto [center, radius, normal] = curr_feature->get_circle();
            const Eigen::Hyperplane<double, 3> plane(normal, center);
            const Transform3d local_to_model_matrix = Geometry::translation_transform(center) * Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), normal);
            const Vec3d local_proj = local_to_model_matrix.inverse() * plane.projection(hit_world);
            double angle = std::atan2(local_proj.y(), local_proj.x());
            if (angle < 0.0) angle += 2.0 * double(M_PI);
            point_on_feature = local_to_model_matrix * Vec3d(radius * std::cos(angle), radius * std::sin(angle), 0.0);
            break;
        }
        default: break;
        }
    }

    // on_mouse LeftDown
    auto detect_current_item = [&]() {
        Item item;
        if (hover_id == SEL_SPHERE_1) {
            if (m_first.is_center) item = {true, m_first.source, {SurfaceFeature(get_feature_offset(*m_first.source))}};
            else if (is_feature_with_center(*m_first.feature)) item = {true, m_first.feature, {SurfaceFeature(get_feature_offset(*m_first.feature))}};
            else item = m_first;
        } else if (hover_id == SEL_SPHERE_2) {
            if (m_second.is_center) item = {true, m_second.source, {SurfaceFeature(get_feature_offset(*m_second.source))}};
            else if (is_feature_with_center(*m_second.feature)) item = {true, m_second.feature, {SurfaceFeature(get_feature_offset(*m_second.feature))}};
            else item = m_second;
        } else if (!m_point_selection)
            item = {false, curr_feature, curr_feature};
        else {
            item = {false, curr_feature, SurfaceFeature(*point_on_feature)};
            auto local_pt = curr_feature->world_tran.inverse() * (*point_on_feature);
            item.feature->origin_surface_feature = std::make_shared<SurfaceFeature>(local_pt);
            item.feature->world_tran = curr_feature->world_tran;
            item.feature->volume = curr_feature->volume;
        }
        return item;
    };
    auto requires_sphere = [this](const Item &item) {
        if (m_point_selection || item.feature->get_type() == SurfaceFeatureType::Point) return true;
        return is_feature_with_center(*item.feature);
    };
    if (m_point_selection && hover_id == FEATURE && !point_on_feature) return state();

    if (m_first.feature.has_value()) {
        const Item item = detect_current_item();
        if (!is_pick_meet_assembly_mode(item)) { m_selected_wrong_feature_waring_tip = true; return state(); }
        m_selected_wrong_feature_waring_tip = false;
        if (m_first != item) {
            bool processed = false;
            if (item.is_center) {
                if (item.source == m_first.feature) { m_first = item; processed = true; }
                else if (item.source == m_second.feature) { m_second = item; processed = true; }
            } else if (is_feature_with_center(*item.feature)) {
                if (m_first.is_center && m_first.source == item.feature) { m_first = item; processed = true; }
                else if (m_second.is_center && m_second.source == item.feature) { m_second = item; processed = true; }
            }
            if (!processed) {
                m_sphere2 = false;
                if (m_second == item)
                    reset_feature2();   // 2nd feature deselection
                else {
                    m_second = item;    // 2nd feature selection
                    m_sphere2 = requires_sphere(item);
                }
            }
        } else {
            m_sphere1 = false;
            if (m_second.feature.has_value()) {   // promote 2nd feature to 1st feature
                reset_feature1();
                m_sphere1 = requires_sphere(m_first);
            } else
                reset_feature1();   // 1st feature deselection
        }
    } else {
        const Item item = detect_current_item();   // 1st feature selection
        if (!is_pick_meet_assembly_mode(item)) { m_selected_wrong_feature_waring_tip = true; return state(); }
        m_selected_wrong_feature_waring_tip = false;
        m_first = item;
        m_sphere1 = requires_sphere(item);
    }
    update_measurement_result();

    // on_render's m_mouse_left_down_mesh_deal: which parts the features are on
    if (m_last_hit_volume >= 0) {
        if (m_second.feature.has_value()) {
            if (m_hit_order_volumes.size() == 1) m_hit_order_volumes.push_back(m_last_hit_volume);
            else if (m_hit_order_volumes.size() >= 2) m_hit_order_volumes[1] = m_last_hit_volume;
            if (m_hit_different_volumes.size() >= 1) {
                if (m_last_hit_volume == m_hit_different_volumes[0]) {
                    if (m_hit_different_volumes.size() == 2) m_hit_different_volumes.erase(m_hit_different_volumes.begin() + 1);
                } else {
                    if (m_hit_different_volumes.size() == 2) m_hit_different_volumes[1] = m_last_hit_volume;
                    else m_hit_different_volumes.push_back(m_last_hit_volume);
                }
            }
        } else if (m_first.feature.has_value()) {
            if (m_hit_order_volumes.empty()) m_hit_order_volumes.push_back(m_last_hit_volume);
            else m_hit_order_volumes[0] = m_last_hit_volume;
            if (m_hit_different_volumes.empty()) m_hit_different_volumes.push_back(m_last_hit_volume);
        }
    }
    Viewport::get().request_redraw();
    return state();
}

// ----------------------------------------------------------------------------------------------- moving the parts

// the moving part (feature 2's): its instance, or the part itself when both features are on one object
void MeasureGizmo::move_volume(int vi, const Transform3d &T, const std::string &snapshot)
{
    const Vol &v = m_vols[vi];
    if (same_model_object()) apply_world_to(T, snapshot, {}, {SelItem{v.obj, v.inst, v.vol}});
    else apply_world_to(T, snapshot, {{v.obj, v.inst}}, {});
}

// register_single_mesh_pick + update_feature_by_tran after a change made here
void MeasureGizmo::after_transform()
{
    update_volumes();
    m_known_revision = Plater::get().revision;
    // the desktop updates only .feature; .source of a whole-feature selection follows it here, or the item would read as
    // a "Point on ..." afterwards
    for (Item *item : {&m_first, &m_second}) {
        if (!item->feature) continue;
        const bool whole = item->source == item->feature;
        update_feature_by_tran(*item->feature);
        if (whole) item->source = item->feature;
    }
    update_measurement_result();
    Viewport::get().request_redraw();
}

void MeasureGizmo::set_distance(const Vec3d &displacement, bool take_shot)
{
    if (m_hit_different_volumes.size() != 2 || displacement.norm() <= 0.0) return;
    move_volume(m_hit_different_volumes[1], Geometry::translation_transform(displacement), take_shot ? "MoveInMeasure" : "");
    after_transform();
}

void MeasureGizmo::set_to_parallel(bool take_shot, bool is_anti_parallel)
{
    if (m_hit_different_volumes.size() != 2) return;
    const auto [idx1, normal1, pt1] = m_first.feature->get_plane();
    const auto [idx2, normal2, pt2] = m_second.feature->get_plane();
    if ((is_anti_parallel && normal1.dot(normal2) > -1 + 1e-3) || (!is_anti_parallel && normal1.dot(normal2) < 1 - 1e-3)) {
        Vec3d axis;
        double angle;
        Matrix3d rotation_matrix;
        Geometry::rotation_from_two_vectors(normal2, -normal1, axis, angle, &rotation_matrix);
        // the rotation keeps the instance's (or the part's) origin where it is
        const Vol &v = m_vols[m_hit_different_volumes[1]];
        const ModelObject *mo = Plater::get().model().objects[v.obj];
        const Vec3d origin = same_model_object() ? Vec3d(v.world.translation()) : Vec3d(mo->instances[v.inst]->get_offset());
        Transform3d r_m = Transform3d::Identity();
        r_m.linear() = rotation_matrix;
        move_volume(m_hit_different_volumes[1], Geometry::translation_transform(origin) * r_m * Geometry::translation_transform(-origin),
                    take_shot ? "RotateInMeasure" : "");
        after_transform();
    }
}

void MeasureGizmo::set_to_reverse_rotation(int feature_index)
{
    if (m_hit_different_volumes.size() != 2 || feature_index >= 2) return;
    const auto [idx, plane_normal, plane_center] = (feature_index == 0 ? m_first : m_second).feature->get_plane();
    const Vec3d new_pt = get_one_point_in_plane(plane_center, plane_normal);
    const Vec3d axis = (new_pt - plane_center).normalized();
    const Transform3d R = Geometry::translation_transform(plane_center) * Transform3d(Eigen::AngleAxisd(PI, axis)) * Geometry::translation_transform(-plane_center);
    move_volume(m_hit_different_volumes[feature_index], R, "ReverseRotateInMeasure");
    after_transform();
}

void MeasureGizmo::set_to_around_center_of_faces(float rotate_degree)
{
    if (m_hit_different_volumes.size() != 2) return;
    const auto [idx2, normal2, pt2] = m_second.feature->get_plane();
    const Transform3d R = Geometry::translation_transform(pt2) * Transform3d(Eigen::AngleAxisd(Geometry::deg2rad(double(rotate_degree)), normal2)) *
                          Geometry::translation_transform(-pt2);
    move_volume(m_hit_different_volumes[1], R, "ReverseRotateInMeasure");
    after_transform();
}

void MeasureGizmo::set_to_center_coincidence()
{
    if (m_hit_different_volumes.size() != 2) return;
    Plater::get().take_snapshot("RotateThenMoveInMeasure");
    set_to_parallel(false, true);
    const auto [idx1, normal1, pt1] = m_first.feature->get_plane();
    const auto [idx2, normal2, pt2] = m_second.feature->get_plane();
    set_distance(pt1 - pt2, false);
}

void MeasureGizmo::set_parallel_distance(float dist)
{
    if (m_hit_different_volumes.size() != 2) return;
    const auto [idx1, normal1, pt1] = m_first.feature->get_plane();
    const auto [idx2, normal2, pt2] = m_second.feature->get_plane();
    Vec3d proj_pt2;
    get_point_projection_to_plane(pt2, pt1, normal1, proj_pt2);
    const Vec3d new_pt2 = proj_pt2 + normal1 * dist;
    move_volume(m_hit_different_volumes[1], Geometry::translation_transform(new_pt2 - pt2), "SetParallelDistanceInMeasure");
    after_transform();
}

// the distance label's "Edit to scale": the whole selection scaled about its centre (TransformationType world relative joint)
void MeasureGizmo::scale_all(double new_value, double old_value)
{
    if (new_value == old_value || new_value <= 0.0 || old_value <= 0.0) return;
    const double ratio = new_value / old_value;
    const Vec3d c = selection_world_box().center();
    apply_world(Geometry::translation_transform(c) * Geometry::scale_transform(ratio * Vec3d::Ones()) * Geometry::translation_transform(-c), _u8L("Scale"));
    after_transform();
}

std::string MeasureGizmo::format_item_text(const Item &item) const
{
    if (!item.feature.has_value()) return _u8L("None");
    const bool use_inches = Core::get().app_config->get_bool("use_inches");
    const std::string units = " " + (use_inches ? _u8L("in") : _u8L("mm"));
    std::string text = (item.source == item.feature) ? surface_feature_type_as_string(item.feature->get_type()) :
                       item.is_center                ? center_on_feature_type_as_string(item.source->get_type()) :
                                                       point_on_feature_type_as_string(item.source->get_type());
    if (item.feature->get_type() == SurfaceFeatureType::Circle) {
        auto [center, radius, normal] = item.feature->get_circle();
        const Vec3d on_circle = center + radius * get_orthogonal(normal, true);
        radius = (on_circle - center).norm();
        if (use_inches) radius = mm_to_in * radius;
        text += " (" + _u8L("Diameter") + ": " + format_double(2.0 * radius) + units + ")";
    } else if (item.feature->get_type() == SurfaceFeatureType::Edge) {
        auto [start, end] = item.feature->get_edge();
        double length = (end - start).norm();
        if (use_inches) length = mm_to_in * length;
        text += " (" + _u8L("Length") + ": " + format_double(length) + units + ")";
    }
    return text;
}

// on_render_input_window (+ show_selection_ui, show_distance_xyz_ui, the assembly parts, render_input_window_warning)
nlohmann::json MeasureGizmo::state()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active) return {{"active", false}};
    check_model_changed();
    const bool use_inches = Core::get().app_config->get_bool("use_inches");
    const std::string units = " " + (use_inches ? _u8L("in") : _u8L("mm"));
    const bool face_face = m_assembly && m_assembly_mode == AssemblyMode::FACE_FACE;
    json st = {{"active", true}, {"assembly", m_assembly}, {"assembly_mode", face_face ? 0 : 1}, {"point_selection", m_point_selection}};
    json labels = {{"modes", {_u8L("Face and face assembly"), _u8L("Point and point assembly")}}, {"mode", _u8L("Mode")},
                   {"point_selection", _u8L("Select point")}, {"done", _u8L("Done")}, {"measure", _u8L("Measure")}};
    if (m_assembly)
        labels["tip"] = face_face ? _u8L("Select 2 faces on objects and \n make objects assemble together.")
                                  : _u8L("Select 2 points or circles on objects and \n specify distance between them.");
    labels["sel1"] = m_assembly ? (face_face ? _u8L("Face 1 (Fixed)") : _u8L("Point 1 (Fixed)")) : _u8L("Selection 1");
    labels["sel2"] = m_assembly ? (face_face ? _u8L("Face 2 (Moving)") : _u8L("Point 2 (Moving)")) : _u8L("Selection 2");
    st["sel1"] = format_item_text(m_first);
    st["sel2"] = format_item_text(m_second);
    st["has1"] = m_first.feature.has_value();
    st["has2"] = m_second.feature.has_value();
    if (m_show_reset_first_tip) st["reset_first_tip"] = _u8L("Feature 1 has been reset, \nfeature 2 has been feature 1");
    // the results table
    json rows = json::array();
    json xyz;
    if (m_second.feature.has_value()) {
        const MeasurementResult &measure = m_measurement_result;
        if (measure.angle.has_value() && !m_assembly)
            rows.push_back({{"label", _u8L("Angle")}, {"value", format_double(Geometry::rad2deg(measure.angle->angle)) + "°"}});
        const bool show_strict = measure.distance_strict.has_value() &&
                                 (!measure.distance_infinite.has_value() || std::abs(measure.distance_strict->dist - measure.distance_infinite->dist) > EPSILON);
        if (measure.distance_infinite.has_value() && !m_assembly) {
            double distance = measure.distance_infinite->dist;
            if (use_inches) distance = mm_to_in * distance;
            rows.push_back({{"label", show_strict ? _u8L("Perpendicular distance") : _u8L("Distance")}, {"value", format_double(distance) + units}});
        }
        if (show_strict && (!m_assembly || m_assembly_mode == AssemblyMode::POINT_POINT)) {
            double distance = measure.distance_strict->dist;
            if (use_inches) distance = mm_to_in * distance;
            rows.push_back({{"label", _u8L("Direct distance")}, {"value", format_double(distance) + units}});
        }
        if (measure.distance_xyz.has_value() && !m_assembly && measure.distance_xyz->norm() > EPSILON) {
            Vec3d distance = *measure.distance_xyz;
            if (use_inches) distance = mm_to_in * distance;
            rows.push_back({{"label", _u8L("Distance XYZ")}, {"value", format_vec3(distance)}});
        }
        if (m_distance.norm() > 0.01 && !face_face) {
            const bool can = m_assembly && m_can_set_xyz_distance;   // ONLY_MEASURE: m_can_set_xyz_distance = false
            const bool single = m_hit_different_volumes.size() == 1;
            xyz = {{"values", {m_distance.x(), m_distance.y(), m_distance.z()}},
                   {"enabled", {!single && can, !single && can, !single && same_model_object() && can}}};
        }
    }
    st["rows"] = rows;
    if (!xyz.is_null()) st["xyz"] = xyz;
    // GLGizmoAssembly: the face and face actions
    if (m_assembly && m_hit_different_volumes.size() == 2 && m_first.feature && m_second.feature &&
        m_first.feature->get_type() == SurfaceFeatureType::Plane && m_second.feature->get_type() == SurfaceFeatureType::Plane) {
        json a = {{"center_coincidence", m_assembly_action.can_set_to_center_coincidence}, {"parallel", m_assembly_action.can_set_to_parallel},
                  {"flip", m_flip_volume_2}};
        if (m_assembly_action.has_parallel_distance) a["parallel_distance"] = m_assembly_action.parallel_distance;
        if (m_assembly_action.can_around_center_of_faces) a["rotate_around_center"] = true;
        st["actions"] = a;
        labels["center_coincidence"] = _u8L("Center coincidence");
        labels["parallel"] = _u8L("Parallel");
        labels["flip"] = _u8L("Flip by Face 2");
        labels["parallel_distance"] = _u8L("Parallel distance:");
        labels["rotate_around_center"] = _u8L("Rotate around center:");
    }
    // render_input_window_warning
    json warnings = json::array();
    if (m_assembly) {
        if (m_hit_different_volumes.size() == 1) warnings.push_back(_u8L("Warning: please select two different meshes."));
        if (m_selected_wrong_feature_waring_tip)
            warnings.push_back(face_face ? _u8L("Warning: please select Plane's feature.") : _u8L("Warning: please select Point's or Circle's feature."));
        if (m_hit_different_volumes.size() == 2 && !same_model_object())
            warnings.push_back(_u8L("Warning") + ": " +
                               _u8L("It is recommended to assemble objects first,\nbecause they are restricted to the bed \nand only parts can be lifted."));
    }
    st["warnings"] = warnings;
    st["labels"] = labels;
    return st;
}

nlohmann::json MeasureGizmo::command(const nlohmann::json &a)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active) return state();
    check_model_changed();
    const std::string what = a.value("what", std::string());
    if (what == "reset1") reset_feature1();
    else if (what == "reset2") reset_feature2();
    else if (what == "reset_all") reset_all_feature();
    else if (what == "point_selection") m_point_selection = a.value("on", false);
    else if (what == "assembly_mode") {   // switch_to_mode
        m_assembly_mode = a.value("mode", 0) == 0 ? AssemblyMode::FACE_FACE : AssemblyMode::POINT_POINT;
        reset_all_feature();
    } else if (what == "distance" && m_hit_different_volumes.size() == 2) {   // the X / Y / Z boxes
        const int axis = a.at("axis");
        const double value = a.at("value");
        Vec3d displacement = Vec3d::Zero();
        displacement[axis] = value - m_distance[axis];
        if (Core::get().app_config->get_bool("use_inches")) displacement /= mm_to_in;
        if (std::abs(displacement[axis]) > EPSILON) set_distance(displacement);
    } else if (what == "scale") scale_all(a.at("value").get<double>(), a.at("old").get<double>());
    else if (what == "center_coincidence") set_to_center_coincidence();
    else if (what == "parallel") set_to_parallel(true);
    else if (what == "flip") { m_flip_volume_2 = !m_flip_volume_2; set_to_reverse_rotation(1); }
    else if (what == "parallel_distance") set_parallel_distance(a.at("value").get<float>());
    else if (what == "rotate_around_center") set_to_around_center_of_faces(a.at("value").get<float>());
    Viewport::get().request_redraw();
    return state();
}

// ----------------------------------------------------------------------------------------------- rendering

namespace {

std::shared_ptr<const TriangleMesh> unit_sphere()
{
    static auto m = std::make_shared<const TriangleMesh>(its_make_sphere(1.0, 2. * PI / 16.));
    return m;
}
std::shared_ptr<const TriangleMesh> unit_cylinder()
{
    static auto m = std::make_shared<const TriangleMesh>(its_make_cylinder(1.0, 1.0, 2. * PI / 16.));
    return m;
}

// init_torus_data
indexed_triangle_set torus(const Vec3d &center, double radius, double thickness, const Vec3d &axis)
{
    const unsigned torus_sector_count = 64, section_sector_count = 16;
    const double torus_sector_step = 2.0 * PI / torus_sector_count, section_sector_step = 2.0 * PI / section_sector_count;
    indexed_triangle_set its;
    const Transform3d local_to_world = Geometry::translation_transform(center) * Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), axis);
    for (unsigned i = 0; i < torus_sector_count; ++i) {
        const double section_angle = torus_sector_step * i;
        const Vec3d radius_dir(std::cos(section_angle), std::sin(section_angle), 0.0);
        const Vec3d local_section_center = radius * radius_dir;
        const Vec3d world_section_center = local_to_world * local_section_center;
        const Vec3d local_section_normal = local_section_center.normalized().cross(Vec3d::UnitZ()).normalized();
        const Vec3d world_section_normal = (local_to_world.linear() * local_section_normal).normalized();
        const Vec3d base_v = thickness * (local_to_world.linear() * radius_dir);
        for (unsigned j = 0; j < section_sector_count; ++j) {
            const Vec3d v = Eigen::AngleAxisd(section_sector_step * j, world_section_normal) * base_v;
            its.vertices.emplace_back((world_section_center + v).cast<float>());
        }
    }
    for (unsigned i = 0; i < torus_sector_count; ++i) {
        const unsigned ii = i * section_sector_count, ii_next = ((i + 1) % torus_sector_count) * section_sector_count;
        for (unsigned j = 0; j < section_sector_count; ++j) {
            const unsigned j_next = (j + 1) % section_sector_count;
            its.indices.emplace_back(ii + j, ii_next + j, ii_next + j_next);
            its.indices.emplace_back(ii + j, ii_next + j_next, ii + j_next);
        }
    }
    return its;
}

} // namespace

void MeasureGizmo::render(const Camera &cam, const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active || !m_first.feature) return;
    const double iz = Gizmos::get().inv_zoom(cam) * Gizmos::get().ui_scale();
    auto color4 = [](const ColorRGBA &c) { return std::array<float, 4>{c.r(), c.g(), c.b(), c.a()}; };
    auto draw = [&](const std::shared_ptr<const TriangleMesh> &mesh, const Transform3d &m, const ColorRGBA &c) {
        const auto col = color4(c);
        gl_draw_lit(mesh, m.matrix().cast<float>(), col.data(), view, proj);
    };
    // the meshes made per feature (torus, plane) are cached by what they show
    static std::map<std::string, std::shared_ptr<const TriangleMesh>> cache;
    if (cache.size() > 32) cache.clear();
    auto render_feature = [&](const SurfaceFeature &feature, const std::vector<ColorRGBA> &colors) {
        switch (feature.get_type()) {
        case SurfaceFeatureType::Point:
            draw(unit_sphere(), Geometry::translation_transform(feature.get_point()) * Geometry::scale_transform(7.5 * iz), colors.front());
            break;
        case SurfaceFeatureType::Circle: {
            const auto &[center, radius, normal] = feature.get_circle();
            char key[160];
            snprintf(key, sizeof key, "c%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.5f", center.x(), center.y(), center.z(), radius, normal.x(), normal.y(), normal.z(), iz);
            auto &m = cache[key];
            if (!m) m = std::make_shared<const TriangleMesh>(torus(center, radius, 5.0 * iz, normal));
            draw(m, Transform3d::Identity(), colors.front());
            if (colors.size() > 1) draw(unit_sphere(), Geometry::translation_transform(center) * Geometry::scale_transform(7.5 * iz), colors.back());
            break;
        }
        case SurfaceFeatureType::Edge: {
            const auto &[from, to] = feature.get_edge();
            const Transform3d edge_matrix = Geometry::translation_transform(from) * Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), to - from) *
                                            Geometry::scale_transform({5.0 * iz, 5.0 * iz, (to - from).norm()});
            draw(unit_cylinder(), edge_matrix, colors.front());
            if (colors.size() > 1)
                if (const std::optional<Vec3d> extra = feature.get_extra_point(); extra.has_value())
                    draw(unit_sphere(), Geometry::translation_transform(*extra) * Geometry::scale_transform(7.5 * iz), colors.back());
            break;
        }
        case SurfaceFeatureType::Plane: {
            if (!feature.plane_indices || !feature.volume) break;
            const int vi = vol_index(feature.volume);
            if (vi < 0 || vi >= int(m_vols.size())) break;
            const auto &[idx, normal, pt] = feature.get_plane();
            char key[64];
            snprintf(key, sizeof key, "p%p:%d", (const void *) m_vols[vi].mesh.get(), idx);
            auto &m = cache[key];
            if (!m) {   // init_plane_data: the plane's triangles lifted along their normal
                const indexed_triangle_set &its = m_vols[vi].mesh->its;
                indexed_triangle_set out;
                for (int t : *feature.plane_indices) {
                    Vec3f v0 = its.vertices[its.indices[t][0]], v1 = its.vertices[its.indices[t][1]], v2 = its.vertices[its.indices[t][2]];
                    const Vec3f n = (v1 - v0).cross(v2 - v0).normalized() * MEASURE_PLNE_NORMAL_OFFSET;
                    const int b = int(out.vertices.size());
                    out.vertices.insert(out.vertices.end(), {v0 + n, v1 + n, v2 + n});
                    out.indices.emplace_back(b, b + 1, b + 2);
                }
                m = std::make_shared<const TriangleMesh>(std::move(out));
            }
            draw(m, feature.world_tran, colors.back());
            break;
        }
        default: break;
        }
    };
    glDisable(GL_CULL_FACE);
    glClear(GL_DEPTH_BUFFER_BIT);   // the features show through the objects
    glEnable(GL_DEPTH_TEST);
    auto render_selected = [&](const Item &item, const ColorRGBA &color) {
        if (is_feature_with_center(*item.feature)) render_feature(*item.feature, {color, NEUTRAL_COLOR});
        else render_feature(*item.feature, {color});
    };
    render_selected(m_first, SELECTED_1ST_COLOR);
    if (m_second.feature) render_selected(m_second, SELECTED_2ND_COLOR);

    // render_dimensioning, in screen space (pixels -> NDC) like the desktop's ss_to_ndc_matrix
    if (!m_second.feature && m_first.feature->get_type() != SurfaceFeatureType::Circle) return;
    const Eigen::Matrix4d vp = (proj.cast<double>() * view.cast<double>());
    auto to_ss = [&](const Vec3d &w) {
        Eigen::Vector4d c = vp * Eigen::Vector4d(w.x(), w.y(), w.z(), 1.);
        return Vec2d((c.x() / c.w() * 0.5 + 0.5) * cam.width, (c.y() / c.w() * 0.5 + 0.5) * cam.height);   // y up, as GL
    };
    const double ui = Gizmos::get().ui_scale();
    std::vector<float> tris;
    auto ndc = [&](const Vec2d &p) { return Vec2f(float(p.x() / cam.width * 2. - 1.), float(p.y() / cam.height * 2. - 1.)); };
    auto add_tri = [&](const Vec2d &a, const Vec2d &b, const Vec2d &c) {
        for (const Vec2d *p : {&a, &b, &c}) { Vec2f n = ndc(*p); tris.insert(tris.end(), {n.x(), n.y(), 0.f}); }
    };
    auto flush = [&](const ColorRGBA &c) {
        if (!tris.empty()) gl_draw_flat(tris, GL_TRIANGLES, Eigen::Matrix4f::Identity(), c);
        tris.clear();
    };
    auto thick = [&](const Vec2d &a, const Vec2d &b, double w) {
        const Vec2d d = b - a;
        if (d.norm() < 1e-9) return;
        const Vec2d n = Vec2d(-d.y(), d.x()).normalized() * (0.5 * w);
        add_tri(a - n, b - n, b + n);
        add_tri(a - n, b + n, a + n);
    };
    auto arrow = [&](const Vec2d &tip, const Vec2d &dir) {   // m_dimensioning.triangle: the tip at the point, pointing along dir
        const Vec2d u = dir.normalized(), n(-u.y(), u.x());
        const double h = TRIANGLE_HEIGHT * ui, b = TRIANGLE_BASE * ui;
        add_tri(tip, tip - u * h + n * (0.5 * b), tip - u * h - n * (0.5 * b));
    };
    auto point_point = [&](const Vec3d &v1, const Vec3d &v2, double distance, const ColorRGBA &color, bool show_first_tri = true) {
        if ((v2 - v1).squaredNorm() < 0.000001 || std::abs(distance) < 0.001) return;
        const Vec2d v1ss = to_ss(v1), v2ss = to_ss(v2);
        if (v1ss.isApprox(v2ss)) return;
        const Vec2d v12ss = v2ss - v1ss;
        const double len = v12ss.norm();
        const bool overlap = len - 2.0 * TRIANGLE_HEIGHT * ui < 0.0;
        const Vec2d u = v12ss / len;
        if (overlap) thick(v1ss - u * (2.0 * TRIANGLE_HEIGHT * ui), v2ss + u * (2.0 * TRIANGLE_HEIGHT * ui), 2.0 * ui);
        else thick(v1ss, v2ss, 2.0 * ui);
        if (show_first_tri) arrow(v1ss, overlap ? u : -u);
        arrow(v2ss, overlap ? -u : u);
        flush(color);
    };
    glDisable(GL_DEPTH_TEST);
    const SurfaceFeature *f1 = &*m_first.feature;
    std::unique_ptr<SurfaceFeature> temp_feature;
    const SurfaceFeature *f2;
    if (m_second.feature) f2 = &*m_second.feature;
    else { temp_feature = std::make_unique<SurfaceFeature>(std::get<0>(m_first.feature->get_circle())); f2 = temp_feature.get(); }
    SurfaceFeatureType ft1 = f1->get_type(), ft2 = f2->get_type();
    if (ft1 > ft2) { std::swap(ft1, ft2); std::swap(f1, f2); }
    // the angle arc (arc_edge_edge / arc_edge_plane / arc_plane_plane)
    if (m_measurement_result.angle.has_value() &&
        ((ft1 == SurfaceFeatureType::Edge && ft2 == SurfaceFeatureType::Edge) || (ft1 == SurfaceFeatureType::Edge && ft2 == SurfaceFeatureType::Plane) ||
         (ft1 == SurfaceFeatureType::Plane && ft2 == SurfaceFeatureType::Plane))) {
        const AngleAndEdges &ang = *m_measurement_result.angle;
        const bool edge_edge = ft1 == SurfaceFeatureType::Edge && ft2 == SurfaceFeatureType::Edge;
        const double radius = edge_edge ? 0.0 : ang.radius;
        if (std::abs(ang.angle) >= EPSILON && std::abs(ang.radius) >= EPSILON && (edge_edge || ang.radius != 0.0)) {
            const double draw_radius = radius > 0.0 ? radius : ang.radius;
            const Vec3d e1_unit = edge_direction(ang.e1.first, ang.e1.second), e2_unit = edge_direction(ang.e2.first, ang.e2.second);
            const unsigned resolution = std::max<unsigned>(2, unsigned(64 * ang.angle / PI));
            const double step = ang.angle / double(resolution);
            const Vec3d normal = e1_unit.cross(e2_unit).normalized();
            auto arc_point = [&](double a) { return Vec3d(ang.center + draw_radius * (Eigen::Quaterniond(Eigen::AngleAxisd(a, normal)) * e1_unit)); };
            for (unsigned i = 0; i < resolution; ++i) thick(to_ss(arc_point(step * i)), to_ss(arc_point(step * (i + 1))), 2.0 * ui);
            // arrows at both ends, along the arc
            const Vec2d s0 = to_ss(arc_point(0)), s0b = to_ss(arc_point(step * 0.5));
            const Vec2d s1 = to_ss(arc_point(ang.angle)), s1b = to_ss(arc_point(ang.angle - step * 0.5));
            arrow(s0, s0 - s0b);
            arrow(s1, s1 - s1b);
            flush(ColorRGBA::WHITE());
            // the edges' extensions to the centre
            const Vec3d e11e12 = ang.e1.second - ang.e1.first, e11center = ang.center - ang.e1.first;
            if (e11center.norm() > EPSILON && e11center.dot(e11e12) < 0.0) thick(to_ss(ang.center), to_ss(ang.center + e1_unit * e11center.norm()), 1.0 * ui);
            const Vec3d e21center = ang.center - ang.e2.first;
            if (e21center.norm() > EPSILON)
                thick(to_ss(ang.center), to_ss(ang.center + e2_unit * ((ang.coplanar && radius > 0.0) ? e21center.norm() : draw_radius)), 1.0 * ui);
            flush(ColorRGBA::LIGHT_GRAY());
        }
    }
    if (m_measurement_result.has_distance_data()) {
        // point_edge: the edge's extension to where the distance is measured
        if (ft1 == SurfaceFeatureType::Point && ft2 == SurfaceFeatureType::Edge && m_measurement_result.distance_infinite) {
            const auto e = f2->get_edge();
            const Vec3d v_proj = m_measurement_result.distance_infinite->to, e1e2 = e.second - e.first, v_proje1 = v_proj - e.first;
            const bool on_e1_side = v_proje1.dot(e1e2) < -EPSILON, on_e2_side = !on_e1_side && v_proje1.norm() > e1e2.norm();
            if (on_e1_side || on_e2_side) { thick(to_ss(on_e1_side ? e.first : e.second), to_ss(v_proj), 1.0 * ui); flush(ColorRGBA::LIGHT_GRAY()); }
        }
        const DistAndPoints &dap = m_measurement_result.distance_infinite ? *m_measurement_result.distance_infinite : *m_measurement_result.distance_strict;
        if (m_second.feature && !(m_assembly && m_assembly_mode == AssemblyMode::FACE_FACE)) {
            Vec3d x_to = dap.from; x_to[0] = dap.to[0];
            point_point(dap.from, x_to, x_to[0] - dap.from[0], ColorRGBA::RED(), false);
            Vec3d y_to = x_to; y_to[1] = dap.to[1];
            point_point(x_to, y_to, y_to[1] - x_to[1], ColorRGBA::GREEN(), false);
            point_point(y_to, dap.to, dap.to[2] - y_to[2], ColorRGBA::BLUE(), false);
        }
        point_point(dap.from, dap.to, dap.dist, ColorRGBA::WHITE());
    }
    glEnable(GL_DEPTH_TEST);
}

// the labels of render_dimensioning, placed by the app over the 3D view
nlohmann::json MeasureGizmo::labels(const Camera &cam)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    json out = json::array();
    if (!m_active || !m_first.feature || (!m_second.feature && m_first.feature->get_type() != SurfaceFeatureType::Circle)) return out;
    const Eigen::Matrix4d vp = cam.projection(1., 10000.) * cam.view();
    auto to_ss = [&](const Vec3d &w) {
        Eigen::Vector4d c = vp * Eigen::Vector4d(w.x(), w.y(), w.z(), 1.);
        return Vec2d((c.x() / c.w() * 0.5 + 0.5) * cam.width, (0.5 - c.y() / c.w() * 0.5) * cam.height);
    };
    const bool use_inches = Core::get().app_config->get_bool("use_inches");
    const std::string units = use_inches ? _u8L("in") : _u8L("mm");
    const SurfaceFeature *f1 = &*m_first.feature;
    std::unique_ptr<SurfaceFeature> temp_feature;
    const SurfaceFeature *f2;
    if (m_second.feature) f2 = &*m_second.feature;
    else { temp_feature = std::make_unique<SurfaceFeature>(std::get<0>(m_first.feature->get_circle())); f2 = temp_feature.get(); }
    SurfaceFeatureType ft1 = f1->get_type(), ft2 = f2->get_type();
    if (ft1 > ft2) { std::swap(ft1, ft2); std::swap(f1, f2); }
    if (m_measurement_result.angle.has_value() &&
        ((ft1 == SurfaceFeatureType::Edge && (ft2 == SurfaceFeatureType::Edge || ft2 == SurfaceFeatureType::Plane)) ||
         (ft1 == SurfaceFeatureType::Plane && ft2 == SurfaceFeatureType::Plane))) {
        const AngleAndEdges &ang = *m_measurement_result.angle;
        const bool edge_edge = ft1 == SurfaceFeatureType::Edge && ft2 == SurfaceFeatureType::Edge;
        if (std::abs(ang.angle) >= EPSILON && std::abs(ang.radius) >= EPSILON) {
            const Vec3d e1_unit = edge_direction(ang.e1.first, ang.e1.second), e2_unit = edge_direction(ang.e2.first, ang.e2.second);
            const Vec3d normal = e1_unit.cross(e2_unit).normalized();
            const Vec3d pos = ang.center + ang.radius * (Eigen::Quaterniond(Eigen::AngleAxisd(0.5 * ang.angle, normal)) * e1_unit);
            const Vec2d s = to_ss(pos);
            out.push_back({{"x", s.x()}, {"y", s.y()}, {"text", format_double(Geometry::rad2deg(ang.angle)) + "°"}});
            (void) edge_edge;
        }
    }
    if (m_measurement_result.has_distance_data()) {
        const DistAndPoints &dap = m_measurement_result.distance_infinite ? *m_measurement_result.distance_infinite : *m_measurement_result.distance_strict;
        if ((dap.to - dap.from).squaredNorm() >= 0.000001 && std::abs(dap.dist) >= 0.001) {
            const Vec2d s = 0.5 * (to_ss(dap.from) + to_ss(dap.to));
            const double value = use_inches ? mm_to_in * dap.dist : dap.dist;
            // "Edit to scale" only when both features are on one part (m_hit_different_volumes.size() < 2)
            out.push_back({{"x", s.x()}, {"y", s.y()}, {"text", format_double(value) + " " + units}, {"value", value},
                           {"editable", m_hit_different_volumes.size() < 2}});
        }
    }
    return out;
}

static Commands reg({
    {"measure.state", [](const json &) { return MeasureGizmo::get().state(); }},
    {"measure.tap", [](const json &a) { return MeasureGizmo::get().tap(Viewport::get().camera(), a.at("x").get<double>(), a.at("y").get<double>()); }},
    {"measure.command", [](const json &a) { return MeasureGizmo::get().command(a); }},
    {"measure.labels", [](const json &) { return MeasureGizmo::get().labels(Viewport::get().camera()); }},
});

} // namespace orca
