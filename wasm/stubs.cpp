/*!
 * @file stubs.cpp
 * Entry points of the parts both Emscripten runtimes leave out (the debugger and listener, the
 * discord library). Each does nothing, or the least the kernel needs. The renderer's are in
 * stubs_headless.cpp.
 */

#include "common/cross_os_debug/xdbg.h"

#include "game/external/discord.h"
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
