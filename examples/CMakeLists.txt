# Backend examples. Each is a standalone executable; a backend's examples are
# only configured when its dependencies are present on this machine.

find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(PULSE QUIET IMPORTED_TARGET libpulse)
endif()

function(interkom_add_example name source)
    add_executable(${name} ${source})
    target_compile_options(${name} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
endfunction()

if(TARGET PkgConfig::PULSE)
    interkom_add_example(example_pulseaudio_capture pulseaudio/capture.cpp)
    interkom_add_example(example_pulseaudio_playback pulseaudio/playback.cpp)
    target_link_libraries(example_pulseaudio_capture PRIVATE PkgConfig::PULSE)
    target_link_libraries(example_pulseaudio_playback PRIVATE PkgConfig::PULSE)
else()
    message(STATUS "examples: libpulse not found, skipping PulseAudio examples")
endif()
