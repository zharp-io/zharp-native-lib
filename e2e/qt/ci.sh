#!/usr/bin/env bash
# The Qt crash test on a GitHub Actions runner (Linux, macOS, Windows under Git Bash), on Qt's
# offscreen platform: the widgets and QML shops from an already-built tree (-DZHARP_QT=ON) crash
# and call qFatal through Crashpad, then run once without failing, against the stand-in API
# (../receiver.mjs). Their debug files become .sym with Mozilla's dump_syms, and check.mjs walks
# each dump with rust-minidump's minidump-stackwalk 0.27. Both tools are the pinned releases,
# checksum-verified, as in ../ci.sh.
#
#   e2e/qt/ci.sh <build dir> [out dir]
set -euo pipefail
BUILD=$1
OUT=${2:-$PWD/e2e-qt-out}
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=8099
rm -rf "$OUT" && mkdir -p "$OUT/artifacts" "$OUT/tools"

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)   DS=dump_syms-x86_64-unknown-linux-gnu.tar.xz;   MS=minidump-stackwalk-x86_64-unknown-linux-musl.tar.xz ;;
  Linux-aarch64)  DS=dump_syms-aarch64-unknown-linux-gnu.tar.xz;  MS=minidump-stackwalk-aarch64-unknown-linux-musl.tar.xz ;;
  Darwin-arm64)   DS=dump_syms-aarch64-apple-darwin.tar.xz;       MS=minidump-stackwalk-aarch64-apple-darwin.tar.xz ;;
  Darwin-x86_64)  DS=dump_syms-x86_64-apple-darwin.tar.xz;        MS=minidump-stackwalk-x86_64-apple-darwin.tar.xz ;;
  MINGW*|MSYS*)   DS=dump_syms-x86_64-pc-windows-msvc.zip;        MS=minidump-stackwalk-x86_64-pc-windows-msvc.zip ;;
  *) echo "no prebuilt tools for $(uname -s) $(uname -m)"; exit 1 ;;
esac
fetch() { # <base url> <archive>
  (cd "$OUT/tools" && curl -fsSLO "$1/$2" && curl -fsSLO "$1/$2.sha256" && \
    { command -v sha256sum >/dev/null && sha256sum -c "$2.sha256" || shasum -a 256 -c "$2.sha256"; } && \
    case "$2" in *.zip) unzip -qo "$2" ;; *) tar -xJf "$2" ;; esac)
}
fetch https://github.com/mozilla/dump_syms/releases/download/v2.3.9 "$DS"
fetch https://github.com/rust-minidump/rust-minidump/releases/download/v0.27.0 "$MS"
DUMP_SYMS=$(find "$OUT/tools" -type f \( -name dump_syms -o -name dump_syms.exe \) | head -1)
STACKWALK_DIR=$(dirname "$(find "$OUT/tools" -type f \( -name minidump-stackwalk -o -name minidump-stackwalk.exe \) | head -1)")
export PATH="$STACKWALK_DIR:$PATH"

find_example() { find "$BUILD" -type f \( -name "$1" -o -name "$1.exe" \) -path '*examples*' | head -1; }
QT_SHOP=$(find_example qt_shop)
QT_QML=$(find_example qt_qml)
echo "qt_shop: $QT_SHOP"
echo "qt_qml: $QT_QML"

# The .sym files, laid out as the stand-in keeps uploaded artifacts.
n=0
sym() {
  n=$((n + 1))
  "$DUMP_SYMS" "$@" > "$OUT/artifacts/$n.breakpad-sym"
  head -1 "$OUT/artifacts/$n.breakpad-sym"
  echo "{\"kind\":\"breakpad-sym\",\"name\":\"$(head -1 "$OUT/artifacts/$n.breakpad-sym" | cut -d' ' -f5 | tr -d '\r')\"}" > "$OUT/artifacts/$n.json"
}
for exe in "$QT_SHOP" "$QT_QML"; do
  case "$exe" in
    *.exe) sym "${exe%.exe}.pdb" ;;
    *) if [ "$(uname -s)" = Darwin ]; then
         for arch in $(lipo -archs "$exe"); do sym -a "$arch" "$exe"; done
       else
         sym "$exe"
       fi ;;
  esac
done

node "$HERE/../receiver.mjs" "$OUT" $PORT &
RECEIVER=$!
trap 'kill $RECEIVER 2>/dev/null || true' EXIT
sleep 1

export ZHARP_KEY=zpk_e2e ZHARP_ENDPOINT=http://127.0.0.1:$PORT/api/v1/otlp ZHARP_RELEASE=2.0.0 ZHARP_ENVIRONMENT=e2e
export ZHARP_SHOP_API=http://127.0.0.1:$PORT/shop ZHARP_DATABASE_PATH="$OUT/db"
export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
count() { ls "$OUT/minidumps" 2>/dev/null | grep -c '\.dmp$' || true; }
i=0
for run in "$QT_SHOP crash" "$QT_SHOP fatal" "$QT_QML crash" "$QT_QML fatal"; do
  echo "== $run"
  $run || echo "   exited with $?"
  i=$((i + 1))
  for _ in $(seq 1 150); do [ "$(count)" -ge $i ] && break; sleep 0.2; done
done
ZHARP_LOGS=true "$QT_SHOP" ok
ZHARP_LOGS=true "$QT_QML" ok

node "$HERE/check.mjs" "$OUT" qt_shop:crash qt_shop:fatal qt_qml:crash qt_qml:fatal
