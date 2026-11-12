# SoC files

One file per chip: where its blocks are. A SoC file is shared by every board with that chip, so it
lives here, with the code, and not in a phone's package.

| File | Chip | Used by |
| --- | --- | --- |
| `mt6768.toml` | MediaTek Helio G85 (MT6768; the G80 and G70 are the same chip) | the `x6826b` package |

## What a SoC file says

| Section | Contents | Needed for |
| --- | --- | --- |
| `[platform]` | The interrupt controller, the cores and how they are numbered, which are the big ones, the firmware's power interface, the UART, one window over the peripherals | Every board: the loader builds the kernel's device tree from it |
| `[blocks]` | The address of every peripheral block a driver maps | The loader and every driver: none carries an address of its own |
| `[cpufreq]` | The big cluster's PLL and the speed it can take at the voltage LK leaves | A board's `big_cores_clock = true` in `[loader]` |

Every key is explained in the file itself and in `devices/_template/soc.toml`.

## Where the values come from

The Android device tree of a phone with the chip, read on the phone as root
(`/sys/firmware/fdt`) and searched with `tools/fdtfind.py FILE WORD`: each key names the node it is
from. What is in no tree is said so in the key's comment.

`tools/boards.py check` validates every SoC file with the boards that use it, and
`tools/boards.py status NAME` says which block a feature of a board still waits for.

A SoC file goes to this branch; a phone's package goes to the `devices` branch.
