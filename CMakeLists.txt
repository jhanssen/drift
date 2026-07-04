cmake_minimum_required(VERSION 3.24)
project(drift LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Sets Dawn_DIR (downloads/extracts the prebuilt bundle on first configure).
add_subdirectory(3rdparty/dawn)
# glaze (FetchContent, header-only). Provides glaze::glaze target.
add_subdirectory(3rdparty/glaze)

find_package(Dawn REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)

# Dawn is built -fno-rtti; match it to avoid typeinfo mismatches.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)

# ---- drift_core: platform-agnostic scene/render code ----
# Rule: core code sees only the portable WebGPU API (webgpu_cpp.h). No
# dawn/native, no Wayland, no GBM, no OS assumptions. This is the code that
# later recompiles unchanged under Emscripten for the browser runtime.
add_library(drift_core STATIC
    src/core/Renderer.cpp
    src/core/Scene.cpp
    src/core/SceneLoader.cpp
    src/core/Nodes.cpp
    src/core/WgslInterface.cpp
)
target_include_directories(drift_core PUBLIC src PRIVATE 3rdparty/stb)
target_link_libraries(drift_core PUBLIC dawn::webgpu_dawn PRIVATE glaze::glaze)

# ---- drift: native Linux/Wayland runtime ----
set(WLGEN src/platform/linux/wayland-generated)
add_executable(drift
    src/main.cpp
    src/platform/linux/Gpu.cpp
    src/platform/linux/WaylandApp.cpp
    src/platform/linux/VideoDecoderFFmpeg.cpp
    ${WLGEN}/xdg-shell-protocol.c
    ${WLGEN}/wlr-layer-shell-unstable-v1-protocol.c
    ${WLGEN}/linux-dmabuf-v1-protocol.c
)
target_include_directories(drift PRIVATE
    ${WLGEN}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    3rdparty/stb
)
target_link_libraries(drift PRIVATE
    drift_core
    ${WAYLAND_CLIENT_LIBRARIES}
    ${GBM_LIBRARIES}
    ${LIBDRM_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    Threads::Threads
    dl
)

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
    )
    target_link_libraries(drift_tests PRIVATE drift_core doctest::doctest)
    add_test(NAME unit COMMAND drift_tests)

    function(drift_golden_test name)
        add_test(NAME golden_${name} COMMAND
            ${CMAKE_SOURCE_DIR}/tests/golden.sh
            $<TARGET_FILE:drift> $<TARGET_FILE:imgcmp>
            ${CMAKE_SOURCE_DIR}/examples/${name}.sceneproject
            ${CMAKE_SOURCE_DIR}/tests/golden/${name}
        )
    endfunction()
    drift_golden_test(plasma)
    drift_golden_test(buoy)
    drift_golden_test(follow)
    drift_golden_test(trail)
    drift_golden_test(nebula)
endif()
