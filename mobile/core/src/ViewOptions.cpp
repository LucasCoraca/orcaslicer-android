// The view toggles of the canvas menu (GLCanvas3D::_render_canvas_toolbar) and the View menu: overhang highlight,
// labels, wireframe, X-Ray (Plater::priv state) and the app config ones (axes, gridlines, focus mode, navigator, ...).
#include "Core.hpp"
#include "Viewport.hpp"
#include "Realistic.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"

using namespace Slic3r;

namespace orca {

Vec3f tilt_up_direction();

namespace {
// GLVolumeCollection::get_selection_support_normal_z (upstream 3DScene.cpp)
struct GLVolumeCollection
{
    float get_selection_support_normal_z() const;
};
#include "slic3r/GUI/SupportNormalZ.inc"

// the app config keys of the toggles, with upstream's defaults (AppConfig::set_defaults)
const std::vector<std::string> &config_keys()
{
    static const std::vector<std::string> keys = {"show_axes", "show_plate_gridlines", "focus_mode", "show_3d_navigator",
                                                  "show_canvas_zoom_button", "use_perspective_camera", "auto_perspective",
                                                  "show_gcode_window", "show_outline", "show_center_of_mass",
                                                  "opengl_realistic_mode", "show_imex_toolhead_boxes"};
    return keys;
}
} // namespace

void Viewport::update_view_presets()
{
    m_overhang_normal_z = GLVolumeCollection().get_selection_support_normal_z();
    m_up_direction      = tilt_up_direction();
    AppConfig &cfg      = *Core::get().app_config;
    m_show_axes         = cfg.get_bool("show_axes");
    m_show_gridlines    = cfg.get_bool("show_plate_gridlines");
    m_focus_mode        = cfg.get_bool("focus_mode");
    m_auto_perspective  = cfg.get_bool("auto_perspective");
    m_show_navigator    = cfg.get_bool("show_3d_navigator");
    m_show_outline      = cfg.get_bool("show_outline");
    Realistic::get().update();
    m_show_toolhead_boxes = cfg.get("show_imex_toolhead_boxes") != "false";
    m_dirty             = true;
}

nlohmann::json Viewport::view_options()
{
    nlohmann::json j = {{"overhang", bool(m_show_overhang)}, {"labels", bool(m_show_labels)},
                        {"wireframe", bool(m_show_wireframe)}, {"xray", bool(m_show_xray)}};
    for (const std::string &k : config_keys()) j[k] = Core::get().app_config->get_bool(k);
    // MainFrame's "Show IDEX/IQEX Toolhead": on unless set off, offered on an IDEX/IQEX printer
    j["show_imex_toolhead_boxes"] = Core::get().app_config->get("show_imex_toolhead_boxes") != "false";
    auto *imex = Core::get().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionBool>("is_imex");
    j["is_imex"] = imex && imex->value;
    return j;
}

void Viewport::set_view_option(const std::string &key, bool on)
{
    if (key == "overhang") m_show_overhang = on;
    else if (key == "labels") m_show_labels = on;
    else if (key == "wireframe") m_show_wireframe = on;
    else if (key == "xray") m_show_xray = on;
    else if (std::find(config_keys().begin(), config_keys().end(), key) != config_keys().end()) {
        Core::get().app_config->set_bool(key, on);
        if (key == "use_perspective_camera") set_view(on ? "perspective" : "ortho");
        if (key == "auto_perspective") m_auto_perspective = on;
    } else
        throw CommandError("unknown view option " + key);
    Core::get().app_config->save();   // the app config keys persist like the desktop's (AppConfig::save on change)
    update_view_presets();
}

static Commands reg({
    {"view.options", [](const json &) -> json { return Viewport::get().view_options(); }},
    {"view.set", [](const json &a) -> json {
         Viewport::get().set_view_option(str_arg(a, "key"), a.at("on").get<bool>());
         return Viewport::get().view_options();
     }},
});

} // namespace orca
