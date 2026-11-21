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

## Frame generation (native APK)

Options > Graphics > **Frame generation**. Code: `patches/k14-sdk/0001-frame-generation.patch`, applied to the
0.10 SDK by `tools/android-build-k14.sh` (`--present_frame_generation=true`).

The presenter shows an in-between frame before every new guest frame, and the real frame half a frame interval
later. Everything runs on the GPU, in the paint's own command buffer:

1. both frames' luma at a quarter of the size;
2. one motion vector per 16x16 pixels, searched symmetrically from the in-between time;
3. per pixel, the best of five block vectors, blended with a plain mix where none fits;
4. a copy of the current frame for next time.

The launcher also asks Android for the screen's fastest mode (90 / 120 Hz), because that's where the extra frames
show.

**Limits:**

- Input still acts at the game's rate.
- The HUD and fast motion can smear a little.
- Below about 30 game FPS (entrances, until the MSAA hook) it gains little.
- No in-between frame for frames more than `present_frame_generation_max_gap_ms` (80 ms) apart.

**Tuning:**

- `--present_frame_generation_search=N` (2..16, default 8): the motion search range.
- `--present_frame_generation_log_s=N`: a `[framegen]` line with the counts every N seconds.

## Defaults learned on the phone (2026-11-21)

- `gpu_native_half2_skip_gpr_draws` on (NATIVE, NATIVE+): those 24 point draws are the only MSAA draws of a match.
  Drawn, every frame falls back (21 FPS instead of 40).
- `vulkan_vertex_residency_cache_off` (all renderers in the native APK): this fixed the skin spikes. The 2K14 port's
  residency cache skips re-uploading vertex ranges it thinks are unchanged; WWE 2K16 streams character meshes into
  reused memory, so close-ups drew stale vertices. The diagnostic run with it off (census + shader dump, 2026-11-21)
  showed clean wrestlers. The skinning shaders themselves were checked: each bone row is read through `a0` as the
  console does, and all 256 float constants are uploaded for them.
- Diagnostics such as `--gpu_native_vertex_census` / `--dump_shaders` cost about 10 FPS (25-30 instead of 38-40).
  Leave Extra arguments empty for normal play.

## The game's scene without MSAA (native APK)

Native truth frames are 1x only, so the game's own 2x MSAA sends entrance frames to the emulated path (5-9 FPS; log
`[native][truth] ... fallbacks (clearing resolve / msaa)`). It also makes single match frames fall back, which shows
as black flicker. The fix is the one the WWE 2K14 port ships (`wwe2k14_force_no_msaa`, its `src/tiling_hooks.h`): the
game builds its scene targets 1x, as one tile.

- `tools/k14-tiling-hooks.py` finds WWE 2K16's scene-target builder in `generated_k14/` by the 2K14 function's
  shape: object fields +296/+300, the layout at +12 with its type at +20, targets at +272/+276, and +4/+8 stored.
  On exactly one match it writes `config/k14_tiling_hooks.toml` and adds the two hook calls into that one generated
  file. `android-build-k14.sh` runs it in step 1. With `python3 tools/k14-tiling-hooks.py` alone it only reports.
- `src/k14_tiling_hooks.h` holds the hooks, the cvar `wwe2k16_force_no_msaa` and the guards. Options > Graphics >
  Anti-aliasing (MSAA) > OFF (now the default) sets the cvar.
- The log line `[wwe2k16][tiling] scene build N: ... force_no_msaa: applied: 1 tile, 1x` confirms it worked. If no
  match is found, the build log shows `not hooked` and the functions it saw.
