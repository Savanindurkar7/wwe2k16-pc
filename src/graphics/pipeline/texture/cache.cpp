/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <atomic>
#include <cstdint>
#include <utility>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_render_to_texture, 24, "GPU",
                     "Texture cache memory limit for render-to-texture (MB)")
    .range(1, 256)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_soft, 384, "GPU",
                     "Soft texture cache memory limit (MB)")
    .range(64, 4096)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_hard, 768, "GPU",
                     "Hard texture cache memory limit (MB)")
    .range(128, 8192)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_soft_lifetime, 30, "GPU",
                     "Soft texture cache memory limit lifetime (seconds)")
    .range(1, 3600);

REXCVAR_DEFINE_BOOL(gpu_3d_to_2d_texture, true, "GPU",
                    "Sample problematic 3D textures through 2D-compatible wrappers")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(anisotropic_override, 3, "GPU",
                     "Forces anisotropic filtering for eligible textures.\n"
                     "Higher values keep textures sharper at oblique angles, but increase texture "
                     "sampling cost.\n"
                     " -1 = No override\n"
                     "  0 = Disable anisotropic filtering\n"
                     "  1 = Force 1x anisotropic filtering\n"
                     "  2 = Force 2x anisotropic filtering\n"
                     "  3 = Force 4x anisotropic filtering\n"
                     "  4 = Force 8x anisotropic filtering\n"
                     "  5 = Force 16x anisotropic filtering")
    .range(-1, 5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(draw_resolution_scale_x, 1, "GPU", "Draw resolution scale X (1 = no scaling)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(draw_resolution_scale_y, 1, "GPU", "Draw resolution scale Y (1 = no scaling)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(resolution_scale, 1, "GPU",
                     "Draw resolution scale for both X and Y axes (same as setting "
                     "draw_resolution_scale_x and draw_resolution_scale_y)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// DEFINE_int32(
//     draw_resolution_scale_x, 1,
//     "Integer pixel width scale used for scaling the rendering resolution "
//     "opaquely to the game.\n"
//     "1, 2 and 3 may be supported, but support of anything above 1 depends on "
//     "the device properties, such as whether it supports sparse binding / tiled "
//     "resources, the number of virtual address bits per resource, and other "
//     "factors.\n"
//     "Various effects and parts of game rendering pipelines may work "
//     "incorrectly as pixels become ambiguous from the game's perspective and "
//     "because half-pixel offset (which normally doesn't affect coverage when "
//     "MSAA isn't used) becomes full-pixel.",
//     "GPU");
// DEFINE_int32(
//     draw_resolution_scale_y, 1,
//     "Integer pixel width scale used for scaling the rendering resolution "
//     "opaquely to the game.\n"
//     "See draw_resolution_scale_x for more information.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_soft, 384,
//     "Maximum host texture memory usage (in megabytes) above which old textures "
//     "will be destroyed.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_soft_lifetime, 30,
//     "Seconds a texture should be unused to be considered old enough to be "
//     "deleted if texture memory usage exceeds texture_cache_memory_limit_soft.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_hard, 768,
//     "Maximum host texture memory usage (in megabytes) above which textures "
//     "will be destroyed as soon as possible.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_render_to_texture, 24,
//     "Part of the host texture memory budget (in megabytes) that will be scaled "
//     "by the current drawing resolution scale.\n"
//     "If texture_cache_memory_limit_soft, for instance, is 384, and this is 24, "
//     "it will be assumed that the game will be using roughly 24 MB of "
//     "render-to-texture (resolve) targets and 384 - 24 = 360 MB of regular "
//     "textures - so with 2x2 resolution scaling, the soft limit will be 360 + "
//     "96 MB, and with 3x3, it will be 360 + 216 MB.",
//     "GPU");

namespace rex::graphics {

const TextureCache::LoadShaderInfo TextureCache::load_shader_info_[kLoadShaderCount] = {
    // k8bpb
    {3, 4, 1, 4},
    // k16bpb
    {4, 4, 2, 4},
    // k32bpb
    {4, 4, 4, 3},
    // k64bpb
    {4, 4, 8, 2},
    // k128bpb
    {4, 4, 16, 1},
    // kR5G5B5A1ToB5G5R5A1
    {4, 4, 2, 4},
    // kR5G6B5ToB5G6R5
    {4, 4, 2, 4},
    // kR5G5B6ToB5G6R5WithRBGASwizzle
    {4, 4, 2, 4},
    // kRGBA4ToBGRA4
    {4, 4, 2, 4},
    // kRGBA4ToARGB4
    {4, 4, 2, 4},
    // kGBGR8ToGRGB8
    {4, 4, 4, 3},
    // kGBGR8ToRGB8
    {4, 4, 8, 3},
    // kBGRG8ToRGBG8
    {4, 4, 4, 3},
    // kBGRG8ToRGB8
    {4, 4, 8, 3},
    // kR10G11B11ToRGBA16
    {4, 4, 8, 3},
    // kR10G11B11ToRGBA16SNorm
    {4, 4, 8, 3},
    // kR11G11B10ToRGBA16
    {4, 4, 8, 3},
    // kR11G11B10ToRGBA16SNorm
    {4, 4, 8, 3},
    // kR16UNormToFloat
    {4, 4, 2, 4},
    // kR16SNormToFloat
    {4, 4, 2, 4},
    // kRG16UNormToFloat
    {4, 4, 4, 3},
    // kRG16SNormToFloat
    {4, 4, 4, 3},
    // kRGBA16UNormToFloat
    {4, 4, 8, 2},
    // kRGBA16SNormToFloat
    {4, 4, 8, 2},
    // kDXT1ToRGBA8
    {4, 4, 4, 2},
    // kDXT3ToRGBA8
    {4, 4, 4, 1},
    // kDXT5ToRGBA8
    {4, 4, 4, 1},
    // kDXNToRG8
    {4, 4, 2, 1},
    // kDXT3A
    {4, 4, 1, 2},
    // kDXT3AAs1111ToBGRA4
    {4, 4, 2, 2},
    // kDXT3AAs1111ToARGB4
    {4, 4, 2, 2},
    // kDXT5AToR8
    {4, 4, 1, 2},
    // kCTX1
    {4, 4, 2, 2},
    // kDepthUnorm
    {4, 4, 4, 3},
    // kDepthFloat
    {4, 4, 4, 3},
};

TextureCache::TextureCache(const RegisterFile& register_file, SharedMemory& shared_memory,
                           uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y)
    : register_file_(register_file),
      shared_memory_(shared_memory),
      draw_resolution_scale_x_(draw_resolution_scale_x),
      draw_resolution_scale_y_(draw_resolution_scale_y) {
  assert_true(draw_resolution_scale_x >= 1);
  assert_true(draw_resolution_scale_x <= kMaxDrawResolutionScaleAlongAxis);
  assert_true(draw_resolution_scale_y >= 1);
  assert_true(draw_resolution_scale_y <= kMaxDrawResolutionScaleAlongAxis);

  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    constexpr uint32_t kScaledResolvePageDwordCount = SharedMemory::kBufferSize / 4096 / 32;
    scaled_resolve_pages_ = std::unique_ptr<uint32_t[]>(new uint32_t[kScaledResolvePageDwordCount]);
    std::memset(scaled_resolve_pages_.get(), 0, kScaledResolvePageDwordCount * sizeof(uint32_t));
    std::memset(scaled_resolve_pages_l2_, 0, sizeof(scaled_resolve_pages_l2_));
    scaled_resolve_global_watch_handle_ =
        shared_memory.RegisterGlobalWatch(ScaledResolveGlobalWatchCallbackThunk, this);
  }
}

TextureCache::~TextureCache() {
  DestroyAllTextures(true);

  if (scaled_resolve_global_watch_handle_) {
    shared_memory().UnregisterGlobalWatch(scaled_resolve_global_watch_handle_);
  }
}

bool TextureCache::GetConfigDrawResolutionScale(uint32_t& x_out, uint32_t& y_out) {
  uint32_t shared_scale = uint32_t(std::max(INT32_C(1), REXCVAR_GET(resolution_scale)));
  bool use_shared_scale = rex::cvar::HasNonDefaultValue("resolution_scale");
  uint32_t config_x = use_shared_scale && !rex::cvar::HasNonDefaultValue("draw_resolution_scale_x")
                          ? shared_scale
                          : uint32_t(std::max(INT32_C(1), REXCVAR_GET(draw_resolution_scale_x)));
  uint32_t config_y = use_shared_scale && !rex::cvar::HasNonDefaultValue("draw_resolution_scale_y")
                          ? shared_scale
                          : uint32_t(std::max(INT32_C(1), REXCVAR_GET(draw_resolution_scale_y)));
  uint32_t clamped_x = std::min(kMaxDrawResolutionScaleAlongAxis, config_x);
  uint32_t clamped_y = std::min(kMaxDrawResolutionScaleAlongAxis, config_y);
  x_out = clamped_x;
  y_out = clamped_y;
  return clamped_x == config_x && clamped_y == config_y;
}

void TextureCache::ClearCache() {
  DestroyAllTextures();
}

void TextureCache::CompletedSubmissionUpdated(uint64_t completed_submission_index) {
  // If memory usage is too high, destroy unused textures.
  uint64_t current_time = rex::chrono::Clock::QueryHostUptimeMillis();
  // texture_cache_memory_limit_render_to_texture is assumed to be included in
  // texture_cache_memory_limit_soft and texture_cache_memory_limit_hard, at 1x,
  // so subtracting 1 from the scale.
  uint32_t limit_scaled_resolve_add_mb =
      REXCVAR_GET(texture_cache_memory_limit_render_to_texture) *
      (draw_resolution_scale_x() * draw_resolution_scale_y() - 1);
  uint32_t limit_soft_mb =
      REXCVAR_GET(texture_cache_memory_limit_soft) + limit_scaled_resolve_add_mb;
  uint32_t limit_hard_mb =
      REXCVAR_GET(texture_cache_memory_limit_hard) + limit_scaled_resolve_add_mb;
  uint32_t limit_soft_lifetime = REXCVAR_GET(texture_cache_memory_limit_soft_lifetime) * 1000;
  bool destroyed_any = false;
  uint32_t destroyed_over_hard = 0;
  while (texture_used_first_ != nullptr) {
    uint64_t total_host_memory_usage_mb =
        (textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20;
    bool limit_hard_exceeded = total_host_memory_usage_mb > limit_hard_mb;
    if (total_host_memory_usage_mb <= limit_soft_mb && !limit_hard_exceeded) {
      break;
    }
    Texture* texture = texture_used_first_;
    if (texture->last_usage_submission_index() > completed_submission_index) {
      break;
    }
    if (!limit_hard_exceeded && (texture->last_usage_time() + limit_soft_lifetime) > current_time) {
      break;
    }
    if (limit_hard_exceeded) {
      ++destroyed_over_hard;
    }
    if (!destroyed_any) {
      destroyed_any = true;
      // The texture being destroyed might have been bound in the previous
      // submissions, and nothing has overwritten the binding yet, so completion
      // of the submission where the texture was last actually used on the GPU
      // doesn't imply that it's not bound currently. Reset bindings if
      // any texture has been destroyed.
      ResetTextureBindings();
    }
    // Remove the texture from the map and destroy it via its unique_ptr.
    auto found_texture_it = textures_.find(texture->key());
    assert_true(found_texture_it != textures_.end());
    if (found_texture_it != textures_.end()) {
      assert_true(found_texture_it->second.get() == texture);
      textures_.erase(found_texture_it);
      // `texture` is invalid now.
    }
  }
  if (destroyed_any) {
    COUNT_profile_set("gpu/texture_cache/textures", textures_.size());
  }
  // Textures destroyed because the hard limit was exceeded are reloaded as soon as they are used again: at a high
  // resolution scale this can mean reloading render-to-texture targets every frame. Logged every 5 s at most.
  if (destroyed_over_hard) {
    static uint64_t last_log_ms = 0;
    static uint32_t pending = 0;
    pending += destroyed_over_hard;
    if (current_time - last_log_ms >= 5000) {
      REXGPU_WARN("[texture-cache] {} textures evicted over the hard limit in {} s (usage {} MB, hard limit {} MB)",
                  pending, last_log_ms ? (current_time - last_log_ms) / 1000 : 0,
                  (textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20, limit_hard_mb);
      pending = 0;
      last_log_ms = current_time;
    }
  }
}

void TextureCache::BeginSubmission(uint64_t new_submission_index) {
  assert_true(new_submission_index > current_submission_index_);
  current_submission_index_ = new_submission_index;
  current_submission_time_ = rex::chrono::Clock::QueryHostUptimeMillis();
}

void TextureCache::BeginFrame() {
  // In case there was a failure to create something in the previous frame, make
  // sure bindings are reset so a new attempt will surely be made if the texture
  // is requested again.
  ResetTextureBindings();
}

void TextureCache::MarkRangeAsResolved(uint32_t start_unscaled, uint32_t length_unscaled,
                                       uint32_t pixel_size_log2, bool scaled) {
  if (length_unscaled == 0) {
    return;
  }
  start_unscaled &= 0x1FFFFFFF;
  length_unscaled = std::min(length_unscaled, 0x20000000 - start_unscaled);

  if (!IsDrawResolutionScaled()) {
    shared_memory().RangeWrittenByGpu(start_unscaled, length_unscaled);
    return;
  }
  if (!scaled || pixel_size_log2 > 3) {
    MarkRangeAsUnscaledGpuWrite(start_unscaled, length_unscaled);
    return;
  }
  // Keep invalidation and replacement of the scaled-resolve metadata atomic
  // with respect to CPU memory writes and range queries.
  auto global_lock = global_critical_region_.Acquire();
  shared_memory().RangeWrittenByGpu(start_unscaled, length_unscaled);

  {
    uint32_t page_first = start_unscaled >> 12;
    uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
    uint32_t block_first = page_first >> 5;
    uint32_t block_last = page_last >> 5;
    for (uint32_t i = block_first; i <= block_last; ++i) {
      uint32_t add_bits = UINT32_MAX;
      if (i == block_first) {
        add_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (i == block_last && (page_last & 31) != 31) {
        add_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      scaled_resolve_pages_[i] |= add_bits;
      scaled_resolve_pages_l2_[i >> 6] |= UINT64_C(1) << (i & 63);
    }

    const uint32_t end_unscaled = start_unscaled + length_unscaled;
    RemoveUnscaledGpuWriteRanges(start_unscaled, end_unscaled - 1);
    std::vector<ScaledResolveRange> updated_ranges;
    updated_ranges.reserve(scaled_resolve_ranges_.size() + 1);
    for (const ScaledResolveRange& range : scaled_resolve_ranges_) {
      const uint32_t range_end = range.start + range.length;
      if (range_end <= start_unscaled || range.start >= end_unscaled) {
        updated_ranges.push_back(range);
        continue;
      }
      if (range.start < start_unscaled) {
        updated_ranges.push_back(
            {range.start, start_unscaled - range.start, range.pixel_size_log2});
      }
      if (range_end > end_unscaled) {
        updated_ranges.push_back(
            {end_unscaled, range_end - end_unscaled, range.pixel_size_log2});
      }
    }
    auto insertion = std::lower_bound(
        updated_ranges.begin(), updated_ranges.end(), start_unscaled,
        [](const ScaledResolveRange& range, uint32_t start) { return range.start < start; });
    updated_ranges.insert(insertion, {start_unscaled, length_unscaled, pixel_size_log2});

    scaled_resolve_ranges_.clear();
    scaled_resolve_ranges_.reserve(updated_ranges.size());
    for (const ScaledResolveRange& range : updated_ranges) {
      if (!scaled_resolve_ranges_.empty()) {
        ScaledResolveRange& previous = scaled_resolve_ranges_.back();
        if (previous.start + previous.length == range.start &&
            previous.pixel_size_log2 == range.pixel_size_log2) {
          previous.length += range.length;
          continue;
        }
      }
      scaled_resolve_ranges_.push_back(range);
    }
  }
}

void TextureCache::GetScaledResolveRanges(uint32_t start_unscaled, uint32_t length_unscaled,
                                          std::vector<ScaledResolveRange>& ranges_out) {
  ranges_out.clear();
  if (!IsDrawResolutionScaled() || !length_unscaled ||
      start_unscaled >= SharedMemory::kBufferSize) {
    return;
  }
  length_unscaled = std::min(length_unscaled, SharedMemory::kBufferSize - start_unscaled);
  const uint64_t end_unscaled = uint64_t(start_unscaled) + length_unscaled;
  auto global_lock = global_critical_region_.Acquire();
  for (const ScaledResolveRange& range : scaled_resolve_ranges_) {
    const uint64_t range_end = uint64_t(range.start) + range.length;
    if (range_end <= start_unscaled) {
      continue;
    }
    if (range.start >= end_unscaled) {
      break;
    }
    uint32_t intersection_start = std::max(start_unscaled, range.start);
    uint32_t intersection_end = uint32_t(std::min(end_unscaled, range_end));
    ranges_out.push_back(
        {intersection_start, intersection_end - intersection_start, range.pixel_size_log2});
  }
}

void TextureCache::GetUnscaledGpuWriteRanges(
    uint32_t start_unscaled, uint32_t length_unscaled,
    std::vector<std::pair<uint32_t, uint32_t>>& ranges_out) {
  ranges_out.clear();
  if (!IsDrawResolutionScaled() || !length_unscaled ||
      start_unscaled >= SharedMemory::kBufferSize) {
    return;
  }
  length_unscaled = std::min(length_unscaled, SharedMemory::kBufferSize - start_unscaled);
  const uint64_t end_unscaled = uint64_t(start_unscaled) + length_unscaled;
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& range : unscaled_gpu_write_ranges_) {
    const uint64_t range_end = uint64_t(range.first) + range.second;
    if (range_end <= start_unscaled) {
      continue;
    }
    if (range.first >= end_unscaled) {
      break;
    }
    uint32_t intersection_start = std::max(start_unscaled, range.first);
    uint32_t intersection_end = uint32_t(std::min(end_unscaled, range_end));
    ranges_out.emplace_back(intersection_start, intersection_end - intersection_start);
  }
}

void TextureCache::RemoveScaledResolveRanges(uint32_t address_first, uint32_t address_last) {
  const uint64_t invalidation_end = uint64_t(address_last) + 1;
  std::vector<ScaledResolveRange> updated_ranges;
  updated_ranges.reserve(scaled_resolve_ranges_.size());
  for (const ScaledResolveRange& range : scaled_resolve_ranges_) {
    const uint64_t range_end = uint64_t(range.start) + range.length;
    if (range_end <= address_first || range.start >= invalidation_end) {
      updated_ranges.push_back(range);
      continue;
    }
    if (range.start < address_first) {
      updated_ranges.push_back(
          {range.start, address_first - range.start, range.pixel_size_log2});
    }
    if (range_end > invalidation_end) {
      updated_ranges.push_back({uint32_t(invalidation_end),
                                uint32_t(range_end - invalidation_end), range.pixel_size_log2});
    }
  }
  scaled_resolve_ranges_.swap(updated_ranges);
}

void TextureCache::RemoveUnscaledGpuWriteRanges(uint32_t address_first, uint32_t address_last) {
  const uint64_t invalidation_end = uint64_t(address_last) + 1;
  std::vector<std::pair<uint32_t, uint32_t>> updated_ranges;
  updated_ranges.reserve(unscaled_gpu_write_ranges_.size());
  for (const auto& range : unscaled_gpu_write_ranges_) {
    const uint64_t range_end = uint64_t(range.first) + range.second;
    if (range_end <= address_first || range.first >= invalidation_end) {
      updated_ranges.push_back(range);
      continue;
    }
    if (range.first < address_first) {
      updated_ranges.emplace_back(range.first, address_first - range.first);
    }
    if (range_end > invalidation_end) {
      updated_ranges.emplace_back(uint32_t(invalidation_end),
                                  uint32_t(range_end - invalidation_end));
    }
  }
  unscaled_gpu_write_ranges_.swap(updated_ranges);
}

void TextureCache::MarkRangeAsUnscaledGpuWrite(uint32_t start_unscaled,
                                               uint32_t length_unscaled) {
  if (!IsDrawResolutionScaled() || !length_unscaled ||
      start_unscaled >= SharedMemory::kBufferSize) {
    if (!IsDrawResolutionScaled()) {
      shared_memory().RangeWrittenByGpu(start_unscaled, length_unscaled);
    }
    return;
  }
  length_unscaled = std::min(length_unscaled, SharedMemory::kBufferSize - start_unscaled);
  uint32_t page_first = start_unscaled >> 12;
  uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
  auto global_lock = global_critical_region_.Acquire();
  shared_memory().RangeWrittenByGpu(start_unscaled, length_unscaled);

  if (!start_unscaled && length_unscaled == SharedMemory::kBufferSize) {
    scaled_resolve_ranges_.clear();
    unscaled_gpu_write_ranges_.clear();
    unscaled_gpu_write_ranges_.emplace_back(0, SharedMemory::kBufferSize);
    std::memset(scaled_resolve_pages_.get(), 0,
                (SharedMemory::kBufferSize / 4096 / 32) * sizeof(uint32_t));
    std::memset(scaled_resolve_pages_l2_, 0, sizeof(scaled_resolve_pages_l2_));
    return;
  }

  RemoveScaledResolveRanges(start_unscaled, start_unscaled + length_unscaled - 1);
  RemoveUnscaledGpuWriteRanges(start_unscaled, start_unscaled + length_unscaled - 1);
  for (uint32_t page = page_first; page <= page_last; ++page) {
    uint32_t block = page >> 5;
    scaled_resolve_pages_[block] &= ~(UINT32_C(1) << (page & 31));
    if (!scaled_resolve_pages_[block]) {
      scaled_resolve_pages_l2_[block >> 6] &= ~(UINT64_C(1) << (block & 63));
    }
  }

  uint64_t page_range_start = uint64_t(page_first) << 12;
  uint64_t page_range_end = (uint64_t(page_last) + 1) << 12;
  for (const ScaledResolveRange& range : scaled_resolve_ranges_) {
    uint64_t range_end = uint64_t(range.start) + range.length;
    if (range_end <= page_range_start || range.start >= page_range_end) {
      continue;
    }
    uint32_t surviving_page_first = std::max(page_first, range.start >> 12);
    uint32_t surviving_page_last =
        std::min(page_last, uint32_t((range_end - 1) >> 12));
    for (uint32_t page = surviving_page_first; page <= surviving_page_last; ++page) {
      uint32_t block = page >> 5;
      scaled_resolve_pages_[block] |= UINT32_C(1) << (page & 31);
      scaled_resolve_pages_l2_[block >> 6] |= UINT64_C(1) << (block & 63);
    }
  }

  uint32_t end_unscaled = start_unscaled + length_unscaled;
  auto insertion = std::lower_bound(
      unscaled_gpu_write_ranges_.begin(), unscaled_gpu_write_ranges_.end(), start_unscaled,
      [](const std::pair<uint32_t, uint32_t>& range, uint32_t start) {
        return range.first < start;
      });
  unscaled_gpu_write_ranges_.insert(insertion, {start_unscaled, length_unscaled});
  std::vector<std::pair<uint32_t, uint32_t>> merged_ranges;
  merged_ranges.reserve(unscaled_gpu_write_ranges_.size());
  for (const auto& range : unscaled_gpu_write_ranges_) {
    if (!merged_ranges.empty() &&
        uint64_t(merged_ranges.back().first) + merged_ranges.back().second == range.first) {
      merged_ranges.back().second += range.second;
    } else {
      merged_ranges.push_back(range);
    }
  }
  unscaled_gpu_write_ranges_.swap(merged_ranges);
}

uint32_t TextureCache::GuestToHostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle) {
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t guest_swizzle_component = (guest_swizzle >> (3 * i)) & 0b111;
    uint32_t host_swizzle_component;
    if (guest_swizzle_component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
      // Get rid of 6 and 7 values (to prevent host GPU errors if the game has
      // something broken) the simple way - by changing them to 4 (0) and 5 (1).
      host_swizzle_component = guest_swizzle_component & 0b101;
    } else {
      host_swizzle_component = (host_format_swizzle >> (3 * guest_swizzle_component)) & 0b111;
    }
    host_swizzle |= host_swizzle_component << (3 * i);
  }
  return host_swizzle;
}

bool TextureCache::PrepareTextureLoad(Texture& texture, PendingTextureLoad& pending_load_out,
                                      PendingSharedMemoryRange* pending_ranges_out,
                                      size_t& pending_range_count_out) {
  uint32_t outdated_mask = texture.outdated_mask();
  if (!outdated_mask) {
    return false;
  }

  bool base_outdated = false;
  bool mips_outdated = false;
  {
    auto global_lock = global_critical_region_.Acquire();
    if (outdated_mask & Texture::kOutdatedBitBase) {
      base_outdated = texture.base_outdated(global_lock);
    }
    if (outdated_mask & Texture::kOutdatedBitMips) {
      mips_outdated = texture.mips_outdated(global_lock);
    }
  }
  if (!base_outdated && !mips_outdated) {
    return false;
  }

  pending_load_out.texture = &texture;
  pending_load_out.load_base = base_outdated;
  pending_load_out.load_mips = mips_outdated;
  pending_range_count_out = 0;

  TextureKey texture_key = texture.key();
  if (base_outdated) {
    PendingSharedMemoryRange pending_range;
    pending_range.start = texture_key.base_page << 12;
    pending_range.length =
        static_cast<uint32_t>(rex::align(texture.GetGuestBaseSize(), UINT32_C(16)));
    pending_ranges_out[pending_range_count_out++] = pending_range;
  }
  if (mips_outdated) {
    PendingSharedMemoryRange pending_range;
    pending_range.start = texture_key.mip_page << 12;
    pending_range.length =
        static_cast<uint32_t>(rex::align(texture.GetGuestMipsSize(), UINT32_C(16)));
    pending_ranges_out[pending_range_count_out++] = pending_range;
  }

  return true;
}

bool TextureCache::CommitPreparedTextureLoad(const PendingTextureLoad& pending_load) {
  if (!pending_load.texture || (!pending_load.load_base && !pending_load.load_mips)) {
    return true;
  }

  Texture& texture = *pending_load.texture;
  TextureKey texture_key = texture.key();
  if (texture_key.scaled_resolve) {
    // Make sure all the scaled resolve memory is resident and accessible from
    // the shader, including any possible padding that hasn't yet been touched
    // by an actual resolve, but is still included in the texture size, so the
    // GPU won't be trying to access unmapped memory.
    if (pending_load.load_base && !EnsureScaledResolveMemoryCommitted(
                                      texture_key.base_page << 12, texture.GetGuestBaseSize(), 4)) {
      return false;
    }
    if (pending_load.load_mips && !EnsureScaledResolveMemoryCommitted(
                                      texture_key.mip_page << 12, texture.GetGuestMipsSize(), 4)) {
      return false;
    }
  }

  if (!LoadTextureDataFromResidentMemoryImpl(texture, pending_load.load_base,
                                             pending_load.load_mips)) {
    return false;
  }

  // Mark the ranges as uploaded and watch them. This is needed for scaled
  // resolves as well to detect when the CPU wants to reuse the memory for a
  // regular texture or a vertex buffer, and thus the scaled resolve version is
  // not up to date anymore.
  texture.MakeUpToDateAndWatch(global_critical_region_.Acquire());
  texture.LogAction("Loaded");

  // Diagnostic (WWE2K16_GPU_STATS=1): every 5 s, how many textures were (re)loaded and which ones most often.
  static const bool gpu_stats = [] {
    const char* v = std::getenv("WWE2K16_GPU_STATS");
    return v && v[0] == '1';
  }();
  if (gpu_stats) {
    static std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, uint32_t> loads;
    static uint64_t window_start = 0;
    static uint64_t bytes = 0;
    loads[{uint32_t(texture_key.width_minus_1) + 1, uint32_t(texture_key.height_minus_1) + 1, uint32_t(texture_key.format),
           uint32_t(texture_key.scaled_resolve), uint32_t(texture_key.base_page)}]++;
    bytes += texture.GetGuestBaseSize();
    const uint64_t now = rex::chrono::Clock::QueryHostUptimeMillis();
    if (!window_start) window_start = now;
    if (now - window_start >= 5000) {
      std::vector<std::pair<uint32_t, std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>>> top;
      uint32_t total = 0, scaled = 0;
      for (auto& [k, c] : loads) {
        top.push_back({c, k});
        total += c;
        if (std::get<3>(k)) scaled += c;
      }
      std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
      std::string s;
      for (size_t i = 0; i < top.size() && i < 8; ++i) {
        auto& [c, k] = top[i];
        s += fmt::format(" {}x{} fmt{}{} @{:05X}:{}", std::get<0>(k), std::get<1>(k), std::get<2>(k),
                         std::get<3>(k) ? " scaled" : "", std::get<4>(k), c);
      }
      REXGPU_INFO("[gpu-stats] texture loads in {} s: {} ({} scaled resolve, {} MB guest){}", (now - window_start) / 1000,
                  total, scaled, bytes >> 20, s);
      loads.clear();
      bytes = 0;
      window_start = now;
    }
  }

  return true;
}

void TextureCache::RequestTextures(uint32_t used_texture_mask) {
  const auto& regs = register_file();

  if (texture_became_outdated_.exchange(false, std::memory_order_acquire)) {
    // A texture has become outdated - make sure whether textures are outdated
    // is rechecked in this draw and in subsequent ones to reload the new data
    // if needed.
    ResetTextureBindings();
  }

  // Update the texture keys and the textures.
  uint32_t bindings_changed = 0;
  std::vector<PendingTextureLoad> pending_texture_loads;
  std::vector<PendingSharedMemoryRange> pending_shared_memory_ranges;
  auto queue_pending_texture_load = [&](Texture* texture) {
    if (texture == nullptr) {
      return;
    }
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      if (pending_load.texture == texture) {
        return;
      }
    }
    PendingTextureLoad pending_load;
    PendingSharedMemoryRange pending_ranges[2];
    size_t pending_range_count = 0;
    if (!PrepareTextureLoad(*texture, pending_load, pending_ranges, pending_range_count)) {
      return;
    }
    pending_texture_loads.push_back(pending_load);
    for (size_t i = 0; i < pending_range_count; ++i) {
      pending_shared_memory_ranges.push_back(pending_ranges[i]);
    }
  };
  uint32_t textures_remaining = used_texture_mask & ~texture_bindings_in_sync_;
  uint32_t index = 0;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    uint32_t index_bit = UINT32_C(1) << index;
    textures_remaining &= ~index_bit;
    TextureBinding& binding = texture_bindings_[index];
    xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(index);
    TextureKey old_key = binding.key;
    uint8_t old_swizzled_signs = binding.swizzled_signs;
    BindingInfoFromFetchConstant(fetch, binding.key, &binding.swizzled_signs);
    texture_bindings_in_sync_ |= index_bit;
    if (!binding.key.is_valid) {
      if (old_key.is_valid) {
        bindings_changed |= index_bit;
      }
      binding.Reset();
      continue;
    }
    uint32_t old_host_swizzle = binding.host_swizzle;
    binding.host_swizzle = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(binding.key));

    // Check if need to load the unsigned and the signed versions of the texture
    // (if the format is emulated with different host bit representations for
    // signed and unsigned - otherwise only the unsigned one is loaded).
    bool key_changed = binding.key != old_key;
    bool any_sign_was_not_signed = texture_util::IsAnySignNotSigned(old_swizzled_signs);
    bool any_sign_was_signed = texture_util::IsAnySignSigned(old_swizzled_signs);
    bool any_sign_is_not_signed = texture_util::IsAnySignNotSigned(binding.swizzled_signs);
    bool any_sign_is_signed = texture_util::IsAnySignSigned(binding.swizzled_signs);
    if (key_changed || binding.host_swizzle != old_host_swizzle ||
        any_sign_is_not_signed != any_sign_was_not_signed ||
        any_sign_is_signed != any_sign_was_signed) {
      bindings_changed |= index_bit;
    }
    bool load_unsigned_data = false, load_signed_data = false;
    if (IsSignedVersionSeparateForFormat(binding.key)) {
      // Can reuse previously loaded unsigned/signed versions if the key is the
      // same and the texture was previously bound as unsigned/signed
      // respectively (checking the previous values of signedness rather than
      // binding.texture != nullptr and binding.texture_signed != nullptr also
      // prevents repeated attempts to load the texture if it has failed to
      // load).
      if (any_sign_is_not_signed) {
        if (key_changed || !any_sign_was_not_signed) {
          binding.texture = FindOrCreateTexture(binding.key);
          load_unsigned_data = true;
        }
      } else {
        binding.texture = nullptr;
      }
      if (any_sign_is_signed) {
        if (key_changed || !any_sign_was_signed) {
          TextureKey signed_key = binding.key;
          signed_key.signed_separate = 1;
          binding.texture_signed = FindOrCreateTexture(signed_key);
          load_signed_data = true;
        }
      } else {
        binding.texture_signed = nullptr;
      }
    } else {
      // Same resource for both unsigned and signed, but descriptor formats may
      // be different.
      if (key_changed) {
        binding.texture = FindOrCreateTexture(binding.key);
        load_unsigned_data = true;
      }
      binding.texture_signed = nullptr;
    }
    if (load_unsigned_data) {
      queue_pending_texture_load(binding.texture);
    }
    if (load_signed_data) {
      queue_pending_texture_load(binding.texture_signed);
    }
  }

  COUNT_profile_set("gpu/texture_cache/request_textures_pending_load_count",
                    uint32_t(pending_texture_loads.size()));
  COUNT_profile_set("gpu/texture_cache/request_textures_pending_range_count",
                    uint32_t(pending_shared_memory_ranges.size()));

  bool batched_shared_memory_request_succeeded = true;
  std::vector<std::pair<uint32_t, uint32_t>> pending_shared_memory_range_pairs;
  if (!pending_shared_memory_ranges.empty()) {
    pending_shared_memory_range_pairs.reserve(pending_shared_memory_ranges.size());
    for (const PendingSharedMemoryRange& pending_range : pending_shared_memory_ranges) {
      pending_shared_memory_range_pairs.emplace_back(pending_range.start, pending_range.length);
    }
    batched_shared_memory_request_succeeded = shared_memory().RequestRanges(
        pending_shared_memory_range_pairs.data(), pending_shared_memory_range_pairs.size());
  }

  if (batched_shared_memory_request_succeeded) {
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      CommitPreparedTextureLoad(pending_load);
    }
  } else {
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      if (pending_load.texture != nullptr) {
        LoadTextureData(*pending_load.texture);
      }
    }
  }
  if (bindings_changed) {
    UpdateTextureBindingsImpl(bindings_changed);
  }
}

