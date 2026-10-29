#!/bin/bash
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
OUT="$ROOT/out/redmi-k40"
export XL_KERNEL="$OUT/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837"
export XL_LOADER="$OUT/loader.efi"
export UEFI_TARGET=umi FD_BASE=0x9FC00000 FD_SIZE=0x300000 PAYLOAD_BASE=0xF0000000
export DTB="${DTB:-$DEVICE_DIR/prebuilt/device-tree.dtb}"
export UEFI_FD="${UEFI_FD:-$ROOT/Mu-Silicium/Build/umiPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd}"
export XL_CMDLINE="${XL_CMDLINE-$(cat "$DEVICE_DIR/boot.args")}"
python3 "$DEVICE_DIR/tools/stage-runtime.py"
if [[ " $XL_CMDLINE " == *" wifi=1 "* && " $XL_CMDLINE " != *" wifi_mac="* ]] && [ -f "$OUT/wifi-boot.args" ]; then
  XL_CMDLINE="$XL_CMDLINE $(cat "$OUT/wifi-boot.args")"; export XL_CMDLINE
fi
exec "$OUT/runtime/xl" "$@"
