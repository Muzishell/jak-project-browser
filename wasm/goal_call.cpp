#include "goal_call.h"

#include <emscripten.h>

#include <cstring>

#include "common/goal_constants.h"

#include "game/kernel/common/kscheme.h"
#include "game/runtime.h"
#include "wasm/goal_thread.h"

// GOAL's shadow stack pointer and process pointer are the mutable "sp" and "pp" globals the GOAL
// modules import. They are JS objects of the EE thread, where all GOAL code runs
// (wasm/goal_modules.js creates them). Pointers are unsigned: the heap can be above 2 GB.
EM_JS(u32, js_goal_get_sp, (), { return Module.goalStackPointer().value; });
EM_JS(void, js_goal_set_sp, (u32 sp), { Module.goalStackPointer().value = sp; });
EM_JS(u32, js_goal_get_pp, (), { return Number(BigInt.asUintN(32, Module.goalProcessPointer().value)); });
EM_JS(void, js_goal_set_pp, (u32 pp), { Module.goalProcessPointer().value = BigInt(pp >>> 0); });

EM_JS(int, js_goal_compile_module, (const u8* wasm, u32 size, u32* table_base), {
  wasm >>>= 0;
  return Module.goalCompileModule(HEAPU8.subarray(wasm, wasm + (size >>> 0)), table_base >>> 0);
});

// symbol names are passed to wasm_goal_symbol_offset (klink) through name_buf
EM_JS(void,
      js_goal_instantiate_module,
      (int handle,
       u32 ee_base,
       u32 st,
       const u8* xmm_area,
       const u32* seg_bases,
       char* name_buf,
       u32 name_buf_size),
      {
        const segs = HEAPU32.subarray(seg_bases >>> 2, (seg_bases >>> 2) + 3);
        name_buf >>>= 0;
        Module.goalInstantiateModule(handle, ee_base >>> 0, st >>> 0, xmm_area >>> 0,
                                     Array.from(segs), (name) => {
          if (name.length >= name_buf_size) {
            throw new Error("GOAL symbol name too long: " + name);
          }
          for (let i = 0; i < name.length; i++) {
            HEAPU8[name_buf + i] = name.charCodeAt(i);
          }
          HEAPU8[name_buf + name.length] = 0;
          return _wasm_goal_symbol_offset(name_buf);
        });
      });

namespace wasm_goal {

namespace {
// the 128-bit register area of the GOAL modules: xmm0 (results), then xmm1-8 (arguments)
alignas(16) u8 xmm_area[16 * 9];
}  // namespace

u64 get_pp() {
  return js_goal_get_pp();
}

void set_pp(u64 pp) {
  js_goal_set_pp(u32(pp));
}

u32 get_sp() {
  return js_goal_get_sp();
}

void set_sp(u32 sp) {
  js_goal_set_sp(sp);
}

u64 nothing_func(u64, u64, u64, u64, u64, u64, u64, u64) {
  return 0;
}

u64 zero_func(u64, u64, u64, u64, u64, u64, u64, u64) {
  return 0;
}

GoalFunc goal_function(u32 goal_function_addr) {
  u32 index;
  memcpy(&index, g_ee_main_mem + goal_function_addr, 4);
  return (GoalFunc)(uintptr_t)index;
}

void set_goal_function(u32 goal_function_addr, void* func) {
  // a C++ function pointer is its index in the indirect function table
  u32 slot[2] = {u32(uintptr_t(func)), 0};
  memcpy(g_ee_main_mem + goal_function_addr, slot, sizeof(slot));
}

u64 call(u32 goal_function_addr, u64 a0, u64 a1, u64 a2, u32 st) {
  if (js_goal_get_sp() == 0) {
    js_goal_set_sp(EE_MAIN_MEM_SIZE - GOAL_STACK_TOP_OFFSET);
  }
  u64 old_pp = get_pp();
  set_pp(st);
  u64 result = goal_function(goal_function_addr)(a0, a1, a2, 0, 0, 0, 0, 0);
  check_no_suspend("call_goal");
  set_pp(old_pp);
  return result;
}

u64 call_on_stack(u32 goal_function_addr, u32 sp, u32 st) {
  u32 old_sp = js_goal_get_sp();
  u64 old_pp = get_pp();
  js_goal_set_sp(sp);
  set_pp(st);
  u64 result = goal_function(goal_function_addr)(0, 0, 0, 0, 0, 0, 0, 0);
  check_no_suspend("call_goal_on_stack");
  set_pp(old_pp);
  js_goal_set_sp(old_sp);
  return result;
}

u64 call8(u32 goal_function_addr, const u64* args, u64 pp) {
  u64 old_pp = get_pp();
  set_pp(pp);
  u64 result = goal_function(goal_function_addr)(args[0], args[1], args[2], args[3], args[4],
                                                 args[5], args[6], args[7]);
  check_no_suspend("mips2c jalr");
  set_pp(old_pp);
  return result;
}

int compile_module(const u8* wasm, u32 size, u32* table_base) {
  return js_goal_compile_module(wasm, size, table_base);
}

void instantiate_module(int handle, u32 st, const u32 seg_bases[3]) {
  char name_buf[256];
  js_goal_instantiate_module(handle, u32(uintptr_t(g_ee_main_mem)), st, xmm_area, seg_bases,
                             name_buf, sizeof(name_buf));
}

}  // namespace wasm_goal
