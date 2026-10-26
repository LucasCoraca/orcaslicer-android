// Help > Benchmark 3D Scene (SceneBenchmark.cpp): the OrcaSliced Combo, the camera turned around it in Prepare and in
// Preview, then the layer slider moved through the layers, every frame timed.
#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "Viewport.hpp"
#include "libslic3r/Point.hpp"

namespace orca {

class Benchmark
{
public:
    static Benchmark &get();
    // GL thread, before a frame of the 3D view is drawn: the camera / top layer of this frame. true: keep drawing
    bool frame();
    // engine thread: run_scene_benchmark (asks, loads the model) / the dialog's Cancel
    void start();
    void cancel() { m_cancel = true; }
    bool running() const { return m_stage != Stage::Done; }

private:
    enum class Stage { Loading, Prepare, Slicing, Preview, Layers, Done };
    struct SceneResult
    {
        std::string         name;
        std::vector<double> frame_ms;
    };

    void begin_scene(Stage stage);
    void end_scene();
    void stop_scene();
    void start_slicing();
    void set_camera(size_t pass_frame);
    void set_top_layer(size_t pass_frame);
    void set_status(const std::string &status, int progress);
    void finish(const std::string &error = std::string());
    void on_first_frame();
    std::string report() const;

    std::atomic<Stage>                    m_stage{Stage::Done};
    std::atomic<bool>                     m_cancel{false};
    int                                   m_polls{0};
    std::chrono::steady_clock::time_point m_last_poll;
    bool                                  m_scene{false};      // a scene is being timed (SceneBenchmarkDialog::m_canvas)
    Slic3r::Vec3d                         m_target{Slic3r::Vec3d::Zero()};
    double                                m_zoom{1.0};
    size_t                                m_frame{0};
    std::chrono::steady_clock::time_point m_last_frame;
    int                                   m_saved_top_layer{-1};
    bool                                  m_restore_swap_interval{false};
    std::vector<SceneResult>              m_results;
    Camera                                m_saved_camera;

    std::string m_vsync;
    int         m_msaa_samples{0};
    int         m_viewport_width{0};
    int         m_viewport_height{0};
    std::string m_camera_type;
    std::string m_gl_renderer, m_gl_version;
};

} // namespace orca
