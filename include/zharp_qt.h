// Zharp for Qt: Qt's log messages, the screen the user is on, and QNetworkAccessManager calls, on
// top of Zharp for C and C++ (zharp.h). Qt 6.5+ and Qt 5.15.
//
//   #include <zharp.hpp>
//   #include <zharp_qt.h>
//
//   int main(int argc, char **argv) {
//       zharp::Init zharp;                  // zharp_init(NULL) … zharp_shutdown(); or with options
//       QApplication app(argc, argv);
//       ZharpQt::init(app);
//       ...
//       return app.exec();
//   }
//
// - Log messages: qWarning and qCritical are breadcrumbs (the last 100 go with a crash or an
//   error), and logs when ZHARP_LOGS=true. qFatal names the crash it ends in ("qFatal: …"), or is
//   sent as an error when crash reporting is off. The handler that was installed before still runs.
// - Screens: the window that has focus (a QWidget window by its class, a QML window by its file)
//   and the current page of a QML StackView, as screen.name on everything recorded and on crashes,
//   with a navigation breadcrumb at each change. ZharpQt::setScreen names your own.
// - Network: ZharpQt::NetworkAccessManager is a QNetworkAccessManager that records each HTTP call
//   as a client span, with its request and response headers (secrets kept as [REDACTED]), and
//   sends traceparent so the server's trace continues this one. ZharpQt::instrument(engine) gives
//   a QML engine one.
//
// CMake: target_link_libraries(my_app PRIVATE zharp::qt). qmake: include(<zharp>/share/zharp/zharp_qt.pri).
//
// MIT License. https://zharp.io/docs/sdks/qt
#ifndef ZHARP_QT_H
#define ZHARP_QT_H

#include "zharp.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#if defined(QT_WIDGETS_LIB)
#  include <QtWidgets/QApplication>
#  include <QtWidgets/QWidget>
#endif
#if defined(QT_QML_LIB)
#  include <QtQml/QQmlEngine>
#  include <QtQml/QQmlNetworkAccessManagerFactory>
#endif

#if defined(_WIN32) && defined(ZHARP_SHARED)
#  if defined(ZHARP_QT_BUILDING)
#    define ZHARP_QT_API __declspec(dllexport)
#  else
#    define ZHARP_QT_API __declspec(dllimport)
#  endif
#elif defined(ZHARP_SHARED) && defined(__GNUC__)
#  define ZHARP_QT_API __attribute__((visibility("default")))
#else
#  define ZHARP_QT_API
#endif

class QWindow;

namespace ZharpQt {

/** What init turns on; all of it by default. */
struct Options {
    /** qWarning, qCritical and qFatal (see above). */
    bool messages = true;
    /** screen.name from the focused window and QML StackView pages. Off: only setScreen names it. */
    bool screens = true;
};

/**
 * Names the screen the user is on (screen.name), for navigation Zharp can't see: a
 * QStackedWidget's pages, your own router. It holds until the next window or page change.
 */
ZHARP_QT_API void setScreen(const QString &name);

/** The screen the user is on, as last recorded; empty before the first. */
ZHARP_QT_API QString screen();

/**
 * A QNetworkAccessManager that records each HTTP call as a span: "GET api.example.com", the URL
 * without its query, the status, request and response headers (secrets [REDACTED]), and a
 * traceparent header on the request. Use it wherever you'd use QNetworkAccessManager.
 */
class ZHARP_QT_API NetworkAccessManager : public QNetworkAccessManager {
public:
    explicit NetworkAccessManager(QObject *parent = nullptr);

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) override;
};

namespace detail {
/** A window's name from what this app links (Qt Widgets), or empty. */
using WindowNamer = QString (*)(QWindow *window);
ZHARP_QT_API void start(QCoreApplication *app, const Options &options, WindowNamer widgets);

#if defined(QT_WIDGETS_LIB)
/** The QWidget behind a window: its class ("SettingsDialog"), else its objectName or title. */
static inline QString widgetWindowName(QWindow *window) {
    const QWidgetList widgets = QApplication::topLevelWidgets();
    for (QWidget *widget : widgets) {
        if (widget->windowHandle() != window) continue;
        const QString type = QString::fromLatin1(widget->metaObject()->className());
        // Qt's own classes (QDialog, QMainWindow, QMessageBox…) say less than the names you gave.
        const bool qtClass = type.size() > 1 && type[0] == QLatin1Char('Q') && type[1].isUpper() && !type.contains(QLatin1String("::"));
        if (!qtClass) return type.section(QLatin1String("::"), -1);
        if (!widget->objectName().isEmpty()) return widget->objectName();
        if (!widget->windowTitle().isEmpty()) return widget->windowTitle();
        return type;
    }
    return QString();
}
#endif
}  // namespace detail

/**
 * Turns the Qt layer on, after QApplication (or QGuiApplication, QCoreApplication) exists and
 * after zharp_init. Call it once.
 */
static inline void init(QCoreApplication *app, const Options &options = Options()) {
#if defined(QT_WIDGETS_LIB)
    detail::start(app, options, &detail::widgetWindowName);
#else
    detail::start(app, options, nullptr);
#endif
}
static inline void init(QCoreApplication &app, const Options &options = Options()) { init(&app, options); }

#if defined(QT_QML_LIB)
/** Gives a QML engine ZharpQt::NetworkAccessManager: XMLHttpRequest and remote images become spans. */
class NetworkAccessManagerFactory : public QQmlNetworkAccessManagerFactory {
public:
    QNetworkAccessManager *create(QObject *parent) override { return new NetworkAccessManager(parent); }
};

/** Records the HTTP calls a QML engine makes (call it before loading QML). */
static inline void instrument(QQmlEngine *engine) {
    static NetworkAccessManagerFactory factory;  // the engine doesn't own it
    engine->setNetworkAccessManagerFactory(&factory);
}
static inline void instrument(QQmlEngine &engine) { instrument(&engine); }
#endif

}  // namespace ZharpQt

#endif  // ZHARP_QT_H
