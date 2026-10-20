#pragma once

/*!
 * @file audio_output.h
 * The audio output of the Emscripten runtimes, in place of cubeb (game/sound/989snd/player.cpp).
 * gk_web plays through SDL3's Web Audio backend (wasm/web/audio_web.cpp); gk_headless has no output.
 */

#include "common/common_types.h"

namespace wasm_audio {

//! Renders frame_count stereo frames of 48 kHz s16 samples (interleaved) into out.
using RenderFunc = void (*)(void* user, s16* out, int frame_count);

//! Start calling render in real time and play what it renders. Like the cubeb stream it replaces,
//! render keeps being called while the browser holds the output back (autoplay before a click):
//! the 989snd player's tick, which the sound code depends on, runs on it.
void start(RenderFunc render, void* user);
//! Stop calling render and close the output.
void stop();

}  // namespace wasm_audio
