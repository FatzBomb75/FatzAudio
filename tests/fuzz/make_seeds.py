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
def sf2(samples, smpl, ifil=(2, 1), sf3=False):
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
    return ck(b"RIFF", b"sfbk" + lst(b"INFO", info) + lst(b"sdta", ck(b"smpl", smpl)) + lst(b"pdta", pdta))
sfw = os.path.join(here, "seeds", "soundfont")
os.makedirs(sfw, exist_ok=True)
pcm = sine16(441, 44.1) + b"\0" * 92 + sine16(882, 88.2, 12000) + b"\0" * 92
def pcm_samples(base):
    a, b = base, base + 441 + 46
    return [("Sine", (a, a + 441, a, a + 441, 44100, 69, 0, 0, 1)),
            ("L", (b, b + 882, b + 10, b + 872, 44100, 57, -5, 2, 4)),   # linked to R
            ("R", (b, b + 882, b + 10, b + 872, 44100, 57, 5, 1, 2))]    # linked to L
open(os.path.join(sfw, "bank.sf2"), "wb").write(sf2(pcm_samples(0), pcm))
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
def dls_rgn(lo, hi, cue, level2, ch=1, vel=(0, 127)):
    body = ck(b"rgnh", struct.pack("<HHHHHH", lo, hi, vel[0], vel[1], 0, 0)) + \
           ck(b"wlnk", struct.pack("<HHII", 0, 0, ch, cue))
    if level2: body += dls_art(True)
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
