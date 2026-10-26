#include "Painter.hpp"

#include <GLES3/gl3.h>
#include <cfloat>
#include <cmath>

#include "Core.hpp"
#include "wx/app.h"
#include "Host.hpp"
#include "Plater.hpp"
#include "Shaders.hpp"
#include "Viewport.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;

namespace orca {

void texture_disp_changed();   // TextureDisp.cpp: the preview follows the paint

// TriangleSelectorGUI / GLVolume colours
static const ColorRGBA NEUTRAL_COLOR   = {0.8f, 0.8f, 0.8f, 1.0f};
static const ColorRGBA ENFORCERS_COLOR = {0.5f, 1.f, 0.5f, 1.f};
static const ColorRGBA BLOCKERS_COLOR  = {1.f, 0.5f, 0.5f, 1.f};

// GUI_App.cpp build_plate_tilt_up_direction
Vec3f tilt_up_direction()
{
    const DynamicPrintConfig &cfg = Core::get().preset_bundle->printers.get_edited_preset().config;
    const auto *opt_x = cfg.option<ConfigOptionFloat>("build_plate_tilt_x");
    const auto *opt_y = cfg.option<ConfigOptionFloat>("build_plate_tilt_y");
    const double tilt_x = opt_x != nullptr ? opt_x->value : 0., tilt_y = opt_y != nullptr ? opt_y->value : 0.;
    if (tilt_x == 0. && tilt_y == 0.) return Vec3f::UnitZ();
    return Vec3d(std::tan(Geometry::deg2rad(tilt_y)), std::tan(Geometry::deg2rad(tilt_x)), 1.).normalized().cast<float>();
}

Painter &Painter::get()
{
    static Painter p;
    return p;
}

std::string Painter::kind()
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_kind;
}

bool Painter::active()
{
    std::lock_guard<std::mutex> l(m_mutex);
    return !m_kind.empty() && m_obj >= 0;
}

int Painter::object()
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_kind.empty() ? -1 : m_obj;
}

void Painter::set_kind(const std::string &kind)
{
    // GLGizmosManager::open_gizmo: the seam, support and fuzzy skin painters turn the realistic view off while open
    if (AppConfig *cfg = Core::get().app_config.get()) {
        static bool restore_realistic = false;
        if (kind.empty()) {
            if (restore_realistic) { cfg->set_bool(SETTING_OPENGL_REALISTIC_MODE, true); cfg->save(); restore_realistic = false; }
        } else if (kind == "seam" || kind == "support" || kind == "fuzzy") {
            if (cfg->get_bool(SETTING_OPENGL_REALISTIC_MODE)) {
                restore_realistic = true;
                cfg->set_bool(SETTING_OPENGL_REALISTIC_MODE, false);
                cfg->save();
            }
        } else
            restore_realistic = false;
        Viewport::get().update_view_presets();
    }
    std::lock_guard<std::mutex> l(m_mutex);
    m_kind = kind;
    m_parts.clear();
    m_preview.reset();
    m_preview_runs.clear();
    m_preview_vol = -1;
    m_obj = -1;
    m_stroking = false;
    // GLGizmoMmuSegmentation / FuzzySkin / Seam: the tools they offer, the brush as the default
    m_tool = "circle";
    if (!kind.empty()) load();
    m_up = kind.empty() ? Vec3f::UnitZ() : tilt_up_direction();
    // GLGizmoFdmSupports::on_render_input_window: the highlight starts at get_selection_support_threshold_angle
    if (kind == "support" && m_obj >= 0) {
        const DynamicPrintConfig &obj_cfg = Plater::get().model().objects[m_obj]->config.get();
        const DynamicPrintConfig &glb_cfg = Core::get().preset_bundle->prints.get_edited_preset().config;
        const bool enable_support = obj_cfg.option("enable_support") ? obj_cfg.opt_bool("enable_support") : glb_cfg.opt_bool("enable_support");
        const SupportType support_type = obj_cfg.option("support_type") ? obj_cfg.opt_enum<SupportType>("support_type") : glb_cfg.opt_enum<SupportType>("support_type");
        const int angle = obj_cfg.option("support_threshold_angle") ? obj_cfg.opt_int("support_threshold_angle") : glb_cfg.opt_int("support_threshold_angle");
        m_highlight_angle = enable_support && is_auto(support_type) ? float(angle) : 0.f;
    } else
        m_highlight_angle = 0.f;
}

// the extruder colours (MMU) or the enforcer / blocker colours, indexed by EnforcerBlockerType
std::vector<ColorRGBA> Painter::state_colors() const
{
    if (m_kind != "mmu") return {NEUTRAL_COLOR, ENFORCERS_COLOR, BLOCKERS_COLOR};
    std::vector<ColorRGBA> c;
    if (auto *fc = Core::get().full_config().option<ConfigOptionStrings>("filament_colour"))
        for (auto &s : fc->values) { ColorRGBA x; c.push_back(decode_color(s, x) ? x : NEUTRAL_COLOR); }
    if (c.empty()) c.push_back(NEUTRAL_COLOR);
    return c;
}

