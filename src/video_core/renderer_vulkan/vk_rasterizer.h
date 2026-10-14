// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/perf_stats.h"
#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {

class GraphicsPipeline;
class Runtime;

/**
 * Guest occlusion queries counted by the GPU. The guest dumps the depth block sample counters
 * (ZPASS_DONE) before the draws it tests, at a 16 byte aligned address, and after them, 8 bytes
 * further; the sum of the differences of the counter pairs is the number of samples that passed
 * the depth test. The counters run all the time on the GPU, so queries may overlap or nest (a
 * query around a whole pass containing smaller queries). Here Vulkan occlusion queries count the
 * samples of consecutive segments of the command stream, shared by all the open guest queries: a
 * segment ends at each dump and at the end of each rendering scope (Vulkan queries begun in a
 * rendering scope must end in it, and only one may be active). A guest query reports the
 * segments between its two dumps, one per counter pair.
 */
class OcclusionQueries final : public RenderingObserver {
public:
    static constexpr u32 MaxCounterPairs = 16;
    static constexpr u32 MaxOpenQueries = 16;

    explicit OcclusionQueries(const Instance& instance, Scheduler& scheduler);
    ~OcclusionQueries() override;

    void OnBeginRendering(const CommandRecorder& cmdbuf) override;
    void OnEndRendering(const CommandRecorder& cmdbuf) override;

    /// Begin dump: counts the samples of the following draws for the query at this address.
    /// Returns false when it cannot be counted (too many open queries).
    bool Begin(VAddr address, u32 num_pairs);

    /// End dump of the query that began at begin_address: stops counting it and fills the Vulkan
    /// queries that counted its samples, oldest first. Returns false when no counted query began
    /// there, or when some of its samples were not counted (too many segments).
    bool End(VAddr begin_address,
             boost::container::static_vector<u32, MaxCounterPairs>& out_queries);

    /// Stops counting all open queries (real queries turned off).
    void AbandonAll();

    /// Starts counting again for the open queries after an end dump, if a rendering scope is
    /// active (otherwise the next one starts counting).
    void Resume();

    /// End of a guest frame: queries open for several frames are forgotten (never ended), so
    /// that they do not keep segments and open slots forever.
    void OnFrameEnd();

    /// The queries are read by the current command buffer: not reused before it completes.
    void MarkUsed(std::span<const u32> queries);

    [[nodiscard]] bool HasOpen() const noexcept {
        return !open.empty();
    }

    /// Whether a counted query begun at this address is open.
    [[nodiscard]] bool IsOpen(VAddr address) const noexcept {
        return std::any_of(open.begin(), open.end(),
                           [address](const OpenQuery& q) { return q.address == address; });
    }

    [[nodiscard]] u32 NumOpen() const noexcept {
        return static_cast<u32>(open.size());
    }

    [[nodiscard]] vk::QueryPool Pool() const noexcept {
        return *pool;
    }

private:
    struct OpenQuery {
        VAddr address;
        u32 max_segments;  ///< Counter pairs of the query: one segment each.
        u32 first_segment; ///< Index in segments of its first segment.
        u32 frame;         ///< Guest frame of its begin dump.
        bool incomplete;   ///< Some of its samples were not counted.
    };

    /// Forgets the segments that no open query needs any more.
    void DropOldSegments();

    void BeginSegment(const CommandRecorder& cmdbuf);
    void EndSegment(const CommandRecorder& cmdbuf);
    /// Ends the active segment: the samples before and after a dump are counted apart.
    void Cut();

    static constexpr u32 NumQueries = 4096;
    static constexpr u32 MaxSegments = 256;

