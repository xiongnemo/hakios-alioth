#!/bin/bash
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
python3 "$DEVICE_DIR/prepare.py" userland
export DEVICE=umi BLUETOOTH=1 GL=1
export RC_JOBS=rc.jobs.umi-boot
export ADSP= REST=${REST:-1}
exec "$ROOT/out/redmi-k40/userland-build/userland/build.sh" "$@"
