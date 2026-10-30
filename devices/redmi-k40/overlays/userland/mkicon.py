#!/usr/bin/env python3
"""The icon of IPA 安装 (src/installer) as the PNGs its Info.plist names, without an asset catalog (actool is a Mac
program): a blue square with an arrow down into a tray. SpringBoard rounds the corners itself.
usage: python3 -I mkicon.py APP_DIR"""
import os
import struct
import sys
import zlib

TOP, BOTTOM = (0x34, 0x9c, 0xff), (0x2a, 0x4d, 0xe0)
SS = 4      # samples per pixel along each side


def white(u, v):
    if 0.44 <= u <= 0.56 and 0.17 <= v <= 0.50:                 # the arrow's shaft
        return True
    if 0.45 <= v <= 0.68:                                         # its head, down to (0.5, 0.68)
        half = 0.22 * (0.68 - v) / 0.23
        if abs(u - 0.5) <= half:
            return True
    if 0.22 <= u <= 0.78 and 0.74 <= v <= 0.82:                   # the tray: its bottom
        return True
    if 0.58 <= v <= 0.82 and (0.22 <= u <= 0.30 or 0.70 <= u <= 0.78):   # and its sides
        return True
    return False


def png(size):
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            hits = sum(white((x + (i + 0.5) / SS) / size, (y + (j + 0.5) / SS) / size)
                       for i in range(SS) for j in range(SS)) / (SS * SS)
            t = y / (size - 1)
            bg = [round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)]
            row += bytes(round(c + (255 - c) * hits) for c in bg)
        rows.append(bytes(row))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    for scale in (2, 3):
        with open(os.path.join(sys.argv[1], 'AppIcon60x60@%dx.png' % scale), 'wb') as f:
            f.write(png(60 * scale))


main()
