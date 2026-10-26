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

# libFuzzer targets: the parser in isolation, and the full request lifecycle
# (parse -> dispatch -> streamed response) over FakeTcpTransport. The corpus in
# http_parser_test.cpp keeps the same invariants under continuous CI coverage.
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