    const Instance& instance;
    Scheduler& scheduler;
    vk::UniqueQueryPool pool;
    vk::QueryControlFlags control_flags{};
    /// GPU tick of the command buffer that last used each query (reused when it completed).
    std::array<u64, NumQueries> query_ticks{};
    u32 next_query{};
    bool active{}; ///< The last segment is counting in the current rendering scope.
    u32 frame{};   ///< Guest frames ended.
    boost::container::static_vector<OpenQuery, MaxOpenQueries> open;
    /// Vulkan queries of the segments since the oldest open query began, in order.
    boost::container::static_vector<u32, MaxSegments> segments;
};

/// What the command processor writes for a guest occlusion counter dump.
enum class OcclusionDumpResult {
    Emulated,          ///< Emulated counters at the dump address (all samples visible).
    EmulatedWithBegin, ///< Emulated counters at the begin address of the query, then at the end
                       ///< address: the begin dump was left to the GPU, which did not count.
    Counted,           ///< The GPU writes the counters.
};

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Scheduler& GetScheduler() noexcept {
        return scheduler;
    }

    [[nodiscard]] Runtime& GetRuntime() noexcept {
        return runtime;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    void Draw(bool is_indexed, u32 index_offset = 0);
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);

    /// The command processor translates the commands of one of the guest's asynchronous compute
    /// queues (asc_queue: their dispatches, copies and fills run on the compute queue).
    void SetAsyncCompute(bool active) noexcept {
        async_compute_translation = active;
    }

    /// asc_queue: submits the work recorded on the compute queue (at the fences of the compute
    /// queues and the end of their submissions).
    void FlushComputeQueue();

    void ScopeMarker(fmt::string_view fmt, fmt::format_args args, auto&& func) {
        if (host_markers_enabled) {
            ScopeMarkerBegin(fmt::vformat(fmt, args));
            func();
            ScopeMarkerEnd();
        } else {
            func();
        }
    }

    void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false);
    void ScopeMarkerEnd(bool from_guest = false);
    void ScopedMarkerInsert(const std::string_view& str, bool from_guest = false);
    void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                 bool from_guest = false);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    /// Guest occlusion counter dump (ZPASS_DONE), counted by real occlusion queries when they are
    /// turned on. Otherwise, or when the query cannot be counted, the caller writes emulated
    /// counters, consistent for the whole query (see OcclusionDumpResult).
    OcclusionDumpResult OcclusionDump(VAddr address, u32 num_pairs);
    /// Whether this address (a dword) is in the results of an occlusion query counted by the GPU
    /// and, if so, the value the GPU writes there when it does not depend on the samples counted
    /// (begin counters and valid bits). A command processor wait for it is then satisfied without
    /// waiting for the GPU: the commands that follow it run after the GPU wrote the results anyway.
    [[nodiscard]] bool IsCountedOcclusionResult(VAddr address, std::optional<u32>& value) const;

    /// Frame x-ray (Tuning::dump_frames): whether the commands of this guest frame are logged.
    [[nodiscard]] bool IsDumping() const noexcept;
    /// Logs one event of the dumped frames (guest markers...).
    void DumpNote(std::string_view kind, std::string_view text);
    /// Frame x-ray: records what the GPU reads in `size` bytes (at most 32) of `buffer` at
    /// `offset`, the copy of guest address `address`, to log it with the guest memory content
    /// when the dump ends (null buffer: guest memory only).
    void ProbeBuffer(std::string_view what, const VideoCore::Buffer* buffer, u64 offset, u32 size,
                     VAddr address);
    /// The same for the GPU copy (arena) of guest memory, when resident.
    void ProbeMemory(std::string_view what, VAddr address, u32 size);
    /// Waits for the GPU and logs the probes of the dump.
    void FinishProbes();
    /// Frame x-ray: after a draw or dispatch, the image dumps Tuning::dump_after asks for.
    void DumpAfter(const Pipeline* pipeline);
    u32 ReadDataFromGds(u32 gsd_offset);
    bool InvalidateMemory(VAddr addr, u64 size, bool assume_locks = false);
    /// The command processor wrote guest memory directly (see BufferCache::OnGpuThreadWrite).
    void OnGpuThreadWrite(VAddr addr, u64 size);
    /// guest_rip: the faulting instruction of a guest read (readback_relaxed_sites), 0 if none.
    bool ReadMemory(VAddr addr, u64 size, bool assume_locks = false, u64 guest_rip = 0);
    bool IsMapped(VAddr addr, u64 size);
    void MapMemory(VAddr addr, u64 size);
    void RegisterMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    u64 Flush();
    void Finish();
    void OnSubmit();
    void OnFence();
    void RetireReadbacks();

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }

    template <typename Func>
    void ForEachMappedRangeInRange(VAddr addr, u64 size, Func&& func) {
        const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (const auto& mapped_range : (mapped_ranges & range)) {
            func(mapped_range);
        }
    }

    std::thread::id GetGpuCommandProcessorThread();