const char* TextureCache::TextureKey::GetLogDimensionName(xenos::DataDimension dimension) {
  switch (dimension) {
    case xenos::DataDimension::k1D:
      return "1D";
    case xenos::DataDimension::k2DOrStacked:
      return "2D";
    case xenos::DataDimension::k3D:
      return "3D";
    case xenos::DataDimension::kCube:
      return "cube";
    default:
      assert_unhandled_case(dimension);
      return "unknown";
  }
}

void TextureCache::TextureKey::LogAction(const char* action) const {
  REXGPU_TRACE(
      "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
      "base at 0x{:08X} (pitch {}), mips at 0x{:08X}",
      action, tiled ? "tiled" : "linear", scaled_resolve ? "scaled " : "", GetWidth(), GetHeight(),
      GetDepthOrArraySize(), GetLogDimensionName(), FormatInfo::Get(format)->name,
      mip_max_level + 1, packed_mips ? "" : "un", mip_max_level != 0 ? "s" : "", base_page << 12,
      pitch << 5, mip_page << 12);
}

void TextureCache::Texture::LogAction(const char* action) const {
  REXGPU_TRACE(
      "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
      "base at 0x{:08X} (pitch {}, size 0x{:08X}), mips at 0x{:08X} (size "
      "0x{:08X})",
      action, key_.tiled ? "tiled" : "linear", key_.scaled_resolve ? "scaled " : "",
      key_.GetWidth(), key_.GetHeight(), key_.GetDepthOrArraySize(), key_.GetLogDimensionName(),
      FormatInfo::Get(key_.format)->name, key_.mip_max_level + 1, key_.packed_mips ? "" : "un",
      key_.mip_max_level != 0 ? "s" : "", key_.base_page << 12, key_.pitch << 5, GetGuestBaseSize(),
      key_.mip_page << 12, GetGuestMipsSize());
}

