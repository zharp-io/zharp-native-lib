// The Qt layer against a stand-in OTLP receiver, on Qt's offscreen platform: Qt's messages as
// breadcrumbs and logs (the earlier handler still runs), qFatal sent as an error when crash
// reporting is off, screen.name from the focused window and setScreen, and
// ZharpQt::NetworkAccessManager's spans with headers, traceparent and failures.
#include <zharp.hpp>
#include <zharp_qt.h>

#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QLoggingCategory>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <cstdlib>
#include <string>

#include "../check.h"
#include "../receiver.h"

#if defined(_WIN32)
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

Q_LOGGING_CATEGORY(shopCategory, "shop.cart")

static QStringList g_earlier;  // what the handler installed before Zharp's saw
static void earlier_handler(QtMsgType, const QMessageLogContext &, const QString &message) { g_earlier.append(message); }

static std::string g_endpoint;
static std::string g_database;

static zharp_options_t options_for(int port) {
    g_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/api/v1/otlp";
    zharp_options_t options;
    zharp_options_init(&options);
    options.key = "zpk_test";
    options.service = "qt-test";
    options.endpoint = g_endpoint.c_str();
    options.crash_reporting = 0;
    options.logs = 1;
    options.database_path = g_database.c_str();
    return options;
}

template <class P>
static bool wait_for(P done, int ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return done();
}

static QNetworkReply *get(QNetworkAccessManager &manager, const QNetworkRequest &request) {
    QNetworkReply *reply = manager.get(request);
    wait_for([reply] { return reply->isFinished(); }, 10000);
    return reply;
}

/** `qt_tests fatal`: qFatal with crash reporting off, so it is sent as an error before Qt aborts. */
static int fatal(int argc, char **argv) {
    zharp_options_t options = options_for(atoi(argv[2]));
    zharp::Init zharp(&options);
    QCoreApplication app(argc, argv);
    ZharpQt::init(app);
    qWarning("about to give up");
    qFatal("database schema is version %d, expected %d", 7, 9);
    return 0;
}

