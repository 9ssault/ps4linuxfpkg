#!/usr/bin/env bash
# Builds assets/initramfs.cpio.gz: the stock initramfs-ps4 plus the unattended
# installer. The result is bundled in the .fpkg so no download is needed.
set -euo pipefail

cd "$(dirname "$0")/.."

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/GayCorp/initramfs-ps4/releases/download/latest/initramfs.cpio.gz}"
CACHE=".cache/upstream-initramfs.cpio.gz"
OUT="assets/initramfs.cpio.gz"

mkdir -p .cache assets

if [ ! -f "$CACHE" ]; then
  echo "downloading upstream initramfs"
  curl -fsL -o "$CACHE" "$UPSTREAM_URL"
fi

python3 scripts/initramfs_tool.py patch "$CACHE" "$OUT" \
  initramfs/ps4li-auto.sh initramfs/hook.sh

# Prove the patched image still contains what we expect.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
python3 scripts/initramfs_tool.py extract "$OUT" "$tmp" >/dev/null

grep -q 'run_hooks autoinstall' "$tmp/init" || {
  echo "verification failed: init has no autoinstall hook" >&2
  exit 1
}
test -s "$tmp/bin/ps4li-auto.sh" || {
  echo "verification failed: auto-install script missing" >&2
  exit 1
}
sh -n initramfs/ps4li-auto.sh

echo "built $OUT ($(stat -c %s "$OUT") bytes)"
