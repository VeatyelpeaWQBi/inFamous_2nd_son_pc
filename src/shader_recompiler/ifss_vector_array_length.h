// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace Shader::Optimization {
struct IfssArrayLength {
    std::uint32_t result, type, zero, buffer;
};
// The installed Sirit has no OpArrayLength entry point. Its project-owned emitter
// reserves a unique IAdd(0,0) result at the exact function position, then replaces
// only those recorded IDs. Both instructions have five words. No vendor edit.
inline bool FinalizeIfssArrayLengths(std::vector<std::uint32_t>& words,
                                    std::span<const IfssArrayLength> requests) {
    if (requests.empty()) return true;
    if (words.size() < 5 || words[0] != 0x07230203) return false;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& r = requests[i];
        if (!r.result || r.result >= words[3] || !r.type || r.type >= words[3] ||
            !r.zero || r.zero >= words[3]) return false;
        for (std::size_t j = 0; j < i; ++j) if (requests[j].result == r.result) return false;
    }
    std::vector<std::size_t> positions(requests.size(), 0);
    for (std::size_t at = 5; at < words.size();) {
        const auto count = words[at] >> 16;
        if (!count || count > words.size() - at) return false;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            const auto& r = requests[i];
            if ((words[at] & 65535u) == 128 && count >= 3 && words[at + 2] == r.result) {
                if (positions[i] || count != 5 || words[at + 1] != r.type ||
                    words[at + 3] != r.zero || words[at + 4] != r.zero ||
                    !r.buffer || r.buffer >= words[3]) return false;
                positions[i] = at;
            }
        }
        at += count;
    }
    for (auto at : positions) if (!at) return false;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto at = positions[i];
        words[at] = (5u << 16) | 68u; // OpArrayLength
        words[at + 3] = requests[i].buffer;
        words[at + 4] = 0; // literal member index, not an ID
    }
    return true;
}
}
