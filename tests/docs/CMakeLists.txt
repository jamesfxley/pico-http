# The examples are compiled without linking, so firmware factories need no
# definition here. The harness reads the document blocks at configure time.
house_cxx_add_doc_examples(pico_http_doc_examples PACKAGE pico_http
    DOCUMENTS ${PROJECT_SOURCE_DIR}/README.md
              ${PROJECT_SOURCE_DIR}/docs/usage-guide.md
    HARNESS_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    LIBRARIES pico_http::core pico_http::testing)
