// The sidebar's filaments: Sidebar's add / delete / merge of filament slots, the mixed filaments (virtual slots blended
// from 2-3 physical ones at slicing time), the automatic flushing volumes, Plater's filament count / delete updates,
// ObjectList's extruder remaps and PartPlate's per-filament data -- compiled from upstream (Filaments.inc*) on
// stand-ins. MixedFilamentDialog is the app's (Host::ask "mixed_filament"); its colour math is here (mixed.* pure
// commands, callable while that dialog holds the engine).
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <boost/log/trivial.hpp>

#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <atomic>
#include <cstring>
#include <limits>
#include <boost/filesystem.hpp>
#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "Settings.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/ColorDecomposeRecipe.hpp"
#include <array>
#include <fstream>
#include <functional>
#include "libslic3r/FlushVolCalc.hpp"
#include "libslic3r/FlushVolPredictor.hpp"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/format.hpp"
#include "wx/msgdlg.h"

namespace orca {
json presets_state();     // Presets.cpp
void presets_changed();
}

// DevPrinterConfigUtil::get_toolhead_display_name (the flushing dialog's extruder labels)
namespace Slic3r {
using json = nlohmann::json;
#include "slic3r/GUI/Filaments.inc.dev"
} // namespace Slic3r

namespace Slic3r { namespace GUI { namespace fil {

// ---- wxColour, as the bodies use it
enum { wxC2S_HTML_SYNTAX = 1 };
struct wxColour
{
    unsigned char r{0}, g{0}, b{0}, a{255};
    bool          ok{false};
    wxColour() = default;
    wxColour(int r, int g, int b, int a = 255) : r(r), g(g), b(b), a(a), ok(true) {}
    wxColour(const std::string &s) { parse(s); }
    wxColour(const char *s) { parse(s); }
    wxColour(const wxString &s) { parse(s.ToStdString()); }
    void parse(const std::string &s)
    {
        unsigned int v = 0;
        if (s.size() == 7 && s[0] == '#' && std::sscanf(s.c_str() + 1, "%x", &v) == 1) {
            r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF; a = 255; ok = true;
        } else if (s.size() == 9 && s[0] == '#' && std::sscanf(s.c_str() + 1, "%x", &v) == 1) {
            r = (v >> 24) & 0xFF; g = (v >> 16) & 0xFF; b = (v >> 8) & 0xFF; a = v & 0xFF; ok = true;
        }
    }
    unsigned char Red() const { return r; }
    unsigned char Green() const { return g; }
    unsigned char Blue() const { return b; }
    unsigned char Alpha() const { return a; }
    bool          IsOk() const { return ok; }
    wxString GetAsString(int) const
    {
        char buf[8];
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X", r, g, b);
        return wxString(buf);
    }
};

// ---- PartPlate: one plate's per-filament data (filament maps, print sequences) in the app's plate config
class FlPlate
{
public:
    explicit FlPlate(orca::Plate &plate) : m_config(plate.config), m_plate(plate) {}
    std::vector<LayerPrintSequence> get_other_layers_print_sequence() const;
    void set_other_layers_print_sequence(const std::vector<LayerPrintSequence> &layer_seq_list);
    void update_first_layer_print_sequence(size_t filament_nums);
    void update_first_layer_print_sequence_when_delete_filament(size_t filament_id);
    void set_filament_count(int filament_count);
    void on_filament_added();
    void on_filament_deleted(int filament_count, int filament_id);
    // PartPlate::contain_instance_totally: the instance is on this plate
    bool contain_instance_totally(ModelObject *object, int instance_id) const
    {
        const auto &objs = orca::Plater::get().model().objects;
        const int   obj  = int(std::find(objs.begin(), objs.end(), object) - objs.begin());
        return m_plate.instances.count({obj, instance_id}) > 0;
    }

private:
    DynamicPrintConfig &m_config;
    orca::Plate        &m_plate;
};
using PartPlate = FlPlate;

class FlPlateList
{
public:
    std::vector<std::unique_ptr<FlPlate>> plates;
    FlPlateList()
    {
        orca::Plater &pl = orca::Plater::get();
        for (size_t i = 0; i < pl.plates.size(); ++i) plates.emplace_back(new FlPlate(pl.plates[i]));
    }
    int      get_plate_count() const { return int(plates.size()); }
    FlPlate *get_plate(int i) { return i >= 0 && i < int(plates.size()) ? plates[i].get() : nullptr; }
    FlPlate *get_curr_plate() { return get_plate(orca::Plater::get().current_plate); }
    void     set_filament_count(int n) { for (auto &p : plates) p->set_filament_count(n); }
    void     on_filament_added(int) { for (auto &p : plates) p->on_filament_added(); }
    void     on_filament_deleted(int n, int id) { for (auto &p : plates) p->on_filament_deleted(n, id); }
};

// ---- ObjectList: the extruder remaps of its items; its data view model stands for the model's objects and layer ranges
enum class ItemType { itUndef = 0, itObject, itVolume, itLayerRoot, itLayer };
struct ObjectDataViewModelNode
{
    ItemType                                            type{ItemType::itObject};
    t_layer_height_range                                range;
    std::vector<std::unique_ptr<ObjectDataViewModelNode>> children;
    size_t                   GetChildCount() const { return children.size(); }
    ObjectDataViewModelNode *GetNthChild(size_t i) const { return children[i].get(); }
    ItemType                 GetType() const { return type; }
    t_layer_height_range     GetLayerRange() const { return range; }
};
struct wxDataViewItem
{
    void *id{nullptr};
    wxDataViewItem(void *p = nullptr) : id(p) {}
    void *GetID() const { return id; }
    explicit operator bool() const { return id != nullptr; }
};
struct FlObjectsModel
{
    std::vector<std::unique_ptr<ObjectDataViewModelNode>> objects;
    explicit FlObjectsModel(const ModelObjectPtrs &objs)
    {
        for (const ModelObject *o : objs) {
            auto node = std::make_unique<ObjectDataViewModelNode>();
            if (!o->layer_config_ranges.empty()) {
                auto root = std::make_unique<ObjectDataViewModelNode>();
                root->type = ItemType::itLayerRoot;
                for (const auto &r : o->layer_config_ranges) {
                    auto layer = std::make_unique<ObjectDataViewModelNode>();
                    layer->type = ItemType::itLayer;
                    layer->range = r.first;
                    root->children.push_back(std::move(layer));
                }
                node->children.push_back(std::move(root));
            }
            objects.push_back(std::move(node));
        }
    }
    wxDataViewItem GetItemById(size_t i) { return i < objects.size() ? wxDataViewItem(objects[i].get()) : wxDataViewItem(); }
    wxDataViewItem GetItemByVolumeId(size_t i, size_t) { return GetItemById(i); }
    void           SetExtruder(const wxString &, wxDataViewItem) {}
};

class FlPlater;
class FlObjectList
{
public:
    ModelObjectPtrs *m_objects{nullptr};
    FlObjectsModel  *m_objects_model{nullptr};
    void update_filament_values_for_items(const size_t filaments_count);
    void update_filament_values_for_items_when_delete_filament(const size_t filament_id, const int replace_id);
    // ObjectList::update_objects_list_filament_column(_when_delete_filament): the remaps; the column is the app's
    void update_objects_list_filament_column(size_t filaments_count)
    {
        FlObjectsModel model(*m_objects);
        m_objects_model = &model;
        update_filament_values_for_items(filaments_count);
        m_objects_model = nullptr;
    }
    void update_objects_list_filament_column_when_delete_filament(size_t filament_id, size_t, int replace_filament_id)
    {
        FlObjectsModel model(*m_objects);
        m_objects_model = &model;
        update_filament_values_for_items_when_delete_filament(filament_id, replace_filament_id);
        m_objects_model = nullptr;
    }
    void update_filament_colors() {}
    void update_info_items(size_t) {}
};

// ---- Sidebar
struct FlWindow
{
    bool m_shown{true};
    bool IsShown() const { return m_shown; }
    void Show(bool show = true) { m_shown = show; }
    void Scroll(int, int) {}
    void Layout() {}
};
// PlaterPresetComboBox of a physical filament slot
struct FlCombo
{
    size_t   idx;
    wxString GetValue() const
    {
        PresetBundle &b = *orca::Core::get().preset_bundle;
        const Preset *p = idx < b.filament_presets.size() ? b.filaments.find_preset(b.filament_presets[idx], false, true) : nullptr;
        return wxString(p ? p->label(false) : (idx < b.filament_presets.size() ? b.filament_presets[idx] : std::string()));
    }
};
class FlSidebar;
constexpr int kSidebarContextMenuFilamentId = -2;
struct FlSidebarPriv
{
    std::vector<FlCombo *> combos_filament;
    std::vector<FlCombo>   combo_store;
    int                    m_menu_filament_id{-1};
    int                    editing_filament{-1};
    FlWindow               content;
    FlWindow              *m_panel_filament_content{&content};
    FlPlater              *plater{nullptr};
};
struct SimpleEvent
{
    SimpleEvent(int, void *) {}
};
enum { EVT_SCHEDULE_BACKGROUND_PROCESS = 1 };
template<class T> void wxPostEvent(T *, const SimpleEvent &) {}   // the commands end with presets_changed (auto reslice)

class FlSidebar
{
public:
    FlSidebarPriv  priv;
    FlSidebarPriv *p{&priv};
    FlWindow       sizer;
    FlWindow      *m_scrolled_sizer{&sizer};
    FlObjectList   list;
    explicit FlSidebar(FlPlater *plater);
    FlPlater                    *GetParent() const { return p->plater; }
    std::vector<FlCombo *>      &combos_filament() { return p->combos_filament; }
    FlObjectList                *obj_list() { return &list; }

    void add_filament();
    void add_custom_filament(wxColour new_col, const std::string &preset_name = "", bool skip_preset_validation = false);
    void delete_filament(size_t filament_id = size_t(-1), int replace_filament_id = -1);
    void change_filament(size_t from_id, size_t to_id);
    void collect_physical_filament_info(std::vector<std::string> &color_strs, std::vector<std::string> &names,
                                        std::vector<std::string> &types, std::vector<size_t> *config_indices = nullptr);
    int  ensure_mixed_filament(const std::vector<unsigned int> &components, const std::vector<int> &ratios);
    void add_mixed_filament();
    void edit_mixed_filament(size_t panel_idx);
    void remove_all_mixed_filaments();
    void delete_mixed_filament_at(size_t panel_idx);
    bool has_broken_mixed_filament() const;
    bool has_broken_mixed_filament(const PartPlate *plate) const;
    void auto_calc_flushing_volumes(const int filament_idx, const int extruder_id = -1);
    void auto_calc_flushing_volumes_internal(const int modify_id, const int extruder_id);
    static bool should_show_SEMM_buttons();
    void decompose_filament_color(int filament_idx);

