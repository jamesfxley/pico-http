// SPDX-License-Identifier: MIT
// The umbrella header is checked by the analysis toolchain.
#include "pico_http/pico_http.hpp"

#include <gtest/gtest.h>

TEST(PicoHttp, VersionConstantsMatchTheBuildSystem) {
    EXPECT_EQ(pico_http::VERSION_MAJOR, PICO_HTTP_CMAKE_VERSION_MAJOR);
    EXPECT_EQ(pico_http::VERSION_MINOR, PICO_HTTP_CMAKE_VERSION_MINOR);
    EXPECT_EQ(pico_http::VERSION_PATCH, PICO_HTTP_CMAKE_VERSION_PATCH);
}
