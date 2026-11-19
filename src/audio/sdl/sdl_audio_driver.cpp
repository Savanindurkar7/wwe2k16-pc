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
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/custom_music_player.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");

namespace rex::audio::sdl {

namespace {

bool AudioFrameProbeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("REX_AUDIO_FRAME_PROBE");
    return value && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

void CountDriverSubmitFrame(bool all_zero) {
  using Clock = std::chrono::steady_clock;
  static std::atomic<uint64_t> submissions{0};
  static std::atomic<uint64_t> zero_submissions{0};
  static std::atomic<int64_t> window_start_ns{0};
  submissions.fetch_add(1, std::memory_order_relaxed);
  if (all_zero) {
    zero_submissions.fetch_add(1, std::memory_order_relaxed);
  }

  const int64_t now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  int64_t start_ns = window_start_ns.load(std::memory_order_relaxed);
  if (!start_ns) {
    window_start_ns.compare_exchange_strong(start_ns, now_ns, std::memory_order_relaxed);
    return;
  }
  if (now_ns - start_ns < 1000000000LL ||
      !window_start_ns.compare_exchange_strong(start_ns, now_ns, std::memory_order_relaxed)) {
    return;
  }

  const uint64_t count = submissions.exchange(0, std::memory_order_relaxed);
  const uint64_t zero_count = zero_submissions.exchange(0, std::memory_order_relaxed);
  const double seconds = static_cast<double>(now_ns - start_ns) / 1.0e9;
  REXAPU_INFO(
      "[audio-frame-probe] SDLAudioDriver::SubmitFrame calls={} rate={:.1f}/s zero={} nonzero={} interval={:.0f}ms",
      count, count / seconds, zero_count, count - zero_count, seconds * 1000.0);
}

}  // namespace

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Prevent SDL from interfering with timer resolution (causes FPS drops)
  SDL_SetHintWithPriority(SDL_HINT_TIMER_RESOLUTION, "0", SDL_HINT_OVERRIDE);

  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  // A sound server that is briefly unavailable (e.g. being reconfigured by a streaming host as the title starts)
  // gets a few short retries before falling back to silent output.
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (OpenOutput()) {
      return true;
    }
    CloseOutput();
    REXAPU_WARN("Audio output not available (attempt {} of 4); retrying", attempt + 1);
    SDL_Delay(500);
  }
  // No usable audio output (no device, sound server unreachable, broken driver). Titles wait on the audio
  // thread to consume frames, so running without any output freezes them shortly after boot (seen 2026-10-01:
  // "Could not setup connection to PulseAudio" -> main thread spun forever in the sound engine). Fall back to
  // SDL's disk driver writing to the null device: it consumes frames at real-time pace, so the game runs silent.
  CloseOutput();
  REXAPU_WARN("No audio output could be opened; continuing without sound (silent real-time output)");
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "disk", SDL_HINT_OVERRIDE);
#ifdef _WIN32
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, "NUL", SDL_HINT_OVERRIDE);
#else
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, "/dev/null", SDL_HINT_OVERRIDE);
#endif
  if (OpenOutput()) {
    return true;
  }
  CloseOutput();
  return false;
}

void SDLAudioDriver::CloseOutput() {
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
}

bool SDLAudioDriver::OpenOutput() {
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  if (obtained_spec.channels == 2) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                            SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  if (AudioFrameProbeEnabled()) {
    bool all_zero = true;
    for (uint32_t sample = 0; sample < frame_samples_; ++sample) {
      if (rex::byte_swap(input_frame[sample]) != 0.0f) {
        all_zero = false;
        break;
      }
    }
    CountDriverSubmitFrame(all_zero);
  }
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

void SDLAudioDriver::Shutdown() {
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::MixCustomMusic(float* interleaved_output, uint32_t frames,
                                    uint32_t output_rate) {
  CustomMusicPlayer::instance().MixFrames(interleaved_output, frames, output_rate,
                                          sdl_device_channels_);
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const bool probe_enabled = AudioFrameProbeEnabled();
  using ProbeClock = std::chrono::steady_clock;
  static auto probe_window_start = ProbeClock::now();
  static uint64_t probe_chunks = 0;
  static uint64_t probe_queued_chunks = 0;
  static uint64_t probe_empty_chunks = 0;
  static uint64_t probe_nonzero_guest_chunks = 0;
  static uint64_t probe_nonzero_output_chunks = 0;
  static uint64_t probe_output_samples = 0;
  static double probe_output_squares = 0.0;
  static float probe_output_peak = 0.0f;
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    bool had_guest_frame = false;
    bool guest_frame_nonzero = false;
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      // Guest silence does not mean silence: a custom song keeps playing.
      driver->MixCustomMusic(data, channel_samples_, frame_frequency_);
      if (probe_enabled) {
        ++probe_empty_chunks;
      }
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      had_guest_frame = true;
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      if (probe_enabled) {
        guest_frame_nonzero = std::any_of(data, data + sample_count,
                                         [](float sample) { return sample != 0.0f; });
      }
      // Mixed on top of the guest frame so the entrance music plays with it
      // instead of replacing it (the game mutes its own music when the user
      // picks a custom song, but does not always do so immediately).
      driver->MixCustomMusic(data, channel_samples_, frame_frequency_);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
    }
    if (probe_enabled) {
      ++probe_chunks;
      if (had_guest_frame) {
        ++probe_queued_chunks;
        if (guest_frame_nonzero) {
          ++probe_nonzero_guest_chunks;
        }
      }
      bool output_nonzero = false;
      for (int sample_index = 0; sample_index < sample_count; ++sample_index) {
        const float value = data[sample_index];
        output_nonzero |= value != 0.0f;
        probe_output_squares += static_cast<double>(value) * value;
        probe_output_peak = std::max(probe_output_peak, std::abs(value));
      }
      probe_output_samples += static_cast<uint64_t>(sample_count);
      if (output_nonzero) {
        ++probe_nonzero_output_chunks;
      }
      const auto now = ProbeClock::now();
      const double seconds = std::chrono::duration<double>(now - probe_window_start).count();
      if (seconds >= 1.0) {
        const double output_rms = probe_output_samples
                                      ? std::sqrt(probe_output_squares / probe_output_samples)
                                      : 0.0;
        REXAPU_INFO(
            "[audio-frame-probe] SDLCallback chunks={} rate={:.1f}/s queued={} empty={} guest_nonzero={} output_nonzero={} output_rms={:.6f} peak={:.6f} interval={:.0f}ms",
            probe_chunks, probe_chunks / seconds, probe_queued_chunks, probe_empty_chunks,
            probe_nonzero_guest_chunks, probe_nonzero_output_chunks, output_rms, probe_output_peak,
            seconds * 1000.0);
        probe_window_start = now;
        probe_chunks = 0;
        probe_queued_chunks = 0;
        probe_empty_chunks = 0;
        probe_nonzero_guest_chunks = 0;
        probe_nonzero_output_chunks = 0;
        probe_output_samples = 0;
        probe_output_squares = 0.0;
        probe_output_peak = 0.0f;
      }
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
