#!/usr/bin/env python3
"""ppm2png.py - convert a QEMU screendump (binary PPM) to PNG.

QEMU's monitor only writes PPM. This exists so screenshots from the VM can be
looked at directly.
"""
import struct
import sys
import zlib


def convert(src, dst):
    data = open(src, 'rb').read()

    # header: P6 <w> <h> <maxval> then pixels
    fields = []
    i = 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] not in (b'\n', b''):
                i += 1
            continue
        if i >= len(data):
            # bytes.isspace() is False for b'', so the loops above spin on a
            # file that ends mid-header (an empty one, or a truncated
            # screendump) until Python runs out of memory.
            raise SystemExit('%s: truncated PPM header (no width/height/maxval)' % src)
        j = i
        while not data[j:j + 1].isspace():
            j += 1
            if j >= len(data):
                raise SystemExit('%s: truncated PPM header (unterminated field)' % src)
        fields.append(data[i:j])
        i = j
    i += 1  # single whitespace after maxval

    if fields[0] != b'P6':
        raise SystemExit('not a binary PPM (P6): %r' % fields[0])
    w, h = int(fields[1]), int(fields[2])
    if len(data) - i < w * h * 3:
        # Short rows are the quiet failure: the PNG is still written (and the
        # tool still prints success) but its IDAT stream covers less than
        # h*(1+3w) bytes, so every decoder rejects it.
        raise SystemExit('%s: pixel data is %d bytes, a %dx%d P6 image needs %d'
                         % (src, max(0, len(data) - i), w, h, w * h * 3))
    px = data[i:i + w * h * 3]
    stride = w * 3
    raw = b''.join(b'\x00' + px[y * stride:(y + 1) * stride] for y in range(h))

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    open(dst, 'wb').write(png)
    print('%s -> %s  %dx%d  %.1f KB' % (src, dst, w, h, len(png) / 1024))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: ppm2png.py <in.ppm> <out.png>')
    convert(sys.argv[1], sys.argv[2])
