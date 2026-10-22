// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>
#include <queue>

#include "common/assert.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/amdgpu/scout.h"

namespace Vulkan {
class Rasterizer;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace AmdGpu {

struct PM4CmdWaitRegMem;

struct Liverpool {
    static constexpr u32 GfxQueueId = 0u;
    static constexpr u32 NumGfxRings = 1u;     // actually 2, but HP is reserved by system software
    static constexpr u32 NumComputePipes = 7u; // actually 8, but #7 is reserved by system software
    static constexpr u32 NumQueuesPerPipe = 8u;
    static constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;
    static constexpr u32 NumTotalQueues = NumGfxRings + NumComputeRings;
    static_assert(NumTotalQueues < 64u); // need to fit into u64 bitmap for ffs

    enum ContextRegs : u32 {
        DbZInfo = 0xA010,
        CbColor0Base = 0xA318,
        CbColor1Base = 0xA327,
        CbColor2Base = 0xA336,
        CbColor3Base = 0xA345,
        CbColor4Base = 0xA354,
        CbColor5Base = 0xA363,
        CbColor6Base = 0xA372,
        CbColor7Base = 0xA381,
        CbColor0Cmask = 0xA31F,
        CbColor1Cmask = 0xA32E,
        CbColor2Cmask = 0xA33D,
        CbColor3Cmask = 0xA34C,
        CbColor4Cmask = 0xA35B,
        CbColor5Cmask = 0xA36A,
        CbColor6Cmask = 0xA379,
        CbColor7Cmask = 0xA388,
    };

    Regs regs{};
    std::unique_ptr<Scout> scout;
    std::shared_ptr<ScoutSubmission> scout_submission;
    const ScoutDraw* current_scout_draw{};
    u64 gfx_submit_seq{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> last_cb_extent{};
    CbDbExtent last_db_extent{};

public:
    explicit Liverpool();
    ~Liverpool();

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    /// End of a guest frame. Also measures how many guest frames the command processor is
    /// behind, and optionally waits for it to catch up (see Tuning::max_gpu_lag_frames).
    void SubmitDone() noexcept;

    void WaitGpuIdle() noexcept {
        std::unique_lock lk{submit_mutex};
        submit_cv.wait(lk, [this] { return num_submits == 0; });
    }

    bool IsGpuIdle() const {
        return num_submits == 0;
    }

    void SetVoPort(Libraries::VideoOut::VideoOutPort* port) {
        vo_port = port;
    }

    void BindRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }

    /// Starts the scout thread (Tuning::scout), which reads the graphics submissions ahead of
    /// the command processor and precomputes the draws for the client.
    void StartScout(ScoutClient& client);

    /// Stops it (before the client goes away).
    void StopScout() {
        scout.reset();
    }

    const Scout* GetScout() const noexcept {
        return scout.get();
    }

    /// The scout entry of the draw the command processor is processing, null when none.
    const ScoutDraw* CurrentScoutDraw() const noexcept {
        return current_scout_draw;
    }

    /// The scout entry of the draw packet after the current one, when already published (its
    /// lookups can be prefetched a draw ahead), null when none.
    const ScoutDraw* PeekNextScoutDraw() const noexcept {
        return scout_submission ? scout_submission->Peek() : nullptr;
    }

    /// Context, config and uconfig register writes that changed a value so far (command
    /// processor thread).
    u64 ContextRegChanges() const noexcept {
        return context_reg_changes;
    }

    template <bool wait_done = false>
    void SendCommand(auto&& func) {
        if constexpr (wait_done) {
            std::binary_semaphore sem{0};
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace([&sem, &func] {
                    func();
                    sem.release();
                });
                ++num_commands;
                submit_cv.notify_one();
            }
            sem.acquire();
        } else {
            std::scoped_lock lk{submit_mutex};
            command_queue.emplace(std::move(func));
            ++num_commands;
            submit_cv.notify_one();
        }
    }

    void ReserveCopyBufferSpace() {
        GpuQueue& gfx_queue = mapped_queues[GfxQueueId];
        std::scoped_lock lk(gfx_queue.m_access);
        constexpr size_t GfxReservedSize = 2_MB >> 2;
        gfx_queue.ccb_buffer.reserve(GfxReservedSize);
        gfx_queue.dcb_buffer.reserve(GfxReservedSize);
    }

    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    /// The queue whose commands the command processor is processing: 0 graphics, 1+ compute.
    [[nodiscard]] s32 CurrentQueue() const noexcept {
        return curr_qid;
    }

    struct AscQueueInfo {
        static constexpr size_t Pm4BufferSize = 1024;
        VAddr map_addr;
        u32* read_addr;
        u32 ring_size_dw;
        u32 pipe_id;
        std::array<u32, Pm4BufferSize> tmp_packet;
        u32 tmp_dwords;
    };
    Common::SlotVector<AscQueueInfo> asc_queues{};

