cmake_minimum_required(VERSION 3.25)

project(interkom
    VERSION 0.1.0
    DESCRIPTION "interkom"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_executable(interkom
    src/main.cpp
)

target_include_directories(interkom PRIVATE src)

target_compile_options(interkom PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

option(INTERKOM_BUILD_EXAMPLES "Build backend examples" ON)

if(INTERKOM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
