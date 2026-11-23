// Zharp for Qt (zharp_qt.h): the message handler, screens and the network access manager. Needs Qt
// Core, Gui and Network; Qt Quick is read through the meta-object system, so a widgets app doesn't
// link it. Internals live in zharp::qt, which Zharp leaves out of an error's stack.
#include "zharp_qt.h"

#include <QtCore/QByteArray>
#include <QtCore/QFileInfo>
#include <QtCore/QList>
#include <QtCore/QMetaMethod>
#include <QtCore/QMutex>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <atomic>
#include <cstdio>
#include <memory>

namespace zharp {
namespace qt {

// ── Log messages ────────────────────────────────────────────────────────────

static QtMessageHandler g_previous = nullptr;
static thread_local bool t_inside = false;

static QByteArray message_text(const QMessageLogContext &context, const QString &message) {
    QByteArray text = message.toUtf8();
    const char *category = context.category;
    if (category && *category && qstrcmp(category, "default") != 0) text = QByteArray(category) + ": " + text;
    return text;
}

/** qWarning and qCritical: breadcrumbs (and logs with ZHARP_LOGS=true). qFatal: the crash's name. */
static void message_handler(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (!t_inside) {
        t_inside = true;
        switch (type) {
            case QtWarningMsg:
                zharp_log(ZHARP_LEVEL_WARN, message_text(context, message).constData());
                break;
            case QtCriticalMsg:
                zharp_log(ZHARP_LEVEL_ERROR, message_text(context, message).constData());
                break;
            case QtFatalMsg: {
                const QByteArray text = message.toUtf8();
                zharp_log(ZHARP_LEVEL_FATAL, message_text(context, message).constData());
                // Qt aborts once the handlers return: Crashpad's report reads "qFatal: <message>".
                // Without crash reporting, the error goes now, before the process ends.
                if (!zharp_set_crash_reason("qFatal", text.constData())) {
                    zharp_capture_error("qFatal", text.constData());
                    zharp_flush(3000);
                }
                break;
            }
            default:
                break;
        }
        t_inside = false;
    }
    if (g_previous) {
        g_previous(type, context, message);
    } else {
        fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, message)));
    }
}

// ── Screens ─────────────────────────────────────────────────────────────────

class Screens {
public:
    ZharpQt::detail::WindowNamer widgets = nullptr;
    bool automatic = true;
    QPointer<QTimer> timer;
    /** StackViews already connected to the timer. */
    QList<QPointer<QObject>> watched;

    void set(const QString &name) {
        if (name.isEmpty()) return;
        QString previous;
        {
            QMutexLocker lock(&mutex_);
            if (name == current_) return;
            previous = current_;
            current_ = name;
        }
        const QByteArray utf8 = name.toUtf8();
        zharp_set_tag("screen.name", utf8.constData());
        const QByteArray crumb = previous.isEmpty() ? utf8 : (previous + QStringLiteral(" \u2192 ") + name).toUtf8();
        zharp_add_breadcrumb("navigation", crumb.constData());
    }

    QString current() {
        QMutexLocker lock(&mutex_);
        return current_;
    }

    /** Looks again soon (once, however many signals arrive meanwhile). */
    void schedule() {
        if (timer) timer->start();
    }

    void refresh();

private:
    QMutex mutex_;
    QString current_;
};

static Screens *g_screens = nullptr;

/** "SettingsPage_QMLTYPE_12" → "SettingsPage"; Qt's own and anonymous types → "". */
static QString qml_type_name(const QObject *object) {
    static const QRegularExpression composite(QStringLiteral("^(\\w+?)_QMLTYPE_\\d+$"));
    const QRegularExpressionMatch m = composite.match(QString::fromLatin1(object->metaObject()->className()));
    return m.hasMatch() ? m.captured(1) : QString();
}

/** A QML page or window: its objectName, its QML type, else its title. */
static QString qml_name(const QObject *object) {
    if (!object->objectName().isEmpty()) return object->objectName();
    QString type = qml_type_name(object);
    if (!type.isEmpty()) return type;
    return object->property("title").toString();
}

