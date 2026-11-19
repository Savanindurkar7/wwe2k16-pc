/**
 * @file        content_probe.h
 * @brief       Lightweight host-side content analysis for GPU-resident buffers
 *
 * Computes mean luma and a stable content hash over stride-sampled pixels
 * from a raw RGBA8 or single-channel byte buffer. Used by the per-second
 * content probe diagnostic (REXCVAR gpu_content_probe).
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::graphics::content_probe {

/// Sample stride for luma/hash computation.
/// Each Nth pixel is sampled; stride=1 means every pixel.
constexpr uint32_t kDefaultStride = 4;

// Evidence-derived guest-address ranges of the resolve targets monitored by
// the per-second content probe. The spans match the "potentially modified
// memory range" of the Resolve trace lines for the adjudicated cycle-152
// targets (logs/cycle150-game.log):
// - kFront  [0x10855000, 0x10BED000) - 1280x720 k_8_8_8_8 (span 0x398000).
// - kScene0 [0x1F591000, 0x1F5A1000) - 256x256 k_8 (span 0x11000).
// - kScene1 [0x1F9FF000, 0x1FBFF000) - 512x512 k_16_16_16_16 (span 0x200000).
constexpr uint32_t kFrontRangeStart = 0x10855000u;
constexpr uint32_t kFrontRangeEnd = 0x10BED000u;
constexpr uint32_t kScene0RangeStart = 0x1F591000u;
constexpr uint32_t kScene0RangeEnd = 0x1F5A1000u;
constexpr uint32_t kScene1RangeStart = 0x1F9FF000u;
constexpr uint32_t kScene1RangeEnd = 0x1FBFF000u;

// Full-surface dimensions of each probe target (cycle-150/152 Resolve trace
// evidence). Content samples are only taken from resolves that cover exactly
// this surface: the cycle-157 smoke showed scene-1 partial (0,0)-origin
// resolves of 168x280 and 152x296 that would otherwise be sampled with a
// luma/hash baseline not comparable to the full-surface one.
constexpr uint32_t kFrontWidth = 1280;
constexpr uint32_t kFrontHeight = 720;
constexpr uint32_t kScene0Width = 256;
constexpr uint32_t kScene0Height = 256;
constexpr uint32_t kScene1Width = 512;
constexpr uint32_t kScene1Height = 512;

// Cycle-152 adjudicated lower-right clock box (1084,629)-(1125,650) = 42x22
// pixels - the only region that changes during the in-match HUD-only black
// stall. The last pixel is (1125,650), so a surface must be at least
// 1126x651 for the box sample to stay in bounds.
constexpr uint32_t kClockBoxX = 1084;
constexpr uint32_t kClockBoxY = 629;
constexpr uint32_t kClockBoxWidth = 42;
constexpr uint32_t kClockBoxHeight = 22;

/// Identity of a resolve destination extent matched against the probe ranges.
enum class ProbeTargetId : uint32_t {
  kNone = 0,
  kFront = 1,
  kScene0 = 2,
  kScene1 = 3,
};

/// Number of probe targets (kFront, kScene0, kScene1).
constexpr uint32_t kProbeTargetCount = 3;

/// Matches a resolve destination extent [extent_start, extent_end) against the
/// evidence-derived probe target ranges. The extent must lie fully inside
/// exactly one range; an empty/inverted extent, an unknown address, or an
/// extent straddling a range boundary returns kNone.
ProbeTargetId MatchProbeTarget(uint32_t extent_start, uint32_t extent_end);

/// Base guest address of a matched probe target range (0 for kNone).
uint32_t ProbeTargetBase(ProbeTargetId target);

/// Whether a resolve destination of the given dimensions is the full surface
/// of the matched probe target (front 1280x720, scene0 256x256, scene1
/// 512x512). Partial-surface resolves are rejected before any content copy.
bool IsFullSurfaceResolve(uint32_t width, uint32_t height, ProbeTargetId target);

/// Whether a surface is large enough to sample the clock box without reading
/// past its last pixel (width >= 1126 && height >= 651).
bool IsClockBoxSampleable(uint32_t width, uint32_t height);

/// Pure per-interval slot state of one probe target (mirrors the Vulkan-side
/// ContentProbeSlotInfo POD, no Vulkan dependency so the consumption reset is
/// unit-testable). A slot is consumed at emission time by Reset(), so an
/// interval without a full-surface record emits the documented no-sample
/// sentinel (base=0, w=h=0, luma=hash=0, frame=0) instead of re-emitting a
/// stale snapshot.
struct ProbeSlot {
  // A full-surface resolve was recorded into this double-buffered slot.
  bool recorded = false;
  // The recorded resolve was copied into the readback buffer and can be
  // sampled (set only together with recorded=true).
  bool sampled = false;
  uint32_t base = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;
  uint32_t bpp_log2 = 0;
  uint32_t length = 0;
  // Monotonic frame counter at record time (0 when no sample).
  uint64_t frame = 0;

  /// Consume the slot after an emission: clears recorded/sampled and zeroes
  /// all metadata.
  void Reset();
};

/// Whether the per-second content probe should emit a sample now.
/// first_enabled covers the first tick after the cvar is enabled.
constexpr bool ShouldEmitSample(int64_t elapsed_ms, bool first_enabled) {
  return first_enabled || elapsed_ms >= 1000;
}

/// Q8.8 luma of one k_16_16_16_16 pixel (8 bytes, 4 little-endian 16-bit
/// channels): the mean of the four channel high bytes, scaled by 256.
uint32_t compute_16bpp_luma_q8(const uint8_t* pixel8);

/// FNV-1a 32-bit hash over a byte buffer.
/// Deterministic, fast, good enough for content-change detection.
uint32_t fnv1a32(const uint8_t* data, size_t length);

/// Compute mean luma over stride-sampled RGBA8 pixels.
/// Luma = 0.299*R + 0.587*G + 0.114*B (fixed-point Q8.8).
/// Returns 0..256 (256 = full white).
uint32_t compute_luma_q8(const uint8_t* rgba8, uint32_t width, uint32_t height,
                         uint32_t stride = kDefaultStride);

/// Compute a content hash over stride-sampled RGBA8 pixels.
/// Hashes the sampled bytes directly (R,G,B per stride step).
uint32_t compute_hash(const uint8_t* rgba8, uint32_t width, uint32_t height,
                      uint32_t stride = kDefaultStride);

/// Compute mean luma over stride-sampled single-channel (k_8) bytes.
/// Returns 0..256.
uint32_t compute_byte_mean(const uint8_t* data, uint32_t width, uint32_t height,
                           uint32_t stride = kDefaultStride);

/// Compute a content hash over stride-sampled single-channel bytes.
uint32_t compute_byte_hash(const uint8_t* data, uint32_t width, uint32_t height,
                           uint32_t stride = kDefaultStride);

/// Sample a Xenos-tiled buffer per pixel with the given stride, computing mean
/// luma and an FNV-1a content hash over the sampled bytes. Per-pixel byte
/// offsets come from texture_util::GetTiledOffset2D, so the sampled byte
/// sequence equals the row-major pixel sequence of the equivalent linear image
/// (the unit-test equivalence contract).
/// bpp_log2: 0 = k_8 (1 byte/px), 2 = k_8_8_8_8 (4 bytes/px),
///           3 = k_16_16_16_16 (8 bytes/px); other values yield 0/0.
/// Luma: k_8 = mean byte (0..255); k_8_8_8_8 = Q8.8 of
/// (299R + 587G + 114B) / 1000 per pixel; k_16_16_16_16 = Q8.8 of the mean of
/// the four channel high bytes per pixel. Hash: FNV-1a over the sampled pixel
/// bytes (k_8: 1 byte; k_8_8_8_8: R,G,B; k_16_16_16_16: all 8 bytes), in
/// ascending byte order per pixel.
void SampleTiled(uint8_t bpp_log2, const uint8_t* tiled_base, uint32_t x0, uint32_t y0,
                 uint32_t width, uint32_t height, uint32_t pitch, uint32_t stride,
                 uint32_t& luma_q8_out, uint32_t& hash_out);

/// Per-emission-interval content probe counters (pure, no Vulkan dependency).
struct ProbeCounters {
  // Bounded per-write ring capacity for clear-register frame stamps. The ring
  // keeps the first 63 writes plus the LATEST write (always retained - it is
  // the one adjacent to a stall onset); the 65th write sets the overflow flag.
  static constexpr uint32_t kMaxClearWriteEntries = 64;
  // One guest write to a clear register, stamped with the monotonic frame
  // ordinal (16-33 ms granularity) at write time.
  struct ClearWriteEntry {
    uint64_t frame = 0;
    uint32_t value = 0;
  };
  // Per-target full-surface resolve summary over the emission interval
  // (appended for EVERY full-surface resolve, not just the sampled one).
  struct ResolveSummary {
    uint64_t first_frame = 0;
    uint64_t last_frame = 0;
    uint32_t count = 0;
  };

  uint32_t rbcc = 0;       // RB_COLOR_CLEAR register writes
  uint32_t rbcc_lo = 0;    // RB_COLOR_CLEAR_LO register writes
  uint32_t rbcc_zero = 0;  // writes of value 0 to either clear register
  // Last value written to each clear register during the current interval.
  // The corresponding COUNT is the presence flag: read rbcc_val only when
  // rbcc > 0 and rbcc_lo_val only when rbcc_lo > 0 (Reset leaves both 0 when
  // there was no write). This makes a clear-to-black at onset visible even
  // when rbcc_zero == 0 (non-zero dark clears) and split _LO-first writes.
  uint32_t rbcc_val = 0;
  uint32_t rbcc_lo_val = 0;
  uint32_t clears = 0;  // resolve-executed clears
  uint32_t draws = 0;   // PM4_DRAW_INDX(_2) packets
  // Per-write frame-stamp rings (sub-second ordering). *_writes_count is the
  // TOTAL number of writes in the interval (uncapped); the ring stores at most
  // kMaxClearWriteEntries entries (first 63 + latest). *_writes_overflow is
  // set once the stored ring has filled (more than 64 writes).
  ClearWriteEntry rbcc_writes[kMaxClearWriteEntries] = {};
  ClearWriteEntry rbcc_lo_writes[kMaxClearWriteEntries] = {};
  uint32_t rbcc_writes_count = 0;
  uint32_t rbcc_lo_writes_count = 0;
  uint8_t rbcc_writes_overflow = 0;
  uint8_t rbcc_lo_writes_overflow = 0;
  // Per-target full-surface resolve frame summaries, indexed by probe target
  // (kFront, kScene0, kScene1).
  ResolveSummary resolve_summary[kProbeTargetCount] = {};

  void OnRbColorClearWrite(uint32_t value, uint64_t frame) {
    ++rbcc;
    rbcc_val = value;
    if (value == 0) {
      ++rbcc_zero;
    }
    const uint32_t slot =
        rbcc_writes_count < kMaxClearWriteEntries ? rbcc_writes_count
                                                  : kMaxClearWriteEntries - 1;
    rbcc_writes[slot] = ClearWriteEntry{frame, value};
    if (rbcc_writes_count >= kMaxClearWriteEntries) {
      rbcc_writes_overflow = 1;
    }
    ++rbcc_writes_count;
  }
  void OnRbColorClearLoWrite(uint32_t value, uint64_t frame) {
    ++rbcc_lo;
    rbcc_lo_val = value;
    if (value == 0) {
      ++rbcc_zero;
    }
    const uint32_t slot =
        rbcc_lo_writes_count < kMaxClearWriteEntries ? rbcc_lo_writes_count
                                                     : kMaxClearWriteEntries - 1;
    rbcc_lo_writes[slot] = ClearWriteEntry{frame, value};
    if (rbcc_lo_writes_count >= kMaxClearWriteEntries) {
      rbcc_lo_writes_overflow = 1;
    }
    ++rbcc_lo_writes_count;
  }
  void OnResolveClearExecuted() { ++clears; }
  void OnDrawPacket() { ++draws; }
  // Append one full-surface resolve of the given probe target at the given
  // frame ordinal. Out-of-range targets are ignored (defensive; the record
  // path only passes valid indices).
  void AppendResolveFrame(uint32_t target_index, uint64_t frame) {
    if (target_index >= kProbeTargetCount) {
      return;
    }
    ResolveSummary& summary = resolve_summary[target_index];
    if (summary.count == 0) {
      summary.first_frame = frame;
    }
    summary.last_frame = frame;
    ++summary.count;
  }
  void Reset() {
    rbcc = 0;
    rbcc_lo = 0;
    rbcc_zero = 0;
    rbcc_val = 0;
    rbcc_lo_val = 0;
    clears = 0;
    draws = 0;
    for (ClearWriteEntry& entry : rbcc_writes) {
      entry = ClearWriteEntry{};
    }
    for (ClearWriteEntry& entry : rbcc_lo_writes) {
      entry = ClearWriteEntry{};
    }
    rbcc_writes_count = 0;
    rbcc_lo_writes_count = 0;
    rbcc_writes_overflow = 0;
    rbcc_lo_writes_overflow = 0;
    for (ResolveSummary& summary : resolve_summary) {
      summary = ResolveSummary{};
    }
  }
};

}  // namespace rex::graphics::content_probe
