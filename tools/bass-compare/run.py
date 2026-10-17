#!/usr/bin/env python3
"""Black-box comparison of FatzAudio against BASS 2.4 (tools/bass-compare/README.md).

Both libraries decode the same generated test files through decode-only streams:
fa_bench drives FatzAudio's public C API, bass_bench drives bass.dll and its
add-ons through their documented public API (loaded at run time, never linked,
unpacked or inspected). Every result is scored against what the file is known to
contain, and the report says, per feature, where FatzAudio matches, beats or
trails BASS.

  decode    every format the app plays: accuracy against FFmpeg's decode of the
            same file and against the original signal (signal-to-error dB),
            where the audio starts (offset), how many frames come out (length),
            the length each library reports before and after a prescan
  speed     decode speed (times real time), time to open, time to the first
            audio, and the memory one stream adds
  seek      seeks to frames all over the file (backwards too) and checks that
            each lands exactly where the library's own full decode has them
  streams   16 streams of one file decoded together: throughput and memory
  loudness  FatzAudio's scan against EBU Tech 3341 cases and a reference
            BS.1770-4 meter; BASS has no loudness meter, so its floor is the time
            it needs just to decode the file the app would have to measure
  disc      .cue/.bin images, track by track, against the exact source audio
  tempo     tools/tempo-compare (bass_fx), when its tools are given

Usage (Windows, from the build folder; Python 3 with numpy and scipy, and an
FFmpeg command line to make the test files):
  python tools\\bass-compare\\run.py --fa build\\bin\\Release\\fa_bench.exe ^
      --bass build\\bin\\Release\\bass_bench.exe --bass-dir C:\\path\\to\\legacy --out bass-results
Linux: Windows builds of both tools under Wine with --wine wine64 (see README).

Writes <out>/report.md, <out>/results.json and the test files (<out>/inputs).
"""
import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import time

import numpy as np
from scipy import signal as sps

SECONDS = 30.0


# ---- Test signals ---------------------------------------------------------------
# Music-like material made here, so the inputs are free of any licence: drums,
# bass, chords and a lead, with a quiet passage and a fade-out. 5.1 gets
# different content in every channel, so a swapped channel shows up.

def tone(rate, freq, dur, harmonics, decay, attack=0.005, vibrato=0.0, rng=None):
    n = int(dur * rate)
    t = np.arange(n) / rate
    ph = 2 * np.pi * freq * t
    if vibrato:
        ph += vibrato * np.sin(2 * np.pi * 5.5 * t) * freq / 5.5 / 100
    x = np.zeros(n)
    for k in range(1, harmonics + 1):
        if freq * k >= rate / 2.2:
            break
        x += np.sin(k * ph + (rng.uniform(0, 6.283) if rng is not None else 0)) / k
    env = np.minimum(t / attack, 1.0) * np.exp(-t / decay)
    return x * env


