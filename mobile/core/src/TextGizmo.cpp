// The Text gizmo (GLGizmoEmboss + its StyleManager): a text volume embossed from a TrueType font, edited in the panel
// (text, style list, font, height, depth, advanced: surface, per glyph, alignment, gaps, boldness, skew, distance,
// rotation, collection, face the camera), moved over the object's surface by a drag, created from the toolbar or the
// context menus. Fonts are the device's (/system/fonts) and Orca's own (resources/fonts).
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>

#include "Core.hpp"
#include "EmbossCommon.hpp"
#include "EmbossPlace.hpp"
#include "Plater.hpp"
#include "TextGizmo.hpp"
#include "Viewport.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ExPolygonsIndex.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TextConfiguration.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

using namespace Slic3r;
using namespace Slic3r::Emboss;
using namespace orca::emboss;
namespace fs = boost::filesystem;

namespace orca {

namespace {

#include "slic3r/GUI/TextLines.inc"

// GLGizmoEmboss.cpp limits
template<typename T> struct Limit { MinMax<T> gui; MinMax<T> values; };
const struct Limits
{
    MinMax<double> emboss{0.01, 1e4};
    MinMax<float>  size_in_mm{0.1f, 1000.f};
    Limit<float>   boldness{{-.5f, .5f}, {-5e5f, 5e5f}};
    Limit<float>   skew{{-1.f, 1.f}, {-100.f, 100.f}};
    MinMax<int>    char_gap{-20000, 20000};
    MinMax<int>    line_gap{-20000, 20000};
    MinMax<float>  angle{-180.f, 180.f};
} limits;

template<typename T> bool apply_limit(T &value, const MinMax<T> &l)
{
    if (value < l.min) { value = l.min; return true; }
    if (value > l.max) { value = l.max; return true; }
    return false;
}
template<typename T> bool apply_limit(std::optional<T> &value, const MinMax<T> &l)
{
    if (!value.has_value()) return false;
    return apply_limit(*value, l);
}

bool is_text_empty(std::string_view text) { return text.empty() || text.find_first_not_of(" \n\t\r") == std::string::npos; }

// ------------------------------------------------------------------------------------------------- the font list

// one face of an installed font file (wxFontEnumerator's face names): family, subfamily from the OpenType name table
struct Face
{
    std::string name;   // what the list shows: the family, plus the subfamily when it is not "Regular"
    std::string family, subfamily;
    std::string path;
    unsigned    index{0};   // in a collection
    bool        bold{false}, italic{false};
};

uint16_t be16(const unsigned char *p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const unsigned char *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// the family / subfamily strings of the face at `offset` (an OpenType offset table)
bool read_names(std::ifstream &f, uint32_t offset, std::string &family, std::string &sub)
{
    unsigned char hdr[12];
    f.seekg(offset);
    if (!f.read(reinterpret_cast<char *>(hdr), 12)) return false;
    const uint16_t num_tables = be16(hdr + 4);
    std::vector<unsigned char> dir(size_t(num_tables) * 16);
    if (!f.read(reinterpret_cast<char *>(dir.data()), dir.size())) return false;
    for (uint16_t t = 0; t < num_tables; ++t) {
        const unsigned char *r = dir.data() + t * 16;
        if (memcmp(r, "name", 4) != 0) continue;
        const uint32_t name_offset = be32(r + 8), name_length = be32(r + 12);
        std::vector<unsigned char> name(name_length);
        f.seekg(name_offset);
        if (name_length < 6 || !f.read(reinterpret_cast<char *>(name.data()), name_length)) return false;
        const uint16_t count = be16(name.data() + 2), storage = be16(name.data() + 4);
        std::map<int, std::string> best;   // nameID -> value (Windows English first)
        std::map<int, int> rank;
        for (uint16_t i = 0; i < count && 6 + (i + 1) * 12 <= name_length; ++i) {
            const unsigned char *n = name.data() + 6 + i * 12;
            const uint16_t platform = be16(n), encoding = be16(n + 2), language = be16(n + 4), id = be16(n + 6), len = be16(n + 8), off = be16(n + 10);
            if (id != 1 && id != 2 && id != 16 && id != 17) continue;
            if (size_t(storage) + off + len > name_length) continue;
            const unsigned char *s = name.data() + storage + off;
            std::string value;
            int r_ = 0;
            if (platform == 3 && (encoding == 1 || encoding == 10)) {   // UTF-16BE
                std::wstring w;
                for (int k = 0; k + 1 < len; k += 2) w.push_back(wchar_t(be16(s + k)));
                value = boost::nowide::narrow(w);
                r_ = language == 0x409 ? 3 : 2;
            } else if (platform == 1 && encoding == 0) {
                value.assign(reinterpret_cast<const char *>(s), len);
                r_ = 1;
            } else
                continue;
            if (r_ > rank[id]) { rank[id] = r_; best[id] = value; }
        }
        family = best.count(16) ? best[16] : best[1];   // the typographic family when there is one
        sub = best.count(17) ? best[17] : best[2];
        return !family.empty();
    }
    return false;
}

std::vector<Face> scan_fonts()
{
    std::vector<Face> out;
    std::vector<std::string> dirs{"/system/fonts", "/product/fonts", resources_dir() + "/fonts"};
    for (const std::string &dir : dirs) {
        boost::system::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            const std::string ext = boost::algorithm::to_lower_copy(it->path().extension().string());
            if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;
            const std::string path = it->path().string();
            if (path.find("Emoji") != std::string::npos) continue;   // colour bitmaps, no outlines
            std::ifstream f(path, std::ios::binary);
            unsigned char hdr[12];
            if (!f.read(reinterpret_cast<char *>(hdr), 12)) continue;
            std::vector<uint32_t> offsets;
            if (memcmp(hdr, "ttcf", 4) == 0) {
                const uint32_t n = be32(hdr + 8);
                std::vector<unsigned char> o(size_t(n) * 4);
                if (!f.read(reinterpret_cast<char *>(o.data()), o.size())) continue;
                for (uint32_t k = 0; k < n; ++k) offsets.push_back(be32(o.data() + 4 * k));
            } else
                offsets.push_back(0);
            for (unsigned k = 0; k < offsets.size(); ++k) {
                Face face;
                if (!read_names(f, offsets[k], face.family, face.subfamily)) continue;
                face.path = path;
                face.index = k;
                const std::string sub = boost::algorithm::to_lower_copy(face.subfamily);
                face.bold = sub.find("bold") != std::string::npos;
                face.italic = sub.find("italic") != std::string::npos || sub.find("oblique") != std::string::npos;
                face.name = (sub.empty() || sub == "regular") ? face.family : face.family + " " + face.subfamily;
                out.push_back(std::move(face));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Face &a, const Face &b) { return a.name < b.name; });
    out.erase(std::unique(out.begin(), out.end(), [](const Face &a, const Face &b) { return a.name == b.name; }), out.end());
    return out;
}

// ------------------------------------------------------------------------------------------------- styles

// StyleManager::Style
struct Style : public EmbossStyle
{
    EmbossProjection     projection;
    std::optional<float> distance;   // [mm] from the surface
    std::optional<float> angle;      // [rad]
    bool operator==(const Style &o) const
    {
        return EmbossStyle::operator==(o) && projection == o.projection && distance == o.distance && angle == o.angle;
    }
};

// the AppConfig sections of the styles (EmbossStyleManager.cpp)
const std::string APP_CONFIG_FONT_NAME = "name", APP_CONFIG_FONT_DESCRIPTOR = "descriptor", APP_CONFIG_FONT_LINE_HEIGHT = "line_height",
                  APP_CONFIG_FONT_DEPTH = "depth", APP_CONFIG_FONT_USE_SURFACE = "use_surface", APP_CONFIG_PER_GLYPH = "per_glyph",
                  APP_CONFIG_VERTICAL_ALIGN = "vertical_align", APP_CONFIG_HORIZONTAL_ALIGN = "horizontal_align",
                  APP_CONFIG_FONT_BOLDNESS = "boldness", APP_CONFIG_FONT_SKEW = "skew", APP_CONFIG_FONT_DISTANCE = "distance",
                  APP_CONFIG_FONT_ANGLE = "angle", APP_CONFIG_FONT_COLLECTION = "collection", APP_CONFIG_FONT_CHAR_GAP = "char_gap",
                  APP_CONFIG_FONT_LINE_GAP = "line_gap", APP_CONFIG_ACTIVE_FONT = "active_font", APP_CONFIG_FONT_FACE = "face_name";
std::string section_name(unsigned index) { return AppConfig::SECTION_EMBOSS_STYLE + ':' + std::to_string(index); }

std::string h_align_name(FontProp::HorizontalAlign a) { return a == FontProp::HorizontalAlign::left ? "left" : a == FontProp::HorizontalAlign::right ? "right" : "center"; }
std::string v_align_name(FontProp::VerticalAlign a) { return a == FontProp::VerticalAlign::top ? "top" : a == FontProp::VerticalAlign::bottom ? "bottom" : "middle"; }

class TextGizmo
{
public:
    static TextGizmo &get() { static TextGizmo g; return g; }

    json open(const json &a);
    void close();
    json state();
    json set(const json &a);
    json style_command(const json &a);
    json fonts();
    bool active() { std::lock_guard<std::recursive_mutex> l(m_mutex); return m_obj >= 0; }
    // the surface drag (on_mouse_for_translate)
    bool drag_begin(const Camera &cam, double px, double py);
    // m_rotate_gizmo: its frame (under the core lock) and the rotation dragged on it
    bool ring(Transform3d &frame, double &radius)
    {
        std::lock_guard<std::recursive_mutex> l(m_mutex);
        ModelVolume *mv = volume();
        if (mv == nullptr) return false;
        emboss::rotate_ring(*mv, m_inst, frame, radius);
        return true;
    }
    // on_stop_dragging: dragging_rotate_gizmo's angle, applied as do_local_z_rotate
    void ring_rotate(double delta)
    {
        std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
        std::lock_guard<std::recursive_mutex> l(m_mutex);
        double a = m_style.angle.value_or(0.f) + delta;
        Geometry::to_range_pi_pi(a);
        set({{"rotation", -a * 180. / M_PI}, {"final", true}});
    }
    void drag_move(const Camera &cam, double px, double py);
    void drag_end();

private:
    void init_styles();
    void store_styles(bool store_active_index);
    bool load_font(const Style &style);
    const Face *face_of(const std::string &name);
    ModelVolume *volume();
    void select_volume(int obj, int inst, int vol);
    std::unique_ptr<DataBase> data_base(ModelVolumeType type);
    bool process(bool snapshot);
    void reinit_text_lines(unsigned count_lines = 0);
    json create(ModelVolumeType type, std::optional<Vec2d> screen);
    std::vector<Style> default_styles();

    std::recursive_mutex m_mutex;
    std::vector<Face>    m_faces;
    bool                 m_faces_scanned{false};
    std::vector<Style>   m_styles;
    size_t               m_style_index{0};   // the stored style the current one comes from (npos: not stored)
    Style                m_style;            // StyleManager::m_style_cache.style
    FontFileWithCache    m_font;
    std::string          m_font_key;
    std::string          m_text;
    int                  m_obj{-1}, m_inst{0};
    size_t               m_volume_id{0};
    bool                 m_keep_up{true};
    bool                 m_unknown_font{false};
    std::string          m_error;
    unsigned             m_session{0};   // bumped when another text volume is edited   // the last job error (a notification on the desktop)
    TextLines            m_text_lines;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    SurfaceDrag          m_drag;
};

// --- fonts

const Face *TextGizmo::face_of(const std::string &name)
{
    if (!m_faces_scanned) { m_faces = scan_fonts(); m_faces_scanned = true; }
    for (const Face &f : m_faces)
        if (f.name == name || f.family == name) return &f;
    return nullptr;
}

// StyleManager::load_style: the font file of the style (cached while the style keeps the same file)
bool TextGizmo::load_font(const Style &style)
{
    std::string path = style.path;
    if (style.type != EmbossStyle::Type::file_path || !fs::exists(path)) {
        // a style from another OS (wx descriptor) or a missing file: by the face name
        const Face *f = style.prop.face_name ? face_of(*style.prop.face_name) : nullptr;
        if (f == nullptr) return false;
        path = f->path;
    }
    if (path == m_font_key && m_font.has_value()) return true;
    std::unique_ptr<FontFile> ff = create_font_file(path.c_str());
    if (ff == nullptr) return false;
    m_font = FontFileWithCache(std::move(ff));
    m_font_key = path;
    return true;
}

// GLGizmoEmboss::create_default_styles: the wx default fonts on the desktop, the device's sans / serif / mono here
std::vector<Style> TextGizmo::default_styles()
{
    auto make = [this](const std::string &name, std::initializer_list<const char *> faces, float size, bool bold = false) -> std::optional<Style> {
        for (const char *fn : faces) {
            const Face *f = face_of(fn);
            if (f == nullptr) continue;
            Style s;
            s.name = name;
            s.path = f->path;
            s.type = EmbossStyle::Type::file_path;
            s.prop.size_in_mm = size;
            s.prop.face_name = f->name;
            s.prop.family = f->family;
            if (bold) s.prop.boldness = 20.f;
            return s;
        }
        return {};
    };
    std::vector<Style> out;
    for (auto s : {make(_u8L("NORMAL"), {"Roboto", "Noto Sans", "Droid Sans"}, 10.f), make(_u8L("SMALL"), {"Roboto", "Noto Sans", "Droid Sans"}, 8.f),
                   make(_u8L("ITALIC"), {"Noto Serif Italic", "Roboto Italic"}, 10.f), make(_u8L("SWISS"), {"Noto Sans", "Roboto", "Droid Sans"}, 10.f),
                   make(_u8L("MODERN"), {"Droid Sans Mono", "Cutive Mono", "Roboto Mono"}, 10.f, true)})
        if (s) out.push_back(*s);
    if (out.empty()) {   // the font shipped with the slicer
        Style s;
        s.name = _u8L("Default font");
        s.path = resources_dir() + "/fonts/NotoSansKR-Regular.ttf";
        s.type = EmbossStyle::Type::file_path;
        out.push_back(s);
    }
    return out;
}

// StyleManager::init: the styles of the app config, else the defaults
void TextGizmo::init_styles()
{
    if (!m_styles.empty()) return;
    AppConfig &cfg = *Core::get().app_config;
    for (unsigned index = 1; cfg.has_section(section_name(index)); ++index) {
        const auto section = cfg.get_section(section_name(index));
        auto get = [&section](const std::string &k) -> std::optional<std::string> {
            auto it = section.find(k);
            return it == section.end() || it->second.empty() ? std::optional<std::string>() : it->second;
        };
        if (!get(APP_CONFIG_FONT_DESCRIPTOR)) continue;
        Style s;
        s.path = *get(APP_CONFIG_FONT_DESCRIPTOR);
        s.type = EmbossStyle::Type::file_path;
        s.name = get(APP_CONFIG_FONT_NAME).value_or("font_name");
        if (auto v = get(APP_CONFIG_FONT_LINE_HEIGHT)) s.prop.size_in_mm = std::stof(*v);
        if (auto v = get(APP_CONFIG_FONT_DEPTH)) s.projection.depth = std::stod(*v);
        s.projection.use_surface = get(APP_CONFIG_FONT_USE_SURFACE) == std::optional<std::string>("true");
        s.prop.per_glyph = get(APP_CONFIG_PER_GLYPH) == std::optional<std::string>("true");
        if (auto v = get(APP_CONFIG_HORIZONTAL_ALIGN)) s.prop.align.first = *v == "left" ? FontProp::HorizontalAlign::left : *v == "right" ? FontProp::HorizontalAlign::right : FontProp::HorizontalAlign::center;
        if (auto v = get(APP_CONFIG_VERTICAL_ALIGN)) s.prop.align.second = *v == "top" ? FontProp::VerticalAlign::top : *v == "bottom" ? FontProp::VerticalAlign::bottom : FontProp::VerticalAlign::center;
        if (auto v = get(APP_CONFIG_FONT_BOLDNESS)) s.prop.boldness = std::stof(*v);
        if (auto v = get(APP_CONFIG_FONT_SKEW)) s.prop.skew = std::stof(*v);
        if (auto v = get(APP_CONFIG_FONT_DISTANCE)) s.distance = std::stof(*v);
        if (auto v = get(APP_CONFIG_FONT_ANGLE)) s.angle = std::stof(*v);
        if (auto v = get(APP_CONFIG_FONT_COLLECTION)) s.prop.collection_number = unsigned(std::stoi(*v));
        if (auto v = get(APP_CONFIG_FONT_CHAR_GAP)) s.prop.char_gap = std::stoi(*v);
        if (auto v = get(APP_CONFIG_FONT_LINE_GAP)) s.prop.line_gap = std::stoi(*v);
        if (auto v = get(APP_CONFIG_FONT_FACE)) s.prop.face_name = *v;
        m_styles.push_back(s);
    }
    if (m_styles.empty()) m_styles = default_styles();
    size_t active = 0;
    if (cfg.has_section(AppConfig::SECTION_EMBOSS_STYLE)) {
        auto section = cfg.get_section(AppConfig::SECTION_EMBOSS_STYLE);
        if (auto it = section.find(APP_CONFIG_ACTIVE_FONT); it != section.end()) active = size_t(std::max(1, std::atoi(it->second.c_str()))) - 1;
    }
    if (active >= m_styles.size()) active = 0;
    // load_valid_style
    for (size_t k = 0; k < m_styles.size(); ++k) {
        const size_t i = (active + k) % m_styles.size();
        if (load_font(m_styles[i])) { m_style_index = i; m_style = m_styles[i]; return; }
    }
    m_styles = default_styles();
    m_style_index = 0;
    m_style = m_styles.front();
    load_font(m_style);
}

void TextGizmo::store_styles(bool store_active_index)
{
    AppConfig &cfg = *Core::get().app_config;
    unsigned index = 1;
    for (const Style &s : m_styles) {
        std::map<std::string, std::string> data;
        data[APP_CONFIG_FONT_NAME] = s.name;
        data[APP_CONFIG_FONT_DESCRIPTOR] = s.path;
        data[APP_CONFIG_FONT_LINE_HEIGHT] = std::to_string(s.prop.size_in_mm);
        data[APP_CONFIG_FONT_DEPTH] = std::to_string(s.projection.depth);
        if (s.projection.use_surface) data[APP_CONFIG_FONT_USE_SURFACE] = "true";
        if (s.prop.per_glyph) data[APP_CONFIG_PER_GLYPH] = "true";
        if (s.prop.align.first != FontProp::HorizontalAlign::center) data[APP_CONFIG_HORIZONTAL_ALIGN] = h_align_name(s.prop.align.first);
        if (s.prop.align.second != FontProp::VerticalAlign::center) data[APP_CONFIG_VERTICAL_ALIGN] = v_align_name(s.prop.align.second);
        if (s.prop.boldness) data[APP_CONFIG_FONT_BOLDNESS] = std::to_string(*s.prop.boldness);
        if (s.prop.skew) data[APP_CONFIG_FONT_SKEW] = std::to_string(*s.prop.skew);
        if (s.distance) data[APP_CONFIG_FONT_DISTANCE] = std::to_string(*s.distance);
        if (s.angle) data[APP_CONFIG_FONT_ANGLE] = std::to_string(*s.angle);
        if (s.prop.collection_number) data[APP_CONFIG_FONT_COLLECTION] = std::to_string(*s.prop.collection_number);
        if (s.prop.char_gap) data[APP_CONFIG_FONT_CHAR_GAP] = std::to_string(*s.prop.char_gap);
        if (s.prop.line_gap) data[APP_CONFIG_FONT_LINE_GAP] = std::to_string(*s.prop.line_gap);
        if (s.prop.face_name) data[APP_CONFIG_FONT_FACE] = *s.prop.face_name;
        cfg.set_section(section_name(index++), data);
    }
    while (cfg.has_section(section_name(index))) cfg.clear_section(section_name(index++));
    if (store_active_index && m_style_index < m_styles.size())
        cfg.set_section(AppConfig::SECTION_EMBOSS_STYLE, {{APP_CONFIG_ACTIVE_FONT, std::to_string(m_style_index + 1)}});
    cfg.save();
}

// --- the edited volume

ModelVolume *TextGizmo::volume()
{
    const Model &m = Plater::get().model();
    if (m_obj < 0 || m_obj >= int(m.objects.size())) return nullptr;
    for (ModelVolume *v : m.objects[m_obj]->volumes)
        if (v->id().id == m_volume_id) return v;
    return nullptr;
}

// GLGizmoEmboss::set_volume_by_selection
void TextGizmo::select_volume(int obj, int inst, int vol)
{
    ModelVolume *v = Plater::get().model().objects[obj]->volumes[vol];
    const TextConfiguration &tc = *v->text_configuration;
    init_styles();
    Style style;
    static_cast<EmbossStyle &>(style) = tc.style;
    style.projection = v->emboss_shape->projection;
    style.angle = calc_angle(*v, inst);
    style.distance = calc_distance(*v, inst);
    m_unknown_font = !load_font(style);
    if (m_unknown_font) load_font(m_styles.empty() ? default_styles().front() : m_styles.front());
    auto it = std::find_if(m_styles.begin(), m_styles.end(), [&](const Style &s) { return s.name == style.name; });
    m_style_index = it == m_styles.end() ? std::string::npos : size_t(it - m_styles.begin());
    m_style = style;
    m_text = tc.text;
    m_obj = obj;
    m_inst = inst;
    m_volume_id = v->id().id;
    ++m_session;
    if (m_cancel) m_cancel->store(true);
    m_cancel.reset();
    m_text_lines.clear();
    if (tc.style.prop.per_glyph) reinit_text_lines();
}

// init_text_lines / TextLinesModel::init: the object's contours on each text line, for the per glyph placement
void TextGizmo::reinit_text_lines(unsigned count_lines)
{
    m_text_lines.clear();
    ModelVolume *mv = volume();
    if (mv == nullptr || mv->is_the_only_one_part() || !m_font.has_value()) return;
    if (count_lines == 0) count_lines = get_count_lines(m_text);
    if (count_lines == 0) return;
    ModelVolumePtrs volumes;   // prepare_volumes_to_slice
    for (ModelVolume *v : mv->get_object()->volumes)
        if (v->is_model_part() && v->id() != mv->id()) volumes.push_back(v);
    Transform3d text_tr = mv->get_matrix();
    if (mv->emboss_shape && mv->emboss_shape->fix_3mf_tr) text_tr = text_tr * mv->emboss_shape->fix_3mf_tr->inverse();
    const FontFile &ff = *m_font.font_file;
    const FontProp &fp = m_style.prop;
    const double line_height_mm = get_line_height(ff, fp) * get_text_shape_scale(fp, ff);
    if (line_height_mm <= 0) return;
    const double ascent_ratio_offset = 1 / 3.;
    const double line_offset = fp.size_in_mm * ascent_ratio_offset;
    const double first_line_center = line_offset + get_align_y_offset_in_mm(fp.align.second, count_lines, ff, fp);
    std::vector<float> line_centers(count_lines);
    for (size_t i = 0; i < count_lines; ++i) line_centers[i] = float(first_line_center - i * line_height_mm);
    const Transform3d c_trafo = text_tr * Eigen::AngleAxis(-M_PI_2, Vec3d::UnitX());
    const Transform3d c_trafo_inv = c_trafo.inverse();
    std::vector<Polygons> line_contours(count_lines);
    for (const ModelVolume *v : volumes) {
        MeshSlicingParams slicing_params;
        slicing_params.trafo = c_trafo_inv * v->get_matrix();
        for (size_t i = 0; i < count_lines; ++i) {
            const Polygons polys = slice_mesh(v->mesh().its, line_centers[i], slicing_params);
            line_contours[i].insert(line_contours[i].end(), polys.begin(), polys.end());
        }
    }
    for (Polygons &contours : line_contours) {   // a line out of the object: the contour at zero
        if (!contours.empty()) continue;
        for (const ModelVolume *v : volumes) {
            MeshSlicingParams slicing_params;
            slicing_params.trafo = c_trafo_inv * v->get_matrix();
            const Polygons polys = slice_mesh(v->mesh().its, 0.f, slicing_params);
            contours.insert(contours.end(), polys.begin(), polys.end());
        }
    }
    m_text_lines = select_closest_contour(line_contours);
    for (size_t i = 0; i < count_lines && i < m_text_lines.size(); ++i) m_text_lines[i].y = line_centers[i];
}

// the TextDataBase of create_emboss_data_base: the text shape made from the font
struct TextDataBase : public DataBase
{
    TextDataBase(DataBase &&parent, const FontFileWithCache &font_file, TextConfiguration &&tc, const EmbossProjection &projection)
        : DataBase(std::move(parent)), m_font_file(font_file), m_text_configuration(std::move(tc))
    {
        shape.projection = projection;
        shape.scale = get_text_shape_scale(m_text_configuration.style.prop, *m_font_file.font_file);
    }
    EmbossShape &create_shape() override
    {
        if (!shape.shapes_with_ids.empty()) return shape;
        std::wstring text_w = boost::nowide::widen(m_text_configuration.text.c_str());
        auto was_canceled = [&c = cancel]() { return c && c->load(); };
        shape.shapes_with_ids = text2vshapes(m_font_file, text_w, m_text_configuration.style.prop, was_canceled);
        return shape;
    }
    void write(ModelVolume &volume) const override
    {
        DataBase::write(volume);
        volume.text_configuration = m_text_configuration;
    }
    FontFileWithCache m_font_file;
    TextConfiguration m_text_configuration;
};

std::unique_ptr<DataBase> TextGizmo::data_base(ModelVolumeType type)
{
    std::string volume_name = m_text;
    std::replace(volume_name.begin(), volume_name.end(), '\n', ' ');
    if (!m_style.prop.per_glyph) m_text_lines.clear();
    if (m_cancel) m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    DataBase base(volume_name, m_cancel);
    base.is_outside = type == ModelVolumeType::MODEL_PART;
    base.text_lines = m_text_lines;
    base.from_surface = m_style.distance;
    TextConfiguration tc{static_cast<EmbossStyle>(m_style), m_text};
    return std::make_unique<TextDataBase>(std::move(base), m_font, std::move(tc), m_style.projection);
}

// GLGizmoEmboss::process + start_update_volume + UpdateJob / UpdateSurfaceVolumeJob
bool TextGizmo::process(bool snapshot)
{
    ModelVolume *mv = volume();
    if (mv == nullptr || is_text_empty(m_text) || !m_font.has_value()) return false;
    std::unique_ptr<DataBase> base = data_base(mv->type());
    try {
        if (!emboss::update(*mv, m_inst, *base, snapshot ? _u8L("Emboss attribute change") : std::string())) return false;
    } catch (const JobException &e) {
        m_error = e.what();
        return false;
    }
    m_volume_id = mv->id().id;
    return true;
}

// --- creating a text volume (create_volume / start_create_volume[_without_position] and their jobs)

json TextGizmo::create(ModelVolumeType volume_type, std::optional<Vec2d> screen)
{
    init_styles();
    m_style = m_style_index < m_styles.size() ? m_styles[m_style_index] : m_style;   // discard_style_changes
    load_font(m_style);
    m_text = _u8L("Embossed text");   // init_create
    m_text_lines.clear();
    m_error.clear();
    std::unique_ptr<DataBase> base = data_base(volume_type);
    try {
        const Created c = emboss::create(*base, volume_type, screen, m_style.angle, m_style.distance, _u8L("Add Emboss text object"),
                                         _u8L("Add Emboss text Volume"));
        select_volume(c.obj, c.inst, c.vol);
    } catch (const JobException &e) {
        m_error = e.what();
    }
    return Plater::get().state();
}

// open: on_shortcut_key / the toolbar (edit the selected text, else create one), "Edit text", "Add ... Text"
json TextGizmo::open(const json &a)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    Plater &p = Plater::get();
    const Model &m = p.model();
    if (a.contains("type")) {   // add_volume_with_gizmo
        const std::string t = a["type"];
        const ModelVolumeType type = t == "negative" ? ModelVolumeType::NEGATIVE_VOLUME : t == "modifier" ? ModelVolumeType::PARAMETER_MODIFIER : ModelVolumeType::MODEL_PART;
        std::optional<Vec2d> screen;
        if (a.contains("x")) screen = Vec2d(a["x"].get<double>(), a["y"].get<double>());
        if (t == "invalid") p.selection.clear();
        return create(type, screen);
    }
    // the selected single volume
    std::set<std::tuple<int, int, int>> vols;
    for (const SelItem &s : p.selection) {
        if (s.obj < 0 || s.obj >= int(m.objects.size())) continue;
        const ModelObject *mo = m.objects[s.obj];
        for (int i = 0; i < int(mo->instances.size()); ++i)
            if (s.inst < 0 || s.inst == i)
                for (int v = 0; v < int(mo->volumes.size()); ++v)
                    if (s.vol < 0 || s.vol == v) vols.insert({s.obj, i, v});
    }
    if (vols.size() == 1) {
        auto [o, i, v] = *vols.begin();
        if (m.objects[o]->volumes[v]->is_text()) { select_volume(o, i, v); return p.state(); }
    }
    return create(ModelVolumeType::MODEL_PART, std::nullopt);
}

// GLGizmoEmboss::close / on_set_state(Off): an empty text is removed; the styles go to the app config
void TextGizmo::close()
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (m_obj < 0) return;
    if (ModelVolume *mv = volume(); mv != nullptr && is_text_empty(m_text)) {
        Plater &p = Plater::get();
        ModelObject *mo = mv->get_object();
        p.take_snapshot("Delete text");
        if (mv->is_the_only_one_part()) p.model().delete_object(mo);
        else mo->delete_volume(std::find(mo->volumes.begin(), mo->volumes.end(), mv) - mo->volumes.begin());
        p.selection.clear();
        p.changed();
    }
    if (m_styles.size()) store_styles(true);
    m_obj = -1;
    m_drag.reset();
}

json TextGizmo::fonts()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_faces_scanned) { m_faces = scan_fonts(); m_faces_scanned = true; }
    json out = json::array();
    for (const Face &f : m_faces) out.push_back({{"name", f.name}, {"path", f.path}});
    return out;
}

