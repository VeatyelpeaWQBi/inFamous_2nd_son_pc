// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <string>
#include <magic_enum/magic_enum.hpp>

#include "common/alignment.h"
#include "common/elf_info.h"
#include "common/memory_patcher.h"
#include "common/perf_stats.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()},
      graphics_queue{&scheduler_, &runtime_, nullptr, 0}, active{&graphics_queue},
      liverpool{liverpool_}, memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    graphics_queue.stream_buffer = &stream_buffer;
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = ARENA_PAGE_SIZE,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    resident_bits.resize(((u64{1} << (ADDRESS_SPACE_BITS - block_shift)) + 63) / 64);
    resident_full.resize((resident_bits.size() + 63) / 64);
    blocks_per_arena_page = ARENA_PAGE_SIZE / block_size;
    blocks_per_arena_page_shift = ARENA_PAGE_BITS - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * NUM_ARENA_PAGES) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * NUM_ARENA_PAGES);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    if (instance.HasReadbackQueue()) {
        readback_queue = std::make_unique<ReadbackQueue>(instance);
    }
    upload_worker = std::make_unique<UploadWorker>(tracker);
    scheduler.SetSubmitDependency([this](u64 seq) { upload_worker->Wait(seq); });
}

BufferCache::~BufferCache() {
    scheduler.SetSubmitDependency({});
}

UploadWorker::UploadWorker(PageManager& page_manager_) : page_manager{page_manager_} {
    thread = std::jthread([this](std::stop_token stoken) { Run(stoken); });
}

UploadWorker::~UploadWorker() {
    thread.request_stop();
    queue_cv.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
}

namespace {
/// The sleeping worker is woken up once this many jobs are queued (each wake up is a system
/// call for the command processor); otherwise it looks for jobs every millisecond, and a
/// submission waiting for jobs does them itself.
constexpr size_t UploadWorkerWakeJobs = 8;
} // Anonymous namespace

u64 UploadWorker::Enqueue(Job&& job) {
    u64 seq;
    bool wake;
    {
        std::scoped_lock lk{queue_mutex};
        queue.push_back(std::move(job));
        seq = enqueued.fetch_add(1, std::memory_order_acq_rel) + 1;
        wake = worker_sleeping && queue.size() >= UploadWorkerWakeJobs;
    }
    if (wake) {
        queue_cv.notify_one();
    }
    return seq;
}

void UploadWorker::Wait(u64 seq) {
    if (completed.load(std::memory_order_acquire) >= seq) {
        return;
    }
    // Jobs the worker took are done once it releases exec_mutex: do those still queued.
    std::scoped_lock exec_lock{exec_mutex};
    if (completed.load(std::memory_order_acquire) < seq) {
        ExecuteQueued();
    }
}

void UploadWorker::Run(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuUploadWorker");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{queue_mutex};
            worker_sleeping = true;
            queue_cv.wait_for(lk, stoken, std::chrono::milliseconds{1},
                              [this] { return queue.size() >= UploadWorkerWakeJobs; });
            worker_sleeping = false;
            if (queue.empty()) {
                continue;
            }
        }
        std::scoped_lock exec_lock{exec_mutex};
        ExecuteQueued();
    }
}

void UploadWorker::ExecuteQueued() {
    u64 last_seq;
    {
        std::scoped_lock lk{queue_mutex};
        if (queue.empty()) {
            return;
        }
        batch.swap(queue);
        last_seq = enqueued.load(std::memory_order_acquire);
    }
    const u64 start_ns = Common::Perf::NowNs();
    // First the protections of all the jobs, merged (fewer system calls)...
    const bool profile_upload = Common::Perf::GetTuning().perf_log.load(std::memory_order_relaxed);
    ranges.clear();
    for (const Job& job : batch) {
        ranges.insert(ranges.end(), job.pages.begin(), job.pages.end());
    }
    std::sort(ranges.begin(), ranges.end());
    u64 num_pages = 0;
    for (size_t i = 0; i < ranges.size();) {
        const u64 first = ranges[i].first;
        u64 end = ranges[i].second;
        for (++i; i < ranges.size() && ranges[i].first <= end; ++i) {
            end = std::max(end, ranges[i].second);
        }
        num_pages += page_manager.RefreshDeferredProtect(first, end);
    }
    // ... then the copies: a guest write to these pages from now on faults.
    const u64 copy_start_ns = profile_upload ? Common::Perf::NowNs() : 0;
    if (profile_upload)
        Common::Perf::Add(Common::Perf::UploadProtectionNs, copy_start_ns - start_ns);
    auto* const memory = Core::Memory::Instance();
    u64 num_bytes = 0;
    for (const Job& job : batch) {
        for (const Copy& copy : job.copies) {
            memory->CopySparseMemory(copy.src, copy.dst, copy.size);
            num_bytes += copy.size;
        }
        if (job.flush_buffer) {
            job.flush_buffer->Flush(job.flush_offset, job.flush_size);
        }
    }
    Common::Perf::Add(Common::Perf::AsyncUploadJobs, batch.size());
    Common::Perf::Add(Common::Perf::AsyncUploadBytes, num_bytes);
    if (profile_upload)
        Common::Perf::Add(Common::Perf::UploadCopyNs, Common::Perf::NowNs() - copy_start_ns);
    Common::Perf::Add(Common::Perf::AsyncUploadPages, num_pages);
    batch.clear();
    completed.store(last_seq, std::memory_order_release);
    Common::Perf::Add(Common::Perf::AsyncUploadBusyNs, Common::Perf::NowNs() - start_ns);
}

BufferCache::ReadbackQueue::ReadbackQueue(const Vulkan::Instance& instance_)
    : instance{instance_}, semaphore{instance_}, command_pool{instance_, &semaphore} {}

BufferCache::ReadbackQueue::~ReadbackQueue() {
    // The command buffers and download buffers must not be in use anymore.
    std::ignore = instance.GetReadbackQueue().waitIdle();
}

u64 BufferCache::ReadbackQueue::Submit(std::span<const Job> jobs, std::span<const Wait> waits) {
    // Committed with the tick the submission below signals.
    const vk::CommandBuffer cmdbuf = command_pool.Commit();
    Vulkan::Check(cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    for (const Job& job : jobs) {
        cmdbuf.copyBuffer(job.src, job.dst, job.copies);
    }
    // Makes the copied data visible to host reads once the semaphore is signaled.
    const vk::MemoryBarrier2 to_host = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &to_host,
    });
    Vulkan::Check(cmdbuf.end());

    const u64 signal_tick = semaphore.NextTick();
    const vk::Semaphore signal_semaphore = semaphore.Handle();
    // The waits make the writes of the submissions up to their ticks available and visible to
    // the copies.
    std::array<vk::Semaphore, 4> wait_semaphores;
    std::array<u64, 4> wait_ticks;
    std::array<vk::PipelineStageFlags, 4> wait_stages;
    u32 num_waits = 0;
    for (const Wait& wait : waits) {
        if (wait.tick == 0 || num_waits == wait_semaphores.size()) {
            continue;
        }
        wait_semaphores[num_waits] = wait.semaphore;
        wait_ticks[num_waits] = wait.tick;
        wait_stages[num_waits++] = vk::PipelineStageFlagBits::eTransfer;
    }
    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = num_waits,
        .pWaitSemaphoreValues = wait_ticks.data(),
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &signal_tick,
    };
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = num_waits,
        .pWaitSemaphores = wait_semaphores.data(),
        .pWaitDstStageMask = wait_stages.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &signal_semaphore,
    };
    const auto result = instance.GetReadbackQueue().submit(submit_info);
    if (result == vk::Result::eErrorDeviceLost) {
        Vulkan::GpuCrashDiag::ReportDeviceLost(instance, "readback submit");
    }
    ASSERT_MSG(result == vk::Result::eSuccess, "Readback submit failed: {}",
               vk::to_string(result));
    Common::Perf::Add(Common::Perf::ReadbackQueueSubmits);
    return signal_tick;
}

bool BufferCache::UseReadbackQueue() const {
    return readback_queue &&
           Common::Perf::GetTuning().readback_queue.load(std::memory_order_relaxed);
}

Vulkan::StagingBufferPool& BufferCache::Staging() const noexcept {
    return active->runtime->GetStagingPool();
}

void BufferCache::SetCrossQueueTracking(QueueContext* compute) {
    if (compute_queue != nullptr && compute_queue != compute) {
        compute_queue->scheduler->SetSubmitDependency({});
    }
    compute_queue = compute;
    if (compute != nullptr) {
        compute->scheduler->SetSubmitDependency([this](u64 seq) { upload_worker->Wait(seq); });
    }
}

void BufferCache::WaitArenaBinds(Vulkan::SubmitInfo& info) {
    // Binds signal memory_semaphore with the tick NextTick returned: the last one is below the
    // current tick (1 at start, no bind yet).
    const u64 last_bind = memory_semaphore.CurrentTick() - 1;
    if (last_bind == 0 || memory_semaphore.IsFree(last_bind)) {
        return;
    }
    for (u32 i = 0; i < info.num_wait_semas; ++i) {
        if (info.wait_semas[i] == memory_semaphore.Handle()) {
            info.wait_ticks[i] = std::max(info.wait_ticks[i], last_bind);
            return;
        }
    }
    info.AddWait(memory_semaphore.Handle(), last_bind);
}

