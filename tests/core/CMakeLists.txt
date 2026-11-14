house_cxx_add_module_test(pico_http core
    NAME pico_http_umbrella_tests
    SOURCES pico_http_test.cpp
)

# The CMake version provides an independent check of the header constants.
house_cxx_version_definitions(pico_http_umbrella_tests pico_http PRIVATE)
