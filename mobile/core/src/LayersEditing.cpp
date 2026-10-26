#include "LayersEditing.hpp"

#include <GLES3/gl3.h>

#include "Core.hpp"
#include "Gizmos.hpp"
#include "Plater.hpp"
#include "Shaders.hpp"
#include "Slicer.hpp"
#include "Viewport.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;

namespace orca {

static const float THICKNESS_BAR_WIDTH = 70.0f;

LayersEditing &LayersEditing::get()
{
    static LayersEditing l;
    return l;
}

bool LayersEditing::enabled()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    return m_enabled;
}

int LayersEditing::object()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    return m_enabled ? m_obj : -1;
}

// the bar is THICKNESS_BAR_WIDTH dp wide at the right edge (thickness_bar_width: x the scale factor)
float LayersEditing::bar_width() const { return THICKNESS_BAR_WIDTH * Gizmos::get().ui_scale(); }

// layers_height_allowed: one FFF object above the bed
static int selected_object()
{
    Plater &p = Plater::get();
    std::set<int> objs;
    for (const SelItem &s : p.selection) objs.insert(s.obj);
    if (objs.size() != 1) return -1;
    const int o = *objs.begin();
    if (o < 0 || o >= int(p.model().objects.size()) || p.model().objects[o]->max_z() <= SINKING_Z_THRESHOLD) return -1;
    return o;
}

// LayersEditing::select_object
void LayersEditing::select_object(int object_id)
{
    const ModelObject *mo = object_id >= 0 ? Plater::get().model().objects[object_id] : nullptr;
    const float new_max_z = mo == nullptr ? 0.0f : float(mo->max_z());
    if (m_obj != object_id || m_object_max_z != new_max_z) {
        m_layer_height_profile.clear();
        m_slicing_parameters.reset();
        m_layers_texture.valid = false;
        m_obj = object_id;
        m_object_max_z = new_max_z;
    }
}

nlohmann::json LayersEditing::set_enabled(bool on)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    const int o = selected_object();
    m_enabled = on && o >= 0;
    // set_config: the profile is rebuilt from the current presets
    m_slicing_parameters.reset();
    m_layer_height_profile.clear();
    m_layers_texture.valid = false;
    m_obj = -1;
    if (m_enabled) select_object(o);
    Viewport::get().request_snapshot();
    return state();
}

void LayersEditing::update_slicing_parameters()
{
    if (!m_slicing_parameters) {
        Vec3d shrinkage(1, 1, 1);   // Print::shrinkage_compensation of the current plate's print, when there is one
        if (PlateSlice *ps = Slicer::get().slice(Plater::get().current_plate); ps && ps->print) shrinkage = ps->print->shrinkage_compensation();
        m_slicing_parameters = std::make_unique<SlicingParameters>(
            PrintObject::slicing_parameters(Core::get().full_config(), *Plater::get().model().objects[m_obj], m_object_max_z, shrinkage));
    }
}

void LayersEditing::adjust_layer_height_profile()
{
    update_slicing_parameters();
    const ModelObject &mo = *Plater::get().model().objects[m_obj];
    PrintObject::update_layer_height_profile(mo, *m_slicing_parameters, m_layer_height_profile);
    Slic3r::adjust_layer_height_profile(mo, *m_slicing_parameters, m_layer_height_profile, last_z, strength, band_width, last_action);
    m_layers_texture.valid = false;
}

void LayersEditing::generate_layer_height_texture()
{
    update_slicing_parameters();
    bool update = !m_layers_texture.valid;
    if (PrintObject::update_layer_height_profile(*Plater::get().model().objects[m_obj], *m_slicing_parameters, m_layer_height_profile)) update = true;
    if (!update && !m_layers_texture.data.empty() && m_layers_texture.cells > 0) return;
    if (m_layers_texture.data.empty()) {
        m_layers_texture.width = 1024;
        m_layers_texture.height = 1024;
        m_layers_texture.data.assign(m_layers_texture.width * m_layers_texture.height * 5, 0);
    }
    m_layers_texture.cells = Slic3r::generate_layer_height_texture(*m_slicing_parameters,
                                                                    Slic3r::generate_object_layers(*m_slicing_parameters, m_layer_height_profile, false),
                                                                    m_layers_texture.data.data(), m_layers_texture.height, m_layers_texture.width, true);
    m_layers_texture.valid = true;
    m_layers_texture.uploaded = false;
}

