# From a clone to the home screen

Every command between `git clone` and the system's home screen on a phone whose package is on the `devices` branch (the first one: `x6826b`), in order, with what each one needs and how long it took.
It was done exactly so on 10 November 2026: a new clone of this branch in an empty directory, on an Intel Mac with macOS 14.0, Apple clang 15.0.0,
Command Line Tools 15.3, no Homebrew. Whatever stopped that clone is fixed in the scripts or said here. On Linux the steps are the same and the
packages are those of [kernel/README.md](../kernel/README.md); that side was not run again for this page.

For a phone that is not the first phone, read [porting.md](porting.md) first: this page is the last step of that one.

## What you need before the first command

| | |
| --- | --- |
| The phone | bootloader unlocked, Android rooted (Magisk), on slot `a`, USB debugging allowed for this computer |
| A partition `xnusys` | 4 GB, for the system image. The first phone's was cut out of `userdata` once (`tools/gpt-carve.py`, step 8 of porting.md). Without it there is no root to boot |
| `backup/boot_a_magisk.img` | **your own** rooted boot image, the one that is in `boot_a` when Android boots. The loader is packed into it. Never in the repository |
| `adb`, `fastboot` in `PATH` | Android's platform-tools |
| A compiler | macOS: the Command Line Tools. Linux: see kernel/README.md |
| The system image 20F66 SDK | `~/theos/sdks/iPhoneOS16.5.sdk` (or `$THEOS/sdks`), from github.com/theos/sdks; the command is in kernel/README.md |
| 7-Zip 23 or newer | as `7z` or `7zz` in `PATH`. It reads APFS disk images. 7-zip.org's own macOS build is `7zz`; of its two archives `-mac` is for Apple silicon and `-mac-x64` for Intel |
| `python3` | 3.9 was enough; no modules beyond the standard ones |
| Disk | 20 GB: 1 GB kernel sources and build, 8 GB while the system is fetched (5.6 GB after), 2.3 GB each for the image and the tree it is made of |
| Network | 7.4 GB from Apple's CDN (the two disk images of the reference phone's the system image 20F66), 47 MB of zig, the sources `kernel/assemble.sh ../xnu-8796-hakios` and `kernel/deps/setup.sh` clone |

The path of the clone must have no spaces in it.

## The commands

```sh
export HAKIOS_SERIAL=<your phone's adb serial>     # only ever in the environment, never in a file

tools/device.py add x6826b   #        the phone's package: what the kernel's table and the loader are built from
kernel/assemble.sh ../xnu-8796-hakios   #  8 s   Apple's xnu-8796.121.2 + kernel/overlay + kernel/patches, next to the clone
kernel/deps/setup.sh            #  1 min what the kernel's build needs: sources, host tools, the SDK's overlay, two libraries
kernel/build.sh -j8 -k        #  3 min the kernel. -k is needed: one harmless error (lldbmacros) ends make with code 2
tools/get-zig.sh             #  1 min zig 0.13.0 into build/zig: its ld.lld links the loader (not needed where ld.lld is installed)
system/fetch.sh              # 30 min at 4 MB/s: the files of the system image (20F66) out of Apple's IPSW; a broken download is taken up again
./userland/build.sh          #  4 min our programs and libraries, userland/root.apfs (4 MB) and userland/system.apfs (2.3 GB)
tools/mkpayload.sh      # 1 s: build/payload.bin = the kernel + the small root
mkdir -p backup && cp <your rooted boot image> backup/boot_a_magisk.img
tools/system-flash.sh        #  3 min system.apfs into xnusys (Android must be up; 2.3 GB free on /data for the way there)
XNU_LOG_OUT=logs/run1.log tools/xnu-cycle.sh      # the board file's [boot]: 120 s, cpus=4 touchscreen=2 mali=1 fbscale=2 fbfit=3    # 5 min
```

The last command builds the loader, packs it with Android's kernel and the payload into `build/boot_a.img` (30 MB; `boot_a` holds 32),
flashes `boot_a`, lets Android come up, asks it for one XNU boot, and reads the log when Android is back. The phone shows the home screen about
100 seconds after the kernel starts and goes back to Android by itself `XNU_SECS` after. Only `boot_a` and `xnusys` are written; slot `b` never.

## What a good run looks like

In `logs/run1.log`, in this order:

- `boot-args: debug=0x8 serial=3 rd=md0 emmc_root=1 cpus=4 touchscreen=2 mali=1 ...`
- `touch: TD4160-... is up` (the loader gave the touch controller its firmware)
- `Darwin Kernel Version 22.5.0: <the time you built it>`
- `mali: came up to step 6 of 6`
- `load_init_program: attempting to load /sbin/launchd`, then `=== 36 checks, 0 failed ===` from our init
- every five seconds, once SpringBoard draws: `MiniIOMobileFramebuffer: 51 frames in 5012 ms (10 a second)` and
  `this frame on average red 118 green 151 blue 148; 100% of it not black`
- `=== end of XNU log ===`, `starting Android`

Ten frames a second is the CPU drawing: this build has no Mesa in it. The GPU path is `GL=1` (and `METAL=1`) of `userland/build.sh`, which needs
`tools/mk-mesa.sh` and for that meson and ninja; it was not part of this test.

To see the screen without looking at the phone, add `fbshot=20` to `XNU_ARGS` and give the run ten seconds more (`XNU_SECS=130`): the kernel
prints a small picture of the frame into the log and `python3 tools/fbshot.py logs/run1.log shot.png` makes a PNG of it. `fbshot=N` counts the
five-second frame reports, N/5 of them, and those start only when SpringBoard draws (about 85 s in): `fbshot=20` is a picture at about 125 s.
The log keeps its last 256 KB, so a picture wider than the default 60 pixels, or one taken long before the end, falls out of it.

## What is not in a run made this way

- No Wi-Fi, Bluetooth, sensors, camera or sound: their firmware is your phone's own and is not fetched (`build.sh: fw/: 0 files ... absent`),
  and each wants its boot-args and jobs; see that subject's journal in [README.md](README.md).
- The five test programs that want an entitlement are unsigned without `ldid` (`build.sh: no ldid: ...`). `LDID=$PWD/tools/ldid-codesign.sh ./userland/build.sh` signs
  them with macOS's `codesign`.
- `lsd` fills the log with `denied attempt to escape CPU monitor`, twenty lines a second. It is harmless and it is why the log's beginning is lost.

## If it does not come back

A kernel that hangs costs a power cycle: the image boots Android unless asked for XNU, once. If `boot_a` itself is broken, the way back is the boot
ROM's download mode (mtkclient, `mtk.py w boot_a backup/boot_a_magisk.img`); check that you can reach it before the first flash.