// GLGizmoPainterBase::update_from_model_object: the selected single instance, one selector per model part
void Painter::load()
{
    Plater &p = Plater::get();
    const Model &m = p.model();
    m_obj = -1;
    if (p.selection.empty()) return;
    const SelItem &s = *p.selection.begin();
    if (s.obj < 0 || s.obj >= int(m.objects.size())) return;
    m_obj = s.obj;
    m_inst = std::max(0, s.inst);
    const ModelObject *mo = m.objects[m_obj];
    const std::vector<ColorRGBA> colors = state_colors();
    for (int vi = 0; vi < int(mo->volumes.size()); ++vi) {
        const ModelVolume *mv = mo->volumes[vi];
        if (!mv->is_model_part()) continue;
        Part part;
        part.vol = vi;
        part.mesh = mv->mesh_ptr();
        part.selector = std::make_unique<SelectorPatch>(*part.mesh);
        part.matrix = mo->instances[m_inst]->get_matrix() * mv->get_matrix();
        const FacetsAnnotation &fa = facets(*const_cast<ModelVolume *>(mv));
        // MMU: up to the filament count; the rest stays as they are. A texture layer's mask has the one state
        // (GLGizmoTextureDisplacement::update_from_model_object: a higher state is reset to NONE)
        part.selector->deserialize(fa.get_data(), false,
                                   m_kind == "mmu" ? EnforcerBlockerType(std::min<size_t>(colors.size(), size_t(EnforcerBlockerType::ExtruderMax)))
                                   : m_kind == "texture" ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::ExtruderMax);
        // rebuild_other_paint_overlay: every other layer's paint, so all layers stay visible while one is edited
        if (m_kind == "texture") {
            indexed_triangle_set others;
            for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
                if (slot == m_slot || mv->texture_displacement_facet(slot).empty()) continue;
                TriangleSelector sel(*part.mesh);
                sel.deserialize(mv->texture_displacement_facet(slot).get_data(), false, EnforcerBlockerType::ENFORCER);
                its_merge(others, sel.get_facets(EnforcerBlockerType::ENFORCER));
            }
            if (!others.indices.empty()) part.others = std::make_shared<const TriangleMesh>(std::move(others));
        }
        rebuild_drawn(part);
        m_parts.push_back(std::move(part));
    }
}

