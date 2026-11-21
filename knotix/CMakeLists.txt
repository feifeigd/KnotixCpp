
add_library(knotix STATIC
    knotix.cpp
    schema/message.cpp
    schema/message.h
    schema/tool.h
)

target_include_directories(knotix PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(knotix PUBLIC nlohmann_json::nlohmann_json)
