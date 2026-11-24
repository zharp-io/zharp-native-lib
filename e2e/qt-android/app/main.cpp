// The e2e app: what it does comes from its first argument (adb shell am start …
// -e applicationArguments <action>):
//   handled — a std::runtime_error caught and reported, then quit
//   segv    — a null pointer write in C++
//   cxx     — an uncaught std::runtime_error
//   fatal   — qFatal("…"), named by ZharpQt's message handler
//   (none)  — start (sends the last run's crash), then quit after a while
#include <zharp.hpp>
#include <zharp_qt.h>

#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>

#include <thread>

#include "cart.h"

class CheckoutWindow : public QMainWindow {
public:
    CheckoutWindow() {
        setWindowTitle("Checkout");
        setCentralWidget(new QLabel("Cart"));
    }
};

int main(int argc, char **argv) {
    zharp_options_t options;
    zharp_options_init(&options);
    options.key = "zpk_qt_android_e2e";
    options.service = "qt-shop-android";
    options.environment = "ci";
    options.endpoint = "http://10.0.2.2:4318/api/v1/otlp";
    zharp_init(&options);

    QApplication app(argc, argv);
    ZharpQt::init(app);
    zharp::set_user("qt-user-7", "qt@example.com");
    CheckoutWindow window;
    window.show();

    const QString action = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    qInfo("qt e2e: %s", qPrintable(action.isEmpty() ? QStringLiteral("launch") : action));
    QTimer::singleShot(2000, [action]() {
        if (action == "handled") {
            try {
                shop::cart_total({"12", "twelve"});
            } catch (const std::exception &e) {
                zharp::capture_exception(e);
            }
            QTimer::singleShot(3000, [] { zharp_flush(3000); QCoreApplication::quit(); });
        } else if (action == "segv") {
            shop::apply_discount_to_missing_order();
        } else if (action == "cxx") {
            // On a thread of its own: nothing catches it, and std::terminate runs there.
            std::thread([] { shop::cart_total({"7", "seven"}); }).join();
        } else if (action == "fatal") {
            qFatal("Payments database missing: %s", "/data/payments.db");
        } else {
            QTimer::singleShot(8000, [] { QCoreApplication::quit(); });
        }
    });
    return app.exec();
}
