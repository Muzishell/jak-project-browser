# Emscripten builds of the jak1 runtime. Configure with:
#   emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
# - gk_headless (M3): no graphics, runs under Node with the real filesystem (NODERAWFS).
#   wasm/stubs_headless.cpp stands in for the renderer.
# - gk_web (M5): the OpenGL renderer on WebGL2, for the browser (wasm/web/).
# Both: the C kernel, overlord, IOP, mips2c, the sound library without an audio backend and input
# through Emscripten's SDL3 port; no listener or other games. wasm/stubs.cpp has the few entry
# points of the left-out parts the kernel still calls.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# one static wasm module: the root CMakeLists turns shared libraries on before it gets here
set(BUILD_SHARED_LIBS OFF)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(SYSTEM ${PROJECT_SOURCE_DIR}/third-party/fmt/include)
# SDL3 from Emscripten's port (its headers, not third-party/SDL's, so they match the library)
# cubeb_export.h, which cubeb's own build would generate
include_directories(SYSTEM ${CMAKE_CURRENT_LIST_DIR}/include)

# fmt uses malloc/free without including <cstdlib>, which Emscripten's libc++ doesn't pull in
# -msse4.2: Emscripten's SSE intrinsics on wasm SIMD, for the mips2c / VU code
set(WASM_COMPILE_FLAGS -pthread -fwasm-exceptions -msimd128 -msse4.2 -O2 -include stdlib.h --use-port=sdl3)
set(WASM_LINK_FLAGS
    -pthread
    --use-port=sdl3
    -fwasm-exceptions
    -sPROXY_TO_PTHREAD=1
    -sPTHREAD_POOL_SIZE=24
    -sDEFAULT_PTHREAD_STACK_SIZE=8MB
    -sSTACK_SIZE=8MB
    -sINITIAL_MEMORY=1GB
    -sMAXIMUM_MEMORY=4GB
    -sALLOW_MEMORY_GROWTH=1
    -sALLOW_TABLE_GROWTH=1
    -sEXIT_RUNTIME=1
    # function names in stack traces
    --profiling-funcs)

add_compile_options(${WASM_COMPILE_FLAGS} -Wno-everything)
add_link_options(${WASM_LINK_FLAGS})

# fmt
add_subdirectory(${PROJECT_SOURCE_DIR}/third-party/fmt ${CMAKE_BINARY_DIR}/third-party/fmt EXCLUDE_FROM_ALL)

# the parts of common the runtime uses
add_library(common_wasm STATIC
    ${PROJECT_SOURCE_DIR}/common/cross_os_debug/xdbg.cpp
    ${PROJECT_SOURCE_DIR}/common/dma/dma.cpp
    ${PROJECT_SOURCE_DIR}/common/dma/dma_copy.cpp
    ${PROJECT_SOURCE_DIR}/common/dma/gs.cpp
    ${PROJECT_SOURCE_DIR}/common/global_profiler/GlobalProfiler.cpp
    ${PROJECT_SOURCE_DIR}/common/log/log.cpp
    ${PROJECT_SOURCE_DIR}/common/util/Assert.cpp
    ${PROJECT_SOURCE_DIR}/common/util/BitUtils.cpp
    ${PROJECT_SOURCE_DIR}/common/util/crc32.cpp
    ${PROJECT_SOURCE_DIR}/common/util/dgo_util.cpp
    ${PROJECT_SOURCE_DIR}/common/util/font/dbs/font_db_jak1.cpp
    ${PROJECT_SOURCE_DIR}/common/util/font/dbs/font_db_jak2.cpp
    ${PROJECT_SOURCE_DIR}/common/util/font/dbs/font_db_jak3.cpp
    ${PROJECT_SOURCE_DIR}/common/util/font/font_utils_korean.cpp
    ${PROJECT_SOURCE_DIR}/common/util/font/font_utils.cpp
    ${PROJECT_SOURCE_DIR}/common/util/FileUtil.cpp
    ${PROJECT_SOURCE_DIR}/common/util/json_util.cpp
    ${PROJECT_SOURCE_DIR}/common/util/os.cpp
    ${PROJECT_SOURCE_DIR}/common/util/print_float.cpp
    ${PROJECT_SOURCE_DIR}/common/util/string_util.cpp
    ${PROJECT_SOURCE_DIR}/common/util/term_util.cpp
    ${PROJECT_SOURCE_DIR}/common/util/Timer.cpp
    ${PROJECT_SOURCE_DIR}/common/util/unicode_util.cpp
    ${PROJECT_SOURCE_DIR}/common/versions/versions.cpp)
target_link_libraries(common_wasm fmt)

