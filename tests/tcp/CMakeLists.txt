house_cxx_add_module_test(pico_http tcp
    SOURCES
        connection_id_test.cpp
        porting_example_test.cpp
        slot_table_test.cpp
        timeout_scheduler_test.cpp
    LIBRARIES pico_http::config
)
