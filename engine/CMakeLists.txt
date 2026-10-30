add_library(vt_engine STATIC
    src/algo.cpp
    src/anim.cpp
    src/background.cpp
    src/cg_scroll.cpp
    src/chromafx.cpp
    src/delta5.cpp
    src/delta7.cpp
    src/delta8.cpp
    src/dve.cpp
    src/dv_colour.cpp
    src/crc32.cpp
    src/effect_algo.cpp
    src/effect_anim.cpp
    src/effect_bar.cpp
    src/effect_basic.cpp
    src/effect_chromafx.cpp
    src/effect_dve.cpp
    src/effect_factory.cpp
    src/effect_gradient.cpp
    src/framestore.cpp
    src/iff.cpp
    src/key.cpp
    src/log.cpp
    src/crouton.cpp
    src/ilbm.cpp
    src/mix.cpp
    src/sound.cpp
    src/switcher.cpp
    src/timing.cpp
)
target_include_directories(vt_engine PUBLIC include)
if(MSVC)
    target_compile_options(vt_engine PRIVATE /W4)
endif()
