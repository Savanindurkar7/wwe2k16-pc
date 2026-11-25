// wwe2k16 - custom Vulkan driver chosen in the Android launcher (Turnip and other adrenotools driver packages).
//
// Adapted from the WWE 2K14 recompilation's Android driver loader (src/android_driver.h, Jarrod Feng, BSD 3-Clause:
// https://github.com/Jarrodfeng/wwe2k14-recomp), which uses libadrenotools (bylaws, BSD 2-Clause).
//
// The launcher unpacks an imported driver zip into the app's own storage (shared storage cannot hold executable
// code) and passes:
//   WWE2K16_DRIVER=<file name>        the driver library, e.g. libvulkan_freedreno.so; unset = the phone's driver
//   WWE2K16_DRIVER_DIR=<dir>/         the folder it was unpacked to
//   WWE2K16_NATIVE_LIB_DIR=<dir>/     the APK's native library folder (libadrenotools' hook libraries are there)
//   WWE2K16_TMP_DIR=<dir>/            a writable folder (Android 9 needs it for patched libraries)
//   WWE2K16_DRIVER_TRIAL=<file>       written by the launcher before PLAY, deleted here after 600 frames: if it is still
//                                     there at the next launch, the driver crashed and the launcher goes back to the
//                                     phone's driver
//
// Before the driver is handed to the SDK it is test-started (an instance and at least one GPU). If that fails, the
// phone's own driver is used and the log says why.

#include "android_driver.h"

#if defined(__ANDROID__) && defined(WWE2K16_HAVE_ADRENOTOOLS)

#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>

#include <rex/logging.h>

namespace rex::ui::vulkan {
using VulkanLoaderOverride = void* (*)();
void SetVulkanLoaderOverride(VulkanLoaderOverride override_fn);
}  // namespace rex::ui::vulkan

namespace wwe2k16::android_driver {
namespace {

constexpr uint32_t kTrialFrames = 600;
std::string g_driver_used;
uint32_t g_frames = 0;

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

std::string WithSlash(std::string dir) {
  if (!dir.empty() && dir.back() != '/') dir += '/';
  return dir;
}

std::string g_device_name;  // r21: the first GPU's name as the opened driver reports it

// r21: one line for the launcher (Drivers.statusFile): "ok <library> <GPU>" or "notloaded <library> <GPU>".
void WriteStatus(const char* result, const std::string& name) {
  const std::string path = Env("WWE2K16_DRIVER_STATUS");
  if (path.empty()) return;
  if (FILE* f = std::fopen(path.c_str(), "w")) {
    std::fprintf(f, "%s %s %s\n", result, name.c_str(), g_device_name.c_str());
    std::fclose(f);
  }
}

bool TestStart(void* handle, const std::string& name) {
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
  if (!gipa) {
    REXLOG_WARN("[driver] {}: no vkGetInstanceProcAddr", name);
    return false;
  }
  auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
  if (!create) {
    REXLOG_WARN("[driver] {}: no vkCreateInstance", name);
    return false;
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "wwe2k16 driver test";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  const VkResult result = create(&ci, nullptr, &instance);
  if (result != VK_SUCCESS || !instance) {
    REXLOG_WARN("[driver] {}: vkCreateInstance failed ({})", name, int(result));
    return false;
  }
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
  auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
  auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
  uint32_t count = 0;
  const bool ok = enumerate && props && enumerate(instance, &count, nullptr) == VK_SUCCESS && count > 0;
  if (ok) {
    std::vector<VkPhysicalDevice> devices(count);
    enumerate(instance, &count, devices.data());
    VkPhysicalDeviceProperties p{};
    props(devices[0], &p);
    g_device_name = p.deviceName;
    REXLOG_INFO("[driver] {}: {} GPU(s); first: {}, Vulkan {}.{}.{}, driver 0x{:08X}", name, count, p.deviceName,
                VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
  } else {
    REXLOG_WARN("[driver] {}: the driver reports no GPU", name);
  }
  if (destroy) destroy(instance, nullptr);
  return ok;
}

void* OpenDriverOnce() {
  const std::string name = Env("WWE2K16_DRIVER");
  if (name.empty()) return nullptr;
  const std::string lib_dir = WithSlash(Env("WWE2K16_NATIVE_LIB_DIR"));
  std::string driver_dir = WithSlash(Env("WWE2K16_DRIVER_DIR"));
  const std::string tmp_dir = WithSlash(Env("WWE2K16_TMP_DIR"));
  if (lib_dir.empty() || driver_dir.empty()) {
    REXLOG_WARN("[driver] {} chosen but the folders were not passed: the phone's driver is used", name);
    return nullptr;
  }
  REXLOG_INFO("[driver] opening {} from {}", name, driver_dir);
  void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
                                            tmp_dir.empty() ? nullptr : tmp_dir.c_str(), lib_dir.c_str(),
                                            driver_dir.c_str(), name.c_str(), nullptr, nullptr);
  if (!handle) {
    const char* error = dlerror();
    REXLOG_WARN("[driver] libadrenotools could not open {} ({}): the phone's driver is used", name,
                error ? error : "no details");
    return nullptr;
  }
  if (!TestStart(handle, name)) {
    REXLOG_WARN("[driver] {} did not start: the phone's driver is used", name);
    return nullptr;  // left open: unloading a half-started driver is not safe
  }
  // r21: a Turnip package must report a Turnip GPU. When the phone's own driver answers instead (libadrenotools
  // could not swap it in: an Adreno 650 on Android 14 kept its Qualcomm 1.1.128 driver, which then crashed), say so.
  if (Env("WWE2K16_DRIVER_EXPECT") == "turnip" && g_device_name.find("Turnip") == std::string::npos &&
      g_device_name.find("turnip") == std::string::npos) {
    REXLOG_WARN("[driver] {} opened, but the GPU reports '{}': the phone's own driver is running, not Turnip", name,
                g_device_name);
    WriteStatus("notloaded", name);
  } else {
    WriteStatus("ok", name);
  }
  REXLOG_INFO("[driver] using {} (custom driver)", name);
  g_driver_used = name;
  return handle;
}

void* OpenDriver() {
  static void* const handle = OpenDriverOnce();
  return handle;
}

}  // namespace

void Install() {
  if (Env("WWE2K16_DRIVER").empty()) {
    REXLOG_INFO("[driver] the phone's own Vulkan driver");
    return;
  }
  rex::ui::vulkan::SetVulkanLoaderOverride(&OpenDriver);
}

void OnFrame() {
  if (g_frames > kTrialFrames) return;
  if (++g_frames == kTrialFrames) {
    const std::string trial = Env("WWE2K16_DRIVER_TRIAL");
    if (!trial.empty() && unlink(trial.c_str()) == 0) {
      REXLOG_INFO("[driver] {} frames drawn with {}: trial file removed", kTrialFrames,
                  g_driver_used.empty() ? std::string("the phone's driver") : g_driver_used);
    }
  }
}

}  // namespace wwe2k16::android_driver

#else

namespace wwe2k16::android_driver {
void Install() {}
void OnFrame() {}
}  // namespace wwe2k16::android_driver

#endif
