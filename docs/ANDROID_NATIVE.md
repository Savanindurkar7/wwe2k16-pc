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

**v2 (who presents):** the first version requested every paint from Android's UI thread, which managed only about 30
paints a second under GPU load, so it cost FPS. The SDK paints from the UI thread whenever its UI layer has something
to draw, and the game's FPS counter was such a thing. With frame generation on, the counter is now a label of the
game activity (`GameActivity.setupFpsLabel`, the game's frame count via JNI from `src/fps_overlay.cpp`). The game
then presents from its own thread, and the frame-generation timer presents the real frame directly. The log line
`[present] paint mode: game thread (immediately)` confirms it (`patches/k14-sdk/0003-present-paint-mode-log.patch`).
The first v2 test still showed `Android UI thread (UI drawers 1)`. The SDK's achievement pop-up layer stays
registered for the whole run, and only in-between frames were shown, 17 a second. With frame generation on, the app
now leaves that layer out (`Wwe2k16App::CreateAchievementNotificationDialog`). A guard
(`0004-frame-generation-guard.patch`) pauses generation for 10 s when more than a quarter of the real frames arrive
late, so frame generation can no longer halve the frame rate.

**Limits:**

- Input still acts at the game's rate.
- The HUD and fast motion can smear a little.
- Below about 30 game FPS it gains little.
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

## Turnip skin fix

With the Turnip driver (Mesa 27 A8xx), wrestlers' skin stretched into spikes in close-ups. The phone's own driver
showed none, and the shaders are correct. Test A (2026-11-21), `IR3_SHADER_DEBUG=nouboopt`, removed them: Turnip's
compiler copies uniform buffers into its constant registers ("UBO optimisation"), and it gets that wrong for the
skinning shaders, which read bone matrices with an index known only at run time (`c[76 + a0]`). Options > Advanced >
**Turnip skin fix** (on by default) sets it for Turnip drivers. When the compiler switches change, the launcher
deletes the pipeline cache (`*.vkpc`) and Mesa's shader cache once (`Settings.clearShaderCaches`).

## Build r10 (2026-11-22)

- **Crowd 20%** (new default, key `crowd_v2`). Phone test: crowd OFF about 44 FPS, 50% about 40.
- **Frame rate 45 FPS (steady)**: `WWE2K16_FRAME_CAP=45` and the screen at 90 Hz (`WWE2K16_DISPLAY_HZ`).
- **Render pass trim** (Options > Graphics, on by default): `kPassTrim` in `src/native_renderer.cpp` holds the WWE
  2K14 port's frame-structure settings that need nothing 2K14-specific. OFF gives the r9 renderer.
- **Frame generation v3**: frame generation ON runs the game at a steady 30 (`WWE2K16_FRAME_CAP=30`), with the screen
  at 60 Hz. One in-between frame per real frame makes 60 on screen. The uncapped version halved the game's own frames,
  because the GPU had no room at about 40.
- **Home screen**: sidebar plus pages (`LauncherActivity`, `Home.java`), with the repository's BG1.png, BG2.jpg,
  BG3.jpg and BG34-.jpg packed as `home_bg1..4`. The credits card (`Credits.card`) shows Catonix's links.

### r10 test results and r11

Phone tests on 2026-11-22, crowd 20%:
- **Render pass trim:** about 53 FPS with it OFF, about 42 with it ON. It is now OFF by default (key `pass_trim_v2`).
- **Frame generation at 30:** the game fell to about 22 real frames. Painting on the GPU command thread blocked
  that thread.

r11 (`0005-frame-generation-paint-thread.patch`) moves every paint to the frame generation thread when frame
generation is on, so the command thread never waits on the swapchain.

### r12 performance log and r13 / r14

- **Performance log (r12):** in a match the game's main thread ran at about 65% and the GPU command thread at about
  60%, while the GPU needed about 8.7 ms of each ~24 ms frame. The two threads take turns.
- **r13 tests:** the CPU fast path (the 2K14 port's Session 6 / 7 defaults) ran at 26-35 FPS, with one game thread
  at about 89%: worse. The native frame (`gpu_native_frame`) ran at 42-57, about 51: the best result so far.
- **r14 defaults:** native frame ON, CPU fast path OFF. Both options stay in Advanced.

### r16 (2026-11-22)

- **One screen:** `Shell.java` draws the art and the sidebar for both the home pages and the settings. Graphics,
  Controls and Settings open the settings in the same frame (section tabs at the top, title with a red underline, a
  line about the section), with no animation. Every option is still there.
- **CPU target:** `CPU_TUNE=new` (default) builds with `-march=armv8.2-a+dotprod+fp16+rcpc -mtune=cortex-a720`.
  `CPU_TUNE=generic` gives the old build for older phones. Changing it rebuilds the SDK and the game once.
- **Busy threads on their own cores** (`WWE2K16_HOT_CORES`, `src/frame_monitor.cpp`): every 2 s the two busiest
  threads each get one of the fastest cores, and all other threads are kept off those cores.
- **CPU fast path SAFE** (default): `kCpSafe`, six of the eleven 2K14 command-processor settings, without the ones
  that made a game thread spin in r13.
- **Crowd culling** (`gpu_scene_crowd_cull=2`, default on): crowd members outside the view are not drawn (exact).
- **GPU thread waiting:** SPIN (default) or SLEEP (`gpu_wait_reg_mem_sleep_us=50`, test, less heat).

### r17 (2026-11-22): Mali, built-in drivers, DLC

Mali logs (Infinix X6871, Mali-G610 MC6, r54p1, System driver):
- **Match FPS:** 29-35 at 480p with crowd 20%. The game's two busiest threads used about 90% and 70% of a core
  (Cortex-A78) at 30 FPS. On the Adreno 810 phone the main game thread used about 74% at 58 FPS. ASTC / EAC textures
  gave about +2 FPS.
- **Pink / magenta hatched wrestler:** the game builds an attire on the GPU (a 2048-wide copy) and compresses it on the
  CPU. On this phone the read-back window closed before that copy. CHARACTER TEXTURE FIX = AUTO now adds the 2K14
  port's attire latch on every GPU that is not an Adreno (`readback_resolve_attire_latch=300`). The new ATTIRE LATCH
  value does the same on any GPU.

Changes:
- **GPU family** (`Gpu.java`): GL_RENDERER from a 1x1 EGL context, cached per system build, passed to the game as
  `WWE2K16_GPU_FAMILY`. Every Mali choice below applies only when the GPU is a Mali. Adreno launches get the same
  arguments as r16; the only change there is the sticky cores below.
- **MALI BOOST** (Advanced, ON): `gpu_quirks_mobile_no_gs`, `gpu_quirks_tiler_classic_passes`, `gpu_quirks_vertex_input_phone`.
  NO GEOMETRY SHADERS ONLY = the first of these alone.
- **MALI TEXTURES** (Advanced, NORMAL): SMALL = `vulkan_texture_transcode` + `_eac`.
- **Busy threads on own cores:** threads named `mali-*` (the driver's shader compiler) are never pinned. On every GPU,
  a thread chosen at the last check now keeps its core, so the two busiest threads no longer swap cores when their
  order flips.
- **Performance log:** `[wwe2k16-threads]` lines add the GPU load and clock when the kernel lets the app read them:
  kgsl (Adreno), GED (MediaTek Mali), mali0 / gpu_busy (Exynos, Tensor).
- **Built-in drivers** (`tools/fetch-drivers.sh`, run by step 6): the latest Banners-Turnip release (A6xx-A7xx, A8xx,
  A710-A722) and malisx2 (Mesa PanVK for Mali, experimental), staged in `assets/drivers/` with `index.txt`. The
  launcher unpacks them on the first start of each APK (`Drivers.installBundled`) and marks them BUILT-IN /
  RECOMMENDED. An Adreno phone that never chose a driver gets the matching Turnip once. Pin a release with
  `TURNIP_TAG=` / `MALISX2_TAG=`; `SKIP_DRIVERS=1` builds without them. Qualcomm's own drivers are not
  redistributable, so players can still only import those.
- **DLC** (sidebar > DLC, `Dlc.java`, `src/dlc_install.cpp`): picked packages (LIVE / PIRS / CON) are copied to
  `userdata/dlc_import/`. Before the game's main thread runs, the SDK's `ContentManager::InstallContent` unpacks each
  one to `userdata/0000000000000000/315A07D2/00000002/<name>/`, plus its header. The result of each install goes to
  `dlc_import/result.txt` and `[wwe2k16-dlc]` log lines. `WWE2K16_DLC=0` turns installing off.

### r18 (2026-11-23): version 1.2.0, GPU presets, Mali heat fix, update popup, tamper check

- **Version** is now a real number: `WWE2K16_RELEASE` (default **1.2.0**) becomes `versionName`, shown in Android App
  Info and in the app. The git hash is kept as a build id (`resValue wwe2k16_build_id`), shown in the status card and
  Logs, so a build is still identifiable. `BuildInfo.versionName` → "1.2.0 (build <hash>)".
- **GPU auto-preset** (`Settings.applyPreset`, `Gpu.family`): on first start the launcher detects the GPU and applies
  the best settings once (`preset_applied_v1`); re-applied any time from Graphics ("Apply best settings for <GPU>").
  Adreno preset = the tested r14-r16 setup (existing users unchanged). Mali preset = 480p, crowd 20 / entrance 10,
  shadows off, DoF off, 45 cap, GPU wait SLEEP, MALI BOOST all, MALI TEXTURES small, native frame on.
- **Mali heat fix:** a Mali-G57 (vivo V2432) held ~58 FPS for 90 s then collapsed to 10 FPS from heat at a 60 cap.
  Fixes: (1) the Mali preset caps at 45 and uses SLEEP wait (SPIN burns a core); (2) a **thermal governor** in
  `frame_rate.cpp` — `GameActivity` forwards the Android thermal status (PowerManager) through
  `nativeSetThermalStatus`, and the governor lowers the cap to the next value the speed-fix handles (60 → 45 → 30)
  while the phone is hot, lifting it as it cools. `HEAT PROTECTION` setting (`WWE2K16_THERMAL_GUARD`: 0 off, 1 normal
  = Adreno default, 2 earlier = Mali default). `[wwe2k16-thermal]` log lines. A cool phone never reaches it, so Adreno
  is unaffected.
- **Update popup** (`VolvisionUpdateChannel`, supplied by the site owner): on launcher start it checks
  volvisionmods.com and offers to open the game page when the site has a newer version than the installed
  `versionName`. Needs INTERNET (added to the manifest). Set the site's version to 1.2.0 for this release, bump it when
  pushing updates.
- **Tamper / anti-repackage** (`Integrity`, `src/integrity.cpp`): if `VolvisionUpdateChannel` is removed or its APP_ID
  edited, if the package name is wrong, if the update check doesn't run at launch, or (game process) if the native
  brand token doesn't match the Java one, the app kills itself after a random delay. Optional signing-cert pin via
  `WWE2K16_CERT_SHA256` (empty by default, so the owner's debug builds run). Both brand copies use the same FNV-1a, so
  the Java and native branding can't be edited apart.
