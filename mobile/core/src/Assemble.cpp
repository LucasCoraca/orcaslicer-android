// The Assembly view (Plater::select_view_3D("Assemble"), the AssembleView canvas): the instances at their assemble
// transformations without the bed, the explosion ratio and selection mode (GLCanvas3D::_render_assemble_control) and the
// assembly info (_render_assemble_info), both compiled from upstream on a stand-in canvas in the ImGui overlay.
#define IMGUI_DEFINE_MATH_OPERATORS
#include <string>
#include <string_view>
#include <vector>

#include "Core.hpp"
#include "Host.hpp"
#include "ImGuiOverlay.hpp"
#include "Gizmos.hpp"
#include "Painter.hpp"
#include "Plater.hpp"
#include "Viewport.hpp"
#include "imgui_internal.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;

namespace orca {
namespace asmv {
using namespace std::literals::string_view_literals;

enum class ECanvasType { CanvasView3D, CanvasPreview, CanvasAssembleView };
static float g_explosion_ratio = 1.f;   // GLVolume::explosion_ratio

struct Size
{
    int w, h;
    int get_width() const { return w; }
    int get_height() const { return h; }
};

// Selection: the volume selection mode of the Assembly view, and the box of the selected parts as shown
struct AsmSelection
{
    enum EMode { Volume, Instance };
    BoundingBoxf3 box;
    EMode get_volume_selection_mode() const { return Plater::get().assemble_part_mode ? Volume : Instance; }
    void  set_volume_selection_mode(EMode m)
    {
        Plater::get().assemble_part_mode = m == Volume;
        Host::event("assemble", {{"part_mode", m == Volume}});
    }
    void lock_volume_selection_mode() {}
    void unlock_volume_selection_mode() {}
    bool is_empty() const { return !box.defined; }
    const BoundingBoxf3 &get_bounding_box() const { return box; }
};
struct AsmGizmos
{
    enum class EType { Undefined, MmSegmentation };
    EType get_current_type() const { return Painter::get().kind() == "mmu" ? EType::MmSegmentation : EType::Undefined; }
    // GLGizmosManager::get_icon_texture_id: the "?" icon (toolbar_tooltip.svg)
    ImTextureID get_icon_texture_id(int) const { return (ImTextureID) (intptr_t) gl_icon_texture("toolbar_tooltip"); }
};
struct GLGizmosManager { enum MENU_ICON_NAME { IC_TOOLBAR_TOOLTIP }; };
struct AsmCanvas;
// GLGizmoUtils' "?" button: the shortcuts as its tooltip (shown while the finger is on it)
namespace GLGizmoUtils {
bool toolbar_circular_button(ImTextureID textureID, const char *id, float scale);
void render_tooltip_button(ImGuiWrapper *imgui_wrapper, const AsmCanvas &canvas, const std::vector<std::pair<wxString, wxString>> &shortcuts, float x, float y);
}
std::string shortkey_alt_prefix() { return "Alt+"; }
struct GLVolume { void set_bounding_boxes_as_dirty() { Viewport::get().reload(); } };
struct AsmVolumes { GLVolume one; std::vector<GLVolume *> volumes{&one}; };

struct AsmCanvas
{
    ECanvasType  m_canvas_type{ECanvasType::CanvasAssembleView};
    float        m_explosion_ratio{1.f};
    AsmSelection m_selection;
    AsmGizmos    m_gizmos;
    AsmVolumes   m_volumes;
    Size         size{1, 1};
    float        scale{1.f};

    Size  get_canvas_size() const { return size; }
    float get_scale() const { return scale; }
    const AsmGizmos &get_gizmos_manager() const { return m_gizmos; }
    // GLCanvas3D's constructor: the shortcuts of the Assembly view's "?" button
    std::vector<std::pair<wxString, wxString>> m_shortcuts_assembly_view = {
        {_L("Left mouse button"), _L("Object selection")},
        {wxString(shortkey_alt_prefix()) + _L("Left mouse button"), _L("Part selection")},
        {_L("1~16 number keys"), _L("Number keys can quickly change the color of objects")},
    };
    float _render_assembly_tooltip_button(ImGuiWrapper *imgui_wrapper) const;
    void  _render_assemble_control();
    void  _render_assemble_info() const;
};
#include "slic3r/GUI/AssembleOverlay.inc"
namespace GLGizmoUtils {
#include "slic3r/GUI/AsmTooltipButton.inc"
}

} // namespace asmv

void render_assemble_overlay(const Camera &cam, bool dark)
{
    using namespace asmv;
    static AsmCanvas c;
    ImGuiWrapper::on_change_color_mode(dark);
    c.size  = {cam.width, cam.height};
    c.scale = Gizmos::get().ui_scale();   // GLCanvas3D::get_scale: the display density
    // the selection box of the parts as shown (exploded)
    c.m_selection.box = Viewport::get().selection_box();
    c.m_explosion_ratio = g_explosion_ratio = Plater::get().explosion_ratio;
    c._render_assemble_control();
    c._render_assemble_info();
    if (g_explosion_ratio != Plater::get().explosion_ratio) {
        Plater::get().explosion_ratio = g_explosion_ratio;
        Viewport::get().reload();
    }
}

static json assemble_set(const json &a)
{
    Plater &p = Plater::get();
    if (a.contains("view")) {
        // Plater::priv::select_view_3D: the gizmos close when the view changes (reset_all_states)
        Viewport::get().set_assemble(a.at("view").get<bool>());
    }
    if (a.contains("part_mode")) p.assemble_part_mode = a.at("part_mode").get<bool>();
    return {{"view", bool(p.assemble_view)}, {"part_mode", bool(p.assemble_part_mode)}, {"explosion", float(p.explosion_ratio)}};
}

// Plater::priv::has_assemble_view (upstream): an instance placed in the Assembly view, or an object of several parts
struct PlaterPriv { bool has_assemble_view() const; };
#include "slic3r/GUI/HasAssembleView.inc"
static json assemble_available(const json &) { return PlaterPriv().has_assemble_view(); }

static Commands reg({
    {"assemble.set", assemble_set},
    {"assemble.available", assemble_available},
});

} // namespace orca
