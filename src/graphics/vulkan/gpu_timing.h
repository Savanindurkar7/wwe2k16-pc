/**
 * Optional Vulkan GPU timestamp census. Queries are recorded into pools tied
 * to the three guest-frame slots and are read only after their submit fence.
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::vulkan {

enum class GpuTimingCategory : uint32_t {
  kOther,
  kSceneDrawsLe640,
  kSceneDraws1280,
  kSceneDrawsOther,
  kResolve,
  kRtTransfer,
  kEdramDump,
  kTextureLoad,
  kSharedMemoryUploadCopy,
  kDepthConvert,
  kPresent,
  kCount,
};

class GpuTiming {
 public:
  static constexpr uint32_t kFrameSlotCount = 3;

  GpuTiming() = default;
  ~GpuTiming();
  GpuTiming(const GpuTiming&) = delete;
  GpuTiming& operator=(const GpuTiming&) = delete;

  // Returns false when disabled or when the queue cannot provide timestamps.
  bool Initialize(const ui::vulkan::VulkanDevice* vulkan_device,
                  uint32_t graphics_queue_family);
  void Shutdown();
  bool enabled() const { return enabled_; }

  // Called by EndSubmission before a job becomes visible to the submit worker.
  void SubmissionWillBeSubmitted(uint64_t frame, uint64_t submission);
  void SubmissionCancelled(uint64_t frame, uint64_t submission);
  void FrameClosed(uint64_t frame);

  // Called by DeferredCommandBuffer::Execute on the thread recording that
  // submission. Category markers are emitted into the deferred stream.
  void BeginRecording(VkCommandBuffer command_buffer, uint64_t frame,
                      uint64_t submission);
  void ChangeCategory(VkCommandBuffer command_buffer,
                      GpuTimingCategory category);
  void EndRecording(VkCommandBuffer command_buffer);

  // The caller must have observed the matching submission fence signaled.
  void ProcessCompleted(uint64_t completed_submission);

  static GpuTimingCategory SceneDrawCategory(uint32_t render_target_width);

 private:
  static constexpr uint32_t kQueriesPerFrameSlot = 65536;
  static constexpr size_t kCategoryCount =
      static_cast<size_t>(GpuTimingCategory::kCount);

  struct Interval {
    uint32_t start_query = 0;
    uint32_t end_query = 0;
    GpuTimingCategory category = GpuTimingCategory::kOther;
  };

  struct SubmissionRecord {
    uint64_t frame = 0;
    uint64_t submission = 0;
    uint32_t frame_slot = 0;
    uint32_t first_query = 0;
    uint32_t query_count = 0;
    uint32_t start_query = UINT32_MAX;
    uint32_t end_query = UINT32_MAX;
    uint64_t overflow_queries = 0;
    std::array<uint64_t, kCategoryCount> category_passes{};
    std::vector<Interval> intervals;
  };

  struct FrameRecord {
    uint64_t expected_submissions = 0;
    uint64_t completed_submissions = 0;
    uint64_t overflow_queries = 0;
    double gpu_ms = 0.0;
    double idle_ms = 0.0;
    std::array<double, kCategoryCount> category_ms{};
    std::array<uint64_t, kCategoryCount> category_passes{};
    bool closed = false;
  };

  struct WindowRecord {
    uint64_t frames = 0;
    uint64_t overflow_queries = 0;
    uint64_t query_read_failures = 0;
    double gpu_ms = 0.0;
    double idle_ms = 0.0;
    std::array<double, kCategoryCount> category_ms{};
    std::array<uint64_t, kCategoryCount> category_passes{};
  };

  struct FrameSlot {
    VkQueryPool query_pool = VK_NULL_HANDLE;
    uint64_t frame = UINT64_MAX;
    uint32_t next_query = 0;
  };

  struct RecordingState {
    SubmissionRecord record;
    bool active = false;
    bool timing_active = false;
    uint32_t active_query = UINT32_MAX;
    GpuTimingCategory active_category = GpuTimingCategory::kOther;
  };

  static bool Requested();
  static const char* CategoryName(GpuTimingCategory category);
  void DisableForUnsupported(const char* reason);
  bool AllocateQuery(VkCommandBuffer command_buffer, FrameSlot& slot,
                     VkPipelineStageFlagBits stage, uint32_t& query_out);
  void CompleteRecord(SubmissionRecord&& record);
  void FinalizeReadyFramesLocked();
  void ReporterMain();

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  VkDevice device_ = VK_NULL_HANDLE;
  PFN_vkCmdWriteTimestamp cmd_write_timestamp_ = nullptr;
  PFN_vkGetQueryPoolResults get_query_pool_results_ = nullptr;
  std::array<FrameSlot, kFrameSlotCount> frame_slots_{};
  RecordingState recording_{};
  uint32_t timestamp_valid_bits_ = 0;
  double timestamp_period_ns_ = 0.0;
  bool enabled_ = false;

  uint64_t previous_submission_end_timestamp_ = 0;
  bool has_previous_submission_end_timestamp_ = false;

  std::mutex mutex_;
  std::condition_variable reporter_condition_;
  std::map<uint64_t, SubmissionRecord> submissions_;
  std::map<uint64_t, FrameRecord> frames_;
  WindowRecord report_window_{};
  std::atomic<bool> reporter_running_{false};
  std::thread reporter_thread_;
};

}  // namespace rex::graphics::vulkan
