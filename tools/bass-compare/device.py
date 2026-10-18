"""Playback on a real device, for run.py (README.md, "Playback on a real device").

Both libraries play through the default output device while loop_rec, which
belongs to neither, records what reaches it. The players print the time of every
call on the same monotonic clock that loop_rec stamps its recording with, so each
call lines up with the recorded audio:

  latency    a click started 20 times: from the play call to the click in the
             recording (median and spread), and the latency each library reports
  crossfade  a 2 s crossfade between two steady tones: when each fade starts after
             the call, how long it takes, how closely it follows its curve and how
             much the loudness dips in the middle
  gapless    one file queued after another: the gap (or overlap) at the join

The tones sit on the centres of analysis bins (468.75 and 703.125 Hz with a
1024-point window at 48 kHz), so each one's level can be followed separately
every millisecond. The recorder's own capture delay is in every latency figure,
the same for both libraries, so the difference between them is exact and the
absolute figures are an upper bound.
"""
import json
import os
import queue
import subprocess
import threading
import time

import numpy as np
from scipy import optimize
from scipy import signal as sps

RATE = 48000
WIN = 1024
HOP = 48                       # 1 ms
FA_TONE, FB_TONE = 468.75, 703.125
LEVEL = 0.25                   # -12 dBFS
TRIALS, GAP_MS = 20, 400
FADE_MS, FADE_AT_MS = 2000, 3000


def make_signals(d, write_wav):
    os.makedirs(d, exist_ok=True)
    t = np.arange(int(10 * RATE)) / RATE
    files = {}

    def save(name, x):
        p = os.path.join(d, name)
        write_wav(p, np.column_stack([x, x]), RATE, 's16')
        files[name] = p
    click = np.zeros(int(0.1 * RATE))
    n = int(0.01 * RATE)
    click[:n] = 0.5 * np.sin(2 * np.pi * 1000 * np.arange(n) / RATE + np.pi / 2)  # starts at full level
    save('click.wav', click)
    save('tone_a.wav', LEVEL * np.sin(2 * np.pi * FA_TONE * t))
    save('tone_b.wav', LEVEL * np.sin(2 * np.pi * FB_TONE * t))
    save('gap_a.wav', LEVEL * np.sin(2 * np.pi * FA_TONE * t[:3 * RATE]))
    save('gap_b.wav', LEVEL * np.sin(2 * np.pi * FB_TONE * t[:2 * RATE]))
    return files


def read_line(q, timeout):
    try:
        return q.get(timeout=timeout)
    except queue.Empty:
        return None


def record(rec, player, args, seconds, work, capture):
    """Starts loop_rec, runs the player once audio is arriving, and returns the
    player's JSON, the recording (mono) and a function from frame to time (ms)."""
    out = os.path.join(work, 'rec.f32')
    cmd = ([rec.wine] if rec.wine else []) + [rec.exe, rec.path(out), str(seconds)] + \
        (['--capture', capture] if capture else [])
    env = dict(os.environ, WINEDEBUG='-all')
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, env=env)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(l) for l in p.stdout] + [lines.put('')], daemon=True).start()

    def fail(msg):
        p.kill()
        p.wait()
        return {'error': 'recorder: ' + msg}, None, None
    first = read_line(lines, 60)
    if not first:
        return fail('no answer' if first is None else 'exited')
    first = json.loads(first)
    if 'error' in first:
        return fail(first['error'])
    res = player.run(*args)
    open(out + '.stop', 'w').close()            # the player is done: stop recording
    last = read_line(lines, seconds + 60)
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()   # everything is written; under Wine the exit sometimes hangs
        p.wait()
    for f in (out + '.stop',):
        if os.path.exists(f):
            os.remove(f)
    if not last:
        return fail('no answer' if last is None else 'exited')
    stamps = json.loads(last)
    if 'error' in stamps or not stamps.get('written') or not os.path.exists(out):
        return fail(stamps.get('error', 'recording not written'))
    if 'error' in res:
        os.remove(out)
        return res, None, None
    x = np.fromfile(out, dtype='<f4').astype(np.float64).reshape(-1, 2).mean(axis=1)
    os.remove(out)
    res['capture_device'] = stamps['device']
    peak = float(np.abs(x).max()) if len(x) else 0.0
    res['recording'] = {'seconds': len(x) / RATE, 'peak_db': 20 * np.log10(peak) if peak > 0 else None}
    if peak < 1e-4:
        res['error'] = 'nothing reached the recorder (%s)' % describe(res)
        return res, x, None
    s = np.array(stamps['stamps'], dtype=np.float64).reshape(-1, 2)
    if len(s) < 10:
        return {'error': 'recorder: no audio arrived'}, None, None
    # Callback i delivered frames up to s[i,0] at s[i,1]: frame f was captured at
    # most s[i,1] - (s[i,0] - f) / rate. The callbacks least delayed by scheduling
    # give the closest bound, so each stamp takes the lowest bound among its
    # neighbours (about a quarter second). Taking it locally keeps the map right
    # across a break in the capture (WASAPI loopback delivers nothing while
    # nothing plays). Both clocks run at the nominal rate to well within 0.1 ms
    # over one test.
    ms_per_frame = 1000.0 / RATE
    bound = s[:, 1] - s[:, 0] * ms_per_frame
    k = 25
    local = np.array([bound[max(0, i - k): i + k + 1].min() for i in range(len(bound))])
    return res, x, (lambda f: np.interp(f, s[:, 0], local) + np.asarray(f) * ms_per_frame)


