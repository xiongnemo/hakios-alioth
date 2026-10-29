#!/bin/bash
# Kernel places with their callers of a prof=1 run (prof: s PC CALLER CALLER2 COUNT) -> function names.
K=~/hk/out/redmi-k40/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped
[ -f $K ] || K=~/hk/out/redmi-k40/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
name() { [ "$1" = 0x0 ] && { echo -; return; }; llvm-symbolizer --obj=$K --functions=linkage --inlining=false "$1" 2>/dev/null | head -1; }
grep '^prof: s ' "$1" | sed 's/\r$//' | head -${2:-25} | while read -r _ _ pc c1 c2 count; do
	printf '%6s %s <- %s <- %s\n' "$count" "$(name $pc)" "$(name $c1)" "$(name $c2)"
done