// The texture must be in the recent usage list. Place it in front now because
// after creation, the texture will likely be used immediately, and it should
// not be destroyed immediately after creation if dropping of old textures is
// performed somehow. The list is maintained by the Texture, not the
// TextureCache itself (unlike the `textures_` container).
TextureCache::Texture::Texture(TextureCache& texture_cache, const TextureKey& key, bool track_usage)
    : texture_cache_(texture_cache),
      key_(key),
      guest_layout_(key.GetGuestLayout()),
      last_usage_submission_index_(texture_cache.current_submission_index_),
      last_usage_time_(texture_cache.current_submission_time_),
      used_previous_(track_usage ? texture_cache.texture_used_last_ : nullptr),
      used_next_(nullptr),
      in_usage_list_(track_usage) {
  if (track_usage) {
    if (texture_cache.texture_used_last_) {
      texture_cache.texture_used_last_->used_next_ = this;
    } else {
      texture_cache.texture_used_first_ = this;
    }
    texture_cache.texture_used_last_ = this;
  }

  // Never try to upload data that doesn't exist.
  base_outdated_ = guest_layout().base.level_data_extent_bytes != 0;
  mips_outdated_ = guest_layout().mips_total_extent_bytes != 0;
  outdated_mask_.store(
      (base_outdated_ ? kOutdatedBitBase : 0) | (mips_outdated_ ? kOutdatedBitMips : 0),
      std::memory_order_relaxed);
}

