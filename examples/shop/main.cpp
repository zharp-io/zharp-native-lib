// The Zharp shop: crashes and errors the way a real C++ app has them.
//
//   shop segv      a null pointer read in Cart::total()        (crash: SIGSEGV / access violation)
//   shop abort     abort() after a failed check                 (crash: SIGABRT)
//   shop throw     a C++ exception nobody catches               (crash: std::runtime_error)
//   shop handled   a caught exception and a C error, reported   (handled errors)
//   shop message   a warning reported as an issue
//   shop ok        a span with logs, nothing fails
#include <zharp.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "cart.h"

#if defined(_MSC_VER)
#  define NOINLINE __declspec(noinline)
#else
#  define NOINLINE __attribute__((noinline))
#endif

NOINLINE static void validate(int quantity) {
    if (quantity < 0) {
        zharp_log(ZHARP_LEVEL_ERROR, "negative quantity; giving up");
        abort();
    }
}

NOINLINE static void load_inventory() {
    throw std::runtime_error("inventory file is corrupt");
}

NOINLINE static int run(const char *mode) {
    Shop shop;
    Cart cart;
    Order order;
    if (!strcmp(mode, "segv")) {
        cart.count = 3;  // items never loaded
        order.cart = &cart;
        return shop.checkout(order);
    }
    if (!strcmp(mode, "abort")) {
        validate(-1);
    }
    if (!strcmp(mode, "throw")) {
        load_inventory();
    }
    if (!strcmp(mode, "handled")) {
        zharp::Span span("checkout");
        try {
            shop.charge(25000);
        } catch (const std::exception &e) {
            zharp::capture_exception(e);
        }
        zharp_capture_error("InventoryError", "SKU 1042 is out of stock");
        return 0;
    }
    if (!strcmp(mode, "message")) {
        zharp_capture_message(ZHARP_LEVEL_WARN, "checkout took 12 s");
        return 0;
    }
    zharp::Span span("nightly report");
    span.set("report.rows", "120");
    zharp_logf(ZHARP_LEVEL_INFO, "report written: %d rows%s", 120, zharp_log_correlation());
    return 0;
}

int main(int argc, char **argv) {
    zharp::Init zharp;
    const char *mode = argc > 1 ? argv[1] : "ok";
    zharp::set_user("u_42", "ada@example.com", "Ada");
    zharp::set_tag("store", "berlin");
    zharp_log(ZHARP_LEVEL_INFO, "shop started");
    zharp_add_breadcrumb("ui", "opened the cart");
    int result = run(mode);
    printf("%s: done\n", mode);
    return result;
}
