/*!
 * @file test_wasm_variables.cpp
 * test_variables.cpp, compiled with the wasm backend and run under Node
 * (OPENGOAL_WASM_BACKEND only). Same templates and expected output as the native tests.
 */

#include <memory>
#include <string>

#include "goalc/compiler/Compiler.h"
#include "gtest/gtest.h"
#include "test/goalc/framework/test_runner.h"
#include "test/goalc/framework/wasm_test_runner.h"

namespace {
class WasmVariableTests : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    compiler = std::make_unique<Compiler>(GameVersion::Jak1, emitter::InstructionSet::WASM);
    runner = std::make_unique<GoalTest::WasmTestRunner>(compiler.get());
  }
  static void TearDownTestSuite() {
    runner.reset();
    compiler.reset();
  }
  void SetUp() override {
    if (GoalTest::WasmTestRunner::node().empty()) {
      GTEST_SKIP() << "node not found (set OPENGOAL_NODE)";
    }
    GoalTest::createDirIfAbsent(GoalTest::getGeneratedDir(category));
  }
  void run(const std::string& file,
           const std::vector<std::string>& expected,
           std::optional<int> truncate = std::nullopt) {
    runner->run_static_test(category, file, expected, truncate);
  }

  static std::unique_ptr<Compiler> compiler;
  static std::unique_ptr<GoalTest::WasmTestRunner> runner;
  const std::string category = "variables";
};
std::unique_ptr<Compiler> WasmVariableTests::compiler;
std::unique_ptr<GoalTest::WasmTestRunner> WasmVariableTests::runner;
}  // namespace

TEST_F(WasmVariableTests, Globals) {
  run("defglobalconstant-1.static.gc", {"17\n"});
  run("defglobalconstant-2.static.gc", {"18\n"});
}

TEST_F(WasmVariableTests, Definitions) {
  run("define.static.gc", {"17\n"});
}

TEST_F(WasmVariableTests, Let) {
  run("let.static.gc", {"30\n"});
  run("let-star.static.gc", {"30\n"});
  run("mlet.static.gc", {"10\n"});
}

TEST_F(WasmVariableTests, StackVars) {
  run("stack-ints.gc", {"12\n"});
  run("stack-ints-2.gc", {"1\n"});
}

TEST_F(WasmVariableTests, StackArrayAlignment) {
  run("stack-array-align.gc", {"3\n"});
}

// Not ported, they need the real runtime (M3):
//  - Bitfields, StaticBitfieldField, StackStructureAlignment, StackBoxedArray: deftype at top
//    level creates the type at runtime through the C kernel's type system (type new method,
//    method-set!, object-new), which the Node test runtime does not implement.
//  - DeReference: prints (-> type parent parent), the kernel's built-in type hierarchy.
//  - InlineAsm: an x86/arm64 asm function by design; asm functions are not supported in wasm.

TEST_F(WasmVariableTests, GetSymbol) {
  run("get-symbol-1.static.gc", {"1375524\n"});  // 0x14fd24 in hex
  run("get-symbol-2.static.gc", {"1375532\n"});  // 0x14fd2c in hex
}

TEST_F(WasmVariableTests, Constants) {
  std::string expected = "\"test string!\"";
  run("string-constant-2.static.gc", {expected}, expected.size());
}

TEST_F(WasmVariableTests, Symbols) {
  run("quote-symbol.static.gc", {"banana\n0\n"});
  std::string expected = "test-string";
  run("string-symbol.static.gc", {expected}, expected.size());
}

TEST_F(WasmVariableTests, Formatting) {
  run("format-reg-order.static.gc", {"test 1 2 3 4 5 6\n0\n"});
}

TEST_F(WasmVariableTests, Pointers) {
  run("pointers.static.gc", {"13\n"});
}
