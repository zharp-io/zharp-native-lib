// The C API: settings, context (user, tags, session, breadcrumbs), handled errors, logs, spans.
#include "zharp.h"

#include "internal.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <initializer_list>
#include <map>
#include <mutex>
#include <thread>
#include <typeinfo>
#include <vector>

#if defined(_MSC_VER)
#  include <intrin.h>
#  pragma intrinsic(_ReturnAddress)
#  define ZHARP_RETURN_ADDRESS() _ReturnAddress()
#  define ZHARP_NOINLINE __declspec(noinline)
#else
#  include <cxxabi.h>
#  define ZHARP_RETURN_ADDRESS() __builtin_return_address(0)
#  define ZHARP_NOINLINE __attribute__((noinline))
#endif

namespace zharp {

const char *const DEFAULT_ENDPOINT = "https://api.zharp.io/api/v1/otlp";
const uint64_t SESSION_IDLE_NANOS = 30ull * 60 * 1000000000ull;
const size_t MAX_BREADCRUMBS = 100;
const size_t MAX_BREADCRUMB_TEXT = 16000;
const size_t MAX_TAGS = 64;

struct State {
    Config config;
    std::mutex mutex;
    std::string device_id;
    std::string session_id;
    uint64_t last_activity = 0;
    std::string user_id, user_email, user_name;
    std::vector<std::pair<std::string, std::string>> tags;
    std::deque<std::string> breadcrumbs;
};

static State *g_state = nullptr;
static std::mutex g_init_mutex;
static std::terminate_handler g_previous_terminate = nullptr;

// ── Settings ────────────────────────────────────────────────────────────────

static std::string trim(std::string text) {
    size_t a = text.find_first_not_of(" \t\r\n");
    size_t b = text.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
}

static std::string pick(const char *option, std::initializer_list<const char *> names) {
    if (option && *option) return trim(option);
    for (const char *name : names) {
        std::string value = trim(env(name));
        if (!value.empty()) return value;
    }
    return std::string();
}

static bool is_true(const std::string &value) {
    std::string v;
    for (char c : value) v += (char)tolower((unsigned char)c);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

static bool is_false(const std::string &value) {
    std::string v;
    for (char c : value) v += (char)tolower((unsigned char)c);
    return v == "0" || v == "false" || v == "no" || v == "off";
}

static bool flag(int option, const char *name, bool fallback) {
    if (option == 0) return false;
    if (option > 0) return true;
    std::string value = trim(env(name));
    if (is_true(value)) return true;
    if (is_false(value)) return false;
    return fallback;
}

std::string minidump_url(const Config &config) {
    std::string base = config.endpoint;
    const std::string otlp = "/otlp";
    if (base.size() >= otlp.size() && base.compare(base.size() - otlp.size(), otlp.size(), otlp) == 0) {
        base = base.substr(0, base.size() - otlp.size());
    }
    std::string key;
    for (unsigned char c : config.key) {
        if (isalnum(c) || c == '_' || c == '-' || c == '.' || c == '~') {
            key += (char)c;
        } else {
            char escaped[4];
            snprintf(escaped, sizeof escaped, "%%%02X", c);
            key += escaped;
        }
    }
    return base + "/minidump?key=" + key;
}

static std::string safe_folder_name(const std::string &name) {
    std::string out;
    for (unsigned char c : name) out += (isalnum(c) || c == '-' || c == '_' || c == '.') ? (char)c : '_';
    return out.empty() ? "app" : out;
}

static Config resolve(const zharp_options_t *o) {
    zharp_options_t defaults;
    zharp_options_init(&defaults);
    if (!o) o = &defaults;
    Config c;
    c.debug = flag(o->debug, "ZHARP_DEBUG", false);
    c.key = pick(o->key, {"ZHARP_KEY", "ZHARP_API_KEY"});
    std::string exe = basename_of(executable_path());
    if (exe.size() > 4 && (exe.compare(exe.size() - 4, 4, ".exe") == 0 || exe.compare(exe.size() - 4, 4, ".EXE") == 0)) exe.resize(exe.size() - 4);
    c.service = pick(o->service, {"ZHARP_SERVICE", "OTEL_SERVICE_NAME"});
    if (c.service.empty()) c.service = exe.empty() ? "app" : exe;
    c.environment = pick(o->environment, {"ZHARP_ENVIRONMENT", "ZHARP_ENV"});
    c.release = pick(o->release, {"ZHARP_RELEASE", "ZHARP_VERSION"});
    c.endpoint = pick(o->endpoint, {"ZHARP_ENDPOINT"});
    if (c.endpoint.empty()) c.endpoint = DEFAULT_ENDPOINT;
    while (!c.endpoint.empty() && c.endpoint.back() == '/') c.endpoint.pop_back();
    c.logs = flag(o->logs, "ZHARP_LOGS", false);
    c.crash_reporting = flag(o->crash_reporting, "ZHARP_CRASH_REPORTING", true);
    c.sessions = flag(o->sessions, "ZHARP_SESSIONS", true);
    c.tracing = flag(o->tracing, "ZHARP_TRACING", true);
    c.sample_rate = o->sample_rate;
    if (!(c.sample_rate > 0)) {
        std::string rate = trim(env("ZHARP_SAMPLE_RATE"));
        c.sample_rate = rate.empty() ? 1.0 : atof(rate.c_str());
    }
    if (!(c.sample_rate > 0) || c.sample_rate > 1) c.sample_rate = 1.0;
    c.handler_path = pick(o->handler_path, {"ZHARP_HANDLER_PATH"});
    if (c.handler_path.empty()) {
#if defined(_WIN32)
        c.handler_path = join_path(dirname_of(executable_path()), "crashpad_handler.exe");
#else
        c.handler_path = join_path(dirname_of(executable_path()), "crashpad_handler");
#endif
    }
    c.database_path = pick(o->database_path, {"ZHARP_DATABASE_PATH"});
    if (c.database_path.empty()) c.database_path = join_path(join_path(cache_dir(), "zharp"), safe_folder_name(c.service));
    return c;
}

// ── Context ─────────────────────────────────────────────────────────────────

/** Called with the state locked: the session, renewed after 30 minutes without activity. */
static void touch_session(State &s) {
    if (!s.config.sessions) return;
    uint64_t now = now_nanos();
    if (s.session_id.empty() || now - s.last_activity > SESSION_IDLE_NANOS) {
        s.session_id = random_uuid();
        crash_set("session_id", s.session_id);
        debug_log("session %s", s.session_id.c_str());
    }
    s.last_activity = now;
}

static const char *level_name(int level) {
    if (level >= ZHARP_LEVEL_FATAL) return "fatal";
    if (level >= ZHARP_LEVEL_ERROR) return "error";
    if (level >= ZHARP_LEVEL_WARN) return "warn";
    if (level >= ZHARP_LEVEL_INFO) return "info";
    if (level >= ZHARP_LEVEL_DEBUG) return "debug";
    return "trace";
}

static std::string breadcrumb_text(const State &s) {
    // Newest last; the oldest are dropped to fit what a crash report can carry.
    size_t total = 0, first = s.breadcrumbs.size();
    while (first > 0 && total + s.breadcrumbs[first - 1].size() + 1 <= MAX_BREADCRUMB_TEXT) {
        total += s.breadcrumbs[first - 1].size() + 1;
        first--;
    }
    std::string out;
    out.reserve(total);
    for (size_t i = first; i < s.breadcrumbs.size(); i++) {
        if (!out.empty()) out += '\n';
        out += s.breadcrumbs[i];
    }
    return out;
}

/** Called with the state locked. */
static void add_breadcrumb(State &s, const char *kind, const char *text) {
    std::string line = iso_time(now_nanos()) + " " + kind + " ";
    std::string message = text ? text : "";
    for (char &c : message) if (c == '\n' || c == '\r') c = ' ';
    if (message.size() > 500) message = message.substr(0, 500) + "…";
    line += message;
    s.breadcrumbs.push_back(line);
    while (s.breadcrumbs.size() > MAX_BREADCRUMBS) s.breadcrumbs.pop_front();
    if (crash_enabled()) crash_set("breadcrumbs", breadcrumb_text(s));
}

using Attributes = std::vector<std::pair<std::string, std::string>>;

/** Called with the state locked: who, which session, the tags. */
static void context_attributes(const State &s, Attributes &out) {
    if (!s.session_id.empty()) out.emplace_back("session.id", s.session_id);
    if (!s.user_id.empty()) out.emplace_back("enduser.id", s.user_id);
    if (!s.user_email.empty()) out.emplace_back("enduser.email", s.user_email);
    if (!s.user_name.empty()) out.emplace_back("enduser.name", s.user_name);
    for (const auto &tag : s.tags) out.push_back(tag);
}

static std::string resource_json(const Config &c, const State &s) {
    OsInfo os = os_info();
    Attributes r = {
        {"service.name", c.service},
        {"service.version", c.release},
        {"deployment.environment.name", c.environment},
        {"telemetry.sdk.name", "zharp"},
        {"telemetry.sdk.language", "cpp"},
        {"telemetry.sdk.version", ZHARP_SDK_VERSION},
        {"process.pid", std::to_string(process_id())},
        {"process.executable.name", basename_of(executable_path())},
        {"host.name", host_name()},
        {"host.arch", os.arch},
        {"os.name", os.name},
        {"os.type", os.type},
        {"os.version", os.version},
        {"device.id", s.device_id},
        {"device.model.identifier", os.model},
    };
    return otlp_attributes(r);
}

static std::string device_id(const Config &c) {
    std::string path = join_path(c.database_path, "device-id");
    std::string id;
    if (read_file(path, id)) {
        id = trim(id);
        if (id.size() == 36) return id;
    }
    id = random_uuid();
    write_file(path, id);
    return id;
}

// ── Spans (thread-local current span) ──────────────────────────────────────

}  // namespace zharp

struct zharp_span {
    bool noop = false;
    bool sampled = false;
    /** From zharp_start_async_span: never this thread's current span; freed where it ends. */
    bool detached = false;
    /** Ended on another thread than it started on: its own thread frees it. */
    std::atomic<bool> ended_elsewhere{false};
    bool ended = false;
    std::string trace_id, span_id, parent_span_id;
    std::string name;
    int kind = 1;  // OTLP: 1 internal, 2 server, 3 client
    uint64_t start = 0;
    zharp::Attributes attributes;
    int status = 0;  // 0 unset, 2 error
    std::string status_message;
    std::thread::id thread;
};

namespace zharp {

static zharp_span g_noop_span;
static const bool g_noop_ready = (g_noop_span.noop = true, g_noop_span.ended = true, true);
/** This thread's open spans, innermost last. */
static thread_local std::vector<zharp_span *> t_spans;
static thread_local char t_correlation[80];

/** This thread's current span: the innermost one still open. */
static zharp_span *current_span() {
    while (!t_spans.empty() && t_spans.back()->ended_elsewhere.load()) {
        delete t_spans.back();
        t_spans.pop_back();
    }
    return t_spans.empty() ? nullptr : t_spans.back();
}

static bool sample(double rate) {
    if (rate >= 1) return true;
    unsigned long long value = strtoull(random_hex(7).c_str(), nullptr, 16);
    return (double)value / (double)(1ull << 56) < rate;
}

static bool valid_hex(const std::string &text, size_t length) {
    if (text.size() != length) return false;
    bool nonzero = false;
    for (char c : text) {
        if (!isxdigit((unsigned char)c)) return false;
        if (c != '0') nonzero = true;
    }
    return nonzero;
}

static zharp_span *start_span(const char *name, const char *traceparent, bool detached = false) {
    State *s = g_state;
    if (!s) return &g_noop_span;
    auto *span = new zharp_span();
    span->detached = detached;
    span->name = name && *name ? name : "(unnamed)";
    span->start = now_nanos();
    span->span_id = random_hex(8);
    span->thread = std::this_thread::get_id();
    zharp_span *parent = current_span();
    std::string header = traceparent ? trim(traceparent) : std::string();
    if (header.size() >= 55 && header[2] == '-' && header[35] == '-' && header[52] == '-' &&
        valid_hex(header.substr(3, 32), 32) && valid_hex(header.substr(36, 16), 16)) {
        for (char &c : header) c = (char)tolower((unsigned char)c);
        span->trace_id = header.substr(3, 32);
        span->parent_span_id = header.substr(36, 16);
        span->sampled = s->config.tracing && (strtol(header.substr(53, 2).c_str(), nullptr, 16) & 1);
    } else if (parent) {
        span->trace_id = parent->trace_id;
        span->parent_span_id = parent->span_id;
        span->sampled = parent->sampled;
    } else {
        span->trace_id = random_hex(16);
        span->sampled = s->config.tracing && sample(s->config.sample_rate);
    }
    if (!detached) t_spans.push_back(span);
    return span;
}

static std::string span_json(const zharp_span &span, uint64_t end) {
    std::string json = "{\"traceId\":\"" + span.trace_id + "\",\"spanId\":\"" + span.span_id + "\"";
    if (!span.parent_span_id.empty()) json += ",\"parentSpanId\":\"" + span.parent_span_id + "\"";
    json += ",\"name\":" + json_string(span.name);
    json += ",\"kind\":" + std::to_string(span.kind);
    json += ",\"startTimeUnixNano\":\"" + std::to_string(span.start) + "\"";
    json += ",\"endTimeUnixNano\":\"" + std::to_string(end) + "\"";
    json += ",\"attributes\":" + otlp_attributes(span.attributes);
    json += ",\"status\":{\"code\":" + std::to_string(span.status);
    if (!span.status_message.empty()) json += ",\"message\":" + json_string(span.status_message);
    json += "}}";
    return json;
}

// ── Records ────────────────────────────────────────────────────────────────

static std::string log_record(int level, const std::string &body, const Attributes &attributes) {
    uint64_t now = now_nanos();
    std::string json = "{\"timeUnixNano\":\"" + std::to_string(now) + "\",\"observedTimeUnixNano\":\"" + std::to_string(now) + "\"";
    json += ",\"severityNumber\":" + std::to_string(level);
    std::string text = level_name(level);
    for (char &c : text) c = (char)toupper((unsigned char)c);
    json += ",\"severityText\":\"" + text + "\"";
    json += ",\"body\":{\"stringValue\":" + json_string(body) + "}";
    json += ",\"attributes\":" + otlp_attributes(attributes);
    zharp_span *span = current_span();
    if (span) json += ",\"traceId\":\"" + span->trace_id + "\",\"spanId\":\"" + span->span_id + "\"";
    json += "}";
    return json;
}

static std::string first_line(const std::string &text) {
    size_t end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end);
}

/** A handled error or message: one log record with exception.* and the stack of the call. */
static void capture(void *caller, int level, const char *type, const char *message) {
    State *s = g_state;
    if (!s) return;
    std::string name = type && *type ? type : "Error";
    std::string text = message ? message : "";
    Stack stack = capture_stack(caller);
    std::string headline = text.empty() ? name : name + ": " + first_line(text);
    std::string trace = headline;
    std::string frames = format_stack(stack);
    if (!frames.empty()) trace += "\n" + frames;

    Attributes attributes = {
        {"exception.type", name},
        {"exception.message", text},
        {"exception.stacktrace", trace},
        {"zharp.handled", "true"},
    };
    std::string modules = format_modules(stack);
    if (!modules.empty()) {
        attributes.emplace_back("zharp.native_modules", modules);
        std::string ids;
        for (const Module &module : stack.modules) {
            if (module.debug_id.empty()) continue;
            if (!ids.empty()) ids += ',';
            ids += module.debug_id;
        }
        attributes.emplace_back("zharp.native_debug_ids", ids);
    }
    std::string record;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        touch_session(*s);
        if (!s->breadcrumbs.empty()) attributes.emplace_back("zharp.breadcrumbs", breadcrumb_text(*s));
        context_attributes(*s, attributes);
        record = log_record(level, headline, attributes);
        add_breadcrumb(*s, level >= ZHARP_LEVEL_ERROR ? "error" : level_name(level), headline.c_str());
    }
    transport_enqueue(Signal::Logs, std::move(record));
    debug_log("captured %s", headline.c_str());
}

