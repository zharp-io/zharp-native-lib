#include "payments.h"

#include <QtCore/QtGlobal>

#if defined(_MSC_VER)
#  define NOINLINE __declspec(noinline)
#else
#  define NOINLINE __attribute__((noinline))
#endif

NOINLINE void Gateway::send(int cents) {
    sent += cents;  // `this` is null: a crash
}

NOINLINE void Payments::charge(int cents) {
    gateway_->send(cents);
    qInfo("charged %d", cents);  // after the call, so charge keeps its own frame (no tail call)
}

NOINLINE void Payments::openDatabase() {
    qFatal("payments.db is missing");
}
