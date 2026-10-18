/*!
 * @file stubs_web.cpp
 * What gk_web's renderer refers to from parts the jak1-only runtime doesn't build.
 */

#include "common/log/log.h"
#include "common/util/dialogs.h"

#include <cstring>

#include "game/overlord/jak3/dma.h"
#include "game/tools/subtitle_editor/subtitle_editor.h"

// common/util/dialogs.cpp uses native file dialogs: in the browser the error goes to the log
namespace dialogs {
void create_error_message_dialog(const std::string& title, const std::string& message) {
  lg::error("{}: {}", title, message);
}
}  // namespace dialogs

// overlord/jak3/dma.cpp: the debug GUI shows jak3's stream buffers; nothing fills them in jak1
namespace jak3 {
OverlordStreamMemory::OverlordStreamMemory() {
  for (auto& x : infos) {
    for (auto& y : x) {
      y.idx = 0;
      strcpy(y.name.chars, "Uninitialized");
    }
  }
}
OverlordStreamMemory g_overlord_stream_memory;
}  // namespace jak3

// tools/subtitle_editor/subtitle_editor.cpp needs the compiler's subtitle project code; the
// editor is a desktop development tool, so the browser has an empty window
SubtitleEditor::SubtitleEditor() {}
void SubtitleEditor::draw_window() {}