// accept_changes: the edited profile goes to the object, with a snapshot
void LayersEditing::accept_changes()
{
    if (m_obj < 0) return;
    Plater &p = Plater::get();
    p.take_snapshot("Variable layer height - Manual edit");
    p.model().objects[m_obj]->layer_height_profile.set(m_layer_height_profile);
    p.changed();
}

nlohmann::json LayersEditing::state()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (m_enabled) select_object(selected_object());
    if (!m_enabled || m_obj < 0) return {{"enabled", m_enabled}};
    update_slicing_parameters();
    PrintObject::update_layer_height_profile(*Plater::get().model().objects[m_obj], *m_slicing_parameters, m_layer_height_profile);
    const bool fixed = m_layer_height_profile.size() >= 4 && check_object_layers_fixed(*m_slicing_parameters, m_layer_height_profile);
    return {{"enabled", true}, {"object", m_obj}, {"adaptive_quality", m_adaptive_quality}, {"radius", m_smooth_params.radius},
            {"keep_min", m_smooth_params.keep_min}, {"band_width", band_width}, {"touch_action", m_touch_action}, {"reset_enabled", !fixed},
            {"labels", {{"title", _u8L("Variable layer height")}, {"adaptive", _u8L("Adaptive")}, {"quality", _u8L("Quality / Speed")},
                        {"smooth", _u8L("Smooth")}, {"radius", _u8L("Radius")}, {"keep_min", _u8L("Keep min")}, {"reset", _u8L("Reset")},
                        {"done", _u8L("Done")}, {"edit_area", _u8L("Increase/decrease edit area")},
                        {"actions", {_u8L("Add detail"), _u8L("Remove detail"), _u8L("Reset to base"), _u8L("Smoothing")}}}}};
}

nlohmann::json LayersEditing::command(const nlohmann::json &a)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_enabled || m_obj < 0) return state();
    Plater &p = Plater::get();
    ModelObject *mo = p.model().objects[m_obj];
    const std::string what = a.value("what", std::string());
    if (what == "adaptive") {   // GLCanvas3D::adaptive_layer_height_profile
        if (a.contains("quality")) m_adaptive_quality = std::clamp(a["quality"].get<float>(), 0.f, 1.f);
        p.take_snapshot("Variable layer height - Adaptive");
        update_slicing_parameters();
        m_layer_height_profile = layer_height_profile_adaptive(*m_slicing_parameters, *mo, m_adaptive_quality);
        mo->layer_height_profile.set(m_layer_height_profile);
        m_layers_texture.valid = false;
        p.changed();
    } else if (what == "smooth") {   // smooth_layer_height_profile
        p.take_snapshot("Variable layer height - Smooth all");
        update_slicing_parameters();
        PrintObject::update_layer_height_profile(*mo, *m_slicing_parameters, m_layer_height_profile);
        m_layer_height_profile = smooth_height_profile(m_layer_height_profile, *m_slicing_parameters, m_smooth_params);
        mo->layer_height_profile.set(m_layer_height_profile);
        m_layers_texture.valid = false;
        p.changed();
    } else if (what == "reset") {   // reset_layer_height_profile
        p.take_snapshot("Variable layer height - Reset");
        mo->layer_height_profile.clear();
        m_layer_height_profile.clear();
        m_layers_texture.valid = false;
        p.changed();
    } else if (what == "set") {
        if (a.contains("quality")) m_adaptive_quality = std::clamp(a["quality"].get<float>(), 0.f, 1.f);
        if (a.contains("radius")) m_smooth_params.radius = unsigned(std::clamp(a["radius"].get<int>(), 1, 10));
        if (a.contains("keep_min")) m_smooth_params.keep_min = a["keep_min"];
        if (a.contains("band_width")) band_width = std::clamp(a["band_width"].get<float>(), 1.5f, 10.f);   // the mouse wheel's range
        if (a.contains("touch_action")) m_touch_action = std::clamp(a["touch_action"].get<int>(), 0, 3);
    }
    Viewport::get().request_redraw();
    return state();
}

// the cursor's height on the bar, 0..1 from the bottom (get_cursor_z_relative), -1000 off the bar
float LayersEditing::cursor_z_relative() const
{
    if (m_cursor_y < 0) return -1000.f;
    const Camera cam = Viewport::get().camera();
    const float b = float(cam.height), t = 0.f;
    return float((b - m_cursor_y - 1.0) / (b - t - 1.0));
}

