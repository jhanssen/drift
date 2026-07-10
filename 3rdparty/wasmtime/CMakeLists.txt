# Prebuilt Wasmtime C API (DESIGN.md §4.5): the native WASM module engine,
# pinned by version AND content hash — the second and last prebuilt
# tarball after Dawn. Native only; the browser build instantiates modules
# through the platform's own WebAssembly engine and never sees this.
#
# The release also ships a `min/` variant (~2 MB) — it has no compiler
# (load-precompiled-only), so it cannot run community-authored modules;
# we take the full library. Exposes the wasmtime::wasmtime static target.
if(EMSCRIPTEN)
    return()
endif()

# v46+ release artifacts include the Pulley feature (the §4.5 hardened
# interpreter default); the v36 LTS line does not ship it.
set(WASMTIME_VERSION "v46.0.1")
set(WASMTIME_PLATFORM "x86_64-linux")
set(WASMTIME_SHA256
    "4b7e7acf08467de6147f11c8fb71c4db7623035064c096976aa969d54171fed4")

set(WASMTIME_ARCHIVE_STEM
    "wasmtime-${WASMTIME_VERSION}-${WASMTIME_PLATFORM}-c-api")
set(WASMTIME_TARBALL_NAME "${WASMTIME_ARCHIVE_STEM}.tar.xz")
set(WASMTIME_URL
    "https://github.com/bytecodealliance/wasmtime/releases/download/${WASMTIME_VERSION}/${WASMTIME_TARBALL_NAME}")
set(WASMTIME_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/${WASMTIME_ARCHIVE_STEM}")

if(NOT EXISTS "${WASMTIME_EXTRACT_DIR}")
    set(WASMTIME_TARBALL_PATH "${CMAKE_CURRENT_BINARY_DIR}/${WASMTIME_TARBALL_NAME}")

    if(NOT EXISTS "${WASMTIME_TARBALL_PATH}")
        message(STATUS "Downloading Wasmtime from ${WASMTIME_URL}")
        file(DOWNLOAD "${WASMTIME_URL}" "${WASMTIME_TARBALL_PATH}"
            EXPECTED_HASH SHA256=${WASMTIME_SHA256}
            STATUS WASMTIME_DOWNLOAD_STATUS
            SHOW_PROGRESS
        )
        list(GET WASMTIME_DOWNLOAD_STATUS 0 WASMTIME_DOWNLOAD_ERROR)
        if(NOT WASMTIME_DOWNLOAD_ERROR EQUAL 0)
            list(GET WASMTIME_DOWNLOAD_STATUS 1 WASMTIME_DOWNLOAD_ERROR_MSG)
            file(REMOVE "${WASMTIME_TARBALL_PATH}")
            message(FATAL_ERROR "Failed to download Wasmtime: ${WASMTIME_DOWNLOAD_ERROR_MSG}")
        endif()
    endif()

    message(STATUS "Extracting Wasmtime")
    file(ARCHIVE_EXTRACT INPUT "${WASMTIME_TARBALL_PATH}"
         DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif()

add_library(wasmtime_prebuilt STATIC IMPORTED GLOBAL)
set_target_properties(wasmtime_prebuilt PROPERTIES
    IMPORTED_LOCATION "${WASMTIME_EXTRACT_DIR}/lib/libwasmtime.a"
    INTERFACE_INCLUDE_DIRECTORIES "${WASMTIME_EXTRACT_DIR}/include")
# Wasmtime's static library needs the usual Rust runtime underpinnings.
target_link_libraries(wasmtime_prebuilt INTERFACE
    Threads::Threads dl m rt)
add_library(wasmtime::wasmtime ALIAS wasmtime_prebuilt)
