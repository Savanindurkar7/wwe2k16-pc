/**
 * Optional Vulkan GPU timestamp census.
 */

#include "gpu_timing.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>

namespace rex::graphics::vulkan {
namespace {

constexpr VkPipelineStageFlagBits kTimestampStartStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
constexpr VkPipelineStageFlagBits kTimestampBoundaryStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

void LogUnavailableOnce(const char* reason) {
  static std::atomic_flag logged = ATOMIC_FLAG_INIT;
  if (!logged.test_and_set(std::memory_order_relaxed)) {
    REXLOG_WARN("[rate-census-gputime] disabled: {}", reason);
  }
}

}  // namespace

GpuTiming::~GpuTiming() { Shutdown(); }

bool GpuTiming::Requested() {
  static const bool requested = [] {
    const char* value = std::getenv("WWE2K16_GPU_TIMING");
    return value != nullptr && std::string_view(value) == "1";
  }();
  return requested;
}

const char* GpuTiming::CategoryName(GpuTimingCategory category) {
  switch (category) {
    case GpuTimingCategory::kOther:
      return "other";
    case GpuTimingCategory::kSceneDrawsLe640:
      return "scene_draws_le640";
    case GpuTimingCategory::kSceneDraws1280:
      return "scene_draws_1280";
    case GpuTimingCategory::kSceneDrawsOther:
      return "scene_draws_other";
    case GpuTimingCategory::kResolve:
      return "resolve";
    case GpuTimingCategory::kRtTransfer:
      return "rt_transfer";
    case GpuTimingCategory::kEdramDump:
      return "edram_dump";
    case GpuTimingCategory::kTextureLoad:
      return "texture_load";
    case GpuTimingCategory::kSharedMemoryUploadCopy:
      return "shared_memory_upload_copy";
    case GpuTimingCategory::kDepthConvert:
      return "depth_convert";
    case GpuTimingCategory::kPresent:
      return "present";
    case GpuTimingCategory::kCount:
      break;
  }
  return "invalid";
}

GpuTimingCategory GpuTiming::SceneDrawCategory(uint32_t render_target_width) {
  if (render_target_width && render_target_width <= 640) {
    return GpuTimingCategory::kSceneDrawsLe640;
  }
  if (render_target_width >= 1152 && render_target_width <= 1408) {
    return GpuTimingCategory::kSceneDraws1280;
  }
  return GpuTimingCategory::kSceneDrawsOther;
}

void GpuTiming::DisableForUnsupported(const char* reason) {
  LogUnavailableOnce(reason);
  enabled_ = false;
}

bool GpuTiming::Initialize(const ui::vulkan::VulkanDevice* vulkan_device,
                           uint32_t graphics_queue_family) {
  if (!Requested()) {
    return false;
  }

  vulkan_device_ = vulkan_device;
  device_ = vulkan_device_->device();
  const ui::vulkan::VulkanInstance::Functions& ifn =
      vulkan_device_->vulkan_instance()->functions();

  VkPhysicalDeviceProperties physical_device_properties = {};
  ifn.vkGetPhysicalDeviceProperties(vulkan_device_->physical_device(),
                                   &physical_device_properties);
  uint32_t queue_family_count = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(),
                                              &queue_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
  if (queue_family_count) {
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(),
                                                &queue_family_count,
                                                queue_family_properties.data());
  }
  if (graphics_queue_family >= queue_family_properties.size()) {
    DisableForUnsupported("graphics queue-family properties are unavailable");
    return false;
  }

  timestamp_valid_bits_ = queue_family_properties[graphics_queue_family].timestampValidBits;
  timestamp_period_ns_ = physical_device_properties.limits.timestampPeriod;
  if (!physical_device_properties.limits.timestampComputeAndGraphics ||
      !timestamp_valid_bits_ || timestamp_period_ns_ <= 0.0) {
    DisableForUnsupported("the selected graphics queue does not support timestamps");
    return false;
  }

