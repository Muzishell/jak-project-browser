/*!
 * @file runtime_wasm.cpp
 * exec_runtime for the headless jak1 Emscripten build. Follows game/runtime.cpp (jak1 path)
 * step for step, minus what the browser runtime doesn't have yet:
 *  - EE memory is a block of linear memory (no mmap at a fixed address, no low-memory guard)
 *  - no DMP/deci2 thread: the listener is not ported (CLAUDE.MD, decision 4)
 *  - no discord; a display and audio output only in gk_web (WASM_DISPLAY, wasm/web/audio_web.cpp),
 *    where the graphics loop runs on the main pthread like runtime.cpp's main thread
 * Keep it in sync with runtime.cpp when the jak1 boot sequence there changes.
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
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
#include <atomic>
#include <emscripten.h>

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/system/hid/display_manager.h"
#include <emscripten/html5_webgl.h>

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

#ifdef WASM_DISPLAY
// The display's refreshes: a requestAnimationFrame chain in the render thread's worker (rAF in a
// dedicated worker follows the page's frames, and stops while the page is hidden) calls
// gk_refresh, below, with each refresh's timestamp. (A plain function: clang-format breaks "=>".)
EM_JS(void, gk_start_refresh_loop, (), {
  if (typeof requestAnimationFrame != "function") {
    return;
  }
  function tick(time) {
    requestAnimationFrame(tick);
    _gk_refresh(time);
  }
  requestAnimationFrame(tick);
});

/*!
 * When to render. The page shows a frame on the display's refresh. Rendering on a 60 Hz clock
 * drifts against the refresh (59.94 Hz, timer jitter...): when the two cross, a refresh gets no new
 * frame or two, which showed as a one-to-two-frame freeze about once per second. And with an
 * OffscreenCanvas, a frame drawn after the worker's rAF callback is committed with the next
 * animation frame, which rAF then waits for (about 25 fps). So while refreshes come in, each one
 * asks for a frame (at most 60 per second on 120/144 Hz displays) and renders it inside its rAF
 * callback when the EE's DMA chain is there, which is the usual case: the EE has the whole
 * refresh to compute it. A late chain is rendered by the main loop as soon as it arrives; until
 * then no GL call is made and the previous image stays on screen. Without refreshes (hidden page,
 * the desktop app's pane) frames are due on a 60 Hz clock as before, because the EE waits for every
 * frame and would stall.
 */
struct FramePacer {
  static constexpr double kFrameMs = 1000.0 / 60.0;
  // like render_game_frame's native wait: render without a chain after that, which also wakes an
  // EE that waits for vsync without having sent one
  static constexpr double kChainTimeoutMs = 40;
  // no refresh for that long: the page isn't drawing frames
  static constexpr double kRefreshLostMs = 50;

  double last_refresh_seen = -1e9;  // emscripten_get_now of the last refresh
  // rAF timestamps (comparable with each other only) for the 60-per-second limit
  double refresh_time = 0, frame_refresh_time = -1e9, pending_refresh_time = 0;
  double pending_since = -1;  // a frame is due since then, waiting for the chain
  double next_clock_frame = 0;
  bool refresh_paced = false;
  bool chain_timed_out = false;
  bool in_refresh = false;  // rendering inside the rAF callback
  int refresh_gaps = 0;     // refreshes more than 25 ms apart: frames the browser didn't draw

  // ?pacing=clock (GK_FRAME_PACING=clock): ignore refreshes, the clock pacing used until now
  bool clock_only = [] {
    const char* p = std::getenv("GK_FRAME_PACING");
    return p && std::string(p) == "clock";
  }();

  void on_refresh(double time, double now) {
    if (clock_only) {
      return;
    }
    refresh_gaps += last_refresh_seen > 0 && time - refresh_time > 25;
    refresh_time = time;
    last_refresh_seen = now;
    if (pending_since < 0 && time - frame_refresh_time >= kFrameMs - 2) {
      pending_since = now;
      pending_refresh_time = time;
    }
  }

  bool refreshes_coming(double now) const {
    return !clock_only && now - last_refresh_seen < kRefreshLostMs;
  }

  bool due(double now) {
    refresh_paced = now - last_refresh_seen < kRefreshLostMs;
    if (pending_since < 0) {
      if (refresh_paced || now < next_clock_frame) {
        return false;
      }
      pending_since = now;
    }
    chain_timed_out = !Gfx::chain_ready();
    return !chain_timed_out || now - pending_since >= kChainTimeoutMs;
  }

