// This file replaces the header that CAF's CMake build generates via
// generate_export_header() for libcaf_test. The vendored copy in this
// repository is always built as a static library, so the export macros are
// empty.

#pragma once

#define CAF_TEST_EXPORT
#define LIBCAF_TEST_NO_EXPORT

#define CAF_TEST_DEPRECATED [[deprecated]]
#define CAF_TEST_DEPRECATED_EXPORT CAF_TEST_EXPORT CAF_TEST_DEPRECATED
#define CAF_TEST_DEPRECATED_NO_EXPORT LIBCAF_TEST_NO_EXPORT CAF_TEST_DEPRECATED
