#!/bin/sh
# Regenerates the container test files of test_demux.c with the ffmpeg CLI.
# Every audio track is a sine of its own frequency and level, so a test can tell
# which track it decoded.
set -e
cd "$(dirname "$0")"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
video="testsrc=size=64x64:rate=1:duration=2"
t440="aevalsrc=0.5*sin(2*PI*440*t):s=22050:d=2"
t1000="aevalsrc=0.25*sin(2*PI*1000*t)|0.25*sin(2*PI*1000*t):s=16000:d=2"
t2000="aevalsrc=0.4*sin(2*PI*2000*t):s=24000:d=2"

# Video + MP3 (track 0) + MP3 stereo (track 1) + AAC (track 2), moov in front.
ff -f lavfi -i "$video" -f lavfi -i "$t440" -f lavfi -i "$t1000" -f lavfi -i "$t2000" \
   -map 0 -map 1 -map 2 -map 3 -c:v libx264 -preset veryslow -crf 51 -pix_fmt yuv420p \
   -c:a:0 libmp3lame -b:a:0 24k -c:a:1 libmp3lame -b:a:1 32k -c:a:2 aac -b:a:2 16k \
   -movflags +faststart -map_metadata -1 -fflags +bitexact av_tracks.mp4

# AAC-LC in M4A, moov after the media data.
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*1000*t)|0.5*sin(2*PI*1000*t):s=48000:d=1" \
   -c:a aac -b:a 32k -map_metadata -1 -fflags +bitexact aac.m4a

# QuickTime 24-bit little-endian PCM ('in24' with an 'enda' box).
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t):s=8000:d=0.75" -c:a pcm_s24le \
   -map_metadata -1 -fflags +bitexact pcm.mov

# Fragmented MP4 with MP3 audio.
ff -f lavfi -i "$t440" -c:a libmp3lame -b:a 24k -movflags frag_keyframe+empty_moov \
   -frag_duration 500000 -map_metadata -1 -fflags +bitexact frag.mp4

# Matroska: video + 16-bit PCM (track 0) + MP3 (track 1).
ff -f lavfi -i "$video" -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t):s=8000:d=1.25" -f lavfi -i "$t1000" \
   -map 0 -map 1 -map 2 -c:v libx264 -preset veryslow -crf 51 -pix_fmt yuv420p \
   -c:a:0 pcm_s16le -c:a:1 libmp3lame -b:a:1 32k -map_metadata -1 -fflags +bitexact av_tracks.mkv

# WebM with Opus.
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*1000*t)|0.5*sin(2*PI*1000*t):s=48000:d=2" \
   -c:a libopus -b:a 16k -map_metadata -1 -fflags +bitexact opus.webm

# ALAC in M4A: lossless, so every decoder gives back the 16-bit sines exactly.
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t)|0.25*sin(2*PI*660*t):s=44100:d=0.25" \
   -c:a alac -sample_fmt s16p -map_metadata -1 -fflags +bitexact alac.m4a

# AAC-LC 5.1: a frequency per channel (L 500, R 700, C 900, LFE 80, Ls 1300,
# Rs 1700 Hz), to check every decoder's channel order against the library's.
ff -f lavfi -i "aevalsrc=0.3*sin(2*PI*500*t)|0.3*sin(2*PI*700*t)|0.3*sin(2*PI*900*t)|0.3*sin(2*PI*80*t)|0.3*sin(2*PI*1300*t)|0.3*sin(2*PI*1700*t):s=48000:d=0.5:c=5.1" \
   -c:a aac -b:a 192k -map_metadata -1 -fflags +bitexact aac51.m4a
ls -l av_tracks.mp4 aac.m4a pcm.mov frag.mp4 av_tracks.mkv opus.webm alac.m4a aac51.m4a
