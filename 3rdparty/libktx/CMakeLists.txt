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
