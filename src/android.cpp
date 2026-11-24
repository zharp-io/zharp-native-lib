// Zharp's C API on Android: the app's Java side runs Zharp for Android (io.zharp:android 1.5+),
// and this forwards to it over JNI, so a Qt (or any NDK) app has one SDK, one session and one
// outbox. What each part does here:
//
//   crashes          Android's own exit record, read by Zharp for Android on the next launch:
//                    the tombstone's crashed thread, abort message (an uncaught C++ exception,
//                    qFatal through zharp_set_crash_reason) and libraries. No signal handler.
//   handled errors   zharp_capture_error: this thread's stack in Zharp's native frame style with
//                    each app library's GNU build ID (Zharp.captureNativeError), so the backend
//                    names the frames from the .sym files the Gradle plugin uploaded.
//   user, tags       Zharp.setUser / setTag; the tag screen.name is Zharp.trackScreen.
//   logs             Qt's messages reach logcat, which Zharp for Android collects; zharp_log
//                    lines aren't sent twice.
//   spans            not on Android yet: zharp_start_span returns a span that records nothing.
#include "internal.h"
#include "zharp.h"

#include <android/log.h>
#include <android/set_abort_message.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#if defined(__GNUC__)
#  define ZHARP_NOINLINE __attribute__((noinline))
#  define ZHARP_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace zharp {

namespace {

JavaVM *g_vm = nullptr;
jclass g_zharp = nullptr;  // io.zharp.android.Zharp, a global ref (FindClass only finds app classes on Java threads)
jmethodID g_capture = nullptr, g_set_user = nullptr, g_set_tag = nullptr, g_track_screen = nullptr, g_flush = nullptr;
bool g_on = false;
std::mutex g_mutex;
pthread_key_t g_detach_key;

/** The JVM the app runs in: Android's runtime exports JNI_GetCreatedJavaVMs. */
JavaVM *java_vm() {
    if (g_vm) return g_vm;
    using GetVMs = jint (*)(JavaVM **, jsize, jsize *);
    auto get = (GetVMs)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    for (const char *lib : {"libnativehelper.so", "libart.so"}) {
        if (get) break;
        if (void *handle = dlopen(lib, RTLD_NOW | RTLD_NOLOAD)) get = (GetVMs)dlsym(handle, "JNI_GetCreatedJavaVMs");
    }
    JavaVM *vm = nullptr;
    jsize count = 0;
    if (get && get(&vm, 1, &count) == JNI_OK && count > 0) g_vm = vm;
    return g_vm;
}

void detach(void *) {
    if (g_vm) g_vm->DetachCurrentThread();
}

/** This thread's JNIEnv, attaching it (and detaching when it exits) if it isn't a Java thread. */
JNIEnv *env() {
    JavaVM *vm = java_vm();
    if (!vm) return nullptr;
    JNIEnv *e = nullptr;
    if (vm->GetEnv((void **)&e, JNI_VERSION_1_6) == JNI_OK) return e;
    if (vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return nullptr;
    pthread_setspecific(g_detach_key, (void *)1);
    return e;
}

bool clear(JNIEnv *e) {
    if (!e->ExceptionCheck()) return false;
    if (g_debug) e->ExceptionDescribe();
    e->ExceptionClear();
    return true;
}

jstring str(JNIEnv *e, const std::string &text) {
    // NewStringUTF wants modified UTF-8; Zharp's strings are plain text, so a NUL-free copy does.
    return e->NewStringUTF(text.c_str());
}

/** The app's Context: Qt's, or the current Application (both public static Java methods). */
jobject app_context(JNIEnv *e) {
    if (jclass qt = e->FindClass("org/qtproject/qt/android/QtNative")) {
        jmethodID get = e->GetStaticMethodID(qt, "getContext", "()Landroid/content/Context;");
        if (!clear(e) && get) {
            jobject context = e->CallStaticObjectMethod(qt, get);
            if (!clear(e) && context) return context;
        } else {
            clear(e);
        }
    } else {
        clear(e);
    }
    jclass thread = e->FindClass("android/app/ActivityThread");
    if (clear(e) || !thread) return nullptr;
    jmethodID current = e->GetStaticMethodID(thread, "currentApplication", "()Landroid/app/Application;");
    if (clear(e) || !current) return nullptr;
    jobject app = e->CallStaticObjectMethod(thread, current);
    return clear(e) ? nullptr : app;
}

void set_option(JNIEnv *e, jclass type, jobject options, const char *setter, const char *value) {
    if (!value || !*value) return;
    jmethodID m = e->GetMethodID(type, setter, "(Ljava/lang/String;)V");
    if (clear(e) || !m) return;
    jstring v = str(e, value);
    e->CallVoidMethod(options, m, v);
    clear(e);
    e->DeleteLocalRef(v);
}

const char *pick(const char *value, const char *variable) {
    if (value && *value) return value;
    const char *from_env = getenv(variable);
    return from_env && *from_env ? from_env : nullptr;
}

std::string native_build_ids(const Stack &stack) {
    std::string out;
    for (const Module &module : stack.modules) {
        if (module.code_id.empty() || module.name.find(".so") == std::string::npos) continue;
        if (!out.empty()) out += '\n';
        out += module.code_id + " " + module.name;
    }
    return out;
}

std::string first_line(const std::string &text) {
    size_t end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end);
}

void capture(void *caller, const char *type, const char *message) {
    if (!g_on) return;
    JNIEnv *e = env();
    if (!e) return;
    std::string name = type && *type ? type : "Error";
    std::string text = message ? message : "";
    Stack stack = capture_stack(caller);
    std::string trace = text.empty() ? name : name + ": " + first_line(text);
    std::string frames = format_stack(stack);
    if (!frames.empty()) trace += "\n" + frames;
    jstring a = str(e, name), b = str(e, text), c = str(e, trace), d = str(e, native_build_ids(stack));
    e->CallStaticVoidMethod(g_zharp, g_capture, a, b, c, d);
    clear(e);
    e->DeleteLocalRef(a);
    e->DeleteLocalRef(b);
    e->DeleteLocalRef(c);
    e->DeleteLocalRef(d);
}

}  // namespace
}  // namespace zharp