def music(rate, channels, seconds=SECONDS, seed=1):
    rng = np.random.default_rng(seed)
    n = int(seconds * rate)
    beat = 0.5
    scale = [220.0 * 2 ** (s / 12) for s in (0, 3, 5, 7, 10, 12, 15, 17)]
    roots = [110.0, 87.31, 130.81, 98.0]                    # A F C G
    voices = {k: np.zeros(n) for k in ('kick', 'hat', 'bass', 'chord', 'lead', 'pad')}

    def add(v, start, x):
        i = int(start * rate)
        if i >= n:
            return
        m = min(len(x), n - i)
        voices[v][i:i + m] += x[:m]

    t = np.arange(int(0.3 * rate)) / rate
    kick = np.sin(2 * np.pi * np.cumsum(45 + 75 * np.exp(-t / 0.04)) / rate) * np.exp(-t / 0.12)
    beats = int(seconds / beat)
    for b in range(beats):
        s = b * beat
        add('kick', s, kick * (0.9 if b % 2 == 0 else 0.6))
        for h in (0, 0.25):
            hat = rng.standard_normal(int(0.05 * rate))
            hat = np.diff(hat, prepend=0) * np.exp(-np.arange(len(hat)) / (0.012 * rate))
            add('hat', s + h, hat * 0.08)
        root = roots[(b // 4) % 4]
        add('bass', s, tone(rate, root / 2, beat, 8, 0.3, rng=rng) * 0.5)
        if b % 4 == 0:
            for semis in (0, 4, 7):
                add('chord', s, tone(rate, root * 2 * 2 ** (semis / 12), 2.0, 6, 1.2, attack=0.03, rng=rng) * 0.18)
            add('pad', s, tone(rate, root * 4, 2.0, 3, 1.5, attack=0.3, rng=rng) * 0.12)
        if rng.uniform() < 0.8:
            note = scale[rng.integers(len(scale))] * 2
            add('lead', s + rng.choice([0, 0.25]), tone(rate, note, 0.45, 10, 0.25, vibrato=0.4, rng=rng) * 0.22)
    tt = np.arange(n) / rate
    shape = np.ones(n)
    shape[(tt >= 20) & (tt < 24)] = 10 ** (-30 / 20)          # a quiet passage
    shape *= np.clip((seconds - tt) / 2.0, 0, 1)              # and a fade-out
    v = {k: x * shape for k, x in voices.items()}
    if channels == 2:
        left = v['kick'] + v['bass'] + 0.8 * v['chord'] + 0.5 * v['lead'] + 1.2 * v['hat'] + v['pad']
        right = v['kick'] + v['bass'] + 0.6 * v['chord'] + 0.9 * v['lead'] + 0.6 * v['hat'] + 0.7 * v['pad']
        out = np.stack([left, right], 1)
    else:                                                     # L R C LFE Ls Rs
        b, a = sps.butter(4, 120 / (rate / 2))
        lfe = sps.lfilter(b, a, v['kick'] + v['bass'])
        out = np.stack([v['chord'] + 0.5 * v['kick'], 0.8 * v['chord'] + 0.5 * v['bass'], v['lead'],
                        lfe, v['hat'], v['pad']], 1)
    return out * (0.7 / np.max(np.abs(out)))


# ---- WAV and raw float I/O ------------------------------------------------------

def write_wav(path, x, rate, kind):
    """kind: 's16', 's24' or 'f32'. Returns the samples exactly as stored."""
    ch = x.shape[1]
    if kind == 's16':
        q = np.clip(np.round(x * 32768), -32768, 32767)
        data, bits, tag, stored = q.astype('<i2').tobytes(), 16, 1, q / 32768
    elif kind == 's24':
        q = np.clip(np.round(x * 8388608), -8388608, 8388607).astype('<i4')
        data = np.frombuffer(q.tobytes(), np.uint8).reshape(-1, 4)[:, :3].tobytes()
        bits, tag, stored = 24, 1, q / 8388608
    else:
        f = x.astype('<f4')
        data, bits, tag, stored = f.tobytes(), 32, 3, f.astype(np.float64)
    fmt = struct.pack('<HHIIHH', tag, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits)
    if ch > 2:                                    # WAVE_FORMAT_EXTENSIBLE with the 5.1 mask
        sub = (b'\x01\x00' if tag == 1 else b'\x03\x00') + b'\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71'
        fmt = struct.pack('<HHIIHH', 0xFFFE, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits) + \
            struct.pack('<HHI', 22, bits, 0x3F) + sub
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 4 + 8 + len(fmt) + 8 + len(data)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data)
    return stored


def read_f32(path, channels):
    x = np.fromfile(path, dtype='<f4').astype(np.float64)
    return x[:len(x) // channels * channels].reshape(-1, channels)


# ---- Scoring --------------------------------------------------------------------

def find_offset(x, ref, rate, max_s=0.25):
    """Frames by which x lags ref (positive: x starts late), from the first 8 s."""
    m = int(8 * rate)
    a, b = x[:m].sum(1), ref[:m].sum(1)
    if not len(a) or not len(b) or not np.any(b):
        return 0
    c = sps.correlate(a, b, mode='full', method='fft')
    lags = np.arange(-len(b) + 1, len(a))
    keep = np.abs(lags) <= int(max_s * rate)
    return int(lags[keep][np.argmax(c[keep])])


def overlap(x, ref, off, edge=2048):
    if off >= 0:
        xa, ra = x[off:], ref
    else:
        xa, ra = x, ref[-off:]
    n = min(len(xa), len(ra))
    lo, hi = min(edge, n // 4), max(n - edge, n - n // 4)
    return xa[lo:hi], ra[lo:hi]


def channel_map(x, ref):
    """For each output channel, the reference channel it matches best."""
    if x.shape[1] != ref.shape[1]:
        return None
    c = np.zeros((x.shape[1], ref.shape[1]))
    for i in range(x.shape[1]):
        for j in range(ref.shape[1]):
            d = np.sqrt(np.sum(x[:, i] ** 2) * np.sum(ref[:, j] ** 2))
            c[i, j] = np.sum(x[:, i] * ref[:, j]) / d if d > 0 else 0
    return [int(j) for j in np.argmax(c, 1)]


def score(x, ref, rate):
    """Signal-to-error dB of x against ref, after finding where it starts."""
    off = find_offset(x, ref, rate)
    xa, ra = overlap(x, ref, off)
    if not len(xa) or x.shape[1] != ref.shape[1]:
        return {'offset': off, 'db': None, 'gain_db': None, 'max_err': None, 'map': None}
    cmap = channel_map(xa, ra)
    if cmap and sorted(cmap) == list(range(ref.shape[1])):
        ra = ra[:, cmap]                 # score a swapped layout on its content; report the swap
    err = xa - ra
    es, rs, xs = np.sum(err ** 2), np.sum(ra ** 2), np.sum(xa ** 2)
    return {'offset': off,
            'db': 200.0 if es == 0 else 10 * math.log10(rs / es),
            'gain_db': 10 * math.log10(xs / rs) if rs > 0 and xs > 0 else None,
            'max_err': float(np.max(np.abs(err))),
            'map': None if cmap == list(range(ref.shape[1])) else cmap}


# ---- BS.1770-4 reference meter (gated integrated loudness) --------------------

def kweight(x, rate):
    """The two K-weighting biquads, derived for any rate (as in libebur128)."""
    f0, g, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = math.tan(math.pi * f0 / rate)
    vh = 10 ** (g / 20)
    vb = vh ** 0.4996667741545416
    a0 = 1 + k / q + k * k
    b1 = [(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0]
    a1 = [1, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    f0, q = 38.13547087602444, 0.5003270373238773
    k = math.tan(math.pi * f0 / rate)
    a2 = [1, 2 * (k * k - 1) / (1 + k / q + k * k), (1 - k / q + k * k) / (1 + k / q + k * k)]
    return sps.lfilter([1, -2, 1], a2, sps.lfilter(b1, a1, x, axis=0), axis=0)


def reference_lufs(x, rate):
    weights = [1.0, 1.0, 1.0, 0.0, 1.41, 1.41] if x.shape[1] == 6 else [1.0] * x.shape[1]
    y = kweight(x, rate) ** 2
    block, step = int(0.4 * rate), int(0.1 * rate)
    starts = range(0, len(y) - block + 1, step)
    cs = np.vstack([np.zeros((1, y.shape[1])), np.cumsum(y, 0)])
    z = np.array([(cs[s + block] - cs[s]) / block for s in starts])
    p = z @ np.array(weights)
    lk = -0.691 + 10 * np.log10(np.maximum(p, 1e-30))
    gated = p[lk > -70]
    if not len(gated):
        return -math.inf
    rel = -0.691 + 10 * math.log10(np.mean(gated)) - 10
    gated = p[(lk > -70) & (lk > rel)]
    return -0.691 + 10 * math.log10(np.mean(gated))


def ebu3341(rate=48000):
    """EBU Tech 3341 cases 1-6: (name, signal, expected LUFS)."""
    def sine(db, secs, ch=2):
        t = np.arange(int(secs * rate)) / rate
        return np.repeat((10 ** (db / 20) * np.sin(2 * np.pi * 1000 * t))[:, None], ch, 1)
    cases = [('1 kHz at -23 dBFS', sine(-23, 20), -23.0),
             ('1 kHz at -33 dBFS', sine(-33, 20), -33.0),
             ('-36/-23/-36 dBFS (gating)', np.vstack([sine(-36, 10), sine(-23, 60), sine(-36, 10)]), -23.0),
             ('-72/-36/-23/-36/-72 dBFS', np.vstack([sine(-72, 10), sine(-36, 10), sine(-23, 60),
                                                     sine(-36, 10), sine(-72, 10)]), -23.0),
             ('-26/-20/-26 dBFS', np.vstack([sine(-26, 20), sine(-20, 20.1), sine(-26, 20)]), -23.0)]
    five = sine(0, 20, 6) * (10 ** (np.array([-28, -28, -24, -200, -30, -30]) / 20))
    cases.append(('5.1: L/R -28, C -24, Ls/Rs -30 dBFS, LFE silent', five, -23.0))
    return cases


# ---- Running the tools ----------------------------------------------------------

class Tool:
    def __init__(self, name, exe, wine=None, prefix=None):
        self.name, self.exe, self.wine, self.prefix = name, exe, wine, prefix or []

    def path(self, p):
        if p == '-' or not self.wine or not os.path.isabs(p):
            return p
        return 'Z:' + p.replace('/', '\\')

    def run(self, *args, timeout=900):
        cmd = ([self.wine] if self.wine else []) + [self.exe] + [self.path(a) for a in self.prefix] + \
            [self.path(a) if isinstance(a, str) and os.sep in a else str(a) for a in args]
        env = dict(os.environ, WINEDEBUG='-all')
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        except subprocess.TimeoutExpired:
            return {'error': 'timed out after %d s' % timeout}
        for line in reversed(p.stdout.splitlines()):
            if line.startswith('{'):
                return json.loads(line)
        return {'error': (p.stderr.strip().splitlines() or ['exit %d' % p.returncode])[-1]}


def ffmpeg(args, ff):
    subprocess.run([ff, '-v', 'error', '-y'] + args, check=True)


def ffmpeg_decode(path, out, channels, ff):
    ffmpeg(['-i', path, '-map', '0:a:0', '-f', 'f32le', '-ac', str(channels), out], ff)


def make_inputs(d, ff, long_minutes, only=None):
    """Writes the test files; returns {name: case} with each file's ground truth."""
    os.makedirs(d, exist_ok=True)
    s44, s48, s51 = music(44100, 2), music(48000, 2, seed=2), music(48000, 6, seed=3)
    cases = {}

    def wav(name, x, rate, kind):
        p = os.path.join(d, name)
        return p, write_wav(p, x, rate, kind)

    p16, q16 = wav('src44_s16.wav', s44, 44100, 's16')
    p24, q24 = wav('src48_s24.wav', s48, 48000, 's24')
    pf, qf = wav('src48_f32.wav', s48, 48000, 'f32')
    p44f, _ = wav('src44_f32.wav', s44, 44100, 'f32')
    p51f, q51 = wav('src51_f32.wav', s51, 48000, 'f32')

    def case(name, path, src, rate, lossless, what):
        cases[name] = {'path': path, 'src': src, 'rate': rate, 'lossless': lossless, 'what': what}

    case('wav16', p16, q16, 44100, True, 'WAV, 16-bit, 44.1 kHz stereo')
    case('wav24', p24, q24, 48000, True, 'WAV, 24-bit, 48 kHz stereo')
    case('wavf32', pf, qf, 48000, True, 'WAV, 32-bit float, 48 kHz stereo')
    enc = [('flac16', 'flac16.flac', p16, ['-c:a', 'flac'], q16, 44100, True, 'FLAC, 16-bit, 44.1 kHz'),
           ('flac24', 'flac24.flac', p24, ['-c:a', 'flac'], q24, 48000, True, 'FLAC, 24-bit, 48 kHz'),
           ('mp3-cbr', 'mp3_cbr192.mp3', p44f, ['-c:a', 'libmp3lame', '-b:a', '192k'], s44, 44100, False, 'MP3, 192 kbit/s CBR, LAME header'),
           ('mp3-vbr', 'mp3_vbr.mp3', p44f, ['-c:a', 'libmp3lame', '-q:a', '2'], s44, 44100, False, 'MP3, VBR (-V2)'),
           ('mp3-noxing', 'mp3_noxing.mp3', p44f, ['-c:a', 'libmp3lame', '-b:a', '128k', '-write_xing', '0'], s44, 44100, False, 'MP3, 128 kbit/s, no Xing/LAME header'),
           ('aac-m4a', 'aac.m4a', p44f, ['-c:a', 'aac', '-b:a', '192k'], s44, 44100, False, 'AAC-LC in M4A, 192 kbit/s'),
           ('aac-adts', 'aac_adts.aac', p44f, ['-c:a', 'aac', '-b:a', '160k'], s44, 44100, False, 'AAC-LC raw ADTS (.aac, AAC radio)'),
           ('ac3-stereo', 'ac3_stereo.ac3', pf, ['-c:a', 'ac3', '-b:a', '192k'], qf, 48000, False, 'AC-3 (Dolby Digital) stereo, 192 kbit/s'),
           ('ac3-5.1', 'ac3_51.ac3', p51f, ['-c:a', 'ac3', '-b:a', '448k'], q51, 48000, False, 'AC-3 5.1, 448 kbit/s'),
           ('eac3-5.1', 'eac3_51.eac3', p51f, ['-c:a', 'eac3', '-b:a', '384k'], q51, 48000, False, 'E-AC-3 (Dolby Digital Plus) 5.1, 384 kbit/s'),
           ('opus', 'opus.opus', pf, ['-c:a', 'libopus', '-b:a', '128k'], qf, 48000, False, 'Opus in Ogg, 128 kbit/s'),
           ('opus-webm', 'opus.webm', pf, ['-c:a', 'libopus', '-b:a', '128k'], qf, 48000, False, 'Opus in WebM'),
           ('vorbis', 'vorbis.ogg', p44f, ['-c:a', 'libvorbis', '-q:a', '5'], s44, 44100, False, 'Ogg Vorbis, q5'),
           ('wma', 'wma.wma', p44f, ['-c:a', 'wmav2', '-b:a', '192k'], s44, 44100, False, 'WMA 2, 192 kbit/s'),
           ('ac3-mkv', 'ac3_51.mkv', p51f, ['-c:a', 'ac3', '-b:a', '448k'], q51, 48000, False, 'AC-3 5.1 in Matroska (video files)'),
           ('aac-mp4', 'aac.mp4', p44f, ['-c:a', 'aac', '-b:a', '192k', '-f', 'mp4'], s44, 44100, False, 'AAC-LC in MP4 (video files)')]
    for name, fn, src_path, opts, src, rate, lossless, what in enc:
        if only and name not in only:
            continue
        p = os.path.join(d, fn)
        ffmpeg(['-i', src_path] + opts + [p], ff)
        case(name, p, src, rate, lossless, what)

    if only:
        return {k: v for k, v in cases.items() if k in only}
    # Long files for scan speed: plain MP3, and a video file (H.264 + AC-3 5.1).
    long_s = long_minutes * 60
    lng = np.vstack([music(48000, 2, 60, seed=10 + i) for i in range(long_minutes)])
    lp = os.path.join(d, 'long48.wav')
    write_wav(lp, lng, 48000, 'f32')
    ffmpeg(['-i', lp, '-c:a', 'libmp3lame', '-b:a', '192k', os.path.join(d, 'long.mp3')], ff)
    ffmpeg(['-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=25:duration=%d' % long_s, '-i', lp,
            '-c:v', 'libx264', '-preset', 'ultrafast', '-b:v', '1500k', '-c:a', 'ac3', '-b:a', '448k',
            '-shortest', os.path.join(d, 'long_video.mkv')], ff)
    ffmpeg(['-i', os.path.join(d, 'long_video.mkv'), '-map', '0:a', '-c', 'copy', os.path.join(d, 'long_video.ac3')], ff)
    return cases


def make_disc(d):
    """A three-track .cue/.bin image of 16-bit 44.1 kHz audio, tracks on sector
    boundaries (588 frames), with a pregap before track 2. Returns the cue path
    and each track's exact audio."""
    x = music(44100, 2, 40, seed=7)
    q = np.clip(np.round(x * 32768), -32768, 32767).astype('<i2')
    sectors = [12 * 75 + 31, 15 * 75 + 2, len(q) // 588 - 12 * 75 - 31 - 15 * 75 - 2]
    with open(os.path.join(d, 'disc.bin'), 'wb') as f:
        f.write(q[:sum(sectors) * 588].tobytes())
    lines, starts, pos = ['FILE "disc.bin" BINARY'], [], 0

    def msf(s):
        return '%02d:%02d:%02d' % (s // 4500, s // 75 % 60, s % 75)
    for i, n in enumerate(sectors):
        lines.append('  TRACK %02d AUDIO' % (i + 1))
        if i == 1:                         # 2 s pregap in the file: INDEX 00, then 01
            lines.append('    INDEX 00 ' + msf(pos))
        starts.append(pos + (150 if i == 1 else 0))
        lines.append('    INDEX 01 ' + msf(starts[-1]))
        pos += n
    # As on a real disc's TOC, a track runs from its INDEX 01 to the next one's, so
    # track 2's pregap plays at the end of track 1.
    starts.append(pos)
    tracks = [q[a * 588:b * 588].astype(np.float64) / 32768 for a, b in zip(starts, starts[1:])]
    cue = os.path.join(d, 'disc.cue')
    with open(cue, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return cue, tracks


# ---- The measurements -----------------------------------------------------------

def decode_case(tool, c, work, runs, prescan_too=True):
    out = os.path.join(work, '%s.f32' % tool.name)
    r = tool.run('decode', c['path'], out)
    res = {'decode': r}
    if 'error' in r:
        return res
    x = read_f32(out, r['channels'])
    os.remove(out)
    res['vs_src'] = score(x, c['src'], c['rate']) if r['channels'] == c['src'].shape[1] else None
    res['vs_ff'] = score(x, c['ff'], c['rate']) if c.get('ff') is not None and r['channels'] == c['ff'].shape[1] else None
    res['frames'] = r['frames']
    if prescan_too:
        p = tool.run('info', c['path'], '--prescan')
        res['length_prescan'] = p.get('length')
    speed = [tool.run('decode', c['path'], '-') for _ in range(runs)]
    speed = [s for s in speed if 'error' not in s] or [r]
    res['total_ms'] = min(s['total_ms'] for s in speed)
    res['open_ms'] = min(s['open_ms'] for s in speed)
    res['first_ms'] = min(s['first_ms'] for s in speed)
    res['mem_kb'] = float(np.median([s['peak_kb'] - s['base_kb'] for s in speed]))
    res['own'] = x
    return res


def seek_targets(rate, frames):
    raw = [0.5, 7.25, 13.0, 2.0, 20.123, 27.5, 9.99, 1.0]
    extra = [123, 1, 777, 17, 0, 5, 333, 1151]
    return [min(int(s * rate) + e, max(frames - 5000, 0)) for s, e in zip(raw, extra)]


def seek_offset(b, own, t, search=None):
    """Where block b (read after seeking to frame t) really is in the library's own
    full decode, as frames from t; None when it is silent or nowhere near."""
    n = len(b)
    if not np.any(b):
        return None
    seg = own[t:t + n]
    if len(seg) == n and np.sum((b - seg) ** 2) <= 1e-6 * max(np.sum(seg ** 2), 1e-30):
        return 0                                   # at the frame asked for (within -60 dB)
    search = search or n
    lo = max(t - search, 0)
    ref = own[lo:t + n + search].sum(1)
    x = b.sum(1)
    if len(ref) < n:
        return None
    # Normalized cross-correlation, so a louder passage nearby cannot win.
    c = sps.correlate(ref, x, mode='valid', method='fft')
    energy = np.convolve(ref ** 2, np.ones(n), mode='valid')
    c = c / np.sqrt(np.maximum(energy, 1e-30) * max(np.sum(x ** 2), 1e-30))
    k = int(np.argmax(c))
    return lo + k - t if c[k] > 0.5 else None


def seek_case(tool, c, own, work, prescan):
    n = 4096
    targets = seek_targets(c['rate'], len(own))
    out = os.path.join(work, '%s_seek.f32' % tool.name)
    args = ['seek', c['path'], out] + (['--prescan'] if prescan else []) + [n] + targets
    r = tool.run(*args)
    if 'error' in r:
        return {'error': r['error']}
    blocks = read_f32(out, r['channels'])
    os.remove(out)
    exact, on_target, worst_off, worst_db = 0, 0, 0, 200.0
    for i, t in enumerate(targets):
        b = blocks[i * n:(i + 1) * n]
        off = seek_offset(b, own, t, c['rate'] // 2)
        if off is None:
            worst_off, worst_db = max(worst_off, 10 ** 9), -200.0
            continue
        seg = own[t + off:t + off + n]
        e = np.sum((b[:len(seg)] - seg) ** 2)
        db = 200.0 if e == 0 else 10 * math.log10(max(np.sum(seg ** 2), 1e-30) / e)
        worst_off, worst_db = max(worst_off, abs(off)), min(worst_db, db)
        on_target += int(off == 0)
        exact += int(off == 0 and db > 90)
    return {'exact': exact, 'on_target': on_target, 'count': len(targets), 'worst_off': worst_off, 'worst_db': worst_db,
            'worst_ms': r.get('worst_ms'), 'ok': sum(s['ok'] for s in r['seeks'])}


def run_tempo(args, out):
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tempo-compare', 'compare.py')
    cmd = [sys.executable, script, '--fa', args.fa_tempo, '--bassfx', args.bassfx,
           '--bass-dir', args.bass_dir, '--out', os.path.join(out, 'tempo')]
    if args.wine:
        cmd += ['--wine', args.wine]
    subprocess.run(cmd, check=True)
    return os.path.join(out, 'tempo', 'report.md')


# ---- Report ---------------------------------------------------------------------

def fmt(v, spec='%.1f', none='–'):
    return none if v is None else spec % v


def winner(fa, bass, higher=True, margin=0.0):
    """'fa', 'bass' or 'tie' for one metric."""
    if fa is None and bass is None:
        return 'tie'
    if fa is None:
        return 'bass'
    if bass is None:
        return 'fa'
    d = (fa - bass) if higher else (bass - fa)
    return 'fa' if d > margin else 'bass' if d < -margin else 'tie'


def bold(cells, w):
    """Bold the winning one of two cells."""
    a, b = cells
    return ('**%s**' % a if w == 'fa' else a, '**%s**' % b if w == 'bass' else b)


def ms_of(frames, rate):
    return None if frames is None else 1000.0 * frames / rate


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fa', required=True, help='fa_bench executable')
    ap.add_argument('--fa-ffmpeg', help='fa_bench to use for formats that need FFmpeg (AAC, Opus, '
                    'Vorbis) when --fa has none; e.g. a native Linux build next to a Wine one')
    ap.add_argument('--ffmpeg-dir', help='folder with the FFmpeg DLLs for fa_bench (your app\'s FFmpeg)')
    ap.add_argument('--bass', required=True, help='bass_bench.exe')
    ap.add_argument('--bass-dir', required=True, help='folder with bass.dll and its add-ons')
    ap.add_argument('--wine', help='run the Windows tools through this (e.g. wine64)')
    ap.add_argument('--fa-wine', action='store_true', help='--fa is a Windows build: run it through --wine too')
    ap.add_argument('--ffmpeg', default='ffmpeg', help='FFmpeg command line, to make the test files')
    ap.add_argument('--fa-tempo', help='fa_tempo_render, to include tools/tempo-compare')
    ap.add_argument('--bassfx', help='bassfx_render.exe, to include tools/tempo-compare')
    ap.add_argument('--runs', type=int, default=5, help='speed runs per file (the best counts)')
    ap.add_argument('--long-minutes', type=int, default=10, help='length of the scan-speed files')
    ap.add_argument('--only', help='comma-separated decode cases to run (default: all)')
    ap.add_argument('--out', default='bass-compare-results')
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    work = os.path.join(out, 'work')
    inputs = os.path.join(out, 'inputs')
    os.makedirs(work, exist_ok=True)
    ffdir = [args.ffmpeg_dir] if args.ffmpeg_dir else []
    fa = Tool('fatzaudio', args.fa, args.wine if args.fa_wine else None, (['--ffmpeg'] + ffdir) if ffdir else [])
    fa_ff = Tool('fatzaudio', args.fa_ffmpeg) if args.fa_ffmpeg else None
    bass = Tool('bass', args.bass, args.wine, [args.bass_dir])

    print('making test files in', inputs)
    only = args.only.split(',') if args.only else None
    cases = make_inputs(inputs, args.ffmpeg, args.long_minutes, only)
    results = {'cases': {}, 'host': {}}
    for name, c in cases.items():
        ch = c['src'].shape[1]
        ffo = os.path.join(work, 'ff.f32')
        ffmpeg_decode(c['path'], ffo, ch, args.ffmpeg)
        c['ff'] = read_f32(ffo, ch)
        os.remove(ffo)
        c['ff_score'] = score(c['ff'], c['src'], c['rate'])
        row = {}
        for tool in (fa, bass):
            print('decode %-12s %s' % (name, tool.name), flush=True)
            res = decode_case(tool, c, work, args.runs)
            if tool is fa and 'error' in res['decode'] and fa_ff:
                res = decode_case(fa_ff, c, work, args.runs)
                res['via'] = 'ffmpeg-native'
            if 'own' in res:
                t = fa_ff if res.get('via') else tool
                res['seek'] = seek_case(t, c, res['own'], work, False)
                res['seek_prescan'] = seek_case(t, c, res['own'], work, True)
                del res['own']
            row[tool.name] = res
        results['cases'][name] = row

    # Many streams at once.
    results['multi'] = {}
    for name in ('mp3-cbr', 'flac16', 'ac3-5.1'):
        if name not in cases:
            continue
        results['multi'][name] = {t.name: t.run('multi', cases[name]['path'], 16) for t in (fa, bass)}

    # Loudness: EBU Tech 3341 and the music files against the reference meter.
    print('loudness', flush=True)
    lres = []
    for i, (label, x, expect) in enumerate(ebu3341()):
        p = os.path.join(inputs, 'ebu3341_%d.wav' % (i + 1))
        write_wav(p, x, 48000, 's24')
        lres.append({'case': label, 'expected': expect, 'fa': fa.run('loudness', p)})
    for name in ('wav24', 'flac16', 'mp3-cbr', 'ac3-5.1'):
        if name in cases:
            c = cases[name]
            lres.append({'case': c['what'], 'expected': reference_lufs(c['ff'], c['rate']),
                         'fa': fa.run('loudness', c['path'])})
    results['loudness'] = lres

    # Scan speed on long files: FatzAudio's full scan against BASS only decoding.
    # Skipped with --only, which makes no long files.
    scan = []
    scan_files = () if only else (
        ('long.mp3', 'MP3, %d min' % args.long_minutes),
        ('long_video.mkv', 'Video file (H.264 + AC-3 5.1 MKV), %d min' % args.long_minutes),
        ('long_video.ac3', 'Its AC-3 track alone, %d min' % args.long_minutes))
    for fn, label in scan_files:
        p = os.path.join(inputs, fn)
        f = fa.run('loudness', p)
        b = bass.run('decode', p, '-')
        fd = fa.run('decode', p, '-')
        scan.append({'file': label, 'size_mb': os.path.getsize(p) / 1e6, 'fa_scan': f, 'fa_decode': fd, 'bass_decode': b})
    results['scan'] = scan

    # Disc images.
    cue, tracks = make_disc(inputs)
    disc = []
    for i, ref in enumerate(tracks):
        o = os.path.join(work, 'disc.f32')
        r = fa.run('disc', cue, i + 1, o)
        if 'error' not in r:
            x = read_f32(o, r['channels'])
            os.remove(o)
            r['exact'] = bool(len(x) == len(ref) and np.array_equal(x, ref.astype(np.float32).astype(np.float64)))
            r['expected_frames'] = len(ref)
        disc.append(r)
    results['disc'] = disc
    results['disc_bass'] = bass.run('decode', cue, '-')

    tempo = None
    if args.fa_tempo and args.bassfx:
        print('tempo-compare', flush=True)
        tempo = run_tempo(args, out)

    with open(os.path.join(out, 'results.json'), 'w') as f:
        json.dump(results, f, indent=1, default=float)
    write_report(out, results, cases, args, tempo)
    shutil.rmtree(work, ignore_errors=True)
    print('wrote', os.path.join(out, 'report.md'))


def write_report(out, results, cases, args, tempo):
    L, worse = [], []
    sample = next((r for row in results['cases'].values() for r in [row.get('bass', {}).get('decode', {})] if 'version' in r), {})
    v = sample.get('version', '')
    bass_ver = '%d.%d.%d.%d' % tuple(int(v[i:i + 2], 16) for i in (0, 2, 4, 6)) if v else 'unknown'
    L.append('# FatzAudio against BASS: black-box comparison\n')
    L.append('Generated by `tools/bass-compare/run.py` on %s. BASS %s (BASS_GetVersion) with %s, '
             'driven through its public API only. Both libraries decode the same generated files '
             'through decode-only streams.%s\n' % (
                 time.strftime('%Y-%m-%d'), bass_ver, sample.get('plugins', 'no add-ons').replace(',', ', '),
                 ' Both run as Windows builds under Wine (%s).' % args.wine if args.wine and args.fa_wine else ''))
    if args.fa_ffmpeg:
        L.append('Rows marked † are formats FatzAudio decodes through the app\'s FFmpeg. No Windows FFmpeg '
                 'was available here, so they ran in a native build with the system FFmpeg; their accuracy '
                 'is comparable, their speed and memory less so.\n')

    # Decoding accuracy.
    L.append('## Decoding accuracy\n')
    L.append('Signal-to-error against FFmpeg\'s decode of the same file and against the original '
             'signal (dB, higher is better; 200 = bit exact). Lossy formats can only approach the '
             'original as far as the codec allows, so "vs FFmpeg" is the decoder\'s own accuracy. '
             'Start is where the first sample lands against the original (ms; 0 is right), length the '
             'frames decoded against the original (+ is extra frames). FFmpeg\'s columns show what a '
             'format allows: AC-3 and MP3 without a LAME header carry no encoder delay, so every '
             'decoder starts late there.\n')
    L.append('| File | FatzAudio vs FFmpeg | BASS vs FFmpeg | FatzAudio vs original | BASS vs original | '
             'FatzAudio start | BASS start | FFmpeg start | FatzAudio length | BASS length | FFmpeg length |')
    L.append('|---|---|---|---|---|---|---|---|---|---|---|')
    for name, row in results['cases'].items():
        c = cases[name]
        f, b = row['fatzaudio'], row['bass']
        dag = ' †' if f.get('via') else ''
        cells = []

        def get(res, key, sub):
            s = res.get(key)
            return None if not s else s.get(sub)
        for key in ('vs_ff', 'vs_src'):
            fv, bv = get(f, key, 'db'), get(b, key, 'db')
            w = winner(fv, bv, True, 0.5)
            cells += bold((fmt(fv), fmt(bv)), w)
            if key == 'vs_ff' and w == 'bass' and fv is not None:
                worse.append('%s: decoder accuracy %.1f dB against BASS %.1f dB%s' % (
                    c['what'], fv, bv, ' (both far below hearing: above 80 dB)' if fv >= 80 else ''))
            if key == 'vs_ff' and 'error' in f['decode'] and bv is not None:
                worse.append('%s: FatzAudio cannot open it (%s); BASS can' % (c['what'], f['decode'].get('error')))
        for key in ('fatzaudio', 'bass'):
            res = row[key]
            off = get(res, 'vs_src', 'offset')
            m = get(res, 'vs_src', 'map')
            cells.append(fmt(ms_of(off, c['rate']), '%.2f') + (' (channels %s)' % m if m else ''))
        cells.append(fmt(ms_of(c['ff_score']['offset'], c['rate']), '%.2f'))
        fo, bo = get(f, 'vs_src', 'offset'), get(b, 'vs_src', 'offset')
        if fo is not None and bo is not None and abs(fo) > abs(bo):
            worse.append('%s: audio starts %d frames off (BASS %d)' % (c['what'], fo, bo))
        n = len(c['src'])
        lens = []
        for key in ('fatzaudio', 'bass'):
            fr = row[key].get('frames')
            lens.append(None if fr is None else fr - n)
        for d in lens + [len(c['ff']) - n]:
            cells.append('–' if d is None else ('exact' if d == 0 else '%+d' % d))
        if lens[0] is not None and lens[1] is not None and abs(lens[0]) > abs(lens[1]):
            worse.append('%s: length off by %d frames (BASS %d)' % (c['what'], lens[0], lens[1]))
        cells = [c['what'] + dag] + cells
        for key in ('fatzaudio', 'bass'):
            e = row[key]['decode'].get('error')
            if e:
                i = 1 if key == 'fatzaudio' else 2
                cells[i] = cells[i + 2] = 'cannot open (%s)' % e
                cells[i + 4] = cells[i + 7] = '–'
        L.append('| ' + ' | '.join(cells) + ' |')

    # Reported length.
    L.append('\n## Reported length\n')
    L.append('What each library says the length is right after opening, and after a prescan '
             '(FA_OPEN_PRESCAN / BASS_STREAM_PRESCAN), against the true length (frames off; exact is right).\n')
    L.append('| File | FatzAudio | BASS | FatzAudio, prescan | BASS, prescan |')
    L.append('|---|---|---|---|---|')
    for name, row in results['cases'].items():
        n = len(cases[name]['src'])
        cells = []
        diffs = []
        for key in ('length', 'length_prescan'):
            for lib in ('fatzaudio', 'bass'):
                r = row[lib]
                v = r['decode'].get('length') if key == 'length' else r.get('length_prescan')
                d = None if v is None or v < 0 else v - n
                diffs.append(d)
                cells.append('unknown' if v is not None and v < 0 else '–' if d is None else 'exact' if d == 0 else '%+d' % d)
        for i, lab in ((0, 'open'), (2, 'prescan')):
            if diffs[i] is not None and diffs[i + 1] is not None and abs(diffs[i]) > abs(diffs[i + 1]):
                worse.append('%s: reported length after %s off by %d frames (BASS %d)' % (cases[name]['what'], lab, diffs[i], diffs[i + 1]))
        L.append('| %s | %s |' % (cases[name]['what'], ' | '.join([cells[0], cells[1], cells[2], cells[3]])))

    # Speed and memory.
    L.append('\n## Speed and memory\n')
    L.append('Best of %d runs. Speed is times real time for the whole file (higher is better); open is '
             'the open call, first audio the time from the open call to the first decoded frames; '
             'memory is the most the process grew by while decoding one stream, from just before it '
             'opens (after a warm-up decode, so loading code and one-time setup are not counted). Bold marks the better one where the '
             'gap is real: more than 10%% in speed, 1 ms in time, 256 KB and 25%% in memory.\n' % args.runs)
    L.append('| File | FatzAudio speed | BASS speed | FatzAudio open ms | BASS open ms | FatzAudio first audio ms | '
             'BASS first audio ms | FatzAudio memory KB | BASS memory KB |')
    L.append('|---|---|---|---|---|---|---|---|---|')
    for name, row in results['cases'].items():
        c = cases[name]
        f, b = row['fatzaudio'], row['bass']
        dur = 1000.0 * len(c['src']) / c['rate']
        fx = dur / f['total_ms'] if f.get('total_ms') else None
        bx = dur / b['total_ms'] if b.get('total_ms') else None
        w = winner(fx, bx, True, 0.1 * (bx or 0))
        cells = list(bold((fmt(fx, '%.0f×'), fmt(bx, '%.0f×')), w))
        if w == 'bass' and fx is not None and not f.get('via'):
            worse.append('%s: decodes at %.0f× real time, BASS %.0f×' % (c['what'], fx, bx))
        for key, spec, marg in (('open_ms', '%.2f', 1.0), ('first_ms', '%.2f', 1.0), ('mem_kb', '%.0f', 256)):
            fv, bv = f.get(key), b.get(key)
            w = winner(fv, bv, False, max(marg, 0.25 * (bv or 0)))
            cells += bold((fmt(fv, spec), fmt(bv, spec)), w)
            if w == 'bass' and fv is not None and not f.get('via'):
                worse.append('%s: %s %s, BASS %s' % (c['what'], {'open_ms': 'opening takes (ms)', 'first_ms': 'first audio after (ms)',
                                                                'mem_kb': 'one stream uses (KB)'}[key], spec % fv, spec % bv))
        L.append('| %s%s | %s |' % (c['what'], ' †' if f.get('via') else '', ' | '.join(cells)))

    # Seeking.
    L.append('\n## Seeking\n')
    L.append('Eight seeks per file to frames all over it, forwards and backwards, each followed by '
             'reading 4096 frames. On target counts the seeks that land on the frame asked for, '
             'measured against the same library\'s own decode of the whole file; off is the worst '
             'miss (ms); same audio is how closely the frames read after the worst seek match that '
             'decode (dB; 200 = identical; lossy decoders restarting mid-file land a little below). '
             'Seek ms is the slowest seek plus its read.\n')
    for key, title in (('seek', 'Without a prescan (how most apps open files)'), ('seek_prescan', 'With a prescan')):
        L.append('**%s**\n' % title)
        L.append('| File | FatzAudio on target | BASS on target | FatzAudio off ms | BASS off ms | '
                 'FatzAudio same audio dB | BASS same audio dB | FatzAudio seek ms | BASS seek ms |')
        L.append('|---|---|---|---|---|---|---|---|---|')
        for name, row in results['cases'].items():
            c = cases[name]
            fs, bs = row['fatzaudio'].get(key) or {}, row['bass'].get(key) or {}
            fs, bs = ({} if 'error' in d else d for d in (fs, bs))
            if not fs and not bs:
                continue
            fe, be = fs.get('on_target'), bs.get('on_target')
            w = winner(fe, be)
            cells = list(bold(('%d/%d' % (fe, fs['count']) if fe is not None else '–',
                               '%d/%d' % (be, bs['count']) if be is not None else '–'), w))
            if w == 'bass' and fe is not None:
                worse.append('%s: %d of 8 seeks on target%s, BASS %d' % (c['what'], fe or 0, ' with a prescan' if key != 'seek' else '', be))
            fo, bo = fs.get('worst_off'), bs.get('worst_off')
            fo = None if fo is None or fo >= 10 ** 9 else ms_of(fo, c['rate'])
            bo = None if bo is None or bo >= 10 ** 9 else ms_of(bo, c['rate'])
            cells += bold((fmt(fo, '%.2f', 'lost' if fs else '–'), fmt(bo, '%.2f', 'lost' if bs else '–')), winner(fo, bo, False, 0.05))
            fd, bd = fs.get('worst_db'), bs.get('worst_db')
            w = winner(fd, bd, True, 3)
            cells += bold((fmt(fd), fmt(bd)), w)
            if w == 'bass' and fd is not None and fd < 60:
                worse.append('%s: the audio right after a seek%s matches a straight decode to %.1f dB, BASS %.1f dB' % (
                    c['what'], ' with a prescan' if key != 'seek' else '', fd, bd))
            fm, bm = fs.get('worst_ms'), bs.get('worst_ms')
            w = winner(fm, bm, False, max(1.0, 0.25 * (bm or 0)))
            cells += bold((fmt(fm, '%.2f'), fmt(bm, '%.2f')), w)
            if w == 'bass' and fm is not None and not row['fatzaudio'].get('via'):
                worse.append('%s: slowest seek%s %.1f ms, BASS %.1f ms' % (c['what'], ' with a prescan' if key != 'seek' else '', fm, bm))
            L.append('| %s | %s |' % (c['what'], ' | '.join(cells)))
        L.append('')

    # Many streams.
    if results['multi']:
        L.append('\n## Sixteen streams at once\n')
        L.append('Sixteen streams of the same file opened together and decoded in turn to the end.\n')
        L.append('| File | FatzAudio speed | BASS speed | FatzAudio memory per stream KB | BASS memory per stream KB |')
        L.append('|---|---|---|---|---|')
        for name, r in results['multi'].items():
            c = cases[name]
            f, b = r['fatzaudio'], r['bass']
            dur = 16 * 1000.0 * len(c['src']) / c['rate']
            fx = dur / f['total_ms'] if 'total_ms' in f else None
            bx = dur / b['total_ms'] if 'total_ms' in b else None
            fm = (f['peak_kb'] - f['base_kb']) / 16 if 'peak_kb' in f else None
            bm = (b['peak_kb'] - b['base_kb']) / 16 if 'peak_kb' in b else None
            ws = winner(fx, bx, True, 0.1 * (bx or 0))
            wm = winner(fm, bm, False, max(64, 0.25 * (bm or 0)))
            cells = list(bold((fmt(fx, '%.0f×', f.get('error', '–')), fmt(bx, '%.0f×', b.get('error', '–'))), ws))
            cells += bold((fmt(fm, '%.0f'), fmt(bm, '%.0f')), wm)
            if wm == 'bass' and fm is not None:
                worse.append('%s: 16 streams take %.0f KB each, BASS %.0f KB' % (c['what'], fm, bm))
            if ws == 'bass' and fx is not None:
                worse.append('%s: 16 streams decode at %.0f× real time, BASS %.0f×' % (c['what'], fx, bx))
            L.append('| %s | %s |' % (c['what'], ' | '.join(cells)))

    # Loudness.
    L.append('\n## Loudness (normalization)\n')
    L.append('BASS has no loudness meter: an app on BASS decodes the file and measures it with its own '
             'code. FatzAudio\'s scan (`fa_scan_loudness_path`) is checked against EBU Tech 3341 (expected '
             'values from the standard; ±0.1 LU allowed) and, for music, against a BS.1770-4 reference '
             'meter written in Python (run.py).\n')
    L.append('| Signal | Expected LUFS | FatzAudio LUFS | Error LU | FatzAudio true peak dBTP |')
    L.append('|---|---|---|---|---|')
    for r in results['loudness']:
        f = r['fa']
        lu = f.get('lufs')
        err = None if lu is None else lu - r['expected']
        L.append('| %s | %.2f | %s | %s | %s |' % (r['case'], r['expected'], fmt(lu, '%.2f', f.get('error', '–')),
                                               fmt(err, '%+.2f'), fmt(f.get('true_peak'), '%.2f')))
    if results['scan']:
        L.append('\nScan speed on long files: FatzAudio\'s complete scan against the time BASS needs only to '
                 'decode the same audio, before any measuring (times real time, higher is better).\n')
        L.append('| File | Size MB | FatzAudio scan | FatzAudio decode only | BASS decode only |')
        L.append('|---|---|---|---|---|')
        for s in results['scan']:
            dur = args.long_minutes * 60000.0
            fx = dur / s['fa_scan']['total_ms'] if 'total_ms' in s['fa_scan'] else None
            fd = dur / s['fa_decode']['total_ms'] if 'total_ms' in s['fa_decode'] else None
            bx = dur / s['bass_decode']['total_ms'] if 'total_ms' in s['bass_decode'] else None
            L.append('| %s | %.0f | %s | %s | %s |' % (s['file'], s['size_mb'], fmt(fx, '%.0f×', s['fa_scan'].get('error', '–')),
                                                   fmt(fd, '%.0f×', s['fa_decode'].get('error', '–')),
                                                   fmt(bx, '%.0f×', 'cannot open (%s)' % s['bass_decode'].get('error'))))
            if fx is not None and bx is not None and fx < bx:
                worse.append('%s: full loudness scan at %.0f× real time is slower than BASS just decoding (%.0f×)' % (s['file'], fx, bx))

    # Disc.
    L.append('\n## Disc images\n')
    b = results['disc_bass']
    L.append('A three-track `.cue` + `.bin` image (16-bit, 44.1 kHz, a 2 s pregap before track 2). basscd '
             'reads physical drives only, so BASS cannot play images: %s.\n' % (
                 'BASS_StreamCreateFile on the .cue fails (%s)' % b['error'] if 'error' in b else 'BASS opened the .cue as %s' % b.get('codec')))
    L.append('| Track | FatzAudio | Frames | Expected |')
    L.append('|---|---|---|---|')
    for i, r in enumerate(results['disc']):
        L.append('| %d | %s | %s | %s |' % (i + 1, 'bit exact' if r.get('exact') else r.get('error', 'differs'),
                                          r.get('frames', '–'), r.get('expected_frames', '–')))
        if not r.get('exact'):
            worse.append('Disc image track %d is not bit exact' % (i + 1))

    if tempo and os.path.exists(tempo):
        L.append('\n## Tempo and pitch (bass_fx)\n')
        L.append('From tools/tempo-compare, run as part of this comparison: [full tables](tempo/report.md).\n')
        with open(tempo) as f:
            t = f.read().split('\n', 1)[1]
        L.append(t.replace('## ', '### '))

    L.append('\n## Not measured here\n')
    L.append('- **Crossfades, fades and gapless joins** run on a playing device in both libraries '
             '(FatzAudio schedules them on the device clock, BASS slides attributes on its update '
             'timer), so a decode-only harness cannot time them. FatzAudio\'s sample accuracy is '
             'covered by tests/test_crossfade.c; measuring BASS needs loopback capture of a real device.')
    L.append('- **CD+G**: BASS has no CD+G support (the app renders it itself). FatzAudio\'s sync is '
             'covered by tests/test_cdg.c.')
    L.append('- **Output latency and web streams**: these need a sound device and a network; the '
             'cloud machines that ran this have neither.')

    L.insert(1, '## Where FatzAudio is worse than BASS\n\n' + ('\n'.join('- ' + w for w in worse) if worse else 'Nowhere in these measurements.') + '\n')
    with open(os.path.join(out, 'report.md'), 'w') as f:
        f.write('\n'.join(L) + '\n')


if __name__ == '__main__':
    main()
