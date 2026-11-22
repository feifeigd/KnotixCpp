add_executable(knotix_tests
    test_main.cpp
    json_test.cpp
    tool_test.cpp
    message_test.cpp
    function_registry_test.cpp
    agent_config_test.cpp
    sse_parser_test.cpp
    sse_end_to_end_test.cpp
    "${PROJECT_SOURCE_DIR}/agent_config.cpp"
    "${PROJECT_SOURCE_DIR}/openai_compatible_client.cpp"
)

find_package(CURL CONFIG REQUIRED)

target_link_libraries(knotix_tests PRIVATE knotix CAF::core CAF::net CAF::test CURL::libcurl)

add_test(NAME knotix_tests COMMAND knotix_tests)
