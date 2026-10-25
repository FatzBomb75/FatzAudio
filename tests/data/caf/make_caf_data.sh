#!/bin/sh
# Regenerates the Core Audio Format test files of test_demux.c with the ffmpeg CLI.
# The two ALAC files carry the samples of ../alac/alac16.flac and alac51_24.flac, so
# the built-in ALAC decoder must match FLAC to the bit; the others are sines.
set -e
cd "$(dirname "$0")"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
ff -i ../alac/alac16.flac -c:a alac -map_metadata -1 -fflags +bitexact alac16.caf
ff -i ../alac/alac51_24.flac -c:a alac -sample_fmt s32p -bits_per_raw_sample 24 \
   -map_metadata -1 -fflags +bitexact alac51_24.caf
s440="aevalsrc=0.5*sin(2*PI*440*t):s=8000:d=0.75"
ff -f lavfi -i "$s440" -c:a pcm_s16be -map_metadata -1 -fflags +bitexact pcm16be.caf
ff -f lavfi -i "$s440" -c:a pcm_s24le -map_metadata -1 -fflags +bitexact pcm24le.caf
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t):s=22050:d=1" -c:a libmp3lame -b:a 32k \
   -map_metadata -1 -fflags +bitexact mp3.caf
# ffmpeg writes the packet table's valid-frame count as packets x frames per packet,
# with no remainder; Apple's tools write the real count. Do the same for the ALAC files
# (44541 and 19200 frames), so the length and the last packet are exact.
python3 - <<'PY'
import struct
for name, frames in (("alac16.caf", 44541), ("alac51_24.caf", 19200)):
    d = bytearray(open(name, "rb").read())
    p = 8
    while p + 12 <= len(d):
        size = struct.unpack(">q", d[p + 4:p + 12])[0]
        if d[p:p + 4] == b"pakt":
            n, valid, prime, rem = struct.unpack(">qqii", d[p + 12:p + 36])
            d[p + 12:p + 36] = struct.pack(">qqii", n, frames, 0, n * 4096 - frames)
        p += 12 + size
    open(name, "wb").write(d)
PY
ls -l *.caf
