#pragma once
#include <stdexcept>
#include <string>

struct Item {
    int price;
    int quantity;
};

class Cart {
public:
    Item *items = nullptr;
    int count = 0;
    int total() const;
};

struct Order {
    Cart *cart = nullptr;
};

class PaymentDeclined : public std::runtime_error {
public:
    explicit PaymentDeclined(const std::string &why) : std::runtime_error(why) {}
};

class Shop {
public:
    int checkout(Order &order);
    void charge(int cents);
};
