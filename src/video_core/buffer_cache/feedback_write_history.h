// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace VideoCore {
// Command-processor-only diagnostic history. These are DECLARED descriptor ranges, not
// proven shader store footprints. Never consulted by synchronization or readback policy.
template <std::size_t Capacity = 4096>
class FeedbackWriteHistory {
public:
    static_assert(Capacity > 0);
    struct Write {
        std::uint64_t address{}, size{}, tick{}, sequence{}, command{}, shader{};
        std::uint64_t declared_ns{}, recorded_ns{};
        std::uint32_t queue{}, kind{};
    };

    std::uint64_t Cursor() const noexcept { return count; }
    std::uint64_t Overwritten() const noexcept { return count > Capacity ? count - Capacity : 0; }
    std::uint64_t OldestSequence() const noexcept {
        return count ? records[(count - std::min<std::uint64_t>(count, Capacity)) % Capacity].sequence : 0;
    }
    void InvalidateCoverage() noexcept { uncertain = true; }
    bool Uncertain() const noexcept { return uncertain; }
    void Record(const Write& write) noexcept {
        if (!Valid(write.address, write.size) || write.queue > 1) {
            uncertain = true;
            return;
        }
        records[count++ % Capacity] = write;
    }
    void Commit(std::uint64_t command, std::uint64_t cursor, std::uint64_t ns) noexcept {
        if (!command) return;
        const auto begin = std::max(cursor, count - std::min<std::uint64_t>(count, Capacity));
        for (auto i = begin; i < count; ++i) {
            auto& w = records[i % Capacity];
            if (w.command == command) w.recorded_ns = ns;
        }
    }
    template <typename Visitor>
    void VisitLatest(std::uint64_t request, std::uint64_t size,
                     std::uint64_t window, std::uint64_t span, Visitor&& visitor) const {
        if (!Valid(request, size) || !Valid(window, span)) return;
        // One most-recent declared overlap for each queue and each requested/window range.
        std::array<std::uint64_t, 4> found;
        found.fill(Max);
        const auto retained = std::min<std::uint64_t>(count, Capacity);
        for (auto n = retained; n; --n) {
            const auto cursor = count - retained + n - 1;
            const auto& w = records[cursor % Capacity];
            if (Overlap(w.address, w.size, request, size) && found[w.queue * 2] == Max)
                found[w.queue * 2] = cursor;
            if (Overlap(w.address, w.size, window, span) && found[w.queue * 2 + 1] == Max)
                found[w.queue * 2 + 1] = cursor;
        }
        for (std::size_t i = 0; i < found.size(); ++i) {
            if (found[i] == Max) continue;
            bool duplicate = false;
            for (std::size_t j = 0; j < i; ++j) duplicate |= found[j] == found[i];
            if (duplicate) continue;
            std::uint32_t scopes = 0;
            for (std::size_t j = i; j < found.size(); ++j) {
                if (found[j] == found[i]) scopes |= j % 2 == 0 ? 1U : 2U;
            }
            visitor(records[found[i] % Capacity], scopes);
        }
    }

private:
    static constexpr auto Max = std::numeric_limits<std::uint64_t>::max();
    static bool Valid(std::uint64_t a, std::uint64_t n) noexcept { return n && n <= Max - a; }
    static bool Overlap(std::uint64_t a, std::uint64_t n, std::uint64_t b, std::uint64_t m) noexcept {
        return a < b + m && b < a + n;
    }
    std::array<Write, Capacity> records{};
    std::uint64_t count{};
    bool uncertain{};
};
} // namespace VideoCore
