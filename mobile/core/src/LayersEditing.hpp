// Variable layer height (GLCanvas3D::LayersEditing): the selected object's layer height profile shown as stripes on the
// object and as a curve in the thickness bar at the right edge of the 3D view; touching the bar adds / removes detail
// (repeating every 100 ms while held, like the desktop's timer), Adaptive / Smooth / Reset from the panel.
#pragma once
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#include <Eigen/Geometry>
#include <nlohmann/json.hpp>

#include "libslic3r/Slicing.hpp"

namespace orca {

struct Camera;

class LayersEditing
{
public:
    static LayersEditing &get();

    nlohmann::json set_enabled(bool on);   // the toolbar button (Plater::priv::on_action_layersediting)
    bool           enabled();
    int            object();               // the edited object (its parts are drawn by render_volumes)
    nlohmann::json state();
    nlohmann::json command(const nlohmann::json &a);   // adaptive, smooth, reset, set (action / band width / radius ...)

    // the thickness bar under the finger (UI thread): press, move, release
    bool           bar_contains(double px, double py);
    bool           press(double px, double py);
    void           move(double px, double py);
    void           release();
    bool           tick();   // GL thread, every frame: the repeating action while held; true while editing

    void           gl_created();
    unsigned       begin_volumes(const Eigen::Matrix4f &proj);   // GL thread: the program for the object's parts, 0 if none
    void           render_overlay();                                                         // GL thread: the bar

private:
    void select_object(int obj);
    void update_slicing_parameters();
    void adjust_layer_height_profile();
    void generate_layer_height_texture();
    void accept_changes();
    float bar_width() const;
    float cursor_z_relative() const;

    std::recursive_mutex                m_mutex;
    bool                                m_enabled{false};
    int                                 m_obj{-1};
    float                               m_object_max_z{0.f};
    std::unique_ptr<Slic3r::SlicingParameters> m_slicing_parameters;
    std::vector<double>       m_layer_height_profile;
    // editing (GLCanvas3D::LayersEditing members)
    float                               m_adaptive_quality{0.5f};
    Slic3r::HeightProfileSmoothingParams m_smooth_params;
    float                               band_width{2.0f};
    float                               strength{0.005f};
    float                               last_z{0.f};
    Slic3r::LayerHeightEditActionType   last_action{Slic3r::LAYER_HEIGHT_EDIT_ACTION_INCREASE};
    int                                 m_touch_action{0};   // what a touch does: 0 add detail, 1 remove detail, 2 reset to base, 3 smoothing
    bool                                m_editing{false};
    double                              m_cursor_y{-1};      // the finger on the bar (pixels), -1 when away
    std::chrono::steady_clock::time_point m_last_action_time;
    // the texture (m_layers_texture)
    struct Texture { std::vector<unsigned char> data; int width{0}, height{0}, cells{0}; bool valid{false}; bool uploaded{false}; } m_layers_texture;
    unsigned                            m_z_texture_id{0};
    unsigned                            m_bar_vao{0}, m_bar_vbo{0};
};

} // namespace orca
