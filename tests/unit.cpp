// The internals: JSON, debug IDs, the stack format, URLs, time.
#include "check.h"
#include "internal.h"

#include <cstring>

#if defined(_MSC_VER)
#  define NOINLINE __declspec(noinline)
#else
#  define NOINLINE __attribute__((noinline))
#endif

using namespace zharp;

static void json() {
    CHECK_EQ(json_string("plain"), "\"plain\"");
    CHECK_EQ(json_string("a \"quote\"\\ and\nnew line\t"), "\"a \\\"quote\\\"\\\\ and\\nnew line\\t\"");
    CHECK_EQ(json_string(std::string("\x01", 1)), "\"\\u0001\"");
    CHECK_EQ(json_string("caf\xc3\xa9 \xe2\x9c\x93"), "\"caf\xc3\xa9 \xe2\x9c\x93\"");
    // Invalid UTF-8 (a lone continuation byte, a cut sequence) becomes U+FFFD.
    CHECK_EQ(json_string("a\x80z"), "\"a\xef\xbf\xbdz\"");
    CHECK_EQ(json_string("\xe2\x9c"), "\"\xef\xbf\xbd\xef\xbf\xbd\"");
    CHECK_EQ(otlp_attributes({{"service.name", "shop"}, {"empty", ""}}),
             "[{\"key\":\"service.name\",\"value\":{\"stringValue\":\"shop\"}}]");
}

static void debug_ids() {
    // A GNU build ID: the first 16 bytes as a GUID (first three fields little-endian), age 0 —
    // what dump_syms writes in the .sym's MODULE line and the minidump records.
    const unsigned char build_id[20] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                        0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14};
    CHECK_EQ(debug_id_from_bytes(build_id, 20, true), "0403020106050807090A0B0C0D0E0F100");
    // A short build ID is padded with zeros.
    CHECK_EQ(debug_id_from_bytes(build_id, 8, true), "040302010605080700000000000000000");
    // A Mach-O UUID: as it is.
    CHECK_EQ(debug_id_from_bytes(build_id, 16, false), "0102030405060708090A0B0C0D0E0F100");
}

static void stacks() {
    Stack stack;
    Module app;
    app.name = "shop";
    app.debug_id = "ABC0";
    app.debug_file = "shop";
    app.base = 0x1000;
    app.size = 0x1000;
    stack.modules.push_back(app);
    Frame named;
    named.address = 0x1234;
    named.module = 0;
    named.function = "Cart::total() const [clone .cold]";
    Frame bare;
    bare.address = 0x1a00;
    bare.module = 0;
    Frame lost;
    lost.address = 0x9999;
    stack.frames = {named, bare, lost};
    CHECK_EQ(format_stack(stack), "#0  Cart::total() const (clone .cold) [shop +0x234]\n#1  shop +0xa00\n#2  ??? +0x9999");
    CHECK_EQ(format_modules(stack), "shop\tABC0\tshop");
}

NOINLINE static Stack capture_here() {
#if defined(_MSC_VER)
    return capture_stack(nullptr);
#else
    return capture_stack(__builtin_return_address(0));
#endif
}

// Not inlined into main: Apple's unwinder reports no frame above main (dyld's start has none),
// so main alone would be the only frame past the caller.
NOINLINE static void capturing() {
    Stack stack = capture_here();
    CHECK(stack.frames.size() >= 2);
    CHECK(!stack.modules.empty());
    if (stack.frames.empty() || stack.modules.empty()) return;
    // The first frame is in this test's own executable, with a debug ID to find its .sym by.
    const Frame &top = stack.frames[0];
    CHECK(top.module >= 0);
    if (top.module < 0) return;
    const Module &module = stack.modules[(size_t)top.module];
    std::string exe = basename_of(executable_path());
    CHECK_EQ(module.name, exe);
#if !defined(__APPLE__) || defined(__x86_64__) || defined(__aarch64__)
    CHECK(module.debug_id.size() >= 33);
#endif
    CHECK(top.address > module.base && top.address < module.base + module.size);
    printf("  captured %zu frames; #0 in %s %s +0x%llx\n", stack.frames.size(), module.name.c_str(), module.debug_id.c_str(),
           (unsigned long long)(top.address - module.base));
}

static void urls() {
    Config config;
    config.endpoint = "https://api.zharp.io/api/v1/otlp";
    config.key = "zpk_a+b/c";
    CHECK_EQ(minidump_url(config), "https://api.zharp.io/api/v1/minidump?key=zpk_a%2Bb%2Fc");
    config.endpoint = "http://127.0.0.1:4318";
    config.key = "zpk_x";
    CHECK_EQ(minidump_url(config), "http://127.0.0.1:4318/minidump?key=zpk_x");
}

static void times() {
    CHECK_EQ(iso_time(1763892000123000000ull), "2025-11-23T10:00:00.123Z");
    CHECK(random_hex(8).size() == 16);
    std::string uuid = random_uuid();
    CHECK(uuid.size() == 36 && uuid[14] == '4');
    CHECK(random_hex(16) != random_hex(16));
}

static void paths() {
    CHECK_EQ(basename_of("/usr/lib/libfoo.so.1"), "libfoo.so.1");
    CHECK_EQ(basename_of("C:\\Games\\Shop.exe"), "Shop.exe");
    CHECK_EQ(basename_of("shop"), "shop");
    CHECK_EQ(dirname_of("/opt/app/shop"), "/opt/app");
}

int main() {
    json();
    debug_ids();
    stacks();
    capturing();
    urls();
    times();
    paths();
    if (g_failures) fprintf(stderr, "%d check(s) failed\n", g_failures);
    else printf("unit tests passed\n");
    return g_failures ? 1 : 0;
}