// on_render_input_window / draw_window: what the panel shows
json TextGizmo::state()
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    ModelVolume *mv = volume();
    if (mv == nullptr || !mv->is_text()) { m_obj = -1; return {{"active", false}}; }
    const bool use_inch = Core::get().app_config->get_bool("use_inches");
    const FontProp &fp = m_style.prop;
    json styles = json::array();
    for (const Style &s : m_styles) styles.push_back(s.name);
    const Style *stored = m_style_index < m_styles.size() ? &m_styles[m_style_index] : nullptr;
    const bool modified = stored == nullptr || !(*stored == m_style);
    // the font's metrics for the slider ranges
    int ascent = 1000;
    if (m_font.has_value()) ascent = get_font_info(*m_font.font_file, fp).ascent;
    // the warnings of draw_text_input
    json warnings = json::array();
    if (is_text_empty(m_text)) warnings.push_back(_u8L("Embossed text cannot contain only white spaces."));
    if (m_font.has_value()) {
        bool exist_unknown = false;
        create_range_text(m_text, *m_font.font_file, fp.collection_number.value_or(0), &exist_unknown);
        if (exist_unknown) warnings.push_back(_u8L("Text contains character glyph (represented by '?') unknown by font."));
    } else
        warnings.push_back(_u8L("The text cannot be written using the selected font. Please try choosing a different font."));
    const Face *face = fp.face_name ? face_of(*fp.face_name) : nullptr;
    const bool only_part = mv->is_the_only_one_part();
    json st = {
        {"active", true},
        {"text", m_text},
        {"styles", styles},
        {"style_index", m_style_index < m_styles.size() ? int(m_style_index) : -1},
        {"style_name", m_style.name},
        {"modified", modified},
        {"face", fp.face_name.value_or(face ? face->name : fs::path(m_style.path).stem().string())},
        {"unknown_font", m_unknown_font},
        {"height", fp.size_in_mm * (use_inch ? 0.0393700787 : 1.)},
        {"depth", m_style.projection.depth * (use_inch ? 0.0393700787 : 1.)},
        {"units", use_inch ? "in" : "mm"},
        {"use_surface", m_style.projection.use_surface},
        {"can_use_surface", m_style.projection.use_surface || !only_part},
        {"per_glyph", fp.per_glyph},
        {"can_per_glyph", fp.per_glyph || !only_part},
        {"align_h", int(fp.align.first)},
        {"align_v", int(fp.align.second)},
        {"char_gap", fp.char_gap.value_or(0)},
        {"line_gap", fp.line_gap.value_or(0)},
        {"multiline", get_count_lines(m_text) > 1},
        {"boldness", fp.boldness.value_or(0.f)},
        {"skew", fp.skew.value_or(0.f)},
        {"bold", fp.boldness.has_value() || (face && face->bold)},
        {"italic", fp.skew.has_value() || (face && face->italic)},
        {"gap_range", {-ascent / 2, ascent / 2}},
        {"boldness_range", {int(ascent * limits.boldness.gui.min), int(ascent * limits.boldness.gui.max)}},
        {"distance", m_style.distance.value_or(0.f)},
        {"distance_range", {-2 * m_style.projection.depth, 2 * m_style.projection.depth}},
        {"can_distance", !m_style.projection.use_surface && !only_part},
        {"rotation", -m_style.angle.value_or(0.f) * 180. / M_PI},
        {"keep_up", m_keep_up},
        {"show_keep_up", !only_part},
        {"collections", m_font.has_value() ? int(m_font.font_file->infos.size()) : 1},
        {"collection", int(fp.collection_number.value_or(0))},
        {"type", mv->type() == ModelVolumeType::NEGATIVE_VOLUME ? "negative" : mv->type() == ModelVolumeType::PARAMETER_MODIFIER ? "modifier" : "part"},
        {"show_type", !only_part},
        {"warnings", warnings},
        {"error", m_error},
        {"session", m_session},
        {"labels", {{"font", _u8L("Font")}, {"height", _u8L("Height")}, {"depth", _u8L("Depth")}, {"use_surface", _u8L("Use surface")},
                    {"per_glyph", _u8L("Per glyph")}, {"alignment", _u8L("Alignment")}, {"char_gap", _u8L("Char gap")}, {"line_gap", _u8L("Line gap")},
                    {"boldness", _u8L("Boldness")}, {"skew", _u8L("Skew ratio")}, {"from_surface", _u8L("From surface")}, {"rotation", _u8L("Rotation")},
                    {"keep_up", _u8L("Keep up")}, {"collection", _u8L("Collection")}, {"advanced", _u8L("Advanced")}, {"operation", _u8L("Operation")},
                    {"join", _u8L("Join")}, {"cut", _u8L("Cut")}, {"modifier", _u8L("Modifier")}, {"reset", _u8L("Reset")}, {"done", _u8L("Done")},
                    {"face_camera", _u8L("Set text to face camera")}, {"style", _u8L("Style")}, {"save", _u8L("Save")}, {"save_as", _u8L("Save as")},
                    {"rename", _u8L("Rename")}, {"remove", _u8L("Remove")}, {"add", _u8L("Add new style")}, {"points", _u8L("points")}}}};
    return st;
}

