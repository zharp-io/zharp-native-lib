/*
 * Zharp for C and C++ — crash reports, errors, logs and traces for native apps.
 *
 *   #include <zharp.h>
 *
 *   int main(void) {
 *       zharp_init(NULL);          // reads ZHARP_KEY, ZHARP_SERVICE, ZHARP_ENVIRONMENT, ZHARP_RELEASE…
 *       ...
 *       zharp_shutdown();          // sends what's left
 *   }
 *
 * Crashes are written by Crashpad's handler process (crashpad_handler, shipped next to your app)
 * and sent as minidumps. Handled errors, logs and spans go to Zharp over OTLP/HTTP from a
 * background thread. Every function is safe to call from any thread, before zharp_init and after
 * zharp_shutdown (it then does nothing).
 *
 * MIT License. https://zharp.io/docs/sdks/native
 */
#ifndef ZHARP_H
#define ZHARP_H

#include <stddef.h>

#define ZHARP_SDK_NAME "zharp-native"
#define ZHARP_SDK_VERSION "0.2.0"

#if defined(_WIN32) && defined(ZHARP_SHARED)
#  if defined(ZHARP_BUILDING)
#    define ZHARP_API __declspec(dllexport)
#  else
#    define ZHARP_API __declspec(dllimport)
#  endif
#elif defined(ZHARP_SHARED) && defined(__GNUC__)
#  define ZHARP_API __attribute__((visibility("default")))
#else
#  define ZHARP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Severity of a log line or message. */
typedef enum zharp_level {
    ZHARP_LEVEL_TRACE = 1,
    ZHARP_LEVEL_DEBUG = 5,
    ZHARP_LEVEL_INFO = 9,
    ZHARP_LEVEL_WARN = 13,
    ZHARP_LEVEL_ERROR = 17,
    ZHARP_LEVEL_FATAL = 21
} zharp_level_t;

/*
 * Settings. Start from zharp_options_init(): every field left as it is falls back to its
 * environment variable, then to the default. Strings are copied by zharp_init.
 */
typedef struct zharp_options {
    /* The app's key (zpk_…) from Applications → your app → Setup. Default: ZHARP_KEY. */
    const char *key;
    /* The service everything is filed under. Default: ZHARP_SERVICE, else the executable's name. */
    const char *service;
    /* e.g. "production". Default: ZHARP_ENVIRONMENT. */
    const char *environment;
    /* The release (version) of your app. Default: ZHARP_RELEASE. */
    const char *release;
    /* For Zharp's own testing; leave it unset. Default: ZHARP_ENDPOINT, else https://api.zharp.io/api/v1/otlp. */
    const char *endpoint;
    /* 1 sends zharp_log lines to Zharp as logs (off by default: the agent brings your log files
       in). -1: ZHARP_LOGS. Lines are breadcrumbs either way. */
    int logs;
    /* 1 writes and sends crash reports with Crashpad (default). -1: ZHARP_CRASH_REPORTING. */
    int crash_reporting;
    /* 1 (default): a desktop app — a session per launch (or after 30 minutes idle) and a device ID.
       0: a daemon or service — neither. -1: ZHARP_SESSIONS. */
    int sessions;
    /* 0 sends errors and logs only, no spans. -1: ZHARP_TRACING (default on). */
    int tracing;
    /* Share of traces kept, 0 < rate <= 1. 0: ZHARP_SAMPLE_RATE, else 1. Errors are always sent. */
    double sample_rate;
    /* Crashpad's handler executable. Default: ZHARP_HANDLER_PATH, else crashpad_handler(.exe) next
       to your executable (CMake: zharp_add_crash_handler(<target>) puts it there). */
    const char *handler_path;
    /* Where crash reports, the device ID and unsent events are kept. Default: ZHARP_DATABASE_PATH,
       else the user's cache folder (…/zharp/<service>). */
    const char *database_path;
    /* 1 prints what the SDK does to stderr. -1: ZHARP_DEBUG. */
    int debug;
    /* 1 turns the SDK off: nothing is captured or sent. -1: ZHARP_DISABLED. */
    int disabled;
} zharp_options_t;

/* Fills `options` with "not set": every value then comes from the environment or its default. */
ZHARP_API void zharp_options_init(zharp_options_t *options);

/* Starts Zharp. `options` may be NULL (everything from the environment). Returns 0 when it is
   sending, 1 when it is off (no key, ZHARP_DISABLED) — your app runs the same either way. Call it
   once, early in main. */
ZHARP_API int zharp_init(const zharp_options_t *options);

/* Sends what is queued (waiting up to 5 seconds), stops the background thread. Crash reporting
   stays on until the process exits. */
