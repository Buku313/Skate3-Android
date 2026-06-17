#!/usr/bin/env python3
"""Minimal XDVDFS (Xbox 360 GDF) extractor.

Ports the parser in src/skate3_iso_installer.cpp so the build-time game dump can
be produced on any host without the GUI app. Extracts either specific files or
the whole tree.

Usage:
  extract_xiso.py ISO OUT_DIR                 # extract everything
  extract_xiso.py ISO OUT_DIR f1 [f2 ...]     # extract only the named paths
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
OFFSETS = [0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000]


def find_game_offset(f, size):
    for cand in OFFSETS:
        mo = cand + 32 * SECTOR
        if mo + len(MAGIC) > size:
            continue
        f.seek(mo)
        if f.read(len(MAGIC)) == MAGIC:
            return cand
    raise SystemExit("Not a recognized Xbox 360 game ISO (no XDVDFS magic).")


def parse_dir(f, game_off, dir_off):
    """Return list of (path, offset, size) for files under dir_off."""
    entries = []
    pending = [(dir_off, 0, "")]
    visited = 0
    while pending:
        directory_offset, node_offset, prefix = pending.pop()
        visited += 1
        if visited > 500000:
            raise SystemExit("Directory tree unexpectedly large.")
        eo = directory_offset + node_offset
        f.seek(eo)
        header = f.read(14)
        if len(header) < 14:
            raise SystemExit("Failed to read a directory entry.")
        left, right, sector, length = struct.unpack_from("<HHII", header, 0)
        attributes = header[12]
        name_length = header[13]
        if name_length == 0 or name_length > 240:
            raise SystemExit("Invalid directory entry name length.")
        f.seek(eo + 14)
        name = f.read(name_length).decode("latin-1")
        if left:
            pending.append((directory_offset, left * 4, prefix))
        if right:
            pending.append((directory_offset, right * 4, prefix))
        full = prefix + name
        if attributes & 0x10:  # directory
            if length != 0:
                pending.append((game_off + sector * SECTOR, 0, full + "/"))
        else:
            entries.append((full, game_off + sector * SECTOR, length))
    return entries


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        raise SystemExit(2)
    iso, out_dir = sys.argv[1], sys.argv[2]
    wanted = {w.replace("\\", "/").lower() for w in sys.argv[3:]}

    with open(iso, "rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        game_off = find_game_offset(f, size)
        f.seek(game_off + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        if not (13 <= root_size <= 32 * 1024 * 1024):
            raise SystemExit("Invalid root directory.")
        entries = parse_dir(f, game_off, game_off + root_sector * SECTOR)

        total = 0
        for path, off, length in entries:
            if wanted and path.lower() not in wanted:
                continue
            target = os.path.join(out_dir, path)
            os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
            f.seek(off)
            remaining = length
            with open(target, "wb") as out:
                while remaining:
                    chunk = f.read(min(remaining, 4 * 1024 * 1024))
                    if not chunk:
                        raise SystemExit("Short read for " + path)
                    out.write(chunk)
                    remaining -= len(chunk)
            total += length
            print(f"  {path}  ({length:,} bytes)")
        print(f"Extracted {total:,} bytes from game offset 0x{game_off:08X} "
              f"({len(entries)} files in image).")


if __name__ == "__main__":
    main()
