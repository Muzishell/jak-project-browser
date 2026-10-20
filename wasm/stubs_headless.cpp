/*!
 * @file stubs_headless.cpp
 * The renderer's entry points for gk_headless, which has no renderer (gk_web builds the real
 * game/graphics). Each does nothing, or the least the kernel needs.
 */

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/sceGraphicsInterface.h"
#include "game/graphics/screenshot.h"
#include "wasm/audio_output.h"

// graphics/sceGraphicsInterface.cpp: no display, so there is no vblank to wait for
u32 sceGsSyncPath(u32, u32) {
  return 0;
}
u32 sceGsSyncV(u32) {
  return 0;
}

// graphics/screenshot.cpp
bool g_want_screenshot = false;
void register_screen_shot_settings(ScreenShotSettings*) {}

// graphics/gfx.cpp. Headless: there is no renderer.
namespace Gfx {
GfxGlobalSettings g_global_settings;
game_settings::DebugSettings g_debug_settings;
SplashScreen g_splash;
const GfxRendererModule* GetCurrentRenderer() {
  return nullptr;
}
bool CollisionRendererGetMask(GfxGlobalSettings::CollisionRendererMode, s64) {
  return false;
}
void CollisionRendererSetMask(GfxGlobalSettings::CollisionRendererMode, s64) {}
void CollisionRendererClearMask(GfxGlobalSettings::CollisionRendererMode, s64) {}
void CollisionRendererSetMode(GfxGlobalSettings::CollisionRendererMode) {}
}  // namespace Gfx

// wasm/audio_output.h. Headless: no audio output, the sound player doesn't tick (as before).
namespace wasm_audio {
void start(RenderFunc, void*) {}
void stop() {}
}  // namespace wasm_audio

// graphics/display.cpp. Headless: there is no display, callers check for null.
namespace Display {
std::shared_ptr<GfxDisplay> GetMainDisplay() {
  return nullptr;
}
}  // namespace Display
