// The Brim Ears gizmo (GLGizmoBrimEars): ear points under the selected instance, added by tapping the object or
// auto-generated from the first layer's corners, stored in ModelObject::brim_points (used with brim type "painted").
// The geometry functions are compiled from upstream (tools/extract_functions.py -> BrimEars.inc).
#pragma once
#include <map>
#include <mutex>
#include <vector>

#include <Eigen/Geometry>
#include <nlohmann/json.hpp>

#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Polygon.hpp"

namespace orca {

struct Camera;

class BrimEarsGizmo
{
public:
    static BrimEarsGizmo &get();

    nlohmann::json open();    // on_set_state(On): first layer, cache from the model
    void           close();   // on_set_state(Off): "Brim ears edit" snapshot + update_model_object
    bool           active();
    int            object() const { return m_obj; }   // the edited object (its instance: instance())
    int            instance() const { return m_inst; }
    nlohmann::json state();
    nlohmann::json set(const nlohmann::json &a);   // radius, max_angle, detection_radius, painted (set brim type)
    nlohmann::json action(const std::string &what);   // auto_generate, remove_selected, remove_all, select_all
    // touch: a tap adds / selects (gizmo_event LeftDown), a long press removes (RightDown), a drag moves an ear
    nlohmann::json tap(const Camera &cam, double px, double py, bool remove);
    bool           drag_begin(const Camera &cam, double px, double py);
    void           drag_move(const Camera &cam, double px, double py);
    void           drag_end();
    void           render(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);   // GL thread

    // GLGizmoBrimEars members the extracted functions use
    class CacheEntry
    {
    public:
        CacheEntry() : brim_point(Slic3r::BrimPoint()), selected(false), normal(Slic3r::Vec3f(0, 0, 1)), is_hover(false), is_error(false) {}
        CacheEntry(const Slic3r::BrimPoint &point, bool sel = false, const Slic3r::Vec3f &norm = Slic3r::Vec3f(0, 0, 1), bool hover = false, bool error = false)
            : brim_point(point), selected(sel), normal(norm), is_hover(hover), is_error(error)
        {}
        bool operator==(const CacheEntry &rhs) const { return (brim_point == rhs.brim_point); }
        bool operator!=(const CacheEntry &rhs) const { return !((*this) == rhs); }
        Slic3r::BrimPoint brim_point;
        bool              selected;
        Slic3r::Vec3f     normal;
        bool              is_hover;
        bool              is_error;
    };
    enum { AllPoints = -2, NoPoints };

    Slic3r::Points    generate_points(Slic3r::Polygon &obj_polygon, float ear_detection_length, float brim_ears_max_angle, bool is_outer);
    void              get_detection_radius_max();
    bool              add_point_to_cache(Slic3r::Vec3f pos, float head_radius, bool selected, Slic3r::Vec3f normal);
    Slic3r::ExPolygon make_polygon(Slic3r::BrimPoint point, const Slic3r::Geometry::Transformation &trsf);
    void              find_single();
    void              select_point(int i);
    void              unselect_point(int i);

private:
    void update_model_object();
    void first_layer_slicer();
    void reload_cache();
    void delete_selected_points();
    Slic3r::Geometry::Transformation instance_transformation() const;
    int  pick_point(const Camera &cam, double px, double py);
    bool unproject_on_mesh2(const Camera &cam, double px, double py, Slic3r::Vec3f &pos);
    float get_brim_default_radius() const;

    std::recursive_mutex                  m_mutex;
    bool                                  m_active{false};
    int                                   m_obj{-1}, m_inst{0};
    float                                 m_new_point_head_radius{0.f};
    float                                 m_max_angle = 125.f;
    float                                 m_detection_radius = 1.f;
    double                                m_detection_radius_max = .0f;
    std::vector<CacheEntry>               m_editing_cache;
    std::map<int, CacheEntry>             m_single_brim;
    Slic3r::ExPolygons                    m_first_layer;
    bool                                  m_selection_empty{true};
    const Slic3r::Vec3d                   m_world_normal = {0, 0, 1};
    int                                   m_drag{-1};
    CacheEntry                            m_point_before_drag;
};

} // namespace orca
