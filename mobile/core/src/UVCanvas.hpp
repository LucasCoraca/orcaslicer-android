// UVEditorCanvas without its GL: the pane's islands, view, selection and gestures (upstream's logic, extracted into
// TextureDisp.inc.uv), drawn by the app from export_view(). Included inside namespace Slic3r::GUI::td by TextureDisp.cpp.

// the pointer events upstream's handlers read: the UV pane's (wxEVT_*), the 3D view's for Adjust placement / seams
enum { wxEVT_LEFT_DOWN = 10200, wxEVT_LEFT_UP, wxEVT_RIGHT_DOWN, wxEVT_RIGHT_UP, wxEVT_MIDDLE_DOWN, wxEVT_MIDDLE_UP, wxEVT_MOTION,
       wxEVT_MOUSEWHEEL };
class wxMouseEvent
{
public:
    wxMouseEvent(int type, double x, double y, bool shift = false, bool ctrl = false, double wheel = 0, bool left_held = false)
        : m_type(type), m_x(x), m_y(y), m_shift(shift), m_ctrl(ctrl), m_wheel(wheel), m_left(left_held) {}
    int     GetEventType() const { return m_type; }
    wxPoint GetPosition() const { return wxPoint(int(std::lround(m_x)), int(std::lround(m_y))); }
    double  GetX() const { return m_x; }
    double  GetY() const { return m_y; }
    bool    ShiftDown() const { return m_shift; }
    bool    ControlDown() const { return m_ctrl; }
    double  GetWheelRotation() const { return m_wheel; }
    int     GetWheelDelta() const { return 120; }
    bool    LeftDown() const { return m_type == wxEVT_LEFT_DOWN; }
    bool    LeftUp() const { return m_type == wxEVT_LEFT_UP; }
    bool    Dragging() const { return m_type == wxEVT_MOTION && m_left; }
    bool    Moving() const { return m_type == wxEVT_MOTION && !m_left; }
    bool    LeftIsDown() const { return m_left; }
    bool    CmdDown() const { return m_ctrl; }

private:
    int    m_type;
    double m_x, m_y;
    bool   m_shift, m_ctrl;
    double m_wheel;
    bool   m_left;
};

class UVEditorCanvas
{
public:
    using IslandTransform = Eigen::Matrix<float, 2, 3>;
    struct Islands
    {
        std::vector<Vec2f>               uvs;
        std::vector<Vec3i32>             indices;
        std::vector<int>                 vertex_island;
        std::vector<std::pair<int, int>> boundary_edges;
        int                              island_count = 0;
    };
    void set_islands(Islands islands);
    void set_island_transforms(std::vector<IslandTransform> transforms);
    void set_island_fill_colors(std::vector<ColorRGBA> colors);
    void set_uv_transform(float tiling_scale, float rotation_deg, bool tile_enabled, bool tile_mirrored);
    void set_background_texture(const std::vector<unsigned char> &grayscale_pixels, int width, int height);
    void set_snap_enabled(bool enabled) { m_snap_enabled = enabled; }
    bool snap_enabled() const { return m_snap_enabled; }
    enum class Command {
        FrameAll, ToggleSnap, AverageScale, CutSelectedIsland, ProjectFromView, JoinSelected, UnjoinSelected,
        Unwrap, SetSeamAngle, SetConnectIslands, SetSelectMode, SetMarkSeams, SetSeamPath, ClearSeams, ClearUVEdits,
        SetBackground, PickTexture, PaneClosed, SelectionChanged
    };
    void run_command(Command cmd, float value = 0.f);
    using CommandFn = std::function<void(Command, float)>;
    void set_command_callback(CommandFn fn) { m_on_command = std::move(fn); }
    enum class Background { Height, Checker, Distortion };
    struct PaneState
    {
        bool                       has_layer       = false;
        wxString                   layer_name;
        float                      tile_mm         = 0.f;
        std::vector<unsigned char> thumbnail_rgb;
        int                        thumbnail_px    = 0;
        float                      seam_angle_deg  = 40.f;
        bool                       connect_islands = true;
        bool                       unwrapped       = false;
        bool                       unwrap_stale    = false;
        bool                       mark_seams      = false;
        bool                       seam_path       = false;
        bool                       has_seams       = false;
        bool                       has_uv_edits    = false;
        Background                 background      = Background::Height;
        int                        island_count    = 0;
        size_t                     face_count      = 0;
    };
    void             set_pane_state(PaneState state);
    const PaneState &pane_state() const { return m_pane_state; }
    using PaneStateFn = std::function<void(const PaneState &)>;
    void set_pane_state_callback(PaneStateFn fn) { m_on_pane_state = std::move(fn); }
    using StatusFn = std::function<void(const wxString &)>;
    void set_status_callback(StatusFn fn) { m_on_status = std::move(fn); }
    using IslandEditFn = std::function<void(int island, const Vec2f &offset_delta, float rotation_delta, float scale_factor, bool finished)>;
    void set_island_edit_callback(IslandEditFn fn) { m_on_island_edit = std::move(fn); }
    enum class SelectMode { Island, Vertex, Edge };
    void       set_select_mode(SelectMode mode);
    SelectMode select_mode() const { return m_select_mode; }
    using UVVertexEditFn = std::function<void(const std::vector<std::pair<int, Vec2f>> &edits)>;
    void set_vertex_edit_callback(UVVertexEditFn fn) { m_on_vertex_edit = std::move(fn); }
    int  selected_island() const { return m_selected_island; }
    bool has_islands() const { return m_islands.island_count > 0 && !m_islands.indices.empty(); }
    const std::vector<int> &selected_islands() const { return m_selection; }
    void reset_view();