// ── Uncaught C++ exceptions ────────────────────────────────────────────────

static std::string demangle(const char *name) {
#if defined(_MSC_VER)
    std::string text = name ? name : "";
    for (const char *prefix : {"class ", "struct "}) {
        if (text.compare(0, strlen(prefix), prefix) == 0) text = text.substr(strlen(prefix));
    }
    return text;
#else
    int status = 0;
    char *out = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::string text = status == 0 && out ? out : (name ? name : "");
    free(out);
    return text;
#endif
}

std::string exception_type_name(const std::type_info &type) {
    return demangle(type.name());
}

}  // namespace zharp

/**
 * std::terminate's handler: an uncaught C++ exception's type and what() go into the crash report,
 * so the crash reads "std::runtime_error: payment declined" rather than SIGABRT. Then the previous
 * handler (or abort()) runs and Crashpad writes the dump. Named zharp_native_* so Zharp leaves it
 * out of the stack.
 */
void zharp_native_terminate_handler() {
    std::string type = "std::terminate";
    std::string message;
    if (std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception &e) {
            type = zharp::exception_type_name(typeid(e));
            message = e.what() ? e.what() : "";
        } catch (...) {
#if defined(_MSC_VER)
            type = "unknown C++ exception";
#else
            const std::type_info *info = abi::__cxa_current_exception_type();
            type = info ? zharp::exception_type_name(*info) : "unknown C++ exception";
#endif
        }
    } else {
        message = "std::terminate called without an active exception";
    }
    zharp::crash_set_exception(type, message.empty() ? "uncaught exception" : message);
    if (zharp::g_previous_terminate) zharp::g_previous_terminate();
    abort();
}

