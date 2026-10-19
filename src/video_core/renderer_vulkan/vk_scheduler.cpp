// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include "common/assert.h"
#include "common/debug.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "core/runtime_policy.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
/// Tells the CPU this thread is spinning (cheaper for the other hyperthread of its core).
inline void CpuRelax() {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}
} // Anonymous namespace

std::mutex Scheduler::submit_mutex;

// Submit callbacks can append barriers while holding the shared queue mutex. They must not
// block for recorder space: a recorder submission may need that same mutex to make room.
static thread_local bool in_submit_callback{};

Scheduler::Scheduler(const Instance& instance, vk::Queue queue_, const char* name_)
    : instance{instance}, queue{queue_ ? queue_ : instance.GetGraphicsQueue()}, name{name_},
      work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    BeginSession();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (recorder_thread.joinable()) {
        SyncRecorder();
        recorder_thread.request_stop();
        recorder_thread.join();
    }
}

void Scheduler::AllowThreadedRecording() {
    if (threading_allowed) {
        return;
    }
    threading_allowed = true;
    recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
}

vk::CommandBuffer Scheduler::CommandBuffer() {
    // The caller records in the command buffer without going through CommandRecorder: what it
    // binds is not known here.
    session_used = true;
    dynamic_state.binds.Forget();
    if (!direct_cmdbuf) {
        // The session is recorded by the recording thread: once it has recorded what it was
        // given, it is idle and the command buffer is recorded here until the submission.
        SyncRecorder();
        direct_cmdbuf = RecorderCommandBuffer();
    }
    return direct_cmdbuf;
}

CommandStream& Scheduler::Stream() {
    session_used = true;
    // Batches of commands go to the recording thread while the next ones are recorded.
    static constexpr size_t DispatchCommands = 256;
    if (stream->NumCommands() >= DispatchCommands && !in_submit_callback) {
        DispatchStream();
    }
    return *stream;
}

void Scheduler::DispatchStream() {
    if (!stream || stream->Empty()) {
        return;
    };
    std::unique_ptr<CommandStream> next;
    bool wake;
    {
        std::unique_lock lk{recorder_mutex};
        WaitForRecorderRoom(lk);
        recorder_jobs.push_back(RecorderJob{.stream = std::move(stream)});
        recorder_queued.fetch_add(1, std::memory_order_release);
        if (!free_streams.empty()) {
            next = std::move(free_streams.back());
            free_streams.pop_back();
        }
        wake = recorder_waiting;
    }
    if (wake) {
        // Only a sleeping recording thread needs to be woken up (a system call).
        recorder_cv.notify_one();
    }
    stream = next ? std::move(next) : std::make_unique<CommandStream>();
}

void Scheduler::WaitForRecorderRoom(std::unique_lock<std::mutex>& lock) {
    // A first-use compile can hold the recorder at a retained bind. Bound the producer's
    // lead to 64 batches (normally 256 commands each), instead of retaining unlimited frames.
    static constexpr size_t MaxQueuedBatches = 64;
    if (Core::Runtime::GetPolicy().compute_compile_parallel &&
        recorder_jobs.size() >= MaxQueuedBatches) {
        recorder_space_cv.wait(lock, [this] { return recorder_jobs.size() < MaxQueuedBatches; });
    }
}

void Scheduler::SyncRecorder() {
    if (!recorder_thread.joinable()) {
        return;
    }
    DispatchStream();
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return recorder_jobs.empty() && !recorder_busy; });
}

