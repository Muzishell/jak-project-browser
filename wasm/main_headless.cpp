/*!
 * @file main_headless.cpp
 * Entry point of the headless jak1 runtime for Emscripten (M3). No display, no sound, no listener.
 * Usage (Node): node gk_headless.js [runtime args, e.g. -fakeiso -debug -nosound -nokernel]
 *   [-wasm-test <object.o> <function> <frames>] (wasm/kernel_test.h)
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
  if (!file_util::setup_project_path({})) {
    lg::error("could not find the jak-project folder");
    return 1;
  }

  GameLaunchOptions game_options;
  game_options.disable_display = true;
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
  auto status = exec_runtime(game_options, arg_count, args.data());
  return status == RuntimeExitStatus::EXIT ? 0 : int(status);
}
