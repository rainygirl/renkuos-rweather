#!/bin/sh
# Cross-compile rweather for x86_64 or arm64 Haiku on a workstation with
# Docker, producing dist/<arch>/rweather without resources (the pkgman recipe
# attaches them on Haiku). Same toolchains as R Bluesky's tools/cross-build.sh:
#
#   tools/cross-build.sh x86_64    # haiku/cross-compiler:x86_64-r1beta4 image
#   tools/cross-build.sh arm64     # container with a Haiku arm64 build tree
#
#   ARM64_CONTAINER   default haiku-builder
#   ARM64_GENERATED   default /root/renku-arm64-work/generated.arm64
set -e

ARCH="$1"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/dist/$ARCH"
mkdir -p "$OUT"
FLAGS="-Wall -Wno-multichar -O2 -o rweather rweather.cpp -lbe -lbnetapi -lnetwork -Xlinker -soname=_APP_"

case "$ARCH" in
x86_64)
	( cd "$HERE" && tar cf - rweather.cpp ) \
		| docker run --rm -i --platform linux/amd64 haiku/cross-compiler:x86_64-r1beta4 \
			sh -c "mkdir -p /work && cd /work && tar xf - && x86_64-unknown-haiku-g++ $FLAGS >&2 && tar cf - rweather" \
		| tar xf - -C "$OUT"
	;;
arm64)
	CONTAINER="${ARM64_CONTAINER:-haiku-builder}"
	GEN="${ARM64_GENERATED:-/root/renku-arm64-work/generated.arm64}"
	P="$GEN/objects/haiku/arm64/packaging/packages_build/minimum"
	WORK="/tmp/rweather-cross-$$"
	( cd "$HERE" && tar cf - rweather.cpp ) \
		| docker exec -i "$CONTAINER" sh -c "
			set -e
			mkdir -p $WORK/sysroot/boot/system $WORK/lib && cd $WORK && tar xf -
			ln -sfn $P/hpkg_-haiku_devel.hpkg/contents/develop $WORK/sysroot/boot/system/develop
			SYSLIBS=\$(ls -d $GEN/build_packages/gcc_syslibs-*-arm64/lib | head -1)
			ln -sfn \$SYSLIBS/libstdc++.so.6 $WORK/lib/libstdc++.so
			ln -sfn \$SYSLIBS/libgcc_s.so.1 $WORK/lib/libgcc_s.so
			$GEN/cross-tools-arm64/bin/aarch64-unknown-haiku-g++ --sysroot=$WORK/sysroot \
				-L$P/hpkg_-haiku.hpkg/contents/lib -L$WORK/lib $FLAGS >&2
			tar cf - rweather
			rm -rf $WORK" \
		| tar xf - -C "$OUT"
	;;
*)
	echo "usage: $0 x86_64|arm64" >&2
	exit 1
	;;
esac

file "$OUT/rweather"
