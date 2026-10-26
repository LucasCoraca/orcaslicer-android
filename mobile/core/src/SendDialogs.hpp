// The host-specific parts of the send dialog (SendDialogs.cpp)
#pragma once
#include <map>
#include <string>
#include <vector>

#include "Core.hpp"
#include "libslic3r/ProjectTask.hpp"

namespace Slic3r { class PrintHost; class DynamicPrintConfig; }

namespace orca {

// what the dialog needs from the presets and the plate (engine thread)
struct SendExtrasInput
{
    int                                host_type{0};
    std::string                        model_id, curr_bed_type;
    bool                               flashforge_local{false};
    std::vector<std::string>           colors, types;
    std::vector<Slic3r::FilamentInfo>  project_filaments;
    std::map<std::string, std::string> recent;   // the "recent" app config keys of the dialogs
};
SendExtrasInput send_extras_input(Slic3r::DynamicPrintConfig *physical_printer_config, int plate);
// the extras of the send dialog, querying the host (the dialog's thread); throws CommandError when the host refuses
json send_extras(const SendExtrasInput &in, Slic3r::PrintHost *host);
// the dialog's answer: the upload's extended info, the remembered choices, and Elegoo's upload-and-print check box
void send_extras_apply(const json &answer, std::map<std::string, std::string> &extended_info, bool &start_print);

} // namespace orca