  void rendered() {
    if (refresh_paced) {
      frame_refresh_time = pending_refresh_time;
    }
    // keep the clock cadence, unless a frame was late by more than a frame
    next_clock_frame = pending_since - next_clock_frame > kFrameMs ? pending_since + kFrameMs
                                                                   : next_clock_frame + kFrameMs;
    pending_since = -1;
  }
};

/*!
 * Frame timing probe, on with ?stats=1 (shell.html sets GK_FRAME_STATS): one summary line every
 * 10 s, nothing per frame (a print from a pthread waits for the page).
 */
struct FrameStats {
  bool enabled = std::getenv("GK_FRAME_STATS") != nullptr;
  double window_start = -1, last_frame = -1, max_gap = 0, gap_sum = 0;
  // time inside Display::render (the renderer's GL calls), to tell a slow browser WebGL from slow
  // game code: the EE's frame waits for the renderer, so either shows as a long kernel dispatch
  double render_sum = 0, render_max = 0;
  int frames = 0, refresh_frames = 0, in_refresh_frames = 0, slow_gaps = 0, chain_timeouts = 0,
      refresh_gaps_at_start = 0, slow_renders = 0;

  void rendered(double ms) {
    if (!enabled) {
      return;
    }
    render_sum += ms;
    render_max = std::max(render_max, ms);
    slow_renders += ms > 16;
  }

  void frame(double now, const FramePacer& pacer) {
    if (!enabled) {
      return;
    }
    if (window_start < 0) {
      window_start = now;
    } else {
      const double gap = now - last_frame;
      max_gap = std::max(max_gap, gap);
      gap_sum += gap;
      slow_gaps += gap > 25;
    }
    last_frame = now;
    frames++;
    refresh_frames += pacer.refresh_paced;
    in_refresh_frames += pacer.in_refresh;
    chain_timeouts += pacer.chain_timed_out;
    if (now - window_start >= 10000) {
      lg::info(
          "[frames] {:.1f} s: {} frames ({} on refreshes, {} inside rAF), mean gap {:.2f} ms, "
          "max {:.1f} ms, {} gaps > 25 ms, {} without a chain, {} refresh gaps > 25 ms; render "
          "mean {:.2f} ms, max {:.1f} ms, {} > 16 ms",
          (now - window_start) / 1000, frames, refresh_frames, in_refresh_frames,
          frames > 1 ? gap_sum / (frames - 1) : 0.0, max_gap, slow_gaps, chain_timeouts,
          pacer.refresh_gaps - refresh_gaps_at_start, frames ? render_sum / frames : 0.0,
          render_max, slow_renders);
      const int gaps = pacer.refresh_gaps;
      *this = FrameStats();
      window_start = now;
      last_frame = now;
      refresh_gaps_at_start = gaps;
    }
  }
};

FramePacer g_pacer;
FrameStats g_frame_stats;

/*!
 * The main loop's timing. Immediate ticks (setImmediate timing: scheduler.postTask, back to back)
 * only while they are needed: without refreshes (frames on the 60 Hz clock) and while a refresh's
 * frame waits for a late chain. Otherwise the refreshes drive the frames and the loop ticks on a
 * slow timer. Back-to-back postTask allocates a task and a promise per tick, and Firefox's garbage
 * and cycle collectors then paused the render thread for 200-500 ms about once a second, which the
 * EE waited out in gl_sync_path (CLAUDE.MD Log 2026-10-21). SDL also sets its own timing (rAF for
 * vsync) when the renderer changes the swap interval, so this runs around every frame.
 */
void update_loop_timing(double now) {
  // rounded up to the OS timer tick in a worker (15.6 ms on Windows); it only notices refreshes
  // stopping (then immediate ticks take over within kRefreshLostMs)
  constexpr int kIdleTickMs = 10;
  const bool immediate = !g_pacer.refreshes_coming(now) || g_pacer.pending_since >= 0;
  int mode, value;
  emscripten_get_main_loop_timing(&mode, &value);
  if (immediate && mode != EM_TIMING_SETIMMEDIATE) {
    emscripten_set_main_loop_timing(EM_TIMING_SETIMMEDIATE, 0);
  } else if (!immediate && (mode != EM_TIMING_SETTIMEOUT || value != kIdleTickMs)) {
    emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, kIdleTickMs);
  }
}

