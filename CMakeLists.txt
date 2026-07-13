cmake_minimum_required(VERSION 3.24)
project(drift LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    enable_language(OBJCXX) # the Cocoa platform layer is Objective-C++
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

# First-party packages under library/ (§20.4): the web bundle ships them
# as a store at /packages, and the test fixture installs them into a
# build-dir store for the goldens.
set(DRIFT_LIBRARY_PACKAGES sway bokeh adjust gradient blur glow displace mask
                           spring)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# webgpu.h implementation, one pinned jhanssen/dawn release for both targets:
# sets Dawn_DIR (native) or DRIFT_EMDAWNWEBGPU_PORT (Emscripten); downloads
# and extracts the matching release asset on first configure.
add_subdirectory(3rdparty/dawn)
# glaze (FetchContent, header-only). Provides glaze::glaze target.
add_subdirectory(3rdparty/glaze)
# libwebp (FetchContent). Provides webpdecoder.
add_subdirectory(3rdparty/libwebp)
# libktx (FetchContent). Provides ktx.
add_subdirectory(3rdparty/libktx)

if(NOT EMSCRIPTEN)
    find_package(Dawn REQUIRED)
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)
    # Wasmtime C API prebuilt (§4.5): the native WASM module engine.
    add_subdirectory(3rdparty/wasmtime)
    if(NOT APPLE)
        pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
        pkg_check_modules(GBM REQUIRED gbm)
        pkg_check_modules(LIBDRM REQUIRED libdrm)
        pkg_check_modules(LIBVA REQUIRED libva libva-drm)
    endif()
    pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)
    # §4.4 native network backend. 7.86 is the compile floor (the ws API
    # and CURLOPT_PREREQFUNCTION exist); the *supported* WebSocket floor
    # is 8.11, where ws ships enabled by default — on a curl built
    # without ws (e.g. the macOS system curl), WS handles degrade to the
    # offline face at runtime and HTTP still works, so configure does not
    # refuse older systems.
    pkg_check_modules(LIBCURL REQUIRED libcurl>=7.86)
endif()

# Dawn is built -fno-rtti; match it to avoid typeinfo mismatches.
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fno-rtti>)

# ---- drift_core: platform-agnostic scene/render code ----
# Rule: core code sees only the portable WebGPU API (webgpu_cpp.h). No
# dawn/native, no Wayland, no GBM, no OS assumptions. This is the code that
# later recompiles unchanged under Emscripten for the browser runtime.
add_library(drift_core STATIC
    src/core/Renderer.cpp
    src/core/Scene.cpp
    src/core/SceneLoader.cpp
    src/core/Nodes.cpp
    src/core/Module.cpp
    src/core/WgslInterface.cpp
    src/core/Mp4Demux.cpp
)
target_include_directories(drift_core PUBLIC src PRIVATE 3rdparty/stb)
if(EMSCRIPTEN)
    # The port supplies <webgpu/webgpu*.h> at compile time and the JS
    # bindings at link time, so both stages need the flag.
    target_compile_options(drift_core PUBLIC "--use-port=${DRIFT_EMDAWNWEBGPU_PORT}")
    target_link_options(drift_core PUBLIC "--use-port=${DRIFT_EMDAWNWEBGPU_PORT}")
else()
    target_link_libraries(drift_core PUBLIC dawn::webgpu_dawn)
    if(APPLE)
        # The Dawn monolith's CMake export does not carry its framework
        # dependencies (Metal backend + IOSurface interop).
        target_link_libraries(drift_core PUBLIC
            "-framework CoreFoundation"
            "-framework Foundation"
            "-framework IOKit"
            "-framework IOSurface"
            "-framework Metal"
            "-framework QuartzCore"
        )
    endif()
endif()
target_link_libraries(drift_core PRIVATE glaze::glaze webpdecoder ktx)

