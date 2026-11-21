#!/usr/bin/env bash
# WWE 2K16 Android, NATIVE RENDERER build: the game on the WWE 2K14 SDK (ReXGlue v0.10.0 + the 2K14 port's 118 SDK
# patches) instead of this project's ReXGlue 0.7.
#
#   1  checks (generated_k14/ from tools/native-stage1.sh, the patched SDK, libadrenotools)
#   2  Java 17, Android SDK + NDK
#   3  the SDK for Android arm64 -> out/k14-android-sdk/       (first time: long; later: incremental)
#   4  the game (native/CMakeLists.txt) -> libmain.so           (first time: very long, ~80k recompiled functions)
#   5  verify the libraries (AArch64, exports, every dependency found)
#   6  the Android project (same launcher as the classic build, SDL's Java from the 0.10 SDK)
#   7  Gradle -> APK
#   8  verify the APK -> out/wwe2k16-native-debug.apk
#
# Same package and debug key as the classic APK (out/wwe2k16-android-debug.apk): it installs over it and keeps your
# settings, saves and drivers; install the classic APK again to go back. Separate build folders: the classic build is
# not touched. Safe to re-run. Log: out/android-build-k14.log
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p out
exec > >(tee -a out/android-build-k14.log) 2>&1
trap 'echo; echo "!!! FAILED at line $LINENO: $BASH_COMMAND"; echo "!!! Full log: $ROOT/out/android-build-k14.log"' ERR

NDK_VER="29.0.14206865"  # NDK 29: libc++ with float from_chars (the 0.10 SDK needs it)
API=29
SDK="$ROOT/sdk/rexglue-k14"
SDK_BUILD="$ROOT/out/build-k14-android-sdk"
SDK_INSTALL="$ROOT/out/k14-android-sdk"
GAME_BUILD="$ROOT/out/build-k14-android"
step() { echo; echo "==== [$(date +%H:%M:%S)] $*"; }
die()  { echo "!!! $*"; exit 1; }

step "1/8 Checks"
[ -f "$SDK/.wwe2k14-patched" ] || die "the WWE 2K14 SDK is not set up: run tools/native-stage1.sh first"
[ -f generated_k14/sources.cmake ] && [ -f generated_k14/wwe2k16_init.h ] \
    || die "generated_k14/ is missing or incomplete: run tools/native-stage1.sh first"
echo "SDK: $(git -C "$SDK" log -1 --format='%h %s') + $(cat "$SDK/.wwe2k14-patched")"
echo "generated_k14: $(ls generated_k14 | wc -l) files, $(du -sh generated_k14 | cut -f1)"
if [ ! -f thirdparty/libadrenotools/CMakeLists.txt ]; then
    mkdir -p thirdparty
    git clone --recursive --depth 1 https://github.com/bylaws/libadrenotools.git thirdparty/libadrenotools
fi
echo "libadrenotools: $(git -C thirdparty/libadrenotools rev-parse --short HEAD)"

step "2/8 Java and Android SDK"
JDK17=/usr/lib/jvm/java-17-openjdk-amd64
if [ ! -x "$JDK17/bin/java" ]; then
    sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq openjdk-17-jdk-headless
fi
[ -x "$JDK17/bin/java" ] || die "JDK 17 is not installed at $JDK17"
export JAVA_HOME="$JDK17" PATH="$JDK17/bin:$PATH"
command -v unzip >/dev/null || sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq unzip
command -v ninja >/dev/null || sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq ninja-build
ASDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
for c in "$ASDK" "$HOME/Android/Sdk" /usr/local/lib/android/sdk /opt/android-sdk; do
    [ -n "$c" ] && [ -d "$c" ] && { ASDK="$c"; break; }
done
[ -n "$ASDK" ] && [ -d "$ASDK" ] || ASDK="$HOME/Android/Sdk"
SDKMANAGER="$(ls "$ASDK"/cmdline-tools/*/bin/sdkmanager 2>/dev/null | head -1 || true)"
if [ -z "$SDKMANAGER" ]; then
    mkdir -p "$ASDK/cmdline-tools" && tmp="$(mktemp -d)"
    curl -fL -o "$tmp/clt.zip" https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
    unzip -q "$tmp/clt.zip" -d "$tmp" && rm -rf "$ASDK/cmdline-tools/latest" && mv "$tmp/cmdline-tools" "$ASDK/cmdline-tools/latest"
    SDKMANAGER="$ASDK/cmdline-tools/latest/bin/sdkmanager"
