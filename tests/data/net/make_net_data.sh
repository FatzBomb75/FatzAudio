#!/bin/sh
# Regenerates the MP3 and AAC files used by test_net.c (needs ffmpeg with libmp3lame).
# tone_cbr.mp3: CBR without a Xing header or ID3 tag (length estimated from the size).
# tone_xing.mp3: with the Xing/Info header and an ID3v2 tag (exact length).
set -e
cd "$(dirname "$0")"
ffmpeg -y -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=3" -ac 2 -c:a libmp3lame -b:a 128k \
    -write_xing 0 -id3v2_version 0 tone_cbr.mp3
ffmpeg -y -f lavfi -i "sine=frequency=330:sample_rate=44100:duration=3" -ac 2 -c:a libmp3lame -b:a 128k tone_xing.mp3
# tone.aac: AAC-LC in ADTS frames (needs only FFmpeg's own AAC encoder).
ffmpeg -y -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=3" -ac 2 -c:a aac -b:a 64k \
    -map_metadata -1 -fflags +bitexact -f adts tone.aac
# frag.m4a: AAC-LC in 6 fragmented-MP4 fragments of 0.5 s, with a sidx in front and
# an mfra at the end (test_net.c pads the fragments apart and switches the indexes off).
ffmpeg -y -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=3" -ac 2 -c:a aac -b:a 64k \
    -movflags +frag_keyframe+empty_moov+default_base_moof+global_sidx -frag_duration 500000 \
    -map_metadata -1 -fflags +bitexact frag.m4a
