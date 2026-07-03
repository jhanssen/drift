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
target_include_directories(drift_core PUBLIC src)
target_link_libraries(drift_core PUBLIC dawn::webgpu_dawn PRIVATE glaze::glaze)

# ---- drift: native Linux/Wayland runtime ----
set(WLGEN src/platform/linux/wayland-generated)
add_executable(drift
    src/main.cpp
    src/platform/linux/Gpu.cpp
    src/platform/linux/WaylandApp.cpp
    ${WLGEN}/xdg-shell-protocol.c
    ${WLGEN}/wlr-layer-shell-unstable-v1-protocol.c
    ${WLGEN}/linux-dmabuf-v1-protocol.c
)
target_include_directories(drift PRIVATE
    ${WLGEN}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    3rdparty/stb
)
target_link_libraries(drift PRIVATE
    drift_core
    ${WAYLAND_CLIENT_LIBRARIES}
    ${GBM_LIBRARIES}
    Threads::Threads
    dl
)
