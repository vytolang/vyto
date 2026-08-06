#!/bin/sh
# Packages apps/hn_android into a signed debug APK, with no Gradle.
#
# Twelve steps, ordered so the cheapest checks fail first. Every one of them
# asserts something a successful command does not: that the ELF is for the
# right architecture, that the manifest that shipped is the one manifest.vt
# produced, that the dex and the .so agree about the JNI boundary.
#
#     sh apps/hn_android/build-apk.sh
#
# NOT run by any test suite, and deliberately so: it needs an NDK and an SDK,
# and the host suite stays toolchain-free so CI never downloads either. Run it
# by hand. It produces an apk; it installs nothing and runs nothing.
#
# The apk is NOT installable-and-verified: nothing here runs on a device or an
# emulator. What it proves is that every artifact in the chain is well-formed
# and that the pieces agree with each other — the manifest with the library
# name, the dex with the .so's JNI exports, the badging with what manifest.vt
# declared.
set -eu
cd "$(dirname "$0")/../.."          # repo root

NDK_ROOT="${ANDROID_NDK_HOME:-$HOME/Android/Ndk}"
SDK_ROOT="${ANDROID_SDK_HOME:-$HOME/Android/Sdk}"
API=24                              # must match manifest.vt's minSdk (android.icu needs 24)
ABI=arm64-v8a
APP=apps/hn_android
BUILD=apps/hn_android/build     # gitignored
LIBNAME=vytoapp                     # VytoActivity.libraryName()
JAVA_SRC=lib/vyto/mobile/android/java

