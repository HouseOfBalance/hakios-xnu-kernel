# Changelog

What changed against branch `x6826b` of the working tree at `6076bc1f` (the same tree as branch `mtk`
of hakios-xnu-kernel at `a9627984`), which this repository was made from on 2026-11-11. Where a path
went: [docs/migrating.md](docs/migrating.md).

## The layout

- The kernel is Apple's pinned release (`kernel/UPSTREAM`) + `kernel/overlay/` + seven patches by
  topic instead of one patch of 77 files; `assemble.sh`, `export.sh`, `verify.sh`, `import-patch.sh`.
  The tree `xnu/apply.sh` made and the tree `kernel/assemble.sh` makes from the first export were
  compared file by file: the same.
- `soc/mt6768.toml` and device packages (`board.toml`) hold what was constants in the loader's
  headers and in the drivers. `tools/boards.py` validates them and generates the kernel's table and
  the loader's header at build time.
- Phones are packages on the `devices` branch; the main branch names none.
- `make check`: lint, board files, host tests with a made-up phone, `components.toml` against the
  fetching scripts, `tools/check-publishable.sh`. CI runs it and `kernel/verify.sh`.

## The kernel

- **The Qualcomm phone's drivers are not here** (23 files; the list is in docs/migrating.md). What the
  remaining files took from them was kept:
  - `hakios_battery.c` is the former battery file without its UEFI part: the `kern.hakios` sysctl
    node, the battery's values, the report a gauge driver makes. The sysctl
    `kern.hakios.battery_from_dsp` keeps its name (user space reads it) and now says "a gauge driver
    has reported".
  - `hakios_wifi_given.c` has the two functions the Wi-Fi station's side and `mtk_wifi.c` used of the
    WCN6750's driver: a line for the log, and the network given in `fw/network`. The log line "which
    network to join" no longer carries a step number.
  - `MiniIOSurfaceDrawn()` moved from the Adreno's client to `HakiosMali.cpp`, with the Mali's half
    of it (the other half waited for a GPU that is not here).
  - `loader_return.c`, `machine_routines.c`, `hakios_input.c`, `hakios_audio.c`, the APFS check and
    mount lost their calls into those drivers. Nothing is saved to a partition on the way back to the
    loader any more: that was the UFS driver's.
- **A MediaTek driver starts only on a board that lists its feature** (`hakios_board_wants()`), after
  its boot argument as before, and takes its blocks' addresses from the table. New: `hakios_board.c`,
  `hakios_board.h`, the sysctls `hw.board.*`, and a line `board: NAME (…): features` early in the log.
- The panel's size in the touch driver, the cell's charge in the gauge driver, CONNSYS's reserved
  memory, LK's framebuffer in the display's flip and the system partition's name in the eMMC driver
  are the board file's.
- The link was compared with `6076bc1f`'s by symbols: what is gone is what the removed objects
  defined.

## The loader

- Built for a named board from `hakios_platform.h`; `link.ld` takes the load address from the board
  file. The instructions of `mtkloader.o` are the same as `6076bc1f`'s, with default flags and with a
  run's (`objdump -d`, no difference); the binary differs by its banner, which now names the board
  and the chip from the data.
- The touch upload, the CONNSYS report and the big cluster's clock are compiled in only for a board
  whose file asks for them.
- `touchfw.h` (the vendor's firmware as C) and `font8x16.h` (a rendering of a system font) are no
  longer files of the repository: `loader/build.sh` makes them from the builder's own files.
- The payload is the kernel and the small root; the UEFI loader's binary is no longer packed into it.

## Names

- The first phone's name went out of code, paths and interfaces: `hakios_*` and `Hakios*` for what is
  common, `mtk_*` for MediaTek's blocks. Kernel and user space changed together; the tables are in
  docs/migrating.md.
- The system image's trade name went out of file names, messages and comments; what is left are the
  literal names of inputs (`iPhoneOS16.5.sdk`, the build `20F66`, `iPhone10,3`), of what the system
  itself looks for, and the notices. `tools/check-publishable.sh` holds the list.

## Verified, and not

See the commit messages: each says what was built, what was checked on the build machine and which
run on the phone showed what.

## Tests of the drivers (2026-11-12)

- `tests/fakekernel/`: enough of XNU's interfaces for a driver's own `.c` to run as a host program
  (pthreads for threads, the host's clock, a table for sysctls), on macOS as well as Linux.
- `tests/run.sh` runs `input_queue`, `station`, `apple80211`, `sparse_driver`, `guard` and
  `audio_device` against it, one scenario a process.
- Three drivers got the bounds checks those tests ask for: `hakios_sta.c` holds the radio's
  channel count to the list's room; `hakios_apple80211.c` holds a network's name and information
  elements to their record; `hakios_sparse.c` checks the header's sizes without a sum that can wrap,
  and refuses a table entry past the image's blocks. Host tests only: not yet run on a phone.
- `tests/pwrap_model.h`: the PMIC wrapper's WACS2 channel and the MT6358's registers as
  `mtk_keys.c` and `mtk_battery.c` drive them, through a `HAKIOS_MMIO_TEST` seam in each (the
  kernel's build is unchanged). `battery_driver_test` (Android's percentage carried by the counter,
  the voltage alone, the two too far apart, charging, no conversion, a wrapper that never answers)
  and `keys_driver_test` (volume and power keys, a bounce, a silent PMIC).
- `kernel/syntax-check.sh` (`make syntax-check`): every overlay file through the host's clang.
