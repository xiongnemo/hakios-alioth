#!/usr/bin/env python3
"""Compile the UMI Wi-Fi adaptation without changing the upstream LinuxKPI templates."""
import importlib.util
import os
from pathlib import Path
import shutil

DEVICE = Path(__file__).resolve().parents[1]
ROOT = DEVICE.parents[1]
spec = importlib.util.spec_from_file_location('umi_prepare', DEVICE / 'prepare.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)
prepare.verify()
sandbox = ROOT / 'out/redmi-k40/wifi-build'
sandbox.mkdir(parents=True, exist_ok=True)
for name in ('wifi', 'tools'):
    shutil.copytree(ROOT / name, sandbox / name, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
prepare.copy_overlay(DEVICE / 'overlays/wifi', sandbox / 'wifi')
prepare.copy_changed(ROOT / 'xnu-env.sh', sandbox / 'xnu-env.sh')
deps = sandbox / 'xnu-deps'
deps.mkdir(exist_ok=True)
for name in ('host', 'hostinc', 'sdk'):
    p = deps / name
    if not p.exists():
        p.symlink_to(ROOT / 'xnu-deps' / name, target_is_directory=True)
build = deps / 'build/ath11k'
build.mkdir(parents=True, exist_ok=True)
cache = os.environ.get('ATH11K_CACHE')
if cache:
    cache = Path(cache).resolve()
    for archive in cache.glob('*.t*'):
        p = build / archive.name
        if archive.is_file() and not p.exists():
            p.symlink_to(archive)
    if not (build / 'src').exists() and (cache / 'src').exists():
        shutil.copytree(cache / 'src', build / 'src', copy_function=os.link, symlinks=True)
link = ROOT / 'xnu-deps/build/ath11k'
if not link.exists() and not link.is_symlink():
    link.symlink_to(build, target_is_directory=True)
print(sandbox)
