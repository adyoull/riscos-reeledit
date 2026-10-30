#!/usr/bin/env python3
# mkv_nocues.py IN OUT: a Matroska file's Cues made a Void element of the
# same length, so it keeps its Duration and everything else but has no
# index (as some rippers write them; FFmpeg writing to a pipe leaves out
# the Duration too). Part of riscos-reeledit. GPL version 2 or later.
import sys

d = bytearray(open(sys.argv[1], 'rb').read())


def element(i):
    # The Cues element's whole length if one starts at i (its size fits), else 0
    b = d[i + 4]
    if not b:
        return 0
    n = 1
    while not (b & (0x80 >> (n - 1))):
        n += 1
    size = b & (0xFF >> n)
    for k in range(1, n):
        size = (size << 8) | d[i + 4 + k]
    total = 4 + n + size
    return total if 9 <= total and i + total <= len(d) and size > 16 else 0


# (the last one: the SeekHead names the Cues' ID too, as data; FFmpeg writes them at the end)
i = d.rfind(bytes.fromhex('1C53BB6B'))
while i >= 0 and not element(i):
    i = d.rfind(bytes.fromhex('1C53BB6B'), 0, i)
if i < 0:
    sys.exit('no Cues in ' + sys.argv[1])
total = element(i)
d[i:i + total] = bytes([0xEC, 0x01]) + (total - 9).to_bytes(7, 'big') + bytes(total - 9)
open(sys.argv[2], 'wb').write(d)
