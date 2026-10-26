#include "Simplify.hpp"

#include <GLES3/gl3.h>
#include <cmath>
#include <set>

#include "Core.hpp"
#include "Plater.hpp"
#include "Viewport.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

using namespace Slic3r;

namespace orca {

namespace {
// thrown from the worker's throw_on_cancel (GLGizmoSimplify's SimplifyCanceledException)
class SimplifyCanceledException : public std::exception
{
public:
    const char *what() const throw() { return L("Model simplification has been canceled"); }
};
} // namespace

Simplify &Simplify::get()
{
    static Simplify s;
    return s;
}

void Simplify::Configuration::fix_count_by_ratio(size_t triangle_count)
{
    if (decimate_ratio <= 0.f)
        wanted_count = static_cast<uint32_t>(triangle_count);
    else if (decimate_ratio >= 100.f)
        wanted_count = 0;
    else
        wanted_count = static_cast<uint32_t>(std::round(triangle_count * (100.f - decimate_ratio) / 100.f));
}

// get_model_volume: exactly one selected volume (a part, or an object with one part and one instance)
static bool selected_volume(int &obj, int &inst, int &vol)
{
    Plater &p = Plater::get();
    const Model &m = p.model();
    std::set<std::tuple<int, int, int>> vols;
    for (const SelItem &s : p.selection) {
        if (s.obj < 0 || s.obj >= int(m.objects.size())) continue;
        const ModelObject *mo = m.objects[s.obj];
        for (int i = 0; i < int(mo->instances.size()); ++i) {
            if (s.inst >= 0 && s.inst != i) continue;
            for (int v = 0; v < int(mo->volumes.size()); ++v)
                if (s.vol < 0 || s.vol == v) vols.insert({s.obj, i, v});
        }
    }
    if (vols.size() != 1) return false;
    std::tie(obj, inst, vol) = *vols.begin();
    return true;
}

void Simplify::open()
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    int obj, inst, vol;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (!selected_volume(obj, inst, vol)) { m_obj = -1; m_volume = nullptr; return; }
    }
    const Model &m = Plater::get().model();
    const ModelVolume *mv = m.objects[obj]->volumes[vol];
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_obj = obj; m_inst = inst; m_vol = vol;
        m_matrix = m.objects[obj]->instances[inst]->get_matrix() * mv->get_matrix();
        if (mv != m_volume) {   // select different model
            m_volume = mv;
            m_configuration.decimate_ratio = 50.;   // default value
            m_configuration.fix_count_by_ratio(mv->mesh().its.indices.size());
            m_preview = mv->mesh_ptr();             // init_model
            m_triangle_count = m_preview->its.indices.size();
        }
        // GLGizmoSimplify::init_model: the selected volume's colour
        ColorRGBA c(0.8f, 0.8f, 0.8f, 1.f);
        if (auto *fc = Core::get().full_config().option<ConfigOptionStrings>("filament_colour")) {
            int e = std::max(1, mv->extruder_id());
            if (e <= int(fc->values.size())) decode_color(fc->values[e - 1], c);
        }
        m_color[0] = c.r(); m_color[1] = c.g(); m_color[2] = c.b(); m_color[3] = c.a();
    }
    process();
    Viewport::get().request_snapshot();
}

void Simplify::close()
{
    stop_worker();
    if (m_worker.joinable()) m_worker.join();
    std::lock_guard<std::mutex> l(m_mutex);
    m_volume = nullptr;   // invalidate selected model
    m_obj = -1;
    m_preview.reset();
}

void Simplify::stop_worker()
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_status == Status::running) m_status = Status::cancelling;
}

bool Simplify::hides(int obj, int inst, int vol)
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_preview && obj == m_obj && inst == m_inst && vol == m_vol;
}

