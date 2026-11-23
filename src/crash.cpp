// Crashes: Crashpad's handler, a separate process that writes the minidump from outside the
// crashing app (so it works when the heap or stack is corrupt) and uploads it to Zharp's
// /api/v1/minidump. What Zharp needs with the dump — release, environment, session, device, user,
// breadcrumbs, tags — rides along as Crashpad annotations, which the handler sends as form fields.
#include "internal.h"

#if defined(ZHARP_WITH_CRASHPAD)

#include <memory>
#include <mutex>

#include "client/annotation.h"
#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/settings.h"
#include "client/simple_string_dictionary.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <exception>
#endif

#if defined(__linux__) || defined(__ANDROID__)
// On Linux the handler finds this process's CrashpadInfo (and so every annotation set while it
// runs) through the .note.crashpad.info ELF note. Crashpad links the note in through a reference
// GCC optimises away at -O2; this keeps it, so the note is in every binary Zharp is linked into.
extern "C" int *CRASHPAD_NOTE_REFERENCE;
__attribute__((used)) static int **const zharp_keep_crashpad_note = &CRASHPAD_NOTE_REFERENCE;
#endif

namespace zharp {

namespace {

// Read by the handler from this process's memory when it crashes: static storage only.
crashpad::StringAnnotation<64> g_session("zharp.session_id");
crashpad::StringAnnotation<256> g_user_id("zharp.user.id");
crashpad::StringAnnotation<256> g_user_email("zharp.user.email");
crashpad::StringAnnotation<256> g_user_name("zharp.user.name");
crashpad::StringAnnotation<16000> g_breadcrumbs("zharp.breadcrumbs");
crashpad::StringAnnotation<256> g_exception_type("zharp.exception.type");
crashpad::StringAnnotation<4096> g_exception_message("zharp.exception.message");

crashpad::SimpleStringDictionary *g_tags = nullptr;
crashpad::CrashpadClient *g_client = nullptr;
bool g_enabled = false;

base::FilePath file_path(const std::string &path) {
#if defined(_WIN32)
    int n = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), &wide[0], n);
    return base::FilePath(wide);
#else
    return base::FilePath(path);
#endif
}

/** Cut at `max` bytes without splitting a UTF-8 character. */
std::string fit(const std::string &value, size_t max) {
    if (value.size() <= max) return value;
    size_t end = max;
    while (end > 0 && ((unsigned char)value[end] & 0xc0) == 0x80) end--;
    return value.substr(0, end);
}

template <crashpad::Annotation::ValueSizeType N>
void set(crashpad::StringAnnotation<N> &annotation, const std::string &value) {
    if (value.empty()) {
        annotation.Clear();
    } else {
        annotation.Set(fit(value, N - 1).c_str());
    }
}

#if defined(_MSC_VER)
// On Windows an uncaught C++ exception never reaches std::terminate's handler: it leaves as SEH
// exception 0xE06D7363 and Crashpad's unhandled-exception filter writes the dump first. This filter
// goes in front of Crashpad's and reads the type (and what(), for a std::exception) from the
// exception record the way the MSVC runtime lays it out, then hands over to Crashpad.
constexpr DWORD kMsvcCppException = 0xE06D7363;
constexpr ULONG_PTR kMsvcMagic = 0x19930520;

// <ehdata.h>: on 64-bit Windows the pointers inside are offsets from the throwing image's base.
struct ThrowInfoRva { unsigned attributes; int unwind; int forward_compat; int catchable_types; };
struct CatchableTypeRva { unsigned properties; int type; int mdisp; int pdisp; int vdisp; int size; int copy; };

LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