  cmd_write_timestamp_ = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
      ifn.vkGetDeviceProcAddr(device_, "vkCmdWriteTimestamp"));
  get_query_pool_results_ = reinterpret_cast<PFN_vkGetQueryPoolResults>(
      ifn.vkGetDeviceProcAddr(device_, "vkGetQueryPoolResults"));
  if (!cmd_write_timestamp_ || !get_query_pool_results_) {
    DisableForUnsupported("timestamp query commands are unavailable");
    return false;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  VkQueryPoolCreateInfo query_pool_create_info = {};
  query_pool_create_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  query_pool_create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query_pool_create_info.queryCount = kQueriesPerFrameSlot;
  for (FrameSlot& slot : frame_slots_) {
    if (dfn.vkCreateQueryPool(device_, &query_pool_create_info, nullptr,
                              &slot.query_pool) != VK_SUCCESS) {
      for (FrameSlot& created_slot : frame_slots_) {
        if (created_slot.query_pool != VK_NULL_HANDLE) {
          dfn.vkDestroyQueryPool(device_, created_slot.query_pool, nullptr);
          created_slot.query_pool = VK_NULL_HANDLE;
        }
      }
      DisableForUnsupported("failed to allocate timestamp query pools");
      return false;
    }
  }

  enabled_ = true;
  reporter_running_.store(true, std::memory_order_release);
  reporter_thread_ = std::thread(&GpuTiming::ReporterMain, this);
  REXLOG_INFO("[gpu-timing] enabled queue_family={} timestamp_period_ns={} "
              "timestamp_valid_bits={} queries_per_frame_slot={}",
              graphics_queue_family, timestamp_period_ns_, timestamp_valid_bits_,
              kQueriesPerFrameSlot);
  return true;
}

void GpuTiming::Shutdown() {
  if (reporter_running_.exchange(false, std::memory_order_acq_rel)) {
    reporter_condition_.notify_all();
  }
  if (reporter_thread_.joinable()) {
    reporter_thread_.join();
  }

  if (device_ != VK_NULL_HANDLE && vulkan_device_) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    for (FrameSlot& slot : frame_slots_) {
      if (slot.query_pool != VK_NULL_HANDLE) {
        dfn.vkDestroyQueryPool(device_, slot.query_pool, nullptr);
        slot.query_pool = VK_NULL_HANDLE;
      }
      slot.frame = UINT64_MAX;
      slot.next_query = 0;
    }
  }
  enabled_ = false;
  cmd_write_timestamp_ = nullptr;
  get_query_pool_results_ = nullptr;
  device_ = VK_NULL_HANDLE;
  vulkan_device_ = nullptr;
}

