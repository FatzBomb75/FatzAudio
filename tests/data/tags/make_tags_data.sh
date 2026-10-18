#!/bin/sh
# Regenerates the tagged files of test_tags.c with the ffmpeg CLI: the same tags
# written by FFmpeg's own muxers into every container, plus small cover images.
# Each file is 0.5 s of a 440 Hz sine, 44.1 kHz stereo (Opus: 48 kHz, AIFF: 8 kHz mono).
set -e
cd "$(dirname "$0")"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
src="aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*440*t):s=44100:d=0.5"
ff -f lavfi -i "color=c=red:s=16x16:d=1" -frames:v 1 cover.jpg
ff -f lavfi -i "color=c=blue:s=16x16:d=1" -frames:v 1 cover.png
set -- -metadata "title=Song Title" -metadata "artist=The Artist" -metadata "album=Great Album" \
    -metadata "album_artist=Various" -metadata "genre=Rock" -metadata "date=2024" -metadata "track=3/12" \
    -metadata "disc=1/2" -metadata "composer=Composer Name" -metadata "copyright=(c) Me" -fflags +bitexact

# ID3v2.3 with a non-ASCII title (UTF-16) and a JPEG cover; ID3v2.4 (UTF-8).
ff -f lavfi -i "$src" -i cover.jpg -map 0 -map 1 -c:a libmp3lame -b:a 128k -id3v2_version 3 \
   -c:v copy -disposition:v attached_pic "$@" -metadata "title=Sóng Tïtle" -metadata:s:v comment="Cover (front)" id3v23.mp3
ff -f lavfi -i "$src" -c:a libmp3lame -b:a 128k -id3v2_version 4 "$@" id3v24.mp3
# FLAC with a PNG picture block.
ff -f lavfi -i "$src" -i cover.png -map 0 -map 1 -c:a flac -c:v copy -disposition:v attached_pic "$@" tags.flac
# MP4 (iTunes atoms) with a JPEG cover.
ff -f lavfi -i "$src" -i cover.jpg -map 0 -map 1 -c:a aac -b:a 96k -c:v copy -disposition:v attached_pic "$@" tags.m4a
# Ogg Opus.
ff -f lavfi -i "$src" -c:a libopus -b:a 64k "$@" tags.opus
# AIFF with an ID3 chunk (8 kHz mono, to keep it small).
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t):s=8000:d=0.5" -write_id3v2 1 "$@" tags.aiff
# Matroska with a cover attachment.
ff -f lavfi -i "$src" -c:a libopus -b:a 64k "$@" -attach cover.jpg -metadata:s:t mimetype=image/jpeg \
   -metadata:s:t filename=cover.jpg tags.mka
