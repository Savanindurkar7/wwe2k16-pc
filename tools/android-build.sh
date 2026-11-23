#!/usr/bin/env bash
# WWE 2K16 Android: SDK patches -> native ARM64 build -> verify -> Android project (launcher + game) -> APK.
# Run after host codegen (generated/sources.cmake). Safe to re-run: every step is incremental or skips finished work.
# Log: out/android-build.log
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

step "1/9 Checks and SDK"
[ -f generated/sources.cmake ] || die "generated/sources.cmake missing - host codegen has not run."
# The Android sources live on the rexglue-android branch. Use sdk/rexglue if it has them; otherwise keep it untouched
# (it holds the host build) and use a separate checkout of that branch.
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
# This project's SDK changes (patches/android-sdk): applied once; a patch already in place is left alone.
for p in "$ROOT"/patches/android-sdk/*.patch; do
    [ -f "$p" ] || continue
    if git -C "$SDK" apply --check "$p" 2>/dev/null; then
        git -C "$SDK" apply "$p" && echo "applied $(basename "$p")"
    elif git -C "$SDK" apply --check -R "$p" 2>/dev/null; then
        echo "already applied: $(basename "$p")"
    else
        die "$(basename "$p") neither applies nor is already applied: the SDK checkout has other changes in those files"
    fi
done
# libadrenotools (custom GPU drivers, BSD-2-Clause).
if [ ! -f thirdparty/libadrenotools/CMakeLists.txt ]; then
    mkdir -p thirdparty
    git clone --recursive --depth 1 https://github.com/bylaws/libadrenotools.git thirdparty/libadrenotools
fi
echo "libadrenotools: $(git -C thirdparty/libadrenotools rev-parse --short HEAD)"

step "2/9 Java and Android SDK"
# Gradle 8.12 runs on Java 17 to 23 (newer Java fails with "Unsupported class file major version"): use Java 17.
JDK17=/usr/lib/jvm/java-17-openjdk-amd64
if [ ! -x "$JDK17/bin/java" ]; then
    echo "Installing JDK 17"
    sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq openjdk-17-jdk-headless
fi
[ -x "$JDK17/bin/java" ] || die "JDK 17 is not installed at $JDK17"
export JAVA_HOME="$JDK17" PATH="$JDK17/bin:$PATH"
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
need=()
[ -d "$ASDK/ndk/$NDK_VER" ]           || need+=("ndk;$NDK_VER")
[ -d "$ASDK/platforms/android-35" ]    || need+=("platforms;android-35")
[ -d "$ASDK/build-tools/35.0.0" ]      || need+=("build-tools;35.0.0")
[ -d "$ASDK/platform-tools" ]          || need+=("platform-tools")
if [ ${#need[@]} -gt 0 ]; then
    echo "Installing: ${need[*]}"
    yes | "$SDKMANAGER" --sdk_root="$ASDK" --licenses >/dev/null 2>&1 || true
    yes | "$SDKMANAGER" --sdk_root="$ASDK" "${need[@]}" > out/sdkmanager.log 2>&1 || { tail -20 out/sdkmanager.log; die "sdkmanager failed (out/sdkmanager.log)"; }
fi
NDK="$ASDK/ndk/$NDK_VER"
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || die "NDK toolchain not found in $NDK"
LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
echo "Android SDK: $ASDK   NDK: $NDK_VER"

step "3/9 Configure Android arm64-v8a (API $API)"
GEN=(); command -v ninja >/dev/null && GEN=(-G Ninja)
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && ! grep -q "REXSDK_DIR:PATH=$SDK\$" "$BUILD_DIR/CMakeCache.txt"; then
    echo "Build dir was configured for a different SDK path - starting it fresh"; rm -rf "$BUILD_DIR"
fi
[ -f "$BUILD_DIR/CMakeCache.txt" ] && GEN=()   # keep the generator an existing build dir already uses
cmake -S . -B "$BUILD_DIR" "${GEN[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
    -DCMAKE_BUILD_TYPE=Release -DREXSDK_DIR="$SDK" -DBUILD_TESTING=OFF

step "4/9 Build libmain.so (first build is long: ~80k recompiled functions)"
MEM_GB=$(( $(awk '/MemAvailable/{print $2}' /proc/meminfo) / 1048576 ))
JOBS=$(nproc); [ $(( MEM_GB / 3 )) -lt "$JOBS" ] && JOBS=$(( MEM_GB / 3 )); [ "$JOBS" -lt 1 ] && JOBS=1
echo "Using $JOBS parallel jobs (${MEM_GB} GB RAM free, $(nproc) cores, $(df -h --output=avail "$ROOT" | tail -1 | tr -d ' ') disk free)"
cmake --build "$BUILD_DIR" --target wwe2k16 --parallel "$JOBS"

step "5/9 Verify native libraries"
MAIN="$(find "$BUILD_DIR" "$SDK/out/android-arm64" -name libmain.so -type f -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1 | cut -d' ' -f2- || true)"
[ -n "$MAIN" ] || die "libmain.so was not produced"
echo "libmain.so: $MAIN ($(du -h "$MAIN" | cut -f1))"
grep -q AArch64 <<<"$("$LLVM/llvm-readelf" -h "$MAIN")" || die "libmain.so is not AArch64"
EXPORTS="$("$LLVM/llvm-nm" -D --defined-only "$MAIN" | awk '{print $NF}')"
for sym in SDL_main JNI_OnLoad; do
    grep -qx "$sym" <<<"$EXPORTS" && echo "  exports $sym" || echo "  WARNING: $sym is not exported - SDL's Java side needs it at launch"
done
SYSTEM_LIBS="libc.so libm.so libdl.so liblog.so libandroid.so libvulkan.so libEGL.so libGLESv1_CM.so libGLESv2.so libGLESv3.so libOpenSLES.so libaaudio.so libmediandk.so libjnigraphics.so libz.so libnativewindow.so libsync.so"
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
# libadrenotools' hook libraries: loaded by path at run time, not linked.
for hook in libhook_impl.so libmain_hook.so; do
    h="$(find "$BUILD_DIR" -name "$hook" -type f 2>/dev/null | head -1 || true)"
    if [ -n "$h" ]; then LIBS+=("$h"); else echo "  WARNING: $hook not built - custom GPU drivers will fall back to the phone's"; fi
done
for l in "${LIBS[@]}"; do echo "  ok  $(basename "$l")  NEEDED: $("$LLVM/llvm-readelf" -d "$l" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"; done

step "6/9 Android project (launcher + game)"
AP="$ROOT/out/android-project"
SDL_AP="$SDK/thirdparty/sdl3/android-project"
rm -rf "$AP" && mkdir -p "$AP"
# SDL's Gradle project and Java sources (they must match the SDL inside libmain.so: same checkout), nothing built.
(cd "$SDL_AP" && tar cf - --exclude=build --exclude=.gradle --exclude=.cxx --exclude='app/libs' --exclude='app/jni' \
    --exclude='local.properties' .) | (cd "$AP" && tar xf -)
# GameActivity replaces the earlier WWE2K16Activity test class.
rm -f "$AP/app/src/main/java/org/libsdl/app/WWE2K16Activity.java"
cp "$ROOT/android/AndroidManifest.xml" "$AP/app/src/main/AndroidManifest.xml"
cp "$ROOT/android/build.gradle" "$AP/app/build.gradle"
mkdir -p "$AP/app/src/main/java" && cp -r "$ROOT/android/java/." "$AP/app/src/main/java/"
cp -r "$ROOT/android/res/." "$AP/app/src/main/res/"
mkdir -p "$AP/app/src/main/assets" && cp "$ROOT/android/licenses/"* "$AP/app/src/main/assets/"
cp thirdparty/libadrenotools/LICENSE "$AP/app/src/main/assets/LICENSE-libadrenotools.txt" 2>/dev/null || true
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
# Square crop, centred across and taken from the top.
im = im.crop(((w - side) // 2, 0, (w - side) // 2 + side, side))
for folder, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
    d = os.path.join(res, "mipmap-" + folder)
    os.makedirs(d, exist_ok=True)
    im.resize((size, size), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
print("app icon: APP_ICON.jpg -> 48..192 px")
PY
else
    echo "  (no APP_ICON.jpg: SDL's default icon)"
fi
# Every native library next to the app.
JNI="$AP/app/libs/arm64-v8a"
mkdir -p "$JNI"
for l in "${LIBS[@]}"; do
    name="$(basename "$l")"; [ "$l" = "$MAIN" ] && name=libmain.so
    soname="$("$LLVM/llvm-readelf" -d "$l" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')"
    [ -n "$soname" ] && name="$soname"
    cp "$l" "$JNI/$name"; echo "  staged $name"
done
echo "sdk.dir=$ASDK" > "$AP/local.properties"

step "7/9 Build APK"
VCODE="$(git rev-list --count HEAD 2>/dev/null || echo 1)"
RELEASE="${WWE2K16_RELEASE:-1.2.0}"   # r18: real version in App Info; git hash kept as the build id
VNAME="$RELEASE"
BUILD_ID="$(git rev-parse --short HEAD 2>/dev/null || echo dev)"
git diff --quiet 2>/dev/null || BUILD_ID="$BUILD_ID+"
(cd "$AP" && chmod +x gradlew && ./gradlew --no-daemon -q assembleDebug -PWWE2K16_VERSION_CODE="$VCODE" -PWWE2K16_VERSION_NAME="$VNAME" -PWWE2K16_BUILD_ID="$BUILD_ID")

step "8/9 Verify APK"
APK_SRC="$AP/app/build/outputs/apk/debug/app-debug.apk"
[ -f "$APK_SRC" ] || die "Gradle finished but $APK_SRC is missing"
APK="$ROOT/out/wwe2k16-android-debug.apk"
cp "$APK_SRC" "$APK"
unzip -l "$APK" | awk '/lib\//{print "  " $4 "  (" $1 " bytes)"}'
for l in "$JNI"/*.so; do grep -q "lib/arm64-v8a/$(basename "$l")" <<<"$(unzip -l "$APK")" || die "$(basename "$l") is missing from the APK"; done
"$ASDK/build-tools/35.0.0/aapt2" dump badging "$APK" 2>/dev/null | grep -E "^package:|^application-label:|launchable-activity" | sed 's/^/  /' || true
"$ASDK/build-tools/35.0.0/apksigner" verify "$APK" && echo "  signature ok"

step "9/9 Done"
echo "APK: $APK ($(du -h "$APK" | cut -f1)), version $VNAME ($VCODE), build $BUILD_ID"
echo "Package com.savanindurkar7.wwe2k16: it installs next to the earlier test APK (org.libsdl.app), which you can uninstall."
echo "Built and verified as a package only. Installing and playing on the phone is the real test."

# Termux: the command that brings this APK to the phone (the Codespace's own name, from GitHub's environment).
CS="${CODESPACE_NAME:-YOUR-CODESPACE-NAME}"
echo
echo "==== Termux (on the phone): download the APK into Download/, then stop the Codespace:"
echo "gh codespace cp -e -c $CS remote:$APK ~/storage/downloads/wwe2k16-${VNAME//+/}-${VCODE}.apk && gh codespace stop -c $CS"
