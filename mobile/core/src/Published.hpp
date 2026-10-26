// Published 3MFs (Published.cpp): the receiving side of Plater::priv::load_files and the author's File > Publish 3MF
#pragma once
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PublishSettings.hpp"

namespace Slic3r { class Model; }

namespace orca {

// Plater::priv::load_files: a "published" 3MF's flag, the author-selected keys and the values of its metadata payload
// (into config_loaded, as the file carries no project_settings.config)
Slic3r::PublishedConfig read_published(const Slic3r::Model &model, Slic3r::DynamicPrintConfig &config_loaded,
                                       Slic3r::ConfigSubstitutionContext &config_substitutions);
// the published metadata is consumed on load: a later save is a normal 3MF
void strip_published(Slic3r::Model &model);

// Plater::get_pending_published / set_pending_published: the session's last published selection (written on publish
// and on loading a published 3MF, read when the Publish dialog opens); cleared by a new project
struct PendingPublished
{
    bool                                        has{false};
    std::vector<std::string>                    keys;
    std::vector<Slic3r::PublishedMaterialEntry> material_keys;
};
PendingPublished &pending_published();

} // namespace orca
