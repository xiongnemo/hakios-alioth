#!/bin/bash
# The firmware this port's drivers load, copied from the phone's own Android into the fw-* directories build.sh reads
# (GPU, audio DSP and amplifiers, Wi-Fi, Bluetooth, vibration, cameras, sensor DSP). Read-only: nothing on the phone
# is written. The phone must be in Android with USB debugging on, and give root to the shell (su: Magisk or the like),
# because the DSPs' images, the Bluetooth partition and the calibration in /mnt/vendor/persist are root's.
#
# usage: pull-firmware.sh OUT_DIR       (ADB=path/to/adb, SERIAL=... to pick one phone of several)
set -euo pipefail
OUT=${1:?usage: pull-firmware.sh OUT_DIR}
ADB=${ADB:-adb}
command -v "$ADB" > /dev/null 2>&1 || [ -x "$ADB" ] || { echo "pull-firmware: no adb ($ADB)" >&2; exit 1; }
A=("$ADB")
[ -z "${SERIAL:-}" ] || A+=(-s "$SERIAL")
# A command for the phone's shell, as root: given on su's standard input, so its quotes and wildcards are the phone
# shell's own. Its output without the carriage returns Windows' adb adds.
run() { printf '%s\n' "$1" | "${A[@]}" shell su 2>/dev/null | tr -d '\r'; }
# Files listed by a command on the phone (one path a line), into a directory, each by its own name: tar through
# exec-out, which is binary-safe. Prints how many; fails when the list is empty.
grab() {
	local into=$1 list
	list=$(run "$2" | grep '^/' | sort -u || true)
	[ -n "$list" ] || return 1
	mkdir -p "$into"
	local t
	t=$(mktemp -d)
	# (tar's own messages must not get into the stream: adb's exec-out carries the phone's stderr with its stdout)
	"${A[@]}" exec-out "su -c 'tar -cf - $(echo $list) 2>/dev/null'" | tar -C "$t" -xf - 2>/dev/null || true
	find "$t" -type f -exec cp -f {} "$into/" \;
	chmod -R u+rwX "$into"                 # (the phone's are read-only for everyone but root)
	rm -rf "$t"
	echo "  $(echo "$list" | wc -l) files -> $into"
}
need() { echo "pull-firmware: $1 not found on the phone" >&2; exit 1; }

state=$("${A[@]}" get-state 2>/dev/null | tr -d '\r' || true)
[ "$state" = device ] || { echo "pull-firmware: no phone in Android over adb (state: ${state:-none})" >&2; exit 1; }
model=$("${A[@]}" shell getprop ro.product.device | tr -d '\r')
case "$model" in alioth|aliothin) ;; *) echo "pull-firmware: this is \"$model\", not alioth (Redmi K40 / POCO F3)" >&2; exit 1;; esac
run id | grep -q 'uid=0' || { echo "pull-firmware: su gave no root shell (allow it on the phone, then run this again)" >&2; exit 1; }
mkdir -p "$OUT"

echo "pull-firmware: GPU"
grab "$OUT/fw-gpu" "find /vendor/firmware -maxdepth 1 -name 'a650_*'" || need "a650_* (GPU)"

echo "pull-firmware: audio DSP and speaker amplifiers"
grab "$OUT/fw-audio" "find /vendor/firmware_mnt/image -maxdepth 1 -name 'adsp.*'" || need "adsp.* (audio DSP)"
grab "$OUT/fw-audio/cs35l41" "find /vendor/firmware -maxdepth 1 -name 'cs35l41-dsp1-spk-prot-revb2.*' -o -maxdepth 1 \
	-name 'cs35l41-dsp1-spk-prot-RCV-revb2.bin'; find /mnt/vendor/persist -name 'cs35l41_cal*.bin'" || need "cs35l41 tuning"

echo "pull-firmware: Bluetooth"
grab "$OUT/fw-bluetooth" "find /vendor/bt_firmware /bt_firmware /vendor/firmware -name htbtfw20.tlv -o -name htnv20.bin" ||
	need "htbtfw20.tlv / htnv20.bin (Bluetooth)"

# The stock NVM has no address of its own in tag 2 (a placeholder): Android hands the controller this phone's, which
# Android keeps (Settings' bluetooth_address; some builds have it as ro.boot.btmacaddr). Written into the NVM here, as Linux's btqca.c does at run time; without
# it every K40 would have the same Bluetooth address.
bd=$("${A[@]}" shell settings get secure bluetooth_address | tr -d '\r')
[[ "$bd" == *:* ]] || bd=$("${A[@]}" shell getprop ro.boot.btmacaddr | tr -d '\r')
[[ "$bd" == *:* ]] || bd=$("${A[@]}" shell getprop persist.vendor.service.bdroid.bdaddr | tr -d '\r')
python3 - "$OUT/fw-bluetooth/htnv20.bin" "$bd" <<'EOF' || need "this phone's Bluetooth address (turn Bluetooth on once in Android)"
import re, struct, sys
path, bd = sys.argv[1:]
if not re.fullmatch(r'([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}', bd):
    sys.exit(1)
