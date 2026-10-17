/*!
 * @file test_wasm_control_statements.cpp
 * test_control_statements.cpp, compiled with the wasm backend and run under Node
 * (OPENGOAL_WASM_BACKEND only). Same templates and expected output as the native tests. The
 * IR-inspection checks of InlineIsInline / AllowInline are front-end tests and stay native-only;
 * their files still run here.
 */

#include <memory>
#include <string>

#include "goalc/compiler/Compiler.h"
#include "gtest/gtest.h"
#include "test/goalc/framework/test_runner.h"
#include "test/goalc/framework/wasm_test_runner.h"

namespace {
class WasmControlStatementTests : public testing::Test {
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
  void run(const std::string& file, const std::vector<std::string>& expected) {
    runner->run_static_test(category, file, expected);
  }

  static std::unique_ptr<Compiler> compiler;
  static std::unique_ptr<GoalTest::WasmTestRunner> runner;
  const std::string category = "control-statements";
};
std::unique_ptr<Compiler> WasmControlStatementTests::compiler;
std::unique_ptr<GoalTest::WasmTestRunner> WasmControlStatementTests::runner;
}  // namespace

TEST_F(WasmControlStatementTests, ConditionalCompilation) {
  run("conditional-compilation.static.gc", {"3\n"});
}

TEST_F(WasmControlStatementTests, Blocks) {
  run("nested-blocks-1.static.gc", {"7\n"});
  run("nested-blocks-2.static.gc", {"8\n"});
  run("nested-blocks-3.static.gc", {"7\n"});
}

TEST_F(WasmControlStatementTests, GoTo) {
  run("goto.static.gc", {"3\n"});
}

TEST_F(WasmControlStatementTests, Branch) {
  run("return-value-of-if.static.gc", {"123\n"});
}

TEST_F(WasmControlStatementTests, DoTimes) {
  run("dotimes.static.gc", {"4950\n"});
}

TEST_F(WasmControlStatementTests, Factorial) {
  run("factorial-recursive.static.gc", {"3628800\n"});
  run("factorial-iterative.static.gc", {"3628800\n"});
}

TEST_F(WasmControlStatementTests, Definitions) {
  run("defun-return-constant.static.gc", {"12\n"});
  run("defun-return-symbol.static.gc", {"42\n"});
}

TEST_F(WasmControlStatementTests, ReturnValue) {
  run("return.static.gc", {"77\n"});
  run("return-arg.static.gc", {"23\n"});
  run("return-colors.static.gc", {"77\n"});
}

TEST_F(WasmControlStatementTests, Calling) {
  run("nested-call.static.gc", {"2\n"});
  run("simple-call.static.gc", {"30\n"});
}

TEST_F(WasmControlStatementTests, Anonymous) {
  run("lambda-1.static.gc", {"2\n"});
}

TEST_F(WasmControlStatementTests, InlineIsInline) {
  run("declare-inline.static.gc", {"32\n"});
}

TEST_F(WasmControlStatementTests, AllowInline) {
  run("inline-call.static.gc", {"44\n"});
}

TEST_F(WasmControlStatementTests, ReturnNone) {
  run("function-returning-none.static.gc", {"1\n"});
}

TEST_F(WasmControlStatementTests, InlineBlock1) {
  run("inline-with-block-1.static.gc", {"1\n"});
}

TEST_F(WasmControlStatementTests, InlineBlock2) {
  run("inline-with-block-2.static.gc", {"3\n"});
}

TEST_F(WasmControlStatementTests, InlineBlock3) {
  run("inline-with-block-3.static.gc", {"4\n"});
}

TEST_F(WasmControlStatementTests, InlineBlock4) {
  run("inline-with-block-4.static.gc", {"3.0000\n0\n"});
}

TEST_F(WasmControlStatementTests, ReturnFromTrick) {
  run("return-from-trick.static.gc", {"1\n"});
}

TEST_F(WasmControlStatementTests, Set) {
  run("set-symbol.static.gc", {"22\n"});
}

TEST_F(WasmControlStatementTests, Protect) {
  run("protect.static.gc", {"33\n"});
}

TEST_F(WasmControlStatementTests, Align) {
  run("align16-1.static.gc", {"80\n"});
  // align16-2 reads (-> integer allocated-length) from the C kernel's built-in type objects,
  // which the Node test runtime does not have. It needs the real runtime (M3).
}

TEST_F(WasmControlStatementTests, Defsmacro) {
  run("defsmacro-defgmacro.static.gc", {"20\n"});
}

TEST_F(WasmControlStatementTests, Desfun) {
  run("desfun.static.gc", {"4\n"});
}

TEST_F(WasmControlStatementTests, DeReference) {
  run("methods.static.gc", {"#t#t\n0\n"});
}
