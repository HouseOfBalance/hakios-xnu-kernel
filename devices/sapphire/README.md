# sapphire: Xiaomi Redmi Note 13 4G

| | |
| --- | --- |
| SoC | Qualcomm Snapdragon 685 (`soc/sm6225.toml`) |
| Panel | 1080 x 2400 AMOLED, 120 Hz, 4320-byte pitch (`cont_splash_region` at `0x5c000000`) |
| Storage | UFS 2.2 (`/soc/ufshc@1d84000`); system partition `xnusys` |
| Verified | Board and SoC configuration validated with `tools/boards.py check` |

## Hardware Overview

- **CPU**: Octa-core Kryo 265 — 4x Kryo 265 Silver (Cortex-A53 @ 1.9 GHz, cores 0–3) + 4x Kryo 265 Gold (Cortex-A73 @ 2.8 GHz, cores 4–7), `core_shift = 8`, PSCI `smc`.
- **Interrupt Controller**: ARM GICv3 at `0x17a00000` (GICD) and `0x17a60000` (GICR).
- **Debug UART**: Qualcomm QUPv3 GENI serial engine at `0x04a90000` (`/soc/serial@4a90000`).
- **GPIO / Pin Controller**: SM6225 TLMM at `0x0f100000` (`/soc/pinctrl@f100000`).
- **Display**: MDSS / DPU at `0x05e00000` with ABL continuous splash framebuffer at `0x5c000000` (`1080 x 2400`, pitch `4320`, size `0x017bb000`).
- **GPU**: Adreno 610 (`0x05900000`).
- **Battery**: 5000 mAh (`5000000` uAh).

## Memory Layout

| Region | Base Address | Size | Description |
| --- | --- | --- | --- |
| `kernel_load` | `0x40080000` | — | ABL arm64 kernel entry point above DRAM base (`0x40000000`) |
| `chain_stub` | `0x4a000000` | `0x1000` | Kernel handoff trampoline page |
| `scratch` | `0x50000000` | `0x200000` | Temporary buffer for GPT and disk reads |
| `fb_base` | `0x5c000000` | `0x017bb000` | ABL continuous splash framebuffer (`cont_splash_region`) |
| `marker` | `0x80000000` | `0x1000` | Boot stage marker page |
| `panic_log` | `0x80100000` | `0x80000` | Persistent panic log region (512 KB) |
| `test_buffer` | `0xa0000000` | `0x400000` | Diagnostic test buffer (4 MB) |
| `ramoops.console` | `0xa1000000` | `0x40000` | Ramoops console output zone (256 KB) |
| `fbdiag.marker` | `0xa1040000` | `0x1000` | Framebuffer diagnostic marker page |
| `ramoops.pmsg` | `0xa1080000` | `0x20000` | Ramoops user message zone (128 KB) |
| `xnu_base` | `0xc0000000` | `0xfe000000` | Main system memory carveout in upper DRAM |

## What works

- Board and SoC definitions (`devices/sapphire/board.toml`, `soc/sm6225.toml`) pass `tools/boards.py check`.
- `/soc`-nested Qualcomm device-tree nodes (`interrupt-controller`, `serial`, `pinctrl`/`tlmm`) and `cont_splash` reserved-memory regions are parsed by `tools/devicetree.py`.
- Early framebuffer console output over the ABL continuous splash region (`0x5c000000`) and ramoops logging (`0xa1000000`).

## What does not

- MediaTek-specific peripheral blocks (MSDC eMMC, MT6358 PMIC keypad/battery, Mali-G52 GPU, MT6631 Connectivity) are not listed in `features`; Qualcomm-specific drivers for UFS (`ufshc`), SPMI PMIC, and Adreno 610 are required for full peripheral bring-up.

## Where the values are from

- Hardware register addresses are taken from the Qualcomm SM6225 (`khaje`) flattened device tree (`/soc/interrupt-controller@17a00000`, `/soc/serial@4a90000`, `/soc/pinctrl@f100000`, `/soc/ufshc@1d84000`, `/soc/qcom,mdss_mdp@5e00000`, `/soc/qcom,kgsl-3d0@5900000`, `/soc/qcom,spmi@c440000`).
- Framebuffer and ramoops ranges follow the Xiaomi Redmi Note 13 4G (`sapphire`) Android bootloader (ABL) `/reserved-memory` layout.

## Verification

Validate the board and SoC definitions from the repository root:

```sh
python3 tools/boards.py check
python3 tools/boards.py status sapphire
python3 tools/boards.py show sapphire
```

## Yours to bring

- An unlocked Xiaomi Redmi Note 13 4G (`sapphire`) boot image (`boot.img`) to repack with the loader, and a `xnusys` GPT partition on UFS containing the system image.