TextureCache::Texture::~Texture() {
  if (mips_watch_handle_) {
    texture_cache().shared_memory().UnwatchMemoryRange(mips_watch_handle_);
  }
  if (base_watch_handle_) {
    texture_cache().shared_memory().UnwatchMemoryRange(base_watch_handle_);
  }

  if (in_usage_list_) {
    if (used_previous_) {
      used_previous_->used_next_ = used_next_;
    } else {
      texture_cache_.texture_used_first_ = used_next_;
    }
    if (used_next_) {
      used_next_->used_previous_ = used_previous_;
    } else {
      texture_cache_.texture_used_last_ = used_previous_;
    }
  }

  texture_cache_.UpdateTexturesTotalHostMemoryUsage(0, host_memory_usage_);
}

void TextureCache::Texture::MakeUpToDateAndWatch(
    const std::unique_lock<std::recursive_mutex>& global_lock) {
  SharedMemory& shared_memory = texture_cache().shared_memory();
  if (base_outdated_) {
    assert_not_zero(GetGuestBaseSize());
    base_outdated_ = false;
    base_watch_handle_ = shared_memory.WatchMemoryRange(
        key().base_page << 12, GetGuestBaseSize(), TextureCache::WatchCallback, this, nullptr, 0);
    outdated_mask_.fetch_and(~kOutdatedBitBase, std::memory_order_release);
  }
  if (mips_outdated_) {
    assert_not_zero(GetGuestMipsSize());
    mips_outdated_ = false;
    mips_watch_handle_ = shared_memory.WatchMemoryRange(
        key().mip_page << 12, GetGuestMipsSize(), TextureCache::WatchCallback, this, nullptr, 1);
    outdated_mask_.fetch_and(~kOutdatedBitMips, std::memory_order_release);
  }
}

