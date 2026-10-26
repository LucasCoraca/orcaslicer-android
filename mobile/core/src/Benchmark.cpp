// Help > Benchmark 3D Scene: SceneBenchmarkDialog / run_scene_benchmark (SceneBenchmark.cpp) on the app's 3D view.
// frame_time_stats is compiled from upstream; the scenes are driven from Viewport::draw on the GL thread (where
// SceneBenchmarkDialog::on_idle renders the canvas), the dialog is the app's ("benchmark" events).
#include "Benchmark.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <thread>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "Preview.hpp"
#include "Slicer.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r_version.h"
#include "slic3r/GUI/BuildCommit.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"

namespace orca {
using namespace Slic3r;
using Slic3r::GUI::MessageDialog;

namespace bench {
#include "slic3r/GUI/SceneBenchmarkTypes.inc"
#include "slic3r/GUI/SceneBenchmark.inc"
} // namespace bench

namespace {
const std::string dots = "...";
// SceneBenchmark.cpp: a scene runs the camera path twice, timing the frames, then averaging the render timings
constexpr size_t WARMUP_FRAMES = 30;
constexpr size_t PASS_FRAMES  = 360;
constexpr size_t SCENE_FRAMES = WARMUP_FRAMES + 2 * PASS_FRAMES;
} // namespace

Benchmark &Benchmark::get()
{
    static Benchmark b;
    return b;
}

// run_scene_benchmark
void Benchmark::start()
{
    if (running()) return;
    MessageDialog confirm(nullptr,
                          _L("The benchmark replaces the current project with the OrcaSliced Combo and slices it. It turns the camera "
                             "around the model in Prepare and in Preview, then moves the layer slider through the sliced layers, timing "
                             "every frame, and shows the results.") + "\n\n" +
                          _L("It uses the current graphics settings and usually takes less than a minute. The main window stays "
                             "locked until it finishes."),
                          _L("Benchmark 3D Scene"), wxICON_INFORMATION | wxOK | wxCANCEL);
    confirm.SetButtonLabel(wxID_OK, _L("Start"), true);
    if (confirm.ShowModal() != wxID_OK) return;
    Core &core = Core::get();
    if (core.call("project.new", json::object()).value("canceled", false)) return;
    Host::event("select_tab", {{"tab", "prepare"}});
    core.call("menu.run", {{"id", "handy:orcasliced_combo"}});
    Plater::get().selection.clear();   // plater->deselect_all()
    Host::event("project", Plater::get().state());
    m_results.clear();
    m_cancel = false;
    m_scene  = false;
    m_polls  = 0;
    m_vsync.clear();
    m_last_poll = std::chrono::steady_clock::now();
    m_stage = Stage::Loading;
    set_status(_u8L("Loading the benchmark model") + dots, 0);
}

bool Benchmark::frame()
{
    const Stage stage = m_stage;
    if (stage == Stage::Done) return false;
    if (m_cancel) {   // the dialog closed: wxEVT_CLOSE_WINDOW
        stop_scene();
        m_stage = Stage::Done;
        Host::event("benchmark", {{"closed", true}});
        return false;
    }
    Viewport &vp = Viewport::get();
    const auto now = std::chrono::steady_clock::now();

    // on_timer: every 100 ms while the model loads and the plate is sliced
    if (stage == Stage::Loading || stage == Stage::Slicing) {
        if (now - m_last_poll < std::chrono::milliseconds(100)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return true;
        }
        m_last_poll = now;
        if (stage == Stage::Loading) {
            BoundingBoxf3 box;
            {
                std::unique_lock<std::recursive_mutex> lock(Core::get().mutex, std::try_to_lock);
                if (!lock) return true;   // the model is still being loaded
                for (const ModelObject *mo : Plater::get().model().objects)
                    for (size_t i = 0; i < mo->instances.size(); ++i) box.merge(mo->instance_bounding_box(i));
            }
            if (!box.defined) {
                if (++m_polls > 50) finish(_u8L("The benchmark model could not be loaded."));
                return true;
            }
            m_target = box.center();
            const Camera cam = vp.camera();
            m_zoom = double(std::min(cam.width, cam.height)) / (1.1 * box.size().norm());
            begin_scene(Stage::Prepare);
        } else {
            if (Slicer::get().running()) return true;
            if (Preview::get().has_data() && vp.preview()) {
                m_polls = 0;
                begin_scene(Stage::Preview);
            } else if (++m_polls > 30)
                finish(_u8L("Slicing failed, so only Prepare was measured."));
        }
        return true;
    }

    // on_idle
    if ((stage == Stage::Prepare) == vp.preview()) {
        finish(_u8L("The benchmark stopped because the 3D view was hidden."));
        return false;
    }
    if (m_frame > WARMUP_FRAMES && m_frame <= WARMUP_FRAMES + PASS_FRAMES)
        m_results.back().frame_ms.push_back(std::chrono::duration<double, std::milli>(now - m_last_frame).count());
    m_last_frame = now;
    if (stage == Stage::Layers)
        // the warm-up ends where the path starts, so the slider moves in every frame
        set_top_layer(m_frame < WARMUP_FRAMES ? PASS_FRAMES - WARMUP_FRAMES + m_frame : (m_frame - WARMUP_FRAMES) % PASS_FRAMES);
    else
        set_camera(m_frame < WARMUP_FRAMES ? 0 : (m_frame - WARMUP_FRAMES) % PASS_FRAMES);
    if (m_frame == 0) on_first_frame();
    if (++m_frame == SCENE_FRAMES) {
        end_scene();
        return m_stage != Stage::Done;
    }
    const int scene = stage == Stage::Prepare ? 0 : stage == Stage::Preview ? 1 : 2;
    if (m_frame % 30 == 0)
        Host::event("benchmark", {{"progress", int(100 * (scene * SCENE_FRAMES + m_frame) / (3 * SCENE_FRAMES))}});
    return true;
}

void Benchmark::on_first_frame()
{
    // a benchmark measures the frames the GPU can draw, not the display refresh rate (GLSurfaceView swaps with interval 1)
    EGLDisplay display = eglGetCurrentDisplay();
    m_restore_swap_interval = display != EGL_NO_DISPLAY && eglSwapInterval(display, 0) == EGL_TRUE;
    if (m_results.size() > 1) return;
    m_vsync = m_restore_swap_interval ? "off" : "on";
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    m_msaa_samples = samples;
    const Camera cam = Viewport::get().camera();
    m_viewport_width  = cam.width;
    m_viewport_height = cam.height;
    m_camera_type     = cam.ortho ? "orthographic" : "perspective";
    if (const GLubyte *s = glGetString(GL_RENDERER)) m_gl_renderer = reinterpret_cast<const char *>(s);
    if (const GLubyte *s = glGetString(GL_VERSION)) m_gl_version = reinterpret_cast<const char *>(s);
}

void Benchmark::begin_scene(Stage stage)
{
    m_stage = stage;
    m_scene = true;
    m_saved_camera = Viewport::get().camera();
    m_frame = 0;
    m_restore_swap_interval = false;
    m_results.push_back({stage == Stage::Prepare ? "Prepare" : stage == Stage::Preview ? "Preview" : "Layers", {}});
    m_results.back().frame_ms.reserve(PASS_FRAMES);
    const int scene = stage == Stage::Prepare ? 0 : stage == Stage::Preview ? 1 : 2;
    set_status((stage == Stage::Prepare ? _u8L("Turning the camera in Prepare") :
                stage == Stage::Preview ? _u8L("Turning the camera in Preview") :
                                          _u8L("Moving through the layers in Preview")) + dots, 100 * scene / 3);
    if (stage == Stage::Layers) {
        m_saved_top_layer = int(Preview::get().layers_view_range()[1]);
        set_camera(0);
    }
}

void Benchmark::end_scene()
{
    const Stage stage = m_stage;
    stop_scene();
    if (stage == Stage::Prepare)
        start_slicing();
    else if (stage == Stage::Preview)
        begin_scene(Stage::Layers);
    else
        finish();
}

void Benchmark::stop_scene()
{
    if (!m_scene) return;
    if (m_frame < SCENE_FRAMES && !m_results.empty()) m_results.pop_back();
    if (m_restore_swap_interval) eglSwapInterval(eglGetCurrentDisplay(), 1);
    m_restore_swap_interval = false;
    if (m_saved_top_layer >= 0) {
        Preview::get().set_top_layer(size_t(m_saved_top_layer));
        Preview::get().notify();
        m_saved_top_layer = -1;
    }
    Camera cam = Viewport::get().camera();
    m_saved_camera.width  = cam.width;
    m_saved_camera.height = cam.height;
    Viewport::get().set_camera(m_saved_camera);
    m_scene = false;
}

// plater->reslice() and the Preview tab: the app slices the plate as its Slice button does
void Benchmark::start_slicing()
{
    m_stage = Stage::Slicing;
    m_polls = 0;
    m_last_poll = std::chrono::steady_clock::now();
    set_status(_u8L("Slicing") + dots, 33);
    Host::event("benchmark", {{"slice", true}});
}

// two turns around the model while the view goes three times from below the plate to nearly straight above it,
// zooming in and out twice (Camera::look_at + set_zoom on the app's camera: azimuth / elevation / distance)
void Benchmark::set_camera(size_t pass_frame)
{
    const double t         = double(pass_frame) / double(PASS_FRAMES);
    const double azimuth   = (-0.75 + 4.0 * t) * PI;
    const double elevation = (30.0 + 55.0 * std::sin(6.0 * PI * t)) * PI / 180.0;
    Viewport &vp = Viewport::get();
    Camera cam = vp.camera();
    cam.target    = m_target;
    // Camera::eye looks from (sin a, -cos a) around Z: upstream's direction (cos az, sin az) is a = az + 90°
    cam.azimuth   = azimuth * 180.0 / PI + 90.0;
    cam.elevation = elevation * 180.0 / PI;
    // zoom = pixels per mm: the view's half height in mm sets the distance
    const double zoom = m_zoom * (1.0 + 0.4 * std::sin(4.0 * PI * t));
    cam.distance  = (double(cam.height) / (2.0 * zoom)) / std::tan(cam.fov * PI / 360.0);
    vp.set_camera(cam);
}

// down to the first layer and back up, as dragging the top of the layer slider does
void Benchmark::set_top_layer(size_t pass_frame)
{
    const double t    = double(pass_frame) / double(PASS_FRAMES);
    const size_t n    = Preview::get().layers_count();
    if (n == 0) return;
    const int    span = int(n) - 1;
    Preview::get().set_top_layer(size_t(std::lround(span * std::abs(1.0 - 2.0 * t))));
}

void Benchmark::set_status(const std::string &status, int progress)
{
    Host::event("benchmark", {{"status", status}, {"progress", progress}, {"title", _u8L("Benchmark 3D Scene")}, {"cancel", _u8L("Cancel")}});
}

void Benchmark::finish(const std::string &error)
{
    stop_scene();
    m_stage = Stage::Done;
    // show_report
    Host::event("benchmark", {{"done", true}, {"error", error}, {"report", m_results.empty() ? std::string() : report()},
                              {"title", _u8L("Benchmark 3D Scene")}, {"copy", _u8L("Copy")}, {"copy_tip", _u8L("Copy the results to the clipboard")},
                              {"ok", _u8L("OK")}});
}

std::string Benchmark::report() const
{
    const AppConfig &config = *Core::get().app_config;
    auto on_off = [](bool on) { return on ? "on" : "off"; };

    std::ostringstream out;
    out << SLIC3R_APP_NAME << " 3D scene benchmark\n"
        << "Version:    " << SoftFever_VERSION << " (" << GUI::build_commit_label << ")\n"
        << "GPU:        " << m_gl_renderer << '\n'
        << "OpenGL:     " << m_gl_version << '\n'
        << "Viewport:   " << m_viewport_width << " x " << m_viewport_height << " px, " << m_camera_type << " camera\n"
        << "Settings:   MSAA " << (m_msaa_samples > 1 ? std::to_string(m_msaa_samples) + "x" : "off")
        << ", FXAA " << on_off(config.get_bool(SETTING_OPENGL_FXAA_ENABLED))
        << ", scene cache " << on_off(config.get_bool(SETTING_OPENGL_SCENE_CACHE)) << ", VSync " << m_vsync << '\n'
        << "Realistic:  ";
    if (config.get_bool(SETTING_OPENGL_REALISTIC_MODE))
        out << "on, Phong " << on_off(config.get_bool(SETTING_OPENGL_REALISTIC_PHONG)) << ", SSAO "
            << on_off(config.get_bool(SETTING_OPENGL_PHONG_SSAO)) << ", shadows " << config.get(SETTING_OPENGL_REALISTIC_SHADOWS)
            << ", in Preview " << on_off(config.get_bool(SETTING_OPENGL_REALISTIC_PREVIEW)) << '\n';
    else
        out << "off\n";
    // the Preview scenes depend on the toolpaths, which depend on the presets it was sliced with
    const PresetBundle &presets = *Core::get().preset_bundle;
    auto preset_name = [](const PresetCollection &collection) {
        return collection.get_selected_preset_name() + (collection.get_edited_preset().is_dirty ? " (modified)" : "");
    };
    out << "Presets:    " << preset_name(presets.printers) << ", " << preset_name(presets.prints) << '\n'
        << "Scene:      OrcaSliced Combo, " << PASS_FRAMES << " frames per pass\n";
    if (Preview::get().has_data())
        out << "Toolpaths:  " << Preview::get().vertices_count() << " vertices, " << Preview::get().layers_count() << " layers\n";
    out << '\n';

    std::vector<bench::FrameTimeStats> stats;
    for (const SceneResult &result : m_results) stats.push_back(bench::frame_time_stats(result.frame_ms));
    auto row = [&out, &stats](const char *label, double bench::FrameTimeStats::*value) {
        out << std::left << std::setw(22) << label << std::right;
        for (const bench::FrameTimeStats &s : stats) out << std::setw(10) << s.*value;
        out << '\n';
    };
    out << std::fixed << std::setprecision(1) << std::setw(22) << "";
    for (const SceneResult &result : m_results) out << std::right << std::setw(10) << result.name;
    out << '\n';
    row("Average FPS", &bench::FrameTimeStats::fps);
    out << std::setprecision(2) << "Frame time (ms)\n";
    row("  average", &bench::FrameTimeStats::average_ms);
    row("  median", &bench::FrameTimeStats::median_ms);
    row("  95th percentile", &bench::FrameTimeStats::p95_ms);
    row("  99th percentile", &bench::FrameTimeStats::p99_ms);
    row("  maximum", &bench::FrameTimeStats::max_ms);
    // FrameProfiler's per-section CPU / GPU timings need GPU timer queries, which GLES does not have here
    for (const SceneResult &result : m_results)
        out << "\nRender timings, " << result.name << " (ms per frame)\n" << "  not supported by the graphics driver\n";
    return out.str();
}

static Commands reg({
    {"benchmark.start", [](const json &) -> json { Benchmark::get().start(); return nullptr; }},
    {"benchmark.cancel", [](const json &) -> json { Benchmark::get().cancel(); return nullptr; }},
});

} // namespace orca
