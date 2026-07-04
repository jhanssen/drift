# doctest: header-only test framework. Provides doctest::doctest target.
include(FetchContent)
set(DOCTEST_WITH_TESTS OFF)
set(DOCTEST_NO_INSTALL ON)
FetchContent_Declare(
    doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(doctest)
