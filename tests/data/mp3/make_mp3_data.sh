#!/bin/sh
# Regenerates the MP3 files used by test_demux.c (needs ffmpeg with libmp3lame).
# Each is 4 s of a 440 Hz tone at -9 dBFS plus quiet noise (so the encoder uses
# its bit reservoir), 176400 frames at 44.1 kHz (88200 at 22.05 kHz).
# cbr.mp3:    128 kbit/s CBR with the LAME header and an ID3v2 tag
# vbr.mp3:    VBR -V6 with the LAME header
# noxing.mp3: 64 kbit/s CBR without a Xing/LAME header
# mpeg2.mp3:  MPEG-2, 22.05 kHz mono, 24 kbit/s, LAME header (one granule per frame)
set -e
cd "$(dirname "$0")"
src="sine=frequency=440:sample_rate=44100:duration=4,volume=0.3536[a];anoisesrc=color=pink:sample_rate=44100:duration=4:amplitude=0.02:seed=7[b];[a][b]amix=inputs=2:normalize=0"
ffmpeg -y -filter_complex "$src" -ac 2 -c:a libmp3lame -b:a 128k -metadata title=cbr -fflags +bitexact cbr.mp3
ffmpeg -y -filter_complex "$src" -ac 2 -c:a libmp3lame -q:a 6 -map_metadata -1 -id3v2_version 0 -fflags +bitexact vbr.mp3
ffmpeg -y -filter_complex "$src" -ac 2 -c:a libmp3lame -b:a 64k -write_xing 0 -id3v2_version 0 -fflags +bitexact noxing.mp3
ffmpeg -y -filter_complex "$src" -ac 1 -ar 22050 -c:a libmp3lame -b:a 24k -id3v2_version 0 -fflags +bitexact mpeg2.mp3
