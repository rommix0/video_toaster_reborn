# vt_assets: Qt-free decoders for the original Toaster UI art, fonts and crouton icons.
add_library(vt_assets STATIC
    src/font.cpp
    src/hunk.cpp
    src/panel.cpp
)
target_include_directories(vt_assets PUBLIC include)
target_link_libraries(vt_assets PUBLIC vt_engine)
if(MSVC)
    target_compile_options(vt_assets PRIVATE /W4)
endif()