void Simplify::set(const nlohmann::json &a)
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (!m_volume) return;
        const size_t orig = static_cast<const ModelVolume *>(m_volume)->mesh().its.indices.size();
        if (a.contains("use_count")) m_configuration.use_count = a["use_count"];
        if (a.contains("reduction")) {
            m_reduction = std::clamp(a["reduction"].get<int>(), 0, 4);
            static const float errors[] = {1e-3f, 1e-2f, 0.1f, 0.5f, 1.f};
            m_configuration.max_error = errors[m_reduction];
        }
        if (a.contains("decimate_ratio")) {
            m_configuration.decimate_ratio = a["decimate_ratio"];
            if (m_configuration.decimate_ratio < 0.f) m_configuration.decimate_ratio = 0.01f;
            if (m_configuration.decimate_ratio > 100.f) m_configuration.decimate_ratio = 100.f;
            m_configuration.fix_count_by_ratio(orig);
        }
        if (a.contains("wireframe")) m_show_wireframe = a["wireframe"];
    }
    process();
    Viewport::get().request_redraw();
}

nlohmann::json Simplify::state()
{
    worker_finished();
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_volume)
        return {{"error", _u8L("Simplification is currently only allowed when a single part is selected")}};
    const ModelVolume *mv = static_cast<const ModelVolume *>(m_volume);
    const size_t orig = mv->mesh().its.indices.size();
    // show preview result triangle count (percent)
    if (!m_configuration.use_count) {
        m_configuration.wanted_count = static_cast<uint32_t>(m_triangle_count);
        m_configuration.decimate_ratio = (1.0f - (m_configuration.wanted_count / (float) orig)) * 100.f;
    }
    return {{"name", mv->name}, {"triangles", orig}, {"use_count", m_configuration.use_count}, {"reduction", m_reduction},
            {"decimate_ratio", m_configuration.decimate_ratio}, {"wanted_count", m_configuration.wanted_count},
            {"wireframe", m_show_wireframe}, {"running", m_status == Status::running}, {"cancelling", m_status == Status::cancelling},
            {"progress", m_progress}, {"ready", bool(m_result)},
            {"levels", {_u8L("Extra high"), _u8L("High"), _u8L("Medium"), _u8L("Low"), _u8L("Extra low")}},
            {"count_text", GUI::format(_L_PLURAL("%1% triangle", "%1% triangles", m_configuration.wanted_count), m_configuration.wanted_count)}};
}

void Simplify::process()
{
    std::unique_lock<std::mutex> l(m_mutex);
    const ModelVolume *mv = static_cast<const ModelVolume *>(m_volume);
    if (mv == nullptr || mv->mesh().its.indices.empty()) return;
    const bool configs_match = mv == m_state_mv && m_state_config == m_configuration;
    const bool result_valid = bool(m_result);
    const bool is_worker_running = m_status == Status::running;
    if ((result_valid || is_worker_running) && configs_match) return;   // finished or waiting for the result already
    if (is_worker_running && !configs_match) {   // outdated: stop it, it restarts itself when cancelled
        m_status = Status::cancelling;
        return;
    }
    if (m_worker.joinable()) {
        l.unlock();
        m_worker.join();
        l.lock();
    }
    m_state_config = m_configuration;
    m_state_mv = mv;
    m_status = Status::running;
    auto its = std::make_unique<indexed_triangle_set>(mv->mesh().its);
    m_worker = std::thread([this](std::unique_ptr<indexed_triangle_set> its) {
        std::function<void(void)> throw_on_cancel = [this]() {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_status == Status::cancelling) throw SimplifyCanceledException();
        };
        std::function<void(int)> statusfn = [this](int percent) {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_progress = percent;
        };
        uint32_t triangle_count = 0;
        float max_error = std::numeric_limits<float>::max();
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_state_config.use_count) triangle_count = m_state_config.wanted_count;
            if (!m_state_config.use_count) max_error = m_state_config.max_error;
            m_progress = 0;
            m_result.reset();
            m_status = Status::running;
        }
        try {
            its_quadric_edge_collapse(*its, triangle_count, &max_error, throw_on_cancel, statusfn);
        } catch (SimplifyCanceledException &) {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_status = Status::idle;
        }
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_status == Status::running) {   // not cancelled: the result is valid
                m_status = Status::idle;
                m_result = std::move(its);
            }
        }
        std::lock_guard<std::mutex> lk(m_mutex);
        m_finished = true;   // worker_finished runs on the engine thread at the next state poll
        Viewport::get().request_redraw();
    }, std::move(its));
}