// the panel's edits: GLGizmoEmboss draw_* handlers
json TextGizmo::set(const json &a)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    ModelVolume *mv = volume();
    if (mv == nullptr) return state();
    const bool use_inch = Core::get().app_config->get_bool("use_inches");
    const double to_mm = use_inch ? 25.4 : 1.;
    FontProp &fp = m_style.prop;
    const bool final_ = a.value("final", true);
    bool changed = false, relines = false;
    if (a.contains("text")) {
        m_text = a["text"].get<std::string>();
        if (fp.per_glyph && get_count_lines(m_text) != m_text_lines.size()) relines = true;
        changed = true;
    }
    if (a.contains("face")) {   // draw_font_list: select_facename
        if (const Face *f = face_of(a["face"].get<std::string>())) {
            Style s = m_style;
            s.path = f->path;
            s.type = EmbossStyle::Type::file_path;
            s.prop.face_name = f->name;
            s.prop.family = f->family;
            s.prop.collection_number.reset();
            if (load_font(s)) { m_style = s; m_unknown_font = false; changed = true; relines = fp.per_glyph; }
        }
    }
    if (a.contains("height")) {   // draw_height / set_height
        fp.size_in_mm = float(a["height"].get<double>() * to_mm);
        apply_limit(fp.size_in_mm, limits.size_in_mm);
        changed = !is_approx(fp.size_in_mm, mv->text_configuration->style.prop.size_in_mm);
        relines = fp.per_glyph;
    }
    if (a.contains("depth")) {   // draw_depth
        m_style.projection.depth = a["depth"].get<double>() * to_mm;
        apply_limit(m_style.projection.depth, limits.emboss);
        changed = !is_approx(m_style.projection.depth, mv->emboss_shape->projection.depth);
    }
    if (a.contains("use_surface")) {
        m_style.projection.use_surface = a["use_surface"];
        if (m_style.projection.use_surface) m_style.distance.reset();
        changed = true;
    }
    if (a.contains("per_glyph")) { fp.per_glyph = a["per_glyph"]; relines = fp.per_glyph; changed = true; }
    if (a.contains("align_h")) { fp.align.first = FontProp::HorizontalAlign(a["align_h"].get<int>()); relines = fp.per_glyph; changed = true; }
    if (a.contains("align_v")) { fp.align.second = FontProp::VerticalAlign(a["align_v"].get<int>()); relines = fp.per_glyph; changed = true; }
    auto opt_int = [&](const char *k, std::optional<int> &v, const MinMax<int> &l) {
        if (!a.contains(k)) return;
        if (a[k].is_null()) v.reset(); else { v = a[k].get<int>(); apply_limit(v, l); if (*v == 0) v.reset(); }
        changed = true;
    };
    auto opt_float = [&](const char *k, std::optional<float> &v, const MinMax<float> &l) {
        if (!a.contains(k)) return;
        if (a[k].is_null()) v.reset(); else { v = a[k].get<float>(); apply_limit(v, l); if (std::abs(*v) < 1e-6f) v.reset(); }
        changed = true;
    };
    opt_int("char_gap", fp.char_gap, limits.char_gap);
    opt_int("line_gap", fp.line_gap, limits.line_gap);
    if (a.contains("line_gap")) relines = fp.per_glyph;
    opt_float("boldness", fp.boldness, limits.boldness.values);
    opt_float("skew", fp.skew, limits.skew.values);
    if (a.contains("collection")) {
        const int c = a["collection"];
        if (c == 0) fp.collection_number.reset(); else fp.collection_number = unsigned(c);
        changed = true;
    }
    if (a.contains("bold")) {   // draw_bold_button: the bold face of the family, else boldness
        const bool on = a["bold"];
        const Face *cur = fp.face_name ? face_of(*fp.face_name) : nullptr;
        fp.boldness.reset();
        const Face *target = nullptr;
        if (cur)
            for (const Face &f : m_faces)
                if (f.family == cur->family && f.bold == on && f.italic == cur->italic) { target = &f; break; }
        if (target && target != cur) { Style s = m_style; s.path = target->path; s.prop.face_name = target->name; if (load_font(s)) m_style = s; }
        else if (on) fp.boldness = 20.f;
        changed = true;
    }
    if (a.contains("italic")) {   // draw_italic_button: the italic face of the family, else skew
        const bool on = a["italic"];
        const Face *cur = fp.face_name ? face_of(*fp.face_name) : nullptr;
        fp.skew.reset();
        const Face *target = nullptr;
        if (cur)
            for (const Face &f : m_faces)
                if (f.family == cur->family && f.italic == on && f.bold == cur->bold) { target = &f; break; }
        if (target && target != cur) { Style s = m_style; s.path = target->path; s.prop.face_name = target->name; if (load_font(s)) m_style = s; }
        else if (on) fp.skew = 0.2f;
        changed = true;
    }
    Plater &p = Plater::get();
    if (a.contains("distance")) {   // From surface: do_local_z_move, a part only
        const float prev = m_style.distance.value_or(0.f);
        std::optional<float> d = float(a["distance"].get<double>() * to_mm);
        if (std::abs(*d) < 1e-6f) d.reset();
        m_style.distance = d;
        if (fp.per_glyph) changed = true;
        else if (!m_style.projection.use_surface) {
            if (final_) p.take_snapshot(_u8L("Text move"));
            local_z_move(*mv, d.value_or(0.f) - prev);
            p.changed();
        }
    }
    if (a.contains("rotation")) {   // do_local_z_rotate
        double angle_rad = -a["rotation"].get<double>() * M_PI / 180.0;
        Geometry::to_range_pi_pi(angle_rad);
        if (final_) p.take_snapshot(_u8L("Text rotate"));
        local_z_rotate(*mv, m_inst, angle_rad - m_style.angle.value_or(0.f));
        m_style.angle = calc_angle(*mv, m_inst);
        if (fp.per_glyph) relines = true;
        if (m_style.projection.use_surface || fp.per_glyph) changed = true;
        p.changed();
    }
    if (a.contains("keep_up")) m_keep_up = a["keep_up"];
    if (a.contains("face_camera")) {   // face_selected_volume_to_camera
        p.take_snapshot(_u8L("Face the camera"));
        face_camera(*mv, m_inst, Viewport::get().camera(), m_keep_up);
        m_style.angle = calc_angle(*mv, m_inst);
        p.changed();
        if (m_style.projection.use_surface || fp.per_glyph) changed = true;
    }
    if (a.contains("type")) {   // draw_model_type
        const std::string t = a["type"];
        const ModelVolumeType nt = t == "negative" ? ModelVolumeType::NEGATIVE_VOLUME : t == "modifier" ? ModelVolumeType::PARAMETER_MODIFIER : ModelVolumeType::MODEL_PART;
        if (change_type(*mv, m_inst, nt, _u8L("Change Text Type"))) changed = true;
    }
    if (relines) reinit_text_lines();
    if (changed && final_) process(true);
    else if (changed) process(false);
    Viewport::get().request_redraw();
    return state();
}

