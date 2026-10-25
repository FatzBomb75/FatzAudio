"""Wraps the raw AAC frames of an ADTS file in a Core Audio Format file the way
Apple's tools do: 'desc', 'kuki' (an ES descriptor with the AudioSpecificConfig),
'pakt' (sizes, priming, remainder) in front of 'data'.

usage: adts_to_caf.py in.aac out.caf format priming remainder [aot ext_rate_index] [--vf]
format is 'aac ', 'aach' or 'aacp'; aot and ext_rate_index make an explicit HE-AAC
config (else the config is the ADTS profile's, with SBR and PS implicit); --vf keeps
the esds version and flags in front of the descriptor, as some writers do."""
import struct
import sys

RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350]


def frames(d):
    p = 0
    while p + 7 <= len(d):
        assert d[p] == 0xFF and d[p + 1] & 0xF6 == 0xF0, "not ADTS"
        header = 7 if d[p + 1] & 1 else 9
        size = ((d[p + 3] & 3) << 11) | (d[p + 4] << 3) | (d[p + 5] >> 5)
        assert d[p + 6] & 3 == 0, "one raw data block per frame"
        yield d[p:p + 7], d[p + header:p + size]
        p += size


class Bits:
    def __init__(self):
        self.v, self.n = 0, 0

    def put(self, v, n):
        self.v, self.n = (self.v << n) | v, self.n + n

    def bytes(self):
        pad = -self.n % 8
        return (self.v << pad).to_bytes((self.n + pad) // 8, "big")


def descr(tag, body):
    n = len(body)
    return bytes([tag, 0x80 | (n >> 21) & 0x7F, 0x80 | (n >> 14) & 0x7F, 0x80 | (n >> 7) & 0x7F, n & 0x7F]) + body


def chunk(kind, body):
    return kind + struct.pack(">q", len(body)) + body


def vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def main():
    src, dst, fmt, priming, remainder = sys.argv[1], sys.argv[2], sys.argv[3].encode(), int(sys.argv[4]), int(sys.argv[5])
    rest = [a for a in sys.argv[6:] if a != "--vf"]
    vf = "--vf" in sys.argv[6:]
    pk = list(frames(open(src, "rb").read()))
    h = pk[0][0]
    profile, ri, cfg = h[2] >> 6, (h[2] >> 2) & 15, ((h[2] & 1) << 2) | (h[3] >> 6)
    b = Bits()
    if rest:
        aot, ext = int(rest[0]), int(rest[1])
        b.put(aot, 5); b.put(ri, 4); b.put(cfg, 4); b.put(ext, 4); b.put(2, 5)
    else:
        b.put(profile + 1, 5); b.put(ri, 4); b.put(cfg, 4)
    b.put(0, 3)
    asc = b.bytes()
    he = fmt != b"aac "
    rate, fpp = RATES[ri] * (2 if he else 1), 1024 * (2 if he else 1)
    dcd = bytes([0x40, 0x15]) + bytes(3) + struct.pack(">II", 128000, 128000) + descr(0x05, asc)
    es = descr(0x03, struct.pack(">HB", 0, 0) + descr(0x04, dcd) + descr(0x06, b"\x02"))
    kuki = (bytes(4) if vf else b"") + es
    desc = struct.pack(">dIIIIII", float(rate), struct.unpack(">I", fmt)[0], 0, 0, fpp, 2 if cfg == 0 else cfg, 0)
    n = len(pk)
    pakt = struct.pack(">qqii", n, n * fpp - priming - remainder, priming, remainder) + b"".join(vlq(len(p[1])) for p in pk)
    data = struct.pack(">I", 1) + b"".join(p[1] for p in pk)
    out = b"caff" + struct.pack(">HH", 1, 0) + chunk(b"desc", desc) + chunk(b"kuki", kuki) + chunk(b"pakt", pakt) + chunk(b"data", data)
    open(dst, "wb").write(out)


main()
