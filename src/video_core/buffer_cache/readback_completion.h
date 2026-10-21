// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace VideoCore {
// One exact read request, owned jointly by its waiting guest and the CP watcher.
// Only the CP publishes, after normal copy/version/protection retirement. Later
// GPU writes still protect the pages and cause the original guest load to fault.
struct ReadbackCompletion {
    std::uint64_t address, size, trace_id;
    std::atomic<bool> published{false};

    ReadbackCompletion(std::uint64_t address_, std::uint64_t size_, std::uint64_t trace_id_)
        : address{address_}, size{size_}, trace_id{trace_id_} {}
    bool Ready() const noexcept { return published.load(std::memory_order_acquire); }
    void Publish() noexcept { published.store(true, std::memory_order_release); }
};
} // namespace VideoCore
