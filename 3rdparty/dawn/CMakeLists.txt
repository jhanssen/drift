# Prebuilt Dawn bundle (jhanssen/dawn): Vulkan backend, DAWN_USE_WAYLAND=ON.
# Ships libwebgpu_dawn.a (+ wire, unused here) and the Dawn CMake config.
# Sets Dawn_DIR for find_package(Dawn); downloads/extracts on first configure.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

set(DAWN_VERSION "v20260703-linux-wayland-wire-alpha")
set(DAWN_COMMIT "b4988e7895f17ec141360722b58923999cee6132")
set(DAWN_REPO "jhanssen/dawn")
set(DAWN_PLATFORM "linux-x86_64-wayland")
set(DAWN_BUILD_TYPE "Release")

set(DAWN_TARBALL_NAME "Dawn-${DAWN_COMMIT}-${DAWN_PLATFORM}-${DAWN_BUILD_TYPE}.tar.gz")
set(DAWN_URL "https://github.com/${DAWN_REPO}/releases/download/${DAWN_VERSION}/${DAWN_TARBALL_NAME}")
set(DAWN_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Dawn-${DAWN_COMMIT}-${DAWN_PLATFORM}-${DAWN_BUILD_TYPE}")

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

set(Dawn_DIR "${DAWN_EXTRACT_DIR}/lib/cmake/Dawn" CACHE PATH "Dawn CMake config directory" FORCE)