vk::CommandBuffer Scheduler::RecorderCommandBuffer() {
    if (!recorder_cmdbuf) {
        recorder_cmdbuf = command_pool.Commit();
        Check(recorder_cmdbuf.begin(vk::CommandBufferBeginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        }));
    }
    return recorder_cmdbuf;
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandRecorder");
    while (true) {
        // Optionally wait a little for the next commands before sleeping: the command processor
        // then rarely has to wake this thread up.
        const u32 spin_us = Core::Runtime::GetPolicy().recorder_spin_us;
        if (spin_us != 0 && recorder_queued.load(std::memory_order_acquire) == 0) {
            const u64 deadline = Core::Runtime::NowNs() + u64{spin_us} * 1000;
            while (recorder_queued.load(std::memory_order_acquire) == 0 &&
                   !stoken.stop_requested() && Core::Runtime::NowNs() < deadline) {
                for (u32 i = 0; i < 16; ++i) {
                    CpuRelax();
                }
            }
        }
        RecorderJob job;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_waiting = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_jobs.empty(); });
            recorder_waiting = false;
            if (recorder_jobs.empty()) {
                return;
            }
            job = std::move(recorder_jobs.front());
            recorder_jobs.pop_front();
            recorder_queued.fetch_sub(1, std::memory_order_relaxed);
            recorder_busy = true;
        }
        recorder_space_cv.notify_one();
        if (job.stream && !job.stream->Empty()) {
            job.stream->Execute(RecorderCommandBuffer());
        }
        if (job.submit) {
            const vk::CommandBuffer cmdbuf = RecorderCommandBuffer();
            Check(cmdbuf.end());
            recorder_cmdbuf = vk::CommandBuffer{};
            std::scoped_lock lk{submit_mutex};
            SubmitToQueue(std::span{&cmdbuf, 1}, job.info);
        };
        {
            std::scoped_lock lk{recorder_mutex};
            if (job.stream) {
                job.stream->Reset();
                free_streams.push_back(std::move(job.stream));
            }
            recorder_busy = false;
            if (recorder_jobs.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    const CommandRecorder cmdbuf = Recorder();
    cmdbuf.beginRendering(rendering_info);
    if (rendering_observer) {
        rendering_observer->OnBeginRendering(cmdbuf);
    }
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    const CommandRecorder cmdbuf = Recorder();
    if (rendering_observer) {
        rendering_observer->OnEndRendering(cmdbuf);
    }
    is_rendering = false;
    cmdbuf.endRendering();
}

vk::CommandBuffer Scheduler::UploadCommandBuffer() {
    auto& upload_cmdbuf = sessions.back().upload;
    if (upload_cmdbuf) {
        return upload_cmdbuf;
    }
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    upload_cmdbuf = command_pool.Commit();
    Check(upload_cmdbuf.begin(begin_info));
    return upload_cmdbuf;
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::Wait(u64 tick) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    work_semaphore.Wait(tick);
    // Only count waits where the CPU actually blocked on the GPU.
}

void Scheduler::PopPendingOperations() {
    // Called before every draw: the lock and the semaphore query (a driver call) only when
    // there is something to pop.
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::unique_lock lk(pending_ops_mutex);
    work_semaphore.Refresh();
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::BeginSession() {
    EndSession();

    // Threaded recording uses the fixed V2 policy.
    stream_session = threading_allowed && Core::Runtime::GetPolicy().threaded_recording;
    if (stream_session) {
        // The recording thread allocates and records the command buffer of the session.
        recorder_used = true;
        direct_cmdbuf = vk::CommandBuffer{};
        if (!stream) {
            stream = std::make_unique<CommandStream>();
        }
        sessions.emplace_back();
    } else {
        if (std::exchange(recorder_used, false)) {
            // The recording thread may still use the command pool for earlier sessions.
            SyncRecorder();
        }
        auto& session = sessions.emplace_back();

        const vk::CommandBufferBeginInfo begin_info = {
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        };
        session.primary = command_pool.Commit();
        Check(session.primary.begin(begin_info));
        direct_cmdbuf = session.primary;
    }

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();
    session_used = false;
}

void Scheduler::EndSession() {
    if (sessions.empty()) {
        return;
    }

    if (on_session) {
        on_session();
    }

    const auto& session = sessions.back();
    if (session.upload) {
        Check(session.upload.end());
    }

    EndRendering();
    if (session.primary) {
        Check(session.primary.end());
    }
}

void Scheduler::AddCrossWait(vk::Semaphore semaphore, u64 tick) {
    for (auto& wait : cross_waits) {
        if (wait.semaphore == semaphore) {
            wait.tick = std::max(wait.tick, tick);
            return;
        }
    }
    ASSERT_MSG(cross_waits.size() < cross_waits.capacity(), "Too many cross queue waits");
    cross_waits.push_back(CrossWait{.semaphore = semaphore, .tick = tick});
}

u64 Scheduler::CrossWaitTick(vk::Semaphore semaphore) const noexcept {
    for (const auto& wait : cross_waits) {
        if (wait.semaphore == semaphore) {
            return wait.tick;
        }
    }
    return 0;
}

void Scheduler::TakeCrossWaits(SubmitInfo& info) {
    for (const auto& wait : cross_waits) {
        info.AddWait(wait.semaphore, wait.tick);
    }
    cross_waits.clear();
}

void Scheduler::WaitSubmitted(u64 tick) {
    if (submitted_tick.load(std::memory_order_acquire) >= tick) {
        return;
    }
    if (tick >= work_semaphore.CurrentTick()) {
        // Not flushed yet: the caller flushes first.
        Flush();
    }
    std::unique_lock lk{submitted_mutex};
    submitted_cv.wait(lk, [&] { return submitted_tick.load(std::memory_order_acquire) >= tick; });
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    // Work of other threads the commands of the session read: done before they are submitted.
    info.upload_dependency =
        std::max(info.upload_dependency, submit_dependency.exchange(0, std::memory_order_acq_rel));
    // Work of other queues the commands of the session depend on.
    TakeCrossWaits(info);
    if (stream_session) {
        // Threaded recording: the recording thread submits the session after recording it.
        const u64 signal_value = work_semaphore.NextTick();
        {
            std::scoped_lock lk{submit_mutex};
            if (on_submit) {
                const bool previous = std::exchange(in_submit_callback, true);
                SCOPE_EXIT { in_submit_callback = previous; };
                on_submit(info);
            }
        }
        EndSession();
        sessions.clear();
        info.AddSignal(work_semaphore.Handle(), signal_value);
        if (direct_cmdbuf) {
            // CommandBuffer() made the end of the session direct: the recording thread is idle.
            Check(direct_cmdbuf.end());
            recorder_cmdbuf = vk::CommandBuffer{};
            std::scoped_lock lk{submit_mutex};
            SubmitToQueue(std::span{&direct_cmdbuf, 1}, info);
        } else {
            std::unique_ptr<CommandStream> next;
            bool wake;
            {
                std::unique_lock lk{recorder_mutex};
                WaitForRecorderRoom(lk);

                recorder_jobs.push_back(RecorderJob{
                    .stream = std::move(stream),
                    .submit = true,
                    .info = info,
                });
                recorder_queued.fetch_add(1, std::memory_order_release);
                if (!free_streams.empty()) {
                    next = std::move(free_streams.back());
                    free_streams.pop_back();
                }
                wake = recorder_waiting;
            }
            if (wake) {
                recorder_cv.notify_one();
            }
            stream = next ? std::move(next) : std::make_unique<CommandStream>();
        }
        direct_cmdbuf = vk::CommandBuffer{};
        BeginSession();
        PopPendingOperations();
        return;
    }

    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = work_semaphore.NextTick();

    if (on_submit) {
        const bool previous = std::exchange(in_submit_callback, true);
        SCOPE_EXIT { in_submit_callback = previous; };
        on_submit(info);
    }

    EndSession();

    std::vector<vk::CommandBuffer> cmd_buffers;
    cmd_buffers.reserve(sessions.size() * 2);

    for (const auto& session : sessions) {
        if (session.upload) {
            cmd_buffers.push_back(session.upload);
        }
        cmd_buffers.push_back(session.primary);
    }
    sessions.clear();

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    SubmitToQueue(cmd_buffers, info);

    BeginSession();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::SubmitToQueue(std::span<const vk::CommandBuffer> cmd_buffers, SubmitInfo& info) {
    if (info.upload_dependency != 0 && submit_dependency_wait) {
        // The GPU reads what the upload worker writes: wait until it is written.
        submit_dependency_wait(info.upload_dependency);
        info.upload_dependency = 0;
    }

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = info.wait_stages.data(),
        .commandBufferCount = static_cast<u32>(cmd_buffers.size()),
        .pCommandBuffers = cmd_buffers.data(),
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    if (queue == instance.GetGraphicsQueue()) {
        ImGui::Core::TextureManager::Submit();
    }
    auto submit_result = queue.submit(submit_info, info.fence);

    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit ({})",
               name);
    // The tick these command buffers signal is now submitted (under submit_mutex: in order).
    for (u32 i = 0; i < info.num_signal_semas; ++i) {
        if (info.signal_semas[i] == work_semaphore.Handle()) {
            const u64 tick = info.signal_ticks[i];
            if (tick > submitted_tick.load(std::memory_order_relaxed)) {
                std::scoped_lock lk{submitted_mutex};
                submitted_tick.store(tick, std::memory_order_release);
                submitted_cv.notify_all();
            }
        }
    }

    work_semaphore.Refresh();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

namespace {
bool SkipRedundantBinds() noexcept { return Core::Runtime::GetPolicy().skip_redundant_binds; }
} // Anonymous namespace

bool DynamicState::BindCache::SkipIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset,
                                              vk::IndexType type) noexcept {
    if (!buffers.NoteIndex(buffer, offset, type)) {
        return false;
    };
    return true;
}

bool DynamicState::BindCache::SkipVertexBuffers(u32 first, u32 count, const vk::Buffer* values,
                                                const vk::DeviceSize* offsets, bool sized_api,
                                                const vk::DeviceSize* sizes,
                                                const vk::DeviceSize* strides) noexcept {
    if (!buffers.NoteVertices(first, count, values, offsets, sized_api, sizes, strides)) {
        return false;
    };
    return true;
}

bool DynamicState::BindCache::SkipPipeline(vk::PipelineBindPoint bind_point,
                                           vk::Pipeline pipeline) noexcept {
    u32 index;
    if (bind_point == vk::PipelineBindPoint::eGraphics) {
        index = 0;
    } else if (bind_point == vk::PipelineBindPoint::eCompute) {
        index = 1;
    } else {
        return false;
    };
    if (pipeline && pipelines[index] == pipeline) {
        if (SkipRedundantBinds()) {
            return true;
        }
        return false;
    }
    pipelines[index] = pipeline;
    if (index == 0) {
        // Another graphics pipeline: the vertex input is only left out under the same pipeline,
        // which leaves no doubt about the state the command buffer keeps for it.
        vertex_input_known = false;
    }
    return false;
}

bool DynamicState::BindCache::SkipPush(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                       u32 offset, u32 size, const void* values) noexcept {
    const u32 index = (stages & vk::ShaderStageFlagBits::eCompute) ? 1 : 0;
    Push& push = pushes[index];
    if (size == 0 || size > MaxPushBytes) {
        push.size = 0;
        return false;
    }
    if (push.size == size && push.layout == layout && push.stages == stages &&
        push.offset == offset && std::memcmp(push.data.data(), values, size) == 0) {
        if (SkipRedundantBinds()) {
            return true;
        }
        return false;
    }
    push.layout = layout;
    push.stages = stages;
    push.offset = offset;
    push.size = size;
    std::memcpy(push.data.data(), values, size);
    return false;
}

bool DynamicState::BindCache::SkipDescriptors(
    vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
    std::span<const vk::WriteDescriptorSet> writes) noexcept {
    u32 index;
    if (bind_point == vk::PipelineBindPoint::eGraphics) {
        index = 0;
    } else if (bind_point == vk::PipelineBindPoint::eCompute) {
        index = 1;
    } else {
        return false;
    }
    Descriptors& cache = descriptors[index];
    // The infos of a write as compared and kept: what the descriptor points to.
    const auto info_at = [](const vk::WriteDescriptorSet& write, u32 i) -> Descriptors::Info {
        switch (write.descriptorType) {
        case vk::DescriptorType::eSampler:
        case vk::DescriptorType::eCombinedImageSampler:
        case vk::DescriptorType::eSampledImage:
        case vk::DescriptorType::eStorageImage:
        case vk::DescriptorType::eInputAttachment: {
            const auto& info = write.pImageInfo[i];
            return {reinterpret_cast<u64>(static_cast<VkSampler>(info.sampler)),
                    reinterpret_cast<u64>(static_cast<VkImageView>(info.imageView)),
                    static_cast<u64>(info.imageLayout)};
        }
        case vk::DescriptorType::eUniformTexelBuffer:
        case vk::DescriptorType::eStorageTexelBuffer:
            return {reinterpret_cast<u64>(static_cast<VkBufferView>(write.pTexelBufferView[i])), 0,
                    0};
        default: {
            const auto& info = write.pBufferInfo[i];
            return {reinterpret_cast<u64>(static_cast<VkBuffer>(info.buffer)), info.offset,
                    info.range};
        }
        }
    };
    // Only plain writes of set 0 are kept (no extension structures, bounded sizes).
    bool cacheable = set == 0 && writes.size() <= Descriptors::MaxWrites;
    u32 total_infos = 0;
    for (const auto& write : writes) {
        if (write.pNext != nullptr) {
            cacheable = false;
            break;
        }
        total_infos += write.descriptorCount;
    }
    if (!cacheable || total_infos > Descriptors::MaxInfos) {
        // Another set pushed with an incompatible layout can disturb set 0 too.
        cache.num_writes = ~0U;
        return false;
    }
    bool same = cache.num_writes == writes.size() && cache.layout == layout &&
                cache.num_infos == total_infos;
    if (same) {
        u32 info_index = 0;
        for (u32 w = 0; w < writes.size() && same; ++w) {
            const auto& write = writes[w];
            const auto& kept = cache.writes[w];
            if (kept.binding != write.dstBinding || kept.array_element != write.dstArrayElement ||
                kept.count != write.descriptorCount || kept.type != write.descriptorType) {
                same = false;
                break;
            }
            for (u32 i = 0; i < write.descriptorCount; ++i) {
                if (!(cache.infos[info_index++] == info_at(write, i))) {
                    same = false;
                    break;
                }
            }
        }
    }
    if (same) {
        if (SkipRedundantBinds()) {
            return true;
        }
        return false;
    }
    cache.layout = layout;
    cache.num_writes = static_cast<u32>(writes.size());
    cache.num_infos = total_infos;
    u32 info_index = 0;
    for (u32 w = 0; w < writes.size(); ++w) {
        const auto& write = writes[w];
        cache.writes[w] = {write.dstBinding, write.dstArrayElement, write.descriptorCount,
                           write.descriptorType};
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            cache.infos[info_index++] = info_at(write, i);
        }
    }
    return false;
}

bool DynamicState::BindCache::SkipVertexInput(
    std::span<const vk::VertexInputBindingDescription2EXT> bindings,
    std::span<const vk::VertexInputAttributeDescription2EXT> attributes) noexcept {
    if (bindings.size() > MaxVertexInputs || attributes.size() > MaxVertexInputs) {
        vertex_input_known = false;
        return false;
    }
    // Compared member by member (operator==): the padding of the structures is not compared.
    if (vertex_input_known && num_vertex_bindings == bindings.size() &&
        num_vertex_attributes == attributes.size() &&
        std::equal(bindings.begin(), bindings.end(), vertex_bindings.begin()) &&
        std::equal(attributes.begin(), attributes.end(), vertex_attributes.begin())) {
        if (SkipRedundantBinds()) {
            return true;
        }
        return false;
    }
    num_vertex_bindings = static_cast<u32>(bindings.size());
    num_vertex_attributes = static_cast<u32>(attributes.size());
    std::copy(bindings.begin(), bindings.end(), vertex_bindings.begin());
    std::copy(attributes.begin(), attributes.end(), vertex_attributes.begin());
    vertex_input_known = true;
    return false;
}

void DynamicState::Commit(const Instance& instance, const CommandRecorder& cmdbuf) {
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled);
    }
}

} // namespace Vulkan
