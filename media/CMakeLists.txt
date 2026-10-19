add_library(vt_media STATIC
    src/ffmpeg.cpp
    src/geometry.cpp
    src/time.cpp
)
target_include_directories(vt_media PUBLIC include)
target_link_libraries(vt_media PUBLIC vt_engine vt::ffmpeg)
if(MSVC)
    target_compile_options(vt_media PRIVATE /W4)
endif()
