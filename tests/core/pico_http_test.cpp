// SPDX-License-Identifier: MIT
// Compiles the umbrella header as a translation unit so the full public surface
// is exercised by the analysis toolchain, and pins the advertised version to
// the values the build system carries.
#include "pico_http/pico_http.hpp"

#include <gtest/gtest.h>

TEST(PicoHttp, VersionConstantsMatchTheBuildSystem) {
    EXPECT_EQ(pico_http::VERSION_MAJOR, PICO_HTTP_CMAKE_VERSION_MAJOR);
    EXPECT_EQ(pico_http::VERSION_MINOR, PICO_HTTP_CMAKE_VERSION_MINOR);
    EXPECT_EQ(pico_http::VERSION_PATCH, PICO_HTTP_CMAKE_VERSION_PATCH);
}
