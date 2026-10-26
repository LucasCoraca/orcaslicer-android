// Published 3MFs: the receiving side of Plater::priv::load_files (the payload rebuilt into the loaded config, the keys
// overlaid on the receiver's own presets by PresetBundle::load_config_model) and the session's published selection.
#include "Published.hpp"

#include <nlohmann/json.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"

namespace orca {

using namespace Slic3r;

PublishedConfig read_published(const Model &model, DynamicPrintConfig &config_loaded, ConfigSubstitutionContext &config_substitutions)
{
    // Plater::priv::load_files, after Model::read_from_archive
    PublishedConfig published_config;
    if (model.model_info != nullptr) {
        auto published_it = model.model_info->metadata_items.find(ORCA_PUBLISHED_TAG);
        if (published_it != model.model_info->metadata_items.end() && is_published_3mf_flag(published_it->second)) {
            published_config.published = true;
            auto keys_it               = model.model_info->metadata_items.find(ORCA_PUBLISHED_KEYS_TAG);
            if (keys_it != model.model_info->metadata_items.end()) {
                try {
                    auto j = nlohmann::json::parse(keys_it->second);
                    if (j.is_array())
                        for (const auto& k : j)
                            if (k.is_string())
                                published_config.published_keys.emplace_back(k.get<std::string>());
                } catch (...) {
                    // Ignore malformed published_keys; the project still loads normally.
                }
            }

            auto material_keys_it = model.model_info->metadata_items.find(ORCA_PUBLISHED_MATERIAL_TAG);
            if (material_keys_it != model.model_info->metadata_items.end()) {
                try {
                    auto jm = nlohmann::json::parse(material_keys_it->second);
                    if (jm.is_array())
                        for (const auto& m : jm) {
                            try {
                                // Malformed entries are isolated so one bad item
                                // cannot discard valid entries that follow it.
                                if (!m.is_object())
                                    continue;
                                PublishedMaterialEntry entry;
                                const auto mat_it = m.find("material");
                                if (mat_it != m.end() && mat_it->is_object()) {
                                    const auto& mat = *mat_it;
                                    if (mat.contains("filament_type") && mat["filament_type"].is_string())
                                        entry.filament_type = mat["filament_type"].get<std::string>();
                                    if (mat.contains("filament_vendor") && mat["filament_vendor"].is_string())
                                        entry.filament_vendor = mat["filament_vendor"].get<std::string>();
                                    if (mat.contains("filament_id") && mat["filament_id"].is_string())
                                        entry.filament_id = mat["filament_id"].get<std::string>();
                                    if (mat.contains("setting_id") && mat["setting_id"].is_string())
                                        entry.setting_id = mat["setting_id"].get<std::string>();
                                    if (mat.contains("name") && mat["name"].is_string())
                                        entry.preset_name = mat["name"].get<std::string>();
                                }
                                if (m.contains("slot") && m["slot"].is_number_integer())
                                    entry.slot = m["slot"].get<int>();
                                const auto entry_keys_it = m.find("keys");
                                if (entry_keys_it != m.end() && entry_keys_it->is_array())
                                    for (const auto& k : *entry_keys_it)
                                        if (k.is_string())
                                            entry.keys.emplace_back(k.get<std::string>());
                                // Fields always written by the current exporter.
                                if (m.contains("full") && m["full"].is_boolean())
                                    entry.full = m["full"].get<bool>();
                                const auto entry_full_keys_it = m.find("full_keys");
                                if (entry_full_keys_it != m.end() && entry_full_keys_it->is_array())
                                    for (const auto& k : *entry_full_keys_it)
                                        if (k.is_string())
                                            entry.full_keys.emplace_back(k.get<std::string>());
                                if (m.contains("publish_type") && m["publish_type"].is_boolean())
                                    entry.publish_type = m["publish_type"].get<bool>();
                                if (m.contains("type") && m["type"].is_string())
                                    entry.publish_type_value = m["type"].get<std::string>();
                                if (m.contains("publish_color") && m["publish_color"].is_boolean())
                                    entry.publish_color = m["publish_color"].get<bool>();
                                if (m.contains("color") && m["color"].is_string())
                                    entry.color = m["color"].get<std::string>();
                                published_config.material_keys.emplace_back(std::move(entry));
                            } catch (const nlohmann::json::exception&) {
                                // Ignore only this malformed material entry.
                            }
                        }
                } catch (const nlohmann::json::exception&) {
                    // Ignore malformed published_material_keys; the project still loads normally.
                }
            }

            // Rebuild the published values from the metadata payload: a published
            // file carries no project_settings.config, so config_loaded is filled
            // from here; a missing or malformed payload leaves it empty and the
            // fallback chain below imports the geometry only.
            auto payload_it = model.model_info->metadata_items.find(ORCA_PUBLISHED_CONFIG_TAG);
            if (payload_it != model.model_info->metadata_items.end()) {
                try {
                    ConfigSubstitutions payload_substitutions =
                        config_loaded.load_from_ini_string(payload_it->second, ForwardCompatibilitySubstitutionRule::Enable);
                    config_substitutions.substitutions.insert(config_substitutions.substitutions.end(),
                                                              std::make_move_iterator(payload_substitutions.begin()),
                                                              std::make_move_iterator(payload_substitutions.end()));
                } catch (...) {
                    // Ignore malformed published_config; the project still loads normally.
                }
            }
        }
    }
    return published_config;
}

void strip_published(Model &model)
{
    if (model.model_info == nullptr) return;
    model.model_info->metadata_items.erase(ORCA_PUBLISHED_TAG);
    model.model_info->metadata_items.erase(ORCA_PUBLISHED_KEYS_TAG);
    model.model_info->metadata_items.erase(ORCA_PUBLISHED_MATERIAL_TAG);
    model.model_info->metadata_items.erase(ORCA_PUBLISHED_CONFIG_TAG);
}

PendingPublished &pending_published()
{
    static PendingPublished p;
    return p;
}

} // namespace orca
