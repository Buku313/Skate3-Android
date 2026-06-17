#!/usr/bin/env python3

import argparse
from pathlib import Path
import shutil
import struct
import sys


BLOCK_SIZE = 0x1000
END_OF_CHAIN = 0xFFFFFF
EXPECTED_FILES = {
    "default.xexp",
    "data/webkit/EAWebkit.xexp",
}


def be16(data, offset):
    return struct.unpack_from(">H", data, offset)[0]


def be32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def u24le(data, offset):
    return data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16)


def round_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


class StfsReader:
    def __init__(self, package_path):
        self.package_path = package_path
        self.data = package_path.read_bytes()
        self.header_size = be32(self.data, 0x340)
        self.metadata_offset = 0x344
        self.volume_descriptor_offset = self.metadata_offset + 0x35
        self.volume_type = be32(self.data, self.metadata_offset + 0x65)
        if self.data[:4] not in (b"CON ", b"LIVE", b"PIRS"):
            raise ValueError(f"{package_path} is not an STFS package")
        if self.volume_type != 0:
            raise ValueError(f"{package_path} is not an STFS volume")
        flags = self.data[self.volume_descriptor_offset + 2]
        self.blocks_per_hash_table = 1 if (flags & 1) else 2

    def block_to_offset(self, block_index):
        block = block_index
        base = 170
        for _ in range(3):
            block += ((block_index + base) // base) * self.blocks_per_hash_table
            if block_index < base:
                break
            base *= 170
        return round_up(self.header_size, BLOCK_SIZE) + (block << 12)

    def hash_block_number(self, block_index, hash_level):
        block_step0 = 170 + self.blocks_per_hash_table
        block_step1 = 28900 + ((170 + 1) * self.blocks_per_hash_table)
        if hash_level == 0:
            if block_index < 170:
                return 0
            block = (block_index // 170) * block_step0
            block += ((block_index // 28900) + 1) * self.blocks_per_hash_table
            if block_index < 28900:
                return block
            return block + self.blocks_per_hash_table
        if hash_level == 1:
            if block_index < 28900:
                return block_step0
            block = (block_index // 28900) * block_step1
            return block + self.blocks_per_hash_table
        return block_step1

    def hash_offset(self, block_index, hash_level):
        return round_up(self.header_size, BLOCK_SIZE) + (
            self.hash_block_number(block_index, hash_level) << 12
        )

    def next_block(self, block_index):
        hash_offset = self.hash_offset(block_index, 0)
        entry_offset = hash_offset + (block_index % 170) * 0x18
        return be32(self.data, entry_offset + 0x14) & 0xFFFFFF

    def read_file(self, start_block, size):
        output = bytearray()
        block_index = start_block
        remaining = size
        while remaining and block_index != END_OF_CHAIN:
            chunk_size = min(BLOCK_SIZE, remaining)
            offset = self.block_to_offset(block_index)
            output.extend(self.data[offset : offset + chunk_size])
            remaining -= chunk_size
            block_index = self.next_block(block_index)
        if remaining:
            raise ValueError(f"STFS chain ended with {remaining} bytes remaining")
        return bytes(output)

    def list_entries(self):
        descriptor = self.volume_descriptor_offset
        table_block_count = struct.unpack_from("<H", self.data, descriptor + 3)[0]
        table_block = u24le(self.data, descriptor + 5)
        entries = []

        for _ in range(table_block_count):
            table_offset = self.block_to_offset(table_block)
            for index in range(0x40):
                entry_offset = table_offset + index * 0x40
                if self.data[entry_offset] == 0:
                    break

                flags = self.data[entry_offset + 40]
                name_length = flags & 0x3F
                name = self.data[entry_offset : entry_offset + name_length].decode("utf-8")
                is_dir = bool(flags & 0x80)
                start_block = u24le(self.data, entry_offset + 47)
                parent_index = be16(self.data, entry_offset + 50)
                length = be32(self.data, entry_offset + 52)

                parent_path = "" if parent_index == 0xFFFF else entries[parent_index]["path"]
                path = name if not parent_path else f"{parent_path.rstrip('/')}/{name}"
                entries.append(
                    {
                        "path": path,
                        "is_dir": is_dir,
                        "start_block": start_block,
                        "length": length,
                    }
                )

            next_table_block = self.next_block(table_block)
            if next_table_block == END_OF_CHAIN:
                break
            table_block = next_table_block

        return entries


def copy_required_file(source_root, relative_path, output_root):
    source = source_root / relative_path
    if not source.is_file():
        raise FileNotFoundError(f"required game file not found: {source}")
    destination = output_root / relative_path
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def main():
    parser = argparse.ArgumentParser(
        description="Stage Skate 3 title update XEXP files for codegen."
    )
    parser.add_argument("--tu", required=True, type=Path)
    parser.add_argument("--game-root", required=True, type=Path)
    parser.add_argument("--out-root", required=True, type=Path)
    args = parser.parse_args()

    reader = StfsReader(args.tu)
    entries = reader.list_entries()
    files = {entry["path"]: entry for entry in entries if not entry["is_dir"]}
    unexpected = sorted(set(files) - EXPECTED_FILES)
    missing = sorted(EXPECTED_FILES - set(files))
    if missing:
        raise ValueError(f"title update is missing expected files: {', '.join(missing)}")
    if unexpected:
        raise ValueError(f"title update contains unexpected payload files: {', '.join(unexpected)}")

    args.out_root.mkdir(parents=True, exist_ok=True)
    copy_required_file(args.game_root, Path("default.xex"), args.out_root)
    copy_required_file(args.game_root, Path("data/webkit/EAWebkit.xex"), args.out_root)

    for relative_name in sorted(EXPECTED_FILES):
        entry = files[relative_name]
        destination = args.out_root / relative_name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(reader.read_file(entry["start_block"], entry["length"]))
        print(f"extracted {relative_name} ({entry['length']} bytes)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
