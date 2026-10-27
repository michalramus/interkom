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
    src/buffer/CommunicateBuffer.cpp
    src/buffer/Input.cpp
    src/buffer/Mixer.cpp
    src/clock/TimerClock.cpp
)
target_include_directories(interkom_core PUBLIC src)
target_compile_options(interkom_core PRIVATE ${INTERKOM_WARNINGS})

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PULSE QUIET IMPORTED_TARGET libpulse)
endif()

if(APPLE)
    target_sources(interkom_core PRIVATE src/audio/Mic_coreaudio.cpp)
    target_link_libraries(interkom_core PUBLIC
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
elseif(TARGET PkgConfig::PULSE)
    target_sources(interkom_core PRIVATE src/audio/Mic_pulseaudio.cpp)
    target_link_libraries(interkom_core PUBLIC PkgConfig::PULSE)
else()
    message(STATUS "interkom: no audio backend found, skipping Mic")
endif()

find_package(uvgrtp QUIET)
if(uvgrtp_FOUND)
    target_sources(interkom_core PRIVATE
        src/rtp/RTPSender.cpp
        src/rtp/RTPReceiver.cpp
    )
    target_link_libraries(interkom_core PUBLIC uvgrtp::uvgrtp)
else()
    message(STATUS "interkom: uvgRTP not found, skipping RTP")
endif()

add_executable(interkom
    src/main.cpp
)
target_link_libraries(interkom PRIVATE interkom_core)
target_compile_options(interkom PRIVATE ${INTERKOM_WARNINGS})

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
