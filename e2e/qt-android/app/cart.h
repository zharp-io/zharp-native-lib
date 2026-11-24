#pragma once
#include <string>
#include <vector>

namespace shop {
/** Totals prices given as text; throws std::runtime_error on one that isn't a number. */
int cart_total(const std::vector<std::string> &prices);
/** Applies a discount to an order that was never loaded: a null pointer write. */
void apply_discount_to_missing_order();
}
