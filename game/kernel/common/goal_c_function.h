#pragma once

/*!
 * @file goal_c_function.h
 * Wrap a C++ function exported to GOAL (make_function_from_c and friends).
 *
 * Native: the plain function pointer, a machine-code stub in EE memory calls it with the GOAL
 * argument registers.
 * Emscripten: a GOAL function is a wasm function of type (i64 x8) -> i64 in the shared table,
 * so the export is an adapter with that type which converts the registers to the C++ function's
 * parameters and its result back (see wasm/goal_call.h).
 */

#ifdef __EMSCRIPTEN__
#include "wasm/goal_call.h"
#define GOAL_C_FUNC(f) ((void*)&wasm_goal::adapter<f, false>)
// third argument replaced by the process pointer, like make_function_from_c(f, true)
#define GOAL_C_FUNC_PP(f) ((void*)&wasm_goal::adapter<f, true>)
// all eight argument registers passed as one u64* array
#define GOAL_C_STACK_FUNC(f) ((void*)&wasm_goal::stack_adapter<f>)
#else
#define GOAL_C_FUNC(f) ((void*)f)
#define GOAL_C_FUNC_PP(f) ((void*)f)
#define GOAL_C_STACK_FUNC(f) ((void*)f)
#endif
