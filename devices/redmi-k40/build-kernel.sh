#!/bin/bash
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
export XNU_SRC="$ROOT/out/redmi-k40/xnu"
python3 "$DEVICE_DIR/prepare.py" kernel
python3 "$DEVICE_DIR/tools/embed-firmware.py" \
  --patch "$ROOT/fw-bluetooth/htbtfw20.tlv" --nvm "$ROOT/fw-bluetooth/htnv20.bin" \
  --output "$XNU_SRC/bsd/dev/umi_bt_firmware.inc"
mkdir -p "$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT"
[ ! -f "$ROOT/xnu-deps/build/ath11k/src/ath11k/qmi.c" ] || "$ROOT/tools/mk-wlfw.sh"
exec "$ROOT/xnu-build.sh" "${@:--j8}"
