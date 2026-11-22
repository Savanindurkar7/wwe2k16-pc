// wwe2k16 - renderer profiles for the WWE 2K14 SDK build (ReXGlue 0.10 + the WWE 2K14 port's 118 SDK patches).
// See native_renderer.h.
//
// The lists are the 2K14 port's (src/gfx_settings.h NativeRendererFlags and the Android block of wwe2k14_app.h, by
// Jarrod Feng, BSD-3), with the settings that are 2K14-only left out: wwe2k14_* (its own game hooks),
// gpu_native_shader_swap (2K14's own copy of its post-processing pass, keyed to 2K14's shaders) and
// gpu_native_truth_color_none (white patches on the Adreno 830 in their runs). Everything else is SDK-side: it works on
// the Xbox 360 GPU command stream, not on one game.
//
// WWE 2K16 builds character textures on the GPU and untiles them on the CPU (resolve_coherency.cpp). The settings that
// skip a resolve's write-back to guest memory ("texture-only" resolves) are therefore only in the "native_plus"
// profile; "native" keeps every write-back, so those textures stay right.

#include "native_renderer.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ppc_compat.h"  // WWE2K16_SDK_K14

namespace wwe2k16::native_renderer {
namespace {

using Flag = std::pair<const char*, const char*>;

// Phone basics, every profile (2K14 Android, Sessions 63-74).
const std::vector<Flag> kPhone = {
    // Mali and PowerVR drivers lack these; nothing in a boot or a match needs them (2K14 Session 73 / 95).
    {"vulkan_require_vertex_pipeline_stores_and_atomics", "false"},
    {"vulkan_require_fill_mode_non_solid", "false"},
    {"vulkan_require_geometry_shader", "false"},
    // Separate submit thread + submit before fence waits: 39.7 -> 47.7 fps in a 2K14 match (Session 63).
    {"vulkan_async_submit", "true"},
    {"vulkan_submit_on_wait", "true"},
    {"vulkan_deferred_descriptor_writes", "true"},
    {"gpu_wait_reg_mem_spin", "true"},
    // Resolve readbacks batched, small one-off copies still exact (Session 74).
    {"readback_resolve_batch", "true"},
    {"readback_resolve_batch_min_bytes", "16385"},
    // Adreno keeps no shader cache of its own: the SDK's VkPipelineCache on disk.
    {"vulkan_disk_pipeline_cache", "true"},
    // The game sets the "invalid" type on many texture fetch constants (as 2K14 does): unset, the SDK skips those
    // textures and logs 500-1100 warnings a second on the GPU command thread.
    {"gpu_allow_invalid_fetch_constants", "true"},
    // The 2K14 port's vertex residency cache skips re-uploading vertex ranges it believes unchanged. On 2K16 that
    // left wrestlers' skin stretched into spikes in close-ups (phone test G, 2026-11-21: much better with it off).
    {"vulkan_vertex_residency_cache_off", "true"},
};

// The native renderer: the scene drawn by the SDK's own renderer with the game's shaders (no EDRAM emulation), on the
// host render-target path (phones have no fragment shader interlock).
const std::vector<Flag> kNative = {
    {"gpu_native", "2"},
    // The D3D library's GPR-flush point draws (24 one-vertex draws) are the only MSAA draws of a 2K16 match: drawn,
    // every frame falls back to the emulated path (21 fps); skipped, the match is truth frames (38-40 fps). Tests r3
    // and test D, 2026-11-21. (Not the cause of the skin spikes.)
    {"gpu_native_half2_skip_gpr_draws", "true"},
    {"gpu_native_truth", "true"},
    {"gpu_native_truth_host_rt", "true"},
    {"gpu_fast_clear", "true"},
    {"gpu_native_half2", "2"},
    {"gpu_native_lean_draws", "true"},
    {"gpu_native_lean_benign_writes", "true"},
    {"gpu_native_gpu_timers", "false"},
    {"vulkan_merge_adjacent_barriers", "true"},
    {"gpu_native_truth_batch_resolves", "true"},
    {"gpu_native_truth_batch_rename", "true"},
    {"gpu_native_truth_batch_fills", "true"},
    {"gpu_native_truth_lazy_pass_deps", "true"},
    {"gpu_native_truth_lazy_pass_style", "1"},
    {"gpu_native_truth_rename_depth", "true"},
    {"gpu_native_truth_pass_dont_care", "true"},
    {"gpu_native_truth_skip_restores", "true"},
    {"gpu_native_truth_fill_rename_check", "true"},
    {"vulkan_uniform_pool_device_local", "true"},
    {"vulkan_device_local_upload_min_heap_mb", "128"},
};

// Model-data shortcuts, off by default since the 2026-11-21 phone tests: with them on, near-camera models (the ring
// post, a wrestler's arm) came out as stretched spikes in NATIVE and NATIVE+ alike, at 480p and 720p. Each is a 2K14
// measurement that never met 2K16's data: trusting geometry pages (2K16 may rewrite vertex data the trust then misses),
// baking vertex endianness (the GPR-flush draw skip is in kNative: it is what makes match frames truth frames). Options > Advanced > Native shortcuts > ALL
// (WWE2K16_NATIVE_SHORTCUTS=1) turns them back on for testing.
const std::vector<Flag> kShortcuts = {
    {"gpu_native_trust_geometry_pages", "true"},
    {"gpu_native_vertex_endian_spec", "true"},
};

// (kShortcuts: on by default in the launcher since phone test T3, 2026-11-22.)
// More speed, less margin: resolves only a texture reads skip their write-back, textures load straight into their
// storage, shader specialisations. Try it after "native" is right.
const std::vector<Flag> kNativePlus = {
    {"gpu_native_truth_skip_writeback", "true"},
    {"gpu_native_truth_skip_writeback_scaled", "true"},
    {"gpu_native_truth_skip_repeat_resolves", "true"},
    {"gpu_native_truth_drop_dead_resolves", "1"},
    {"gpu_native_truth_direct_resolve_scaled", "true"},
    {"gpu_native_truth_direct_resolve_scaled_full", "true"},
    {"gpu_native_truth_direct_resolve_scaled_texture", "true"},
    {"gpu_native_truth_direct_resolve_scaled_full_texture", "true"},
    {"gpu_native_truth_direct_resolve_scaled_small", "true"},
    {"gpu_native_truth_direct_resolve_full_1x", "true"},
    {"gpu_native_truth_direct_resolve_depth", "true"},
    {"gpu_native_truth_fill_direct", "true"},
    {"gpu_native_truth_fill_direct_small", "true"},
    {"gpu_native_truth_fill_direct_depth", "true"},
    {"gpu_native_truth_defer_dest_use", "true"},
    {"gpu_native_truth_defer_dest_use_scaled", "true"},
    {"gpu_native_truth_texture_fill_scaled", "true"},
    {"gpu_native_truth_texture_fill_scaled_kinds", "32"},
    {"vulkan_texture_load_direct_unscaled", "true"},
    {"vulkan_texture_load_direct_unscaled_any_page", "true"},
    {"vulkan_texture_native_storage_small", "true"},
    {"vulkan_texture_sample_written_early", "true"},
    {"gpu_native_ps_texture_spec", "true"},
    {"gpu_native_ps_mul_mode", "2"},
    {"gpu_native_ps_implicit_lod", "true"},
    {"gpu_native_ps_epilogue_spec", "true"},
    {"gpu_native_vs_mul_mode", "1"},
    {"gpu_native_vs_mul_fetch_finite", "true"},
    {"gpu_native_fma", "3"},
};

// Render pass trim (Options > Graphics > Render pass trim, WWE2K16_PASS_TRIM=1): the WWE 2K14 port's "frame structure"
// work (its native_frame.h, Sessions 52-80), the parts that need nothing of 2K14's own: no shader hashes, no game-side
// owned frame. Each was byte-identical in 2K14's picture tests and cuts passes, copies or barriers:
//   colour_none        depth-only passes keep their colour with LOAD/STORE_OP_NONE (tile GPUs: no load + store)
//   clear_attachments  real depth clears (the GPU's hierarchical depth works)
//   dedupe_state       fewer state commands a draw
//   cover_outdated     fills that write every texel no longer refused
//   cube faces         reflection cube faces filled by their copies, one barrier pair for 6 faces
//   alias_direct*      alias loads straight from the owner images (store dispatches 208 -> 0 a 2K14 match frame)
//   defer_*_resolves   copies batched instead of flushing alone with their own barriers
//   skip_writeback_depth / drop_dead_copies / drop_dead_passes: copies and passes nothing reads turned into no-ops
//   exact_rows / carry_copies / serve_copies / serve_ahead: the scene copies served from the scene image, the
//                      scene pass no longer split for the arena screen (one 1280x720 colour + depth store / load less)
//   crowd_cull 2       crowd members entirely outside the draw's view skipped (exact)
// 2K16 is the same engine; whether every one holds here is what the phone test shows (OFF = the r9 renderer).
const std::vector<Flag> kPassTrim = {
    {"gpu_native_truth_color_none", "true"},
    {"gpu_native_truth_clear_attachments", "true"},
    {"gpu_native_truth_dedupe_state", "true"},
    {"gpu_native_truth_cover_outdated", "true"},
    {"gpu_native_truth_fill_cube_faces", "true"},
    {"gpu_native_truth_texture_only_cube_faces", "true"},
    {"gpu_native_truth_alias_direct", "true"},
    {"gpu_native_truth_alias_direct_rgba16", "true"},
    {"gpu_native_truth_alias_direct_lazy_flush", "true"},
    {"gpu_native_truth_defer_memory_resolves", "true"},
    {"gpu_native_truth_defer_depth_resolves", "true"},
    {"gpu_native_truth_defer_full_resolves", "true"},
    {"gpu_native_truth_skip_writeback_depth", "true"},
    {"gpu_native_truth_drop_dead_copies", "true"},
    {"gpu_native_truth_drop_dead_passes", "true"},
    {"gpu_native_truth_exact_rows", "true"},
    {"gpu_native_truth_carry_copies", "true"},
    {"gpu_native_truth_serve_copies", "true"},
    {"gpu_native_truth_serve_ahead", "true"},
    {"gpu_scene_crowd_cull", "2"},
};

// Command-processor fast path (WWE2K16_CP_FAST=1, Options > Advanced > CPU fast path, on by default): the WWE 2K14
// port's Session 6 / 7 defaults (its wwe2k14_app.h, "CP work per frame -38% in the heavy stretch, -32% in the match",
// each shadow-compared against the slow path with no difference). They live in 2K14's app, not in the SDK's defaults,
// so this port never had them. The performance log (2026-11-22) showed why they matter here: in a match the game's
// main thread (~65%) and the GPU command thread (~60%) take turns, the GPU itself needs only ~8.7 ms of a ~24 ms
// frame - the GPU command thread's work per draw is on the critical path.
const std::vector<Flag> kCpFast = {
    {"gpu_type0_bulk_writes", "true"},                // CP1: register ranges written in bulk
    {"gpu_type0_block_split", "true"},                // CP1B: mixed ranges split
    {"gpu_request_ranges_fast", "true"},              // CP2: no allocation, lock-free check per range
    {"vulkan_reuse_texture_bindings", "true"},        // CP5: texture descriptor sets reused when unchanged
    {"vulkan_sampler_param_cache", "true"},           // CP6: sampler parameters cached
    {"vulkan_constants_dynamic_offsets", "true"},     // CP7: constant buffers by dynamic offset (no set per draw)
    {"vulkan_viewport_info_cache", "true"},           // viewport info cached
    {"vulkan_async_swap", "true"},                    // no drain of the GPU at the swap
    {"vulkan_pipeline_no_submit_wait", "true"},       // no wait on background pipeline compiles
    {"gpu_request_textures_skip_unchanged", "true"},  // TEXREQ: unchanged texture requests skipped
    {"gpu_upload_skip_unchanged", "true"},            // UPSKIP: pages whose bytes equal the GPU copy not re-uploaded
};

// The native frame (WWE2K16_NATIVE_FRAME=1, Options > Advanced, test): the command processor answers the game
// (fences, waits) without drawing, a drawing side runs the same commands behind it - so the game's thread does not
// wait for the drawing. The 2K14 port's own frame (its native_frame.h); untested on 2K16.
const std::vector<Flag> kNativeFrame = {
    {"gpu_native_frame", "true"},
    {"gpu_native_frame_answers_lean", "true"},
};

// A value from the launcher's extra arguments (or the command line) wins: only settings still at default are set.
int Apply(const std::vector<Flag>& flags) {
  int set = 0;
  for (const auto& [name, value] : flags) {
    // Anything given on the command line / Extra arguments wins, even when it equals the default (false is a default).
#if WWE2K16_SDK_K14
    if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault || rex::cvar::HasNonDefaultValue(name)) continue;
#else
    if (rex::cvar::HasNonDefaultValue(name)) continue;  // (0.7 has no GetFlagSource; the profiles are 0.10-only anyway)
#endif
    if (rex::cvar::SetFlagByName(name, value)) {
      ++set;
    } else {
      REXLOG_WARN("[wwe2k16-renderer] {} is not a setting of this SDK", name);
    }
  }
  return set;
}

}  // namespace

