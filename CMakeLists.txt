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

set(INTERKOM_WARNINGS
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Components. The audio backend is picked by platform; RTP needs uvgRTP.
add_library(interkom_core STATIC
    src/buffer/source/CommunicateBuffer.cpp
    src/buffer/jitter/DepthPolicy.cpp
    src/buffer/jitter/HeapStore.cpp
    src/buffer/Input.cpp
    src/buffer/mix/Mixer.cpp
    src/buffer/jitter/RepeatFadePlc.cpp
    src/buffer/mix/Subtractor.cpp
    src/clock/DeviceClock.cpp
    src/clock/TimerClock.cpp
)
target_include_directories(interkom_core PUBLIC src)
target_compile_options(interkom_core PRIVATE ${INTERKOM_WARNINGS})

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PULSE QUIET IMPORTED_TARGET libpulse)
endif()

set(INTERKOM_AUDIO ON)
if(APPLE)
    target_sources(interkom_core PRIVATE
        src/audio/Mic_coreaudio.cpp
        src/audio/Speaker_coreaudio.cpp
    )
    target_link_libraries(interkom_core PUBLIC
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
elseif(TARGET PkgConfig::PULSE)
    target_sources(interkom_core PRIVATE
        src/audio/Mic_pulseaudio.cpp
        src/audio/Speaker_pulseaudio.cpp
    )
    target_link_libraries(interkom_core PUBLIC PkgConfig::PULSE)
else()
    set(INTERKOM_AUDIO OFF)
    message(STATUS "interkom: no audio backend found, skipping Mic and Speaker")
endif()

# uvgRTP: an installed copy wins, otherwise it is fetched and built in-tree
# without Crypto++ (no SRTP), so a fresh checkout needs nothing preinstalled.
include(FetchContent)
set(UVGRTP_DISABLE_CRYPTO ON CACHE BOOL "" FORCE)
set(UVGRTP_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(UVGRTP_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(UVGRTP_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(UVGRTP_DISABLE_WERROR ON CACHE BOOL "" FORCE)
FetchContent_Declare(uvgrtp
    GIT_REPOSITORY https://github.com/ultravideo/uvgRTP.git
    GIT_TAG v4.0.1
    GIT_SHALLOW TRUE
    SYSTEM
    FIND_PACKAGE_ARGS
)
FetchContent_MakeAvailable(uvgrtp)
if(NOT TARGET uvgrtp::uvgrtp)
    # In-tree build: same name as the installed package, and its own
    # warnings are not ours to fix.
    add_library(uvgrtp::uvgrtp ALIAS uvgrtp)
    target_compile_options(uvgrtp PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
endif()

if(TARGET uvgrtp::uvgrtp)
    target_sources(interkom_core PRIVATE
        src/rtp/RTPSender.cpp
        src/rtp/RTPReceiver.cpp
    )
    target_link_libraries(interkom_core PUBLIC uvgrtp::uvgrtp)
else()
    message(STATUS "interkom: uvgRTP not found, skipping RTP")
endif()

# The two programs need both the network and the audio side.
if(INTERKOM_AUDIO)
    foreach(program server client)
        add_executable(interkom_${program} src/${program}_main.cpp)
        target_link_libraries(interkom_${program} PRIVATE interkom_core)
        target_compile_options(interkom_${program} PRIVATE ${INTERKOM_WARNINGS})
    endforeach()
else()
    message(STATUS "interkom: needs an audio backend, skipping server and client")
endif()

option(INTERKOM_BUILD_EXAMPLES "Build backend examples" ON)

if(INTERKOM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

option(INTERKOM_BUILD_TESTS "Build tests" ON)

if(INTERKOM_BUILD_TESTS)
    enable_testing()
    add_executable(jitter_buffer_test tests/jitter_buffer_test.cpp)
    target_link_libraries(jitter_buffer_test PRIVATE interkom_core)
    target_compile_options(jitter_buffer_test PRIVATE ${INTERKOM_WARNINGS})
    add_test(NAME jitter_buffer COMMAND jitter_buffer_test)

    add_executable(buffer_test tests/buffer_test.cpp)
    target_link_libraries(buffer_test PRIVATE interkom_core)
    target_compile_options(buffer_test PRIVATE ${INTERKOM_WARNINGS})
    add_test(NAME buffer COMMAND buffer_test)
endif()