static bool visible(const QObject *item) {
    const QVariant v = item->property("visible");
    return !v.isValid() || v.toBool();
}

/** The StackViews in and under `root` (QML builds QObject parents along its declarations). */
static QList<QObject *> stack_views(QObject *root) {
    QList<QObject *> found;
    if (!root) return found;
    const QList<QObject *> all = root->findChildren<QObject *>();
    for (QObject *object : all) {
        if (object->inherits("QQuickStackView")) found.append(object);
    }
    return found;
}

/** The page on top: the current item of the innermost visible StackView, connected as it's met. */
static QString top_page(Screens &screens, QObject *root, int depth = 0) {
    if (!root || depth > 8) return QString();
    QList<QObject *> views = stack_views(root);
    QObject *content = qvariant_cast<QObject *>(root->property("contentItem"));
    if (content && content != root) views.append(stack_views(content));
    QString page;
    for (QObject *view : views) {
        bool known = false;
        for (const QPointer<QObject> &w : screens.watched) known = known || w == view;
        if (!known && screens.timer) {
            const int signal = view->metaObject()->indexOfSignal("currentItemChanged()");
            const int slot = screens.timer->metaObject()->indexOfSlot("start()");
            if (signal >= 0 && slot >= 0) {
                QObject::connect(view, view->metaObject()->method(signal), screens.timer, screens.timer->metaObject()->method(slot));
                screens.watched.append(view);
            }
        }
        if (!visible(view)) continue;
        QObject *item = qvariant_cast<QObject *>(view->property("currentItem"));
        if (!item) continue;
        const QString nested = top_page(screens, item, depth + 1);
        const QString name = nested.isEmpty() ? qml_name(item) : nested;
        if (!name.isEmpty()) page = name;  // the last one found is the innermost
    }
    return page;
}

static QString window_name(Screens &screens, QWindow *window) {
    if (window->inherits("QQuickWindow")) {
        const QString page = top_page(screens, window);
        if (!page.isEmpty()) return page;
        // QQuickView: its QML file ("Main.qml" → "Main").
        const QUrl source = window->property("source").toUrl();
        if (source.isValid() && !source.isEmpty()) return QFileInfo(source.path()).completeBaseName();
        const QString name = qml_name(window);
        if (!name.isEmpty()) return name;
    }
    if (screens.widgets) {
        const QString name = screens.widgets(window);
        if (!name.isEmpty()) return name;
    }
    if (!window->objectName().isEmpty()) return window->objectName();
    if (!window->title().isEmpty()) return window->title();
    return QString::fromLatin1(window->metaObject()->className());
}

void Screens::refresh() {
    if (!automatic) return;
    QWindow *window = QGuiApplication::focusWindow();
    if (!window) return;  // the app went to the background: the screen stays what it was
    const Qt::WindowType type = window->type();
    if (type == Qt::Popup || type == Qt::ToolTip || type == Qt::SplashScreen) return;  // menus, tooltips
    set(window_name(*this, window));
}

}  // namespace qt
}  // namespace zharp

// ── Network ─────────────────────────────────────────────────────────────────

namespace zharp {
namespace qt {

static QByteArray method_of(QNetworkAccessManager::Operation op, const QNetworkRequest &request) {
    switch (op) {
        case QNetworkAccessManager::HeadOperation: return "HEAD";
        case QNetworkAccessManager::GetOperation: return "GET";
        case QNetworkAccessManager::PutOperation: return "PUT";
        case QNetworkAccessManager::PostOperation: return "POST";
        case QNetworkAccessManager::DeleteOperation: return "DELETE";
        default: {
            const QByteArray verb = request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
            return verb.isEmpty() ? QByteArray("GET") : verb.toUpper();
        }
    }
}

/** The call's outcome on its span, then the span ends. */
static void finish(QNetworkReply *reply, zharp_span_t *span) {
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int code = status.isValid() ? status.toInt() : 0;
    if (code > 0) zharp_span_set_attribute(span, "http.response.status_code", QByteArray::number(code).constData());
    const QList<QNetworkReply::RawHeaderPair> headers = reply->rawHeaderPairs();
    for (const QNetworkReply::RawHeaderPair &header : headers) {
        zharp_span_set_header(span, "response", header.first.constData(), header.second.constData());
    }
    // Redirects followed: the URL that answered.
    const QUrl url = reply->url().adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
    zharp_span_set_attribute(span, "url.full", url.toString().toUtf8().constData());
    if (code >= 400) {
        zharp_span_set_error(span, ("HTTP " + QByteArray::number(code)).constData());
    } else if (reply->error() != QNetworkReply::NoError) {
        zharp_span_set_error(span, reply->errorString().toUtf8().constData());
    }
    zharp_end_span(span);
}

}  // namespace qt
}  // namespace zharp

