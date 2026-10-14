// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;

u64 AvailableHostMemory();
u64 AvailableDeviceMemory(const Instance& instance);

class DriverPipelineCache {
public:
    struct Lease {
        std::shared_lock<std::shared_mutex> lock;
        vk::PipelineCache cache;
    };
    DriverPipelineCache(const Instance& instance, std::filesystem::path path, u64 limit);
    ~DriverPipelineCache();
    vk::PipelineCache Handle() const {
        return *cache;
    }
    // A driver snapshot never makes the command processor wait for our mutex:
    // overlapping new compilations simply use Vulkan's valid no-cache path.
    Lease Acquire();
    void Compiled();
    void Start();
    void Stop();
    bool Save(bool final = false);

private:
    const Instance& instance;
    std::filesystem::path path;
    u64 limit;
    vk::UniquePipelineCache cache;
    std::shared_mutex gate;
    std::mutex save_mutex, wait_mutex;
    std::condition_variable_any wake;
    std::atomic<u64> revision{}, last_compile_ns{};
    std::atomic<bool> closing{};
    u64 saved_revision{};
    std::jthread writer;
};
} // namespace Vulkan
