// Stand-in for wx/secretstore.h: the system keychain is a file per service in the app's private storage (not
// readable by other apps), which is what Android offers without the Keystore round trip. Host.cpp.
#pragma once
#include <string>
#include "wx/string.h"

class wxSecretValue
{
public:
    wxSecretValue() = default;
    explicit wxSecretValue(const wxString &s) : m_data(s.ToStdString()), m_ok(true) {}
    bool        IsOk() const { return m_ok; }
    const void *GetData() const { return m_data.data(); }
    size_t      GetSize() const { return m_data.size(); }
    wxString    GetAsString() const { return wxString(m_data); }

private:
    friend class wxSecretStore;
    std::string m_data;
    bool        m_ok{false};
};

class wxSecretStore
{
public:
    static wxSecretStore GetDefault() { return wxSecretStore(); }
    bool IsOk() const { return true; }
    bool Save(const wxString &service, const wxString &user, const wxSecretValue &secret);
    bool Load(const wxString &service, wxString &user, wxSecretValue &secret) const;
    bool Delete(const wxString &service);
};
