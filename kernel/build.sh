#!/bin/bash
# Build the kernel. Extra arguments go to make (-j8 -k: -k is needed, the one harmless error of the
# build, installing lldbmacros, ends make with code 2).
#
# Before make: the tree is made or brought up to date with the repository (kernel/assemble.sh), the
# board table is written into it for the device packages that are here (tools/boards.py gen), and the
# camera's two tables, if you made them from your own phone's capture (build/kernel/), are put there.
#   HAKIOS_XNU_FROM=SRC   take Apple's release from this repository or clone instead of the network
set -e
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
if [ -d "$XNU_SRC" ]; then
	"$HAKIOS_KERNEL/assemble.sh" --update "$XNU_SRC"
else
	"$HAKIOS_KERNEL/assemble.sh" ${HAKIOS_XNU_FROM:+--from "$HAKIOS_XNU_FROM"} "$XNU_SRC"
fi
"$HAKIOS/tools/boards.py" gen > /dev/null
for made in "$HAKIOS/build/boards/hakios_board_table.h" "$HAKIOS"/build/kernel/*.h; do
	[ -f "$made" ] || continue
	cmp -s "$made" "$XNU_SRC/bsd/dev/${made##*/}" || cp "$made" "$XNU_SRC/bsd/dev/"
done
cd "$XNU_SRC"
# The tools the build compiles for this machine (SETUP/) must use this machine's own headers. On
# macOS clang takes SDKROOT from the environment, and SDKROOT here says iphoneos, so without this
# they are compiled against the wrong SDK and stop at "architecture not supported".
# kernel/deps/hostinc stands in for what macOS has and Linux does not (getprogname, byteswap.h,
# the Mach-O headers); on macOS it is in the way, and the SDK is this machine's own.
if [ "$(uname -s)" = Darwin ]; then
	HOST_EXTRA="-isysroot $(/usr/bin/xcrun --show-sdk-path)"
else
	HOST_EXTRA="-include $XNUDEPS/hostinc/hostcompat.h -I$XNUDEPS/hostinc"
fi
exec make HAKIOS_OBJECTS= SDKROOT=iphoneos ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=${MACHINE:-BCM2837} KERNEL_CONFIGS=DEVELOPMENT \
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 \
  HOST_CC="clang $HOST_EXTRA -Wno-unused-command-line-argument -Wno-error=implicit-function-declaration" HOST_FLEX=flex HOST_BISON=bison HOST_GM4=m4 CC=xnu-clang CXX=xnu-clang++ LOGCOLORS=n "$@"
