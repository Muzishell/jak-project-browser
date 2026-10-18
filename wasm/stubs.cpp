/*!
 * @file stubs.cpp
 * Entry points of the parts the headless Emscripten runtime leaves out (graphics, the debugger and
 * listener, the discord library, mips2c). Each does nothing, or the least the kernel needs.
 */

#include "common/cross_os_debug/xdbg.h"

#include "game/external/discord.h"
#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/sceGraphicsInterface.h"
#include "game/graphics/screenshot.h"
#include "game/mips2c/mips2c_table.h"
#include "game/system/Deci2Server.h"

#include "third-party/discord-rpc/include/discord_rpc.h"

// cross_os_debug/xdbg.cpp only has Linux/Windows versions; the debugger is not ported
namespace xdbg {
std::string ThreadID::to_string() const {
  return "wasm-thread";
}
ThreadID get_current_thread_id() {
  return ThreadID();
}
}  // namespace xdbg

// system/Deci2Server.cpp: the listener is not ported (decision 4), so no server is ever created.
// sce/deci2.cpp still calls into it when the kernel registers its protocol driver.
void Deci2Server::send_data(void*, u16) {}
void Deci2Server::send_proto_ready(Deci2Driver*, int*) {}
void Deci2Server::send_shutdown() {}
void Deci2Server::lock() {}
void Deci2Server::unlock() {}

// the discord-rpc library (game/external/discord*.cpp are built): no discord in the browser
extern "C" {
void Discord_Initialize(const char*, DiscordEventHandlers*, int, const char*) {}
void Discord_Shutdown() {}
void Discord_UpdatePresence(const DiscordRichPresence*) {}
void Discord_ClearPresence() {}
}

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

// graphics/display.cpp. Headless: there is no display, callers check for null.
namespace Display {
std::shared_ptr<GfxDisplay> GetMainDisplay() {
  return nullptr;
}
}  // namespace Display

// mips2c/mips2c_table.cpp. mips2c functions are not wired up for wasm yet (they become table
// entries like the C kernel functions, decision 3).
namespace Mips2C {
PerGameVersion<std::unordered_map<std::string, std::vector<void (*)()>>> gMips2CLinkCallbacks = {
    {}, {}, {}, {}};
LinkedFunctionTable gLinkedFunctionTable;
Rng gRng;

void LinkedFunctionTable::reg(const std::string&, u64 (*)(void*), u32) {}

u32 LinkedFunctionTable::get(const std::string& name) {
  ASSERT_MSG(false, "mips2c function " + name + " is not available in the wasm runtime yet");
  return 0;
}
}  // namespace Mips2C
