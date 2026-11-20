cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

project(FunctionCalling LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_executable(FunctionCalling main.cpp)

find_package(CAF CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

add_executable(FunctionCallingCAF
    caf_main.cpp
    agent_config.cpp
    agent_conversation.cpp
    openai_compatible_client.cpp
)
target_link_libraries(FunctionCallingCAF PRIVATE CAF::core CAF::net CURL::libcurl)
                               