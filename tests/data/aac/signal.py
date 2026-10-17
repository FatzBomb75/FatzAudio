# signal.py out.wav rate channels seconds: the test signal of make_aac_data.sh, a
# different harmonic tone per channel with a slow tremolo, a sweep up to a
# quarter of the rate, noise and decaying 3 kHz clicks (transients), so every
# channel has its own content and both long and short blocks get used.
import numpy as np, wave, sys

def make(path, rate, ch, secs, seed=1):
    rng = np.random.default_rng(seed)
    t = np.arange(int(rate * secs)) / rate
    out = []
    for c in range(ch):
        f0 = [220, 277.18, 329.63, 440, 110, 164.8, 196, 246.9][c % 8]
        x = np.zeros_like(t)
        for h in range(1, 12):
            x += (0.25 / h) * np.sin(2 * np.pi * f0 * h * t + c + h) * (0.6 + 0.4 * np.sin(2 * np.pi * (0.3 + 0.1 * c) * t))
        sweep = 0.1 * np.sin(2 * np.pi * (100 * t + (rate / 4 - 100) / (2 * secs) * t * t))
        noise = rng.standard_normal(len(t)) * 0.05 * (1 + np.sin(2 * np.pi * 0.2 * t + c))
        clicks = np.zeros_like(t)
        period = int(rate * (0.11 + 0.03 * c))
        for k in range(period // 3, len(t) - 400, period):
            n = np.arange(400)
            clicks[k:k + 400] += 0.7 * np.exp(-n / 40.0) * np.sin(2 * np.pi * 3000 * n / rate) * rng.choice([-1, 1])
        out.append(x + sweep + noise + clicks)
    y = np.stack(out, 1)
    y = y / np.max(np.abs(y)) * 0.8
    pcm = (y * 32767).astype('<i2')
    w = wave.open(path, 'wb')
    w.setnchannels(ch)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(pcm.tobytes())
    w.close()

make(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4]))