// the style list: draw_style_list, save / save as / rename / remove / add, Reset
json TextGizmo::style_command(const json &a)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    init_styles();
    const std::string what = a.value("what", std::string());
    auto unique = [this](std::string name) {   // make_unique_name
        auto exists = [this](const std::string &n) { return std::any_of(m_styles.begin(), m_styles.end(), [&](const Style &s) { return s.name == n; }); };
        if (!exists(name)) return name;
        for (int k = 1;; ++k) if (!exists(name + " (" + std::to_string(k) + ")")) return name + " (" + std::to_string(k) + ")";
    };
    bool reprocess = false;
    if (what == "select") {
        const size_t i = a.value("index", 0);
        if (i < m_styles.size() && load_font(m_styles[i])) {
            // fix_transformation: the angle / distance of the new style move the volume
            m_style = m_styles[i];
            m_style_index = i;
            reprocess = true;
        }
    } else if (what == "save") {
        if (m_style_index < m_styles.size()) m_styles[m_style_index] = m_style;
        else { m_style.name = unique(m_style.name); m_style_index = m_styles.size(); m_styles.push_back(m_style); }
        store_styles(true);
    } else if (what == "save_as" || what == "add") {
        Style s = m_style;
        s.name = unique(a.value("name", what == "add" ? _u8L("Custom") : m_style.name));
        m_styles.push_back(s);
        m_style = s;
        m_style_index = m_styles.size() - 1;
        store_styles(true);
        reprocess = true;   // the volume stores the style name
    } else if (what == "rename") {
        const std::string name = a.value("name", std::string());
        if (!name.empty() && m_style_index < m_styles.size()) {
            m_styles[m_style_index].name = unique(name);
            m_style.name = m_styles[m_style_index].name;
            store_styles(true);
            reprocess = true;
        }
    } else if (what == "remove") {
        if (m_style_index < m_styles.size() && m_styles.size() > 1) {
            m_styles.erase(m_styles.begin() + m_style_index);
            m_style_index = std::min(m_style_index, m_styles.size() - 1);
            m_style = m_styles[m_style_index];
            load_font(m_style);
            store_styles(true);
            reprocess = true;
        }
    } else if (what == "reset") {   // reset_to_default_style: the stored style, keeping the text and operation
        if (m_style_index < m_styles.size()) { m_style = m_styles[m_style_index]; load_font(m_style); reprocess = true; }
    }
    if (reprocess) {
        if (m_style.prop.per_glyph) reinit_text_lines(); else m_text_lines.clear();
        process(true);
    }
    return state();
}

