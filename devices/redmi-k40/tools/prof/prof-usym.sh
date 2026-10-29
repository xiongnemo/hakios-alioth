#!/bin/bash
# User addresses of a prof=1 run -> functions: the GL library and eagl (our own dylibs, at the bases eagl said),
# else the dyld shared cache (minus its slide). usage: prof-usym.sh TOPFILE SLIDE MESA_BASE EAGL_BASE [N]
TOP=$1 SLIDE=$(($2)) MESA=$(($3)) EAGL=$(($4)) N=${5:-50}
R=~/hk/out/redmi-k40/userland-build/userland/root/lib
MESALIB=~/hk/xnu-deps/build/mesa/build-softpipe-freedreno/src/gallium/targets/osmesa/libOSMesa.8.dylib   # (unstripped: same layout)
EAGLLIB=$R/libhakios.gl.dylib
D=~/dyldcache/dyld_shared_cache_arm64
I=~/tools/ipsw/ipsw
mesa_end=$((MESA + 0x4000000)); eagl_end=$((EAGL + 0x200000))
grep '^prof: u ' "$TOP" | sed 's/\r$//' | head -"$N" | while read -r _ _ addr count; do
	a=$((addr & 0x7fffffffffffffff)); k=""
	[ $((addr >> 63 & 1)) = 1 ] && k="[syscall from] "
	if [ $a -ge $MESA ] && [ $a -lt $mesa_end ]; then
		off=$(printf '%#x' $((a - MESA)))
		sym="libOSMesa+$off $(llvm-symbolizer --obj=$MESALIB --inlining=false $off 2>/dev/null | head -1)"
	elif [ $a -ge $EAGL ] && [ $a -lt $eagl_end ]; then
		off=$(printf '%#x' $((a - EAGL)))
		sym="eagl+$off $(llvm-symbolizer --obj=$EAGLLIB --inlining=false $off 2>/dev/null | head -1)"
	elif [ $a -ge $((0x180000000 + SLIDE)) ]; then
		u=$(printf '%#x' $((a - SLIDE)))
		sym="cache $u $($I dyld a2s "$D" "$u" 2>&1 | tail -1 | sed 's/.*INFO//')"
	else
		sym="?"
	fi
	printf '%6s %s%s\n' "$count" "$k" "$sym"
done
