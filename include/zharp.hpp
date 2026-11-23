// Zharp for C++: RAII on top of the C API (zharp.h). Header-only.
//
//   int main() {
//       zharp::Init zharp;                       // zharp_init(NULL) … zharp_shutdown()
//       zharp::Span span("load level");          // ends when it goes out of scope
//       try {
//           load();
//       } catch (const std::exception &e) {
//           zharp::capture_exception(e);         // "std::runtime_error: file missing", handled
//       }
//   }
#ifndef ZHARP_HPP
#define ZHARP_HPP

#include "zharp.h"

#include <exception>
#include <string>
#include <typeinfo>

// Inlined into your code, so the stack Zharp records starts where you report the error.
#if defined(_MSC_VER)
#  define ZHARP_HPP_INLINE __forceinline
#else
#  define ZHARP_HPP_INLINE inline __attribute__((always_inline))
#endif

#if !defined(_MSC_VER)
#  include <cstdlib>
#  include <cxxabi.h>
#endif

namespace zharp {

/** zharp_init on construction, zharp_shutdown on destruction (which sends what's queued). */
class Init {
public:
    explicit Init(const zharp_options_t *options = nullptr) : started_(zharp_init(options) == 0) {}
    ~Init() { zharp_shutdown(); }
    Init(const Init &) = delete;
    Init &operator=(const Init &) = delete;
    /** Whether Zharp is sending (false without a key, or with ZHARP_DISABLED). */
    bool started() const { return started_; }

private:
    bool started_;
};

/** A span that ends when it goes out of scope. An exception leaving the scope marks it failed. */
class Span {
public:
    explicit Span(const char *name) : span_(zharp_start_span(name)), exceptions_(std::uncaught_exceptions()) {}
    Span(const char *name, const char *traceparent) : span_(zharp_start_span_from(name, traceparent)), exceptions_(std::uncaught_exceptions()) {}
    ~Span() {
        if (std::uncaught_exceptions() > exceptions_) zharp_span_set_error(span_, "an exception left the span");
        zharp_end_span(span_);
    }
    Span(const Span &) = delete;
    Span &operator=(const Span &) = delete;

    Span &set(const char *key, const std::string &value) {
        zharp_span_set_attribute(span_, key, value.c_str());
        return *this;
    }
    Span &header(const char *side, const char *name, const std::string &value) {
        zharp_span_set_header(span_, side, name, value.c_str());
        return *this;
    }
    Span &kind(int kind) {
        zharp_span_set_kind(span_, kind);
        return *this;
    }
    void error(const std::string &message) { zharp_span_set_error(span_, message.c_str()); }
    zharp_span_t *get() const { return span_; }

private:
    zharp_span_t *span_;
    int exceptions_;
};

/** The readable name of a type: "std::runtime_error", "shop::PaymentDeclined". */
inline std::string type_name(const std::type_info &type) {
#if defined(_MSC_VER)
    std::string name = type.name();
    for (const char *prefix : {"class ", "struct "}) {
        std::string p = prefix;
        if (name.compare(0, p.size(), p) == 0) name = name.substr(p.size());
    }
    return name;
#else
    int status = 0;
    char *demangled = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
    std::string name = status == 0 && demangled ? demangled : type.name();
    std::free(demangled);
    return name;
#endif
}

/**
 * Reports an exception you caught, as a handled error: its type, what(), and the stack where you
 * report it (C++ keeps no stack of the throw; report it in the catch nearest to it).
 */
ZHARP_HPP_INLINE void capture_exception(const std::exception &e) {
    zharp_capture_error(type_name(typeid(e)).c_str(), e.what());
}

/** Inside a catch block: reports whatever was thrown, std::exception or not. */
ZHARP_HPP_INLINE void capture_current_exception() {
    std::exception_ptr current = std::current_exception();
    if (!current) return;
    try {
        std::rethrow_exception(current);
    } catch (const std::exception &e) {
        capture_exception(e);
    } catch (const char *text) {
        zharp_capture_error("const char*", text);
    } catch (const std::string &text) {
        zharp_capture_error("std::string", text.c_str());
    } catch (...) {
#if defined(_MSC_VER)
        zharp_capture_error("unknown exception", "");
#else
        const std::type_info *info = abi::__cxa_current_exception_type();
        zharp_capture_error(info ? type_name(*info).c_str() : "unknown exception", "");
#endif
    }
}

/** Reports an exception thrown by `work` (then rethrows it). */
template <class F>
auto guard(F &&work) -> decltype(work()) {
    try {
        return work();
    } catch (...) {
        capture_current_exception();
        throw;
    }
}

inline void log(zharp_level_t level, const std::string &message) { zharp_log(level, message.c_str()); }
inline void set_user(const std::string &id, const std::string &email = "", const std::string &name = "") {
    zharp_set_user(id.c_str(), email.empty() ? nullptr : email.c_str(), name.empty() ? nullptr : name.c_str());
}
inline void set_tag(const std::string &key, const std::string &value) { zharp_set_tag(key.c_str(), value.c_str()); }

}  // namespace zharp

#endif  // ZHARP_HPP
