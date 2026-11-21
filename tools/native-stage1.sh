#!/usr/bin/env bash
# Native renderer, stage 1: does WWE 2K16 recompile with the WWE 2K14 SDK (ReXGlue v0.10.0 + the 2K14 port's 118 SDK
# patches: its Android support and the native renderer's GPU side)?
#
# Works in separate folders only; the current Android build (sdk/rexglue*, generated/) is not touched:
#   sdk/rexglue-k14/            ReXGlue v0.10.0 + 2K14 patches
#   thirdparty/wwe2k14-recomp/  the 2K14 port (BSD-3), for its patch series
#   out/build-k14-host/         the host build of that SDK
#   generated_k14/              WWE 2K16 recompiled with it
# Log: out/native-stage1.log
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p out
exec > >(tee -a out/native-stage1.log) 2>&1
trap 'echo; echo "!!! FAILED at line $LINENO: $BASH_COMMAND"; echo "!!! Log: $ROOT/out/native-stage1.log"' ERR
step() { echo; echo "==== [$(date +%H:%M:%S)] $*"; }
die()  { echo "!!! $*"; exit 1; }

SDK="$ROOT/sdk/rexglue-k14"
K14="$ROOT/thirdparty/wwe2k14-recomp"
BUILD="$ROOT/out/build-k14-host"

step "1/4 ReXGlue v0.10.0"
[ -f game/default.xex ] || die "game/default.xex not found: the code generator needs your game files there (as for the first build)"
if [ ! -d "$SDK/.git" ]; then
    git clone https://github.com/rexglue/rexglue-sdk.git "$SDK"
    git -C "$SDK" checkout -q c94f5eb
    git -C "$SDK" submodule update --init --recursive --depth 1 --jobs 4
fi
echo "SDK: $(git -C "$SDK" log -1 --format='%h %s')"

step "2/4 WWE 2K14 SDK patches"
if [ ! -d "$K14/.git" ]; then
    git clone --depth 1 https://github.com/Jarrodfeng/wwe2k14-recomp.git "$K14"
fi
echo "2K14 port: $(git -C "$K14" log -1 --format='%h %cs')"
if [ -f "$SDK/.wwe2k14-patched" ]; then
    echo "already patched ($(cat "$SDK/.wwe2k14-patched"))"
else
    n=0
    for p in "$K14"/patches/rexglue-sdk/full/*.diff; do
        git -C "$SDK" apply --whitespace=nowarn "$p" || die "$(basename "$p") does not apply"
        n=$((n + 1))
    done
    echo "$n patches from 2K14 $(git -C "$K14" rev-parse --short HEAD)" > "$SDK/.wwe2k14-patched"
    echo "applied $n patches"
fi

step "3/4 Host build of the SDK's code generator"
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libxss-dev libxrandr-dev libxcursor-dev libxi-dev \
    libxinerama-dev libxtst-dev >/dev/null 2>&1 || true
# Clang 18 + GCC 13 headers: the concepts macro for C++20+ targets only (as in the root CMakeLists.txt). This file runs
# as CMAKE_PROJECT_TOP_LEVEL_INCLUDES, before the compiler is known, so no compiler check here (clang-18 is forced
# below anyway).
cat > out/k14-concepts.cmake <<'EOF'
set(_cxx20 "$<AND:$<COMPILE_LANGUAGE:CXX>,$<VERSION_GREATER_EQUAL:$<TARGET_PROPERTY:CXX_STANDARD>,20>>")
add_compile_options("$<${_cxx20}:-D__cpp_concepts=202002L>" "$<${_cxx20}:-Wno-builtin-macro-redefined>")
EOF
GEN=(); command -v ninja >/dev/null && [ ! -f "$BUILD/CMakeCache.txt" ] && GEN=(-G Ninja)
cmake -S "$SDK" -B "$BUILD" "${GEN[@]}" -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_BUILD_TYPE=Release -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF -DCMAKE_CXX_FLAGS=-mssse3 \
    -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES="$ROOT/out/k14-concepts.cmake" > out/k14-configure.log 2>&1 \
    || { tail -30 out/k14-configure.log; die "configure failed (out/k14-configure.log)"; }
cmake --build "$BUILD" --target rexglue --parallel "$(nproc)"
REXGLUE="$(find "$SDK/out" "$BUILD" -name rexglue -type f -perm -u+x 2>/dev/null | head -1 || true)"
[ -n "$REXGLUE" ] || die "rexglue was not built"
echo "code generator: $REXGLUE ($("$REXGLUE" --version 2>&1 | head -1))"

step "4/4 Recompile WWE 2K16 with it (into generated_k14/)"
# 0.10 reads a manifest ([project] / [entrypoint]): wwe2k16_k14_manifest.toml. Skipped when generated_k14/ is complete
# (FORCE_CODEGEN=1 redoes it).
if [ -f generated_k14/sources.cmake ] && [ -f generated_k14/wwe2k16_init.h ] && [ "${FORCE_CODEGEN:-0}" != 1 ]; then
    echo "generated_k14/ is already there ($(ls generated_k14 | wc -l) files): skipped (FORCE_CODEGEN=1 to redo)"
    echo "==== DONE."
    exit 0
fi
rm -rf generated_k14
start=$(date +%s)
"$REXGLUE" codegen wwe2k16_k14_manifest.toml > out/k14-codegen.log 2>&1 || { tail -40 out/k14-codegen.log; die "codegen failed (out/k14-codegen.log)"; }
echo "codegen took $(( $(date +%s) - start )) s"
grep -E "Recompiling|functions|complete|error|warning" out/k14-codegen.log | grep -vi "debug" | tail -12 || true
echo "generated_k14: $(ls generated_k14 | wc -l) files, $(du -sh generated_k14 | cut -f1)"
echo
echo "==== DONE. Send me the last 40 lines of out/k14-codegen.log (or this whole output)."
