qt_standard_project_setup() # AUTOMOC

add_executable(vt_app_tests
    test_main.cpp
    test_app_e2e.cpp
    test_main_window.cpp
    test_mixer_dock.cpp
    test_monitor.cpp
    test_position_dock.cpp
    test_project_grid.cpp
    test_settings.cpp
    test_toaster_screen.cpp
)
target_link_libraries(vt_app_tests PRIVATE vt_app_lib Qt6::Test)
target_compile_definitions(vt_app_tests PRIVATE
    VT_DATA_ROOT="${CMAKE_SOURCE_DIR}/.."
    VT_MEDIA_FIXTURES="${CMAKE_BINARY_DIR}/media_fixtures")
if(MSVC)
    target_compile_options(vt_app_tests PRIVATE /W4)
endif()
vt_copy_ffmpeg_dlls(vt_app_tests)

add_test(NAME vt_app_tests COMMAND vt_app_tests)
vt_qt_test_env(vt_app_tests)
# The end-to-end tests play the Plan 2 media fixtures (tests/media/make_fixtures.cmake).
set_tests_properties(vt_app_tests PROPERTIES FIXTURES_REQUIRED media_fixtures)
# The real-time end-to-end test needs the desktop and a camera (label `live`, tools\live.bat).
add_test(NAME vt_app_live COMMAND vt_app_tests realtimeRecordingWithLiveInputs)
vt_qt_test_env(vt_app_live)
set_tests_properties(vt_app_live PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VT_APP_LIVE=1"
    FIXTURES_REQUIRED media_fixtures
    LABELS live)
