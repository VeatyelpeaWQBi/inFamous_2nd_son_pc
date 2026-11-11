// SPDX-FileCopyrightText: Copyright 2026 inFamous_2nd_son_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace VideoCore {

inline bool IsFeedbackWordPrefetchProducer(std::uint64_t shader, std::uint64_t size,
                                          bool page_cluster) {
    return size == 48 && (shader == 0xa4edc92b || (page_cluster && shader == 0x19100b24));
}

struct FeedbackWordWindow {
    std::uint64_t start{}, end{};
};

inline bool IsFeedbackWindowWord(std::uint64_t address,
                                const std::array<FeedbackWordWindow, 2>& windows) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (!address || (address & 3) || address > max - 4) return false;
    for (const auto& window : windows) {
        // An eligibility bound only: this never marks any byte clean or authorizes a copy.
        if (window.end > window.start && window.end - window.start <= 512 * 1024 &&
            address >= window.start && address + 4 <= window.end) return true;
    }
    return false;
}

inline bool IsFeedbackProducerSeed(std::uint64_t address, std::uint64_t size,
                                  std::uint64_t shader,
                                  const std::array<FeedbackWordWindow, 2>& windows) {
    return shader != 0 && size == 48 && address <= UINT64_MAX - size &&
           IsFeedbackWindowWord(address + 4, windows);
}

inline bool IsExactFeedbackMov32(const std::array<std::uint8_t, 3>& instruction,
                                std::uint64_t rax, std::uint64_t fault) {
    return instruction == std::array<std::uint8_t, 3>{0x8b, 0x40, 0x04} &&
           rax <= std::numeric_limits<std::uint64_t>::max() - 4 && rax + 4 == fault &&
           (fault & 3) == 0;
}

// A scheduling hint, never evidence that bytes are clean or that a shader only writes here.
// A declared small write must contain an actual, recently observed small CPU read.
inline bool IsFeedbackCheckpointWrite(std::uint64_t write, std::uint64_t size,
                                      std::uint64_t read, std::uint64_t read_size) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    return size != 0 && size <= 64 && read_size != 0 && read_size <= 8 &&
           size <= max - write && read_size <= max - read && read >= write &&
           read_size <= size && read - write <= size - read_size;
}

// CP-thread-only selective page provenance. Coarse tracking remains authoritative elsewhere.
// Learning seeds a page with ALL conservative dependencies. Every subsequent declared buffer
// write must call NoteWrite, even when no shader actually writes a particular invocation.
// A missing page, overflow or capacity exhaustion never authorizes a narrowed dependency.
class FeedbackPageWriters {
public:
    using Ticks = std::array<std::uint64_t, 2>;
    static constexpr std::uint64_t PageSize = 4096;
    static constexpr std::size_t Capacity = 64;

    void Clear() noexcept {
        count = 0;
    }

    bool Learn(std::uint64_t address, const Ticks& conservative) {
        if (conservative == Ticks{}) {
            return false;
        }
        const auto page = address / PageSize;
        const auto pos = Find(page);
        if (pos != count && entries[pos].page == page) {
            return true; // Do not re-seed from a newer, unrelated coarse writer.
        }
        if (count == Capacity) {
            return false;
        }
        std::move_backward(entries.begin() + pos, entries.begin() + count,
                           entries.begin() + count + 1);
        entries[pos] = Entry{page, conservative};
        ++count;
        return true;
    }

    void NoteWrite(std::uint64_t address, std::uint64_t size, std::uint64_t tick,
                   std::uint32_t queue) {
        if (!ValidRange(address, size) || tick == 0 || queue >= 2) {
            Clear(); // Unknown writes invalidate the proof rather than retaining stale pages.
            return;
        }
        const auto last = (address + size - 1) / PageSize;
        for (auto pos = Find(address / PageSize); pos < count && entries[pos].page <= last;
             ++pos) {
            auto& old = entries[pos].ticks[queue];
            old = std::max(old, tick); // Retain the other queue's outstanding dependency too.
        }
    }