using namespace zharp;

extern "C" {

void zharp_options_init(zharp_options_t *options) {
    if (!options) return;
    memset(options, 0, sizeof *options);
    options->logs = -1;
    options->crash_reporting = -1;
    options->sessions = -1;
    options->tracing = -1;
    options->debug = -1;
    options->disabled = -1;
}

int zharp_init(const zharp_options_t *options) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_on) return 0;
    pthread_key_create(&g_detach_key, detach);
    const char *debug = getenv("ZHARP_DEBUG");
    g_debug = (options && options->debug == 1) || (debug && *debug == '1');
    if ((options && options->disabled == 1) || getenv("ZHARP_DISABLED")) return 1;
    JNIEnv *e = env();
    if (!e) {
        warn("no Java VM found: Zharp for Android can't be reached, nothing is sent");
        return 1;
    }
    jclass zharp = e->FindClass("io/zharp/android/Zharp");
    jclass type = zharp ? e->FindClass("io/zharp/android/ZharpOptions") : nullptr;
    if (clear(e) || !zharp || !type) {
        warn("io.zharp:android isn't in the app: add implementation(\"io.zharp:android:1.5.0\") to its build.gradle. Nothing is sent.");
        return 1;
    }
    g_zharp = (jclass)e->NewGlobalRef(zharp);
    g_capture = e->GetStaticMethodID(zharp, "captureNativeError", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    g_set_user = e->GetStaticMethodID(zharp, "setUser", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    g_set_tag = e->GetStaticMethodID(zharp, "setTag", "(Ljava/lang/String;Ljava/lang/String;)V");
    g_track_screen = e->GetStaticMethodID(zharp, "trackScreen", "(Ljava/lang/String;)V");
    g_flush = e->GetStaticMethodID(zharp, "flush", "()V");
    jmethodID init = e->GetStaticMethodID(zharp, "init", "(Landroid/content/Context;Lio/zharp/android/ZharpOptions;)V");
    jmethodID active = e->GetStaticMethodID(zharp, "isActive", "()Z");
    if (clear(e) || !g_capture || !g_set_user || !g_set_tag || !g_track_screen || !g_flush || !init || !active) {
        warn("io.zharp:android is older than 1.5.0: update it. Nothing is sent.");
        return 1;
    }
    // The app may have started Zharp for Android itself (Application.onCreate): that one stays.
    bool running = e->CallStaticBooleanMethod(zharp, active);
    if (clear(e)) running = false;
    if (!running) {
        jobject context = app_context(e);
        jmethodID make = e->GetMethodID(type, "<init>", "()V");
        jobject javaOptions = (!clear(e) && make && context) ? e->NewObject(type, make) : nullptr;
        if (clear(e) || !javaOptions) {
            warn("couldn't start Zharp for Android (no Context). Nothing is sent.");
            return 1;
        }
        set_option(e, type, javaOptions, "setKey", pick(options ? options->key : nullptr, "ZHARP_KEY"));
        set_option(e, type, javaOptions, "setService", pick(options ? options->service : nullptr, "ZHARP_SERVICE"));
        set_option(e, type, javaOptions, "setEnvironment", pick(options ? options->environment : nullptr, "ZHARP_ENVIRONMENT"));
        set_option(e, type, javaOptions, "setRelease", pick(options ? options->release : nullptr, "ZHARP_RELEASE"));
        set_option(e, type, javaOptions, "setEndpoint", pick(options ? options->endpoint : nullptr, "ZHARP_ENDPOINT"));
        e->CallStaticVoidMethod(zharp, init, context, javaOptions);
        if (clear(e)) {
            warn("Zharp for Android didn't start. Nothing is sent.");
            return 1;
        }
    }
    g_on = true;
    debug_log("Zharp on, through Zharp for Android");
    return 0;
}

void zharp_shutdown(void) {
    zharp_flush(5000);
}

int zharp_flush(int) {
    if (!g_on) return 1;
    JNIEnv *e = env();
    if (!e) return 0;
    e->CallStaticVoidMethod(g_zharp, g_flush);
    return clear(e) ? 0 : 1;
}

ZHARP_NOINLINE void zharp_capture_error(const char *type, const char *message) {
    capture(ZHARP_RETURN_ADDRESS(), type, message);
}

ZHARP_NOINLINE void zharp_capture_message(zharp_level_t level, const char *message) {
    if (level < ZHARP_LEVEL_WARN) return;
    capture(ZHARP_RETURN_ADDRESS(), "Message", message);
}

int zharp_set_crash_reason(const char *type, const char *message) {
    // The tombstone keeps the abort message: the crash reads "type: message" instead of SIGABRT.
    std::string text = std::string(type && *type ? type : "Error") + ": " + (message ? message : "");
    android_set_abort_message(text.c_str());
    return 1;
}

void zharp_log(zharp_level_t, const char *) {
    // Logs reach Zharp through logcat (Zharp for Android collects it): not sent twice.
}

void zharp_logf(zharp_level_t, const char *, ...) {}

void zharp_add_breadcrumb(const char *, const char *) {}

const char *zharp_log_correlation(void) {
    return "";
}

void zharp_set_user(const char *id, const char *email, const char *name) {
    if (!g_on) return;
    JNIEnv *e = env();
    if (!e) return;
    jstring a = id && *id ? str(e, id) : nullptr, b = email && *email ? str(e, email) : nullptr, c = name && *name ? str(e, name) : nullptr;
    e->CallStaticVoidMethod(g_zharp, g_set_user, a, b, c);
    clear(e);
}

void zharp_set_tag(const char *key, const char *value) {
    if (!g_on || !key || !*key) return;
    JNIEnv *e = env();
    if (!e) return;
    if (strcmp(key, "screen.name") == 0) {
        if (!value || !*value) return;
        jstring v = str(e, value);
        e->CallStaticVoidMethod(g_zharp, g_track_screen, v);
        clear(e);
        e->DeleteLocalRef(v);
        return;
    }
    jstring k = str(e, key), v = value ? str(e, value) : nullptr;
    e->CallStaticVoidMethod(g_zharp, g_set_tag, k, v);
    clear(e);
    e->DeleteLocalRef(k);
    if (v) e->DeleteLocalRef(v);
}

// Spans aren't on Android yet: one span that records nothing, so instrumented code runs unchanged.
struct zharp_span {
    int unused;
};
static zharp_span g_noop_span = {0};

zharp_span_t *zharp_start_span(const char *) { return &g_noop_span; }
zharp_span_t *zharp_start_span_from(const char *, const char *) { return &g_noop_span; }
zharp_span_t *zharp_start_async_span(const char *) { return &g_noop_span; }
void zharp_span_set_kind(zharp_span_t *, int) {}
void zharp_span_set_name(zharp_span_t *, const char *) {}
void zharp_span_set_attribute(zharp_span_t *, const char *, const char *) {}
void zharp_span_set_header(zharp_span_t *, const char *, const char *, const char *) {}
void zharp_span_set_error(zharp_span_t *, const char *) {}
void zharp_end_span(zharp_span_t *) {}
int zharp_traceparent(char *, size_t) { return 0; }
int zharp_span_traceparent(const zharp_span_t *, char *, size_t) { return 0; }

}  // extern "C"
