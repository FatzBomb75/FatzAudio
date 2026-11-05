#!/bin/sh
# Regenerates the FLV test files of test_demux.c (needs ffmpeg). Sines at known frequencies and
# levels: 440 Hz at 0.5 (left) and 660 Hz at 0.3 (right); 2 s at 44.1 kHz next to a small video
# track, PCM 0.75 s at 11.025 kHz (the lowest rate FLV has that is near 8 kHz), Nellymoser mono.
set -e
cd "$(dirname "$0")/flv"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
vid="-f lavfi -i color=c=blue:s=64x48:r=10:d=2"
sine="aevalsrc=0.5*sin(2*PI*440*t)|0.3*sin(2*PI*660*t):s=44100:d=2"
ff $vid -f lavfi -i "$sine" -c:v flv1 -c:a libmp3lame -b:a 128k -ar 44100 -fflags +bitexact -f flv mp3.flv
ff $vid -f lavfi -i "$sine" -c:v flv1 -c:a aac -b:a 128k -fflags +bitexact -f flv aac.flv
ff -f lavfi -i "$sine" -c:a aac -b:a 128k -fflags +bitexact -f flv aac_audio_only.flv
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t)|0.3*sin(2*PI*660*t):s=11025:d=0.75" -c:a pcm_s16le -fflags +bitexact -f flv pcm16.flv
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t):s=16000:d=0.5" -c:a nellymoser -fflags +bitexact -f flv nelly.flv
ls -l
