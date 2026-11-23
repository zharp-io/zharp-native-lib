#pragma once

#include <QtCore/QObject>

/** The card gateway, never connected. */
struct Gateway {
    int sent = 0;
    void send(int cents);
};

/** What the QML pages call. */
class Payments : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    /** Charges through a gateway that was never set up: a null write in Gateway::send. */
    Q_INVOKABLE void charge(int cents);
    /** The payment database is missing: qFatal. */
    Q_INVOKABLE void openDatabase();

private:
    Gateway *gateway_ = nullptr;
};
