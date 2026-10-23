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
    interkom_add_example(example_rtp_pulseaudio_send rtp/pulseaudio_send.cpp)
    interkom_add_example(example_rtp_pulseaudio_recv rtp/pulseaudio_recv.cpp)
    foreach(target
        example_pulseaudio_capture
        example_pulseaudio_playback
        example_rtp_pulseaudio_send
        example_rtp_pulseaudio_recv
    )
        target_link_libraries(${target} PRIVATE PkgConfig::PULSE)
    endforeach()
else()
    message(STATUS "examples: libpulse not found, skipping PulseAudio examples")
endif()

if(APPLE)
    interkom_add_example(example_coreaudio_capture coreaudio/capture.cpp)
    interkom_add_example(example_coreaudio_playback coreaudio/playback.cpp)
    interkom_add_example(example_rtp_coreaudio_send rtp/coreaudio_send.cpp)
    interkom_add_example(example_rtp_coreaudio_recv rtp/coreaudio_recv.cpp)
    foreach(target
        example_coreaudio_capture
        example_coreaudio_playback
        example_rtp_coreaudio_send
        example_rtp_coreaudio_recv
    )
        target_link_libraries(${target} PRIVATE
            "-framework AudioToolbox"
            "-framework AudioUnit"
            "-framework CoreAudio"
            "-framework CoreFoundation"
        )
    endforeach()
else()
    message(STATUS "examples: not Apple, skipping CoreAudio examples")
endif()
