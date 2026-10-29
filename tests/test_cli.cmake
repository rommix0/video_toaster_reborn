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

# 4. A framestore (FRAM crouton) is not an effect: its picture goes to still.png, a PNG of the
#    visible area (640x432; the IHDR width and height are at bytes 16..23), and --underscan gives
#    the whole raster. The whole file is pinned by its SHA-256: both were checked by decoding them
#    with Pillow (chunk CRCs and zlib stream valid, the 75 % bars at 191).
set(out4 "${WORK_DIR}/still")
execute_process(
    COMMAND "${VT_RENDER}" "${VT_DATA_ROOT}/4.2/Framestore/ColorBars" --out "${out4}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE so ERROR_VARIABLE se)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "vt_render on a framestore failed (${rc}): ${so}${se}")
endif()
if(NOT EXISTS "${out4}/still.png")
    message(FATAL_ERROR "no still.png written for the framestore")
endif()
file(READ "${out4}/still.png" png LIMIT 24 HEX)
if(NOT png STREQUAL "89504e470d0a1a0a0000000d4948445200000280000001b0")  # signature, IHDR 640 x 432
    message(FATAL_ERROR "still.png is not a 640x432 PNG: ${png}")
endif()
file(SHA256 "${out4}/still.png" sha4)
if(NOT sha4 STREQUAL "5e0055900e572526963a5fd128163ee4e65e76ef81c849fb45e15ca029526eaf")
    message(FATAL_ERROR "still.png changed (sha256 ${sha4})")
endif()

set(out5 "${WORK_DIR}/still_underscan")
execute_process(
    COMMAND "${VT_RENDER}" "${VT_DATA_ROOT}/4.2/Framestore/ColorBars" --out "${out5}" --underscan
    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE se)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "vt_render --underscan on a framestore failed (${rc}): ${se}")
endif()
file(READ "${out5}/still.png" png5 LIMIT 24 HEX)
if(NOT png5 STREQUAL "89504e470d0a1a0a0000000d4948445200000300000001e2")  # 768 x 482
    message(FATAL_ERROR "underscan still.png is not 768x482: ${png5}")
endif()
file(SHA256 "${out5}/still.png" sha5)
if(NOT sha5 STREQUAL "a8570f842cae5fcb0bece6022b678bc739a025b865e9e80b72335a8aecdbf919")
    message(FATAL_ERROR "underscan still.png changed (sha256 ${sha5})")
endif()

# 5. The keyer: Superimpose = the key card (--c), Key Black. At L 0 the key source shows alone (the
#    same frame as the card on Main), at L 257 Main shows alone; --tbar 0 renders one held frame.
set(fade "${VT_DATA_ROOT}/4.2/Effects/2000FX/Smooth Fade")
foreach(run "card;--a;card" "l0;--c;card;--key;black;--level;0" "plain" "l257;--c;card;--key;black;--level;257")
    list(POP_FRONT run name)
    execute_process(
        COMMAND "${VT_RENDER}" "${fade}" --out "${WORK_DIR}/key_${name}" --tbar 0 ${run}
        RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE se)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "vt_render key run ${name} failed (${rc}): ${se}")
    endif()
    file(GLOB held "${WORK_DIR}/key_${name}/*.ppm")
    list(LENGTH held nh)
    if(NOT nh EQUAL 1)
        message(FATAL_ERROR "--tbar renders one frame, ${name} gave ${nh}")
    endif()
    file(SHA256 "${WORK_DIR}/key_${name}/0001.ppm" sha_${name})
endforeach()
if(NOT sha_l0 STREQUAL sha_card OR NOT sha_l257 STREQUAL sha_plain OR sha_card STREQUAL sha_plain)
    message(FATAL_ERROR "key at L 0 / 257 is not the key source / Main alone")
endif()

# 6. --stride writes frame 0001 and every Nth after it; --after frames are always written.
execute_process(
    COMMAND "${VT_RENDER}" "${fade}" --out "${WORK_DIR}/stride" --stride 1000 --after 1
    RESULT_VARIABLE rc OUTPUT_VARIABLE so ERROR_VARIABLE se)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "vt_render --stride failed (${rc}): ${se}")
endif()
string(REGEX MATCH "rendered ([0-9]+) frames" _ "${so}")
file(GLOB strided "${WORK_DIR}/stride/*.ppm")
list(LENGTH strided ns)
if(NOT ns EQUAL 2 OR NOT EXISTS "${WORK_DIR}/stride/0001.ppm" OR NOT CMAKE_MATCH_1 GREATER 2)
    message(FATAL_ERROR "--stride 1000 --after 1: expected 0001 and the end frame of ${CMAKE_MATCH_1}, got ${ns} files")
endif()