// ── The C API ────────────────────────────────────────────────────────────────

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
    std::lock_guard<std::mutex> init_lock(g_init_mutex);
    if (g_state) return 0;
    Config config = resolve(options);
    g_debug = config.debug;
    if (flag(options ? options->disabled : -1, "ZHARP_DISABLED", false)) {
        debug_log("ZHARP_DISABLED: nothing is captured or sent");
        return 1;
    }
    if (config.key.empty()) {
        warn("no key: set ZHARP_KEY (Applications → your app → Setup) or zharp_options_t.key. Nothing is sent.");
        return 1;
    }
    make_dirs(config.database_path);

    auto *s = new State();
    s->config = config;
    if (config.sessions) s->device_id = device_id(config);

    std::map<std::string, std::string> annotations = {
        {"zharp.service", config.service},
        {"zharp.sdk", ZHARP_SDK_NAME "/" ZHARP_SDK_VERSION},
        {"prod", config.service},
    };
    if (!config.release.empty()) {
        annotations["zharp.release"] = config.release;
        annotations["ver"] = config.release;
    }
    if (!config.environment.empty()) annotations["zharp.environment"] = config.environment;
    if (!s->device_id.empty()) annotations["zharp.device_id"] = s->device_id;
    if (config.crash_reporting) crash_start(config, annotations);

    if (!g_previous_terminate) g_previous_terminate = std::set_terminate(zharp_native_terminate_handler);

    transport_start(config, resource_json(config, *s));
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        touch_session(*s);
    }
    g_state = s;
    debug_log("Zharp on: service \"%s\"%s%s, sending to %s%s", config.service.c_str(),
              config.environment.empty() ? "" : ", environment ", config.environment.c_str(), config.endpoint.c_str(),
              config.logs ? ", logs on" : "");
    return 0;
}

