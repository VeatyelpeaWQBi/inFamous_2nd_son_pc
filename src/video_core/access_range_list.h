// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/interval_set.h"

namespace VideoCore {

// Buffer hazards carry no payload to overwrite: their exact operation is set union.
// Keep the general interval implementation available for an isolated comparison.
class AccessRangeList : public IntervalList<Interval> {
public:
    void AddUnion(Interval value) {
        if (value.start >= value.end) return;
        if (intervals.empty() || intervals.back().end < value.start) {
            intervals.push_back(value);
            return;
        }
        // Repeated/ascending bindings do not require binary search or a splice.
        auto& back = intervals.back();
        if (back.start <= value.start) {
            back.end = std::max(back.end, value.end);
            return;
        }
        auto first = std::ranges::lower_bound(intervals, value.start, {}, &Interval::end);
        auto last = std::ranges::upper_bound(first, intervals.end(), value.end, {}, &Interval::start);
        if (first == last) {
            intervals.insert(first, value);
            return;
        }
        const Interval merged{std::min(first->start, value.start),
                              std::max(std::prev(last)->end, value.end)};
        *first = merged;
        intervals.erase(std::next(first), last);
    }
};

} // namespace VideoCore
