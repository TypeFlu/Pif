#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <fcntl.h>
#include "zygisk.hpp"
#include "dobby.h"
#include "json.hpp"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "PIF", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PIF", __VA_ARGS__)

#define DEX_PATH "/data/adb/modules/playintegrityfix/classes.dex"

#define TS_PATH "/data/adb/modules/tricky_store"

#define DEFAULT_JSON "/data/adb/modules/playintegrityfix/pif.json"
#define CUSTOM_JSON_FORK "/data/adb/modules/playintegrityfix/custom.pif.json"
#define CUSTOM_JSON "/data/adb/pif.json"

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

static bool DEBUG = false;
static std::string DEVICE_INITIAL_SDK_INT = "21", SECURITY_PATCH, BUILD_ID;

typedef void (*T_Callback)(void *, const char *, const char *, uint32_t);

static T_Callback o_callback = nullptr;

static void modify_callback(void *cookie, const char *name, const char *value, uint32_t serial) {

    if (!cookie || !name || !value || !o_callback) return;

    const char *oldValue = value;

    std::string_view prop(name);

    if (prop == "init.svc.adbd") {
        value = "stopped";
    } else if (prop == "sys.usb.state") {
        value = "mtp";
    } else if (prop.ends_with("api_level")) {
        if (!DEVICE_INITIAL_SDK_INT.empty()) {
            value = DEVICE_INITIAL_SDK_INT.c_str();
        }
    } else if (prop.ends_with(".security_patch")) {
        if (!SECURITY_PATCH.empty()) {
            value = SECURITY_PATCH.c_str();
        }
    } else if (prop.ends_with(".build.id")) {
        if (!BUILD_ID.empty()) {
            value = BUILD_ID.c_str();
        }
    }

    if (strcmp(oldValue, value) == 0) {
        if (DEBUG) LOGD("[%s]: %s (unchanged)", name, oldValue);
    } else {
        LOGD("[%s]: %s -> %s", name, oldValue, value);
    }

    return o_callback(cookie, name, value, serial);
}

static void (*o_system_property_read_callback)(prop_info *, T_Callback, void *) = nullptr;

static void my_system_property_read_callback(prop_info *pi, T_Callback callback, void *cookie) {
    if (pi && callback && cookie) o_callback = callback;
    return o_system_property_read_callback(pi, modify_callback, cookie);
}

static bool doHook() {
    void *ptr = DobbySymbolResolver(nullptr, "__system_property_read_callback");

    if (ptr && DobbyHook(ptr, (void *) my_system_property_read_callback,
                         (void **) &o_system_property_read_callback) == 0) {
        LOGD("hook __system_property_read_callback successful at %p", ptr);
        return true;
    }

    LOGE("hook __system_property_read_callback failed!");
    return false;
}