void TextureCache::Texture::MarkAsUsed() {
  if (!in_usage_list_) {
    return;
  }
  assert_true(last_usage_submission_index_ <= texture_cache_.current_submission_index_);
  // This is called very frequently, don't relink unless needed for caching.
  if (last_usage_submission_index_ >= texture_cache_.current_submission_index_) {
    return;
  }
  last_usage_submission_index_ = texture_cache_.current_submission_index_;
  last_usage_time_ = texture_cache_.current_submission_time_;
  if (used_next_ == nullptr) {
    // Already the most recently used.
    return;
  }
  if (used_previous_ != nullptr) {
    used_previous_->used_next_ = used_next_;
  } else {
    texture_cache_.texture_used_first_ = used_next_;
  }
  used_next_->used_previous_ = used_previous_;
  used_previous_ = texture_cache_.texture_used_last_;
  used_next_ = nullptr;
  texture_cache_.texture_used_last_->used_next_ = this;
  texture_cache_.texture_used_last_ = this;
}

void TextureCache::Texture::WatchCallback(
    [[maybe_unused]] const std::unique_lock<std::recursive_mutex>& global_lock, bool is_mip) {
  if (is_mip) {
    assert_not_zero(GetGuestMipsSize());
    mips_outdated_ = true;
    mips_watch_handle_ = nullptr;
    outdated_mask_.fetch_or(kOutdatedBitMips, std::memory_order_release);
  } else {
    assert_not_zero(GetGuestBaseSize());
    base_outdated_ = true;
    base_watch_handle_ = nullptr;
    outdated_mask_.fetch_or(kOutdatedBitBase, std::memory_order_release);
  }
}

