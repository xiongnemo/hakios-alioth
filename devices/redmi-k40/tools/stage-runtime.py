#!/usr/bin/env python3
"""Select UMI artifacts in a generated copy of xl, leaving upstream scripts intact."""
from pathlib import Path
import os

DEVICE = Path(__file__).resolve().parents[1]
ROOT = DEVICE.parents[1]
OUT = ROOT / 'out/redmi-k40'
sandbox = OUT / 'runtime'
sandbox.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'xl').read_text()
source = source.replace('K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837',
                        'K=${XL_KERNEL:?UMI kernel path required}')
source = source.replace('[ -f $K ] || K=prebuilt/mach.development.bcm2837',
                        '[ -f "$K" ] || { echo "Build the UMI kernel first" >&2; exit 1; }')
source = source.replace('L=xnuloader/loader.efi', 'L=${XL_LOADER:?UMI loader path required}')
source = source.replace('[ -f $L ] || L=prebuilt/loader.efi',
                        '[ -f "$L" ] || { echo "Build the UMI loader first" >&2; exit 1; }')
# Use the existing host's phone lock across old and new UMI workspaces.
source = source.replace('/darwin-hakios-phone', '/hackios-phone')
(sandbox / 'xl').write_text(source)
(sandbox / 'xl').chmod(0o755)
for name, target in {'xnuloader': ROOT / 'xnuloader', 'prebuilt': ROOT / 'prebuilt',
                     'mkxnuimg.py': DEVICE / 'tools/mkimage.py',
                     'tools': ROOT / 'tools',
                     'userland': OUT / 'userland-build/userland'}.items():
    path = sandbox / name
    if not path.exists() and not path.is_symlink():
        path.symlink_to(target, target_is_directory=target.is_dir())
# Match the old per-phone Wi-Fi address input without embedding it in source.
mac = ROOT / 'fw-wifi/mac'
if mac.exists():
    address = mac.read_text().strip()
    fields = address.split(':')
    if len(fields) != 6 or any(len(x) != 2 or any(c not in '0123456789abcdefABCDEF' for c in x) for x in fields):
        raise SystemExit('Invalid private Wi-Fi MAC input')
    arguments = os.environ.get('XL_CMDLINE', '')
    if 'wifi=1' in arguments and 'wifi_mac=' not in arguments:
        # xl is generated locally; the address file remains outside git.
        # The caller supplies this through a private runtime file read by run.sh.
        (OUT / 'wifi-boot.args').write_text('wifi_mac=' + address + '\n')
