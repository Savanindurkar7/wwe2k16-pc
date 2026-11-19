/**
 * @file        content_probe_test.cpp
 * @brief       Unit tests for the content_probe luma/hash helpers
 *
 * Tests deterministic byte-buffer luma and hash computation with known inputs.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/util/content_probe.h>

namespace {

// Writes a linear image into a Xenos-tiled buffer using the SDK's own tiled
// offset function, mirroring how the probe reads the resolved target back.
void WriteTiledFromLinear(uint8_t bpp_log2, uint32_t width, uint32_t height, uint32_t pitch,
                          const uint8_t* linear, std::vector<uint8_t>& tiled) {
  const uint32_t bytes_per_pixel = 1u << bpp_log2;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      int32_t offset =
          rex::graphics::texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, bpp_log2);
      REQUIRE(offset >= 0);
      std::memcpy(tiled.data() + size_t(offset),
                  linear + (size_t(y) * width + x) * bytes_per_pixel, bytes_per_pixel);
    }
  }
}

}  // namespace

TEST_CASE("content_probe FNV-1a 32-bit hash is deterministic", "[content_probe]") {
  SECTION("Empty input produces expected hash") {
    const uint8_t empty[] = {};
    CHECK(rex::graphics::content_probe::fnv1a32(empty, 0) == 2166136261u);
  }

  SECTION("Known input produces deterministic hash") {
    const uint8_t data[] = {0xFF, 0x00, 0x80, 0x7F};
    uint32_t h1 = rex::graphics::content_probe::fnv1a32(data, sizeof(data));
    uint32_t h2 = rex::graphics::content_probe::fnv1a32(data, sizeof(data));
    CHECK(h1 == h2);
    CHECK(h1 != 0);  // Should not be all-zero
  }
}

TEST_CASE("content_probe RGBA8 luma computation", "[content_probe]") {
  // Create a 4x4 RGBA8 buffer with known values
  SECTION("Pure black yields luma 0") {
    std::array<uint8_t, 4 * 4 * 4> buf{};  // All zeros
    uint32_t luma = rex::graphics::content_probe::compute_luma_q8(buf.data(), 4, 4, 1);
    CHECK(luma == 0);
  }

  SECTION("Pure white yields luma 256 (Q8.8 = 1.0 * 256)") {
    std::array<uint8_t, 4 * 4 * 4> buf;
    buf.fill(0);
    for (size_t i = 0; i < buf.size(); i += 4) {
      buf[i + 0] = 255;  // R
      buf[i + 1] = 255;  // G
      buf[i + 2] = 255;  // B
    }
    uint32_t luma = rex::graphics::content_probe::compute_luma_q8(buf.data(), 4, 4, 1);
    // Luma = 0.299*255 + 0.587*255 + 0.114*255 = 255.0 (exact)
    // Q8.8: 255 * 256 = 65280
    CHECK(luma == 65280);
  }

  SECTION("Mid-gray yields expected luma") {
    std::array<uint8_t, 4 * 4 * 4> buf;
    buf.fill(0);
    for (size_t i = 0; i < buf.size(); i += 4) {
      buf[i + 0] = 128;
      buf[i + 1] = 128;
      buf[i + 2] = 128;
    }
    uint32_t luma = rex::graphics::content_probe::compute_luma_q8(buf.data(), 4, 4, 1);
    // Luma = 0.299*128 + 0.587*128 + 0.114*128 = 128.0
    // Q8.8: 128 * 256 = 32768
    CHECK(luma == 32768);
  }

  SECTION("Stride sampling reduces sample count") {
    std::array<uint8_t, 8 * 8 * 4> buf;
    buf.fill(0);
    // Set every other pixel to white
    for (uint32_t y = 0; y < 8; y += 2) {
      for (uint32_t x = 0; x < 8; x += 2) {
        size_t idx = (y * 8 + x) * 4;
        buf[idx + 0] = 255;
        buf[idx + 1] = 255;
        buf[idx + 2] = 255;
      }
    }
    // With stride=1, samples all 64 pixels (16 white, 48 black = 1/4 white)
    // Luma = 1/4 * 65280 = 16320
    uint32_t luma_full = rex::graphics::content_probe::compute_luma_q8(buf.data(), 8, 8, 1);
    // With stride=2, samples 16 pixels (all white)
    uint32_t luma_stride2 = rex::graphics::content_probe::compute_luma_q8(buf.data(), 8, 8, 2);
    CHECK(luma_full == 16320);  // 1/4 white, 3/4 black
    CHECK(luma_stride2 == 65280);  // All sampled pixels are white
  }

  SECTION("Zero dimensions yield 0") {
    std::array<uint8_t, 16> buf{};
    CHECK(rex::graphics::content_probe::compute_luma_q8(buf.data(), 0, 4, 1) == 0);
    CHECK(rex::graphics::content_probe::compute_luma_q8(buf.data(), 4, 0, 1) == 0);
    CHECK(rex::graphics::content_probe::compute_luma_q8(nullptr, 4, 4, 1) == 0);
  }
}

TEST_CASE("content_probe RGBA8 hash computation", "[content_probe]") {
  SECTION("All-black buffer yields deterministic hash") {
    std::array<uint8_t, 4 * 4 * 4> buf{};
    uint32_t h = rex::graphics::content_probe::compute_hash(buf.data(), 4, 4, 1);
    CHECK(h != 0);  // Should not be all-zero
    // Deterministic
    CHECK(rex::graphics::content_probe::compute_hash(buf.data(), 4, 4, 1) == h);
  }

  SECTION("Different buffers produce different hashes") {
    std::array<uint8_t, 4 * 4 * 4> buf_a{}, buf_b{};
    buf_a.fill(0);
    buf_b.fill(0);
    buf_b[0] = 1;  // Change one byte
    uint32_t h_a = rex::graphics::content_probe::compute_hash(buf_a.data(), 4, 4, 1);
    uint32_t h_b = rex::graphics::content_probe::compute_hash(buf_b.data(), 4, 4, 1);
    CHECK(h_a != h_b);
  }

  SECTION("Stride sampling changes hash") {
    std::array<uint8_t, 8 * 8 * 4> buf;
    buf.fill(0);
    // Fill with a pattern
    for (uint32_t y = 0; y < 8; ++y) {
      for (uint32_t x = 0; x < 8; ++x) {
        size_t idx = (y * 8 + x) * 4;
        buf[idx + 0] = static_cast<uint8_t>((x * 32 + y * 17) & 0xFF);
        buf[idx + 1] = static_cast<uint8_t>((x * 17 + y * 31) & 0xFF);
        buf[idx + 2] = static_cast<uint8_t>((x * 13 + y * 23) & 0xFF);
      }
    }
    uint32_t h1 = rex::graphics::content_probe::compute_hash(buf.data(), 8, 8, 1);
    uint32_t h2 = rex::graphics::content_probe::compute_hash(buf.data(), 8, 8, 2);
    // Different strides sample different pixels, so hashes should differ
    CHECK(h1 != h2);
  }

  SECTION("Zero dimensions yield 0") {
    std::array<uint8_t, 16> buf{};
    CHECK(rex::graphics::content_probe::compute_hash(buf.data(), 0, 4, 1) == 0);
    CHECK(rex::graphics::content_probe::compute_hash(buf.data(), 4, 0, 1) == 0);
    CHECK(rex::graphics::content_probe::compute_hash(nullptr, 4, 4, 1) == 0);
  }
}

TEST_CASE("content_probe single-channel byte mean", "[content_probe]") {
  SECTION("All zeros yields mean 0") {
    std::array<uint8_t, 4 * 4> buf{};
    uint32_t mean = rex::graphics::content_probe::compute_byte_mean(buf.data(), 4, 4, 1);
    CHECK(mean == 0);
  }

  SECTION("All 0xFF yields mean 255") {
    std::array<uint8_t, 4 * 4> buf;
    buf.fill(0xFF);
    uint32_t mean = rex::graphics::content_probe::compute_byte_mean(buf.data(), 4, 4, 1);
    CHECK(mean == 255);
  }

  SECTION("Mixed values yield correct mean") {
    std::array<uint8_t, 4> buf = {0, 128, 128, 255};
    uint32_t mean = rex::graphics::content_probe::compute_byte_mean(buf.data(), 4, 1, 1);
    // (0 + 128 + 128 + 255) / 4 = 511 / 4 = 127 (integer division)
    CHECK(mean == 127);
  }

  SECTION("Zero dimensions yield 0") {
    std::array<uint8_t, 16> buf{};
    CHECK(rex::graphics::content_probe::compute_byte_mean(buf.data(), 0, 4, 1) == 0);
    CHECK(rex::graphics::content_probe::compute_byte_mean(nullptr, 4, 4, 1) == 0);
  }
}

TEST_CASE("content_probe single-channel byte hash", "[content_probe]") {
  SECTION("Deterministic for same input") {
    std::array<uint8_t, 16> buf{};
    buf.fill(0x42);
    uint32_t h1 = rex::graphics::content_probe::compute_byte_hash(buf.data(), 4, 4, 1);
    uint32_t h2 = rex::graphics::content_probe::compute_byte_hash(buf.data(), 4, 4, 1);
    CHECK(h1 == h2);
    CHECK(h1 != 0);
  }

  SECTION("Different inputs produce different hashes") {
    std::array<uint8_t, 16> buf_a{}, buf_b{};
    buf_a.fill(0);
    buf_b.fill(1);
    CHECK(rex::graphics::content_probe::compute_byte_hash(buf_a.data(), 4, 4, 1) !=
          rex::graphics::content_probe::compute_byte_hash(buf_b.data(), 4, 4, 1));
  }

  SECTION("Zero dimensions yield 0") {
    std::array<uint8_t, 16> buf{};
    CHECK(rex::graphics::content_probe::compute_byte_hash(buf.data(), 0, 4, 1) == 0);
    CHECK(rex::graphics::content_probe::compute_byte_hash(nullptr, 4, 4, 1) == 0);
  }
}

TEST_CASE("content_probe stride=1 covers every pixel", "[content_probe]") {
  // When stride=1, every pixel should be included in the average
  SECTION("Luma with stride=1 equals weighted average of all pixels") {
    // 2x2 buffer: one black, three white
    std::array<uint8_t, 2 * 2 * 4> buf;
    buf.fill(0);
    // Pixel (0,0) = black
    // Pixel (1,0) = white
    buf[4 + 0] = 255; buf[4 + 1] = 255; buf[4 + 2] = 255;
    // Pixel (0,1) = white
    buf[4 * 2 + 0] = 255; buf[4 * 2 + 1] = 255; buf[4 * 2 + 2] = 255;
    // Pixel (1,1) = white
    buf[4 * 2 + 4 + 0] = 255; buf[4 * 2 + 4 + 1] = 255; buf[4 * 2 + 4 + 2] = 255;

    uint32_t luma = rex::graphics::content_probe::compute_luma_q8(buf.data(), 2, 2, 1);
    // Average luma = (0 + 255 + 255 + 255) / 4 = 191.25
    // Q8.8: 191 * 256 = 48896, plus fractional part
    // Using the formula: (299*R + 587*G + 114*B) / 1000 * 256
    // For white: (299+587+114)*255/1000*256 = 1000*255/1000*256 = 255*256 = 65280
    // For black: 0
    // Average: (0 + 65280 + 65280 + 65280) / 4 = 48960
    CHECK(luma == 48960);
  }
}

TEST_CASE("content_probe probe target identity matcher", "[content_probe]") {
  using rex::graphics::content_probe::MatchProbeTarget;
  using rex::graphics::content_probe::ProbeTargetId;

  SECTION("Evidence-derived full ranges match their identity") {
    CHECK(MatchProbeTarget(0x10855000u, 0x10BED000u) == ProbeTargetId::kFront);
    CHECK(MatchProbeTarget(0x1F591000u, 0x1F5A1000u) == ProbeTargetId::kScene0);
    CHECK(MatchProbeTarget(0x1F9FF000u, 0x1FBFF000u) == ProbeTargetId::kScene1);
  }

  SECTION("Subranges inside a range match the same identity") {
    CHECK(MatchProbeTarget(0x10855000u, 0x10856000u) == ProbeTargetId::kFront);
    CHECK(MatchProbeTarget(0x1F591000u + 0x1000u, 0x1F591000u + 0x2000u) ==
          ProbeTargetId::kScene0);
    CHECK(MatchProbeTarget(0x1F9FF000u + 0x1000u, 0x1F9FF000u + 0x2000u) ==
          ProbeTargetId::kScene1);
  }

  SECTION("Unknown addresses are rejected") {
    CHECK(MatchProbeTarget(0x16500000u, 0x16510000u) == ProbeTargetId::kNone);
    CHECK(MatchProbeTarget(0x104BD000u, 0x104BE000u) == ProbeTargetId::kNone);
    CHECK(MatchProbeTarget(0x10855000u - 0x1000u, 0x10855000u) == ProbeTargetId::kNone);
    CHECK(MatchProbeTarget(0x10BED000u, 0x10BF0000u) == ProbeTargetId::kNone);
  }

  SECTION("Straddling and empty extents are rejected") {
    CHECK(MatchProbeTarget(0x1F5A0FFFu, 0x1F5A2000u) == ProbeTargetId::kNone);
    CHECK(MatchProbeTarget(0x10855000u, 0x10855000u) == ProbeTargetId::kNone);
    CHECK(MatchProbeTarget(0x10856000u, 0x10855000u) == ProbeTargetId::kNone);
  }

  SECTION("Probe target bases are the range starts") {
    using rex::graphics::content_probe::ProbeTargetBase;
    CHECK(ProbeTargetBase(ProbeTargetId::kFront) == 0x10855000u);
    CHECK(ProbeTargetBase(ProbeTargetId::kScene0) == 0x1F591000u);
    CHECK(ProbeTargetBase(ProbeTargetId::kScene1) == 0x1F9FF000u);
    CHECK(ProbeTargetBase(ProbeTargetId::kNone) == 0u);
  }
}

TEST_CASE("content_probe emission cadence", "[content_probe]") {
  using rex::graphics::content_probe::ShouldEmitSample;
  CHECK_FALSE(ShouldEmitSample(0, false));
  CHECK_FALSE(ShouldEmitSample(500, false));
  CHECK_FALSE(ShouldEmitSample(999, false));
  CHECK(ShouldEmitSample(1000, false));
  CHECK(ShouldEmitSample(1001, false));
  CHECK(ShouldEmitSample(0, true));
}

TEST_CASE("content_probe 16bpp channel-high-byte luma", "[content_probe]") {
  using rex::graphics::content_probe::compute_16bpp_luma_q8;

  SECTION("Black pixel yields 0") {
    std::array<uint8_t, 8> pixel{};
    CHECK(compute_16bpp_luma_q8(pixel.data()) == 0);
  }

  SECTION("All 0xFFFF channels yield 65280") {
    std::array<uint8_t, 8> pixel;
    pixel.fill(0xFF);
    CHECK(compute_16bpp_luma_q8(pixel.data()) == 65280);
  }

  SECTION("Pinned mixed pixel") {
    // High bytes 0x12=18, 0x56=86, 0x9A=154, 0xDE=222 -> (18+86+154+222)/4=120.
    std::array<uint8_t, 8> pixel = {0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A, 0xF0, 0xDE};
    CHECK(compute_16bpp_luma_q8(pixel.data()) == 120u * 256u);
  }

  SECTION("Integer truncation is pinned") {
    // High bytes 1, 1, 1, 0 -> 3/4 = 0.
    std::array<uint8_t, 8> pixel = {0, 1, 0, 1, 0, 1, 0, 0};
    CHECK(compute_16bpp_luma_q8(pixel.data()) == 0);
  }

  SECTION("Nullptr yields 0") { CHECK(compute_16bpp_luma_q8(nullptr) == 0); }
}

TEST_CASE("content_probe tiled sampler equivalence with linear helpers", "[content_probe]") {
  using namespace rex::graphics;

  SECTION("All-black 256x256 k_8 matches the linear byte hash") {
    std::vector<uint8_t> tiled(256 * 256, 0);
    uint32_t luma = 123, hash = 456;
    content_probe::SampleTiled(0, tiled.data(), 0, 0, 256, 256, 256,
                               content_probe::kDefaultStride, luma, hash);
    CHECK(luma == 0);
    CHECK(hash == content_probe::compute_byte_hash(tiled.data(), 256, 256,
                                                   content_probe::kDefaultStride));
  }

  SECTION("Quarter-white 8x8 k_8_8_8_8 written through GetTiledOffset2D") {
    constexpr uint32_t kWidth = 8, kHeight = 8, kPitch = 8;
    std::vector<uint8_t> linear(size_t(kWidth) * kHeight * 4, 0);
    for (uint32_t y = 0; y < kHeight; y += 2) {
      for (uint32_t x = 0; x < kWidth; x += 2) {
        size_t idx = (size_t(y) * kWidth + x) * 4;
        linear[idx + 0] = 255;
        linear[idx + 1] = 255;
        linear[idx + 2] = 255;
      }
    }
    std::vector<uint8_t> tiled(4096, 0);
    WriteTiledFromLinear(2, kWidth, kHeight, kPitch, linear.data(), tiled);
    uint32_t luma = 0, hash = 0;
    content_probe::SampleTiled(2, tiled.data(), 0, 0, kWidth, kHeight, kPitch, 1, luma, hash);
    CHECK(luma == 16320);  // 1/4 white, 3/4 black, all 64 pixels sampled
    CHECK(hash == content_probe::compute_hash(linear.data(), kWidth, kHeight, 1));
  }

  SECTION("All-0xFF 4x4 k_16_16_16_16 matches FNV-1a over all 8 bytes") {
    constexpr uint32_t kWidth = 4, kHeight = 4, kPitch = 4;
    std::vector<uint8_t> linear(size_t(kWidth) * kHeight * 8, 0xFF);
    std::vector<uint8_t> tiled(4096, 0);
    WriteTiledFromLinear(3, kWidth, kHeight, kPitch, linear.data(), tiled);
    uint32_t luma = 0, hash = 0;
    content_probe::SampleTiled(3, tiled.data(), 0, 0, kWidth, kHeight, kPitch, 1, luma, hash);
    CHECK(luma == 65280);  // 0xFF high bytes in all four channels
    CHECK(hash == content_probe::fnv1a32(linear.data(), linear.size()));
  }
}

TEST_CASE("content_probe tiled sampler pinned constants and stride", "[content_probe]") {
  using namespace rex::graphics;

  SECTION("Pinned non-trivial RGBA8 pattern") {
    constexpr uint32_t kWidth = 4, kHeight = 4, kPitch = 4;
    std::vector<uint8_t> linear(size_t(kWidth) * kHeight * 4, 0);
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        size_t idx = (size_t(y) * kWidth + x) * 4;
        linear[idx + 0] = uint8_t(x * 16 + 1);
        linear[idx + 1] = uint8_t(y * 16 + 2);
        linear[idx + 2] = uint8_t((x + y) * 8 + 3);
        linear[idx + 3] = 255;
      }
    }
    std::vector<uint8_t> tiled(4096, 0);
    WriteTiledFromLinear(2, kWidth, kHeight, kPitch, linear.data(), tiled);
    uint32_t luma = 0, hash = 0;
    content_probe::SampleTiled(2, tiled.data(), 0, 0, kWidth, kHeight, kPitch, 1, luma, hash);
    CHECK(luma == 6608);
    CHECK(hash == 0x49AAA245u);
  }

  SECTION("Stride 4 differs from stride 1 on a quarter-white pattern") {
    constexpr uint32_t kWidth = 8, kHeight = 8, kPitch = 8;
    std::vector<uint8_t> linear(size_t(kWidth) * kHeight * 4, 0);
    for (uint32_t y = 0; y < kHeight; y += 2) {
      for (uint32_t x = 0; x < kWidth; x += 2) {
        size_t idx = (size_t(y) * kWidth + x) * 4;
        linear[idx + 0] = 255;
        linear[idx + 1] = 255;
        linear[idx + 2] = 255;
      }
    }
    std::vector<uint8_t> tiled(4096, 0);
    WriteTiledFromLinear(2, kWidth, kHeight, kPitch, linear.data(), tiled);
    uint32_t luma1 = 0, hash1 = 0, luma4 = 0, hash4 = 0;
    content_probe::SampleTiled(2, tiled.data(), 0, 0, kWidth, kHeight, kPitch, 1, luma1, hash1);
    content_probe::SampleTiled(2, tiled.data(), 0, 0, kWidth, kHeight, kPitch,
                               content_probe::kDefaultStride, luma4, hash4);
    CHECK(luma1 == 16320);
    CHECK(luma4 == 65280);  // stride 4 samples only white pixels
    CHECK(hash1 != hash4);
  }

  SECTION("Zero dimensions and nullptr yield 0/0") {
    std::vector<uint8_t> tiled(4096, 0);
    uint32_t luma = 7, hash = 9;
    content_probe::SampleTiled(2, tiled.data(), 0, 0, 0, 0, 8, 4, luma, hash);
    CHECK(luma == 0);
    CHECK(hash == 0);
    luma = 7;
    hash = 9;
    content_probe::SampleTiled(2, nullptr, 0, 0, 8, 8, 8, 4, luma, hash);
    CHECK(luma == 0);
    CHECK(hash == 0);
  }
}

TEST_CASE("content_probe probe counter semantics", "[content_probe]") {
  rex::graphics::content_probe::ProbeCounters counters;

  counters.OnRbColorClearWrite(0x11223344u, 1);
  CHECK(counters.rbcc == 1);
  CHECK(counters.rbcc_zero == 0);

  counters.OnRbColorClearWrite(0, 2);
  CHECK(counters.rbcc == 2);
  CHECK(counters.rbcc_zero == 1);

  counters.OnRbColorClearLoWrite(0, 3);
  CHECK(counters.rbcc_lo == 1);
  CHECK(counters.rbcc_zero == 2);

  counters.OnRbColorClearLoWrite(0xDEADBEEFu, 4);
  CHECK(counters.rbcc_lo == 2);
  CHECK(counters.rbcc_zero == 2);

  counters.OnResolveClearExecuted();
  counters.OnResolveClearExecuted();
  CHECK(counters.clears == 2);

  counters.OnDrawPacket();
  CHECK(counters.draws == 1);

  counters.Reset();
  CHECK(counters.rbcc == 0);
  CHECK(counters.rbcc_lo == 0);
  CHECK(counters.rbcc_zero == 0);
  CHECK(counters.clears == 0);
  CHECK(counters.draws == 0);
}

TEST_CASE("content_probe clear-register value capture", "[content_probe]") {
  using rex::graphics::content_probe::ProbeCounters;

  ProbeCounters counters;
  // No write in the interval: the count is the presence flag and the value
  // fields stay 0, so they must be read only when the count is non-zero.
  CHECK(counters.rbcc == 0);
  CHECK(counters.rbcc_val == 0);
  CHECK(counters.rbcc_lo == 0);
  CHECK(counters.rbcc_lo_val == 0);

  SECTION("Split _LO-first clear-to-black keeps both values visible") {
    counters.OnRbColorClearLoWrite(0x00000000u, 1);
    CHECK(counters.rbcc_lo == 1);
    CHECK(counters.rbcc_lo_val == 0x00000000u);  // zero is a captured value
    CHECK(counters.rbcc_zero == 1);
    counters.OnRbColorClearWrite(0x00000000u, 2);
    CHECK(counters.rbcc == 1);
    CHECK(counters.rbcc_val == 0x00000000u);
    CHECK(counters.rbcc_zero == 2);
  }

  SECTION("Last value wins within one interval") {
    counters.OnRbColorClearWrite(0x00112233u, 1);
    counters.OnRbColorClearWrite(0xDEADBEEFu, 2);
    CHECK(counters.rbcc == 2);
    CHECK(counters.rbcc_val == 0xDEADBEEFu);
    CHECK(counters.rbcc_zero == 0);
    counters.OnRbColorClearLoWrite(0xFF000000u, 3);
    counters.OnRbColorClearLoWrite(0x80000000u, 4);
    CHECK(counters.rbcc_lo == 2);
    CHECK(counters.rbcc_lo_val == 0x80000000u);
  }

  SECTION("A non-zero dark clear is visible even with rbcc_zero == 0") {
    counters.OnRbColorClearWrite(0x00010101u, 1);
    CHECK(counters.rbcc == 1);
    CHECK(counters.rbcc_zero == 0);
    CHECK(counters.rbcc_val == 0x00010101u);
  }

  SECTION("Reset is the per-emission boundary and zeroes the values") {
    counters.OnRbColorClearWrite(0xFFFFFFFFu, 1);
    counters.OnRbColorClearLoWrite(0x12345678u, 2);
    counters.Reset();
    CHECK(counters.rbcc == 0);
    CHECK(counters.rbcc_lo == 0);
    CHECK(counters.rbcc_zero == 0);
    CHECK(counters.rbcc_val == 0);
    CHECK(counters.rbcc_lo_val == 0);
    // No write after the reset: values stay 0 (presence flag is the count).
    CHECK(counters.rbcc == 0);
    CHECK(counters.rbcc_val == 0);
  }
}

TEST_CASE("content_probe full-surface resolve gate", "[content_probe]") {
  using rex::graphics::content_probe::IsFullSurfaceResolve;
  using rex::graphics::content_probe::ProbeTargetId;

  SECTION("Full surfaces of each identity are accepted") {
    CHECK(IsFullSurfaceResolve(1280, 720, ProbeTargetId::kFront));
    CHECK(IsFullSurfaceResolve(256, 256, ProbeTargetId::kScene0));
    CHECK(IsFullSurfaceResolve(512, 512, ProbeTargetId::kScene1));
  }

  SECTION("Observed partial scene-1 resolves are rejected") {
    CHECK_FALSE(IsFullSurfaceResolve(168, 280, ProbeTargetId::kScene1));
    CHECK_FALSE(IsFullSurfaceResolve(152, 296, ProbeTargetId::kScene1));
  }

  SECTION("Near-miss dimensions are rejected") {
    CHECK_FALSE(IsFullSurfaceResolve(511, 512, ProbeTargetId::kScene1));
    CHECK_FALSE(IsFullSurfaceResolve(512, 511, ProbeTargetId::kScene1));
    CHECK_FALSE(IsFullSurfaceResolve(1280, 719, ProbeTargetId::kFront));
    CHECK_FALSE(IsFullSurfaceResolve(1279, 720, ProbeTargetId::kFront));
    CHECK_FALSE(IsFullSurfaceResolve(256, 128, ProbeTargetId::kScene0));
  }

  SECTION("Dimensions of one identity do not validate another") {
    CHECK_FALSE(IsFullSurfaceResolve(1280, 720, ProbeTargetId::kScene1));
    CHECK_FALSE(IsFullSurfaceResolve(256, 256, ProbeTargetId::kFront));
    CHECK_FALSE(IsFullSurfaceResolve(512, 512, ProbeTargetId::kScene0));
  }

  SECTION("No identity and zero dimensions are rejected") {
    CHECK_FALSE(IsFullSurfaceResolve(1280, 720, ProbeTargetId::kNone));
    CHECK_FALSE(IsFullSurfaceResolve(0, 0, ProbeTargetId::kFront));
  }
}

TEST_CASE("content_probe clock-box sample guard", "[content_probe]") {
  using rex::graphics::content_probe::IsClockBoxSampleable;

  SECTION("Minimum covering dimensions are accepted") {
    CHECK(IsClockBoxSampleable(1126, 651));
    CHECK(IsClockBoxSampleable(1280, 720));
  }

  SECTION("One pixel short is rejected") {
    CHECK_FALSE(IsClockBoxSampleable(1125, 651));
    CHECK_FALSE(IsClockBoxSampleable(1126, 650));
    CHECK_FALSE(IsClockBoxSampleable(42, 22));
    CHECK_FALSE(IsClockBoxSampleable(0, 0));
  }
}

TEST_CASE("content_probe slot consumption reset emits the no-sample sentinel", "[content_probe]") {
  using rex::graphics::content_probe::ProbeSlot;

  ProbeSlot slot;
  CHECK_FALSE(slot.recorded);
  CHECK_FALSE(slot.sampled);
  CHECK(slot.base == 0);
  CHECK(slot.width == 0);
  CHECK(slot.height == 0);
  CHECK(slot.pitch == 0);
  CHECK(slot.bpp_log2 == 0);
  CHECK(slot.length == 0);
  CHECK(slot.frame == 0);

  // A full-surface sample recorded in an interval.
  slot.recorded = true;
  slot.sampled = true;
  slot.base = 0x1F9FF000u;
  slot.width = 512;
  slot.height = 512;
  slot.pitch = 512;
  slot.bpp_log2 = 3;
  slot.length = 0x200000;
  slot.frame = 4242;
  CHECK(slot.recorded);
  CHECK(slot.sampled);

  // Consumption at emission clears every field, so an interval with no
  // full-surface record emits base=0/w=h=0/luma=hash=0/frame=0 (F1) instead
  // of re-emitting this snapshot.
  slot.Reset();
  CHECK_FALSE(slot.recorded);
  CHECK_FALSE(slot.sampled);
  CHECK(slot.base == 0);
  CHECK(slot.width == 0);
  CHECK(slot.height == 0);
  CHECK(slot.pitch == 0);
  CHECK(slot.bpp_log2 == 0);
  CHECK(slot.length == 0);
  CHECK(slot.frame == 0);
}

TEST_CASE("content_probe clear-write frame-stamp ring", "[content_probe]") {
  using rex::graphics::content_probe::ProbeCounters;

  SECTION("64 writes fill the ring without overflow") {
    ProbeCounters counters;
    for (uint32_t i = 0; i < ProbeCounters::kMaxClearWriteEntries; ++i) {
      counters.OnRbColorClearWrite(0xA0000000u + i, 1000 + i);
    }
    CHECK(counters.rbcc == 64);
    CHECK(counters.rbcc_writes_count == 64);
    CHECK(counters.rbcc_writes_overflow == 0);
    // First 63 writes kept in order...
    for (uint32_t i = 0; i < 63; ++i) {
      CHECK(counters.rbcc_writes[i].frame == 1000 + i);
      CHECK(counters.rbcc_writes[i].value == 0xA0000000u + i);
    }
    // ...and the 64th (latest) write in the last slot.
    CHECK(counters.rbcc_writes[63].frame == 1063);
    CHECK(counters.rbcc_writes[63].value == 0xA000003Fu);
  }

  SECTION("65th write sets overflow and overwrites only the latest slot") {
    ProbeCounters counters;
    for (uint32_t i = 0; i < ProbeCounters::kMaxClearWriteEntries; ++i) {
      counters.OnRbColorClearWrite(0xB0000000u + i, 2000 + i);
    }
    counters.OnRbColorClearWrite(0xCCCCCCCCu, 3000);
    CHECK(counters.rbcc == 65);
    CHECK(counters.rbcc_writes_count == 65);
    CHECK(counters.rbcc_writes_overflow == 1);
    // First 63 writes untouched...
    for (uint32_t i = 0; i < 63; ++i) {
      CHECK(counters.rbcc_writes[i].frame == 2000 + i);
      CHECK(counters.rbcc_writes[i].value == 0xB0000000u + i);
    }
    // ...the 65th (latest) write replaces the 64th in the last slot.
    CHECK(counters.rbcc_writes[63].frame == 3000);
    CHECK(counters.rbcc_writes[63].value == 0xCCCCCCCCu);
    // The latest write is also the captured last value.
    CHECK(counters.rbcc_val == 0xCCCCCCCCu);
  }

  SECTION("Further writes keep overwriting the latest slot") {
    ProbeCounters counters;
    for (uint32_t i = 0; i < ProbeCounters::kMaxClearWriteEntries + 10; ++i) {
      counters.OnRbColorClearWrite(0, 4000 + i);
    }
    CHECK(counters.rbcc == 74);
    CHECK(counters.rbcc_writes_count == 74);
    CHECK(counters.rbcc_writes_overflow == 1);
    CHECK(counters.rbcc_zero == 74);
    CHECK(counters.rbcc_writes[63].frame == 4073);
  }
}

TEST_CASE("content_probe clear-write rings are independent per register", "[content_probe]") {
  using rex::graphics::content_probe::ProbeCounters;

  ProbeCounters counters;
  counters.OnRbColorClearWrite(0x11111111u, 10);
  counters.OnRbColorClearLoWrite(0x22222222u, 11);
  counters.OnRbColorClearLoWrite(0x33333333u, 12);

  CHECK(counters.rbcc == 1);
  CHECK(counters.rbcc_writes_count == 1);
  CHECK(counters.rbcc_writes[0].frame == 10);
  CHECK(counters.rbcc_writes[0].value == 0x11111111u);
  CHECK(counters.rbcc_writes_overflow == 0);

  CHECK(counters.rbcc_lo == 2);
  CHECK(counters.rbcc_lo_writes_count == 2);
  CHECK(counters.rbcc_lo_writes[0].frame == 11);
  CHECK(counters.rbcc_lo_writes[0].value == 0x22222222u);
  CHECK(counters.rbcc_lo_writes[1].frame == 12);
  CHECK(counters.rbcc_lo_writes[1].value == 0x33333333u);
  CHECK(counters.rbcc_lo_writes_overflow == 0);

  // Overflow on one ring does not affect the other.
  for (uint32_t i = 0; i < ProbeCounters::kMaxClearWriteEntries + 1; ++i) {
    counters.OnRbColorClearWrite(0, 100 + i);
  }
  CHECK(counters.rbcc_writes_overflow == 1);
  CHECK(counters.rbcc_lo_writes_overflow == 0);
  CHECK(counters.rbcc_lo_writes_count == 2);
  CHECK(counters.rbcc_lo_writes[1].frame == 12);
}

TEST_CASE("content_probe per-target resolve summaries", "[content_probe]") {
  using rex::graphics::content_probe::ProbeCounters;

  ProbeCounters counters;
  // No appends: all summaries empty (count is the presence flag).
  for (uint32_t i = 0; i < rex::graphics::content_probe::kProbeTargetCount; ++i) {
    CHECK(counters.resolve_summary[i].count == 0);
    CHECK(counters.resolve_summary[i].first_frame == 0);
    CHECK(counters.resolve_summary[i].last_frame == 0);
  }

  counters.AppendResolveFrame(1, 500);  // kScene0
  counters.AppendResolveFrame(1, 501);
  counters.AppendResolveFrame(0, 499);  // kFront
  counters.AppendResolveFrame(2, 502);  // kScene1
  counters.AppendResolveFrame(2, 503);
  counters.AppendResolveFrame(2, 504);

  CHECK(counters.resolve_summary[0].first_frame == 499);
  CHECK(counters.resolve_summary[0].last_frame == 499);
  CHECK(counters.resolve_summary[0].count == 1);
  CHECK(counters.resolve_summary[1].first_frame == 500);
  CHECK(counters.resolve_summary[1].last_frame == 501);
  CHECK(counters.resolve_summary[1].count == 2);
  CHECK(counters.resolve_summary[2].first_frame == 502);
  CHECK(counters.resolve_summary[2].last_frame == 504);
  CHECK(counters.resolve_summary[2].count == 3);

  // Out-of-range targets are ignored (defensive).
  counters.AppendResolveFrame(7, 999);
  CHECK(counters.resolve_summary[2].count == 3);
}

TEST_CASE("content_probe counter reset re-arms rings and summaries", "[content_probe]") {
  using rex::graphics::content_probe::ProbeCounters;

  ProbeCounters counters;
  for (uint32_t i = 0; i < ProbeCounters::kMaxClearWriteEntries + 3; ++i) {
    counters.OnRbColorClearWrite(0, 10 + i);
    counters.OnRbColorClearLoWrite(0x10101010u, 20 + i);
  }
  counters.AppendResolveFrame(0, 100);
  counters.AppendResolveFrame(1, 200);
  counters.AppendResolveFrame(2, 300);
  counters.OnResolveClearExecuted();
  counters.OnDrawPacket();

  counters.Reset();

  CHECK(counters.rbcc == 0);
  CHECK(counters.rbcc_lo == 0);
  CHECK(counters.rbcc_zero == 0);
  CHECK(counters.rbcc_val == 0);
  CHECK(counters.rbcc_lo_val == 0);
  CHECK(counters.clears == 0);
  CHECK(counters.draws == 0);
  CHECK(counters.rbcc_writes_count == 0);
  CHECK(counters.rbcc_lo_writes_count == 0);
  CHECK(counters.rbcc_writes_overflow == 0);
  CHECK(counters.rbcc_lo_writes_overflow == 0);
  for (const auto& entry : counters.rbcc_writes) {
    CHECK(entry.frame == 0);
    CHECK(entry.value == 0);
  }
  for (const auto& entry : counters.rbcc_lo_writes) {
    CHECK(entry.frame == 0);
    CHECK(entry.value == 0);
  }
  for (const auto& summary : counters.resolve_summary) {
    CHECK(summary.first_frame == 0);
    CHECK(summary.last_frame == 0);
    CHECK(summary.count == 0);
  }

  // Re-armed: a fresh write lands in slot 0.
  counters.OnRbColorClearWrite(0x55555555u, 77);
  CHECK(counters.rbcc_writes_count == 1);
  CHECK(counters.rbcc_writes[0].frame == 77);
  CHECK(counters.rbcc_writes[0].value == 0x55555555u);
  CHECK(counters.rbcc_writes_overflow == 0);
}

