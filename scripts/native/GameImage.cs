﻿// SPDX-FileCopyrightText: Copyright 2026 sshadPS4contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Ifss.Native {
    public interface IGameImage { byte[] Read(ulong address, int size); }
    public sealed class GameImage : IGameImage {
        public sealed class Segment {
            public int index; public uint flags;
            public ulong address, file_offset, file_size, mapped_size;
        }
        public readonly byte[] Data;
        public readonly List<Segment> Segments = new List<Segment>();
        public static void Range(byte[] data, ulong offset, ulong size) {
            if (offset > (ulong)data.LongLength || size > (ulong)data.LongLength - offset)
                throw new InvalidDataException("Truncated binary structure.");
        }
        public static ushort U16(byte[] b, ulong at) { Range(b, at, 2); return BitConverter.ToUInt16(b, checked((int)at)); }
        public static uint U32(byte[] b, ulong at) { Range(b, at, 4); return BitConverter.ToUInt32(b, checked((int)at)); }
        public static ulong U64(byte[] b, ulong at) { Range(b, at, 8); return BitConverter.ToUInt64(b, checked((int)at)); }
        public GameImage(string path) : this(File.ReadAllBytes(path)) { }
        public GameImage(byte[] bytes) {
            Data = bytes;
            ulong elf = 0;
            var self = new Dictionary<int, ulong[]>();
            if (U32(bytes, 0) == 0x1d3d154f) {
                int count = U16(bytes, 24); elf = checked(32UL + (ulong)count * 32);
                Range(bytes, 32, (ulong)count * 32);
                for (int i = 0; i < count; ++i) {
                    ulong entry = 32UL + (ulong)i * 32, flags = U64(bytes, entry);
                    if ((flags & 0x800) == 0) continue;
                    if ((flags & 10) != 0) throw new InvalidDataException("Encrypted/compressed SELF segments are unsupported.");
                    int index = (int)((flags >> 20) & 0xfff);
                    if (self.ContainsKey(index)) throw new InvalidDataException("Duplicate SELF segment.");
                    self[index] = new [] { U64(bytes, entry + 8), U64(bytes, entry + 16) };
                }
            }
            Range(bytes, elf, 64);
            if (U32(bytes, elf) != 0x464c457f || bytes[(int)elf + 4] != 2 || bytes[(int)elf + 5] != 1 ||
                U16(bytes, elf + 18) != 62 || U16(bytes, elf + 54) != 56)
                throw new InvalidDataException("Expected a little-endian x86-64 ELF/SELF.");
            ulong table = checked(elf + U64(bytes, elf + 32));
            int entries = U16(bytes, elf + 56); Range(bytes, table, (ulong)entries * 56);
            for (int i = 0; i < entries; ++i) {
                ulong entry = table + (ulong)i * 56;
                uint kind = U32(bytes, entry); ulong memory = U64(bytes, entry + 40);
                if ((kind != 1 && kind != 0x61000010) || memory == 0) continue;
                ulong file = U64(bytes, entry + 32), alignment = U64(bytes, entry + 48), offset = U64(bytes, entry + 8);
                if (file > memory || (alignment != 0 && (alignment & (alignment - 1)) != 0))
                    throw new InvalidDataException("Invalid ELF segment size/alignment.");
                ulong mapped = alignment == 0 ? memory : checked(memory + alignment - 1) & ~(alignment - 1);
                if (self.Count != 0) {
                    ulong[] storage;
                    if (!self.TryGetValue(i, out storage) || storage[1] < file) throw new InvalidDataException("Missing/truncated SELF load segment.");
                    offset = storage[0];
                }
                Range(bytes, offset, file);
                ulong address = U64(bytes, entry + 16); if (mapped > UInt64.MaxValue - address) throw new InvalidDataException("ELF address range overflow.");
                Segments.Add(new Segment { index = i, flags = U32(bytes, entry + 4), address = address,
                                           file_offset = offset, file_size = file, mapped_size = mapped });
            }
            if (Segments.Count == 0) throw new InvalidDataException("Executable has no mapped load segments.");
        }
        public byte[] Read(ulong address, int size) {
            if (size <= 0) throw new InvalidDataException("Invalid patch range.");
            foreach (var segment in Segments) {
                if (address < segment.address) continue;
                ulong relative = address - segment.address;
                if (relative > segment.mapped_size || (ulong)size > segment.mapped_size - relative) continue;
                var result = new byte[size];
                int count = relative >= segment.file_size ? 0 : (int)Math.Min((ulong)size, segment.file_size - relative);
                if (count > 0) Buffer.BlockCopy(Data, checked((int)(segment.file_offset + relative)), result, 0, count);
                return result;
            }
            throw new InvalidDataException("Unmapped patch range: " + address.ToString("x"));
        }
        public static string Fnv1a(byte[] bytes) {
            ulong hash = 0xcbf29ce484222325;
            unchecked { foreach (byte value in bytes) hash = (hash ^ value) * 0x100000001b3; }
            return hash.ToString("x16");
        }
        public static Dictionary<string, object> Identity(string path) { return Identity(File.ReadAllBytes(path)); }
        public static Dictionary<string, object> Identity(byte[] bytes) {
            if (U32(bytes, 0) != 0x46535000) throw new InvalidDataException("Invalid param.sfo.");
            ulong keys = U32(bytes, 8), values = U32(bytes, 12), count = U32(bytes, 16);
            ulong tableEnd = checked(20 + count * 16); Range(bytes, 0, tableEnd);
            if (keys < tableEnd || keys > values || values > (ulong)bytes.Length) throw new InvalidDataException("Invalid SFO key table.");
            var result = new Dictionary<string, object>();
            var utf8 = new UTF8Encoding(false, true);
            for (ulong i = 0; i < count; ++i) {
                ulong entry = 20 + i * 16, start = keys + U16(bytes, entry), length = U32(bytes, entry + 4), at = values + U32(bytes, entry + 12);
                if (start >= values) throw new InvalidDataException("Invalid SFO key.");
                int end = checked((int)start);
                while ((ulong)end < values && bytes[end] != 0) ++end;
                if ((ulong)end == values) throw new InvalidDataException("Unterminated SFO key.");
                Range(bytes, at, length);
                var name = utf8.GetString(bytes, (int)start, end - (int)start);
                if (name != "TITLE_ID" && name != "APP_VER" && name != "CONTENT_ID" && name != "VERSION") continue;
                if (result.ContainsKey(name)) throw new InvalidDataException("Duplicate SFO identity field.");
                result[name] = utf8.GetString(bytes, checked((int)at), checked((int)length)).TrimEnd('\0');
            }
            return result;
        }
    }
}