void TextureCache::WatchCallback(const std::unique_lock<std::recursive_mutex>& global_lock,
                                 void* context, void* data, uint64_t argument,
                                 bool invalidated_by_gpu) {
  Texture& texture = *static_cast<Texture*>(context);
  texture.WatchCallback(global_lock, argument != 0);
  texture.texture_cache().texture_became_outdated_.store(true, std::memory_order_release);
}

void TextureCache::DestroyAllTextures(bool from_destructor) {
  ResetTextureBindings(from_destructor);
  textures_.clear();
  COUNT_profile_set("gpu/texture_cache/textures", 0);
}

TextureCache::Texture* TextureCache::FindOrCreateTexture(TextureKey key) {
  // Check if the texture is a scaled resolve texture.
  if (IsDrawResolutionScaled() && key.tiled && IsScaledResolveSupportedForFormat(key)) {
    texture_util::TextureGuestLayout scaled_resolve_guest_layout = key.GetGuestLayout();
    if ((scaled_resolve_guest_layout.base.level_data_extent_bytes &&
         IsRangeScaledResolved(key.base_page << 12,
                               scaled_resolve_guest_layout.base.level_data_extent_bytes)) ||
        (scaled_resolve_guest_layout.mips_total_extent_bytes &&
         IsRangeScaledResolved(key.mip_page << 12,
                               scaled_resolve_guest_layout.mips_total_extent_bytes))) {
      key.scaled_resolve = 1;
    }
  }

  auto get_host_extent = [this, &key](uint32_t& host_width_out, uint32_t& host_height_out,
                                      uint32_t& host_depth_or_array_size_out) {
    host_width_out = key.GetWidth();
    host_height_out = key.GetHeight();
    if (key.scaled_resolve) {
      host_width_out *= draw_resolution_scale_x();
      host_height_out *= draw_resolution_scale_y();
    }
    host_depth_or_array_size_out = key.GetDepthOrArraySize();
  };
  auto try_drop_top_level = [&key]() {
    if (!key.mip_page || !key.mip_max_level) {
      return false;
    }
    uint32_t old_width = key.GetWidth();
    uint32_t old_height = key.GetHeight();
    uint32_t old_depth_or_array_size = key.GetDepthOrArraySize();
    uint32_t new_width = std::max(old_width >> 1, UINT32_C(1));
    uint32_t new_height = std::max(old_height >> 1, UINT32_C(1));
    uint32_t new_depth_or_array_size = key.dimension == xenos::DataDimension::k3D
                                           ? std::max(old_depth_or_array_size >> 1, UINT32_C(1))
                                           : old_depth_or_array_size;
    if (new_width == old_width && new_height == old_height &&
        new_depth_or_array_size == old_depth_or_array_size) {
      return false;
    }
    const FormatInfo* format_info = FormatInfo::Get(key.format);
    uint32_t rebased_pitch_texels = std::max(rex::next_pow2(old_width) >> 1, UINT32_C(1));
    rebased_pitch_texels = rex::align(rebased_pitch_texels, format_info->block_width);
    rebased_pitch_texels = rex::align(rebased_pitch_texels, xenos::kTextureTileWidthHeight);
    if (!key.tiled) {
      uint32_t rebased_pitch_blocks = rebased_pitch_texels / format_info->block_width;
      uint32_t rebased_row_pitch_bytes = rebased_pitch_blocks * format_info->bytes_per_block();
      rebased_row_pitch_bytes =
          rex::align(rebased_row_pitch_bytes, xenos::kTextureLinearRowAlignmentBytes);
      rebased_pitch_blocks = rebased_row_pitch_bytes / format_info->bytes_per_block();
      rebased_pitch_texels = rebased_pitch_blocks * format_info->block_width;
    }
    uint32_t rebased_pitch = std::max(rebased_pitch_texels >> 5, UINT32_C(1));
    if (rebased_pitch > ((UINT32_C(1) << 9) - 1)) {
      return false;
    }
    key.width_minus_1 = new_width - 1;
    key.height_minus_1 = new_height - 1;
    key.depth_or_array_size_minus_1 = new_depth_or_array_size - 1;
    key.base_page = key.mip_page;
    key.mip_page = 0;
    key.pitch = rebased_pitch;
    key.mip_max_level = 0;
    key.packed_mips = 0;
    return true;
  };
  uint32_t host_width, host_height, host_depth_or_array_size;
  get_host_extent(host_width, host_height, host_depth_or_array_size);
  // If the host can't support the full texture extent, first try using the
  // unscaled version of a scaled resolve texture, then fall back to the first
  // stored mip level only.
  uint32_t max_host_width_height = GetMaxHostTextureWidthHeight(key.dimension);
  uint32_t max_host_depth_or_array_size = GetMaxHostTextureDepthOrArraySize(key.dimension);
  while (true) {
    bool width_height_too_large =
        host_width > max_host_width_height || host_height > max_host_width_height;
    bool depth_or_array_too_large = host_depth_or_array_size > max_host_depth_or_array_size;
    if (!width_height_too_large && !depth_or_array_too_large) {
      break;
    }
    bool size_adjusted = false;
    if (key.scaled_resolve) {
      key.scaled_resolve = 0;
      size_adjusted = true;
    } else if ((width_height_too_large ||
                (depth_or_array_too_large && key.dimension == xenos::DataDimension::k3D)) &&
               try_drop_top_level()) {
      size_adjusted = true;
    }
    if (!size_adjusted) {
      return nullptr;
    }
    get_host_extent(host_width, host_height, host_depth_or_array_size);
  }

  // Try to find an existing texture.
  // TODO(Triang3l): Reuse a texture with mip_page unchanged, but base_page
  // previously 0, now not 0, to save memory - common case in streaming.
  auto found_texture_it = textures_.find(key);
  if (found_texture_it != textures_.end()) {
    return found_texture_it->second.get();
  }

  // Create the texture and add it to the map.
  Texture* texture;
  {
    std::unique_ptr<Texture> new_texture = CreateTexture(key);
    if (!new_texture) {
      key.LogAction("Failed to create");
      return nullptr;
    }
    assert_true(new_texture->key() == key);
    texture = textures_.emplace(key, std::move(new_texture)).first->second.get();
  }
  COUNT_profile_set("gpu/texture_cache/textures", textures_.size());
  texture->LogAction("Created");
  return texture;
}

