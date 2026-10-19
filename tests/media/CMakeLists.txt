add_executable(vt_media_tests
    test_ffmpeg.cpp
    test_time_geometry.cpp
)
target_link_libraries(vt_media_tests PRIVATE vt_media catch2_amalgamated)
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
