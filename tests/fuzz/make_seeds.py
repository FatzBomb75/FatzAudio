#!/usr/bin/env python3
"""Writes the small ZIP, HLS, WAV, MIDI, tracker and tag seeds for the fuzz targets into
seeds/. The other audio seeds are the files in tests/data (run.sh passes those)."""
import os, struct, zipfile

here = os.path.dirname(os.path.abspath(__file__))
data = os.path.join(here, "..", "data")
for d in ("zip", "hls"):
    os.makedirs(os.path.join(here, "seeds", d), exist_ok=True)

def zinfo(n, compress):   # a fixed date, so the committed seeds don't change on every run
    zi = zipfile.ZipInfo(n, date_time=(2026, 1, 1, 0, 0, 0))
    zi.compress_type = compress
    return zi
def zipseed(name, entries, compress):
    with zipfile.ZipFile(os.path.join(here, "seeds", "zip", name), "w", compress) as z:
        for n, b in entries:
            z.writestr(zinfo(n, compress), b)

mp3 = open(os.path.join(data, "mp3", "cbr.mp3"), "rb").read()[:6000]
zipseed("stored.zip", [("a.txt", b"hello"), ("song.mp3", mp3)], zipfile.ZIP_STORED)
zipseed("deflate.zip", [("a.txt", b"hello hello hello hello" * 20), ("song.mp3", mp3)], zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(os.path.join(here, "seeds", "zip", "zip64.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    with z.open(zinfo("big.bin", zipfile.ZIP_DEFLATED), "w", force_zip64=True) as f:
        f.write(b"abc" * 500)

# Songs inside ZIPs, for the readers behind them (tags.c read_zip, stream.c open_zip_entry): an ID3-tagged MP3 and a FLAC
# with tags and a cue sheet, stored and deflated, and a MIDI file with a text file in front.
tagsd = os.path.join(data, "tags")
tagged = open(os.path.join(tagsd, "id3v24.mp3"), "rb").read()[:20000]
flac = open(os.path.join(tagsd, "tags.flac"), "rb").read()[:20000]
zipseed("song_tags.zip", [("readme.txt", b"karaoke"), ("tagged.mp3", tagged), ("tagged.flac", flac)], zipfile.ZIP_STORED)
zipseed("song_tags_deflate.zip", [("tagged.mp3", tagged), ("tagged.flac", flac)], zipfile.ZIP_DEFLATED)

def shared_range_zip(k, data):
    # One local entry, k central entries pointing at it (final audit H2: entries after the first are left out)
    import zlib
    crc = zlib.crc32(data) & 0xffffffff
    c = zlib.compressobj(9, zlib.DEFLATED, -15); comp = c.compress(data) + c.flush()
    local = struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 0, 8, 0, 0, crc, len(comp), len(data), 8, 0) + b"song.mp3" + comp
    cd = b""
    for i in range(k):
        nm = b"s%05d.mp3" % i
        cd += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 20, 20, 0, 8, 0, 0, crc, len(comp), len(data), len(nm), 0, 0, 0, 0, 0, 0) + nm
    return local + cd + struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, k, k, len(cd), len(local), 0)
open(os.path.join(here, "seeds", "zip", "shared_range.zip"), "wb").write(
    shared_range_zip(8, b"ID3\x04\x00\x00\x7f\x7f\x7f\x7f" + bytes(1 << 20)))

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
# Effect and tuning SysEx: GS EFX (40 03 ..) cycling through the insertion types with their parameters and the
# part switch (40 4x 22), XG insertion (03 0n ..) and variation (02 01 40 ..), GS and XG drum setups, GS and XG
# scale tuning, the universal master tuning and volume. Each type change is followed by notes, so it renders.
def gs_dt(addr, data):   # Roland DT1 (device 10, model 42): the checksum is not checked, so it is left 0
    return sysex(bytes([0x41, 0x10, 0x42, 0x12]) + bytes(addr) + bytes(data) + b"\x00\xf7")
def xg_pc(addr, data):
    return sysex(bytes([0x43, 0x10, 0x4C]) + bytes(addr) + bytes(data) + b"\xf7")
def note_pair(ch, k, d=24): return [(0, bytes([0x90 | ch, k, 100])), (d, bytes([0x80 | ch, k, 0]))]
gs_types = [(1, 0), (1, 1), (1, 2), (1, 3), (1, 0x10), (1, 0x11), (1, 0x20), (1, 0x21), (1, 0x22), (1, 0x23), (1, 0x24), (1, 0x25),
            (1, 0x26), (1, 0x30), (1, 0x31), (1, 0x40), (1, 0x41), (1, 0x42), (1, 0x43), (1, 0x44), (1, 0x50), (1, 0x51),
            (1, 0x52), (1, 0x53), (1, 0x54), (1, 0x55), (1, 0x56), (1, 0x57), (1, 0x60), (1, 0x61), (1, 0x70), (1, 0x71),
            (1, 0x72), (1, 0x73), (2, 0), (2, 3), (2, 0x0C), (4, 0), (4, 3), (5, 0), (0x11, 0), (0x11, 3), (0x11, 8)]
