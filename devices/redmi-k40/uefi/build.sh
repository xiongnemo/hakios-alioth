#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
python=${UEFI_PYTHON:-$root/venv-uefi/bin/python}
command -v nasm >/dev/null || { echo 'nasm must be on PATH' >&2; exit 1; }
# Use the local nasm instead of requiring Mono for the NuGet distribution.
dep=Mu-Silicium/Mu_Basecore/BaseTools/Bin/mu_nasm_extdep
mkdir -p "$dep/Linux-x86-64"
printf 'version: 20016.1.1\n' > "$dep/extdep_state.yaml"
ln -sfn "$(command -v nasm)" "$dep/Linux-x86-64/nasm"
ln -sfn "$(command -v ndisasm)" "$dep/Linux-x86-64/ndisasm"
if [ "${PREPARE:-0}" = 1 ]; then
    "$python" Mu-Silicium/Platforms/Xiaomi/umiPkg/DeviceBuild.py --setup -t RELEASE
    "$python" Mu-Silicium/Platforms/Xiaomi/umiPkg/DeviceBuild.py --update -t RELEASE
fi
"$python" Mu-Silicium/Platforms/Xiaomi/umiPkg/DeviceBuild.py TARGET=RELEASE ENABLE_SECUREBOOT=0 \
    FD_BASE=0x9fc00000 FD_SIZE=0x300000 FD_BLOCKS=0x300 DEVICE_MODEL=0
devices/redmi-k40/build-loader.sh