#ifdef __linux__
    u32 GetGpuCommandProcessorThreadId();
#endif

private:
    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed) const;
    void UpdateViewportScissorState() const;
    void UpdateDepthStencilState() const;
    void UpdatePrimitiveState(bool is_indexed) const;
    void UpdateRasterizationState() const;
    void UpdateColorBlendingState(const GraphicsPipeline* pipeline) const;

    bool FilterDraw();

    void TraceShaderResources(const Shader::Info& stage);
    void TraceBufferWriters(const Shader::Info& stage);
    void BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data);
    /// Finish immutable table uploads after vertex/index preparation, immediately before
    /// recording descriptors. This prevents a later ring wrap from overwriting reused data.
    void FinalizeFlatbufUploads();
    void BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding);
    bool BindResources(const Pipeline* pipeline);

    /// Tuning::bind_prefetch: prefetches the memo entries the bindings of a draw read first,
    /// from its scout entry (the current draw if not done yet, and the next one), and the
    /// index buffer lookup of the current draw.
    void PrefetchDrawLookups(bool is_indexed, u32 index_offset);
    void PrefetchScoutLookups(const AmdGpu::ScoutDraw& draw);
    void PrefetchTexMemo(u64 key) const noexcept;

    /// Logs a draw or dispatch of the dumped frames: shaders and their buffers and images, render
    /// targets, blending and depth state.
    void DumpCommand(std::string_view kind, const Pipeline* pipeline, std::string_view params);

    /// Work skipped while its pipeline compiles leaves the buffers it would write stale. Indirect
    /// arguments read from them could be garbage, so indirect draws and dispatches reading them
    /// are skipped too, until something writes those buffers.
    void RecordSkippedWrites(const Pipeline& pipeline);
    bool ReadsSkippedWrites(VAddr address, u64 size);
    void ClearSkippedWrites(VAddr address, u64 size);

    void BindVertexBuffers(const GraphicsPipeline* pipeline);
    void BindIndexBuffer(u32 index_offset = 0);

    void ResetBindings(bool is_compute);

    bool IsComputeMetaClear(const Pipeline* pipeline);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    bool IsComputeImageClear(const Pipeline* pipeline);

