// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace VideoCore {
inline constexpr std::uint64_t IfssSortShaderHash = 0xa16fa93aULL;

inline bool IsPinnedIfssSortCode(std::span<const std::uint32_t> code) {
    if (code.size_bytes() != 708) return false;
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto word : code)
        for (unsigned shift = 0; shift < 32; shift += 8)
            hash = (hash ^ ((word >> shift) & 255u)) * 1099511628211ULL;
    return hash == 0x8b30a7eaddd8ca74ULL;
}

struct IfssSortDispatch {
    std::array<std::uint32_t, 3> groups{}, threads{}, start{};
    std::uint32_t user_regs{}, tgid_mask{};
    bool scratch{};
};
struct IfssSortBuffer {
    std::uint64_t address{};
    std::uint32_t size{}, stride{};
    bool written{}, formatted{}, swizzled{}, add_tid{};
};

// The pinned program stores formatted state at resource 1 offset 0 (conditional SGPR10),
// and two DWORDs at globalId.x * 8 in resources 3 and 4. Loop iterations reuse that
// SAME destination. This proves an upper bound, not that all bytes are written.
// Reads may span the full descriptors: callers must retain their uploads/bindings/barriers.
inline std::optional<std::array<std::uint32_t, 5>> IfssSortWritePrefixes(
    const IfssSortDispatch& dispatch, const std::array<IfssSortBuffer, 5>& buffers) {
    if (!dispatch.groups[0] || dispatch.groups[1] != 1 || dispatch.groups[2] != 1 ||
        dispatch.threads != std::array<std::uint32_t, 3>{64, 1, 1} ||
        dispatch.start != std::array<std::uint32_t, 3>{} || dispatch.user_regs != 14 ||
        dispatch.tgid_mask != 1 || dispatch.scratch) return std::nullopt;
    const std::uint64_t array_bytes = std::uint64_t{dispatch.groups[0]} * 64 * 8;
    if (array_bytes > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        const auto& b = buffers[i];
        if (!b.address || b.address % 4 || !b.size || b.address >= (1ULL << 40) ||
            b.size > (1ULL << 40) - b.address || (b.formatted && i != 1) ||
            b.swizzled || b.add_tid ||
            b.written != (i == 1 || i >= 3)) return std::nullopt;
        if (i < 3 ? b.size != 16 : (b.stride != 8 || array_bytes > b.size))
            return std::nullopt;
        for (std::size_t j = 0; j < i; ++j) {
            const auto& a = buffers[j];
            if (b.address < a.address + a.size && a.address < b.address + b.size)
                return std::nullopt;
        }
    }
    // SGPR10 is also the state descriptor's record count. Keep that descriptor's complete
    // write footprint rather than infer a separate scheduling flag or format width.
    return std::array<std::uint32_t, 5>{0, buffers[1].size, 0,
                                       static_cast<std::uint32_t>(array_bytes),
                                       static_cast<std::uint32_t>(array_bytes)};
}
} // namespace VideoCore