BufferCache::WindowWriters BufferCache::WindowWritersOf(VAddr start, u64 size) {
    WindowWriters writers;
    writers.ticks = memory_tracker->GetWriteTicks(start, size);
    if (compute_queue == nullptr) {
        writers.ticks[1] = 0;
    }
    for (u32 queue = 0; queue < 2; ++queue) {
        const u64 tick = writers.ticks[queue];
        if (tick == 0) {
            continue;
        }
        Vulkan::Scheduler& sched = queue == 0 ? scheduler : *compute_queue->scheduler;
        writers.submitted &= tick <= sched.SubmittedTick();
        writers.done &= sched.IsFree(tick);
        writers.current |= tick >= sched.CurrentTick();
    }
    return writers;
}

boost::container::static_vector<BufferCache::ReadbackQueue::Wait, 2> BufferCache::ReadbackWaits(
    const WindowWriters& writers) {
    boost::container::static_vector<ReadbackQueue::Wait, 2> waits;
    if (writers.ticks[0] != 0) {
        waits.push_back(ReadbackQueue::Wait{scheduler.GetWorkSemaphore()->Handle(), writers.ticks[0]});
    }
    if (writers.ticks[1] != 0 && compute_queue != nullptr) {
        waits.push_back(ReadbackQueue::Wait{compute_queue->scheduler->GetWorkSemaphore()->Handle(),
                                            writers.ticks[1]});
    }
    return waits;
}

void BufferCache::WaitComputeWriter(const WindowWriters& writers) {
    const u64 tick = writers.ticks[1];
    if (tick == 0 || compute_queue == nullptr || compute_queue->scheduler->IsFree(tick)) {
        return;
    }
    if (tick >= compute_queue->scheduler->CurrentTick()) {
        FlushComputeQueue();
    }
    scheduler.AddCrossWait(compute_queue->scheduler->GetWorkSemaphore()->Handle(), tick);
    Common::Perf::Add(Common::Perf::CrossWaitsGraphics);
}

void BufferCache::FinishAllQueues() {
    if (compute_queue != nullptr && compute_queue != active) {
        compute_queue->scheduler->Finish();
    }
    if (active != &graphics_queue) {
        graphics_queue.scheduler->Finish();
    }
    Sched().Finish();
}

void BufferCache::FlushComputeQueue() {
    if (compute_queue == nullptr || !compute_queue->scheduler->HasWork()) {
        return;
    }
    // A wait on a graphics tick is submitted only once that tick is (threaded recording submits
    // a flushed session a little later).
    const u64 graphics_tick = compute_queue->scheduler->CrossWaitTick(
        graphics_queue.scheduler->GetWorkSemaphore()->Handle());
    if (graphics_tick != 0) {
        graphics_queue.scheduler->WaitSubmitted(graphics_tick);
    }
    compute_queue->scheduler->Flush();
    Common::Perf::Add(Common::Perf::ComputeQueueSubmits);
}

void BufferCache::NoteGdsAccess(bool is_written) {
    if (compute_queue == nullptr) {
        return;
    }
    const u32 queue = active->index;
    const u32 other = queue ^ 1;
    u64 dependency = gds_write_tick[other];
    if (is_written) {
        dependency = std::max(dependency, gds_read_tick[other]);
    }
    if (dependency != 0) {
        QueueContext& other_queue = other == 0 ? graphics_queue : *compute_queue;
        if (!other_queue.scheduler->IsFree(dependency)) {
            if (dependency >= other_queue.scheduler->CurrentTick()) {
                // The session being recorded on the other queue: submitted first (never a wait
                // on a tick that is not).
                if (other == 0) {
                    graphics_queue.scheduler->Flush();
                } else {
                    FlushComputeQueue();
                }
                Common::Perf::Add(Common::Perf::CrossQueueFlushes);
            }
            Sched().AddCrossWait(other_queue.scheduler->GetWorkSemaphore()->Handle(), dependency);
            Common::Perf::Add(queue == 0 ? Common::Perf::CrossWaitsGraphics
                                         : Common::Perf::CrossWaitsCompute);
        }
    }
    (is_written ? gds_write_tick : gds_read_tick)[queue] = Sched().CurrentTick();
}

void BufferCache::NoteBdaAccess() {
    if (compute_queue == nullptr) {
        return;
    }
    const u32 other = active->index ^ 1;
    const u64 tick = bda_write_tick[other];
    QueueContext& other_queue = other == 0 ? graphics_queue : *compute_queue;
    if (tick == 0 || other_queue.scheduler->IsFree(tick)) {
        return;
    }
    if (tick >= other_queue.scheduler->CurrentTick()) {
        if (other == 0) {
            graphics_queue.scheduler->Flush();
        } else {
            FlushComputeQueue();
        }
        Common::Perf::Add(Common::Perf::CrossQueueFlushes);
    }
    Sched().AddCrossWait(other_queue.scheduler->GetWorkSemaphore()->Handle(), tick);
    Common::Perf::Add(active->index == 0 ? Common::Perf::CrossWaitsGraphics
                                         : Common::Perf::CrossWaitsCompute);
}

void BufferCache::NoteQueueAccess(VAddr device_addr, u64 size, bool is_written) {
    if (compute_queue == nullptr) {
        return;
    }
    const u32 queue = active->index;
    if (queue == 0 && !memory_tracker->ComputeTouched(device_addr, size)) {
        // The compute queue never used these words: nothing to wait for, and no need to
        // remember this access (the compute queue waits for all the graphics work recorded so
        // far when it first uses a word).
        return;
    }
    bool untouched = false;
    u64 dependency = memory_tracker->CrossQueueAccess(device_addr, size, queue,
                                                      Sched().CurrentTick(), is_written, untouched);
    QueueContext& other_queue = queue == 0 ? *compute_queue : graphics_queue;
    if (queue == 1 && untouched) {
        // Graphics reads of these words are unknown: everything recorded so far on the graphics
        // queue comes first.
        dependency = std::max(dependency, graphics_queue.scheduler->CurrentTick());
        Common::Perf::Add(Common::Perf::CrossQueueFirstTouches);
    }
    if (dependency == 0 || other_queue.scheduler->IsFree(dependency)) {
        return;
    }
    if (dependency >= other_queue.scheduler->CurrentTick()) {
        // The session being recorded on the other queue: submitted first (never a wait on a
        // tick that is not).
        if (queue == 1) {
            graphics_queue.scheduler->Flush();
        } else {
            FlushComputeQueue();
        }
        Common::Perf::Add(Common::Perf::CrossQueueFlushes);
    }
    Sched().AddCrossWait(other_queue.scheduler->GetWorkSemaphore()->Handle(), dependency);
    Common::Perf::Add(queue == 0 ? Common::Perf::CrossWaitsGraphics
                                 : Common::Perf::CrossWaitsCompute);
}

bool BufferCache::IsReadbackDone(bool on_readback_queue, u64 tick) {
    if (on_readback_queue) {
        return readback_queue->IsFree(tick);
    }
    return scheduler.IsFree(tick);
}

void NextUploadEpoch() noexcept {
    g_upload_epoch.fetch_add(1, std::memory_order_acq_rel);
    Common::Perf::Add(Common::Perf::UploadEpochs);
}

void BufferCache::TickFrame() {
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
    // Write the readbacks the GPU has finished, their pages become readable without a fault.
    RetireReadbacks();

    // Hot pages are protected again from time to time, to notice those the guest stopped
    // writing every frame (the others become hot again after one fault).
    const u32 frame = g_tracker_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    // Write merge: pages not uploaded for a few seconds forget their content before the guest
    // write (their GPU bytes are then overwritten at their next upload, as without merge).
    if (frame % 64 == 0) {
        memory_tracker->PrunePageSnapshots(frame, 240);
    }
    const u32 cooldown =
        Common::Perf::GetTuning().hot_page_cooldown_frames.load(std::memory_order_relaxed);
    if (cooldown != 0 && frame % cooldown == 0) {
        const u32 cooled = memory_tracker->CoolHotPages();
        Common::Perf::Add(Common::Perf::HotPageDemotions, cooled);
    }
}

void BufferCache::OnGpuThreadWrite(VAddr device_addr, u64 size) {
    NoteHostWrite();
    memory_tracker->OnGpuThreadWrite(device_addr, size);
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    // Guest write faults invalidate at most 8 bytes, unmaps and other invalidations whole
    // ranges. Faults of the command processor thread come with assume_locks.
    const bool is_fault = size <= 8;
    const bool discarded = memory_tracker->InvalidateRegion(
        device_addr, size, is_fault, assume_locks, [this, device_addr, size, assume_locks] {
            ReadMemory(device_addr, size, true, assume_locks);
        });
    if (discarded) {
        Common::Perf::Add(Common::Perf::ReadbackDiscards);
        if (Common::Perf::FrameDumpActive().load(std::memory_order_relaxed)) [[unlikely]] {
            // Frame x-ray: the GPU data of the page is forgotten (write discard), or kept on
            // the GPU (write merge). Any thread.
            LOG_INFO(Render_Vulkan, "DUMPX discard {:x} size {} {}", device_addr, size,
                     assume_locks ? "gpu-thread" : "guest");
        }
    }
}