bool TextureCache::LoadTextureData(Texture& texture) {
  PendingTextureLoad pending_load;
  PendingSharedMemoryRange pending_ranges[2];
  size_t pending_range_count = 0;
  if (!PrepareTextureLoad(texture, pending_load, pending_ranges, pending_range_count)) {
    return true;
  }

  std::pair<uint32_t, uint32_t> pending_range_pairs[2];
  for (size_t i = 0; i < pending_range_count; ++i) {
    pending_range_pairs[i] = std::make_pair(pending_ranges[i].start, pending_ranges[i].length);
  }
  if (!shared_memory().RequestRanges(pending_range_pairs, pending_range_count)) {
    return false;
  }

  return CommitPreparedTextureLoad(pending_load);
}

namespace {

// The real Xenos hardware binds texture fetch constants with the "invalid"
// type if the rest of the descriptor is usable - the game writes such
// descriptors, but the type field doesn't stop the hardware from sampling
// them. Accept them only when they are self-consistent, so garbage descriptors
// are still rejected by BindingInfoFromFetchConstant below.
bool IsSelfConsistentInvalidTextureFetch(const xenos::xe_gpu_texture_fetch_t& fetch) {
  // The base address must point somewhere.
  if (fetch.base_address == 0) {
    return false;
  }
  // The size fields are stored with 1 subtracted from each component. Reject
  // zero dimensions and dimensions beyond the hardware limits of the
  // dimension. The 1D width limit mirrors the check in
  // BindingInfoFromFetchConstant.
  uint32_t width;
  switch (fetch.dimension) {
    case xenos::DataDimension::k1D:
      width = fetch.size_1d.width + 1;
      if (width == 0 || width > xenos::kTexture1DMaxWidth) {
        return false;
      }
      break;
    case xenos::DataDimension::k2DOrStacked: {
      width = fetch.size_2d.width + 1;
      uint32_t height = fetch.size_2d.height + 1;
      uint32_t depth_or_array_size = fetch.stacked ? fetch.size_2d.stack_depth + 1 : 1;
      if (width == 0 || height == 0 || width > xenos::kTexture2DCubeMaxWidthHeight ||
          height > xenos::kTexture2DCubeMaxWidthHeight ||
          depth_or_array_size > xenos::kTexture2DMaxStackDepth) {
        return false;
      }
    } break;
    case xenos::DataDimension::k3D: {
      width = fetch.size_3d.width + 1;
      uint32_t height = fetch.size_3d.height + 1;
      uint32_t depth = fetch.size_3d.depth + 1;
      if (width == 0 || height == 0 || depth == 0 ||
          width > xenos::kTexture3DMaxWidthHeight || height > xenos::kTexture3DMaxWidthHeight ||
          depth > xenos::kTexture3DMaxDepth) {
        return false;
      }
    } break;
    case xenos::DataDimension::kCube: {
      width = fetch.size_2d.width + 1;
      uint32_t height = fetch.size_2d.height + 1;
      if (width == 0 || height == 0 || width > xenos::kTexture2DCubeMaxWidthHeight ||
          height > xenos::kTexture2DCubeMaxWidthHeight) {
        return false;
      }
    } break;
    default:
      return false;
  }
  // The format must be one that the texture system knows about (the 6-bit
  // field always has a table entry, but keep the guard).
  const FormatInfo* format_info = FormatInfo::Get(fetch.format);
  if (format_info == nullptr) {
    return false;
  }
  // The pitch is in pixels >> 5, and may legitimately exceed the aligned
  // width, but it must be able to cover at least the block-aligned width, and
  // the field is 9 bits wide.
  uint32_t pitch = fetch.pitch;
  if (pitch > ((UINT32_C(1) << 9) - 1) ||
      (pitch << 5) < rex::align(width, format_info->block_width)) {
    return false;
  }
  return true;
}

}  // namespace

