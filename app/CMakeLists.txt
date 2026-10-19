qt_standard_project_setup() # AUTOMOC

# Everything except main.cpp, so tests can link the app's code.
add_library(vt_app_lib STATIC
    src/main_window.cpp
    src/mixer_dock.cpp
    src/monitor_window.cpp
    src/project_grid.cpp
    src/settings.cpp
    src/toaster_screen.cpp
)
target_include_directories(vt_app_lib PUBLIC src)
# The default Toaster data root (settings.cpp).
target_compile_definitions(vt_app_lib PRIVATE VT_DATA_ROOT="${CMAKE_SOURCE_DIR}/..")
target_link_libraries(vt_app_lib PUBLIC Qt6::Widgets vt_media vt_assets)
if(MSVC)
    target_compile_options(vt_app_lib PRIVATE /W4 /utf-8)
    target_compile_options(vt_app_lib INTERFACE /utf-8)
endif()

qt_add_executable(vtoast WIN32 src/main.cpp)
target_link_libraries(vtoast PRIVATE vt_app_lib)
if(MSVC)
    target_compile_options(vtoast PRIVATE /W4)
endif()
vt_deploy_qt(vtoast)
vt_copy_ffmpeg_dlls(vtoast)
