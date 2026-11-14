house_cxx_add_module_test(pico_http http
    SOURCES
        http_headers_test.cpp
        http_response_test.cpp
        http_route_table_test.cpp
        http_parsing_test.cpp
        http_parser_test.cpp
        http_service_test.cpp
        http_deferred_test.cpp
    LIBRARIES pico_http::testing pico_util::heap pico_util::testing
)

# The fixed parser corpus runs in CI. libFuzzer explores generated input for
# the parser and service.
house_cxx_add_fuzzer(pico_http_parser_fuzz
    PACKAGE pico_http
    SOURCE http_parser_fuzz.cpp
    LIBRARIES pico_http::http
)
house_cxx_add_fuzzer(pico_http_service_fuzz
    PACKAGE pico_http
    SOURCE http_service_fuzz.cpp
    LIBRARIES pico_http::http pico_http::testing
)

# An HTTPResponse is several KiB, and the receive callback runs on a 2 KiB
# core-0 stack. GCC reports every frame of the service, so the check holds
# whatever is inlined into what (AR-13).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_library(pico_http_http_stack_probe OBJECT
        ${PROJECT_SOURCE_DIR}/src/http/http_service.cpp)
    # Compile the service source as the library does, with its private
    # include paths and definitions.
    target_link_libraries(pico_http_http_stack_probe PRIVATE
        pico_http_strict pico_http::http)
    target_include_directories(pico_http_http_stack_probe PRIVATE
        $<TARGET_PROPERTY:pico_http_http,INCLUDE_DIRECTORIES>)
    target_compile_definitions(pico_http_http_stack_probe PRIVATE
        $<TARGET_PROPERTY:pico_http_http,COMPILE_DEFINITIONS>)
    # Sanitiser instrumentation enlarges every frame, so the bound applies to
    # uninstrumented host frames. The object is never linked, so no runtime is
    # missing.
    target_compile_options(pico_http_http_stack_probe PRIVATE
        -fstack-usage -fno-sanitize=all)
    # One compile command per source keeps clang-tidy to the library's own.
    set_target_properties(pico_http_http_stack_probe PROPERTIES
        EXPORT_COMPILE_COMMANDS OFF)
    add_test(NAME pico_http.StackUsage.BoundaryResponsesBuildNoHttpResponse
        COMMAND ${CMAKE_COMMAND}
            -DOBJECT=$<TARGET_OBJECTS:pico_http_http_stack_probe>
            -DLIMIT=1024
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_stack_usage.cmake)
endif()
