// wwe2k16 - renderer profiles (WWE2K16_RENDERER, launcher: Options > Renderer).
//
//   classic       the SDK's emulated Xbox 360 GPU (EDRAM emulation); the only renderer of the ReXGlue 0.7 build
//   native        WWE 2K14 SDK build: the SDK's native renderer draws the scene (gpu_native 2, truth frames on the host
//                 render-target path), every resolve still written back for the CPU
//   native_plus   native + the faster resolve / texture paths (texture-only resolves skip their write-back)
//
// Apply() runs in OnPreSetup once the GPU plugin is loaded (its settings exist from then on).
#pragma once

#include <string_view>

namespace wwe2k16::native_renderer {

std::string_view Profile();
void Apply();

}  // namespace wwe2k16::native_renderer
