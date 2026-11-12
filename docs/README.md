# Documentation

| | |
| --- | --- |
| [architecture.md](architecture.md) | How the pieces fit together: the boot chain, device data, the kernel, the loader, what is not here |
| [porting.md](porting.md) | **XNU on another MediaTek phone**: what the phone must allow, the values to read from it, the order of work with the check for each step |
| [building.md](building.md) | **From a clone to the home screen**: every command, what each needs and how long it took |
| [drivers.md](drivers.md) | Every driver: the part it is for, its feature and boot argument, how tied to one phone it is |
| [boot-args.md](boot-args.md) | Every boot argument the kernel reads, with the comment beside its read (generated: `make docs`) |
| [writing-a-driver.md](writing-a-driver.md) | A new driver: the rules, the steps, an example driver with its host test |
| [troubleshooting.md](troubleshooting.md) | What went wrong on the first phone, what it looked like, what fixed it |
| [migrating.md](migrating.md) | For a branch from the old layout: where things went, what the names became |
| [bluetooth-porting.md](bluetooth-porting.md) | Bluetooth: the skeleton driver and the five operations a phone supplies |
| [camera-porting.md](camera-porting.md) | The camera: the skeleton driver and its table of operations |
| [metal.md](metal.md), [air.md](air.md), [air-report.md](air-report.md) | The Metal device; the shader front end for Mesa and its corpus report |
| [../kernel/README.md](../kernel/README.md), [../kernel/patches/README.md](../kernel/patches/README.md) | Working on the kernel; what each patch changes in Apple's files |
| [../devices/README.md](../devices/README.md) | Device packages and board files |

A phone's own story (what it did, run by run, and where the guides' facts were found) is its
package's `journal/` on the `devices` branch.
