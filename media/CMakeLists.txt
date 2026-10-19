set(VT_MEDIA_SOURCES
    src/audio_convert.cpp
    src/devices.cpp
    src/ffmpeg.cpp
    src/geometry.cpp
    src/input.cpp
    src/input_demux.cpp
    src/mixer.cpp
    src/program_loop.cpp
    src/recorder.cpp
    src/time.cpp
    src/video_convert.cpp
)

# The same sources twice: vt_media is what ships, vt_media_testable adds the test seams
# (VT_MEDIA_TEST_SEAMS, e.g. RecorderSettings::failAfterBytesForTest) and is linked by tests only.
foreach(lib vt_media vt_media_testable)
    add_library(${lib} STATIC ${VT_MEDIA_SOURCES})
    target_include_directories(${lib} PUBLIC include)
    target_link_libraries(${lib} PUBLIC vt_engine vt::ffmpeg)
    target_link_libraries(${lib} PRIVATE winmm) # timeBeginPeriod, the program loop's fallback timer
    if(MSVC)
        target_compile_options(${lib} PRIVATE /W4)
    endif()
endforeach()
target_compile_definitions(vt_media_testable PUBLIC VT_MEDIA_TEST_SEAMS)
