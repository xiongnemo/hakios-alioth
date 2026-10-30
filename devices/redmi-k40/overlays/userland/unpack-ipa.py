#!/usr/bin/env python3
"""Payload/NAME.app of each IPA into DEST/NAME.app, Unix modes and symbolic links kept (an image's /redwood/apps).

usage: unpack-ipa.py DEST A.ipa [B.ipa ...]"""
import os
import plistlib
import stat
import sys
import zipfile


def unpack(ipa, dest):
    with zipfile.ZipFile(ipa) as z:
        app = None
        for info in z.infolist():
            parts = info.filename.split('/')
            if len(parts) < 2 or parts[0] != 'Payload' or not parts[1].endswith('.app') or '..' in parts:
                continue
            app = app or os.path.join(dest, parts[1])
            out = os.path.join(dest, *parts[1:])
            if info.is_dir():
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            mode = info.external_attr >> 16 if info.create_system == 3 else 0
            data = z.read(info)
            if stat.S_ISLNK(mode):
                if os.path.lexists(out):
                    os.unlink(out)
                os.symlink(data.decode(), out)
                continue
            with open(out, 'wb') as f:
                f.write(data)
            os.chmod(out, (mode & 0o777) or 0o644)
    if app is None:
        sys.exit('unpack-ipa: %s: no Payload/NAME.app in it' % ipa)
    with open(os.path.join(app, 'Info.plist'), 'rb') as f:
        info = plistlib.load(f)
    # (a zip made without Unix modes leaves the program not executable)
    os.chmod(os.path.join(app, info['CFBundleExecutable']), 0o755)
    print('unpack-ipa: %s -> %s (%s)' % (os.path.basename(ipa), app, info['CFBundleIdentifier']))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    for ipa in sys.argv[2:]:
        unpack(ipa, sys.argv[1])


main()