def describe(res):
    r = res.get('recording', {})
    return '%.1f s recorded, loudest %s' % (r.get('seconds', 0), 'silence' if r.get('peak_db') is None
                                             else '%.0f dBFS' % r['peak_db'])


def level(e, keep):
    """The steady level of a tone over `keep` (its median), or 0."""
    return float(np.median(e[keep])) if keep.any() else 0.0


def envelope(x, freq):
    """Level of the tone at `freq`, one value per HOP frames (sample k*HOP is the
    centre of the window), scaled so a sine of amplitude a reads a."""
    w = np.hanning(WIN)
    k = w * np.exp(-2j * np.pi * freq * np.arange(WIN) / RATE)
    y = sps.fftconvolve(x, k[::-1], mode='valid')[::HOP]
    return np.abs(y) * 2 / w.sum(), np.arange(len(y)) * HOP + WIN // 2


def cross50(e, start, rising):
    """First index from `start` where e crosses 0.5 (linear interpolation)."""
    above = e[start:] >= 0.5 if rising else e[start:] < 0.5
    i = int(np.argmax(above)) + start
    if not above.any() or i == 0:
        return None
    return i - 1 + (0.5 - e[i - 1]) / (e[i] - e[i - 1])


def dropout(ga, gb, keep):
    """True when the two tones together fall far below full level somewhere in
    `keep`: the recording lost audio (a virtual or overloaded device glitched),
    which would read as a bad fade or a gap. The quietest a fade gets is -3 dB."""
    return bool(keep.any() and (ga[keep] ** 2 + gb[keep] ** 2).min() < 0.3)


def latency(x, ftime, res):
    if 'calls' not in res:
        return res
    on = np.abs(x) > 0.3 * np.abs(x).max()      # the click, whatever the recording level
    t_frames = ftime(np.arange(len(x)))
    lat = []
    for tc in res['calls']:
        i0 = int(np.searchsorted(t_frames, tc))
        hit = np.flatnonzero(on[i0:i0 + int((GAP_MS - 50) * RATE / 1000)])   # before the next click
        lat.append(None if not len(hit) else ftime(i0 + hit[0]) - tc)
    # The first start after opening the device also wakes the device up; it is
    # reported on its own and left out of the rest.
    got = [v for v in lat[1:] if v is not None]
    res.update({'latency_ms': lat, 'first_ms': lat[0] if lat else None, 'heard': len(got), 'trials': len(lat) - 1})
    if not got:
        res['error'] = 'no click within %d ms of any start (%s)' % (GAP_MS - 50, describe(res))
    if got:
        res.update({'median_ms': float(np.median(got)), 'min_ms': float(np.min(got)),
                    'max_ms': float(np.max(got)), 'spread_ms': float(np.percentile(got, 90) - np.percentile(got, 10))})
    del res['calls']
    return res


