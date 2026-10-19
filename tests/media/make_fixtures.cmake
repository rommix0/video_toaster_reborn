# Generates the media test fixtures with ffmpeg.exe (run as a CTest fixture setup).
# Inputs: -DFFMPEG=<ffmpeg.exe> -DOUT_DIR=<directory>. Existing files are kept.
file(MAKE_DIRECTORY "${OUT_DIR}")

function(make_fixture name)
    if(EXISTS "${OUT_DIR}/${name}")
        return()
    endif()
    execute_process(COMMAND "${FFMPEG}" -y -v error ${ARGN} "${OUT_DIR}/${name}.tmp.mkv"
        RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        file(REMOVE "${OUT_DIR}/${name}.tmp.mkv")
        message(FATAL_ERROR "ffmpeg failed (${rc}) making ${name}:\n${err}")
    endif()
    file(RENAME "${OUT_DIR}/${name}.tmp.mkv" "${OUT_DIR}/${name}")
endfunction()

# Luma carries the frame index: Y = 16 + 5 * (N mod 40). Commas are avoided in the geq
# expressions, so the filter strings need no escaping.
set(LUMA "format=yuv420p,geq=lum=16+N*5-200*floor(N/40):cb=128:cr=128")

make_fixture(f30.mkv
    -f lavfi -i "color=c=black:s=640x480:r=30000/1001:d=5,${LUMA}"
    -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=5"
    -c:v ffv1 -c:a pcm_s16le -ac 1)

make_fixture(f25_169.mkv
    -f lavfi -i "color=c=black:s=1280x720:r=25:d=3,${LUMA}"
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=3"
    -af aformat=channel_layouts=5.1
    -c:v ffv1 -c:a pcm_s16le)

make_fixture(f60_sar.mkv
    -f lavfi -i "color=c=black:s=704x480:r=60000/1001:d=2,format=rgb24,geq=r=X-256*floor(X/256):g=Y-256*floor(Y/256):b=64*floor(X/256)+16*floor(Y/256),setsar=10/11"
    -c:v ffv1 -pix_fmt bgr0)
