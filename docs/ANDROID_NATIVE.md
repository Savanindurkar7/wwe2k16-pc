# Android: the native renderer APK

Two APKs come from this repository. Both use the same package, the same launcher and the same settings:

| APK | SDK | Build |
| --- | --- | --- |
| classic (`out/wwe2k16-android-debug.apk`) | ReXGlue 0.7 (yukes fork + Android, `patches/android-sdk/`) | `tools/android-build.sh` |
| native (`out/wwe2k16-native-debug.apk`) | ReXGlue v0.10.0 + the WWE 2K14 port's 118 SDK patches | `tools/native-stage1.sh`, then `tools/android-build-k14.sh` |

Installing one over the other keeps saves, settings and drivers.

## Building the native APK (Codespace)

    bash tools/native-stage1.sh        # once: the 0.10 SDK + 2K14 patches, its code generator, generated_k14/
    bash tools/android-build-k14.sh    # the SDK for arm64, libmain.so, the APK

The first `android-build-k14.sh` run compiles the SDK and the whole recompiled game for ARM64, which takes hours on a
Codespace. Later runs are incremental. `FORCE_CODEGEN=1 bash tools/native-stage1.sh` regenerates `generated_k14/`.

## What is different in the native APK

* **Renderer** (Options > Graphics):
  * CLASSIC is the emulated GPU.
  * NATIVE is the SDK's native renderer (`gpu_native 2`, truth frames on the host render-target path).
  * NATIVE+ is NATIVE plus the texture-only resolves and the direct texture paths.

  The setting lists are in `src/native_renderer.cpp`. They are the 2K14 port's own lists without the parts that only
  fit WWE 2K14 (`wwe2k14_*`, `gpu_native_shader_swap`, `gpu_native_truth_color_none`).
* **Resolution 360p / 480p / 540p** with NATIVE / NATIVE+ (`gpu_native_truth_draw_height`). Pair it with the **SGSR**
  upscaler; Sharpness sets SGSR's edge sharpness.
* **Character textures**: the 0.10 SDK has no targeted readback (`MakeGpuWritesVisibleToCpu`, a 0.7 patch here). It
  switches exact readback on (`readback_resolve_override 3`) while the game untiles a GPU-built texture, and turns it
  off 3 s after the last untile (`src/resolve_coherency.cpp`). Options > Advanced > Character texture fix > ALWAYS EXACT
  keeps it on, at a cost in speed.
* **Utility cache drive** (`cache:`): mounted by the app (`MountUtilityCache`), since 0.10 has no `--mount_cache`.
* **Achievements**: the SDK's own pop-ups; this project's toast is left out of the build.
* **Not in the native APK yet**:
  * the stall counters (the frame monitor logs frame times, and its loading columns stay 0);
  * `texture_min_lod` (Texture quality);
  * `vulkan_swapchain_image_count`.

  The SDK ignores those settings.

## Source layout

`src/` compiles against both SDKs. `src/ppc_compat.h` (`WWE2K16_SDK_K14`) and `__has_include` checks choose the API:

* `PPC_FUNC` and the guest-memory macros;
* the input drivers: per controller slot on 0.7, a synthetic device on 0.10;
* the GPU: built in on 0.7, the `xenos` plugin loaded in `OnPreSetup` on 0.10;
* the disc reader header and the achievement API.

`native/CMakeLists.txt` builds `src/` with `generated_k14/` against an installed SDK package.
