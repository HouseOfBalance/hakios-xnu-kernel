# Tools

| | |
| --- | --- |
| `boards.py`, `device.py`, `minitoml.py` | Board and SoC files: check, status, the generated tables; device packages; the TOML reader for a Python without one |
| `components.py`, `lint.sh`, `check-publishable.sh` | `components.toml` against the fetching scripts; the checks of form; what must never be published |
| `xnu-cycle.sh`, `boot-xnu.sh`, `system-flash.sh`, `mkpayload.sh` | One run on the phone: the payload, the loader built with the run's flags, `boot_a` written, one XNU boot asked for, the log read back; the system image into its partition |
| `msdc-test.sh`, `touch-probe.sh`, `gpu-probe.sh`, `cam-probe.sh` | The loader's probes, each a flash, one boot and a report (docs/porting.md) |
| `fbshot.py`, `fbjpg.py`, `fbrecv.py`, `recording.py`, `sound.py`, `frames.py`, `latency.py` | What a run left in its log or in memory, made into a picture, a video, a sound file, numbers |
| `fdtfind.py`, `gpt-carve.py`, `userdata-carve.py`, `android/` | Reading a phone: its device tree, its partition table (new tables are written to files only), small tools run under Android |
| `mktouchfw.py`, `mkfont8x16.swift`, `camtable.py`, `camrx.py`, `camraw.py`, `camthumb.py` | The loader's and the camera's local inputs, made from your own phone's files |
| `get-zig.sh`, `mk-mesa.sh`, `mk-libdeflate.sh`, `mk-jb.sh`, `ldid-codesign.sh` | Fetching and building what `components.toml` pins |
| `apfs/`, `dsc/`, `air/`, `mesa/` | The APFS image maker and its tests; a reader of the shared cache; the shader front end's checks; the patches and sources for Mesa |

Each script says at its head what it does and what it needs. The ones that touch a phone take its
serial number from `HAKIOS_SERIAL`, never from a file, and stop when the phone is not on slot `a`.
