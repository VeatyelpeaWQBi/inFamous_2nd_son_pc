// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace VideoCore {

// Conservative acceleration for the *middle*, complete 256 KiB words of a region.
// Callers still check partial endpoint words and CPU modification state. Mutations are
// serialized by the region lock. Clear a summary bit before clearing any page bit; publish
// a full word only after setting its last page. This never authorizes stale-data reuse.
class GpuFullWordSummary {
public:
    void BeforeClear(std::uint64_t index) noexcept {
        full.fetch_and(~(std::uint64_t{1} << index), std::memory_order_release);
    }

    void AfterFullSet(std::uint64_t index) noexcept {
        full.fetch_or(std::uint64_t{1} << index, std::memory_order_release);
    }

    [[nodiscard]] bool Contains(std::uint64_t first, std::uint64_t last) const noexcept {
        if (first > last) return true;
        const auto mask = (~std::uint64_t{0} >> (63 - (last - first))) << first;
        return (full.load(std::memory_order_acquire) & mask) == mask;
    }

private:
    std::atomic<std::uint64_t> full{};
};
} // namespace VideoCore
