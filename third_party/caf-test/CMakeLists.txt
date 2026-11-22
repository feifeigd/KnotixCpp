# Vendored copy of CAF's libcaf_test (CAF 1.0.2), built as a static library.
# The vcpkg port of CAF does not ship the test module, so the sources under
# caf/ are extracted from the upstream 1.0.2 release. The two headers that
# CAF's own CMake build would generate (caf/detail/test_export.hpp and
# caf/log/test.hpp) are provided alongside the sources.

file(GLOB_RECURSE CAF_TEST_SOURCES CONFIGURE_DEPENDS "caf/test/*.cpp")
list(FILTER CAF_TEST_SOURCES EXCLUDE REGEX "\\.test\\.cpp$")

add_library(caf_test STATIC ${CAF_TEST_SOURCES})
add_library(CAF::test ALIAS caf_test)

target_include_directories(caf_test PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(CAF CONFIG REQUIRED)
target_link_libraries(caf_test PUBLIC CAF::core)

target_compile_features(caf_test PUBLIC cxx_std_23)
