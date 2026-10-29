#!/bin/bash
# Install the source port into the exact Mu-Silicium revision it was tested on.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
mu="$root/Mu-Silicium"
[ -d "$mu/.git" ] || { echo "Clone Mu-Silicium and check out $(cat "$here/BASE") first" >&2; exit 1; }
[ "$(git -C "$mu" rev-parse HEAD)" = "$(cat "$here/BASE")" ] || { echo 'Mu-Silicium revision differs from uefi/umi/BASE' >&2; exit 1; }
cp -a "$here/files/." "$mu/"
cp "${DTB:-$root/devices/redmi-k40/prebuilt/device-tree.dtb}" "$mu/Resources/DTBs/umi.dtb"
echo 'UMI source port installed; extract.py supplies the device firmware modules.'