void zharp_shutdown(void) {
    std::lock_guard<std::mutex> init_lock(g_init_mutex);
    if (!g_state) return;
    transport_stop(5000);
    // The state stays: other threads may still be calling in. Crash reporting stays on too.
    State *s = g_state;
    g_state = nullptr;
    (void)s;
}

int zharp_flush(int timeout_ms) {
    if (!g_state) return 1;
    return transport_flush(timeout_ms) ? 1 : 0;
}

ZHARP_NOINLINE void zharp_capture_error(const char *type, const char *message) {
    capture(ZHARP_RETURN_ADDRESS(), ZHARP_LEVEL_ERROR, type, message);
}

ZHARP_NOINLINE void zharp_capture_message(zharp_level_t level, const char *message) {
    int severity = level < ZHARP_LEVEL_WARN ? ZHARP_LEVEL_WARN : (int)level;
    capture(ZHARP_RETURN_ADDRESS(), severity, "Message", message);
}

int zharp_set_crash_reason(const char *type, const char *message) {
    // Crash reporting outlives zharp_shutdown, so this does too.
    if (!crash_enabled()) return 0;
    crash_set_exception(type && *type ? type : "Error", message && *message ? message : "fatal error");
    return 1;
}

void zharp_log(zharp_level_t level, const char *message) {
    State *s = g_state;
    if (!s || !message) return;
    std::string record;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        add_breadcrumb(*s, level_name(level), message);
        if (!s->config.logs) return;
        touch_session(*s);
        Attributes attributes;
        context_attributes(*s, attributes);
        record = log_record(level, message, attributes);
    }
    transport_enqueue(Signal::Logs, std::move(record));
}

