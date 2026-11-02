#!/bin/bash
# Make the bootable package for a Redmi K40 / POCO F3 out of the kit (build-kit.sh, published on GitHub) and what
# cannot be published, made here on your computer:
#   - the system image, from the iOS 16.5 IPSW for the iPhone X that sysimage/fetch.sh downloads from Apple's public
#     servers (nothing is decrypted or circumvented);
#   - the firmware the drivers load, from your phone's own Android (pull-firmware.sh: read-only, needs root);
#   - the loader with the iPhone identity, made from the IPSW's device tree;
#   - the Wi-Fi network iOS joins.
# The package (ios-alioth-k40/, with start.bat) is yours: it holds Apple's files, the vendor's firmware and your Wi-Fi
# password -- do not pass it on. Nothing here writes to the phone.
#
# usage: assemble.sh [OUT_DIR]          (default: ./ios-alioth-k40 beside the kit)
#   ADB=...        the adb to use (on WSL, Windows' adb.exe works: the script finds the package's own)
#   SSID=... PSK=...   the Wi-Fi network, instead of being asked
#   FW_FROM=DIR    take the fw-* directories from DIR instead of the phone (a second assembly)
#   SYSIMAGE_FROM=DIR  use an IPSW extraction already made (another kit's source/sysimage)
set -euo pipefail
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# A kit on a Windows drive (WSL's /mnt/c ...): the work is done in the Linux file system instead -- iOS's file names
# differ in case only, which a Windows drive cannot keep apart, and it is many times faster -- and only the finished
# package is written beside the kit.
case "$KIT" in
/mnt/[a-z]/*)
	WORK=${ALIOTH_WORK:-$HOME/$(basename "$KIT")}
	echo "assemble: the kit is on a Windows drive; working in $WORK (the Linux file system), the package beside the kit"
	mkdir -p "$WORK"
	cp -a "$KIT/prebuilt" "$KIT/source" "$KIT/assemble.sh" "$KIT/README.md" "$WORK/" 2> /dev/null || cp -a "$KIT/." "$WORK/"
	exec bash "$WORK/assemble.sh" "$(realpath -m "${1:-$KIT/../ios-alioth-k40}")"
	;;
esac
ROOT=$KIT/source
OUT=$(realpath -m "${1:-$KIT/../ios-alioth-k40}")
TOOLS=$KIT/.tools
mkdir -p "$TOOLS"
export PATH=$TOOLS:$PATH
log() { printf '\n== %s\n' "$*"; }
die() { echo "assemble: $*" >&2; exit 1; }
[ -f "$KIT/prebuilt/kernel-k40.bin" ] && [ -d "$ROOT/devices/redmi-k40" ] || die "this is not an unpacked kit ($KIT)"

log "what this computer needs"
missing=
for t in python3 cc make clang curl unzip git; do command -v $t > /dev/null || missing="$missing $t"; done
command -v lld-link > /dev/null || command -v ld.lld > /dev/null || missing="$missing lld"
if [ -n "$missing" ]; then
	echo "missing:$missing"
	if command -v apt-get > /dev/null; then
		echo "installing them with apt (sudo may ask for your password)"
		sudo apt-get update -qq
		pkgs=$(echo "$missing" | sed 's/\bcc\b/build-essential/; s/\bmake\b/build-essential/; s/\blld\b/lld/')
		sudo apt-get install -y -qq $pkgs python3-venv > /dev/null
	else
		die "install:$missing"
	fi
fi
# (the loader is an EFI application: lld as its COFF linker, which it is when called by the name lld-link)
if ! command -v lld-link > /dev/null; then
	for l in lld ld.lld; do command -v $l > /dev/null && { ln -sf "$(command -v $l)" "$TOOLS/lld-link"; break; }; done
fi
command -v lld-link > /dev/null || die "no lld-link (install lld)"
# 7-Zip 23 or newer reads APFS; distributions often have older ones: the official build then, beside the kit
if ! { command -v 7z > /dev/null && [ "$(7z 2>&1 | grep -o '7-Zip [0-9]*' | head -1 | awk '{print $2}')" -ge 23 ] 2> /dev/null; }; then
	echo "7-Zip 23 or newer: fetching the official Linux build"
	curl -fsSL https://github.com/ip7z/7zip/releases/download/25.01/7z2501-linux-x64.tar.xz | tar -xJ -C "$TOOLS" 7zz
	ln -sf "$TOOLS/7zz" "$TOOLS/7z"
fi
# lzfse, for the IPSW's device tree (xnuloader/mkidentity.sh), when there is none: built from its BSD source
if ! command -v lzfse > /dev/null && ! python3 -c 'import lzfse' 2> /dev/null && [ ! -x "$ROOT/xnu-deps/host/bin/lzfse" ]; then
	git clone -q --depth 1 https://github.com/lzfse/lzfse "$TOOLS/lzfse-src"
	make -s -C "$TOOLS/lzfse-src" > /dev/null
	mkdir -p "$ROOT/xnu-deps/host/bin"
	cp "$TOOLS/lzfse-src/build/bin/lzfse" "$ROOT/xnu-deps/host/bin/lzfse"
fi
# adb: the given one, the system's, or (on WSL) the package's Windows adb.exe, fetched with Google's platform tools
PT=$TOOLS/platform-tools-windows
fetch_platform_tools() {
	[ -f "$PT/adb.exe" ] && return 0
	curl -fsSL -o "$TOOLS/pt.zip" https://dl.google.com/android/repository/platform-tools-latest-windows.zip
	rm -rf "$PT" && mkdir -p "$PT" && unzip -q -o "$TOOLS/pt.zip" -d "$TOOLS/pt" && cp "$TOOLS"/pt/platform-tools/* "$PT/" && rm -rf "$TOOLS/pt" "$TOOLS/pt.zip"
}
fetch_platform_tools
if [ -z "${ADB:-}" ]; then
	if grep -qi microsoft /proc/version 2> /dev/null; then ADB=$PT/adb.exe; chmod +x "$ADB"
	elif command -v adb > /dev/null; then ADB=adb
	else die "no adb: install Android's platform tools (adb), or set ADB=..."; fi
fi
export ADB

log "1/5 the iOS 16.5 IPSW (iPhone X), from Apple's servers: about 7.4 GB the first time"
if [ -n "${SYSIMAGE_FROM:-}" ]; then
	for d in rootfs cryptex cryptex-app 20F66__iPhone10,3_6; do
		[ -e "$ROOT/sysimage/$d" ] || cp -al "$SYSIMAGE_FROM/$d" "$ROOT/sysimage/$d"
	done
	cp -p "$SYSIMAGE_FROM"/{system.manifest,system.files,var.skel,rootfs.slt,.done-*} "$ROOT/sysimage/" 2> /dev/null || true
	# (an extraction made before this kit's files were unpacked: as new as they are, or build.sh takes it for stale)
	touch "$ROOT"/sysimage/.done-* "$ROOT/sysimage/system.manifest"
else
	(cd "$ROOT/sysimage" && bash fetch.sh)
fi

log "2/5 the firmware, from your phone's Android (read-only; allow root for the shell when the phone asks)"
if [ -n "${FW_FROM:-}" ]; then
	for d in fw-gpu fw-audio fw-wifi fw-bluetooth fw-camera fw-sensors fw-haptic; do
		[ -d "$FW_FROM/$d" ] && { rm -rf "$ROOT/$d"; cp -r "$FW_FROM/$d" "$ROOT/$d"; }
	done
else
	bash "$ROOT/devices/redmi-k40/kit/pull-firmware.sh" "$ROOT"
fi
# the loader shows the phone's own serial over USB, so one "fastboot -s" reaches both bootloaders (start.bat takes
# "xnuloader" as well)
s=${SERIAL:-$("$ADB" get-serialno 2> /dev/null | tr -d '\r' || true)}
mkdir -p "$ROOT/fw-usb"
[[ "$s" =~ ^[A-Za-z0-9]+$ ]] && echo "$s" > "$ROOT/fw-usb/serial" || rm -f "$ROOT/fw-usb/serial"

log "3/5 the Wi-Fi network iOS joins at every boot"
if [ ! -f "$ROOT/fw-wifi/network" ] || [ -n "${SSID:-}" ]; then
	[ -n "${SSID:-}" ] || read -r -p "Wi-Fi name (SSID): " SSID
	[ -n "${PSK:-}" ] || { read -r -s -p "its password: " PSK; echo; }
	mkdir -p "$ROOT/fw-wifi"
	(umask 077; printf '%s\n%s\n' "$SSID" "$PSK" > "$ROOT/fw-wifi/network")
fi

log "4/5 the system image and the root disk, with the kit's programs (about an hour, mostly packing 2.3 GB)"
PREBUILT=1 PREBUILT_DIR="$KIT/prebuilt/userland" bash "$ROOT/devices/redmi-k40/build-userland.sh" > "$KIT/userland.log" 2>&1 ||
	{ tail -30 "$KIT/userland.log"; die "the images were not made (log: $KIT/userland.log)"; }
U=$ROOT/out/redmi-k40/userland-build/userland
[ -f "$U/root.apfs" ] && [ -f "$U/system.apfs" ] || die "no root.apfs / system.apfs (log: $KIT/userland.log)"

log "5/5 the loader (with the identity from the IPSW), the boot image and the payload"
(cd "$ROOT" && bash devices/redmi-k40/build-loader.sh > "$KIT/loader.log" 2>&1) || { tail -20 "$KIT/loader.log"; die "the loader did not build"; }
O=$ROOT/out/redmi-k40
python3 "$ROOT/xnuloader/mkpayload.py" -o "$O/usb-payload.bin" loader.efi="$O/loader.efi" --usb
(cd "$ROOT" && UEFI_TARGET=alioth FD_BASE=0x9FC00000 FD_SIZE=0x300000 PAYLOAD_BASE=0xF0000000 \
	UEFI_FD=devices/redmi-k40/prebuilt/SILICIUM_UEFI.fd DTB=devices/redmi-k40/prebuilt/device-tree.dtb \
	SHIM=devices/redmi-k40/prebuilt/BootShim.bin \
	python3 devices/redmi-k40/tools/mkimage.py -p "$O/usb-payload.bin" -o "$O/alioth-xnu.img" > /dev/null)
mac=
[ -f "$ROOT/fw-wifi/mac" ] && mac=$(head -1 "$ROOT/fw-wifi/mac" | tr -d '\r ')
mkdir -p "$OUT/boot" "$OUT/bin" "$OUT/apps"
# the payload as the one-click script expects it (start.ps1 adds the clock, the camera and the apps' token at boot):
# "XNUP", the kernel, the root disk, the kernel's arguments -- this phone's verified set, usb=3 (the PC sees an iPhone),
# touch_exit=0 (five fingers held on the screen do not end iOS: that is the test runs' way out)
python3 - "$KIT/prebuilt/kernel-k40.bin" "$U/root.apfs" "$ROOT/devices/redmi-k40/boot.args" "$mac" "$OUT/boot/run-payload.bin" <<'EOF'
import struct, sys
kernel, root, args, mac, out = sys.argv[1:]
words = open(args).read().split()
if not any(w.startswith('usb=') for w in words):
    words.append('usb=3')
if not any(w.startswith('touch_exit=') for w in words):
    words.append('touch_exit=0')
if mac and not any(w.startswith('wifi_mac=') for w in words):
    words.append('wifi_mac=' + mac)
cmdline = ' '.join(['sb_delay=8'] + words)
body = b''
for name, path in (('kernel', kernel), ('ramdisk', root)):
    data = open(path, 'rb').read()
    body += struct.pack('<16sII', name.encode(), len(data), 0) + data + b'\0' * (-len(data) % 16)
c = cmdline.encode() + b'\0'
body += struct.pack('<16sII', b'cmdline', len(c), 0) + c + b'\0' * (-len(c) % 16) + b'\0' * 24
payload = struct.pack('<II', 0x50554E58, len(body)) + body
if len(payload) > 63 << 20:
    sys.exit('assemble: the payload is %d bytes, more than the loader takes (63 MB)' % len(payload))
open(out, 'wb').write(payload)
print('payload %d bytes; kernel arguments: %s' % (len(payload), cmdline.replace(mac, '<this phone>') if mac else cmdline))
EOF
cp "$O/alioth-xnu.img" "$OUT/boot/alioth-boot.img"
echo "copying the system image ($(( $(stat -c %s "$U/system.apfs") >> 20 )) MB)"
cp "$U/system.apfs" "$OUT/boot/system.apfs"
P=$ROOT/devices/redmi-k40/kit/package
cp "$P"/start.ps1 "$P"/*.bat "$P"/说明.txt "$P"/README.txt "$OUT/"
cp "$P"/apps-说明.txt "$OUT/apps/说明.txt"
cp "$PT"/adb.exe "$PT"/fastboot.exe "$PT"/AdbWinApi.dll "$PT"/AdbWinUsbApi.dll "$OUT/bin/" 2> /dev/null || true
# the apps written for this port, from their releases on GitHub (installed at every boot from apps/)
for app in ios-gpu-check:GPUCheck ios-sensor-check:SensorCheck ios-motion-test:MotionTest ios-map:Map; do
	repo=${app%%:*} ipa=${app##*:}
	url=$(curl -fsSL "https://api.github.com/repos/xiongnemo/$repo/releases/latest" | grep -o "https://[^\"]*/$ipa-[^\"]*\.ipa" | head -1 || true)
	[ -n "$url" ] && curl -fsSL -o "$OUT/apps/$(basename "$url")" "$url" && echo "apps/$(basename "$url")" || echo "($repo: no release to fetch)"
done
echo
echo "assemble: the package is $OUT"
echo "  With the phone in Android and connected, run start.bat (启动.bat) there. Nothing is flashed; a reboot brings Android back."
echo "  It holds Apple's files, the vendor's firmware and your Wi-Fi password: keep it to yourself."
