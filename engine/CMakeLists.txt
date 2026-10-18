add_library(vt_engine STATIC
    src/crc32.cpp
    src/iff.cpp
    src/crouton.cpp
)
target_include_directories(vt_engine PUBLIC include)
if(MSVC)
    target_compile_options(vt_engine PRIVATE /W4)
endif()
