#pragma once

/*!
 * @file mips2c_call.h
 * mips2c functions as GOAL functions in the Emscripten runtime (native: the stub that
 * LinkedFunctionTable::reg writes and _mips2c_call_systemv).
 */

#include "common/common_types.h"

namespace wasm_goal {

//! A GOAL-callable (i64 x8) -> i64 function (a table index) that runs a mips2c function: it
//! builds the ExecutionContext from the argument registers, pp and st, gives it stack_size bytes
//! below the GOAL stack pointer and returns v0. There is a fixed number of these.
void* mips2c_function(u64 (*exec)(void*), u32 stack_size);

}  // namespace wasm_goal
