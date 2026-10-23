#!/bin/sh
# ps4-linux-installer: unattended install.
#
# Runs from the 'autoinstall' hook patched into init. It only acts when a
# ps4li.conf job file sits next to the kernel, so a normal boot is untouched.
# The logic mirrors the stock install-linux-ext.sh / install-linux-int.sh
# scripts, minus every prompt.

. /functions.sh

src=/temp
conf=''
source=''
mode='ext'
fs='ext4'
size_gb=0
job=''
target=''

say() { einfo "ps4li: $*"; }
warn() { ewarn "ps4li: $*"; }
fail() { eerror "ps4li: $*"; }

# ---------------------------------------------------------------- job lookup

read_job() {
	while IFS='=' read -r key value; do
		case "${key}" in
			mode)    mode="${value}" ;;
			fs)      fs="${value}" ;;
			size_gb) size_gb="${value}" ;;
			job)     job="${value}" ;;
		esac
	done < "${conf}"
}

# Marker written to the freshly formatted boot partition, so a job that could
# not be deleted from a read-only source does not reinstall on every boot.
job_already_done() {
	[ -n "${job}" ] || return 1

	for d in /dev/sd[a-z]; do
		[ -b "${d}1" ] || continue
		mount -o ro "${d}1" "${src}" 2>/dev/null || continue
		if [ -f "${src}/ps4li.done" ] && grep -q "^job=${job}$" "${src}/ps4li.done" 2>/dev/null; then
			umount "${src}"
			return 0
		fi
		umount "${src}"
	done

	return 1
}

# Removes the job file so it cannot run twice. Returns non-zero when the
# source is read-only; the caller then relies on the ps4li.done marker.
consume_job() {
	mount -o remount,rw "${src}" 2>/dev/null
	if rm -f "${conf}" 2>/dev/null && [ ! -f "${conf}" ]; then
		say "job consumed"
		return 0
	fi
	warn "source is read-only, relying on ps4li.done"
	return 1
}

find_job() {
	mkdir -p "${src}"
	mountpoint -q "${src}" && umount "${src}" 2>/dev/null

	internal=''
	part="$(ps4hdd_part 2>/dev/null)" && internal="${part%27}"

	for d in /dev/sd[a-z]; do
		[ -b "${d}" ] || continue
		[ -n "${internal}" ] && [ "${d}" = "${internal}" ] && continue
		mount -o ro "${d}1" "${src}" 2>/dev/null || continue
		if [ -f "${src}/ps4li.conf" ]; then
			conf="${src}/ps4li.conf"
			source="${d}"
			return 0
		fi
		umount "${src}"
	done

	ps4hdd_open >/dev/null 2>&1 || true
	if [ -f /ps4hdd/linux/boot/ps4li.conf ]; then
		conf=/ps4hdd/linux/boot/ps4li.conf
		source=internal
		return 0
	fi

	return 1
}

# ------------------------------------------------------------------- external

disk_count() {
	local n=0 d
	for d in ${cands}; do n=$((n + 1)); done
	echo "${n}"
}

install_external() {
	local distro cands internal part d name lbs total fat_first fat_last
	local ext_first ext_last n

	if ! distro="$(ps4_find_distro "${src}")"; then
		fail "no distro archive in ${src}"
		return 1
	fi

	mountpoint -q /newroot && umount /newroot 2>/dev/null

	internal=''
	part="$(ps4hdd_part 2>/dev/null)" && internal="${part%27}"

	cands=''
	for d in /dev/sd[a-z]; do
		[ -b "${d}" ] || continue
		[ "${d}" = "${source}" ] && continue
		[ -n "${internal}" ] && [ "${d}" = "${internal}" ] && continue
		cands="${cands} ${d}"
	done

	# Wiping the wrong drive is unrecoverable, so refuse to guess.
	if [ "$(disk_count)" -ne 1 ]; then
		fail "need exactly one empty USB drive as the install target, found $(disk_count)."
		warn "Connect the source drive and one empty target drive, then reboot."
		return 1
	fi

	set -- ${cands}
	target="$1"
	say "source ${source}, target ${target}"

	for p in "${target}"[0-9]*; do
		[ -b "${p}" ] && grep -q "^${p} " /proc/mounts && umount "${p}"
	done

	name="${target#/dev/}"
	lbs="$(cat "/sys/block/${name}/queue/logical_block_size")"
	total=$(($(cat "/sys/block/${name}/size") * 512 / lbs))
	[ "${total}" -gt 4294967295 ] && total=4294967295

	fat_first=2048
	fat_last=$((fat_first + 52428800 / lbs - 1))
	ext_first=$((fat_last + 1))
	ext_last=$((total - 1))

	say "partitioning ${target}"
	printf 'o\nn\np\n1\n%s\n%s\nn\np\n2\n%s\n%s\nt\n1\nc\nw\n' \
		"${fat_first}" "${fat_last}" "${ext_first}" "${ext_last}" | fdisk -u "${target}" >/dev/null
	blockdev --rereadpt "${target}" 2>/dev/null

	n=0
	while ! [ -b "${target}1" ] || ! [ -b "${target}2" ]; do
		n=$((n + 1))
		if [ "${n}" -gt 10 ]; then
			fail "partitions on ${target} did not show up"
			return 1
		fi
		sleep 1
	done

	say "formatting the boot partition"
	if ! mkfs.vfat "${target}1" >/dev/null || ! mount "${target}1" /newroot; then
		fail "could not format or mount ${target}1"
		return 1
	fi

	cp "${src}/bzImage" "${src}/initramfs.cpio.gz" /newroot/
	[ -f "${src}/bootargs.txt" ] && cp "${src}/bootargs.txt" /newroot/
	[ -f "${src}/vram.txt" ] && cp "${src}/vram.txt" /newroot/
	if [ -n "${job}" ]; then
		printf 'job=%s\n' "${job}" > /newroot/ps4li.done
	fi
	umount /newroot

	say "formatting the Linux partition"
	if ! mke2fs-new -q -t ext4 -F -L psxitarch -O ^has_journal "${target}2" || ! mount "${target}2" /newroot; then
		fail "could not format or mount ${target}2"
		return 1
	fi

	say "extracting $(basename "${distro}"), this takes a while"
	if ! ps4_extract "${distro}"; then
		fail "distro extraction failed"
		return 1
	fi

	ps4_prepare_newroot
	sync
	umount /newroot
	return 0
}

