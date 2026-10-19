qt_standard_project_setup() # AUTOMOC

add_executable(vt_app_tests
    test_main.cpp
    test_main_window.cpp
    test_project_grid.cpp
    test_toaster_screen.cpp
)
target_link_libraries(vt_app_tests PRIVATE vt_app_lib Qt6::Test)
target_compile_definitions(vt_app_tests PRIVATE VT_DATA_ROOT="${CMAKE_SOURCE_DIR}/..")
if(MSVC)
    target_compile_options(vt_app_tests PRIVATE /W4)
endif()
vt_copy_ffmpeg_dlls(vt_app_tests)

add_test(NAME vt_app_tests COMMAND vt_app_tests)
vt_qt_test_env(vt_app_tests)
