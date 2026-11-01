#!/bin/bash
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
export XNU_SRC="$ROOT/out/redmi-k40/xnu"
python3 "$DEVICE_DIR/prepare.py" kernel
# The Bluetooth controller's firmware into the kernel when fw-bluetooth/ has it; BT_EMBED=0 (and any build without it,
# such as the published kit's) leaves it out, and the driver reads it from the system image's /fw at boot instead.
BT_FW="$ROOT/fw-bluetooth"
[ "${BT_EMBED:-1}" != 0 ] || BT_FW=/nonexistent
python3 "$DEVICE_DIR/tools/embed-firmware.py" \
  --patch "$BT_FW/htbtfw20.tlv" --nvm "$BT_FW/htnv20.bin" \
  --output "$XNU_SRC/bsd/dev/umi_bt_firmware.inc"
mkdir -p "$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT"
[ ! -f "$ROOT/xnu-deps/build/ath11k/src/ath11k/qmi.c" ] || "$ROOT/tools/mk-wlfw.sh"
exec bash "$ROOT/xnu-build.sh" "${@:--j8}"
