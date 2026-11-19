/**
 * @file        content_probe.cpp
 * @brief       Implementation of host-side content analysis helpers
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <rex/graphics/util/content_probe.h>

#include <rex/graphics/pipeline/texture/util.h>

namespace rex::graphics::content_probe {

ProbeTargetId MatchProbeTarget(uint32_t extent_start, uint32_t extent_end) {
  if (extent_end <= extent_start) {
    return ProbeTargetId::kNone;
  }
  struct Range {
    uint32_t start;
    uint32_t end;
    ProbeTargetId id;
  };
  static constexpr Range kRanges[] = {
      {kFrontRangeStart, kFrontRangeEnd, ProbeTargetId::kFront},
      {kScene0RangeStart, kScene0RangeEnd, ProbeTargetId::kScene0},
      {kScene1RangeStart, kScene1RangeEnd, ProbeTargetId::kScene1},
  };
  for (const Range& range : kRanges) {
    if (extent_start >= range.start && extent_end <= range.end) {
      return range.id;
    }
  }
  return ProbeTargetId::kNone;
}

uint32_t ProbeTargetBase(ProbeTargetId target) {
  switch (target) {
    case ProbeTargetId::kFront:
      return kFrontRangeStart;
    case ProbeTargetId::kScene0:
      return kScene0RangeStart;
    case ProbeTargetId::kScene1:
      return kScene1RangeStart;
    default:
      return 0;
  }
}

bool IsFullSurfaceResolve(uint32_t width, uint32_t height, ProbeTargetId target) {
  switch (target) {
    case ProbeTargetId::kFront:
      return width == kFrontWidth && height == kFrontHeight;
    case ProbeTargetId::kScene0:
      return width == kScene0Width && height == kScene0Height;
    case ProbeTargetId::kScene1:
      return width == kScene1Width && height == kScene1Height;
    default:
      return false;
  }
}

bool IsClockBoxSampleable(uint32_t width, uint32_t height) {
  return width >= kClockBoxX + kClockBoxWidth && height >= kClockBoxY + kClockBoxHeight;
}

void ProbeSlot::Reset() { *this = ProbeSlot{}; }

uint32_t compute_16bpp_luma_q8(const uint8_t* pixel8) {
  if (!pixel8) {
    return 0;
  }
  // 4 little-endian 16-bit channels; the high byte of each is the coarse value.
  return (uint32_t(pixel8[1]) + uint32_t(pixel8[3]) + uint32_t(pixel8[5]) +
          uint32_t(pixel8[7])) /
         4u * 256u;
}

uint32_t fnv1a32(const uint8_t* data, size_t length) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 16777619u;  // FNV prime
  }
  return hash;
}

uint32_t compute_luma_q8(const uint8_t* rgba8, uint32_t width, uint32_t height,
                         uint32_t stride) {
  if (!rgba8 || width == 0 || height == 0) return 0;
  uint64_t luma_sum = 0;
  uint32_t count = 0;
  for (uint32_t y = 0; y < height; y += stride) {
    const uint8_t* row = rgba8 + y * width * 4;
    for (uint32_t x = 0; x < width; x += stride) {
      uint32_t r = row[x * 4 + 0];
      uint32_t g = row[x * 4 + 1];
      uint32_t b = row[x * 4 + 2];
      // Q8.8 fixed-point luma: (299*R + 587*G + 114*B) / 1000, scaled by 256
      luma_sum += (299u * r + 587u * g + 114u * b) * 256u / 1000u;
      ++count;
    }
  }
  return count > 0 ? static_cast<uint32_t>(luma_sum / count) : 0;
}

uint32_t compute_hash(const uint8_t* rgba8, uint32_t width, uint32_t height,
                      uint32_t stride) {
  if (!rgba8 || width == 0 || height == 0) return 0;
  // Hash just the RGB components of sampled pixels (3 bytes each)
  size_t sample_count = 0;
  for (uint32_t y = 0; y < height; y += stride) {
    const uint8_t* row = rgba8 + y * width * 4;
    for (uint32_t x = 0; x < width; x += stride) {
      ++sample_count;
    }
  }
  // Build a compact byte array of sampled RGB triples
  // For efficiency, hash in-place over the original buffer with stride
  uint32_t hash = 2166136261u;
  for (uint32_t y = 0; y < height; y += stride) {
    const uint8_t* row = rgba8 + y * width * 4;
    for (uint32_t x = 0; x < width; x += stride) {
      uint32_t idx = x * 4;
      hash ^= row[idx + 0]; hash *= 16777619u;
      hash ^= row[idx + 1]; hash *= 16777619u;
      hash ^= row[idx + 2]; hash *= 16777619u;
    }
  }
  return hash;
}

uint32_t compute_byte_mean(const uint8_t* data, uint32_t width, uint32_t height,
                           uint32_t stride) {
  if (!data || width == 0 || height == 0) return 0;
  uint64_t sum = 0;
  uint32_t count = 0;
  for (uint32_t y = 0; y < height; y += stride) {
    const uint8_t* row = data + y * width;
    for (uint32_t x = 0; x < width; x += stride) {
      sum += row[x];
      ++count;
    }
  }
  return count > 0 ? static_cast<uint32_t>(sum / count) : 0;
}

uint32_t compute_byte_hash(const uint8_t* data, uint32_t width, uint32_t height,
                           uint32_t stride) {
  if (!data || width == 0 || height == 0) return 0;
  uint32_t hash = 2166136261u;
  for (uint32_t y = 0; y < height; y += stride) {
    const uint8_t* row = data + y * width;
    for (uint32_t x = 0; x < width; x += stride) {
      hash ^= row[x];
      hash *= 16777619u;
    }
  }
  return hash;
}

void SampleTiled(uint8_t bpp_log2, const uint8_t* tiled_base, uint32_t x0, uint32_t y0,
                 uint32_t width, uint32_t height, uint32_t pitch, uint32_t stride,
                 uint32_t& luma_q8_out, uint32_t& hash_out) {
  luma_q8_out = 0;
  hash_out = 0;
  if (!tiled_base || !width || !height || !pitch || !stride) {
    return;
  }
  if (bpp_log2 != 0 && bpp_log2 != 2 && bpp_log2 != 3) {
    return;
  }
  const uint32_t bytes_per_pixel = 1u << bpp_log2;
  uint64_t luma_sum = 0;
  uint32_t count = 0;
  uint32_t hash = 2166136261u;
  for (uint32_t y = y0; y < y0 + height; y += stride) {
    for (uint32_t x = x0; x < x0 + width; x += stride) {
      uint32_t offset = uint32_t(
          texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, bpp_log2));
      const uint8_t* pixel = tiled_base + offset;
      switch (bpp_log2) {
        case 0: {
          // k_8: mean byte.
          luma_sum += pixel[0];
          hash ^= pixel[0];
          hash *= 16777619u;
        } break;
        case 2: {
          // k_8_8_8_8: Q8.8 RGB luma over the sampled pixels.
          luma_sum +=
              (299u * pixel[0] + 587u * pixel[1] + 114u * pixel[2]) * 256u / 1000u;
          hash ^= pixel[0];
          hash *= 16777619u;
          hash ^= pixel[1];
          hash *= 16777619u;
          hash ^= pixel[2];
          hash *= 16777619u;
        } break;
        case 3: {
          // k_16_16_16_16: Q8.8 luma of the four channel high bytes; hash all
          // 8 bytes of the pixel in ascending order.
          luma_sum += compute_16bpp_luma_q8(pixel);
          for (uint32_t i = 0; i < bytes_per_pixel; ++i) {
            hash ^= pixel[i];
            hash *= 16777619u;
          }
        } break;
      }
      ++count;
    }
  }
  luma_q8_out = count ? static_cast<uint32_t>(luma_sum / count) : 0;
  hash_out = count ? hash : 0;
}

}  // namespace rex::graphics::content_probe
