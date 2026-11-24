#include "cart.h"

#include <stdexcept>

namespace shop {

namespace {
struct Order {
    int id;
    int total;
};
}  // namespace

__attribute__((noinline)) int parse_price(const std::string &text) {
    for (char c : text) {
        if (c < '0' || c > '9') throw std::runtime_error("Price is not a number: \"" + text + "\"");
    }
    return std::stoi(text);
}

int cart_total(const std::vector<std::string> &prices) {
    int total = 0;
    for (const auto &price : prices) total += parse_price(price);
    return total;
}

__attribute__((noinline)) static void apply_discount(Order *order, int percent) {
    order->total -= order->total * percent / 100;
}

void apply_discount_to_missing_order() {
    Order *volatile loaded = nullptr;
    apply_discount(loaded, 10);
}

}  // namespace shop
