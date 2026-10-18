# ps_rewrite.py in.aac trace.txt out.aac seed iid_mode icc_mode ipdopd
#
# Replaces the parametric stereo data of a mono HE-AAC v2 ADTS file (one SCE, its
# SBR in a fill element, no CRC) with random parameters in any mode, so the tests
# cover what encoders seldom write: 34-band and fine-resolution modes, mixing
# procedure R_B, variable envelope borders, time and frequency delta coding, and
# IPD/OPD phase data. trace.txt is the stderr of a decoder built with
# -DFA_AAC_TRACE run over in.aac (FRAME, FIL and EXT bit positions).
# ps_codes.json holds the PS Huffman codes (ISO/IEC 14496-3 Annex 8.B) as
# value -> [code, length].
import copy, json, os, random, sys

inp, trace, outp = sys.argv[1:4]
seed, iid_mode, icc_mode, ipdopd = (int(v) for v in sys.argv[4:8])
R = random.Random(seed)
C = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ps_codes.json')))
data = open(inp, 'rb').read()

frames = []
for line in open(trace):
    t = line.split()
    if t[0] == 'FRAME':
        frames.append({})
    elif t[0] == 'FIL' and 'ext' not in frames[-1]:
        frames[-1]['fil'] = int(t[1])
    elif t[0] == 'EXT':
        frames[-1]['ext'] = int(t[1])

NPAR = [10, 20, 34, 10, 20, 34]
NIPD = [5, 11, 17, 5, 11, 17]
NUMENV = [[0, 1, 2, 4], [1, 2, 3, 4]]
state = {'iid': [0] * 34, 'icc': [0] * 34, 'ipd': [0] * 17, 'opd': [0] * 17}


def huff(name, v):
    c, l = C[name][str(v)]
    return [(c >> (l - 1 - i)) & 1 for i in range(l)]


def num(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def walk(prev, lo, hi, step, mod=None):
    out = []
    for p in prev:
        v = p + R.randint(-step, step)
        if R.random() < 0.05:
            v = R.randint(lo, hi)
        out.append(v % mod if mod else max(lo, min(hi, v)))
    return out


def code_par(key, vals, fname, tname, mod=None):
    prev = state[key][:len(vals)]
    dt = R.random() < 0.4
    bits = [1 if dt else 0]
    for i, v in enumerate(vals):
        ref = prev[i] if dt else (vals[i - 1] if i else 0)
        bits += huff(tname if dt else fname, (v - ref) % mod if mod else v - ref)
    state[key][:len(vals)] = vals
    return bits


def ps_bits():
    b = [1, 1] + num(iid_mode, 3) + [1] + num(icc_mode, 3) + [1 if ipdopd else 0]
    fc = R.randint(0, 1)
    idx = R.randint(0, 3)
    ne = NUMENV[fc][idx]
    b += [fc] + num(idx, 2)
    if fc:
        for x in sorted(R.sample(range(32), ne)):
            b += num(x, 5)
    fine = iid_mode > 2
    for e in range(ne):
        vals = walk(state['iid'][:NPAR[iid_mode]], -15 if fine else -7, 15 if fine else 7, 3)
        b += code_par('iid', vals, 'f_huff_iid_fine' if fine else 'f_huff_iid_def',
                      't_huff_iid_fine' if fine else 't_huff_iid_def')
    for e in range(ne):
        b += code_par('icc', walk(state['icc'][:NPAR[icc_mode]], 0, 7, 2), 'f_huff_icc', 't_huff_icc')
    if ipdopd:
        ext = [0, 0, 1]
        for e in range(ne):
            n = NIPD[iid_mode]
            ext += code_par('ipd', walk(state['ipd'][:n], 0, 7, 2, 8), 'f_huff_ipd', 't_huff_ipd', 8)
            ext += code_par('opd', walk(state['opd'][:n], 0, 7, 2, 8), 'f_huff_opd', 't_huff_opd', 8)
        ext += [0]
        while len(ext) % 8:
            ext.append(0)
        cnt = len(ext) // 8
        b += (num(cnt, 4) if cnt < 15 else num(15, 4) + num(cnt - 15, 8)) + ext
    return b


def tobits(bs):
    return [(x >> (7 - i)) & 1 for x in bs for i in range(8)]


def tobytes(b):
    return bytes(int(''.join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))


out = bytearray()
pos = 0
fi = 0
while pos + 7 <= len(data):
    ln = ((data[pos + 3] & 3) << 11) | (data[pos + 4] << 3) | (data[pos + 5] >> 5)
    fr = data[pos:pos + ln]
    pos += ln
    f = frames[fi]
    fi += 1
    if 'ext' not in f:
        out += fr
        continue
    bits = tobits(fr)
    fil, ext = f['fil'], f['ext']
    cnt = int(''.join(map(str, bits[fil:fil + 4])), 2)
    payload = bits[fil + (4 if cnt < 15 else 12):ext]   # extension type + SBR data before bs_extended_data
    for tries in range(100):
        saved = copy.deepcopy(state)
        sext = [1, 0] + ps_bits()                         # EXTENSION_ID_PS
        while len(sext) % 8:
            sext.append(0)
        n = len(sext) // 8
        if n + len(payload) // 8 + 4 <= 268:              # a fill element holds at most 269 bytes
            break
        state.clear()
        state.update(saved)
    else:
        sys.exit('PS data too big')
    pl = payload + [1] + (num(n, 4) if n < 15 else num(15, 4) + num(n - 15, 8)) + sext
    while len(pl) % 8:
        pl.append(0)
    n = len(pl) // 8
    nb = bits[:fil] + (num(n, 4) if n < 15 else num(15, 4) + num(n - 14, 8)) + pl + [1, 1, 1]   # ID_END
    while len(nb) % 8:
        nb.append(0)
    nf = bytearray(tobytes(nb))
    L = len(nf)
    nf[3] = (nf[3] & 0xFC) | (L >> 11)
    nf[4] = (L >> 3) & 0xFF
    nf[5] = (nf[5] & 0x1F) | ((L & 7) << 5)
    out += nf
open(outp, 'wb').write(out)
