#!/bin/bash
# The host tests: everything that can be checked on this machine, with no phone and no kernel toolchain.
#   - the tools (tests/tools_test.py): the TOML reader, the board files' checks, the generated tables
#   - the loader compiles for the stand-in phone of tests/devices (syntax and types; nothing is linked),
#     so that a key the loader uses and the generator does not write is found here
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
python3 tests/tools_test.py
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
tools/boards.py --alone --devices tests/devices --soc-dir tests/soc gen --out "$out" > /dev/null
if command -v clang > /dev/null; then
	printf '%s\n' '#include <stdint.h>' '#define TOUCH_FW_SIZE 0u' '#define TOUCH_FW_FLAG 0u' 'static const uint8_t touch_fw[1] = { 0 };' > "$out/touchfw.h"
	echo 'static const unsigned char font8x16[95][16] = { { 0 } };' > "$out/font8x16.h"
	clang --target=aarch64-linux-gnu -ffreestanding -fno-builtin -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra -Werror \
		-Wno-unused-function -fsyntax-only -I"$out/phone-a" -I"$out" loader/mtkloader.c
	echo "tests: the loader compiles for the stand-in phone"
else
	echo "tests: no clang on this machine: the loader was not compiled"
fi
echo "tests: ok"
