cmake_minimum_required(VERSION 3.16)

project(FunctionCalling LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_executable(FunctionCalling main.cpp)

find_package(CAF CONFIG REQUIRED)

add_executable(FunctionCallingCAF caf_main.cpp)
target_link_libraries(FunctionCallingCAF PRIVATE CAF::core)
