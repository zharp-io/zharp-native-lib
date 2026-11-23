// The SDK end to end against a stand-in OTLP receiver: what a handled error, a message, logs and
// spans look like on the wire; logs off by default; an offline batch kept and sent by the next
// launch; ZHARP_DISABLED and a missing key.
#include <zharp.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

#include "check.h"
#include "receiver.h"

#if defined(_WIN32)
#  include <process.h>
#  define getpid _getpid
static void set_env(const char *name, const char *value) { _putenv_s(name, value ? value : ""); }
#else
#  include <unistd.h>
static void set_env(const char *name, const char *value) {
    if (value) setenv(name, value, 1);
    else unsetenv(name);
}
#endif

#if defined(_MSC_VER)
#  define NOINLINE __declspec(noinline)
#else
#  define NOINLINE __attribute__((noinline))
#endif

static std::string g_database;

static zharp_options_t options_for(int port, int logs) {
    static std::string endpoint;
    endpoint = "http://127.0.0.1:" + std::to_string(port) + "/api/v1/otlp";
    zharp_options_t options;
    zharp_options_init(&options);
    options.key = "zpk_test";
    options.service = "itest";
    options.release = "1.2.3";
    options.environment = "test";
    options.endpoint = endpoint.c_str();
    options.crash_reporting = 0;
    options.logs = logs;
    options.database_path = g_database.c_str();
    return options;
}

NOINLINE static void charge_card() {
    try {
        throw std::invalid_argument("card number has 15 digits");
    } catch (const std::exception &e) {
        zharp::capture_exception(e);
    }
}

