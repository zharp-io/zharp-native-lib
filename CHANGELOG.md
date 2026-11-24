# Changelog

## 0.2.0 — 2026-11-24

- Qt for Android: the C and Qt API forward to Zharp for Android (`io.zharp:android` 1.5.0+) over
  JNI. Crashes (SIGSEGV, an uncaught C++ exception, `abort()`, `qFatal`) come from Android's own
  exit records on the next launch (Android 12+ with the crashed thread, Android 11 the signal
  only); `zharp_capture_error` and `zharp::capture_exception` send the stack with each library's
  build ID; screen, user and tags go to Zharp for Android. Crashpad and libcurl are left out on
  Android. Not on Android yet: spans and `zharp_log`. Checked end to end on the emulator on CI.

## 0.1.0 — 2026-11-24

The first release: a C SDK with a C++ layer, for Windows, macOS and Linux.

- `zharp_init(&options)` / `zharp_init(NULL)`: settings from `zharp_options_t` or `ZHARP_KEY`,
  `ZHARP_SERVICE`, `ZHARP_ENVIRONMENT`, `ZHARP_RELEASE`, `ZHARP_ENDPOINT`, `ZHARP_LOGS`,
  `ZHARP_CRASH_REPORTING`, `ZHARP_SESSIONS`, `ZHARP_TRACING`, `ZHARP_SAMPLE_RATE`,
  `ZHARP_HANDLER_PATH`, `ZHARP_DATABASE_PATH`, `ZHARP_DISABLED`, `ZHARP_DEBUG`.
- Crashes through Crashpad (getsentry/crashpad, pinned): minidumps to `/api/v1/minidump` with the
  release, environment, service, session, device, user, tags and the last 100 breadcrumbs as
  annotations. An uncaught C++ exception is recorded with its type and `what()`.
- Handled errors and messages (`zharp_capture_error`, `zharp_capture_message`,
  `zharp::capture_exception`, `zharp::capture_current_exception`, `zharp::guard`) with the stack
  of the call and each module's Breakpad debug ID, named by Zharp from the uploaded `.sym` files.
- Logs off by default; `zharp_log` / `zharp_logf` lines are breadcrumbs, and logs with
  `ZHARP_LOGS=true`. `zharp_log_correlation()` stamps your own log lines with
  ` trace_id=… span_id=…`.
- Spans (`zharp_start_span`, `zharp_start_span_from(traceparent)`, `zharp::Span`), attributes,
  headers with secrets `[REDACTED]`, `zharp_traceparent` for outgoing calls; `zharp_curl_perform`
  for libcurl.
- Sessions per launch (or after 30 minutes idle) and a device ID, for desktop apps;
  `sessions = 0` for services.
- OTLP/HTTP JSON from a background thread (libcurl, WinHTTP), with an on-disk outbox for batches
  that can't be sent.
- CMake: `zharp::zharp`, `zharp_add_crash_handler`, `zharp_upload_debug_files`; FetchContent,
  `add_subdirectory` and `find_package(zharp)`; a vcpkg overlay port and a Conan recipe.
- `zharp_start_async_span` (a span that doesn't become the thread's current one, for work that
  finishes later), `zharp_span_traceparent`, and `zharp_set_crash_reason` (names a crash that is
  about to happen, for frameworks that abort on purpose). Named crashes (that, and uncaught
  exceptions) carry the stack recorded where they were named. Prebuilt shared binaries with the Qt
  layer (built against Qt 6.8, so for Qt 6.8 and later; Linux x64 needs glibc 2.35+, Linux arm64
  glibc 2.39+) from the release workflow. Older Qt 6 and Qt 5.15 build the SDK from source.
- Qt (`zharp::qt`, `zharp_qt.h`, Qt 6.5+ and 5.15, CMake and qmake): `ZharpQt::init(app)`
  chains a message handler (`qWarning` and `qCritical` as breadcrumbs or logs, `qFatal` naming
  the crash), sets `screen.name` from the focused window and QML `StackView` pages, and
  `ZharpQt::NetworkAccessManager` / `ZharpQt::instrument(engine)` record HTTP calls as spans
  with headers and `traceparent`. vcpkg feature `qt`, Conan option `qt`.