namespace ZharpQt {

NetworkAccessManager::NetworkAccessManager(QObject *parent) : QNetworkAccessManager(parent) {}

QNetworkReply *NetworkAccessManager::createRequest(Operation op, const QNetworkRequest &original, QIODevice *outgoingData) {
    const QString scheme = original.url().scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        return QNetworkAccessManager::createRequest(op, original, outgoingData);  // qrc:, file:, data:
    }
    QNetworkRequest request(original);
    const QByteArray method = zharp::qt::method_of(op, request);
    const QUrl url = request.url().adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
    const QByteArray host = url.host().toUtf8();

    zharp_span_t *span = zharp_start_async_span((method + ' ' + host).constData());
    zharp_span_set_kind(span, 2);
    zharp_span_set_attribute(span, "http.request.method", method.constData());
    zharp_span_set_attribute(span, "url.full", url.toString().toUtf8().constData());
    zharp_span_set_attribute(span, "server.address", host.constData());
    if (url.port() > 0) zharp_span_set_attribute(span, "server.port", QByteArray::number(url.port()).constData());
    const QList<QByteArray> names = request.rawHeaderList();
    for (const QByteArray &name : names) zharp_span_set_header(span, "request", name.constData(), request.rawHeader(name).constData());
    char traceparent[64];
    if (!request.hasRawHeader("traceparent") && zharp_span_traceparent(span, traceparent, sizeof traceparent)) {
        request.setRawHeader("traceparent", traceparent);
    }

    QNetworkReply *reply = QNetworkAccessManager::createRequest(op, request, outgoingData);
    // finished comes once, aborted calls included; a reply deleted before it ends the span too.
    auto ended = std::make_shared<std::atomic<bool>>(false);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, span, ended] {
        if (ended->exchange(true)) return;
        zharp::qt::finish(reply, span);
    });
    QObject::connect(reply, &QObject::destroyed, [span, ended] {
        if (ended->exchange(true)) return;
        zharp_span_set_error(span, "the reply was deleted before it finished");
        zharp_end_span(span);
    });
    return reply;
}

void setScreen(const QString &name) {
    static zharp::qt::Screens manual;  // before init, or with screens off
    (zharp::qt::g_screens ? *zharp::qt::g_screens : manual).set(name);
}

QString screen() {
    return zharp::qt::g_screens ? zharp::qt::g_screens->current() : QString();
}

namespace detail {

void start(QCoreApplication *app, const Options &options, WindowNamer widgets) {
    if (!app || zharp::qt::g_screens) return;
    auto *screens = new zharp::qt::Screens();
    screens->widgets = widgets;
    screens->automatic = options.screens;
    zharp::qt::g_screens = screens;

    if (options.messages) {
        QtMessageHandler previous = qInstallMessageHandler(zharp::qt::message_handler);
        if (previous != zharp::qt::message_handler) zharp::qt::g_previous = previous;
    }

    auto *gui = qobject_cast<QGuiApplication *>(app);
    if (!gui || !options.screens) return;  // a QCoreApplication has no windows
    auto *timer = new QTimer(app);
    timer->setSingleShot(true);
    timer->setInterval(0);
    QObject::connect(timer, &QTimer::timeout, app, [screens] { screens->refresh(); });
    screens->timer = timer;
    QObject::connect(gui, &QGuiApplication::focusWindowChanged, timer, [timer](QWindow *) { timer->start(); });
    screens->schedule();
}

}  // namespace detail
}  // namespace ZharpQt
