#!/usr/bin/env python3
"""The reserved-memory regions the kernel's drivers load firmware into, from a phone's own device tree (issue #1).

usage: dt-regions.py FDT [--all]
  FDT    the phone's flattened device tree as its firmware left it: /sys/firmware/fdt (kit/pull-firmware.sh saves it
         as fw-board/runtime-fdt.dtb). XBL sizes removed_region by itself, and the PIL regions above it move with it
         from one firmware to the next (a HyperOS 1.0 K40: all 0x5c00000 higher than this port's phone).
  --all  print every region's boot-args, not only those that differ from the kernel's built-in ones

Printed on stdout: the boot-args (board_pil_gpu=0x... board_pil_gpu_size=0x... -- sys/hakios.board.h) for the
regions that differ, on one line; on stderr, a line for each region. Nodes are matched by their name before the '@'
(the prebuilt DTB writes them without unit addresses). Exit 1 when the tree cannot be read.
"""
import struct
import sys

# the kernel's built-in regions (the phone the port was brought up on): node -> (boot-arg, base, size)
REGIONS = {
    'pil_gpu_region': ('pil_gpu', 0x8681a000, 0x2000),
    'pil_slpi_region': ('pil_slpi', 0x88c00000, 0x2f00000),
    'pil_adsp_region': ('pil_adsp', 0x8bb00000, 0x2500000),
}
FDT_MAGIC = 0xd00dfeed
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


def parse(blob):
    """-> {path: {prop: bytes}}"""
    magic, total, off_struct, off_strings = struct.unpack_from('>IIII', blob, 0)
    if magic != FDT_MAGIC or total > len(blob):
        raise ValueError('not a flattened device tree (magic %#x, %d of %d bytes)' % (magic, len(blob), total))
    strings = blob[off_strings:]

    def name_at(off):
        return strings[off:strings.index(b'\0', off)].decode()

    nodes, path, pos = {}, [], off_struct
    while True:
        (token,) = struct.unpack_from('>I', blob, pos)
        pos += 4
        if token == BEGIN_NODE:
            end = blob.index(b'\0', pos)
            path.append(blob[pos:end].decode())
            pos = (end + 4) & ~3
            nodes.setdefault('/'.join(path) or '/', {})
        elif token == END_NODE:
            path.pop()
        elif token == PROP:
            length, nameoff = struct.unpack_from('>II', blob, pos)
            pos += 8
            nodes['/'.join(path) or '/'][name_at(nameoff)] = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == NOP:
            continue
        elif token == END:
            return nodes
        else:
            raise ValueError('a token %d at %#x' % (token, pos - 4))


def cells(value, n):
    out = 0
    for i in range(n):
        out = out << 32 | struct.unpack_from('>I', value, 4 * i)[0]
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__)
    try:
        nodes = parse(open(args[0], 'rb').read())
    except (OSError, ValueError, struct.error) as e:
        print('dt-regions: %s: %s' % (args[0], e), file=sys.stderr)
        sys.exit(1)
    rm = nodes.get('/reserved-memory')
    if rm is None:
        print('dt-regions: no /reserved-memory node', file=sys.stderr)
        sys.exit(1)
    ac = cells(rm['#address-cells'], 1) if '#address-cells' in rm else 2
    sc = cells(rm['#size-cells'], 1) if '#size-cells' in rm else 2
    words = []
    for path, props in nodes.items():
        parent, _, leaf = path.rpartition('/')
        name = leaf.split('@')[0]
        if parent != '/reserved-memory' or name not in REGIONS or 'reg' not in props:
            continue
        arg, base0, size0 = REGIONS[name]
        reg = props['reg']
        base, size = cells(reg, ac), cells(reg[4 * ac:], sc)
        same = base == base0 and size == size0
        print('dt-regions: %-16s %#x + %#x%s' % (name, base, size, ' (as built in)' if same else
              ' (built in: %#x + %#x)' % (base0, size0)), file=sys.stderr)
        if not same or '--all' in sys.argv:
            words += ['board_%s=%#x' % (arg, base), 'board_%s_size=%#x' % (arg, size)]
    print(' '.join(words))


if __name__ == '__main__':
    main()
