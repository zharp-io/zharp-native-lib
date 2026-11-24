#!/usr/bin/env bash
# Zharp for Qt on Android, on the emulator (android-emulator-runner, KVM): the Qt app reports a
# handled C++ error, then crashes three ways (a null pointer, an uncaught C++ exception, qFatal);
# each crash is reported by the next launch from Android's exit record.
#   bash run.sh <apk> <unstripped libqt_shop_android_x86_64.so>
set -uo pipefail
APK=$1 LIB=$2
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$RUNNER_TEMP/sent"
APP=io.zharp.e2e.qtshop
FAILED=0
mkdir -p "$OUT"
API="$(adb shell getprop ro.build.version.sdk | tr -d '\r')"
echo "API level $API"
adb install -r "$APK" || exit 1
python3 "$HERE/mock_zharp.py" "$OUT" > "$RUNNER_TEMP/mock.log" 2>&1 &
MOCK=$!
sleep 1

run() { # action, seconds
  adb shell am force-stop "$APP"
  adb shell am start -n "$APP/org.qtproject.qt.android.bindings.QtActivity" ${1:+-e applicationArguments "$1"} > /dev/null
  sleep "$2"
  echo "  --- logcat (${1:-launch})"
  adb logcat -d | grep -E "zharp|Zharp|qt e2e|DEBUG|libc |AndroidRuntime|F qt" | tail -20 | sed 's/^/    /'
  adb logcat -c
}
logs() { python3 "$HERE/otlp_logs.py" "$OUT" > "$OUT/logs.json"; }
fail() { echo "::error::API $API: $1"; FAILED=1; }
count() { python3 -c "import json,sys; print(sum(1 for r in json.load(open('$OUT/logs.json')) if r['attributes'].get('exception.type') == sys.argv[1]))" "$1"; }
check() { # exception.type, python expression on record r (a: its attributes) that must hold
  python3 - "$1" "$2" <<'PY' || fail "check on $1: $2"
import json, os, sys
kind, condition = sys.argv[1], sys.argv[2]
records = [r for r in json.load(open(os.environ["OUT"] + "/logs.json")) if r["attributes"].get("exception.type") == kind]
assert records, "no %s record" % kind
r = records[-1]
a = r["attributes"]
assert eval(condition), "%s\n%s" % (condition, a.get("exception.stacktrace"))
print("  ok  %s: %s" % (kind, condition))
PY
}
# The frame of `function` in the stack, symbolized from the unstripped library at its offset:
# what the backend does with the uploaded .sym file.
line_of() { # exception.type, function, file
  python3 - "$1" "$2" "$LIB" "$3" <<'PY' || fail "symbolizing $2 for $1"
import json, os, re, subprocess, sys
kind, function, lib, file = sys.argv[1:5]
r = [r for r in json.load(open(os.environ["OUT"] + "/logs.json")) if r["attributes"].get("exception.type") == kind][-1]
a = r["attributes"]
ids = a.get("zharp.native_build_ids", "")
notes = subprocess.run(["llvm-readelf", "-n", lib], capture_output=True, text=True).stdout
build_id = re.search(r"Build ID: ([0-9a-f]+)", notes).group(1)
assert build_id + " libqt_shop_android_x86_64.so" in ids, "build ID %s not in %r" % (build_id, ids)
print("  ok  %s: build ID %s matches the library" % (kind, build_id))
offsets = re.findall(r"libqt_shop_android_x86_64\.so \+0x([0-9a-f]+)", a["exception.stacktrace"])
assert offsets, a["exception.stacktrace"]
found = []
for i, off in enumerate(offsets):
    address = int(off, 16) - (1 if i or kind not in ("SIGSEGV",) else 0)
    out = subprocess.run(["llvm-symbolizer", "--obj=" + lib, hex(address)], capture_output=True, text=True).stdout.split("\n")
    found.append("%s %s" % (out[0], os.path.basename(out[1]) if len(out) > 1 else ""))
print("     frames: " + " | ".join(found[:5]))
assert any(function in f and file + ":" in f and not f.endswith(":0:0") for f in found), "%s in %s not among %s" % (function, file, found)
print("  ok  %s: %s, %s, line" % (kind, function, file))
PY
}
export OUT

adb logcat -c
run "" 10            # the first launch with Zharp: earlier exits aren't reported
run handled 10
logs
[ "$(count std::runtime_error)" = 1 ] || fail "expected one handled std::runtime_error, got $(count std::runtime_error)"
check std::runtime_error "a['zharp.handled'] == 'true' and a['exception.message'] == 'Price is not a number: \"twelve\"'"
check std::runtime_error "a.get('session.id') and a.get('enduser.id') == 'qt-user-7' and a.get('zharp.native_build_ids')"
check std::runtime_error "a.get('screen.name') == 'Checkout'"
line_of std::runtime_error main main.cpp      # where it was caught and reported

run segv 15           # time for Android to attach the tombstone to the exit record
echo "  --- Android's exit records"
adb shell dumpsys activity exit-info "$APP" | grep -E "reason=|status=|description=|trace" | head -12 | sed 's/^/    /'
adb logcat -d -b crash | grep -E "signal|Abort|#0[0-3]" | head -12 | sed 's/^/    /'
run "" 15
logs
[ "$(count SIGSEGV)" = 1 ] || fail "expected one SIGSEGV report, got $(count SIGSEGV)"
check SIGSEGV "a['zharp.handled'] == 'false' and a['exception.message'].startswith('SEGV_MAPERR at 0x')"
check SIGSEGV "a.get('screen.name') == 'Checkout' and a.get('session.id')"
line_of SIGSEGV apply_discount cart.cpp

run cxx 15
run "" 15
logs
check SIGABRT "'std::runtime_error' in a['exception.message'] and 'seven' in a['exception.message']"
line_of SIGABRT shop::parse_price cart.cpp

run fatal 15
run "" 15
logs
check SIGABRT "a['exception.message'].startswith('qFatal: Payments database missing: /data/payments.db')"

run "" 10            # nothing new: nothing sent again
logs
[ "$(count SIGSEGV)" = 1 ] && [ "$(count SIGABRT)" = 2 ] && [ "$(count std::runtime_error)" = 1 ] || fail "a report was sent twice: $(count SIGSEGV) SIGSEGV, $(count SIGABRT) SIGABRT, $(count std::runtime_error) handled"
python3 -c "import json; [print('  sent:', r['attributes'].get('exception.type'), '|', r['attributes'].get('exception.message','')[:90]) for r in json.load(open('$OUT/logs.json')) if r['attributes'].get('exception.type')]"

kill $MOCK
exit $FAILED
