// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace VideoCore {

// Command-processor-only provenance cache. Seeds include all conservative dependencies that
// precede registration; EVERY subsequent GPU write must be observed, including wide stores.
// Eviction loses precision, never dependencies. This does not mark bytes clean or infer a
// shader's actual store footprint from a descriptor.
class DemandPageWriters {
public:
    static constexpr std::uint64_t PageSize = 4096;
    static constexpr std::size_t Capacity = 128;
    using Ticks = std::array<std::uint64_t, 2>;

    void Clear() noexcept { entries = {}; used = 0; }

    void Watch(std::uint64_t page, Ticks seed) noexcept {
        if (page % PageSize != 0 || page > Max - PageSize) return;
        auto& entry = entries[Index(page)];
        if (entry.valid && entry.page == page) return;
        if (!entry.valid) ++used;
        entry = Entry{page, seed, true};
    }

    void NoteWrite(std::uint64_t address, std::uint64_t size, std::uint32_t queue,
                   std::uint64_t tick) noexcept {
        if (used == 0 || size == 0) return;
        if (queue >= 2 || tick == 0 || size > Max - address) {
            entries = {};
            used = 0;
            return;
        }
        const auto end = address + size;
        const auto first = address & ~(PageSize - 1);
        const auto last = (end - 1) & ~(PageSize - 1);
        if ((last - first) / PageSize < Capacity) {
            // Small feedback buffers normally touch one page: constant-time lookup.
            for (auto page = first;; page += PageSize) {
                auto& entry = entries[Index(page)];
                if (entry.valid && entry.page == page) {
                    entry.ticks[queue] = std::max(entry.ticks[queue], tick);
                }
                if (page == last) break;
            }
        } else {
            // A wide descriptor may touch every tracked page, never guess its store indices.
            for (auto& entry : entries) {
                if (entry.valid && address < entry.page + PageSize && entry.page < end) {
                    entry.ticks[queue] = std::max(entry.ticks[queue], tick);
                }
            }
        }
    }

    std::optional<Ticks> Query(std::uint64_t address, std::uint64_t size) const noexcept {
        if (size == 0 || size > Max - address || size > Capacity * PageSize) return {};
        Ticks ticks{};
        const auto end = address + size;
        for (auto page = address & ~(PageSize - 1); page < end;) {
            const auto& found = entries[Index(page)];
            if (!found.valid || found.page != page) return {};
            for (unsigned q = 0; q < 2; ++q) ticks[q] = std::max(ticks[q], found.ticks[q]);
            if (page > Max - PageSize) return {};
            page += PageSize;
        }
        return ticks;
    }

private:
    static constexpr auto Max = std::numeric_limits<std::uint64_t>::max();
    struct Entry { std::uint64_t page{}; Ticks ticks{}; bool valid{}; };
    std::array<Entry, Capacity> entries{};
    std::size_t used{};
    static std::size_t Index(std::uint64_t page) noexcept {
        return ((page / PageSize) * 11400714819323198485ULL) >> (64 - 7);
    }
};

struct DemandRange {
    std::uint64_t start{}, end{}, arena{};
    std::uint32_t source{};
};
struct DemandBatch {
    std::uint64_t start{}, end{}, arena{}, sources{};
};
struct DemandBatchPlan {
    std::array<DemandBatch, 64> batches{};
    std::size_t count{};
};

// Coalesce BEFORE deferred staging allocation, unlike the retired V3 copy-command proposal.
// No gaps, no cross-arena merging, no extra bytes beyond observed page hints, bounded span.
inline DemandBatchPlan PlanDemandBatches(std::span<const DemandRange> input,
                                        std::uint64_t max_span = 65536) {
    DemandBatchPlan plan;
    if (input.size() > 64) return plan;
    std::array<DemandRange, 64> ranges{};
    std::copy(input.begin(), input.end(), ranges.begin());
    std::sort(ranges.begin(), ranges.begin() + input.size(), [](const auto& a, const auto& b) {
        return a.arena < b.arena || (a.arena == b.arena && a.start < b.start);
    });
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto& range = ranges[i];
        if (range.source >= 64 || range.start >= range.end ||
            range.end - range.start > max_span) continue;
        auto* last = plan.count ? &plan.batches[plan.count - 1] : nullptr;
        if (last && last->arena == range.arena && range.start <= last->end &&
            std::max(last->end, range.end) - last->start <= max_span) {
            last->end = std::max(last->end, range.end);
            last->sources |= std::uint64_t{1} << range.source;
        } else {
            plan.batches[plan.count++] =
                {range.start, range.end, range.arena, std::uint64_t{1} << range.source};
        }
    }
    return plan;
}
} // namespace VideoCore
