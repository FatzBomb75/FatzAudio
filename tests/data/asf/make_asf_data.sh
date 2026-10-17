#!/bin/sh
# Regenerates the ASF / WMA test files of test_asf.c with the ffmpeg CLI. Every
# audio track is a sine of its own frequency and level, so a test can tell which
# track it decoded. FFmpeg has no WMA Pro, Lossless or Voice encoder: those codecs
# are not covered by files here.
set -e
cd "$(dirname "$0")"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

# WMA 2, 44.1 kHz stereo, 2 s at 440 Hz, small packets (many of them), with a title,
# an author and ReplayGain tags (Extended Content Description).
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*440*t):s=44100:d=2" \
   -c:a wmav2 -b:a 64k -packet_size 1000 \
   -metadata title="Test Tone" -metadata artist="FatzAudio" \
   -metadata REPLAYGAIN_TRACK_GAIN="-6.50 dB" -metadata REPLAYGAIN_TRACK_PEAK="0.5" \
   -fflags +bitexact tone.wma

# 16-bit PCM in ASF, 8 kHz mono, 1.25 s at 440 Hz. Media objects bigger than the
# packets (fragments). Decodes without FFmpeg: exact lengths and seeks.
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t):s=8000:d=1.25" -c:a pcm_s16le -packet_size 512 \
   -map_metadata -1 -fflags +bitexact pcm.asf

# Video + PCM (track 0, 440 Hz mono 8 kHz) + WMA 1 (track 1, 1000 Hz stereo 22.05 kHz)
# + WMA 2 (track 2, 2000 Hz mono 16 kHz). The video makes FFmpeg write a Simple Index.
ff -f lavfi -i "testsrc=size=64x64:rate=5:duration=2" \
   -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t):s=8000:d=2" \
   -f lavfi -i "aevalsrc=0.25*sin(2*PI*1000*t)|0.25*sin(2*PI*1000*t):s=22050:d=2" \
   -f lavfi -i "aevalsrc=0.4*sin(2*PI*2000*t):s=16000:d=2" \
   -map 0 -map 1 -map 2 -map 3 -c:v wmv2 -q:v 31 -g 5 \
   -c:a:0 pcm_s16le -c:a:1 wmav1 -b:a:1 32k -c:a:2 wmav2 -b:a:2 24k \
   -map_metadata -1 -fflags +bitexact av_tracks.asf
ls -l tone.wma pcm.asf av_tracks.asf
