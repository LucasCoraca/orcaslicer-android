// The cloud print hosts' sign-in (CloudHosts.cpp)
#pragma once
#include <string>

#include "slic3r/GUI/Jobs/OAuthJob.hpp"

namespace Slic3r { class PrintHost; }

namespace orca {

// OAuthDialog: the browser sign-in, waiting with "Authorizing..." / Cancel (any thread but the engine's)
Slic3r::GUI::OAuthResult simplyprint_oauth(const Slic3r::GUI::OAuthParams &params);
// PrinterCloudAuthDialog: the API key the host's login page posts, empty when closed
std::string cloud_auth_api_key(Slic3r::PrintHost *host);

} // namespace orca
