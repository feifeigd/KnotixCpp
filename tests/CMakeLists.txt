add_executable(knotix_tests
    test_main.cpp
    json_test.cpp
    tool_test.cpp
    message_test.cpp
    function_registry_test.cpp
    agent_config_test.cpp
    "${PROJECT_SOURCE_DIR}/agent_config.cpp"
)

target_link_libraries(knotix_tests PRIVATE knotix CAF::core CAF::test)

add_test(NAME knotix_tests COMMAND knotix_tests)
