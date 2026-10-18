#pragma once

/*!
 * @file kernel_test.h
 * Headless runtime test mode (-wasm-test): runs GOAL code in the wasm kernel without the listener,
 * the way the native kernel tests (test/goalc/test_goal_kernel.cpp) send it.
 *
 *   -wasm-test <object.o> <function> <frames>
 *
 * links object.o (a wasm object file from goalc --instruction-set wasm, e.g. asm-file :write),
 * runs <function> as the listener function on the first kernel frame, then exits after <frames>
 * kernel frames. Between "==== wasm-test begin" and "==== wasm-test end" lines on stdout, what the
 * listener would get as prints (format #t) is a "==== wasm-test print <JSON string>" line per
 * frame; the rest is printed as usual (test/goalc/wasm/kernel-test.mjs).
 */

namespace wasm_kernel_test {

//! Remove and remember the -wasm-test arguments. Returns false on a malformed option.
bool parse_args(int* argc, const char** argv);
//! Called by the kernel loop before each kernel-dispatcher call.
void before_dispatch();
//! Called by the kernel loop after each kernel-dispatcher call, before ClearPending.
void after_dispatch();

}  // namespace wasm_kernel_test
