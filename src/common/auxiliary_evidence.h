// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <ostream>
#include <span>
#include <string_view>

namespace Common::AuxiliaryEvidence {
using Word = std::uint64_t;
constexpr unsigned MaxPackets = 64;
struct Context { Word id{}; unsigned packets{}, omitted{}; };
constexpr Word Pack(std::uint32_t low, std::uint32_t high) {
    return Word{low} | (Word{high} << 32);
}
inline bool ValidWords(unsigned kind, std::span<const std::uint32_t> words) {
    const auto count = kind == 3 ? 4u : 7u;
    const auto opcode = kind == 1 ? 0x50u : kind == 2 ? 0x49u : 0x3fu;
    return kind >= 1 && kind <= 3 && words.size() == count &&
           words[0] >> 30 == 3 && ((words[0] >> 8) & 0xff) == opcode &&
           ((words[0] >> 16) & 0x3fff) + 2 == count;
}
void Configure(std::string_view title, std::string_view version, std::string_view sha,
               unsigned limit, unsigned delay_seconds);
Context Begin(Word queue, Word base, Word bytes, Word parent) noexcept;
// kind 1 DMA_DATA, 2 RELEASE_MEM, 3 INDIRECT_BUFFER. No guest-memory dereference
// beyond the packet already selected and bounded by the normal decoder.
void Packet(Context& context, unsigned kind, Word address, bool buffered,
            std::span<const std::uint32_t> words) noexcept;
void End(const Context& context, bool finished, bool consumed, bool indirect) noexcept;
void WriteMetadata(std::ostream& output);
} // namespace Common::AuxiliaryEvidence