# ---- drift_web: browser runtime (Emscripten) ----
# Ships the example scenes in the bundle's MEMFS. Video decodes through
# WebCodecs (mp4: h264/h265/av1, demuxed by core/Mp4Demux); a codec the
# browser cannot decode plays back as a black layer with a console error.
if(EMSCRIPTEN)
    add_executable(drift_web
        src/platform/web/main.cpp
        src/platform/web/VideoDecoderWebCodecs.cpp
    )
    target_link_libraries(drift_web PRIVATE drift_core)
    set(DRIFT_WEB_SCENES plasma buoy follow trail pulse handoff swarm sparks
                         comets fireworks bokeh sway fit effects spring)
    set(DRIFT_WEB_PRELOADS)
    foreach(scene IN LISTS DRIFT_WEB_SCENES)
        list(APPEND DRIFT_WEB_PRELOADS
            "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/examples/${scene}.sceneproject@/scenes/${scene}")
    endforeach()
    # Preloaded content is baked into drift.data at link time — editing a
    # scene or library file must relink or the bundle serves stale files.
    # The glob re-runs at configure time; adding a NEW file needs a cmake
    # rerun, which adding it to a preload list above forces anyway.
    file(GLOB_RECURSE DRIFT_WEB_CONTENT CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/examples/* ${CMAKE_SOURCE_DIR}/library/*)
    set_target_properties(drift_web PROPERTIES
        OUTPUT_NAME drift
        SUFFIX ".html"
        LINK_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/platform/web/shell.html;${DRIFT_WEB_CONTENT}")
    # First-party packages ship as a store at /packages (§20.2); the web
    # runtime defaults DRIFT_PACKAGE_PATH there.
    foreach(pkg IN LISTS DRIFT_LIBRARY_PACKAGES)
        file(READ ${CMAKE_SOURCE_DIR}/library/${pkg}/manifest.json _manifest)
        string(JSON _pkg_version GET "${_manifest}" version)
        list(APPEND DRIFT_WEB_PRELOADS
            "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/library/${pkg}@/packages/${pkg}/${_pkg_version}")
    endforeach()
    target_link_options(drift_web PRIVATE
        "-sALLOW_MEMORY_GROWTH=1"
        "-sSTACK_SIZE=1048576"
        "-sENVIRONMENT=web"
        # UTF8ToString/stringToUTF8 are used by the module bridge's EM_JS
        # bodies; exporting guarantees they are linked into the glue.
        "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,UTF8ToString,stringToUTF8"
        # malloc/free back the storage-import trampolines' heap staging.
        "-sEXPORTED_FUNCTIONS=_main,_malloc,_free"
        "SHELL:--shell-file ${CMAKE_SOURCE_DIR}/src/platform/web/shell.html"
        ${DRIFT_WEB_PRELOADS}
    )
    # The editor page loads drift.js/.wasm/.data from its own directory.
    # A real dependency (not POST_BUILD): editing the editor alone must
    # propagate without relinking drift_web.
    file(GLOB_RECURSE DRIFT_EDITOR_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/platform/web/editor/*")
    # Both OUTPUTs so a deleted build-dir module tree re-triggers the copy;
    # the rm -rf prunes files that a source rename would otherwise strand.
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/editor.html"
               "${CMAKE_BINARY_DIR}/editor/main.js"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/src/platform/web/editor.html"
            "${CMAKE_BINARY_DIR}/editor.html"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/editor"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/src/platform/web/editor"
            "${CMAKE_BINARY_DIR}/editor"
        DEPENDS "${CMAKE_SOURCE_DIR}/src/platform/web/editor.html"
                ${DRIFT_EDITOR_SOURCES})
    add_custom_target(drift_editor ALL
        DEPENDS "${CMAKE_BINARY_DIR}/editor.html"
                "${CMAKE_BINARY_DIR}/editor/main.js")
    return()
endif()

# ---- drift: native runtime (Linux/Wayland, macOS/Cocoa) ----
add_executable(drift
    src/main.cpp
    src/platform/ControlServer.cpp
    src/platform/ModuleNetCurl.cpp
    src/platform/ModuleWasmtime.cpp
    src/platform/VideoDecoderFFmpeg.cpp
    src/platform/WsCodec.cpp
)
target_include_directories(drift PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
    3rdparty/stb
)
target_link_libraries(drift PRIVATE
    drift_core
    wasmtime::wasmtime
    glaze::glaze
    ${FFMPEG_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    Threads::Threads
)
if(APPLE)
    target_sources(drift PRIVATE
        src/platform/mac/CocoaApp.mm
        src/platform/mac/Gpu.cpp
    )
    set_source_files_properties(src/platform/mac/CocoaApp.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # Metal/QuartzCore/IOSurface ride drift_core's interface with Dawn.
    target_link_libraries(drift PRIVATE
        "-framework Cocoa"
        "-framework CoreVideo"
    )
    # Homebrew ffmpeg (and a keg-only curl, when PKG_CONFIG_PATH selects
    # one) live outside the default search path.
    target_link_directories(drift PRIVATE
        ${FFMPEG_LIBRARY_DIRS} ${LIBCURL_LIBRARY_DIRS})
else()
    set(WLGEN src/platform/linux/wayland-generated)
    target_sources(drift PRIVATE
        src/platform/linux/Gpu.cpp
        src/platform/linux/WaylandApp.cpp
        ${WLGEN}/xdg-shell-protocol.c
        ${WLGEN}/wlr-layer-shell-unstable-v1-protocol.c
        ${WLGEN}/linux-dmabuf-v1-protocol.c
        ${WLGEN}/viewporter-protocol.c
        ${WLGEN}/fractional-scale-v1-protocol.c
        ${WLGEN}/commit-timing-v1-protocol.c
        ${WLGEN}/presentation-time-protocol.c
    )
    target_include_directories(drift PRIVATE
        ${WLGEN}
        ${WAYLAND_CLIENT_INCLUDE_DIRS}
        ${GBM_INCLUDE_DIRS}
        ${LIBDRM_INCLUDE_DIRS}
        ${LIBVA_INCLUDE_DIRS}
    )
    target_link_libraries(drift PRIVATE
        ${WAYLAND_CLIENT_LIBRARIES}
        ${GBM_LIBRARIES}
        ${LIBDRM_LIBRARIES}
        ${LIBVA_LIBRARIES}
        dl
    )
endif()

# ---- tests ----
# Unit tests (doctest) for core logic, plus golden-image tests that render
# example scenes headless (on lavapipe for determinism) and fuzzy-compare
# against checked-in PNGs. Run with: ctest --test-dir build
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(3rdparty/doctest)

    add_executable(imgcmp tools/imgcmp.cpp)
    target_include_directories(imgcmp PRIVATE 3rdparty/stb)

    add_executable(drift_tests
        tests/unit/wgsl_interface_test.cpp
        tests/unit/scene_loader_test.cpp
        tests/unit/value_precision_test.cpp
        tests/unit/sequence_test.cpp
        tests/unit/ws_codec_test.cpp
        tests/unit/mp4_demux_test.cpp
        tests/unit/package_store_test.cpp
        tests/unit/value_nodes_test.cpp
        tests/unit/module_test.cpp
        tests/unit/module_native_test.cpp
        tests/unit/module_net_curl_test.cpp
        src/platform/ModuleNetCurl.cpp
        src/platform/ModuleWasmtime.cpp
        src/platform/WsCodec.cpp
    )
    target_include_directories(drift_tests PRIVATE ${LIBCURL_INCLUDE_DIRS})
    target_link_directories(drift_tests PRIVATE ${LIBCURL_LIBRARY_DIRS})
    target_link_libraries(drift_tests PRIVATE drift_core doctest::doctest
                                              glaze::glaze ktx
                                              wasmtime::wasmtime
                                              ${LIBCURL_LIBRARIES}
                                              Threads::Threads)
    # mp4_demux_test parses committed fixtures instead of embedding bytes.
    target_compile_definitions(drift_tests
        PRIVATE DRIFT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME unit COMMAND drift_tests)

    # Goldens resolve library/ packages (§20) from a store the fixture
    # installs into the build dir — the repo→install→store→loader pipeline
    # is part of what they test.
    foreach(pkg IN LISTS DRIFT_LIBRARY_PACKAGES)
        add_test(NAME pkgstore_${pkg} COMMAND
            node ${CMAKE_SOURCE_DIR}/tools/driftpkg.mjs install ${pkg}
            --repo ${CMAKE_SOURCE_DIR}/library
            --store ${CMAKE_BINARY_DIR}/pkgstore
        )
        set_tests_properties(pkgstore_${pkg} PROPERTIES
            FIXTURES_SETUP pkgstore)
    endforeach()

    function(drift_golden_test name)
        add_test(NAME golden_${name} COMMAND
            ${CMAKE_SOURCE_DIR}/tests/golden.sh
            $<TARGET_FILE:drift> $<TARGET_FILE:imgcmp>
            ${CMAKE_SOURCE_DIR}/examples/${name}.sceneproject
            ${CMAKE_SOURCE_DIR}/tests/golden/${name}
        )
        set_tests_properties(golden_${name} PROPERTIES
            ENVIRONMENT "DRIFT_PACKAGE_PATH=${CMAKE_BINARY_DIR}/pkgstore"
            FIXTURES_REQUIRED pkgstore
        )
    endfunction()
    drift_golden_test(plasma)
    drift_golden_test(buoy)
    drift_golden_test(follow)
    drift_golden_test(trail)
    drift_golden_test(nebula)
    drift_golden_test(pulse)
    drift_golden_test(handoff)
    drift_golden_test(swarm)
    drift_golden_test(sparks)
    drift_golden_test(comets)
    drift_golden_test(fireworks)
    drift_golden_test(embers)
    drift_golden_test(haze)
    drift_golden_test(plume)
    drift_golden_test(cinders)
    drift_golden_test(wisps)
    drift_golden_test(fireflies)
    drift_golden_test(bokeh)
    drift_golden_test(sway)
    drift_golden_test(fit)
    drift_golden_test(effects)
    drift_golden_test(spring)

    # §4.4 browser network backend: the wasm bundle in headless Chrome
    # via CDP against loopback servers (tests/web/net_test.mjs). Exits
    # 77 — reported as skipped, not failed — when Chrome or the web
    # bundle (build-web/, or $DRIFT_WEB_BUNDLE) is absent, so the
    # native-only workflow needs neither.
    add_test(NAME web_net COMMAND
        node ${CMAKE_SOURCE_DIR}/tests/web/net_test.mjs)
    set_tests_properties(web_net PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
