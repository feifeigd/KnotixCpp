
add_library(knotix STATIC
    knotix.cpp
    schema/message.cpp
    schema/message.h
    schema/tool.h
)

target_include_directories(knotix PUBLIC "${PROJECT_SOURCE_DIR}")
