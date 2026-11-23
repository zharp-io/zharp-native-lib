#include "cart.h"

#include <zharp.h>

#if defined(_MSC_VER)
#  define NOINLINE __declspec(noinline)
#else
#  define NOINLINE __attribute__((noinline))
#endif

NOINLINE int Shop::checkout(Order &order) {
    zharp_add_breadcrumb("shop", "checkout started");
    return order.cart->total();
}

NOINLINE void Shop::charge(int cents) {
    if (cents > 10000) throw PaymentDeclined("card declined: amount over the limit");
}
