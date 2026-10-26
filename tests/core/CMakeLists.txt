house_cxx_add_module_test(pico_http core
    NAME pico_http_umbrella_tests
    SOURCES pico_http_test.cpp
)

# The umbrella header's version constants are hand-maintained; feeding the
# project() version in here is what makes the test able to catch drift between
# the two rather than comparing the header against itself.
house_cxx_version_definitions(pico_http_umbrella_tests pico_http PRIVATE)
