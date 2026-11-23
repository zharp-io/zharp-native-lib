#!/bin/sh
# The Qt layer end to end on Linux, in Docker (e2e/qt/Dockerfile: Debian's Qt 6.8 and Qt 5.15):
#   1. Qt 6 + CMake: the SDK with ZHARP_QT, its tests (offscreen), the widgets shop and the QML shop.
#   2. Qt 5.15 + qmake: the SDK built shared and installed, the widgets shop built with qmake
#      against share/zharp/zharp_qt.pri.
#   3. The shops' debug files uploaded with the Zharp CLI to a stand-in API; each shop crashes
#      (a null read or write) and calls qFatal under Xvfb, and runs once without failing with
#      ZHARP_LOGS=true; its prices lookup goes to the stand-in too.
#   4. check.mjs walks each dump with minidump-stackwalk 0.27 and the uploaded .sym files, and
#      checks the screen, the breadcrumbs, qFatal, the logs and the HTTP spans.
#
#   docker build -t zharp-native-sdk-qt e2e/qt
#   docker run --rm -v <this repo>:/src -v <zharp-js-sdk>/packages/cli:/cli:ro -v <out>:/out \
#     zharp-native-sdk-qt sh /src/e2e/qt/run.sh
set -eu
OUT=/out/qt
BUILD=${BUILD:-/build/e2e-qt6}
BUILD5=${BUILD5:-/build/e2e-qt5}
PREFIX5=/opt/zharp-qt5
QMAKE_BUILD=/build/e2e-qmake
PORT=8099
rm -rf "$OUT" && mkdir -p "$OUT"

echo "== build: Qt $(qmake6 -query QT_VERSION), CMake, static"
cmake -S /src -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DZHARP_QT=ON > "$OUT/configure-qt6.log"
ninja -C "$BUILD" > "$OUT/build-qt6.log"
(cd "$BUILD" && ctest --output-on-failure)

echo "== build: Qt $(qmake -query QT_VERSION), CMake, shared, installed; then the shop with qmake"
cmake -S /src -B "$BUILD5" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DZHARP_QT=ON -DZHARP_QT_MAJOR=5 \
  -DBUILD_SHARED_LIBS=ON -DZHARP_BUILD_EXAMPLES=OFF -DZHARP_INSTALL=ON -DCMAKE_INSTALL_PREFIX="$PREFIX5" > "$OUT/configure-qt5.log"
ninja -C "$BUILD5" > "$OUT/build-qt5.log"
(cd "$BUILD5" && ctest --output-on-failure)
cmake --install "$BUILD5" > "$OUT/install-qt5.log"
rm -rf "$QMAKE_BUILD" && mkdir -p "$QMAKE_BUILD"
(cd "$QMAKE_BUILD" && qmake ZHARP_PREFIX="$PREFIX5" /src/examples/qt/widgets/qt_shop.pro > "$OUT/qmake.log" && make -j"$(nproc)" >> "$OUT/qmake.log")
test -x "$QMAKE_BUILD/crashpad_handler"  # the .pri copied it next to the shop

node /src/e2e/receiver.mjs "$OUT" $PORT &
RECEIVER=$!
trap 'kill $RECEIVER 2>/dev/null || true' EXIT
sleep 1

echo "== debug-files upload (the Zharp CLI, dump_syms from PATH)"
export ZHARP_AUTH_TOKEN=zat_e2e ZHARP_URL=http://127.0.0.1:$PORT
node /cli/dist/cli.js debug-files upload "$BUILD/examples/qt/qt_shop" --app 7 --include-sources
node /cli/dist/cli.js debug-files upload "$BUILD/examples/qt/qt_qml" --app 7 --include-sources
node /cli/dist/cli.js debug-files upload "$QMAKE_BUILD/qt_shop" --app 7 --include-sources
unset ZHARP_AUTH_TOKEN ZHARP_URL

export ZHARP_KEY=zpk_e2e
export ZHARP_ENDPOINT=http://127.0.0.1:$PORT/api/v1/otlp
export ZHARP_RELEASE=2.0.0
export ZHARP_ENVIRONMENT=e2e
export ZHARP_SHOP_API=http://127.0.0.1:$PORT/shop
export ZHARP_DATABASE_PATH=/tmp/zharp-qt-e2e
rm -rf "$ZHARP_DATABASE_PATH"

count() { ls "$OUT/minidumps" 2>/dev/null | grep -c '\.dmp$' || true; }
n=0
crash() { # <label> <command…>: runs it and waits for its dump
  label=$1; shift
  echo "== $label"
  "$@" || echo "   exited with $?"
  n=$((n + 1))
  i=0
  while [ "$(count)" -lt $n ] && [ $i -lt 150 ]; do sleep 0.2; i=$((i + 1)); done
}

# Under Xvfb (xcb, no window manager); the QML shop renders with Qt Quick's software backend.
xvfb-run -a -s "-screen 0 1280x800x24" sh -c '
  set -u
  OUT='"$OUT"'; BUILD='"$BUILD"'
  count() { ls "$OUT/minidumps" 2>/dev/null | grep -c "\.dmp\$" || true; }
  n=0
  crash() { label=$1; shift; echo "== $label"; "$@" || echo "   exited with $?"; n=$((n + 1)); i=0
    while [ "$(count)" -lt $n ] && [ $i -lt 150 ]; do sleep 0.2; i=$((i + 1)); done; }
  export QT_QUICK_BACKEND=software
  crash "qt_shop crash (Qt 6, xcb)" timeout 60 "$BUILD/examples/qt/qt_shop" crash
  crash "qt_shop fatal (Qt 6, xcb)" timeout 60 "$BUILD/examples/qt/qt_shop" fatal
  crash "qt_qml crash (Qt 6, xcb)" timeout 60 "$BUILD/examples/qt/qt_qml" crash
  crash "qt_qml fatal (Qt 6, xcb)" timeout 60 "$BUILD/examples/qt/qt_qml" fatal
  echo "== qt_shop ok, qt_qml ok (ZHARP_LOGS=true)"
  ZHARP_LOGS=true timeout 60 "$BUILD/examples/qt/qt_shop" ok
  ZHARP_LOGS=true timeout 60 "$BUILD/examples/qt/qt_qml" ok
'
n=4
# Qt 5.15, built with qmake, on Qt's offscreen platform.
QT_QPA_PLATFORM=offscreen crash "qt_shop crash (Qt 5.15, qmake, offscreen)" timeout 60 "$QMAKE_BUILD/qt_shop" crash

echo "== check"
node /src/e2e/qt/check.mjs "$OUT" qt_shop:crash qt_shop:fatal qt_qml:crash qt_qml:fatal qt_shop:crash
