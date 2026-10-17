/*!
 * @file test_wasm_vs_native.cpp
 * Differential tests (OPENGOAL_WASM_BACKEND only): each template in
 * source_templates/wasm-vs-native runs on the native no-kernel runtime and under Node with the
 * wasm backend, and the outputs must be identical. Native is the reference (see CLAUDE.MD).
 */

#include <memory>
#include <string>
#include <thread>

#include "inja.hpp"

#include "goalc/compiler/Compiler.h"
#include "gtest/gtest.h"
#include "test/goalc/framework/test_runner.h"
#include "test/goalc/framework/wasm_test_runner.h"

namespace {
class WasmVsNativeTests : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    runtime_thread = std::thread(GoalTest::runtime_no_kernel_jak1);
    native = std::make_unique<Compiler>(GameVersion::Jak1, emitter::kNativeInstructionSet);
    wasm = std::make_unique<Compiler>(GameVersion::Jak1, emitter::InstructionSet::WASM);
    wasm_runner = std::make_unique<GoalTest::WasmTestRunner>(wasm.get());
  }
  static void TearDownTestSuite() {
    native->shutdown_target();
    runtime_thread.join();
    wasm_runner.reset();
    wasm.reset();
    native.reset();
  }
  void SetUp() override {
    if (GoalTest::WasmTestRunner::node().empty()) {
      GTEST_SKIP() << "node not found (set OPENGOAL_NODE)";
    }
    GoalTest::createDirIfAbsent(GoalTest::getGeneratedDir(category));
  }

  void check(const std::string& file) {
    auto env = GoalTest::getInjaEnvironment(category);
    env.write(file, {}, file);
    auto native_out =
        native->run_test_from_file("test/goalc/source_generated/" + category + "/" + file);
    std::string expected;
    for (auto& part : native_out) {
      expected += part;
    }
    ASSERT_FALSE(expected.empty()) << "native run printed nothing";
    wasm_runner->run_test(category, file, {expected});
  }

  static std::thread runtime_thread;
  static std::unique_ptr<Compiler> native;
  static std::unique_ptr<Compiler> wasm;
  static std::unique_ptr<GoalTest::WasmTestRunner> wasm_runner;
  const std::string category = "wasm-vs-native";
};
std::thread WasmVsNativeTests::runtime_thread;
std::unique_ptr<Compiler> WasmVsNativeTests::native;
std::unique_ptr<Compiler> WasmVsNativeTests::wasm;
std::unique_ptr<GoalTest::WasmTestRunner> WasmVsNativeTests::wasm_runner;
}  // namespace

TEST_F(WasmVsNativeTests, StackVariables) {
  check("stack-vars.gc");
}

TEST_F(WasmVsNativeTests, Int128Ops) {
  check("i128-ops.gc");
}

TEST_F(WasmVsNativeTests, VectorFloatOps) {
  check("vf-ops.gc");
}
