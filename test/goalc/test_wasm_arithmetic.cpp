/*!
 * @file test_wasm_arithmetic.cpp
 * test_arithmetic.cpp, compiled with the wasm backend and run under Node (OPENGOAL_WASM_BACKEND
 * only). Same templates and expected output as the native tests.
 */

#include <memory>
#include <string>

#include "goalc/compiler/Compiler.h"
#include "gtest/gtest.h"
#include "test/goalc/framework/test_runner.h"
#include "test/goalc/framework/wasm_test_runner.h"

namespace {
class WasmArithmeticTests : public testing::Test {
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
  const std::string category = "arithmetic";
};
std::unique_ptr<Compiler> WasmArithmeticTests::compiler;
std::unique_ptr<GoalTest::WasmTestRunner> WasmArithmeticTests::runner;
}  // namespace

// the edge values from genIntegerTests in test_arithmetic.cpp, without the random ones
TEST_F(WasmArithmeticTests, EvalIntegers) {
  int index = 0;
  for (s64 x : {s64(UINT32_MAX), s64(INT32_MIN), s64(INT32_MAX), s64(0), s64(INT64_MAX),
                s64(INT64_MIN), s64(-2147483648), s64(12345678), s64(-987654321)}) {
    for (int y : {-1, 0, 1}) {
      s64 value = s64(u64(x) + u64(s64(y)));  // wraps like the native test
      nlohmann::json data;
      data["integer"] = std::to_string(value);
      runner->run_template_test(category, "eval-integer.template.gc", data,
                                "wasm-eval-integer-" + std::to_string(index++) + ".generated.gc",
                                {std::to_string(value) + "\n"});
    }
  }
}

TEST_F(WasmArithmeticTests, Addition) {
  run("add-int-literals.static.gc", {"13\n"});
  run("add-let.static.gc", {"7\n"});
}

TEST_F(WasmArithmeticTests, AddIntegerFunction) {
  run("add-function.static.gc", {"21\n"});
}

TEST_F(WasmArithmeticTests, AddIntegerMultiple) {
  run("add-int-multiple.static.gc", {"15\n"});
  run("add-int-multiple-2.static.gc", {"15\n"});
}

TEST_F(WasmArithmeticTests, AddIntegerVariables) {
  run("add-int-vars.static.gc", {"7\n"});
}

TEST_F(WasmArithmeticTests, AshFunction) {
  run("ash.static.gc", {"18\n"});
}

TEST_F(WasmArithmeticTests, Division) {
  run("divide-1.static.gc", {"6\n"});
  run("divide-2.static.gc", {"7\n"});
}

TEST_F(WasmArithmeticTests, IntegerSymbol) {
  run("negative-int-symbol.static.gc", {"-123\n"});
}

TEST_F(WasmArithmeticTests, Modulus) {
  run("mod.static.gc", {"7\n"});
}

TEST_F(WasmArithmeticTests, Multiplication) {
  run("multiply.static.gc", {"-12\n"});
  run("multiply-let.static.gc", {"3\n"});
}

TEST_F(WasmArithmeticTests, NestedFunctionCall) {
  run("nested-function.static.gc", {"10\n"});
}

TEST_F(WasmArithmeticTests, VariableShift) {
  run("shiftvs.static.gc", {"11\n"});
}

TEST_F(WasmArithmeticTests, FixedShift) {
  run("shift-fixed.static.gc", {"11\n"});
}

TEST_F(WasmArithmeticTests, Subtraction) {
  run("subtract-1.static.gc", {"4\n"});
  run("subtract-2.static.gc", {"4\n"});
  run("subtract-let.static.gc", {"3\n"});
}

TEST_F(WasmArithmeticTests, Multiplication2) {
  run("multiply32.static.gc", {"-1234478448\n"});
  run("multiply64.static.gc", {"93270638141856400\n"});
}

TEST_F(WasmArithmeticTests, Constants) {
  run("float.static.gc", {"1067316150\n"});
  run("function-return-float-constant.static.gc", {"3.14149\n0\n"});
}

TEST_F(WasmArithmeticTests, Operations) {
  run("float-pow.static.gc", {"256\n0\n"});
  run("float-product.static.gc", {"120.0000\n0\n"});
}

TEST_F(WasmArithmeticTests, Symbols) {
  run("float-in-symbol.static.gc", {"2345.6000\n0\n"});
}

TEST_F(WasmArithmeticTests, Functions) {
  run("float-function.static.gc", {"10.152\n0\n"});
  run("nested-float-functions.static.gc",
      {"i 1.4400 3.4000\nr 10.1523\ni 1.2000 10.1523\nr 17.5432\n17.543 10.152\n0\n"});
}

TEST_F(WasmArithmeticTests, MinMax) {
  run("float-max.static.gc", {"3.70\n0\n"});
  run("float-min.static.gc", {"-1.20\n0\n"});
}

TEST_F(WasmArithmeticTests, LogicalOperators) {
  run("logand.static.gc", {"4\n"});
  run("logior.static.gc", {"60\n"});
  run("logxor.static.gc", {"56\n"});
}

TEST_F(WasmArithmeticTests, Comparison) {
  run("signed-int-compare.static.gc", {"12\n"});
}

TEST_F(WasmArithmeticTests, DivideSigns) {
  run("divide-signs.static.gc",
      {"fffffffffffffffb 7ffffffffffffffb fffffffffffffffd 55555552\n0\n"});
}

TEST_F(WasmArithmeticTests, ModUnsigned) {
  run("mod-unsigned.static.gc", {"ffffffffffffffff 5\n0\n"});
}