void TextureCache::BindingInfoFromFetchConstant(const xenos::xe_gpu_texture_fetch_t& fetch,
                                                TextureKey& key_out, uint8_t* swizzled_signs_out) {
  // Reset the key and the signedness.
  key_out.MakeInvalid();
  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out = uint8_t(xenos::TextureSign::kUnsigned) * uint8_t(0b01010101);
  }

  switch (fetch.type) {
    case xenos::FetchConstantType::kTexture:
      break;
    case xenos::FetchConstantType::kInvalidTexture:
      if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
        break;
      }
      if (IsSelfConsistentInvalidTextureFetch(fetch)) {
        static std::atomic_flag accepted_invalid_texture_logged = ATOMIC_FLAG_INIT;
        if (!accepted_invalid_texture_logged.test_and_set(std::memory_order_relaxed)) {
          REXGPU_DEBUG(
              "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) has "
              "\"invalid\" type, but is self-consistent - accepting it as a texture.",
              fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4,
              fetch.dword_5);
        }
        break;
      }
      REXGPU_WARN(
          "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
          "has \"invalid\" type! This is incorrect behavior, but you can try "
          "bypassing this by launching with "
          "--gpu_allow_invalid_fetch_constants=true.",
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
      return;
    default:
      REXGPU_WARN(
          "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
          "is completely invalid!",
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
      return;
  }

  uint32_t width_minus_1, height_minus_1, depth_or_array_size_minus_1;
  uint32_t base_page, mip_page, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_or_array_size_minus_1, &base_page,
                                                 &mip_page, nullptr, &mip_max_level);
  if (base_page == 0 && mip_page == 0) {
    // No texture data at all.
    return;
  }
  if (fetch.dimension == xenos::DataDimension::k1D) {
    bool is_invalid_1d = false;
    // TODO(Triang3l): Support long 1D textures.
    if (width_minus_1 >= xenos::kTexture2DCubeMaxWidthHeight) {
      REXGPU_ERROR(
          "1D texture is too wide ({}) - ignoring! Please report this "
          "developers",
          width_minus_1 + 1);
      is_invalid_1d = true;
    }
    assert_false(fetch.tiled);
    if (fetch.tiled) {
      REXGPU_ERROR(
          "1D texture has tiling enabled in the fetch constant, but this "
          "appears to be completely wrong - ignoring! Please report this "
          "developers");
      is_invalid_1d = true;
    }
    assert_false(fetch.packed_mips);
    if (fetch.packed_mips) {
      REXGPU_ERROR(
          "1D texture has packed mips enabled in the fetch constant, but this "
          "appears to be completely wrong - ignoring! Please report this "
          "developers");
      is_invalid_1d = true;
    }
    if (is_invalid_1d) {
      return;
    }
  }

  xenos::TextureFormat format = GetBaseFormat(fetch.format);

  key_out.base_page = base_page;
  key_out.mip_page = mip_page;
  key_out.dimension = fetch.dimension;
  key_out.width_minus_1 = width_minus_1;
  key_out.height_minus_1 = height_minus_1;
  key_out.depth_or_array_size_minus_1 = depth_or_array_size_minus_1;
  key_out.pitch = fetch.pitch;
  key_out.mip_max_level = mip_max_level;
  key_out.tiled = fetch.tiled;
  key_out.packed_mips = fetch.packed_mips;
  key_out.format = format;
  key_out.endianness = fetch.endianness;

  key_out.is_valid = 1;

  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out = texture_util::SwizzleSigns(fetch);
  }
}

void TextureCache::ResetTextureBindings(bool from_destructor) {
  uint32_t bindings_reset = 0;
  for (size_t i = 0; i < texture_bindings_.size(); ++i) {
    TextureBinding& binding = texture_bindings_[i];
    if (!binding.key.is_valid) {
      continue;
    }
    binding.Reset();
    bindings_reset |= UINT32_C(1) << i;
  }
  texture_bindings_in_sync_ &= ~bindings_reset;
  if (!from_destructor && bindings_reset) {
    UpdateTextureBindingsImpl(bindings_reset);
  }
}

void TextureCache::UpdateTexturesTotalHostMemoryUsage(uint64_t add, uint64_t subtract) {
  textures_total_host_memory_usage_ = textures_total_host_memory_usage_ - subtract + add;
  COUNT_profile_set(
      "gpu/texture_cache/total_host_memory_usage_mb",
      uint32_t((textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20));
}

bool TextureCache::IsRangeScaledResolved(uint32_t start_unscaled, uint32_t length_unscaled) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }

  start_unscaled = std::min(start_unscaled, SharedMemory::kBufferSize);
  length_unscaled = std::min(length_unscaled, SharedMemory::kBufferSize - start_unscaled);
  if (!length_unscaled) {
    return false;
  }

  // Two-level check for faster rejection since resolve targets are usually
  // placed in relatively small and localized memory portions (confirmed by
  // testing - pretty much all times the deeper level was entered, the texture
  // was a resolve target).
  uint32_t page_first = start_unscaled >> 12;
  uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
  uint32_t block_first = page_first >> 5;
  uint32_t block_last = page_last >> 5;
  uint32_t l2_block_first = block_first >> 6;
  uint32_t l2_block_last = block_last >> 6;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = l2_block_first; i <= l2_block_last; ++i) {
    uint64_t l2_block = scaled_resolve_pages_l2_[i];
    if (i == l2_block_first) {
      l2_block &= ~((UINT64_C(1) << (block_first & 63)) - 1);
    }
    if (i == l2_block_last && (block_last & 63) != 63) {
      l2_block &= (UINT64_C(1) << ((block_last & 63) + 1)) - 1;
    }
    uint32_t block_relative_index;
    while (rex::bit_scan_forward(l2_block, &block_relative_index)) {
      l2_block &= ~(UINT64_C(1) << block_relative_index);
      uint32_t block_index = (i << 6) + block_relative_index;
      uint32_t check_bits = UINT32_MAX;
      if (block_index == block_first) {
        check_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (block_index == block_last && (page_last & 31) != 31) {
        check_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      if (scaled_resolve_pages_[block_index] & check_bits) {
        return true;
      }
    }
  }
  return false;
}

void TextureCache::ScaledResolveGlobalWatchCallbackThunk(
    const std::unique_lock<std::recursive_mutex>& global_lock, void* context,
    uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu) {
  TextureCache* texture_cache = reinterpret_cast<TextureCache*>(context);
  texture_cache->ScaledResolveGlobalWatchCallback(global_lock, address_first, address_last,
                                                  invalidated_by_gpu);
}

void TextureCache::ScaledResolveGlobalWatchCallback(
    const std::unique_lock<std::recursive_mutex>& global_lock, uint32_t address_first,
    uint32_t address_last, bool invalidated_by_gpu) {
  assert_true(IsDrawResolutionScaled());
  if (invalidated_by_gpu) {
    // GPU writes are tracked explicitly by their call sites - resolve writes
    // may preserve scaled data, while memexports invalidate it.
    return;
  }
  // CPU writes invalidate scaled resolve contents. Textures themselves will be
  // invalidated by their shared memory watches.
  uint32_t resolve_page_first = address_first >> 12;
  uint32_t resolve_page_last = address_last >> 12;
  uint32_t resolve_block_first = resolve_page_first >> 5;
  uint32_t resolve_block_last = resolve_page_last >> 5;
  uint32_t resolve_l2_block_first = resolve_block_first >> 6;
  uint32_t resolve_l2_block_last = resolve_block_last >> 6;
  for (uint32_t i = resolve_l2_block_first; i <= resolve_l2_block_last; ++i) {
    uint64_t resolve_l2_block = scaled_resolve_pages_l2_[i];
    // Pre-mask to only process blocks within the write range.
    if (i == resolve_l2_block_first) {
      resolve_l2_block &= ~((UINT64_C(1) << (resolve_block_first & 63)) - 1);
    }
    if (i == resolve_l2_block_last && (resolve_block_last & 63) != 63) {
      resolve_l2_block &= (UINT64_C(1) << ((resolve_block_last & 63) + 1)) - 1;
    }
    uint32_t resolve_block_relative_index;
    while (rex::bit_scan_forward(resolve_l2_block, &resolve_block_relative_index)) {
      resolve_l2_block &= ~(UINT64_C(1) << resolve_block_relative_index);
      uint32_t resolve_block_index = (i << 6) + resolve_block_relative_index;
      uint32_t resolve_keep_bits = 0;
      if (resolve_block_index == resolve_block_first) {
        resolve_keep_bits |= (UINT32_C(1) << (resolve_page_first & 31)) - 1;
      }
      if (resolve_block_index == resolve_block_last && (resolve_page_last & 31) != 31) {
        resolve_keep_bits |= ~((UINT32_C(1) << ((resolve_page_last & 31) + 1)) - 1);
      }
      scaled_resolve_pages_[resolve_block_index] &= resolve_keep_bits;
      if (scaled_resolve_pages_[resolve_block_index] == 0) {
        scaled_resolve_pages_l2_[i] &= ~(UINT64_C(1) << resolve_block_relative_index);
      }
    }
  }
  RemoveScaledResolveRanges(address_first, address_last);
  RemoveUnscaledGpuWriteRanges(address_first, address_last);
}

}  // namespace rex::graphics
