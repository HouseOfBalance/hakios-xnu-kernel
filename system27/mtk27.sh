#!/bin/bash
# The 27 release's user space on the MediaTek port, from this repository on a Mac (macOS 14 was the one
# it was made on). The scripts of system27/ came from the Qualcomm port and expect its team's working tree,
# darwin-renoir, beside them; what they take from it is in this repository, so this script makes that
# tree out of links to it (build/ios27/darwin-renoir), with theos (build/ios27/theos), ldid and pacstrip.
#
#   system27/mtk27.sh env       the trees above, and system27/tools/pacstrip
#   system27/mtk27.sh gl        Mesa with panfrost for arm64e (tools/mk-mesa.sh MESA_ARCH=arm64e), and gl26.sh's
#                          libhakios-gl.dylib and gl_test over it
#   system27/mtk27.sh libs      inj26.sh: our libraries for the system's processes, arm64e (BUILD=24B119)
#   system27/mtk27.sh metal     metal27.sh: the Metal driver over that Mesa
#   system27/mtk27.sh mali      mali27.sh: hakios-mali (our own Mali job descriptors) and mali-probe, into $OBJ/mali
#   system27/mtk27.sh root      root26-sys.sh: the small image, $REL/root26-sys.apfs, with the Metal driver
#                          (and mali-probe, when the mali step has made it)
#   system27/mtk27.sh system    mkimage26.py --prune: the system image, $REL/system27-prune.apfs
#   system27/mtk27.sh all       all six, in this order
#
# REL: the extracted IPSW (iPhone12,1_27.2_24B119: rootfs/, cryptex/os/, cryptex/app/, dyld/, dyld-strip/);
# default build/ios27/ios27. RC_JOBS=FILE: the run's job list (root). Needs: userland/build.sh run once
# (its root/ is the small image's start), kernel/deps/setup.sh, theos's iPhoneOS16.5.sdk ($THEOS).
# Then: HAKIOS_PAYLOAD=flash tools/system-flash.sh $REL/system27-prune.apfs, HAKIOS_RAMDISK=$REL/root26-sys.apfs
# tools/mkpayload.sh, and tools/xnu-cycle.sh with HAKIOS_PAYLOAD=flash (docs/release27.md).
set -e -o pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/kernel/env.sh"
DK=$ROOT/build/ios27
DR=$DK/darwin-renoir
TH=$DK/theos
export REL=${REL:-$DK/ios27}
OBJ=$DK/obj
log() { echo "mtk27: $*"; }

