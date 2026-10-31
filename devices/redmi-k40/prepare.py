#!/usr/bin/env python3
"""Stage UMI code into generated trees; never overwrite upstream driver templates."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess

DEVICE = Path(__file__).resolve().parent
ROOT = DEVICE.parents[1]
OUT = ROOT / 'out/redmi-k40'
KERNEL = OUT / 'xnu'

def copy_changed(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if (not target.exists() or source.read_bytes() != target.read_bytes() or
            (source.stat().st_mode & 0o777) != (target.stat().st_mode & 0o777)):
        shutil.copy2(source, target)

def copy_overlay(source, target):
    for path in source.rglob('*'):
        if path.is_file() and '__pycache__' not in path.parts:
            copy_changed(path, target / path.relative_to(source))

def verify():
    manifest = DEVICE / 'upstream-templates.sha256'
    for line in manifest.read_text().splitlines():
        expected, relative = line.split('  ', 1)
        path = ROOT / relative
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise SystemExit(f'Upstream template changed: {relative}; keep UMI changes under {DEVICE.relative_to(ROOT)}')

def kernel():
    verify()
    OUT.mkdir(parents=True, exist_ok=True)
    if not KERNEL.exists():
        subprocess.run([str(ROOT / 'xnu/apply.sh'), str(KERNEL)], check=True)
    patch = DEVICE / 'xnu-device.patch'
    applied = OUT / 'applied-device.patch'
    if applied.exists() and applied.read_bytes() != patch.read_bytes():
        subprocess.run(['git', '-C', str(KERNEL), 'apply', '--reverse', str(applied)], check=True)
    if not applied.exists() or applied.read_bytes() != patch.read_bytes():
        check = subprocess.run(['git', '-C', str(KERNEL), 'apply', '--check', str(patch)], capture_output=True)
        if check.returncode:
            subprocess.run(['git', '-C', str(KERNEL), 'apply', '--reverse', '--check', str(patch)], check=True)
        else:
            subprocess.run(['git', '-C', str(KERNEL), 'apply', str(patch)], check=True)
        copy_changed(patch, applied)
    copy_overlay(DEVICE / 'overlays/xnu/files', KERNEL)
    print(KERNEL)

def userland():
    verify()
    sandbox = OUT / 'userland-build'
    sandbox.mkdir(parents=True, exist_ok=True)
    shutil.copytree(ROOT / 'userland', sandbox / 'userland', dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('obj', 'root', 'sysroot', '*.apfs', '__pycache__'))
    copy_overlay(DEVICE / 'overlays/userland', sandbox / 'userland')
    for name in ('xnu-env.sh', 'xnu-deps', 'tools', 'prebuilt', 'sysimage', 'devices', 'wifi',
                 'fw-gpu', 'fw-audio', 'fw-haptic', 'fw-wifi', 'fw-bluetooth', 'fw-camera', 'fw-sensors',
                 'fw-modem', 'apps'):
        path = sandbox / name
        if not path.exists() and not path.is_symlink():
            path.symlink_to(ROOT / name, target_is_directory=(ROOT / name).is_dir())
    for name in ('root.apfs', 'system.apfs'):
        target = OUT / name
        if not target.exists() and not target.is_symlink():
            target.symlink_to(sandbox / 'userland' / name)
    print(sandbox / 'userland')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('kernel', 'userland', 'verify'))
    action = parser.parse_args().action
    {'kernel': kernel, 'userland': userland, 'verify': verify}[action]()
