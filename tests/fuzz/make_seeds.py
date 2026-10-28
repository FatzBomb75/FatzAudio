#!/usr/bin/env python3
"""Writes the small ZIP and HLS seeds for the fuzz targets into seeds/. The audio
seeds are the files in tests/data (run.sh passes those directories)."""
import os, struct, zipfile

here = os.path.dirname(os.path.abspath(__file__))
data = os.path.join(here, "..", "data")
for d in ("zip", "hls"):
    os.makedirs(os.path.join(here, "seeds", d), exist_ok=True)

def zipseed(name, entries, compress):
    with zipfile.ZipFile(os.path.join(here, "seeds", "zip", name), "w", compress) as z:
        for n, b in entries:
            z.writestr(n, b)

mp3 = open(os.path.join(data, "mp3", "cbr.mp3"), "rb").read()[:6000]
zipseed("stored.zip", [("a.txt", b"hello"), ("song.mp3", mp3)], zipfile.ZIP_STORED)
zipseed("deflate.zip", [("a.txt", b"hello hello hello hello" * 20), ("song.mp3", mp3)], zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(os.path.join(here, "seeds", "zip", "zip64.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    with z.open("big.bin", "w", force_zip64=True) as f:
        f.write(b"abc" * 500)

hls = {
 "master.m3u8": "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\nlow.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=256000,CODECS=\"mp4a.40.5\"\nhigh/a.m3u8\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",DEFAULT=YES,URI=\"aud.m3u8\"\n",
 "media.m3u8": "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:7\n#EXTINF:6.0,\ns1.aac\n#EXT-X-DISCONTINUITY\n#EXTINF:5.5,\ns2.aac\n#EXT-X-ENDLIST\n",
 "fmp4.m3u8": "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:4\n#EXT-X-MAP:URI=\"init.mp4\",BYTERANGE=\"720@0\"\n#EXTINF:4.0,\n#EXT-X-BYTERANGE:1000@720\nseg.m4s\n#EXTINF:4.0,\n#EXT-X-BYTERANGE:1000\nseg.m4s\n#EXT-X-ENDLIST\n",
 "key.m3u8": "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\",IV=0x000102030405060708090a0b0c0d0e0f\n#EXTINF:6,\ns1.ts\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:6,\ns2.ts\n#EXT-X-ENDLIST\n",
}
for n, t in hls.items():
    open(os.path.join(here, "seeds", "hls", n), "w").write(t)

# MPEG-TS: PAT, PMT (one ADTS AAC stream on PID 0x101) and audio packets, after a NUL.
def ts(pid, payload, start, cc):
    hdr = bytes([0x47, (0x40 if start else 0) | (pid >> 8), pid & 255, 0x10 | (cc & 15)])
    return hdr + payload + b"\xff" * (184 - len(payload))
pat = b"\x00\x00\xb0\x0d\x00\x01\xc1\x00\x00\x00\x01\xf0\x00" + b"\x00" * 4
pmt = b"\x00\x02\xb0\x12\x00\x01\xc1\x00\x00\xe1\x01\xf0\x00\x0f\xe1\x01\xf0\x00" + b"\x00" * 4
adts = open(os.path.join(data, "net", "tone.aac"), "rb").read()[:2000]
pes = b"\x00\x00\x01\xc0\x00\x00\x80\x00\x00" + adts
stream = ts(0, b"\x00" + pat, True, 0) + ts(0x100, b"\x00" + pmt, True, 0)
for i in range(0, len(pes), 184):
    stream += ts(0x101, pes[i:i + 184], i == 0, i // 184)
open(os.path.join(here, "seeds", "hls", "ts.bin"), "wb").write(hls["media.m3u8"].encode() + b"\x00" + stream)
