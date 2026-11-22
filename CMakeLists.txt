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

find_package(nlohmann_json CONFIG REQUIRED)

add_subdirectory(knotix)

add_executable(FunctionCalling main.cpp)
target_link_libraries(FunctionCalling PRIVATE knotix)

find_package(CAF CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

add_executable(FunctionCallingCAF
    caf_main.cpp
    agent_config.cpp
    agent_conversation.cpp
    openai_compatible_client.cpp
)
target_link_libraries(FunctionCallingCAF PRIVATE knotix CAF::core CAF::net CURL::libcurl)

option(KNOTIX_BUILD_TESTS "Build the CAF-based unit tests" ON)
if(KNOTIX_BUILD_TESTS)
    add_subdirectory(third_party/caf-test)
    enable_testing()
    add_subdirectory(tests)
endif()
                               