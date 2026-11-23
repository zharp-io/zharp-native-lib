// A few lines of test harness: CHECK keeps going, the count of failures is the exit code.
#pragma once
#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        std::string _a = (a), _b = (b);                                                                    \
        if (_a != _b) {                                                                                    \
            fprintf(stderr, "%s:%d: CHECK_EQ failed:\n  %s\n  %s\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); \
            g_failures++;                                                                                  \
        }                                                                                                  \
    } while (0)
static bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