private:
    friend class VideoCore::BufferCache;

    /// asc_queue: whether the compute queue can run this dispatch (buffers only, push
    /// descriptors), with the compute queue objects created at first use.
    bool UseComputeQueue(const ComputePipeline* pipeline);
    /// asc_queue: whether a copy or fill of the compute queue being translated runs there.
    [[nodiscard]] bool UseComputeQueueForCopy() const noexcept {
        return async_compute_translation && compute_scheduler != nullptr &&
               Common::Perf::GetTuning().asc_queue.load(std::memory_order_relaxed);
    }
    /// asc_queue: records the dispatch, copy or fill in scope on the compute queue when told
    /// to (the buffer cache binds, uploads and tracks the dependencies for that queue).
    class ComputeQueueScope {
    public:
        ComputeQueueScope(Rasterizer& rasterizer_, bool compute) : rasterizer{rasterizer_} {
            if (compute) {
                rasterizer.on_compute_queue = true;
                rasterizer.active_runtime = rasterizer.compute_runtime.get();
                rasterizer.active_scheduler = rasterizer.compute_scheduler.get();
                rasterizer.buffer_cache.SetQueue(&rasterizer.compute_context);
                Common::Perf::Add(Common::Perf::AscQueueDispatches);
            }
        }
        ~ComputeQueueScope() {
            if (rasterizer.on_compute_queue) {
                rasterizer.on_compute_queue = false;
                rasterizer.active_runtime = &rasterizer.runtime;
                rasterizer.active_scheduler = &rasterizer.scheduler;
                rasterizer.buffer_cache.SetQueue(nullptr);
            }
        }

    private:
        Rasterizer& rasterizer;
    };
    /// asc_queue: the runtime of the queue the current dispatch, copy or fill is recorded on.
    [[nodiscard]] Runtime& Rt() noexcept {
        return *active_runtime;
    }
    /// asc_queue: the scheduler of that queue.
    [[nodiscard]] Scheduler& Sched() noexcept {
        return *active_scheduler;
    }

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    /// asc_queue: the compute queue objects (created when first used) and the active ones.
    std::unique_ptr<Scheduler> compute_scheduler;
    std::unique_ptr<Runtime> compute_runtime;
    std::unique_ptr<VideoCore::StreamBuffer> compute_stream;
    VideoCore::QueueContext compute_context{};
    Runtime* active_runtime;
    Scheduler* active_scheduler;
    bool async_compute_translation{};
    bool on_compute_queue{}; ///< The current dispatch, copy or fill goes to the compute queue.
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    Common::SharedFirstMutex mapped_ranges_mutex;
    PipelineCache pipeline_cache;
    const bool host_markers_enabled;
    const bool guest_markers_enabled;

    struct Probe {
        u32 frame;
        u32 seq;
        std::string what;
        VAddr address;
        u32 size;
        u32 offset; ///< In the probe staging buffer.
        bool gpu;
        std::array<u32, 8> host;
        std::string state;
    };
    std::vector<Probe> probes;
    StagingBufferRef probe_staging{};
    u32 probe_used{};
    /// Frame x-ray: the vertex buffer ranges bound by the last draw (dump_after "hash:vb").
    struct BoundVertexRange {
        VAddr base;
        u64 size;
        const VideoCore::Buffer* buffer;
        u64 offset;
    };
    boost::container::static_vector<BoundVertexRange, 32> dump_vertex_ranges;
    /// Writes the GPU and guest copies of a range to the x-ray folder (waits for the GPU).
    void DumpBufferToFile(std::string_view name, const VideoCore::Buffer* buffer, u64 offset,
                          VAddr address, u64 size);

    using RenderTargetInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    std::array<RenderTargetInfo, AmdGpu::NUM_COLOR_BUFFERS> cb_descs;
    std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc> db_desc;
    /// Render target memo (Tuning::rt_memo): the registers and size hint a slot's description
    /// (cb_descs, db_desc) was last built from, and the cache generations its image was found
    /// with. The same registers find the same image while neither generation changes.
    struct RtMemo {
        bool valid{};
        u32 hint{};
        u64 generation{};
        u32 image_generation{};
        VideoCore::ImageId image_id{};
        u64 touched_gc_tick{};
        std::array<u32, sizeof(AmdGpu::ColorBuffer) / sizeof(u32)> regs{};
    };
    std::array<RtMemo, AmdGpu::NUM_COLOR_BUFFERS> rt_memos{};
    struct DbMemo {
        bool valid{};
        u32 hint{};
        u64 generation{};
        u32 image_generation{};
        VideoCore::ImageId image_id{};
        u64 touched_gc_tick{};
        VAddr htile_address{};
        u32 depth_view{};
        u32 depth_control{};
        std::array<u32, sizeof(AmdGpu::DepthBuffer) / sizeof(u32)> regs{};
    };
    DbMemo db_memo{};
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES> bound_images;
    struct BoundBuffer {
        const VideoCore::Buffer* buffer;
        u64 offset;
        u32 size;
        bool is_written;
        VAddr address{}; ///< Guest address (frame x-ray), 0 for emulator buffers.
    };
    boost::container::static_vector<BoundBuffer, Shader::NUM_BUFFERS> bound_buffers;

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Shader::PushData push_data;
    /// Immutable flattened resource tables uploaded in the current stream submission.
    /// Contents are compared on every use; bounded to 64 * 4 KiB, command processor only.
    std::array<VideoCore::RuntimeUploadMemo<4096>, 64> flatbuf_upload_memos;
    struct PendingFlatbuf {
        std::span<const u32> data;
        u32 buffer_info;
        u32 memo_index;
        bool reusable;
    };
    boost::container::static_vector<PendingFlatbuf, Shader::NUM_BUFFERS> pending_flatbufs;

    using ImageBindingInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;
    /// Texture binding memo (Tuning::tex_memo): the image and view found for a texture
    /// descriptor of a shader, valid while the texture cache generation and the image
    /// generation do not change. Small entries: a lookup is a cache miss at most.
    struct TexMemo {
        u64 key{};
        u64 generation{};
        u32 image_generation{};
        VideoCore::ImageId image_id{};
        VideoCore::ImageViewId view_id{};
        const void* view_backing{}; ///< The image backing the view belongs to.
        VideoCore::ImageViewInfo view_info{};
        VideoCore::TextureCache::BindingType type{};
    };
    static constexpr u32 NumTexMemoBits = 12;
    static constexpr size_t NumTexMemos = size_t{1} << NumTexMemoBits;
    std::unique_ptr<std::array<TexMemo, NumTexMemos>> tex_memos;
    /// Per binding of BindTextures: what the binding was made from, whether the memo gave it
    /// (no refresh check needed), its memo entry to complete, and the view it remembers.
    struct ImageSource {
        const Shader::ImageResource* image_desc{};
        AmdGpu::Image tsharp{};
        u32 index{};
        bool fresh{};
        TexMemo* memo{};
        const void* view_backing{};
        VideoCore::ImageViewId view_id{};
    };
    boost::container::static_vector<ImageSource, Shader::NUM_IMAGES> image_sources;
    /// Sampler memo (with tex_memo): the sampler handle found for an S#, valid while no sampler
    /// was deleted (TextureCache::SamplersGeneration).
    struct SamplerMemo {
        u64 key{};
        u64 generation{};
        vk::Sampler sampler{};
    };
    std::array<SamplerMemo, 1024> sampler_memos{};
    /// bind_prefetch: the last scout entry whose lookups were prefetched.
    const AmdGpu::ScoutDraw* last_prefetched_draw{};
    /// Statistics (DrawsSameContext): the context register changes and pipeline of the last
    /// draw.
    u64 last_draw_context_changes{~u64{0}};
    const GraphicsPipeline* last_draw_pipeline{};
    bool attachment_feedback_loop{};
    bool needs_barrier{};
    struct SkippedWrite {
        VAddr start;
        VAddr end;
        const Pipeline* producer; ///< Entry expires once this pipeline is compiled.
    };
    boost::container::static_vector<SkippedWrite, 32> skipped_writes;
    OcclusionQueries occlusion;

    /// Forgets the results of counted occlusion queries in this range (written again).
    void ForgetCountedResults(VAddr address, u64 size);
    struct CountedResults {
        VAddr address;
        u32 num_pairs;  ///< Counter pairs written by the GPU.
        u32 num_counts; ///< Pairs whose end counter holds samples counted by the GPU.
    };
    static constexpr size_t MaxCountedResults = 64;
    /// Results of the last occlusion queries counted by the GPU, most recent last.
    boost::container::static_vector<CountedResults, MaxCountedResults> counted_results;
};

} // namespace Vulkan
