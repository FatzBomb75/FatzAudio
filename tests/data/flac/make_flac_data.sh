#!/bin/sh
# Regenerates the FLAC test files of test_flac.c with the ffmpeg CLI: the same
# FLAC stream as a plain .flac (the reference), in Ogg, Matroska and MP4.
set -e
cd "$(dirname "$0")"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
# Stereo, 1 s at 44100 Hz: 440 Hz left, 1000 Hz right; 16-bit, block size 1152.
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t)|0.25*sin(2*PI*1000*t):s=44100:d=1" -c:a flac -sample_fmt s16 \
   -frame_size 1152 -metadata ARTIST="Artist F" -metadata TITLE="Lossless" \
   -metadata REPLAYGAIN_TRACK_GAIN="-7.25 dB" -fflags +bitexact tone.flac
ff -i tone.flac -c copy -fflags +bitexact tone.oga
ff -i tone.flac -c copy -fflags +bitexact tone_flac.mkv
ff -i tone.flac -c copy -fflags +bitexact -strict -2 tone_flac.mp4
# 5.1, 24-bit, 48000 Hz, 0.2 s: channel n is (n+1)*100 Hz at 0.1*(n+1).
six='0.1*sin(2*PI*100*t)|0.2*sin(2*PI*200*t)|0.3*sin(2*PI*300*t)|0.4*sin(2*PI*400*t)|0.5*sin(2*PI*500*t)|0.6*sin(2*PI*600*t)'
ff -f lavfi -i "aevalsrc=$six:s=48000:d=0.2:c=5.1" -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 \
   -map_metadata -1 -fflags +bitexact surround.flac
ff -i surround.flac -c copy -fflags +bitexact surround_flac.mkv
ff -i surround.flac -c copy -fflags +bitexact surround.oga
