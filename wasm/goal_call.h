#pragma once

/*!
 * @file goal_call.h
 * Calls between C++ and GOAL code in the Emscripten runtime.
 *
 * Every GOAL function is a wasm function of type (i64 x8) -> i64 in Emscripten's indirect function
 * table, which GOAL modules import as their table, so a C++ function pointer of type GoalFunc is a
 * table index GOAL can call, and a GOAL function's table index is a GoalFunc C++ can call.
 * The conversions follow what the native x86 stubs do (game/kernel/asm_funcs_x86_64.asm): the GOAL
 * argument registers are the C arguments, and rax is the result.
 */

#include <bit>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "common/common_types.h"

#include "game/kernel/common/Ptr.h"

namespace wasm_goal {

template <typename T>
struct is_goal_ptr : std::false_type {};
template <typename T>
struct is_goal_ptr<Ptr<T>> : std::true_type {};

using GoalFunc = u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64);

//! The process pointer (the x86 r13 / GOAL pp register): the mutable "pp" global of the modules.
u64 get_pp();
void set_pp(u64 pp);
//! The GOAL shadow stack pointer (the rsp register), a GOAL address: the mutable "sp" global.
u32 get_sp();
void set_sp(u32 sp);

template <typename T>
T from_reg(u64 value) {
  if constexpr (std::is_same_v<T, float>) {
    // GOAL passes floats as their bits in a GPR
    return std::bit_cast<float>(u32(value));
  } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
    return T(value);
  } else if constexpr (is_goal_ptr<T>::value) {
    // a GOAL address: Ptr<T> is passed by value, its u32 is the low half of the register
    return T(u32(value));
  } else if constexpr (std::is_pointer_v<T>) {
    // natively the register is passed as is; these functions don't dereference it
    return T(uintptr_t(value));
  } else {
    static_assert(std::is_integral_v<T>, "GOAL arguments are integers, floats or pointers");
  }
}

template <typename R>
u64 to_reg(R value) {
  if constexpr (std::is_same_v<R, float>) {
    return std::bit_cast<u32>(value);
  } else if constexpr (is_goal_ptr<R>::value) {
    return u64(value.offset);
  } else if constexpr (std::is_pointer_v<R>) {
    return u64(uintptr_t(value));
  } else if constexpr (std::is_signed_v<R> && sizeof(R) == 8) {
    return u64(value);
  } else if constexpr (std::is_integral_v<R> || std::is_enum_v<R>) {
    // a 32-bit or smaller result is zero extended in rax, like mov eax
    using U = std::make_unsigned_t<std::conditional_t<std::is_enum_v<R>, int, R>>;
    return u64(U(value));
  } else {
    static_assert(std::is_integral_v<R>, "GOAL results are integers or floats");
  }
}

template <typename R, typename... Args>
u64 call_with_regs(R (*f)(Args...), const u64* regs) {
  static_assert(sizeof...(Args) <= 8, "GOAL functions take at most 8 arguments");
  return [&]<size_t... I>(std::index_sequence<I...>) -> u64 {
    if constexpr (std::is_void_v<R>) {
      f(from_reg<Args>(regs[I])...);
      return 0;
    } else {
      return to_reg(f(from_reg<Args>(regs[I])...));
    }
  }(std::index_sequence_for<Args...>{});
}

//! GOAL-callable wrapper of a C++ function. With replace_arg3_with_pp, the fourth argument
//! register is the process pointer, like the native make_function_from_c(f, true).
template <auto F, bool replace_arg3_with_pp>
u64 adapter(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  u64 regs[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  if constexpr (replace_arg3_with_pp) {
    regs[3] = get_pp();
  }
  return call_with_regs(F, regs);
}

//! GOAL-callable wrapper of a C++ function taking all eight argument registers as an array
//! (make_stack_arg_function_from_c).
template <auto F>
u64 stack_adapter(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  u64 regs[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  using R = decltype(F(regs));
  if constexpr (std::is_void_v<R>) {
    F(regs);
    return 0;
  } else {
    return to_reg(F(regs));
  }
}

//! Functions with no C++ counterpart: return nothing (0) and return zero.
u64 nothing_func(u64, u64, u64, u64, u64, u64, u64, u64);
u64 zero_func(u64, u64, u64, u64, u64, u64, u64, u64);

//! The table index of a GOAL function stored at a GOAL function address.
GoalFunc goal_function(u32 goal_function_addr);

//! Store a C++ GOAL-callable function into a GOAL function object (the slot after its type tag).
void set_goal_function(u32 goal_function_addr, void* func);

//! call_goal: call a GOAL function on the current GOAL stack. The first call starts that stack at
//! the top of the GOAL stack, like the kernel loop does. pp is st during the call, like the x86
//! stub.
u64 call(u32 goal_function_addr, u64 a0, u64 a1, u64 a2, u32 st);
//! call_goal_on_stack: call a GOAL function with the GOAL stack pointer set to sp (a GOAL address).
u64 call_on_stack(u32 goal_function_addr, u32 sp, u32 st);

//! klink: compile the wasm module of an object file and reserve its functions' table entries.
//! Returns a handle for instantiate_module; *table_base is the first entry.
int compile_module(const u8* wasm, u32 size, u32* table_base);
//! klink, once the object is linked: instantiate its module, which fills its table entries.
//! seg_bases are the GOAL addresses of the main, debug and top-level segments (0 if absent).
void instantiate_module(int handle, u32 st, const u32 seg_bases[3]);

}  // namespace wasm_goal
