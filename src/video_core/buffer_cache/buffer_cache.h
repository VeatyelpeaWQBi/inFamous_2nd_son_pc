// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <optional>
#include <string>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/buffer_cache/upload_worker.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

/// A GPU queue the command processor records work for: the graphics queue, or the compute queue
/// running the work of the guest's asynchronous compute queues alongside it (asc_queue). Each
/// has its own command buffers and timeline (scheduler), barrier tracking and staging memory
/// (runtime) and stream buffer.
struct QueueContext {
    Vulkan::Scheduler* scheduler;
    Vulkan::Runtime* runtime;
    StreamBuffer* stream_buffer;
    u32 index; ///< 0 graphics, 1 compute (RegionManager::CrossQueueAccess).
};

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    static constexpr u64 ARENA_PAGE_BITS = 32;
    static constexpr u64 ARENA_PAGE_SIZE = u64{1} << ARENA_PAGE_BITS;
    static constexpr u64 NUM_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

public:
    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer of the active queue.
    [[nodiscard]] StreamBuffer& GetStreamBuffer() noexcept {
        return *active->stream_buffer;
    }

    /// The queue the bindings, uploads and copies are recorded for (asc_queue): nullptr for the
    /// graphics queue. Command processor thread only.
    void SetQueue(QueueContext* context) noexcept {
        active = context != nullptr ? context : &graphics_queue;
    }

    [[nodiscard]] const QueueContext& ActiveQueue() const noexcept {
        return *active;
    }

    [[nodiscard]] bool OnComputeQueue() const noexcept {
        return active->index != 0;
    }

    /// Cross queue dependencies are tracked while the compute queue is in use (asc_queue). The
    /// compute scheduler waits for the upload worker like the graphics one.
    void SetCrossQueueTracking(QueueContext* compute);

    /// asc_queue: a submission of the compute queue waits for the sparse binds of the arena
    /// done so far (issued on the graphics queue, which waits for them in order).
    void WaitArenaBinds(Vulkan::SubmitInfo& info);

    /// Cross queue dependencies (asc_queue): the active queue accesses the GDS buffer (its own
    /// tracking: the GDS is not guest memory).
    void NoteGdsAccess(bool is_written);

    /// Cross queue dependencies (asc_queue): the active queue reads the BDA page table.
    void NoteBdaAccess();

    /// Cross queue dependencies (asc_queue): the active queue accesses a range of guest memory
    /// through the GPU copy, in its current command buffer: a wait on the other queue is added
    /// to the current session when needed (and the graphics session is flushed first when the
    /// compute queue depends on it).
    void NoteQueueAccess(VAddr device_addr, u64 size, bool is_written);

    /// Cross queue dependencies (asc_queue): the active queue uploads the range to the GPU copy
    /// in its current command buffer.
    void NoteQueueUpload(VAddr device_addr, u64 size);

    /// Waits for the GPU to finish the work of every queue submitted so far (readbacks on the
    /// command processor thread).
    void FinishAllQueues();

    /// Submits the session of the compute queue if it recorded commands, after the graphics
    /// work it waits for has been handed to the driver (asc_queue).
    void FlushComputeQueue();

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    void TickFrame();

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    /// guest_rip: the faulting instruction of a guest read (readback_relaxed_sites), 0 if none.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false,
                    u64 guest_rip = 0);

    /// The command processor wrote guest memory directly (constant RAM dumps, WRITE_DATA, DMA
    /// done on the CPU): pages written without a fault are uploaded again.
    void OnGpuThreadWrite(VAddr device_addr, u64 size);

    /// Called at guest fence points: downloads what the GPU wrote in memory the guest read back
    /// recently, so its next reads find the data ready instead of waiting for the GPU.
    void PrefetchReadbacks();

    /// Writes the pending readbacks the GPU has finished. Cheap: only checks the GPU tick known
    /// from the last submission or wait.
    void RetireReadbacks();

    /// Tuning::bind_prefetch: prefetches what ObtainBuffer reads first for a read only binding
    /// of the range (its stream copy or sync memo entry, its tracking region), command processor.
    void PrefetchLookup(VAddr device_addr, u64 size) const noexcept;

    /// Tuning::bind_prefetch: prefetches the guest memory a small read only binding of the range
    /// will copy into the stream buffer, unless its last copy is reused or its pages are bound
    /// in place (stream_static). Called a little before the binding, once its PrefetchLookup
    /// entries are cached (the copies read memory guest threads just wrote, in their caches).
    void PrefetchStreamSource(VAddr device_addr, u64 size) noexcept;

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                           bool is_written,
                                                           bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Whether the GPU copy of the region has data guest memory lacks: GPU modified, or bytes
    /// the GPU wrote in pages whose GPU state a guest write discarded (write merge). Such memory
    /// must be read and written on the GPU.
    [[nodiscard]] bool HasGpuData(VAddr addr, size_t size);

    /// Frame x-ray: the arena holding the range and the range's offset in it, null when its
    /// pages are not resident. Nothing is synchronized or changed.
    [[nodiscard]] std::pair<const Buffer*, u64> PeekArena(VAddr device_addr, u32 size);

    /// Frame x-ray: the tracking state of the range's pages, as text.
    [[nodiscard]] std::string DescribePages(VAddr device_addr, u32 size);

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    /// Whether every block of the range is backed by memory (blocks never stop being resident).
    bool IsResident(u64 first_block, u64 last_block) const;
    void MarkResident(u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    /// GPU modified ranges come as many small scattered islands, so a readback downloads a
    /// window around the requested address.
    std::pair<VAddr, VAddr> GetReadbackWindow(const Buffer* arena, VAddr device_addr, u64 size);

    /// A download from the GPU that was recorded and submitted without waiting for it. The guest
    /// thread that requested it waits for the GPU, then the data is written to guest memory.
    struct PendingReadback {
        u64 tick{};
        u64 record_seq{}; ///< GPU writes numbered above it came after the download was recorded.
        Vulkan::StagingBufferRef download{};
        boost::container::small_vector<vk::BufferCopy, 8> copies;
        VAddr arena_base{};
        VAddr window_start{};
        VAddr window_end{};
        vk::Buffer arena_handle{};
        bool prefetch{};
        bool on_readback_queue{}; ///< tick is on the readback queue, else the graphics queue.
        /// readback_relaxed_sites: pages a guest read was let through while the download was in
        /// flight (no longer GPU modified): they get the new data too if unchanged since.
        boost::container::small_vector<VAddr, 4> relaxed_pages;
    };

    /// Copies GPU data to host memory on the second queue of the graphics family. A copy waits
    /// only for the graphics submission that last wrote its data, not for the work recorded
    /// after it, which is what a readback recorded in the graphics command buffer waits for.
    class ReadbackQueue {
    public:
        struct Job {
            vk::Buffer src;
            vk::Buffer dst;
            std::span<const vk::BufferCopy> copies;
        };

        explicit ReadbackQueue(const Vulkan::Instance& instance);
        ~ReadbackQueue();

        /// A timeline semaphore tick the copies wait for (the queue that wrote the data).
        struct Wait {
            vk::Semaphore semaphore;
            u64 tick;
        };

        /// Submits the copies once the timelines reach their ticks. Returns the readback
        /// semaphore tick signaled when the data is visible to the host.
        u64 Submit(std::span<const Job> jobs, std::span<const Wait> waits);

        Vulkan::Semaphore& Semaphore() noexcept {
            return semaphore;
        }

        bool IsFree(u64 tick) {
            if (semaphore.IsFree(tick)) {
                return true;
            }
            semaphore.Refresh();
            return semaphore.IsFree(tick);
        }

    private:
        const Vulkan::Instance& instance;
        Vulkan::Semaphore semaphore;
        Vulkan::CommandPool command_pool;
    };

    /// True when readbacks go through the readback queue (available and enabled).
    bool UseReadbackQueue() const;

    /// The GPU work that last wrote the GPU modified pages of a window: the tick of each queue
    /// (0 graphics, 1 compute; 0 if none) and where that work is.
    struct WindowWriters {
        std::array<u64, 2> ticks{};
        bool submitted{true}; ///< Every writer is handed to the driver (readback queue usable).
        bool done{true};      ///< Every writer is done on the GPU.
        bool current{};       ///< A writer is in a session being recorded.
    };
    WindowWriters WindowWritersOf(VAddr start, u64 size);
    /// The waits of a readback queue copy of a window.
    boost::container::static_vector<ReadbackQueue::Wait, 2> ReadbackWaits(const WindowWriters&);
    /// A copy of the window recorded in the graphics session: it waits for the compute writer.
    void WaitComputeWriter(const WindowWriters& writers);

    bool IsReadbackDone(bool on_readback_queue, u64 tick);

    /// Registers the window as pending and collects the copies of its GPU modified ranges into a
    /// download buffer, without recording them. Returns false when there is nothing to download
    /// (the window stays registered).
    bool CollectReadback(const Buffer* arena, PendingReadback& readback);

    /// Collects the window and records its copies in the graphics command buffer.
    bool RecordReadback(const Buffer* arena, PendingReadback& readback);

    /// Remembers a window the guest read back, candidate for prefetching.
    void NoteHotWindow(VAddr start, VAddr end);

    struct HotWindow {
        VAddr start;
        VAddr end;
        u64 last_read_ns;
        u64 prefetch_tick; ///< Tick of the last prefetch of the window, at most one in flight.
        u64 prefetch_seq;  ///< Write sequence the last prefetch covered (readback_keep_ranges).
        bool prefetch_on_queue; ///< prefetch_tick is on the readback queue.
    };

    /// Downloads the GPU modified memory around a request and waits for it (command processor).
    void ReadbackSync(VAddr device_addr, u64 size, bool is_write);
    /// Tuning::readback_keep_ranges, see CollectReadback.
    [[nodiscard]] bool KeepRanges() const;

    /// Records and submits the download of the GPU modified memory around a request. Returns the
    /// tick to wait for on wait_semaphore before calling CompleteReadbacks, or 0 when there is
    /// nothing to wait for.
    u64 BeginReadback(VAddr device_addr, u64 size, bool is_write,
                      Vulkan::Semaphore*& wait_semaphore);

    /// Writes the pending readbacks the GPU has finished to guest memory: in any order, except
    /// after an unfinished earlier readback of the same window. With wait, waits for all of them
    /// and writes them in submission order.
    void CompleteReadbacks(bool wait);

    /// Writes the pending readback at this position of pending_readbacks to guest memory.
    void FinishReadback(size_t index);

    /// Guest thread: waits for a readback tick (reported if it takes seconds).
    void WaitReadback(VAddr device_addr, Vulkan::Semaphore* semaphore, u64 tick);

    /// Command processor: the in flight readback to wait for before the range can be written,
    /// nullptr if none (see CompleteReadbacks).
    const PendingReadback* FirstUnfinishedReadback(VAddr device_addr, u64 size);

    /// Whether a guest read by this instruction is let through without waiting for the GPU
    /// (readback_relaxed_sites).
    [[nodiscard]] bool IsRelaxedSite(u64 guest_rip) const;

    /// Command processor: the download of the window of a relaxed read is recorded (or the one in
    /// flight is used), then its page becomes readable at once with the data guest memory has.
    void BeginRelaxedReadback(VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                            bool is_texel_buffer);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    /// Tests whether the GPU could use the guest memory around this address in place (import of
    /// host memory), through the guest mapping and through its backing, and logs the results.
    void ProbeHostImport(VAddr address);
    u32 probed_host_import{};

    /// The scheduler, runtime, staging pool and stream buffer of the active queue (asc_queue).
    [[nodiscard]] Vulkan::Scheduler& Sched() const noexcept {
        return *active->scheduler;
    }
    [[nodiscard]] Vulkan::Runtime& Rt() const noexcept {
        return *active->runtime;
    }
    [[nodiscard]] Vulkan::StagingBufferPool& Staging() const noexcept;
    [[nodiscard]] StreamBuffer& Stream() const noexcept {
        return *active->stream_buffer;
    }

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler; ///< Graphics queue.
    Vulkan::Runtime& runtime;     ///< Graphics queue.
    Vulkan::StagingBufferPool& staging_pool; ///< Graphics queue.
    QueueContext graphics_queue;
    QueueContext* active;
    QueueContext* compute_queue{}; ///< Set while asc_queue is in use (cross queue tracking).
    /// Cross queue dependencies of the GDS buffer: last tick of each queue reading and writing;
    /// of the BDA page table: last tick of each queue writing it.
    std::array<u64, 2> gds_read_tick{};
    std::array<u64, 2> gds_write_tick{};
    std::array<u64, 2> bda_write_tick{};
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;
    RangeSet pending_download_ranges; ///< readback_keep_ranges: downloads in flight.
    std::deque<PendingReadback> pending_readbacks;
    std::unique_ptr<ReadbackQueue> readback_queue;
    boost::container::static_vector<HotWindow, 64> hot_windows;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    bool fault_process_pending{};

    /// Recent copies of small read only buffers in the stream buffer, reused while nothing can
    /// have changed them: same command buffer (the stream buffer keeps its data until that
    /// command buffer completes), same upload epoch (no guest CPU write released since) and no
    /// memory written by the emulator since. Command processor thread only.
    struct StreamCopy {
        VAddr address{};
        u64 tick{};
        u64 epoch{};
        u64 host_write_gen{};
        u64 offset{};
        u32 size{};
        u32 queue{}; ///< The stream buffer (queue) holding the copy; ticks are per queue.
    };
    static constexpr size_t NumStreamCopies = 4096;
    std::array<StreamCopy, NumStreamCopies> stream_copies{};
    u32 stream_clean_sample{};

    /// Sync memo (Tuning::sync_memo): read only ranges found up to date in the GPU copy of guest
    /// memory, with the CPU modification versions of their regions at that time. Command
    /// processor thread only.
    struct SyncMemo {
        VAddr address{};
        const Buffer* arena{};
        std::array<u64, 2> versions{};
        u32 size{};
    };
    static constexpr size_t NumSyncMemos = 4096;
    std::array<SyncMemo, NumSyncMemos> sync_memos{};

    std::array<const Buffer*, NUM_ARENA_PAGES> address_space{};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;
    /// One bit per block of the address space, set once the block is resident: answers the
    /// residency check of every binding without searching resident_ranges.
    std::vector<u64> resident_bits;
    std::vector<u64> resident_full; ///< A bit per word of resident_bits that is all ones.

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};

    /// Upload worker (async_uploads) and the deferred protections of the upload being prepared.
    std::unique_ptr<UploadWorker> upload_worker;
    std::vector<std::pair<u64, u64>> deferred_pages;
};

} // namespace VideoCore
