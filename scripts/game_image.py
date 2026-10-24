# SPDX-FileCopyrightText: Copyright 2026 shadPS4 contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only SELF/ELF patch analysis using the installed Python standard library.

No game bytes are written to the report. Padding/BSS is interpreted as zero-filled
using the same segment alignment as src/core/module.cpp, not as bytes in the SELF.
"""

import struct


def unpack(fmt, data, offset=0):
    if offset < 0 or offset + struct.calcsize(fmt) > len(data):
        raise ValueError("Truncated binary structure")
    return struct.unpack_from(fmt, data, offset)


LOAD_TYPES = (1, 0x61000010)


def fnv1a(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


class Image:
    def __init__(self, path):
        self.data = path.read_bytes()
        self.segments = []
        self_segments = {}
        elf_offset = 0
        if self.data[:4] == bytes.fromhex("4f153d1d"):
            count = unpack("<H", self.data, 24)[0]
            elf_offset = 32 + count * 32
            for index in range(count):
                flags, offset, size, _ = unpack(
                    "<QQQQ", self.data, 32 + index * 32
                )
                if flags & 0x800:
                    if flags & (2 | 8):
                        raise ValueError("Encrypted/compressed SELF segments are unsupported")
                    self_segments[(flags >> 20) & 0xFFF] = (offset, size)
        header = unpack("<16sHHIQQQIHHHHHH", self.data, elf_offset)
        if header[0][:6] != b"\x7fELF\x02\x01" or header[2] != 62 or header[9] != 56:
            raise ValueError("Expected a little-endian x86-64 ELF/SELF")
        for index in range(header[10]):
            p = unpack("<IIQQQQQQ", self.data,
                                   elf_offset + header[5] + index * header[9])
            kind, flags, offset, address, _, file_size, memory_size, alignment = p
            if kind not in LOAD_TYPES or memory_size == 0:
                continue
            if file_size > memory_size:
                raise ValueError("Segment file size exceeds memory size")
            if alignment and alignment & (alignment - 1):
                raise ValueError("Invalid segment alignment")
            mapped_size = ((memory_size + alignment - 1) & ~(alignment - 1)
                           if alignment else memory_size)
            if self_segments:
                if index not in self_segments:
                    raise ValueError("Missing SELF load segment")
                offset, stored_size = self_segments[index]
                if stored_size < file_size:
                    raise ValueError("Truncated SELF segment")
            if offset + file_size > len(self.data):
                raise ValueError("Segment lies outside the file")
            self.segments.append({"index": index, "flags": flags, "address": address,
                                  "file_offset": offset, "file_size": file_size,
                                  "mapped_size": mapped_size})

    def segment_for(self, address, size):
        if address < 0 or size <= 0:
            raise ValueError("Invalid patch range")
        for segment in self.segments:
            if (segment["address"] <= address and
                    address + size <= segment["address"] + segment["mapped_size"]):
                return segment
        raise ValueError(f"Unmapped patch range {address:#x}+{size:#x}")

    def read(self, address, size):
        segment = self.segment_for(address, size)
        relative = address - segment["address"]
        count = max(0, min(size, segment["file_size"] - relative))
        start = segment["file_offset"] + relative
        return self.data[start:start + count] + bytes(size - count)


def read_identity(path):
    data = path.read_bytes()
    magic, _, key_offset, value_offset, count = unpack("<IIIII", data)
    if magic != 0x46535000:
        raise ValueError("Invalid param.sfo")
    result = {}
    for index in range(count):
        key, _, length, _, value = unpack("<HHIII", data, 20 + index * 16)
        start = key_offset + key
        if not 20 + count * 16 <= key_offset <= start < value_offset <= len(data):
            raise ValueError("Invalid SFO key table")
        terminator = data.find(b"\0", start, value_offset)
        if terminator < 0 or value_offset + value + length > len(data):
            raise ValueError("Invalid SFO entry")
        name = data[start:terminator].decode("utf-8")
        if name in ("TITLE_ID", "APP_VER", "CONTENT_ID", "VERSION"):
            result[name] = data[value_offset + value:value_offset + value + length].rstrip(
                b"\0").decode("utf-8")
    return result
