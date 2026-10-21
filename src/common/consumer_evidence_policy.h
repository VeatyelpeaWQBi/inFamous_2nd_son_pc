// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>

namespace Common::ConsumerEvidence {
using Word = std::uint64_t;
inline constexpr Word ConsumerIdBit = Word{1} << 63;
inline constexpr std::string_view Fingerprint =
    "2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab";
enum Register : unsigned { Rax, Rcx, Rdx, Rbx, Rsi, Rdi, R15, R14, Rsp, Rbp, R13, R12, NumRegisters };
struct Site {
    Word rva;
    std::array<std::uint8_t, 16> bytes;
    unsigned length;
    unsigned width{4};
};
inline constexpr std::array Sites{
    Site{0x3f3eff,{0x8b,0x40,0x04},3},
    Site{0x3f3fcc,{0x8b,0x70,0x28},3},
    Site{0x3ff65e,{0xc4,0xa1,0x7a,0x10,0x04,0x38},6},
    Site{0x3ff664,{0xc4,0xa1,0x7a,0x10,0x4c,0x38,0x08},7},
    Site{0x3ff66b,{0xc4,0xa3,0x79,0x21,0x44,0x38,0x04,0x10},8},
    Site{0x169624,{0x44,0x39,0x34,0xc1},4},
    Site{0x3ff6d9,{0x8b,0x54,0x0b,0x08},4},
    Site{0x3ff6e1,{0x48,0x8b,0x0c,0x0b},4,8},
};
inline bool Verified(std::string_view title, std::string_view version, std::string_view sha) {
    return title == "CUSA00309" && version == "01.07" && sha == Fingerprint;
}
inline bool Matches(std::span<const std::uint8_t> image, const Site& site) {
    return site.rva <= image.size() && site.length <= image.size() - site.rva &&
           std::equal(site.bytes.begin(), site.bytes.begin() + site.length, image.begin() + site.rva);
}
inline constexpr std::array SourceLoads{
    Site{0x3f3ef3,{0x49,0x8b,0x87,0xf0,0,0,0,0x48,0x85,0xc0,0x74,0x7c},12},
    Site{0x3f3fc5,{0x49,0x8b,0x87,0xf0,0,0,0},7},
};
inline constexpr std::array RecordCopies{
    Site{0x3ff6d9,{0x8b,0x54,0x0b,0x08,0x41,0x89,0x55,0x08},8},
    Site{0x3ff6e1,{0x48,0x8b,0x0c,0x0b,0x49,0x89,0x4d,0x00,0x49,0x83,0xc5,0x60},12},
};
inline bool MatchesSite(std::span<const std::uint8_t> image, unsigned index) {
    return index < Sites.size() && Matches(image,Sites[index]) &&
           (index >= SourceLoads.size() || Matches(image,SourceLoads[index])) &&
           (index < 6 || Matches(image,RecordCopies[index-6]));
}
inline Word OperandAddress(unsigned site, const std::array<Word, NumRegisters>& r) {
    switch (site) {
    case 0:return r[Rax] + 4;
    case 1:return r[Rax] + 0x28;
    case 2:return r[Rax] + r[R15];
    case 3:return r[Rax] + r[R15] + 8;
    case 4:return r[Rax] + r[R15] + 4;
    case 5:return r[Rcx] + 8 * r[Rax];
    case 6:return r[Rbx] + r[Rcx] + 8;
    case 7:return r[Rbx] + r[Rcx];
    default:return 0;
    }
}
// Saturating lifetime budget. Reloading a tuning file cannot replenish it.
inline unsigned Reserve(std::atomic<unsigned>& used, unsigned limit) {
    auto value = used.load(std::memory_order_relaxed);
    while (value < limit) {
        if (used.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value + 1;
    }
    return 0;
}
inline Word Tag(Word id, bool selected) { return id && selected ? id | ConsumerIdBit : id; }
} // namespace Common::ConsumerEvidence