void GpuTiming::SubmissionWillBeSubmitted(uint64_t frame, uint64_t submission) {
  if (!enabled_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  ++frames_[frame].expected_submissions;
  (void)submission;
}

void GpuTiming::SubmissionCancelled(uint64_t frame, uint64_t submission) {
  if (!enabled_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  submissions_.erase(submission);
  auto it = frames_.find(frame);
  if (it != frames_.end() && it->second.expected_submissions) {
    --it->second.expected_submissions;
    FinalizeReadyFramesLocked();
  }
}

void GpuTiming::FrameClosed(uint64_t frame) {
  if (!enabled_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = frames_.find(frame);
  if (it == frames_.end()) {
    return;
  }
  it->second.closed = true;
  FinalizeReadyFramesLocked();
}

bool GpuTiming::AllocateQuery(VkCommandBuffer command_buffer, FrameSlot& slot,
                              VkPipelineStageFlagBits stage, uint32_t& query_out) {
  if (slot.next_query >= kQueriesPerFrameSlot) {
    return false;
  }
  query_out = slot.next_query++;
  cmd_write_timestamp_(command_buffer, stage, slot.query_pool, query_out);
  return true;
}

void GpuTiming::BeginRecording(VkCommandBuffer command_buffer, uint64_t frame,
                               uint64_t submission) {
  if (!enabled_) {
    return;
  }
  assert_false(recording_.active);
  recording_ = {};
  recording_.active = true;
  recording_.record.frame = frame;
  recording_.record.submission = submission;
  recording_.record.frame_slot = uint32_t(frame % kFrameSlotCount);

  FrameSlot& slot = frame_slots_[recording_.record.frame_slot];
  if (slot.frame != frame) {
    if (slot.next_query) {
      vulkan_device_->functions().vkCmdResetQueryPool(command_buffer, slot.query_pool, 0,
                                                     slot.next_query);
    }
    slot.frame = frame;
    slot.next_query = 0;
  }
  recording_.record.first_query = slot.next_query;
  recording_.timing_active = AllocateQuery(command_buffer, slot, kTimestampStartStage,
                                           recording_.record.start_query);
  if (!recording_.timing_active) {
    ++recording_.record.overflow_queries;
  } else {
    recording_.active_query = recording_.record.start_query;
  }
}

void GpuTiming::ChangeCategory(VkCommandBuffer command_buffer,
                               GpuTimingCategory category) {
  if (!enabled_ || !recording_.active || recording_.active_category == category) {
    return;
  }

  ++recording_.record.category_passes[static_cast<size_t>(recording_.active_category)];
  FrameSlot& slot = frame_slots_[recording_.record.frame_slot];
  // Keep one slot free for the final timestamp so a changed category does not
  // consume the only query that could close the current interval.
  if (recording_.timing_active && slot.next_query + 1 < kQueriesPerFrameSlot) {
    uint32_t boundary_query = UINT32_MAX;
    if (AllocateQuery(command_buffer, slot, kTimestampBoundaryStage, boundary_query)) {
      recording_.record.intervals.push_back(
          {recording_.active_query, boundary_query, recording_.active_category});
      recording_.active_query = boundary_query;
    }
  } else if (recording_.timing_active) {
    ++recording_.record.overflow_queries;
    recording_.timing_active = false;
    recording_.active_query = UINT32_MAX;
  }
  recording_.active_category = category;
}

void GpuTiming::EndRecording(VkCommandBuffer command_buffer) {
  if (!enabled_ || !recording_.active) {
    return;
  }
  ++recording_.record.category_passes[static_cast<size_t>(recording_.active_category)];
  FrameSlot& slot = frame_slots_[recording_.record.frame_slot];
  if (recording_.timing_active) {
    if (AllocateQuery(command_buffer, slot, kTimestampBoundaryStage,
                      recording_.record.end_query)) {
      recording_.record.intervals.push_back(
          {recording_.active_query, recording_.record.end_query,
           recording_.active_category});
    } else {
      ++recording_.record.overflow_queries;
    }
  }
  recording_.record.query_count = slot.next_query - recording_.record.first_query;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    submissions_.emplace(recording_.record.submission, std::move(recording_.record));
  }
  recording_ = {};
}

void GpuTiming::ProcessCompleted(uint64_t completed_submission) {
  if (!enabled_) {
    return;
  }

  std::vector<SubmissionRecord> completed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = submissions_.begin();
    while (it != submissions_.end() && it->first <= completed_submission) {
      completed.push_back(std::move(it->second));
      it = submissions_.erase(it);
    }
  }
  for (SubmissionRecord& record : completed) {
    CompleteRecord(std::move(record));
  }
}

void GpuTiming::CompleteRecord(SubmissionRecord&& record) {
  const size_t category_count = kCategoryCount;
  double interval_ms[static_cast<size_t>(GpuTimingCategory::kCount)] = {};
  double idle_ms = 0.0;
  bool end_timestamp_valid = false;
  uint64_t end_timestamp = 0;
  bool start_timestamp_valid = false;
  uint64_t start_timestamp = 0;
  bool query_read_failed = false;

  if (record.query_count && record.start_query != UINT32_MAX) {
    std::vector<uint64_t> query_results(record.query_count);
    VkResult result = get_query_pool_results_(
        device_, frame_slots_[record.frame_slot].query_pool, record.first_query,
        record.query_count, VkDeviceSize(query_results.size() * sizeof(uint64_t)),
        query_results.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result == VK_SUCCESS) {
      start_timestamp = query_results[record.start_query - record.first_query];
      start_timestamp_valid = true;
      if (record.end_query != UINT32_MAX) {
        end_timestamp = query_results[record.end_query - record.first_query];
        end_timestamp_valid = true;
      }
      const uint64_t timestamp_mask =
          timestamp_valid_bits_ >= 64 ? UINT64_MAX
                                      : ((UINT64_C(1) << timestamp_valid_bits_) - 1);
      for (const Interval& interval : record.intervals) {
        const uint64_t start = query_results[interval.start_query - record.first_query];
        const uint64_t end = query_results[interval.end_query - record.first_query];
        const uint64_t ticks = (end - start) & timestamp_mask;
        const double elapsed_ms = double(ticks) * timestamp_period_ns_ / 1000000.0;
        interval_ms[static_cast<size_t>(interval.category)] += elapsed_ms;
      }
      if (start_timestamp_valid && has_previous_submission_end_timestamp_) {
        const uint64_t ticks = (start_timestamp - previous_submission_end_timestamp_) &
                               timestamp_mask;
        idle_ms = double(ticks) * timestamp_period_ns_ / 1000000.0;
      }
    } else {
      query_read_failed = true;
    }
  }

  if (end_timestamp_valid && !query_read_failed) {
    previous_submission_end_timestamp_ = end_timestamp;
    has_previous_submission_end_timestamp_ = true;
  } else {
    has_previous_submission_end_timestamp_ = false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  FrameRecord& frame = frames_[record.frame];
  frame.overflow_queries += record.overflow_queries;
  frame.idle_ms += idle_ms;
  for (size_t i = 0; i < category_count; ++i) {
    frame.category_ms[i] += interval_ms[i];
    frame.category_passes[i] += record.category_passes[i];
    frame.gpu_ms += interval_ms[i];
  }
  ++frame.completed_submissions;
  if (query_read_failed) {
    ++report_window_.query_read_failures;
  }
  FinalizeReadyFramesLocked();
}

void GpuTiming::FinalizeReadyFramesLocked() {
  auto it = frames_.begin();
  while (it != frames_.end()) {
    FrameRecord& frame = it->second;
    if (!frame.closed || frame.completed_submissions < frame.expected_submissions) {
      ++it;
      continue;
    }
    ++report_window_.frames;
    report_window_.gpu_ms += frame.gpu_ms;
    report_window_.idle_ms += frame.idle_ms;
    report_window_.overflow_queries += frame.overflow_queries;
    for (size_t i = 0; i < kCategoryCount; ++i) {
      report_window_.category_ms[i] += frame.category_ms[i];
      report_window_.category_passes[i] += frame.category_passes[i];
    }
    it = frames_.erase(it);
  }
}

void GpuTiming::ReporterMain() {
  for (;;) {
    WindowRecord sample;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      reporter_condition_.wait_for(lock, std::chrono::seconds(1), [this] {
        return !reporter_running_.load(std::memory_order_acquire);
      });
      if (!reporter_running_.load(std::memory_order_acquire)) {
        return;
      }
      sample = report_window_;
      report_window_ = {};
    }

    const double frame_divisor = sample.frames ? double(sample.frames) : 1.0;
    std::string line = fmt::format(
        "[rate-census-gputime] frames={} gpu_ms_per_frame={:.3f} "
        "gpu_idle_ms_per_frame={:.3f} overflow_queries={} query_read_failures={}",
        sample.frames, sample.gpu_ms / frame_divisor, sample.idle_ms / frame_divisor,
        sample.overflow_queries, sample.query_read_failures);
    for (size_t i = 0; i < kCategoryCount; ++i) {
      const auto category = static_cast<GpuTimingCategory>(i);
      line += fmt::format(" {}_ms_per_frame={:.3f} {}_passes_per_frame={:.2f}",
                          CategoryName(category), sample.category_ms[i] / frame_divisor,
                          CategoryName(category),
                          double(sample.category_passes[i]) / frame_divisor);
    }
    REXLOG_INFO("{}", line);
  }
}

}  // namespace rex::graphics::vulkan
