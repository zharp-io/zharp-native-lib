#include "shop_window.h"

#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#if defined(_MSC_VER)
#  define NOINLINE __declspec(noinline)
#else
#  define NOINLINE __attribute__((noinline))
#endif

NOINLINE int Basket::total() const {
    int sum = 0;
    for (int i = 0; i < count; i++) sum += prices[i];  // prices is null: a crash
    return sum;
}

CheckoutDialog::CheckoutDialog(const QString &mode, QWidget *parent) : QDialog(parent), mode_(mode) {
    setWindowTitle(QStringLiteral("Checkout"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("3 items"), this));
    basket_.count = 3;
}

void CheckoutDialog::start() {
    qWarning("payment provider is slow (%d ms)", 2300);
    const QByteArray api = qgetenv("ZHARP_SHOP_API").isEmpty() ? QByteArray("http://127.0.0.1:8099/shop") : qgetenv("ZHARP_SHOP_API");
    QNetworkRequest request(QUrl(QString::fromUtf8(api + "/prices?session=abc123")));
    request.setRawHeader("Authorization", "Bearer shop-secret");
    request.setRawHeader("Accept", "application/json");
    QNetworkReply *reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (mode_ == QLatin1String("crash")) confirm();
        else if (mode_ == QLatin1String("fatal")) loadPaymentMethods();
        else QTimer::singleShot(100, qApp, &QApplication::quit);
    });
}

NOINLINE void CheckoutDialog::confirm() {
    const int total = basket_.total();
    qInfo("charging %d", total);
}

NOINLINE void CheckoutDialog::loadPaymentMethods() {
    qFatal("payment methods table is missing (schema %d)", 7);
}

ShopWindow::ShopWindow(const QString &mode) : mode_(mode) {
    setWindowTitle(QStringLiteral("Zharp shop"));
    auto *button = new QPushButton(QStringLiteral("Checkout"), this);
    setCentralWidget(button);
    connect(button, &QPushButton::clicked, this, &ShopWindow::openCheckout);
}

void ShopWindow::openCheckout() {
    auto *dialog = new CheckoutDialog(mode_, this);
    dialog->show();
    dialog->activateWindow();
    // Once the dialog has focus (and so is the screen), go on.
    QTimer::singleShot(300, dialog, &CheckoutDialog::start);
}
