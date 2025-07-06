#include "zygisk.hpp"
#include <android/log.h>
#include <string>
#include <dlfcn.h>
#include <unistd.h>
#include <cerrno>
#include <filesystem>
#include <sys/stat.h>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "PIF", __VA_ARGS__)

#define DEX_PATH "/data/adb/modules/playintegrityfix/classes.dex"

#define LIB_64 "/data/adb/modules/playintegrityfix/inject/arm64-v8a.so"
#define LIB_32 "/data/adb/modules/playintegrityfix/inject/armeabi-v7a.so"

#define DEFAULT_PIF "/data/adb/modules/playintegrityfix/pif.prop"
#define CUSTOM_PIF "/data/adb/pif.prop"

#define VENDING_PACKAGE "com.android.vending"
#define DROIDGUARD_PACKAGE "com.google.android.gms.unstable"

static ssize_t xread(int fd, void *buffer, size_t count_to_read) {
    ssize_t total_read = 0;
    char *current_buf = static_cast<char *>(buffer);
    size_t remaining_bytes = count_to_read;

    while (remaining_bytes > 0) {
        ssize_t ret = TEMP_FAILURE_RETRY(read(fd, current_buf, remaining_bytes));

        if (ret < 0) {
            return -1;
        }

        if (ret == 0) {
            break;
        }

        current_buf += ret;
        total_read += ret;
        remaining_bytes -= ret;
    }

    return total_read;
}

static ssize_t xwrite(int fd, const void *buffer, size_t count_to_write) {
    ssize_t total_written = 0;
    const char *current_buf = static_cast<const char *>(buffer);
    size_t remaining_bytes = count_to_write;

    while (remaining_bytes > 0) {
        ssize_t ret = TEMP_FAILURE_RETRY(write(fd, current_buf, remaining_bytes));

        if (ret < 0) {
            return -1;
        }

        if (ret == 0) {
            break;
        }

        current_buf += ret;
        total_written += ret;
        remaining_bytes -= ret;
    }

    return total_written;
}

static bool copyFile(const std::string &from, const std::string &to, mode_t perms = 0777) {
    return std::filesystem::exists(from) &&
           std::filesystem::copy_file(
                   from,
                   to,
                   std::filesystem::copy_options::overwrite_existing
           ) &&
           !chmod(
                   to.c_str(),
                   perms
           );
}

static void companion(int fd) {
    bool ok = true;

    int size = 0;
    xread(fd, &size, sizeof(int));

    std::string dir;
    dir.resize(size);
    auto bytes = xread(fd, dir.data(), size);
    dir.resize(bytes);
    dir.shrink_to_fit();

    LOGD("[COMPANION] GMS dir: %s", dir.c_str());

    auto libFile = dir + "/libinject.so";
#if defined(__aarch64__)
    ok &= copyFile(LIB_64, libFile);
#elif defined(__arm__)
    ok &= copyFile(LIB_32, libFile);
#endif

    LOGD("[COMPANION] copied inject lib");

    auto dexFile = dir + "/classes.dex";
    ok &= copyFile(DEX_PATH, dexFile, 0644);

    LOGD("[COMPANION] copied dex");

    auto pifFile = dir + "/pif.prop";
    if (!copyFile(CUSTOM_PIF, pifFile)) {
        if (!copyFile(DEFAULT_PIF, pifFile)) {
            ok = false;
        }
    }

    LOGD("[COMPANION] copied pif");

    xwrite(fd, &ok, sizeof(bool));
}

using namespace zygisk;

class PlayIntegrityFix : public ModuleBase {
public:
    void onLoad(Api *api_, JNIEnv *env_) override {
        this->api = api_;
        this->env = env_;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        api->setOption(DLCLOSE_MODULE_LIBRARY);

        if (!args)
            return;

        std::string dir, name;

        auto rawDir = env->GetStringUTFChars(args->app_data_dir, nullptr);

        if (rawDir) {
            dir = rawDir;
            env->ReleaseStringUTFChars(args->app_data_dir, rawDir);
        }

        auto rawName = env->GetStringUTFChars(args->nice_name, nullptr);

        if (rawName) {
            name = rawName;
            env->ReleaseStringUTFChars(args->nice_name, rawName);
        }

        std::string_view vDir(dir);
        bool isGms =
                vDir.ends_with("/com.google.android.gms") || vDir.ends_with("/com.android.vending");

        if (!isGms)
            return;

        api->setOption(FORCE_DENYLIST_UNMOUNT);

        std::string_view vName(name);
        isGmsUnstable = vName == DROIDGUARD_PACKAGE;
        isVending = vName == VENDING_PACKAGE;

        if (!isGmsUnstable && !isVending) {
            api->setOption(DLCLOSE_MODULE_LIBRARY);
            return;
        }

        auto fd = api->connectCompanion();

        int size = static_cast<int>(dir.size());
        xwrite(fd, &size, sizeof(int));

        xwrite(fd, dir.data(), size);

        bool ok = false;
        xread(fd, &ok, sizeof(bool));

        close(fd);

        if (ok)
            gmsDir = dir;
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        if (gmsDir.empty())
            return;

        typedef bool (*InitFuncPtr)(JavaVM *, const std::string &, bool, bool);

        void *handle = dlopen((gmsDir + "/libinject.so").c_str(), RTLD_NOW);

        if (!handle)
            return;

        auto init_func = reinterpret_cast<InitFuncPtr>(dlsym(handle, "init"));

        JavaVM *vm = nullptr;
        env->GetJavaVM(&vm);

        if (init_func(vm, gmsDir, isGmsUnstable, isVending)) {
            LOGD("dlclose injected lib");
            dlclose(handle);
        }
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        api->setOption(DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;
    std::string gmsDir;
    bool isGmsUnstable = false;
    bool isVending = false;
};

REGISTER_ZYGISK_MODULE(PlayIntegrityFix)

REGISTER_ZYGISK_COMPANION(companion)