#pragma once

/*!
 * @file goal_thread.h
 * GOAL thread switches and throw in the Emscripten runtime (CLAUDE.MD, M4). These are the C
 * functions the INSTRUCTION_SET 'wasm branches of goal_src/jak1/kernel/gkernel.gc and gstate.gc
 * call where x86 code saves and restores registers, stack pointers and return addresses.
 *
 * Wasm can't capture a pc or switch stacks, so:
 *
 * Suspend: thread-suspend unwinds the thread's wasm call stack with Binaryen's Asyncify (every
 * GOAL module is instrumented, goalc/compiler/wasm/WasmCodeGenerator.h) back to thread_run, which
 * keeps the unwound frames until the next thread-resume rewinds them. The thread's frames on the
 * GOAL shadow stack are copied to its backup stack and back by the GOAL code, like x86.
 *
 * Leaving a thread early (return-from-thread, abandon-thread, deactivate of the running process),
 * restarting the main thread with new code (enter-state) and throw are C++ exceptions, caught by
 * the C++ frame that entered the thread or the catch frame. Wasm exceptions unwind GOAL frames
 * like C++ frames, and the runtime is built with -fwasm-exceptions.
 *
 * A suspend can't unwind through a C++ frame, so there is none between a thread's entry and its
 * suspends: GOAL code calls GOAL code directly, and call_goal checks nothing suspended under it.
 */

#include "common/common_types.h"

namespace wasm_goal {

//! thread-resume: run a main thread on its stack until it suspends or leaves.
//! A fresh thread (set-to-run) calls bootstrap (set-to-run-bootstrap) with sp at the thread's
//! stack pointer, otherwise the suspended call stack is rewound. Its unwound frames are kept
//! between suspend and resume under an id in the u32 at the GOAL address blob_slot (0: none),
//! which is in the thread object, so it moves with a relocated process. enter-state restarts
//! bootstrap with sp at stack_top and pp at process.
u64 thread_run(u32 bootstrap, u32 fresh, u32 sp, u32 stack_top, u32 blob_slot, u32 process);
//! thread-suspend: start unwinding to thread_run. When the thread is resumed, the rewind ends
//! here and this returns.
u64 thread_suspend();
//! return-from-thread: leave the running thread (main or temporary) and go back to the kernel.
[[noreturn]] void thread_exit();
//! enter-state of the main thread: start the thread over with set-to-run-bootstrap.
[[noreturn]] void thread_restart();
//! Forget a suspended call stack (process deactivated while suspended), see thread_run.
void thread_free(u32 blob_slot);
//! reset-and-call: call func(thread, func) on a temporary thread, already set up by the caller.
u64 call_thread(u32 func, u32 thread);
//! catch-frame new: call func with the 6 parameters in param_block. Returns its result, or the
//! value thrown to this frame.
u64 catch_frame(u32 frame, u32 func, u32 param_block);
//! throw-dispatch: return value from the catch-frame new call of frame.
[[noreturn]] void throw_to(u32 frame, u64 value);

//! Abort if GOAL code suspended under a C++ call into GOAL (what is the call for the message).
void check_no_suspend(const char* what);

}  // namespace wasm_goal
