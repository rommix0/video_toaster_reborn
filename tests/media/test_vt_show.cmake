# CTest script test for vt_show. Run as:
#   cmake -DVT_SHOW=<exe> -DFFPROBE=<ffprobe.exe> -DVT_DATA_ROOT=<dir> -DFIXTURES=<dir>
#         -DWORK_DIR=<scratch> -P test_vt_show.cmake
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# 1. A scripted show: 30 fps 4:3 and 25 fps 16:9 files, Page Peel ! at fast from t = 1 s, 3 s
#    recorded losslessly.
set(show "${WORK_DIR}/show.mkv")
execute_process(
    COMMAND "${VT_SHOW}"
        --in1 "file:${FIXTURES}/f30.mkv" --in2 "file:${FIXTURES}/f25_169.mkv"
        --effect "${VT_DATA_ROOT}/4.2/Effects/4000FX/PagePeel/Page Peel !"
        --speed fast --auto-at 1.0 --seconds 3 --format ffv1 --out "${show}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE so ERROR_VARIABLE se)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "vt_show failed (${rc}): ${so}${se}")
endif()
if(NOT EXISTS "${show}")
    message(FATAL_ERROR "vt_show wrote no file")
endif()

execute_process(
    COMMAND "${FFPROBE}" -v error -count_frames -show_streams -of json "${show}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE se)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "ffprobe failed (${rc}): ${se}")
endif()

string(JSON nStreams LENGTH "${json}" streams)
set(haveVideo FALSE)
set(haveAudio FALSE)
math(EXPR last "${nStreams} - 1")
foreach(i RANGE ${last})
    string(JSON type GET "${json}" streams ${i} codec_type)
    if(type STREQUAL "video")
        set(haveVideo TRUE)
        string(JSON w GET "${json}" streams ${i} width)
        string(JSON h GET "${json}" streams ${i} height)
        string(JSON frames GET "${json}" streams ${i} nb_read_frames)
        string(JSON sar GET "${json}" streams ${i} sample_aspect_ratio)
        string(JSON rate GET "${json}" streams ${i} avg_frame_rate)
        if(NOT w EQUAL 720 OR NOT h EQUAL 480)
            message(FATAL_ERROR "video is ${w}x${h}, expected 720x480")
        endif()
        # round(3 * 60000 / 1001) = 180
        if(NOT frames EQUAL 180)
            message(FATAL_ERROR "video has ${frames} frames, expected 180")
        endif()
        if(NOT sar STREQUAL "10:11")
            message(FATAL_ERROR "sample aspect ratio ${sar}, expected 10:11")
        endif()
        # Matroska stores 1 ms timestamps, so the muxer reports 19001/317 (59.94006) rather than
        # 60000/1001 (Ruling 9): accept anything within 0.01 fps. Fixed point, 5 decimals.
        if(NOT rate MATCHES "^([0-9]+)/([0-9]+)$")
            message(FATAL_ERROR "avg_frame_rate '${rate}' is not a fraction")
        endif()
        math(EXPR fixed "${CMAKE_MATCH_1} * 100000 / ${CMAKE_MATCH_2}")
        math(EXPR diff "${fixed} - 60000 * 100000 / 1001")
        if(diff LESS -1000 OR diff GREATER 1000)
            message(FATAL_ERROR "avg_frame_rate ${rate} is not within 0.01 fps of 60000/1001")
        endif()
    elseif(type STREQUAL "audio")
        set(haveAudio TRUE)
        string(JSON sr GET "${json}" streams ${i} sample_rate)
        string(JSON ch GET "${json}" streams ${i} channels)
        if(NOT sr EQUAL 48000 OR NOT ch EQUAL 2)
            message(FATAL_ERROR "audio is ${sr} Hz ${ch} ch, expected 48000 Hz stereo")
        endif()
    endif()
endforeach()
if(NOT haveVideo OR NOT haveAudio)
    message(FATAL_ERROR "the show needs a video and an audio stream")
endif()

# 2. Listing devices works whatever is plugged in.
execute_process(COMMAND "${VT_SHOW}" --list-devices
    RESULT_VARIABLE rc OUTPUT_VARIABLE so ERROR_VARIABLE se)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "vt_show --list-devices failed (${rc}): ${so}${se}")
endif()

# 3. Bad arguments fail with a message.
foreach(bad "--in1;bogus:x;--out;${WORK_DIR}/bad.mkv"
            "--in1;file:${FIXTURES}/f30.mkv;--effect;${VT_DATA_ROOT}/no/such/effect;--out;${WORK_DIR}/bad.mkv")
    execute_process(COMMAND "${VT_SHOW}" ${bad}
        RESULT_VARIABLE rc OUTPUT_VARIABLE so ERROR_VARIABLE se)
    if(rc EQUAL 0)
        message(FATAL_ERROR "vt_show accepted: ${bad}")
    endif()
    if("${so}${se}" STREQUAL "")
        message(FATAL_ERROR "vt_show gave no reason for: ${bad}")
    endif()
endforeach()
