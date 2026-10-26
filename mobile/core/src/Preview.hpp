// The G-code preview of the current plate: libvgcode (Orca's G-code viewer library), fed with the slice result.
#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include <Eigen/Geometry>
#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

namespace libvgcode { class Viewer; }

namespace orca {

class Preview
{
public:
    static Preview &get();
    void gl_created();
    // GL thread: (re)load when the result changed, then draw. true: needs another frame
    bool render(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);
    // GL thread: the pending result / changes, loaded before the frame draws anything
    void update();
    // Realistic View: the toolpaths as shadow casters (their box up to the top shown layer, and what changes them)
    bool shadow_casters(Slic3r::BoundingBoxf3 &bb, size_t &signature);
    // SequentialView::Marker: where the tool is once the moves slider has left the end (m_show_marker, kept until the
    // next load), the result's z offset, and whether the Filament view colours the toolpaths (GL thread)
    bool tool_marker(Slic3r::Vec3f &pos, float &z_offset, bool &color_by_filament);
    // GCodeViewer::set_clipping_plane: the section view; cuts away where dot(plane, (x, y, z, 1)) < 0
    void set_clipping_plane(const std::array<float, 4> &plane);
    void render_shadow_casters(const Eigen::Matrix4f &light_view, const Eigen::Matrix4f &light_proj, const Slic3r::Vec3d &eye);
    void result_changed(int plate) { m_pending = true; m_plate = plate; }
    // GCodeViewer::get_layers_z_range: the shown layer ids (GL thread)
    std::array<size_t, 2> layers_view_range() const;
    void show_plate(int plate) { if (plate != m_plate) { m_plate = plate; m_pending = true; } }

    // the legend / slider state (GL thread; sent to the app as "preview" events)
    nlohmann::json info();
    // queue a change for the GL thread (the caller requests a frame)
    void set(const nlohmann::json &a);

    // the G-code window's lines around the current move (GCodeViewer::SequentialView::GCodeWindow)
    nlohmann::json gcode_lines(int count);
    // the 3D scene benchmark (GL thread): what is loaded, and the top of the shown layers as the layer slider moves it
    bool   has_data() const { return m_has_data; }
    size_t layers_count() const;
    size_t vertices_count() const;
    void   set_top_layer(size_t top);
    // the slider follows (a "preview" event)
    void   notify();

    // GCodeViewer::export_toolpaths_to_obj (the visible toolpaths); false when nothing is loaded
    bool export_toolpaths_to_obj(const std::string &path);

private:
    // what GCodeViewer::load keeps from the result for the legend (render_legend) and the G-code window
    struct LegendData
    {
        Slic3r::PrintEstimatedStatistics stats;
        double total_weight{0}, total_used_filament{0}, total_cost{0};   // PrintStatistics of the plate's print
        std::vector<float> diameters, densities;
        std::array<unsigned, size_t(Slic3r::EMoveType::Count)> counts{};
        std::array<std::array<float, size_t(Slic3r::PrintEstimatedStatistics::ETimeMode::Count)>, size_t(Slic3r::EMoveType::Count)> times{};
        std::array<float, size_t(Slic3r::EMoveType::Count)> distances{};
        std::vector<Slic3r::CustomGCode::Item> custom_gcodes;
        size_t extruders_count{1};
        int nozzles{1};
        std::string filename;
        std::vector<size_t> lines_ends;
    };
    nlohmann::json legend();
    LegendData m_legend;
    void load();
    void apply(const nlohmann::json &a);
    std::unique_ptr<libvgcode::Viewer> m_viewer;
    std::mutex                         m_mutex;
    std::atomic<bool>                  m_pending{false};
    std::atomic<int>                   m_plate{0};
    unsigned                           m_loaded_id{~0u};
    int                                m_loaded_plate{-1};
    bool                               m_has_data{false};
    bool                               m_show_marker{false};
    float                              m_z_offset{0.f};
    nlohmann::json                     m_pending_set;
};

} // namespace orca
