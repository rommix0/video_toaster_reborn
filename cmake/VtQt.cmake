# Qt 6.8 (aqtinstall MSVC build) for the app. Nothing from VT_QT_DIR is committed or installed;
# vt_deploy_qt copies the runtime next to the executable.
set(VT_QT_DIR "${CMAKE_SOURCE_DIR}/../Qt/6.8.3/msvc2022_64" CACHE PATH
    "Root of the Qt 6.8 MSVC kit (lib/cmake/Qt6, bin/, plugins/)")
get_filename_component(VT_QT_DIR "${VT_QT_DIR}" ABSOLUTE)

if(NOT EXISTS "${VT_QT_DIR}/lib/cmake/Qt6/Qt6Config.cmake")
    message(FATAL_ERROR
        "Qt not found: ${VT_QT_DIR}/lib/cmake/Qt6/Qt6Config.cmake is missing.\n"
        "Install Qt 6.8 for msvc2022_64 (e.g. with aqtinstall) and set -DVT_QT_DIR=<its root> "
        "(default: ${CMAKE_SOURCE_DIR}/../Qt/6.8.3/msvc2022_64).")
endif()

list(APPEND CMAKE_PREFIX_PATH "${VT_QT_DIR}")
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Test)

# Copies the Qt runtime (DLLs, platform plugins) next to <target> after each build.
function(vt_deploy_qt target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${VT_QT_DIR}/bin/windeployqt.exe"
            $<IF:$<CONFIG:Debug>,--debug,--release>
            --no-translations --no-compiler-runtime
            "$<TARGET_FILE:${target}>")
endfunction()

# Qt Test runs headless (offscreen platform) and finds Qt's DLLs through PATH.
function(vt_qt_test_env test)
    set_tests_properties(${test} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${VT_QT_DIR}/bin")
endfunction()
