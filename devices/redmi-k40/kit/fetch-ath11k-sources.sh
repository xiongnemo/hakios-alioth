#!/bin/bash
# The Wi-Fi build's two source archives (tools/mk-ath11k.sh: ath11k from Linux, net80211 and its kin from FreeBSD),
# fetched ahead into DIR with retries and checked against mk-ath11k.sh's own checksums. mk-ath11k.sh fetches them
# itself with "curl -s --fail": no retry, and nothing in the log when a download fails -- two CI runs in a row ended
# "build-kit: Wi-Fi did not build" with an empty log (2026-11-19). ATH11K_CACHE=DIR then hands them to
# devices/redmi-k40/tools/stage-wifi.py, which links them where mk-ath11k.sh looks first.
# usage: fetch-ath11k-sources.sh DIR
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
M=$ROOT/tools/mk-ath11k.sh
D=${1:?usage: fetch-ath11k-sources.sh DIR}
get() { sed -n "s/^$1=//p" "$M" | head -1; }
V=$(get VERSION)
VS=$(get SHA256)
F=$(get FREEBSD)
FS=$(get FREEBSD_SHA256)
[ -n "$V" ] && [ -n "$VS" ] && [ -n "$F" ] && [ -n "$FS" ] || { echo "fetch-ath11k: no versions in $M" >&2; exit 1; }
mkdir -p "$D"
fetch() {       # FILE URL SHA256
	local f=$D/$1
	if [ -s "$f" ] && echo "$3  $f" | sha256sum -c --quiet 2>/dev/null; then
		echo "fetch-ath11k: $1 (already here)"
		return
	fi
	rm -f "$f"
	curl -fsSL --retry 6 --retry-delay 20 --retry-all-errors --connect-timeout 30 -o "$f" "$2"
	echo "$3  $f" | sha256sum -c --quiet || { rm -f "$f"; echo "fetch-ath11k: $1 is not the archive expected" >&2; exit 1; }
	echo "fetch-ath11k: $1 ($(du -h "$f" | cut -f1))"
}
fetch "linux-$V.tar.xz" "https://cdn.kernel.org/pub/linux/kernel/v${V%%.*}.x/linux-$V.tar.xz" "$VS"
fetch "freebsd-$F-src.txz" "https://download.freebsd.org/releases/amd64/$F-RELEASE/src.txz" "$FS"