std::pair<VAddr, VAddr> BufferCache::GetReadbackWindow(const Buffer* arena, VAddr device_addr,
                                                       u64 size) {
    const u64 window_size =
        Common::Perf::GetTuning().readback_window_bytes.load(std::memory_order_relaxed);
    const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
    const VAddr window_start =
        std::max<VAddr>(Common::AlignDown(device_addr, window_size), arena->cpu_addr);
    // Page aligned: downloads and protection work on whole pages.
    const VAddr window_end = std::min<VAddr>(
        Common::AlignUp(std::max<VAddr>(window_start + window_size, device_addr + size),
                        BYTES_PER_PAGE),
        arena_end);
    return {window_start, window_end};
}

bool BufferCache::IsRelaxedSite(u64 guest_rip) const {
    if (guest_rip == 0) {
        return false;
    }
    const auto& tuning = Common::Perf::GetTuning();
    if (tuning.relaxed_all.load(std::memory_order_relaxed)) {
        return true;
    }
    const u32 count = tuning.num_relaxed_sites.load(std::memory_order_acquire);
    const u64 base = MemoryPatcher::g_eboot_address;
    if (count == 0 || base == 0 || guest_rip < base) {
        return false;
    }
    const u64 block = (guest_rip - base) & ~u64{0xFF};
    for (u32 i = 0; i < count; ++i) {
        if (tuning.relaxed_sites[i].load(std::memory_order_relaxed) == block) {
            return true;
        }
    }
    return false;
}