fi
export ANDROID_HOME="$ASDK" ANDROID_SDK_ROOT="$ASDK"
need=()
[ -d "$ASDK/ndk/$NDK_VER" ]        || need+=("ndk;$NDK_VER")
[ -d "$ASDK/platforms/android-35" ] || need+=("platforms;android-35")
[ -d "$ASDK/build-tools/35.0.0" ]   || need+=("build-tools;35.0.0")
[ -d "$ASDK/platform-tools" ]       || need+=("platform-tools")
if [ ${#need[@]} -gt 0 ]; then
    yes | "$SDKMANAGER" --sdk_root="$ASDK" --licenses >/dev/null 2>&1 || true
    yes | "$SDKMANAGER" --sdk_root="$ASDK" "${need[@]}" > out/sdkmanager.log 2>&1 || { tail -20 out/sdkmanager.log; die "sdkmanager failed"; }
fi
NDK="$ASDK/ndk/$NDK_VER"
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
[ -f "$TOOLCHAIN" ] || die "NDK toolchain not found in $NDK"
LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
echo "Android SDK: $ASDK   NDK: $NDK_VER   Java: $(java -version 2>&1 | head -1)"

MEM_GB=$(( $(awk '/MemAvailable/{print $2}' /proc/meminfo) / 1048576 ))
JOBS=$(nproc); [ $(( MEM_GB / 3 )) -lt "$JOBS" ] && JOBS=$(( MEM_GB / 3 )); [ "$JOBS" -lt 1 ] && JOBS=1
echo "Using $JOBS parallel jobs (${MEM_GB} GB RAM free, $(nproc) cores, $(df -h --output=avail "$ROOT" | tail -1 | tr -d ' ') disk free)"

step "3/8 The WWE 2K14 SDK for Android arm64 (-> out/k14-android-sdk)"
# As the 2K14 port builds it (tools/android/build_sdk_android.ps1): shared libc++, ARMv8.2, no Tracy / FidelityFX / tests.
if [ ! -f "$SDK_BUILD/CMakeCache.txt" ]; then
    cmake -S "$SDK" -B "$SDK_BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
        -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SDK_INSTALL" \
        -DCMAKE_C_FLAGS=-march=armv8.2-a -DCMAKE_CXX_FLAGS=-march=armv8.2-a \
        -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FIDELITYFX=OFF -DREXGLUE_BUILD_TESTS=OFF \
        -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF > out/k14-android-sdk-configure.log 2>&1 \
        || { tail -40 out/k14-android-sdk-configure.log; rm -rf "$SDK_BUILD"; die "SDK configure failed (out/k14-android-sdk-configure.log)"; }
fi
cmake --build "$SDK_BUILD" --target install --parallel "$JOBS"
for so in librexruntime.so librexgpu-xenos.so; do
    [ -f "$SDK_INSTALL/lib/$so" ] || die "$so is not in $SDK_INSTALL/lib"
    echo "  $so ($(du -h "$SDK_INSTALL/lib/$so" | cut -f1))"
done

step "4/8 The game: libmain.so (native/CMakeLists.txt)"
if [ ! -f "$GAME_BUILD/CMakeCache.txt" ]; then
    cmake -S native -B "$GAME_BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
        -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS=-march=armv8.2-a -DCMAKE_CXX_FLAGS=-march=armv8.2-a \
        -DCMAKE_PREFIX_PATH="$SDK_INSTALL" -DCMAKE_FIND_ROOT_PATH="$SDK_INSTALL" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
        > out/k14-android-game-configure.log 2>&1 \
        || { tail -40 out/k14-android-game-configure.log; rm -rf "$GAME_BUILD"; die "game configure failed (out/k14-android-game-configure.log)"; }
fi
cmake --build "$GAME_BUILD" --target wwe2k16 --parallel "$JOBS"

step "5/8 Verify native libraries"
MAIN="$(find "$GAME_BUILD" -name libmain.so -type f -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1 | cut -d' ' -f2- || true)"
[ -n "$MAIN" ] || die "libmain.so was not produced"
echo "libmain.so: $MAIN ($(du -h "$MAIN" | cut -f1))"
grep -q AArch64 <<<"$("$LLVM/llvm-readelf" -h "$MAIN")" || die "libmain.so is not AArch64"
EXPORTS="$("$LLVM/llvm-nm" -D --defined-only "$MAIN" | awk '{print $NF}')"
for sym in SDL_main Java_com_savanindurkar7_wwe2k16_TouchControls_nativeSetPad; do
    grep -qx "$sym" <<<"$EXPORTS" && echo "  libmain.so exports $sym" || echo "  WARNING: libmain.so does not export $sym"
done
# SDL (and its JNI_OnLoad) is inside the SDK runtime in this build.
grep -qx JNI_OnLoad <<<"$("$LLVM/llvm-nm" -D --defined-only "$SDK_INSTALL/lib/librexruntime.so" | awk '{print $NF}')" \
    && echo "  librexruntime.so exports JNI_OnLoad (SDL)" || echo "  WARNING: librexruntime.so does not export JNI_OnLoad"
SYSTEM_LIBS="libc.so libm.so libdl.so liblog.so libandroid.so libvulkan.so libEGL.so libGLESv1_CM.so libGLESv2.so libGLESv3.so libOpenSLES.so libaaudio.so libmediandk.so libjnigraphics.so libz.so libnativewindow.so libsync.so"
LIBS=("$MAIN" "$SDK_INSTALL/lib/librexgpu-xenos.so"); queue=("${LIBS[@]}"); SEEN=" libmain.so librexgpu-xenos.so "
while [ ${#queue[@]} -gt 0 ]; do
    lib="${queue[0]}"; queue=("${queue[@]:1}")
    for dep in $("$LLVM/llvm-readelf" -d "$lib" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        case " $SYSTEM_LIBS " in *" $dep "*) continue;; esac
        case "$SEEN" in *" $dep "*) continue;; esac
        SEEN+="$dep "
        found="$(find "$SDK_INSTALL/lib" "$GAME_BUILD" -name "$dep" \( -type f -o -type l \) 2>/dev/null | head -1 || true)"
        [ -z "$found" ] && [ "$dep" = libc++_shared.so ] && found="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
        [ -n "$found" ] || die "$(basename "$lib") needs $dep, which was not built and is not an Android system library"
        found="$(readlink -f "$found")"; LIBS+=("$found"); queue+=("$found")
    done
done
for hook in libhook_impl.so libmain_hook.so; do
    h="$(find "$GAME_BUILD" -name "$hook" -type f 2>/dev/null | head -1 || true)"
    if [ -n "$h" ]; then LIBS+=("$h"); else echo "  WARNING: $hook not built - custom GPU drivers will fall back to the phone's"; fi
done
for l in "${LIBS[@]}"; do echo "  ok  $(basename "$l")  NEEDED: $("$LLVM/llvm-readelf" -d "$l" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"; done

step "6/8 Android project"
AP="$ROOT/out/android-project-k14"
SDL_AP="$SDK/thirdparty/sdl3/android-project"   # SDL's Java must match the SDL inside librexruntime.so
[ -d "$SDL_AP" ] || die "$SDL_AP is missing (git -C $SDK submodule update --init --recursive)"
rm -rf "$AP" && mkdir -p "$AP"
(cd "$SDL_AP" && tar cf - --exclude=build --exclude=.gradle --exclude=.cxx --exclude='app/libs' --exclude='app/jni' \
    --exclude='local.properties' .) | (cd "$AP" && tar xf -)
cp "$ROOT/android/AndroidManifest.xml" "$AP/app/src/main/AndroidManifest.xml"
cp "$ROOT/android/build.gradle" "$AP/app/build.gradle"
mkdir -p "$AP/app/src/main/java" && cp -r "$ROOT/android/java/." "$AP/app/src/main/java/"
cp -r "$ROOT/android/res/." "$AP/app/src/main/res/"
mkdir -p "$AP/app/src/main/assets" && cp "$ROOT/android/licenses/"* "$AP/app/src/main/assets/"
cp thirdparty/libadrenotools/LICENSE "$AP/app/src/main/assets/LICENSE-libadrenotools.txt" 2>/dev/null || true
cp thirdparty/wwe2k14-recomp/LICENSE "$AP/app/src/main/assets/LICENSE-wwe2k14-recomp.txt" 2>/dev/null || true
# Marks this APK as the native renderer build (the launcher reads it: Options > Renderer).
echo "k14" > "$AP/app/src/main/assets/sdk.txt"
if [ -f "$ROOT/BG.jpg" ]; then
    mkdir -p "$AP/app/src/main/res/drawable-nodpi" && cp "$ROOT/BG.jpg" "$AP/app/src/main/res/drawable-nodpi/home_bg.jpg"
fi
if [ -f "$ROOT/APP_ICON.jpg" ]; then
    python3 -c "import PIL" 2>/dev/null || python3 -m pip install --user -q pillow 2>/dev/null || python3 -m pip install --user -q --break-system-packages pillow
    python3 - "$ROOT/APP_ICON.jpg" "$AP/app/src/main/res" <<'PY'
import sys, os
from PIL import Image
src, res = sys.argv[1], sys.argv[2]
im = Image.open(src).convert("RGB")
w, h = im.size
side = min(w, h)
im = im.crop(((w - side) // 2, 0, (w - side) // 2 + side, side))
for folder, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
    d = os.path.join(res, "mipmap-" + folder)
    os.makedirs(d, exist_ok=True)
    im.resize((size, size), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
PY
fi
JNI="$AP/app/libs/arm64-v8a"
mkdir -p "$JNI"
for l in "${LIBS[@]}"; do
    name="$(basename "$l")"; [ "$l" = "$MAIN" ] && name=libmain.so
    soname="$("$LLVM/llvm-readelf" -d "$l" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')"
    [ -n "$soname" ] && name="$soname"
    cp "$l" "$JNI/$name"; echo "  staged $name"
done
# Debug info out of the APK (the unstripped libraries stay in the build folders, for ndk-stack).
for l in "$JNI"/*.so; do "$LLVM/llvm-strip" --strip-debug "$l"; done
echo "sdk.dir=$ASDK" > "$AP/local.properties"

step "7/8 Build APK"
VCODE="$(git rev-list --count HEAD 2>/dev/null || echo 1)"
VNAME="native-$(git rev-parse --short HEAD 2>/dev/null || echo dev)"
git diff --quiet 2>/dev/null || VNAME="$VNAME+"
(cd "$AP" && chmod +x gradlew && ./gradlew --no-daemon -q assembleDebug -PWWE2K16_VERSION_CODE="$VCODE" -PWWE2K16_VERSION_NAME="$VNAME")

step "8/8 Verify APK"
APK_SRC="$AP/app/build/outputs/apk/debug/app-debug.apk"
[ -f "$APK_SRC" ] || die "Gradle finished but $APK_SRC is missing"
APK="$ROOT/out/wwe2k16-native-debug.apk"
cp "$APK_SRC" "$APK"
LISTING="$(unzip -l "$APK")"
awk '/lib\//{print "  " $4 "  (" $1 " bytes)"}' <<<"$LISTING"
for l in "$JNI"/*.so; do grep -q "lib/arm64-v8a/$(basename "$l")" <<<"$LISTING" || die "$(basename "$l") is missing from the APK"; done
grep -q "assets/sdk.txt" <<<"$LISTING" || die "assets/sdk.txt is missing from the APK"
"$ASDK/build-tools/35.0.0/aapt2" dump badging "$APK" 2>/dev/null | grep -E "^package:|^application-label:" | sed 's/^/  /' || true
"$ASDK/build-tools/35.0.0/apksigner" verify "$APK" && echo "  signature ok"
echo
echo "==== DONE. APK: $APK ($(du -h "$APK" | cut -f1)), version $VNAME ($VCODE)"
echo "In the game: Options > Renderer: Classic / Native / Native+ (Native+ is the fastest, the least tested)."
