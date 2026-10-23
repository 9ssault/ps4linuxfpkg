<img src="branding/banner.svg" alt="PS4 Linux Installer" width="100%">

# PS4 Linux Installer

A single fake package (`.pkg`) that sets up Linux on a jailbroken PS4 with the
same steps as [DionKill's tutorial](https://dionkill.github.io/ps4-linux-tutorial/),
but without copying files over FTP or typing install commands.

Pick a distro, pick the drive, press X. The app downloads the kernel, the
patched rescue initramfs and the distro, places them where the loader expects
them, queues an unattended install, and starts the boot payload. The console
installs itself and lands on the desktop.

<p align="center">
  <img src="branding/logo.svg" alt="logo" width="140">
</p>

## What happens on a first install

1. You choose **Install Linux**, storage, southbridge and distro.
2. The app writes everything the tutorial asks for:
   - `bzImage` + `initramfs.cpio.gz` + `bootargs.txt` + `vram.txt`
   - `distro.tar.xz`
   - a `ps4li.conf` job file for the unattended installer
   - the loader payload in `/data/payloads/`
3. It sends the loader to GoldHEN's BinLoader.
4. The kernel boots the patched initramfs, which formats and extracts without a
   single prompt, then continues straight into the new system.

**No rescue-shell typing.** The patched initramfs is upstream
[initramfs-ps4](https://github.com/GayCorp/initramfs-ps4) plus one hook. Without
a job file it behaves exactly like the stock image, so it stays useful for
manual installs.

## Features

| | |
| --- | --- |
| Zero-typing install | Internal HDD or external USB, fully unattended |
| Manual mode | Optional; drops to the normal rescue shell if you prefer |
| More options | Boot VRAM, internal filesystem, image size, boot arguments |
| PS4-blue UI | 1080p, controller and keyboard, Inter typeface |
| Step notifications | Every download, write, payload send and error also lands in the PS4 notification panel |
| Server discovery | Probes `192.168.18.16` and `127.0.0.1` for FTP 2121 / BinLoader 9090 |
| FTP fallback | If the app sandbox blocks `/data`, writes go through GoldHEN's FTP |
| Bundled payload | The loader ships inside the package, no extra download |

## Requirements

- PS4 with **GoldHEN v2.4b18.10+** (v2.4b18.8 cannot boot Linux).
- GoldHEN → Server settings: **FTP server** and **BinLoader server** enabled.
- Internet access on the console.
- External install: a FAT32/exFAT USB drive for the boot files **and a second,
  empty drive** for Linux. The second drive is erased.
- A disc inside the drive? Remove it before installing.

## Install the package

1. Copy `out/ps4-linux-installer.pkg` to a USB drive.
2. GoldHEN → Debug Settings → Package Installer → source `usb[ usb:/ ]`.
3. Install **PS4 Linux Installer**, then launch it from the home screen.

## Options

| Option | Values | Notes |
| --- | --- | --- |
| Automatic install | On / Off | Off leaves the stock rescue-shell workflow |
| Boot VRAM | 1024 / 2048 / 3072 / 4096 MB | 2 GB is a good default for the desktop |
| Internal filesystem | ext4 / btrfs | Internal installs only |
| Internal size | Auto / 16 / 32 / 64 / 128 GB | Auto = free space minus 4 GB, capped at 64 GB |
| Boot arguments | Default / Performance | Performance adds `mitigations=off` |

The installer always uses 1 GB of VRAM for the first boot, as the guide
requires; the VRAM option applies to **Boot Linux** afterwards.

## Distros

| Distro | Source | Size |
| --- | --- | --- |
| Arch + KDE | Erkkola's releases | ~2.1 GB |
| Arch + XFCE | Erkkola's releases | ~1.3 GB |
| Artix + OpenRC | Erkkola's releases | ~1.1 GB |
| Artix + dinit | Erkkola's releases | ~1.1 GB |
| CachyOS Light | Mega/Mediafire only | import from USB |

For CachyOS, put its `.tar.xz` in the USB root and pick the import entry.

## Kernels

| Southbridge | Kernel |
| --- | --- |
| Aeolia / Belize | 7.1.3 strawberry (mirror) |
| Baikal | 7.0.8 strawberry (mirror) |

The southbridge is listed under **Settings → System Information**.

## Server addresses

The app looks for GoldHEN on `192.168.18.16` first and falls back to loopback:

| Service | Port | Purpose |
| --- | --- | --- |
| FTP | 2121 | file writes when the sandbox blocks them, directory creation |
| BinLoader | 9090 | receives the Linux boot payload |

The home screen shows which host answered. If both show as down, the console
cannot reach those ports and you have to launch the payload from
**GoldHEN → Utilities → PayLoader**.

## Layout

```
src/main.c        screens, options, downloads, job file, payload handoff
src/ui.c          SDL2 + FreeType renderer (Inter regular/semibold), input
src/net.c         libSceHttp downloads with manual redirects, BinLoader sender
src/ftp.c         minimal GoldHEN FTP client (probe, MKD, STOR)
src/storage.c     paths, free space, filesystem type, archive lookup
src/catalog.h     kernel and distro catalog with the exact URLs
initramfs/        unattended installer + init hook baked into the initramfs
scripts/          toolchain setup, initramfs build, payload fetch, icon, build
branding/         SVG logo and banner
```

## Building

```bash
scripts/setup-toolchain.sh    # OpenOrbis v0.5.4 + LLVM 18 + .NET deps
scripts/build.sh              # -> out/ps4-linux-installer.pkg
```

`build.sh` also fetches the loader payload and builds the patched initramfs on
first run. It needs `curl`, `tar`, `unzip`, `dpkg-deb`, `python3` and about
1.5 GB of disk. No root is needed.

Useful sub-scripts:

```bash
scripts/build-initramfs.sh                  # rebuild assets/initramfs.cpio.gz
python3 scripts/make-icon.py                # regenerate sce_sys/icon0.png
python3 scripts/initramfs_tool.py list assets/initramfs.cpio.gz
```

## Verification

Checked in the build environment:

- Clean build, no warnings; the package is 16 MB with the initramfs inside.
- `PkgTool.Core pkg_extract` round-trips every bundled file byte-for-byte
  (`eboot.bin`, both payloads, both fonts, the PRX modules, `right.sprx`).
- The embedded `param.sfo` reads back as `PS4 Linux Installer` /
  `PLNX00001` / `IV0000-PLNX00001_00-PS4LINUXINSTALL0`.
- `eboot.bin` starts with the `4F 15 3D 1D` SELF magic.
- The patched initramfs re-parses after writing: `init` contains the
  `run_hooks autoinstall` line, `bin/ps4li-auto.sh` and the hook are present
  and executable, and `sh -n` accepts the auto-install script.
- The job file the app writes matches the keys the initramfs script parses.

Checked against a live console (GoldHEN FTP on `192.168.18.16:2121`):

- The FTP fallback path works where the sandbox would block a write: `MKD` on
  `/data/payloads`, `STOR` of the 305 KB loader and of the 16 MB package, and
  `DELE` all return `226`, and every file read back byte-identical (SHA-256).
- The package uploads to a USB drive at `usb0:/ps4-linux-installer.pkg` at
  roughly 2-3 MB/s and matches the release asset exactly.

**Still needs the console, in person:** BinLoader on `9090` must be switched on
in GoldHEN's server settings, launch and UI behaviour, HTTPS redirects through
`libSceHttp`, download throughput for the distro images, the sandboxed `/data`
and `/user` probes, and the unattended installer actually partitioning a real
drive and booting.

Safety choices worth knowing:

- The unattended external install refuses to run unless **exactly one**
  non-source USB drive is connected, so it cannot pick the wrong disk.
- The job file is deleted before anything is written; if it ever cannot be
  deleted, a `ps4li.done` marker prevents a reinstall on every boot.
- A failed unattended install drops to the normal rescue shell with the error
  still on screen.

## Releasing

`out/ps4-linux-installer.pkg` is the release asset. After `gh auth login`:

```bash
scripts/publish.sh <owner>/ps4linuxfpkg v1.0.0
```

The script creates or updates the remote, pushes the branch, and attaches the
package plus `SHA256SUMS.txt` to a release. To do it by hand:

```bash
mkdir -p dist && cp out/ps4-linux-installer.pkg dist/
(cd dist && sha256sum ps4-linux-installer.pkg > SHA256SUMS.txt)
gh release create v1.0.0 dist/ps4-linux-installer.pkg dist/SHA256SUMS.txt \
  --title "v1.0.0" --notes-file docs/RELEASE_NOTES.md
```

## Credits

The install method, kernel mirrors and distro images come from the PS4 Linux
community: rmux, feeRnt, crashniels, marcan/fail0verflow, rancido, eeply,
valeryy, mircoho, codedwrench, GayCorp's initramfs and the rest of the credits
on the tutorial site. This app only moves their files around.

Bundled third-party content:

- [ps4-linux-loader](https://github.com/ps4-linux/ps4-linux-loader) v25.1,
  fetched at build time.
- [initramfs-ps4](https://github.com/GayCorp/initramfs-ps4), patched.
- [Inter](https://rsms.me/inter/) typeface, SIL Open Font License 1.1
  (`assets/fonts/Inter-LICENSE.txt`).
