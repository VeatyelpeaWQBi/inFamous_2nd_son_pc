// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Common::PathTrace {
// Exactly one producer and one consumer. A full ring drops the new record; the producer
// never waits for the writer. Entries are published only after their entire value is copied.
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::is_trivially_copyable_v<T>);
public:
    bool Push(const T& value) noexcept {
        const auto tail = write.load(std::memory_order_relaxed);
        if (tail - read.load(std::memory_order_acquire) == Capacity) return false;
        entries[tail & (Capacity - 1)] = value;
        write.store(tail + 1, std::memory_order_release);
        return true;
    }
    bool Pop(T& value) noexcept {
        const auto head = read.load(std::memory_order_relaxed);
        if (head == write.load(std::memory_order_acquire)) return false;
        value = entries[head & (Capacity - 1)];
        read.store(head + 1, std::memory_order_release);
        return true;
    }
private:
    alignas(64) std::atomic<std::uint64_t> write{0};
    alignas(64) std::atomic<std::uint64_t> read{0};
    std::array<T, Capacity> entries{};
};
} // namespace Common::PathTrace
