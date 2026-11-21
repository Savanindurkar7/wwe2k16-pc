// WWE 2K16, native (WWE 2K14 SDK) build: build the game's 3D scene without MSAA, so the native renderer can draw it.
// The same fix as the WWE 2K14 port's src/tiling_hooks.h (Jarrod Feng's wwe2k14-recomp, BSD-3), for the same Yuke's
// engine function at WWE 2K16's address. tools/k14-tiling-hooks.py finds that function in the local generated code
// by its shape and puts the two calls below into it (config/k14_tiling_hooks.toml has the addresses).
//
//   --wwe2k16_force_no_msaa=true   (Options > Graphics > Anti-aliasing (MSAA) > OFF) the scene targets are built 1x,
//                                  one tile. Default off: the game's own 2x MSAA.
//
// Why it matters: the native renderer's truth frames are 1x only. With the game's 2x MSAA every entrance frame falls
// back to the emulated path (5-9 fps; log "[native][truth] ... fallbacks (msaa / clearing resolve)"), and in a match
// single frames that fall back flicker.
//
// How the engine builds its scene targets (2K14 port, static reading; the 2K16 function has the same shape):
//   builder(scene object, MSAA type, layout flag, render device). A layout lookup fills the 256-byte layout at
//   scene object + 12 (width, height, MSAA type, tile count, then x1,y1,x2,y2 per tile); the next call sizes the
//   targets from it; then, only when the layout's type is non-zero, the colour (+272) and depth (+276) targets are
//   created with that type (type 0 is the engine's "not set up yet" state: no targets at all); last, the requested
//   type / flag are stored at + 4 / + 8.
// So, as in 2K14:
//   1. wwe2k16_tiling_layout_chosen (right after the layout lookup, before the sizer): with the setting on and a
//      non-zero type, rewrite the layout the way the engine's own 1-tile fallback writes it, with type 0. + 4 / + 8
//      keep the requested values, so the game's own "settings changed?" compare stays quiet.
//   2. wwe2k16_tiling_create_gate (the compare of the layout type against 0): for a build forced here, report
//      non-zero so the targets are created; the creation calls read the type again and get 0, i.e. 1x.
// Applies from the game's first real scene build after start (the setting is read at start; no runtime toggle).
//
// Guards (the layout is only rewritten when it looks like the scene's): width 320..2048 and height 180..2048, type 1
// or 2, 1..16 tiles, and 1x colour + depth fit the 10 MiB EDRAM (2048 tiles of 80x16 pixels at 32 bits).

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>

REXCVAR_DEFINE_BOOL(wwe2k16_force_no_msaa, false, "wwe2k16",
                    "Build the 3D scene without the game's 2x MSAA (one tile, 1x): the native renderer can then draw "
                    "entrances and matches itself. Read at start");

namespace wwe2k16::tiling {

inline constexpr uint32_t kLayoutWidth = 12;
inline constexpr uint32_t kLayoutHeight = 16;
inline constexpr uint32_t kLayoutType = 20;  // 0/1/2 = 1x/2x/4x
inline constexpr uint32_t kLayoutTiles = 24;
inline constexpr uint32_t kLayoutTile0 = 28;
inline constexpr uint64_t kEdramTiles = 2048;

inline uint32_t Load(uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address));
}
inline void Store(uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address), value);
}

// The build in progress on this thread (both hooks run inside one call of the builder, on one thread).
inline thread_local uint32_t t_forced_object = 0;
inline std::atomic<uint32_t> g_builds{0};

}  // namespace wwe2k16::tiling

// Mid-asm hooks: global, non-inline (the generated code declares them extern). This header is included once
// (wwe2k16_app.h, native build only).

void wwe2k16_tiling_layout_chosen(PPCRegister& object_reg) {
  using namespace wwe2k16::tiling;
  const uint32_t object = object_reg.u32;
  t_forced_object = 0;
  if (!object) return;
  const uint32_t width = Load(object + kLayoutWidth);
  const uint32_t height = Load(object + kLayoutHeight);
  const uint32_t type = Load(object + kLayoutType);
  const uint32_t tiles = Load(object + kLayoutTiles);
  const bool force = REXCVAR_GET(wwe2k16_force_no_msaa);
  const uint64_t tiles_1x = (uint64_t(width) + 79) / 80 * ((uint64_t(height) + 15) / 16);
  const char* result;
  if (!force) {
    result = "off (the game's MSAA)";
  } else if (type == 0) {
    result = "nothing to do (type 0)";
  } else if (type > 2 || width < 320 || width > 2048 || height < 180 || height > 2048 || tiles < 1 || tiles > 16) {
    result = "NOT applied (the layout does not look like a scene's)";
  } else if (2 * tiles_1x > kEdramTiles) {
    result = "NOT applied (1x colour + depth too big for the EDRAM)";
  } else {
    Store(object + kLayoutType, 0);
    Store(object + kLayoutTiles, 1);
    Store(object + kLayoutTile0 + 0, 0);
    Store(object + kLayoutTile0 + 4, 0);
    Store(object + kLayoutTile0 + 8, width);
    Store(object + kLayoutTile0 + 12, height);
    t_forced_object = object;
    result = "applied: 1 tile, 1x";
  }
  const uint32_t n = ++g_builds;
  if (n <= 16 || (n & (n - 1)) == 0) {
    REXLOG_INFO("[wwe2k16][tiling] scene build {}: object 0x{:08X}, {}x{}, game type {} ({}), {} tile(s); "
                "force_no_msaa: {}",
                n, object, width, height, type, type == 0 ? "1x" : type == 1 ? "2x" : type == 2 ? "4x" : "?", tiles,
                result);
  }
}

void wwe2k16_tiling_create_gate(PPCRegister& type_reg, PPCRegister& object_reg) {
  using namespace wwe2k16::tiling;
  if (t_forced_object && t_forced_object == object_reg.u32 && type_reg.u32 == 0) type_reg.u64 = 1;
  t_forced_object = 0;
}