    std::thread::id GetGpuCommandProcessorThread() {
        return gpu_id;
    }

#ifdef __linux__
    u32 GetGpuCommandProcessorThreadId() {
        return gpu_tid;
    }
#endif

private:
    struct Task {
        struct promise_type {
            auto get_return_object() {
                Task task{};
                task.handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return task;
            }
            static constexpr std::suspend_always initial_suspend() noexcept {
                // We want the task to be suspended at start
                return {};
            }
            static constexpr std::suspend_always final_suspend() noexcept {
                return {};
            }
            void unhandled_exception() {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    UNREACHABLE_MSG("Unhandled exception: {}", e.what());
                }
            }
            void return_void() {}
            struct empty {};
            std::suspend_always yield_value(empty&&) {
                return {};
            }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;
    };

    using CmdBuffer = std::pair<std::span<const u32>, std::span<const u32>>;
    CmdBuffer CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb, u64 scout_seq = 0);
    void BeginScoutDraw(const void* packet);
    void EndScoutDraw();
    Task ProcessCeUpdate(std::span<const u32> ccb);
    /// submit_ns: when the guest submitted the work (top level submissions only, for statistics).
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid, u64 submit_ns = 0);

    void ProcessCommands();
    void Process(std::stop_token stoken);

    /// Remembers values the command processor wrote at small addresses (labels, fences), so a
    /// wait satisfied by one of them is not mistaken for a guest CPU signal.
    void RecordCpLabel(VAddr address, const void* data, u32 num_bytes);
    /// Same, for a command processor write of guest memory: copies taken before may be stale.
    void NoteCpWrite(VAddr address, const void* data, u32 num_bytes);
    /// Writes emulated depth block sample counters for an occlusion counter dump: they grow at
    /// each dump, so every query sees samples visible.
    void WriteEmulatedOcclusionCounters(VAddr address);
    /// Whether the value at the address is the last one the command processor wrote there.
    bool IsWrittenByCp(VAddr address) const;
    /// Starts a new upload epoch when the satisfied wait may have been released by the guest CPU
    /// (which writes the data the following commands read before the value they wait for).
    void OnWaitRegMemSatisfied(const PM4CmdWaitRegMem& wait);
    /// A wait for the results of an occlusion query counted by the GPU is satisfied at once when
    /// the value waited for does not depend on the samples counted (valid bits): the commands that
    /// follow run after the GPU wrote the results, reading them back would wait for the GPU.
    bool SkipOcclusionWait(const PM4CmdWaitRegMem& wait);

    struct GpuQueue {
        std::mutex m_access{};
        std::atomic<u32> dcb_buffer_offset;
        std::atomic<u32> ccb_buffer_offset;
        std::vector<u32> dcb_buffer;
        std::vector<u32> ccb_buffer;
        std::queue<Task::Handle> submits{};
        ComputeProgram cs_state{};
    };
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    u32 num_mapped_queues{1u}; // GFX is always available

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};

    struct ConstantEngine {
        void Reset() {
            ce_count = 0;
            de_count = 0;
            ce_compare_count = 0;
        }

        [[nodiscard]] u32 Diff() const {
            ASSERT_MSG(ce_count >= de_count, "DE counter is ahead of CE");
            return ce_count - de_count;
        }

        u32 ce_compare_count{};
        u32 ce_count{};
        u32 de_count{};
        static std::array<u8, 48_KB> constants_heap;
    } cblock{};

    Vulkan::Rasterizer* rasterizer{};
    Libraries::VideoOut::VideoOutPort* vo_port{};
    const bool guest_markers_enabled;
    std::jthread process_thread{};
    std::atomic<u32> num_submits{};
    /// Asynchronous compute submissions not fully processed yet. The PS4 runs them alongside the
    /// graphics queue; here the graphics queue can give way to them (Tuning::asc_yield_packets).
    std::atomic<u32> asc_submits{};
    /// Submissions queued by the guest and fully processed by the command processor (monotonic),
    /// and the number queued at the end of each recent guest frame.
    std::atomic<u64> submits_issued{};
    std::atomic<u64> submits_completed{};
    static constexpr size_t NumFrameMarks = 16;
    std::array<u64, NumFrameMarks> frame_marks{};
    u64 frame_index{};
    std::atomic<u32> num_commands{};
    std::atomic<bool> submit_done{};
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    std::queue<Common::UniqueFunction<void>> command_queue{};
    std::thread::id gpu_id;
#ifdef __linux__
    u32 gpu_tid;
#endif
    s32 curr_qid{-1};
    /// Packets and commands processed by the command processor thread, used to tell work from
    /// spinning on unsatisfied waits. Only accessed by that thread, counted at every packet: on
    /// a cache line of its own, away from the fields guest threads write when they submit
    /// (counting there waited for that line to come back from their cores).
    alignas(64) u64 processed_packets{};
    /// Graphics packets processed since the graphics queue last gave way to compute work. Only
    /// accessed by the command processor thread.
    u32 gfx_packets_since_yield{};
    /// Context, config and uconfig register writes that changed a value (and state clears), by
    /// the command processor: draws with the count of the draw before have the same render
    /// state. Only accessed by that thread.
    u64 context_reg_changes{};
    /// Time the command processor thread slept inside packets (flip label waits).
    u64 blocked_ns{};
    struct CpLabel {
        VAddr address{};
        u32 value{};
    };
    static constexpr size_t NumCpLabels = 1024;
    /// Last values written by the command processor, indexed by dword address. Only accessed
    /// by the command processor thread.
    std::array<CpLabel, NumCpLabels> cp_labels{};
};

} // namespace AmdGpu