// --- moving the text over the surface (SurfaceDrag)

bool TextGizmo::drag_begin(const Camera &cam, double px, double py)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    ModelVolume *mv = volume();
    return mv != nullptr && m_drag.begin(*mv, m_inst, cam, px, py, m_keep_up);
}

void TextGizmo::drag_move(const Camera &cam, double px, double py)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (ModelVolume *mv = volume()) m_drag.move(*mv, m_inst, cam, px, py, m_keep_up);
}

void TextGizmo::drag_end()
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    ModelVolume *mv = volume();
    if (mv == nullptr) { m_drag.reset(); return; }
    m_drag.end(*mv);
    m_style.distance.reset();   // the text lies on the surface
    m_style.angle = calc_angle(*mv, m_inst);
    if (m_style.projection.use_surface || m_style.prop.per_glyph) {
        if (m_style.prop.per_glyph) reinit_text_lines();
        process(false);
    } else
        Plater::get().changed();
}

} // namespace

bool text_gizmo_drag_begin(const Camera &cam, double px, double py) { return TextGizmo::get().drag_begin(cam, px, py); }
void text_gizmo_drag_move(const Camera &cam, double px, double py) { TextGizmo::get().drag_move(cam, px, py); }
void text_gizmo_drag_end() { TextGizmo::get().drag_end(); }
bool text_gizmo_ring(Transform3d &frame, double &radius) { return TextGizmo::get().active() && TextGizmo::get().ring(frame, radius); }
void text_gizmo_ring_rotate(double delta) { TextGizmo::get().ring_rotate(delta); }
bool text_gizmo_active() { return TextGizmo::get().active(); }
void text_gizmo_close() { TextGizmo::get().close(); }

static Commands reg({
    {"text.open", [](const json &a) { return TextGizmo::get().open(a); }},
    {"text.state", [](const json &) { return TextGizmo::get().state(); }},
    {"text.set", [](const json &a) { return TextGizmo::get().set(a); }},
    {"text.style", [](const json &a) { return TextGizmo::get().style_command(a); }},
    {"text.fonts", [](const json &) { return TextGizmo::get().fonts(); }},
});

} // namespace orca
