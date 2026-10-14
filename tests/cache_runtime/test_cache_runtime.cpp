// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include "common/serdes.h"
#include "video_core/cache_file.h"
#include "video_core/cache_writer.h"
#include "video_core/renderer_vulkan/vk_driver_cache_format.h"
#include "video_core/renderer_vulkan/vk_prewarm_budget.h"

static void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
static void Budgets() {
    using B = Vulkan::PrewarmBudget;
    auto large = B::FromAvailable(64 * B::GiB, 16 * B::GiB);
    Check(large.host_bytes == 8 * B::GiB && large.driver_bytes == B::GiB, "large host budgets");
    Check(large.host_reserve == 8 * B::GiB && large.device_reserve == 4 * B::GiB, "game reserves");
    Check(large.CanContinue(16 * B::GiB, 8 * B::GiB, B::GiB), "large host prewarming");
    Check(!large.CanContinue(7 * B::GiB, 8 * B::GiB, 0), "RAM pressure stops prewarming");
    Check(!large.CanContinue(16 * B::GiB, 3 * B::GiB, 0), "VRAM pressure stops prewarming");
    Check(!large.CanContinue(16 * B::GiB, 8 * B::GiB, large.host_bytes), "estimate cap");
    Check(!B::FromAvailable(64 * B::GiB, 0).CanContinue(64 * B::GiB, 0, 0),
          "zero available GPU memory stops prewarming");
    auto small = B::FromAvailable(256 * B::MiB, 128 * B::MiB);
    Check(small.host_bytes == 0 && small.staging_warm_bytes == 0,
          "low memory does not preallocate");
    auto extreme =
        B::FromAvailable(std::numeric_limits<u64>::max(), std::numeric_limits<u64>::max());
    Check(extreme.host_bytes == 8 * B::GiB, "budget arithmetic cannot overflow");
}
static void DriverIdentity() {
    std::array<u8, 16> uuid{};
    uuid[0] = 42;
    std::vector<u8> bytes(48);
    std::memcpy(bytes.data(), "SHADVKC2", 8);
    const auto set = [&](size_t offset, u32 value) {
        std::memcpy(bytes.data() + offset, &value, 4);
    };
    set(8, 123);
    set(12, 1);
    set(16, 32);
    set(20, 1);
    set(24, 0x10de);
    set(28, 99);
    std::memcpy(bytes.data() + 32, uuid.data(), uuid.size());
    Check(Vulkan::DriverCacheCompatible(bytes, 0x10de, 99, 123, uuid), "matching driver cache");
    Check(!Vulkan::DriverCacheCompatible(bytes, 0x10de, 100, 123, uuid), "different GPU rejected");
    Check(!Vulkan::DriverCacheCompatible(bytes, 0x10de, 99, 124, uuid), "driver update rejected");
    set(16, 1000);
    Check(!Vulkan::DriverCacheCompatible(bytes, 0x10de, 99, 123, uuid),
          "invalid header length rejected");
    bytes.resize(10);
    Check(!Vulkan::DriverCacheCompatible(bytes, 0x10de, 99, 123, uuid),
          "truncated driver cache rejected");
}
static void Archives() {
    Serialization::Archive ar;
    Serialization::Writer writer{ar};
    const std::string text = "cache";
    const std::vector<u32> original{1, 2, 3};
    writer.Write(text);
    writer.Write(original);
    auto data = ar.TakeOff();
    const auto* pointer = data.data();
    Serialization::Archive input{std::move(data)};
    Check(input.CurrPtr() == pointer, "archive moves its snapshot without copying");
    Serialization::Reader reader{input};
    std::string restored;
    std::vector<u32> values;
    reader.Read(restored);
    reader.Read(values);
    Check(restored == text && values == original, "archive round trip");
    bool rejected{};
    try {
        u64 missing{};
        reader.Read(missing);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected, "truncated archive fails without aborting the game");
    Serialization::Archive invalid;
    Serialization::Writer invalid_writer{invalid};
    invalid_writer.Write(std::numeric_limits<size_t>::max());
    Serialization::Archive malformed{invalid.TakeOff()};
    Serialization::Reader malformed_reader{malformed};
    rejected = false;
    try {
        malformed_reader.Read(values);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected, "invalid element count rejected before allocation");
}
static void QueueBehavior() {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{}, release{}, background = true;
    const auto caller = std::this_thread::get_id();
    std::map<std::string, std::vector<u8>> saved;
    Storage::AsyncCacheWriter writer{[&](const std::string& name, std::span<const u8> bytes) {
        std::unique_lock lock{mutex};
        background &= std::this_thread::get_id() != caller;
        if (name == "hold") {
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        }
        saved[name] = std::vector<u8>{bytes.begin(), bytes.end()};
        return true;
    }};
    writer.SetBudget(12);
    Check(writer.Enqueue("hold", std::vector<u8>(10, 1)), "first snapshot accepted");
    {
        std::unique_lock lock{mutex};
        Check(condition.wait_for(lock, std::chrono::seconds{2}, [&] { return entered; }),
              "writer started");
    }
    Check(writer.Enqueue("a", std::vector<u8>(8, 2)), "queued snapshot");
    Check(writer.Enqueue("a", std::vector<u8>(4, 3)), "latest snapshot replaces pending data");
    Check(writer.Enqueue("b", std::vector<u8>(8, 4)), "remaining budget accepted");
    Check(!writer.Enqueue("c", std::vector<u8>(1, 5)),
          "full queue defers without waiting for filesystem");
    {
        std::lock_guard lock{mutex};
        release = true;
    }
    condition.notify_all();
    writer.Close();
    Check(background && saved.size() == 3 && saved["a"] == std::vector<u8>(4, 3),
          "background drain keeps latest snapshots");
    Check(writer.Statistics().written == 3 && writer.Statistics().deferred == 1,
          "queue accounting");
    Check(!writer.Enqueue("late", std::vector<u8>{1}), "closed queue rejects late producer");
    Storage::AsyncCacheWriter failing{[](const std::string& name, std::span<const u8>) {
        if (name == "bad")
            throw std::runtime_error("simulated write failure");
        return true;
    }};
    failing.Enqueue("bad", std::vector<u8>{1});
    failing.Enqueue("good", std::vector<u8>{2});
    failing.Close();
    Check(failing.Statistics().failed == 1 && failing.Statistics().written == 1,
          "failed write does not stop remaining persistence");
}
static void Files(const std::filesystem::path& directory) {
    using namespace Storage;
    const auto file = directory / "blob.bin", raw = directory / "legacy.bin",
               bad = directory / "bad.bin";
    std::vector<u8> bytes(8192, 0x3a), restored;
    Check(CacheFile::Write(file, bytes) && CacheFile::Read(file, restored) && restored == bytes,
          "persisted snapshot round trip");
    Check(!CacheFile::Read(file, restored, 100), "oversized snapshot rejected");
    bytes.assign(4096, 0x5b);
    Check(CacheFile::Write(file, bytes) && CacheFile::Read(file, restored) && restored == bytes,
          "atomic replacement");
    {
        std::ofstream output(raw, std::ios::binary);
        output.write("legacy", 6);
    }
    Check(!CacheFile::Read(raw, restored) && CacheFile::Read(raw, restored, 100, true),
          "legacy import must be explicit");
    {
        std::ifstream input(file, std::ios::binary);
        std::ofstream output(bad, std::ios::binary);
        std::vector<char> content((std::istreambuf_iterator<char>(input)), {});
        content.back() ^= 1;
        output.write(content.data(), content.size());
    }
    Check(!CacheFile::Read(bad, restored), "corrupted payload checksum rejected");
    {
        std::ofstream output(bad, std::ios::binary | std::ios::trunc);
        output.write("SHADCA01", 8);
    }
    Check(!CacheFile::Read(bad, restored, 100, true),
          "truncated envelope cannot masquerade as legacy data");
    std::atomic<bool> stop{}, torn{};
    std::thread reader{[&] {
        std::vector<u8> snapshot;
        while (!stop.load()) {
            if (CacheFile::Read(file, snapshot)) {
                const auto first = snapshot.front();
                if (!std::all_of(snapshot.begin(), snapshot.end(),
                                 [first](u8 value) { return value == first; }))
                    torn.store(true);
            }
        }
    }};
    for (u8 value = 1; value < 30; ++value)
        CacheFile::Write(file, std::vector<u8>(4096, value));
    stop.store(true);
    reader.join();
    Check(!torn.load(), "concurrent readers never accept a partial snapshot");
    Check(CacheFile::Write(file, bytes) && CacheFile::Read(file, restored),
          "final complete snapshot retained");
    std::filesystem::remove(file);
    std::filesystem::remove(raw);
    std::filesystem::remove(bad);
}
int main(int argc, char** argv) {
    Check(argc == 2, "explicit build output directory required");
    const auto base = std::filesystem::absolute(argv[1]);
    const auto directory =
        base / ("cache-runtime-data-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(std::filesystem::create_directory(directory), "unique test directory");
    Budgets();
    DriverIdentity();
    Archives();
    QueueBehavior();
    Files(directory);
    Check(std::filesystem::remove(directory), "only owned test files were removed");
    std::cout
        << "PASS: budgets, driver identity, corrupt archives, async drain, atomic snapshots\n";
}
