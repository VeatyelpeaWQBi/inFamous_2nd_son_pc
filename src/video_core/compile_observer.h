// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include "common/types.h"

namespace VideoCore {
// Rare compilation boundaries only. Independent of per-draw counters/CPU sampling,
// so a lightweight workload can prove it stayed warm without enabling either.
struct CompileObserver {
    std::atomic<u64> shaders{}, pipelines{}, active{};
    static CompileObserver& Get() {
        static CompileObserver observer;
        return observer;
    }
};
class ObserveCompile {
public:
    explicit ObserveCompile(bool shader) {
        auto& observer = CompileObserver::Get();
        observer.active.fetch_add(1, std::memory_order_acq_rel);
        (shader ? observer.shaders : observer.pipelines).fetch_add(1, std::memory_order_release);
    }
    ~ObserveCompile() {
        CompileObserver::Get().active.fetch_sub(1, std::memory_order_release);
    }
    ObserveCompile(const ObserveCompile&) = delete;
    ObserveCompile& operator=(const ObserveCompile&) = delete;
};
} // namespace VideoCore
