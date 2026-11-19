/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace rex {
namespace ui {
namespace vulkan {
namespace util {

namespace {

// Cycle 28 diagnostic instrumentation (logging only, no behavior change).
//
// CreateDedicatedAllocationImage has four silent `return false` paths, and the
// host-side Vulkan allocation failure behind the 560x4688 render target
// creation failure was not attributable to a VkResult. These helpers log the
// first kAllocationFailureDetailLimit failures of each kind in full, then only
// every kAllocationFailureCounterInterval-th occurrence (a counter line), so a
// long failing run cannot flood the log.
constexpr uint32_t kAllocationFailureDetailLimit = 8;
constexpr uint32_t kAllocationFailureCounterInterval = 1024;

struct AllocationFailureLogState {
  std::atomic<uint32_t> count{0};
};

bool ShouldLogAllocationFailure(AllocationFailureLogState& state, uint32_t* const ordinal_out) {
  const uint32_t ordinal = state.count.fetch_add(1, std::memory_order_relaxed) + 1;
  *ordinal_out = ordinal;
  return ordinal <= kAllocationFailureDetailLimit ||
         (ordinal % kAllocationFailureCounterInterval) == 0;
}

const char* VkResultNameImpl(const VkResult result) {
  switch (result) {
    case VK_SUCCESS:
      return "VK_SUCCESS";
    case VK_NOT_READY:
      return "VK_NOT_READY";
    case VK_TIMEOUT:
      return "VK_TIMEOUT";
    case VK_EVENT_SET:
      return "VK_EVENT_SET";
    case VK_EVENT_RESET:
      return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
      return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
      return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
      return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
      return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
      return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
      return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
      return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
      return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
      return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY:
      return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
      return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
      return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_ERROR_FRAGMENTATION:
      return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_NOT_PERMITTED:
      return "VK_ERROR_NOT_PERMITTED";
    case VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR:
      return "VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR:
      return "VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR:
      return "VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR:
      return "VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR:
      return "VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR:
      return "VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR";
    case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
      return "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
    case VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT:
      return "VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
      return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    case VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR:
      return "VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR";
    case VK_ERROR_COMPRESSION_EXHAUSTED_EXT:
      return "VK_ERROR_COMPRESSION_EXHAUSTED_EXT";
    case VK_ERROR_NOT_ENOUGH_SPACE_KHR:
      return "VK_ERROR_NOT_ENOUGH_SPACE_KHR";
    case VK_PIPELINE_COMPILE_REQUIRED:
      return "VK_PIPELINE_COMPILE_REQUIRED";
    case VK_THREAD_IDLE_KHR:
      return "VK_THREAD_IDLE_KHR";
    case VK_THREAD_DONE_KHR:
      return "VK_THREAD_DONE_KHR";
    case VK_OPERATION_DEFERRED_KHR:
      return "VK_OPERATION_DEFERRED_KHR";
    case VK_OPERATION_NOT_DEFERRED_KHR:
      return "VK_OPERATION_NOT_DEFERRED_KHR";
    case VK_INCOMPATIBLE_SHADER_BINARY_EXT:
      return "VK_INCOMPATIBLE_SHADER_BINARY_EXT";
    case VK_PIPELINE_BINARY_MISSING_KHR:
      return "VK_PIPELINE_BINARY_MISSING_KHR";
    case VK_ERROR_SURFACE_LOST_KHR:
      return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
      return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR:
      return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
      return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
      return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT:
      return "VK_ERROR_VALIDATION_FAILED_EXT";
    case VK_ERROR_INVALID_SHADER_NV:
      return "VK_ERROR_INVALID_SHADER_NV";
    case VK_ERROR_UNKNOWN:
      return "VK_ERROR_UNKNOWN";
    default:
      return "VK_RESULT_UNRECOGNIZED";
  }
}

uint32_t GetMemoryHeapIndex(const VulkanDevice* const vulkan_device,
                            const uint32_t memory_type_index) {
  VkPhysicalDeviceMemoryProperties memory_properties;
  vulkan_device->vulkan_instance()->functions().vkGetPhysicalDeviceMemoryProperties(
      vulkan_device->physical_device(), &memory_properties);
  if (memory_type_index < memory_properties.memoryTypeCount) {
    return memory_properties.memoryTypes[memory_type_index].heapIndex;
  }
  return UINT32_MAX;
}

void LogProcessStatusOnce() {
  FILE* const status_file = std::fopen("/proc/self/status", "r");
  if (!status_file) {
    REXGPU_ERROR("VulkanAllocDiag: /proc/self/status open failed");
    return;
  }
  char line[256];
  while (std::fgets(line, sizeof(line), status_file)) {
    if (std::strncmp(line, "VmPeak:", 7) == 0 || std::strncmp(line, "VmSize:", 7) == 0 ||
        std::strncmp(line, "VmRSS:", 6) == 0 || std::strncmp(line, "VmHWM:", 6) == 0 ||
        std::strncmp(line, "VmSwap:", 7) == 0 || std::strncmp(line, "Threads:", 8) == 0) {
      size_t length = std::strlen(line);
      while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
      }
      REXGPU_ERROR("VulkanAllocDiag: process {}", static_cast<const char*>(line));
    }
  }
  std::fclose(status_file);
}

// One-shot process status plus the heap dump, emitted on the first image
// allocation failure of the process.
void LogMemoryBudgetOnce(const VulkanDevice* const vulkan_device) {
  static std::atomic<bool> dump_done{false};
  bool expected = false;
  if (!dump_done.compare_exchange_strong(expected, true)) {
    return;
  }
  REXGPU_ERROR("VulkanAllocDiag: === first host image allocation failure: one-shot dump ===");
  LogProcessStatusOnce();
  LogVulkanMemoryBudget(vulkan_device, "allocation failure");
}

std::atomic<uint64_t> vk_failure_count{0};
std::atomic<bool> device_fault_query_done{false};

bool ShouldLogVkFailure() {
  const uint64_t ordinal = vk_failure_count.fetch_add(1, std::memory_order_relaxed) + 1;
  // Retain the first few distinct failures, then exponentially thin repeated
  // errors while keeping a useful counter for a long-lived failing run.
  return ordinal <= 8 || (ordinal & (ordinal - 1)) == 0;
}

void LogDeviceFaultInfoOnce(const VulkanDevice* const vulkan_device) {
  bool expected = false;
  if (!device_fault_query_done.compare_exchange_strong(expected, true,
                                                       std::memory_order_relaxed)) {
    return;
  }
  if (!vulkan_device || !vulkan_device->extensions().ext_EXT_device_fault ||
      !vulkan_device->properties().deviceFault) {
    REXGPU_ERROR("VulkanDeviceFault: VK_EXT_device_fault deviceFault feature is unavailable");
    return;
  }
  const VulkanInstance::Functions& ifn = vulkan_device->vulkan_instance()->functions();
  const auto get_device_proc_addr = ifn.vkGetDeviceProcAddr;
  const PFN_vkGetDeviceFaultInfoEXT get_fault_info =
      get_device_proc_addr
          ? PFN_vkGetDeviceFaultInfoEXT(get_device_proc_addr(vulkan_device->device(),
                                                             "vkGetDeviceFaultInfoEXT"))
          : nullptr;
  if (!get_fault_info) {
    REXGPU_ERROR("VulkanDeviceFault: VK_EXT_device_fault is unavailable or not enabled");
    return;
  }

  VkDeviceFaultCountsEXT counts = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
  VkDeviceFaultInfoEXT info = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
  VkResult result = get_fault_info(vulkan_device->device(), &counts, &info);
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    REXGPU_ERROR("VulkanDeviceFault: count query returned {} ({})", VkResultNameImpl(result),
                 int32_t(result));
    return;
  }

  constexpr uint32_t kMaxFaultDetailsToLog = 256;
  const uint32_t address_capacity = std::min(counts.addressInfoCount, kMaxFaultDetailsToLog);
  const uint32_t vendor_capacity = std::min(counts.vendorInfoCount, kMaxFaultDetailsToLog);
  std::vector<VkDeviceFaultAddressInfoEXT> address_infos(address_capacity);
  std::vector<VkDeviceFaultVendorInfoEXT> vendor_infos(vendor_capacity);
  info.pAddressInfos = address_infos.empty() ? nullptr : address_infos.data();
  info.pVendorInfos = vendor_infos.empty() ? nullptr : vendor_infos.data();
  VkDeviceFaultCountsEXT collected_counts = counts;
  collected_counts.addressInfoCount = address_capacity;
  collected_counts.vendorInfoCount = vendor_capacity;
  collected_counts.vendorBinarySize = 0;
  result = get_fault_info(vulkan_device->device(), &collected_counts, &info);
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    REXGPU_ERROR("VulkanDeviceFault: detail query returned {} ({})", VkResultNameImpl(result),
                 int32_t(result));
    return;
  }

  REXGPU_ERROR("VulkanDeviceFault: description='{}' addresses={} vendor_infos={} binary_bytes={}",
               info.description, collected_counts.addressInfoCount,
               collected_counts.vendorInfoCount, collected_counts.vendorBinarySize);
  const uint32_t address_count = std::min<uint32_t>(collected_counts.addressInfoCount,
                                                    uint32_t(address_infos.size()));
  for (uint32_t i = 0; i < address_count; ++i) {
    const VkDeviceFaultAddressInfoEXT& address = address_infos[i];
    REXGPU_ERROR(
        "VulkanDeviceFault: address[{}] type={} reported=0x{:X} precision=0x{:X}", i,
        uint32_t(address.addressType), address.reportedAddress, address.addressPrecision);
  }
  const uint32_t vendor_count =
      std::min<uint32_t>(collected_counts.vendorInfoCount, uint32_t(vendor_infos.size()));
  for (uint32_t i = 0; i < vendor_count; ++i) {
    const VkDeviceFaultVendorInfoEXT& vendor = vendor_infos[i];
    REXGPU_ERROR("VulkanDeviceFault: vendor[{}] description='{}' code=0x{:X} data=0x{:X}", i,
                 vendor.description, vendor.vendorFaultCode, vendor.vendorFaultData);
  }
}

AllocationFailureLogState create_image_failure_log_state;
AllocationFailureLogState choose_memory_type_failure_log_state;
AllocationFailureLogState allocate_memory_failure_log_state;
AllocationFailureLogState bind_image_memory_failure_log_state;

}  // namespace

const char* VkResultName(const VkResult result) { return VkResultNameImpl(result); }

void LogVulkanMemoryBudget(const VulkanDevice* const vulkan_device, const char* const reason) {
  if (!vulkan_device) {
    return;
  }
  const VulkanInstance::Functions& ifn = vulkan_device->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device->physical_device();
  VkPhysicalDeviceMemoryProperties2 memory_properties_2 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
  VkPhysicalDeviceMemoryBudgetPropertiesEXT memory_budget = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
  const bool has_memory_budget = vulkan_device->extensions().ext_EXT_memory_budget &&
                                 ifn.vkGetPhysicalDeviceMemoryProperties2 != nullptr;
  if (has_memory_budget) {
    memory_properties_2.pNext = &memory_budget;
    ifn.vkGetPhysicalDeviceMemoryProperties2(physical_device, &memory_properties_2);
  } else {
    ifn.vkGetPhysicalDeviceMemoryProperties(physical_device,
                                            &memory_properties_2.memoryProperties);
  }
  const VkPhysicalDeviceMemoryProperties& memory_properties =
      memory_properties_2.memoryProperties;
  REXGPU_INFO(
      "VulkanMemoryBudget: reason='{}' device='{}' heaps={} memory_types={} extension_available={}",
      reason ? reason : "unspecified",
      static_cast<const char*>(vulkan_device->properties().deviceName),
      uint32_t(memory_properties.memoryHeapCount), uint32_t(memory_properties.memoryTypeCount),
      has_memory_budget);
  for (uint32_t heap_index = 0; heap_index < memory_properties.memoryHeapCount; ++heap_index) {
    const VkMemoryHeap& heap = memory_properties.memoryHeaps[heap_index];
    if (has_memory_budget) {
      const VkDeviceSize heap_budget = memory_budget.heapBudget[heap_index];
      const VkDeviceSize heap_usage = memory_budget.heapUsage[heap_index];
      REXGPU_INFO(
          "VulkanMemoryBudget: heap[{}] flags=0x{:X} size={} budget={} usage={} available={}",
          heap_index, uint32_t(heap.flags), heap.size, heap_budget, heap_usage,
          heap_budget > heap_usage ? heap_budget - heap_usage : 0);
    } else {
      REXGPU_INFO("VulkanMemoryBudget: heap[{}] flags=0x{:X} size={} budget=unavailable usage=unavailable",
                  heap_index, uint32_t(heap.flags), heap.size);
    }
  }
  for (uint32_t memory_type_index = 0; memory_type_index < memory_properties.memoryTypeCount;
       ++memory_type_index) {
    const VkMemoryType& memory_type = memory_properties.memoryTypes[memory_type_index];
    REXGPU_INFO("VulkanMemoryBudget: memory_type[{}] flags=0x{:X} heap={}", memory_type_index,
                uint32_t(memory_type.propertyFlags), uint32_t(memory_type.heapIndex));
  }
  if (!has_memory_budget) {
    REXGPU_INFO("VulkanMemoryBudget: VK_EXT_memory_budget is unavailable; budget/usage not reported");
  }
}

void LogVkResultFailure(const VulkanDevice* const vulkan_device, const char* const operation,
                        const VkResult result) {
  if (result == VK_ERROR_DEVICE_LOST) {
    LogDeviceFaultInfoOnce(vulkan_device);
  }
  if (!ShouldLogVkFailure()) {
    return;
  }
  REXGPU_ERROR("VulkanResult: {} returned {} ({})", operation ? operation : "unknown operation",
               VkResultNameImpl(result), int32_t(result));
  LogVulkanMemoryBudget(vulkan_device, operation ? operation : "Vulkan failure");
}

VkResult QueueSubmit(const VulkanDevice* const vulkan_device, const VkQueue queue,
                     const uint32_t submit_count, const VkSubmitInfo* const submit_infos,
                     const VkFence fence) {
#if !defined(NDEBUG) || defined(REXGLUE_VULKAN_FAILURE_TEST)
  static const uint64_t fail_on_submit = [] {
    const char* const value = std::getenv("WWE2K16_TEST_VULKAN_DEVICE_LOST_SUBMIT_N");
    if (!value || !*value) {
      return uint64_t(0);
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end != value && *end == '\0' ? uint64_t(parsed) : uint64_t(0);
  }();
  static std::atomic<uint64_t> submit_count_for_testing{0};
  const uint64_t ordinal =
      submit_count_for_testing.fetch_add(1, std::memory_order_relaxed) + 1;
  if (fail_on_submit && ordinal == fail_on_submit) {
    REXLOG_WARN("WWE2K16_TEST_VULKAN_DEVICE_LOST_SUBMIT_N injected VK_ERROR_DEVICE_LOST at submit {}",
                ordinal);
    return VK_ERROR_DEVICE_LOST;
  }
#endif
  return vulkan_device->functions().vkQueueSubmit(queue, submit_count, submit_infos, fence);
}

void FlushMappedMemoryRange(const VulkanDevice* const vulkan_device, const VkDeviceMemory memory,
                            const uint32_t memory_type, const VkDeviceSize offset,
                            const VkDeviceSize memory_size, const VkDeviceSize size) {
  assert_false(size != VK_WHOLE_SIZE && memory_size == VK_WHOLE_SIZE);
  assert_true(memory_size == VK_WHOLE_SIZE || offset <= memory_size);
  assert_true(memory_size == VK_WHOLE_SIZE || size <= memory_size - offset);
  if (!size || (vulkan_device->memory_types().host_coherent & (uint32_t(1) << memory_type))) {
    return;
  }
  VkMappedMemoryRange range;
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.pNext = nullptr;
  range.memory = memory;
  range.offset = offset;
  range.size = size;
  const VkDeviceSize non_coherent_atom_size = vulkan_device->properties().nonCoherentAtomSize;
  range.offset = offset / non_coherent_atom_size * non_coherent_atom_size;
  if (size != VK_WHOLE_SIZE) {
    range.size =
        std::min(rex::round_up(offset + size, non_coherent_atom_size), memory_size) - range.offset;
  }
  vulkan_device->functions().vkFlushMappedMemoryRanges(vulkan_device->device(), 1, &range);
}

bool CreateDedicatedAllocationBuffer(const VulkanDevice* const vulkan_device,
                                     const VkDeviceSize size, const VkBufferUsageFlags usage,
                                     const MemoryPurpose memory_purpose, VkBuffer& buffer_out,
                                     VkDeviceMemory& memory_out, uint32_t* const memory_type_out,
                                     VkDeviceSize* const memory_size_out) {
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = 0;
  buffer_create_info.size = size;
  buffer_create_info.usage = usage;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  VkBuffer buffer;
  if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements memory_requirements;
  dfn.vkGetBufferMemoryRequirements(device, buffer, &memory_requirements);
  uint32_t memory_type = ChooseMemoryType(vulkan_device->memory_types(),
                                          memory_requirements.memoryTypeBits, memory_purpose);
  if (memory_type == UINT32_MAX) {
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = memory_requirements.size;
  memory_allocate_info.memoryTypeIndex = memory_type;
  VkMemoryDedicatedAllocateInfo memory_dedicated_allocate_info;
  if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
    memory_dedicated_allocate_info.pNext = memory_allocate_info.pNext;
    memory_allocate_info.pNext = &memory_dedicated_allocate_info;
    memory_dedicated_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memory_dedicated_allocate_info.pNext = nullptr;
    memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
    memory_dedicated_allocate_info.buffer = buffer;
  }
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  if (dfn.vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return false;
  }

  buffer_out = buffer;
  memory_out = memory;
  if (memory_type_out) {
    *memory_type_out = memory_type;
  }
  if (memory_size_out) {
    *memory_size_out = memory_allocate_info.allocationSize;
  }
  return true;
}

bool CreateDedicatedAllocationImage(const VulkanDevice* const vulkan_device,
                                    const VkImageCreateInfo& create_info,
                                    const MemoryPurpose memory_purpose, VkImage& image_out,
                                    VkDeviceMemory& memory_out, uint32_t* const memory_type_out,
                                    VkDeviceSize* const memory_size_out) {
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkImage image;
  const VkResult create_image_result = dfn.vkCreateImage(device, &create_info, nullptr, &image);
  if (create_image_result != VK_SUCCESS) {
    uint32_t failure_ordinal;
    if (ShouldLogAllocationFailure(create_image_failure_log_state, &failure_ordinal)) {
      REXGPU_ERROR(
          "VulkanAllocDiag: vkCreateImage failed #{} result={} ({}) extent={}x{}x{} "
          "mipLevels={} arrayLayers={} samples=0x{:X} format={} usage=0x{:X} tiling={} "
          "flags=0x{:X}",
          failure_ordinal, int32_t(create_image_result), VkResultName(create_image_result),
          create_info.extent.width, create_info.extent.height, create_info.extent.depth,
          create_info.mipLevels, create_info.arrayLayers, uint32_t(create_info.samples),
          uint32_t(create_info.format), uint32_t(create_info.usage), uint32_t(create_info.tiling),
          uint32_t(create_info.flags));
    }
    LogMemoryBudgetOnce(vulkan_device);
    return false;
  }

  VkMemoryRequirements memory_requirements;
  dfn.vkGetImageMemoryRequirements(device, image, &memory_requirements);
  uint32_t memory_type = ChooseMemoryType(vulkan_device->memory_types(),
                                          memory_requirements.memoryTypeBits, memory_purpose);
  if (memory_type == UINT32_MAX) {
    uint32_t failure_ordinal;
    if (ShouldLogAllocationFailure(choose_memory_type_failure_log_state, &failure_ordinal)) {
      REXGPU_ERROR(
          "VulkanAllocDiag: ChooseMemoryType failed #{} supported_types=0x{:X} purpose={}",
          failure_ordinal, memory_requirements.memoryTypeBits, uint32_t(memory_purpose));
    }
    LogMemoryBudgetOnce(vulkan_device);
    dfn.vkDestroyImage(device, image, nullptr);
    return false;
  }

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = memory_requirements.size;
  memory_allocate_info.memoryTypeIndex = memory_type;
  VkMemoryDedicatedAllocateInfo memory_dedicated_allocate_info;
  if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
    memory_dedicated_allocate_info.pNext = memory_allocate_info.pNext;
    memory_allocate_info.pNext = &memory_dedicated_allocate_info;
    memory_dedicated_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memory_dedicated_allocate_info.pNext = nullptr;
    memory_dedicated_allocate_info.image = image;
    memory_dedicated_allocate_info.buffer = VK_NULL_HANDLE;
  }
  VkDeviceMemory memory;
  const VkResult allocate_memory_result =
      dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory);
  if (allocate_memory_result != VK_SUCCESS) {
    uint32_t failure_ordinal;
    if (ShouldLogAllocationFailure(allocate_memory_failure_log_state, &failure_ordinal)) {
      REXGPU_ERROR(
          "VulkanAllocDiag: vkAllocateMemory failed #{} result={} ({}) allocationSize={} "
          "memoryTypeIndex={} heapIndex={} memoryTypeBits=0x{:X}",
          failure_ordinal, int32_t(allocate_memory_result), VkResultName(allocate_memory_result),
          memory_allocate_info.allocationSize, memory_type,
          GetMemoryHeapIndex(vulkan_device, memory_type), memory_requirements.memoryTypeBits);
    }
    LogMemoryBudgetOnce(vulkan_device);
    dfn.vkDestroyImage(device, image, nullptr);
    return false;
  }

  const VkResult bind_image_memory_result = dfn.vkBindImageMemory(device, image, memory, 0);
  if (bind_image_memory_result != VK_SUCCESS) {
    uint32_t failure_ordinal;
    if (ShouldLogAllocationFailure(bind_image_memory_failure_log_state, &failure_ordinal)) {
      REXGPU_ERROR(
          "VulkanAllocDiag: vkBindImageMemory failed #{} result={} ({}) allocationSize={} "
          "memoryTypeIndex={} heapIndex={}",
          failure_ordinal, int32_t(bind_image_memory_result),
          VkResultName(bind_image_memory_result), memory_allocate_info.allocationSize, memory_type,
          GetMemoryHeapIndex(vulkan_device, memory_type));
    }
    LogMemoryBudgetOnce(vulkan_device);
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return false;
  }

  image_out = image;
  memory_out = memory;
  if (memory_type_out) {
    *memory_type_out = memory_type;
  }
  if (memory_size_out) {
    *memory_size_out = memory_allocate_info.allocationSize;
  }
  return true;
}

