add_library(vt_engine STATIC
    src/anim.cpp
    src/delta5.cpp
    src/delta7.cpp
    src/delta8.cpp
    src/crc32.cpp
    src/iff.cpp
    src/crouton.cpp
    src/ilbm.cpp
    src/sound.cpp
    src/timing.cpp
)
target_include_directories(vt_engine PUBLIC include)
if(MSVC)
    target_compile_options(vt_engine PRIVATE /W4)
endif()
