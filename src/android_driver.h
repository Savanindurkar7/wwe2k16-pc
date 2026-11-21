// wwe2k16 - custom Vulkan driver chosen in the Android launcher. See android_driver.cpp.

#pragma once

namespace wwe2k16::android_driver {

// Before anything starts Vulkan (OnPreSetup): routes the SDK's Vulkan library through the chosen driver.
void Install();

// Once per presented game frame: removes the launcher's trial file after a few hundred good frames.
void OnFrame();

}  // namespace wwe2k16::android_driver