set(GAME ${PROJECT_SOURCE_DIR}/game)
# the mips2c code GOAL objects link to (decision 3), as plain C++
file(GLOB MIPS2C_JAK1_SOURCES CONFIGURE_DEPENDS ${GAME}/mips2c/jak1_functions/*.cpp)

# everything but the renderer and the entry point
add_library(runtime_core STATIC
    ${GAME}/kernel/common/fileio.cpp
    ${GAME}/kernel/common/kboot.cpp
    ${GAME}/kernel/common/kdgo.cpp
    ${GAME}/kernel/common/kdsnetm.cpp
    ${GAME}/kernel/common/klink.cpp
    ${GAME}/kernel/common/klisten.cpp
    ${GAME}/kernel/common/kmachine.cpp
    ${GAME}/kernel/common/kmalloc.cpp
    ${GAME}/kernel/common/kmemcard.cpp
    ${GAME}/kernel/common/kprint.cpp
    ${GAME}/kernel/common/kscheme.cpp
    ${GAME}/kernel/common/ksocket.cpp
    ${GAME}/kernel/common/ksound.cpp
    ${GAME}/kernel/jak1/fileio.cpp
    ${GAME}/kernel/jak1/kboot.cpp
    ${GAME}/kernel/jak1/kdgo.cpp
    ${GAME}/kernel/jak1/klink.cpp
    ${GAME}/kernel/jak1/klisten.cpp
    ${GAME}/kernel/jak1/kmachine.cpp
    ${GAME}/kernel/jak1/kprint.cpp
    ${GAME}/kernel/jak1/kscheme.cpp
    ${GAME}/kernel/jak1/ksound.cpp
    ${GAME}/overlord/common/dma.cpp
    ${GAME}/overlord/common/fake_iso.cpp
    ${GAME}/overlord/common/iso_api.cpp
    ${GAME}/overlord/common/iso.cpp
    ${GAME}/overlord/common/isocommon.cpp
    ${GAME}/overlord/common/overlord.cpp
    ${GAME}/overlord/common/sbank.cpp
    ${GAME}/overlord/common/soundcommon.cpp
    ${GAME}/overlord/common/srpc.cpp
    ${GAME}/overlord/common/ssound.cpp
    ${GAME}/overlord/jak1/dma.cpp
    ${GAME}/overlord/jak1/fake_iso.cpp
    ${GAME}/overlord/jak1/iso_api.cpp
    ${GAME}/overlord/jak1/iso_queue.cpp
    ${GAME}/overlord/jak1/iso.cpp
    ${GAME}/overlord/jak1/isocommon.cpp
    ${GAME}/overlord/jak1/overlord.cpp
    ${GAME}/overlord/jak1/ramdisk.cpp
    ${GAME}/overlord/jak1/srpc.cpp
    ${GAME}/overlord/jak1/ssound.cpp
    ${GAME}/overlord/jak1/stream.cpp
    ${GAME}/sce/deci2.cpp
    ${GAME}/sce/iop.cpp
    ${GAME}/sce/libcdvd_ee.cpp
    ${GAME}/sce/libdma.cpp
    ${GAME}/sce/libgraph.cpp
    ${GAME}/sce/libpad.cpp
    ${GAME}/sce/libscf.cpp
    ${GAME}/sce/sif_ee_memcard.cpp
    ${GAME}/sce/sif_ee.cpp
    ${GAME}/sce/stubs.cpp
    ${GAME}/external/discord.cpp
    ${GAME}/external/discord_jak1.cpp
    ${GAME}/settings/settings.cpp
    ${GAME}/system/hid/devices/dualsense_effects.cpp
    ${GAME}/system/hid/devices/game_controller.cpp
    ${GAME}/system/hid/devices/keyboard.cpp
    ${GAME}/system/hid/devices/mouse.cpp
    ${GAME}/system/hid/display_manager.cpp
    ${GAME}/system/hid/input_bindings.cpp
    ${GAME}/system/hid/input_manager.cpp
    ${GAME}/system/hid/sdl_util.cpp
    ${GAME}/system/background_worker.cpp
    ${GAME}/system/IOP_Kernel.cpp
    ${GAME}/system/iop_thread.cpp
    ${GAME}/system/SystemThread.cpp
    ${GAME}/sound/989snd/fakeplayer.cpp
    ${GAME}/sound/989snd/player.cpp
    ${GAME}/sound/989snd/midi_handler.cpp
    ${GAME}/sound/989snd/ame_handler.cpp
    ${GAME}/sound/989snd/blocksound_handler.cpp
    ${GAME}/sound/989snd/musicbank.cpp
    ${GAME}/sound/989snd/sfxblock.cpp
    ${GAME}/sound/989snd/sfxgrain.cpp
    ${GAME}/sound/989snd/plugin.cpp
    ${GAME}/sound/989snd/loader.cpp
    ${GAME}/sound/989snd/vagvoice.cpp
    ${GAME}/sound/989snd/lfo.cpp
    ${GAME}/sound/989snd/util.cpp
    ${GAME}/sound/common/synth.cpp
    ${GAME}/sound/common/voice.cpp
    ${GAME}/sound/common/envelope.cpp
    ${GAME}/sound/sndshim.cpp
    ${GAME}/sound/sdshim.cpp
    ${CMAKE_CURRENT_LIST_DIR}/goal_call.cpp
    ${CMAKE_CURRENT_LIST_DIR}/goal_thread.cpp
    ${CMAKE_CURRENT_LIST_DIR}/kernel_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/libco_pthread.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mips2c_call.cpp
    ${CMAKE_CURRENT_LIST_DIR}/stubs.cpp
    ${GAME}/mips2c/mips2c_table.cpp
    ${MIPS2C_JAK1_SOURCES})
target_include_directories(runtime_core PRIVATE ${PROJECT_SOURCE_DIR}/third-party/libco)
target_link_libraries(runtime_core common_wasm)

# gk_headless: Node, no renderer
add_executable(gk_headless
    ${CMAKE_CURRENT_LIST_DIR}/runtime_wasm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/main_headless.cpp
    ${CMAKE_CURRENT_LIST_DIR}/stubs_headless.cpp)
target_link_libraries(gk_headless runtime_core)
target_link_options(gk_headless PRIVATE -sNODERAWFS=1 -sENVIRONMENT=node
                    --pre-js ${CMAKE_CURRENT_LIST_DIR}/goal_modules.js)
set_property(TARGET gk_headless APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/goal_modules.js)

include(${CMAKE_CURRENT_LIST_DIR}/web/gk_web.cmake)
