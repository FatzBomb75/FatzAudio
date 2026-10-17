#!/usr/bin/env python3
"""Black-box comparison of tempo and pitch quality: FatzAudio against bass_fx (and
SoundTouch / Rubber Band when their command-line tools are installed).

Every engine renders the same test signals; each result is scored against the
ideal result, which for these signals is known exactly:

  tone    a 440 Hz sine: the share of the output that is not a clean sine at the
          expected frequency (phasing, warble, splice noise), as signal-to-error
          dB in 4096-frame windows. Higher is better.
  chord   the same for a C major triad (three sines). Higher is better.
  voice   a 220 Hz tone with 12 harmonics (a held, sung vowel), scored like
          tone over all its harmonics. Higher is better.
  cents   the pitch error of the tone, in cents. Closer to 0 is better.
  attack  drum-like bursts: how much sound leaks in before each hit (pre-echo,
          dB below the hit). Lower is better.
  timing  the same bursts: how far each hit lands from where it belongs, in ms.
          Lower is better.
  length  output length against the exact expected length, in ms.

Usage (Windows, from a build folder; Python 3 with numpy):
  python compare.py --fa bin\\Release\\fa_tempo_render.exe \\
      --bassfx bin\\Release\\bassfx_render.exe --bass-dir C:\\path\\to\\legacy --out results
Linux with wine: add --wine wine64.

Writes results/report.md and every rendered WAV (results/renders) for listening.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

import numpy as np

RATE = 48000


# ---- WAV ---------------------------------------------------------------------

def write_wav16(path, x):
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 1:
        x = np.stack([x, x], 1)
    pcm = np.clip(np.round(x * 32767), -32768, 32767).astype('<i2')
    data = pcm.tobytes()
    ch = x.shape[1]
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ')
        f.write(struct.pack('<IHHIIHH', 16, 1, ch, RATE, RATE * 2 * ch, 2 * ch, 16))
        f.write(b'data' + struct.pack('<I', len(data)) + data)


def read_wav(path):
    """Channel 0 of a PCM16/24/32 or float32 WAV, as float64, and the rate."""
    with open(path, 'rb') as f:
        b = f.read()
    if b[:4] != b'RIFF' or b[8:12] != b'WAVE':
        raise ValueError(path + ': not a WAV file')
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack('<I', b[pos + 4:pos + 8])[0]
        body = b[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
            if fmt[0] == 0xFFFE and len(body) >= 26:          # WAVE_FORMAT_EXTENSIBLE
                fmt = (struct.unpack('<H', body[24:26])[0],) + fmt[1:]
        elif cid == b'data':
            data = body if size != 0xFFFFFFFF and size else b[pos + 8:]
        pos += 8 + size + (size & 1)
    tag, ch, rate, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(data[:len(data) // 4 * 4], dtype='<f4').astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(data[:len(data) // 2 * 2], dtype='<i2') / 32768.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(data[:len(data) // 4 * 4], dtype='<i4') / 2147483648.0
    elif tag == 1 and bits == 24:
        n = len(data) // 3
        a = np.frombuffer(data[:n * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / 8388608.0
    else:
        raise ValueError('%s: unsupported WAV format %d/%d bits' % (path, tag, bits))
    return x[: len(x) // ch * ch].reshape(-1, ch)[:, 0], rate


# ---- test signals ----------------------------------------------------------------

def sig_tone(sec=6.0):
    t = np.arange(int(sec * RATE)) / RATE
    return 0.5 * np.sin(2 * np.pi * 440 * t)


CHORD = (261.63, 329.63, 392.00)


def sig_chord(sec=6.0):
    t = np.arange(int(sec * RATE)) / RATE
    return sum(0.25 * np.sin(2 * np.pi * f * t) for f in CHORD)


VOICE_F0, VOICE_H = 220.0, 12


def sig_voice(sec=6.0):
    t = np.arange(int(sec * RATE)) / RATE
    return sum(0.6 / h * np.sin(2 * np.pi * VOICE_F0 * h * t) for h in range(1, VOICE_H + 1)) * 0.5


HIT_EVERY = 0.3


def sig_hits(sec=6.0):
    rng = np.random.default_rng(7)
    x = np.zeros(int(sec * RATE))
    n = int(0.08 * RATE)
    env = np.exp(-np.arange(n) / (0.012 * RATE))
    for k in range(int(sec / HIT_EVERY)):
        at = int((k * HIT_EVERY + 0.1) * RATE)
        x[at:at + n] += 0.6 * env * rng.standard_normal(n)
    return x


SIGNALS = {'tone': sig_tone, 'chord': sig_chord, 'voice': sig_voice, 'hits': sig_hits}


# ---- measurements ------------------------------------------------------------------

def fit_score(y, freqs, win=4096):
    """Mean signal-to-error dB of a least-squares fit of sines at `freqs`, in windows."""
    skip = int(0.4 * RATE)
    t = np.arange(win) / RATE
    basis = [np.ones(win)]
    for f in freqs:
        basis += [np.sin(2 * np.pi * f * t), np.cos(2 * np.pi * f * t)]
    a = np.stack(basis, 1)
    scores = []
    for at in range(skip, len(y) - skip - win, win):
        seg = y[at:at + win]
        coef, *_ = np.linalg.lstsq(a, seg, rcond=None)
        fit = a @ coef
        scores.append(10 * np.log10(np.sum(fit ** 2) / (np.sum((seg - fit) ** 2) + 1e-30)))
    return float(np.mean(scores)) if scores else float('nan')


def measure_cents(y, want):
    """Average frequency from rising zero crossings over the steady part."""
    seg = y[int(0.5 * RATE): len(y) - int(0.5 * RATE)]
    zc = np.where((seg[:-1] < 0) & (seg[1:] >= 0))[0]
    if len(zc) < 10:
        return float('nan')
    t = zc + (-seg[zc]) / (seg[zc + 1] - seg[zc])
    f = RATE * (len(t) - 1) / (t[-1] - t[0])
    return 1200 * np.log2(f / want)


def hits(y, tempo):
    """Drum-like bursts: (median pre-echo dB, median timing error ms). Each hit is
    found near where it belongs (the first 1 ms frame within +-40 ms reaching half
    its peak); pre-echo is the loudest 1 ms of the 25 ms before it, relative to
    the hit's peak. Time-domain stretchers keep attacks sharp but move hits
    around; phase vocoders keep the timing but smear a little sound ahead."""
    ms = RATE // 1000
    env = np.sqrt(np.convolve(y ** 2, np.ones(ms) / ms, mode='same'))
    echo, timing = [], []
    k = 1
    while True:
        want = int((k * HIT_EVERY + 0.1) / tempo * RATE)
        if want + 80 * ms >= len(y) - RATE // 2:
            break
        lo, hi = want - 40 * ms, want + 40 * ms
        peak = env[lo:hi + 30 * ms].max()
        onset = lo + int(np.argmax(env[lo:hi] >= 0.5 * peak))
        before = env[onset - 28 * ms: onset - 3 * ms]
        echo.append(20 * np.log10((before.max() + 1e-9) / (peak + 1e-9)))
        timing.append(abs(onset - want) * 1000.0 / RATE)
        k += 1
    if not echo:
        return float('nan'), float('nan')
    return float(np.median(echo)), float(np.median(timing))


def score(signal, y, tempo, pitch, src_len):
    p = 2 ** (pitch / 12.0)
    res = {'length': (len(y) - src_len / tempo) * 1000.0 / RATE}
    if signal == 'tone':
        res['tone'] = fit_score(y, [440 * p])
        res['cents'] = measure_cents(y, 440 * p)
    elif signal == 'chord':
        res['chord'] = fit_score(y, [f * p for f in CHORD])
    elif signal == 'voice':
        res['voice'] = fit_score(y, [VOICE_F0 * h * p for h in range(1, VOICE_H + 1) if VOICE_F0 * h * p < 20000])
    elif signal == 'hits':
        res['attack'], res['timing'] = hits(y, tempo)
    return res


# ---- engines -----------------------------------------------------------------------

def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise RuntimeError(' '.join(cmd) + '\n' + r.stderr.decode(errors='replace')[-2000:])


def winpath(path, wine):
    path = os.path.abspath(path)
    return 'Z:' + path.replace('/', '\\') if wine else path


def engines(args):
    found = []
    if args.fa:
        found.append(('FatzAudio', lambda i, o, t, p: run([args.fa, i, o, '--tempo', str(t), '--pitch', str(p)])))
    if args.bassfx and args.bass_dir:
        def bassfx(i, o, t, p):
            cmd = [args.bassfx, winpath(args.bass_dir, args.wine), winpath(i, args.wine), winpath(o, args.wine),
                   '--tempo', str(t), '--pitch', str(p)]
            run(([args.wine] if args.wine else []) + cmd)
        found.append(('bass_fx', bassfx))
    st = args.soundstretch or shutil.which('soundstretch')
    if st:
        found.append(('SoundTouch', lambda i, o, t, p: run([st, i, o, '-tempo=%g' % ((t - 1) * 100), '-pitch=%g' % p])))
    rb = args.rubberband or shutil.which('rubberband')
    if rb:
        found.append(('Rubber Band', lambda i, o, t, p: run([rb, '-q', '-t', '%g' % (1 / t), '-p', '%g' % p, i, o])))
    return found


CASES = [
    ('tone', 0.8, 0), ('tone', 1.25, 0), ('tone', 1, -3), ('tone', 1, 3), ('tone', 1, -12), ('tone', 1.15, 2),
    ('chord', 0.8, 0), ('chord', 1.25, 0), ('chord', 1, -3), ('chord', 1, 5),
    ('voice', 0.9, 0), ('voice', 1.2, 0), ('voice', 1, -4), ('voice', 1, 4),
    ('hits', 0.8, 0), ('hits', 1.25, 0), ('hits', 1, -3),
]

METRICS = [('tone', 'tone dB', True), ('chord', 'chord dB', True), ('voice', 'voice dB', True),
           ('cents', 'pitch error (cents)', None), ('attack', 'pre-echo before hits, dB', False),
           ('timing', 'hits moved from their time, ms', False), ('length', 'length error (ms)', None)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fa', help='fa_tempo_render executable')
    ap.add_argument('--bassfx', help='bassfx_render.exe')
    ap.add_argument('--bass-dir', help='folder with bass.dll and bass_fx.dll')
    ap.add_argument('--wine', help='run bassfx_render.exe through this (e.g. wine64)')
    ap.add_argument('--soundstretch', help='SoundTouch command line (found on PATH if not given)')
    ap.add_argument('--rubberband', help='Rubber Band command line (found on PATH if not given)')
    ap.add_argument('--out', default='tempo-compare-results')
    args = ap.parse_args()

    sig_dir, ren_dir = os.path.join(args.out, 'signals'), os.path.join(args.out, 'renders')
    os.makedirs(sig_dir, exist_ok=True)
    os.makedirs(ren_dir, exist_ok=True)
    lengths = {}
    for name, gen in SIGNALS.items():
        x = gen()
        lengths[name] = len(x)
        write_wav16(os.path.join(sig_dir, name + '.wav'), x)

    eng = engines(args)
    if not eng:
        sys.exit('no engine to run: give --fa and/or --bassfx with --bass-dir')
    results = {}
    for name, tempo, pitch in CASES:
        src = os.path.join(sig_dir, name + '.wav')
        for ename, render in eng:
            out = os.path.join(ren_dir, '%s_t%g_p%+g_%s.wav' % (name, tempo, pitch, ename.replace(' ', '')))
            try:
                render(src, out, tempo, pitch)
                y, _ = read_wav(out)
                results[(name, tempo, pitch, ename)] = score(name, y, tempo, pitch, lengths[name])
            except Exception as e:  # noqa: BLE001 - report and go on
                print('%s %s t=%g p=%g failed: %s' % (ename, name, tempo, pitch, e), file=sys.stderr)
        print('%-6s tempo %-5g pitch %+g done' % (name, tempo, pitch))

    names = [e for e, _ in eng]
    lines = ['# Tempo and pitch: black-box comparison', '',
             'Each engine renders the same 6 s test signals (48 kHz). Scores compare the output with the '
             'ideal result (see the metric list at the top of compare.py).', '']
    for key, label, higher in METRICS:
        rows = [(c, results.get(c[:3] + (e,), {}).get(key)) for c in CASES for e in names]
        if not any(v is not None for _, v in rows):
            continue
        lines += ['## ' + label + ('' if higher is None else ' (higher is better)' if higher else ' (lower is better)'), '',
                  '| signal | tempo | pitch | ' + ' | '.join(names) + ' |',
                  '|---|---|---|' + '---|' * len(names)]
        for name, tempo, pitch in CASES:
            vals = [results.get((name, tempo, pitch, e), {}).get(key) for e in names]
            if all(v is None for v in vals):
                continue
            fmt = '%.2f' if key == 'cents' else '%.1f'
            # Compared as printed, so a tie on the shown value is a tie; no bold when all tie.
            shown = [None if v is None or v != v else float(fmt % v) + 0.0 for v in vals]
            num = [v for v in shown if v is not None]
            best = None
            if len(set(num)) > 1:
                best = (max(num) if higher else min(num)) if higher is not None else min(num, key=abs)
            cells = []
            for v in shown:
                if v is None:
                    cells.append('-')
                else:
                    s = fmt % v
                    cells.append('**%s**' % s if best is not None and (v == best or (higher is None and abs(v) == abs(best))) else s)
            lines.append('| %s | %g | %+g | %s |' % (name, tempo, pitch, ' | '.join(cells)))
        lines.append('')
    with open(os.path.join(args.out, 'report.md'), 'w') as f:
        f.write('\n'.join(lines))
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