make_env() {
	mkdir -p $DR/bin $DR/userland $DR/ios165 $DR/tools $DR/xnu-deps/host $TH/sdks $TH/toolchain/linux/iphone/bin \
		$DR/build-tools/theos/toolchain/linux/iphone/bin $OBJ
	[ -d "$THEOS/sdks/iPhoneOS16.5.sdk" ] || { echo "mtk27: no $THEOS/sdks/iPhoneOS16.5.sdk"; exit 1; }
	ln -sfn "$THEOS/sdks/iPhoneOS16.5.sdk" $TH/sdks/iPhoneOS16.5.sdk
	if [ "$(uname -s)" = Darwin ]; then
		cp "$ROOT/system27/tools/macos/ldid" $TH/toolchain/linux/iphone/bin/ldid
		cp "$ROOT/system27/tools/macos/ldid" $DR/build-tools/theos/toolchain/linux/iphone/bin/ldid
		# GNU sed's -i without a suffix, which macOS's sed takes as -i ''
		cat > $DR/bin/sed <<'SH'
#!/bin/bash
args=()
for a in "$@"; do if [ "$a" = -i ]; then args+=(-i ""); else args+=("$a"); fi; done
exec /usr/bin/sed "${args[@]}"
SH
		chmod +x $DR/bin/sed
	else
		REAL_LDID=$(command -v ldid || true)
		[ -z "$REAL_LDID" ] && [ -x "$THEOS/toolchain/linux/iphone/bin/ldid" ] && REAL_LDID="$THEOS/toolchain/linux/iphone/bin/ldid"
		[ -z "$REAL_LDID" ] && [ -x "$HOME/tools-dl/ldid" ] && REAL_LDID="$HOME/tools-dl/ldid"
		[ -n "$REAL_LDID" ] || { echo "mtk27: no Linux ldid binary found"; exit 1; }
		ln -sfn "$REAL_LDID" $TH/toolchain/linux/iphone/bin/ldid
		ln -sfn "$REAL_LDID" $DR/build-tools/theos/toolchain/linux/iphone/bin/ldid
	fi
	cat > $DR/xnu-env.sh <<SH
# darwin-renoir's xnu-env.sh, as the system27/ scripts use it: this repository's tools and theos
source "$ROOT/kernel/env.sh"
export THEOS=$TH
export PATH=$DR/bin:\$PATH
SH
	ln -sfn "$ROOT/system/select-system.py" $DR/ios165/select-system.py
	ln -sfn "$ROOT/tools/apfs" $DR/tools/apfs
	ln -sfn "$ROOT/tools/font_trim.py" $DR/tools/font_trim.py
	ln -sfn "$ROOT/kernel/deps/build" $DR/xnu-deps/build
	ln -sfn "$ROOT/kernel/deps/host/bin" $DR/xnu-deps/host/bin
	# userland: links, but root/ a copy of its own (root26-sys.sh writes into it). Its etc/rc hands process 1
	# to /sbin/launchd.system, which is the system's launchd in this image as in 16.5's (mkimage26.py; /sbin/launchd
	# is stage0, which mounts the small image on /hakios and runs its init)
	[ -d "$ROOT/userland/root/etc" ] || { echo "mtk27: no userland/root (run userland/build.sh once)"; exit 1; }
	for f in "$ROOT"/userland/*; do
		[ "${f##*/}" = root ] || ln -sfn "$f" "$DR/userland/${f##*/}"
	done
	rm -rf "${DR:?}/userland/root"
	cp -a "$ROOT/userland/root" $DR/userland/root
	if [ ! -x "$ROOT/system27/tools/pacstrip" ] || [ "$ROOT/system27/tools/pacstrip.c" -nt "$ROOT/system27/tools/pacstrip" ]; then
		log "building pacstrip"
		if [ "$(uname -s)" = Darwin ]; then
			clang -O2 -w -I"$ROOT/system27/tools/macos" -include "$ROOT/system27/tools/macos/cfr.h" \
				-o "$ROOT/system27/tools/pacstrip" "$ROOT/system27/tools/pacstrip.c"
		else
			cc -O2 -w -o "$ROOT/system27/tools/pacstrip" "$ROOT/system27/tools/pacstrip.c" -lcrypto
		fi
	fi
	# gl26.sh writes into system27/userland/obj26 whatever OBJ says: that is $OBJ here
	ln -sfn "$OBJ" "$ROOT/system27/userland/obj26"
	log "env: $DR, $TH"
}

common() { echo "DK=$DK DR=$DR THEOS=$TH OBJ=$OBJ LDID=$TH/toolchain/linux/iphone/bin/ldid"; }

libs() {
	log "libraries (inj26.sh)"
	env $(common) BUILD=${BUILD:-24B119} bash "$ROOT/system27/userland/inj26.sh"
}

gl() {
	log "Mesa for arm64e"
	MESA_ARCH=arm64e DRIVERS=softpipe,panfrost bash "$ROOT/tools/mk-mesa.sh"
	"$ROOT/system27/tools/pacstrip" macho "$ROOT/kernel/deps/build/mesa/out-arm64e/libOSMesa.8.dylib" $OBJ/libOSMesa.8.dylib > /dev/null
	$TH/toolchain/linux/iphone/bin/ldid -S $OBJ/libOSMesa.8.dylib
	log "libhakios-gl.dylib, gl_test (gl26.sh)"
	env $(common) bash "$ROOT/system27/userland/gl26.sh"
}

metal() {
	log "the Metal driver (metal27.sh)"
	env $(common) MESA=$OBJ/libOSMesa.8.dylib bash "$ROOT/system27/userland/metal27.sh"
}

mali() {
	log "hakios-mali and mali-probe (mali27.sh)"
	env $(common) bash "$ROOT/system27/userland/mali27.sh"
}

root() {
	log "the small image (root26-sys.sh)"
	local metal= mali=
	[ -f $OBJ/metal/HakiosMetal ] && metal=$OBJ/metal
	[ -f $OBJ/mali/mali-probe ] && mali=$OBJ/mali
	env $(common) METAL27=$metal MALI27=$mali bash "$ROOT/system27/userland/root26-sys.sh"
}

system() {
	log "the system image (mkimage26.py --prune)"
	env $(common) LAUNCHDAEMONS=$REL/jobs26 WORK=$REL/image-tree-prune \
		python3 "$ROOT/system27/tools/mkimage26.py" --prune $REL/system27-prune.apfs
}

case "$1" in
env) make_env ;;
gl) make_env; gl ;;
libs) make_env; libs ;;
metal) make_env; metal ;;
mali) make_env; mali ;;
root) make_env; root ;;
system) make_env; system ;;
all) make_env; gl; libs; metal; root; system ;;
*) sed -n '2,22p' "$0"; exit 1 ;;
esac
