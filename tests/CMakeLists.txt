
add_subdirectory(core)
add_subdirectory(util)
add_subdirectory(tcp)
add_subdirectory(http)
add_subdirectory(config)
# The fake transport is built only for host tests.
if(NOT PICO_HTTP_BUILD_FIRMWARE)
    add_subdirectory(testing)
    # The document examples also use the fake transport.
    add_subdirectory(docs)
endif()
