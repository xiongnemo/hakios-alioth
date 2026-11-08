#!/bin/bash
# The Metal side of Mesa for the K40 (plan M1): libhakios.air.dylib -- hackios-xnu-userland a79ff07's air_metal.c,
# the rwa_* library a Metal driver calls (a Metal function's bitcode through the AIR frontend into Gallium, drawn or
# dispatched on the Adreno) -- and air_metal_test, the library driven as the driver drives it.
#
# A tree of its own (xnu-deps/build/mesa-metal), so that OpenGL ES's libOSMesa (tools/mk-mesa.sh) is not touched:
# the same Mesa release with tools/mesa/*.patch, except that air-ir3.patch is tools/mesa/metal's (ours plus the
# library wrapper), then tools/mesa/metal/*.patch (air-metal.patch: the library; two ir3 patches the Metal path
# needs); the frontend is tools/mesa/metal/air (a79ff07's, MIT: tools/mesa/metal/LICENSE.hackios-xnu-userland).
# -> xnu-deps/build/mesa-metal/out/libhakios.air.dylib (install name /redwood/lib/libhakios.air.dylib), air_metal_test
set -e
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
source "$ROOT/xnu-env.sh"
B=$ROOT/xnu-deps/build/mesa-metal
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
CC=$ROOT/xnu-deps/host/bin/xnu-clang
mkdir -p "$B/out"
cd "$B"

patches() {
	for p in "$ROOT"/tools/mesa/*.patch; do
		[ "$(basename "$p")" = air-ir3.patch ] || echo "$p"
	done
	ls "$ROOT"/tools/mesa/metal/*.patch
}
PATCHES=$(cat $(patches) | sha256sum | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.hakios-patches 2>/dev/null)" != "$PATCHES" ]; then
	# (the archive the OpenGL ES build fetched, if it is there)
	[ -f mesa-$VERSION.tar.xz ] || cp "$ROOT/xnu-deps/build/mesa/mesa-$VERSION.tar.xz" . 2>/dev/null ||
		curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	for p in $(patches); do
		echo "mk-mesa-metal.sh: $(basename "$p")"
		patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"
	done
	echo "$PATCHES" > mesa-$VERSION/.hakios-patches
fi
rm -rf mesa-$VERSION/src/hakios-drm mesa-$VERSION/src/compiler/air
cp -r "$ROOT/tools/mesa/hakios-drm" mesa-$VERSION/src/hakios-drm
cp -r "$ROOT/tools/mesa/metal/air" mesa-$VERSION/src/compiler/air
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH

cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
DRIVERS=softpipe,freedreno
BUILD=build-${DRIVERS//,/-}
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C $BUILD src/gallium/drivers/freedreno/libhakios.air.dylib src/gallium/drivers/freedreno/air_metal_test > ninja.log
# -x keeps the global symbols (the rwa_* of air_metal.h); the install name is the path on the phone
llvm-strip -x -o out/libhakios.air.dylib $BUILD/src/gallium/drivers/freedreno/libhakios.air.dylib
install_name_tool -id /redwood/lib/libhakios.air.dylib out/libhakios.air.dylib
cp $BUILD/src/gallium/drivers/freedreno/air_metal_test out/air_metal_test
install_name_tool -change @rpath/libhakios.air.dylib /redwood/lib/libhakios.air.dylib out/air_metal_test 2>/dev/null || true
cat $(patches) "$ROOT"/tools/mesa/hakios-drm/* "$ROOT"/tools/mesa/metal/air/* | sha256sum > out/built-from
echo "mk-mesa-metal.sh: $B/out/libhakios.air.dylib ($(stat -c %s out/libhakios.air.dylib) bytes," \
	"$(nm -gU out/libhakios.air.dylib | grep -c ' _rwa_') rwa_* functions), air_metal_test ($(stat -c %s out/air_metal_test) bytes)"