    // the UI updates: the app's sidebar redraws from presets_state
    void update_mixed_filament_list();
    void update_dynamic_filament_list() {}
    // Sidebar::on_filament_count_change / on_filaments_delete: one combo per physical slot again, then the mixed list
    void rebuild_combos();
    void on_filament_count_change(size_t) { rebuild_combos(); update_mixed_filament_list(); }
    void on_filaments_delete(size_t) { rebuild_combos(); update_mixed_filament_list(); }
    void set_flushing_volume_warning(bool) {}
    // Sidebar::is_new_project_in_gcode3mf: a previewed G-code is closed first
    bool is_new_project_in_gcode3mf();
    // what update_mixed_filament_list found: a mixed slot an object on the current plate uses is broken
    bool m_mixed_filament_broken{false};
};
using Sidebar = FlSidebar;

// Plater::priv: its config is the plater's copy of the full config (the support filaments and colours it updates)
class FlPlaterPriv
{
public:
    DynamicPrintConfig full{orca::Core::get().full_config()};
    DynamicPrintConfig *config{&full};
    Model              &model{orca::Plater::get().model()};
};
class FlPlater
{
public:
    FlPlaterPriv  priv;
    FlPlaterPriv *p{&priv};
    FlSidebar     side{this};
    FlPlateList   plate_list;

    FlSidebar   &sidebar() { return side; }
    FlPlateList &get_partplate_list() { return plate_list; }
    Model       &model() { return orca::Plater::get().model(); }
    void         update() {}
    void         update_project_dirty_from_presets() {}
    static wxColour get_next_color_for_filament() { return wxColour(orca::next_filament_color()); }

    void on_filament_count_change(size_t num_filaments);
    void on_filaments_delete(size_t num_filaments, size_t filament_id, int replace_filament_id, const std::vector<unsigned char> &is_mixed_before_delete = {});
    std::vector<size_t>      mixed_filament_config_indices() const;
    std::vector<size_t>      physical_filament_config_indices() const;
    std::vector<std::string> get_extruder_colors_from_plater_config(const GCodeProcessorResult *const result = nullptr) const;
    bool                     update_filament_colors_in_full_config();

    // Plater::priv's textured model import (TextureImport below)
    FlSidebar *m_sidebar{&side};
    using LoadProgressCallback = std::function<bool(int, const wxString &)>;
    struct TextureImportResult;
    void apply_textured_mesh_import_result(Slic3r::Model &loaded_model, const std::vector<size_t> &obj_idxs, const TextureImportResult &result,
                                           LoadProgressCallback progress_callback = {}, bool update_scene = true);
};
using Plater = FlPlater;

// Tab::update / Tab::select_preset of the print and filament tabs
struct FlTab
{
    Preset::Type type;
    void update() {}
    void select_preset(const std::string &name, bool = false, const std::string & = "", bool = false)
    {
        // the Filament tab's preset (Presets.cpp select_preset)
        if (type == Preset::TYPE_FILAMENT) orca::Core::get().call("presets.select", {{"type", "filament"}, {"name", name}, {"force", true}});
    }
};

// MixedFilamentDialog: the app's dialog
#include "slic3r/GUI/Filaments.inc.res"
class MixedFilamentDialog
{
public:
    MixedFilamentDialog(void *, const std::vector<std::string> &colors, const std::vector<std::string> &names, const std::vector<std::string> &types = {})
        : m_colors(colors), m_names(names), m_types(types), m_title(_u8L("Add Mixed Filament"))
    {
        m_result.components = {1, (colors.size() >= 2) ? 2u : 1u};
        m_result.ratios     = {50, 50};
    }
    MixedFilamentDialog(void *, const MixedFilamentResult &existing, const std::vector<std::string> &colors, const std::vector<std::string> &names,
                        const std::vector<std::string> &types = {})
        : m_result(existing), m_colors(colors), m_names(names), m_types(types), m_title(_u8L("Edit Mixed Filament"))
    {
        if (m_result.components.size() < 2) {
            m_result.components = {1, (colors.size() >= 2) ? 2u : 1u};
            m_result.ratios     = {50, 50};
        }
    }
    int ShowModal();
    MixedFilamentResult get_result() const { return m_result; }

private:
    MixedFilamentResult      m_result;
    std::vector<std::string> m_colors, m_names, m_types;
    std::string              m_title;
};

struct FlApp
{
    PresetBundle *preset_bundle{nullptr};
    AppConfig    *app_config{nullptr};
    FlPlater     *m_plater{nullptr};
    FlTab         print_tab{Preset::TYPE_PRINT}, filament_tab{Preset::TYPE_FILAMENT};
    FlPlater     *plater() { return m_plater; }
    Model        &model() { return orca::Plater::get().model(); }
    FlTab        *get_tab(Preset::Type t) { return t == Preset::TYPE_FILAMENT ? &filament_tab : &print_tab; }
    FlSidebar    &sidebar() { return m_plater->sidebar(); }
    FlObjectList *obj_list() { return m_plater->sidebar().obj_list(); }
    size_t        filaments_cnt() const { return preset_bundle->filament_presets.size(); }
    bool          is_gcode_viewer() const { return false; }
    bool          dark_mode() const { return orca::Core::get().app_config->get("dark_color_mode") == "1"; }
    // the Bambu network agent's filament id translation: no agent on Android (ids stay as they are)
    struct Agent
    {
        std::string to_orca_filament_id(const std::string &id) { return id; }
        std::string from_orca_filament_id(const std::string &id) { return id; }
    };
    Agent *getAgent() { return nullptr; }
};
FlApp &wxGetApp()
{
    static FlApp app;
    app.preset_bundle = orca::Core::get().preset_bundle.get();
    app.app_config    = orca::Core::get().app_config.get();
    return app;
}

FlSidebar::FlSidebar(FlPlater *plater)
{
    p->plater = plater;
    rebuild_combos();
    list.m_objects = &orca::Plater::get().model().objects;
}

// one combo per physical slot (Sidebar::on_filament_count_change)
void FlSidebar::rebuild_combos()
{
    PresetBundle &b = *orca::Core::get().preset_bundle;
    const auto *is_mixed = b.project_config.option<ConfigOptionBools>("filament_is_mixed");
    p->combo_store.clear();
    p->combos_filament.clear();
    for (size_t i = 0; i < b.filament_presets.size(); ++i)
        if (!is_mixed || i >= is_mixed->values.size() || !is_mixed->values[i]) p->combo_store.push_back({i});
    for (FlCombo &c : p->combo_store) p->combos_filament.push_back(&c);
}

bool FlSidebar::is_new_project_in_gcode3mf()
{
    if (!orca::Plater::get().only_gcode) return false;
    std::string text = _u8L("After completing your operation, %s project will be closed and create a new project.");
    if (auto at = text.find("%s"); at != std::string::npos) text.replace(at, 2, "G-code");
    MessageDialog(nullptr, wxString(text), _L("Warning"), wxOK | wxICON_WARNING).ShowModal();
    orca::Core::get().call("project.new", orca::json::object());
    return true;
}

// FilamentBitmapUtils' blend of N colours (blend_color_multi with the weights scaled to ints)
static std::string blend_n(const std::vector<std::string> &hex, const std::vector<double> &weights)
{
    std::vector<int> w;
    for (double x : weights) w.push_back(int(std::lround(x * 10000.0)));
    return blend_color_multi(hex, w);
}

// ---- ColorDecomposeDialog / ColorDecomposeSupport (Sidebar::decompose_filament_color): the dialog's logic on stand-in
// widgets, the app draws it from decompose_state()
static const wxColour s_black(0, 0, 0);
#define wxBLACK (&s_black)
using DecomposeMode = ColorDecomposeRecipeMode;
#include "slic3r/GUI/Filaments.inc.cdt"
// ColorDecomposeSupport.hpp's constants
inline constexpr const char *kDecomposePlaBasicType      = "PLA Basic";
inline constexpr const char *kDecomposePetgBasicType     = "PETG Basic";
inline constexpr const char *kDecomposePlaShortType      = "PLA";
inline constexpr const char *kDecomposePetgShortType     = "PETG";
inline constexpr const char *kDecomposePlaFilamentId     = "GFA00";
inline constexpr const char *kDecomposePetgFilamentId    = "GFG00";
inline constexpr const char *kDecomposeBambuPresetPrefix = "Bambu ";
#include "slic3r/GUI/Filaments.inc.cst"
struct MixedFilamentResult;
// a wxPanel / wxStaticText / CheckBox / Button the dialog's logic shows, labels, checks and enables
struct FlSize { int x{400}, y{20}; };
struct FlWidget
{
    bool     shown{true}, value{false}, enabled{true};
    wxString label;
    wxColour bg;
    void   Show(bool s = true) { shown = s; }
    void   Hide() { shown = false; }
    bool   IsShown() const { return shown; }
    void   Refresh() {}
    void   Layout() {}
    void   SetLabel(const wxString &l) { label = l; }
    void   Wrap(int) {}
    FlSize GetClientSize() const { return {}; }
    void   SetBackgroundColour(const wxColour &c) { bg = c; }
    void   SetValue(bool v) { value = v; }
    void   Enable(bool e = true) { enabled = e; }
};
enum { DD_ITEM_STYLE_SPLIT_ITEM = 1, DD_ITEM_STYLE_DISABLED = 2, wxCB_READONLY = 0, wxHORIZONTAL = 4, wxEXPAND = 0x2000 };
struct wxBitmapStub {};
static const wxBitmapStub wxNullBitmap;
struct FlFont {};
namespace Label { static const FlFont Body_13; }
// the material type selector (ComboBox): its items, the disabled group titles among them
struct ComboBox
{
    struct Item { wxString label; int style; };
    std::vector<Item> items;
    int               sel{-1};
    template<class... A> ComboBox(A &&...) {}
    int  Append(const wxString &label, const wxBitmapStub & = wxNullBitmap, int style = 0) { items.push_back({label, style}); return int(items.size()) - 1; }
    void SetSelection(int i) { sel = i; }
    int  GetSelection() const { return sel; }
    void SetFont(const FlFont &) {}
    template<class E, class F> void Bind(E, F &&) {}
};
struct wxBoxSizer
{
    explicit wxBoxSizer(int) {}
    template<class... A> void Add(A &&...) {}
};
class ColorDecomposeDialog
{
public:
    ColorDecomposeDialog(void *, int filament_idx, const wxColour &target_color, const std::vector<std::string> &physical_colors,
                         const std::vector<std::string> &filament_names, const std::vector<std::string> &filament_types,
                         size_t current_filament_count = 0, size_t max_filament_count = 32, std::vector<size_t> physical_config_indices = {});
    ~ColorDecomposeDialog() { delete m_type_combo; }
    ColorDecomposeResult get_result() const { return m_result; }
    void set_missing_physical_calculator(std::function<size_t(const ColorDecomposeResult &)> fn);
    int  ShowModal();

private:
    wxBoxSizer *create_filament_selector();
    void select_mode(DecomposeMode mode);
    void update_card_styles();
    void update_card_visibility();
    void update_mode_card_contents() {}   // the app draws the cards from m_mode_results
    void update_matched_color_display();
    void update_ok_button_state();
    void update_filament_limit_warning();
    void compute_decomposition();
    bool try_build_single_base_result(DecomposeMode mode, ColorDecomposeResult &out) const;
    void Layout() {}
    void Fit() {}
    int  FromDIP(int v) const { return v; }