void zharp_logf(zharp_level_t level, const char *format, ...) {
    if (!g_state || !format) return;
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    zharp_log(level, buffer);
}

void zharp_add_breadcrumb(const char *category, const char *message) {
    State *s = g_state;
    if (!s || !message) return;
    std::lock_guard<std::mutex> lock(s->mutex);
    std::string kind = category && *category ? category : "breadcrumb";
    for (char &c : kind) if (c == ' ') c = '_';
    add_breadcrumb(*s, kind.c_str(), message);
}

const char *zharp_log_correlation(void) {
    zharp_span *span = current_span();
    if (!span) return "";
    snprintf(t_correlation, sizeof t_correlation, " trace_id=%s span_id=%s", span->trace_id.c_str(), span->span_id.c_str());
    return t_correlation;
}

void zharp_set_user(const char *id, const char *email, const char *name) {
    State *s = g_state;
    if (!s) return;
    std::lock_guard<std::mutex> lock(s->mutex);
    s->user_id = id ? id : "";
    s->user_email = email ? email : "";
    s->user_name = name ? name : "";
    crash_set("user.id", s->user_id);
    crash_set("user.email", s->user_email);
    crash_set("user.name", s->user_name);
}

void zharp_set_tag(const char *key, const char *value) {
    State *s = g_state;
    if (!s || !key || !*key) return;
    std::lock_guard<std::mutex> lock(s->mutex);
    std::string k = key;
    for (auto it = s->tags.begin(); it != s->tags.end(); ++it) {
        if (it->first == k) {
            s->tags.erase(it);
            break;
        }
    }
    if (value && *value && s->tags.size() < MAX_TAGS) s->tags.emplace_back(k, value);
    crash_set_tag(k, value ? value : "");
}