efx = [(0, gs_reset), (0, b"\xb0\x5b\x40"), (0, gs_dt((0x40, 0x03, 0x17), (100, 80, 60))),   # sends to reverb, chorus, delay
       (0, gs_dt((0x40, 0x40, 0x22), (1,))), (0, gs_dt((0x40, 0x41, 0x22), (1,)))]
for i, (m, l) in enumerate(gs_types):
    efx += [(0, gs_dt((0x40, 0x03, 0x00), (m, l))), (0, gs_dt((0x40, 0x03, 0x03), tuple((i * 7 + j * 5) & 0x7F for j in range(20))))]
    efx += note_pair(0, 60 + i % 12) + note_pair(1, 48 + i % 12, 12)
efx += [(0, gs_dt((0x40, 0x03, 0x1B), (3, 90, 4, 80))), (0, gs_dt((0x40, 0x40, 0x22), (0,))), (0, gs_dt((0x40, 0x41, 0x22), (0,)))] + note_pair(0, 64)
xg_ins = [(0, xg_on), (0, b"\xb0\x5b\x40")]
xg_msbs = [(0x49, 1), (0x4E, 1), (0x4E, 2), (0x50, 0), (0x50, 1), (0x51, 0), (0x52, 0), (0x52, 1), (0x52, 8), (0x53, 0), (0x54, 0),
           (0x55, 0), (0x56, 0), (0x57, 0), (0x58, 0), (0x5D, 0), (0x5E, 0), (0x5F, 0), (0x5F, 1), (0x60, 0), (0x61, 1), (0x41, 0), (0x42, 0)]
for i, (m, l) in enumerate(xg_msbs):
    k = i & 1
    xg_ins += [(0, xg_pc((3, k, 0), (m, l))), (0, xg_pc((3, k, 2), tuple((i * 11 + j * 3) & 0x7F for j in range(10)))),
               (0, xg_pc((3, k, 0x0C), (0,))), (0, xg_pc((3, k, 0x0D), (90, 64, 32, 100, 50))),
               (0, xg_pc((3, k, 0x30), tuple((i + j) & 0x7F for j in range(20))))]
    xg_ins += note_pair(0, 60 + i % 12) + note_pair(1, 40 + i % 12, 12)
xg_var = [(0, xg_on)]
for i, (m, l) in enumerate([(5, 0), (6, 0), (7, 0), (0x10, 0), (0x11, 0), (0x12, 0), (0x13, 0), (0x14, 0), (0x15, 0), (0x16, 0), (0x17, 0),
                            (0x41, 0), (0x42, 0), (0x44, 0), (0x45, 0), (0x49, 1), (0x50, 0), (0x5E, 0)]):
    xg_var += [(0, xg_pc((2, 1, 0x40), (m, l))), (0, xg_pc((2, 1, 0x42), tuple((i * 5 + j * 9) & 0x7F for j in range(16)))),
               (0, xg_pc((2, 1, 0x5A), (1, 64, 100))), (0, xg_pc((2, 1, 0x70), (0, 64, 64, 64, 64, 64)))]
    xg_var += note_pair(0, 62 + i % 9) + note_pair(2, 55 + i % 9, 12)
xg_var += [(0, xg_pc((2, 1, 0x03), (2, 1))), (0, xg_pc((2, 1, 0x0C), (90,))), (0, xg_pc((2, 1, 0x20), (0x41, 0))), (0, xg_pc((2, 1, 0x2C), (80,)))] + \
          note_pair(0, 64)
tune = [(0, gs_reset), (0, sysex(bytes.fromhex("7f 7f 04 03 00 50 f7"))), (0, sysex(bytes.fromhex("7f 7f 04 04 00 46 f7"))),
        (0, sysex(bytes.fromhex("7f 7f 04 01 00 60 f7"))),
        (0, gs_dt((0x40, 0x10, 0x40), tuple(40 + 3 * j for j in range(12)))),                    # GS scale tuning, part 1
        (0, xg_pc((0x08, 0x00, 0x41), tuple(50 + 2 * j for j in range(12)))),                     # XG scale tuning, part 1
        (0, gs_dt((0x41, 0x10, 0x24), (60, 70, 80, 90))), (0, gs_dt((0x41, 0x00, 0x24), (64,))),  # GS drum maps (pitch, level, ...)
        (0, xg_pc((0x30, 0x24, 0x00), (70, 20, 64, 100, 60, 40))), (0, xg_pc((0x31, 0x26, 0x03), (50, 90, 30))),   # XG drum setups
        (0, xg_pc((0x00, 0x00, 0x7D), (0,))),                                                      # drum setup reset
        (0, b"\xb0\x65\x00"), (0, b"\xb0\x64\x02"), (0, b"\xb0\x06\x41"), (0, b"\xb0\x26\x00")]
tune_notes = []
for k in (60, 64, 67, 71):
    tune_notes += note_pair(0, k, 48)
