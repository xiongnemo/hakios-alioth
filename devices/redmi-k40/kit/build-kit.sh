#!/bin/bash
# The kit for the Redmi K40 / POCO F3: what can be built from this repository alone and published -- the kernel with
# this phone's drivers, our programs and libraries for iOS (with Mesa's OpenGL ES), and the source they were built
# from, with the scripts that make the rest on the user's own computer (assemble.sh: the system image from their IPSW,
# the firmware from their phone, the loader with the iPhone identity, their Wi-Fi). GitHub Actions runs this
# (.github/workflows/kit.yml); it runs the same on a machine set up as devices/redmi-k40/README.md says.
#
# usage: devices/redmi-k40/kit/build-kit.sh OUT_DIR [VERSION]     -> OUT_DIR/alioth-kit-VERSION.zip
#
# Needs: theos ($THEOS) with its Linux toolchain and sdks/iPhoneOS16.5.sdk; the IPSW's files in sysimage/
# (sysimage/fetch.sh) -- only to link our programs against: nothing of iOS goes into the kit. No firmware is used even
# where fw-* exists: the kernel is built without the Bluetooth firmware (BT_EMBED=0), the programs without images.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=$(realpath -m "${1:?usage: build-kit.sh OUT_DIR [VERSION]}")
cd "$ROOT"
VERSION=${2:-$(git describe --tags --always 2>/dev/null || date +%Y%m%d)}
JOBS=${JOBS:-$(nproc)}
log() { echo "build-kit: $*"; }
die() { echo "build-kit: $*" >&2; exit 1; }
mkdir -p "$OUT"                         # (and the logs beside it: OUT.wifi.log, OUT.kernel.log, OUT.userland.log)
git diff --quiet HEAD -- . 2>/dev/null || log "WARNING: the tree has changes that are not committed; the kit's source/ is HEAD's"

log "the kernel tree and its dependencies"
python3 devices/redmi-k40/prepare.py kernel > /dev/null
# (stage 2 exports this tree's headers and builds two libraries against them)
XNU_SRC="$ROOT/out/redmi-k40/xnu" bash xnu-deps/setup.sh || { for l in xnu-deps/*.log; do echo "== $l"; tail -40 "$l"; done; die "the kernel's dependencies were not set up"; }
log "Wi-Fi (ath11k)"
bash devices/redmi-k40/build-wifi.sh > "$OUT.wifi.log" 2>&1 || { tail -20 "$OUT.wifi.log"; die "Wi-Fi did not build"; }
log "the kernel (-j$JOBS), without any firmware in it"
mkdir -p "$(dirname "$OUT")"
BT_EMBED=0 bash devices/redmi-k40/build-kernel.sh -j"$JOBS" -k > "$OUT.kernel.log" 2>&1 || true
K=out/redmi-k40/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
if grep -v 'lldbmacros\|memory_error' "$OUT.kernel.log" | grep -q 'error:' || [ ! -f "$K" ]; then
	grep -n 'error:' "$OUT.kernel.log" | grep -v 'lldbmacros\|memory_error' | head -20
	die "the kernel did not build (log: $OUT.kernel.log)"
fi
grep -q 'umi_bt_patch_size = 0u' out/redmi-k40/xnu/bsd/dev/umi_bt_firmware.inc || die "Bluetooth firmware went into the kernel"

log "our programs and libraries, and Mesa"
if [ ! -d sysimage/rootfs/usr/lib ]; then
	log "the IPSW's files from Apple's servers, only to link against (sysimage/fetch.sh)"
	(cd sysimage && bash fetch.sh) || die "the IPSW's files were not fetched"
fi
COMPILE_ONLY=1 bash devices/redmi-k40/build-userland.sh > "$OUT.userland.log" 2>&1 || { tail -30 "$OUT.userland.log"; die "the user space did not build"; }
U=out/redmi-k40/userland-build/userland/obj
for f in stage0 init child hello usbmux libhakios.gl.dylib libOSMesa.8.dylib libhakios.ui.dylib ipainstall IPAInstaller; do
	[ -f "$U/$f" ] || die "the user space build has no $f"
done

log "the kit"
NAME=alioth-kit-$VERSION
D="$OUT/$NAME"
rm -rf "$D" "$OUT/$NAME.zip"
mkdir -p "$D/prebuilt/userland" "$D/source"
cp "$K" "$D/prebuilt/kernel-k40.bin"
find "$U" -maxdepth 1 -type f ! -name '*.o' ! -name '*.h' ! -name '*.new' -exec cp -p {} "$D/prebuilt/userland/" \;
git archive HEAD | tar -x -C "$D/source"
cp devices/redmi-k40/kit/assemble.sh devices/redmi-k40/kit/assemble.bat "$D/"
cp devices/redmi-k40/kit/README-kit.md "$D/README.md"
cat > "$D/prebuilt/BUILD" <<EOF
alioth kit $VERSION
built $(date -u '+%Y-%m-%d %H:%M UTC') from $(git rev-parse HEAD)
kernel: xnu-8796.121.2 with HakiOS and devices/redmi-k40 (no firmware in it)
userland: devices/redmi-k40/overlays/userland, COMPILE_ONLY=1 (GL=1, Mesa $(cat out/redmi-k40/userland-build/xnu-deps/build/mesa/out/version 2>/dev/null || echo 25))
EOF
(cd "$D/prebuilt" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
# nothing of iOS and no firmware: a last look at what goes out
if find "$D/prebuilt" -type f | grep -E -i '\.(apfs|dmg|ipsw|mdt|mbn|tlv|fw|elf)$'; then die "a file that does not belong in the kit"; fi
# (Python's zipfile: it keeps the programs' modes, and needs no zip installed)
python3 - "$OUT" "$NAME" <<'EOF'
import os, sys, zipfile
out, name = sys.argv[1:]
with zipfile.ZipFile(os.path.join(out, name + '.zip'), 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for top, dirs, files in os.walk(os.path.join(out, name)):
        dirs.sort()
        for f in sorted(files):
            path = os.path.join(top, f)
            z.write(path, os.path.relpath(path, out))
EOF
log "$OUT/$NAME.zip: $(du -h "$OUT/$NAME.zip" | cut -f1) (kernel $(du -h "$D/prebuilt/kernel-k40.bin" | cut -f1), $(ls "$D/prebuilt/userland" | wc -l) programs and libraries)"