class PlayIntegrityFix : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *_api, JNIEnv *_env) override {
        this->api = _api;
        this->env = _env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {

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

        bool isGms = dir.ends_with("/com.google.android.gms");
        bool isGmsUnstable = isGms && name == "com.google.android.gms.unstable";

        if (!isGms) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

        if (!isGmsUnstable) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        int fd = api->connectCompanion();

        size_t dexSize = 0, jsonSize = 0;
        std::string jsonStr;

        xread(fd, &dexSize, sizeof(size_t));
        xread(fd, &jsonSize, sizeof(size_t));

        if (dexSize > 0) {
            dexVector.resize(dexSize);
            xread(fd, dexVector.data(), dexSize);
        }

        if (jsonSize > 0) {
            jsonStr.resize(jsonSize);
            jsonSize = xread(fd, jsonStr.data(), jsonSize);
            json = nlohmann::json::parse(jsonStr, nullptr, false, true);
        }

        bool trickyStore = false;
        xread(fd, &trickyStore, sizeof(bool));

        bool testSignedRom = false;
        xread(fd, &testSignedRom, sizeof(bool));

        close(fd);

        LOGD("Dex file size: %zu", dexSize);
        LOGD("Json file size: %zu", jsonSize);

        parseJSON();

        if (trickyStore) {
            LOGD("TrickyStore module detected!");
            spoofProvider = false;
            spoofProps = false;
        }

        if (testSignedRom) {
            LOGD("--- ROM IS SIGNED WITH TEST KEYS ---");
            spoofSignature = true;
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (dexVector.empty() || json.empty())
            return;

        UpdateBuildFields();

        if (spoofProvider || spoofSignature) {
            injectDex();
        } else {
            LOGD("Dex file won't be injected due spoofProvider and spoofSignature are false");
        }

        if (spoofProps) {
            if (!doHook()) {
                dlclose();
            }
        } else {
            dlclose();
        }

        json.clear();

        dexVector.clear();
        dexVector.shrink_to_fit();
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    std::vector<char> dexVector;
    nlohmann::json json;
    bool spoofProps = true;
    bool spoofProvider = true;
    bool spoofSignature = false;

    void dlclose() {
        LOGD("dlclose zygisk lib");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

    void parseJSON() {
        if (json.empty()) return;

        if (json.contains("DEVICE_INITIAL_SDK_INT")) {
            if (json["DEVICE_INITIAL_SDK_INT"].is_string()) {
                DEVICE_INITIAL_SDK_INT = json["DEVICE_INITIAL_SDK_INT"].get<std::string>();
            } else if (json["DEVICE_INITIAL_SDK_INT"].is_number_integer()) {
                DEVICE_INITIAL_SDK_INT = std::to_string(json["DEVICE_INITIAL_SDK_INT"].get<int>());
            } else {
                LOGE("Couldn't parse DEVICE_INITIAL_SDK_INT value!");
            }
            json.erase("DEVICE_INITIAL_SDK_INT");
        }

        if (json.contains("spoofProvider") && json["spoofProvider"].is_boolean()) {
            spoofProvider = json["spoofProvider"].get<bool>();
            json.erase("spoofProvider");
        }

        if (json.contains("spoofProps") && json["spoofProps"].is_boolean()) {
            spoofProps = json["spoofProps"].get<bool>();
            json.erase("spoofProps");
        }

        if (json.contains("spoofSignature") && json["spoofSignature"].is_boolean()) {
            spoofSignature = json["spoofSignature"].get<bool>();
            json.erase("spoofSignature");
        }

        if (json.contains("DEBUG") && json["DEBUG"].is_boolean()) {
            DEBUG = json["DEBUG"].get<bool>();
            json.erase("DEBUG");
        }

        if (json.contains("FINGERPRINT") && json["FINGERPRINT"].is_string()) {
            std::string fingerprint = json["FINGERPRINT"].get<std::string>();

            std::vector<std::string> vector;
            auto parts = fingerprint | std::views::split('/');

            for (const auto &part: parts) {
                auto subParts = std::string(part.begin(), part.end()) | std::views::split(':');
                for (const auto &subPart: subParts) {
                    vector.emplace_back(subPart.begin(), subPart.end());
                }
            }

            if (vector.size() == 8) {
                json["BRAND"] = vector[0];
                json["PRODUCT"] = vector[1];
                json["DEVICE"] = vector[2];
                json["RELEASE"] = vector[3];
                json["ID"] = vector[4];
                json["INCREMENTAL"] = vector[5];
                json["TYPE"] = vector[6];
                json["TAGS"] = vector[7];
            } else {
                LOGE("Error parsing fingerprint values!");
            }
        }

        if (json.contains("SECURITY_PATCH") && json["SECURITY_PATCH"].is_string()) {
            SECURITY_PATCH = json["SECURITY_PATCH"].get<std::string>();
        }

        if (json.contains("ID") && json["ID"].is_string()) {
            BUILD_ID = json["ID"].get<std::string>();
        }
    }

    void injectDex() {
        LOGD("get system classloader");
        auto clClass = env->FindClass("java/lang/ClassLoader");
        auto getSystemClassLoader = env->GetStaticMethodID(clClass, "getSystemClassLoader",
                                                           "()Ljava/lang/ClassLoader;");
        auto systemClassLoader = env->CallStaticObjectMethod(clClass, getSystemClassLoader);

        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            return;
        }

        LOGD("create class loader");
        auto dexClClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        auto dexClInit = env->GetMethodID(dexClClass, "<init>",
                                          "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        auto buffer = env->NewDirectByteBuffer(dexVector.data(),
                                               static_cast<jlong>(dexVector.size()));
        auto dexCl = env->NewObject(dexClClass, dexClInit, buffer, systemClassLoader);

        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            return;
        }

        LOGD("load class");
        auto loadClass = env->GetMethodID(clClass, "loadClass",
                                          "(Ljava/lang/String;)Ljava/lang/Class;");
        auto entryClassName = env->NewStringUTF("es.chiteroman.playintegrityfix.EntryPoint");
        auto entryClassObj = env->CallObjectMethod(dexCl, loadClass, entryClassName);
        auto entryPointClass = (jclass) entryClassObj;

        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            return;
        }

        LOGD("call init");
        auto entryInit = env->GetStaticMethodID(entryPointClass, "init", "(Ljava/lang/String;ZZ)V");
        auto jsonStr = env->NewStringUTF(json.dump().c_str());
        env->CallStaticVoidMethod(entryPointClass, entryInit, jsonStr, spoofProvider,
                                  spoofSignature);

        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }

        env->DeleteLocalRef(entryClassName);
        env->DeleteLocalRef(entryClassObj);
        env->DeleteLocalRef(jsonStr);
        env->DeleteLocalRef(dexCl);
        env->DeleteLocalRef(buffer);
        env->DeleteLocalRef(dexClClass);
        env->DeleteLocalRef(clClass);

        LOGD("jni memory free");
    }

    void UpdateBuildFields() {
        jclass buildClass = env->FindClass("android/os/Build");
        jclass versionClass = env->FindClass("android/os/Build$VERSION");

        for (auto &[key, val]: json.items()) {
            if (!val.is_string()) continue;

            const char *fieldName = key.c_str();

            jfieldID fieldID = env->GetStaticFieldID(buildClass, fieldName, "Ljava/lang/String;");

            if (env->ExceptionCheck()) {
                env->ExceptionClear();

                fieldID = env->GetStaticFieldID(versionClass, fieldName, "Ljava/lang/String;");

                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    continue;
                }
            }

            if (fieldID != nullptr) {
                std::string str = val.get<std::string>();
                const char *value = str.c_str();
                jstring jValue = env->NewStringUTF(value);

                env->SetStaticObjectField(buildClass, fieldID, jValue);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    continue;
                }

                LOGD("Set '%s' to '%s'", fieldName, value);
            }
        }
    }
};

static std::vector<char> readFile(const char *path) {
    FILE *file = fopen(path, "rb");

    if (!file) return {};

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    std::vector<char> vector(size);
    fread(vector.data(), 1, size, file);

    fclose(file);

    return vector;
}

static bool checkOtaZip() {
    int fd = open("/system/etc/security/otacerts.zip", O_RDONLY);
    if (fd < 0)
        return true;

    std::vector<uint8_t> buf;
    uint8_t tmp[512];
    ssize_t n;
    bool found = false;
    const char* pattern = "testkey";
    const size_t patternLen = strlen(pattern);

    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        buf.insert(buf.end(), tmp, tmp + n);
        if (buf.size() >= patternLen) {
            auto it = std::search(
                buf.end() - std::min(buf.size(), patternLen + sizeof(tmp)),
                buf.end(),
                pattern, pattern + patternLen
            );
            if (it != buf.end()) {
                found = true;
                break;
            }
        }
    }

    close(fd);
    return found;
}

static void companion(int fd) {

    std::vector<char> dex, json;

    if (std::filesystem::exists(DEX_PATH)) {
        dex = readFile(DEX_PATH);
    }

    if (std::filesystem::exists(CUSTOM_JSON)) {
        json = readFile(CUSTOM_JSON);
    } else if (std::filesystem::exists(CUSTOM_JSON_FORK)) {
        json = readFile(CUSTOM_JSON_FORK);
    } else if (std::filesystem::exists(DEFAULT_JSON)) {
        json = readFile(DEFAULT_JSON);
    }

    size_t dexSize = dex.size();
    size_t jsonSize = json.size();

    xwrite(fd, &dexSize, sizeof(size_t));
    xwrite(fd, &jsonSize, sizeof(size_t));

    if (dexSize > 0) {
        xwrite(fd, dex.data(), dexSize);
    }

    if (jsonSize > 0) {
        xwrite(fd, json.data(), jsonSize);
    }

    std::string ts(TS_PATH);
    bool trickyStore = std::filesystem::exists(ts) &&
                       !std::filesystem::exists(ts + "/disable") &&
                       !std::filesystem::exists(ts + "/remove");
    xwrite(fd, &trickyStore, sizeof(bool));

    bool testSignedRom = checkOtaZip();
    xwrite(fd, &testSignedRom, sizeof(bool));
}

REGISTER_ZYGISK_MODULE(PlayIntegrityFix)

REGISTER_ZYGISK_COMPANION(companion)
