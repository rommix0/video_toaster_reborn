add_library(catch2_amalgamated STATIC
    ${CMAKE_SOURCE_DIR}/third_party/catch2/catch_amalgamated.cpp)
target_include_directories(catch2_amalgamated SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/catch2)

add_executable(vt_tests
    test_crc32.cpp
    test_crouton.cpp
    test_golden.cpp
)
target_link_libraries(vt_tests PRIVATE vt_engine catch2_amalgamated)
target_include_directories(vt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(vt_tests PRIVATE
    VT_DATA_ROOT="${CMAKE_SOURCE_DIR}/.."
    VT_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
if(MSVC)
    target_compile_options(vt_tests PRIVATE /W4)
endif()
add_test(NAME vt_tests COMMAND vt_tests)