    std::optional<Ticks> Query(std::uint64_t address, std::uint64_t size) const {
        if (!ValidRange(address, size)) {
            return std::nullopt;
        }
        auto page = address / PageSize;
        const auto last = (address + size - 1) / PageSize;
        if (last - page >= Capacity) {
            return std::nullopt;
        }
        auto pos = Find(page);
        Ticks ticks{};
        for (;;) {
            if (pos == count || entries[pos].page != page) {
                return std::nullopt;
            }
            for (unsigned q = 0; q < 2; ++q) {
                ticks[q] = std::max(ticks[q], entries[pos].ticks[q]);
            }
            if (page == last) {
                return ticks;
            }
            ++page;
            ++pos;
        }
    }

private:
    struct Entry {
        std::uint64_t page{};
        Ticks ticks{};
    };
    static bool ValidRange(std::uint64_t address, std::uint64_t size) {
        return size != 0 && size <= std::numeric_limits<std::uint64_t>::max() - address;
    }
    std::size_t Find(std::uint64_t page) const {
        return static_cast<std::size_t>(
            std::lower_bound(entries.begin(), entries.begin() + count, page,
                             [](const Entry& entry, auto value) { return entry.page < value; }) -
            entries.begin());
    }
    std::array<Entry, Capacity> entries{};
    std::size_t count{};
};

// Exact declaration-overlap provenance for private four-byte reads. Unlike page provenance,
// a neighbouring arena ending before the word cannot advance this word's dependency.
// Versions distinguish repeated writes within the same scheduler tick.
class FeedbackWordWriters {
public:
    static constexpr std::size_t Capacity = 1024;
    struct Token {
        std::uint64_t tick{}, version{};
        bool operator==(const Token&) const = default;
    };
    void Clear() noexcept { count = 0; speculative_count = 0; }
    bool Learn(std::uint64_t address, std::uint64_t conservative, std::size_t limit = 64,
               bool observed = true) {
        if ((address & 3) || address > UINT64_MAX - 4 || !conservative) return false;
        const auto pos = Find(address);
        if (pos < count && entries[pos].address == address) {
            if (observed && !entries[pos].observed) {
                entries[pos].observed = true;
                --speculative_count;
            }
            return true;
        }
        if (count >= std::min(limit, entries.size())) return false;
        if (!observed && speculative_count >= 64) return false;
        std::move_backward(entries.begin() + pos, entries.begin() + count,
                           entries.begin() + count + 1);
        entries[pos] = {address, {conservative, ++serial}, observed};
        ++count;
        if (!observed) ++speculative_count;
        return true;
    }
    void RetainInWindows(const std::array<FeedbackWordWindow, 2>& windows) {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < count; ++i)
            if (IsFeedbackWindowWord(entries[i].address, windows)) entries[kept++] = entries[i];
        count = kept; // Preserve kept versions; a removed/relearned word gets a new serial.
        speculative_count = 0;
        for (std::size_t i = 0; i < count; ++i) speculative_count += !entries[i].observed;
    }
    void NoteWrite(std::uint64_t address, std::uint64_t size, std::uint64_t tick) {
        if (!size || size > UINT64_MAX - address || !tick) { Clear(); return; }
        for (auto i = Find(address > 3 ? address - 3 : 0);
             i < count && entries[i].address < address + size; ++i) {
            auto& e = entries[i];
            if (address < e.address + 4) {
                e.token.tick = std::max(e.token.tick, tick);
                e.token.version = ++serial;
            }
        }
    }
    template <typename Visitor>
    void VisitOverlaps(std::uint64_t address, std::uint64_t size, Visitor&& visit) const {
        if (!size || size > UINT64_MAX - address) return;
        // A write can begin in the last three bytes of an earlier aligned word.
        for (auto i = Find(address > 3 ? address - 3 : 0);
             i < count && entries[i].address < address + size; ++i) {
            if (address < entries[i].address + 4) visit(entries[i].address);
        }
    }
    std::optional<Token> Query(std::uint64_t address) const {
        const auto pos = Find(address);
        if (pos < count && entries[pos].address == address) return entries[pos].token;
        return std::nullopt;
    }
private:
    struct Entry { std::uint64_t address; Token token; bool observed; };
    std::size_t Find(std::uint64_t address) const {
        return static_cast<std::size_t>(
            std::lower_bound(entries.begin(), entries.begin() + count, address,
                             [](const Entry& entry, auto value) { return entry.address < value; }) -
            entries.begin());
    }
    std::array<Entry, Capacity> entries{};
    std::size_t count{};
    std::size_t speculative_count{};
    std::uint64_t serial{};
};
} // namespace VideoCore