zharp_span_t *zharp_start_span(const char *name) {
    return start_span(name, nullptr);
}

zharp_span_t *zharp_start_span_from(const char *name, const char *traceparent) {
    return start_span(name, traceparent);
}

zharp_span_t *zharp_start_async_span(const char *name) {
    return start_span(name, nullptr, true);
}

void zharp_span_set_kind(zharp_span_t *span, int kind) {
    if (!span || span->noop) return;
    span->kind = kind == 1 ? 2 : kind == 2 ? 3 : 1;
}

void zharp_span_set_name(zharp_span_t *span, const char *name) {
    if (!span || span->noop || !name || !*name) return;
    span->name = name;
}

void zharp_span_set_attribute(zharp_span_t *span, const char *key, const char *value) {
    if (!span || span->noop || !key || !*key) return;
    for (auto &kv : span->attributes) {
        if (kv.first == key) {
            kv.second = value ? value : "";
            return;
        }
    }
    if (span->attributes.size() < 128) span->attributes.emplace_back(key, value ? value : "");
}

static const char *const SECRET_PARTS[] = {"authorization", "cookie", "password", "passwd", "secret", "token", "apikey", "api-key",
                                           "accesskey", "access-key", "privatekey", "private-key", "credential", "signature",
                                           "session", "csrf", "xsrf", "jwt"};

