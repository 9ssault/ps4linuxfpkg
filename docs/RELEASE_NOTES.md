# PS4 Linux Installer v1.0.5

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
  environment status for storage, FTP and PayLoader.
- **Server discovery.** Probes `192.168.18.16` then `127.0.0.1` for GoldHEN's
  FTP (2121) and PayLoader (9090).
- **FTP fallback.** If the app sandbox blocks `/data`, files are uploaded
  through GoldHEN's FTP server instead.
- **Bundled payload.** `ps4-linux-loader` v25.1 ships in the package.
- **Step notifications.** Startup, every download and write, each sandbox
  fallback, the payload send and every failure also post to the PS4
  notification panel, so progress stays visible even if the app loses focus.

## Fixed in v1.0.5

- **PayLoader.** GoldHEN calls the payload listener *PayLoader*, not BinLoader,
  so the app uses that name too. Its state is no longer probed at startup: a
  bare connect-and-hang-up disturbs the listener, which is why it reported
  "down" while the setting was on. The first real send is the test now, with
  three attempts, and the pill turns red only after those fail.

## Fixed in v1.0.4

- **Internal installs were writing to a read-only mount.** The distro archive
  went to `/user/system/boot`, which the console mounts read-only while it is
  running (even GoldHEN's FTP gets `550` there), so the 2 GB download died a
  fraction of a second in. It now goes to `/data/linux/distro.tar.xz` next to
  the boot files, and the unattended installer looks there first.
- **Write failures say why.** A short write now logs the destination, the byte
  count and `errno`, so a full or refused drive is obvious from
  `/data/ps4li.log`.
- **Progress redraw.** The work screen refreshed every 120 ms; it now refreshes
  every 80 ms. Idle screens only redraw on input, which is why a frame counter
  shows a low number while nothing is happening.

## Fixed in v1.0.3

- **Distro download.** GitHub answers a release asset with a ~5 KB redirect
  header and a ~2 KB signed URL. libSceHttp rejected the first with
  `TOO_LARGE_RESPONSE_HEADER` and the app truncated the second, so every
  distro failed right after the kernel. The header limit is raised and URLs
  can now be 4 KB long.
- **Frame cost.** The canvas was forced to a different pixel format than the
  window, so every present converted two million pixels on the CPU. The canvas
  is now created in the window's own format, alpha blending avoids division,
  and the frame time is written to the log.

## Fixed in v1.0.2

- **Free space.** Orbis reports no available blocks on FAT32 USB drives and on
  the PFS data partition, so a drive with gigabytes left was shown as `0 KB
  free` and the size check was skipped. The reading now falls back to the
  free-block count, and says `unknown` instead of `0 KB`.
- **Notifications.** They were posted with a user id where the console wants
  the notification type, so they appeared empty. They now read as plain steps
  ("Downloading the kernel (bzImage)", "Writing the rescue initramfs").
- **Speed.** The renderer re-rasterised every glyph on every frame at 1080p,
  which is where the ~2 fps came from. Glyphs are cached and the screen is only
  redrawn when something changes.
- **Diagnostics.** Every step and the raw `statfs` counters go to
  `/data/ps4li.log`, readable over GoldHEN's FTP after a failed install.

## Distros

Arch + KDE, Arch + XFCE, Artix + OpenRC, Artix + dinit. CachyOS Light is
available as a USB import because it is only hosted on Mega/Mediafire.

## Requirements

- GoldHEN v2.4b18.10 or newer (not v2.4b18.8).
- FTP and PayLoader enabled in GoldHEN's server settings.
- External installs need a second, empty USB drive; it will be erased.

## Install

1. Put `ps4-linux-installer.pkg` on a USB drive.
2. GoldHEN → Debug Settings → Package Installer → source `usb[ usb:/ ]`.
3. Install and launch **PS4 Linux Installer**.

## Known limitations

- The console-side behaviour is untested in CI: sandbox writes, PayLoader
  reachability, HTTPS redirects and the unattended partitioning all need a
  real PS4.
- PS4 system settings (resolution, RGB range) and enabling GoldHEN's servers
  remain manual.
- A disc in the drive can corrupt the install; remove it first.

## SHA256

See `SHA256SUMS.txt` attached to this release.
