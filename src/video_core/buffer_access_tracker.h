// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <span>
#include <vector>
#include "common/interval_set.h"

namespace VideoCore {

// A dependency scope owns the actual ranges, never just a buffer identity. Recycle their
// storage at the next barrier, without letting a previous scope affect the next one's hazards.
template <typename Handle>
class BufferAccessTracker {
public:
    struct Access {
        Handle handle;
        IntervalList<Interval> read_ranges;
        IntervalList<Interval> write_ranges;
    };

    Access& Get(Handle handle) {
        if (current && current->handle == handle) return *current;
        const auto active = std::span{entries}.first(count);
        const auto found = std::ranges::find(active, handle, &Access::handle);
        if (found != active.end()) {
            current = &*found;
        } else {
            if (count == entries.size()) entries.emplace_back(handle);
            else entries[count].handle = handle;
            current = &entries[count++];
        }
        return *current;
    }

    void ResetScope() {
        for (auto& entry : std::span{entries}.first(count)) {
            entry.read_ranges.Clear();
            entry.write_ranges.Clear();
        }
        count = 0;
        current = nullptr;
        // A pathological dependency scope may mention far more resources than a draw or a
        // particle update. Retain ordinary high-water storage, not an unlimited history.
        if (entries.size() > 256) {
            entries.clear();
        }
    }

private:
    std::vector<Access> entries;
    size_t count{};
    Access* current{};
};
} // namespace VideoCore
