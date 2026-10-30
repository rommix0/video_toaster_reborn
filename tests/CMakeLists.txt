add_library(catch2_amalgamated STATIC
    ${CMAKE_SOURCE_DIR}/third_party/catch2/catch_amalgamated.cpp)
target_include_directories(catch2_amalgamated SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/catch2)

add_executable(vt_tests
    test_algo.cpp
    test_anim.cpp
    test_background.cpp
    test_cgkeys.cpp
    test_chromafx.cpp
    test_crc32.cpp
    test_crouton.cpp
    test_dve.cpp
    test_effects.cpp
    test_framestore.cpp
    test_golden.cpp
    test_ilbm.cpp
    test_key.cpp
    test_mix.cpp
    test_switcher.cpp
    test_timing.cpp
)
target_link_libraries(vt_tests PRIVATE vt_engine vt_assets catch2_amalgamated)
target_include_directories(vt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(vt_tests PRIVATE
    VT_DATA_ROOT="${CMAKE_SOURCE_DIR}/.."
    VT_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
if(MSVC)
    target_compile_options(vt_tests PRIVATE /W4)
endif()
add_test(NAME vt_tests COMMAND vt_tests)

# vt_render: a CTest script test (the CLI renders Page Peel ! at fast speed; 60 frames).
add_test(NAME vt_render_cli COMMAND ${CMAKE_COMMAND}
    -DVT_RENDER=$<TARGET_FILE:vt_render>
    -DVT_DATA_ROOT=${CMAKE_SOURCE_DIR}/..
    -DWORK_DIR=${CMAKE_BINARY_DIR}/vt_render_cli_work
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.cmake)

add_subdirectory(media)
add_subdirectory(assets)
add_subdirectory(app)