void BufferCache::BeginRelaxedReadback(VAddr device_addr, u64 size) {
    // Readbacks the GPU has finished are written first: the page may be up to date already.
    CompleteReadbacks(false);
    const VAddr page_start = Common::AlignDown(device_addr, BYTES_PER_PAGE);
    const VAddr page_end = Common::AlignUp(device_addr + size, BYTES_PER_PAGE);
    if (!memory_tracker->IsRegionGpuModified(page_start, page_end - page_start)) {
        return;
    }
    const u32 first_block = device_addr >> block_shift;
    const u32 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    const auto [window_start, window_end] = GetReadbackWindow(arena, device_addr, size);
    // Downloaded again at the next fences while the guest keeps reading it.
    NoteHotWindow(window_start, window_end);

    // The GPU data of the window no pending readback covers is downloaded now, as for a read
    // that waits (nobody waits for it).
    PendingReadback readback{
        .arena_base = arena->cpu_addr,
        .window_start = window_start,
        .window_end = window_end,
    };
    const WindowWriters writers = WindowWritersOf(window_start, window_end - window_start);
    const bool use_queue = UseReadbackQueue() && writers.submitted;
    if (use_queue ? CollectReadback(arena, readback) : RecordReadback(arena, readback)) {
        if (use_queue) {
            const ReadbackQueue::Job job{
                .src = readback.arena_handle,
                .dst = readback.download.buffer->Handle(),
                .copies = readback.copies,
            };
            readback.tick = readback_queue->Submit(std::span{&job, 1}, ReadbackWaits(writers));
            readback.on_readback_queue = true;
        } else {
            WaitComputeWriter(writers);
            readback.tick = scheduler.CurrentTick();
            scheduler.MarkReadbackSession();
            scheduler.Flush();
        }
        pending_readbacks.push_back(std::move(readback));
        Common::Perf::Add(Common::Perf::ReadbackAsync);
    } else {
        memory_tracker->RemovePendingReadback(window_start, window_end);
    }
    // Every readback in flight for these pages gives them its data when it lands, unless the
    // guest or the GPU changed them meanwhile.
    for (VAddr page = page_start; page < page_end; page += BYTES_PER_PAGE) {
        for (auto& pending : pending_readbacks) {
            if (pending.window_start <= page && page < pending.window_end &&
                std::ranges::find(pending.relaxed_pages, page) == pending.relaxed_pages.end()) {
                pending.relaxed_pages.push_back(page);
            }
        }
    }
    // Readable at once: the guest reads what guest memory holds (the previous download).
    memory_tracker->UnmarkRegionAsGpuModified(page_start, page_end - page_start, false);
    Common::Perf::Add(Common::Perf::ReadbackRelaxed);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks,
                             u64 guest_rip) {
    const u64 request_ns = Common::Perf::NowNs();
    // Particle trace (Tuning::trace_reads): what the guest gets when it reads GPU written memory
    // in the traced range, logged once the read is complete.
    const auto& trace_tuning = Common::Perf::GetTuning();
    const u64 trace_lo = trace_tuning.trace_reads_lo.load(std::memory_order_relaxed);
    const bool traced = trace_lo != 0 && device_addr + size > trace_lo &&
                        device_addr < trace_tuning.trace_reads_hi.load(std::memory_order_relaxed);
    bool trace_retry = false;
    bool trace_nothing = false;
    SCOPE_EXIT {
        if (!traced) [[likely]] {
            return;
        }
        std::array<u32, 4> words{};
        const u64 n = std::min<u64>(size, sizeof(words));
        memory->CopySparseMemory(device_addr, reinterpret_cast<u8*>(words.data()), n);
        std::string bytes;
        for (u64 i = 0; i < (n + 3) / 4; ++i) {
            bytes += fmt::format("{}{:08x}", i ? "," : "", words[i]);
        }
        LOG_INFO(Render_Vulkan, "PTRACE read f{} {:x}+{} {} {} {} wait {}us{}{}",
                 Common::Perf::GuestFrameCounter().load(std::memory_order_relaxed), device_addr,
                 size, is_write ? "write" : "read", assume_locks ? "gpu-thread" : "guest", bytes,
                 (Common::Perf::NowNs() - request_ns) / 1000, trace_retry ? " retry" : "",
                 trace_nothing ? " nothing" : "");
    };
    if (Common::Perf::FrameDumpActive().load(std::memory_order_relaxed)) [[unlikely]] {
        // Frame x-ray: a guest access to GPU written memory (precise readback). Any thread.
        LOG_INFO(Render_Vulkan, "DUMPX readback {:x} size {} {} {}", device_addr, size,
                 is_write ? "write" : "read", assume_locks ? "gpu-thread" : "guest");
    }
    Common::Perf::ScopedTimer stall_timer{is_write ? Common::Perf::ReadbackWrites
                                                   : Common::Perf::ReadbackReads,
                                          Common::Perf::ReadbackStallNs};
    if (!assume_locks && !is_write && IsRelaxedSite(guest_rip)) {
        // readback_relaxed_sites: this code reads GPU results without waiting for the GPU (the
        // previous ones, the new ones land in guest memory once the GPU is done).
        liverpool->SendCommand<true>([&] {
            Common::Perf::Add(Common::Perf::ReadbackQueueNs, Common::Perf::NowNs() - request_ns);
            BeginRelaxedReadback(device_addr, size);
        });
        return;
    }
    if (!assume_locks &&
        Common::Perf::GetTuning().async_readbacks.load(std::memory_order_relaxed)) {
        // The command processor records and submits the download, then goes on with its work
        // while this (guest) thread waits for the GPU. Previously the command processor itself
        // waited for the GPU to become idle, hundreds of times per second in busy scenes.
        u64 wait_tick = 0;
        Vulkan::Semaphore* wait_semaphore = nullptr;
        liverpool->SendCommand<true>([&] {
            Common::Perf::Add(Common::Perf::ReadbackQueueNs, Common::Perf::NowNs() - request_ns);
            wait_tick = BeginReadback(device_addr, size, is_write, wait_semaphore);
        });
        if (wait_tick == 0) {
            trace_nothing = true;
            return;
        }
        WaitReadback(device_addr, wait_semaphore, wait_tick);
        for (u32 round = 0;; ++round) {
            // An earlier readback of the window (another queue) may still be in flight: its data
            // must land first, then this one's. The guest thread waits for it (a few times at
            // most), not the command processor.
            Vulkan::Semaphore* next_semaphore = nullptr;
            u64 next_tick = 0;
            const bool may_wait = round < 4;
            liverpool->SendCommand<true>([&] {
                CompleteReadbacks(false);
                if (!memory_tracker->IsRegionGpuModified(device_addr, size)) {
                    if (is_write) {
                        memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                    }
                    return;
                }
                if (const PendingReadback* pending =
                        may_wait ? FirstUnfinishedReadback(device_addr, size) : nullptr) {
                    next_semaphore = pending->on_readback_queue ? &readback_queue->Semaphore()
                                                                : scheduler.GetWorkSemaphore();
                    next_tick = pending->tick;
                    return;
                }
                // The GPU wrote this memory again while the download was in flight. Download it
                // synchronously so the request completes instead of faulting again.
                trace_retry = true;
                Common::Perf::Add(Common::Perf::ReadbackAsyncRetries);
                ReadbackSync(device_addr, size, is_write);
            });
            if (next_tick == 0) {
                break;
            }
            WaitReadback(device_addr, next_semaphore, next_tick);
        }
        return;
    }
    const auto flush_request = [this, device_addr, size, is_write, request_ns] {
        // Time spent waiting for the command processor thread to pick up the request.
        Common::Perf::Add(Common::Perf::ReadbackQueueNs, Common::Perf::NowNs() - request_ns);
        ReadbackSync(device_addr, size, is_write);
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::WaitReadback(VAddr device_addr, Vulkan::Semaphore* semaphore, u64 tick) {
    const u64 wait_start = Common::Perf::NowNs();
    // A readback takes milliseconds: one still waiting after seconds is reported once (hang
    // diagnostics), then the wait goes on.
    if (!semaphore->WaitFor(tick, 2'000'000'000ULL)) {
        const bool on_queue = readback_queue && semaphore == &readback_queue->Semaphore();
        LOG_WARNING(Render_Vulkan,
                    "Readback of {:#x} still waiting after 2 s: tick {} of the {} queue (done "
                    "{}), graphics submitted {} recorded {} done {}",
                    device_addr, tick, on_queue ? "readback" : "graphics",
                    semaphore->KnownGpuTick(), scheduler.SubmittedTick(), scheduler.CurrentTick(),
                    scheduler.GetWorkSemaphore()->KnownGpuTick());
        semaphore->Wait(tick);
    }
    Common::Perf::Add(Common::Perf::ReadbackAsyncWaitNs, Common::Perf::NowNs() - wait_start);
}

const BufferCache::PendingReadback* BufferCache::FirstUnfinishedReadback(VAddr device_addr,
                                                                         u64 size) {
    const auto overlaps = [](const PendingReadback& readback, VAddr start, VAddr end) {
        return readback.window_start < end && start < readback.window_end;
    };
    for (size_t i = 0; i < pending_readbacks.size(); ++i) {
        const PendingReadback& readback = pending_readbacks[i];
        if (!overlaps(readback, device_addr, device_addr + size)) {
            continue;
        }
        if (!IsReadbackDone(readback.on_readback_queue, readback.tick)) {
            return &readback;
        }
        // Done but not written yet: an earlier readback of its window is still in flight.
        for (size_t j = 0; j < i; ++j) {
            const PendingReadback& earlier = pending_readbacks[j];
            if (overlaps(earlier, readback.window_start, readback.window_end) &&
                !IsReadbackDone(earlier.on_readback_queue, earlier.tick)) {
                return &earlier;
            }
        }
    }
    return nullptr;
}

void BufferCache::ReadbackSync(VAddr device_addr, u64 size, bool is_write) {
    // Pending asynchronous readbacks keep their pages protected until they are written.
    CompleteReadbacks(true);
    const u32 first_block = device_addr >> block_shift;
    const u32 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    const auto [window_start, window_end] = GetReadbackWindow(arena, device_addr, size);
    // CPU writes to the window must not discard GPU data the download below writes back.
    memory_tracker->AddPendingReadback(window_start, window_end);
    DownloadMemory(arena, window_start, window_end - window_start);
    memory_tracker->RemovePendingReadback(window_start, window_end);
    if (is_write) {
        memory_tracker->MarkRegionAsCpuModified(device_addr, size);
    }
}

bool BufferCache::RecordReadback(const Buffer* arena, PendingReadback& readback) {
    if (!CollectReadback(arena, readback)) {
        return false;
    }
    runtime.CopyBuffer(arena, readback.download.buffer, readback.copies);
    return true;
}

bool BufferCache::KeepRanges() const {
    return Common::Perf::GetTuning().readback_keep_ranges.load(std::memory_order_relaxed);
}

bool BufferCache::CollectReadback(const Buffer* arena, PendingReadback& readback) {
    // Registered before looking at the pages: from now on CPU writes to the window flush instead
    // of discarding the GPU data, which this readback would overwrite when it completes.
    memory_tracker->AddPendingReadback(readback.window_start, readback.window_end);
    readback.arena_handle = arena->Handle();
    readback.record_seq = memory_tracker->CurrentWriteSeq();
    // readback_keep_ranges: the GPU modified ranges stay known until the download has landed in
    // guest memory (FinishReadback), so the GPU copy keeps being read and, with write merge,
    // kept, meanwhile. Otherwise they are forgotten now, so a later download skips them.
    const bool keep_ranges = KeepRanges();

    u64 total_size_bytes = 0;
    const auto add_copy = [&](VAddr start, u64 size) {
        readback.copies.push_back(vk::BufferCopy{
            .srcOffset = start - readback.arena_base,
            .dstOffset = total_size_bytes,
            .size = size,
        });
        // Align up to avoid cache conflicts
        total_size_bytes += Common::AlignUp(size, 64ULL);
    };
    memory_tracker->ForEachDownloadRange<false>(
        readback.window_start, readback.window_end - readback.window_start,
        [&](u64 address, u64 range_size) {
            gpu_modified_ranges.ForEachInRange(address, range_size, [&](VAddr start, VAddr end) {
                if (keep_ranges) {
                    // Bytes a pending download already copies are not copied again.
                    pending_download_ranges.ForEachNotInRange(start, end - start, add_copy);
                } else {
                    add_copy(start, end - start);
                }
            });
            if (!keep_ranges) {
                gpu_modified_ranges.Subtract(address, range_size);
            }
        });
    if (readback.copies.empty()) {
        return false;
    }
    if (keep_ranges) {
        for (const auto& copy : readback.copies) {
            pending_download_ranges.Add(readback.arena_base + copy.srcOffset, copy.size);
        }
    }
    // Held until the readback completes. Power of two sizes so the buffers get reused.
    const u64 request_size = std::max<u64>(std::bit_ceil(total_size_bytes), 64_KB);
    readback.download = staging_pool.Request(request_size, MemoryType::HostCached, 0, true);
    for (auto& copy : readback.copies) {
        copy.dstOffset += readback.download.offset;
    }
    Common::Perf::Add(Common::Perf::ReadbackBytes, total_size_bytes);
    return true;
}

void BufferCache::NoteHotWindow(VAddr start, VAddr end) {
    const u64 now = Common::Perf::NowNs();
    for (auto& window : hot_windows) {
        if (window.start == start && window.end == end) {
            window.last_read_ns = now;
            return;
        }
    }
    if (hot_windows.size() == hot_windows.capacity()) {
        hot_windows.erase(std::ranges::min_element(hot_windows, {}, &HotWindow::last_read_ns));
    }
    hot_windows.push_back(HotWindow{
        .start = start,
        .end = end,
        .last_read_ns = now,
        .prefetch_tick = 0,
        .prefetch_seq = 0,
        .prefetch_on_queue = false,
    });
}

u64 BufferCache::BeginReadback(VAddr device_addr, u64 size, bool is_write,
                               Vulkan::Semaphore*& wait_semaphore) {
    // Readbacks the GPU has finished are written first, the requested data may be among them.
    CompleteReadbacks(false);

    const u32 first_block = device_addr >> block_shift;
    const u32 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    const auto [window_start, window_end] = GetReadbackWindow(arena, device_addr, size);
    if (!is_write && size <= BYTES_PER_PAGE) {
        // Guest reads (faults), not writes nor large invalidations such as unmaps.
        NoteHotWindow(window_start, window_end);
    }

    PendingReadback readback{
        .arena_base = arena->cpu_addr,
        .window_start = window_start,
        .window_end = window_end,
    };
    // The command buffers that last wrote the GPU modified pages of the window, and where they
    // are now (statistics: what the guest waits for).
    const WindowWriters writers = WindowWritersOf(window_start, window_end - window_start);
    if (!is_write && size <= BYTES_PER_PAGE) {
        if (writers.done) {
            Common::Perf::Add(Common::Perf::ReadbackProducerDone);
        } else if (writers.submitted) {
            Common::Perf::Add(Common::Perf::ReadbackProducerSubmitted);
        } else if (!writers.current) {
            Common::Perf::Add(Common::Perf::ReadbackProducerQueued);
        } else {
            Common::Perf::Add(Common::Perf::ReadbackProducerCurrent);
        }
    }
    // readback_queue: the copy runs on the readback queue, waiting only for the command buffers
    // that wrote the data, once they are submitted to the driver. Data written by commands not
    // submitted yet (being recorded, or handed to the recording thread) is copied by the graphics
    // command buffer: the readback queue never waits for a signal that is not submitted yet
    // (with threaded recording, such a wait froze the game).
    const bool queue_enabled = UseReadbackQueue();
    const bool use_queue = queue_enabled && writers.submitted;
    // Pages of the window downloaded by earlier readbacks stay protected until those complete:
    // wait for the last overlapping one (earlier ones of another queue may complete after it,
    // the guest thread then waits for them too, see ReadMemory).
    const PendingReadback* overlapping = nullptr;
    for (const auto& pending : pending_readbacks) {
        if (pending.window_start < window_end && window_start < pending.window_end) {
            overlapping = &pending;
        }
    }
    bool recorded = false;
    if (KeepRanges() && overlapping &&
        overlapping->record_seq >=
            memory_tracker->GetWriteSeq(window_start, window_end - window_start)) {
        // readback_keep_ranges: the ranges are still known, a new download would repeat the
        // pending one, which was recorded after the last GPU write of the window.
        memory_tracker->AddPendingReadback(window_start, window_end);
    } else {
        recorded = use_queue ? CollectReadback(arena, readback) : RecordReadback(arena, readback);
    }
    if (!recorded) {
        // Nothing left to download in the window.
        memory_tracker->RemovePendingReadback(window_start, window_end);
        if (!overlapping) {
            // Same as a synchronous readback.
            if (is_write) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
            }
            return 0;
        }
        if (overlapping->prefetch) {
            Common::Perf::Add(Common::Perf::ReadbackPrefetchHits);
        }
        wait_semaphore = overlapping->on_readback_queue ? &readback_queue->Semaphore()
                                                        : scheduler.GetWorkSemaphore();
        return overlapping->tick;
    }
    const u64 submit_start = Common::Perf::NowNs();
    if (queue_enabled) {
        Common::Perf::Add(use_queue ? Common::Perf::ReadbackQueueReads
                                    : Common::Perf::ReadbackQueueFallbacks);
    }
    if (use_queue) {
        const ReadbackQueue::Job job{
            .src = readback.arena_handle,
            .dst = readback.download.buffer->Handle(),
            .copies = readback.copies,
        };
        readback.tick = readback_queue->Submit(std::span{&job, 1}, ReadbackWaits(writers));
        readback.on_readback_queue = true;
        wait_semaphore = &readback_queue->Semaphore();
    } else {
        WaitComputeWriter(writers);
        readback.tick = scheduler.CurrentTick();
        scheduler.MarkReadbackSession();
        scheduler.Flush();
        wait_semaphore = scheduler.GetWorkSemaphore();
    }
    Common::Perf::Add(Common::Perf::ReadbackSyncNs, Common::Perf::NowNs() - submit_start);
    const u64 wait_tick = readback.tick;
    pending_readbacks.push_back(std::move(readback));
    Common::Perf::Add(Common::Perf::ReadbackAsync);
    return wait_tick;
}

void BufferCache::RetireReadbacks() {
    if (pending_readbacks.empty()) {
        return;
    }
    // Cheap check of the known GPU progress first (called between the packets of the command
    // processor); the readback queue is queried when it has readbacks and none is known done.
    bool any_done = false;
    bool any_on_queue = false;
    for (const auto& readback : pending_readbacks) {
        if (readback.on_readback_queue) {
            any_on_queue = true;
            any_done = readback_queue->Semaphore().IsFree(readback.tick);
        } else {
            any_done = scheduler.GetWorkSemaphore()->IsFree(readback.tick);
        }
        if (any_done) {
            break;
        }
    }
    if (!any_done && any_on_queue) {
        readback_queue->Semaphore().Refresh();
        any_done = std::ranges::any_of(pending_readbacks, [this](const PendingReadback& readback) {
            return readback.on_readback_queue && readback_queue->Semaphore().IsFree(readback.tick);
        });
    }
    if (any_done) {
        CompleteReadbacks(false);
    }
}

void BufferCache::PrefetchReadbacks() {
    if (hot_windows.empty()) {
        return;
    }
    const auto& tuning = Common::Perf::GetTuning();
    // Only precise readbacks keep the pages protected until the data is written.
    if (!tuning.async_readbacks.load(std::memory_order_relaxed) ||
        !tuning.readback_prefetch.load(std::memory_order_relaxed) ||
        EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Precise) {
        return;
    }
    RetireReadbacks();

    // Windows the guest read back recently are likely read again after this fence: download what
    // the GPU wrote there now, in one submission. When the copies complete before the guest
    // reads, the pages are written and unprotected without any fault.
    constexpr u64 HotWindowLifetimeNs = 1'000'000'000ULL;
    constexpr u64 MaxPrefetchBytes = 8_MB;
    const u64 now = Common::Perf::NowNs();
    const u64 tick = scheduler.CurrentTick();
    // With the readback queue, a window whose writer is already submitted to the driver is
    // copied there, waiting only for that writer. The others (written by commands not submitted
    // yet) are copied by the command buffer being recorded, which is then submitted.
    const bool queue_enabled = UseReadbackQueue();
    const size_t first_queued = pending_readbacks.size();
    std::array<u64, 2> queue_wait_ticks{};
    bool recorded_on_queue = false;
    bool recorded_on_graphics = false;
    u64 prefetched_bytes = 0;
    for (auto it = hot_windows.begin(); it != hot_windows.end();) {
        if (prefetched_bytes >= MaxPrefetchBytes) {
            break;
        }
        if (now - it->last_read_ns > HotWindowLifetimeNs) {
            it = hot_windows.erase(it);
            continue;
        }
        HotWindow& window = *it;
        ++it;
        const VAddr start = window.start;
        const VAddr end = window.end;
        // One prefetch in flight per window, and only when the GPU wrote something there since.
        if ((window.prefetch_tick != 0 &&
             !(window.prefetch_on_queue
                   ? readback_queue->IsFree(window.prefetch_tick)
                   : scheduler.GetWorkSemaphore()->IsFree(window.prefetch_tick))) ||
            !gpu_modified_ranges.Intersects(start, end - start) ||
            !memory_tracker->IsRegionGpuModified(start, end - start)) {
            continue;
        }
        const u64 window_write_seq = memory_tracker->GetWriteSeq(start, end - start);
        if (KeepRanges() && window_write_seq <= window.prefetch_seq) {
            // readback_keep_ranges: the ranges stay known after a download, the writes since
            // the last prefetch tell whether there is anything new to download.
            continue;
        }
        const WindowWriters writers = WindowWritersOf(start, end - start);
        const bool on_queue = queue_enabled && writers.submitted;
        const auto* arena = GetArena(start >> block_shift, (end - 1) >> block_shift);
        PendingReadback readback{
            .tick = tick,
            .arena_base = arena->cpu_addr,
            .window_start = start,
            .window_end = end,
            .prefetch = true,
            .on_readback_queue = on_queue,
        };
        if (!(on_queue ? CollectReadback(arena, readback) : RecordReadback(arena, readback))) {
            memory_tracker->RemovePendingReadback(start, end);
            continue;
        }
        prefetched_bytes += readback.download.size;
        window.prefetch_tick = tick;
        window.prefetch_seq = window_write_seq;
        window.prefetch_on_queue = on_queue;
        if (on_queue) {
            queue_wait_ticks[0] = std::max(queue_wait_ticks[0], writers.ticks[0]);
            queue_wait_ticks[1] = std::max(queue_wait_ticks[1], writers.ticks[1]);
            recorded_on_queue = true;
        } else {
            WaitComputeWriter(writers);
            recorded_on_graphics = true;
        }
        pending_readbacks.push_back(std::move(readback));
        Common::Perf::Add(Common::Perf::ReadbackPrefetches);
    }
    if (recorded_on_graphics) {
        scheduler.MarkReadbackSession();
        scheduler.Flush();
        Common::Perf::Add(Common::Perf::ReadbackPrefetchFlushes);
    }
    if (!recorded_on_queue) {
        return;
    }
    // One submission for the windows of the readback queue, after the last graphics submission
    // that wrote them (all submitted to the driver already).
    boost::container::small_vector<ReadbackQueue::Job, 16> jobs;
    for (size_t i = first_queued; i < pending_readbacks.size(); ++i) {
        const auto& readback = pending_readbacks[i];
        if (!readback.on_readback_queue) {
            continue;
        }
        jobs.push_back(ReadbackQueue::Job{
            .src = readback.arena_handle,
            .dst = readback.download.buffer->Handle(),
            .copies = readback.copies,
        });
    }
    WindowWriters queue_writers;
    queue_writers.ticks = queue_wait_ticks;
    const u64 queue_tick = readback_queue->Submit(jobs, ReadbackWaits(queue_writers));
    for (size_t i = first_queued; i < pending_readbacks.size(); ++i) {
        auto& readback = pending_readbacks[i];
        if (!readback.on_readback_queue) {
            continue;
        }
        readback.tick = queue_tick;
        for (auto& window : hot_windows) {
            if (window.start == readback.window_start && window.end == readback.window_end) {
                window.prefetch_tick = queue_tick;
            }
        }
    }
}

void BufferCache::CompleteReadbacks(bool wait) {
    if (wait) {
        // All of them, in order.
        while (!pending_readbacks.empty()) {
            PendingReadback& readback = pending_readbacks.front();
            if (!IsReadbackDone(readback.on_readback_queue, readback.tick)) {
                const u64 sync_start = Common::Perf::NowNs();
                if (readback.on_readback_queue) {
                    readback_queue->Semaphore().Wait(readback.tick);
                } else {
                    scheduler.Wait(readback.tick);
                }
                Common::Perf::Add(Common::Perf::ReadbackSyncNs,
                                  Common::Perf::NowNs() - sync_start);
            }
            FinishReadback(0);
            pending_readbacks.pop_front();
        }
        return;
    }
    if (pending_readbacks.empty()) {
        return;
    }
    // The readbacks the GPU has finished, in any order (the graphics queue and the readback queue
    // progress independently), except after an unfinished earlier readback of the same window:
    // the later download has the newer data and must land last. The GPU progress of each queue
    // is queried once.
    scheduler.GetWorkSemaphore()->Refresh();
    if (readback_queue) {
        readback_queue->Semaphore().Refresh();
    }
    const auto done = [this](const PendingReadback& readback) {
        return readback.on_readback_queue ? readback_queue->Semaphore().IsFree(readback.tick)
                                          : scheduler.GetWorkSemaphore()->IsFree(readback.tick);
    };
    for (size_t i = 0; i < pending_readbacks.size();) {
        const PendingReadback& readback = pending_readbacks[i];
        bool blocked = false;
        for (size_t j = 0; j < i && !blocked; ++j) {
            const PendingReadback& earlier = pending_readbacks[j];
            blocked = earlier.window_start < readback.window_end &&
                      readback.window_start < earlier.window_end;
        }
        if (blocked || !done(readback)) {
            ++i;
            continue;
        }
        if (i != 0) {
            Common::Perf::Add(Common::Perf::ReadbackOutOfOrder);
        }
        FinishReadback(i);
        pending_readbacks.erase(pending_readbacks.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void BufferCache::FinishReadback(size_t index) {
    PendingReadback& readback = pending_readbacks[index];
    const u64 finish_start = Common::Perf::NowNs();
    if (KeepRanges()) {
        for (const auto& copy : readback.copies) {
            pending_download_ranges.Subtract(readback.arena_base + copy.srcOffset, copy.size);
        }
    }
    // Pages of the window still GPU modified. While the readback is pending, CPU writes do not
    // discard GPU data, so these include every page it downloaded.
    boost::container::small_vector<std::pair<VAddr, VAddr>, 16> pages;
    memory_tracker->ForEachDownloadRange<false>(
        readback.window_start, readback.window_end - readback.window_start,
        [&](u64 address, u64 range_size) { pages.emplace_back(address, address + range_size); });

    if (!readback.copies.empty()) {
        readback.download.Invalidate();
        for (const auto& copy : readback.copies) {
            const VAddr copy_start = readback.arena_base + copy.srcOffset;
            const VAddr copy_end = copy_start + copy.size;
            const u8* src = readback.download.mapped + (copy.dstOffset - readback.download.offset);
            for (const auto& [start, end] : pages) {
                const VAddr lo = std::max(copy_start, start);
                const VAddr hi = std::min(copy_end, end);
                if (lo < hi) {
                    memory->TryWriteBacking(std::bit_cast<u8*>(lo), src + (lo - copy_start),
                                            hi - lo);
                }
            }
        }
        // Guest memory changed under copies taken before (after the writes: a copy that saw the
        // new count saw the new data).
        NoteHostWrite();
    }

    // The pages are now up to date in guest memory, except those written again by the GPU since
    // the download was recorded, or downloaded by a later readback: they stay GPU modified.
    const bool keep_ranges = KeepRanges();
    const auto keep_page = [&](VAddr page) {
        if (keep_ranges ? memory_tracker->GetWriteSeq(page, BYTES_PER_PAGE) > readback.record_seq
                        : gpu_modified_ranges.Intersects(page, BYTES_PER_PAGE)) {
            return true;
        }
        for (size_t i = index + 1; i < pending_readbacks.size(); ++i) {
            const auto& later = pending_readbacks[i];
            if (page >= later.window_end || page + BYTES_PER_PAGE <= later.window_start) {
                continue;
            }
            for (const auto& copy : later.copies) {
                const VAddr start = later.arena_base + copy.srcOffset;
                if (page < start + copy.size && start < page + BYTES_PER_PAGE) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto unmark = [&](VAddr run_start, VAddr run_end) {
        memory_tracker->UnmarkRegionAsGpuModified(run_start, run_end - run_start, false);
        if (keep_ranges) {
            // Guest memory has these bytes now.
            gpu_modified_ranges.Subtract(run_start, run_end - run_start);
        }
    };
    for (const auto& [start, end] : pages) {
        VAddr run_start = start;
        for (VAddr page = start; page < end; page += BYTES_PER_PAGE) {
            if (keep_page(page)) {
                if (run_start < page) {
                    unmark(run_start, page);
                }
                run_start = page + BYTES_PER_PAGE;
            }
        }
        if (run_start < end) {
            unmark(run_start, end);
        }
    }
    // readback_relaxed_sites: pages a guest read was let through meanwhile (no longer GPU
    // modified) get the new data too, unless the guest or the GPU changed them since.
    if (!readback.relaxed_pages.empty() && !readback.copies.empty()) {
        bool refreshed_any = false;
        for (const VAddr page : readback.relaxed_pages) {
            const bool refreshed = memory_tracker->WithCleanPageLocked(page, [&] {
                for (const auto& copy : readback.copies) {
                    const VAddr copy_start = readback.arena_base + copy.srcOffset;
                    const VAddr lo = std::max(copy_start, page);
                    const VAddr hi = std::min(copy_start + copy.size, page + BYTES_PER_PAGE);
                    if (lo < hi) {
                        const u8* src =
                            readback.download.mapped + (copy.dstOffset - readback.download.offset);
                        memory->TryWriteBacking(std::bit_cast<u8*>(lo), src + (lo - copy_start),
                                                hi - lo);
                    }
                }
            });
            refreshed_any |= refreshed;
            Common::Perf::Add(refreshed ? Common::Perf::ReadbackRelaxedRefreshes
                                        : Common::Perf::ReadbackRelaxedSkips);
        }
        if (refreshed_any) {
            NoteHostWrite();
        }
    }
    // A write request marks its range CPU modified once its own wait is over (ReadMemory), only
    // if the range is no longer GPU modified: a page cannot be readable-protected but writable.
    memory_tracker->RemovePendingReadback(readback.window_start, readback.window_end);
    if (readback.download.buffer) {
        staging_pool.FreeDeferred(readback.download);
    }
    Common::Perf::Add(Common::Perf::ReadbackSyncNs, Common::Perf::NowNs() - finish_start);
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    {
        // Submits all recorded GPU work and waits for the GPU to become idle.
        const u64 sync_start = Common::Perf::NowNs();
        scheduler.MarkReadbackSession();
        FinishAllQueues();
        Common::Perf::Add(Common::Perf::ReadbackSyncNs, Common::Perf::NowNs() - sync_start);
        Common::Perf::Add(Common::Perf::ReadbackBytes, total_size_bytes);
    }

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    NoteHostWrite();
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
}

void BufferCache::PrefetchLookup(VAddr device_addr, u64 size) const noexcept {
    // The keys of ObtainBuffer (the size it is given is 32 bits).
    const u64 key = device_addr ^ (static_cast<u64>(static_cast<u32>(size)) << 40);
    const void* entry;
    size_t entry_size;
    if (size <= STREAM_THRESHOLD) {
        entry = &stream_copies[(key * 0x9E3779B97F4A7C15ULL) >> (64 - 12)];
        entry_size = sizeof(StreamCopy);
    } else {
        entry = &sync_memos[((key ^ 0x5bd1e995ULL) * 0x9E3779B97F4A7C15ULL) >> (64 - 12)];
        entry_size = sizeof(SyncMemo);
    }
    PrefetchLine(entry);
    PrefetchLine(static_cast<const u8*>(entry) + entry_size - 1);
    memory_tracker->PrefetchRegion(device_addr);
}

void BufferCache::PrefetchStreamSource(VAddr device_addr, u64 size) noexcept {
    if (size == 0 || size > STREAM_THRESHOLD) {
        return;
    }
    const auto& tuning = Common::Perf::GetTuning();
    // As ObtainBuffer: a copy reused, or pages bound in place, read nothing.
    if (tuning.stream_dedup.load(std::memory_order_relaxed)) {
        const u64 key = device_addr ^ (size << 40);
        const auto& reuse = stream_copies[(key * 0x9E3779B97F4A7C15ULL) >> (64 - 12)];
        if (reuse.address == device_addr && reuse.size == size &&
            reuse.tick == scheduler.CurrentTick() && reuse.epoch == CurrentUploadEpoch() &&
            reuse.host_write_gen == CurrentHostWriteGen()) {
            return;
        }
    }
    if (tuning.stream_static.load(std::memory_order_relaxed) &&
        memory_tracker->IsRegionCpuClean(
            device_addr, size, tuning.stream_static_frames.load(std::memory_order_relaxed))) {
        return;
    }
    constexpr u64 MaxLines = 16;
    const VAddr first = device_addr & ~VAddr{63};
    const u64 lines = std::min<u64>((device_addr + size - first + 63) / 64, MaxLines);
    for (u64 i = 0; i < lines; ++i) {
        PrefetchLine(reinterpret_cast<const void*>(first + i * 64));
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !HasGpuData(device_addr, size)) {
        const auto& tuning = Common::Perf::GetTuning();
        const u32 stable_frames = tuning.stream_static_frames.load(std::memory_order_relaxed);
        StreamCopy* reuse = nullptr;
        u64 epoch = 0;
        u64 host_write_gen = 0;
        if (tuning.stream_dedup.load(std::memory_order_relaxed)) {
            // A copy of this range made in this command buffer and epoch is reused first: the
            // cheapest answer (checking the GPU copy below reads the page tracking tables).
            // Read before copying: a change while copying makes the entry unusable.
            epoch = CurrentUploadEpoch();
            host_write_gen = CurrentHostWriteGen();
            const u64 key = device_addr ^ (static_cast<u64>(size) << 40);
            reuse = &stream_copies[(key * 0x9E3779B97F4A7C15ULL) >> (64 - 12)];
            if (reuse->address == device_addr && reuse->size == size &&
                reuse->queue == active->index && reuse->tick == Sched().CurrentTick() &&
                reuse->epoch == epoch && reuse->host_write_gen == host_write_gen) {
                Common::Perf::Add(Common::Perf::StreamReuses);
                Common::Perf::Add(Common::Perf::StreamReuseBytes, size);
                return {&Stream(), reuse->offset};
            }
        }
        if (tuning.stream_static.load(std::memory_order_relaxed)) {
            if (!is_texel_buffer &&
                memory_tracker->IsRegionCpuClean(device_addr, size, stable_frames)) {
                // The GPU copy of these pages is up to date: no copy or upload is needed to read
                // them there (the binding is then tracked for barriers like any arena range).
                const u64 first_block = device_addr >> block_shift;
                const u64 last_block = (device_addr + size - 1) >> block_shift;
                const auto* arena = GetArena(first_block, last_block);
                const u64 arena_offset = arena->Offset(device_addr);
                // Unless the range was written since the last barrier (an upload): reading it in
                // place would need a full pipeline barrier, a copy needs none.
                if (!Rt().IsBufferAccessed(arena, arena_offset, size)) {
                    Common::Perf::Add(Common::Perf::StreamArenaBinds);
                    EnsureResident(arena, first_block, last_block);
                    NoteQueueAccess(device_addr, size, false);
                    return {arena, arena_offset};
                }
                Common::Perf::Add(Common::Perf::StreamBarrierAvoided);
            }
        } else if ((++stream_clean_sample & 15) == 0 && !is_texel_buffer &&
                   memory_tracker->IsRegionCpuClean(device_addr, size, stable_frames)) {
            // Statistics only (one binding in 16): copies stream_static would avoid.
            Common::Perf::Add(Common::Perf::StreamCleanCopies, 16);
        }
        StreamBuffer& stream = Stream();
        const auto [data, offset] = stream.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream.Commit();
        Common::Perf::Add(Common::Perf::StreamCopies);
        Common::Perf::Add(Common::Perf::StreamCopyBytes, size);
        if (reuse) {
            // The copy stays valid until the command buffer it was committed in completes.
            *reuse = StreamCopy{
                .address = device_addr,
                .tick = Sched().CurrentTick(),
                .epoch = epoch,
                .host_write_gen = host_write_gen,
                .offset = offset,
                .size = size,
                .queue = active->index,
            };
        }
        return {&stream, offset};
    }
    if (const u32 probe = Common::Perf::GetTuning().probe_host_import.load(
            std::memory_order_relaxed);
        probe != probed_host_import) [[unlikely]] {
        probed_host_import = probe;
        if (probe != 0) {
            ProbeHostImport(device_addr);
        }
    }
    // Sync memo: a read only range bound again while no guest write touched its regions since
    // it was last found up to date needs no synchronization (large vertex and index buffers
    // bound by most draws; the checks walk the page tracking tables of the whole range).
    const auto& tuning = Common::Perf::GetTuning();
    const bool memo_candidate = !is_written && !is_texel_buffer &&
                                tuning.sync_memo.load(std::memory_order_relaxed) &&
                                tuning.hot_page_faults.load(std::memory_order_relaxed) == 0;
    SyncMemo* memo = nullptr;
    std::array<u64, 2> versions{};
    bool have_versions = false;
    if (memo_candidate) {
        const u64 key = device_addr ^ (static_cast<u64>(size) << 40) ^ 0x5bd1e995ULL;
        memo = &sync_memos[(key * 0x9E3779B97F4A7C15ULL) >> (64 - 12)];
        have_versions = memory_tracker->GetCpuVersions(device_addr, size, versions);
        if (have_versions && memo->address == device_addr && memo->size == size &&
            memo->versions == versions) {
            Common::Perf::Add(Common::Perf::SyncMemoHits);
            NoteQueueAccess(device_addr, size, false);
            return {memo->arena, memo->arena->Offset(device_addr)};
        }
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (is_texel_buffer && !is_written && !OnComputeQueue()) {
        SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    // Cross queue dependencies before the write tick of this access is recorded.
    NoteQueueAccess(device_addr, size, is_written);
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
        // The command buffer being recorded writes the range (readback queue waits for it).
        memory_tracker->SetWriteTick(device_addr, size, Sched().CurrentTick(), active->index);
    } else if (memo && have_versions) {
        // Versions read before the synchronization: a write during it changes them.
        Common::Perf::Add(Common::Perf::SyncMemoMisses);
        *memo = SyncMemo{
            .address = device_addr,
            .arena = arena,
            .versions = versions,
            .size = size,
        };
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (HasGpuData(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

std::pair<const Buffer*, u64> BufferCache::PeekArena(VAddr device_addr, u32 size) {
    if (size == 0 || device_addr == 0) {
        return {nullptr, 0};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    if (!IsResident(first_block, last_block)) {
        return {nullptr, 0};
    }
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    const auto* arena = address_space[first_page];
    if (!arena || address_space[last_page] != arena) {
        return {nullptr, 0};
    }
    return {arena, arena->Offset(device_addr)};
}

std::string BufferCache::DescribePages(VAddr device_addr, u32 size) {
    if (size == 0 || device_addr == 0) {
        return "none";
    }
    const bool merge =
        Common::Perf::GetTuning().readback_write_merge.load(std::memory_order_relaxed);
    return fmt::format("cpu{} gpu{} ranges{} owned{} pending{} tick{}",
                       memory_tracker->IsRegionCpuModified(device_addr, size) ? 1 : 0,
                       memory_tracker->IsRegionGpuModified(device_addr, size) ? 1 : 0,
                       gpu_modified_ranges.Intersects(device_addr, size) ? 1 : 0,
                       merge && memory_tracker->IsGpuOwned(device_addr, size) ? 1 : 0,
                       memory_tracker->IsReadbackPending(device_addr, size) ? 1 : 0,
                       memory_tracker->GetWriteTick(device_addr, size));
}

bool BufferCache::HasGpuData(VAddr addr, size_t size) {
    if (memory_tracker->IsRegionGpuModified(addr, size)) {
        return true;
    }
    // Write merge: bytes the GPU wrote in pages whose GPU state a guest write discarded are only
    // on the GPU (whether or not their ranges are still known: a page stays owned until an
    // upload finds nothing of the GPU's to keep in it).
    return Common::Perf::GetTuning().readback_write_merge.load(std::memory_order_relaxed) &&
           memory_tracker->IsGpuOwned(addr, size);
}

void CopyGuestPage(VAddr page, u8* dest) {
    Core::Memory::Instance()->CopySparseMemory(page, dest, BYTES_PER_PAGE);
}

void BufferCache::SynchronizeDmaBuffers() {
    fault_process_pending = true;
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << ARENA_PAGE_BITS, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << ARENA_PAGE_BITS);
    const u64 first_size = first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE;
    const u64 last_size = last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> ARENA_PAGE_BITS;
    for (u32 page = 0; page < (first_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

static constexpr u64 BitsMask(u64 first_bit, u64 count) {
    return (count == 64 ? ~u64{0} : ((u64{1} << count) - 1)) << first_bit;
}

bool BufferCache::IsResident(u64 first_block, u64 last_block) const {
    const u64 first_word = first_block / 64;
    const u64 last_word = last_block / 64;
    if (first_word == last_word) {
        const u64 mask = BitsMask(first_block % 64, last_block - first_block + 1);
        return (resident_bits[first_word] & mask) == mask;
    }
    const u64 first_mask = BitsMask(first_block % 64, 64 - first_block % 64);
    if ((resident_bits[first_word] & first_mask) != first_mask) {
        return false;
    }
    const u64 last_mask = BitsMask(0, last_block % 64 + 1);
    if ((resident_bits[last_word] & last_mask) != last_mask) {
        return false;
    }
    // The words in between must be fully resident: a bit per word (bindings covering the whole
    // guest memory span tens of thousands of blocks).
    for (u64 word = first_word + 1; word < last_word;) {
        const u64 bit = word % 64;
        const u64 count = std::min<u64>(64 - bit, last_word - word);
        const u64 mask = BitsMask(bit, count);
        if ((resident_full[word / 64] & mask) != mask) {
            return false;
        }
        word += count;
    }
    return true;
}

void BufferCache::MarkResident(u64 first_block, u64 last_block) {
    for (u64 block = first_block; block <= last_block;) {
        const u64 bit = block % 64;
        const u64 count = std::min<u64>(64 - bit, last_block - block + 1);
        const u64 word = block / 64;
        resident_bits[word] |= BitsMask(bit, count);
        if (resident_bits[word] == ~u64{0}) {
            resident_full[word / 64] |= u64{1} << (word % 64);
        }
        block += count;
    }
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    if (IsResident(first_block, last_block)) {
        return;
    }
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });
    // Every block of the range is resident once this returns.
    MarkResident(first_block, last_block);

    if (bind_ranges.Empty()) {
        return;
    }

    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        Staging().Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset >> block_shift;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    // The page table is read by the shaders of both queues: a copy on the active queue, waited
    // for by the other one when it reads it next (NoteGdsAccess tracks it like the GDS).
    Rt().CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
    if (compute_queue != nullptr) {
        bda_write_tick[active->index] = Sched().CurrentTick();
    }
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    const auto& tuning = Common::Perf::GetTuning();
    const bool hash_hot_pages = tuning.hot_page_faults.load(std::memory_order_relaxed) != 0 &&
                                tuning.hot_page_hash.load(std::memory_order_relaxed);
    const auto hash_page = [this](VAddr page_addr) {
        return memory->HashSparseMemory(page_addr, BYTES_PER_PAGE);
    };
    if (hash_hot_pages) {
        // Hot pages unchanged since their last upload need none in this epoch.
        memory_tracker->SkipUnchangedHotPages(device_addr, size, hash_page);
    }
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const auto add_copy = [&](VAddr addr, u64 copy_size) {
        copies.emplace_back(total_size_bytes, addr, copy_size);
        total_size_bytes += copy_size;
    };
    // Write merge: pages whose GPU data a guest write discarded only upload the bytes outside
    // those the GPU wrote, and those the guest changed since (compared with the content they
    // had before that write).
    const bool merge = tuning.readback_write_merge.load(std::memory_order_relaxed) &&
                       memory_tracker->HasPageSnapshots();
    boost::container::small_vector<std::pair<VAddr, VAddr>, 4> guest_owned;
    // Upload worker: the pages uploaded here are protected again by the worker, which copies
    // them only afterwards (no write merge or hot pages). Ranges the GPU writes too
    // (async_written_uploads): HK must apply read protection now. Translated fences can let
    // the guest read particle feedback before the worker applies a deferred protection;
    // that read would see stale CPU bytes and never request the required GPU readback.
    const bool async_upload =
        !merge && !hash_hot_pages && tuning.hot_page_faults.load(std::memory_order_relaxed) == 0 &&
        tuning.async_uploads.load(std::memory_order_relaxed) &&
        (!is_written || tuning.async_written_uploads.load(std::memory_order_relaxed));
    // Keep the original policy for other regions; staging copies still run on the worker.
    const bool defer_protection = async_upload &&
        !(is_written && Common::ElfInfo::Instance().GameSerial() == "CUSA00309");
    if (async_upload) {
        deferred_pages.clear();
        if (defer_protection) PageManager::DeferProtect(&deferred_pages);
    }
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written, [&](u64 addr, u64 size, auto&& keep_gpu_owned) {
            if (!merge) {
                add_copy(addr, size);
                return;
            }
            VAddr run_start = addr;
            const VAddr end = addr + size;
            PageSnapshot before;
            for (VAddr page = addr; page < end; page += BYTES_PER_PAGE) {
                if (!memory_tracker->TakePageSnapshot(page, before)) {
                    continue;
                }
                boost::container::small_vector<std::pair<VAddr, VAddr>, 8> gpu_bytes;
                gpu_modified_ranges.ForEachInRange(page, BYTES_PER_PAGE, [&](VAddr lo, VAddr hi) {
                    gpu_bytes.emplace_back(lo, hi);
                });
                if (gpu_bytes.empty()) {
                    continue;
                }
                if (run_start < page) {
                    add_copy(run_start, page - run_start);
                }
                run_start = page + BYTES_PER_PAGE;
                Common::Perf::Add(Common::Perf::MergePages);
                PageSnapshot now;
                CopyGuestPage(page, now.data());
                VAddr cursor = page;
                u64 kept = 0;
                for (const auto [lo, hi] : gpu_bytes) {
                    if (cursor < lo) {
                        add_copy(cursor, lo - cursor);
                    }
                    // Inside bytes the GPU wrote: only those the guest changed are uploaded.
                    u64 i = lo - page;
                    const u64 last = hi - page;
                    while (i < last) {
                        const u64 same_start = i;
                        while (i < last && now[i] == before[i]) {
                            ++i;
                        }
                        kept += i - same_start;
                        const u64 changed_start = i;
                        while (i < last && now[i] != before[i]) {
                            ++i;
                        }
                        if (i != changed_start) {
                            add_copy(page + changed_start, i - changed_start);
                            guest_owned.emplace_back(page + changed_start, page + i);
                            Common::Perf::Add(Common::Perf::MergeGuestBytes, i - changed_start);
                        }
                    }
                    cursor = hi;
                }
                if (cursor < page + BYTES_PER_PAGE) {
                    add_copy(cursor, page + BYTES_PER_PAGE - cursor);
                }
                if (kept != 0) {
                    Common::Perf::Add(Common::Perf::MergeKeptBytes, kept);
                    keep_gpu_owned(page);
                }
            }
            if (run_start < end) {
                add_copy(run_start, end - run_start);
            }
        });
    if (defer_protection) {
        PageManager::DeferProtect(nullptr);
    }
    for (const auto [lo, hi] : guest_owned) {
        // The guest wrote these bytes after the GPU: guest memory has their current value.
        gpu_modified_ranges.Subtract(lo, hi - lo);
    }
    if (async_upload && (!copies.empty() || !deferred_pages.empty())) {
        Common::Perf::ScopedTimer upload_timer{Common::Perf::BufferUploads,
                                               Common::Perf::BufferUploadNs};
        Common::Perf::Add(Common::Perf::BufferUploadBytes, total_size_bytes);
        UploadWorker::Job job;
        job.pages = deferred_pages;
        if (!copies.empty()) {
            const auto staging = Staging().Request(total_size_bytes, MemoryType::HostUncached);
            job.copies.reserve(copies.size());
            for (auto& copy : copies) {
                job.copies.push_back(UploadWorker::Copy{
                    .src = copy.dstOffset,
                    .dst = staging.mapped + copy.srcOffset,
                    .size = copy.size,
                });
                copy.srcOffset += staging.offset;
                copy.dstOffset -= arena->cpu_addr;
            }
            job.flush_buffer = staging.buffer;
            job.flush_offset = staging.offset;
            job.flush_size = staging.size;
            // The worker writes the staging memory before the submission of these copies.
            Rt().CopyBuffer(staging.buffer, arena, copies);
            NoteQueueUpload(device_addr, size);
        }
        Sched().RequireBeforeSubmit(upload_worker->Enqueue(std::move(job)));
    } else if (!copies.empty()) {
        Common::Perf::ScopedTimer upload_timer{Common::Perf::BufferUploads,
                                               Common::Perf::BufferUploadNs};
        Common::Perf::Add(Common::Perf::BufferUploadBytes, total_size_bytes);
        const auto staging = Staging().Request(total_size_bytes, MemoryType::HostUncached);
        for (auto& copy : copies) {
            memory->CopySparseMemory(copy.dstOffset, staging.mapped + copy.srcOffset, copy.size);
            if (hash_hot_pages) {
                // The hashes must describe what was uploaded.
                memory_tracker->VerifyHotPageHashes(copy.dstOffset, copy.size, hash_page);
            }
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        staging.Flush();
        Rt().CopyBuffer(staging.buffer, arena, copies);
        NoteQueueUpload(device_addr, size);
    }
    if (is_texel_buffer && !is_written && !OnComputeQueue()) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

void BufferCache::NoteQueueUpload(VAddr device_addr, u64 size) {
    // A GPU write of the range: the other queue must be done reading and writing it (the guest
    // changed the data once its own fences told it the GPU was done; those fences are signaled
    // when the commands are translated, before the GPU runs them).
    NoteQueueAccess(device_addr, size, true);
}

void BufferCache::ProbeHostImport(VAddr address) {
    if (!instance.HasExternalMemoryHost()) {
        LOG_INFO(Render_Vulkan, "Host import probe: VK_EXT_external_memory_host not available");
        return;
    }
    const auto device = instance.GetDevice();
    const auto properties =
        instance.GetPhysicalDevice()
            .getProperties2<vk::PhysicalDeviceProperties2,
                            vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>();
    const u64 alignment = std::max<u64>(
        properties.get<vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>()
            .minImportedHostPointerAlignment,
        4_KB);
    const u64 size = Common::AlignUp(64_KB, alignment);
    const VAddr base = Common::AlignDown(address, alignment);
    // The guest mapping of the memory (protected pages included), and its backing (the same
    // physical memory, never protected), which a GPU copy in place would use.
    u8* backing = nullptr;
    if (const auto* entry = memory->GetAddressSpace().BackingPages().find(base >> 14);
        entry && *entry) {
        backing = *entry + (base & (16_KB - 1));
    }
    const auto memory_properties = instance.GetPhysicalDevice().getMemoryProperties();
    const auto probe = [&](const char* what, void* pointer) {
        if (pointer == nullptr) {
            LOG_INFO(Render_Vulkan, "Host import probe {}: no address", what);
            return;
        }
        vk::MemoryHostPointerPropertiesEXT host_properties{};
        const vk::Result result = device.getMemoryHostPointerPropertiesEXT(
            vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, pointer, &host_properties);
        LOG_INFO(Render_Vulkan,
                 "Host import probe {}: {} at {} (alignment {:#x}): properties {}, memory types "
                 "{:#x}",
                 what, size, fmt::ptr(pointer), alignment, vk::to_string(result),
                 host_properties.memoryTypeBits);
        if (result != vk::Result::eSuccess || host_properties.memoryTypeBits == 0) {
            return;
        }
        u32 type_index = std::countr_zero(host_properties.memoryTypeBits);
        for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
            const auto flags = memory_properties.memoryTypes[i].propertyFlags;
            if ((host_properties.memoryTypeBits >> i & 1) != 0 &&
                (flags & vk::MemoryPropertyFlagBits::eHostCoherent)) {
                type_index = i;
                break;
            }
        }
        const vk::ImportMemoryHostPointerInfoEXT import_info = {
            .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
            .pHostPointer = pointer,
        };
        const vk::MemoryAllocateInfo alloc_info = {
            .pNext = &import_info,
            .allocationSize = size,
            .memoryTypeIndex = type_index,
        };
        vk::DeviceMemory imported{};
        const vk::Result alloc_result = device.allocateMemory(&alloc_info, nullptr, &imported);
        LOG_INFO(Render_Vulkan, "Host import probe {}: import into memory type {} ({}): {}",
                 what, type_index,
                 vk::to_string(memory_properties.memoryTypes[type_index].propertyFlags),
                 vk::to_string(alloc_result));
        if (alloc_result == vk::Result::eSuccess) {
            device.freeMemory(imported);
        }
    };
    probe("guest mapping", reinterpret_cast<void*>(base));
    probe("backing", backing);
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    if (submit_result == vk::Result::eErrorDeviceLost) {
        Vulkan::GpuCrashDiag::ReportDeviceLost(instance, "sparse binding");
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
