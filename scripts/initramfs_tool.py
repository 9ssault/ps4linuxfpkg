#!/usr/bin/env python3
"""Read, extract and patch gzipped newc (initramfs) cpio archives.

No cpio(1) is needed: entries are handled in memory, so symlinks, permissions
and hardlink counts survive untouched.

  initramfs_tool.py list    <archive.cpio.gz>
  initramfs_tool.py extract <archive.cpio.gz> <output_dir>
  initramfs_tool.py patch   <in.cpio.gz> <out.cpio.gz> <auto.sh> <hook.sh>
"""
import gzip
import os
import stat
import sys

MAGIC = b"070701"
HEADER_SIZE = 110
TRAILER = "TRAILER!!!"

# Offset of the "run_hooks autoinstall" insertion point inside init.
INIT_ANCHOR = b"run_hooks ps4hdd_open\nfi\n"
INIT_INSERT = (
    b"run_hooks ps4hdd_open\nfi\n\n"
    b"# ps4-linux-installer: unattended install, only when a job is queued.\n"
    b"run_hooks autoinstall\n"
)


class Entry:
    def __init__(self, name, mode, data, ino=0, uid=0, gid=0, nlink=1,
                 mtime=0, devmajor=0, devminor=0, rdevmajor=0, rdevminor=0):
        self.name = name
        self.mode = mode
        self.data = data
        self.ino = ino
        self.uid = uid
        self.gid = gid
        self.nlink = nlink
        self.mtime = mtime
        self.devmajor = devmajor
        self.devminor = devminor
        self.rdevmajor = rdevmajor
        self.rdevminor = rdevminor

    @property
    def is_dir(self):
        return stat.S_ISDIR(self.mode)

    @property
    def is_symlink(self):
        return stat.S_ISLNK(self.mode)


def _align4(value):
    return (4 - (value % 4)) % 4


def read_archive(path):
    with gzip.open(path, "rb") as fp:
        blob = fp.read()

    entries = []
    offset = 0

    while offset + HEADER_SIZE <= len(blob):
        header = blob[offset:offset + HEADER_SIZE]
        if header[:6] != MAGIC:
            raise ValueError("not a newc archive at offset %d" % offset)

        fields = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        (ino, mode, uid, gid, nlink, mtime, filesize,
         devmajor, devminor, rdevmajor, rdevminor, namesize, _check) = fields

        offset += HEADER_SIZE
        name = blob[offset:offset + namesize - 1].decode("utf-8", "replace")
        offset += namesize
        offset += _align4(HEADER_SIZE + namesize)

        if name == TRAILER:
            break

        data = blob[offset:offset + filesize]
        offset += filesize
        offset += _align4(filesize)

        entries.append(Entry(name, mode, data, ino, uid, gid, nlink, mtime,
                             devmajor, devminor, rdevmajor, rdevminor))

    return entries


def write_archive(entries, path):
    out = bytearray()

    for entry in entries:
        name = entry.name.encode("utf-8") + b"\0"
        header = MAGIC + b"".join(
            b"%08X" % value for value in (
                entry.ino, entry.mode, entry.uid, entry.gid, entry.nlink,
                entry.mtime, len(entry.data), entry.devmajor, entry.devminor,
                entry.rdevmajor, entry.rdevminor, len(name), 0,
            )
        )
        out += header
        out += name
        out += b"\0" * _align4(HEADER_SIZE + len(name))
        out += entry.data
        out += b"\0" * _align4(len(entry.data))

    out += MAGIC + b"".join(b"%08X" % value for value in
                            (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, 0))
    out += b"TRAILER!!!\0"
    out += b"\0" * _align4(HEADER_SIZE + 11)

    try:
        with gzip.open(path, "wb", compresslevel=9) as fp:
            fp.write(out)
    except OSError:
        # gzip.open on some platforms truncates the file when it already exists.
        os.remove(path)
        with gzip.open(path, "wb", compresslevel=9) as fp:
            fp.write(out)


def find(entries, name):
    for entry in entries:
        if entry.name == name:
            return entry
    return None


def cmd_list(path):
    entries = read_archive(path)
    total = 0
    for entry in entries:
        kind = "d" if entry.is_dir else ("l" if entry.is_symlink else "-")
        print("%s %06o %8d %s" % (kind, stat.S_IMODE(entry.mode),
                                  len(entry.data), entry.name))
        total += len(entry.data)
    print("-- %d entries, %d bytes of file data" % (len(entries), total))


def cmd_extract(path, out_dir):
    entries = read_archive(path)
    os.makedirs(out_dir, exist_ok=True)

    for entry in entries:
        target = os.path.join(out_dir, entry.name)

        if entry.is_dir:
            os.makedirs(target, exist_ok=True)
            continue

        parent = os.path.dirname(target)
        if parent:
            os.makedirs(parent, exist_ok=True)

        if entry.is_symlink:
            if os.path.lexists(target):
                os.remove(target)
            os.symlink(entry.data.decode("utf-8", "replace"), target)
            continue

        with open(target, "wb") as fp:
            fp.write(entry.data)

        os.chmod(target, stat.S_IMODE(entry.mode))

    print("extracted %d entries to %s" % (len(entries), out_dir))


def cmd_patch(src, dst, auto_script, hook_script):
    entries = read_archive(src)

    init = find(entries, "init")
    if init is None:
        raise SystemExit("init not found in %s" % src)

    if INIT_INSERT in init.data:
        print("init already patched")
    else:
        if INIT_ANCHOR not in init.data:
            raise SystemExit(
                "init layout changed upstream: anchor '%s' not found. "
                "Update INIT_ANCHOR in scripts/initramfs_tool.py." % INIT_ANCHOR.decode().strip()
            )
        init.data = init.data.replace(INIT_ANCHOR, INIT_INSERT, 1)
        print("patched init")

    with open(auto_script, "rb") as fp:
        auto_data = fp.read()
    with open(hook_script, "rb") as fp:
        hook_data = fp.read()

    next_ino = max((entry.ino for entry in entries), default=0) + 1

    def upsert(name, data, mode):
        nonlocal next_ino
        entry = find(entries, name)
        if entry:
            entry.data = data
            entry.mode = mode
            return
        entries.append(Entry(name, mode, data, ino=next_ino, nlink=1))
        next_ino += 1

    upsert("bin/ps4li-auto.sh", auto_data, stat.S_IFREG | 0o755)
    upsert("hooks/autoinstall/10-ps4li-auto", hook_data, stat.S_IFREG | 0o755)

    write_archive(entries, dst)

    # Re-read to prove the archive we just wrote parses and carries the files.
    check = read_archive(dst)
    for name in ("bin/ps4li-auto.sh", "hooks/autoinstall/10-ps4li-auto"):
        if find(check, name) is None:
            raise SystemExit("verification failed: %s missing after write" % name)

    print("wrote %s (%d entries)" % (dst, len(check)))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    command = argv[1]

    if command == "list":
        cmd_list(argv[2])
    elif command == "extract" and len(argv) == 4:
        cmd_extract(argv[2], argv[3])
    elif command == "patch" and len(argv) == 6:
        cmd_patch(argv[2], argv[3], argv[4], argv[5])
    else:
        print(__doc__)
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
