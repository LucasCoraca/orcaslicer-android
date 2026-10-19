// Android start-up glue for Orca Slicer: app-private directories and the unpacked `resources/` tree.
//
// OrcaSlicer.cpp's main() calls orca_android::bootstrap() before CLI::run(), which then finds the resources
// through ORCA_RESOURCES_DIR and the user data through ORCA_DATA_DIR.

#include <QtCore/QJniObject>
#include <QtCore/QCoreApplication>
#include <QtWidgets/QWidget>
#include <QtCore/QMetaObject>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <boost/filesystem.hpp>
#include <miniz.h>

#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "orca", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "orca", __VA_ARGS__)

namespace fs = boost::filesystem;

namespace orca_android {

namespace {

std::string path_of(const QJniObject &file)
{
    return file.callObjectMethod("getAbsolutePath", "()Ljava/lang/String;").toString().toStdString();
}

std::string read_stamp(const fs::path &p)
{
    std::ifstream in(p.string());
    std::string s;
    std::getline(in, s);
    return s;
}

// Unpack the "resources.zip" asset into `dest` unless the stamp inside it already matches.
bool extract_resources(const QJniObject &context, const fs::path &dest)
{
    QJniObject jassets = context.callObjectMethod("getAssets", "()Landroid/content/res/AssetManager;");
    AAssetManager *mgr = AAssetManager_fromJava(QJniEnvironment().jniEnv(), jassets.object());
    if (!mgr) {
        LOGE("no AssetManager");
        return false;
    }
    AAsset *asset = AAssetManager_open(mgr, "resources.zip", AASSET_MODE_BUFFER);
    if (!asset) {
        LOGE("asset resources.zip is missing from the APK");
        return false;
    }
    const void *buf = AAsset_getBuffer(asset);
    const size_t len = size_t(AAsset_getLength64(asset));

    mz_zip_archive zip{};
    if (!buf || !mz_zip_reader_init_mem(&zip, buf, len, 0)) {
        LOGE("resources.zip is not a valid zip");
        AAsset_close(asset);
        return false;
    }

    std::string new_stamp;
    {
        const int idx = mz_zip_reader_locate_file(&zip, ".orca-stamp", nullptr, 0);
        if (idx >= 0) {
            size_t sz = 0;
            void *p = mz_zip_reader_extract_to_heap(&zip, mz_uint(idx), &sz, 0);
            if (p) {
                new_stamp.assign(static_cast<char *>(p), sz);
                mz_free(p);
            }
        }
    }

    const fs::path stamp_file = dest / ".orca-stamp";
    if (!new_stamp.empty() && fs::exists(stamp_file) && read_stamp(stamp_file) == new_stamp) {
        LOGI("resources up to date (%s)", new_stamp.c_str());
        mz_zip_reader_end(&zip);
        AAsset_close(asset);
        return true;
    }

    LOGI("unpacking resources (%zu bytes) to %s", len, dest.c_str());
    boost::system::error_code ec;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);

    bool ok = true;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n && ok; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        if (std::string(st.m_filename) == ".orca-stamp")
            continue; // written last so an interrupted unpack is retried on the next launch
        const fs::path out = dest / st.m_filename;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(out, ec);
            continue;
        }
        fs::create_directories(out.parent_path(), ec);
        ok = mz_zip_reader_extract_to_file(&zip, i, out.c_str(), 0) != 0;
        if (!ok)
            LOGE("cannot extract %s", st.m_filename);
    }
    if (ok && !new_stamp.empty()) {
        std::ofstream stamp(stamp_file.string(), std::ios::binary | std::ios::trunc);
        stamp << new_stamp;
    }
    mz_zip_reader_end(&zip);
    AAsset_close(asset);
    LOGI("resources unpacked: %s", ok ? "ok" : "FAILED");
    return ok;
}

