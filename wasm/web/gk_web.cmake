# gk_web: the jak1 runtime with the OpenGL renderer on WebGL2 (CLAUDE.MD, M5). Included by
# wasm/CMakeLists.txt. Open build-wasm/wasm/gk_web.html through wasm/web/serve.mjs (it sends the
# COOP/COEP headers pthreads need, and serves the files wasm/web/stage_data.sh staged at /data/).

set(GRAPHICS ${GAME}/graphics)
set(OPENGL ${GRAPHICS}/opengl_renderer)
set(TP ${PROJECT_SOURCE_DIR}/third-party)

add_library(imgui_wasm STATIC
    ${TP}/imgui/imgui.cpp
    ${TP}/imgui/imgui_draw.cpp
    ${TP}/imgui/imgui_tables.cpp
    ${TP}/imgui/imgui_widgets.cpp
    ${TP}/imgui/imgui_impl_sdl3.cpp
    ${TP}/imgui/imgui_impl_opengl3.cpp
    ${TP}/imgui/imgui_stdlib.cpp
    ${TP}/imgui/imgui_style.cpp)
# imgui's GL backend in its WebGL2 / GLES 3 mode
target_compile_definitions(imgui_wasm PRIVATE IMGUI_IMPL_OPENGL_ES3)

# zstd for the .fr3 files, in C (no x86 assembly)
file(GLOB ZSTD_SOURCES CONFIGURE_DEPENDS ${TP}/zstd/lib/common/*.c ${TP}/zstd/lib/compress/*.c
     ${TP}/zstd/lib/decompress/*.c)
add_library(zstd_wasm STATIC ${ZSTD_SOURCES})
target_compile_definitions(zstd_wasm PRIVATE ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0)

# the parts of common only the renderer uses
add_library(common_web STATIC
    ${PROJECT_SOURCE_DIR}/common/custom_data/TFrag3Data.cpp
    ${PROJECT_SOURCE_DIR}/common/texture/texture_slots.cpp
    ${PROJECT_SOURCE_DIR}/common/util/compress.cpp
    ${PROJECT_SOURCE_DIR}/common/util/FrameLimiter.cpp
    # the subtitle editor's REPL client (it never connects in the browser)
    ${PROJECT_SOURCE_DIR}/common/cross_sockets/XSocketClient.cpp
    ${PROJECT_SOURCE_DIR}/common/cross_sockets/XSocket.cpp
    ${PROJECT_SOURCE_DIR}/common/repl/nrepl/ReplClient.cpp)
target_link_libraries(common_web common_wasm zstd_wasm)

add_executable(gk_web
    ${GRAPHICS}/display.cpp
    ${GRAPHICS}/gfx.cpp
    ${GRAPHICS}/jak2_texture_remap.cpp
    ${GRAPHICS}/jak3_texture_remap.cpp
    ${GRAPHICS}/jakx_texture_remap.cpp
    ${GRAPHICS}/screenshot.cpp
    ${OPENGL}/background/background_common.cpp
    ${OPENGL}/background/Hfrag.cpp
    ${OPENGL}/background/Shrub.cpp
    ${OPENGL}/background/TFragment.cpp
    ${OPENGL}/background/Tie3.cpp
    ${OPENGL}/BlitDisplays.cpp
    ${OPENGL}/BucketRenderer.cpp
    ${OPENGL}/CollideMeshRenderer.cpp
    ${OPENGL}/debug_gui.cpp
    ${OPENGL}/DepthCue.cpp
    ${OPENGL}/DirectRenderer.cpp
    ${OPENGL}/DirectRenderer2.cpp
    ${OPENGL}/dma_helpers.cpp
    ${OPENGL}/EyeRenderer.cpp
    ${OPENGL}/foreground/Generic2_Build.cpp
    ${OPENGL}/foreground/Generic2_DMA.cpp
    ${OPENGL}/foreground/Generic2_OpenGL.cpp
    ${OPENGL}/foreground/Generic2.cpp
    ${OPENGL}/foreground/Generic2BucketRenderer.cpp
    ${OPENGL}/foreground/Merc2.cpp
    ${OPENGL}/foreground/Merc2BucketRenderer.cpp
    ${OPENGL}/foreground/Shadow2.cpp
    ${OPENGL}/loader/Loader.cpp
    ${OPENGL}/loader/LoaderStages.cpp
    ${OPENGL}/ocean/CommonOceanRenderer.cpp
    ${OPENGL}/ocean/OceanEnvmap.cpp
    ${OPENGL}/ocean/OceanMid_PS2.cpp
    ${OPENGL}/ocean/OceanMid.cpp
    ${OPENGL}/ocean/OceanMidAndFar.cpp
    ${OPENGL}/ocean/OceanNear_PS2.cpp
    ${OPENGL}/ocean/OceanNear.cpp
    ${OPENGL}/ocean/OceanTexture_PC.cpp
    ${OPENGL}/ocean/OceanTexture.cpp
    ${OPENGL}/opengl_utils.cpp
    ${OPENGL}/OpenGLRenderer.cpp
    ${OPENGL}/Profiler.cpp
    ${OPENGL}/ProgressRenderer.cpp
    ${OPENGL}/Shader.cpp
    ${OPENGL}/Shadow_PS2.cpp
    ${OPENGL}/ShadowRenderer.cpp
    ${OPENGL}/SkyBlendCPU.cpp
    ${OPENGL}/SkyBlendGPU.cpp
    ${OPENGL}/SkyRenderer.cpp
    ${OPENGL}/SlowTimeEffect.cpp
    ${OPENGL}/sprite/GlowRenderer.cpp
    ${OPENGL}/sprite/Sprite3_Distort.cpp
    ${OPENGL}/sprite/Sprite3_Glow.cpp
    ${OPENGL}/sprite/Sprite3.cpp
    ${OPENGL}/TextureAnimator.cpp
    ${OPENGL}/TextureAnimatorDefs.cpp
    ${OPENGL}/TextureUploadHandler.cpp
    ${OPENGL}/VisDataHandler.cpp
    ${OPENGL}/Warp.cpp
    ${GRAPHICS}/pipelines/opengl.cpp
    ${GRAPHICS}/sceGraphicsInterface.cpp
    ${GRAPHICS}/texture/jak1_tpage_dir.cpp
    ${GRAPHICS}/texture/jak2_tpage_dir.cpp
    ${GRAPHICS}/texture/jak3_tpage_dir.cpp
    ${GRAPHICS}/texture/TextureConverter.cpp
    ${GRAPHICS}/texture/TexturePool.cpp
    ${GAME}/tools/filter_menu/filter_menu.cpp
    ${GAME}/tools/subtitle_editor/subtitle_editor_db.cpp
    ${GAME}/tools/subtitle_editor/subtitle_editor_repl_client.cpp
    ${TP}/glad/src/glad.c
    ${TP}/stb_image/stb_image.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../runtime_wasm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio_web.cpp
    ${CMAKE_CURRENT_LIST_DIR}/main_web.cpp
    ${CMAKE_CURRENT_LIST_DIR}/stubs_web.cpp
    ${CMAKE_CURRENT_LIST_DIR}/webgl_compat.cpp)
target_compile_definitions(gk_web PRIVATE WASM_DISPLAY)
target_include_directories(gk_web PRIVATE ${TP}/glad/include ${TP}/imgui)
target_link_libraries(gk_web runtime_core common_web imgui_wasm)
target_link_options(gk_web PRIVATE
    -sENVIRONMENT=web,worker
    # WebGL2 on the canvas, transferred to the main pthread as an OffscreenCanvas
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sFULL_ES3=1
    -sGL_ENABLE_GET_PROC_ADDRESS=1
    -sOFFSCREENCANVAS_SUPPORT=1
    # WasmFS: the game files are fetched lazily from /data/ (main_web.cpp), from any thread
    -sWASMFS=1
    # repo files (no game data) preloaded into the virtual filesystem
    # (SHELL: so CMake doesn't merge the repeated flag)
    "SHELL:--preload-file ${PROJECT_SOURCE_DIR}/game/graphics/opengl_renderer/shaders@/jak-project/game/graphics/opengl_renderer/shaders"
    # gamepad mappings the input manager loads (Gamepad API pads mostly use the standard mapping)
    "SHELL:--preload-file ${PROJECT_SOURCE_DIR}/game/assets/sdl_controller_db.txt@/jak-project/game/assets/sdl_controller_db.txt"
    "SHELL:--pre-js ${CMAKE_CURRENT_LIST_DIR}/../goal_modules.js"
    "SHELL:--pre-js ${CMAKE_CURRENT_LIST_DIR}/web_pre.js"
    "SHELL:--shell-file ${CMAKE_CURRENT_LIST_DIR}/shell.html")
set_target_properties(gk_web PROPERTIES SUFFIX ".html")
# next to gk_web.html: the page's script, and the service worker of the OPFS file cache (its scope
# is the page's folder)
add_custom_command(TARGET gk_web POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/data_sw.js
                           ${CMAKE_CURRENT_LIST_DIR}/page.js $<TARGET_FILE_DIR:gk_web>)
# relink (repack the preloaded files) when a shader changes
file(GLOB WEB_SHADERS CONFIGURE_DEPENDS ${OPENGL}/shaders/*)
set_property(TARGET gk_web APPEND PROPERTY LINK_DEPENDS
             ${CMAKE_CURRENT_LIST_DIR}/../goal_modules.js ${CMAKE_CURRENT_LIST_DIR}/web_pre.js
             ${CMAKE_CURRENT_LIST_DIR}/shell.html ${CMAKE_CURRENT_LIST_DIR}/data_sw.js
             ${CMAKE_CURRENT_LIST_DIR}/page.js ${WEB_SHADERS})