def crossfade(x, ftime, res):
    if 'fade' not in res:
        return res
    ea, centre = envelope(x, FA_TONE)
    eb, _ = envelope(x, FB_TONE)
    t = ftime(centre) - res['fade']                # ms after the crossfade call
    # Levels relative to each tone's own steady level, so the recording level
    # (system volume, a microphone) does not matter.
    ref_a = level(ea, (t > -1000) & (t < -100))
    ref_b = level(eb, (t > FADE_MS + 200) & (t < FADE_MS + 700))
    if ref_a < 1e-4 or ref_a < 0.1 * ea.max():
        res['error'] = 'first stream not heard where expected (%s)' % describe(res)
        return res
    if ref_b < 1e-4 or ref_b < 0.1 * eb.max():
        res['error'] = 'second stream not heard where expected (%s)' % describe(res)
        return res
    ga, gb = ea / ref_a, eb / ref_b
    equal = res.get('curve') == 'equal'

    def shape(u):
        u = np.clip(u, 0, 1)
        return np.sin(u * np.pi / 2) if equal else u
    region = (t > -500) & (t < FADE_MS + 800)   # the players stop 1 s after the fade
    tt = t[region]
    fits = {}
    for name, g, rising in (('out', ga, False), ('in', gb, True)):
        gg = g[region]

        def err(p):
            s, d = p
            u = (tt - s) / max(d, 1)
            return (shape(u) if rising else shape(1 - u)) - gg
        guess_start = tt[int(np.argmax(gg > 0.1))] if rising else tt[int(np.argmax(gg < 0.9))]
        f = optimize.least_squares(err, [guess_start, FADE_MS], x_scale=[10, 100])
        fits[name] = {'start_ms': float(f.x[0]), 'duration_ms': float(f.x[1]),
                      'residual': float(np.sqrt(np.mean(f.fun ** 2)))}
    if dropout(ga, gb, (t > -1000) & (t < FADE_MS + 800)):
        res['error'] = 'dropout in the recording'
        return res
    power = ga ** 2 + gb ** 2
    during = (t > fits['out']['start_ms']) & (t < fits['out']['start_ms'] + fits['out']['duration_ms'])
    res.update({'fade_out': fits['out'], 'fade_in': fits['in'],
                'skew_ms': fits['in']['start_ms'] - fits['out']['start_ms'],
                'dip_db': float(10 * np.log10(max(power[during].min(), 1e-9))) if during.any() else None,
                'requested_ms': FADE_MS})
    return res


