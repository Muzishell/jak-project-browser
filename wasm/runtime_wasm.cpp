/*!
 * @file runtime_wasm.cpp
 * exec_runtime for the headless jak1 Emscripten build. Follows game/runtime.cpp (jak1 path)
 * step for step, minus what the browser runtime doesn't have yet:
 *  - EE memory is a block of linear memory (no mmap at a fixed address, no low-memory guard)
 *  - no DMP/deci2 thread: the listener is not ported (CLAUDE.MD, decision 4)
 *  - no sound or discord; a display only in gk_web (WASM_DISPLAY), where the graphics loop runs
 *    on the main pthread like runtime.cpp's main thread
 * Keep it in sync with runtime.cpp when the jak1 boot sequence there changes.
 */

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/versions/versions.h"

#include "game/kernel/common/fileio.h"
#include "game/kernel/common/kboot.h"
#include "game/kernel/common/kdgo.h"
#include "game/kernel/common/kdsnetm.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/klisten.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kmemcard.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kboot.h"
#include "game/kernel/jak1/kdgo.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/overlord/common/fake_iso.h"
#include "game/overlord/common/iso.h"
#include "game/overlord/common/sbank.h"
#include "game/overlord/common/srpc.h"
#include "game/overlord/common/ssound.h"
#include "game/overlord/jak1/dma.h"
#include "game/overlord/jak1/fake_iso.h"
#include "game/overlord/jak1/iso.h"
#include "game/overlord/jak1/iso_queue.h"
#include "game/overlord/jak1/overlord.h"
#include "game/overlord/jak1/ramdisk.h"
#include "game/overlord/jak1/srpc.h"
#include "game/overlord/jak1/stream.h"
#include "game/runtime.h"
#ifdef WASM_DISPLAY
#include <emscripten.h>

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"

namespace webgl_compat {
// wasm/web/webgl_compat.cpp
void log_errors();
}  // namespace webgl_compat
#endif
#include "game/sce/deci2.h"
#include "game/sce/iop.h"
#include "game/sce/libcdvd_ee.h"
#include "game/sce/sif_ee.h"
#include "game/system/SystemThread.h"
#include "game/system/iop_thread.h"

u8* g_ee_main_mem = nullptr;
u8* g_ee_main_mem_exec = nullptr;
std::thread::id g_main_thread_id = std::thread::id();
GameVersion g_game_version = GameVersion::Jak1;
BackgroundWorker g_background_worker;
int g_server_port = DECI2_PORT;