// OpenSSL/libcurl want one PEM bundle (SSL_CERT_FILE); Android ships the trust store as a directory of
// hashed PEM files. Concatenate them once per launch into the app's private files directory.
bool build_ca_bundle(const fs::path &out)
{
    static const char *dirs[] = { "/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts" };
    for (const char *d : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(d, ec))
            continue;
        std::ofstream bundle(out.string(), std::ios::binary | std::ios::trunc);
        size_t n = 0;
        for (const auto &entry : std::filesystem::directory_iterator(d, ec)) {
            std::ifstream in(entry.path().string(), std::ios::binary);
            if (!in)
                continue;
            bundle << in.rdbuf() << "\n";
            ++n;
        }
        if (n > 0) {
            LOGI("CA bundle: %zu certificates from %s", n, d);
            return true;
        }
    }
    LOGE("no system CA store found");
    return false;
}

} // namespace

std::string qwidget_debug(void *handle)
{
    if (!handle)
        return "null";
    QWidget *w = static_cast<QWidget *>(handle);
    auto col = [](const QColor &c) { return c.name(QColor::HexArgb).toStdString(); };
    std::string s = std::string(w->metaObject()->className()) + " enabled=" + (w->isEnabled() ? "1" : "0")
        + " autofill=" + (w->autoFillBackground() ? "1" : "0")
        + " opaque=" + (w->testAttribute(Qt::WA_OpaquePaintEvent) ? "1" : "0")
        + " nosysbg=" + (w->testAttribute(Qt::WA_NoSystemBackground) ? "1" : "0")
        + " translucent=" + (w->testAttribute(Qt::WA_TranslucentBackground) ? "1" : "0")
        + " pal[btn=" + col(w->palette().color(QPalette::Button)) + " win=" + col(w->palette().color(QPalette::Window))
        + " base=" + col(w->palette().color(QPalette::Base)) + " text=" + col(w->palette().color(QPalette::ButtonText)) + "]"
        + " bgrole=" + std::to_string(int(w->backgroundRole()))
        + " ss=" + (w->styleSheet().isEmpty() ? std::string("-") : w->styleSheet().simplified().toStdString());
    return s;
}

void qwidget_force_opaque(void *handle)
{
    if (!handle)
        return;
    QWidget *w = static_cast<QWidget *>(handle);
    QPalette p = w->palette();
    bool changed = false;
    for (QPalette::ColorRole role : { QPalette::Button, QPalette::Window, QPalette::Base }) {
        if (p.color(role).alpha() == 0) {
            p.setColor(role, QColor(244, 244, 244));
            changed = true;
        }
    }
    if (changed)
        w->setPalette(p);
}

bool qwidget_visible(void *handle)
{
    return handle != nullptr && static_cast<QWidget *>(handle)->isVisible();
}

// Orca logs through Boost.Log to stdout/stderr, which Android discards: forward both to logcat (tag "orca-out").
static void forward_stdio_to_logcat()
{
    int fds[2];
    if (pipe(fds) != 0)
        return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    const int rd = fds[0];
    std::thread([rd] {
        std::string line;
        char buf[1024];
        ssize_t n;
        while ((n = read(rd, buf, sizeof buf)) > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    __android_log_write(ANDROID_LOG_INFO, "orca-out", line.c_str());
                    line.clear();
                } else {
                    line.push_back(buf[i]);
                }
            }
        }
    }).detach();
}

void bootstrap()
{
    forward_stdio_to_logcat();
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        LOGE("no Android context");
        return;
    }

    const std::string files = path_of(context.callObjectMethod("getFilesDir", "()Ljava/io/File;"));
    const std::string cache = path_of(context.callObjectMethod("getCacheDir", "()Ljava/io/File;"));

    const fs::path resources = fs::path(files) / "resources";
    const fs::path data      = fs::path(files) / "OrcaSlicer";
    boost::system::error_code ec;
    fs::create_directories(data, ec);

    setenv("HOME", files.c_str(), 1);
    setenv("TMPDIR", cache.c_str(), 1);
    setenv("ORCA_RESOURCES_DIR", resources.c_str(), 1);
    setenv("ORCA_DATA_DIR", data.c_str(), 1);

    const fs::path cabundle = fs::path(files) / "cacert.pem";
    if (build_ca_bundle(cabundle))
        setenv("SSL_CERT_FILE", cabundle.c_str(), 1);

    extract_resources(context, resources);
}

} // namespace orca_android
