#!/bin/sh
# Regenerates the Ogg test files of test_ogg.c and test_net.c with the ffmpeg CLI
# (libopus and libvorbis encoders). Every stream is a sine of its own frequency
# and level, so a test can tell which one it decoded.
set -e
cd "$(dirname "$0")"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
sine() { echo "aevalsrc=$1:s=$2:d=$3"; }

# Opus stereo, 2 s at 1000 Hz (pre-skip 312, end trimmed by the last granule).
ff -f lavfi -i "$(sine '0.5*sin(2*PI*1000*t)|0.5*sin(2*PI*1000*t)' 48000 2)" -c:a libopus -b:a 24k \
   -map_metadata -1 -fflags +bitexact tone.opus
# Vorbis mono, 1.5 s at 440 Hz, 44100 Hz.
ff -f lavfi -i "$(sine '0.5*sin(2*PI*440*t)' 44100 1.5)" -c:a libvorbis -q:a 0 \
   -map_metadata -1 -fflags +bitexact tone.ogg
# 5.1 in both codecs (mapping family 1): channel n is (n+1)*100 Hz at 0.1*(n+1).
six='0.1*sin(2*PI*100*t)|0.2*sin(2*PI*200*t)|0.3*sin(2*PI*300*t)|0.4*sin(2*PI*400*t)|0.5*sin(2*PI*500*t)|0.6*sin(2*PI*600*t)'
ff -f lavfi -i "aevalsrc=$six:s=48000:d=1:c=5.1" -c:a libopus -b:a 96k -map_metadata -1 -fflags +bitexact surround.opus
ff -f lavfi -i "aevalsrc=$six:s=48000:d=1:c=5.1" -c:a libvorbis -q:a 0 -map_metadata -1 -fflags +bitexact surround.ogg
# Two audio streams in one file: Opus 440 Hz (track 0), Vorbis 1000 Hz at 44100 Hz (track 1).
ff -f lavfi -i "$(sine '0.5*sin(2*PI*440*t)' 48000 1)" -f lavfi -i "$(sine '0.25*sin(2*PI*1000*t)' 44100 1)" \
   -map 0 -map 1 -c:a:0 libopus -b:a:0 16k -c:a:1 libvorbis -q:a:1 0 -map_metadata -1 -fflags +bitexact twotracks.ogg
# Chained: Vorbis 44100 Hz 440 Hz 1.5 s, Vorbis 22050 Hz 2000 Hz 1 s, Opus stereo 300 Hz 1 s,
# each link with its own serial number.
ff -f lavfi -i "$(sine '0.4*sin(2*PI*2000*t)' 22050 1)" -c:a libvorbis -q:a 0 -map_metadata -1 \
   -fflags +bitexact -serial_offset 200 link2.ogg
ff -f lavfi -i "$(sine '0.3*sin(2*PI*300*t)|0.3*sin(2*PI*300*t)' 48000 1)" -c:a libopus -b:a 24k -map_metadata -1 \
   -fflags +bitexact -serial_offset 300 link3.opus
ff -i tone.ogg -c copy -fflags +bitexact -serial_offset 100 link1.ogg
cat link1.ogg link2.ogg link3.opus > chained.ogg
rm link1.ogg link2.ogg link3.opus
# Web radio: two 1 s Opus songs with titles, chained (test_net.c loops them).
ff -f lavfi -i "$(sine '0.5*sin(2*PI*500*t)|0.5*sin(2*PI*500*t)' 48000 1)" -c:a libopus -b:a 16k \
   -metadata ARTIST="Artist A" -metadata TITLE="Song 1" -fflags +bitexact -serial_offset 11 song1.opus
ff -f lavfi -i "$(sine '0.5*sin(2*PI*800*t)|0.5*sin(2*PI*800*t)' 48000 1)" -c:a libopus -b:a 16k \
   -metadata ARTIST="Artist B" -metadata TITLE="Song 2" -fflags +bitexact -serial_offset 22 song2.opus
cat song1.opus song2.opus > radio.opus
rm song1.opus song2.opus
# Loudness tags in the comment headers.
ff -f lavfi -i "$(sine '0.5*sin(2*PI*1000*t)' 48000 0.2)" -c:a libopus -b:a 16k \
   -metadata R128_TRACK_GAIN=-1280 -fflags +bitexact r128.opus
ff -f lavfi -i "$(sine '0.5*sin(2*PI*1000*t)' 44100 0.2)" -c:a libvorbis -q:a 0 \
   -metadata REPLAYGAIN_TRACK_GAIN="-6.50 dB" -metadata REPLAYGAIN_TRACK_PEAK=0.5 -fflags +bitexact rg.ogg
ls -l *.opus *.ogg