FacetsAnnotation &Painter::facets(ModelVolume &mv) const
{
    return m_kind == "support" ? mv.supported_facets : m_kind == "seam" ? mv.seam_facets : m_kind == "mmu" ? mv.mmu_segmentation_facets
         : m_kind == "texture" ? mv.texture_displacement_facet(std::clamp(m_slot, 0, int(TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1))
                               : mv.fuzzy_skin_facets;
}

// TriangleSelectorGUI::update_render_data: the facets of each state as their own mesh
void Painter::rebuild_drawn(Part &part)
{
    part.drawn.clear();
    const std::vector<ColorRGBA> colors = state_colors();
    const ModelObject *mo = Plater::get().model().objects[m_obj];
    // MMU: the unpainted triangles in the part's own filament colour (ebt_colors[0])
    ColorRGBA base = NEUTRAL_COLOR;
    if (m_kind == "mmu") {
        int e = mo->volumes[part.vol]->extruder_id();
        base = colors[std::clamp(e > 0 ? e - 1 : 0, 0, int(colors.size()) - 1)];
    }
    auto state_color = [&](int st) { return st == 0 ? base : m_kind == "mmu" ? colors[std::min(st - 1, int(colors.size()) - 1)] : colors[std::min(st, 2)]; };
    if (m_kind == "mmu" && m_tool == "gap_fill") {
        // TriangleSelectorPatch with the filter state: one mesh per patch, the fragments in their neighbour's colour
        part.selector->clear_patches();
        part.selector->update_triangles_per_patch();
        for (const TrianglePatch &patch : part.selector->patches()) {
            indexed_triangle_set its;
            for (size_t i = 0; i + 2 < patch.patch_vertices.size(); i += 3)
                its.vertices.emplace_back(patch.patch_vertices[i], patch.patch_vertices[i + 1], patch.patch_vertices[i + 2]);
            for (size_t i = 0; i + 2 < patch.triangle_indices.size(); i += 3)
                its.indices.emplace_back(patch.triangle_indices[i], patch.triangle_indices[i + 1], patch.triangle_indices[i + 2]);
            if (its.indices.empty()) continue;
            ColorRGBA c;
            if (patch.is_fragment() && !patch.neighbor_types.empty()) {
                c = state_color(int(*patch.neighbor_types.begin()));
                c.a(0.85f);
            } else
                c = state_color(int(patch.type));
            part.drawn.push_back({c, std::make_shared<const TriangleMesh>(std::move(its))});
        }
        return;
    }
    const int max_state = m_kind == "mmu" ? int(colors.size()) : 2;
    for (int st = 0; st <= max_state; ++st) {
        indexed_triangle_set its = part.selector->get_facets(EnforcerBlockerType(st));
        if (its.indices.empty()) continue;
        part.drawn.push_back({state_color(st), std::make_shared<const TriangleMesh>(std::move(its))});
    }
}

void Painter::set(const json &a)
{
    std::lock_guard<std::mutex> l(m_mutex);
    const std::string old_tool = m_tool;
    const float old_gap = SelectorPatch::gap_area;
    if (a.contains("tool")) m_tool = a["tool"].get<std::string>();
    if (a.contains("gap_area")) SelectorPatch::gap_area = std::clamp(a["gap_area"].get<float>(), SelectorPatch::GapAreaMin, SelectorPatch::GapAreaMax);
    // CursorRadiusMin / Max, SmartFillAngleMin / Max, CursorHeightMin / Max
    if (a.contains("radius")) m_cursor_radius = std::clamp(a["radius"].get<float>(), 0.4f, 8.f);
    if (a.contains("smart_fill_angle")) m_smart_fill_angle = std::clamp(a["smart_fill_angle"].get<float>(), 0.f, 90.f);
    if (a.contains("height")) m_cursor_height = std::clamp(a["height"].get<float>(), 0.1f, 8.f);
    if (a.contains("mode")) m_mode = a["mode"];
    if (a.contains("extruder")) m_extruder = a["extruder"];
    if (a.contains("overhangs_only")) m_overhangs_only = a["overhangs_only"];
    if (a.contains("highlight_angle")) m_highlight_angle = a["highlight_angle"];
    if (a.contains("vertical_only")) { m_vertical_only = a["vertical_only"]; if (m_vertical_only) m_horizontal_only = false; }
    if (a.contains("horizontal_only")) { m_horizontal_only = a["horizontal_only"]; if (m_horizontal_only) m_vertical_only = false; }
    // MMU: the base colours follow the filaments; the gap fill tool shows the patches (tool_changed / set_filter_state)
    if (m_kind == "mmu" && (a.contains("extruder") || (old_tool != m_tool && (old_tool == "gap_fill" || m_tool == "gap_fill")) ||
                            old_gap != SelectorPatch::gap_area))
        for (Part &p : m_parts) rebuild_drawn(p);
}

json Painter::state()
{
    std::lock_guard<std::mutex> l(m_mutex);
    json colors = json::array();
    if (m_kind == "mmu")
        if (auto *fc = Core::get().full_config().option<ConfigOptionStrings>("filament_colour")) colors = fc->values;
    return {{"kind", m_kind}, {"object", m_obj}, {"tool", m_tool}, {"radius", m_cursor_radius}, {"smart_fill_angle", m_smart_fill_angle},
            {"height", m_cursor_height}, {"mode", m_mode}, {"extruder", m_extruder}, {"overhangs_only", m_overhangs_only},
            {"highlight_angle", m_highlight_angle}, {"vertical_only", m_vertical_only}, {"horizontal_only", m_horizontal_only},
            {"colors", colors}, {"gap_area", SelectorPatch::gap_area}};
}

// update_raycast_cache: the closest hit over the model parts, in the part's mesh coordinates
bool Painter::raycast(const Camera &cam, double px, double py, int &part, Vec3f &hit, int &facet)
{
    Vec3d o, d;
    cam.ray(px, py, o, d);
    double best = 1e30;
    part = -1;
    Vec3d cut_n;
    double cut_d;
    const bool cut_on = Viewport::get().clipping_plane(cut_n, cut_d);   // the clipped part is not painted
    for (int i = 0; i < int(m_parts.size()); ++i) {
        const Part &p = m_parts[i];
        const Transform3d inv = p.matrix.inverse();
        const Vec3d lo = inv * o, ld = inv.linear() * d;
        const auto &its = p.mesh->its;
        for (int f = 0; f < int(its.indices.size()); ++f) {
            const Vec3d a = its.vertices[its.indices[f][0]].cast<double>(), b = its.vertices[its.indices[f][1]].cast<double>(),
                        c = its.vertices[its.indices[f][2]].cast<double>();
            Vec3d e1 = b - a, e2 = c - a, pv = ld.cross(e2);
            double det = e1.dot(pv);
            if (std::abs(det) < 1e-12) continue;
            Vec3d tv = lo - a;
            double u = tv.dot(pv) / det;
            if (u < 0 || u > 1) continue;
            Vec3d q = tv.cross(e1);
            double v = ld.dot(q) / det;
            if (v < 0 || u + v > 1) continue;
            double t = e2.dot(q) / det;
            if (t <= 0) continue;
            const Vec3d world = p.matrix * Vec3d(lo + t * ld);
            if (cut_on && cut_n.dot(world) > cut_d) continue;
            double tw = (world - o).norm();
            if (tw < best) { best = tw; part = i; hit = Vec3d(lo + t * ld).cast<float>(); facet = f; }
        }
    }
    return part >= 0;
}

EnforcerBlockerType Painter::new_state() const
{
    if (m_mode == 2) return EnforcerBlockerType::NONE;   // shift: erase
    // GLGizmoTextureDisplacement: the left button paints the active layer (erases in Erase mode), the right one the other
    if (m_kind == "texture") return m_mode == 1 ? EnforcerBlockerType::NONE : EnforcerBlockerType::ENFORCER;
    if (m_kind == "mmu") return EnforcerBlockerType(m_extruder + 1);
    if (m_kind == "fuzzy") return m_mode == 1 ? EnforcerBlockerType::NONE : EnforcerBlockerType::FUZZY_SKIN;
    return m_mode == 1 ? EnforcerBlockerType::BLOCKER : EnforcerBlockerType::ENFORCER;
}

// GLGizmoPainterBase::gizmo_event (LeftDown / Dragging) for the brush and the fill tools
void Painter::apply(const Camera &cam, double px, double py, bool first)
{
    Vec2d mouse(px, py);
    if (!first && (m_tool == "circle" || m_tool == "sphere")) {
        if (m_vertical_only) mouse.x() = m_first.x();
        else if (m_horizontal_only) mouse.y() = m_first.y();
    }
    // get_projected_mouse_positions: in-between positions every pixel so a fast stroke has no gaps
    std::vector<Vec2d> positions{mouse};
    if (m_has_last && !first) {
        const size_t between = size_t((mouse - m_last).norm() / 1.0);
        if (between > 0) {
            const Vec2d diff = (m_last - mouse) / double(between + 1);
            for (size_t k = 1; k <= between; ++k) positions.emplace_back(mouse + double(k) * diff);
            positions.emplace_back(m_last);
        }
    }
    struct Hit { int part; Vec3f hit; int facet; };
    std::vector<Hit> hits;
    for (const Vec2d &mp : positions) {
        Hit h;
        if (!raycast(cam, mp.x(), mp.y(), h.part, h.hit, h.facet)) break;
        hits.push_back(h);
    }
    m_has_last = false;
    if (hits.empty()) return;
    const EnforcerBlockerType state = new_state();
    Vec3d cut_n;
    double cut_d;
    const bool cut_on = Viewport::get().clipping_plane(cut_n, cut_d);
    const float highlight = m_overhangs_only ? m_highlight_angle : 0.f;
    std::set<int> touched;
    for (size_t i = 0; i < hits.size(); ++i) {
        Part &p = m_parts[hits[i].part];
        const Transform3d trafo_nt = Transform3d(p.matrix.linear());
        // GLGizmoPainterBase::get_clipping_plane_in_volume_coordinates
        TriangleSelector::ClippingPlane clp;
        if (cut_on) {
            const Vec3d n = Transform3d(p.matrix.linear().transpose()) * cut_n;
            const float offset = float((p.matrix.inverse() * Vec3d(cut_n * cut_d)).dot(n));
            clp = TriangleSelector::ClippingPlane({float(n.x()), float(n.y()), float(n.z()), offset});
        }
        const Vec3f camera_pos = (p.matrix.inverse() * cam.eye()).cast<float>();
        if (m_tool == "fill" || m_tool == "smart_fill" || m_tool == "pointer") {
            // seed_fill_select_triangles / bucket_fill_select_triangles, applied at once on a tap
            if (m_tool == "smart_fill")
                p.selector->seed_fill_select_triangles(hits[i].hit, hits[i].facet, trafo_nt, clp, m_smart_fill_angle, highlight, m_up, true);
            else if (m_tool == "pointer")
                p.selector->bucket_fill_select_triangles(hits[i].hit, hits[i].facet, clp, -1.f, false, true);
            else
                p.selector->bucket_fill_select_triangles(hits[i].hit, hits[i].facet, clp, m_smart_fill_angle, true, true);
            p.selector->seed_fill_apply_on_triangles(state);
        } else if (m_tool == "height_range") {
            const float z_world = float((p.matrix * hits[i].hit.cast<double>()).z());
            auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(z_world, camera_pos, m_cursor_height, p.matrix, clp);
            p.selector->select_patch(hits[i].facet, std::move(cursor), state, trafo_nt, true, highlight, m_up);
        } else {
            const auto ctype = m_tool == "sphere" ? TriangleSelector::SPHERE : TriangleSelector::CIRCLE;
            if (i + 1 < hits.size() && hits[i + 1].part == hits[i].part) {
                auto cursor = TriangleSelector::DoublePointCursor::cursor_factory(hits[i].hit, hits[i + 1].hit, camera_pos, m_cursor_radius, ctype, p.matrix, clp);
                p.selector->select_patch(hits[i].facet, std::move(cursor), state, trafo_nt, true, highlight, m_up);
            } else if (hits.size() == 1) {
                auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(hits[i].hit, camera_pos, m_cursor_radius, ctype, p.matrix, clp);
                p.selector->select_patch(hits[i].facet, std::move(cursor), state, trafo_nt, true, highlight, m_up);
            }
        }
        touched.insert(hits[i].part);
        if (m_tool == "fill" || m_tool == "smart_fill" || m_tool == "pointer") break;
    }
    for (int t : touched) rebuild_drawn(m_parts[t]);
    m_last = mouse;
    m_has_last = true;
}

bool Painter::stroke_begin(const Camera &cam, double px, double py)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_kind.empty() || m_parts.empty() || m_tool == "gap_fill") return false;   // the gap fill tool does not paint
    int part, facet;
    Vec3f hit;
    if (!raycast(cam, px, py, part, hit, facet)) return false;
    m_stroking = true;
    m_has_last = false;
    m_first = Vec2d(px, py);
    apply(cam, px, py, true);
    Viewport::get().request_redraw();
    return true;
}

