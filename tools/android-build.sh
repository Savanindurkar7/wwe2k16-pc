#!/usr/bin/env bash
# WWE 2K16 Android: native ARM64 build -> verify -> stage -> APK. Run after host codegen (generated/sources.cmake).
# Safe to re-run: every step is incremental or skips work that is already done. Log: out/android-build.log
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p out
exec > >(tee -a out/android-build.log) 2>&1
trap 'echo; echo "!!! FAILED at line $LINENO: $BASH_COMMAND"; echo "!!! Full log: $ROOT/out/android-build.log"' ERR

NDK_VER="28.2.13676358"
API=29
BUILD_DIR="$ROOT/sdk/rexglue/out/build/android-arm64"
step() { echo; echo "==== [$(date +%H:%M:%S)] $*"; }
die()  { echo "!!! $*"; exit 1; }

step "1/8 Checks"
[ -f generated/sources.cmake ] || die "generated/sources.cmake missing - host codegen has not run."

SDK="$ROOT/sdk/rexglue"
if [ ! -f "$SDK/src/core/fiber_android.cpp" ]; then
    SDK="$ROOT/sdk/rexglue-android"
    echo "sdk/rexglue has no Android sources; using $SDK (branch rexglue-android)"
    if [ ! -d "$SDK/.git" ]; then
        git clone --branch rexglue-android --single-branch https://github.com/Savanindurkar7/wwe2k16-pc.git "$SDK"
    fi
    git -C "$SDK" submodule update --init --recursive --depth 1 --jobs 4
fi
echo "SDK: $SDK ($(git -C "$SDK" rev-parse --short HEAD))"

step "2/8 Java and Android SDK"
if ! java -version 2>&1 | grep -qE 'version "(1[7-9]|2[0-9])'; then
    echo "Installing JDK 17"
    sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq openjdk-17-jdk-headless
    export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64 PATH="/usr/lib/jvm/java-17-openjdk-amd64/bin:$PATH"
fi
command -v unzip >/dev/null || sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq unzip
java -version 2>&1 | head -1
ASDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
for c in "$ASDK" "$HOME/Android/Sdk" /usr/local/lib/android/sdk /opt/android-sdk; do
    [ -n "$c" ] && [ -d "$c" ] && { ASDK="$c"; break; }
done
[ -n "$ASDK" ] && [ -d "$ASDK" ] || ASDK="$HOME/Android/Sdk"
SDKMANAGER="$(ls "$ASDK"/cmdline-tools/*/bin/sdkmanager 2>/dev/null | head -1 || true)"
if [ -z "$SDKMANAGER" ]; then
    echo "Installing Android command-line tools into $ASDK"
    mkdir -p "$ASDK/cmdline-tools" && tmp="$(mktemp -d)"
    curl -fL -o "$tmp/clt.zip" https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
    unzip -q "$tmp/clt.zip" -d "$tmp" && rm -rf "$ASDK/cmdline-tools/latest" && mv "$tmp/cmdline-tools" "$ASDK/cmdline-tools/latest"
    SDKMANAGER="$ASDK/cmdline-tools/latest/bin/sdkmanager"
