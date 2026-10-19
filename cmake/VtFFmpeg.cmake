# FFmpeg 9 (shared build) as imported targets. Nothing from VT_FFMPEG_DIR is committed or installed;
# its DLLs are copied next to each executable that links vt_media (vt_copy_ffmpeg_dlls).
set(VT_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/../ffmpeg-9.0.2-full_build-shared" CACHE PATH
    "Root of the FFmpeg 9 shared build (include/, lib/, bin/)")
get_filename_component(VT_FFMPEG_DIR "${VT_FFMPEG_DIR}" ABSOLUTE)

if(NOT EXISTS "${VT_FFMPEG_DIR}/include/libavcodec/version_major.h")
    message(FATAL_ERROR
        "FFmpeg not found: ${VT_FFMPEG_DIR}/include/libavcodec/version_major.h is missing.\n"
        "Extract an FFmpeg 9 'full_build-shared' package and set -DVT_FFMPEG_DIR=<its root> "
        "(default: ${CMAKE_SOURCE_DIR}/../ffmpeg-9.0.2-full_build-shared).")
endif()

add_library(vt_ffmpeg INTERFACE)
add_library(vt::ffmpeg ALIAS vt_ffmpeg)
# FFmpeg's headers produce warnings under /W4, so they are SYSTEM includes.
target_include_directories(vt_ffmpeg SYSTEM INTERFACE "${VT_FFMPEG_DIR}/include")

# <library>=<DLL major version>
foreach(entry avcodec=63 avformat=63 avdevice=63 avutil=61 swscale=10 swresample=7)
    string(REPLACE "=" ";" parts "${entry}")
    list(GET parts 0 lib)
    list(GET parts 1 major)
    add_library(ffmpeg_${lib} SHARED IMPORTED GLOBAL)
    set_target_properties(ffmpeg_${lib} PROPERTIES
        IMPORTED_IMPLIB "${VT_FFMPEG_DIR}/lib/${lib}.lib"
        IMPORTED_LOCATION "${VT_FFMPEG_DIR}/bin/${lib}-${major}.dll")
    target_link_libraries(vt_ffmpeg INTERFACE ffmpeg_${lib})
endforeach()

# Copies every FFmpeg DLL (avdevice needs avfilter, which no target links directly) next to <target>.
function(vt_copy_ffmpeg_dlls target)
    file(GLOB dlls "${VT_FFMPEG_DIR}/bin/*.dll")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dlls} "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS)
endfunction()
