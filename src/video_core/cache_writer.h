// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "common/types.h"

namespace Storage {
// The callback owns filesystem/format policy. Producers only move snapshots into
// this bounded queue; a shutdown drains accepted snapshots, including replacements.
class AsyncCacheWriter {
public:
    using Write = std::function<bool(const std::string&, std::span<const u8>)>;
    struct Counts {
        u64 written, failed, deferred;
    };
    AsyncCacheWriter(Write write_, std::function<void()> initialize = {})
        : write{std::move(write_)}, worker{[this, initialize](std::stop_token stop) {
              if (initialize)
                  initialize();
              Run(stop);
          }} {}
    ~AsyncCacheWriter() {
        Close();
    }
    void SetBudget(u64 value) {
        std::lock_guard lock{mutex};
        budget = value;
    }
    bool Enqueue(std::string name, std::vector<u8>&& data) {
        std::lock_guard lock{mutex};
        if (!accepting || data.empty())
            return false;
        const auto old = pending.find(name);
        const u64 previous = old == pending.end() ? 0 : old->second.size();
        if (data.size() > budget - std::min(budget, bytes - previous)) {
            deferred.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        bytes = bytes - previous + data.size();
        if (old == pending.end()) {
            order.push_back(name);
            pending.emplace(std::move(name), std::move(data));
        } else
            old->second = std::move(data);
        cv.notify_one();
        return true;
    }
    Counts Statistics() const {
        return {written.load(std::memory_order_relaxed), failed.load(std::memory_order_relaxed),
                deferred.load(std::memory_order_relaxed)};
    }
    void Close() {
        if (!worker.joinable())
            return;
        {
            std::lock_guard lock{mutex};
            accepting = false;
        }
        worker.request_stop();
        cv.notify_all();
        worker.join();
    }

private:
    void Run(std::stop_token stop) {
        for (;;) {
            std::string name;
            std::vector<u8> data;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, stop, [this] { return !order.empty(); });
                if (order.empty()) {
                    if (stop.stop_requested())
                        return;
                    continue;
                }
                name = std::move(order.front());
                order.pop_front();
                auto entry = pending.find(name);
                data = std::move(entry->second);
                bytes -= data.size();
                pending.erase(entry);
            }
            bool success{};
            try {
                success = write(name, data);
            } catch (...) {
                success = false;
            }
            (success ? written : failed).fetch_add(1, std::memory_order_relaxed);
        }
    }
    Write write;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::string> order;
    std::unordered_map<std::string, std::vector<u8>> pending;
    u64 bytes{}, budget = 128_MB;
    bool accepting = true;
    std::atomic<u64> written{}, failed{}, deferred{};
    std::jthread worker;
};
} // namespace Storage
