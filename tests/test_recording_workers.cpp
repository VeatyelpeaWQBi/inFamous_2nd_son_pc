// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <vector>
#include "common/recording_workers.h"

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

int main() {
    for (const auto count : {2u, 4u}) {
        std::array<std::atomic<unsigned>, 4> owners{};
        std::atomic<unsigned> running{}, peak{};
        std::promise<void> release;
        auto gate = release.get_future().share();
        std::vector<std::future<unsigned>> results;
        Common::RecordingWorkers<unsigned> workers{count};
        for (unsigned i = 0; i < count; ++i) {
            results.push_back(workers.Submit([&, value = std::make_unique<unsigned>(i)](auto owner) {
                Require(owners[owner].fetch_add(1) == 0, "one exclusive pool owner per worker");
                const auto active = running.fetch_add(1) + 1;
                auto prior = peak.load();
                while (prior < active && !peak.compare_exchange_weak(prior, active)) {}
                gate.wait();
                --running;
                --owners[owner];
                return *value;
            }));
        }
        // Wait for actual concurrency, bounded so a broken executor fails instead of hanging.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (peak.load() != count && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const bool concurrent = peak.load() == count;
        release.set_value();
        Require(concurrent, "all configured workers record concurrently");
        for (unsigned i = 0; i < count; ++i) Require(results[i].get() == i, "FIFO consumer retains task identity");
        auto failed = workers.Submit([](auto) -> unsigned { throw std::runtime_error("expected"); });
        bool propagated{};
        try { (void)failed.get(); } catch (const std::runtime_error&) { propagated = true; }
        Require(propagated, "worker errors reach the publisher");
        Require(workers.Submit([](auto) { return 7u; }).get() == 7, "executor survives task failure");
    }
    std::atomic<unsigned> drained{};
    std::vector<std::future<unsigned>> pending;
    {
        Common::RecordingWorkers<unsigned> workers{2};
        for (unsigned i = 0; i < 64; ++i) pending.push_back(workers.Submit([&](auto) { return ++drained; }));
    }
    Require(drained == 64, "destruction drains accepted streams before releasing state");
    for (auto& future : pending) (void)future.get();
    bool rejected{};
    try { Common::RecordingWorkers<unsigned> invalid{3}; } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "unsupported worker counts are rejected");
    std::puts("Recording workers concurrency, ownership, failure and drain tests passed.");
}
