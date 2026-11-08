add_executable(vt_assets_tests
    test_font.cpp
    test_hunk.cpp
    test_icons.cpp
    test_panel.cpp
)
target_link_libraries(vt_assets_tests PRIVATE vt_assets catch2_amalgamated)
target_include_directories(vt_assets_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(vt_assets_tests PRIVATE
    VT_DATA_ROOT="${VT_DATA_ROOT}"
    VT_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
if(MSVC)
    target_compile_options(vt_assets_tests PRIVATE /W4)
endif()
add_test(NAME vt_assets_tests COMMAND vt_assets_tests)
