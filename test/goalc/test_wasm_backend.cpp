/*!
 * @file test_wasm_backend.cpp
 * Tests for the experimental WebAssembly backend (built only with OPENGOAL_WASM_BACKEND).
 */

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/util/FileUtil.h"

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

namespace {
class WasmCodegen : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    compiler = std::make_unique<Compiler>(GameVersion::Jak1, InstructionSet::WASM);
  }
  static void TearDownTestSuite() { compiler.reset(); }

  //! Compile to a module, check the wasm magic, and optionally dump it for inspection.
  static std::vector<u8> compile(const std::string& src,
                                 const std::string& name,
                                 std::string* text = nullptr) {
    auto bytes = compiler->compile_wasm_module_from_string(src, name, text);
    EXPECT_GE(bytes.size(), 8u);
    if (bytes.size() >= 4) {
      EXPECT_EQ(memcmp(bytes.data(), "\0asm", 4), 0);
    }
    if (const char* dump_dir = std::getenv("OPENGOAL_WASM_DUMP_DIR")) {
      file_util::write_binary_file(fs::path(dump_dir) / (name + ".wasm"), bytes.data(),
                                   bytes.size());
    }
    return bytes;
  }

  static std::unique_ptr<Compiler> compiler;
};
std::unique_ptr<Compiler> WasmCodegen::compiler;

std::string arithmetic_template(const std::string& name) {
  return file_util::read_text_file(file_util::get_jak_project_dir() / "test" / "goalc" /
                                   "source_templates" / "arithmetic" / (name + ".static.gc"));
}
}  // namespace

// integer-only arithmetic templates with no function definitions
TEST_F(WasmCodegen, IntegerArithmeticTemplates) {
  for (const char* file :
       {"add-int-literals", "add-let", "subtract-let", "multiply-let", "multiply32", "multiply64",
        "divide-1", "divide-2", "mod", "logand", "logior", "logxor", "shiftvs", "shift-fixed"}) {
    SCOPED_TRACE(file);
    EXPECT_NO_THROW(compile(arithmetic_template(file), file));
  }
}

// these need function addresses (defun) or static data, which come with linking
TEST_F(WasmCodegen, TemplatesNeedingLinkingAreRejected) {
  for (const char* file : {"add-int-multiple", "add-int-multiple-2", "add-int-vars", "subtract-1",
                           "subtract-2", "multiply", "divide-signs", "mod-unsigned"}) {
    SCOPED_TRACE(file);
    try {
      compile(arithmetic_template(file), file);
      ADD_FAILURE() << "expected an unsupported IR error";
    } catch (std::runtime_error& e) {
      std::string what = e.what();
      EXPECT_TRUE(what.find("mov-fa") != std::string::npos ||
                  what.find("mov-sva") != std::string::npos)
          << what;
    }
  }
}

TEST_F(WasmCodegen, LoopUsesStructuredControlFlow) {
  std::string text;
  compile("(let ((x 0) (i 0)) (while (< i 10) (+! x i) (+! i 1)) x)", "wasm-loop", &text);
  EXPECT_NE(text.find("(loop"), std::string::npos) << text;
  EXPECT_NE(text.find("i64.lt_s"), std::string::npos) << text;
}

TEST_F(WasmCodegen, CallThroughSymbol) {
  std::string text;
  compile("(define-extern wasm-callee (function int int int)) (wasm-callee 3 4)", "wasm-call",
          &text);
  EXPECT_NE(text.find("(import \"sym\" \"wasm-callee\""), std::string::npos) << text;
  EXPECT_NE(text.find("call_indirect"), std::string::npos) << text;
  EXPECT_NE(text.find("(import \"env\" \"memory\""), std::string::npos) << text;
  EXPECT_NE(text.find("(import \"env\" \"table\""), std::string::npos) << text;
}

TEST_F(WasmCodegen, UnsupportedIrIsAnError) {
  // floats are not in the wasm backend yet
  EXPECT_ANY_THROW(compile("(+ 1.0 2.0)", "wasm-float"));
}
