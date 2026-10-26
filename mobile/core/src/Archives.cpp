// File > Import ZIP Archive (Plater::preview_zip_archive with FileArchiveDialog) and File > Import Configs
// (MainFrame::load_config_file with ConfigsOverwriteConfirmDialog).
#include <regex>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace orca {

void presets_changed();

namespace {

// FileArchiveDialog: the model files in the archive (pattern_drop), each with its uncompressed size
json zip_list(const json &a)
{
    const std::string path = str_arg(a, "path");
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, path)) throw CommandError(GUI::format(_u8L("Loading of a ZIP archive on path %1% has failed."), path));
    const std::regex pattern_drop(".*[.](stl|obj|amf|3mf|step|stp)", std::regex::icase);
    json entries = json::array();
    mz_zip_archive_file_stat stat;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
        if (!mz_zip_reader_file_stat(&archive, i, &stat) || stat.m_is_directory) continue;
        const std::string name = decode_archive_entry_path(&archive, stat);
        if (!std::regex_match(fs::path(name).extension().string(), pattern_drop)) continue;
        entries.push_back({{"path", name}, {"size", stat.m_uncomp_size}});
    }
    close_zip_reader(&archive);
    return {{"entries", entries}, {"title", _u8L("Archive preview")}, {"open", _u8L("Open")}, {"cancel", _u8L("Cancel")}};
}

// preview_zip_archive after OK: the selected entries unzipped into the temporary folder (renamed when taken), loaded
json zip_extract(const json &a)
{
    const std::string path = str_arg(a, "path");
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, path)) throw CommandError(GUI::format(_u8L("Loading of a ZIP archive on path %1% has failed."), path));
    const fs::path archive_dir = fs::path(temporary_dir()) / "zip";
    fs::create_directories(archive_dir);
    std::vector<std::string> project_paths;
    mz_zip_archive_file_stat stat;
    for (const auto &sel : a.at("entries")) {
        const fs::path entry = sel.at("path").get<std::string>();
        const size_t size = sel.at("size");
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
            if (!mz_zip_reader_file_stat(&archive, i, &stat) || size != stat.m_uncomp_size) continue;
            if (fs::path(decode_archive_entry_path(&archive, stat)) != entry) continue;
            const std::string extension = entry.extension().string();
            const std::string just_filename = entry.stem().string();
            std::string final_filename = just_filename;
            for (size_t version = 1; fs::exists(archive_dir / (final_filename + extension)); ++version)
                final_filename = just_filename + "(" + std::to_string(version) + ")";
            const fs::path final_path = archive_dir / (final_filename + extension);
            std::string buffer((size_t) stat.m_uncomp_size, 0);
            if (!mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, buffer.data(), buffer.size(), 0)) {
                close_zip_reader(&archive);
                throw CommandError(GUI::format(_u8L("Failed to unzip file to %1%: %2%"), final_path.string(),
                                               mz_zip_get_error_string(mz_zip_get_last_error(&archive))));
            }
            fs::ofstream(final_path, std::ios::binary) << buffer;
            project_paths.push_back(final_path.string());
            break;
        }
    }
    close_zip_reader(&archive);
    if (project_paths.empty()) return Plater::get().state();
    return Core::get().call("project.load", {{"paths", project_paths}});
}

// MainFrame::load_config_file
json import_configs(const json &a)
{
    Core &core = Core::get();
    std::vector<std::string> cfiles;
    for (const auto &f : a.at("paths")) cfiles.push_back(f.get<std::string>());
    core.preset_bundle->import_presets(cfiles, [](std::string const &name) {
            // ConfigsOverwriteConfirmDialog: No / Yes / No to All / Yes to All
            json ans = Host::ask({{"kind", "message"},
                                  {"message", GUI::format(_u8L("A config exists with the same name: %s. Do you want to overwrite it?"), name)},
                                  {"caption", _u8L("Overwrite config")},
                                  {"buttons", json::array({{{"id", 0}, {"label", _u8L("No")}}, {{"id", 1}, {"label", _u8L("Yes")}},
                                                           {{"id", 2}, {"label", _u8L("No to All")}}, {{"id", 3}, {"label", _u8L("Yes to All")}}})}});
            return ans.value("id", 0);
        },
        ForwardCompatibilitySubstitutionRule::Enable, *core.app_config);
    core.preset_bundle->update_compatible(PresetSelectCompatibleType::Always);
    presets_changed();
    std::string msg = cfiles.size() == 1 ? GUI::format(_u8L("There is %d config imported. (Only non-system and compatible configs)"), cfiles.size())
                                         : GUI::format(_u8L("There are %d configs imported. (Only non-system and compatible configs)"), cfiles.size());
    if (cfiles.empty()) msg += _u8L("\nHint: Make sure you have added the corresponding printer before importing the configs.");
    return {{"message", msg}, {"caption", _u8L("Import result")}};
}

} // namespace

static Commands reg({
    {"zip.list", zip_list},
    {"zip.extract", zip_extract},
    {"configs.import", import_configs},
});

} // namespace orca
