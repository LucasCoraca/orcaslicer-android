#include "Host.hpp"

#include <android/log.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"

namespace orca {

static JavaVM   *g_vm         = nullptr;
static jobject   g_host       = nullptr;
static jmethodID g_on_event   = nullptr;
static jmethodID g_on_ask     = nullptr;

void Host::attach(JavaVM *vm, jobject host)
{
    g_vm   = vm;
    JNIEnv *e = env();
    g_host = e->NewGlobalRef(host);
    jclass cls = e->GetObjectClass(g_host);
    g_on_event = e->GetMethodID(cls, "onEvent", "(Ljava/lang/String;Ljava/lang/String;)V");
    g_on_ask   = e->GetMethodID(cls, "onAsk", "(Ljava/lang/String;)Ljava/lang/String;");
}

JNIEnv *Host::env()
{
    JNIEnv *e = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&e), JNI_VERSION_1_6) == JNI_EDETACHED) {
        // engine worker threads (slicing, TBB) attach once and stay attached
        g_vm->AttachCurrentThread(&e, nullptr);
    }
    return e;
}

void Host::event(const std::string &kind, const json &data)
{
    if (!g_host)
        return;
    JNIEnv *e = env();
    jstring k = e->NewStringUTF(kind.c_str());
    jstring d = e->NewStringUTF(data.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
    e->CallVoidMethod(g_host, g_on_event, k, d);
    e->DeleteLocalRef(k);
    e->DeleteLocalRef(d);
}

json Host::ask(const json &request)
{
    if (!g_host)
        return json::object();
    JNIEnv *e = env();
    jstring r   = e->NewStringUTF(request.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
    auto    ans = static_cast<jstring>(e->CallObjectMethod(g_host, g_on_ask, r));
    e->DeleteLocalRef(r);
    json out = json::object();
    if (ans) {
        const char *c = e->GetStringUTFChars(ans, nullptr);
        out = json::parse(c, nullptr, false);
        e->ReleaseStringUTFChars(ans, c);
        e->DeleteLocalRef(ans);
    }
    return out.is_discarded() ? json::object() : out;
}

void Host::log(const std::string &msg) { __android_log_write(ANDROID_LOG_INFO, "orcacore", msg.c_str()); }

} // namespace orca

namespace Slic3r { namespace GUI {

int host_message_dialog(const std::string &message, const std::string &caption, long style, const std::string &yes_label,
                        const std::string &no_label, const std::string &ok_label, bool dsa_button, bool *dsa_checked,
                        const std::string &dsa_label)
{
    orca::json buttons = orca::json::array();
    if (style & wxYES) buttons.push_back({{"id", wxID_YES}, {"label", yes_label.empty() ? I18N::translate_utf8("Yes") : yes_label}});
    if (style & wxNO) buttons.push_back({{"id", wxID_NO}, {"label", no_label.empty() ? I18N::translate_utf8("No") : no_label}});
    if (style & wxOK) buttons.push_back({{"id", wxID_OK}, {"label", ok_label.empty() ? I18N::translate_utf8("OK") : ok_label}});
    if (style & wxCANCEL) buttons.push_back({{"id", wxID_CANCEL}, {"label", I18N::translate_utf8("Cancel")}});
    const char *icon = (style & wxICON_ERROR) ? "error" : (style & wxICON_WARNING) ? "warning" : (style & wxICON_QUESTION) ? "question" : "info";
    orca::json ans = orca::Host::ask({{"kind", "message"}, {"message", message}, {"caption", caption}, {"icon", icon},
                                      {"buttons", buttons}, {"dsa", dsa_button}, {"dsa_label", dsa_label}});
    if (dsa_checked)
        *dsa_checked = ans.value("dsa", false);
    int def = (style & wxNO) ? wxID_NO : (style & wxCANCEL) ? wxID_CANCEL : wxID_OK;
    return ans.value("id", def);
}

}} // namespace Slic3r::GUI
