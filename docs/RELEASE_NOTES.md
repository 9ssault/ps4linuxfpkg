# PS4 Linux Installer v1.0.1

One fake package that performs the whole PS4 Linux setup from
[dionkill.github.io/ps4-linux-tutorial](https://dionkill.github.io/ps4-linux-tutorial/).

## Highlights

- **Zero-typing install.** The package carries a patched `initramfs-ps4` that
  formats the target, copies the boot files, extracts the distro and continues
  into the new system without a single prompt. Without a queued job it behaves
  exactly like the stock image.
- **More options.** Automatic or manual install, boot VRAM (1/2/3/4 GB),
  internal filesystem (ext4/btrfs), internal image size (auto/16/32/64/128 GB)
  and performance boot arguments (`mitigations=off`).
- **PS4-blue UI.** 1080p, controller and keyboard, Inter typeface, live
  environment status for storage, FTP and BinLoader.
- **Server discovery.** Probes `192.168.18.16` then `127.0.0.1` for GoldHEN's
  FTP (2121) and BinLoader (9090).
- **FTP fallback.** If the app sandbox blocks `/data`, files are uploaded
  through GoldHEN's FTP server instead.
- **Bundled payload.** `ps4-linux-loader` v25.1 ships in the package.
- **Step notifications.** Startup, every download and write, each sandbox
  fallback, the payload send and every failure also post to the PS4
  notification panel, so progress stays visible even if the app loses focus.

## Distros

Arch + KDE, Arch + XFCE, Artix + OpenRC, Artix + dinit. CachyOS Light is
available as a USB import because it is only hosted on Mega/Mediafire.

## Requirements

- GoldHEN v2.4b18.10 or newer (not v2.4b18.8).
- FTP and BinLoader enabled in GoldHEN's server settings.
- External installs need a second, empty USB drive; it will be erased.

## Install

1. Put `ps4-linux-installer.fpkg` on a USB drive.
2. GoldHEN → Debug Settings → Package Installer → source `usb[ usb:/ ]`.
3. Install and launch **PS4 Linux Installer**.

## Known limitations

- The console-side behaviour is untested in CI: sandbox writes, BinLoader
  reachability, HTTPS redirects and the unattended partitioning all need a
  real PS4.
- PS4 system settings (resolution, RGB range) and enabling GoldHEN's servers
  remain manual.
- A disc in the drive can corrupt the install; remove it first.

## SHA256

See `SHA256SUMS.txt` attached to this release.
