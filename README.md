# Zharp for C and C++

[Zharp](https://zharp.io) for native apps, games and services on Windows, macOS and Linux:
crashes through [Crashpad](https://chromium.googlesource.com/crashpad/crashpad), the errors you
handle, breadcrumbs, logs and spans — each read as functions, files and lines from your build's
debug files.

- **Crashes** are written by Crashpad's handler, a separate process that watches your app, so a
  crash is reported even when the heap or stack is corrupt. An uncaught C++ exception arrives as
  itself (`std::runtime_error: inventory file is corrupt`), not as `SIGABRT`.
- **Handled errors** (`zharp_capture_error`, `zharp::capture_exception`) carry the stack where you
  report them, the user, the session and the last 100 breadcrumbs.
- **Logs** are off by default (the Zharp agent brings in your log files); `zharp_log` lines are
  breadcrumbs either way. **Spans** for long work, with `traceparent` in and out.
- A C API (`zharp.h`) for C, C++, Rust FFI and engines; a header-only C++ layer (`zharp.hpp`); a
  libcurl helper (`zharp_curl.h`) with request and response headers, secrets `[REDACTED]`.

CMake 3.16+, a C++17 compiler (MSVC, Clang, GCC). Linux needs libcurl (`libcurl4-openssl-dev`).

## Install

```cmake
include(FetchContent)
FetchContent_Declare(zharp
    GIT_REPOSITORY https://github.com/zharp-io/zharp-native-lib.git
    GIT_TAG v0.1.0)
FetchContent_MakeAvailable(zharp)

target_link_libraries(my_app PRIVATE zharp::zharp)
zharp_add_crash_handler(my_app)                          # crashpad_handler next to my_app
zharp_upload_debug_files(my_app APP <application id> INCLUDE_SOURCES)
```

`add_subdirectory(zharp-native-sdk)` works the same. Installed (`cmake --install`, vcpkg, Conan):

```cmake
find_package(zharp CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE zharp::zharp)
zharp_add_crash_handler(my_app)
```

Ship `crashpad_handler` (`crashpad_handler.exe`) next to your executable; that is where the SDK
looks for it (or set `ZHARP_HANDLER_PATH`):

```cmake
install(PROGRAMS $<TARGET_FILE:zharp::crashpad_handler> DESTINATION bin)
```

## Set up

```cpp
#include <zharp.hpp>

int main(int argc, char **argv) {
    zharp_options_t options;
    zharp_options_init(&options);
    options.key = "zpk_…";                   // or ZHARP_KEY
    options.environment = "production";
    options.release = "1.4.2";
    zharp::Init zharp(&options);             // zharp_shutdown() when main returns

    try {
        load_level(path);
    } catch (const std::exception &e) {
        zharp::capture_exception(e);         // a handled error, with the stack of this call
    }

    zharp::Span span("nightly report");      // ends with its scope
    zharp::set_user("u_42", "ada@example.com");
    zharp_log(ZHARP_LEVEL_INFO, "report written");
}
```

From C: `zharp_init(NULL)` reads everything from the environment;
`zharp_capture_error("ConfigError", "config file missing")`; `zharp_shutdown()` before exit.
Daemons and services set `options.sessions = 0` (no sessions or device ID).

| Option | Variable | Default |
|---|---|---|
| `key` | `ZHARP_KEY` | none: nothing is sent |
| `service` | `ZHARP_SERVICE` | the executable's name |
| `environment` | `ZHARP_ENVIRONMENT` | none |
| `release` | `ZHARP_RELEASE` | none |
| `endpoint` | `ZHARP_ENDPOINT` | `https://api.zharp.io/api/v1/otlp` |
| `logs` | `ZHARP_LOGS` | off |
| `crash_reporting` | `ZHARP_CRASH_REPORTING` | on |
| `sessions` | `ZHARP_SESSIONS` | on: a session per launch, or after 30 minutes idle |
| `tracing` | `ZHARP_TRACING` | on |
| `sample_rate` | `ZHARP_SAMPLE_RATE` | 1 |
| `handler_path` | `ZHARP_HANDLER_PATH` | `crashpad_handler` next to the executable |
| `database_path` | `ZHARP_DATABASE_PATH` | the user's cache folder, `zharp/<service>` |
| `disabled` | `ZHARP_DISABLED` | off |
| `debug` | `ZHARP_DEBUG` | off (prints what the SDK does to stderr) |

## How it works

- **Crashes:** Crashpad's handler writes a minidump and POSTs it to
  `/api/v1/minidump?key=…` (gzipped multipart, as Crashpad always does), with the release,
  environment, service, session, device, user, tags and breadcrumbs as annotations. A report that
  can't be sent waits in `<database>/crashpad` and goes with the next launch. Zharp walks the dump
  with rust-minidump's `minidump-stackwalk` and the `.sym` files you uploaded.
- **Everything else** is OTLP/HTTP JSON to `/api/v1/otlp/v1/logs` and `/v1/traces`, from one
  background thread (libcurl on Linux and macOS, WinHTTP on Windows). Batches that can't be sent
  are kept in `<database>/outbox` (up to 100) and sent later, also by the next launch.
- **Handled errors** are one log record each with `exception.*`, their stack as
  `#0  shop +0x7ebd` frames (a name where the module exports one), and the modules' Breakpad debug
  IDs in `zharp.native_modules`. Zharp names the frames from the same `.sym` files as crashes.
- **Uncaught C++ exceptions:** the SDK's `std::terminate` handler records the exception's type and
  `what()` (chaining to the handler that was there), then Crashpad writes the dump.

## Readable stack traces

Build with debug information (`RelWithDebInfo`, or `/Zi` + `/DEBUG` with MSVC) and upload it per
release build. `zharp_upload_debug_files` does that after the build when `ZHARP_AUTH_TOKEN` and
Node.js are there (skipping, never failing the build, when they aren't); in CI you can run it
yourself:

```sh
ZHARP_AUTH_TOKEN=… npx @zharp/cli debug-files upload build/ --app <id> --include-sources
```

## Crashpad

`external/crashpad` is [getsentry/crashpad](https://github.com/getsentry/crashpad) — Crashpad
with CMake files — pinned to `bc334ef` (see `ZHARP_CRASHPAD_COMMIT` in `CMakeLists.txt`). Clone
with `--recursive`; a copy without the submodule fetches the same commit at configure time.
`ZHARP_CRASHPAD_SOURCE_DIR` builds another checkout; `-DZHARP_CRASHPAD=OFF` builds without crash
reporting. Crashpad is Apache-2.0 (see `NOTICE`).

## Developing

```sh
git clone --recursive …
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build && ctest --test-dir build --output-on-failure
```

End to end on Linux, in Docker (gcc or clang): the shop example crashes three ways and reports
handled errors to a stand-in Zharp API; its debug files are uploaded with the Zharp CLI and each
dump is walked with `minidump-stackwalk` 0.27:

```sh
docker build -t zharp-native-sdk-dev e2e
docker run --rm -v "$PWD:/src" -v <zharp-js-sdk>/packages/cli:/cli:ro -v "$PWD/e2e/out:/out" \
  -e CC=clang -e CXX=clang++ -e NAME=clang zharp-native-sdk-dev sh /src/e2e/run.sh
```

GitHub Actions (`.github/workflows/ci.yml`) runs the tests and the crash check on Linux x64 and
arm64, macOS (universal) and Windows (MSVC). Packaging: `ports/vcpkg/zharp` (an overlay port) and
`conanfile.py`.

## License

MIT. Crashpad, built and shipped with it, is Apache-2.0.
