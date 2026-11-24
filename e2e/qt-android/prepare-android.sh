#!/usr/bin/env bash
# The app's Android package folder (QT_ANDROID_PACKAGE_SOURCE_DIR = e2e/qt-android/android), as a
# Qt app gets one (Qt Creator: Create Templates): Qt's build.gradle and AndroidManifest.xml, with
#  - Zharp for Android in the dependencies (from Maven Local here: built from its branch),
#  - cleartext HTTP allowed, for the stand-in Zharp on the runner (10.0.2.2) — not for real apps.
#   ./prepare-android.sh <Qt for Android's root>
set -euo pipefail
QT=$1
mkdir -p android
cp "$QT/src/android/templates/build.gradle" android/build.gradle
cp "$QT/src/android/templates/AndroidManifest.xml" android/AndroidManifest.xml
python3 - <<'PY'
import re
p = "android/build.gradle"
s = open(p).read()
s = s.replace("mavenCentral()", "mavenCentral()\n        mavenLocal()")
s = re.sub(r"(\ndependencies \{\n)", r"\1    implementation 'io.zharp:android:1.5.0'\n", s, count=1)
assert "io.zharp:android" in s, s
open(p, "w").write(s)
p = "android/AndroidManifest.xml"
s = open(p).read()
s = s.replace("<application ", '<application android:usesCleartextTraffic="true" ', 1)
assert "usesCleartextTraffic" in s
open(p, "w").write(s)
PY
grep -n -A4 "^dependencies" android/build.gradle
grep -n "usesCleartextTraffic\|<activity" android/AndroidManifest.xml | head -3
