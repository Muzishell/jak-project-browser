#include "kernel_test.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "common/listener_common.h"
#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"

#include "game/kernel/common/klink.h"
#include "game/kernel/common/klisten.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/klink.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kscheme.h"

#include "fmt/format.h"

namespace wasm_kernel_test {
namespace {
bool enabled = false;
std::string object_path;
std::string function_name;
int frame_count = 0;
int frame = 0;

void load_and_start() {
  auto data = file_util::read_binary_file(object_path);
  // like a listener code message: a copy in the debug heap, linked and its top level run
  auto buffer = kmalloc(kdebugheap, s32(data.size()) + 0x40, KMALLOC_ALIGN_64, "wasm-test");
  ASSERT_MSG(buffer.offset, "wasm-test: no memory for the object file");
  memcpy(buffer.c(), data.data(), data.size());
  auto name = file_util::base_name_no_ext(object_path);
  jak1::link_and_exec(buffer, name.c_str(), s32(data.size()), kdebugheap, LINK_FLAG_EXECUTE, true);

  // what the native kernel test does first, then the code it sends
  jak1::intern_from_c("*use-old-listener-print*")->value = s7.offset + jak1_symbols::FIX_SYM_TRUE;
  auto func = jak1::intern_from_c(function_name.c_str())->value;
  ASSERT_MSG(func != s7.offset, "wasm-test: " + function_name + " is not defined");
  jak1::ListenerFunction->value = func;
  // the boot's prints aren't part of the test
  ClearPending();
  lg::print("==== wasm-test begin\n");
}

void print_json_string(const char* text) {
  std::string out = "\"";
  for (const char* c = text; *c; c++) {
    switch (*c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (u8(*c) < 0x20) {
          out += fmt::format("\\u{:04x}", u8(*c));
        } else {
          out += *c;
        }
    }
  }
  lg::print("{}\"\n", out);
}
}  // namespace

bool parse_args(int* argc, const char** argv) {
  for (int i = 1; i < *argc; i++) {
    if (strcmp(argv[i], "-wasm-test") != 0) {
      continue;
    }
    if (i + 3 >= *argc) {
      return false;
    }
    enabled = true;
    object_path = argv[i + 1];
    function_name = argv[i + 2];
    frame_count = atoi(argv[i + 3]);
    for (int j = i + 4; j < *argc; j++) {
      argv[j - 4] = argv[j];
    }
    *argc -= 4;
    return frame_count > 0;
  }
  return true;
}

void before_dispatch() {
  if (!enabled) {
    return;
  }
  if (frame == 0) {
    load_and_start();
  } else if (frame == frame_count) {
    // the output of the last frame was printed by ClearPending after it
    lg::print("==== wasm-test end\n");
    MasterExit = RuntimeExitStatus::EXIT;
  }
  frame++;
}

void after_dispatch() {
  if (!enabled || frame == 0 || PrintPending.offset == 0) {
    return;
  }
  // what the listener would get as print messages (format #t), apart from what goes to stdout
  // directly (format 0): one JSON string per frame
  const char* text = PrintBufArea.cast<char>().c() + sizeof(ListenerMessageHeader);
  if (*text) {
    lg::print("==== wasm-test print ");
    print_json_string(text);
  }
  clear_print();
}

}  // namespace wasm_kernel_test
