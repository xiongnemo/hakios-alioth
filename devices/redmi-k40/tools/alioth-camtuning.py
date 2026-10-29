#!/usr/bin/env python3
"""Make a camera's colour tuning (RWCISP02, as libhakios.media's processor reads it: userland/src/camera_isp.c)
out of one of alioth's tuning files (/vendor/lib64/camera/com.qti.tuned.alioth_<maker>_<sensor>_<camera>.bin:
the vendor's, kept in fw-camera/ and never in the repository; so is what this makes of it).

usage: alioth-camtuning.py TUNED.bin OUT.isp      (alioth-camtuning.py TUNED.bin -   lists what is there only)

The tools/camtuning.py of the tree reads Parameter Parser V3.0.1 files; alioth's are V2.0.0 (read by
umi-camsensor-v2.py beside this): the symbols are 64-bit, and so is every scalar field of a structure -- a
float in the low half of its eight bytes -- while arrays of floats stay packed. The trees are the same:
mod_<m>_led_idx_data { led, regions, symbol } (24 bytes), mod_<m>_aec_data { lux index from, to, gain
from, to, ranges, symbol } (48 bytes), mod_<m>_cct_data { CCT from, to, then the values, packed }.
A V2 file holds a module once for each of its uses: the colour matrix taken is the first tree that is
not the identity (the IPE's: the BPS's and IFE's are left as they are), the lens shading the first one
whose gains are not all 1. The AWB's reference lights (refPtV1) are taken where there are the ten
camtuning.py knows, as there; else none.
"""
import importlib.util
import struct
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location('v2', Path(__file__).with_name('umi-camsensor-v2.py'))
v2 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v2)

GRID = 17 * 13
REFERENCE_CCTS = (10000, 7500, 6500, 5000, 4150, 4000, 3800, 2850, 2300, 1900)


def slot_float(q):
    return struct.unpack('<f', struct.pack('<I', q & 0xffffffff))[0]


def trees(data, pool, syms, name, floats):
    """every tree of the module: [(lux from, lux to, [(cct from, cct to, values)])], without the flash"""
    out = []
    for sid, s in sorted(syms.items()):
        if s['name'] != 'mod_%s_led_idx_data' % name:
            continue
        led = v2.value(data, pool, s)
        entry = next((e for e in struct.iter_unpack('<3Q', led) if e[0] == 0), None)
        if entry is None:
            continue
        _, regions, aec_sid = entry
        aec = v2.value(data, pool, syms[aec_sid])
        tree = []
        for i in range(regions):
            lux_from, lux_to, _, _, n, cct_sid = struct.unpack_from('<6Q', aec, 48 * i)
            cct = v2.value(data, pool, syms[cct_sid])
            size = len(cct) // n
            ranges = []
            for j in range(n):
                c_from, c_to = struct.unpack_from('<2Q', cct, size * j)
                values = struct.unpack_from('<%df' % floats, cct, size * j + 16)
                ranges.append((slot_float(c_from), slot_float(c_to), values))
            tree.append((slot_float(lux_from), slot_float(lux_to), ranges))
        out.append(tree)
    return out


def references(data, pool, syms):
    s = next((s for s in syms.values() if s['name'] == 'refPtV1'), None)
    if s is None:
        return []
    q = struct.unpack('<%dQ' % (s['size'] // 8), v2.value(data, pool, s))
    f = [slot_float(x) for x in q]
    points = [(f[i], f[i + 1]) for i in range(6, len(f) - 1, 2) if f[i] > 0 and f[i + 1] > 0]
    if len(points) != len(REFERENCE_CCTS):
        return []
    return [(rg, bg, cct) for (rg, bg), cct in zip(points, REFERENCE_CCTS)]


def identity(m):
    return all(abs(m[i] - (1.0 if i in (0, 4, 8) else 0.0)) < 1e-4 for i in range(9))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data, pool, _, syms = v2.load_module(sys.argv[1])
    ccm = next((t for t in trees(data, pool, syms, 'cc13', 9) if not all(identity(v) for _, _, r in t for _, _, v in r)), [])
    # (V2 keeps each channel's grid as 222 floats: the 17 x 13 and one more; taken R, Gr, Gb, B as camtuning.py has them)
    lsc = [[(lf, lt, [(cf, ct, tuple(x for c in range(4) for x in v[c * (GRID + 1):c * (GRID + 1) + GRID])) for cf, ct, v in r])
            for lf, lt, r in t] for t in trees(data, pool, syms, 'lsc34', 4 * (GRID + 1))]
    lsc = next((t for t in lsc if not all(abs(x - 1.0) < 1e-4 for _, _, r in t for _, _, v in r for x in v)), [])
    refs = references(data, pool, syms)
    for what, tree in (('colour matrix', ccm), ('lens shading', lsc)):
        for lux_from, lux_to, ranges in tree:
            for cct_from, cct_to, v in ranges:
                shown = (' '.join('%.3f' % x for x in v) if what == 'colour matrix' else
                         'corner gains R %.2f Gr %.2f Gb %.2f B %.2f' % tuple(v[c * GRID] for c in range(4)))
                print('%-13s lux index %4.0f-%-4.0f CCT %5.0f-%-5.0f %s' % (what, lux_from, lux_to, cct_from, cct_to, shown))
    print('reference lights: %d' % len(refs))
    if sys.argv[2] == '-':
        return
    out = bytearray(b'RWCISP02')
    for tree in (ccm, lsc):
        out += struct.pack('<I', len(tree))
        for lux_from, lux_to, ranges in tree:
            out += struct.pack('<2fI', lux_from, lux_to, len(ranges))
            for cct_from, cct_to, v in ranges:
                out += struct.pack('<2f%df' % len(v), cct_from, cct_to, *v)
    out += struct.pack('<I', len(refs))
    for ref in refs:
        out += struct.pack('<3f', *ref)
    Path(sys.argv[2]).write_bytes(out)


if __name__ == '__main__':
    main()
