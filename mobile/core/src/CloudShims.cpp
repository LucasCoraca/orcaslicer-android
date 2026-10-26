// The wx pieces upstream's cloud agents use (shim/wx): the user data dir, the "system keychain" and the machine / user
// ids that bind the stored token to this install.
#include <fstream>
#include <iterator>
#include <unistd.h>
#include <boost/filesystem.hpp>

#include "libslic3r/Utils.hpp"
#include "wx/secretstore.h"
#include "wx/stdpaths.h"
#include "wx/utils.h"

namespace fs = boost::filesystem;

wxString wxStandardPaths::GetUserDataDir() const { return wxString(Slic3r::data_dir()); }

// one file per service under <data_dir>/secrets: app-private storage, unreadable by other apps
static fs::path secret_path(const wxString &service)
{
    std::string name = service.ToStdString();
    for (char &c : name)
        if (!isalnum((unsigned char) c) && c != '.' && c != '-' && c != '_') c = '_';
    return fs::path(Slic3r::data_dir()) / "secrets" / name;
}

bool wxSecretStore::Save(const wxString &service, const wxString &user, const wxSecretValue &secret)
{
    boost::system::error_code ec;
    fs::create_directories(secret_path(service).parent_path(), ec);
    std::ofstream f(secret_path(service).string(), std::ios::binary | std::ios::trunc);
    const std::string u = user.ToStdString();
    f << u.size() << '\n' << u << secret.m_data;
    return bool(f);
}

bool wxSecretStore::Load(const wxString &service, wxString &user, wxSecretValue &secret) const
{
    std::ifstream f(secret_path(service).string(), std::ios::binary);
    if (!f) return false;
    size_t n = 0;
    f >> n;
    f.get();
    std::string u(n, '\0');
    f.read(&u[0], std::streamsize(n));
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    user = wxString(u);
    secret.m_data = data;
    secret.m_ok = true;
    return true;
}

bool wxSecretStore::Delete(const wxString &service)
{
    boost::system::error_code ec;
    return fs::remove(secret_path(service), ec);
}

wxString wxGetHostName()
{
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf) - 1) != 0) return wxString("android");
    return wxString(buf);
}

wxString wxGetUserId() { return wxString(std::to_string(getuid())); }
