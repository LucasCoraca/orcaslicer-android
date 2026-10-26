#define IMGUI_DEFINE_MATH_OPERATORS   // ImVec2 arithmetic of ImGuiWrapper
#include "Viewport.hpp"
#include "ImGuiOverlay.hpp"

#include <GLES3/gl3.h>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

#include "Shaders.hpp"
#include "imgui_internal.h"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include <boost/algorithm/string/predicate.hpp>

using namespace Slic3r;
using Slic3r::GUI::into_u8;

namespace orca {

// ImGuiWrapper's colours (ImGuiWrapper.cpp) used by init_style
const ImVec4 ImGuiWrapper::COL_GREY_DARK         = {0.333f, 0.333f, 0.333f, 1.0f};
const ImVec4 ImGuiWrapper::COL_GREY_LIGHT        = {0.4f, 0.4f, 0.4f, 1.0f};
const ImVec4 ImGuiWrapper::COL_ORANGE_DARK       = {0.757f, 0.404f, 0.216f, 1.0f};
const ImVec4 ImGuiWrapper::COL_ORANGE_LIGHT      = ImGuiWrapper::to_ImVec4(ColorRGBA::ORANGE());
const ImVec4 ImGuiWrapper::COL_WINDOW_BACKGROUND = {0.1f, 0.1f, 0.1f, 0.8f};
const ImVec4 ImGuiWrapper::COL_BUTTON_BACKGROUND = COL_ORANGE_DARK;
const ImVec4 ImGuiWrapper::COL_BLUE_LIGHT        = ImVec4(0.122f, 0.557f, 0.918f, 1.0f);
const ImVec4 ImGuiWrapper::COL_HOVER             = {0.933f, 0.933f, 0.933f, 1.0f};
const ImVec4 ImGuiWrapper::COL_ACTIVE            = {0.675f, 0.675f, 0.675f, 1.0f};
const ImVec4 ImGuiWrapper::COL_ORCA              = ImGuiWrapper::to_ImVec4(ColorRGBA::ORCA());
const ImVec4 ImGuiWrapper::COL_ORCA_DARK         = { 0.f       , 103 / 255.f, 91  / 255.f, 1.f };
const ImVec4 ImGuiWrapper::COL_ORCA_HOVER        = { 38 / 255.f, 166 / 255.f, 154 / 255.f, 1.f };
const ImVec4 ImGuiWrapper::COL_ORCA_HOVER_DARK   = { 0.f       , 129 / 255.f, 114  / 255.f, 1.f };
const ImVec4 ImGuiWrapper::COL_GREEN_LIGHT       = {0.f, 156 / 255.f, 136 / 255.f, 0.25f};
const ImVec4 ImGuiWrapper::COL_SEPARATOR         = {0.93f, 0.93f, 0.93f, 1.0f};
const ImVec4 ImGuiWrapper::COL_SEPARATOR_DARK    = {0.24f, 0.24f, 0.27f, 1.0f};
const ImVec4 ImGuiWrapper::COL_TITLE_BG          = {0.745f, 0.745f, 0.745f, 1.0f};
const ImVec4 ImGuiWrapper::COL_WINDOW_BG         = {1.000f, 1.000f, 1.000f, 1.0f};
const ImVec4 ImGuiWrapper::COL_WINDOW_BG_DARK    = {45 / 255.f, 45 / 255.f, 49 / 255.f, 1.f};

static bool m_is_dark_mode = false;
void ImGuiWrapper::on_change_color_mode(bool is_dark) { m_is_dark_mode = is_dark; }

#include "slic3r/GUI/ImGuiStyle.inc"

void ImGuiWrapper::set_next_window_pos(float x, float y, int flag, float pivot_x, float pivot_y)
{
    ImGui::SetNextWindowPos(ImVec2(x, y), (ImGuiCond) flag, ImVec2(pivot_x, pivot_y));
    ImGui::SetNextWindowSize(ImVec2(0.0, 0.0));
}
bool ImGuiWrapper::begin(const std::string &name, int flags) { return ImGui::Begin(name.c_str(), nullptr, (ImGuiWindowFlags) flags); }
bool ImGuiWrapper::begin(const std::string &name, bool *close, int flags) { return ImGui::Begin(name.c_str(), close, (ImGuiWindowFlags) flags); }
void ImGuiWrapper::end() { ImGui::End(); }
void ImGuiWrapper::text(const char *label) { ImGui::Text("%s", label); }
void ImGuiWrapper::text(const std::string &label) { text(label.c_str()); }
void ImGuiWrapper::text_colored(const ImVec4 &color, const char *label) { ImGui::TextColored(color, "%s", label); }
void ImGuiWrapper::text_colored(const ImVec4 &color, const std::string &label) { text_colored(color, label.c_str()); }

ImGuiWrapper &imgui_wrapper()
{
    static ImGuiWrapper w;
    return w;
}

namespace {
struct State
{
    ImGuiContext *ctx{nullptr};
    GLuint        font_tex{0}, vao{0}, vbo{0}, ebo{0};
    float         font_px{0.f};
    unsigned      gl_generation{0};   // the EGL context of font_tex / vao
    std::mutex    mutex;
    struct Input { float x, y; bool down; };
    std::deque<Input> input;
    float             mouse_x{-1e6f}, mouse_y{-1e6f};
    bool              mouse_down{false};
    bool              capturing{false};
    std::vector<ImRect> windows;   // the interactive windows of the last frame
};
State &state()
{
    static State s;
    return s;
}

void ensure_context(float scale)
{
    State &s = state();
    // ImGuiWrapper's font size at the canvas scale
    const float font_px = std::round(16.f * scale);
    // a new EGL context: the texture and the buffers are made again
    if (s.gl_generation != Viewport::gl_generation()) {
        s.font_tex = s.vao = s.vbo = s.ebo = 0;
        s.font_px = 0.f;
        s.gl_generation = Viewport::gl_generation();
    }
    if (s.ctx && s.font_px == font_px) return;
    if (!s.ctx) {
        IMGUI_CHECKVERSION();
        s.ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(s.ctx);
        ImGui::GetIO().IniFilename = nullptr;
        imgui_wrapper().init_style();
    }
    ImGui::SetCurrentContext(s.ctx);
    ImGuiStyle &style = ImGui::GetStyle();
    ImGuiStyle  fresh;   // the metrics scaled from the defaults, the colours kept
    std::copy(std::begin(style.Colors), std::end(style.Colors), std::begin(fresh.Colors));
    fresh.WindowRounding = style.WindowRounding;
    style                = fresh;
    style.ScaleAllSizes(scale);
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    // Latin-1 plus the overlay strings of the language; Orca's default ranges add all CJK ideographs: too big for GLES
    static ImVector<ImWchar> ranges;
    ranges.clear();
    ImFontGlyphRangesBuilder builder;
    static const ImWchar latin[] = {0x0020, 0x00FF, 0x2070, 0x209F, 0x00B7, 0x00B7, 0x00B3, 0x00B3, 0};
    builder.AddRanges(latin);
    for (const char *l : {"Front", "Back", "Top", "Bottom", "Left", "Right"}) builder.AddText(_u8L_CONTEXT(l, "Camera View").c_str());
    for (const char *l : {"X", "Y", "Z"}) builder.AddText(_u8L_CONTEXT(l, "Axis").c_str());
    for (const char *l : {"Plate center of mass", "Part center of mass", "Support center of mass", "Assembly center of mass",
                          "Object center of mass", "Finished print", "Printed up to layer %1%", "Weight", "Volume",
                          "Center in bounding box", "Bounding box size", "Moment of inertia", "mm"})
        builder.AddText(_u8L(l).c_str());
    builder.AddText(u8"³·²");
    builder.BuildRanges(&ranges);
    io.Fonts->AddFontFromFileTTF((resources_dir() + "/fonts/HarmonyOS_Sans_SC_Regular.ttf").c_str(), font_px, nullptr, ranges.Data);
    unsigned char *pixels;
    int            w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    if (!s.font_tex) glGenTextures(1, &s.font_tex);
    glBindTexture(GL_TEXTURE_2D, s.font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    io.Fonts->SetTexID((ImTextureID)(intptr_t) s.font_tex);
    s.font_px = font_px;
}

// ImGuiWrapper::render_draw_data on GLES with the ES imgui shader
void render_draw_data(ImDrawData *dd, int fb_h)
{
    State &s    = state();
    GLuint prog = gl::orca_program("imgui");
    if (!prog || dd->CmdListsCount == 0) return;
    if (!s.vao) { glGenVertexArrays(1, &s.vao); glGenBuffers(1, &s.vbo); glGenBuffers(1, &s.ebo); }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glUseProgram(prog);
    const float L = dd->DisplayPos.x, R = dd->DisplayPos.x + dd->DisplaySize.x, T = dd->DisplayPos.y, B = dd->DisplayPos.y + dd->DisplaySize.y;
    const float ortho[16] = {2.f / (R - L), 0, 0, 0, 0, 2.f / (T - B), 0, 0, 0, 0, -1.f, 0, (R + L) / (L - R), (T + B) / (B - T), 0, 1.f};
    glUniformMatrix4fv(glGetUniformLocation(prog, "ProjMtx"), 1, GL_FALSE, ortho);
    glUniform1i(glGetUniformLocation(prog, "s_texture"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s.vao);
    glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ebo);
    const GLint pos = glGetAttribLocation(prog, "Position"), uv = glGetAttribLocation(prog, "UV"), col = glGetAttribLocation(prog, "Color");
    glEnableVertexAttribArray(pos); glEnableVertexAttribArray(uv); glEnableVertexAttribArray(col);
    glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void *) IM_OFFSETOF(ImDrawVert, pos));
    glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void *) IM_OFFSETOF(ImDrawVert, uv));
    glVertexAttribPointer(col, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (void *) IM_OFFSETOF(ImDrawVert, col));
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList *cl = dd->CmdLists[n];
        glBufferData(GL_ARRAY_BUFFER, cl->VtxBuffer.Size * sizeof(ImDrawVert), cl->VtxBuffer.Data, GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, cl->IdxBuffer.Size * sizeof(ImDrawIdx), cl->IdxBuffer.Data, GL_STREAM_DRAW);
        for (const ImDrawCmd &cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            glScissor(int(cmd.ClipRect.x), int(fb_h - cmd.ClipRect.w), int(cmd.ClipRect.z - cmd.ClipRect.x), int(cmd.ClipRect.w - cmd.ClipRect.y));
            glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t) cmd.TextureId);
            glDrawElements(GL_TRIANGLES, GLsizei(cmd.ElemCount), sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                           (void *) (cmd.IdxOffset * sizeof(ImDrawIdx)));
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
} // namespace