def gapless(x, ftime, res):
    if 'play' not in res:
        return res
    ea, centre = envelope(x, FA_TONE)
    eb, _ = envelope(x, FB_TONE)
    if ea.max() < 1e-4 or eb.max() < 1e-4:
        res['error'] = '%s file not heard (%s)' % ('first' if ea.max() < 1e-4 else 'second', describe(res))
        return res
    ga = ea / level(ea, ea > 0.5 * ea.max())   # relative to each file's steady level
    gb = eb / level(eb, eb > 0.5 * eb.max())
    on = int(np.argmax(ga > 0.5))
    rise = cross50(gb, on, True)
    if rise is None:
        res['error'] = 'second file not heard (%s)' % describe(res)
        return res
    # The first file's end: the last time it is at half level (it plays once, and
    # a dropout inside it is caught below).
    fall = int(np.flatnonzero(ga >= 0.5)[-1])
    if fall + 1 >= len(ga):
        res['error'] = 'first file did not end'
        return res
    fall = fall + (ga[fall] - 0.5) / (ga[fall] - ga[fall + 1])
    n = RATE // HOP                            # 1 s of envelope after the join
    keep = np.zeros(len(ga), bool)
    keep[on + 30: int(fall) - 30] = True       # all of the first file, away from its edges
    keep[int(rise) + 30: int(rise) + n] = True  # the second file
    if dropout(ga, gb, keep):
        res['error'] = 'dropout in the recording'
        return res
    res['gap_ms'] = (rise - fall) * HOP * 1000.0 / RATE
    # Silent samples at the join: the exact length of a gap at sample level.
    mid = int(round((fall + rise) / 2 * HOP + WIN // 2))
    seg = np.abs(x[max(0, mid - RATE // 10): mid + RATE // 10]) < 1e-3
    best, cur = 0, 0
    for v in seg:
        cur = cur + 1 if v else 0
        best = max(best, cur)
    res['silent_frames'] = best if best > 2 else 0
    res['start_ms'] = ftime(on * HOP + WIN // 2) - res['play']
    return res


def run(args, fa, bass, rec, inputs, work, write_wav):
    files = make_signals(os.path.join(inputs, 'device'), write_wav)
    kept = os.path.join(os.path.dirname(inputs), 'failed-recordings')
    wine_slack = 15 if (rec.wine or bass.wine or fa.wine) else 5   # Wine takes seconds to start a program
    tests = [
        ('latency', latency, (TRIALS + 1) * GAP_MS / 1000 + 1,
         lambda t: ['play-latency', files['click.wav'], TRIALS + 1, GAP_MS]),
        ('crossfade linear', crossfade, (FADE_AT_MS + FADE_MS) / 1000 + 2,
         lambda t: ['play-crossfade', files['tone_a.wav'], files['tone_b.wav'], FADE_MS, FADE_AT_MS, 'linear']),
        ('crossfade equal power', crossfade, (FADE_AT_MS + FADE_MS) / 1000 + 2,
         lambda t: ['play-crossfade', files['tone_a.wav'], files['tone_b.wav'], FADE_MS, FADE_AT_MS, 'equal']),
        ('gapless', gapless, 6.5, lambda t: ['play-gapless', files['gap_a.wav'], files['gap_b.wav']]),
        ('gapless normal sync', gapless, 6.5,
         lambda t: ['play-gapless', files['gap_a.wav'], files['gap_b.wav'], 'normal']),
    ]
    results = {}
    for name, analyse, seconds, argv in tests:
        row = {}
        for tool in (fa, bass):
            if tool is fa and name == 'gapless normal sync':
                continue                            # a BASS-only variant
            if tool is bass and name == 'crossfade equal power':
                row['bass'] = {'error': 'BASS has no equal-power slide'}
                continue
            for attempt in range(3):              # a recording with a dropout is taken again
                print('device %-22s %s' % (name, tool.name), flush=True)
                res, x, ftime = record(rec, tool, argv(tool), seconds + wine_slack, work, args.capture)
                if 'error' not in res:
                    res = analyse(x, ftime, res)
                if 'error' in res and x is not None:
                    # Kept for a look: what the recorder got when a measurement failed.
                    os.makedirs(kept, exist_ok=True)
                    p = os.path.join(kept, '%s-%s.wav' % (name.replace(' ', '-'), tool.name))
                    write_wav(p, np.column_stack([x, x]), RATE, 's16')
                    res['kept_recording'] = p
                res['attempts'] = attempt + 1
                time.sleep(0.5)
                if res.get('error') != 'dropout in the recording':
                    break
            row[tool.name] = res
        results[name] = row
    return results


def fmt(v, spec='%.1f', none='–'):
    return none if v is None else spec % v


def failures(results):
    """The measurements that did not produce a result."""
    return ['%s (%s): %s' % (name, lib, r['error']) for name, row in results.items() for lib, r in row.items()
            if 'error' in r and r['error'] != 'BASS has no equal-power slide']


def report(results, worse):
    L = ['\n## Playback on a real device\n']
    lat = results.get('latency', {})
    dev = next((r.get('capture_device') for row in results.values() for r in row.values() if r.get('capture_device')), None)
    L.append('Both libraries played through the default output device while `loop_rec` recorded it%s. '
             'Times are from the call in the app to the sound in the recording, on one clock. The '
             'recorder\'s own capture delay is included in every figure and is the same for both, so '
             'the difference between the libraries is exact. BASS ran with `BASS_Init(-1, 48000, '
             'BASS_DEVICE_LATENCY)` and its default buffer settings; FatzAudio with its defaults.\n'
             % (' (%s)' % dev if dev else ''))

    L.append('### Start latency\n')
    L.append('A click started %d times (`fa_stream_play` / `BASS_ChannelPlay`), %d ms apart. Lower is '
             'better; the spread is between the 10th and 90th percentile. The first start after opening '
             'the device is shown on its own (it can include the device waking up) and left out of the '
             'other columns.\n' % (TRIALS, GAP_MS))
    L.append('| | Median ms | Fastest | Slowest | Spread | Heard | First start | Latency the library reports |')
    L.append('|---|---|---|---|---|---|---|---|')
    for name, label in (('fatzaudio', 'FatzAudio'), ('bass', 'BASS')):
        r = lat.get(name, {})
        if 'error' in r:
            L.append('| %s | %s | | | | | | |' % (label, r['error']))
            continue
        L.append('| %s | %s | %s | %s | %s | %s of %s | %s | %s ms |' % (
            label, fmt(r.get('median_ms')), fmt(r.get('min_ms')), fmt(r.get('max_ms')), fmt(r.get('spread_ms')),
            r.get('heard', 0), r.get('trials', 0), fmt(r.get('first_ms'), '%.1f', 'not heard'),
            fmt(r.get('reported_latency_ms'), '%.1f')))
    f, b = lat.get('fatzaudio', {}), lat.get('bass', {})
    if f.get('median_ms') is not None and b.get('median_ms') is not None:
        if f['median_ms'] > b['median_ms'] + 2:
            worse.append('Start latency: %.1f ms from play to sound, BASS %.1f ms' % (f['median_ms'], b['median_ms']))
        if f['spread_ms'] > b['spread_ms'] + 2:
            worse.append('Start latency varies by %.1f ms, BASS %.1f ms' % (f['spread_ms'], b['spread_ms']))

    L.append('\n### Crossfade\n')
    L.append('A %d ms crossfade between two steady tones (`fa_crossfade`; with BASS, the new stream '
             'starts at volume 0 and `BASS_ChannelSlideAttribute` slides both volumes). Start is when '
             'each fade began after the call; skew is how far apart the two began (0 is right); '
             'curve error is how far the level strayed from the curve asked for (RMS, fraction of full '
             'level); the dip is the loudness at the quietest point of the fade (0 dB for equal power, '
             '-3 dB is what a linear fade gives). BASS slides are linear only.\n' % FADE_MS)
    L.append('| Curve | Library | Fade-out starts ms | Fade-in starts ms | Skew ms | Duration ms | Curve error | Dip dB |')
    L.append('|---|---|---|---|---|---|---|---|')
    for name in ('crossfade linear', 'crossfade equal power'):
        for lib, label in (('fatzaudio', 'FatzAudio'), ('bass', 'BASS')):
            r = results.get(name, {}).get(lib, {})
            curve = name.split(' ', 1)[1]
            if 'error' in r:
                L.append('| %s | %s | %s | | | | | |' % (curve, label, r['error']))
                continue
            if 'fade_out' not in r:
                continue
            fo, fi = r['fade_out'], r['fade_in']
            L.append('| %s | %s | %s | %s | %s | %s / %s | %.3f / %.3f | %s |' % (
                curve, label, fmt(fo['start_ms']), fmt(fi['start_ms']), fmt(r['skew_ms'], '%+.1f'),
                fmt(fo['duration_ms'], '%.0f'), fmt(fi['duration_ms'], '%.0f'), fo['residual'], fi['residual'],
                fmt(r['dip_db'], '%+.1f')))
    rf = results.get('crossfade linear', {}).get('fatzaudio', {})
    rb = results.get('crossfade linear', {}).get('bass', {})
    if 'skew_ms' in rf and 'skew_ms' in rb:
        if abs(rf['skew_ms']) > abs(rb['skew_ms']) + 1:
            worse.append('Crossfade: the two fades start %.1f ms apart, BASS %.1f ms' % (rf['skew_ms'], rb['skew_ms']))
        for k in ('fade_out', 'fade_in'):
            if abs(rf[k]['duration_ms'] - FADE_MS) > abs(rb[k]['duration_ms'] - FADE_MS) + 5:
                worse.append('Crossfade: %s lasts %.0f ms of %d, BASS %.0f ms' % (
                    k.replace('_', '-'), rf[k]['duration_ms'], FADE_MS, rb[k]['duration_ms']))
            if rf[k]['residual'] > rb[k]['residual'] + 0.01:
                worse.append('Crossfade: %s strays %.3f from its curve, BASS %.3f' % (
                    k.replace('_', '-'), rf[k]['residual'], rb[k]['residual']))
        if rf['fade_out']['start_ms'] > rb['fade_out']['start_ms'] + 5:
            worse.append('Crossfade: starts %.1f ms after the call, BASS %.1f ms' % (
                rf['fade_out']['start_ms'], rb['fade_out']['start_ms']))

    L.append('\n### Gapless\n')
    L.append('A 3 s file with a second file queued after it: `fa_stream_set_next` with no crossfade; with '
             'BASS, a `BASS_SYNC_END` sync on the first stream starts the second (with and without '
             '`BASS_SYNC_MIXTIME`). The gap is from the first file\'s end to the second\'s start in the '
             'recording (0 is gapless, negative is an overlap); silent frames counts samples of silence at '
             'the join.\n')
    L.append('| Library | Gap ms | Silent frames at the join |')
    L.append('|---|---|---|')
    for name, lib, label in (('gapless', 'fatzaudio', 'FatzAudio'), ('gapless', 'bass', 'BASS, mixtime sync'),
                             ('gapless normal sync', 'bass', 'BASS, normal sync')):
        r = results.get(name, {}).get(lib, {})
        if not r:
            continue
        if 'error' in r:
            L.append('| %s | %s | |' % (label, r['error']))
            continue
        L.append('| %s | %s | %s |' % (label, fmt(r.get('gap_ms'), '%+.2f'), r.get('silent_frames', '–')))
    gf = results.get('gapless', {}).get('fatzaudio', {}).get('gap_ms')
    gb = [results.get(n, {}).get('bass', {}).get('gap_ms') for n in ('gapless', 'gapless normal sync')]
    gb = [abs(v) for v in gb if v is not None]
    if gf is not None and gb and abs(gf) > min(gb) + 0.5:
        worse.append('Gapless: a %.2f ms gap at the join, BASS %.2f ms' % (gf, min(gb)))
    return L
