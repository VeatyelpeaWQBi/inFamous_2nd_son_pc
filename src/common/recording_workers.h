// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace Common {

// Recording only: the owning scheduler publishes results in its original order.
// A worker index owns its Vulkan command pool for the lifetime of this executor.
template <typename Result>
class RecordingWorkers {
public:
    explicit RecordingWorkers(std::size_t count) {
        if (count != 2 && count != 4) {
            throw std::invalid_argument("Recording worker count must be 2 or 4");
        }
        try {
            for (std::size_t index = 0; index < count; ++index) {
            workers.emplace_back([this, index] {
                for (;;) {
                    std::packaged_task<Result(std::size_t)> task;
                    {
                        std::unique_lock lock{mutex};
                        changed.wait(lock, [this] { return closing || !tasks.empty(); });
                        if (tasks.empty()) {
                            return;
                        }
                        task = std::move(tasks.front());
                        tasks.pop_front();
                    }
                    task(index);
                }
            });
            }
        } catch (...) {
            {
                std::scoped_lock lock{mutex};
                closing = true;
            }
            changed.notify_all();
            workers.clear();
            throw;
        }
    }

    ~RecordingWorkers() {
        {
            std::scoped_lock lock{mutex};
            closing = true;
        }
        changed.notify_all();
        // Drain accepted tasks before their captured streams/resources are released.
        workers.clear();
    }

    RecordingWorkers(const RecordingWorkers&) = delete;
    RecordingWorkers& operator=(const RecordingWorkers&) = delete;

    template <typename Function>
    std::future<Result> Submit(Function&& function) {
        std::packaged_task<Result(std::size_t)> task{std::forward<Function>(function)};
        auto result = task.get_future();
        {
            std::scoped_lock lock{mutex};
            if (closing) {
                throw std::logic_error("Recording executor is closing");
            }
            tasks.push_back(std::move(task));
        }
        changed.notify_one();
        return result;
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::packaged_task<Result(std::size_t)>> tasks;
    bool closing{};
    // Last member, joined before shared state dies.
    std::vector<std::jthread> workers;
};

} // namespace Common
