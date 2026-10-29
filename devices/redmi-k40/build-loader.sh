#!/bin/bash
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
OUT="$ROOT/out/redmi-k40"
mkdir -p "$OUT"
if [ ! -f "$OUT/identity.h" ]; then
  [ -f "$ROOT/xnuloader/identity.h" ] || "$ROOT/xnuloader/mkidentity.sh"
  cp "$ROOT/xnuloader/identity.h" "$OUT/identity.h"
fi
if [ ! -f "$OUT/owner_key.h" ]; then
  printf '#define OWNER_KEY_PINNED 0\nstatic const uint8_t owner_key[32] = {0};\nstatic const char owner_key_fp[] = "";\n' > "$OUT/owner_key.h"
fi
[ -z "${OWNER_KEY_H:-}" ] || cp "$OWNER_KEY_H" "$OUT/owner_key.h"
if [ ! -f "$OUT/darwin_identity.h" ] || [ -n "${DARWIN_ID:-}" ]; then
  python3 "$DEVICE_DIR/tools/darwinid.py" header "${DARWIN_ID:-}" "$OUT/darwin_identity.h"
fi
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -DLOADER_UMI -DLOADER_ALIOTH \
  -I"$OUT" -I"$DEVICE_DIR/loader" -c "$DEVICE_DIR/loader/loader.c" -o "$OUT/loader.obj"
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 \
  /out:"$OUT/loader.efi" "$OUT/loader.obj"
ls -l "$OUT/loader.efi"
