#!/bin/bash
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
export XNU_SRC="$ROOT/out/redmi-k40/xnu"
python3 "$DEVICE_DIR/tools/stage-wifi.py"
exec bash "$ROOT/out/redmi-k40/wifi-build/tools/mk-ath11k.sh" "$@"
