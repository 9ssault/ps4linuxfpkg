#!/usr/bin/env bash
# Builds out/ps4-linux-installer.pkg. Run scripts/setup-toolchain.sh first.
set -euo pipefail

OPT="${OPT:-$HOME/opt}"

export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$OPT/OpenOrbis/PS4Toolchain}"
export PATH="$OPT/clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04/bin:$PATH"
export LD_LIBRARY_PATH="$OPT/tinfo5/lib/x86_64-linux-gnu:$OPT/ssl11/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

if [ ! -x "$OO_PS4_TOOLCHAIN/bin/linux/create-gp4" ]; then
  echo "toolchain missing; run scripts/setup-toolchain.sh first" >&2
  exit 1
fi

# The boot payload and the patched initramfs are built once and cached.
"$(dirname "$0")/fetch-payload.sh"

if [ ! -f assets/initramfs.cpio.gz ]; then
  "$(dirname "$0")/build-initramfs.sh"
fi

make "$@"