std::string_view Profile() {
  const char* v = std::getenv("WWE2K16_RENDERER");
  const std::string_view p = v ? v : "";
  if (p == "native" || p == "native_plus") return p;
  return "classic";
}

void Apply() {
#if WWE2K16_SDK_K14
  const std::string_view profile = Profile();
  int set = 0;
#if defined(__ANDROID__)
  set += Apply(kPhone);
#endif
  if (profile == "native" || profile == "native_plus") set += Apply(kNative);
  if (profile == "native_plus") set += Apply(kNativePlus);
  const char* shortcuts = std::getenv("WWE2K16_NATIVE_SHORTCUTS");
  const bool with_shortcuts = shortcuts && shortcuts[0] == '1' && profile != "classic";
  if (with_shortcuts) set += Apply(kShortcuts);
  if (const char* cp = std::getenv("WWE2K16_CP_FAST"); cp && cp[0] == '1' && profile != "classic") {
    set += Apply(kCpFast);
    REXLOG_INFO("[wwe2k16-renderer] CPU fast path on ({} settings from the 2K14 port's Sessions 6 / 7)", kCpFast.size());
  }
  if (const char* nf = std::getenv("WWE2K16_NATIVE_FRAME"); nf && nf[0] == '1' && profile != "classic") {
    set += Apply(kNativeFrame);
    REXLOG_INFO("[wwe2k16-renderer] native frame on (answers and drawing apart)");
  }
  const char* trim = std::getenv("WWE2K16_PASS_TRIM");
  const bool with_trim = trim && trim[0] == '1' && profile != "classic";
  if (with_trim) {
    set += Apply(kPassTrim);
    REXLOG_INFO("[wwe2k16-renderer] render pass trim on ({} settings from the 2K14 port's frame structure)",
                kPassTrim.size());
  }
  REXLOG_INFO("[wwe2k16-renderer] profile {}{}: {} settings applied; gpu_native={} truth={} host_rt={}", profile,
              with_shortcuts ? " + model shortcuts" : "", set,
              rex::cvar::GetFlagByName("gpu_native"), rex::cvar::GetFlagByName("gpu_native_truth"),
              rex::cvar::GetFlagByName("gpu_native_truth_host_rt"));
#else
  if (Profile() != "classic") {
    REXLOG_WARN("[wwe2k16-renderer] WWE2K16_RENDERER={} needs the native build (tools/android-build-k14.sh); this "
                "build has the classic renderer only",
                Profile());
  }
#endif
}

}  // namespace wwe2k16::native_renderer