static void everything_on_the_wire() {
    Receiver receiver;
    zharp_options_t options = options_for(receiver.port(), 1);
    CHECK(zharp_init(&options) == 0);

    zharp_set_user("u_1", "ada@example.com", "Ada");
    zharp_set_tag("store", "berlin");
    zharp_log(ZHARP_LEVEL_INFO, "opened the cart");

    char traceparent[64] = {0};
    CHECK(zharp_traceparent(traceparent, sizeof traceparent) == 0);
    CHECK_EQ(zharp_log_correlation(), "");
    {
        zharp::Span outer("checkout");
        outer.set("order.id", "o_9");
        outer.header("request", "Authorization", "Bearer s3cret");
        outer.header("request", "Accept", "application/json");
        outer.header("response", "Set-Cookie", "sid=abc");
        std::string correlation = zharp_log_correlation();
        CHECK(correlation.size() == 1 + 9 + 32 + 9 + 16);
        CHECK(contains(correlation, " trace_id=") && contains(correlation, " span_id="));
        CHECK(zharp_traceparent(traceparent, sizeof traceparent) == 1);
        CHECK(strlen(traceparent) == 55 && strncmp(traceparent, "00-", 3) == 0);
        {
            zharp::Span inner("charge");
            charge_card();
        }
        zharp_capture_error("InventoryError", "SKU 1042 is out of stock");
    }
    zharp_capture_message(ZHARP_LEVEL_WARN, "checkout took 12 s");
    {
        // A trace continued from a request's traceparent header.
        zharp::Span server("GET /orders", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
        server.kind(1);
    }
    CHECK(zharp_flush(10000) == 1);

    std::string logs = receiver.bodies("/api/v1/otlp/v1/logs");
    std::string traces = receiver.bodies("/api/v1/otlp/v1/traces");
    // The resource.
    CHECK(contains(logs, "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"itest\"}}"));
    CHECK(contains(logs, "{\"key\":\"service.version\",\"value\":{\"stringValue\":\"1.2.3\"}}"));
    CHECK(contains(logs, "{\"key\":\"deployment.environment.name\",\"value\":{\"stringValue\":\"test\"}}"));
    CHECK(contains(logs, "{\"key\":\"telemetry.sdk.language\",\"value\":{\"stringValue\":\"cpp\"}}"));
    CHECK(contains(logs, "\"device.id\""));
    // The handled C++ exception: its type, message, a native stack and the modules to name it with.
    CHECK(contains(logs, "{\"key\":\"exception.type\",\"value\":{\"stringValue\":\"std::invalid_argument\"}}"));
    CHECK(contains(logs, "{\"key\":\"exception.message\",\"value\":{\"stringValue\":\"card number has 15 digits\"}}"));
    CHECK(contains(logs, "\"stringValue\":\"std::invalid_argument: card number has 15 digits\\n#0  "));
    CHECK(contains(logs, "{\"key\":\"zharp.handled\",\"value\":{\"stringValue\":\"true\"}}"));
    CHECK(contains(logs, "\"zharp.native_modules\""));
    CHECK(contains(logs, "\"zharp.native_debug_ids\""));
    CHECK(contains(logs, "{\"key\":\"exception.type\",\"value\":{\"stringValue\":\"InventoryError\"}}"));
    CHECK(contains(logs, "{\"key\":\"exception.type\",\"value\":{\"stringValue\":\"Message\"}}"));
    CHECK(contains(logs, "\"severityNumber\":13"));
    // Context on every record: session, user, tags, and the breadcrumbs before an error.
    CHECK(contains(logs, "\"session.id\""));
    CHECK(contains(logs, "{\"key\":\"enduser.id\",\"value\":{\"stringValue\":\"u_1\"}}"));
    CHECK(contains(logs, "{\"key\":\"enduser.email\",\"value\":{\"stringValue\":\"ada@example.com\"}}"));
    CHECK(contains(logs, "{\"key\":\"store\",\"value\":{\"stringValue\":\"berlin\"}}"));
    CHECK(contains(logs, "\"zharp.breadcrumbs\""));
    CHECK(contains(logs, "info opened the cart"));
    // Logs are on here: the line is a log record too, inside no span.
    CHECK(contains(logs, "\"body\":{\"stringValue\":\"opened the cart\"}"));
    // Errors inside a span carry its trace.
    CHECK(contains(logs, "\"traceId\":\""));

    // Spans: names, the child under its parent, headers with secrets redacted, the continued trace.
    CHECK(contains(traces, "\"name\":\"checkout\""));
    CHECK(contains(traces, "\"name\":\"charge\""));
    CHECK(contains(traces, "\"parentSpanId\":\""));
    CHECK(contains(traces, "{\"key\":\"order.id\",\"value\":{\"stringValue\":\"o_9\"}}"));
    CHECK(contains(traces, "{\"key\":\"http.request.header.authorization\",\"value\":{\"stringValue\":\"[REDACTED]\"}}"));
    CHECK(contains(traces, "{\"key\":\"http.request.header.accept\",\"value\":{\"stringValue\":\"application/json\"}}"));
    CHECK(contains(traces, "{\"key\":\"http.response.header.set-cookie\",\"value\":{\"stringValue\":\"[REDACTED]\"}}"));
    CHECK(!contains(traces, "s3cret"));
    CHECK(contains(traces, "\"traceId\":\"0af7651916cd43dd8448eb211c80319c\",\"spanId\":\""));
    CHECK(contains(traces, "\"parentSpanId\":\"b7ad6b7169203331\""));
    CHECK(contains(traces, "\"kind\":2"));

    // The key goes as a bearer token.
    bool authorized = false;
    for (const Request &r : receiver.requests()) authorized = authorized || contains(r.headers, "Authorization: Bearer zpk_test");
    CHECK(authorized);
    zharp_shutdown();
}

static void logs_off_by_default() {
    Receiver receiver;
    zharp_options_t options = options_for(receiver.port(), -1);
    CHECK(zharp_init(&options) == 0);
    zharp_log(ZHARP_LEVEL_WARN, "a line only the agent should bring in");
    zharp_capture_error("Boom", "an error still goes");
    CHECK(zharp_flush(10000) == 1);
    std::string logs = receiver.bodies("/v1/logs");
    CHECK(!contains(logs, "\"body\":{\"stringValue\":\"a line only the agent should bring in\"}"));
    // …but it is a breadcrumb of the error.
    CHECK(contains(logs, "warn a line only the agent should bring in"));
    CHECK(contains(logs, "\"stringValue\":\"Boom\""));
    zharp_shutdown();
}

static void offline_then_next_launch() {
    int port;
    {
        Receiver probe;
        port = probe.port();
    }
    // Nobody listening: the batch is kept in the outbox.
    zharp_options_t options = options_for(port, 0);
    CHECK(zharp_init(&options) == 0);
    zharp_capture_error("OfflineError", "sent by the next launch");
    CHECK(zharp_flush(10000) == 0);
    zharp_shutdown();

    // The next launch sends it.
    Receiver receiver(port);
    options = options_for(port, 0);
    CHECK(zharp_init(&options) == 0);
    bool arrived = receiver.wait([&] { return contains(receiver.bodies("/v1/logs"), "OfflineError"); }, 15000);
    CHECK(arrived);
    zharp_shutdown();
}

static void rejected_key_is_not_retried() {
    Receiver receiver(0, 401);
    zharp_options_t options = options_for(receiver.port(), 0);
    CHECK(zharp_init(&options) == 0);
    zharp_capture_error("Unauthorized", "nobody keeps this");
    CHECK(zharp_flush(10000) == 0);
    zharp_shutdown();
    CHECK(receiver.requests().size() == 1);
}

static void off_switches() {
    set_env("ZHARP_DISABLED", "true");
    zharp_options_t options = options_for(1, 0);
    CHECK(zharp_init(&options) == 1);
    set_env("ZHARP_DISABLED", nullptr);
    // Calls do nothing when Zharp is off.
    zharp_capture_error("Ignored", "off");
    zharp_span_t *span = zharp_start_span("noop");
    zharp_span_set_attribute(span, "k", "v");
    zharp_end_span(span);
    CHECK(zharp_flush(100) == 1);

    set_env("ZHARP_KEY", nullptr);
    zharp_options_init(&options);
    options.database_path = g_database.c_str();
    CHECK(zharp_init(&options) == 1);
}

int main() {
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = getenv("TEMP");
    g_database = std::string(tmp ? tmp : "/tmp") + "/zharp-itest-" + std::to_string(getpid());

    everything_on_the_wire();
    logs_off_by_default();
    offline_then_next_launch();
    rejected_key_is_not_retried();
    off_switches();
    if (g_failures) fprintf(stderr, "%d check(s) failed\n", g_failures);
    else printf("integration tests passed\n");
    return g_failures ? 1 : 0;
}