namespace {

int g_argc = 0;
const char** g_argv = nullptr;

void ee_runner(SystemThreadInterface& iface) {
  // EE memory is a block of linear memory; GOAL code gets its address as ee_base
  g_ee_main_mem = (u8*)aligned_alloc(4096, EE_MAIN_MEM_SIZE);
  if (!g_ee_main_mem) {
    lg::error("Failed to allocate main memory");
    iface.initialization_complete();
    return;
  }
  g_ee_main_mem_exec = g_ee_main_mem;
  lg::debug("Main memory at 0x{:08x}, {} MB", (u32)(uintptr_t)g_ee_main_mem,
            EE_MAIN_MEM_SIZE / (1 << 20));

  lg::debug("[EE] Initialization complete!");
  iface.initialization_complete();

  lg::debug("[EE] Run!");
  memset((void*)g_ee_main_mem, 0, EE_MAIN_MEM_SIZE);
  // no 512 kB null guard in wasm (CLAUDE.MD, decision 6)

  fileio_init_globals();
  jak1::kboot_init_globals();
  kboot_init_globals_common();
  kdgo_init_globals();
  jak1::kdgo_init_globals();
  kdsnetm_init_globals_common();
  klink_init_globals();
  kmachine_init_globals_common();
  jak1::kscheme_init_globals();
  kscheme_init_globals_common();
  kmalloc_init_globals_common();
  klisten_init_globals();
  jak1::klisten_init_globals();
  kmemcard_init_globals();
  kprint_init_globals_common();

  jak1::goal_main(g_argc, g_argv);
  lg::debug("[EE] Done!");

  iop::LIBRARY_kill();
  iface.trigger_shutdown();
}

void ee_worker_runner(SystemThreadInterface& iface) {
  iface.initialization_complete();
  while (!iface.get_want_exit()) {
    if (!g_background_worker.process_queues()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

void iop_runner(SystemThreadInterface& iface) {
  IOP iop;
  lg::debug("[IOP] Restart!");
  iop.reset_allocator();
  ee::LIBRARY_sceSif_register(&iop);
  iop::LIBRARY_register(&iop);
#ifdef WASM_DISPLAY
  Gfx::register_vsync_callback([&iop]() { iop.kernel.signal_vblank(); });
#endif
  // headless: no display, so nothing calls iop.kernel.signal_vblank()

  jak1::dma_init_globals();
  iso_init_globals();
  jak1::iso_init_globals();
  fake_iso_init_globals();
  jak1::fake_iso_init_globals();
  jak1::iso_queue_init_globals();
  jak1::ramdisk_init_globals();
  sbank_init_globals();
  jak1::srpc_init_globals();
  srpc_init_globals();
  ssound_init_globals();
  jak1::stream_init_globals();

  iface.initialization_complete();

  lg::debug("[IOP] Wait for OVERLORD to start...");
  iop.wait_for_overlord_start_cmd();
  if (iop.status == IOP_OVERLORD_INIT) {
    lg::debug("[IOP] Run!");
  } else {
    lg::debug("[IOP] Shutdown!");
    return;
  }

  iop.reset_allocator();
  bool complete = false;
  jak1::start_overlord_wrapper(iop.overlord_argc, iop.overlord_argv, &complete);
  while (!complete) {
    iop.kernel.dispatch();
  }
  iop.signal_overlord_init_finish();

  while (!iface.get_want_exit() && !iop.want_exit) {
    auto wait_duration = iop.kernel.dispatch();
    if (wait_duration) {
      iop.wait_run_iop(*wait_duration);
    }
  }
#ifdef WASM_DISPLAY
  Gfx::clear_vsync_callback();
#endif
}
}  // namespace

RuntimeExitStatus exec_runtime(GameLaunchOptions game_options, int argc, const char** argv) {
  g_argc = argc;
  g_argv = argv;
  g_main_thread_id = std::this_thread::get_id();
  g_game_version = game_options.game_version;
  ASSERT_MSG(g_game_version == GameVersion::Jak1, "the wasm runtime is jak1 only");
#ifdef WASM_DISPLAY
  bool enable_display = !game_options.disable_display;
  // graphics first: the EE uploads textures during boot
  if (enable_display) {
    Gfx::Init(g_game_version);
  }
#endif

  iop::LIBRARY_INIT();
  ee::LIBRARY_INIT_sceCd();
  ee::LIBRARY_INIT_sceDeci2();
  ee::LIBRARY_INIT_sceSif();

  // static: with a display, this function unwinds into the browser's frame loop and the threads
  // are joined from there
  static SystemThreadManager tm;
  auto& iop_thread = tm.create_thread("IOP");
  static auto* ee_thread = &tm.create_thread("EE");
  auto& ee_worker_thread = tm.create_thread("EE-Worker");

  iop_thread.start([](SystemThreadInterface& sti) { iop_runner(sti); });
  ee_worker_thread.start(ee_worker_runner);
  ee_thread->start(ee_runner);

  static auto shutdown = []() {
    ee_thread->join();
    tm.shutdown();
    tm.join();
    lg::info("GOAL Runtime Shutdown (code {})", fmt::underlying(MasterExit));
    free(g_ee_main_mem);
  };

#ifdef WASM_DISPLAY
  if (enable_display) {
    // the video loop, one frame per browser animation frame instead of runtime.cpp's blocking
    // Gfx::Loop: a WebGL context on an OffscreenCanvas only presents when its thread returns to
    // the event loop. When the EE asks to exit, shut down from there.
    emscripten_set_main_loop(
        []() {
          if (MasterExit == RuntimeExitStatus::RUNNING) {
            if (Display::GetMainDisplay()) {
              Display::GetMainDisplay()->render();
              webgl_compat::log_errors();
            }
            return;
          }
          emscripten_cancel_main_loop();
          shutdown();
          Gfx::Exit();
          emscripten_force_exit(MasterExit == RuntimeExitStatus::EXIT ? 0 : int(MasterExit));
        },
        // a 60 Hz timer rather than requestAnimationFrame (fps 0), which a hidden or throttled
        // page stops: the EE waits for each frame (vsync), so the game would stall with it
        60, true);
  }
#endif

  shutdown();
  return MasterExit;
}
