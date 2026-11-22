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
# This project's own SDK changes on top (patches/k14-sdk: frame generation, ...): applied once; a patch already in
# place is left alone.
# Patches build on each other (0002 changes lines of 0001), so "already applied" is decided from the newest one back:
# the newest patch that reverses cleanly is in place, and so is every patch before it.
PATCHES=("$ROOT"/patches/k14-sdk/*.patch)
applied_upto=-1
for ((i=${#PATCHES[@]}-1; i>=0; i--)); do
    [ -f "${PATCHES[$i]}" ] || continue
    if git -C "$SDK" apply --check -R "${PATCHES[$i]}" 2>/dev/null; then applied_upto=$i; break; fi
done
for ((i=0; i<${#PATCHES[@]}; i++)); do
    p="${PATCHES[$i]}"
    [ -f "$p" ] || continue
    if [ "$i" -le "$applied_upto" ]; then
        echo "already applied: $(basename "$p")"
    elif git -C "$SDK" apply --check "$p" 2>/dev/null; then
        git -C "$SDK" apply "$p" && echo "applied $(basename "$p")"
    else
        die "$(basename "$p") neither applies nor is already applied to $SDK"
    fi
done
K14_PATCHES_ID="$(cat "$ROOT"/patches/k14-sdk/*.patch 2>/dev/null | sha1sum | cut -c1-12)"
echo "generated_k14: $(ls generated_k14 | wc -l) files, $(du -sh generated_k14 | cut -f1)"
# The game's scene without MSAA (src/k14_tiling_hooks.h): find the scene-target builder in generated_k14/ and hook it
# (once; only the one generated file it is in rebuilds). Not found = the build goes on, MSAA OFF just does nothing.
python3 tools/k14-tiling-hooks.py --apply || echo "(tiling hook finder failed: the build goes on without it)"
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
[ -f "$ASDK/ndk/$NDK_VER/build/cmake/android.toolchain.cmake" ] || { rm -rf "$ASDK/ndk/$NDK_VER"; need+=("ndk;$NDK_VER"); }
[ -d "$ASDK/platforms/android-35" ] || need+=("platforms;android-35")
[ -d "$ASDK/build-tools/35.0.0" ]   || need+=("build-tools;35.0.0")
[ -d "$ASDK/platform-tools" ]       || need+=("platform-tools")
if [ ${#need[@]} -gt 0 ]; then
    yes | "$SDKMANAGER" --sdk_root="$ASDK" --licenses >/dev/null 2>&1 || true
    yes | "$SDKMANAGER" --sdk_root="$ASDK" "${need[@]}" > out/sdkmanager.log 2>&1 || true
fi
NDK="$ASDK/ndk/$NDK_VER"
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
[ -f "$TOOLCHAIN" ] || die "NDK toolchain not found in $NDK"
LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
echo "Android SDK: $ASDK   NDK: $NDK_VER   Java: $(java -version 2>&1 | head -1)"

MEM_GB=$(( $(awk '/MemAvailable/{print $2}' /proc/meminfo) / 1048576 ))
JOBS=$(nproc); [ $(( MEM_GB / 3 )) -lt "$JOBS" ] && JOBS=$(( MEM_GB / 3 )); [ "$JOBS" -lt 1 ] && JOBS=1
echo "Using $JOBS parallel jobs (${MEM_GB} GB RAM free, $(nproc) cores, $(df -h --output=avail "$ROOT" | tail -1 | tr -d ' ') disk free)"

# Small disks (a 32 GB Codespace holds the NDK, two SDKs and the game): no debug info (-g0, about a third of the object
# size), and the SDK's build folder is deleted once the SDK is installed (KEEP_SDK_BUILD=1 keeps it). The libraries in
# the APK are the same; only crash stacks lose line numbers.
# CPU target (CPU_TUNE): "new" (default since r16) = the ARMv8.2 instructions every phone from about 2019 has (dot
# product, half-precision floats, RCpc loads), with the code scheduled for Cortex-A720 / A715-class big cores
# (Snapdragon 7s Gen 3, 8 Gen 2 / 3 and similar): the game's threads and the GPU command thread run a little faster.
# "generic" = plain ARMv8.2 (the r1-r15 builds), for older phones.
case "${CPU_TUNE:-new}" in
    generic) FLAGS="-march=armv8.2-a -g0" ;;
    *)       FLAGS="-march=armv8.2-a+dotprod+fp16+rcpc -mtune=cortex-a720 -g0" ;;
esac
echo "CPU target: ${CPU_TUNE:-new} ($FLAGS)"
SDK_STAMP="$SDK_INSTALL/.built-from"
SDK_ID="$(git -C "$SDK" rev-parse HEAD) $(cat "$SDK/.wwe2k14-patched") ndk $NDK_VER k14-sdk $K14_PATCHES_ID flags $FLAGS"
free_gb() { df -Pk "$ROOT" | awk 'NR==2{print int($4/1048576)}'; }

step "3/8 The WWE 2K14 SDK for Android arm64 (-> out/k14-android-sdk)"
# As the 2K14 port builds it (tools/android/build_sdk_android.ps1): shared libc++, ARMv8.2, no Tracy / FidelityFX / tests.
echo "disk free: $(free_gb) GB"
if [ -f "$SDK_STAMP" ] && [ "$(cat "$SDK_STAMP")" = "$SDK_ID" ] && [ -f "$SDK_INSTALL/lib/librexruntime.so" ] \
    && [ -f "$SDK_INSTALL/lib/librexgpu-xenos.so" ]; then
    echo "already built and installed ($SDK_INSTALL): skipped"
else
[ "$(free_gb)" -ge 4 ] || die "only $(free_gb) GB free: the SDK build needs about 4 GB (see the clean-up command in the chat / docs/ANDROID_NATIVE.md)"
if [ ! -f "$SDK_BUILD/CMakeCache.txt" ]; then
    cmake -S "$SDK" -B "$SDK_BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
        -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SDK_INSTALL" \
        -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
        -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FIDELITYFX=OFF -DREXGLUE_BUILD_TESTS=OFF \
        -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF > out/k14-android-sdk-configure.log 2>&1 \
        || { tail -40 out/k14-android-sdk-configure.log; rm -rf "$SDK_BUILD"; die "SDK configure failed (out/k14-android-sdk-configure.log)"; }
fi
cmake --build "$SDK_BUILD" --target install --parallel "$JOBS"
echo "$SDK_ID" > "$SDK_STAMP"
# The build folder makes the next SDK change (patches/k14-sdk) an incremental build: kept while there is room
# (KEEP_SDK_BUILD=1 always keeps it, =0 always deletes it).
if [ "${KEEP_SDK_BUILD:-auto}" = 0 ] || { [ "${KEEP_SDK_BUILD:-auto}" = auto ] && [ "$(free_gb)" -lt 10 ]; }; then
    rm -rf "$SDK_BUILD"; echo "SDK installed; its build folder deleted to save space (now $(free_gb) GB free)"
else
    echo "SDK installed; build folder kept for incremental SDK builds ($(free_gb) GB free)"
fi
fi
for so in librexruntime.so librexgpu-xenos.so; do
    [ -f "$SDK_INSTALL/lib/$so" ] || die "$so is not in $SDK_INSTALL/lib"
    echo "  $so ($(du -h "$SDK_INSTALL/lib/$so" | cut -f1))"
done

step "4/8 The game: libmain.so (native/CMakeLists.txt)"
# Compiler flags changed (CPU_TUNE): configure again (a full rebuild of the game's code, once).
if [ -f "$GAME_BUILD/CMakeCache.txt" ] && [ "$(cat "$GAME_BUILD/.flags" 2>/dev/null)" != "$FLAGS" ]; then
    echo "compiler flags changed: the game's code is built again (once)"
    rm -f "$GAME_BUILD/CMakeCache.txt"
fi
if [ ! -f "$GAME_BUILD/CMakeCache.txt" ]; then
    cmake -S native -B "$GAME_BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
        -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
        -DCMAKE_PREFIX_PATH="$SDK_INSTALL" -DCMAKE_FIND_ROOT_PATH="$SDK_INSTALL" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
        > out/k14-android-game-configure.log 2>&1 \
        || { tail -40 out/k14-android-game-configure.log; rm -rf "$GAME_BUILD"; die "game configure failed (out/k14-android-game-configure.log)"; }
fi
echo "disk free: $(free_gb) GB (the game's objects take about 4-6 GB)"
cmake --build "$GAME_BUILD" --target wwe2k16 --parallel "$JOBS"
echo "$FLAGS" > "$GAME_BUILD/.flags"

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
# Built-in GPU drivers (r17): Mesa Turnip for Adreno, malisx2 (PanVK) for Mali, from their GitHub releases.
bash "$ROOT/tools/fetch-drivers.sh" "$AP/app/src/main/assets" || echo "  WARNING: built-in drivers not staged"
# Marks this APK as the native renderer build (the launcher reads it: Options > Renderer).
echo "k14" > "$AP/app/src/main/assets/sdk.txt"
# Home screen art: the repository's BG1.png, BG2.jpg, BG3.jpg and BG34-.jpg as home_bg1..4 (JPEG, at most 1920 wide,
# so the APK stays small); the old single BG.jpg is no longer used.
rm -f "$AP/app/src/main/res/drawable-nodpi/home_bg."* "$AP/app/src/main/res/drawable-nodpi/home_bg"[0-9].*
mkdir -p "$AP/app/src/main/res/drawable-nodpi"
python3 -c "import PIL" 2>/dev/null || python3 -m pip install --user -q pillow 2>/dev/null || python3 -m pip install --user -q --break-system-packages pillow 2>/dev/null || true
n=0
for pair in "BG1.png:home_bg1" "BG2.jpg:home_bg2" "BG3.jpg:home_bg3" "BG34-.jpg:home_bg4"; do
    src="$ROOT/${pair%%:*}"; dst="$AP/app/src/main/res/drawable-nodpi/${pair##*:}.jpg"
    [ -f "$src" ] || continue
    python3 - "$src" "$dst" <<'PY' 2>/dev/null || cp "$src" "${dst%.jpg}.${src##*.}"
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
if im.width > 1920:
    im = im.resize((1920, round(im.height * 1920 / im.width)), Image.LANCZOS)
im.save(sys.argv[2], "JPEG", quality=88, optimize=True)
PY
    n=$((n + 1))
done
echo "home screen art: $n picture(s)"
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
echo "  built-in drivers: $(grep -c "assets/drivers/.*\.zip" <<<"$LISTING" || true)"
"$ASDK/build-tools/35.0.0/aapt2" dump badging "$APK" 2>/dev/null | grep -E "^package:|^application-label:" | sed 's/^/  /' || true
"$ASDK/build-tools/35.0.0/apksigner" verify "$APK" && echo "  signature ok"
echo
echo "==== DONE. APK: $APK ($(du -h "$APK" | cut -f1)), version $VNAME ($VCODE)"
echo "In the game: Options > Renderer: Classic / Native / Native+ (Native+ is the fastest, the least tested)."

# Termux: the command that brings this APK to the phone (the Codespace's own name, from GitHub's environment).
CS="${CODESPACE_NAME:-YOUR-CODESPACE-NAME}"
echo
echo "==== Termux (on the phone): download the APK into Download/, then stop the Codespace:"
echo "gh codespace cp -e -c $CS remote:$APK ~/storage/downloads/wwe2k16-${VNAME//+/}-${VCODE}.apk && gh codespace stop -c $CS"
