#!/usr/bin/env bash
# Fetches the ps4-linux-loader payload that gets bundled into the .pkg.
# The payload is not vendored in this repo (upstream has no license file), so
# it is pulled from the official release at build time.
set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="v25.1"
URL="https://github.com/ps4-linux/ps4-linux-loader/releases/download/${VERSION}/ps4-linux-loader-${VERSION}.zip"

if [ -f ps4-linux-loader.elf ] && [ -f ps4-linux-loader.bin ]; then
  exit 0
fi

command -v unzip >/dev/null 2>&1 || {
  echo "unzip is required to unpack the payload release" >&2
  exit 1
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

curl -fsL -o "$tmp/loader.zip" "$URL"
unzip -q "$tmp/loader.zip" -d "$tmp"

# 1024 MB variant: vram.txt overrides it at runtime, and 1 GB is what the
# guide requires for the first installation.
cp "$tmp/ps4-linux-loader-${VERSION}/elf/linux-1024mb.elf" ps4-linux-loader.elf
cp "$tmp/ps4-linux-loader-${VERSION}/bin/linux-1024mb.bin" ps4-linux-loader.bin

echo "fetched ps4-linux-loader ${VERSION}"