bool LayersEditing::bar_contains(double px, double py)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_enabled || m_obj < 0) return false;
    const Camera cam = Viewport::get().camera();
    return px >= cam.width - bar_width() && px <= cam.width && py >= 0 && py <= cam.height;
}

// _perform_layer_editing_action: the touch's height and action, adjust now and again every 100 ms while held
bool LayersEditing::press(double px, double py)
{
    if (!bar_contains(px, py)) return false;
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    m_editing = true;
    move(px, py);
    return true;
}

void LayersEditing::move(double px, double py)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_editing) return;
    const Camera cam = Viewport::get().camera();
    m_cursor_y = std::clamp(py, 0.0, double(cam.height));
    last_z = m_object_max_z * float((cam.height - m_cursor_y - 1.0) / double(cam.height));
    // the mouse buttons and Shift: left = decrease (add detail), right = increase, Shift+left = reduce (reset), Shift+right = smooth
    static const LayerHeightEditActionType actions[] = {LAYER_HEIGHT_EDIT_ACTION_DECREASE, LAYER_HEIGHT_EDIT_ACTION_INCREASE,
                                                        LAYER_HEIGHT_EDIT_ACTION_REDUCE, LAYER_HEIGHT_EDIT_ACTION_SMOOTH};
    last_action = actions[m_touch_action];
    adjust_layer_height_profile();
    m_last_action_time = std::chrono::steady_clock::now();
    Viewport::get().request_redraw();
}

void LayersEditing::release()
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_editing) return;
    m_editing = false;
    m_cursor_y = -1;
    accept_changes();
    Viewport::get().request_redraw();
}

bool LayersEditing::tick()
{
    std::unique_lock<std::recursive_mutex> core(Core::get().mutex, std::try_to_lock);
    if (!core.owns_lock()) return m_editing;
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_editing) return false;
    if (std::chrono::steady_clock::now() - m_last_action_time >= std::chrono::milliseconds(100)) {   // the 100 ms timer
        adjust_layer_height_profile();
        m_last_action_time = std::chrono::steady_clock::now();
    }
    return true;
}

void LayersEditing::gl_created()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    m_z_texture_id = 0;
    m_bar_vao = m_bar_vbo = 0;
    m_layers_texture.uploaded = false;
}

// render_volumes: the "variable_layer_height" shader with the Z texture for the object's parts; Viewport then draws each
// part setting its matrices (volume_world_matrix, view_model_matrix, view_normal_matrix, object_max_z = 0)
unsigned LayersEditing::begin_volumes(const Eigen::Matrix4f &proj)
{
    std::unique_lock<std::recursive_mutex> core(Core::get().mutex, std::try_to_lock);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_enabled) return 0;
    if (core.owns_lock()) select_object(selected_object());   // GLCanvas3D::_render_objects: the selected object, every frame
    if (m_obj < 0 || m_obj >= int(Plater::get().model().objects.size())) return 0;
    if (core.owns_lock()) generate_layer_height_texture();
    if (m_layers_texture.data.empty() || m_layers_texture.cells <= 0) return 0;
    GLuint prog = gl::orca_program("variable_layer_height");
    if (!prog) return 0;
    if (!m_z_texture_id) {   // LayersEditing::init
        glGenTextures(1, &m_z_texture_id);
        glBindTexture(GL_TEXTURE_2D, m_z_texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
        m_layers_texture.uploaded = false;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_z_texture_id);
    if (!m_layers_texture.uploaded) {   // both levels of detail
        const int w = m_layers_texture.width, h = m_layers_texture.height;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_layers_texture.data.data());
        glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, w / 2, h / 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_layers_texture.data.data() + w * h * 4);
        m_layers_texture.uploaded = true;
    }
    glUseProgram(prog);
    auto U = [prog](const char *n) { return glGetUniformLocation(prog, n); };
    glUniform1f(U("z_to_texture_row"), float(m_layers_texture.cells - 1) / (float(m_layers_texture.width) * m_object_max_z));
    glUniform1f(U("z_texture_row_to_normalized"), 1.0f / float(m_layers_texture.height));
    glUniform1f(U("z_cursor"), m_object_max_z * cursor_z_relative());
    glUniform1f(U("z_cursor_band_width"), band_width);
    glUniform1f(U("object_max_z"), 0.f);
    glUniform1i(U("z_texture"), 0);
    glUniformMatrix4fv(U("projection_matrix"), 1, GL_FALSE, proj.data());
    return prog;
}