fi
export ANDROID_HOME="$ASDK" ANDROID_SDK_ROOT="$ASDK"
yes | "$SDKMANAGER" --sdk_root="$ASDK" --licenses >/dev/null || true
need=()
[ -d "$ASDK/ndk/$NDK_VER" ]           || need+=("ndk;$NDK_VER")
[ -d "$ASDK/platforms/android-35" ]    || need+=("platforms;android-35")
[ -d "$ASDK/build-tools/35.0.0" ]      || need+=("build-tools;35.0.0")
[ -d "$ASDK/platform-tools" ]          || need+=("platform-tools")
if [ ${#need[@]} -gt 0 ]; then echo "Installing: ${need[*]}"; yes | "$SDKMANAGER" --sdk_root="$ASDK" "${need[@]}" >/dev/null; fi
NDK="$ASDK/ndk/$NDK_VER"
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || die "NDK toolchain not found in $NDK"
LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
echo "Android SDK: $ASDK   NDK: $NDK_VER"

step "3/8 Configure Android arm64-v8a (API $API)"
GEN=(); command -v ninja >/dev/null && GEN=(-G Ninja)
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && ! grep -q "REXSDK_DIR:PATH=$SDK\$" "$BUILD_DIR/CMakeCache.txt"; then
    echo "Build dir was configured for a different SDK path - starting it fresh"; rm -rf "$BUILD_DIR"
fi
[ -f "$BUILD_DIR/CMakeCache.txt" ] && GEN=()
cmake -S . -B "$BUILD_DIR" "${GEN[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
    -DCMAKE_BUILD_TYPE=Release -DREXSDK_DIR="$SDK" -DBUILD_TESTING=OFF

step "4/8 Build libmain.so (long: ~80k recompiled functions)"
MEM_GB=$(( $(awk '/MemAvailable/{print $2}' /proc/meminfo) / 1048576 ))
JOBS=$(nproc); [ $(( MEM_GB / 3 )) -lt "$JOBS" ] && JOBS=$(( MEM_GB / 3 )); [ "$JOBS" -lt 1 ] && JOBS=1
echo "Using $JOBS parallel jobs (${MEM_GB} GB RAM free, $(nproc) cores)"
cmake --build "$BUILD_DIR" --target wwe2k16 --parallel "$JOBS"

step "5/8 Verify native libraries"
MAIN="$(find "$BUILD_DIR" "$SDK/out/android-arm64" -name libmain.so -type f -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1 | cut -d' ' -f2- || true)"
[ -n "$MAIN" ] || die "libmain.so was not produced"
echo "libmain.so: $MAIN ($(du -h "$MAIN" | cut -f1))"
"$LLVM/llvm-readelf" -h "$MAIN" | grep -q "AArch64" || die "libmain.so is not AArch64"
EXPORTS="$("$LLVM/llvm-nm" -D --defined-only "$MAIN" | awk '{print $NF}')"
for sym in SDL_main JNI_OnLoad; do
    grep -qx "$sym" <<<"$EXPORTS" && echo "  exports $sym" || echo "  WARNING: $sym is not exported - SDL's Java side needs it at launch"
done
SYSTEM_LIBS="libc.so libm.so libdl.so liblog.so libandroid.so libvulkan.so libEGL.so libGLESv1_CM.so libGLESv2.so libGLESv3.so libOpenSLES.so libaaudio.so libmediandk.so libjnigraphics.so libz.so"
LIBS=("$MAIN"); queue=("$MAIN"); SEEN=" libmain.so "
while [ ${#queue[@]} -gt 0 ]; do
    lib="${queue[0]}"; queue=("${queue[@]:1}")
    for dep in $("$LLVM/llvm-readelf" -d "$lib" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        case " $SYSTEM_LIBS " in *" $dep "*) continue;; esac
        case "$SEEN" in *" $dep "*) continue;; esac
        SEEN+="$dep "
        found="$(find "$(dirname "$MAIN")" "$BUILD_DIR" "$SDK/out/android-arm64" -name "$dep" \( -type f -o -type l \) 2>/dev/null | head -1 || true)"
        [ -z "$found" ] && [ "$dep" = libc++_shared.so ] && found="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
        [ -n "$found" ] || die "$(basename "$lib") needs $dep, which was not built and is not an Android system library"
        found="$(readlink -f "$found")"; LIBS+=("$found"); queue+=("$found")
    done
done
for l in "${LIBS[@]}"; do echo "  ok  $(basename "$l")  NEEDED: $("$LLVM/llvm-readelf" -d "$l" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"; done

step "6/8 SDL Android project (WWE2K16Activity)"
AP="$SDK/thirdparty/sdl3/android-project"
ACT="$AP/app/src/main/java/org/libsdl/app/WWE2K16Activity.java"
if [ -f "$ACT" ] && grep -q "WWE2K16Activity" "$AP/app/src/main/AndroidManifest.xml"; then
    echo "Custom activity and manifest already in place - leaving them unchanged"
elif git -C "$SDK/thirdparty/sdl3" apply --check ../../android-sdl3-android.patch 2>/dev/null; then
    git -C "$SDK/thirdparty/sdl3" apply ../../android-sdl3-android.patch && echo "Applied android-sdl3-android.patch"
else
    die "SDL Android project is partly modified and the patch does not apply - check $AP by hand"
fi
grep -q 'return new String\[\] { "main" }' "$ACT" || die "WWE2K16Activity.getLibraries() does not load \"main\""

step "7/8 Stage libraries and build APK"
JNI="$AP/app/libs/arm64-v8a"
rm -rf "$JNI" && mkdir -p "$JNI"
for l in "${LIBS[@]}"; do
    name="$(basename "$l")"; [ "$l" = "$MAIN" ] && name=libmain.so
    soname="$("$LLVM/llvm-readelf" -d "$l" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')"
    [ -n "$soname" ] && name="$soname"
    cp "$l" "$JNI/$name"; echo "  staged $name"
done
echo "sdk.dir=$ASDK" > "$AP/local.properties"
(cd "$AP" && chmod +x gradlew && ./gradlew --no-daemon assembleDebug -PEXCLUDE_NATIVE_LIBS)

step "8/8 Verify APK"
APK_SRC="$AP/app/build/outputs/apk/debug/app-debug.apk"
[ -f "$APK_SRC" ] || die "Gradle finished but $APK_SRC is missing"
APK="$ROOT/out/wwe2k16-android-debug.apk"
cp "$APK_SRC" "$APK"
unzip -l "$APK" | awk '/lib\//{print "  " $4 "  (" $1 " bytes)"}'
for l in "$JNI"/*.so; do unzip -l "$APK" | grep -q "lib/arm64-v8a/$(basename "$l")" || die "$(basename "$l") is missing from the APK"; done
"$ASDK/build-tools/35.0.0/apksigner" verify --print-certs "$APK" | head -3
echo
echo "==== DONE. APK: $APK ($(du -h "$APK" | cut -f1))"
echo "Built and verified as a package only. Installing and launching on a device is the next, separate test."
