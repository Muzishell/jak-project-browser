#include "goal_thread.h"

#include <cstring>
#include <emscripten.h>
#include <vector>

#include "common/util/Assert.h"

#include "game/runtime.h"
#include "wasm/goal_call.h"

#include "fmt/format.h"

// the Asyncify globals every GOAL module imports (wasm/goal_modules.js)
EM_JS(int, js_asyncify_get_state, (), { return Module.goalAsyncifyState().value; });
EM_JS(void, js_asyncify_set, (int state, u32 data), {
  Module.goalAsyncifyState().value = state;
  Module.goalAsyncifyData().value = data;
});

namespace wasm_goal {
namespace {

// __asyncify_state
enum AsyncifyState { NORMAL = 0, UNWINDING = 1, REWINDING = 2 };

// what __asyncify_data points to: the asyncify stack grows up from pos, error past end
struct AsyncifyData {
  u32 pos;
  u32 end;
};

// one unwind happens at a time (only the EE thread runs GOAL code): unwind into this buffer, then
// copy the used part to the thread's blob. Rewinding pops it back from the top.
constexpr u32 kUnwindBufferSize = 1 << 20;
alignas(16) u8 unwind_buffer[kUnwindBufferSize];
AsyncifyData unwind_data;

u32 linear_addr(const void* p) {
  return u32(uintptr_t(p));
}

void start_unwind_data(u32 used) {
  unwind_data.pos = linear_addr(unwind_buffer) + used;
  unwind_data.end = linear_addr(unwind_buffer) + kUnwindBufferSize;
}

// the C++ exceptions that leave GOAL frames
struct ThreadExit {};
struct ThreadRestart {};
struct GoalThrow {
  u32 frame;
  u64 value;
};

// unwound call stacks of suspended threads, by id - 1
std::vector<std::vector<u8>> blobs;
std::vector<u32> free_ids;

u32 read_u32(u32 goal_addr) {
  u32 value;
  memcpy(&value, g_ee_main_mem + goal_addr, 4);
  return value;
}

void write_u32(u32 goal_addr, u32 value) {
  memcpy(g_ee_main_mem + goal_addr, &value, 4);
}

std::vector<u8>& blob(u32 id) {
  ASSERT_MSG(id > 0 && id <= blobs.size(), fmt::format("wasm: bad suspended thread id {}", id));
  return blobs[id - 1];
}

u32 new_blob(const u8* data, u32 size) {
  u32 id;
  if (free_ids.empty()) {
    blobs.emplace_back();
    id = u32(blobs.size());
  } else {
    id = free_ids.back();
    free_ids.pop_back();
  }
  blob(id).assign(data, data + size);
  return id;
}

void free_blob(u32 id) {
  if (id) {
    blob(id).clear();
    free_ids.push_back(id);
  }
}

}  // namespace

void check_no_suspend(const char* what) {
  ASSERT_MSG(js_asyncify_get_state() == NORMAL,
             fmt::format("wasm: GOAL code suspended under {}, which can't be suspended", what));
}

u64 thread_run(u32 bootstrap, u32 fresh, u32 sp, u32 stack_top, u32 blob_slot, u32 process) {
  check_no_suspend("thread-resume");
  const u32 kernel_sp = get_sp();
  const u32 id = read_u32(blob_slot);
  write_u32(blob_slot, 0);
  if (fresh) {
    // set-to-run on a suspended thread drops its stack
    free_blob(id);
  } else {
    ASSERT_MSG(id, "wasm: thread-resume of a thread that has no suspended stack");
    auto& frames = blob(id);
    memcpy(unwind_buffer, frames.data(), frames.size());
    start_unwind_data(u32(frames.size()));
    free_blob(id);
    js_asyncify_set(REWINDING, linear_addr(&unwind_data));
  }

  set_sp(sp);
  for (;;) {
    try {
      // both a fresh start and a rewind enter through bootstrap, the thread's outermost frame
      goal_function(bootstrap)(0, 0, 0, 0, 0, 0, 0, 0);
    } catch (const ThreadRestart&) {
      // enter-state: the code of the new state starts on an empty stack
      ASSERT(js_asyncify_get_state() == NORMAL);
      set_sp(stack_top);
      set_pp(process);
      continue;
    } catch (const ThreadExit&) {
    } catch (const GoalThrow& t) {
      ASSERT_MSG(false,
                 fmt::format("wasm: throw to catch frame #x{:x}, which is not in the running "
                             "thread",
                             t.frame));
    }
    break;
  }

  const int state = js_asyncify_get_state();
  if (state == UNWINDING) {
    // the thread suspended: keep its frames until the next resume
    js_asyncify_set(NORMAL, 0);
    write_u32(blob_slot, new_blob(unwind_buffer, unwind_data.pos - linear_addr(unwind_buffer)));
  } else {
    ASSERT_MSG(state == NORMAL, "wasm: a resumed thread returned before reaching its suspend");
  }
  set_sp(kernel_sp);
  return 0;
}

u64 thread_suspend() {
  if (js_asyncify_get_state() == REWINDING) {
    // resumed: the rewind is back where the unwind started
    ASSERT(unwind_data.pos == linear_addr(unwind_buffer));
    js_asyncify_set(NORMAL, 0);
    return 0;
  }
  start_unwind_data(0);
  js_asyncify_set(UNWINDING, linear_addr(&unwind_data));
  return 0;
}

void thread_exit() {
  throw ThreadExit{};
}

void thread_restart() {
  throw ThreadRestart{};
}

void thread_free(u32 blob_slot) {
  free_blob(read_u32(blob_slot));
  write_u32(blob_slot, 0);
}

u64 call_thread(u32 func, u32 thread) {
  check_no_suspend("reset-and-call");
  u64 result = 0;
  try {
    // with the arguments the x86 version jumps with
    result = goal_function(func)(thread, func, 0, 0, 0, 0, 0, 0);
  } catch (const ThreadExit&) {
  } catch (const ThreadRestart&) {
    ASSERT_MSG(false, "wasm: enter-state restarted a temporary thread");
  }
  if (js_asyncify_get_state() == UNWINDING) {
    // a temporary thread is never resumed (x86 saves it to a backup stack nobody restores)
    js_asyncify_set(NORMAL, 0);
  }
  return result;
}

u64 catch_frame(u32 frame, u32 func, u32 param_block) {
  const u32 sp = get_sp();
  u64 params[6];
  memcpy(params, g_ee_main_mem + param_block, sizeof(params));
  try {
    u64 result =
        goal_function(func)(params[0], params[1], params[2], params[3], params[4], params[5], 0, 0);
    check_no_suspend("a catch frame");
    return result;
  } catch (const GoalThrow& t) {
    if (t.frame != frame) {
      throw;
    }
    // the x86 version restores the stack pointer saved in the frame
    set_sp(sp);
    return t.value;
  }
}

void throw_to(u32 frame, u64 value) {
  throw GoalThrow{frame, value};
}

}  // namespace wasm_goal