void Painter::stroke_move(const Camera &cam, double px, double py)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_stroking) return;
    if (m_tool == "fill" || m_tool == "smart_fill" || m_tool == "pointer") return;   // fills act once
    apply(cam, px, py, false);
    Viewport::get().request_redraw();
}

// GLGizmoFdmSupports / Seam / MmuSegmentation / FuzzySkin::update_model_object
void Painter::stroke_end()
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_stroking) return;
    m_stroking = false;
    m_has_last = false;
    std::unique_lock<std::recursive_mutex> core(Core::get().mutex);
    Plater &p = Plater::get();
    Model &m = p.model();
    if (m_obj < 0 || m_obj >= int(m.objects.size())) return;
    ModelObject *mo = m.objects[m_obj];
    const char *name = m_kind == "support" ? "Support painting" : m_kind == "seam" ? "Seam painting" : m_kind == "mmu" ? "MMU segmentation"
                     : m_kind == "texture" ? "Texture displacement editing" : "Fuzzy skin painting";
    p.take_snapshot(name);
    bool updated = false;
    for (Part &part : m_parts) {
        if (part.vol >= int(mo->volumes.size())) continue;
        ModelVolume *mv = mo->volumes[part.vol];
        updated |= facets(*mv).set(*part.selector);
    }
    if (m_kind == "texture") {
        // GLGizmoTextureDisplacement::update_model_object: the preview follows every stroke (once the painter's lock
        // is released: the gizmo asks the painter for its object)
        wx_call_after([] {
            std::lock_guard<std::recursive_mutex> lk(Core::get().mutex);
            texture_disp_changed();
        });
        if (updated) p.changed();
        return;
    }
    if (updated) {
        mo->invalidate_bounding_box();
        core.unlock();
        p.changed();
    }
}

