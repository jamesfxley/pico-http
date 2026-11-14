cmake_minimum_required(VERSION 3.26)
if(PICO_SDK_PATH)
    include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    project(pico_http_consumer_smoke LANGUAGES C CXX ASM)
    pico_sdk_init()
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
else()
    project(pico_http_consumer_smoke LANGUAGES CXX)
endif()

# The installed package supplies profile definitions. The smoke test must be
# excluded by .lintignore because it has no entry in the merged compile
# database.
find_package(pico_http REQUIRED)

if(PICO_SDK_PATH)
    add_executable(consumer_smoke firmware_smoke.cxx)
else()
    add_executable(consumer_smoke consumer_smoke.cxx)
endif()

# Naming each exported target detects a missing export, including the shipped
# test double.
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