    ColorDecomposeResult                               m_result;
    std::array<ColorDecomposeResult, 3>                m_mode_results;
    int                                                m_filament_idx{-1};
    wxColour                                           m_target_color;
    std::vector<std::string>                           m_physical_colors, m_filament_names, m_filament_types, m_project_types;
    std::string                                        m_preferred_type;
    std::vector<std::string>                           m_combo_item_types;
    size_t                                             m_current_filament_count{0};
    size_t                                             m_max_filament_count{32};
    std::vector<size_t>                                m_physical_config_indices;
    std::function<size_t(const ColorDecomposeResult &)> m_missing_calculator;
    ComboBox                                           *m_type_combo{nullptr};
    FlWidget w_matched_swatch, w_matched_rgb, w_card_ml, w_card_cmyw, w_card_rybw, w_arb, w_chk_ml, w_chk_cmyw, w_chk_rybw, w_hint, w_warn, w_warn_text, w_ok;
    FlWidget *m_matched_swatch{&w_matched_swatch}, *m_matched_rgb_text{&w_matched_rgb}, *m_card_material_list{&w_card_ml}, *m_card_cmyw{&w_card_cmyw},
             *m_card_rybw{&w_card_rybw}, *m_arb_column_panel{&w_arb}, *m_chk_material_list{&w_chk_ml}, *m_chk_cmyw{&w_chk_cmyw}, *m_chk_rybw{&w_chk_rybw},
             *m_no_card_hint{&w_hint}, *m_limit_warning_panel{&w_warn}, *m_limit_warning_text{&w_warn_text}, *m_btn_ok{&w_ok};
    DecomposeMode m_selected_mode{DecomposeMode::MaterialList};
};
std::string        decompose_normalize_color_hex(std::string color);
DecomposeOfficialComponent lookup_decompose_official_component(const std::string &basic_type, DecomposeBaseColor base_color, const wxColour &fallback);
size_t count_decompose_new_physical_filaments(const ColorDecomposeResult &result, const std::vector<std::string> &physical_colors,
                                              const std::vector<std::string> &physical_types, size_t source_physical_idx,
                                              const std::vector<size_t> *physical_config_indices = nullptr);
std::string filament_type_for_color_decompose(Preset *preset);
bool prepare_decompose_mixed_result(const ColorDecomposeResult &dialog_result, size_t source_config_idx, size_t source_physical_idx,
                                    const std::vector<std::string> &physical_colors, const std::vector<std::string> &physical_types,
                                    const std::vector<size_t> &physical_config_indices, MixedFilamentResult &mixed_result,
                                    std::vector<DecomposeMissingComponent> &missing_components);
bool confirm_create_decompose_missing_components(wxWindow *parent, const std::vector<DecomposeMissingComponent> &missing);
// the sidebar as the tip's parent
inline bool confirm_create_decompose_missing_components(class FlSidebar *, const std::vector<DecomposeMissingComponent> &missing)
{
    return confirm_create_decompose_missing_components(static_cast<wxWindow *>(nullptr), missing);
}
void set_created_standard_component_metadata(size_t config_idx, const DecomposeOfficialComponent &component);
// ColorDecomposeDialog.cpp's base colours
static const wxColour CMYW_CYAN(0, 255, 255), CMYW_MAGENTA(255, 0, 255), CMYW_YELLOW(255, 255, 0), CMYW_WHITE(255, 255, 255);
static const wxColour RYBW_RED(255, 0, 0), RYBW_YELLOW(255, 255, 0), RYBW_BLUE(0, 0, 255), RYBW_WHITE(255, 255, 255);
struct wxCommandEvent { void StopPropagation() {} };
enum { wxEVT_COMBOBOX = 1 };
#include "slic3r/GUI/Filaments.inc.cdfn"

bool has_filaments(const std::vector<std::string> &model_filaments);
bool is_support_filament(int extruder_id, bool strict_check = true);
bool is_flush_config_modified();
std::vector<int> get_min_flush_volumes(const DynamicPrintConfig &full_config, size_t nozzle_id);
#include "slic3r/GUI/Filaments.inc.fn"
#include "slic3r/GUI/Filaments.inc"
#include "slic3r/GUI/Filaments.inc.pl"
#include "slic3r/GUI/Filaments.inc.ol"
#include "slic3r/GUI/Filaments.inc.pp"
#include "slic3r/GUI/Filaments.inc.app"
using json = nlohmann::json;
#include "slic3r/GUI/Filaments.inc.cs"
#include "slic3r/GUI/Filaments.inc.cd"

ColorDecomposeDialog::ColorDecomposeDialog(void *, int filament_idx, const wxColour &target_color, const std::vector<std::string> &physical_colors,
                                           const std::vector<std::string> &filament_names, const std::vector<std::string> &filament_types,
                                           size_t current_filament_count, size_t max_filament_count, std::vector<size_t> physical_config_indices)
    : m_filament_idx(filament_idx), m_target_color(target_color), m_physical_colors(physical_colors), m_filament_names(filament_names),
      m_filament_types(filament_types), m_current_filament_count(current_filament_count), m_max_filament_count(max_filament_count),
      m_physical_config_indices(std::move(physical_config_indices))
{
    // the constructor's logic (build_ui's selector, then the first decomposition)
    for (const auto &t : m_filament_types)
        if (std::find(m_project_types.begin(), m_project_types.end(), t) == m_project_types.end()) m_project_types.push_back(t);
    if (m_filament_idx >= 0 && static_cast<size_t>(m_filament_idx) < m_filament_types.size())
        m_preferred_type = m_filament_types[m_filament_idx];
    else if (!m_project_types.empty())
        m_preferred_type = m_project_types.front();
    delete create_filament_selector();
    update_card_visibility();
    compute_decomposition();
    update_matched_color_display();
    update_ok_button_state();
}

// the app's dialog: the state, its answer (another material type, another mode, OK, Cancel) until OK or Cancel
int ColorDecomposeDialog::ShowModal()
{
    auto hex = [](const wxColour &c) { return c.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(); };
    for (;;) {
        json items = json::array();
        for (const auto &it : m_type_combo->items) items.push_back({{"label", it.label.ToStdString()}, {"group", (it.style & DD_ITEM_STYLE_SPLIT_ITEM) != 0}});
        json cards = json::array();
        const FlWidget *card_w[3] = {m_card_material_list, m_card_cmyw, m_card_rybw};
        for (size_t m = 0; m < 3; ++m) {
            json comps = json::array();
            for (const DecomposeComponent &c : m_mode_results[m].components) comps.push_back({{"color", hex(c.colour)}, {"ratio", c.ratio}});
            cards.push_back({{"mode", m}, {"visible", card_w[m]->IsShown()}, {"components", comps}});
        }
        const json r = orca::Host::ask({{"kind", "color_decompose"},
                                        {"title", _u8L("Decompose Color")},
                                        {"items", items},
                                        {"selection", m_type_combo->GetSelection()},
                                        {"target", hex(m_target_color)},
                                        {"matched", hex(m_result.matched_color)},
                                        {"arbitrary", m_arb_column_panel->IsShown()},
                                        {"mode", int(m_selected_mode)},
                                        {"cards", cards},
                                        {"hint", m_no_card_hint->IsShown() ? _u8L("At least two filaments of the same material type are required for decomposition") : ""},
                                        {"warning", m_limit_warning_panel->IsShown() ? m_limit_warning_text->label.ToStdString() : ""},
                                        {"ok", m_btn_ok->enabled},
                                        {"strings", {{"target", _u8L("Target Color")}, {"select", _u8L("Select Color Decomposition")},
                                                     {"arbitrary", _u8L("Arbitrary Mode")}, {"standard", _u8L("Standard Mode")},
                                                     {"material_list", _u8L("Material List")}, {"ok", _u8L("OK")}, {"cancel", _u8L("Cancel")}}}});
        const std::string action = r.value("action", "cancel");
        if (action == "type") {
            // the selector's wxEVT_COMBOBOX
            const int sel = r.value("selection", -1);
            m_type_combo->SetSelection(sel);
            if (sel >= 0 && size_t(sel) < m_combo_item_types.size() && !m_combo_item_types[sel].empty()) m_preferred_type = m_combo_item_types[sel];
            update_card_visibility();
            compute_decomposition();
            update_matched_color_display();
            update_ok_button_state();
        } else if (action == "mode")
            select_mode(DecomposeMode(r.value("mode", 0)));
        else if (action == "ok" && m_btn_ok->enabled)
            return wxID_OK;
        else
            return wxID_CANCEL;
    }
}

// Sidebar::update_mixed_filament_list without its panels: the mixed slots' colours follow their components (the
// blend of the physical colours), the broken-components notification, the per-plate check of a broken slot in use
void FlSidebar::update_mixed_filament_list()
{
    auto *plater = GetParent();
    auto &project_config = wxGetApp().preset_bundle->project_config;
    auto  mixed_indices  = plater->mixed_filament_config_indices();
    size_t num_physical  = p->combos_filament.size();

    auto *is_mixed_opt   = project_config.option<ConfigOptionBools>("filament_is_mixed");
    auto *components_opt = project_config.option<ConfigOptionStrings>("filament_mixed_components");
    auto *ratios_opt     = project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
    auto *colours_opt    = project_config.option<ConfigOptionStrings>("filament_colour");
    const bool has_mixed = num_physical >= 2 && !mixed_indices.empty();

    std::vector<size_t> broken_slots;
    if (is_mixed_opt && components_opt) {
        broken_slots = check_mixed_filament_integrity(is_mixed_opt->values, components_opt->values, num_physical);
        std::vector<std::string> physical_types;
        auto &preset_bundle = *wxGetApp().preset_bundle;
        for (size_t i = 0; i < num_physical; ++i) {
            std::string ft;
            if (i < preset_bundle.filament_presets.size())
                if (auto *preset = preset_bundle.filaments.find_preset(preset_bundle.filament_presets[i])) {
                    std::string display_type;
                    ft = preset->config.get_filament_type(display_type);
                }
            physical_types.push_back(ft.empty() ? "PLA" : ft);
        }
        auto mismatch = check_mixed_filament_type_consistency(is_mixed_opt->values, components_opt->values, physical_types);
        broken_slots.insert(broken_slots.end(), mismatch.begin(), mismatch.end());
    }
    std::set<size_t> broken_set(broken_slots.begin(), broken_slots.end());
    orca::Host::event("mixed_filament_broken", {{"show", has_mixed && !broken_set.empty()},
                                                 {"text", _u8L("Mixed filament has invalid or mismatched components. Please re-edit affected entries.")}});

    if (has_mixed) {
        std::vector<std::string> physical_colors;
        if (colours_opt)
            for (size_t i = 0; i < num_physical && i < colours_opt->values.size(); ++i) physical_colors.push_back(colours_opt->values[i]);
        for (size_t cfg_idx : mixed_indices) {
            std::vector<unsigned int> comp_ids = components_opt && cfg_idx < components_opt->values.size() ? parse_mixed_components(components_opt->values[cfg_idx])
                                                                                                           : std::vector<unsigned int>{};
            std::vector<int> comp_ratios;
            if (ratios_opt && cfg_idx < ratios_opt->values.size()) {
                CNumericLocalesSetter c_locale_setter;
                std::istringstream iss(ratios_opt->values[cfg_idx]);
                std::string tok;
                while (std::getline(iss, tok, ',')) {
                    float v = 0;
                    if (std::sscanf(tok.c_str(), "%f", &v) == 1) comp_ratios.push_back((int) (v * 100 + 0.5f));
                }
            }
            if (!comp_ids.empty() && comp_ratios.size() != comp_ids.size()) {
                int n = (int) comp_ids.size();
                comp_ratios.assign(n, 100 / n);
                comp_ratios[0] += 100 - (100 / n) * n;
            }
            if (!broken_set.count(cfg_idx) && !comp_ids.empty()) {
                const std::string c = blend_mixed_color(comp_ids, comp_ratios, physical_colors);
                if (colours_opt && cfg_idx < colours_opt->values.size() && colours_opt->values[cfg_idx] != c) {
                    colours_opt->values[cfg_idx] = c;
                    if (auto *mc = project_config.option<ConfigOptionStrings>("filament_multi_colour"); mc && cfg_idx < mc->values.size())
                        mc->values[cfg_idx] = c;
                }
            }
        }
    }

    size_t total = wxGetApp().preset_bundle->filament_presets.size();
    obj_list()->update_objects_list_filament_column(total);
    plater->update_filament_colors_in_full_config();

    m_mixed_filament_broken = false;
    if (!broken_slots.empty()) {
        std::set<size_t> broken_1based;
        for (size_t s : broken_slots) broken_1based.insert(s + 1);
        if (auto *curr_plate = plater->get_partplate_list().get_curr_plate())
            for (auto &obj : plater->model().objects) {
                if (!curr_plate->contain_instance_totally(obj, 0)) continue;
                int obj_ext = obj->config.has("extruder") ? obj->config.extruder() : 1;
                bool found = broken_1based.count((size_t) obj_ext) > 0;
                for (auto *vol : obj->volumes) {
                    if (found) break;
                    int vol_ext = vol->config.has("extruder") ? vol->config.extruder() : obj_ext;
                    if (broken_1based.count((size_t) vol_ext)) found = true;
                    if (vol->is_model_part() && !vol->mmu_segmentation_facets.empty())
                        for (size_t s : broken_1based)
                            if (vol->mmu_segmentation_facets.has_facets(*vol, EnforcerBlockerType(s))) { found = true; break; }
                }
                for (auto &[range, cfg] : obj->layer_config_ranges)
                    if (cfg.has("extruder")) {
                        int e = cfg.option("extruder")->getInt();
                        if (e > 0 && broken_1based.count((size_t) e)) found = true;
                    }
                if (found) { m_mixed_filament_broken = true; break; }
            }
    }
}

// ---- FlushVolCalc / WipingDialog's default volumes (is_flush_config_modified)
class FlWiping
{
public:
    using VolumeMatrix = std::vector<std::vector<double>>;
    static VolumeMatrix CalcFlushingVolumes(int extruder_id);
    static int          CalcFlushingVolume(const wxColour &from_, const wxColour &to_, int min_flush_volume, int nozzle_flush_dataset);
};
using WipingDialog = FlWiping;
#include "slic3r/GUI/Filaments.inc.wipe"
#include "slic3r/GUI/Filaments.inc.mod"

// ---- WipingDialog (the Flushing volumes dialog): its page's data and the edits it stores, the page is the app's
using json = nlohmann::json;
static const float g_min_flush_multiplier = 0.f;   // WipeTowerDialog.cpp
static const float g_max_flush_multiplier = 3.f;
class FlushDialog
{
public:
    using VolumeMatrix = std::vector<std::vector<double>>;
    static VolumeMatrix CalcFlushingVolumes(int extruder_id) { return FlWiping::CalcFlushingVolumes(extruder_id); }
    wxString            BuildTableObjStr();
    wxString            BuildTextObjStr(bool multi_language = true);
    void                StoreFlushData(int extruder_num, const std::vector<std::vector<double>> &flush_volume_vecs, const std::vector<double> &flush_multipliers);
    std::vector<double> ExpandToFullMatrix(const std::vector<double> &sub_matrix, int nozzle_idx) const;
    std::vector<double> GetFlattenMatrix() const;
    std::vector<double> GetMultipliers() const;

