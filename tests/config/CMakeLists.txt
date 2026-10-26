house_cxx_add_module_test(pico_http config
    SOURCES profile_binding_test.cpp
    LIBRARIES pico_http::lwipopts_fallback
)