void Painter::set_slot(int slot)
{
    std::lock_guard<std::mutex> l(m_mutex);
    m_slot = slot;
    m_parts.clear();
    m_stroking = false;
    if (!m_kind.empty()) load();
}

// GLGizmoTextureDisplacement::update_model_object: the selectors into the active layer's mask, no snapshot
void Painter::flush()
{
    std::lock_guard<std::mutex> l(m_mutex);
    Model &m = Plater::get().model();
    if (m_obj < 0 || m_obj >= int(m.objects.size())) return;
    ModelObject *mo = m.objects[m_obj];
    for (Part &part : m_parts)
        if (part.vol < int(mo->volumes.size())) facets(*mo->volumes[part.vol]).set(*part.selector);
}

// GLGizmoTextureDisplacement::select_whole_model and its Erase whole model
void Painter::fill_all(bool paint)
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (Part &part : m_parts) {
            if (paint)
                for (int i = 0; i < int(part.mesh->its.indices.size()); ++i) part.selector->set_facet(i, EnforcerBlockerType::ENFORCER);
            else
                part.selector->reset();
            rebuild_drawn(part);
        }
        m_stroking = true;
    }
    stroke_end();
    Viewport::get().request_redraw();
}

void Painter::set_lines(const std::string &key, std::vector<Vec3f> lines)
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        LineSet &ls = m_lines[key];
        ls.lines = std::move(lines);
        ls.dirty = true;
    }
    Viewport::get().request_redraw();
    Host::event("redraw", json::object());
}

void Painter::set_shader_mesh(const std::string &key, std::optional<ShaderMesh> mesh)
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (mesh) m_shader_meshes[key] = std::move(*mesh);
        else m_shader_meshes.erase(key);
    }
    Viewport::get().request_redraw();
    Host::event("redraw", json::object());
}

void Painter::set_texture(int id, std::shared_ptr<const std::vector<unsigned char>> rgba, int w, int h)
{
    std::lock_guard<std::mutex> l(m_mutex);
    Tex &t = m_textures[id];
    if (t.gl) m_dead_textures.push_back(t.gl);
    t = Tex{std::move(rgba), w, h, 0};
}

void Painter::drop_texture(int id)
{
    std::lock_guard<std::mutex> l(m_mutex);
    auto it = m_textures.find(id);
    if (it == m_textures.end()) return;
    if (it->second.gl) m_dead_textures.push_back(it->second.gl);
    m_textures.erase(it);
}

indexed_triangle_set Painter::facets_strict(int part, EnforcerBlockerType state, std::vector<int> *source)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (part < 0 || part >= int(m_parts.size())) return {};
    return m_parts[size_t(part)].selector->get_facets_strict(state, source);
}

