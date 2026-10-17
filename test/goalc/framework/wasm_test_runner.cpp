#include "wasm_test_runner.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#include "inja.hpp"

#include "common/symbols.h"
#include "common/util/FileUtil.h"

#include "goalc/compiler/Compiler.h"
#include "gtest/gtest.h"
#include "test/goalc/framework/test_runner.h"

#include "fmt/format.h"

namespace GoalTest {
namespace {

struct CommandResult {
  int status = -1;
  std::string output;
};

CommandResult run_command(const std::string& command) {
  CommandResult result;
  FILE* pipe = popen((command + " 2>&1").c_str(), "r");
  if (!pipe) {
    return result;
  }
  std::array<char, 4096> buffer;
  size_t count;
  while ((count = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
    result.output.append(buffer.data(), count);
  }
  result.status = pclose(pipe);
  return result;
}

std::string join(const std::vector<std::string>& parts) {
  std::string result;
  for (auto& part : parts) {
    result += part;
  }
  return result;
}

}  // namespace

const std::string& WasmTestRunner::node() {
  static const std::string node = []() -> std::string {
    const char* from_env = std::getenv("OPENGOAL_NODE");
    std::string candidate = from_env ? from_env : "node";
    auto result = run_command(fmt::format("\"{}\" --version", candidate));
    return result.status == 0 ? candidate : "";
  }();
  return node;
}

void WasmTestRunner::run_static_test(const std::string& category,
                                     const std::string& file,
                                     const std::vector<std::string>& expected,
                                     std::optional<int> truncate) {
  auto env = getInjaEnvironment(category);
  env.write(file, {}, file);
  run_test(category, file, expected, truncate);
}

void WasmTestRunner::run_template_test(const std::string& category,
                                       const std::string& template_file,
                                       const nlohmann::json& data,
                                       const std::string& out_file,
                                       const std::vector<std::string>& expected) {
  auto env = getInjaEnvironment(category);
  env.write(template_file, data, out_file);
  run_test(category, out_file, expected);
}

void WasmTestRunner::run_test(const std::string& category,
                              const std::string& file,
                              const std::vector<std::string>& expected,
                              std::optional<int> truncate) {
  SCOPED_TRACE(category + "/" + file);
  wasm_backend::WasmObject compiled;
  try {
    compiled = m_compiler->compile_wasm_object_from_file("test/goalc/source_generated/" + category +
                                                         "/" + file);
  } catch (std::exception& e) {
    ADD_FAILURE() << "wasm compile failed: " << e.what();
    return;
  }

  auto dir = fs::temp_directory_path() / "opengoal-wasm-tests" / category;
  fs::create_directories(dir);
  auto obj_path = dir / (file + ".o");
  auto wasm_path = dir / (file + ".wasm");
  file_util::write_binary_file(obj_path, compiled.object.data(), compiled.object.size());
  file_util::write_binary_file(wasm_path, compiled.wasm.data(), compiled.wasm.size());

  auto script = file_util::get_jak_project_dir() / "test" / "goalc" / "wasm" / "run.mjs";
  std::string files;
  for (auto& [obj, wasm] : m_history) {
    files += fmt::format(" \"{}\" \"{}\"", obj, wasm);
  }
  files += fmt::format(" \"{}\" \"{}\"", obj_path.string(), wasm_path.string());
  auto result = run_command(fmt::format(
      "\"{}\" \"{}\" --true-offset {} --empty-offset {}{}", node(), script.string(),
      true_symbol_offset(GameVersion::Jak1), empty_pair_offset_from_s7(GameVersion::Jak1), files));
  EXPECT_EQ(result.status, 0) << result.output;
  auto output = truncate ? result.output.substr(0, *truncate) : result.output;
  EXPECT_EQ(output, join(expected));
  if (result.status == 0) {
    m_history.emplace_back(obj_path.string(), wasm_path.string());
  }
}

}  // namespace GoalTest
