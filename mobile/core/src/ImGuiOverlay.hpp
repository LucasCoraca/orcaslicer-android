// ImGui over the 3D view (GLCanvas3D's overlays: the 3D navigator, the center of mass details): one context, Orca's
// font and base style (ImGuiWrapper::init_style), GLES rendering of the draw lists, touch input fed one change per
// frame (ImGui 1.83 reads the mouse state per frame).
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include "wx/string.h"
#include "imgui.h"
#include "libslic3r/Color.hpp"

namespace orca {

// The ImGuiWrapper calls of the upstream overlay code compiled into the core
struct ImGuiWrapper
{
    static const ImVec4 COL_GREY_DARK, COL_GREY_LIGHT, COL_ORANGE_DARK, COL_ORANGE_LIGHT, COL_WINDOW_BACKGROUND, COL_BUTTON_BACKGROUND,
        COL_BLUE_LIGHT, COL_HOVER, COL_ACTIVE, COL_ORCA, COL_ORCA_DARK, COL_ORCA_HOVER, COL_ORCA_HOVER_DARK, COL_GREEN_LIGHT, COL_SEPARATOR, COL_SEPARATOR_DARK, COL_TITLE_BG, COL_WINDOW_BG,
        COL_WINDOW_BG_DARK;
    struct LastSliderStatus
    {
        bool hovered{false}, edited{false}, clicked{false}, deactivated_after_edit{false}, can_take_snapshot{false};
    };
    LastSliderStatus m_last_slider_status;
    float            m_style_scaling{1.f};
    float            get_style_scaling() const { return m_style_scaling; }
    static void      on_change_color_mode(bool is_dark);
    static void      push_toolbar_style(const float scale);
    static void      pop_toolbar_style();
    static void      push_common_window_style(const float scale);
    static void      pop_common_window_style();
    static void      push_combo_style(const float scale);
    static void      pop_combo_style();
    bool             bbl_slider_float_style(const std::string &label, float *v, float v_min, float v_max, const char *format = "%.3f",
                                            float power = 1.0f, bool clamp = true, const wxString &tooltip = {});
    bool             bbl_slider_float(const std::string &label, float *v, float v_min, float v_max, const char *format = "%.3f",
                                      float power = 1.0f, bool clamp = true, const wxString &tooltip = {});
    void             tooltip(const char *label, float wrap_width);
    static ImVec2    calc_text_size(std::string_view text, bool hide_text_after_double_hash = false, float wrap_width = -1.0f);
    ImVec2           get_item_spacing() const;
    static ImVec4 to_ImVec4(const Slic3r::ColorRGBA &c) { return {c.r(), c.g(), c.b(), c.a()}; }
    bool  requires_extra_frame{false};

    void init_style();
    void set_next_window_pos(float x, float y, int flag, float pivot_x = 0.f, float pivot_y = 0.f);
    bool begin(const std::string &name, int flags = 0);
    bool begin(const std::string &name, bool *close, int flags = 0);
    void end();
    void set_requires_extra_frame() { requires_extra_frame = true; }
    static void text(const char *label);
    static void text(const std::string &label);
    static void text_colored(const ImVec4 &color, const char *label);
    static void text_colored(const ImVec4 &color, const std::string &label);
};
ImGuiWrapper &imgui_wrapper();

class ImGuiOverlay
{
public:
    static ImGuiOverlay &get();
    // GL thread: one ImGui frame of the given content over the current framebuffer; true when it needs more frames
    bool frame(int width, int height, float scale, const std::function<void()> &content);
    // UI thread: a touch (0 down, 1 move, 2 up); a press is taken when it is on an ImGui window of the last frame or
    // when `claim` says so (the navigator's box), then everything until the finger lifts. True when taken.
    bool touch(float x, float y, int action, bool claim);
    // the press position of the last tap the overlay did not take (for the canvas' own hit tests)
    bool over_window(float x, float y) const;
};

} // namespace orca
