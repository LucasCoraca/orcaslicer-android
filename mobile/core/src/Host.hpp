// The app side of the core: callbacks into Kotlin (org.orcaport.mobile.engine.Host).
#pragma once
#include <jni.h>
#include <string>
#include <nlohmann/json.hpp>

namespace orca {

using json = nlohmann::json;

class Host
{
public:
    static void attach(JavaVM *vm, jobject host);   // host: global ref, kept for the process lifetime

    // An event for the UI (progress, notifications, state changes). Any thread.
    static void event(const std::string &kind, const json &data);
    // A modal question; blocks the calling thread until the user answers. Returns the chosen button id.
    static json ask(const json &request);
    static void log(const std::string &msg);

private:
    static JNIEnv *env();
};

} // namespace orca