ZHARP_API void zharp_shutdown(void);

/* Sends what is queued, waiting up to `timeout_ms`. Returns 1 when everything was sent. */
ZHARP_API int zharp_flush(int timeout_ms);

/* ── Errors ─────────────────────────────────────────────────────────────── */

/* Reports an error you handled: an issue (marked handled) with `type` as its name, `message`, the
   stack of this call, the user, the session and the current span. */
ZHARP_API void zharp_capture_error(const char *type, const char *message);

/* Reports a message as an issue, with the stack of this call. ZHARP_LEVEL_WARN and above. */
ZHARP_API void zharp_capture_message(zharp_level_t level, const char *message);

/* Names the crash that is about to happen, for code that aborts on purpose (Qt's qFatal, your
   own assert handler): the crash report then reads as "type: message" instead of SIGABRT.
   Returns 1 when a crash report will carry it, 0 when crash reporting is off (report it with
   zharp_capture_error instead). */
ZHARP_API int zharp_set_crash_reason(const char *type, const char *message);

/* ── Logs and breadcrumbs ───────────────────────────────────────────────── */

/* A log line. Always kept as a breadcrumb (the last 100 go with a crash or an error); sent to
   Zharp as a log only when logs are on (ZHARP_LOGS=true). */
ZHARP_API void zharp_log(zharp_level_t level, const char *message);

/* zharp_log with printf formatting. */
ZHARP_API void zharp_logf(zharp_level_t level, const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* A breadcrumb without a log: "opened the settings window", "loaded level 3". */
ZHARP_API void zharp_add_breadcrumb(const char *category, const char *message);

/* " trace_id=<32 hex> span_id=<16 hex>" for the current span on this thread, or "" outside one.
   Append it to the lines your own logger writes, and the Zharp agent links them to the trace.
   The string belongs to this thread and changes with the next call. */
ZHARP_API const char *zharp_log_correlation(void);

/* ── Context ────────────────────────────────────────────────────────────── */

/* Who is using the app; any argument may be NULL. zharp_set_user(NULL, NULL, NULL) clears it. */
ZHARP_API void zharp_set_user(const char *id, const char *email, const char *name);

/* A tag on everything sent after it, and on crashes. NULL value removes it. Up to 64 tags. */
ZHARP_API void zharp_set_tag(const char *key, const char *value);

/* ── Traces ─────────────────────────────────────────────────────────────── */

typedef struct zharp_span zharp_span_t;

/* Starts a span: a child of this thread's current span, else a new trace. It becomes the current
   span on this thread until it ends. Never NULL (a no-op span when Zharp is off). */
ZHARP_API zharp_span_t *zharp_start_span(const char *name);

/* Starts a span that continues a trace from another process: `traceparent` is the W3C header
   ("00-<trace id>-<span id>-01") a request came with. NULL or invalid starts a new trace. */
ZHARP_API zharp_span_t *zharp_start_span_from(const char *name, const char *traceparent);

/* Starts a span for work that finishes later, such as an asynchronous HTTP call: a child of this
   thread's current span that does not become the current span, so what this thread does next
   isn't filed under it. End it on any thread. */
ZHARP_API zharp_span_t *zharp_start_async_span(const char *name);

/* Marks the span as a server request (1), a client call (2) or internal work (0, the default). */
ZHARP_API void zharp_span_set_kind(zharp_span_t *span, int kind);

/* Renames the span, e.g. once a request has been routed. */
ZHARP_API void zharp_span_set_name(zharp_span_t *span, const char *name);

ZHARP_API void zharp_span_set_attribute(zharp_span_t *span, const char *key, const char *value);

/* A request or response header ("request" / "response"), as http.<side>.header.<name>. Secrets
   (authorization, cookies, tokens, API keys, sessions…) are kept as [REDACTED]. */
ZHARP_API void zharp_span_set_header(zharp_span_t *span, const char *side, const char *name, const char *value);

/* Marks the span as failed, with a short description. */
ZHARP_API void zharp_span_set_error(zharp_span_t *span, const char *message);

/* Ends the span and queues it. The span must not be used after this. */
ZHARP_API void zharp_end_span(zharp_span_t *span);

/* Writes this thread's current span as a W3C traceparent header value into `out` (at least 56
   bytes), for an outgoing request. Returns 0 when there is no current span. */
ZHARP_API int zharp_traceparent(char *out, size_t size);

/* The same for a given span (one from zharp_start_async_span, say). Returns 0 for a no-op span. */
ZHARP_API int zharp_span_traceparent(const zharp_span_t *span, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* ZHARP_H */