# ------------------------------------------------------------------- internal

install_internal() {
	local boot img distro free_gb size

	boot=/ps4hdd/system/boot
	img=/ps4hdd/app/linux.img

	if ! ps4hdd_open; then
		fail "could not open the PS4 internal drive"
		return 1
	fi

	mkdir -p /ps4hdd/app

	# The installer app cannot write to ${boot} while the console is running
	# (it is mounted read-only there), so the archive waits on the data
	# partition next to the kernel. Prefer that, keep the old spot as fallback.
	distro=''
	for d in /ps4hdd/linux /ps4hdd/linux/boot "${boot}"; do
		[ -d "${d}" ] || continue
		distro="$(ps4_find_distro "${d}")" || distro=''
		[ -n "${distro}" ] && break
	done

	if [ -z "${distro}" ]; then
		fail "no distro archive on the internal drive (looked in /ps4hdd/linux and ${boot})"
		return 1
	fi

	free_gb="$(df -Pk /ps4hdd | awk 'END { print int($4 / 1048576) }')"
	if [ "${free_gb}" -lt 2 ]; then
		fail "less than 2 GB free on the internal drive"
		return 1
	fi

	size="${size_gb}"
	case "${size}" in
		''|*[!0-9]*) size=0 ;;
	esac

	# Default: use the free space minus a safety margin, capped at 64 GB.
	if [ "${size}" -lt 1 ]; then
		size=$((free_gb - 4))
		[ "${size}" -gt 64 ] && size=64
	fi
	[ "${size}" -gt "$((free_gb - 1))" ] && size=$((free_gb - 1))
	[ "${size}" -ge 1 ] || { fail "no usable free space"; return 1; }

	say "creating ${size} GB image on the internal drive"
	rm -f "${img}"

	if ! dd if=/dev/null of="${img}" bs=1048576 count=0 seek="$((size * 1024))" 2>/dev/null; then
		fail "could not create ${img}"
		return 1
	fi

	case "${fs}" in
		btrfs|2) mkfs.btrfs -q -f "${img}" ;;
		*)       mkfs.ext4 -q -F -O ^orphan_file,^metadata_csum_seed "${img}" ;;
	esac

	if [ "$?" -ne 0 ] || ! mount "${img}" /newroot; then
		fail "could not create or mount the filesystem"
		rm -f "${img}"
		return 1
	fi

	say "extracting $(basename "${distro}"), this takes a while"
	if ! ps4_extract "${distro}"; then
		fail "distro extraction failed"
		return 1
	fi

	ps4_prepare_newroot
	sync
	umount /newroot
	return 0
}

# ----------------------------------------------------------------------- main

if ! find_job; then
	exit 0
fi

say "job file found: ${conf}"
cfgtext="$(cat "${conf}")"
read_job
say "mode=${mode} fs=${fs} size_gb=${size_gb}"

if job_already_done; then
	say "job ${job} already completed, booting normally"
	[ "${source}" = internal ] || umount "${src}" 2>/dev/null
	exit 0
fi

consumed=1
if [ "${source}" != internal ]; then
	consume_job || consumed=0
fi

ok=0
if [ "${mode}" = internal ]; then
	install_internal && ok=1
else
	install_external && ok=1
fi

[ "${source}" = internal ] || umount "${src}" 2>/dev/null

if [ "${ok}" -ne 1 ]; then
	warn "unattended install failed, dropping to the rescue shell"
	# Put the job back so 'sh /bin/ps4li-auto.sh' can retry it by hand.
	if [ "${consumed}" -eq 1 ] && [ "${source}" != internal ]; then
		printf '%s\n' "${cfgtext}" > "${conf}" 2>/dev/null
		if [ -f "${conf}" ]; then
			ewarn "Fix the problem above, then run: sh /bin/ps4li-auto.sh"
		else
			ewarn "Fix the problem above, then reboot and install again from the app."
		fi
	else
		ewarn "Fix the problem above, then reboot; the job will run again."
	fi
	exit 0
fi

say "install complete, continuing to boot"
