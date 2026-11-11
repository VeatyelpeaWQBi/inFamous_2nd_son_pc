// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

namespace VideoCore {
// Owned by one command's stack. A fresh resource-table epoch invalidates every entry,
// including when an Info address is reused. Null descriptors are valid cached values.
template <typename Sharp, std::size_t Capacity>
class CommandBufferSharps {
public:
    CommandBufferSharps() noexcept {}
    void Begin(const void* source, std::uint64_t epoch, std::size_t count) {
        owner = source;
        generation = epoch;
        size = count;
        present.reset();
        complete = false;
    }
    void Store(std::size_t index, const Sharp& sharp) {
        if (index < size && index < Capacity) {
            values[index] = sharp;
            present.set(index);
        }
    }
    void Publish() {
        complete = owner != nullptr && size <= Capacity;
    }
    const Sharp* Find(const void* source, std::uint64_t epoch, std::size_t count,
                      std::size_t index) const {
        if (!complete || source != owner || epoch != generation || count != size || index >= size ||
            index >= Capacity || !present[index]) {
            return nullptr;
        }
        return &values[index];
    }

private:
    // Uninitialized until Store; Find never reads an absent entry.
    std::array<Sharp, Capacity> values;
    std::bitset<Capacity> present{};
    const void* owner{};
    std::uint64_t generation{};
    std::size_t size{};
    bool complete{};
};
} // namespace VideoCore
