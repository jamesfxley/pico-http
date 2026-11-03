pico_family_add_device_test(pico_http_device_tests
    PACKAGE pico_http
    SOURCES main.cpp
    LIBRARIES pico_http::core ${PICO_LWIPOPTS_TARGET})
