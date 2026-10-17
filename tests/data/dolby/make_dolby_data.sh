#!/bin/sh
# Regenerates the Dolby test files of test_dolby.c: E-AC-3 streams from
# eac3_writer.c with what the ffmpeg CLI decodes them to (32-bit float WAV), and
# FFmpeg-encoded tones in MP4 and Matroska.
set -e
cd "$(dirname "$0")"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
cc -O2 -I../../../src -I../../../include -I../../../third_party/miniaudio -o eac3_writer \
   eac3_writer.c ../../../src/mdct.c ../../../src/fa_platform.c -lm -lpthread

# name seed layout frames
gen() {
    ./eac3_writer "$1.ec3" "$2" "$3" "$4"
    ff -f eac3 -i "$1.ec3" -c:a pcm_f32le "$1.wav"
}
gen mono 1 mono 8
gen stereo 3 stereo 3
gen 51 2 5.1 2
gen 71 1 7.1 2
gen 2_2 2 any 4
gen 3_1_lfe 3 any 4
gen 2_1_lfe 5 any 4
gen 3_0_lfe 7 any 4
gen 2_0_lfe 39 any 4
gen 1_0_lfe 4 any 4
# FFmpeg applies the stream's dynamic range by default (FA_DYNAMIC_RANGE_NORMAL);
# these are full range and heavy compression (NIGHT).
ff -drc_scale 0 -f eac3 -i stereo.ec3 -c:a pcm_f32le stereo_full.wav
ff -heavy_compr 1 -f eac3 -i stereo.ec3 -c:a pcm_f32le stereo_night.wav
rm eac3_writer

ff -i ../tone.ac3 -c:a copy -map_metadata -1 -fflags +bitexact tone_ac3.mp4
ff -i ../tone.eac3 -c:a copy -map_metadata -1 -fflags +bitexact tone_eac3.mkv
ls -l *.ec3 *.wav *.mp4 *.mkv
