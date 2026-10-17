/*!
 * @file test_wasm_backend.cpp
 * Tests for the experimental WebAssembly backend (built only with OPENGOAL_WASM_BACKEND).
 */

#include <cstdlib>
#include <cstring>

#include "binaryen-c.h"
#include "goalc/compiler/Compiler.h"
#include "goalc/emitter/InstructionSet.h"
#include "goalc/emitter/Register.h"
#include "gtest/gtest.h"

using namespace emitter;

TEST(WasmBackend, RegisterLayoutMatchesX86) {
  const auto& wasm = reg_info(InstructionSet::WASM);
  const auto& x86 = reg_info(InstructionSet::X86);
  EXPECT_EQ(&wasm, &x86);

  EXPECT_TRUE(Register(RAX).is_gpr(InstructionSet::WASM));
  EXPECT_FALSE(Register(RAX).is_xmm(InstructionSet::WASM));
  EXPECT_TRUE(Register(XMM3).is_xmm(InstructionSet::WASM));
  EXPECT_TRUE(Register(XMM3).is_128bit_simd(InstructionSet::WASM));
  EXPECT_EQ(Register(XMM3).hw_id(InstructionSet::WASM), 3);
  EXPECT_EQ(Register(R12).hw_id(InstructionSet::WASM), 12);
  EXPECT_EQ(Register(RDI).print(InstructionSet::WASM), "rdi");
}

TEST(WasmBackend, BinaryenModuleValidates) {
  BinaryenModuleRef module = BinaryenModuleCreate();
  BinaryenType i64_pair[2] = {BinaryenTypeInt64(), BinaryenTypeInt64()};
  BinaryenType params = BinaryenTypeCreate(i64_pair, 2);
  BinaryenExpressionRef body =
      BinaryenBinary(module, BinaryenAddInt64(), BinaryenLocalGet(module, 0, BinaryenTypeInt64()),
                     BinaryenLocalGet(module, 1, BinaryenTypeInt64()));
  BinaryenAddFunction(module, "add", params, BinaryenTypeInt64(), nullptr, 0, body);
  BinaryenAddFunctionExport(module, "add", "add");
  EXPECT_TRUE(BinaryenModuleValidate(module));

  auto result = BinaryenModuleAllocateAndWrite(module, nullptr);
  ASSERT_GE(result.binaryBytes, 8u);
  EXPECT_EQ(memcmp(result.binary, "\0asm", 4), 0);
  free(result.binary);
  BinaryenModuleDispose(module);
}

TEST(WasmBackend, CompilerFrontEndForWasm) {
  Compiler compiler(GameVersion::Jak1, InstructionSet::WASM);

  goos::Object instr_set;
  ASSERT_TRUE(compiler.get_goos().get_global_variable_by_name("INSTRUCTION_SET", &instr_set));
  ASSERT_TRUE(instr_set.is_symbol());
  EXPECT_STREQ(instr_set.as_symbol().name_ptr, "wasm");

  // division, mod and variable shifts take x86-only register constraints on native
  EXPECT_NO_THROW(compiler.run_front_end_on_string(
      "(defun wasm-front-end-test ((a int) (b int)) (+ (/ a b) (mod a b) (shl a b) (sar a b)))"));
}
