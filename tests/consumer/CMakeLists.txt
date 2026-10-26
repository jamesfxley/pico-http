cmake_minimum_required(VERSION 3.26)
if(PICO_SDK_PATH)
    include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    project(pico_http_consumer_smoke LANGUAGES C CXX ASM)
    pico_sdk_init()
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
else()
    project(pico_http_consumer_smoke LANGUAGES CXX)
endif()

# Stands in for a downstream project consuming an installed pico-http. Its
# purpose is to prove that the sizing profile reaches a consumer's own
# translation units: consumer_smoke.cxx static_asserts the layout it sees against
# the profile this build was configured with, so a profile that failed to
# propagate is a compile error here rather than a silent ODR mismatch at link
# time.
#
# This translation unit compiles only against an installed package, so it has
# no entry in the merged compile database and the repo-wide clang-tidy sweep
# must skip it. The root .lintignore is what does that; the .cxx extension does
# not; the lint gate enumerates .cxx alongside .cpp and .cc.
find_package(pico_http REQUIRED)

if(PICO_SDK_PATH)
    add_executable(consumer_smoke firmware_smoke.cxx)
else()
    add_executable(consumer_smoke consumer_smoke.cxx)
endif()

# pico_http::testing is linked as well as pico_http::core: the test double is
# part of the shipped surface, and an export set that drops it fails only here.
# Every exported target is named, not only the aggregate. The aggregate pulls
# most of them in transitively, so a module dropped from the export set would
# still resolve here and the smoke would report nothing -- which is the one
# contract this executable exists to prove.
target_link_libraries(consumer_smoke PRIVATE
    pico_http::core
    pico_http::config
    pico_http::util
    pico_http::tcp
    pico_http::http
    pico_http::lwipopts_assertions
    pico_http::lwipopts_fallback
)

target_compile_features(consumer_smoke PRIVATE cxx_std_20)

if(PICO_SDK_PATH)
    target_include_directories(consumer_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(consumer_smoke PRIVATE pico_http::tcp_lwip)
else()
    target_link_libraries(consumer_smoke PRIVATE pico_http::testing)
endif()