// GLModel::render with the recorded shader: the camera's matrices of this frame, the textures bound, the mesh drawn
void Painter::draw_shader_mesh(const std::string &key, const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj, const Transform3d &part)
{
    auto it = m_shader_meshes.find(key);
    if (it == m_shader_meshes.end() || !it->second.data || it->second.data->empty()) return;
    const ShaderMesh &m = it->second;
    GLuint prog = gl::orca_program(m.shader);
    if (!prog) return;
    GpuMesh &g = m_shader_gpu[key];
    if (g.data != m.data) {
        if (!g.vao) { glGenVertexArrays(1, &g.vao); glGenBuffers(1, &g.vbo); }
        glBindVertexArray(g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        glBufferData(GL_ARRAY_BUFFER, m.data->size() * sizeof(float), m.data->data(), GL_STATIC_DRAW);
        const GLint lp = glGetAttribLocation(prog, "v_position"), ln = glGetAttribLocation(prog, "v_normal"), lt = glGetAttribLocation(prog, "v_tex_coord");
        if (lp >= 0) { glEnableVertexAttribArray(GLuint(lp)); glVertexAttribPointer(GLuint(lp), 3, GL_FLOAT, GL_FALSE, 32, (void *) 0); }
        if (ln >= 0) { glEnableVertexAttribArray(GLuint(ln)); glVertexAttribPointer(GLuint(ln), 3, GL_FLOAT, GL_FALSE, 32, (void *) 12); }
        if (lt >= 0) { glEnableVertexAttribArray(GLuint(lt)); glVertexAttribPointer(GLuint(lt), 2, GL_FLOAT, GL_FALSE, 32, (void *) 24); }
        glBindVertexArray(0);
        g.data = m.data;
    }
    glUseProgram(prog);
    for (const auto &[name, u] : m.uniforms) {
        const GLint loc = glGetUniformLocation(prog, name.c_str());
        if (loc < 0) continue;
        switch (u.type) {
        case 0: glUniform1i(loc, u.i.empty() ? 0 : u.i[0]); break;
        case 1: glUniform1f(loc, u.f[0]); break;
        case 2: glUniform2f(loc, u.f[0], u.f[1]); break;
        case 3: glUniform3f(loc, u.f[0], u.f[1], u.f[2]); break;
        case 4: glUniform4f(loc, u.f[0], u.f[1], u.f[2], u.f[3]); break;
        case 9: glUniformMatrix3fv(loc, 1, GL_FALSE, u.f.data()); break;
        case 16: glUniformMatrix4fv(loc, 1, GL_FALSE, u.f.data()); break;
        }
    }
    // this frame's camera
    const Eigen::Matrix4f model = part.matrix().cast<float>();
    const Eigen::Matrix4f vm = view * model;
    const Eigen::Matrix3f vn = view.block<3, 3>(0, 0);
    glUniformMatrix4fv(glGetUniformLocation(prog, "view_model_matrix"), 1, GL_FALSE, vm.data());
    glUniformMatrix4fv(glGetUniformLocation(prog, "projection_matrix"), 1, GL_FALSE, proj.data());
    glUniformMatrix3fv(glGetUniformLocation(prog, "view_normal_matrix"), 1, GL_FALSE, vn.data());
    if (auto ta = m.uniforms.find("tex_anchor"); ta != m.uniforms.end()) {
        const Eigen::Matrix4f inv = view.inverse();
        const Vec3f eye(inv(0, 3), inv(1, 3), inv(2, 3));
        glUniform3f(glGetUniformLocation(prog, "eye_model_pos"), eye.x() - ta->second.f[0], eye.y() - ta->second.f[1], eye.z() - ta->second.f[2]);
    }
    for (const auto &[unit, id] : m.textures) {
        auto t = m_textures.find(id);
        if (t == m_textures.end() || !t->second.rgba) continue;
        if (!t->second.gl) {
            glGenTextures(1, &t->second.gl);
            glBindTexture(GL_TEXTURE_2D, t->second.gl);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->second.w, t->second.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->second.rgba->data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        glActiveTexture(GL_TEXTURE0 + GLenum(unit));
        glBindTexture(GL_TEXTURE_2D, t->second.gl);
        if (m.wrap) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, m.wrap);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, m.wrap);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(m.data->size() / 8));
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Painter::set_overlay(std::vector<OverlayPrim> prims)
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_overlay = std::move(prims);
    }
    Viewport::get().request_redraw();
    Host::event("redraw", json::object());
}

void Painter::set_wireframe(bool on)
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_wireframe = on;
    }
    Viewport::get().request_redraw();
}

void Painter::edit_part(int part, const std::function<void(TriangleSelector &)> &fn)
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (part < 0 || part >= int(m_parts.size())) return;
        fn(*m_parts[size_t(part)].selector);
        rebuild_drawn(m_parts[size_t(part)]);
    }
    Viewport::get().request_redraw();
}

void Painter::set_preview(int vol, std::shared_ptr<const TriangleMesh> mesh, std::vector<PreviewRun> runs)
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_preview_vol = vol;
        m_preview = std::move(mesh);
        m_preview_runs = std::move(runs);
    }
    Viewport::get().request_redraw();
    Host::event("redraw", json::object());
}

// "Erase all" (GLGizmoFdmSupports::on_render_input_window, remove_all)
void Painter::erase_all()
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (Part &part : m_parts) {
            part.selector->reset();
            rebuild_drawn(part);
        }
        m_stroking = true;
    }
    stroke_end();
    Viewport::get().request_redraw();
}

// GLGizmoPainterBase::render_triangles: the parts with the "mm_gouraud" shader (slope = overhang highlight)
void Painter::gap_fill()
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (Part &part : m_parts) {
            part.selector->update_selector_triangles();
            rebuild_drawn(part);
        }
        m_stroking = true;
    }
    stroke_end();   // update_model_object
    Viewport::get().request_redraw();
}

