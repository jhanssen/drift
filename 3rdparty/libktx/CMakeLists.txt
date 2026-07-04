# libktx (KTX-Software): KTX2 container + Basis Universal transcoding.
# Provides the ktx target (read+write; tests use write to author fixtures).
include(FetchContent)
set(KTX_FEATURE_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_TESTS OFF)
set(KTX_FEATURE_TOOLS OFF)
set(KTX_FEATURE_DOC OFF)
set(KTX_FEATURE_GL_UPLOAD OFF)
set(KTX_FEATURE_VK_UPLOAD OFF)
FetchContent_Declare(
    ktx
    URL https://github.com/KhronosGroup/KTX-Software/archive/refs/tags/v4.4.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(ktx)

# Under Emscripten KTX-Software unconditionally adds its own JS-binding
# modules (libktx.js etc.); they fail to link with KTX_FEATURE_GL_UPLOAD=OFF
# and we only want the static library. Keep them out of the default build.
foreach(ktx_web_target ktx_js ktx_js_read msc_basis_transcoder_js)
    if(TARGET ${ktx_web_target})
        set_target_properties(${ktx_web_target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()
