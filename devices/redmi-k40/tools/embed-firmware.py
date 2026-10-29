#!/usr/bin/env python3
"""Generate an ignored C include for local, per-device QCA6390 firmware."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--patch', type=Path, required=True)
parser.add_argument('--nvm', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()

def read(path, tlv_type):
    if not path.is_file():
        return b''
    data = path.read_bytes()
    if len(data) < 4 or data[0] != tlv_type or int.from_bytes(data[1:4], 'little') != len(data) - 4:
        raise SystemExit(f'{path}: invalid QCA TLV length or type')
    return data

def emit(name, data):
    lines = [f'const uint8_t {name}[] = {{']
    for off in range(0, len(data), 16):
        lines.append('    ' + ','.join(f'0x{b:02x}' for b in data[off:off+16]) + ',')
    if not data:
        lines.append('    0,')
    lines += ['};', f'const size_t {name}_size = {len(data)}u;']
    return '\n'.join(lines)

patch = read(args.patch, 1)
nvm = read(args.nvm, 2)
args.output.parent.mkdir(parents=True, exist_ok=True)
contents = '#include <stdint.h>\n#include <stddef.h>\n' + \
    emit('umi_bt_patch', patch) + '\n' + emit('umi_bt_nvm', nvm) + '\n'
if not args.output.exists() or args.output.read_text() != contents:
    args.output.write_text(contents)
print(f'embedded local QCA6390 firmware: patch={len(patch)} NVM={len(nvm)}')
