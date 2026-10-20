/*!
 * @file main_web.cpp
 * Entry point of the jak1 runtime for the browser (M5): the renderer on WebGL2. Runs on the main
 * pthread (PROXY_TO_PTHREAD). The runtime arguments come from the page's URL (wasm/web/shell.html,
 * ?args=-fakeiso+-debug).
 */

#include <fcntl.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/runtime.h"
#include "wasm/kernel_test.h"
#include <emscripten/wasmfs.h>
#include <sys/stat.h>

namespace {
// Fetched in ranges of this size; a file up to twice as big is fetched whole on first access.
constexpr uint32_t kDataChunkSize = 2 << 20;

/*!
 * Mount the game files (out/ of the repo) at /jak-project/out, backed by HTTP fetches of /data/
 * (decision 9, M7). Nothing is downloaded until a file is read, then only the ranges read.
 * data/manifest.txt lists the files, one path relative to out/ per line (wasm/web/stage_data.sh):
 * the fetch backend can't list a directory, so each file is created up front.
 */
bool mount_game_data() {
  const std::string root = "/jak-project/out";
  // WasmFS creates the preloaded directories read-only (0555)
  chmod("/jak-project", 0777);
  backend_t backend = wasmfs_create_fetch_backend("data", kDataChunkSize);
  if (int rc = wasmfs_create_directory(root.c_str(), 0777, backend); rc != 0) {
    lg::error("could not mount {} ({})", root, rc);
    return false;
  }
  const std::string manifest = root + "/manifest.txt";
  close(open(manifest.c_str(), O_RDONLY | O_CREAT, 0444));
  std::ifstream list(manifest);
  int count = 0;
  for (std::string line; std::getline(list, line);) {
    if (line.empty()) {
      continue;
    }
    // parent directories, then the (still empty) file
    for (size_t slash = line.find('/'); slash != std::string::npos;
         slash = line.find('/', slash + 1)) {
      mkdir((root + "/" + line.substr(0, slash)).c_str(), 0777);
    }
    close(open((root + "/" + line).c_str(), O_RDONLY | O_CREAT, 0444));
    count++;
  }
  if (count == 0) {
    lg::error("no game files: data/manifest.txt is missing or empty (wasm/web/stage_data.sh)");
    return false;
  }
  lg::info("{} game files under {}, fetched on first read", count, root);
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  lg::initialize();
  // the repo files the runtime reads: shaders etc. preloaded under /jak-project
  // (wasm/web/gk_web.cmake), the game files fetched from /data/
  if (!mount_game_data()) {
    return 1;
  }
  if (!file_util::setup_project_path(fs::path("/jak-project"))) {
    lg::error("could not find the jak-project folder");
    return 1;
  }
  // settings and saves in memory for now (OPFS is decision 11)
  file_util::override_user_config_dir("/user", true);

  GameLaunchOptions game_options;
  game_options.game_version = GameVersion::Jak1;

  // static: the runtime keeps argv (g_argv), and this function unwinds into the browser's frame
  // loop (wasm/runtime_wasm.cpp) while the EE thread still reads it
  static std::vector<const char*> args = {"gk"};
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