void Simplify::worker_finished()
{
    bool restart;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (!m_finished) return;
        m_finished = false;
        if (m_status == Status::running) return;   // someone started the worker again
    }
    if (m_worker.joinable()) m_worker.join();
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (!m_volume) return;   // the gizmo is off
        if (m_result) {
            m_preview = std::make_shared<const TriangleMesh>(*m_result);   // init_model
            m_triangle_count = m_result->indices.size();
        }
        restart = m_state_config != m_configuration || m_state_mv != m_volume;
    }
    if (restart) process();   // settings were changed: restart the worker immediately
    Viewport::get().request_redraw();
}

nlohmann::json Simplify::apply()
{
    Plater &p = Plater::get();
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::unique_lock<std::mutex> l(m_mutex);
    if (!m_volume || !m_result || m_obj < 0 || m_obj >= int(p.model().objects.size())) return nullptr;
    ModelObject *mo = p.model().objects[m_obj];
    if (m_vol >= int(mo->volumes.size()) || mo->volumes[m_vol] != m_volume) return nullptr;
    ModelVolume *mv = mo->volumes[m_vol];
    l.unlock();
    p.take_snapshot(GUI::format("Simplify %1%", mv->name));
    const bool keep_painting = Core::get().app_config->get_bool("keep_painting");
    bool painting_removed = false;
    if (!keep_painting)   // Plater::clear_before_change_mesh
        for (ModelVolume *v : mo->volumes) {
            painting_removed |= !v->supported_facets.empty() || !v->seam_facets.empty() || !v->mmu_segmentation_facets.empty() || !v->fuzzy_skin_facets.empty();
            v->supported_facets.reset();
            v->seam_facets.reset();
            v->mmu_segmentation_facets.reset();
            v->fuzzy_skin_facets.reset();
        }
    l.lock();
    std::optional<TriangleSelector::SavedPainting> saved = keep_painting ? mv->save_painting() : std::optional<TriangleSelector::SavedPainting>{};
    mv->set_mesh(std::move(*m_result));
    mv->restore_painting(saved);
    m_result.reset();
    mv->calculate_convex_hull();
    mv->invalidate_convex_hull_2d();
    mv->set_new_unique_id();
    mo->invalidate_bounding_box();
    mo->ensure_on_bed();
    l.unlock();
    p.changed();
    json st = p.state();
    if (painting_removed) st["notice"] = _u8L("Custom supports and color painting were removed before repairing.");
    st["close_gizmo"] = true;
    return st;
}

// GLGizmoSimplify::on_render: the preview with gouraud_light, then the wireframe in white
void Simplify::render(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    std::shared_ptr<const TriangleMesh> mesh;
    Eigen::Matrix4f model;
    float color[4];
    bool wire;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (!m_preview || m_obj < 0) return;
        mesh = m_preview;
        model = m_matrix.matrix().cast<float>();
        std::copy(std::begin(m_color), std::end(m_color), color);
        wire = m_show_wireframe;
    }
    glEnable(GL_DEPTH_TEST);
    gl_draw_lit(mesh, model, color, view, proj);
    if (wire) {
        std::vector<float> xyz;
        xyz.reserve(mesh->its.indices.size() * 18);
        for (const Vec3i32 &f : mesh->its.indices)
            for (int k = 0; k < 3; ++k) {
                const Vec3f a = (m_matrix * mesh->its.vertices[f[k]].cast<double>()).cast<float>();
                const Vec3f b = (m_matrix * mesh->its.vertices[f[(k + 1) % 3]].cast<double>()).cast<float>();
                xyz.insert(xyz.end(), {a.x(), a.y(), a.z(), b.x(), b.y(), b.z()});
            }
        glDepthFunc(GL_LEQUAL);
        gl_draw_flat(xyz, GL_LINES, proj * view, ColorRGBA::WHITE(), 1.f);
        glDepthFunc(GL_LESS);
    }
}

static Commands reg({
    {"simplify.state", [](const json &) { return Simplify::get().state(); }},
    {"simplify.set", [](const json &a) { Simplify::get().set(a); return Simplify::get().state(); }},
    {"simplify.apply", [](const json &) { return Simplify::get().apply(); }},
});

} // namespace orca