CC="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang"
BT=$(ls -d "$SDK_ROOT"/build-tools/* 2>/dev/null | sort -V | tail -1)
ANDROID_JAR=$(ls "$SDK_ROOT"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -1)

step() { printf '\n== %s\n' "$1"; }
die()  { echo "FATAL: $1" >&2; exit 1; }

# --- 0. preflight ---------------------------------------------------------
# Every "works on my machine" failure lands here rather than three steps in.
step "0  preflight"
[ -x ./vytoc ]      || die "./vytoc not built — run 'make' first"
[ -x "$CC" ]        || die "NDK clang not found: $CC"
[ -n "$BT" ]        || die "no build-tools under $SDK_ROOT"
[ -n "$ANDROID_JAR" ] || die "no android.jar under $SDK_ROOT/platforms"
for t in aapt2 d8 zipalign apksigner; do
    [ -x "$BT/$t" ] || die "$t missing from $BT"
done
command -v javac >/dev/null || die "javac not on PATH"
command -v keytool >/dev/null || die "keytool not on PATH"
echo "ndk=$(sed -n 's/^Pkg.Revision = //p' "$NDK_ROOT/source.properties")"
echo "build-tools=$(basename "$BT")  jar=$(basename "$(dirname "$ANDROID_JAR")")"

rm -rf "$BUILD"
mkdir -p "$BUILD/apk/lib/$ABI" "$BUILD/res" "$BUILD/classes" "$BUILD/dex"

# --- 1. manifest ----------------------------------------------------------
# Runs on the host with no shim behind it, so a bad manifest costs nothing to
# find. manifest.vt panics rather than printing, so `set -e` stops here and the
# redirect leaves an empty file instead of a broken one.
step "1  manifest (vytoc run manifest.vt)"
./vytoc run "$APP/manifest.vt" > "$BUILD/AndroidManifest.xml"
[ -s "$BUILD/AndroidManifest.xml" ] || die "manifest.vt produced nothing"
grep -q 'package="dev.vyto.hn"' "$BUILD/AndroidManifest.xml" || die "manifest lost its package"
# targetSdk >= 30 hides other packages from an app that has not declared an
# interest in them, so without this block Actions.view_url resolves to nothing
# and returns false with no diagnostic anywhere. Tapping a story link would
# just do nothing, on a device, silently. mf.uses(ACT_VIEW_URL) emits it.
grep -q '<queries>' "$BUILD/AndroidManifest.xml" \
    || die "no <queries> — story links would silently fail to open on targetSdk >= 30"
echo "ok  $(wc -l < "$BUILD/AndroidManifest.xml") lines"

# --- 2. native ------------------------------------------------------------
step "2  native (.so for android-arm64)"
so=$(./vytoc build "$APP/$LIBNAME.vt" --target android-arm64 --shared --cc "$CC" | tail -1)
[ -f "$so" ] || die "no .so produced"
cp "$so" "$BUILD/apk/lib/$ABI/lib$LIBNAME.so"
echo "ok  $(du -h "$so" | cut -f1)  $so"

# --- 3. ELF audit ---------------------------------------------------------
# The checks that a successful link does not give you. -shared tolerates
# unresolved symbols, so "it linked" says nothing about whether it will load.
step "3  ELF audit"
readelf -h "$so" | grep -q 'AArch64'        || die "not an AArch64 object"
nm -D --defined-only "$so" | grep -q ' T vyto_app_main' || die "no vyto_app_main — --shared did not emit the entry"
nm -D --defined-only "$so" | grep -q ' T JNI_OnLoad'    || die "no JNI_OnLoad — jni_boot.c did not compile in"
# #link conditions are OS-prefix matches, so `if "android"` dropping is silent.
for l in liblog.so libandroid.so libjnigraphics.so; do
    readelf -d "$so" | grep -q "\[$l\]" || die "#link silently dropped $l"
done
# Android 15 runs 16 KB pages; 4 KB-aligned LOAD segments simply do not load.
if readelf -lW "$so" | awk '/^  LOAD/ {print $NF}' | grep -qv '0x4000'; then
    die "LOAD segments not 16 KB aligned — will not load on Android 15+"
fi
echo "ok  AArch64, entry + JNI_OnLoad present, 3 platform libs linked, 16 KB aligned"

# --- 4. API level ---------------------------------------------------------
# Building against 24 while declaring a lower minSdk installs fine and then
# crashes on the devices that minSdk was lowered for.
step "4  API level agreement"
minsdk=$(sed -n 's/.*android:minSdkVersion="\([0-9]*\)".*/\1/p' "$BUILD/AndroidManifest.xml")
[ "$minsdk" = "$API" ] || die "manifest minSdk=$minsdk but built against API $API"
echo "ok  minSdk=$minsdk == NDK target API $API"

# --- 5. Java -> dex -------------------------------------------------------
# android.jar on the bootclasspath and nothing else: no AndroidX, no jar
# dependencies. If something sneaks one in, javac fails here.
step "5  java + d8"
javac -nowarn -source 8 -target 8 -bootclasspath "$ANDROID_JAR" \
      -d "$BUILD/classes" "$JAVA_SRC"/dev/vyto/android/*.java 2>&1 | grep -v '^Note:' || true
[ -f "$BUILD/classes/dev/vyto/android/VytoActivity.class" ] || die "javac produced no classes"
"$BT/d8" --min-api "$API" --output "$BUILD/dex" \
         $(find "$BUILD/classes" -name '*.class') >/dev/null
[ -f "$BUILD/dex/classes.dex" ] || die "d8 produced no classes.dex"
cp "$BUILD/dex/classes.dex" "$BUILD/apk/classes.dex"
echo "ok  $(find "$BUILD/classes" -name '*.class' | wc -l) classes -> $(du -h "$BUILD/dex/classes.dex" | cut -f1) dex"

# --- 6. JNI agreement -----------------------------------------------------
# Java -> C. A `native` method with no matching export throws
# UnsatisfiedLinkError the first time it is called, not at load.
step "6  JNI native-method agreement"
grep -oP 'public static native \w+ \K\w+' "$JAVA_SRC/dev/vyto/android/Native.java" | sort > "$BUILD/declared.txt"
nm -D --defined-only "$so" | grep -oP 'Java_dev_vyto_android_Native_\K\w+' | sort > "$BUILD/impl.txt"
diff "$BUILD/declared.txt" "$BUILD/impl.txt" >/dev/null || {
    diff "$BUILD/declared.txt" "$BUILD/impl.txt"; die "native methods do not match .so exports"
}
echo "ok  $(wc -l < "$BUILD/declared.txt") native methods matched"

# --- 7. resources ---------------------------------------------------------
# Catches a manifest naming an icon or theme that was never compiled — which
# otherwise fails at *install* time, on a device, with a useless message.
step "7  resources (aapt2 compile + link)"
"$BT/aapt2" compile --dir "$APP/res" -o "$BUILD/res/res.zip"
"$BT/aapt2" link -o "$BUILD/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$BUILD/AndroidManifest.xml" \
    --min-sdk-version "$API" --target-sdk-version 34 \
    "$BUILD/res/res.zip"
[ -f "$BUILD/base.apk" ] || die "aapt2 link produced nothing"
echo "ok  base.apk linked, every @reference resolved"

# --- 8. package -----------------------------------------------------------
# aapt2 already emitted the resources and the binary manifest; add the dex and
# the .so. -0 .so keeps native libs stored, which is what extractNativeLibs=false
# in the manifest requires.
step "8  package (dex + .so into the apk)"
cp "$BUILD/base.apk" "$BUILD/unsigned.apk"
( cd "$BUILD/apk" && zip -q -X "../unsigned.apk" classes.dex && \
  zip -q -X -0 "../unsigned.apk" "lib/$ABI/lib$LIBNAME.so" )
echo "ok  $(unzip -l "$BUILD/unsigned.apk" | tail -1 | awk '{print $2}') entries"

# --- 9. align -------------------------------------------------------------
# -p page-aligns uncompressed .so files so they can be mapped straight out of
# the apk rather than extracted.
step "9  zipalign"
"$BT/zipalign" -p -f 4 "$BUILD/unsigned.apk" "$BUILD/aligned.apk"
"$BT/zipalign" -c -p 4 "$BUILD/aligned.apk" >/dev/null || die "alignment check failed"
echo "ok  page-aligned and verified"

# --- 10. sign -------------------------------------------------------------
step "10 sign (debug key)"
# Generated on first use and then kept. It lives OUTSIDE $BUILD, which step 0
# wipes: regenerating it per build gives every apk a different signing key, and
# `adb install -r` then fails with INSTALL_FAILED_UPDATE_INCOMPATIBLE — the
# device refuses an update signed by a different key, so every iteration would
# need an uninstall first.
#
# Never committed. Signing keys do not belong in a repository, and .gitignore
# carries a *.keystore rule for exactly this file.
KS="$APP/debug.keystore"
[ -f "$KS" ] || keytool -genkeypair -v -keystore "$KS" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Vyto Debug, OU=local, O=local, L=, S=, C=" >/dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
                --ks-key-alias androiddebugkey \
                --out "$BUILD/hn.apk" "$BUILD/aligned.apk"
"$BT/apksigner" verify --print-certs "$BUILD/hn.apk" > "$BUILD/certs.txt" 2>&1 \
    || { cat "$BUILD/certs.txt"; die "signature does not verify"; }
echo "ok  signed and verified"

# --- 11. badging ----------------------------------------------------------
# Reads the *linked* manifest back out of the apk, which is the only way to
# know the manifest that got packaged is the one manifest.vt produced.
step "11 badging (what actually shipped)"
"$BT/aapt2" dump badging "$BUILD/hn.apk" > "$BUILD/badging.txt"
grep -E "^package:|^application-label:|^minSdkVersion:|^targetSdkVersion:|^uses-permission:|^native-code:|^launchable-activity:" \
     "$BUILD/badging.txt"
grep -q "native-code: '$ABI'" "$BUILD/badging.txt" || die "apk carries no $ABI native code"
grep -q "uses-permission: name='android.permission.INTERNET'" "$BUILD/badging.txt" \
    || die "INTERNET permission did not survive into the apk"
grep -q "minSdkVersion:'$API'" "$BUILD/badging.txt" \
    || die "packaged minSdk is not $API — the linked manifest is not manifest.vt's"
grep -q "launchable-activity: name='dev.vyto.android.VytoActivity'" "$BUILD/badging.txt" \
    || die "no launchable activity — the app would install with no icon to tap"

# extractNativeLibs=false is a promise that the .so is STORED, not deflated:
# the loader maps it out of the apk instead of unpacking it. zip -0 above is
# what keeps that true, and unzip -v reports the method per entry.
unzip -v "$BUILD/hn.apk" | grep "lib/$ABI/lib$LIBNAME.so" | grep -q Stored \
    || die ".so is compressed — extractNativeLibs=false requires it stored"

printf '\n== apk: %s (%s)\n' "$BUILD/hn.apk" "$(du -h "$BUILD/hn.apk" | cut -f1)"
echo "   built and self-consistent. NOT installed, NOT run — no device involved."
