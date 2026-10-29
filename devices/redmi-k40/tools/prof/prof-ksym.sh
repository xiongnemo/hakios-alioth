#!/bin/bash
# Kernel addresses of a prof=1 run (unslid) -> functions of the K40 kernel. usage: prof-ksym.sh TOPFILE
K=~/hk/out/redmi-k40/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped
[ -f $K ] || K=~/hk/out/redmi-k40/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
grep '^prof: k ' "$1" | sed 's/\r$//' | head -25 | while read -r _ _ addr count; do
	sym=$(llvm-symbolizer --obj=$K --functions=linkage --inlining=false "$addr" 2>/dev/null | head -1)
	printf '%6s %s %s\n' "$count" "$addr" "$sym"
done