drums2 = [(0, b"\x99\x24\x7f"), (24, b"\x89\x24\x00"), (0, b"\x99\x26\x7f"), (24, b"\x89\x26\x00"), (0, b"\x99\x2a\x60"), (24, b"\x89\x2a\x00")]
seeds["efx_gs.mid"] = smf(1, [efx, melody])
seeds["efx_xg_insertion.mid"] = smf(1, [xg_ins, melody], div=480)
seeds["efx_xg_variation.mid"] = smf(1, [xg_var, melody], div=480)
seeds["tuning_drums.mid"] = smf(1, [tune + tune_notes + drums2 + [(0, xg_on)] + drums2, melody])
seeds["song.rmi"] = b"RIFF" + struct.pack("<I", 4 + 8 + len(seeds["fmt0.mid"])) + b"RMID" + b"data" + \
    struct.pack("<I", len(seeds["fmt0.mid"])) + seeds["fmt0.mid"]
for n, b in seeds.items():
    open(os.path.join(mw, n), "wb").write(b)
zipseed("song_midi.zip", [("a.txt", b"hello"), ("song.mid", seeds["efx_gs.mid"])], zipfile.ZIP_DEFLATED)

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

# Tags: ID3v2 frames stored with the zlib flag (v2.3 and v2.4), Lyrics3 v1.00 and v2.00 at the end of an
# MP3 stub. (The MIDI seeds, the FLAC cue sheet and SYLT files are passed by run.sh from elsewhere.)
import zlib
tg = os.path.join(here, "seeds", "tags")
os.makedirs(tg, exist_ok=True)
def synchsafe(v): return bytes([(v >> 21) & 127, (v >> 14) & 127, (v >> 7) & 127, v & 127])
def id3(ver, frames): return b"ID3" + bytes([ver, 0, 0]) + synchsafe(len(frames)) + frames
stub = b"\xff\xfb\x90\x00" + b"\x00" * 400
z = zlib.compress(b"\x00Compressed Title")
f3 = b"TIT2" + struct.pack(">I", 4 + len(z)) + b"\x00\x80" + struct.pack(">I", 17) + z
z = zlib.compress(b"\x03Zlib Artist")
f4 = b"TPE1" + synchsafe(4 + len(z)) + b"\x00\x09" + synchsafe(12) + z
open(os.path.join(tg, "id3v23_zlib.mp3"), "wb").write(id3(3, f3) + stub)
open(os.path.join(tg, "id3v24_zlib.mp3"), "wb").write(id3(4, f4 + f4) + stub)
def l3(fid, d): return fid + b"%05d" % len(d) + d
body = l3(b"IND", b"10") + l3(b"LYR", b"Line one\r\nLine two") + l3(b"ETT", b"Title") + l3(b"EAR", b"Artist")
v1 = b"TAG" + b"T".ljust(30, b"\0") + b"\0" * 95
open(os.path.join(tg, "lyrics3_v2.mp3"), "wb").write(stub + b"LYRICSBEGIN" + body + b"%06d" % (len(body) + 11) + b"LYRICS200" + v1)
open(os.path.join(tg, "lyrics3_v1.mp3"), "wb").write(stub + b"LYRICSBEGIN" + b"Just the words" + b"LYRICSEND")

# Soundfonts: SF2 with several presets, zones, key/velocity ranges, modulators, looped and
# stereo samples and a drum kit; SF3 with a Vorbis sample (ffmpeg once, committed); DLS Level 1
# (art1) and Level 2 (rgn2, art2, an 8-bit and a stereo wave). Built here, as test_midi.c does.
def ck(cid, body):
    return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
def lst(t, body): return ck(b"LIST", t + body)
def name20(s): return s.encode().ljust(20, b"\0")[:20]
def sine16(n, period, amp=20000):
    return b"".join(struct.pack("<h", int(round(math.sin(2 * math.pi * i / period) * amp))) for i in range(n))