ImGuiOverlay &ImGuiOverlay::get()
{
    static ImGuiOverlay o;
    return o;
}

bool ImGuiOverlay::frame(int width, int height, float scale, const std::function<void()> &content)
{
    State &s = state();
    ensure_context(scale);
    ImGui::SetCurrentContext(s.ctx);
    ImGuiIO &io    = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(width), float(height));
    io.DeltaTime   = 1.f / 60.f;
    bool more      = false;
    {
        std::lock_guard<std::mutex> l(s.mutex);
        if (!s.input.empty()) {
            const State::Input in = s.input.front();
            s.input.pop_front();
            s.mouse_x = in.x; s.mouse_y = in.y; s.mouse_down = in.down;
        }
        more = !s.input.empty();
    }
    io.MousePos     = ImVec2(s.mouse_x, s.mouse_y);
    io.MouseDown[0] = s.mouse_down;
    imgui_wrapper().requires_extra_frame = false;
    ImGui::NewFrame();
    content();
    ImGui::Render();
    render_draw_data(ImGui::GetDrawData(), height);
    {
        // the windows a press goes to (not the full-canvas ImGuizmo window, which takes no input)
        std::lock_guard<std::mutex> l(s.mutex);
        s.windows.clear();
        // an auto-sized window is hidden on the frames it measures itself: draw again until it shows
        for (ImGuiWindow *w : s.ctx->Windows)
            if (w->Active && (w->HiddenFramesCannotSkipItems > 0 || w->HiddenFramesCanSkipItems > 0)) imgui_wrapper().requires_extra_frame = true;
        for (ImGuiWindow *w : s.ctx->Windows)
            if (w->Active && !w->Hidden && !(w->Flags & ImGuiWindowFlags_NoMouseInputs) && !(w->Flags & ImGuiWindowFlags_NoInputs) &&
                !(w->Flags & ImGuiWindowFlags_ChildWindow))
                s.windows.push_back(w->Rect());
    }
    // a lifted finger moves away so nothing stays hovered
    if (!s.mouse_down && !more) { s.mouse_x = -1e6f; s.mouse_y = -1e6f; }
    return more || imgui_wrapper().requires_extra_frame || s.mouse_down || ImGui::IsAnyItemActive();
}

bool ImGuiOverlay::over_window(float x, float y) const
{
    for (const ImRect &r : state().windows)
        if (r.Contains(ImVec2(x, y))) return true;
    return false;
}

bool ImGuiOverlay::touch(float x, float y, int action, bool claim)
{
    State &s = state();
    std::lock_guard<std::mutex> l(s.mutex);
    if (action == 0) {
        bool on_window = false;
        for (const ImRect &r : s.windows)
            if (r.Contains(ImVec2(x, y))) on_window = true;
        s.capturing = claim || on_window;
        if (!s.capturing) return false;
        s.input.push_back({x, y, false});   // hover first: ImGui and ImGuizmo act on the hovered item
        s.input.push_back({x, y, true});
        return true;
    }
    if (!s.capturing) return false;
    if (action == 1) s.input.push_back({x, y, true});
    else {
        s.input.push_back({x, y, false});
        s.capturing = false;
    }
    return true;
}

} // namespace orca
