// The Zharp shop as a Qt Widgets app: the shop window opens the checkout dialog, which logs a
// warning, looks the prices up over HTTP (a span, with traceparent), then
//
//   qt_shop crash   reads the basket's prices through a null pointer (Basket::total)
//   qt_shop fatal   qFatal in CheckoutDialog::loadPaymentMethods
//   qt_shop ok      quits (with ZHARP_LOGS=true the warning is sent as a log)
//
// ZHARP_SHOP_API is the prices service's base URL (default http://127.0.0.1:8099/shop).
#include <zharp.hpp>
#include <zharp_qt.h>

#include <QtCore/QTimer>
#include <QtWidgets/QApplication>

#include "shop_window.h"

int main(int argc, char **argv) {
    zharp::Init zharp;
    QApplication app(argc, argv);
    ZharpQt::init(app);
    zharp::set_user("u_42", "ada@example.com", "Ada");

    const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("ok");
    ShopWindow window(mode);
    window.show();
    window.activateWindow();
    QTimer::singleShot(300, &window, &ShopWindow::openCheckout);
    return app.exec();
}
