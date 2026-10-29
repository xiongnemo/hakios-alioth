#!/usr/bin/env python3
"""
Pack the redwood UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from prebuilt/uefi/.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]

Another device (common/devices/<name>.conf, read by common/build.sh) sets these in the environment:
  UEFI_TARGET   Mu-Silicium device name (default redwood)    UEFI_FD, DTB   files to use instead
  FD_BASE, FD_SIZE, PAYLOAD_BASE                              addresses from its UEFI port's memory map
"""
import argparse, gzip, os, subprocess, sys, tempfile
from pathlib import Path

HERE = str(Path(__file__).resolve().parents[3])
MU = os.path.join(HERE, 'Mu-Silicium')
PREBUILT = os.path.join(HERE, 'devices', 'poco-x5-pro', 'prebuilt')
def env_int(name, default):
    v = os.environ.get(name, '')
    return int(v, 0) if v else default
TARGET = os.environ.get('UEFI_TARGET') or 'redwood'
FD_BASE, FD_SIZE = env_int('FD_BASE', 0x9FD00000), env_int('FD_SIZE', 0x200000)
PAYLOAD_BASE, PAYLOAD_MAX = env_int('PAYLOAD_BASE', 0xF0000000), 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output', default=os.path.join(HERE, 'xnu-redwood.img'))
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

# A foreign target must never fall back to redwood's firmware or fixed-address shim.
if TARGET != 'redwood':
    for key in ('FD_BASE', 'FD_SIZE', 'PAYLOAD_BASE'):
        if not os.environ.get(key):
            sys.exit(f'{TARGET}: set {key} from the UMI UEFI memory map')
    if FD_BASE % 16 or FD_SIZE <= 0 or FD_SIZE % 16 or PAYLOAD_BASE % 0x10000:
        sys.exit('unaligned UEFI/payload addresses or size')
    if FD_BASE < PAYLOAD_BASE + PAYLOAD_MAX and PAYLOAD_BASE < FD_BASE + FD_SIZE:
        sys.exit('UEFI FD overlaps payload reservation')

fd_path = os.environ.get('UEFI_FD') or os.path.join(MU, f'Build/{TARGET}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if TARGET != 'redwood':
    if not os.path.isfile(fd_path):
        sys.exit(f'{TARGET}: missing UMI UEFI FD: {fd_path}; set UEFI_FD (no redwood fallback)')
    dtb_path = os.environ.get('DTB') or os.path.join(MU, f'Resources/DTBs/{TARGET}.dtb')
    if not os.path.isfile(dtb_path):
        sys.exit(f'{TARGET}: missing device tree: {dtb_path}')
    if TARGET == 'umi':
        result = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', dtb_path],
                                capture_output=True, text=True)
        if result.returncode or 'model = "Qualcomm Technologies, Inc. xiaomi umi";' not in result.stdout:
            sys.exit('umi: DTB is not the supplied Xiaomi Mi 10 device tree')
    if TARGET == 'alioth':
        result = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', dtb_path],
                                capture_output=True, text=True)
        if result.returncode or 'model = "Qualcomm Technologies, Inc. xiaomi alioth";' not in result.stdout:
            sys.exit('alioth: DTB is not the Redmi K40 device tree')
    # Build a shim for the supplied FD, independently of a Mu-Silicium checkout.
    # The user-supplied UEFI must implement the XNUP loader hook and reserve its RAM.
    if os.environ.get('SHIM'):
        # a shim known to work with this FD (the one in the phone's proven boot image)
        shim = open(os.environ['SHIM'], 'rb').read()
    else:
      with tempfile.TemporaryDirectory(prefix='hackios-shim-') as tmp:
          obj, raw = os.path.join(tmp, 'shim.o'), os.path.join(tmp, 'shim.bin')
          subprocess.check_call(['clang', '--target=aarch64-linux-gnu', '-c',
              os.path.join(HERE, 'devices/redmi-k40/tools/bootshim.S'), f'-DFD_BASE={FD_BASE:#x}',
              f'-DFD_SIZE={FD_SIZE:#x}', f'-DPAYLOAD_BASE={PAYLOAD_BASE:#x}', '-o', obj])
          subprocess.check_call(['aarch64-linux-gnu-objcopy', '-O', 'binary', obj, raw])
          shim = open(raw, 'rb').read()
    parts = {'fd': fd_path, 'dtb': dtb_path, 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
elif os.path.isfile(fd_path) and os.path.isdir(os.path.join(MU, 'BootShim')):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{TARGET}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'redwood.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
    if os.environ.get('UEFI_FD'): parts['fd'] = os.environ['UEFI_FD']
if TARGET == 'redwood':
    shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
if len(fd) != FD_SIZE:
    sys.exit(f'FD size {len(fd):#x} differs from FD_SIZE={FD_SIZE:#x}')

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

if os.environ.get('DTB'): parts['dtb'] = os.environ['DTB']
dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    # board "HACKiOS": how the loader of an installed phone tells which A/B slot's boot partition is its own
    # (xnuloader/flashboot.c, ab_switch_back) -- an Android boot image never has that name
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', '1', '--board', 'HACKiOS', '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