    int                 m_max_flush_volume{Slic3r::g_max_flush_volume};
    VolumeMatrix        m_raw_matrixs;
    std::vector<double> m_flush_multipliers;
    std::vector<size_t> m_physical_indices;
};
#include "slic3r/GUI/Filaments.inc.wdfn"
#include "slic3r/GUI/Filaments.inc.wd"

// ---- MixedFilamentDialog::ShowModal: the app's dialog edits the result
int MixedFilamentDialog::ShowModal()
{
    using orca::json;
    json comps = json::array(), ratios = json::array(), curve = json::array();
    for (unsigned int c : m_result.components) comps.push_back(c);
    for (int r : m_result.ratios) ratios.push_back(r);
    for (const GradientAnchor &a : m_result.gradient_curve)
        curve.push_back({a.x, a.y, std::isfinite(a.m_in) ? json(a.m_in) : json(nullptr), std::isfinite(a.m_out) ? json(a.m_out) : json(nullptr)});
    const json r = orca::Host::ask({{"kind", "mixed_filament"},
                                    {"title", m_title},
                                    {"colors", m_colors},
                                    {"names", m_names},
                                    {"types", m_types},
                                    {"components", comps},
                                    {"ratios", ratios},
                                    {"gradient", m_result.gradient_enabled},
                                    {"direction", m_result.gradient_direction},
                                    {"per_part", m_result.per_part_gradient},
                                    {"curve", curve}});
    if (!r.value("ok", false)) return wxID_CANCEL;
    MixedFilamentResult res;
    for (const json &c : r.value("components", json::array())) res.components.push_back(c.get<unsigned int>());
    for (const json &x : r.value("ratios", json::array())) res.ratios.push_back(x.get<int>());
    res.gradient_enabled   = r.value("gradient", false);
    res.gradient_direction = r.value("direction", 0);
    res.per_part_gradient  = r.value("per_part", false);
    for (const json &a : r.value("curve", json::array())) {
        GradientAnchor g;
        g.x     = a.at(0).get<double>();
        g.y     = a.at(1).get<double>();
        g.m_in  = a.size() > 2 && a[2].is_number() ? a[2].get<double>() : NAN;
        g.m_out = a.size() > 3 && a[3].is_number() ? a[3].get<double>() : NAN;
        res.gradient_curve.push_back(g);
    }
    m_result = std::move(res);
    return wxID_OK;
}


// ---- TextureImportDialog (Plater::priv::run_textured_mesh_import_dialog): a textured / coloured model's colours
// clustered (libslic3r TexturePainting) and mapped to filaments; the dialog's logic on stand-ins, the app draws each
// state of Host::ask "texture_import", the previews rendered here
static constexpr const char *DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE = "PLA Basic";   // TextureImportDialog.cpp
static constexpr const char *DEFAULT_VIRTUAL_FILAMENT_SHORT_TYPE = "PLA";
static constexpr const char *DEFAULT_VIRTUAL_FILAMENT_NAME       = "Bambu PLA Basic";
using wxPanel = FlWidget;
#include "slic3r/GUI/Filaments.inc.tit"
#include "slic3r/GUI/Filaments.inc.tifn"
#include "slic3r/GUI/Filaments.inc.tihas"

struct FlPlater::TextureImportResult
{
    Slic3r::PaintedMesh                  painted;
    std::vector<Slic3r::FilamentMatch>   matches;
    std::vector<std::array<float, 4>>    new_filament_colors;
    std::vector<std::string>             new_filament_preset_names;
    std::vector<TextureNewMixedFilament> new_mixed_filaments;
    std::vector<TextureFilamentEntry>    filament_entries;
    size_t                               existing_filament_count = 0;
    bool                                 skipped = false;
    bool                                 fallback_to_geometry_only = false;
    wxString                             fallback_warning;
};
using TextureImportResult = FlPlater::TextureImportResult;

// TexturePreviewCanvas: what the dialog shows -- the mesh (original or painted) with per-face colours
struct TexturePreview
{
    std::map<std::array<std::size_t, 3>, std::array<float, 3>> color_map;   // set_filament_color_map
    void set_filament_color_map(const std::map<std::array<std::size_t, 3>, std::array<float, 3>> &m) { color_map = m; }
};

class TextureImportDialog
{
public:
    TextureImportDialog(void *, const Slic3r::TexturedMesh &textured_mesh, const std::vector<TextureFilamentEntry> &filament_entries)
        : m_textured_mesh(textured_mesh), m_filament_entries(filament_entries)
    {
        // the constructor's filament lists
        for (size_t i = 0; i < m_filament_entries.size(); ++i) {
            auto &entry = m_filament_entries[i];
            entry.dialog_index = (int) i;
            entry.color_hex = texture_normalize_color_hex(entry.color_hex);
            if (entry.name.empty()) entry.name = GUI::format(_u8L("Filament %d"), i + 1);
            m_filament_color_strs.push_back(entry.color_hex);
            m_filament_names.push_back(entry.name);
            m_filament_colors_rgba.push_back(parse_color_string(entry.color_hex));
        }
        m_existing_filament_count = m_filament_colors_rgba.size();
        m_default_virtual_filament_preset_name = resolve_default_virtual_filament_preset_name();
        if (!m_textured_mesh.precomputed_face_colors.empty()) m_original_colors = m_textured_mesh.precomputed_face_colors;
        else Slic3r::sample_original_face_colors(m_textured_mesh, m_original_colors);
    }
    int  ShowModal();
    Slic3r::PaintedMesh                get_painted_mesh() const { return m_painted; }
    std::vector<Slic3r::FilamentMatch> get_matches() const { return !m_current_matches.empty() ? m_current_matches : build_matches_from_rows(); }
    bool was_skipped() const { return m_skipped; }
    bool fallback_to_geometry_only() const { return m_fallback_to_geometry_only; }
    const std::vector<std::array<float, 4>>     &get_new_filament_colors() const { return m_new_filament_colors; }
    const std::vector<std::string>              &get_new_filament_preset_names() const { return m_new_filament_preset_names; }
    const std::vector<TextureNewMixedFilament>  &get_new_mixed_filaments() const { return m_new_mixed_filaments; }
    const std::vector<TextureFilamentEntry>     &get_filament_entries() const { return m_filament_entries; }
    size_t get_existing_filament_count() const { return m_existing_filament_count; }

private:
    void sort_current_matches_by_filament_index();
    void restore_current_match_order(const std::vector<Slic3r::FilamentMatch> &previous_matches);
    size_t max_filament_count() const;
    bool   can_add_virtual_filament() const;
    int    find_closest_filament_index(const std::array<std::size_t, 3> &color) const;
    int    add_virtual_filament(const std::array<float, 4> &rgba, const std::string &hex, const std::string &preset_name = std::string());
    int    add_virtual_mixed_filament(const std::string &color_hex, const std::vector<int> &component_dialog_indices, const std::vector<int> &ratios);
    void   compact_used_virtual_filaments();
    std::vector<int> compute_display_numbers() const;
    void   apply_auto_standard_mix(TextureAutoMixMode mode);
    void   reset_auto_mix();
    bool   add_decomposed_mixed_filament(size_t row_index);
    void   do_auto_match();
    std::vector<Slic3r::FilamentMatch> build_matches_from_rows() const;
    bool   has_valid_result() const;
    bool   is_params_dirty() const;
    void   update_filament_color_map();
    // the dialog's UI updates: the app redraws from the next ask
    void rebuild_mapping_rows();
    void dismiss_auto_mix_popup() {}
    void update_drop_warning_visibility() {}
    void update_auto_mix_reset_visibility() {}
    // start_computation + on_computation_complete / _error, on the engine thread with progress events
    bool compute(bool auto_color, bool initial);
    std::string render_preview(int mode, double yaw, double pitch, int w, int h) const;