void Painter::render(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_kind.empty()) return;
    GLuint prog = gl::orca_program("mm_gouraud");
    if (!prog) return;
    glUseProgram(prog);
    auto U = [prog](const char *n) { return glGetUniformLocation(prog, n); };
    glUniformMatrix4fv(U("projection_matrix"), 1, GL_FALSE, proj.data());
    glUniform2f(U("z_range"), -FLT_MAX, FLT_MAX);
    // GLGizmoPainterBase::get_clipping_plane_data: the object clipper the section view moves
    Vec3d cut_n;
    double cut_d;
    if (Viewport::get().clipping_plane(cut_n, cut_d)) glUniform4f(U("clipping_plane"), float(-cut_n.x()), float(-cut_n.y()), float(-cut_n.z()), float(cut_d));
    else glUniform4f(U("clipping_plane"), 0.f, 0.f, 1.f, FLT_MAX);
    glUniform1i(U("show_wireframe"), m_wireframe ? 1 : 0);
    // GLGizmoFdmSupports uses the canvas slope while open; the other painters do not
    glUniform1i(U("slope.actived"), m_kind == "support" ? 1 : 0);
    glUniform1f(U("slope.normal_z"), -std::cos(float(Geometry::deg2rad(double(m_highlight_angle)))));
    glUniform3f(U("slope.up_direction"), m_up.x(), m_up.y(), m_up.z());
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);   // the gap fill's fragments are drawn at alpha 0.85
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // drop the buffers of meshes that were rebuilt
    for (auto it = m_gpu.begin(); it != m_gpu.end();) {
        if (it->second.owner.expired()) { glDeleteBuffers(1, &it->second.vbo); glDeleteVertexArrays(1, &it->second.vao); it = m_gpu.erase(it); }
        else ++it;
    }
    auto upload = [this](const std::shared_ptr<const TriangleMesh> &mesh) -> Gpu & {
        auto it = m_gpu.find(mesh.get());
        if (it == m_gpu.end()) {
            // un-indexed triangles with barycentric coordinates (TriangleSelectorGUI's render data)
            std::vector<float> data;
            data.reserve(mesh->its.indices.size() * 18);
            for (const Vec3i32 &f : mesh->its.indices)
                for (int k = 0; k < 3; ++k) {
                    const Vec3f &v = mesh->its.vertices[f[k]];
                    data.insert(data.end(), {v.x(), v.y(), v.z(), k == 0 ? 1.f : 0.f, k == 1 ? 1.f : 0.f, k == 2 ? 1.f : 0.f});
                }
            Gpu g;
            g.owner = mesh;
            glGenVertexArrays(1, &g.vao);
            glBindVertexArray(g.vao);
            glGenBuffers(1, &g.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
            glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *) 0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *) 12);
            g.count = GLsizei(data.size() / 6);
            it = m_gpu.emplace(mesh.get(), g).first;
        }
        return it->second;
    };
    if (!m_dead_textures.empty()) { glDeleteTextures(GLsizei(m_dead_textures.size()), m_dead_textures.data()); m_dead_textures.clear(); }
    // render_shaded_preview_mesh: the Fast view's shaded relief in place of the textured part (the first model part)
    const bool shaded = !m_parts.empty() && m_shader_meshes.count("shaded") && !m_stroking;
    if (shaded) {
        draw_shader_mesh("shaded", view, proj, m_parts.front().matrix);
        glUseProgram(prog);
    }
    for (const Part &part : m_parts) {
        if (shaded && &part == &m_parts.front()) continue;
        const Eigen::Matrix4f model = part.matrix.matrix().cast<float>();
        const Eigen::Matrix4f vm = view * model;
        const Eigen::Matrix3f nm = view.block<3, 3>(0, 0) * model.block<3, 3>(0, 0).inverse().transpose();
        const Eigen::Matrix3f wn = model.block<3, 3>(0, 0).inverse().transpose();
        const bool left_handed = model.block<3, 3>(0, 0).determinant() < 0.f;
        glUniformMatrix4fv(U("view_model_matrix"), 1, GL_FALSE, vm.data());
        glUniformMatrix3fv(U("view_normal_matrix"), 1, GL_FALSE, nm.data());
        glUniformMatrix4fv(U("volume_world_matrix"), 1, GL_FALSE, model.data());
        glUniformMatrix3fv(U("slope.volume_world_normal_matrix"), 1, GL_FALSE, wn.data());
        glUniform1i(U("volume_mirrored"), left_handed ? 1 : 0);
        if (left_handed) glFrontFace(GL_CW);
        // GLGizmoTextureDisplacement::render_painter_gizmo: the displaced preview in place of the part; the selector's
        // highlight over it (depth biased) only while a stroke is on
        // under the checker / distortion overlay the undisplaced part is drawn (render_painter_gizmo: use_true_preview)
        const bool preview = m_preview && part.vol == m_preview_vol && !m_shader_meshes.count("uvcheck");
        if (preview) {
            Gpu &g = upload(m_preview);
            glBindVertexArray(g.vao);
            for (const PreviewRun &r : m_preview_runs) {
                glUniform4f(U("uniform_color"), r.color.r(), r.color.g(), r.color.b(), r.color.a());
                glDrawArrays(GL_TRIANGLES, GLint(r.first * 3), GLsizei(r.count * 3));
            }
        }
        if (!preview || m_stroking) {
            if (preview) {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-1.f, -1.f);
            }
            for (const auto &[c, mesh] : part.drawn) {
                Gpu &g = upload(mesh);
                glUniform4f(U("uniform_color"), c.r(), c.g(), c.b(), c.a());
                glBindVertexArray(g.vao);
                glDrawArrays(GL_TRIANGLES, 0, g.count);
            }
            if (preview) glDisable(GL_POLYGON_OFFSET_FILL);
        }
        // render_paint_overlay(m_other_paint_glmodel): translucent, pulled toward the camera, no depth writes
        if (part.others && !preview) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-2.f, -2.f);
            glDepthMask(GL_FALSE);
            Gpu &g = upload(part.others);
            glUniform4f(U("uniform_color"), 0.55f, 0.55f, 0.55f, 0.55f);
            glBindVertexArray(g.vao);
            glDrawArrays(GL_TRIANGLES, 0, g.count);
            glDepthMask(GL_TRUE);
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
        if (left_handed) glFrontFace(GL_CCW);
    }
    glBindVertexArray(0);
    // render_uvcheck_mesh: the checker / distortion over the painted patch, pulled toward the camera
    if (!m_parts.empty() && m_shader_meshes.count("uvcheck")) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.f, -1.f);
        draw_shader_mesh("uvcheck", view, proj, m_parts.front().matrix);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    // render_island_overlay: the UV editor's selected islands, translucent, no depth writes
    if (!m_parts.empty() && m_shader_meshes.count("island")) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-2.f, -2.f);
        glDepthMask(GL_FALSE);
        draw_shader_mesh("island", view, proj, m_parts.front().matrix);
        glDepthMask(GL_TRUE);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    // the line sets with the "flat" shader, on the first model part (the textured volume): render_subdivide_preview's
    // cyan, render_seam_overlay's red seams, yellow hover and green path anchor (over everything while marking)
    if (!m_parts.empty())
        if (GLuint flat = gl::orca_program("flat")) {
            glUseProgram(flat);
            const Eigen::Matrix4f vm = view * m_parts.front().matrix.matrix().cast<float>();
            glUniformMatrix4fv(glGetUniformLocation(flat, "view_model_matrix"), 1, GL_FALSE, vm.data());
            glUniformMatrix4fv(glGetUniformLocation(flat, "projection_matrix"), 1, GL_FALSE, proj.data());
            for (auto &[key, ls] : m_lines) {
                if (ls.dirty) {
                    if (!ls.vao) { glGenVertexArrays(1, &ls.vao); glGenBuffers(1, &ls.vbo); }
                    glBindVertexArray(ls.vao);
                    glBindBuffer(GL_ARRAY_BUFFER, ls.vbo);
                    glBufferData(GL_ARRAY_BUFFER, ls.lines.size() * sizeof(Vec3f), ls.lines.data(), GL_STATIC_DRAW);
                    glEnableVertexAttribArray(0);
                    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *) 0);
                    glBindVertexArray(0);
                    ls.dirty = false;
                }
                if (ls.lines.empty()) continue;
                const ColorRGBA c = key == "seams" ? ColorRGBA(1.0f, 0.15f, 0.15f, 1.0f) : key == "seam_hover" ? ColorRGBA(1.0f, 0.9f, 0.15f, 1.0f)
                                  : key == "seam_anchor" ? ColorRGBA(0.2f, 1.0f, 0.4f, 1.0f) : ColorRGBA(0.2f, 0.9f, 1.0f, 0.7f);
                glUniform4f(glGetUniformLocation(flat, "uniform_color"), c.r(), c.g(), c.b(), c.a());
                const bool on_top = key == "seam_hover" || key == "seam_anchor" || (key == "seams" && m_seams_on_top);
                if (on_top) glDisable(GL_DEPTH_TEST); else glDepthFunc(GL_LEQUAL);
                glBindVertexArray(ls.vao);
                glDrawArrays(GL_LINES, 0, GLsizei(ls.lines.size()));
                glBindVertexArray(0);
                if (on_top) glEnable(GL_DEPTH_TEST); else glDepthFunc(GL_LESS);
            }
        }
    // render_adjust_texture_gizmo: the handle with the "flat" shader, over the model (no depth test)
    if (!m_overlay.empty())
        if (GLuint flat = gl::orca_program("flat")) {
            if (!m_overlay_vao) { glGenVertexArrays(1, &m_overlay_vao); glGenBuffers(1, &m_overlay_vbo); }
            glUseProgram(flat);
            glUniformMatrix4fv(glGetUniformLocation(flat, "view_model_matrix"), 1, GL_FALSE, view.data());
            glUniformMatrix4fv(glGetUniformLocation(flat, "projection_matrix"), 1, GL_FALSE, proj.data());
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glBindVertexArray(m_overlay_vao);
            glBindBuffer(GL_ARRAY_BUFFER, m_overlay_vbo);
            glEnableVertexAttribArray(0);
            for (const OverlayPrim &p : m_overlay) {
                glBufferData(GL_ARRAY_BUFFER, p.points.size() * sizeof(Vec3f), p.points.data(), GL_STREAM_DRAW);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *) 0);
                glUniform4f(glGetUniformLocation(flat, "uniform_color"), p.color.r(), p.color.g(), p.color.b(), p.color.a());
                glDrawArrays(p.lines ? GL_LINES : GL_TRIANGLES, 0, GLsizei(p.points.size()));
            }
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
        }
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
}

// ---------------------------------------------------------------------------------------------------------------

static Commands reg({
    {"paint.set", [](const json &a) {
        if (a.contains("kind")) Painter::get().set_kind(a["kind"].get<std::string>());
        Painter::get().set(a);
        Viewport::get().request_snapshot();
        return Painter::get().state();
    }},
    {"paint.state", [](const json &) { return Painter::get().state(); }},
    {"paint.erase_all", [](const json &) { Painter::get().erase_all(); return Painter::get().state(); }},
    {"paint.gap_fill", [](const json &) { Painter::get().gap_fill(); return Painter::get().state(); }},
});

} // namespace orca
