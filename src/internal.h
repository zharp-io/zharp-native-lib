// Internals shared by the SDK's sources. Not installed; not part of the API.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <typeinfo>
#include <vector>

namespace zharp {

// ── Settings ────────────────────────────────────────────────────────────────

struct Config {
    std::string key;
    std::string service;
    std::string environment;
    std::string release;
    /** The OTLP base, without a trailing slash: https://api.zharp.io/api/v1/otlp. */
    std::string endpoint;
    bool logs = false;
    bool crash_reporting = true;
    bool sessions = true;
    bool tracing = true;
    double sample_rate = 1.0;
    std::string handler_path;
    std::string database_path;
    bool debug = false;
};

/** POST /api/v1/minidump?key=… next to the OTLP endpoint. */
std::string minidump_url(const Config &config);

/** Prints "[zharp] …" to stderr when debug output is on (or always, for warnings). */
void debug_log(const char *format, ...);
void warn(const char *format, ...);
extern bool g_debug;

// ── Platform ────────────────────────────────────────────────────────────────

std::string env(const char *name);
std::string executable_path();
std::string dirname_of(const std::string &path);
std::string basename_of(const std::string &path);
std::string join_path(const std::string &a, const std::string &b);
/** The user's cache folder: $XDG_CACHE_HOME or ~/.cache, ~/Library/Caches, %LOCALAPPDATA%. */
std::string cache_dir();
bool make_dirs(const std::string &path);
bool file_exists(const std::string &path);
bool read_file(const std::string &path, std::string &out);
bool write_file(const std::string &path, const std::string &data);
bool remove_file(const std::string &path);
/** File names (not paths) in a folder, sorted. */
std::vector<std::string> list_dir(const std::string &path);
std::string host_name();
struct OsInfo {
    std::string name;     // "Linux", "Windows", "macOS"
    std::string version;  // "6.8.0", "10.0.26200", "15.1"
    std::string type;     // "linux", "windows", "darwin"
    std::string arch;     // "amd64", "arm64"
    std::string model;    // macOS: "Mac15,3"
};
OsInfo os_info();
int process_id();

// ── Time and randomness ────────────────────────────────────────────────────

uint64_t now_nanos();
/** 2026-11-23T10:15:00.123Z */
std::string iso_time(uint64_t nanos);
std::string random_hex(size_t bytes);
std::string random_uuid();

// ── JSON ────────────────────────────────────────────────────────────────────

/** `text` as a JSON string, quotes included. Invalid UTF-8 becomes U+FFFD. */
std::string json_string(const std::string &text);
/** OTLP KeyValue list: [{"key":"k","value":{"stringValue":"v"}},…] */
std::string otlp_attributes(const std::vector<std::pair<std::string, std::string>> &attributes);

// ── Stacks ──────────────────────────────────────────────────────────────────

struct Module {
    std::string name;        // "shop", "libfoo.so", "Shop.exe"
    std::string debug_id;    // Breakpad's: GUID + age, 33+ uppercase hex ('' when unknown)
    std::string debug_file;  // "Shop.pdb" on Windows, the module's own name elsewhere
    std::string code_id;     // ELF: the GNU build ID as lowercase hex ('' elsewhere, or when it has none)
    uint64_t base = 0;       // where its addresses start (what .sym offsets are relative to)
    uint64_t size = 0;
};

struct Frame {
    uint64_t address = 0;  // a return address (every frame of a captured stack is one)
    int module = -1;       // index into Stack::modules
    std::string function;  // from the dynamic symbol table, when the module exports it
};

struct Stack {
    std::vector<Frame> frames;
    std::vector<Module> modules;  // only the modules the frames are in
};

/**
 * This thread's stack, from the caller of the public function (whose return address is
 * `caller`; frames above it are the SDK's own and are dropped). Up to `max` frames.
 */
Stack capture_stack(void *caller, int max = 64);

/**
 * The stack as Zharp's native frame style: "#0  Cart::total() [shop +0x1234]", or
 * "#0  shop +0x1234" without a name. The backend names the frames from the uploaded .sym files.
 */
std::string format_stack(const Stack &stack);
/** One line per module: "<name>\t<debug id>\t<debug file>" (attribute zharp.native_modules). */
std::string format_modules(const Stack &stack);

/** Breakpad's debug ID for a GNU build ID or a Mach-O UUID: the first 16 bytes as a GUID, age 0. */
std::string debug_id_from_bytes(const unsigned char *bytes, size_t length, bool swap_guid_fields);

// ── Transport ───────────────────────────────────────────────────────────────

enum class Signal { Logs, Traces };

/** Starts the sender thread. `resource` is the OTLP resource's attributes JSON list. */
void transport_start(const Config &config, const std::string &resource);
/** Queues one OTLP log record or span (JSON object). Dropped past the queue's limit. */
void transport_enqueue(Signal signal, std::string record);
/** Sends what is queued; true when all of it was sent within `timeout_ms`. */
bool transport_flush(int timeout_ms);
void transport_stop(int timeout_ms);

/** One POST. Returns the HTTP status, or -1 when nothing came back (error in `error`). */
int http_post(const std::string &url, const std::vector<std::pair<std::string, std::string>> &headers,
              const std::string &body, int timeout_ms, std::string *error);
void http_global_init();

// ── Crashes ─────────────────────────────────────────────────────────────────

/** Starts Crashpad's handler. False (with a warning) when it can't. */
bool crash_start(const Config &config, const std::map<std::string, std::string> &annotations);
bool crash_enabled();
void crash_set(const char *which, const std::string &value);  // "user.id", "session_id", "breadcrumbs"…
void crash_set_tag(const std::string &key, const std::string &value);
/**
 * What the crash about to happen is (the std::terminate handler, zharp_set_crash_reason): its type
 * and message, and the stack where it was named, recorded in-process (where every module's unwind
 * tables are at hand) as format_stack and format_modules write it. Zharp names those frames from
 * the .sym files, as for handled errors, when the walked dump can't get through the libraries
 * an abort runs in.
 */
void crash_set_exception(const std::string &type, const std::string &message, const std::string &stack = std::string(),
                         const std::string &modules = std::string());
/** "std::runtime_error" for a type_info, as each compiler names it. */
std::string exception_type_name(const std::type_info &type);

}  // namespace zharp