static int is_secret_header(const char *name) {
    std::string lower;
    for (const char *p = name; p && *p; p++) lower += (char)tolower((unsigned char)*p);
    if (lower == "auth" || lower == "sid") return 1;
    for (const char *part : SECRET_PARTS) {
        if (lower.find(part) != std::string::npos) return 1;
    }
    return 0;
}

void zharp_span_set_header(zharp_span_t *span, const char *side, const char *name, const char *value) {
    if (!span || span->noop || !name || !*name) return;
    std::string lower;
    for (const char *p = name; *p; p++) lower += (char)tolower((unsigned char)*p);
    std::string which = side && strcmp(side, "response") == 0 ? "response" : "request";
    std::string key = "http." + which + ".header." + lower;
    std::string text = is_secret_header(lower.c_str()) ? "[REDACTED]" : (value ? value : "");
    if (text.size() > 1000) text = text.substr(0, 1000) + "…";
    for (auto &kv : span->attributes) {
        if (kv.first == key) {
            // A header sent twice: its values joined, as HTTP does.
            if (kv.second != "[REDACTED]") kv.second += ", " + text;
            return;
        }
    }
    if (span->attributes.size() < 128) span->attributes.emplace_back(key, text);
}

void zharp_span_set_error(zharp_span_t *span, const char *message) {
    if (!span || span->noop) return;
    span->status = 2;
    span->status_message = message ? message : "";
}

void zharp_end_span(zharp_span_t *span) {
    if (!span || span->noop || span->ended) return;
    span->ended = true;
    uint64_t end = now_nanos();
    State *s = g_state;
    if (s && span->sampled) {
        Attributes context;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            context_attributes(*s, context);
        }
        for (auto &kv : context) {
            bool present = false;
            for (auto &existing : span->attributes) present = present || existing.first == kv.first;
            if (!present) span->attributes.push_back(kv);
        }
        transport_enqueue(Signal::Traces, span_json(*span, end));
    }
    if (span->detached) {
        // On no thread's stack of open spans.
        delete span;
        return;
    }
    if (span->thread != std::this_thread::get_id()) {
        // Its own thread's stack still holds it: freed there.
        span->ended_elsewhere.store(true);
        return;
    }
    for (size_t i = t_spans.size(); i-- > 0;) {
        if (t_spans[i] == span) {
            t_spans.erase(t_spans.begin() + (long)i);
            break;
        }
    }
    delete span;
}

int zharp_traceparent(char *out, size_t size) {
    return zharp_span_traceparent(current_span(), out, size);
}

int zharp_span_traceparent(const zharp_span_t *span, char *out, size_t size) {
    if (!out || size < 56 || !span || span->noop) return 0;
    snprintf(out, size, "00-%s-%s-%s", span->trace_id.c_str(), span->span_id.c_str(), span->sampled ? "01" : "00");
    return 1;
}

}  // extern "C"