    // the port's: the pane's size in pixels, a pointer / key event, what the app draws
    void           set_size(int w, int h) { m_size = wxSize(std::max(1, w), std::max(1, h)); }
    void           on_mouse(wxMouseEvent &evt);
    void           on_key(wxKeyEvent &evt);
    nlohmann::json export_view();
    std::string    background_file();
    bool           modal_gesture() const { return m_gesture == Gesture::RotateIslandModal || m_gesture == Gesture::ScaleIslandModal; }
    wxString       m_status;

private:
    // wxGLCanvas
    wxSize GetSize() const { return m_size; }
    void   Refresh() { ++m_revision; }
    void   SetFocus() {}
    bool   HasCapture() const { return m_captured; }
    void   CaptureMouse() { m_captured = true; }
    void   ReleaseMouse() { m_captured = false; }
    wxPoint ScreenToClient(const wxPoint &p) const { return p; }
    wxPoint wxGetMousePosition() const { return m_cursor_px; }
    wxSize   m_size{1, 1};
    bool     m_captured = false;
    unsigned m_revision = 0;
    unsigned m_background_revision = 0;

    void  update_status();
    void  update_hover(const wxPoint &pos);
    void  content_bounds(Vec2f &min_uv, Vec2f &max_uv) const;
    void  framed_bounds(Vec2f &min_uv, Vec2f &max_uv) const;
    void  fit_view_to_content();
    void  view_half_extents(float &half_w, float &half_h) const;
    Vec2f screen_to_uv(const wxPoint &px) const;
    Vec2f island_uv(size_t vertex) const;
    int   island_at(const Vec2f &uv) const;
    int   vertex_at(const Vec2f &uv) const;
    std::pair<int, int> edge_at(const Vec2f &uv) const;
    void  move_vertex_raw(int unwrapped_vertex, const Vec2f &delta_uv);
    Vec2f island_centroid(int island) const;
    Vec2f uv_delta_to_unwrap(const Vec2f &delta_uv) const;
    Vec2f snap_correction(int island) const;
    void  end_gesture();
    void  cancel_gesture();
    void  grab_mouse();
    void  drop_mouse();

    Islands                      m_islands;
    std::vector<IslandTransform> m_transforms;
    std::vector<ColorRGBA>       m_island_fill_colors;
    std::vector<std::vector<int>> m_island_boundary_verts;
    std::vector<std::vector<std::pair<int, int>>> m_island_boundary_edges;
    std::vector<std::vector<int>>                 m_island_tris;
    std::vector<std::pair<Vec2f, Vec2f>>          m_island_raw_bounds;
    bool m_mesh_dirty = true;
    float m_tiling_scale  = 1.f;
    float m_rotation_deg  = 0.f;
    bool  m_tile_enabled  = true;
    bool  m_tile_mirrored = false;
    bool  m_snap_enabled  = false;
    std::vector<unsigned char> m_background_pixels;
    int                        m_background_width = 0, m_background_height = 0;
    bool                       m_background_dirty = false;
    bool                       m_background_quad_dirty = true;
    Vec2f m_pan       = Vec2f(0.5f, 0.5f);
    float m_zoom      = 0.75f;
    bool  m_needs_fit = true;
    enum class Gesture { None, Pan, MoveIsland, RotateIsland, RotateIslandModal, ScaleIslandModal, MoveVertex, MoveEdge };
    Gesture             m_gesture         = Gesture::None;
    SelectMode          m_select_mode  = SelectMode::Island;
    int                 m_active_vertex = -1;
    std::pair<int, int> m_active_edge{ -1, -1 };
    std::vector<int>                 m_sel_vertices;
    std::vector<std::pair<int, int>> m_sel_edges;
    bool is_vertex_selected(int v) const { return std::find(m_sel_vertices.begin(), m_sel_vertices.end(), v) != m_sel_vertices.end(); }
    bool is_edge_selected(const std::pair<int, int> &e) const { return std::find(m_sel_edges.begin(), m_sel_edges.end(), e) != m_sel_edges.end(); }
    std::vector<int> selected_edge_endpoints() const;
    wxPoint m_cursor_px{ 0, 0 };
    bool    m_cursor_inside = false;
    bool                m_vertex_edit_moved = false;
    int                 m_hover_island = -1;
    int                 m_hover_vertex = -1;
    std::pair<int, int> m_hover_edge{ -1, -1 };
    int     m_selected_island = -1;
    std::vector<int> m_selection;
    bool             is_selected(int island) const { return std::find(m_selection.begin(), m_selection.end(), island) != m_selection.end(); }
    wxPoint m_drag_last_px;
    Vec2f   m_gesture_last_uv    = Vec2f::Zero();
    float   m_gesture_last_angle = 0.f;
    float   m_gesture_last_dist  = 0.f;
    float   m_rot_raw_deg       = 0.f;
    float   m_rot_applied_deg   = 0.f;
    float   m_rot_base_deg      = 0.f;
    float   m_rot_display_deg   = 0.f;
    float   m_modal_scale_accum = 1.f;
    float   island_rotation_deg(int island) const;
    IslandEditFn   m_on_island_edit;
    UVVertexEditFn m_on_vertex_edit;
    CommandFn      m_on_command;
    StatusFn       m_on_status;
    PaneState      m_pane_state;
    PaneStateFn    m_on_pane_state;
};