def sf2(samples, smpl, ifil=(2, 1), sf3=False, sm24=None):
    phdr = name20("Piano") + struct.pack("<HHHIII", 0, 0, 0, 0, 0, 0) + \
           name20("Strings") + struct.pack("<HHHIII", 48, 0, 1, 0, 0, 0) + \
           name20("Kit") + struct.pack("<HHHIII", 0, 128, 2, 0, 0, 0) + \
           name20("EOP") + struct.pack("<HHHIII", 0, 0, 3, 0, 0, 0)
    pbag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 2, 0) + struct.pack("<HH", 3, 1) + struct.pack("<HH", 4, 1)
    # preset modulator: CC1 (linear, unipolar) -> vibrato LFO pitch (gen 6), 50 cents
    pmod = struct.pack("<HHhHH", 0x0081, 6, 50, 0, 0) + b"\0" * 10
    pgen = struct.pack("<HH", 43, 0x7F00) + struct.pack("<HH", 41, 0) + struct.pack("<HH", 41, 1) + \
           struct.pack("<HH", 41, 2) + struct.pack("<HH", 0, 0)
    inst = name20("Sine") + struct.pack("<H", 0) + name20("Pad") + struct.pack("<H", 2) + \
           name20("Drum") + struct.pack("<H", 4) + name20("EOI") + struct.pack("<H", 5)
    ibag = b"".join(struct.pack("<HH", g, m) for g, m in ((0, 0), (6, 0), (10, 1), (13, 1), (19, 2), (22, 2)))
    # instrument modulators: velocity (concave, negative) -> filter cutoff; pressure -> volume
    imod = struct.pack("<HHhHH", 0x0502, 8, -2400, 0, 0) + struct.pack("<HHhHH", 0x000D, 48, 100, 0, 0) + b"\0" * 10
    g = lambda o, v: struct.pack("<Hh", o, v)
    gu = lambda o, v: struct.pack("<HH", o, v)
    igen = gu(43, 0x3C00) + g(34, -1200) + g(38, -3600) + g(8, 9000) + gu(54, 1) + g(53, 0) + \
           gu(43, 0x7F3D) + g(9, 200) + gu(54, 3) + g(53, 1) + \
           g(26, -2000) + g(17, 250) + gu(53, 2) + \
           gu(43, 0x2423) + gu(44, 0x7F40) + g(5, 50) + g(22, -500) + g(57, 1) + g(53, 0) + \
           g(51, -12) + g(52, 30) + g(53, 0) + g(0, 0)   # the terminal record
    shdr = b"".join(name20(n) + struct.pack("<IIIIIBbHH", *v) for n, v in samples) + name20("EOS") + b"\0" * 26
    pdta = ck(b"phdr", phdr) + ck(b"pbag", pbag) + ck(b"pmod", pmod) + ck(b"pgen", pgen) + ck(b"inst", inst) + \
           ck(b"ibag", ibag) + ck(b"imod", imod) + ck(b"igen", igen) + ck(b"shdr", shdr)
    info = ck(b"ifil", struct.pack("<HH", *ifil)) + ck(b"INAM", b"Seed bank\0") + ck(b"isng", b"EMU8000\0")
    return ck(b"RIFF", b"sfbk" + lst(b"INFO", info) + lst(b"sdta", ck(b"smpl", smpl) + (ck(b"sm24", sm24) if sm24 is not None else b"")) + lst(b"pdta", pdta))
sfw = os.path.join(here, "seeds", "soundfont")
os.makedirs(sfw, exist_ok=True)
pcm = sine16(441, 44.1) + b"\0" * 92 + sine16(882, 88.2, 12000) + b"\0" * 92
def pcm_samples(base):
    a, b = base, base + 441 + 46
    return [("Sine", (a, a + 441, a, a + 441, 44100, 69, 0, 0, 1)),
            ("L", (b, b + 882, b + 10, b + 872, 44100, 57, -5, 2, 4)),   # linked to R
            ("R", (b, b + 882, b + 10, b + 872, 44100, 57, 5, 1, 2))]    # linked to L
open(os.path.join(sfw, "bank.sf2"), "wb").write(sf2(pcm_samples(0), pcm))
# 24-bit: ifil 2.04 and an sm24 chunk (the low bytes, one per sample, padded to even); and the same bank with sm24 one byte
# short and with a version too old for it (both ignored by the reader).
nsmp = len(pcm) // 2
low = bytes((i * 37) & 0xFF for i in range(nsmp))
open(os.path.join(sfw, "bank_sm24.sf2"), "wb").write(sf2(pcm_samples(0), pcm, ifil=(2, 4), sm24=low + b"\0" * (nsmp & 1)))
open(os.path.join(sfw, "bank_sm24_short.sf2"), "wb").write(sf2(pcm_samples(0), pcm, ifil=(2, 4), sm24=low[:-3]))
open(os.path.join(sfw, "bank_sm24_old.sf2"), "wb").write(sf2(pcm_samples(0), pcm, ifil=(2, 1), sm24=low + b"\0" * (nsmp & 1)))

# Stacks: one preset whose global preset zone and two zones (overlapping key ranges, velocity layers) lead to instruments
# with a global zone, a linked stereo pair and a second layer, so the zone merge, the generator limits and the
# modulator lists run through every level.
def sf2_stack():
    g = lambda o, v: struct.pack("<Hh", o, v)
    gu = lambda o, v: struct.pack("<HH", o, v)
    pbag_gens = [[g(48, 60)], [gu(43, 0x3F00), gu(41, 0)], [gu(43, 0x7F20), gu(44, 0x7F40), g(33, -1200), gu(41, 1)]]
    ibag_gens = [[g(8, 9000), g(34, -1500)], [gu(54, 1), g(53, 0)], [gu(43, 0x7F00), gu(44, 0x3F00), g(53, 1), g(51, -12)],
                 [gu(44, 0x7F40), g(53, 2), g(52, 25), g(17, 250)]]
    def bags(zones):
        out, at = b"", 0
        for z in zones:
            out += struct.pack("<HH", at, 0)
            at += len(z)
        return out + struct.pack("<HH", at, 0)
    phdr = name20("Stack") + struct.pack("<HHHIII", 0, 0, 0, 0, 0, 0) + name20("EOP") + struct.pack("<HHHIII", 0, 1, 3, 0, 0, 0)
    inst = name20("I0") + struct.pack("<H", 0) + name20("I1") + struct.pack("<H", 2) + name20("EOI") + struct.pack("<H", 4)
    shdr = b"".join(name20(n) + struct.pack("<IIIIIBbHH", *v) for n, v in pcm_samples(0)) + name20("EOS") + b"\0" * 26
    pdta = ck(b"phdr", phdr) + ck(b"pbag", bags(pbag_gens)) + ck(b"pmod", b"\0" * 10) + ck(b"pgen", b"".join(sum(pbag_gens, [])) + g(0, 0)) + \
           ck(b"inst", inst) + ck(b"ibag", bags(ibag_gens)) + ck(b"imod", b"\0" * 10) + ck(b"igen", b"".join(sum(ibag_gens, [])) + g(0, 0)) + \
           ck(b"shdr", shdr)
    info = ck(b"ifil", struct.pack("<HH", 2, 4)) + ck(b"INAM", b"Stack\0\0")
    return ck(b"RIFF", b"sfbk" + lst(b"INFO", info) + lst(b"sdta", ck(b"smpl", pcm) + ck(b"sm24", low + b"\0" * (nsmp & 1))) + lst(b"pdta", pdta))