data = bytearray(open(path, 'rb').read())
assert data[0] == 2 and int.from_bytes(data[1:4], 'little') == len(data) - 4, 'not a QCA NVM file'
off = 4
while off + 12 <= len(data):                   # records: u16 tag, u16 length, 8 bytes of header, the data
    tag, length = struct.unpack('<HH', data[off:off + 4])
    if tag == 2:
        data[off + 12:off + 18] = bytes.fromhex(bd.replace(':', ''))[::-1]   # (least significant byte first)
        open(path, 'wb').write(data)
        print('  the Bluetooth address written into htnv20.bin')
        sys.exit(0)
    off += 12 + length
sys.exit('no address tag in htnv20.bin')
EOF

echo "pull-firmware: vibration"
grab "$OUT/fw-haptic" "ls /vendor/firmware/aw8697_haptic.bin" || echo "  (none: no vibration patterns)"

echo "pull-firmware: Wi-Fi"
# (in the modem partition's image/ on this phone; /vendor/firmware on others)
grab "$OUT/fw-wifi/qca6390" "for d in /vendor/firmware_mnt/image/qca6390 /vendor/firmware/qca6390; do for f in amss20.bin m3.bin \
	regdb.bin bd_k11a.elf; do [ -f \$d/\$f ] && echo \$d/\$f; done; done" || need "qca6390/ (Wi-Fi)"
grab "$OUT/fw-wifi" "find /vendor/etc /vendor/firmware -name WCNSS_qcom_cfg.ini | head -1" || echo "  (no WCNSS_qcom_cfg.ini)"
# (the address the radio is to use: Android's for Wi-Fi, which the router knows already)
mac=$(run "cat /sys/class/net/wlan0/address" | head -1)
[[ "$mac" =~ ^[0-9a-f:]{17}$ ]] && echo "$mac" > "$OUT/fw-wifi/mac" || echo "  (no Wi-Fi address: wlan0 is down)"

echo "pull-firmware: cameras"
grab "$OUT/fw-camera" "find /vendor/lib64/camera -maxdepth 1 -name 'com.qti.sensormodule.alioth_*.bin' -o -maxdepth 1 \
	-name 'com.qti.tuned.alioth_*.bin'" || need "com.qti.sensormodule.alioth_* (cameras)"
# (each camera's module from the maker this phone has: Android's persist.vendor.camera.*.info)
"${A[@]}" shell getprop | tr -d '\r' | grep 'persist.vendor.camera.*info' | grep -o 'alioth_[a-z0-9]*_[a-z0-9]*_[a-z]*' |
	sort -u > "$OUT/fw-camera/fitted" || true

echo "pull-firmware: sensor DSP"
grab "$OUT/fw-sensors" "find /vendor/firmware_mnt/image -maxdepth 1 -name 'slpi.*'" || need "slpi.* (sensor DSP)"
S="$OUT/fw-sensors/sensors"
rm -rf "$S"
mkdir -p "$S/mnt/vendor/persist/sensors" "$S/vendor/etc/acdbdata" "$S/sys/devices/soc0" "$S/usr/lib/qcom/adsp"
# (what the sensors' process reads through the kernel, laid out as hexagonrpcd names it: its JSON configuration, the
# registry and calibration, the SLPI's own libraries, the SoC's identity -- see bsd/dev/hakios.slpi.c)
"${A[@]}" exec-out "su -c 'tar -C /vendor/etc -cf - sensors 2>/dev/null'" 2>/dev/null | tar -C "$S/vendor/etc" -xf -
"${A[@]}" exec-out "su -c 'tar -C /mnt/vendor/persist/sensors -cf - . 2>/dev/null'" 2>/dev/null | tar -C "$S/mnt/vendor/persist/sensors" -xf - || true
"${A[@]}" exec-out "su -c 'tar -C /vendor/dsp/sdsp -cf - . 2>/dev/null'" 2>/dev/null | tar -C "$S/usr/lib/qcom/adsp" -xf - || true
run 'for f in /sys/devices/soc0/*; do [ -f "$f" ] && echo "${f##*/} = $(cat "$f" 2>/dev/null)"; done' |
	while IFS= read -r line; do
		n=${line%% = *}; v=${line#* = }
		case "$n" in */*|""|.*) continue;; esac
		printf '%s\n' "$v" > "$S/sys/devices/soc0/$n"
	done
[ -d "$S/vendor/etc/sensors" ] || need "/vendor/etc/sensors"

echo "pull-firmware: $OUT: $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1)"
