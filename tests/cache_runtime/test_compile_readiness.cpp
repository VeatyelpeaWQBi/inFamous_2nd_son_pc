// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>
#include <vector>
#include "video_core/compile_readiness.h"

static void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

static void AlreadyCompiled() {
    VideoCore::CompileReadiness ready;
    Check(!ready.IsReady(), "a pending compile cannot expose a handle");
    ready.Publish();
    ready.Wait(); // Publication before registration of a waiter must not lose a wakeup.
    Check(ready.IsReady(), "a prewarmed handle is immediately usable");
}

static void PublicationAndMultipleConsumers() {
    constexpr size_t NumReaders = 16;
    VideoCore::CompileReadiness ready;
    std::array<std::uint64_t, 128> driver_result{}; // Published via ready, not atomic itself.
    std::array<bool, NumReaders> observed{};
    std::atomic<size_t> entered{};
    std::vector<std::jthread> consumers;
    for (size_t reader = 0; reader < NumReaders; ++reader) {
        consumers.emplace_back([&, reader] {
            entered.fetch_add(1, std::memory_order_release);
            entered.notify_all();
            ready.Wait();
            bool complete = true;
            for (size_t i = 0; i < driver_result.size(); ++i) {
                complete &= driver_result[i] == 0x12340000ULL + i;
            }
            observed[reader] = complete;
        });
    }
    for (size_t count = entered.load(std::memory_order_acquire); count != NumReaders;
         count = entered.load(std::memory_order_acquire)) {
        entered.wait(count, std::memory_order_acquire);
    }
    Check(!ready.IsReady(), "waiting consumers cannot publish compilation themselves");
    for (size_t i = 0; i < driver_result.size(); ++i) {
        driver_result[i] = 0x12340000ULL + i;
    }
    ready.Publish();
    consumers.clear(); // Every consumer finishes before the compiled result's owner dies.
    for (const bool complete : observed) {
        Check(complete, "every consumer acquires the entire compiled result");
    }
}

static void OutOfOrderCompilationKeepsCommandOrder() {
    std::array<VideoCore::CompileReadiness, 3> ready;
    std::array<std::uint32_t, 3> handles{};
    std::array<std::uint32_t, 3> submitted{};
    std::atomic<bool> entered{};
    std::jthread recorder([&] {
        entered.store(true, std::memory_order_release);
        entered.notify_all();
        for (size_t command = 0; command < ready.size(); ++command) {
            ready[command].Wait();
            submitted[command] = handles[command];
        }
    });
    entered.wait(false, std::memory_order_acquire);
    // Compiler workers finish later commands first. The recorder must keep all commands
    // and wait for their dependencies instead of skipping the pending first effect.
    handles[2] = 30;
    ready[2].Publish();
    handles[1] = 20;
    ready[1].Publish();
    Check(!ready[0].IsReady(), "later compilations cannot satisfy an earlier dependency");
    handles[0] = 10;
    ready[0].Publish();
    recorder.join();
    Check(submitted == std::array<std::uint32_t, 3>{10, 20, 30},
          "ordered consumers keep every effect after out-of-order compilation");
}

int main() {
    static_assert(!std::is_copy_constructible_v<VideoCore::CompileReadiness>);
    AlreadyCompiled();
    PublicationAndMultipleConsumers();
    OutOfOrderCompilationKeepsCommandOrder();
    std::cout << "Compile readiness checks passed\n";
}