open(os.path.join(sfw, "stack.sf2"), "wb").write(sf2_stack())

ogg = os.path.join(sfw, "sample.ogg.tmp")
sf3_out = os.path.join(sfw, "bank.sf3")
if not os.path.exists(sf3_out) and shutil.which("ffmpeg"):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "sine=f=440:r=22050:d=0.05", "-c:a", "libvorbis",
                    "-f", "ogg", "-fflags", "+bitexact", ogg], check=True)
    o = open(ogg, "rb").read()
    os.remove(ogg)
    pad = b"\0" if len(o) & 1 else b""
    at = (len(o) + len(pad)) // 2
    smp = [("Ogg", (0, len(o), 0, 500, 22050, 69, 0, 0, 0x11))] + pcm_samples(at)[1:]
    open(sf3_out, "wb").write(sf2(smp, o + pad + pcm, ifil=(3, 1)))

def dls_wave(ch, bits, rate, n, period, loop=True):
    fmt = struct.pack("<HHIIHH", 1, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits)
    if bits == 8:
        d = bytes((128 + int(round(math.sin(2 * math.pi * i / period) * 120))) & 0xFF for i in range(n * ch))
    else:
        d = sine16(n * ch, period)
    wsmp = struct.pack("<IHhiII", 20, 69, 0, 0, 0, 1 if loop else 0) + (struct.pack("<IIII", 16, 0, 0, n) if loop else b"")
    return lst(b"wave", ck(b"fmt ", fmt) + ck(b"wsmp", wsmp) + ck(b"data", d))
def dls_art(level2):
    conns = [(0, 0, 0x209, 0, -3600 * 65536), (0, 0, 0x20A, 0, 1000 * 65536), (0x2, 0, 0x206, 0, -300 * 65536),
             (0x81, 0, 0x3, 0, 50 * 65536)]
    if level2:
        conns += [(0, 0, 0x500, 0, 6000 * 65536), (0, 0, 0x501, 0, 10 * 65536), (0x0, 0, 0x30C, 0, 500 * 65536)]
    body = struct.pack("<II", 8, len(conns)) + b"".join(struct.pack("<HHHHi", *c) for c in conns)
    return lst(b"lar2" if level2 else b"lart", ck(b"art2" if level2 else b"art1", body))
def dls_rgn(lo, hi, cue, level2, ch=1, vel=(0, 127), cdl=None):
    body = ck(b"rgnh", struct.pack("<HHHHHH", lo, hi, vel[0], vel[1], 0, 0)) + \
           ck(b"wlnk", struct.pack("<HHII", 0, 0, ch, cue))
    if level2: body += dls_art(True)
    if cdl is not None: body += ck(b"cdl ", cdl)
    return lst(b"rgn2" if level2 else b"rgn ", body)
def dls_ins(bank, prog, nm, rgns, art):
    body = ck(b"insh", struct.pack("<III", len(rgns), bank, prog)) + lst(b"lrgn", b"".join(rgns))
    if art: body += art
    return lst(b"ins ", body + lst(b"INFO", ck(b"INAM", nm + b"\0")))
def dls(level2):
    waves = [dls_wave(1, 16, 44100, 441, 44.1), dls_wave(1, 8, 22050, 300, 50.0), dls_wave(2, 16, 44100, 400, 100.0, False)]
    offs, pool = [], b""
    for w in waves:
        offs.append(len(pool))
        pool += w
    ins = dls_ins(0, 0, b"Sine", [dls_rgn(0, 127, 0, level2)], dls_art(level2)) + \
          dls_ins(0, 5, b"Split", [dls_rgn(0, 63, 1, level2), dls_rgn(64, 127, 2, level2, 3, (0, 90))], None) + \
          dls_ins(0x80000000, 0, b"Kit", [dls_rgn(35, 40, 0, level2)], None)
    body = b"DLS " + ck(b"colh", struct.pack("<I", 3)) + lst(b"lins", ins) + \
           ck(b"ptbl", struct.pack("<II", 8, len(offs)) + b"".join(struct.pack("<I", o) for o in offs)) + \
           lst(b"wvpl", pool) + lst(b"INFO", ck(b"INAM", b"Seed DLS\0"))
    return ck(b"RIFF", body)
