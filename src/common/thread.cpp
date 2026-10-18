// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <functional>
#include <ctime>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/libraries/fiber/fiber.h"
#include "core/libraries/kernel/threads/pthread.h"

#include "common/error.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "ntapi.h"
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#elif defined(_WIN32)
#include "common/string_util.h"
#include <windows.h>
#else
#if defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#else
#include <pthread.h>
#endif
#include <sched.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __FreeBSD__
#define cpu_set_t cpuset_t
#endif

namespace Common {

#ifdef __APPLE__

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // CPU time to grant.
    const std::chrono::nanoseconds computation_ns = period_ns / 2;

    // Determine the timebase for converting time to ticks.
    struct mach_timebase_info timebase{};
    mach_timebase_info(&timebase);
    const auto ticks_per_ns =
        static_cast<double>(timebase.denom) / static_cast<double>(timebase.numer);

    const auto period_ticks =
        static_cast<u32>(static_cast<double>(period_ns.count()) * ticks_per_ns);
    const auto computation_ticks =
        static_cast<u32>(static_cast<double>(computation_ns.count()) * ticks_per_ns);

    thread_time_constraint_policy policy = {
        .period = period_ticks,
        .computation = computation_ticks,
        // Should not matter since preemptible is false, but needs to be >= computation regardless.
        .constraint = computation_ticks,
        .preemptible = false,
    };

    int ret = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (ret != KERN_SUCCESS) {
        LOG_ERROR(Common, "Could not set thread to real-time with period {} ns: {}",
                  period_ns.count(), ret);
    }
}

#else

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // Not implemented
}

#endif

#ifdef _WIN32

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    auto handle = GetCurrentThread();
    int windows_priority = 0;
    switch (new_priority) {
    case ThreadPriority::Low:
        windows_priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case ThreadPriority::Normal:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    case ThreadPriority::High:
        windows_priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case ThreadPriority::VeryHigh:
        windows_priority = THREAD_PRIORITY_HIGHEST;
        break;
    case ThreadPriority::Critical:
        windows_priority = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    }
    SetThreadPriority(handle, windows_priority);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();

    LARGE_INTEGER interval{
        .QuadPart = -1 * (duration.count() / 100u),
    };
    HANDLE timer = ::CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, &interval, 0, NULL, NULL, 0);
    const auto ret = WaitForSingleObjectEx(timer, INFINITE, interruptible);
    ::CloseHandle(timer);

    if (remaining) {
        const auto end_sleep = std::chrono::high_resolution_clock::now();
        const auto sleep_time = end_sleep - begin_sleep;
        *remaining = duration > sleep_time ? duration - sleep_time : std::chrono::nanoseconds(0);
    }
    return ret == WAIT_OBJECT_0;
}

#else

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    pthread_t this_thread = pthread_self();

    const auto scheduling_type = SCHED_OTHER;
    s32 max_prio = sched_get_priority_max(scheduling_type);
    s32 min_prio = sched_get_priority_min(scheduling_type);
    u32 level = std::max(static_cast<u32>(new_priority) + 1, 4U);

    struct sched_param params;
    if (max_prio > min_prio) {
        params.sched_priority = min_prio + ((max_prio - min_prio) * level) / 4;
    } else {
        params.sched_priority = min_prio - ((min_prio - max_prio) * level) / 4;
    }

    pthread_setschedparam(this_thread, scheduling_type, &params);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    timespec request = {
        .tv_sec = duration.count() / 1'000'000'000,
        .tv_nsec = duration.count() % 1'000'000'000,
    };
    timespec remain;
    int ret;
    while ((ret = nanosleep(&request, &remain)) < 0 && errno == EINTR) {
        if (interruptible) {
            break;
        }
        request = remain;
    }
    if (remaining) {
        *remaining = std::chrono::nanoseconds(remain.tv_sec * 1'000'000'000 + remain.tv_nsec);
    }
    return ret == 0 || errno != EINTR;
}

#endif

#ifdef _WIN32