const char *safe_what(const std::exception *e) {
    __try {
        return e->what();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

LONG WINAPI unhandled_cpp_exception(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode == kMsvcCppException && record->NumberParameters >= 3 &&
        record->ExceptionInformation[0] == kMsvcMagic && record->ExceptionInformation[2]) {
        const char *object = reinterpret_cast<const char *>(record->ExceptionInformation[1]);
#if defined(_WIN64)
        const char *base = record->NumberParameters >= 4 ? reinterpret_cast<const char *>(record->ExceptionInformation[3]) : nullptr;
#else
        const char *base = nullptr;
#endif
        auto at = [base](int offset) { return base ? base + offset : reinterpret_cast<const char *>((uintptr_t)(unsigned)offset); };
        const auto *throw_info = reinterpret_cast<const ThrowInfoRva *>(record->ExceptionInformation[2]);
        if (throw_info->catchable_types) {
            const int *types = reinterpret_cast<const int *>(at(throw_info->catchable_types));
            std::string type;
            std::string message;
            // The first catchable type is the thrown one; the rest are its bases.
            for (int i = 0; i < types[0]; i++) {
                const auto *catchable = reinterpret_cast<const CatchableTypeRva *>(at(types[1 + i]));
                const auto *type_info = reinterpret_cast<const std::type_info *>(at(catchable->type));
                if (i == 0) type = exception_type_name(*type_info);
                if (object && catchable->pdisp < 0 && *type_info == typeid(std::exception)) {
                    const char *what = safe_what(reinterpret_cast<const std::exception *>(object + catchable->mdisp));
                    if (what) message = what;
                    break;
                }
            }
            if (!type.empty()) crash_set_exception(type, message.empty() ? "uncaught exception" : message);
        }
    }
    return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
#endif

}  // namespace

bool crash_start(const Config &config, const std::map<std::string, std::string> &annotations) {
    if (g_client) return g_enabled;
    if (config.handler_path.empty() || !file_exists(config.handler_path)) {
        warn("crash reporting is off: Crashpad's handler isn't at %s. Ship crashpad_handler next to your executable "
             "(CMake: zharp_add_crash_handler(<target>)) or set ZHARP_HANDLER_PATH.",
             config.handler_path.empty() ? "(no path)" : config.handler_path.c_str());
        return false;
    }
    std::string database = join_path(config.database_path, "crashpad");
    make_dirs(database);
    std::unique_ptr<crashpad::CrashReportDatabase> db = crashpad::CrashReportDatabase::Initialize(file_path(database));
    if (db && db->GetSettings()) db->GetSettings()->SetUploadsEnabled(true);

    g_tags = new crashpad::SimpleStringDictionary();
    crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(g_tags);

    // Without --no-rate-limit the handler sends one report an hour and drops the rest.
    std::vector<std::string> arguments = {"--no-rate-limit"};
    g_client = new crashpad::CrashpadClient();
    std::string url = minidump_url(config);
    bool ok = g_client->StartHandler(file_path(config.handler_path), file_path(database), file_path(database), url,
                                     /*http_proxy*/ std::string(), annotations, arguments,
                                     /*restartable*/ true, /*asynchronous_start*/ false);
    if (!ok) {
        warn("crash reporting is off: Crashpad's handler (%s) didn't start", config.handler_path.c_str());
        return false;
    }
    g_enabled = true;
#if defined(_MSC_VER)
    // After StartHandler, which set Crashpad's own filter: ours runs first and chains to it.
    g_previous_filter = SetUnhandledExceptionFilter(unhandled_cpp_exception);
#endif
    debug_log("crash reporting on: %s, reports in %s", config.handler_path.c_str(), database.c_str());
    return true;
}

bool crash_enabled() {
    return g_enabled;
}

void crash_set(const char *which, const std::string &value) {
    std::string name = which;
    if (name == "session_id") set(g_session, value);
    else if (name == "user.id") set(g_user_id, value);
    else if (name == "user.email") set(g_user_email, value);
    else if (name == "user.name") set(g_user_name, value);
    else if (name == "breadcrumbs") set(g_breadcrumbs, value);
}

void crash_set_tag(const std::string &key, const std::string &value) {
    if (!g_tags) return;
    std::string k = fit(key, crashpad::SimpleStringDictionary::key_size - 1);
    if (value.empty()) g_tags->RemoveKey(k.c_str());
    else g_tags->SetKeyValue(k.c_str(), fit(value, crashpad::SimpleStringDictionary::value_size - 1).c_str());
}

void crash_set_exception(const std::string &type, const std::string &message) {
    set(g_exception_type, type);
    set(g_exception_message, message);
}

}  // namespace zharp

#else  // Built without Crashpad (ZHARP_CRASHPAD=OFF): errors, logs and spans only.

namespace zharp {

bool crash_start(const Config &, const std::map<std::string, std::string> &) {
    debug_log("built without Crashpad: crashes aren't reported");
    return false;
}
bool crash_enabled() { return false; }
void crash_set(const char *, const std::string &) {}
void crash_set_tag(const std::string &, const std::string &) {}
void crash_set_exception(const std::string &, const std::string &) {}

}  // namespace zharp

#endif
