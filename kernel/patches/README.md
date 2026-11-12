# What the patches change in Apple's files

Everything this port *adds* is in `kernel/overlay/`. These seven patches are the changes to the 77
files Apple ships, grouped by subsystem. `series` assigns every file to one patch;
`kernel/export.sh` regenerates the patches from an assembled tree, so they are never edited by hand.
The reason for each change is in a comment beside it in the code. This page is the map.

| Patch | Files | What it is for |
| --- | --- | --- |
| `0001-build` | `Makefile`, `SETUP/config/doconf`, `makedefs/`, `config/`, the `conf/files` lists, `OSData.h`, `kalloc.h`, the lldb macros' syntax check | Building with upstream clang on Linux and with the Command Line Tools on macOS instead of Xcode; the configuration (`BCM2837` is the open configuration the port reuses); the files of `kernel/overlay/` added to the lists; the firehose and pthread libraries linked from the build's own SDK; objects made outside the tree can be linked in (`HAKIOS_OBJECTS`) |
| `0002-platform-arm64` | `osfmk/arm/`, `osfmk/arm64/`, `pexpert/`, `osfmk/console/video_console.c`, `bsd/dev/arm/km.c` | A generic arm64 machine: the loader's boot protocol and the return to it instead of powering off, bring-up checkpoints, interrupts through a GICv3, secondary cores through PSCI and which of them are the small ones (`small_cores`), the pmap without PPL, the console copied to the loader's log |
| `0003-iokit` | `iokit/Kernel/`, `iokit/bsddev/IOKitBSDInit.cpp`, `KernelConfigTables.cpp` | IOKit without Apple's platform drivers: the system never sleeps, no IOStorageFamily (the root is a memory device or the eMMC behind `emmc_root=1`, a second RAM image is `/dev/md1`), the platform UUID once the crypto stand-in has registered |
| `0004-mach` | `osfmk/kern/`, `bsd/kern/sys_work_interval.c` | Scheduling with slower cores as secondaries, per-CPU data on one processor, work-interval calls answered, panics and fatal user exceptions written to the loader's log, the AMFI and Sandbox stand-ins registered at start-up |
| `0005-vm` | `bsd/vm/vm_unix.c` | The shared region accepts a cache that lies on a volume mounted inside the Preboot volume's `Cryptexes` directory: the port mounts each cryptex image there as it is |
| `0006-bsd-process` | `bsd/kern/`, `bsd/dev/arm/systemcalls.c`, `bsd/sys/uio_internal.h` | Exec (libraries inserted into programs by name, `kern.exec_inject`), library validation never required and the privileges an entitlement would grant answered (nothing is signed here), memory pressure thresholds counted from the memory there really is, who ends which process and why kept for the log, diagnostics (`systrace=`, `applog=1`) |
| `0007-vfs` | `bsd/vfs/`, `bsd/miscfs/`, `bsd/nfs/nfs_serv.c`, `bsd/dev/memdev.c`, `bsd/sys/disk.h`, `security/mac_vfs_subr.c` | `apfs` and `ramfs` registered, the root on a RAM disk or on the flash, process 1 may bind-mount, and code that was left to rot in the open release made to compile (mockfs, the NFS server, quotas) |

To see exactly what a patch does to a file: `git -C ../xnu-8796-hakios diff upstream -- PATH` in an
assembled tree.
