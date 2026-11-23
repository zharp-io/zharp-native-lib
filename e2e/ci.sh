#!/usr/bin/env bash
# The crash test on a GitHub Actions runner (Linux, macOS, Windows under Git Bash): the shop from
# an already-built tree crashes three ways through Crashpad and reports handled errors to the
# stand-in API (receiver.mjs); its debug files become .sym with Mozilla's dump_syms, and each
# dump is walked with rust-minidump's minidump-stackwalk 0.27 (check.mjs). Both tools are the
# pinned releases, checksum-verified. Once @zharp/cli with debug-files is released, the upload
# step becomes `npx @zharp/cli debug-files upload` as in run.sh.
#
#   e2e/ci.sh <build dir> [out dir]
set -euo pipefail
BUILD=$1
OUT=${2:-$PWD/e2e-out}
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=8099
rm -rf "$OUT" && mkdir -p "$OUT/artifacts" "$OUT/tools"

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)   DS=dump_syms-x86_64-unknown-linux-gnu.tar.xz;   MS=minidump-stackwalk-x86_64-unknown-linux-musl.tar.xz ;;
  Linux-aarch64)  DS=dump_syms-aarch64-unknown-linux-gnu.tar.xz;  MS= ;;  # no prebuilt minidump-stackwalk: built below
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
if [ -n "$MS" ]; then
  fetch https://github.com/rust-minidump/rust-minidump/releases/download/v0.27.0 "$MS"
else
  # The same pinned version from crates.io, with its own lock file.
  cargo install minidump-stackwalk --version 0.27.0 --locked --quiet --root "$OUT/tools/minidump-stackwalk"
fi
DUMP_SYMS=$(find "$OUT/tools" -type f \( -name dump_syms -o -name dump_syms.exe \) | head -1)
STACKWALK_DIR=$(dirname "$(find "$OUT/tools" -type f \( -name minidump-stackwalk -o -name minidump-stackwalk.exe \) | head -1)")
export PATH="$STACKWALK_DIR:$PATH"

SHOP=$(find "$BUILD" -type f \( -name shop -o -name shop.exe \) -path '*examples*' | head -1)
HELLO=$(find "$BUILD" -type f \( -name hello_c -o -name hello_c.exe \) -path '*examples*' | head -1)
echo "shop: $SHOP"

# The .sym files, laid out as the stand-in keeps uploaded artifacts.
n=0
sym() {
  n=$((n + 1))
  "$DUMP_SYMS" "$@" > "$OUT/artifacts/$n.breakpad-sym"
  head -1 "$OUT/artifacts/$n.breakpad-sym"
  echo "{\"kind\":\"breakpad-sym\",\"name\":\"$(head -1 "$OUT/artifacts/$n.breakpad-sym" | cut -d' ' -f5 | tr -d '\r')\"}" > "$OUT/artifacts/$n.json"
}
case "$SHOP" in
  *.exe) sym "${SHOP%.exe}.pdb" ;;
  *) if [ "$(uname -s)" = Darwin ]; then
       # The DWARF stays in the .o files until dsymutil gathers it; without the .dSYM a .sym has
       # names (from the symbol table) but no files or lines.
       dsymutil "$SHOP" -o "$OUT/shop.dSYM"
       # Both files: files and lines from the .dSYM, unwind tables (STACK CFI) from the binary.
       for arch in $(lipo -archs "$SHOP"); do sym -a "$arch" "$OUT/shop.dSYM/Contents/Resources/DWARF/shop" "$SHOP"; done
     else
       sym "$SHOP"
       if [ "$(uname -m)" = aarch64 ]; then
         # On ARM64 the walk can't get from abort() (libc) back into the app by frame pointers alone:
         # the caller's stack pointer isn't in the frame record, and the app's unwind rules start
         # from it. libc's and libstdc++'s own unwind tables (.eh_frame, no debug info needed) do
         # it, as a symbol server for the distribution's libraries would.
         for lib in libc.so.6 libstdc++.so.6; do
           sym "$(ldd "$SHOP" | awk -v l="$lib" '$1 == l { print $3 }')"
         done
       fi
     fi ;;
esac

node "$HERE/receiver.mjs" "$OUT" $PORT &
RECEIVER=$!
trap 'kill $RECEIVER 2>/dev/null || true' EXIT
sleep 1

export ZHARP_KEY=zpk_e2e ZHARP_ENDPOINT=http://127.0.0.1:$PORT/api/v1/otlp ZHARP_RELEASE=1.0.0 ZHARP_ENVIRONMENT=e2e
export ZHARP_DATABASE_PATH="$OUT/db"
count() { ls "$OUT/minidumps" 2>/dev/null | grep -c '\.dmp$' || true; }
i=0
for mode in segv abort throw; do
  echo "== shop $mode"
  "$SHOP" $mode || echo "   exited with $?"
  i=$((i + 1))
  for _ in $(seq 1 150); do [ "$(count)" -ge $i ] && break; sleep 0.2; done
done
"$SHOP" handled
"$SHOP" message
ZHARP_LOGS=true "$SHOP" ok
"$HELLO"

node "$HERE/check.mjs" "$OUT" segv abort throw
