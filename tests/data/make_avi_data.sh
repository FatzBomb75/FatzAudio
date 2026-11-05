#!/bin/sh
# Regenerates the AVI test files of test_demux.c (needs ffmpeg and python3). All are sines at
# known frequencies and levels: 440 Hz at 0.5 (left) and 660 Hz at 0.3 (right). The MPEG and
# AC-3 files are 2 s at 44.1 kHz next to a small video track; the PCM ones 0.75 s at 8 kHz.
# make_avi_variants.py makes the files ffmpeg does not: OpenDML, no index, absolute offsets.
set -e
cd "$(dirname "$0")/avi"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
vid="-f lavfi -i color=c=blue:s=64x48:r=10:d=2"
sine="aevalsrc=0.5*sin(2*PI*440*t)|0.3*sin(2*PI*660*t):s=44100:d=2"
s8k="aevalsrc=0.5*sin(2*PI*440*t)|0.3*sin(2*PI*660*t):s=8000:d=0.75"
for c in mp3cbr:"libmp3lame -b:a 128k" mp3vbr:"libmp3lame -q:a 5" mp2:"mp2 -b:a 128k" ac3:"ac3 -b:a 96k" aac:"aac -b:a 128k"; do
    ff $vid -f lavfi -i "$sine" -c:v mpeg4 -c:a ${c#*:} -fflags +bitexact -f avi "${c%%:*}.avi"
done
ff $vid -f lavfi -i "$s8k" -c:v mpeg4 -c:a pcm_s16le -fflags +bitexact -f avi pcm16.avi
ff -f lavfi -i "$s8k" -c:a pcm_u8 -fflags +bitexact -f avi pcm_u8.avi
ff -f lavfi -i "$s8k" -c:a pcm_mulaw -fflags +bitexact -f avi ulaw.avi
# Two audio tracks: MP3 then PCM.
ff $vid -f lavfi -i "$sine" -f lavfi -i "$s8k" -map 0 -map 1 -map 2 -c:v mpeg4 -c:a:0 libmp3lame -b:a:0 128k -c:a:1 pcm_s16le \
    -fflags +bitexact -f avi two_audio.avi
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t):s=8000:d=0.25" -c:a pcm_s16le -metadata title="AVI Title" -metadata artist="AVI Artist" \
    -fflags +bitexact -f avi tagged.avi
python3 -I ../make_avi_variants.py
ls -l