int main(int argc, char **argv) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = getenv("TEMP");
    g_database = std::string(tmp ? tmp : "/tmp") + "/zharp-qt-test-" + std::to_string(getpid());
    if (argc > 2 && std::string(argv[1]) == "fatal") return fatal(argc, argv);

    Receiver receiver;
    zharp_options_t options = options_for(receiver.port());
    CHECK(zharp_init(&options) == 0);
    qInstallMessageHandler(earlier_handler);
    QGuiApplication app(argc, argv);
    ZharpQt::init(app);

    // ── Messages ───────────────────────────────────────────────────────────
    qDebug("a debug line stays out");
    qWarning("disk almost full: %d%%", 93);
    qCCritical(shopCategory, "cart total is negative");
    CHECK(g_earlier.contains(QStringLiteral("disk almost full: 93%")));  // chained
    CHECK(g_earlier.contains(QStringLiteral("cart total is negative")));

    // ── Screens ────────────────────────────────────────────────────────────
    QWindow checkout;
    checkout.setObjectName(QStringLiteral("CheckoutWindow"));
    checkout.resize(200, 200);
    checkout.show();
    checkout.requestActivate();
    CHECK(wait_for([] { return ZharpQt::screen() == QStringLiteral("CheckoutWindow"); }));
    QWindow settings;
    settings.setTitle(QStringLiteral("Settings"));
    settings.resize(200, 200);
    settings.show();
    settings.requestActivate();
    CHECK(wait_for([] { return ZharpQt::screen() == QStringLiteral("Settings"); }));
    // A popup (a menu) leaves the screen as it was.
    QWindow menu;
    menu.setFlags(Qt::Popup);
    menu.setObjectName(QStringLiteral("Menu"));
    menu.resize(50, 50);
    menu.show();
    menu.requestActivate();
    wait_for([] { return false; }, 300);
    CHECK(ZharpQt::screen() == QStringLiteral("Settings"));
    menu.hide();
    zharp_capture_error("CheckoutError", "card declined on Settings");
    ZharpQt::setScreen(QStringLiteral("Receipt"));
    CHECK(ZharpQt::screen() == QStringLiteral("Receipt"));

    // ── Network ────────────────────────────────────────────────────────────
    Receiver api;
    Receiver missing(0, 404);
    int closed;
    {
        Receiver probe;
        closed = probe.port();
    }
    ZharpQt::NetworkAccessManager manager;
    {
        zharp::Span checkout_span("checkout");
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/orders?token=s3cret").arg(api.port())));
        request.setRawHeader("Authorization", "Bearer s3cret");
        request.setRawHeader("Accept", "application/json");
        QNetworkReply *ok = get(manager, request);
        CHECK(ok->error() == QNetworkReply::NoError);
        ok->deleteLater();
        // What the server got: traceparent, in checkout's trace.
        char current[64] = {0};
        zharp_traceparent(current, sizeof current);
        std::string sent = api.requests().empty() ? std::string() : api.requests()[0].headers;
        CHECK(contains(sent, std::string("traceparent: ") + std::string(current, 36)));
        CHECK(!contains(sent, std::string("traceparent: ") + current));  // the HTTP span's own ID

        QNetworkReply *notFound = get(manager, QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/missing").arg(missing.port()))));
        CHECK(notFound->error() != QNetworkReply::NoError);
        notFound->deleteLater();
        QNetworkReply *refused = get(manager, QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/down").arg(closed))));
        CHECK(refused->error() != QNetworkReply::NoError);
        delete refused;
    }
    QCoreApplication::processEvents();
    CHECK(zharp_flush(10000) == 1);

    std::string logs = receiver.bodies("/v1/logs");
    std::string traces = receiver.bodies("/v1/traces");
    // Warnings and criticals are logs (ZHARP_LOGS on here) and breadcrumbs; debug lines neither.
    CHECK(contains(logs, "\"severityNumber\":13,\"severityText\":\"WARN\",\"body\":{\"stringValue\":\"disk almost full: 93%\"}"));
    CHECK(contains(logs, "\"severityNumber\":17,\"severityText\":\"ERROR\",\"body\":{\"stringValue\":\"shop.cart: cart total is negative\"}"));
    CHECK(!contains(logs, "a debug line stays out"));
    CHECK(contains(logs, "warn disk almost full: 93%"));
    // The screen on what was recorded there, and the way the user went.
    CHECK(contains(logs, "{\"key\":\"screen.name\",\"value\":{\"stringValue\":\"Settings\"}}"));
    CHECK(contains(logs, "navigation CheckoutWindow \xE2\x86\x92 Settings"));
    // HTTP spans.
    CHECK(contains(traces, "\"name\":\"GET 127.0.0.1\""));
    CHECK(contains(traces, "\"kind\":3"));
    CHECK(contains(traces, "{\"key\":\"http.request.method\",\"value\":{\"stringValue\":\"GET\"}}"));
    CHECK(contains(traces, "{\"key\":\"url.full\",\"value\":{\"stringValue\":\"http://127.0.0.1:" + std::to_string(api.port()) + "/orders\"}}"));
    CHECK(!contains(traces, "s3cret"));
    CHECK(contains(traces, "{\"key\":\"http.request.header.authorization\",\"value\":{\"stringValue\":\"[REDACTED]\"}}"));
    CHECK(contains(traces, "{\"key\":\"http.request.header.accept\",\"value\":{\"stringValue\":\"application/json\"}}"));
    CHECK(contains(traces, "{\"key\":\"http.response.status_code\",\"value\":{\"stringValue\":\"200\"}}"));
    CHECK(contains(traces, "{\"key\":\"http.response.header.content-length\",\"value\":{\"stringValue\":\"0\"}}"));
    CHECK(contains(traces, "\"status\":{\"code\":2,\"message\":\"HTTP 404\"}"));
    CHECK(contains(traces, "/down\"}}"));
    CHECK(contains(traces, "{\"key\":\"screen.name\",\"value\":{\"stringValue\":\"Receipt\"}}"));
    CHECK(contains(traces, "\"name\":\"checkout\""));

    // ── qFatal, crash reporting off: sent as an error before Qt aborts ──────
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("fatal"), QString::number(receiver.port())});
    CHECK(child.waitForFinished(20000));
    CHECK(child.exitStatus() == QProcess::CrashExit || child.exitCode() != 0);
    logs = receiver.bodies("/v1/logs");
    CHECK(contains(logs, "{\"key\":\"exception.type\",\"value\":{\"stringValue\":\"qFatal\"}}"));
    CHECK(contains(logs, "{\"key\":\"exception.message\",\"value\":{\"stringValue\":\"database schema is version 7, expected 9\"}}"));
    CHECK(contains(logs, "warn about to give up"));

    zharp_shutdown();
    if (g_failures) fprintf(stderr, "%d check(s) failed\n", g_failures);
    else printf("Qt tests passed\n");
    return g_failures ? 1 : 0;
}
