# The test tree mirrors src/ (CXX-13.2), so src/ is exactly the shippable
# surface: an install, footprint, lint or coverage rule over it needs no test
# exclusion. One directory per module, added after src/ so every
# pico_http::<module> target the tests link already exists.

add_subdirectory(core)
add_subdirectory(util)
add_subdirectory(tcp)
add_subdirectory(http)
add_subdirectory(config)
# The host-side fake transport is not built for firmware images, so neither are
# its tests. Mirrors the guard on src/testing/.
if(NOT PICO_HTTP_BUILD_FIRMWARE)
    add_subdirectory(testing)
    # The examples use the fake transport too, so they are host-only as well.
    add_subdirectory(docs)
endif()