# DLS 2 conditional chunks ('cdl '): true and false constants, a query of "supports DLS 2" compared to 1, a query of an
# unknown GUID, sample memory above a constant, a stack underflow and an operator that is not one (read as true).
def cdl_const(v): return struct.pack("<HI", 15, v)
guid_dls2 = struct.pack("<IHH", 0xf14599e5, 0x4689, 0x11d2) + bytes([0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6])
guid_mem = struct.pack("<IHH", 0x178f2f28, 0xc364, 0x11d1) + bytes([0xa7, 0x60, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12])
cdls = [cdl_const(1), cdl_const(0), struct.pack("<H", 16) + guid_dls2 + cdl_const(1) + struct.pack("<H", 13),
        struct.pack("<H", 17) + b"\x01" * 16, struct.pack("<H", 16) + guid_mem + cdl_const(1 << 20) + struct.pack("<H", 12),
        struct.pack("<H", 1), struct.pack("<H", 99) + cdl_const(1), cdl_const(5) + cdl_const(0) + struct.pack("<H", 6) + struct.pack("<H", 14),
        cdl_const(1) + struct.pack("<H", 14) + cdl_const(7)]
def dls_cdl():
    waves = [dls_wave(1, 16, 44100, 441, 44.1)]
    ins = b""
    for i, c in enumerate(cdls):
        r = [dls_rgn(0, 63, 0, True, cdl=c), dls_rgn(64, 127, 0, True, cdl=cdls[(i + 1) % len(cdls)])]
        body = ck(b"insh", struct.pack("<III", len(r), 0, i)) + lst(b"lrgn", b"".join(r)) + dls_art(True) + ck(b"cdl ", c)
        ins += lst(b"ins ", body + lst(b"INFO", ck(b"INAM", b"Cond%d\0" % i)))
    body = b"DLS " + ck(b"colh", struct.pack("<I", len(cdls))) + lst(b"lins", ins) + \
           ck(b"ptbl", struct.pack("<II", 8, 1) + struct.pack("<I", 0)) + lst(b"wvpl", waves[0])
    return ck(b"RIFF", body)
open(os.path.join(sfw, "cdl.dls"), "wb").write(dls_cdl())
open(os.path.join(sfw, "level1.dls"), "wb").write(dls(False))
open(os.path.join(sfw, "level2.dls"), "wb").write(dls(True))

# CD+G: a .cdg song (colour tables, memory and border presets, normal and XOR tiles,
# scrolls with offsets, transparency) of 1500 packs, so the disc path of the target
# decides on it too; the first 4 bytes are the surface options (see fuzz_cdg.c).
def cdg_pack(instr, d):
    return bytes([9, instr, 0, 0]) + bytes(d).ljust(16, b"\0") + b"\0" * 4
def cdg_song():
    pk = []
    lo = [(0, 0, 0), (15, 0, 0), (0, 15, 0), (0, 0, 15), (15, 15, 15), (8, 8, 8), (15, 15, 0), (0, 15, 15)]
    ct = lambda c: b"".join(bytes([(r << 2) | (g >> 2), ((g & 3) << 4) | b]) for r, g, b in c)
    pk += [cdg_pack(30, ct(lo)), cdg_pack(31, ct(reversed(lo))), cdg_pack(1, [0, 0]), cdg_pack(2, [5])]
    for row in range(18):
        for col in range(0, 50, 3):
            bits = [(0x3F >> (i % 3)) & 0x3F for i in range(12)]
            pk.append(cdg_pack(6 if (row + col) % 2 else 38, [4, (row + col) % 16, row, col] + bits))
    pk += [cdg_pack(28, [i * 4 for i in range(16)]), cdg_pack(20, [3, 0x21, 0x15]), cdg_pack(24, [0, 0x13, 0x22]),
           cdg_pack(1, [9, 1]), cdg_pack(2, [1])]
    for k in range(40):
        pk.append(cdg_pack(6, [1, 2, k % 18, (k * 7) % 50] + [0x2A] * 12))
    while len(pk) < 1500:
        pk.append(b"\0" * 24 if len(pk) % 5 else cdg_pack(20 + 4 * (len(pk) % 2), [len(pk) % 16, 0x10 | len(pk) % 6, 0x20 | len(pk) % 12]))
    return b"".join(pk)
cw = os.path.join(here, "seeds", "cdg")
os.makedirs(cw, exist_ok=True)
song = cdg_song()
for n, h in {"classic.bin": b"\0\0\0\0", "indexed_into.bin": bytes([0x02, 0x82, 0, 0]),
             "scaled_smooth7.bin": bytes([0x0B, 0x84 | (2 << 3), 29, 4]),   # 29 % 11 = 7, bit 4 edge blend
             "scaled_fx.bin": bytes([0x30, 0xA4 | (1 << 3), 0xC5, 0x3A]), "sized.bin": bytes([0x48, 0x48, 40, 70])}.items():
    open(os.path.join(cw, n), "wb").write(h + song)

