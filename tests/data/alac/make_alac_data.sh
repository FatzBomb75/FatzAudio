#!/bin/sh
# Regenerates the ALAC test files of test_demux.c with the ffmpeg CLI: each ALAC
# file has a FLAC copy of the same samples (both lossless), so the built-in ALAC
# decoder must match the FLAC decoder to the bit.
set -e
cd "$(dirname "$0")"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
# 5.1 at 24 bits, 0.4 s (the last frame a partial one): a sine, loud noise, silence
# then a sine, a low tone with faint noise, very faint noise, a sweep.
e='0.5*sin(2*PI*500*t)|0.9*(2*random(0)-1)|if(lt(t\,0.2)\,0\,0.3*sin(2*PI*900*t))'
e="$e"'|0.25*sin(2*PI*80*t)+0.001*(2*random(1)-1)|0.0001*(2*random(2)-1)|0.4*sin(2*PI*(200+2000*t)*t)'
ff -f lavfi -i "aevalsrc=$e:s=48000:d=0.4:c=5.1" -c:a alac -sample_fmt s32p -bits_per_raw_sample 24 \
   -map_metadata -1 -fflags +bitexact alac51_24.m4a
ff -i alac51_24.m4a -c:a flac -map_metadata -1 -fflags +bitexact alac51_24.flac
# 16-bit stereo in Matroska: a sine with noise, and silence then a sine.
ff -f lavfi -i 'aevalsrc=0.5*sin(2*PI*440*t)+0.2*(2*random(0)-1)|if(lt(t\,0.3)\,0\,0.25*sin(2*PI*660*t)):s=44100:d=1.01' \
   -c:a alac -sample_fmt s16p -map_metadata -1 -fflags +bitexact alac16.mkv
ff -i alac16.mkv -c:a flac -map_metadata -1 -fflags +bitexact alac16.flac
ls -l alac51_24.m4a alac51_24.flac alac16.mkv alac16.flac