    Slic3r::TexturedMesh               m_textured_mesh;
    std::vector<std::string>           m_filament_color_strs, m_filament_names;
    std::vector<std::array<float, 4>>  m_filament_colors_rgba;
    std::vector<TextureFilamentEntry>  m_filament_entries;
    size_t                             m_existing_filament_count = 0;
    std::vector<std::array<float, 4>>  m_new_filament_colors;
    std::vector<std::string>           m_new_filament_preset_names;
    std::vector<TextureNewMixedFilament> m_new_mixed_filaments;
    std::string                        m_default_virtual_filament_preset_name;
    TextureImportState                 m_state = TextureImportState::Idle;
    bool                               m_skipped = false, m_fallback_to_geometry_only = false, m_filaments_dropped = false;
    bool                               m_auto_mix_applied = false;
    TextureAutoMixMode                 m_auto_mix_mode = TextureAutoMixMode::CMYW;
    Slic3r::PaintedMesh                m_painted;
    std::vector<Slic3r::FilamentMatch> m_current_matches;
    std::vector<FilamentMappingRow>    m_mapping_rows;
    std::vector<std::array<std::size_t, 3>> m_original_colors;
    Slic3r::TexturePaintingSettings::MeshRepairDecision m_mesh_repair_decision = Slic3r::TexturePaintingSettings::MeshRepairDecision::Ask;
    int  m_param_color_count = 4, m_param_smooth = 5, m_applied_color_count = -1, m_applied_smooth = -1;
    // the auto-merge checkbox (wxCheckBox, on by default)
    struct MergeBox { bool value{true}; bool GetValue() const { return value; } } m_merge_box;
    MergeBox *m_auto_merge_cb{&m_merge_box};
    TexturePreview  m_preview;
    TexturePreview *m_preview_canvas{&m_preview};
    int             m_view_mode = 2;   // TexturePreviewCanvas::RenderMode: Original 0, MultiColor 1, FilamentMap 2
    double          m_yaw = 0.6, m_pitch = 0.5;
};
#include "slic3r/GUI/Filaments.inc.ti"

// rebuild_mapping_rows: one row per match (the rows' widgets are the app's)
void TextureImportDialog::rebuild_mapping_rows()
{
    m_mapping_rows.clear();
    m_mapping_rows.resize(m_current_matches.size());
    for (size_t ci = 0; ci < m_current_matches.size(); ++ci) {
        auto &row = m_mapping_rows[ci];
        row.cluster_id = m_current_matches[ci].cluster_index;
        row.source_color = m_current_matches[ci].cluster_color;
        row.source_hex = rgb_to_hex(row.source_color).ToStdString();
        row.target_filament_idx = m_current_matches[ci].filament_index;
    }
}

static std::atomic<bool> g_texture_cancel{false};

bool TextureImportDialog::compute(bool auto_color, bool initial)
{
    for (;;) {
        g_texture_cancel = false;
        m_state = TextureImportState::Computing;
        Slic3r::TexturePaintingSettings settings;
        settings.target_colors_num    = auto_color ? 0 : (size_t) m_param_color_count;
        settings.smooth_weight        = m_param_smooth / 10.0;
        settings.mesh_repair_decision = m_mesh_repair_decision;
        settings.mesh_repair_callback = [](const indexed_triangle_set &mesh, indexed_triangle_set &repaired_mesh,
                                           std::function<void(const char *, unsigned)> progress_callback, std::function<bool()> cancel_callback,
                                           std::string *error_message) -> bool {
            struct Canceled {};
            TriangleMesh tm(mesh);
            try {
                if (!repair_mesh(tm, nullptr, error_message, [&cancel_callback](int) {
                        if (cancel_callback && cancel_callback()) throw Canceled();
                    }))
                    return false;
            } catch (const Canceled &) {
                return false;
            }
            repaired_mesh = tm.its;
            return true;
        };
        bool repair_required = false;
        settings.mesh_repair_decision_required = &repair_required;
        auto progress_cb = [](int percent, const char *) {
            orca::Host::event("texture_import_progress", {{"percent", percent}, {"text", _u8L("Computing texture colors...")}});
        };
        auto cancel_cb = []() { return g_texture_cancel.load(); };
        progress_cb(0, nullptr);
        Slic3r::PaintedMesh result;
        const bool ok = !m_textured_mesh.precomputed_face_colors.empty()
                            ? Slic3r::face_colors_to_painting(m_textured_mesh, result, settings, progress_cb, cancel_cb)
                            : Slic3r::texture_to_painting(m_textured_mesh, result, settings, progress_cb, cancel_cb);
        orca::Host::event("texture_import_progress", {{"done", true}});
        if (g_texture_cancel) {   // on_computation_error, cancelled
            m_state = has_valid_result() ? TextureImportState::Ready : TextureImportState::Idle;
            if (has_valid_result() && m_applied_color_count >= 0) { m_param_color_count = m_applied_color_count; m_param_smooth = m_applied_smooth; }
            return !initial;
        }
        if (!ok && repair_required) {   // on_mesh_repair_decision_required
            MessageDialog dlg(nullptr, _L("The mesh has non-manifold geometry or open boundaries. You can import it as-is or repair it before importing."),
                              _L("Mesh repair"), wxYES_NO | wxCANCEL | wxICON_WARNING | wxYES_DEFAULT);
            dlg.SetButtonLabel(wxID_YES, _L("Import without repair"));
            dlg.SetButtonLabel(wxID_NO, _L("Repair and import"), true);
            dlg.SetButtonLabel(wxID_CANCEL, _L("Cancel"));
            const int ret = dlg.ShowModal();
            if (ret == wxID_CANCEL) {
                m_state = has_valid_result() ? TextureImportState::Ready : TextureImportState::Idle;
                return !initial;
            }
            m_mesh_repair_decision = ret == wxID_NO ? Slic3r::TexturePaintingSettings::MeshRepairDecision::RepairAndImport
                                                    : Slic3r::TexturePaintingSettings::MeshRepairDecision::ImportWithoutRepair;
            continue;
        }
        if (!ok) {   // on_computation_error
            if (initial) { m_fallback_to_geometry_only = true; return false; }
            m_state = TextureImportState::Error;
            MessageDialog(nullptr, _L("Computation failed. Please adjust parameters and retry."), _L("Error"), wxOK | wxICON_ERROR).ShowModal();
            return true;
        }
        // on_computation_complete
        m_painted = std::move(result);
        const int actual_colors = (int) m_painted.cluster_colors.size();
        if (actual_colors >= 2 && actual_colors <= (int) max_filament_count()) m_param_color_count = std::clamp(actual_colors, 1, (int) max_filament_count());
        m_current_matches.clear();
        if (m_filament_colors_rgba.size() > m_existing_filament_count) m_filament_colors_rgba.resize(m_existing_filament_count);
        if (m_filament_color_strs.size() > m_existing_filament_count) m_filament_color_strs.resize(m_existing_filament_count);
        if (m_filament_names.size() > m_existing_filament_count) m_filament_names.resize(m_existing_filament_count);
        if (m_filament_entries.size() > m_existing_filament_count) m_filament_entries.resize(m_existing_filament_count);
        for (size_t i = 0; i < m_filament_entries.size(); ++i) {
            m_filament_entries[i].dialog_index = (int) i;
            m_filament_entries[i].color_hex = i < m_filament_color_strs.size() ? texture_normalize_color_hex(m_filament_color_strs[i]) : "#808080";
            m_filament_entries[i].name = i < m_filament_names.size() ? m_filament_names[i] : GUI::format(_u8L("Filament %d"), i + 1);
        }
        m_new_filament_colors.clear();
        m_new_filament_preset_names.clear();
        m_new_mixed_filaments.clear();
        do_auto_match();
        compact_used_virtual_filaments();
        sort_current_matches_by_filament_index();
        update_filament_color_map();
        rebuild_mapping_rows();
        m_applied_color_count = m_param_color_count;
        m_applied_smooth      = m_param_smooth;
        m_state = TextureImportState::Ready;
        m_view_mode = 2;
        return true;
    }
}

// TexturePreviewCanvas::render: the mesh lit from the camera, each face in its colour (original, clusters or the
// mapped filament), orthographic and fit to the view; written as a PNG for the app
std::string TextureImportDialog::render_preview(int mode, double yaw, double pitch, int W, int H) const
{
    const bool painted = mode != 0 && !m_painted.indices.empty();
    const auto &verts = painted ? m_painted.vertices : m_textured_mesh.vertices;
    const auto &tris  = painted ? m_painted.indices : m_textured_mesh.indices;
    if (verts.empty() || tris.empty()) return {};
    auto face_rgb = [&](size_t f) -> std::array<float, 3> {
        if (!painted) {
            if (f < m_original_colors.size()) return {m_original_colors[f][0] / 255.f, m_original_colors[f][1] / 255.f, m_original_colors[f][2] / 255.f};
            return {0.7f, 0.7f, 0.7f};
        }
        const auto &c = f < m_painted.face_colors.size() ? m_painted.face_colors[f] : std::array<std::size_t, 3>{180, 180, 180};
        if (mode == 2)
            if (auto it = m_preview.color_map.find(c); it != m_preview.color_map.end()) return it->second;
        return {c[0] / 255.f, c[1] / 255.f, c[2] / 255.f};
    };
    // camera: yaw around Z, pitch above the horizon; screen x right, y up, depth toward the viewer
    const Vec3d fwd(std::cos(pitch) * std::sin(yaw), -std::cos(pitch) * std::cos(yaw), std::sin(pitch));   // toward the viewer
    Vec3d right = Vec3d::UnitZ().cross(fwd);
    if (right.norm() < 1e-6) right = Vec3d::UnitX();
    right.normalize();
    const Vec3d up = fwd.cross(right).normalized();
    BoundingBoxf3 bb;
    for (const auto &v : verts) bb.merge(Vec3d(v[0], v[1], v[2]));
    const Vec3d  c = bb.center();
    const double r = std::max(1e-6, bb.radius());
    const double scale = 0.46 * std::min(W, H) / r;
    std::vector<Vec3d> p(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        const Vec3d v = Vec3d(verts[i][0], verts[i][1], verts[i][2]) - c;
        p[i] = Vec3d(W / 2.0 + v.dot(right) * scale, H / 2.0 - v.dot(up) * scale, v.dot(fwd));
    }
    std::vector<uint8_t> rgb(size_t(W) * H * 3);
    const bool dark = orca::Core::get().app_config->get("dark_color_mode") == "1";
    // preview_bg of the dialog: #EEEEEE (dark #4C4C55)
    for (size_t i = 0; i < size_t(W) * H; ++i) { rgb[i * 3] = dark ? 0x4C : 0xEE; rgb[i * 3 + 1] = dark ? 0x4C : 0xEE; rgb[i * 3 + 2] = dark ? 0x55 : 0xEE; }
    std::vector<float> depth(size_t(W) * H, -std::numeric_limits<float>::max());
    for (size_t f = 0; f < tris.size(); ++f) {
        const auto &t = tris[f];
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || size_t(t[0]) >= p.size() || size_t(t[1]) >= p.size() || size_t(t[2]) >= p.size()) continue;
        const Vec3d &a = p[t[0]], &b = p[t[1]], &d = p[t[2]];
        const Vec3d wa(verts[t[0]][0], verts[t[0]][1], verts[t[0]][2]), wb(verts[t[1]][0], verts[t[1]][1], verts[t[1]][2]),
            wd(verts[t[2]][0], verts[t[2]][1], verts[t[2]][2]);
        Vec3d n = (wb - wa).cross(wd - wa);
        if (n.norm() < 1e-12) continue;
        n.normalize();
        const double light = 0.35 + 0.65 * std::abs(n.dot(fwd));
        const auto col = face_rgb(f);
        const uint8_t R = uint8_t(std::clamp(col[0] * light, 0.0, 1.0) * 255), G = uint8_t(std::clamp(col[1] * light, 0.0, 1.0) * 255),
                      B = uint8_t(std::clamp(col[2] * light, 0.0, 1.0) * 255);
        const int minx = std::max(0, int(std::floor(std::min({a.x(), b.x(), d.x()})))), maxx = std::min(W - 1, int(std::ceil(std::max({a.x(), b.x(), d.x()}))));
        const int miny = std::max(0, int(std::floor(std::min({a.y(), b.y(), d.y()})))), maxy = std::min(H - 1, int(std::ceil(std::max({a.y(), b.y(), d.y()}))));
        const double area = (b.x() - a.x()) * (d.y() - a.y()) - (b.y() - a.y()) * (d.x() - a.x());
        if (std::abs(area) < 1e-12) continue;
        for (int y = miny; y <= maxy; ++y)
            for (int x = minx; x <= maxx; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double w0 = ((b.x() - px) * (d.y() - py) - (b.y() - py) * (d.x() - px)) / area;
                const double w1 = ((d.x() - px) * (a.y() - py) - (d.y() - py) * (a.x() - px)) / area;
                const double w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = float(w0 * a.z() + w1 * b.z() + w2 * d.z());
                float &zb = depth[size_t(y) * W + x];
                if (z <= zb) continue;
                zb = z;
                uint8_t *o = &rgb[(size_t(y) * W + x) * 3];
                o[0] = R; o[1] = G; o[2] = B;
            }
    }
    static int counter = 0;
    const std::string path = (boost::filesystem::temp_directory_path() / ("texture_preview_" + std::to_string(++counter % 4) + ".png")).string();
    return Slic3r::png::write_rgb_to_file(path, size_t(W), size_t(H), rgb) ? path : std::string();
}

int TextureImportDialog::ShowModal()
{
    if (m_state == TextureImportState::Idle && m_painted.face_colors.empty())
        if (!compute(true, true)) return wxID_CANCEL;
    for (;;) {
        const std::vector<int> numbers = compute_display_numbers();
        auto number = [&](int idx) { return idx >= 0 && idx < (int) numbers.size() && numbers[idx] > 0 ? numbers[idx] : idx + 1; };
        json filaments = json::array();
        for (const auto &e : m_filament_entries) {
            json comps = json::array();
            for (unsigned int k : e.mixed_components)
                if (k >= 1 && k <= m_filament_entries.size()) comps.push_back({{"color", m_filament_entries[k - 1].color_hex}, {"number", number(int(k) - 1)}});
            filaments.push_back({{"index", e.dialog_index}, {"number", number(e.dialog_index)}, {"name", e.name}, {"color", e.color_hex},
                                 {"kind", int(e.kind)}, {"components", comps}, {"ratios", e.mixed_ratios}});
        }
        json rows = json::array();
        for (size_t i = 0; i < m_mapping_rows.size(); ++i) {
            const auto &row = m_mapping_rows[i];
            const int   t   = row.target_filament_idx;
            rows.push_back({{"source", row.source_hex}, {"target", t},
                            {"delta_e", i < m_current_matches.size() ? m_current_matches[i].delta_e : 0.0}});
        }
        const bool ready = m_state == TextureImportState::Ready, valid = has_valid_result();
        const std::string image = render_preview(m_view_mode, m_yaw, m_pitch, 480, 400);
        const json r = orca::Host::ask(
            {{"kind", "texture_import"},
             {"title", _u8L("Import Model")},
             {"image", image},
             {"view", m_view_mode},
             {"color_count", m_param_color_count},
             {"max_colors", max_filament_count()},
             {"smooth", m_param_smooth},
             {"auto_merge", m_merge_box.value},
             {"mix_mode", int(m_auto_mix_mode)},
             {"mix_applied", m_auto_mix_applied},
             {"dirty", ready && valid && is_params_dirty()},
             {"ok", ready && valid && !is_params_dirty()},
             {"dropped", ready && m_filaments_dropped},
             {"can_add", can_add_virtual_filament()},
             {"rows", rows},
             {"filaments", filaments},
             {"strings",
              {{"original", _u8L("Original")}, {"multicolor", _u8L("Multi-Color")}, {"color_count", _u8L("Color Count")}, {"auto", _u8L("Auto")},
               {"smooth", _u8L("Smooth Level")}, {"apply", _u8L("Apply")}, {"apply_tip", _u8L("Convert texture to painting using the specified color count and smooth level")},
               {"auto_tip", _u8L("Automatically determine the optimal color count only and recompute filament mapping")},
               {"reminder", _u8L("Reminder: parameters changed, click Apply to take effect")}, {"mapping", _u8L("Filament Mapping")},
               {"reset_mix", _u8L("Reset filament mapping to the state before one-click mixing")},
               {"cmyw", _u8L("One-click CMYW auto-mix")}, {"rybw", _u8L("One-click RYBW auto-mix")},
               {"auto_merge", _u8L("Auto-merge same filament")}, {"auto_merge_tip", _u8L("Automatically merge identical filaments into existing filaments in the project")},
               {"skip", _u8L("Skip Matching")}, {"skip_tip", _u8L("Skip filament mapping and import as a single-color model")}, {"ok", _u8L("Confirm")},
               {"physical", _u8L("Project Physical Filaments")}, {"new_physical", _u8L("New Physical Filaments")},
               {"mixed", _u8L("Project Mixed Filaments")}, {"new_mixed", _u8L("New Mixed Filaments")}, {"decompose", _u8L("Decompose Color")},
               {"add", _u8L("+ Add Material")},
               {"dropped", format(_L_PLURAL("The project supports up to %d filament. Extra filaments will be discarded.",
                                            "The project supports up to %d filaments. Extra filaments will be discarded.", max_filament_count()),
                                  max_filament_count())}}}});
        const std::string a = r.value("action", "cancel");
        if (a == "color_count") m_param_color_count = std::clamp(r.value("value", 4), 1, (int) max_filament_count());
        else if (a == "smooth") m_param_smooth = std::clamp(r.value("value", 5), 0, 10);
        else if (a == "apply") compute(false, false);
        else if (a == "auto") compute(true, false);
        else if (a == "auto_merge") {   // on_auto_merge_toggled
            m_merge_box.value = r.value("value", true);
            if (m_state == TextureImportState::Ready) {
                const auto previous = m_current_matches;
                do_auto_match();
                restore_current_match_order(previous);
                compact_used_virtual_filaments();
                update_filament_color_map();
                rebuild_mapping_rows();
            }
        } else if (a == "auto_mix") {   // set_auto_mix_mode
            m_auto_mix_mode = TextureAutoMixMode(r.value("mode", 0));
            apply_auto_standard_mix(m_auto_mix_mode);
        } else if (a == "reset_mix") reset_auto_mix();
        else if (a == "select") {   // FilamentSelectPopup's on_select
            const size_t row = r.value("row", 0);
            const int    idx = r.value("index", -1);
            if (row < m_mapping_rows.size()) {
                m_mapping_rows[row].target_filament_idx = idx;
                if (row < m_current_matches.size()) m_current_matches[row].filament_index = idx;
                update_filament_color_map();
            }
        } else if (a == "add") {   // on_add_filament: a colour picked for the row
            const size_t row = r.value("row", 0);
            const wxColour clr(r.value("color", std::string("#808080")));
            const std::array<float, 4> rgba = {clr.Red() / 255.f, clr.Green() / 255.f, clr.Blue() / 255.f, 1.0f};
            const int new_idx = add_virtual_filament(rgba, clr.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
            if (new_idx >= 0 && row < m_mapping_rows.size()) {
                m_mapping_rows[row].target_filament_idx = new_idx;
                if (row < m_current_matches.size()) m_current_matches[row].filament_index = new_idx;
                rebuild_mapping_rows();
                update_filament_color_map();
            }
        } else if (a == "decompose") add_decomposed_mixed_filament(r.value("row", 0));
        else if (a == "view") {
            m_view_mode = r.value("mode", m_view_mode);
            m_yaw       = r.value("yaw", m_yaw);
            m_pitch     = std::clamp(r.value("pitch", m_pitch), -1.5, 1.5);
        } else if (a == "skip") {   // on_skip_clicked
            m_skipped = true;
            m_new_filament_colors.clear();
            m_new_filament_preset_names.clear();
            m_new_mixed_filaments.clear();
            m_current_matches.clear();
            return wxID_CANCEL;
        } else if (a == "ok") {   // on_ok_clicked
            if (m_state != TextureImportState::Ready || !has_valid_result() || is_params_dirty()) continue;
            m_current_matches = build_matches_from_rows();
            if (m_current_matches.empty()) continue;
            compact_used_virtual_filaments();
            return wxID_OK;
        } else
            return wxID_CANCEL;
    }
}

#include "slic3r/GUI/Filaments.inc.tiapply"

}}} // namespace Slic3r::GUI::fil