// Sets the debugger-visible name of the current thread.
static u64 AutoGpuThreadsMask() {
    const auto read = [](LOGICAL_PROCESSOR_RELATIONSHIP relation) {
        std::vector<u8> buffer;
        DWORD length = 0;
        GetLogicalProcessorInformationEx(relation, nullptr, &length);
        if (length == 0) {
            return buffer;
        }
        buffer.resize(length);
        if (!GetLogicalProcessorInformationEx(
                relation, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()),
                &length)) {
            return std::vector<u8>{};
        }
        buffer.resize(length);
        return buffer;
    };
    const auto for_each = [](const std::vector<u8>& buffer, auto&& visit) {
        for (size_t offset = 0; offset + sizeof(DWORD) * 2 <= buffer.size();) {
            const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                buffer.data() + offset);
            if (entry->Size < sizeof(DWORD) * 2 || entry->Size > buffer.size() - offset) {
                break;
            }
            visit(*entry);
            offset += entry->Size;
        }
    };
    // L3 caches of processor group 0 (the emulator does not use more than 64 processors).
    std::vector<std::pair<u64, u64>> l3; // (size, mask)
    for_each(read(RelationCache), [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& entry) {
        if (entry.Cache.Level == 3 && entry.Cache.GroupMask.Group == 0) {
            l3.emplace_back(entry.Cache.CacheSize, static_cast<u64>(entry.Cache.GroupMask.Mask));
        }
    });
    if (l3.size() < 2) {
        return 0;
    }
    std::ranges::sort(l3, std::greater<>{});
    if (l3[0].first == l3[1].first) {
        return 0; // Same L3 on every die: nothing to choose.
    }
    const u64 domain = l3[0].second;
    std::vector<u64> core_first; // Lowest logical processor of each core of that L3.
    for_each(read(RelationProcessorCore),
             [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& entry) {
                 if (entry.Processor.GroupCount == 0 || entry.Processor.GroupMask[0].Group != 0) {
                     return;
                 }
                 const u64 core = static_cast<u64>(entry.Processor.GroupMask[0].Mask);
                 if (core != 0 && (core & domain) == core) {
                     core_first.push_back(core & (~core + 1));
                 }
             });
    std::ranges::sort(core_first);
    u64 mask = 0;
    for (size_t i = 1; i < core_first.size(); ++i) {
        mask |= core_first[i];
    };
    return mask;
}

void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());
    const std::string_view worker{name};
    if (!Libraries::Kernel::g_curthread &&
        (worker == "shadPS4:GpuCommandProcessor" || worker == "shadPS4:GpuCommandRecorder" ||
         worker == "shadPS4:Scout" || worker == "shadPS4:GpuUploadWorker")) {
        static const u64 selected = AutoGpuThreadsMask();
        DWORD_PTR allowed{}, system{};
        if (selected && GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) {
            const auto mask = static_cast<DWORD_PTR>(selected) & allowed;
            if (mask) {
                SetThreadAffinityMask(GetCurrentThread(), mask);
            }
        }
    }
}

void SetThreadName(void* thread, const char* name) {
    SetThreadDescription(thread, UTF8ToUTF16W(name).data());
}

#else // !_WIN32, so must be POSIX threads

// MinGW with the POSIX threading model does not support pthread_setname_np
#if !defined(_WIN32) || defined(_MSC_VER)
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
#ifdef __APPLE__
    pthread_setname_np(name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__)
    // Linux limits thread names to 15 characters and will outright reject any
    // attempt to set a longer name with ERANGE.
    std::string truncated(name, std::min(strlen(name), static_cast<std::size_t>(15)));
    if (int e = pthread_setname_np(pthread_self(), truncated.c_str())) {
        errno = e;
        LOG_ERROR(Common, "Failed to set thread name to '{}': {}", truncated, GetLastErrorMsg());
    }
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

void SetThreadName(void* thread, const char* name) {
    // TODO
}
#endif

#if defined(_WIN32)
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    // Do Nothing on MinGW
}

void SetThreadName(void* thread, const char* name) {
    // Do Nothing on MinGW
}
#endif

#endif

AccurateTimer::AccurateTimer(std::chrono::nanoseconds target_interval)
    : target_interval(target_interval) {}

void AccurateTimer::Start() {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();
    if (total_wait.count() > 0) {
        AccurateSleep(total_wait, nullptr, false);
    }
    start_time = std::chrono::high_resolution_clock::now();
    total_wait -= std::chrono::duration_cast<std::chrono::nanoseconds>(start_time - begin_sleep);
}

void AccurateTimer::End() {
    auto now = std::chrono::high_resolution_clock::now();
    total_wait +=
        target_interval - std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time);
}

std::string GetCurrentThreadName() {
    using namespace Libraries::Kernel;
    if (g_curthread && !g_curthread->name.empty()) {
        if (g_curthread->tcb->tcb_fiber) {
            return fmt::format("{}@@{}", g_curthread->name,
                               g_curthread->tcb->tcb_fiber->current_fiber->name);
        }
        return g_curthread->name;
    }
#ifdef _WIN32
    PWSTR name{};
    if (FAILED(GetThreadDescription(GetCurrentThread(), &name)) || name == nullptr) {
        return "<unknown name>";
    }
    const auto result = Common::UTF16ToUTF8(name);
    LocalFree(name);
    return result;
#else
    char name[256];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        return "<unknown name>";
    }
    return std::string{name};
#endif
}

} // namespace Common