VkPipeline CreateComputePipeline(const VulkanDevice* const vulkan_device,
                                 const VkPipelineLayout layout, const VkShaderModule shader,
                                 const VkSpecializationInfo* const specialization_info,
                                 const char* const entry_point) {
  VkComputePipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = nullptr;
  pipeline_create_info.flags = 0;
  pipeline_create_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_create_info.stage.pNext = nullptr;
  pipeline_create_info.stage.flags = 0;
  pipeline_create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_create_info.stage.module = shader;
  pipeline_create_info.stage.pName = entry_point;
  pipeline_create_info.stage.pSpecializationInfo = specialization_info;
  pipeline_create_info.layout = layout;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  VkPipeline pipeline;
  if (vulkan_device->functions().vkCreateComputePipelines(vulkan_device->device(), VK_NULL_HANDLE,
                                                          1, &pipeline_create_info, nullptr,
                                                          &pipeline) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

VkPipeline CreateComputePipeline(const VulkanDevice* const vulkan_device, VkPipelineLayout layout,
                                 const uint32_t* shader_code, size_t shader_code_size_bytes,
                                 const VkSpecializationInfo* specialization_info,
                                 const char* entry_point) {
  const VkShaderModule shader =
      CreateShaderModule(vulkan_device, shader_code, shader_code_size_bytes);
  if (shader == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  const VkPipeline pipeline =
      CreateComputePipeline(vulkan_device, layout, shader, specialization_info, entry_point);
  vulkan_device->functions().vkDestroyShaderModule(vulkan_device->device(), shader, nullptr);
  return pipeline;
}

}  // namespace util
}  // namespace vulkan
}  // namespace ui
}  // namespace rex
