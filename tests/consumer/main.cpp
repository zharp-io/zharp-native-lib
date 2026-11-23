#include <zharp.hpp>

#include <cstdio>
#include <stdexcept>

int main() {
    zharp::Init zharp;  // no ZHARP_KEY here: Zharp stays off and every call is a no-op
    try {
        throw std::runtime_error("consumer");
    } catch (const std::exception &e) {
        zharp::capture_exception(e);
    }
    zharp::Span span("work");
    printf("consumer ran (zharp %s)\n", zharp.started() ? "on" : "off");
    return 0;
}
