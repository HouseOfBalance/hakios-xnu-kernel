# Contributing

## Before you send anything

```sh
make check
```

It must pass. It runs the lint, validates the board and SoC files and runs the host tests. CI runs the
same, plus `kernel/verify.sh`.

## Where a change goes

| The change | Goes in | Then |
| --- | --- | --- |
| A new phone, or a value of an existing one | Its device package on the `devices` branch: `NAME/board.toml`, with its README and journal. Not `devices/NAME` on the main branch, which git ignores there. | `tools/boards.py check`, with the package in `devices/` of a checkout of the main branch |
| A new SoC, or a value of an existing one | `soc/SOC.toml` on the main branch | `tools/boards.py check` |
| A driver, or anything else added to the kernel | `kernel/overlay/` | `kernel/build.sh` |
| A change to one of Apple's files | An assembled tree, then `kernel/export.sh` | Never edit `kernel/patches/*.patch` by hand: `kernel/verify.sh` rejects it. |
| The loader | `loader/` | `loader/build.sh`; addresses come from the board files, not from code. |
| A library inserted into a process, a test program | `userland/src/` | `userland/build.sh` |

[docs/architecture.md](docs/architecture.md) explains the split; [kernel/README.md](kernel/README.md)
has the two ways of working on the kernel.

## Rules that keep the project portable

- **No device knowledge in device-independent code.** An address, a pin or a range that differs
  between phones belongs in the board or SoC file, and reaches the code through `hakios_board()` in
  the kernel and `hakios_platform.h` in the loader.
- **A driver checks its feature.** After its boot argument, before it touches anything:
  `if (!hakios_board_wants(HAKIOS_HAS_…, "name"))`, which says which line is missing.
- **Off by default, read before write.** A new driver does nothing without its boot argument, and its
  first level only reads and logs.
- **The loader says what it is about to do before anything that can hang**, so the last line of a
  log is the step that hung. It runs with the MMU off: no SIMD, no unaligned access, no builtins.
- **Say what was verified.** "Compiles", "passes the host tests" and "seen working on the phone" are
  three different statements. Commit messages and documents say which one applies, with the run.
- **Names.** `hakios_*` for what is the same on any phone, `mtk_*` for MediaTek's blocks, a chip's
  name only where the code is that chip's. No phone's name in a file's name.

## What must never be committed

`tools/check-publishable.sh` (part of `make check`) looks for these, but the rule is yours to keep:

- Apple's files: anything from a system image, SDK headers, shaders, the kernel itself.
- Firmware and binaries of the phone's maker: touch, CONNSYS, sensor hub firmware, device trees, boot
  images, the camera's register tables made from a capture.
- Pictures of a screen, built output, home-directory paths, a phone's serial number, keys.

A value read from a vendor's device tree or driver (an address, a range) is a fact and is fine, with
its source named in a comment.

## Commits

- One change per commit, with a body that says what changed and why.
- Name the paths. Say how it was verified, and what was not: a change that was run on a phone says
  which run and what the log showed.
- `kernel/export.sh` before committing if you worked in a tree. The board tables are not committed.

## Style

C in the kernel follows the files around it: tabs, the comment above the code explains *why*, a
driver starts with a comment that says what the hardware is and where each number comes from. Shell
scripts run under bash 3.2 (macOS still has it). Python needs nothing outside the standard library
and runs on 3.9.
