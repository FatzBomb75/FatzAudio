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

# WAV family (miniaudio's dr_wav and the AIFF reader): PCM 8/16/24/32, float, A-law, mu-law,
# MS and IMA ADPCM, WAVE_FORMAT_EXTENSIBLE, Wave64, RF64, AIFF and AIFF-C. ffmpeg writes them
# once; the files are committed, so this only runs when one is missing.
import shutil, subprocess
wav = os.path.join(here, "seeds", "wav")
os.makedirs(wav, exist_ok=True)
wavs = {
 "u8.wav": ["-c:a", "pcm_u8"], "s16.wav": ["-c:a", "pcm_s16le"], "s24.wav": ["-c:a", "pcm_s24le"],
 "s32.wav": ["-c:a", "pcm_s32le"], "f32.wav": ["-c:a", "pcm_f32le"], "f64.wav": ["-c:a", "pcm_f64le"],
 "alaw.wav": ["-c:a", "pcm_alaw"], "mulaw.wav": ["-c:a", "pcm_mulaw"], "msadpcm.wav": ["-c:a", "adpcm_ms"],
 "imaadpcm.wav": ["-c:a", "adpcm_ima_wav"], "ext51.wav": ["-ac", "6", "-c:a", "pcm_s16le"],
 "s16.w64": ["-c:a", "pcm_s16le"], "s16_rf64.wav": ["-rf64", "always", "-c:a", "pcm_s16le"],
 "s16.aiff": ["-c:a", "pcm_s16be"], "s24.aiff": ["-c:a", "pcm_s24be"], "f32.aifc": ["-f", "aiff", "-c:a", "pcm_f32be"],
}
for n, args in wavs.items():
    out = os.path.join(wav, n)
    if os.path.exists(out) or not shutil.which("ffmpeg"):
        continue
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "sine=f=440:r=8000:d=0.1",
                    "-ac", "2"] + args + ["-metadata", "title=Seed", out], check=True)

# MIDI: format 0 and 1, RMID, karaoke .kar words, GS/XG SysEx, RPNs, pitch bend, drums,
# running status, tempo changes and text events.
def vlq(v):
    b = [v & 0x7F]
    v >>= 7
    while v:
        b.insert(0, 0x80 | (v & 0x7F))
        v >>= 7
    return bytes(b)
def trk(events):
    body = b"".join(vlq(d) + e for d, e in events) + b"\x00\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(body)) + body
def smf(fmt, tracks, div=96):
    return b"MThd" + struct.pack(">IHHH", 6, fmt, len(tracks), div) + b"".join(trk(t) for t in tracks)
def meta(t, b): return b"\xff" + bytes([t]) + vlq(len(b)) + b
def sysex(b): return b"\xf0" + vlq(len(b)) + b
gs_reset = sysex(bytes.fromhex("41 10 42 12 40 00 7f 00 41 f7"))
gs_rev = sysex(bytes.fromhex("41 10 42 12 40 01 30 04 0b f7"))
xg_on = sysex(bytes.fromhex("43 10 4c 00 00 7e 00 f7"))
xg_rev = sysex(bytes.fromhex("43 10 4c 02 01 00 01 10 6c f7"))
notes = []
for i, k in enumerate((60, 64, 67, 72)):
    notes += [(0 if i == 0 else 24, bytes([0x90, k, 100])), (48, bytes([k, 0]))]   # running status note off
melody = [(0, meta(0x51, b"\x07\xa1\x20")), (0, meta(0x58, b"\x04\x02\x18\x08")), (0, meta(0x03, b"Seed")),
          (0, b"\xc0\x00"), (0, b"\xb0\x07\x64"), (0, b"\xb0\x5b\x40"), (0, b"\xb0\x5d\x20"),
          (0, b"\xb0\x65\x00"), (0, b"\xb0\x64\x00"), (0, b"\xb0\x06\x0c"), (0, b"\xe0\x00\x50")] + notes + \
         [(0, meta(0x51, b"\x03\xd0\x90")), (0, b"\x90\x48\x50"), (96, b"\x80\x48\x00")]
drums = [(0, b"\x99\x24\x7f"), (24, b"\x89\x24\x00"), (0, b"\x99\x26\x7f"), (24, b"\x89\x26\x00")]
mw = os.path.join(here, "seeds", "midi")
os.makedirs(mw, exist_ok=True)
seeds = {
 "fmt0.mid": smf(0, [melody + drums]),
 "fmt1_gs.mid": smf(1, [[(0, gs_reset), (0, gs_rev)], melody, drums]),
 "fmt1_xg.mid": smf(1, [[(0, xg_on), (0, xg_rev)], melody], div=480),
 "smpte.mid": smf(0, [melody], div=0xE728),
 "song.kar": smf(1, [[(0, meta(0x01, b"@KMIDI KARAOKE FILE")), (0, meta(0x01, b"@TSeed"))],
                     [(0, meta(0x01, b"@I")), (0, meta(0x01, b"\\Hel")), (24, meta(0x01, b"lo ")),
                      (24, meta(0x01, b"/world")), (24, meta(0x05, b"la"))], melody]),
}
seeds["song.rmi"] = b"RIFF" + struct.pack("<I", 4 + 8 + len(seeds["fmt0.mid"])) + b"RMID" + b"data" + \
    struct.pack("<I", len(seeds["fmt0.mid"])) + seeds["fmt0.mid"]
for n, b in seeds.items():
    open(os.path.join(mw, n), "wb").write(b)

# Tracker: a 4-channel ProTracker module (M.K.) and an 8-channel one (8CHN) with a looped
# sine, a few effects and an order jump. libopenmpt is fuzzed upstream; this covers our glue.
import math
def mod(tag, nch):
    m = b"Seed Module".ljust(20, b"\0")
    slen = 64
    for i in range(31):
        m += (b"Sine" if i == 0 else b"").ljust(22, b"\0")
        m += struct.pack(">HBBHH", slen // 2 if i == 0 else 0, 0, 64 if i == 0 else 0, 0, slen // 2 if i == 0 else 1)
    m += bytes([2, 127, 0, 1] + [0] * 126) + tag
    def cell(smp, period, fx, arg): return bytes([(smp & 0xF0) | (period >> 8), period & 0xFF, ((smp & 15) << 4) | fx, arg])
    for p in range(2):
        rows = [[b"\0\0\0\0"] * nch for _ in range(64)]
        rows[0][0] = cell(1, 428, 0xF, 4)
        rows[4][1] = cell(1, 214, 0x4, 0x44)
        rows[8][0] = cell(0, 0, 0xA, 0x02)
        rows[12][nch - 1] = cell(1, 856, 0xE, 0x91)
        rows[16][0] = cell(0, 0, 0xB if p else 0xD, 0)
        m += b"".join(b"".join(r) for r in rows)
    return m + bytes((int(round(math.sin(2 * math.pi * i / slen) * 100)) & 0xFF) for i in range(slen))
tw = os.path.join(here, "seeds", "tracker")
os.makedirs(tw, exist_ok=True)
open(os.path.join(tw, "mk.mod"), "wb").write(mod(b"M.K.", 4))
open(os.path.join(tw, "8chn.mod"), "wb").write(mod(b"8CHN", 8))
