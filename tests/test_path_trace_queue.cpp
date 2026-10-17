// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include "common/path_trace_queue.h"

namespace {
struct Record { std::uint64_t sequence{}, payload{}, inverse{}; };
Record Make(std::uint64_t sequence) {
    return {sequence, sequence * 0x9e3779b97f4a7c15ULL, ~sequence};
}
void Require(bool ok) {
    if (!ok) throw std::runtime_error("path trace queue regression failed");
}
void Same(const Record& actual, std::uint64_t sequence) {
    const auto expected = Make(sequence);
    Require(actual.sequence == expected.sequence && actual.payload == expected.payload &&
            actual.inverse == expected.inverse);
}
}
int main() {
    Common::PathTrace::SpscRing<Record, 4> ring;
    Record result;
    Require(!ring.Pop(result));
    for (std::uint64_t i = 0; i < 4; ++i) Require(ring.Push(Make(i)));
    Require(!ring.Push(Make(99))); // Overflow must not overwrite an unpublished record.
    for (std::uint64_t i = 0; i < 2; ++i) { Require(ring.Pop(result)); Same(result, i); }
    for (std::uint64_t i = 4; i < 6; ++i) Require(ring.Push(Make(i)));
    for (std::uint64_t i = 2; i < 6; ++i) { Require(ring.Pop(result)); Same(result, i); }
    Require(!ring.Pop(result));

    Common::PathTrace::SpscRing<Record, 64> concurrent;
    constexpr std::uint64_t Count = 100'000;
    std::atomic<bool> failed{false};
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < Count; ++i) {
            while (!concurrent.Push(Make(i))) std::this_thread::yield();
        }
    });
    for (std::uint64_t i = 0; i < Count; ++i) {
        while (!concurrent.Pop(result)) std::this_thread::yield();
        const auto expected = Make(i);
        if (result.sequence != i || result.payload != expected.payload || result.inverse != ~i)
            failed.store(true, std::memory_order_relaxed);
    }
    producer.join();
    Require(!failed.load() && !concurrent.Pop(result));
}
