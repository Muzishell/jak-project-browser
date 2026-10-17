/*!
 * @file test_wasm_collections.cpp
 * test_collections.cpp, compiled with the wasm backend and run under Node
 * (OPENGOAL_WASM_BACKEND only). Same templates and expected output as the native tests.
 */

#include <memory>
#include <string>

#include "goalc/compiler/Compiler.h"
#include "gtest/gtest.h"
#include "test/goalc/framework/test_runner.h"
#include "test/goalc/framework/wasm_test_runner.h"

namespace {
class WasmCollectionTests : public testing::Test {
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
  const std::string category = "collections";
};
std::unique_ptr<Compiler> WasmCollectionTests::compiler;
std::unique_ptr<GoalTest::WasmTestRunner> WasmCollectionTests::runner;
}  // namespace

TEST_F(WasmCollectionTests, Pairs) {
  run("empty-pair.static.gc", {"()\n0\n"});
  run("pair-check.static.gc", {"#t#f\n0\n"});
}

TEST_F(WasmCollectionTests, Lists) {
  run("list.static.gc", {"(a b c d)\n0\n"});
}

TEST_F(WasmCollectionTests, InlineArray) {
  run("inline-array-field.static.gc", {"16\n"});
}

TEST_F(WasmCollectionTests, Operations) {
  run("cons.static.gc", {"(a . b)\n0\n"});
  run("car-cdr-get.static.gc", {"ab\n0\n"});
  run("car-cdr-set.static.gc", {"(c . d)\n0\n"});
  run("nested-car-cdr-set.static.gc", {"efgh\n((e . g) f . h)\n0\n"});
}
