/*!
 * @file main_web.cpp
 * Entry point of the jak1 runtime for the browser (M5): the renderer on WebGL2. Runs on the main
 * pthread (PROXY_TO_PTHREAD). The runtime arguments come from the page's URL (wasm/web/shell.html,
 * ?args=-fakeiso+-debug).
 */

#include <string>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/runtime.h"
#include "wasm/kernel_test.h"

int main(int argc, char** argv) {
  lg::initialize();
  // the repo files the runtime reads are preloaded under /jak-project (wasm/web/gk_web.cmake)
  if (!file_util::setup_project_path(fs::path("/jak-project"))) {
    lg::error("could not find the jak-project folder");
    return 1;
  }
  // settings and saves in memory for now (OPFS is decision 11)
  file_util::override_user_config_dir("/user", true);

  GameLaunchOptions game_options;
  game_options.game_version = GameVersion::Jak1;

  std::vector<const char*> args = {"gk"};
  for (int i = 1; i < argc; i++) {
    args.push_back(argv[i]);
  }
  int arg_count = int(args.size());
  if (!wasm_kernel_test::parse_args(&arg_count, args.data())) {
    lg::error("usage: -wasm-test <object.o> <function> <frames>");
    return 1;
  }
  try {
    auto status = exec_runtime(game_options, arg_count, args.data());
    return status == RuntimeExitStatus::EXIT ? 0 : int(status);
  } catch (std::exception& ex) {
    // like game/main.cpp
    lg::error("Unexpected exception occurred - {}", ex.what());
    throw;
  }
}
