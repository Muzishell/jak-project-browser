/*!
 * @file audio_web.cpp
 * gk_web's audio output (wasm/audio_output.h): an SDL3 audio stream, which Emscripten's SDL3 port
 * plays through Web Audio on the page's thread (resampling to the AudioContext rate and resuming
 * it on the first user gesture).
 *
 * A producer thread renders on the wall clock rather than when the output asks, so the 989snd
 * player keeps ticking while the browser holds the output back; the stream is kept between
 * kMinQueued and kMaxQueued ahead (a sleep in a browser worker lasts at least the OS timer tick,
 * ~16 ms). Rendering runs ahead when the output drains faster than the clock and drops audio when
 * the output stops taking it.
 */

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "common/log/log.h"

#include "wasm/audio_output.h"

namespace wasm_audio {
namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrameBytes = kChannels * sizeof(s16);
constexpr int kChunkFrames = 256;
constexpr int kMinQueued = kRate * 40 / 1000;
constexpr int kMaxQueued = kRate * 150 / 1000;

SDL_AudioStream* g_stream = nullptr;
std::thread g_thread;
std::atomic_bool g_running = false;

void produce(RenderFunc render, void* user) {
  using clock = std::chrono::steady_clock;
  std::vector<s16> chunk(kChunkFrames * kChannels);
  const auto start = clock::now();
  s64 rendered = 0;
  while (g_running) {
    const s64 due = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start)
                        .count() *
                    kRate / 1000000;
    while (g_running) {
      const int queued = SDL_GetAudioStreamQueued(g_stream) / kFrameBytes;
      if (rendered >= due && queued >= kMinQueued) {
        break;
      }
      render(user, chunk.data(), kChunkFrames);
      rendered += kChunkFrames;
      if (queued < kMaxQueued) {
        SDL_PutAudioStreamData(g_stream, chunk.data(), kChunkFrames * kFrameBytes);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

}  // namespace

void start(RenderFunc render, void* user) {
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    lg::error("audio: SDL audio init failed: {}", SDL_GetError());
    return;
  }
  const SDL_AudioSpec spec = {SDL_AUDIO_S16LE, kChannels, kRate};
  g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!g_stream) {
    lg::error("audio: no audio output: {}", SDL_GetError());
    return;
  }
  SDL_ResumeAudioStreamDevice(g_stream);
  g_running = true;
  g_thread = std::thread(produce, render, user);
  lg::info("audio: Web Audio output started");
}

void stop() {
  if (!g_stream) {
    return;
  }
  g_running = false;
  if (g_thread.joinable()) {
    g_thread.join();
  }
  SDL_DestroyAudioStream(g_stream);
  g_stream = nullptr;
}

}  // namespace wasm_audio
