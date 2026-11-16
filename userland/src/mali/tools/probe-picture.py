#!/usr/bin/env python3
"""probe-picture.py LOG OUT.png [NAME]: the last whole picture mali-probe printed into LOG ("mali-picture NAME WxH I/N:
base64 of RGB"), as a PNG eight times as big (a 64x64 target is small)."""
import base64, re, struct, sys, zlib

text = open(sys.argv[1], 'rb').read().decode('latin-1')
want = sys.argv[3] if len(sys.argv) > 3 else None
found = {}
for m in re.finditer(r'mali-picture (\S+) (\d+)x(\d+) (\d+)/(\d+): ([A-Za-z0-9+/=]+)', text):
    name, w, h, i, n, data = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), m.group(6)
    if want and name != want:
        continue
    found.setdefault((name, w, h, n), {})[i] = data
whole = [(k, v) for k, v in found.items() if len(v) == k[3]]
if not whole:
    sys.exit('probe-picture.py: no whole picture in the log')
(name, w, h, n), lines = whole[-1]
rgb = base64.b64decode(''.join(lines[i] for i in range(1, n + 1)))
S = 8
rows = b''
for y in range(h):
    row = b''.join(rgb[3 * (y * w + x):3 * (y * w + x) + 3] * S for x in range(w))
    rows += (b'\0' + row) * S
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * S, h * S, 8, 2, 0, 0, 0)) + \
      chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
print('probe-picture.py: %s %dx%d -> %s' % (name, w, h, sys.argv[2]))
