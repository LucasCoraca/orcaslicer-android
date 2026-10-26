// Help > About (AboutDialog) and its License Info (CopyrightsDialog): upstream's texts, build label and library list.
#include <string>
#include <vector>

#include "Core.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r_version.h"
#include "slic3r/GUI/BuildCommit.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

using namespace Slic3r;

namespace orca {
namespace {
struct CopyrightsDialog
{
    struct Entry {
        Entry(const std::string &lib_name, const std::string &copyright, const std::string &link) : lib_name(lib_name), copyright(copyright), link(link) {}
        std::string lib_name, copyright, link;
    };
    std::vector<Entry> m_entries;
    void fill_entries();
};
#include "slic3r/GUI/AboutEntries.inc"

json about(const json &)
{
    CopyrightsDialog c;
    c.fill_entries();
    json libs = json::array();
    for (auto &e : c.m_entries) libs.push_back({{"name", e.lib_name}, {"link", e.link}});
    const bool dark = Core::get().app_config->get("dark_color_mode") == "1";
    return {{"title", (boost::format(_utf8(L("About %s"))) % SLIC3R_APP_FULL_NAME).str()},
            {"logo", resources_dir() + (dark ? "/images/OrcaSlicer_about_dark.svg" : "/images/OrcaSlicer_about.svg")},
            {"version", SoftFever_VERSION},
            // TRN %s is the commit the application was built from
            {"build", GUI::format(_u8L("Build %s"), GUI::build_commit_label)},
            {"text", {_u8L("Open-source slicing stands on a tradition of collaboration and attribution. Slic3r, created by Alessandro Ranellucci and the RepRap community, laid the foundation. PrusaSlicer by Prusa Research built on that work, Bambu Studio forked from PrusaSlicer, and SuperSlicer extended it with community-driven enhancements. Each project carried the work of its predecessors forward, crediting those who came before."),
                      _u8L("OrcaSlicer began in that same spirit, drawing from PrusaSlicer, BambuStudio, SuperSlicer, and CuraSlicer. But it has since grown far beyond its origins — introducing advanced calibration tools, precise wall and seam control and hundreds of other features."),
                      _u8L("Today, OrcaSlicer is the most widely used and actively developed open-source slicer in the 3D printing community. Many of its innovations have been adopted by other slicers, making it a driving force for the entire industry.")}},
            {"copyright", _u8L("Copyright(C) 2026 OrcaSlicer Pte Ltd All Rights Reserved")},
            {"site", "https://www.orcaslicer.com"},
            {"license_button", _u8L("License Info")},
            // CopyrightsDialog::get_html_text
            {"license", {{"title", _u8L("License")},
                         {"licensed", _u8L("Orca Slicer is licensed under ")},
                         {"gpl", _u8L("GNU Affero General Public License, version 3")},
                         {"gpl_link", "https://www.gnu.org/licenses/agpl-3.0.html"},
                         {"based", _u8L("Orca Slicer is based on PrusaSlicer and BambuStudio")},
                         {"libraries", _u8L("Libraries")},
                         {"uses", _u8L("This software uses open source components whose copyright and other proprietary rights belong to their respective owners")},
                         {"entries", libs}}}};
}
} // namespace

static Commands reg({{"app.about", about}});

} // namespace orca