// The game view's size in CSS pixels, asked by the page (gk_set_view_size); applied by the render
// thread, which owns the window and its OffscreenCanvas.
std::atomic<int> g_view_w = 0, g_view_h = 0;
int g_applied_view_w = 0, g_applied_view_h = 0;

void apply_view_size() {
  const int w = g_view_w, h = g_view_h;
  if (w <= 0 || h <= 0 || (w == g_applied_view_w && h == g_applied_view_h)) {
    return;
  }
  auto display = Display::GetMainDisplay();
  if (!display || !display->get_display_manager()) {
    return;
  }
  // like a resized desktop window: SDL resizes the canvas (times the pixel ratio, the window is
  // high density) and the renderer draws at the new framebuffer size
  display->get_display_manager()->enqueue_set_window_size(w, h);
  g_applied_view_w = w;
  g_applied_view_h = h;
}

// one frame, if one is due: from the rAF callback or the main loop
void render_if_due(double now) {
  if (MasterExit != RuntimeExitStatus::RUNNING || !g_pacer.due(now)) {
    return;
  }
  apply_view_size();
  g_frame_stats.frame(now, g_pacer);
  g_pacer.rendered();
  if (Display::GetMainDisplay()) {
    const double render_start = emscripten_get_now();
    Display::GetMainDisplay()->render();
    webgl_compat::log_errors();
    g_frame_stats.rendered(emscripten_get_now() - render_start);
    // the drawing buffer the frame went to, once per change (what the view size gave)
    static int logged_w = 0, logged_h = 0;
    int fb_w = 0, fb_h = 0;
    emscripten_webgl_get_drawing_buffer_size(emscripten_webgl_get_current_context(), &fb_w, &fb_h);
    if (fb_w != logged_w || fb_h != logged_h) {
      lg::info("[view] drawing buffer {}x{} (view {}x{} CSS px)", fb_w, fb_h, g_applied_view_w,
               g_applied_view_h);
      logged_w = fb_w;
      logged_h = fb_h;
    }
  }
}
#endif
}  // namespace

#ifdef WASM_DISPLAY
// called by the page (wasm/web/page.js) when the game view's size changes, in CSS pixels
extern "C" EMSCRIPTEN_KEEPALIVE void gk_set_view_size(int w, int h) {
  g_view_w = w;
  g_view_h = h;
}

// called by the worker's rAF chain (gk_start_refresh_loop) on each display refresh
extern "C" EMSCRIPTEN_KEEPALIVE void gk_refresh(double time) {
  const double now = emscripten_get_now();
  g_pacer.on_refresh(time, now);
  g_pacer.in_refresh = true;
  render_if_due(now);
  g_pacer.in_refresh = false;
  // a frame still due here waits for its chain: immediate ticks render it when it arrives
  update_loop_timing(now);
}
#endif

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
    // the video loop, one frame per main loop tick instead of runtime.cpp's blocking Gfx::Loop:
    // a WebGL context on an OffscreenCanvas only presents when its thread returns to the event
    // loop. When the EE asks to exit, shut down from there.
    gk_start_refresh_loop();
    emscripten_set_main_loop(
        []() {
          if (MasterExit == RuntimeExitStatus::RUNNING) {
            // Paced by FramePacer, not by the main loop's timing: in a worker a browser rounds
            // setTimeout up to the OS timer tick (15.6 ms on Windows: a 60 Hz timeout ran at
            // 32 Hz), and the main loop's rAF timing would stall the game while the page is
            // hidden. Immediate ticks render a chain that came late for its refresh, and the
            // clock-paced frames; update_loop_timing slows the loop down when neither is
            // waiting. This replaces the renderer's frame limiter, which sleeps.
            const double now = emscripten_get_now();
            render_if_due(now);
            update_loop_timing(now);
            return;
          }
          emscripten_cancel_main_loop();
          shutdown();
          Gfx::Exit();
          emscripten_force_exit(MasterExit == RuntimeExitStatus::EXIT ? 0 : int(MasterExit));
        },
        // until the first tick switches to setImmediate timing
        60, true);
  }
#endif

  shutdown();
  return MasterExit;
}
