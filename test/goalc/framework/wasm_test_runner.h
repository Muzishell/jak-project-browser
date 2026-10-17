#pragma once

/*!
 * @file wasm_test_runner.h
 * Runs compiler test templates through the wasm backend and executes them under Node with
 * test/goalc/wasm/run.mjs (OPENGOAL_WASM_BACKEND only). Mirrors CompilerTestRunner: same
 * templates, same expected output.
 */

#include <optional>
#include <string>
#include <vector>

#include "third-party/json.hpp"

class Compiler;

namespace GoalTest {

class WasmTestRunner {
 public:
  explicit WasmTestRunner(Compiler* compiler) : m_compiler(compiler) {}

  //! Node to run modules with: $OPENGOAL_NODE, or node on the PATH. Empty if neither works.
  static const std::string& node();

  //! Write a static template to source_generated, then run it. Like CompilerTestRunner,
  //! truncate compares only the first characters of the output.
  void run_static_test(const std::string& category,
                       const std::string& file,
                       const std::vector<std::string>& expected,
                       std::optional<int> truncate = std::nullopt);
  //! Render a template with data to out_file in source_generated, then run it.
  void run_template_test(const std::string& category,
                         const std::string& template_file,
                         const nlohmann::json& data,
                         const std::string& out_file,
                         const std::vector<std::string>& expected);
  //! Run a file that is already in source_generated/<category>.
  void run_test(const std::string& category,
                const std::string& file,
                const std::vector<std::string>& expected,
                std::optional<int> truncate = std::nullopt);

 private:
  Compiler* m_compiler = nullptr;
  // files that ran in this suite so far, replayed first: native tests of a suite share one
  // runtime, and later files can use what earlier ones defined
  std::vector<std::pair<std::string, std::string>> m_history;
};

}  // namespace GoalTest
