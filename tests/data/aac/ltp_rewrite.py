# ltp_rewrite.py in.aac trace.txt out.aac seed
#
# Adds long-term prediction data to an AAC-LTP ADTS file (no CRC, one raw data
# block per frame) whose encoder wrote none: FFmpeg's encoder accepts the LTP
# profile but never predicts. Random lags, gains and band flags go into about
# half of the long-window channels, so the tests cover the LTP decoding of both
# separate and common-window channel pairs. trace.txt is the stderr of a decoder
# built with -DFA_AAC_TRACE run over in.aac (FRAME, COMMON, PRED and END bit
# positions).
import random, sys

inp, trace, outp = sys.argv[1:4]
R = random.Random(int(sys.argv[4]))
data = open(inp, 'rb').read()

frames = []
common = False
for line in open(trace):
    t = line.split()
    if t[0] == 'FRAME':
        frames.append({'pred': []})
    elif t[0] == 'COMMON':
        common = True
    elif t[0] == 'PRED':
        frames[-1]['pred'].append((int(t[1]), int(t[2]), common))
        common = False
    elif t[0] == 'END':
        frames[-1]['end'] = int(t[1])


def num(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def ltp(max_sfb):
    if R.random() < 0.5:
        return [0]
    b = [1] + num(R.randint(1, 2047), 11) + num(R.randint(0, 4), 3)   # lag, gain up to 0.98
    return b + [1 if R.random() < 0.7 else 0 for _ in range(min(max_sfb, 40))]


out = bytearray()
pos = 0
for f in frames:
    ln = ((data[pos + 3] & 3) << 11) | (data[pos + 4] << 3) | (data[pos + 5] >> 5)
    fr = data[pos:pos + ln]
    pos += ln
    bits = [(x >> (7 - i)) & 1 for x in fr for i in range(8)]
    nb, at = [], 0
    for p, max_sfb, cw in f['pred']:
        assert bits[p] == 0, 'the input already has prediction data'
        nb += bits[at:p] + [1] + ltp(max_sfb) + (ltp(max_sfb) if cw else [])
        at = p + 1
    nb += bits[at:f['end']]
    while len(nb) % 8:
        nb.append(0)
    nf = bytearray(int(''.join(map(str, nb[i:i + 8])), 2) for i in range(0, len(nb), 8))
    L = len(nf)
    nf[3] = (nf[3] & 0xFC) | (L >> 11)
    nf[4] = (L >> 3) & 0xFF
    nf[5] = (nf[5] & 0x1F) | ((L & 7) << 5)
    out += nf
assert pos == len(data)
open(outp, 'wb').write(out)
