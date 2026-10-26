// The Measure and Assembly gizmos (GLGizmoMeasure, GLGizmoAssembly): features of the selected parts picked by touch
// (libslic3r Measure: points, edges, circles, planes), the measurement between two of them with its dimensioning, editing
// the distance (move / scale) and the assembly actions (parallel, center coincidence, flip, parallel distance, rotate).
#pragma once
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Geometry>
#include <nlohmann/json.hpp>

#include "libslic3r/Measure.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace orca {

struct Camera;

class MeasureGizmo
{
public:
    static MeasureGizmo &get();

    void           open(bool assembly);   // on_set_state(On)
    void           close();
    bool           active();
    nlohmann::json state();
    nlohmann::json tap(const Camera &cam, double px, double py);   // on_mouse LeftDown on the hovered feature
    nlohmann::json command(const nlohmann::json &a);
    void           render(const Camera &cam, const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);   // GL thread
    nlohmann::json labels(const Camera &cam);   // the dimensioning labels in screen pixels (ImGui windows on the desktop)

private:
    struct Vol
    {
        int obj, inst, vol;
        Slic3r::Transform3d world;
        std::shared_ptr<Slic3r::Measure::Measuring> measuring;
        std::shared_ptr<const Slic3r::TriangleMesh> mesh;
    };
    struct Item   // GLGizmoMeasure::SelectedFeatures::Item
    {
        bool is_center{false};
        std::optional<Slic3r::Measure::SurfaceFeature> source;
        std::optional<Slic3r::Measure::SurfaceFeature> feature;
        bool operator==(const Item &o) const { return is_center == o.is_center && source == o.source && feature == o.feature; }
        bool operator!=(const Item &o) const { return !(*this == o); }
        void reset() { is_center = false; source.reset(); feature.reset(); }
    };
    enum class AssemblyMode { FACE_FACE, POINT_POINT };

    void   update_volumes();               // register_single_mesh_pick: the selected parts and their Measuring
    void   check_model_changed();          // data_changed: an outside change resets the features
    void   update_measurement_result();
    void   reset_all_feature();
    void   reset_feature1();
    void   reset_feature2();
    bool   is_pick_meet_assembly_mode(const Item &item) const;
    bool   same_model_object() const;
    void   update_feature_by_tran(Slic3r::Measure::SurfaceFeature &feature);
    void   update_world_plane_features(Slic3r::Measure::Measuring *m, Slic3r::Measure::SurfaceFeature &f);
    void   move_volume(int vi, const Slic3r::Transform3d &T, const std::string &snapshot);   // the instance, or the part in one object
    void   after_transform();
    void   set_distance(const Slic3r::Vec3d &displacement, bool take_shot = true);
    void   set_to_parallel(bool take_shot, bool is_anti_parallel = false);
    void   set_to_reverse_rotation(int feature_index);
    void   set_to_around_center_of_faces(float rotate_degree);
    void   set_to_center_coincidence();
    void   set_parallel_distance(float dist);
    void   scale_all(double new_value, double old_value);
    std::string format_item_text(const Item &item) const;

    std::recursive_mutex                    m_mutex;
    bool                                    m_active{false};
    bool                                    m_assembly{false};      // EMeasureMode::ONLY_ASSEMBLY
    AssemblyMode                            m_assembly_mode{AssemblyMode::FACE_FACE};
    bool                                    m_point_selection{false};   // EMode::PointSelection (Shift on the desktop)
    std::vector<Vol>                        m_vols;
    Item                                    m_first, m_second;
    bool                                    m_sphere1{false}, m_sphere2{false};   // SEL_SPHERE_1 / 2 raycasters
    std::vector<int>                        m_hit_different_volumes, m_hit_order_volumes;
    int                                     m_last_hit_volume{-1};
    Slic3r::Measure::MeasurementResult      m_measurement_result;
    Slic3r::Measure::AssemblyAction         m_assembly_action;
    bool                                    m_can_set_xyz_distance{false};
    Slic3r::Vec3d                           m_distance{0, 0, 0};
    bool                                    m_show_reset_first_tip{false};
    bool                                    m_selected_wrong_feature_waring_tip{false};
    bool                                    m_flip_volume_2{false};
    unsigned                                m_known_revision{~0u};
};

} // namespace orca
