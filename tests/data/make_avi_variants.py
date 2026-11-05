"""AVI variants for test_demux.c: pcm16.avi with no idx1, with idx1 offsets from the file start,
a file whose RIFF size is wrong, and an OpenDML file in two RIFFs (AVI and AVIX) with a super
index and standard indexes. Run from tests/data/avi (make_avi_data.sh does)."""
import math
import struct

LE = '<'


def chunk(cid, data):
    out = cid + struct.pack('<I', len(data)) + data
    return out + (b'\0' if len(data) & 1 else b'')


def lst(kind, data):
    return chunk(b'LIST', kind + data)


def find(d, cid, start=0):
    return d.find(cid, start)


def top_chunks(d):
    pos, out = 12, []
    while pos + 8 <= len(d):
        size = struct.unpack_from('<I', d, pos + 4)[0]
        out.append((d[pos:pos + 4], pos, size, d[pos + 8:pos + 12]))
        pos += 8 + size + (size & 1)
    return out


def riff(d):
    return b'RIFF' + struct.pack('<I', len(d) - 8) + d[8:]


src = open('pcm16.avi', 'rb').read()
tops = top_chunks(src)
idx = [t for t in tops if t[0] == b'idx1'][0]
movi = [t for t in tops if t[0] == b'LIST' and t[3] == b'movi'][0]

# 1. no idx1: the file ends after the movi list.
noidx = riff(src[:idx[1]])
open('pcm16_noindex.avi', 'wb').write(noidx)

# 2. idx1 offsets from the file start (some muxers write them so).
e = bytearray(src[idx[1]:idx[1] + 8 + idx[2]])
for i in range(8, len(e), 16):
    off = struct.unpack_from('<I', e, i + 8)[0]
    struct.pack_into('<I', e, i + 8, off + movi[1] + 8)
open('pcm16_absolute.avi', 'wb').write(src[:idx[1]] + bytes(e))

# 3. RIFF and movi sizes that lie (a capture that was cut): the chunks still walk.
lie = bytearray(src[:idx[1]])
struct.pack_into('<I', lie, 4, 0)
struct.pack_into('<I', lie, movi[1] + 4, 0)
open('pcm16_zero_sizes.avi', 'wb').write(bytes(lie))

# 4. OpenDML: 8 kHz stereo PCM with a video stream, 0.75 s, the second half in an AVIX RIFF.
rate, ch, secs = 8000, 2, 0.75
frames = int(rate * secs)
pcm = b''.join(struct.pack('<hh', int(0.5 * 32767 * math.sin(2 * math.pi * 440 * n / rate)),
                           int(0.3 * 32767 * math.sin(2 * math.pi * 660 * n / rate))) for n in range(frames))
step = 800 * 4  # 0.1 s per chunk
parts = [pcm[i:i + step] for i in range(0, len(pcm), step)]
strh_v = b'vids' + b'MP42' + struct.pack('<IHHIIIIIIIIhhhh', 0, 0, 0, 0, 1, 10, 0, len(parts), 0, 0xFFFFFFFF, 0, 0, 0, 64, 48)
strf_v = struct.pack('<IiiHHIIiiII', 40, 64, 48, 1, 24, 0x3234504D, 64 * 48 * 3, 0, 0, 0, 0)
strh_a = b'auds' + struct.pack('<IIHHIIIIIIII', 0, 0, 0, 0, 0, 1, rate, 0, frames, 0, 0xFFFFFFFF, 4) + struct.pack('<hhhh', 0, 0, 0, 0)
strf_a = struct.pack('<HHIIHH', 1, ch, rate, rate * 4, 4, 16)
n_super = 2


def layout(super_entries):
    indx = b'indx'
    sup = struct.pack('<HBBI4s12x', 4, 0, 0, len(super_entries), b'01wb')
    for off, size, dur in super_entries:
        sup += struct.pack('<QII', off, size, dur)
    sup += b'\0' * (16 * (n_super - len(super_entries)))
    strl_v = lst(b'strl', chunk(b'strh', strh_v) + chunk(b'strf', strf_v))
    strl_a = lst(b'strl', chunk(b'strh', strh_a) + chunk(b'strf', strf_a) + chunk(b'indx', sup))
    avih = chunk(b'avih', struct.pack('<IIIIIIIIII16x', 100000, 0, 0, 0x10, len(parts), 0, 2, 0, 64, 48))
    odml = lst(b'odml', chunk(b'dmlh', struct.pack('<I244x', len(parts))))
    return lst(b'hdrl', avih + strl_v + strl_a + odml)


def make_movi(chunks):
    """chunks: list of (id, data). Returns the movi list and the offsets of each chunk's data in it."""
    body, offs, pos = b'', [], 12  # after LIST, size, 'movi'
    for cid, data in chunks:
        offs.append(pos + 8)
        c = chunk(cid, data)
        body += c
        pos += len(c)
    return lst(b'movi', body), offs


def std_index(base, entries):
    body = struct.pack('<HBBI4sQI', 2, 0, 1, len(entries), b'01wb', base, 0)
    for off, size in entries:
        body += struct.pack('<II', off, size)
    return chunk(b'ix01', body)


half = len(parts) // 2
first, second = parts[:half], parts[half:]
dummy = b'\0' * 100
c1 = []
for p in first:
    c1 += [(b'00dc', dummy), (b'01wb', p)]
c2 = []
for p in second:
    c2 += [(b'00dc', dummy), (b'01wb', p)]
movi1, offs1 = make_movi(c1)
movi2, offs2 = make_movi(c2)
hdrl_len = len(layout([(0, 0, 0)]))
junk = chunk(b'JUNK', b'\0' * 64)
# File layout: RIFF AVI 'hdrl' JUNK movi1 ix01(first) | RIFF AVIX movi2 ix01(second)
riff1_head = 12 + hdrl_len + len(junk)
movi1_at = riff1_head
ix1_at = movi1_at + len(movi1)
a_off1 = [movi1_at + o for o in offs1[1::2]]
ix1 = std_index(0, [(o, len(p)) for o, p in zip(a_off1, first)])
riff1_len = ix1_at + len(ix1)
riff2_head = riff1_len + 12
movi2_at = riff2_head
ix2_at = movi2_at + len(movi2)
a_off2 = [movi2_at + o for o in offs2[1::2]]
ix2 = std_index(0, [(o, len(p)) for o, p in zip(a_off2, second)])
hdrl = layout([(ix1_at, len(ix1), len(first)), (ix2_at, len(ix2), len(second))])
assert len(hdrl) == hdrl_len
avi1 = b'AVI ' + hdrl + junk + movi1 + ix1
avix = b'AVIX' + movi2 + ix2
data = b'RIFF' + struct.pack('<I', len(avi1)) + avi1 + b'RIFF' + struct.pack('<I', len(avix)) + avix
open('pcm16_opendml.avi', 'wb').write(data)

# 5. The same OpenDML file with the super index removed (stream header kept): the walk finds AVIX.
nodml = bytearray(data)
at = nodml.find(b'indx')
nodml[at:at + 4] = b'JUNK'
open('pcm16_avix_noindex.avi', 'wb').write(bytes(nodml))
