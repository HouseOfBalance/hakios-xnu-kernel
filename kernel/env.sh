# shellcheck shell=bash
# source this (from bash) before building the kernel by hand; kernel/build.sh does it itself.
#
# What is where (each can be set in the environment before sourcing):
#   XNU_SRC     the assembled kernel tree (kernel/assemble.sh makes it), next to the repository
#   THEOS       where sdks/iPhoneOS16.5.sdk is (default ~/theos); on Linux its cctools as well
# XNUDEPS, what the build needs beside the tree, is kernel/deps: its own files are the repository's,
# what kernel/deps/setup.sh fetches and builds there is ignored by git.
HAKIOS_KERNEL=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HAKIOS=$(cd "$HAKIOS_KERNEL/.." && pwd)
export HAKIOS_KERNEL HAKIOS
# shellcheck source=UPSTREAM
source "$HAKIOS_KERNEL/UPSTREAM"
export THEOS=${THEOS:-$HOME/theos}
# (named after the release: xnu-8796.121.2 -> xnu-8796-hakios)
export XNU_SRC=${XNU_SRC:-$(cd "$HAKIOS/.." && pwd)/${UPSTREAM_TAG%%.*}-hakios}
export XNUDEPS=$HAKIOS_KERNEL/deps
export PATH=$XNUDEPS/host/bin:$PATH
export XNU_SDK_PATH=$XNUDEPS/sdk/iPhoneOS16.5.xnu.sdk
export XNU_SDK_VERSION=16.5
export XNU_PLATFORM_PATH=$XNUDEPS/sdk/Platforms/iPhoneOS.platform
