# Prebuilt Dawn (jhanssen/dawn), one pinned release for every target:
#  - native Linux: Vulkan/Wayland bundle with libwebgpu_dawn.a and the Dawn
#    CMake config; sets Dawn_DIR for find_package(Dawn).
#  - native macOS: Metal bundle (arm64) from the same commit, same layout.
#  - Emscripten: the emdawnwebgpu port package (text-only) built from the
#    same commit, so webgpu_cpp.h matches the native bundles exactly; sets
#    DRIFT_EMDAWNWEBGPU_PORT for --use-port.
# Downloads/extracts on first configure. The Linux/wasm assets are produced
# by scripts/build-wire-release.sh in the Dawn fork; the macOS asset by the
# same recipe with the Metal backend in place of Vulkan/Wayland.
if(NOT EMSCRIPTEN)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(DAWN_PLATFORM "linux-x86_64-wayland")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(DAWN_PLATFORM "macos-arm64")
    else()
        message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
    endif()
endif()

set(DAWN_VERSION "v20260703-linux-wayland-wire-alpha")
set(DAWN_COMMIT "b4988e7895f17ec141360722b58923999cee6132")
set(DAWN_REPO "jhanssen/dawn")
set(DAWN_BUILD_TYPE "Release")

if(EMSCRIPTEN)
    set(DAWN_ARCHIVE_STEM "emdawnwebgpu_pkg-${DAWN_COMMIT}")
else()
    set(DAWN_ARCHIVE_STEM "Dawn-${DAWN_COMMIT}-${DAWN_PLATFORM}-${DAWN_BUILD_TYPE}")
endif()

set(DAWN_TARBALL_NAME "${DAWN_ARCHIVE_STEM}.tar.gz")
set(DAWN_URL "https://github.com/${DAWN_REPO}/releases/download/${DAWN_VERSION}/${DAWN_TARBALL_NAME}")
set(DAWN_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/${DAWN_ARCHIVE_STEM}")

if(NOT EXISTS "${DAWN_EXTRACT_DIR}")
    set(DAWN_TARBALL_PATH "${CMAKE_CURRENT_BINARY_DIR}/${DAWN_TARBALL_NAME}")

    if(NOT EXISTS "${DAWN_TARBALL_PATH}")
        message(STATUS "Downloading Dawn from ${DAWN_URL}")
        file(DOWNLOAD "${DAWN_URL}" "${DAWN_TARBALL_PATH}"
            STATUS DAWN_DOWNLOAD_STATUS
            SHOW_PROGRESS
        )
        list(GET DAWN_DOWNLOAD_STATUS 0 DAWN_DOWNLOAD_ERROR)
        if(NOT DAWN_DOWNLOAD_ERROR EQUAL 0)
            list(GET DAWN_DOWNLOAD_STATUS 1 DAWN_DOWNLOAD_ERROR_MSG)
            file(REMOVE "${DAWN_TARBALL_PATH}")
            message(FATAL_ERROR "Failed to download Dawn: ${DAWN_DOWNLOAD_ERROR_MSG}")
        endif()
    endif()

    message(STATUS "Extracting Dawn")
    file(ARCHIVE_EXTRACT INPUT "${DAWN_TARBALL_PATH}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif()

if(EMSCRIPTEN)
    set(DRIFT_EMDAWNWEBGPU_PORT "${DAWN_EXTRACT_DIR}/emdawnwebgpu.port.py"
        CACHE FILEPATH "emdawnwebgpu.port.py for --use-port")
    if(NOT EXISTS "${DRIFT_EMDAWNWEBGPU_PORT}")
        message(FATAL_ERROR "emdawnwebgpu port not found at ${DRIFT_EMDAWNWEBGPU_PORT}")
    endif()
else()
    set(Dawn_DIR "${DAWN_EXTRACT_DIR}/lib/cmake/Dawn" CACHE PATH "Dawn CMake config directory" FORCE)
endif()
