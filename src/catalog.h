#pragma once

#include <stddef.h>

/* Where the installer gets its payloads from.
 *
 * Every URL here was checked against the upstream release at build time; the
 * tutorial's own "Files" page is the source of truth if one of them rots.
 */

#define INITRAMFS_URL \
    "https://github.com/GayCorp/initramfs-ps4/releases/download/latest/initramfs.cpio.gz"

typedef struct {
    const char *name;
    const char *detail;
    const char *url;             /* NULL: must be imported from the USB drive */
    unsigned long long size;     /* approximate download size in bytes */
} DistroEntry;

typedef struct {
    const char *name;
    const char *detail;
    const char *url;
} KernelEntry;

/* Southbridge decides which kernel binary actually boots. The console reports
 * it under Settings > System Information, so we just let the user pick. */
static const KernelEntry KERNELS[] = {
    { "Aeolia / Belize",
      "PS4 Phat, most Slim and most Pro consoles",
      "https://gatto.fae5.de/archive/kernels/strawberry/aeolia/7.1.3/bzImage" },
    { "Baikal",
      "Some Slim and Pro consoles (check System Information)",
      "https://gatto.fae5.de/archive/kernels/strawberry/baikal/7.0.8/bzImage" },
};

#define KERNEL_COUNT ((int)(sizeof(KERNELS) / sizeof(KERNELS[0])))

/* CachyOS is the guide's recommendation but it only exists on Mega/Mediafire,
 * which cannot be fetched programmatically, so it is the "import" entry. */
static const DistroEntry DISTROS[] = {
    { "Arch + KDE",
      "Erkkola's Arch image, KDE desktop",
      "https://github.com/ErkkolaMaitohappo/arch-ps4-aur-smth-fork/releases/download/v1.1.1%2Bkde/psxitarch.tar.xz",
      2137002704ULL },

    { "Arch + XFCE",
      "Erkkola's Arch image, XFCE desktop",
      "https://github.com/ErkkolaMaitohappo/arch-ps4-aur-smth-fork/releases/download/v1.0.1%2Bxfce4/psxitarch.tar.xz",
      1319030084ULL },

    { "Artix + OpenRC",
      "Lean Arch-based image, OpenRC init",
      "https://github.com/ErkkolaMaitohappo/ps4-arch-based-distros/releases/download/artix-ps4-V1/artix-openrc-ps4-V1.1.tar.xz",
      1142321876ULL },

    { "Artix + dinit",
      "Lean Arch-based image, dinit init",
      "https://github.com/ErkkolaMaitohappo/ps4-arch-based-distros/releases/download/artix-ps4-V1/artix-dinit-ps4-V1.1.tar.xz",
      1140855136ULL },

    { "CachyOS Light (USB import)",
      "Mega/Mediafire only: copy the .tar.xz to the USB drive first",
      NULL,
      0ULL },
};

#define DISTRO_COUNT ((int)(sizeof(DISTROS) / sizeof(DISTROS[0])))
