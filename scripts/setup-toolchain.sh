#!/usr/bin/env bash
# Installs everything needed to build the .pkg into $HOME/opt.
# Safe to re-run; each step is skipped when the artifact already exists.
set -euo pipefail

OPT="${OPT:-$HOME/opt}"
mkdir -p "$OPT"

say() { printf '\n== %s\n' "$*"; }

# 1. OpenOrbis PS4 toolchain: headers, stubs, create-fself, create-gp4, PkgTool.
TOOLCHAIN="$OPT/OpenOrbis/PS4Toolchain"
if [ ! -x "$TOOLCHAIN/bin/linux/create-gp4" ]; then
  say "downloading the OpenOrbis toolchain"
  curl -L -o "$OPT/toolchain.tar.gz" \
    https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz
  tar -xzf "$OPT/toolchain.tar.gz" -C "$OPT"
else
  say "OpenOrbis toolchain already present"
fi

# 2. LLVM 18. The toolchain's build scripts assume clang/ld.lld of this vintage
#    for the x86_64-pc-freebsd12-elf target.
LLVM="$OPT/clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04"
if [ ! -x "$LLVM/bin/clang" ]; then
  say "downloading LLVM 18"
  curl -L -o "$OPT/llvm.tar.xz" \
    https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04.tar.xz
  tar -xf "$OPT/llvm.tar.xz" -C "$OPT"
else
  say "LLVM 18 already present"
fi

# 3. The prebuilt clang is linked against libtinfo.so.5, which modern distros
#    no longer ship. Extract it from the Ubuntu package instead of installing.
if [ ! -f "$OPT/tinfo5/lib/x86_64-linux-gnu/libtinfo.so.5" ]; then
  say "extracting libtinfo5"
  mkdir -p "$OPT/tinfo5"
  if ! curl -fsL -o "$OPT/libtinfo5.deb" \
      https://archive.ubuntu.com/ubuntu/pool/universe/n/ncurses/libtinfo5_6.3-2_amd64.deb; then
    curl -fsL -o "$OPT/libtinfo5.deb" \
      https://archive.ubuntu.com/ubuntu/pool/main/n/ncurses/libtinfo5_6.1-1ubuntu1.18.04.1_amd64.deb
  fi
  dpkg-deb -x "$OPT/libtinfo5.deb" "$OPT/tinfo5"
else
  say "libtinfo5 already extracted"
fi

# 4. PkgTool.Core is a .NET 6 application: it wants libssl1.1 and, without ICU,
#    DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1.
if [ ! -f "$OPT/ssl11/usr/lib/x86_64-linux-gnu/libssl.so.1.1" ]; then
  say "extracting libssl1.1"
  mkdir -p "$OPT/ssl11"
  curl -fsL -o "$OPT/libssl1.1.deb" \
    https://archive.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2_amd64.deb
  dpkg-deb -x "$OPT/libssl1.1.deb" "$OPT/ssl11"
else
  say "libssl1.1 already extracted"
fi

say "done"
echo "Set OO_PS4_TOOLCHAIN=$TOOLCHAIN (scripts/build.sh does this for you)."
