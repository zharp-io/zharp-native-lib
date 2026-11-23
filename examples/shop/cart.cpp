#include "cart.h"

// The sum of the cart. A null `items` with a count (an order whose cart was never loaded) reads
// through a null pointer: the crash the example's "segv" mode shows.

int Cart::total() const {
    int sum = 0;
    for (int i = 0; i < count; i++) {
        sum += items[i].price * items[i].quantity;
    }
    return sum;
}
