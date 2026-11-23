// The Zharp shop in QML: an ApplicationWindow with a StackView. The cart page looks the prices up
// with XMLHttpRequest (a span, through ZharpQt::instrument), logs a console.warn and pushes the
// payment page, which then
//
//   qt_qml crash   charges through a null gateway (Gateway::send, from Payments::charge)
//   qt_qml fatal   qFatal in Payments::openDatabase
//   qt_qml ok      quits
//
// ZHARP_SHOP_API is the prices service's base URL (default http://127.0.0.1:8099/shop).
#include <zharp.hpp>
#include <zharp_qt.h>

#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>

#include "payments.h"

int main(int argc, char **argv) {
    zharp::Init zharp;
    QGuiApplication app(argc, argv);
    ZharpQt::init(app);

    QQmlApplicationEngine engine;
    ZharpQt::instrument(engine);
    Payments payments;
    const QByteArray api = qgetenv("ZHARP_SHOP_API");
    engine.rootContext()->setContextProperty(QStringLiteral("payments"), &payments);
    engine.rootContext()->setContextProperty(QStringLiteral("mode"), argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("ok"));
    engine.rootContext()->setContextProperty(QStringLiteral("shopApi"), api.isEmpty() ? QStringLiteral("http://127.0.0.1:8099/shop") : QString::fromUtf8(api));
    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;
    return app.exec();
}
