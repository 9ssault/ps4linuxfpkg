#!/bin/sh
# Checks that the job file written by the app is parsed the way the patched
# initramfs expects. Runs on any POSIX shell, no PS4 needed.
set -eu

cd "$(dirname "$0")/.."

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

fail=0

check() {
	if [ "$2" = "$3" ]; then
		printf 'ok   %s\n' "$1"
	else
		printf 'FAIL %s: expected %s, got %s\n' "$1" "$3" "$2" >&2
		fail=1
	fi
}

# Same format string as write_job() in src/main.c.
printf 'mode=%s\nfs=%s\nsize_gb=%d\njob=%08x\n' ext btrfs 32 3735928559 > "$tmp"

# Use the parser from the initramfs script itself, not a copy of it.
parser="$(awk '/^read_job\(\)/,/^}$/' initramfs/ps4li-auto.sh)"
eval "$parser"

conf="$tmp"
mode=''
fs=''
size_gb=''
job=''
read_job

check "mode" "$mode" "ext"
check "fs" "$fs" "btrfs"
check "size_gb" "$size_gb" "32"
check "job" "$job" "deadbeef"

printf 'mode=%s\nfs=%s\nsize_gb=%d\njob=%08x\n' internal ext4 0 1 > "$tmp"
mode=''; fs=''; size_gb=''; job=''
read_job

check "internal mode" "$mode" "internal"
check "ext4" "$fs" "ext4"
check "auto size" "$size_gb" "0"

# The init script must call the hook exactly once.
calls="$(grep -c 'run_hooks autoinstall' initramfs/ps4li-auto.sh 2>/dev/null || true)"
check "no stray hook call in auto script" "$calls" "0"

if [ -f assets/initramfs.cpio.gz ]; then
	python3 scripts/initramfs_tool.py list assets/initramfs.cpio.gz >/dev/null
	check "initramfs parses" "$?" "0"
fi

[ "$fail" -eq 0 ] && echo "all checks passed"
exit "$fail"