namespace orca {
using namespace Slic3r;
using namespace Slic3r::GUI::fil;

// the filament section of the sidebar: the physical slots (presets_state "filaments" lists them all), the mixed ones,
// which buttons upstream shows (show_SEMM_buttons, the purge mode button), the flushing volumes' modified mark
json filaments_state()
{
    PresetBundle &b = *Core::get().preset_bundle;
    const auto *is_mixed = b.project_config.option<ConfigOptionBools>("filament_is_mixed");
    const auto *comps    = b.project_config.option<ConfigOptionStrings>("filament_mixed_components");
    const auto *ratios   = b.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
    const auto *colours  = b.project_config.option<ConfigOptionStrings>("filament_colour");
    const auto *grad     = b.project_config.option<ConfigOptionBools>("filament_mixed_gradient");
    json physical = json::array(), mixed = json::array();
    size_t n_physical = 0;
    for (size_t i = 0; i < b.filament_presets.size(); ++i) {
        if (is_mixed && i < is_mixed->values.size() && is_mixed->values[i]) continue;
        physical.push_back(i);
        ++n_physical;
    }
    std::vector<size_t> broken;
    if (is_mixed && comps) {
        broken = check_mixed_filament_integrity(is_mixed->values, comps->values, n_physical);
        std::vector<std::string> types;
        for (size_t i = 0; i < n_physical; ++i) {
            std::string ft, display;
            if (i < b.filament_presets.size())
                if (auto *preset = b.filaments.find_preset(b.filament_presets[i])) ft = preset->config.get_filament_type(display);
            types.push_back(ft.empty() ? "PLA" : ft);
        }
        auto mismatch = check_mixed_filament_type_consistency(is_mixed->values, comps->values, types);
        broken.insert(broken.end(), mismatch.begin(), mismatch.end());
    }
    for (size_t i = 0; i < b.filament_presets.size(); ++i) {
        if (!is_mixed || i >= is_mixed->values.size() || !is_mixed->values[i]) continue;
        json m = {{"index", i},
                  {"color", colours && i < colours->values.size() ? colours->values[i] : "#888888"},
                  {"components", comps && i < comps->values.size() ? json(parse_mixed_components(comps->values[i])) : json::array()},
                  {"ratios", ratios && i < ratios->values.size() ? json(parse_mixed_ratios(ratios->values[i], comps && i < comps->values.size() ? parse_mixed_components(comps->values[i]).size() : 0)) : json::array()},
                  {"gradient", grad && i < grad->values.size() && grad->values[i]},
                  {"broken", std::find(broken.begin(), broken.end(), i) != broken.end()}};
        // the swatch's fade over the model's height (mixed_gradient_ramp): bottom to top
        if (m["gradient"].get<bool>() && m["components"].size() == 2) {
            const auto curve = [&] {
                const auto *curve_opt = b.project_config.option<ConfigOptionStrings>("filament_mixed_gradient_curve");
                if (curve_opt && i < curve_opt->values.size() && !curve_opt->values[i].empty()) {
                    GradientCurve c = parse_gradient_curve(curve_opt->values[i]);
                    if (c.points.size() >= 2) return c;
                }
                double start = kGradientMinRatio, end = kGradientMaxRatio;
                const auto *range_opt = b.project_config.option<ConfigOptionStrings>("filament_mixed_gradient_range");
                if (range_opt && i < range_opt->values.size() && !range_opt->values[i].empty()) {
                    CNumericLocalesSetter c_locale_setter;
                    float v0 = 0, v1 = 0;
                    if (std::sscanf(range_opt->values[i].c_str(), "%f,%f", &v0, &v1) == 2 && v0 > 0 && v0 < 1 && v1 > 0 && v1 < 1) {
                        start = v0;
                        end   = v1;
                    }
                }
                GradientCurve c;
                c.points = {{0.0, start, NAN, NAN}, {1.0, end, NAN, NAN}};
                return c;
            }();
            const auto ids = parse_mixed_components(comps->values[i]);
            auto colour = [&](unsigned int id) { return id >= 1 && colours && id <= colours->values.size() ? colours->values[id - 1] : std::string("#D9D9D9"); };
            json ramp = json::array();
            for (int s = 0; s < 24; ++s) {
                const double r1 = sample_gradient_curve(curve, (s + 0.5) / 24);
                ramp.push_back(blend_n({colour(ids[0]), colour(ids[1])}, {r1, 1.0 - r1}));
            }
            m["ramp"] = ramp;
        }
        mixed.push_back(std::move(m));
    }
    // TabPrinter::update: the purge mode button for printers with several nozzles per extruder or fast purge, the mode
    // normalised to what the printer supports
    const Preset &printer = b.printers.get_edited_preset();
    const auto *fast_purge = printer.config.option<ConfigOptionBool>("support_fast_purge_mode");
    const auto *max_nozzles = printer.config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    const bool has_multiple_nozzle = max_nozzles && std::any_of(max_nozzles->values.begin(), max_nozzles->values.end(),
                                                                [](int v) { return v > 1 && v != ConfigOptionIntsNullable::nil_value(); });
    const bool support_fast_purge = fast_purge && fast_purge->value;
    const bool show_purge_mode    = has_multiple_nozzle || support_fast_purge;
    if (auto *pvm = b.project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode")) {
        if (!show_purge_mode) pvm->value = PrimeVolumeMode::pvmDefault;
        else if (!has_multiple_nozzle && pvm->value == PrimeVolumeMode::pvmSaving) pvm->value = PrimeVolumeMode::pvmDefault;
        else if (!support_fast_purge && pvm->value == PrimeVolumeMode::pvmFast) pvm->value = PrimeVolumeMode::pvmDefault;
    }
    return {{"physical", physical},
            {"mixed", mixed},
            {"semm", FlSidebar::should_show_SEMM_buttons()},
            {"at_limit", b.filament_presets.size() >= size_t(EnforcerBlockerType::ExtruderMax)},
            {"flush_modified", n_physical > 1 && is_flush_config_modified()},
            {"purge_mode", show_purge_mode}};
}

// every command: the upstream action on a fresh stand-in plater, then what follows a preset change
template<class F> static json with_sidebar(F &&f)
{
    FlPlater plater;
    wxGetApp().m_plater = &plater;
    f(plater.sidebar());
    wxGetApp().m_plater = nullptr;
    presets_changed();
    ++Plater::get().revision;
    return {{"presets", presets_state()}, {"project", Plater::get().state()}};
}

// Plater::priv::on_filament_color_changed / on_select_preset: the flushing volumes of a slot (-1: all) are recalculated,
// the mixed slots' colours follow
void filaments_auto_flush(int idx)
{
    FlPlater plater;
    wxGetApp().m_plater = &plater;
    plater.sidebar().auto_calc_flushing_volumes(idx);
    plater.sidebar().update_mixed_filament_list();
    wxGetApp().m_plater = nullptr;
}
// Sidebar::ensure_mixed_filament for the texture displacement bake (TextureDisp.cpp): the slot printing a blend,
// created when the project has none
int filaments_ensure_mixed(const std::vector<unsigned int> &components, const std::vector<int> &ratios)
{
    FlPlater plater;
    wxGetApp().m_plater = &plater;
    const size_t before = Core::get().preset_bundle->filament_presets.size();
    const int slot = plater.sidebar().ensure_mixed_filament(components, ratios);
    wxGetApp().m_plater = nullptr;
    if (Core::get().preset_bundle->filament_presets.size() != before) presets_changed();
    return slot;
}
void filaments_refresh_mixed()
{
    FlPlater plater;
    wxGetApp().m_plater = &plater;
    plater.sidebar().update_mixed_filament_list();
    wxGetApp().m_plater = nullptr;
}

static Commands reg({
    // the sidebar's "+" (Sidebar::add_filament: a physical slot before the mixed ones) and "-" (delete_filament: the last)
    {"filaments.add", [](const json &) { return with_sidebar([](FlSidebar &s) { s.add_filament(); }); }},
    {"filaments.delete", [](const json &a) {
         return with_sidebar([&](FlSidebar &s) { s.delete_filament(a.contains("index") ? a["index"].get<size_t>() : size_t(-1), a.value("replace", -1)); });
     }},
    // the filament menu's "Merge with" (Sidebar::change_filament)
    {"filaments.merge", [](const json &a) { return with_sidebar([&](FlSidebar &s) { s.change_filament(a.at("from"), a.at("to")); }); }},
    // a colour change of a slot: the flushing volumes follow (auto_calculate_flush)
    {"filaments.auto_flush", [](const json &a) { return with_sidebar([&](FlSidebar &s) { s.auto_calc_flushing_volumes(a.value("index", -1)); }); }},
    {"mixed.add", [](const json &) { return with_sidebar([](FlSidebar &s) { s.add_mixed_filament(); }); }},
    {"mixed.edit", [](const json &a) { return with_sidebar([&](FlSidebar &s) { s.edit_mixed_filament(a.at("index")); }); }},
    {"mixed.delete", [](const json &a) { return with_sidebar([&](FlSidebar &s) { s.delete_mixed_filament_at(a.at("index")); }); }},
    {"mixed.delete_last", [](const json &) {
         return with_sidebar([](FlSidebar &s) {
             auto idx = s.GetParent()->mixed_filament_config_indices();
             if (!idx.empty()) s.delete_mixed_filament_at(idx.size() - 1);
         });
     }},
    {"mixed.remove_all", [](const json &) { return with_sidebar([](FlSidebar &s) { s.remove_all_mixed_filaments(); }); }},
    // the filament menu's "Decompose Color" (Sidebar::decompose_filament_color)
    {"filaments.decompose", [](const json &a) { return with_sidebar([&](FlSidebar &s) { s.decompose_filament_color(a.at("index").get<int>()); }); }},
    {"filaments.state", [](const json &) { return filaments_state(); }},
    // PurgeModeDialog: Standard and Fast (support_fast_purge_mode) or Prime Saving (several nozzles), the mode of the project
    {"purge_mode.get", [](const json &) -> json {
         PresetBundle &b = *Core::get().preset_bundle;
         const auto *fast = b.printers.get_edited_preset().config.option<ConfigOptionBool>("support_fast_purge_mode");
         const bool  is_fast_mode = fast && fast->value;
         const auto *cur = b.project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode");
         const PrimeVolumeMode mode = cur ? cur->value : pvmDefault;
         return {{"title", _u8L("Purge Mode Settings")},
                 {"options", json::array({{{"mode", int(pvmDefault)}, {"title", _u8L_CONTEXT("Standard", "Mode")}, {"icon", "shield"},
                                           {"text", _u8L("Perform full purging with speed and temperature transition for the best print quality.")}},
                                          is_fast_mode ? json{{"mode", int(pvmFast)}, {"title", _u8L("Fast")}, {"icon", "leaf"},
                                                              {"text", _u8L("Uses optimized purge temperature, multiplier and stronger extrusion for faster purging. May cause slight color mixing in some extreme cases.")}}
                                                       : json{{"mode", int(pvmSaving)}, {"title", _u8L("Prime Saving")}, {"icon", "leaf"},
                                                              {"text", _u8L("Reduces prime waste and prints faster. May cause slight color mixing or small surface defects.")}}})},
                 {"mode", int(mode)},
                 {"confirm", _u8L("Confirm")},
                 {"cancel", _u8L("Cancel")}};
     }},
    {"purge_mode.set", [](const json &a) -> json {
         Core::get().preset_bundle->project_config.set_key_value("prime_volume_mode", new ConfigOptionEnum<PrimeVolumeMode>(PrimeVolumeMode(a.at("mode").get<int>())));
         presets_changed();
         return presets_state();
     }},
});

// ---- the Flushing volumes dialog (WebDialogs.cpp "flushing"): the messages of resources/web/flush/WipingDialog.html
static std::unique_ptr<FlushDialog> g_flush;
// "init": the table and the texts (buildTable / buildText)
json flushing_init()
{
    g_flush = std::make_unique<FlushDialog>();
    FlPlater plater;
    wxGetApp().m_plater = &plater;
    json r = {{"table", json::parse(g_flush->BuildTableObjStr().ToStdString())}, {"text", json::parse(g_flush->BuildTextObjStr(true).ToStdString())}};
    wxGetApp().m_plater = nullptr;
    return r;
}
// "updateMatrix": Re-calculate of one extruder
json flushing_update(int extruder_id)
{
    FlPlater plater;
    wxGetApp().m_plater = &plater;
    json r = MatrixFlatten(FlushDialog::CalcFlushingVolumes(extruder_id));
    wxGetApp().m_plater = nullptr;
    return r;
}
// "storeData": the edited volumes clamped by the multipliers, then open_flushing_dialog's submit
void flushing_store(const json &j)
{
    if (!g_flush) return;
    int extruder_num = j["number_of_extruders"].get<int>();
    std::vector<std::vector<double>> store_matrixs;
    for (auto iter = j["raw_matrix"].begin(); iter != j["raw_matrix"].end(); ++iter) store_matrixs.emplace_back((*iter).get<std::vector<double>>());
    std::vector<double> store_multipliers = j["flush_multiplier"].get<std::vector<double>>();
    size_t cols_temp_matrix = store_matrixs.empty() ? 0 : store_matrixs[0].size();
    if (store_multipliers.size() == store_matrixs.size() && cols_temp_matrix > 0)
        for (size_t idx = 0; idx < store_multipliers.size(); ++idx) {
            double m_max_flush_volume_t = (double) g_flush->m_max_flush_volume, m_store_multipliers = store_multipliers[idx];
            std::transform(store_matrixs[idx].begin(), store_matrixs[idx].end(), store_matrixs[idx].begin(),
                           [m_max_flush_volume_t, m_store_multipliers](double inputx) { return std::clamp(inputx, 0.0, m_max_flush_volume_t / m_store_multipliers); });
        }
    g_flush->StoreFlushData(extruder_num, store_matrixs, store_multipliers);
    auto &project_config = Core::get().preset_bundle->project_config;
    auto matrix = g_flush->GetFlattenMatrix();
    auto flush_multipliers = g_flush->GetMultipliers();
    project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = std::vector<double>(matrix.begin(), matrix.end());
    project_config.option<ConfigOptionFloats>("flush_multiplier")->values = std::vector<double>(flush_multipliers.begin(), flush_multipliers.end());
    g_flush.reset();
    presets_changed();
}

// Plater::priv::load_files' textured model step (run_textured_mesh_import_dialog, then
// apply_textured_mesh_import_result): false when the user cancelled the import
bool texture_import(Model &model)
{
    if (!model.texture_mesh || !has_importable_texture(*model.texture_mesh)) return true;
    if (model.objects.empty()) { model.texture_mesh.reset(); return true; }
    const wxString fallback_warning = _L("Texture import failed. The model appears to contain texture data, but the texture import process could not be completed. The model will be imported as geometry only.");
    FlPlater plater;
    wxGetApp().m_plater = &plater;
    std::vector<TextureFilamentEntry> filament_entries;
    {
        auto &preset_bundle  = *wxGetApp().preset_bundle;
        auto &project_config = preset_bundle.project_config;
        auto *colours_opt    = project_config.option<ConfigOptionStrings>("filament_colour");
        auto *is_mixed_opt   = project_config.option<ConfigOptionBools>("filament_is_mixed");
        auto *type_opt       = project_config.option<ConfigOptionStrings>("filament_type");
        auto *components_opt = project_config.option<ConfigOptionStrings>("filament_mixed_components");
        auto *ratios_opt     = project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
        const size_t total = preset_bundle.filament_presets.size();
        for (size_t i = 0; i < total; ++i) {
            TextureFilamentEntry entry;
            entry.kind = (is_mixed_opt && i < is_mixed_opt->values.size() && is_mixed_opt->values[i]) ? TextureFilamentKind::ExistingMixed
                                                                                                         : TextureFilamentKind::ExistingPhysical;
            entry.dialog_index = (int) filament_entries.size();
            entry.project_config_index = i;
            entry.color_hex = (colours_opt && i < colours_opt->values.size()) ? colours_opt->values[i] : "#808080";
            entry.type = (type_opt && i < type_opt->values.size()) ? type_opt->values[i] : "";
            std::string name;
            if (auto *preset = preset_bundle.filaments.find_preset(preset_bundle.filament_presets[i])) name = preset->label(false);
            if (name.empty()) name = GUI::format(_u8L("Filament %d"), int(i + 1));
            entry.name = name;
            if (entry.kind == TextureFilamentKind::ExistingMixed) {
                if (components_opt && i < components_opt->values.size()) entry.mixed_components = parse_mixed_components(components_opt->values[i]);
                for (double ratio : parse_mixed_ratios(ratios_opt && i < ratios_opt->values.size() ? ratios_opt->values[i] : "", entry.mixed_components.size()))
                    entry.mixed_ratios.push_back((int) std::lround(ratio * 100.0));
            }
            filament_entries.push_back(std::move(entry));
        }
    }
    TextureImportResult result;
    bool proceed = true;
    {
        TextureImportDialog dlg(nullptr, *model.texture_mesh, filament_entries);
        if (dlg.ShowModal() != wxID_OK) {
            if (dlg.was_skipped()) result.skipped = true;
            else if (dlg.fallback_to_geometry_only()) {
                Slic3r::GUI::MessageDialog(nullptr, fallback_warning, _L("Texture Import Warning"), wxOK | wxICON_WARNING).ShowModal();
            } else
                proceed = false;
        } else {
            result.painted = dlg.get_painted_mesh();
            result.matches = dlg.get_matches();
            result.new_filament_colors = dlg.get_new_filament_colors();
            result.new_filament_preset_names = dlg.get_new_filament_preset_names();
            result.new_mixed_filaments = dlg.get_new_mixed_filaments();
            result.filament_entries = dlg.get_filament_entries();
            result.existing_filament_count = dlg.get_existing_filament_count();
        }
    }
    if (proceed && !result.painted.face_colors.empty() && !result.matches.empty()) {
        std::vector<size_t> idxs(model.objects.size());
        std::iota(idxs.begin(), idxs.end(), 0);
        plater.apply_textured_mesh_import_result(model, idxs, result, {}, false);
    }
    model.texture_mesh.reset();
    wxGetApp().m_plater = nullptr;
    presets_changed();
    return proceed;
}

// ---- the mixed filament dialog's colour math (FilamentBitmapUtils / FilamentMixer), without the engine lock
static json mixed_blend(const json &a)
{
    std::vector<std::string> hex = a.at("colors");
    std::vector<double>      w   = a.at("weights");
    return blend_n(hex, w);
}
// draw_mixed_ratio_blend_bar's columns: lerp_blend(first, second, 1 - t)
static json mixed_lerp_ramp(const json &a)
{
    const wxColour f(a.at("from").get<std::string>()), t(a.at("to").get<std::string>());
    const int n = std::clamp(a.value("steps", 64), 2, 1024);
    json out = json::array();
    for (int x = 0; x < n; ++x) {
        unsigned char r, g, b;
        filament_mixer_lerp(f.Red(), f.Green(), f.Blue(), t.Red(), t.Green(), t.Blue(), float(double(x) / n), &r, &g, &b);
        out.push_back(wxColour(r, g, b).GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
    }
    return out;
}
// MixedFilamentDialog::blend_colors (filament_mixer_lerp) of each [a, b, ratio_a]
static json mixed_lerp(const json &a)
{
    json out = json::array();
    for (const json &p : a.at("pairs")) {
        const wxColour x(p.at(0).get<std::string>()), y(p.at(1).get<std::string>());
        unsigned char r, g, b;
        filament_mixer_lerp(x.Red(), x.Green(), x.Blue(), y.Red(), y.Green(), y.Blue(), float(1.0 - p.at(2).get<double>()), &r, &g, &b);
        out.push_back(wxColour(r, g, b).GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
    }
    return out;
}
static GradientCurve curve_arg(const json &a)
{
    GradientCurve c;
    for (const json &p : a.at("curve"))
        c.points.push_back({p.at(0).get<double>(), p.at(1).get<double>(), p.size() > 2 && p[2].is_number() ? p[2].get<double>() : NAN,
                            p.size() > 3 && p[3].is_number() ? p[3].get<double>() : NAN});
    return c;
}
// sample_gradient_curve at n points, and the colour ramp of the two components (sample_gradient_ramp)
static json mixed_curve(const json &a)
{
    const GradientCurve c = curve_arg(a);
    const int n = std::clamp(a.value("steps", 128), 2, 2048);
    json ys = json::array(), ramp = json::array();
    const bool colours = a.contains("first");
    for (int i = 0; i <= n; ++i) {
        const double y = sample_gradient_curve(c, double(i) / n);
        ys.push_back(y);
        if (colours) ramp.push_back(blend_n({a["first"].get<std::string>(), a["second"].get<std::string>()}, {y, 1.0 - y}));
    }
    return {{"y", ys}, {"ramp", ramp}};
}
// the triangle picker's fill: every pixel inside the triangle blended by its barycentric weights (blend_n_colors)
static json mixed_triangle(const json &a)
{
    std::vector<std::string> cols = a.at("colors");
    const int w = std::clamp(a.value("width", 120), 8, 400), h = std::clamp(a.value("height", 104), 8, 400);
    // vertices: top centre, bottom left, bottom right of the w x h box
    const double x0 = w / 2.0, y0 = 0, x1 = 0, y1 = h, x2 = w, y2 = h;
    const double det = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    std::vector<int> px(size_t(w) * h, 0);
    std::map<std::tuple<int, int, int>, int> cache;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double l0 = ((y1 - y2) * (x + 0.5 - x2) + (x2 - x1) * (y + 0.5 - y2)) / det;
            const double l1 = ((y2 - y0) * (x + 0.5 - x2) + (x0 - x2) * (y + 0.5 - y2)) / det;
            const double l2 = 1 - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            // quantised weights (1%): neighbouring pixels share the blend
            const auto key = std::make_tuple(int(l0 * 100), int(l1 * 100), int(l2 * 100));
            auto it = cache.find(key);
            if (it == cache.end()) {
                const wxColour c(blend_n(cols, {l0, l1, l2}));
                it = cache.emplace(key, int(0xFF000000u | (unsigned(c.Red()) << 16) | (unsigned(c.Green()) << 8) | c.Blue())).first;
            }
            px[size_t(y) * w + x] = it->second;
        }
    return {{"width", w}, {"height", h}, {"pixels", px}};
}
static PureCommands pure({{"texture.cancel", [](const json &) -> json { g_texture_cancel = true; return nullptr; }}, {"mixed.blend", mixed_blend}, {"mixed.lerp", mixed_lerp}, {"mixed.lerp_ramp", mixed_lerp_ramp}, {"mixed.curve", mixed_curve}, {"mixed.triangle", mixed_triangle}});

} // namespace orca