# Disc images: a .cue sheet, a NUL and the image (written as fuzz_cue.bin/.wav/.sub, see
# fuzz_cue.c). Tracks with pregaps, indexes, flags and CD-Text lines; a WAVE image; a
# MOTOROLA (big-endian) one; one whose image is CD+G subcode; and one with no sheet, whose
# bytes are a TOC and CD-Text for the simulated drive.
def cdtext_packs(entries):
    out = b""
    for typ, block, text in entries:
        track = 0
        for off in range(0, len(text), 12):
            part = text[off:off + 12].ljust(12, b"\0")
            q = bytes([typ, track, len(out) // 18, block << 4]) + part
            crc = 0
            for byte in q:
                crc ^= byte << 8
                for _ in range(8):
                    crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
            out += q + struct.pack(">H", ~crc & 0xFFFF)
            track += text[off:off + 12].count(b"\0")
    return out
qw = os.path.join(here, "seeds", "cue")
os.makedirs(qw, exist_ok=True)
sectors = b"".join(sine16(1176, 40 + s, 9000) for s in range(20))      # 20 sectors of stereo frames
cues = {
 "three_tracks.cue": ("REM GENRE \"Karaoke\"\r\nCATALOG 0123456789012\r\nPERFORMER \"Seed\"\r\nTITLE \"Caf\xc3\xa9\"\r\n"
                      "FILE \"fuzz_cue.bin\" BINARY\r\n  TRACK 01 AUDIO\r\n    TITLE \"One\"\r\n    ISRC USABC2600001\r\n"
                      "    INDEX 01 00:00:00\r\n  TRACK 02 AUDIO\r\n    SONGWRITER \"Who\"\r\n    INDEX 00 00:00:05\r\n"
                      "    INDEX 01 00:00:08\r\n  TRACK 03 AUDIO\r\n    FLAGS DCP PRE\r\n    PREGAP 00:00:02\r\n"
                      "    INDEX 01 00:00:12\r\nREM SESSION 02\r\nFILE \"fuzz_cue.bin\" BINARY\r\n  TRACK 04 MODE1/2352\r\n"
                      "    INDEX 01 00:00:00\r\n", sectors),
 "wave.cue": ("FILE \"fuzz_cue.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:00:09\n",
              b"RIFF" + struct.pack("<I", 36 + len(sectors)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 44100, 176400, 4, 16) +
              b"data" + struct.pack("<I", len(sectors)) + sectors),
 "motorola.cue": ("FILE \"fuzz_cue.bin\" MOTOROLA\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    POSTGAP 00:00:03\n"
                  "    INDEX 01 00:00:10\n", sectors),
 "cdg_sub.cue": ("FILE \"fuzz_cue.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n", song[:96 * 300]),
}
toc = bytes([0, 0, 1, 3])
for i, lba in enumerate((0, 6, 13, 20)):
    toc += bytes([0, 0x10 if i < 3 else 0x14, i + 1 if i < 3 else 0xAA, 0]) + struct.pack(">I", lba)
toc = struct.pack(">H", len(toc) - 2) + toc[2:]
ct = cdtext_packs([(0x8F, 0, b"\0"), (0x80, 0, b"Album\0One\0\x09\0Three\0"), (0x81, 0, b"Artist\0\0Solo\0"),
                   (0x8E, 0, b"0123456789012\0USABC2600001\0")])
cues["mmc_drive.cue"] = ("", toc + struct.pack(">H", len(ct) + 2) + b"\0\0" + ct + sectors[:40000])
for n, (sheet, img) in cues.items():
    open(os.path.join(qw, n), "wb").write(sheet.encode("latin-1") + b"\0" + img)

# HLS target: playlists as .pls / .m3u radio lists too (the target reads the same text both ways).
open(os.path.join(here, "seeds", "hls", "list.pls"), "w").write(
    "[playlist]\nNumberOfEntries=2\nFile1= http://radio.example:8000/live.mp3 \nTitle1=One\nFile2=../b.aac\n")
open(os.path.join(here, "seeds", "hls", "list.m3u"), "w").write(
    "#EXTM3U\n#EXTINF:-1,Station\n\n  /streams/main.mp3  \nhttps://u:p@h.example/x\n")

# Net target (fuzz_net.c): the first byte is the mode. 0: URL NUL headers NUL URL (Set-Cookie lines and their dates);
# 1: a whole server reply (status line, headers, body); 2: a file served with byte ranges; 3: an HLS playlist, a NUL,
# then the segment bytes.
nw = os.path.join(here, "seeds", "net")
os.makedirs(nw, exist_ok=True)
def nseed(name, mode, payload): open(os.path.join(nw, name), "wb").write(bytes([mode]) + payload)
nseed("cookies.bin", 0, b"https://www.example.com/a/b/c?x=1\0Set-Cookie: sid=abc; Path=/a; Domain=example.com; Secure; HttpOnly; "
      b"Expires=Wed, 21 Oct 2037 07:28:00 GMT\r\nset-cookie: t=1; Max-Age=3600; Path=/; SameSite=Lax\r\n"
      b"Set-Cookie: old=1; Expires=Thu, 01 Jan 1970 00:00:01 GMT\r\nSet-Cookie: =x\r\nSet-Cookie: n=v; Domain=.com\r\n"
      b"Set-Cookie: d=1; Expires=Sunday, 06-Nov-94 08:49:37 GMT\r\nSet-Cookie: e=2; Expires=Sun Nov  6 08:49:37 1994\r\n"
      b"Content-Length: 5\r\nLocation: ../up/x.mp3\r\n\0http://sub.example.com/a/b/d")
nseed("playlist.bin", 0, b"http://h.example/dir/list.pls\0[playlist]\r\nFile1=http://s.example/live\r\n\0../x.m3u8")
reply_mp3 = open(os.path.join(data, "net", "tone_cbr.mp3"), "rb").read()[:20000]
chunks = b"".join(b"%x\r\n" % len(reply_mp3[i:i + 4000]) + reply_mp3[i:i + 4000] + b"\r\n" for i in range(0, len(reply_mp3), 4000)) + b"0\r\n\r\n"
nseed("reply_chunked.bin", 1, b"HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nTransfer-Encoding: chunked\r\nSet-Cookie: s=1; Path=/\r\n\r\n" + chunks)
nseed("reply_length.bin", 1, b"HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: %d\r\n\r\n" % len(reply_mp3) + reply_mp3)
nseed("reply_unframed.bin", 1, b"HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n" + reply_mp3)
nseed("reply_redirect.bin", 1, b"HTTP/1.1 302 Found\r\nLocation: /next/../y.mp3\r\nSet-Cookie: r=1\r\nContent-Length: 4\r\n\r\nmove")
nseed("reply_206.bin", 1, b"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-%d/%d\r\nContent-Length: %d\r\n\r\n" %
      (len(reply_mp3) - 1, len(reply_mp3) * 2, len(reply_mp3)) + reply_mp3)
meta = b"StreamTitle='Artist - Song';StreamUrl='http://x';"
meta += b"\0" * (-len(meta) % 16)
icy = b""
for i in range(0, 8000, 2000): icy += reply_mp3[i:i + 2000] + (bytes([len(meta) // 16]) + meta if i == 2000 else b"\0")
nseed("reply_icy.bin", 1, b"ICY 200 OK\r\nicy-name:Seed\r\nicy-metaint:2000\r\nicy-br:128\r\nContent-Type: audio/mpeg\r\n\r\n" + icy)
nseed("file_frag_m4a.bin", 2, open(os.path.join(data, "net", "frag.m4a"), "rb").read())
nseed("file_frag_mp4.bin", 2, open(os.path.join(data, "frag.mp4"), "rb").read())
nseed("file_mkv.bin", 2, open(os.path.join(data, "av_tracks.mkv"), "rb").read())
nseed("file_webm.bin", 2, open(os.path.join(data, "opus.webm"), "rb").read())
nseed("file_mp3.bin", 2, reply_mp3)
nseed("hls_aac.bin", 3, b"#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:3\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:1.5,\n#EXT-X-BYTERANGE:6000@0\ns.aac\n"
      b"#EXTINF:1.5,\n#EXT-X-BYTERANGE:6000\ns.aac\n#EXT-X-ENDLIST\n\0" + open(os.path.join(data, "net", "tone.aac"), "rb").read()[:12000])
nseed("hls_ts.bin", 3, b"#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:3.0,\ns1.ts\n#EXT-X-ENDLIST\n\0" +
      open(os.path.join(here, "seeds", "hls", "ts.bin"), "rb").read().split(b"\0", 1)[-1])

# Sound target (fuzz_sound.c): a script length byte, the script (op | arg << 3), the sound's file.
sdw = os.path.join(here, "seeds", "sound")
os.makedirs(sdw, exist_ok=True)
def sdseed(name, script, filebytes): open(os.path.join(sdw, name), "wb").write(bytes([len(script)]) + bytes(script) + filebytes)
play = lambda vol, pan: 0 | (vol << 3)
sdseed("wav_voices.bin", [play(16, 0), play(8, 31), 5 | 3 << 3, play(16, 16), play(16, 16), 3, 2 | 0 << 3, play(16, 0), 4, 3, 5 | 7 << 3, 6, 1,
                          play(4, 0), 7, 5 | 1 << 3], open(os.path.join(here, "seeds", "wav", "s16.wav"), "rb").read())
sdseed("midi_steal.bin", [2 | 1 << 3, play(16, 0), play(16, 0), play(16, 0), 2 | 2 << 3, 5 | 2 << 3, 2 | 4 << 3, play(10, 0), 3, 4, 4, 1],
       seeds["fmt0.mid"])
sdseed("pcm_raw.bin", [play(16, 0), 5 | 5 << 3, play(16, 0), 6, play(16, 0), 1, 7], bytes(range(256)) * 8)
