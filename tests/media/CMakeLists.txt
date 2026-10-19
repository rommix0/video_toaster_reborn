add_executable(vt_media_tests
    test_audio.cpp
    test_audio_out.cpp
    test_devices.cpp
    test_ffmpeg.cpp
    test_input.cpp
    test_live_clock.cpp
    test_mixer.cpp
    test_program_loop.cpp
    test_recorder.cpp
    test_time_geometry.cpp
    test_video_convert.cpp
)
target_link_libraries(vt_media_tests PRIVATE vt_media_testable catch2_amalgamated)
# live_clock.h is internal to vt_media; its unit test reaches into media/src.
target_include_directories(vt_media_tests PRIVATE ${CMAKE_SOURCE_DIR}/media/src)
target_compile_definitions(vt_media_tests PRIVATE
    VT_DATA_ROOT="${CMAKE_SOURCE_DIR}/.."
    VT_MEDIA_FIXTURES="${CMAKE_BINARY_DIR}/media_fixtures")
if(MSVC)
    target_compile_options(vt_media_tests PRIVATE /W4)
endif()
vt_copy_ffmpeg_dlls(vt_media_tests)

# Test media come from the FFmpeg build's ffmpeg.exe; nothing binary is committed.
add_test(NAME vt_media_fixtures COMMAND ${CMAKE_COMMAND}
    -DFFMPEG=${VT_FFMPEG_DIR}/bin/ffmpeg.exe
    -DOUT_DIR=${CMAKE_BINARY_DIR}/media_fixtures
    -P ${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.cmake)
set_tests_properties(vt_media_fixtures PROPERTIES FIXTURES_SETUP media_fixtures)

# Tests tagged [live] need devices or a desktop; they carry the `live` label (tools\live.bat).
add_test(NAME vt_media_tests COMMAND vt_media_tests "~[live]")
# --allow-running-no-tests: Catch2 exits non-zero when a filter matches nothing (no live tests yet).
add_test(NAME vt_media_live COMMAND vt_media_tests "[live]" --allow-running-no-tests)
set_tests_properties(vt_media_tests vt_media_live PROPERTIES FIXTURES_REQUIRED media_fixtures)
set_tests_properties(vt_media_live PROPERTIES LABELS live)

# vt_show end to end: a scripted show recorded to an FFV1 MKV and probed (needs the media fixtures).
add_test(NAME vt_show_cli COMMAND ${CMAKE_COMMAND}
    -DVT_SHOW=$<TARGET_FILE:vt_show>
    -DFFPROBE=${VT_FFMPEG_DIR}/bin/ffprobe.exe
    -DVT_DATA_ROOT=${CMAKE_SOURCE_DIR}/..
    -DFIXTURES=${CMAKE_BINARY_DIR}/media_fixtures
    -DWORK_DIR=${CMAKE_BINARY_DIR}/vt_show_cli_work
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test_vt_show.cmake)
set_tests_properties(vt_show_cli PROPERTIES FIXTURES_REQUIRED media_fixtures)
