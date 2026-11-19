/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/immediate_drawer.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

REXCVAR_DEFINE_BOOL(vulkan_validation_enabled, false, "UI/Vulkan",
                    "Enable Vulkan validation layers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(vulkan_device, -1, "UI/Vulkan", "Vulkan device index (-1 for auto selection)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(vulkan_device_id, "", "UI/Vulkan",
                      "Vulkan physical device vendor:device ID (for example 10DE:2783)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vulkan_prefer_geometry_shader, true, "UI/Vulkan",
                    "Prefer physical devices supporting geometryShader when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    vulkan_prefer_fragment_stores_and_atomics, true, "UI/Vulkan",
    "Prefer physical devices supporting fragmentStoresAndAtomics when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_prefer_vertex_pipeline_stores_and_atomics, true, "UI/Vulkan",
                    "Prefer physical devices supporting vertexPipelineStoresAndAtomics when "
                    "auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_prefer_fill_mode_non_solid, true, "UI/Vulkan",
                    "Prefer physical devices supporting fillModeNonSolid when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {
namespace ui {
namespace vulkan {

namespace {

bool ParseVulkanDeviceId(const std::string_view text, uint32_t& vendor_id,
                         uint32_t& device_id) {
  const size_t separator = text.find(':');
  if (separator == std::string_view::npos || text.find(':', separator + 1) != std::string_view::npos ||
      separator < 4 || separator > 8 || text.size() - separator - 1 < 4 ||
      text.size() - separator - 1 > 8) {
    return false;
  }
  auto parse_component = [](const std::string_view component, uint32_t& value) {
    value = 0;
    if (component.size() < 4 || component.size() > 8) return false;
    for (const char character : component) {
      value <<= 4;
      if (character >= '0' && character <= '9') {
        value |= uint32_t(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        value |= uint32_t(character - 'a' + 10);
      } else if (character >= 'A' && character <= 'F') {
        value |= uint32_t(character - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  };
  return parse_component(text.substr(0, separator), vendor_id) &&
         parse_component(text.substr(separator + 1), device_id);
}

const char* PhysicalDeviceTypeName(VkPhysicalDeviceType type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
      return "CPU";
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
    default:
      return "other";
  }
}

}  // namespace

std::unique_ptr<VulkanProvider> VulkanProvider::Create(const bool with_gpu_emulation,
                                                       const bool with_presentation) {
  std::unique_ptr<VulkanProvider> provider(new VulkanProvider());

  provider->vulkan_instance_ =
      VulkanInstance::Create(with_presentation, REXCVAR_GET(vulkan_validation_enabled));
  if (!provider->vulkan_instance_) {
    return nullptr;
  }

  std::vector<VkPhysicalDevice> physical_devices;
  provider->vulkan_instance_->EnumeratePhysicalDevices(physical_devices);

  if (physical_devices.empty()) {
    REXLOG_WARN("No Vulkan physical devices available");
    return nullptr;
  }

  const VulkanInstance::Functions& ifn = provider->vulkan_instance_->functions();

  REXLOG_WARN(
      "Available Vulkan physical devices (use 'vulkan_device' for an index or "
      "'vulkan_device_id' for vendor:device identity):");
  for (size_t physical_device_index = 0; physical_device_index < physical_devices.size();
       ++physical_device_index) {
    VkPhysicalDeviceProperties physical_device_properties = {};
    ifn.vkGetPhysicalDeviceProperties(physical_devices[physical_device_index],
                                      &physical_device_properties);
    REXLOG_WARN("* {}: {} (type: {}, id: {:04X}:{:04X})", physical_device_index,
                physical_device_properties.deviceName,
                PhysicalDeviceTypeName(physical_device_properties.deviceType),
                physical_device_properties.vendorID, physical_device_properties.deviceID);
  }

  const std::string requested_device_id = REXCVAR_GET(vulkan_device_id);
  if (!requested_device_id.empty()) {
    uint32_t requested_vendor_id = 0;
    uint32_t requested_device_id_value = 0;
    if (!ParseVulkanDeviceId(requested_device_id, requested_vendor_id,
                             requested_device_id_value)) {
      REXLOG_WARN("Ignoring malformed vulkan_device_id '{}'; falling back to automatic selection",
                  requested_device_id);
    } else {
      bool found_matching_device = false;
      for (const VkPhysicalDevice physical_device : physical_devices) {
        VkPhysicalDeviceProperties properties = {};
        ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);
        if (properties.vendorID != requested_vendor_id ||
            properties.deviceID != requested_device_id_value) {
          continue;
        }
        found_matching_device = true;
        provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
            provider->vulkan_instance_.get(), physical_device, with_gpu_emulation,
            with_presentation);
        if (provider->vulkan_device_) break;
      }
      if (!found_matching_device) {
        REXLOG_WARN("No Vulkan physical device matches vulkan_device_id '{}'; falling back to "
                    "automatic selection",
                    requested_device_id);
      } else if (!provider->vulkan_device_) {
        REXLOG_WARN("Vulkan physical device ID '{}' could not be initialized; falling back to "
                    "automatic selection",
                    requested_device_id);
      }
    }
  } else if (REXCVAR_GET(vulkan_device) >= 0 &&
             uint32_t(REXCVAR_GET(vulkan_device)) < physical_devices.size()) {
    provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
        provider->vulkan_instance_.get(), physical_devices[REXCVAR_GET(vulkan_device)],
        with_gpu_emulation, with_presentation);
  }

  if (!provider->vulkan_device_) {
    std::vector<VkPhysicalDevice> physical_devices_ordered = physical_devices;
    bool prefer_geometry_shader = REXCVAR_GET(vulkan_prefer_geometry_shader);
    bool prefer_fragment_stores = REXCVAR_GET(vulkan_prefer_fragment_stores_and_atomics);
    bool prefer_vertex_stores = REXCVAR_GET(vulkan_prefer_vertex_pipeline_stores_and_atomics);
    bool prefer_fill_mode_non_solid = REXCVAR_GET(vulkan_prefer_fill_mode_non_solid);
    const bool score_features =
        with_gpu_emulation &&
        (prefer_geometry_shader || prefer_fragment_stores || prefer_vertex_stores ||
         prefer_fill_mode_non_solid);
    if (physical_devices.size() > 1) {
      struct PhysicalDeviceScore {
        VkPhysicalDevice physical_device;
        uint32_t score;
        bool is_discrete;
      };
      std::vector<PhysicalDeviceScore> scored_devices;
      scored_devices.reserve(physical_devices.size());
      for (const VkPhysicalDevice physical_device : physical_devices) {
        VkPhysicalDeviceProperties properties = {};
        ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);
        uint32_t score = 0;
        if (score_features) {
          VkPhysicalDeviceFeatures supported_features = {};
          ifn.vkGetPhysicalDeviceFeatures(physical_device, &supported_features);
          if (prefer_geometry_shader && supported_features.geometryShader) {
            ++score;
          }
          if (prefer_fragment_stores && supported_features.fragmentStoresAndAtomics) {
            ++score;
          }
          if (prefer_vertex_stores && supported_features.vertexPipelineStoresAndAtomics) {
            ++score;
          }
          if (prefer_fill_mode_non_solid && supported_features.fillModeNonSolid) {
            ++score;
          }
        }
        scored_devices.push_back(
            {physical_device, score, properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU});
      }

      std::stable_sort(scored_devices.begin(), scored_devices.end(),
                       [](const PhysicalDeviceScore& a, const PhysicalDeviceScore& b) {
                         if (a.is_discrete != b.is_discrete) {
                           return a.is_discrete;
                         }
                         return a.score > b.score;
                       });

      const bool prefer_discrete =
          !scored_devices.empty() && scored_devices.front().is_discrete !=
                                         scored_devices.back().is_discrete;
      const bool prefer_features = score_features && !scored_devices.empty() &&
                                   scored_devices.front().score != scored_devices.back().score;
      if (prefer_discrete || prefer_features) {
        physical_devices_ordered.clear();
        physical_devices_ordered.reserve(scored_devices.size());
        for (const PhysicalDeviceScore& scored_device : scored_devices) {
          physical_devices_ordered.push_back(scored_device.physical_device);
        }
      }
    }

    for (const VkPhysicalDevice physical_device : physical_devices_ordered) {
      provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
          provider->vulkan_instance_.get(), physical_device, with_gpu_emulation, with_presentation);
      if (provider->vulkan_device_) {
        break;
      }
    }

    if (!provider->vulkan_device_) {
      REXLOG_WARN(
          "Couldn't choose a compatible Vulkan physical device or initialize a "
          "Vulkan logical device");
      return nullptr;
    }
  }

  VkPhysicalDeviceProperties selected_properties = {};
  ifn.vkGetPhysicalDeviceProperties(provider->vulkan_device_->physical_device(),
                                    &selected_properties);
  REXLOG_INFO("[wwe2k16-profile:gpu] Selected Vulkan physical device: {} (type: {}, "
              "id: {:04X}:{:04X}, driver version: 0x{:X})",
              selected_properties.deviceName,
              PhysicalDeviceTypeName(selected_properties.deviceType),
              selected_properties.vendorID, selected_properties.deviceID,
              selected_properties.driverVersion);

  if (with_presentation) {
    provider->ui_samplers_ = UISamplers::Create(provider->vulkan_device_.get());
    if (!provider->ui_samplers_) {
      return nullptr;
    }
  }

  return provider;
}

std::unique_ptr<Presenter> VulkanProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return VulkanPresenter::Create(host_gpu_loss_callback, vulkan_device(), ui_samplers());
}

std::unique_ptr<ImmediateDrawer> VulkanProvider::CreateImmediateDrawer() {
  return VulkanImmediateDrawer::Create(vulkan_device(), ui_samplers());
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
