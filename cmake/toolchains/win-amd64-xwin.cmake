# Cross-compile ReXGlue (and games built on it) for Windows x64 from Linux.
#
# Compiler: LLVM clang/clang++ (GNU-style driver, MSVC ABI), same driver as the
# native win-amd64 preset. Linker: lld-link. Windows SDK + MSVC CRT: an xwin
# splat (`xwin --accept-license splat --output <dir>`).
#
# Inputs (cache or environment):
#   XWIN_DIR   xwin splat output (default: $ENV{XWIN_DIR} or $HOME/xwin)
#   LLVM_BIN   directory with clang, lld-link, llvm-lib, llvm-rc, llvm-mt
#              (default: $ENV{LLVM_BIN} or /usr/lib/llvm-20/bin)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
  if(DEFINED ENV{XWIN_DIR})
    set(XWIN_DIR "$ENV{XWIN_DIR}")
  else()
    set(XWIN_DIR "$ENV{HOME}/xwin")
  endif()
endif()
if(NOT LLVM_BIN)
  if(DEFINED ENV{LLVM_BIN})
    set(LLVM_BIN "$ENV{LLVM_BIN}")
  else()
    set(LLVM_BIN "/usr/lib/llvm-20/bin")
  endif()
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "xwin splat directory")
set(LLVM_BIN "${LLVM_BIN}" CACHE PATH "LLVM tools directory")
# try_compile() projects re-read this file; forward the two inputs to them.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR LLVM_BIN)

if(NOT EXISTS "${XWIN_DIR}/sdk/include/um/windows.h")
  message(FATAL_ERROR "Windows SDK not found in XWIN_DIR='${XWIN_DIR}' (run xwin splat)")
endif()

set(_triple x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER   "${LLVM_BIN}/clang")
set(CMAKE_CXX_COMPILER "${LLVM_BIN}/clang++")
set(CMAKE_C_COMPILER_TARGET   ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_triple})
set(CMAKE_LINKER "${LLVM_BIN}/lld-link")
# The GNU-style driver makes CMake call the archiver ar-style (qc); lld-link reads these archives.
set(CMAKE_AR     "${LLVM_BIN}/llvm-ar")
set(CMAKE_RANLIB "${LLVM_BIN}/llvm-ranlib")
set(CMAKE_RC_COMPILER "${LLVM_BIN}/llvm-rc")
set(CMAKE_MT     "${LLVM_BIN}/llvm-mt")

set(_inc
  "${XWIN_DIR}/crt/include"
  "${XWIN_DIR}/sdk/include/ucrt"
  "${XWIN_DIR}/sdk/include/um"
  "${XWIN_DIR}/sdk/include/shared"
  "${XWIN_DIR}/sdk/include/winrt")
# The SDK/CRT headers must come AFTER clang's resource headers (what clang-cl's
# /imsvc does), otherwise the CRT's stddef.h (non-constant offsetof) and
# mmintrin.h (union __m64) shadow clang's. -I/-isystem search before the
# resource dir; -internal-isystem appends after it.
set(XWIN_CPU_FLAGS "-march=x86-64-v3" CACHE STRING "Target CPU flags")
set(_cflags "${XWIN_CPU_FLAGS}")
foreach(_d IN LISTS _inc)
  string(APPEND _cflags " -Xclang -internal-isystem -Xclang \"${_d}\"")
endforeach()
# *_FLAGS_INIT is replaced by a user -DCMAKE_<LANG>_FLAGS; fail loudly then.
foreach(_lang C CXX)
  if(DEFINED CACHE{CMAKE_${_lang}_FLAGS} AND NOT "$CACHE{CMAKE_${_lang}_FLAGS}" MATCHES "internal-isystem")
    message(FATAL_ERROR "Do not set CMAKE_${_lang}_FLAGS with this toolchain (it drops the Windows SDK headers); use XWIN_CPU_FLAGS")
  endif()
endforeach()
set(CMAKE_C_FLAGS_INIT   "${_cflags}")
set(CMAKE_CXX_FLAGS_INIT "${_cflags}")
set(CMAKE_RC_STANDARD_INCLUDE_DIRECTORIES ${_inc})

set(_lflags "-fuse-ld=lld")
foreach(_d crt/lib/x86_64 sdk/lib/um/x86_64 sdk/lib/ucrt/x86_64)
  string(APPEND _lflags " -L\"${XWIN_DIR}/${_d}\"")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_lflags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lflags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lflags}")

# Static MSVC runtime: the exe needs no VC++ redistributable on the target.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

# Find libraries/headers only in the target environment, programs on the host.
set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# xwin ships release CRT libs only (no libcmtd.lib); probe with a release config.
set(CMAKE_TRY_COMPILE_CONFIGURATION RelWithDebInfo)
