#!/bin/bash
# hakios-mali for the 27 release's processes (arm64e): the library that writes the Mali-G52's job descriptors itself
# (userland/src/mali, no Gallium, no Panfrost) and its probe, mali-probe, which runs every stage against the kernel's
# HakiosMali. Built with the system's clang (host/bin/xnu-clang takes it for arm64e), as metal27.sh builds the Metal
# driver. The descriptors are packed by Mesa's generated genxml/v7_pack.h, made here from the Mesa tree's v7.xml by
# Mesa's gen_pack.py (MESA_SRC: kernel/deps/build/mesa/mesa-25.0.7, which kernel/deps/setup.sh unpacks).
#
# Output: $OBJ/mali/{mali-probe, libhakios-mali.a} (OBJ: as mtk27.sh's, build/ios27/obj), for root26-sys.sh
# MALI27=$OBJ/mali, which puts mali-probe into the small image as /hakios/bin/mali-probe.
# On this machine, against the fake GPU: make -C userland/src/mali test.
set -e
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/kernel/env.sh"
MESA_SRC=${MESA_SRC:-$ROOT/kernel/deps/build/mesa/mesa-25.0.7}
O=${OBJ:-$ROOT/build/ios27/obj}/mali
SDK=${SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}
PACSTRIP=$ROOT/system27/tools/pacstrip
LDID=${LDID:-$ROOT/system27/tools/macos/ldid}
S=$ROOT/userland/src/mali
[ -f "$MESA_SRC/src/panfrost/lib/genxml/v7.xml" ] || { echo "mali27.sh: no Mesa tree in $MESA_SRC (kernel/deps/setup.sh)"; exit 1; }
[ -d "$SDK" ] || { echo "mali27.sh: no $SDK"; exit 1; }
if [ ! -x "$PACSTRIP" ] || [ "$ROOT/system27/tools/pacstrip.c" -nt "$PACSTRIP" ]; then
	# as mtk27.sh env builds it
	clang -O2 -w -I"$ROOT/system27/tools/macos" -include "$ROOT/system27/tools/macos/cfr.h" -o "$PACSTRIP" \
		"$ROOT/system27/tools/pacstrip.c"
fi
mkdir -p $O/gen/genxml
python3 "$MESA_SRC/src/panfrost/lib/genxml/gen_pack.py" "$MESA_SRC/src/panfrost/lib/genxml/v7.xml" > $O/gen/genxml/v7_pack.h

CC="xnu-clang -arch arm64e -isysroot $SDK -mios-version-min=16.0"
CXX="xnu-clang++ -arch arm64e -isysroot $SDK -mios-version-min=16.0"
$CC -std=c11 -O2 -I$O/gen -I"$MESA_SRC/src" -I"$MESA_SRC/include" -I"$MESA_SRC/src/panfrost/lib" -c $S/v7.c -o $O/v7.o
for f in gpu pass draw fake_gpu iokit_transport probe; do
	$CXX -std=c++17 -O2 -fno-exceptions -fno-rtti -I$S -c $S/$f.cpp -o $O/$f.o
done
rm -f $O/libhakios-mali.a
ar rcs $O/libhakios-mali.a $O/v7.o $O/gpu.o $O/pass.o $O/draw.o $O/fake_gpu.o $O/iokit_transport.o
$CXX -o $O/mali-probe $O/probe.o $O/libhakios-mali.a -framework IOKit -framework CoreFoundation -lc++
# arm64e code made runnable on a processor without pointer authentication, then signed (root26-sys.sh signs it
# again, ad hoc, with the rest of the small image)
$PACSTRIP macho $O/mali-probe $O/mali-probe.strip > /dev/null && mv $O/mali-probe.strip $O/mali-probe
"$LDID" -S $O/mali-probe
file -b $O/mali-probe | cut -c1-60
ls -la $O/mali-probe $O/libhakios-mali.a
