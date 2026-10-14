// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

namespace VideoCore {

// A single compilation publishes its handle once. Consumers may prepare owned commands
// meanwhile, but must acquire this signal before reading/binding the compiled handle.
class CompileReadiness {
public:
    [[nodiscard]] bool IsReady() const noexcept {
        return ready.load(std::memory_order_acquire);
    }

    void Wait() const noexcept {
        while (!IsReady()) {
            ready.wait(false, std::memory_order_acquire);
        }
    }

    void Publish() noexcept {
        ready.store(true, std::memory_order_release);
        ready.notify_all();
    }

private:
    std::atomic<bool> ready{false};
};

} // namespace VideoCore
