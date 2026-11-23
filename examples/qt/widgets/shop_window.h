#pragma once

#include <QtWidgets/QDialog>
#include <QtWidgets/QMainWindow>

#include <zharp_qt.h>

/** A basket whose prices were never loaded. */
struct Basket {
    int count = 0;
    const int *prices = nullptr;
    int total() const;
};

class CheckoutDialog : public QDialog {
    Q_OBJECT
public:
    CheckoutDialog(const QString &mode, QWidget *parent = nullptr);
    /** Looks up the prices over HTTP, then goes on as `mode` says. */
    void start();

private:
    void confirm();
    void loadPaymentMethods();

    QString mode_;
    Basket basket_;
    ZharpQt::NetworkAccessManager network_;
};

class ShopWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ShopWindow(const QString &mode);
    void openCheckout();

private:
    QString mode_;
};
