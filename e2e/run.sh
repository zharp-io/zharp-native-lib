#!/bin/sh
# End to end on Linux, in Docker: build the SDK and the shop example with one compiler, upload the
# shop's debug files with the Zharp CLI, crash it three ways (null read, abort, uncaught C++
# exception) and report handled errors, all against a stand-in for the Zharp API; then walk each
# dump with minidump-stackwalk 0.27 and the uploaded .sym files.
#
#   docker build -t zharp-native-sdk-dev e2e
#   docker run --rm -v <this repo>:/src -v <zharp-js-sdk>/packages/cli:/cli:ro -v <out>:/out \
#     -e CC=gcc -e CXX=g++ -e NAME=gcc zharp-native-sdk-dev sh /src/e2e/run.sh
set -eu
CC=${CC:-gcc}
CXX=${CXX:-g++}
NAME=${NAME:-$CC}
BUILD=/build/e2e-$NAME
OUT=/out/$NAME
PORT=8099
rm -rf "$OUT" && mkdir -p "$OUT"

echo "== build ($CC)"
cmake -S /src -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" > "$OUT/configure.log"
ninja -C "$BUILD" > "$OUT/build.log"
(cd "$BUILD" && ctest --output-on-failure)

node /src/e2e/receiver.mjs "$OUT" $PORT &
RECEIVER=$!
trap 'kill $RECEIVER 2>/dev/null || true' EXIT
sleep 1

echo "== debug-files upload (the Zharp CLI, dump_syms from PATH)"
ZHARP_AUTH_TOKEN=zat_e2e ZHARP_URL=http://127.0.0.1:$PORT node /cli/dist/cli.js debug-files upload "$BUILD/examples/shop" --app 7 --include-sources

export ZHARP_KEY=zpk_e2e
export ZHARP_ENDPOINT=http://127.0.0.1:$PORT/api/v1/otlp
export ZHARP_RELEASE=1.0.0
export ZHARP_ENVIRONMENT=e2e
export ZHARP_DATABASE_PATH=/tmp/zharp-$NAME
rm -rf "$ZHARP_DATABASE_PATH"

count() { ls "$OUT/minidumps" 2>/dev/null | grep -c '\.dmp$' || true; }
n=0
for mode in segv abort throw; do
  echo "== shop $mode"
  "$BUILD/examples/shop" $mode || echo "   exited with $?"
  n=$((n + 1))
  i=0
  while [ "$(count)" -lt $n ] && [ $i -lt 100 ]; do sleep 0.2; i=$((i + 1)); done
done
for mode in handled message; do
  echo "== shop $mode"
  "$BUILD/examples/shop" $mode
done
echo "== shop ok (ZHARP_LOGS=true)"
ZHARP_LOGS=true "$BUILD/examples/shop" ok
echo "== hello_c"
"$BUILD/examples/hello_c"

echo "== check"
node /src/e2e/check.mjs "$OUT" segv abort throw