// render_active_object_annotations + render_profile: the colour bar and the profile curve
void LayersEditing::render_overlay()
{
    std::unique_lock<std::recursive_mutex> core(Core::get().mutex, std::try_to_lock);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_enabled || m_obj < 0 || m_layers_texture.cells <= 0 || !m_slicing_parameters || !m_z_texture_id) return;
    const Camera cam = Viewport::get().camera();
    const float cnv_width = float(cam.width), cnv_height = float(cam.height);
    if (cnv_width == 0.f || cnv_height == 0.f) return;
    GLuint prog = gl::orca_program("variable_layer_height");
    if (!prog) return;
    glUseProgram(prog);
    auto U = [prog](const char *n) { return glGetUniformLocation(prog, n); };
    glUniform1f(U("z_to_texture_row"), float(m_layers_texture.cells - 1) / (float(m_layers_texture.width) * m_object_max_z));
    glUniform1f(U("z_texture_row_to_normalized"), 1.0f / float(m_layers_texture.height));
    glUniform1f(U("z_cursor"), m_object_max_z * cursor_z_relative());
    glUniform1f(U("z_cursor_band_width"), band_width);
    glUniform1f(U("object_max_z"), m_object_max_z);
    const Eigen::Matrix4f I = Eigen::Matrix4f::Identity();
    const Eigen::Matrix3f I3 = Eigen::Matrix3f::Identity();
    glUniformMatrix4fv(U("view_model_matrix"), 1, GL_FALSE, I.data());
    glUniformMatrix4fv(U("projection_matrix"), 1, GL_FALSE, I.data());
    glUniformMatrix3fv(U("view_normal_matrix"), 1, GL_FALSE, I3.data());
    glUniform1i(U("z_texture"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_z_texture_id);
    // the colour bar: a quad at the right edge, texture v = height
    const float lft = 1.0f - 2.0f * bar_width() / cnv_width, r = 1.0f, t = 1.0f, b = -1.0f;
    const float quad[] = {lft, b, 0, 0, 0, 1, 0, 0,  r, b, 0, 0, 0, 1, 1, 0,  r, t, 0, 0, 0, 1, 1, 1,
                          r, t, 0, 0, 0, 1, 1, 1,  lft, t, 0, 0, 0, 1, 0, 1,  lft, b, 0, 0, 0, 1, 0, 0};
    if (!m_bar_vao) { glGenVertexArrays(1, &m_bar_vao); glGenBuffers(1, &m_bar_vbo); }
    glBindVertexArray(m_bar_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_bar_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *) 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *) 12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *) 24);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(2);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    // render_profile: the base layer height line (black) and the profile (blue)
    const float scale_x = bar_width() / float(1.12 * m_slicing_parameters->max_layer_height);
    const float scale_y = cnv_height / m_object_max_z;
    const float axis_x = 2.0f * ((cnv_width - bar_width() + float(m_slicing_parameters->layer_height) * scale_x) / cnv_width - 0.5f);
    gl_draw_flat({axis_x, -1.f, 0.f, axis_x, 1.f, 0.f}, GL_LINES, I, ColorRGBA::BLACK(), 1.f);
    std::vector<float> strip;
    for (size_t i = 0; i + 1 < m_layer_height_profile.size(); i += 2)
        strip.insert(strip.end(), {2.0f * ((cnv_width - bar_width() + float(m_layer_height_profile[i + 1]) * scale_x) / cnv_width - 0.5f),
                                   2.0f * (float(m_layer_height_profile[i]) * scale_y / cnv_height - 0.5f), 0.f});
    if (strip.size() >= 6) gl_draw_flat(strip, GL_LINE_STRIP, I, ColorRGBA::BLUE(), 1.f);
    glEnable(GL_DEPTH_TEST);
}

static Commands reg({
    {"vlh.enable", [](const json &a) { return LayersEditing::get().set_enabled(a.value("on", true)); }},
    {"vlh.state", [](const json &) { return LayersEditing::get().state(); }},
    {"vlh.command", [](const json &a) { return LayersEditing::get().command(a); }},
});

} // namespace orca
