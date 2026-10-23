#!/usr/bin/env bash
# Verifies a built .fpkg: extracts it again with PkgTool and checks every file
# inside against the source tree, plus the param.sfo fields and the icon.
#
#   scripts/verify-package.sh out/ps4-linux-installer.pkg
set -euo pipefail

PKG="${1:-out/ps4-linux-installer.pkg}"

cd "$(dirname "$0")/.."

OPT="${OPT:-$HOME/opt}"
export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$OPT/OpenOrbis/PS4Toolchain}"
export LD_LIBRARY_PATH="$OPT/tinfo5/lib/x86_64-linux-gnu:$OPT/ssl11/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

TOOL="$OO_PS4_TOOLCHAIN/bin/linux/PkgTool.Core"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0

check() {
  if [ "$2" = "$3" ]; then
    printf 'ok   %s\n' "$1"
  else
    printf 'FAIL %s: expected %s, got %s\n' "$1" "$3" "$2" >&2
    fail=1
  fi
}

test -f "$PKG" || { echo "missing $PKG" >&2; exit 1; }

"$TOOL" pkg_extract "$PKG" "$TMP/extract" >/dev/null

# Every file the package is supposed to carry must be byte-identical.
FILES="eboot.bin ps4-linux-loader.elf ps4-linux-loader.bin
       assets/fonts/Inter-Regular.ttf assets/fonts/Inter-SemiBold.ttf
       assets/initramfs.cpio.gz sce_sys/about/right.sprx
       sce_module/libc.prx sce_module/libSceFios2.prx"

for f in $FILES; do
  if cmp -s "$f" "$TMP/extract/uroot/$f"; then
    echo "ok   $f"
  else
    echo "FAIL $f differs from the source tree" >&2
    fail=1
  fi
done

# param.sfo is a PKG entry, not part of the inner PFS.
"$TOOL" pkg_extractentry "$PKG" 9 "$TMP/param.sfo" >/dev/null
grep -q 'PS4 Linux Installer' "$TMP/param.sfo" || { echo "FAIL sfo title" >&2; fail=1; }
grep -q 'PLNX00001' "$TMP/param.sfo" || { echo "FAIL sfo title id" >&2; fail=1; }
grep -q 'IV0000-PLNX00001_00-PS4LINUXINSTALL0' "$TMP/param.sfo" || { echo "FAIL sfo content id" >&2; fail=1; }

ICON_SIZE="$(stat -c %s sce_sys/icon0.png)"
ICON_ENTRY="$("$TOOL" pkg_listentries "$PKG" | awk '$NF == "ICON0_PNG" { print $2 }')"
check "icon entry size (0x$(printf '%X' "$ICON_SIZE"))" "$ICON_ENTRY" "0x$(printf '%X' "$ICON_SIZE")"

# The bundled initramfs must still be the patched one.
python3 scripts/initramfs_tool.py extract "$TMP/extract/uroot/assets/initramfs.cpio.gz" "$TMP/ir" >/dev/null
check "init hook" "$(grep -c 'run_hooks autoinstall' "$TMP/ir/init")" "1"
test -s "$TMP/ir/bin/ps4li-auto.sh" || { echo "FAIL initramfs lacks the auto installer" >&2; fail=1; }
echo "ok   initramfs contents"

# The executable must be a fake-signed PS4 SELF.
MAGIC="$(od -An -tx1 -N4 "$TMP/extract/uroot/eboot.bin" | tr -d ' \n')"
check "eboot SELF magic" "$MAGIC" "4f153d1d"

if [ "$fail" -eq 0 ]; then
  echo "PACKAGE OK: $PKG"
else
  echo "PACKAGE FAILED" >&2
fi

exit "$fail"
