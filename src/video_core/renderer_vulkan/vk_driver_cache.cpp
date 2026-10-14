// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_driver_cache.h"
#include "video_core/renderer_vulkan/vk_driver_cache_format.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/perf_stats.h"
#include "common/thread.h"
#include "video_core/cache_file.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

namespace Vulkan {
namespace {
constexpr size_t Prefix = DriverCachePrefix;
bool Compatible(std::span<const u8> data, const Instance& instance) {
    const auto uuid = instance.GetPipelineCacheUUID();
    return DriverCacheCompatible(data, instance.GetVendorID(), instance.GetDeviceID(),
                                 instance.GetDriverVersion(),
                                 std::span<const u8, 16>{uuid.data(), 16});
}
} // namespace

u64 AvailableHostMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory))
        return memory.ullAvailPhys;
#elif defined(__linux__)
    struct sysinfo memory{};
    if (sysinfo(&memory) == 0)
        return (u64(memory.freeram) + memory.bufferram) * memory.mem_unit;
#endif
    return 2_GB; // Conservative fallback; no assumption of a large host on other platforms.
}

u64 AvailableDeviceMemory(const Instance& instance) {
    if (!instance.CanReportMemoryUsage())
        return instance.GetTotalMemoryBudget();
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    vk::PhysicalDeviceMemoryProperties2 props{.pNext = &budget};
    instance.GetPhysicalDevice().getMemoryProperties2(&props);
    u64 available{};
    for (u32 i = 0; i < props.memoryProperties.memoryHeapCount; ++i) {
        if (props.memoryProperties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
            available += budget.heapBudget[i] > budget.heapUsage[i]
                             ? budget.heapBudget[i] - budget.heapUsage[i]
                             : 0;
        }
    }
    return available;
}

DriverPipelineCache::DriverPipelineCache(const Instance& instance_, std::filesystem::path path_,
                                         u64 limit_)
    : instance{instance_}, path{std::move(path_)}, limit{std::min<u64>(limit_, 1_GB)} {
    std::vector<u8> initial;
    if (!path.empty() && Storage::CacheFile::Read(path, initial, limit) &&
        !Compatible(initial, instance))
        initial.clear();
    vk::PipelineCacheCreateInfo
        info{}; // Internally synchronized by Vulkan, no EXTERNALLY_SYNC flag.
    if (!initial.empty()) {
        info.initialDataSize = initial.size() - Prefix;
        info.pInitialData = initial.data() + Prefix;
    }
    auto [result, created] = instance.GetDevice().createPipelineCacheUnique(info);
    if (result != vk::Result::eSuccess && !initial.empty()) {
        initial.clear();
        auto fallback = instance.GetDevice().createPipelineCacheUnique({});
        result = fallback.result;
        created = std::move(fallback.value);
    }
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create driver pipeline cache: {}",
               vk::to_string(result));
    cache = std::move(created);
    LOG_INFO(Render, "Driver cache restored: {} bytes, save limit {} MiB", initial.size(),
             limit / 1_MB);
}

DriverPipelineCache::~DriverPipelineCache() {
    Stop();
}
DriverPipelineCache::Lease DriverPipelineCache::Acquire() {
    if (closing.load(std::memory_order_acquire))
        return {{}, vk::PipelineCache{}};
    std::shared_lock lock{gate, std::try_to_lock};
    if (lock.owns_lock() && closing.load(std::memory_order_acquire))
        lock.unlock();
    const auto handle = lock.owns_lock() ? *cache : vk::PipelineCache{};
    return {std::move(lock), handle};
}
void DriverPipelineCache::Compiled() {
    last_compile_ns.store(Common::Perf::NowNs(), std::memory_order_relaxed);
    revision.fetch_add(1, std::memory_order_release);
}
void DriverPipelineCache::Start() {
    closing.store(false, std::memory_order_release);
    if (path.empty() || limit < 1_MB || writer.joinable())
        return;
    writer = std::jthread{[this](std::stop_token stop) {
        Common::SetCurrentThreadName("shadPS4:DriverCacheIO");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        std::unique_lock lock{wait_mutex};
        while (!stop.stop_requested()) {
            wake.wait_for(lock, stop, std::chrono::seconds{15}, [] { return false; });
            if (stop.stop_requested())
                break;
            lock.unlock();
            try {
                Save();
            } catch (const std::exception& error) {
                LOG_WARNING(Render, "Background driver cache save failed: {}", error.what());
            }
            lock.lock();
        }
    }};
}
void DriverPipelineCache::Stop() {
    closing.store(true, std::memory_order_release);
    if (writer.joinable()) {
        writer.request_stop();
        wake.notify_all();
        writer.join();
    }
    try {
        if (!Save(true))
            LOG_WARNING(Render, "Driver cache final snapshot could not be saved");
    } catch (const std::exception& error) {
        LOG_WARNING(Render, "Driver cache save failed: {}", error.what());
    }
}
bool DriverPipelineCache::Save(bool final) {
    std::lock_guard saving{save_mutex};
    u64 current = revision.load(std::memory_order_acquire);
    if (path.empty() || (!final && current == saved_revision) || limit < Prefix + 32)
        return true;
    if (!final &&
        Common::Perf::NowNs() - last_compile_ns.load(std::memory_order_relaxed) < 2'000'000'000ULL)
        return false;
    std::vector<u8> bytes;
    const auto start = Common::Perf::NowNs();
    {
        std::unique_lock lock{gate, std::defer_lock};
        if (final)
            lock.lock(); // Shutdown only: no new cache leases can enter.
        else if (!lock.try_lock())
            return false;
        current = revision.load(std::memory_order_acquire);
        if (current == saved_revision)
            return true;
        size_t size{};
        if (instance.GetDevice().getPipelineCacheData(*cache, &size, nullptr) !=
                vk::Result::eSuccess ||
            size < 32 || size > limit - Prefix || size > AvailableHostMemory() / 4)
            return false;
        bytes.resize(Prefix + size);
        std::memcpy(bytes.data(), "SHADVKC2", 8);
        const u32 driver = instance.GetDriverVersion(), schema = 1;
        std::memcpy(bytes.data() + 8, &driver, 4);
        std::memcpy(bytes.data() + 12, &schema, 4);
        const auto result =
            instance.GetDevice().getPipelineCacheData(*cache, &size, bytes.data() + Prefix);
        if (result != vk::Result::eSuccess)
            return false; // Never publish an incomplete snapshot.
        bytes.resize(Prefix + size);
    }
    const auto snapshot_ms = (Common::Perf::NowNs() - start) / 1'000'000;
    if (!Storage::CacheFile::Write(path, bytes))
        return false;
    saved_revision = current;
    LOG_INFO(Render, "Driver cache saved{}: {} bytes, snapshot {} ms, revision {}",
             final ? " on shutdown" : " asynchronously", bytes.size(), snapshot_ms, current);
    return true;
}
} // namespace Vulkan
