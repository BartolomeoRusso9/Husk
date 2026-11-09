#!/bin/bash
# Stage build/java-runtime, build/android-system and the PS4 layer
# (build/ios-arm64/lib/libhuskps4.dylib) without the Lineage system image or
# the PS4 core's private trees.
#
# build_java_runtime.sh and build_android_system.sh copy those trees out of an
# Android system image that is not in this repository, and without them Xcode
# only warns: the IPA installs, but Java apps die with "no java/lang/Object on
# the boot class path" and every HTTPS call fails with "Trust anchor for
# certification path not found".
#
# Those parts do not depend on Husk's code -- libcore/ICU/tzdata/conscrypt,
# framework-res.apk, CA certificates, fonts -- so they are taken from an
# official release IPA. Husk's own Java framework (husk-framework*.dex) does
# depend on the code, so it is always compiled from src/java-framework.
#
# Usage: scripts/stage_release_runtime.sh
#   HUSK_RELEASE   release tag to take the Android files from (default 3.4)
#   HUSK_JDK       JDK home (default: $JAVA_HOME, then Homebrew's openjdk)
#   HUSK_D8        r8.jar (default: downloaded from Google's Maven, R8 8.5.35)
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE="${HUSK_RELEASE:-3.4}"
R8_VERSION=8.5.35
CACHE="$HUSK_ROOT/third_party/release-runtime"
JRT="$HUSK_ROOT/build/java-runtime"
SYS="$HUSK_ROOT/build/android-system"
mkdir -p "$CACHE"

has_javac() { [ -n "$1" ] && [ -x "$1/bin/javac" ] && [ -x "$1/bin/jar" ]; }
JDK="${HUSK_JDK:-}"
has_javac "$JDK" || JDK="${JAVA_HOME:-}"
has_javac "$JDK" || JDK="/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home"
has_javac "$JDK" || { echo "error: no JDK found; set HUSK_JDK or run: brew install openjdk" >&2; exit 1; }

D8="${HUSK_D8:-$CACHE/r8-$R8_VERSION.jar}"
if [ ! -f "$D8" ]; then
    echo "==> downloading R8 $R8_VERSION"
    curl -fsSL -o "$D8.part" "https://dl.google.com/android/maven2/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar"
    mv "$D8.part" "$D8"
fi

IPA="$CACHE/Husk-$RELEASE.ipa"
if [ ! -f "$IPA" ]; then
    echo "==> downloading Husk $RELEASE release IPA"
    curl -fsSL -o "$IPA.part" "https://github.com/Leviidev/Husk/releases/download/$RELEASE/Husk.ipa"
    mv "$IPA.part" "$IPA"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
unzip -q "$IPA" 'Payload/*/java-runtime/*' 'Payload/*/android-system/*' 'Payload/*/Frameworks/libhuskps4.dylib' -d "$TMP"
APP="$(ls -d "$TMP"/Payload/*.app)"
[ -f "$APP/java-runtime/com.android.art/javalib/core-oj.jar" ] || { echo "error: $IPA has no java-runtime" >&2; exit 1; }
[ -d "$APP/android-system/cacerts" ] || { echo "error: $IPA has no android-system" >&2; exit 1; }

echo "==> android-system from $RELEASE"
mkdir -p "$SYS"
rsync -a --delete "$APP/android-system/" "$SYS/"

echo "==> java-runtime libraries from $RELEASE"
mkdir -p "$JRT"
rsync -a --delete --exclude 'husk-framework*.dex' --exclude 'aosp/' "$APP/java-runtime/" "$JRT/"

# The PS4 layer: build_ps4_ios.sh needs Bachata-S4 and husk-fex, which are not
# public. Husk talks to it only through husk_console_module_v1
# (husk-console-abi.h), so the release's dylib works as long as that ABI is the
# same. A locally built one is never overwritten.
PS4="$HUSK_ROOT/build/ios-arm64/lib/libhuskps4.dylib"
if [ -f "$PS4" ] && [ ! -f "$PS4.from-release" ]; then
    echo "==> libhuskps4.dylib: keeping the locally built one"
elif [ -f "$APP/Frameworks/libhuskps4.dylib" ]; then
    echo "==> libhuskps4.dylib (PS4 layer) from $RELEASE"
    mkdir -p "$(dirname "$PS4")"
    cp "$APP/Frameworks/libhuskps4.dylib" "$PS4"
    echo "$RELEASE" > "$PS4.from-release"
else
    echo "==> $IPA has no libhuskps4.dylib; the app is packaged without the PS4 layer" >&2
fi

echo "==> husk-framework.dex from src/java-framework (JDK: $JDK)"
HUSK_JDK="$JDK" HUSK_D8="$D8" "$HUSK_ROOT/src/java-framework/build.sh" "$JRT/husk-framework.dex"

echo "==> staged: java-runtime $(du -sh "$JRT" | cut -f1), android-system $(du -sh "$SYS" | cut -f1)"
