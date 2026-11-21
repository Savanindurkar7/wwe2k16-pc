// wwe2k16 - one source for both SDKs: the current Android SDK (ReXGlue 0.7, rexglue-android) and the WWE 2K14 SDK
// (ReXGlue 0.10 + the 2K14 port's patches, the native renderer's base). 0.7 has rex/ppc/memory.h with the PPC_* guest
// memory macros; 0.10 renamed the function macros (REX_FUNC / REX_EXTERN) and moved the memory macros into the
// generated precompiled header. This gives the hooks the 0.7 names on both.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

#if __has_include(<rex/ppc/memory.h>)
// ReXGlue 0.7 (rexglue-android): everything is there already.
#include <rex/ppc/memory.h>
#define WWE2K16_SDK_K14 0
#else
// ReXGlue 0.10 (WWE 2K14 SDK).
#include <rex/chrono/clock.h>
#include <rex/ppc/func.h>
#define WWE2K16_SDK_K14 1

#ifndef PPC_FUNC
#define PPC_FUNC(x) REX_FUNC(x)
#endif
// Same rule as the 0.10 codegen header (REX_PHYS_HOST_OFFSET): an offset only on ARM64 macOS.
#if REX_PLATFORM_MAC && REX_ARCH_ARM64
#define WWE2K16_PHYS_HOST_OFFSET(x) ((uint32_t(x) >= 0xE0000000u) ? 0x1000u : 0u)
#else
#define WWE2K16_PHYS_HOST_OFFSET(x) 0u
#endif
#define WWE2K16_GUEST_PTR(type, x) ((volatile type*)(base + uint32_t(x) + WWE2K16_PHYS_HOST_OFFSET(x)))
#ifndef PPC_LOAD_U8
#define PPC_LOAD_U8(x) (*WWE2K16_GUEST_PTR(uint8_t, x))
#define PPC_LOAD_U16(x) __builtin_bswap16(*WWE2K16_GUEST_PTR(uint16_t, x))
#define PPC_LOAD_U32(x) __builtin_bswap32(*WWE2K16_GUEST_PTR(uint32_t, x))
#define PPC_LOAD_U64(x) __builtin_bswap64(*WWE2K16_GUEST_PTR(uint64_t, x))
#define PPC_STORE_U8(x, y) (*WWE2K16_GUEST_PTR(uint8_t, x) = (y))
#define PPC_STORE_U16(x, y) (*WWE2K16_GUEST_PTR(uint16_t, x) = __builtin_bswap16(y))
#define PPC_STORE_U32(x, y) (*WWE2K16_GUEST_PTR(uint32_t, x) = __builtin_bswap32(y))
#define PPC_STORE_U64(x, y) (*WWE2K16_GUEST_PTR(uint64_t, x) = __builtin_bswap64(y))
#endif
#ifndef PPC_QUERY_TIMEBASE
#define PPC_QUERY_TIMEBASE() rex::chrono::Clock::QueryGuestTickCount()
#endif
#endif
