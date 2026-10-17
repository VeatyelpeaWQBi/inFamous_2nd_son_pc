// SPDX-FileCopyrightText: Copyright 2026 inFamous_2nd_son_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace VideoCore {

/// Caller owns synchronization and the in-flight limit. FIFO within each class; a waiting
/// background job runs after at most eight foreground selections.
template <typename T>
class PriorityCompileQueue {
public:
    struct Entry {
        T value;
        bool foreground;
    };
    static constexpr std::size_t MaxForegroundBurst = 8;

    void Push(T value, bool foreground) {
        (foreground ? urgent : background).push_back(std::move(value));
    }
    [[nodiscard]] bool Empty() const noexcept {
        return urgent.empty() && background.empty();
    }
    std::optional<Entry> Pop() {
        if (Empty()) {
            return std::nullopt;
        }
        const bool foreground = !urgent.empty() &&
                                (background.empty() || burst < MaxForegroundBurst);
        auto& selected = foreground ? urgent : background;
        Entry entry{std::move(selected.front()), foreground};
        selected.pop_front();
        burst = foreground ? (burst < MaxForegroundBurst ? burst + 1 : burst) : 0;
        return entry;
    }

private:
    std::deque<T> urgent;
    std::deque<T> background;
    std::size_t burst{};
};

} // namespace VideoCore
