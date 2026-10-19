#include "mips2c_call.h"

#include <array>
#include <utility>

#include "common/util/Assert.h"

#include "game/kernel/common/kscheme.h"
#include "game/mips2c/mips2c_private.h"
#include "wasm/goal_call.h"

namespace wasm_goal {
namespace {

// jak1 registers about 90 mips2c functions
constexpr int kMaxMips2cFunctions = 256;

struct Mips2cFunction {
  u64 (*exec)(void*) = nullptr;
  u32 stack_size = 0;
};
std::array<Mips2cFunction, kMaxMips2cFunctions> functions;
int function_count = 0;

// The contexts, one per nesting depth (a mips2c function calling GOAL that calls mips2c).
// _mips2c_call_systemv builds the context on the x86 stack without clearing it, so consecutive
// calls from the same place see the registers the previous call left there, and some code
// relies on it: jak1 init-sky-regs / set-tex-offset copy all their vf registers to sky_regs_vfs,
// including vf27 they never set, which keeps the sky giftag set by the call before.
// Reusing one uncleared context per depth keeps that behaviour. Only the EE thread runs mips2c.
constexpr int kMaxDepth = 16;
std::array<Mips2C::ExecutionContext, kMaxDepth> contexts;
int depth = 0;

u64 run(const Mips2cFunction& f, const u64 (&args)[8]) {
  namespace m = Mips2C;
  ASSERT_MSG(depth < kMaxDepth, "mips2c calls nested too deep");
  m::ExecutionContext& ctx = contexts[depth];
  // also restored when a GOAL throw (a C++ exception) leaves through this frame
  struct DepthGuard {
    DepthGuard() { depth++; }
    ~DepthGuard() { depth--; }
  } guard;
  const m::Gpr arg_regs[8] = {m::a0, m::a1, m::a2, m::a3, m::t0, m::t1, m::t2, m::t3};
  for (int i = 0; i < 8; i++) {
    ctx.gprs[arg_regs[i]].du64[0] = args[i];
  }
  ctx.gprs[m::s6].du64[0] = get_pp();
  ctx.gprs[m::s7].du64[0] = ::s7.offset;
  // the MIPS stack is the GOAL stack: the function uses stack_size bytes below sp, and GOAL code
  // it calls runs below that
  u32 goal_sp = get_sp();
  ctx.gprs[m::sp].du64[0] = goal_sp;
  set_sp(goal_sp - ((f.stack_size + 15) & ~15u));
  f.exec(&ctx);
  set_sp(goal_sp);
  return ctx.gprs[m::v0].du64[0];
}

template <int N>
u64 slot(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  const u64 args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  return run(functions[N], args);
}

template <size_t... N>
constexpr std::array<GoalFunc, sizeof...(N)> make_slots(std::index_sequence<N...>) {
  return {&slot<int(N)>...};
}
const auto slots = make_slots(std::make_index_sequence<kMaxMips2cFunctions>{});

}  // namespace

void* mips2c_function(u64 (*exec)(void*), u32 stack_size) {
  ASSERT_MSG(function_count < kMaxMips2cFunctions, "out of wasm mips2c function slots");
  functions[function_count] = {exec, stack_size};
  return (void*)slots[function_count++];
}

}  // namespace wasm_goal
