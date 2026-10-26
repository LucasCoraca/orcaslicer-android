// JNI entry points (Kotlin: org.orcaport.mobile.engine.Native). Commands take and return JSON;
// every result is {"ok": <value>} or {"error": "<message>"}.
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <android/log.h>
#include <exception>
#include <string>

#include "Core.hpp"
#include "Host.hpp"

using orca::json;

static JavaVM *g_vm = nullptr;

static std::string jstr(JNIEnv *env, jstring s)
{
    if (!s) return {};
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}

static jstring out(JNIEnv *env, const json &j)
{
    return env->NewStringUTF(j.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
}

// stdout/stderr -> logcat
static int g_pipe[2];
static void *log_thread(void *)
{
    char buf[1024]; std::string line; ssize_t n;
    while ((n = read(g_pipe[0], buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') { __android_log_write(ANDROID_LOG_INFO, "orcacore-out", line.c_str()); line.clear(); }
            else line += buf[i];
        }
    return nullptr;
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *)
{
    g_vm = vm;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (pipe(g_pipe) == 0) {
        dup2(g_pipe[1], 1); dup2(g_pipe[1], 2);
        pthread_t t; pthread_create(&t, nullptr, log_thread, nullptr); pthread_detach(t);
    }
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_orcaport_mobile_engine_Native_init(JNIEnv *env, jclass, jobject host, jstring resources, jstring data, jstring language)
{
    orca::Host::attach(g_vm, host);
    try {
        return out(env, {{"ok", orca::Core::get().init(jstr(env, resources), jstr(env, data), jstr(env, language))}});
    } catch (const std::exception &ex) {
        return out(env, {{"error", ex.what()}});
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_orcaport_mobile_engine_Native_call(JNIEnv *env, jclass, jstring cmd, jstring args)
{
    std::string c = jstr(env, cmd);
    try {
        json a = json::parse(jstr(env, args), nullptr, false);
        if (a.is_discarded()) a = json::object();
        return out(env, {{"ok", orca::Core::get().call(c, a)}});
    } catch (const std::exception &ex) {
        __android_log_print(ANDROID_LOG_WARN, "orcacore", "%s: %s", c.c_str(), ex.what());
        return out(env, {{"error", ex.what()}});
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_orcaport_mobile_engine_Native_pure(JNIEnv *env, jclass, jstring cmd, jstring args)
{
    std::string c = jstr(env, cmd);
    try {
        json a = json::parse(jstr(env, args), nullptr, false);
        if (a.is_discarded()) a = json::object();
        return out(env, {{"ok", orca::Core::call_pure(c, a)}});
    } catch (const std::exception &ex) {
        return out(env, {{"error", ex.what()}});
    }
}

// _L() for the app's own strings: the same gettext catalog as the engine (read-only once loaded, any thread)
#include "I18n.hpp"
extern "C" JNIEXPORT jstring JNICALL
Java_org_orcaport_mobile_engine_Native_tr(JNIEnv *env, jclass, jstring s, jstring ctx)
{
    const std::string msg = jstr(env, s);
    const std::string c   = ctx ? jstr(env, ctx) : std::string();
    const std::string r   = c.empty() ? orca::translate(msg) : orca::translate_ctx(msg, c);
    return env->NewStringUTF(r.c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// 3D view (Viewport.cpp, Preview.cpp). gl* run on the GL thread, vp* on the UI thread.
#include "Preview.hpp"
#include "Viewport.hpp"
#include "LayersEditing.hpp"
#include "Gizmos.hpp"

#define VP(name) extern "C" JNIEXPORT
using orca::Viewport;

VP() void JNICALL Java_org_orcaport_mobile_engine_Native_glCreated(JNIEnv *, jclass) { Viewport::get().gl_created(); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_glResize(JNIEnv *, jclass, jint w, jint h) { Viewport::get().resize(w, h); }
VP() jboolean JNICALL Java_org_orcaport_mobile_engine_Native_glDraw(JNIEnv *, jclass) { return Viewport::get().draw(); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpOrbit(JNIEnv *, jclass, jdouble dx, jdouble dy) { Viewport::get().orbit(dx, dy); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpPan(JNIEnv *, jclass, jdouble dx, jdouble dy) { Viewport::get().pan(dx, dy); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpZoom(JNIEnv *, jclass, jdouble f, jdouble x, jdouble y) { Viewport::get().zoom(f, x, y); }
VP() jstring JNICALL Java_org_orcaport_mobile_engine_Native_vpPick(JNIEnv *env, jclass, jdouble x, jdouble y) { return out(env, Viewport::get().pick(x, y)); }
VP() jboolean JNICALL Java_org_orcaport_mobile_engine_Native_vpDragStart(JNIEnv *, jclass, jdouble x, jdouble y) { return Viewport::get().drag_start(x, y); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpDragMove(JNIEnv *, jclass, jdouble x, jdouble y) { Viewport::get().drag_move(x, y); }
VP() jstring JNICALL Java_org_orcaport_mobile_engine_Native_vpDragEnd(JNIEnv *env, jclass) { return out(env, Viewport::get().drag_end()); }
// the variable layer height bar: a press starts editing at once (the desktop's mouse down + 100 ms timer)
VP() jboolean JNICALL Java_org_orcaport_mobile_engine_Native_vpLayersPress(JNIEnv *, jclass, jdouble x, jdouble y) { return orca::LayersEditing::get().press(x, y); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpLayersMove(JNIEnv *, jclass, jdouble x, jdouble y) { orca::LayersEditing::get().move(x, y); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpLayersRelease(JNIEnv *, jclass) { orca::LayersEditing::get().release(); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpView(JNIEnv *env, jclass, jstring v) { Viewport::get().set_view(jstr(env, v)); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpZoomTo(JNIEnv *env, jclass, jstring v) { Viewport::get().zoom_to(jstr(env, v)); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpPreview(JNIEnv *, jclass, jboolean on, jint plate)
{
    Viewport::get().set_preview(on);
    if (on) orca::Preview::get().show_plate(plate);
}
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_previewSet(JNIEnv *env, jclass, jstring a)
{
    json j = json::parse(jstr(env, a), nullptr, false);
    if (!j.is_discarded()) orca::Preview::get().set(j);
}
VP() jstring JNICALL Java_org_orcaport_mobile_engine_Native_vpOverlay(JNIEnv *env, jclass) { return out(env, Viewport::get().overlay()); }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpUiScale(JNIEnv *, jclass, jfloat s) { orca::Gizmos::get().set_ui_scale(s); }
namespace orca { extern std::atomic<bool> shift_down; }
VP() void JNICALL Java_org_orcaport_mobile_engine_Native_vpShift(JNIEnv *, jclass, jboolean down) { orca::shift_down = down; }
VP() jboolean JNICALL Java_org_orcaport_mobile_engine_Native_vpComTap(JNIEnv *, jclass, jdouble x, jdouble y) { return Viewport::get().center_of_mass_tap(x, y); }
VP() jboolean JNICALL Java_org_orcaport_mobile_engine_Native_vpNavTouch(JNIEnv *, jclass, jfloat x, jfloat y, jint action) { return Viewport::get().navigator_touch(x, y, action); }
VP() jboolean JNICALL Java_org_orcaport_mobile_engine_Native_vpPaintTap(JNIEnv *, jclass, jdouble x, jdouble y) { return Viewport::get().paint_tap(x, y); }
