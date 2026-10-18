# CTest script test for vt_render. Run as:
#   cmake -DVT_RENDER=<exe> -DVT_DATA_ROOT=<dir> -DWORK_DIR=<scratch> -P test_cli.cmake
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# 1. Page Peel ! at fast speed is one second (manual, Tutorials p24): 60 frames at 59.94 fps, one
#    per animation frame, cropped to the visible area.
set(out "${WORK_DIR}/peel")
execute_process(
    COMMAND "${VT_RENDER}" "${VT_DATA_ROOT}/4.2/Effects/4000FX/PagePeel/Page Peel !" --out "${out}" --speed fast
    RESULT_VARIABLE rc OUTPUT_VARIABLE so ERROR_VARIABLE se)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "vt_render failed (${rc}): ${so}${se}")
endif()
file(GLOB frames "${out}/*.ppm")
list(LENGTH frames n)
if(NOT n EQUAL 60)
    message(FATAL_ERROR "expected exactly 60 .ppm files, found ${n}")
endif()
if(NOT EXISTS "${out}/0001.ppm" OR NOT EXISTS "${out}/0060.ppm")
    message(FATAL_ERROR "frames must be numbered 0001..0060")
endif()
# P6 header "P6\n640 432\n255\n" is 15 bytes, then 640*432*3 bytes of RGB.
file(SIZE "${out}/0001.ppm" sz)
math(EXPR want "15 + 640 * 432 * 3")
if(NOT sz EQUAL want)
    message(FATAL_ERROR "cropped frame size ${sz}, expected ${want}")
endif()
file(READ "${out}/0001.ppm" head LIMIT 15)
if(NOT head STREQUAL "P6\n640 432\n255\n")
    message(FATAL_ERROR "bad PPM header")
endif()

# 2. --underscan writes the whole 768x482 raster.
set(out2 "${WORK_DIR}/underscan")
execute_process(
    COMMAND "${VT_RENDER}" "${VT_DATA_ROOT}/4.2/Effects/4000FX/PagePeel/Page Peel !" --out "${out2}" --speed fast --underscan
    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE se)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "vt_render --underscan failed (${rc}): ${se}")
endif()
file(SIZE "${out2}/0001.ppm" sz2)
math(EXPR want2 "15 + 768 * 482 * 3")
if(NOT sz2 EQUAL want2)
    message(FATAL_ERROR "underscan frame size ${sz2}, expected ${want2}")
endif()

# 3. An effect that cannot be played prints a reason and fails.
execute_process(
    COMMAND "${VT_RENDER}" "${VT_DATA_ROOT}/no/such/effect" --out "${WORK_DIR}/bad"
    RESULT_VARIABLE rc OUTPUT_VARIABLE so ERROR_VARIABLE se)
if(rc EQUAL 0)
    message(FATAL_ERROR "vt_render accepted a missing effect")
endif()
if("${so}${se}" STREQUAL "")
    message(FATAL_ERROR "vt_render gave no reason for the missing effect")
endif()
