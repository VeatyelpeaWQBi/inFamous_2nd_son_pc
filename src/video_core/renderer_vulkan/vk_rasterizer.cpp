// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/runtime_policy.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"
#include <array>
#include <filesystem>
#include <fstream>

namespace Vulkan {

namespace {

// Keep sampled read-only depth and its stencil attachment in the same layout.
vk::ImageLayout DepthTargetLayout(bool has_stencil, bool depth_write, bool stencil_write) {
    if (depth_write) {
        return has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                           : vk::ImageLayout::eDepthAttachmentOptimal;
    }
    if (stencil_write) {
        return vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal;
    }
    return has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                       : vk::ImageLayout::eDepthReadOnlyOptimal;
}

} // Anonymous namespace

OcclusionQueries::OcclusionQueries(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const vk::QueryPoolCreateInfo pool_info = {
        .queryType = vk::QueryType::eOcclusion,
        .queryCount = NumQueries,
    };
    auto [result, query_pool] = instance.GetDevice().createQueryPoolUnique(pool_info);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create occlusion query pool: {}",
               vk::to_string(result));
    pool = std::move(query_pool);
    if (instance.IsOcclusionQueryPreciseSupported()) {
        control_flags = vk::QueryControlFlagBits::ePrecise;
    }
    scheduler.SetRenderingObserver(this);
}

OcclusionQueries::~OcclusionQueries() { scheduler.SetRenderingObserver(nullptr); }

void OcclusionQueries::OnBeginRendering(const CommandRecorder& cmdbuf) {
    if (!open.empty() && !active) {
        BeginSegment(cmdbuf);
    }
}

void OcclusionQueries::OnEndRendering(const CommandRecorder& cmdbuf) {
    if (active) {
        EndSegment(cmdbuf);
    }
}

bool OcclusionQueries::Begin(VAddr address, u32 num_pairs) {
    // A query begun again at the same address starts over.
    for (auto it = open.begin(); it != open.end(); ++it) {
        if (it->address == address) {
            open.erase(it);
            break;
        }
    }
    if (open.size() >= MaxOpenQueries) {
        return false;
    }
    Cut();
    open.push_back(OpenQuery{
        .address = address,
        .max_segments = std::clamp<u32>(num_pairs, 1, MaxCounterPairs),
        .first_segment = static_cast<u32>(segments.size()),
        .frame = frame,
        .incomplete = false,
    });
    Resume();
    return true;
}

bool OcclusionQueries::End(VAddr begin_address,
                           boost::container::static_vector<u32, MaxCounterPairs>& out_queries) {
    const auto it = std::find_if(open.begin(), open.end(), [begin_address](const OpenQuery& q) {
        return q.address == begin_address;
    });
    if (it == open.end()) {
        return false;
    }
    Cut();
    const OpenQuery query = *it;
    open.erase(it);
    const u32 num_segments = static_cast<u32>(segments.size()) - query.first_segment;
    const bool counted = !query.incomplete && num_segments <= query.max_segments;
    if (counted) {
        out_queries.assign(segments.begin() + query.first_segment, segments.end());
    }
    DropOldSegments();
    return counted;
}

void OcclusionQueries::DropOldSegments() {
    u32 oldest = static_cast<u32>(segments.size());
    for (const OpenQuery& q : open) {
        oldest = std::min(oldest, q.first_segment);
    }
    if (oldest == 0) {
        return;
    }
    if (active && oldest == segments.size()) {
        // The active segment counts for no open query.
        --oldest;
    }
    segments.erase(segments.begin(), segments.begin() + oldest);
    for (OpenQuery& q : open) {
        q.first_segment -= oldest;
    }
}

void OcclusionQueries::OnFrameEnd() {
    ++frame;
    const auto old_size = open.size();
    for (auto it = open.begin(); it != open.end();) {
        if (frame - it->frame > 2) {
            it = open.erase(it);
        } else {
            ++it;
        }
    }
    if (open.size() == old_size) {
        return;
    }
    if (open.empty()) {
        Cut();
        segments.clear();
        return;
    }
    DropOldSegments();
}

void OcclusionQueries::AbandonAll() {
    Cut();
    open.clear();
    segments.clear();
}

void OcclusionQueries::MarkUsed(std::span<const u32> queries) {
    const u64 tick = scheduler.CurrentTick();
    for (const u32 query : queries) {
        query_ticks[query] = tick;
    }
}

void OcclusionQueries::Cut() {
    if (active) {
        EndSegment(scheduler.Recorder());
    }
}

void OcclusionQueries::Resume() {
    if (!open.empty() && !active && scheduler.IsRendering()) {
        BeginSegment(scheduler.Recorder());
    }
}

void OcclusionQueries::BeginSegment(const CommandRecorder& cmdbuf) {
    const auto not_counted = [this] {
        // The samples of the open queries until the next segment are lost.
        for (OpenQuery& q : open) {
            q.incomplete = true;
        };
    };
    if (segments.size() >= MaxSegments || !instance.IsHostQueryResetSupported()) {
        not_counted();
        return;
    }
    const u32 query = next_query;
    const u64 last_use = query_ticks[query];
    if (last_use != 0 && !scheduler.IsFree(last_use)) {
        // Its last use (thousands of queries ago) is still executing.
        not_counted();
        return;
    }
    next_query = (next_query + 1) % NumQueries;
    // Reset from the host, as this is inside a rendering scope.
    instance.GetDevice().resetQueryPool(*pool, query, 1);
    query_ticks[query] = scheduler.CurrentTick();
    cmdbuf.beginQuery(*pool, query, control_flags);
    segments.push_back(query);
    active = true;
}

void OcclusionQueries::EndSegment(const CommandRecorder& cmdbuf) {
    cmdbuf.endQuery(*pool, segments.back());
    active = false;
}

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, active_runtime{&runtime_},
      active_scheduler{&scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      occlusion{instance, scheduler} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
        // The scout follows the command stream from the start, so that its registers match
        // the command processor's whenever Tuning::scout turns it on.
        liverpool->StartScout(pipeline_cache);
    }
    memory->SetRasterizer(this);
    // Only the command processor records with this scheduler (the presenter records its frame
    // copy on that thread too): its commands may be recorded by a thread of their own.
    scheduler.AllowThreadedRecording();

    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });
    pipeline_cache.SetPipelineDrainCallback([this] {
        if (compute_scheduler) {
            compute_scheduler->Finish();
        }
        scheduler.Finish();
    });
}

Rasterizer::~Rasterizer() {
    liverpool->StopScout();
    if (compute_scheduler) {
        buffer_cache.SetCrossQueueTracking(nullptr);
        compute_scheduler->Finish();
    }
    // The recorder may still own a bind capturing a PipelineCache entry. Drain it before
    // member destruction releases those entries, descriptors or resource buffers.
    scheduler.Finish();
}

bool Rasterizer::UseComputeQueue(const ComputePipeline* pipeline) {
    if (!async_compute_translation || !instance.HasComputeQueue() ||
        !Core::Runtime::GetPolicy().asc_queue) {
        return false;
    }
    // Buffers only: the images (layouts, barriers) are tracked for the graphics queue; the
    // descriptors must be pushed (sets are recycled with the graphics timeline).
    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    if (!cs.images.empty() || !cs.samplers.empty() || cs.uses_dma ||
        !pipeline->UsesPushDescriptors()) {
        return false;
    }
    if (!compute_scheduler) {
        static constexpr u64 ComputeStreamSize = 64_MB;
        compute_scheduler =
            std::make_unique<Scheduler>(instance, instance.GetComputeQueue(), "compute");
        compute_runtime = std::make_unique<Runtime>(instance, *compute_scheduler);
        compute_stream = std::make_unique<VideoCore::StreamBuffer>(
            instance, *compute_scheduler, VideoCore::MemoryType::Stream, ComputeStreamSize);
        compute_context = VideoCore::QueueContext{
            .scheduler = compute_scheduler.get(),
            .runtime = compute_runtime.get(),
            .stream_buffer = compute_stream.get(),
            .index = 1,
        };
        compute_scheduler->SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
            compute_runtime->FlushBarriers();
            buffer_cache.SubmitPendingArenaBinds(info);
            buffer_cache.WaitArenaBinds(info);
        });
        buffer_cache.SetCrossQueueTracking(&compute_context);
    }
    return true;
}

void Rasterizer::FlushComputeQueue() {
    if (compute_scheduler) {
        buffer_cache.FlushComputeQueue();
    }
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        ;
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    // Render target memo: a slot whose registers and size hint (given by the commands, not
    // learned by the cache) are those its description was built from keeps its image while the
    // cache and the image do not change. The generation is read before the search: a change
    // during it leaves the entry invalid until the next draw finds the image again.
    const bool use_rt_memo = Core::Runtime::GetPolicy().rt_memo;
    const auto found_again = [&](VideoCore::ImageId image_id, u64& touched_gc_tick) {
        bound_images.emplace_back(image_id);
        auto& image = texture_cache.GetImage(image_id);
        image.tick_accessed_last = scheduler.CurrentTick();
        if (touched_gc_tick != texture_cache.GcTick()) {
            // As FindImage does, for the garbage collector (once per frame).
            texture_cache.TouchImageLocked(image_id);
            touched_gc_tick = texture_cache.GcTick();
        }
        image.binding.is_target = 1u;
    };
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        auto hint = liverpool->last_cb_extent[cb];
        const bool memo_candidate = use_rt_memo && hint.Valid();
        auto& memo = rt_memos[cb];
        if (memo_candidate && memo.valid && image_id && memo.image_id == image_id &&
            memo.hint == hint.raw &&
            std::memcmp(memo.regs.data(), &col_buf, sizeof(memo.regs)) == 0 &&
            memo.generation == texture_cache.Generation() &&
            memo.image_generation == texture_cache.ImageGeneration(image_id)) {
            found_again(image_id, memo.touched_gc_tick);
            continue;
        }
        if (!hint.Valid()) {
            hint = texture_cache.LearnedTargetExtent(col_buf.Address(), col_buf.Pitch());
        }
        const u64 generation = memo_candidate ? texture_cache.Generation() : 0;
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
        if (memo_candidate) {
            memo.valid = generation == texture_cache.Generation();
            memo.hint = hint.raw;
            memo.generation = generation;
            memo.image_generation = texture_cache.ImageGeneration(image_id);
            memo.image_id = image_id;
            memo.touched_gc_tick = texture_cache.GcTick();
            std::memcpy(memo.regs.data(), &col_buf, sizeof(memo.regs));
        } else {
            memo.valid = false;
        }
    }

    // The PS4 depth block counts the samples of occlusion queries even when the depth test is
    // off (a query without depth test measures the full size of the tested shape). Host GPUs
    // count them in their depth unit: the depth buffer stays attached while a query is open.
    const bool depth_for_query = occlusion.HasOpen() && regs.depth_buffer.DepthValid();
    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid()) ||
        depth_for_query) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        auto hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        static_assert(sizeof(regs.depth_view) == sizeof(u32) &&
                      sizeof(regs.depth_control) == sizeof(u32));
        u32 depth_view;
        u32 depth_control;
        std::memcpy(&depth_view, &regs.depth_view, sizeof(u32));
        std::memcpy(&depth_control, &regs.depth_control, sizeof(u32));
        const bool memo_candidate = use_rt_memo && hint.Valid();
        auto& memo = db_memo;
        if (memo_candidate && memo.valid && image_id && memo.image_id == image_id &&
            memo.hint == hint.raw && memo.htile_address == htile_address &&
            memo.depth_view == depth_view && memo.depth_control == depth_control &&
            std::memcmp(memo.regs.data(), &regs.depth_buffer, sizeof(memo.regs)) == 0 &&
            memo.generation == texture_cache.Generation() &&
            memo.image_generation == texture_cache.ImageGeneration(image_id)) {
            found_again(image_id, memo.touched_gc_tick);
        } else {
            if (!hint.Valid()) {
                hint = texture_cache.LearnedTargetExtent(regs.depth_buffer.DepthAddress(),
                                                         regs.depth_buffer.Pitch());
            }
            const u64 generation = memo_candidate ? texture_cache.Generation() : 0;
            std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                              htile_address, hint);
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            auto& image = texture_cache.GetImage(image_id);
            image.binding.is_target = 1u;
            if (memo_candidate) {
                memo.valid = generation == texture_cache.Generation();
                memo.hint = hint.raw;
                memo.generation = generation;
                memo.image_generation = texture_cache.ImageGeneration(image_id);
                memo.image_id = image_id;
                memo.touched_gc_tick = texture_cache.GcTick();
                memo.htile_address = htile_address;
                memo.depth_view = depth_view;
                memo.depth_control = depth_control;
                std::memcpy(memo.regs.data(), &regs.depth_buffer, sizeof(memo.regs));
            } else {
                memo.valid = false;
            }
        }
    } else {
        db_desc.first = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(const AmdGpu::Regs& regs, const Shader::Info& info,
                                          const Shader::Gcn::FetchShaderData& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (!fetch_shader.Empty()) {
        if (vertex_offset == 0 && fetch_shader.vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader.vertex_offset_sgpr];
        }
        if (fetch_shader.instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader.instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ;
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const auto& regs = liverpool->regs;
    if (regs.num_indices == 0) [[unlikely]] {
        // No vertex to draw: nothing the GPU would do (skip_empty_draws), before looking the
        // pipeline and the resources up.
        if (Core::Runtime::GetPolicy().skip_empty_draws) {
            return;
        }
    }
    if (Core::Runtime::GetPolicy().bind_prefetch) {
        // Read while the pipeline is looked up: the bindings find their memo entries cached.
        PrefetchDrawLookups(is_indexed, index_offset);
    }
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }
    if (!pipeline->IsReady()) {
        // Still compiled by a worker thread, the draw is skipped instead of stalling.
        RecordSkippedWrites(*pipeline);
        return;
    }

    // Statistics: draws a fast path for unchanged render state could take.
    if (const u64 changes = liverpool->ContextRegChanges(); changes == last_draw_context_changes) {

    } else {
        last_draw_context_changes = changes;
    }
    last_draw_pipeline = pipeline;

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    };
    const auto state = BeginRendering(pipeline);

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer(index_offset);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    FinalizeFlatbufUploads();
    pipeline->BindResources(set_writes, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.Recorder();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    };

    ResetBindings(false);
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline) {
        return;
    };
    if (!pipeline->IsReady() || ReadsSkippedWrites(arg_address + offset, u64{stride} * max_count) ||
        (count_address != 0 && ReadsSkippedWrites(count_address, 4))) {
        RecordSkippedWrites(*pipeline);
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    };
    const auto state = BeginRendering(pipeline);

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer();
    }

    const auto [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, stride * max_count);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    FinalizeFlatbufUploads();
    pipeline->BindResources(set_writes, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.Recorder();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_offset, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        };
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_offset,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        };
    }

    ;
    ResetBindings(false);
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    if (cs_program.dim_x == 0 || cs_program.dim_y == 0 || cs_program.dim_z == 0) [[unlikely]] {
        // No workgroup to run (skip_empty_draws).
        if (Core::Runtime::GetPolicy().skip_empty_draws) {
            return;
        }
    }
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        return;
    }

    // asc_queue: the dispatch, its bindings and uploads go to the compute queue.
    const ComputeQueueScope queue_scope{*this, UseComputeQueue(pipeline)};
    if (!BindResources(pipeline)) {
        return;
    };

    if (needs_barrier) {
        Rt().FlushBarriers();
    }

    if (!on_compute_queue) {
        scheduler.EndRendering();
    }
    FinalizeFlatbufUploads();
    pipeline->BindResources(Sched(), set_writes, push_data);

    const auto cmdbuf = Sched().Recorder();
    pipeline->BindCompute(Sched());
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    if (on_compute_queue)
        ;

    ResetBindings(true);
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (ReadsSkippedWrites(address + offset, size)) {
        RecordSkippedWrites(*pipeline);
        return;
    }

    // asc_queue: the dispatch, its bindings and uploads go to the compute queue.
    const ComputeQueueScope queue_scope{*this, UseComputeQueue(pipeline)};
    if (!BindResources(pipeline)) {
        return;
    };

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= Rt().IsBufferAccessed(buffer, base, size);

    if (needs_barrier) {
        Rt().FlushBarriers();
    }

    if (!on_compute_queue) {
        scheduler.EndRendering();
    }
    FinalizeFlatbufUploads();
    pipeline->BindResources(Sched(), set_writes, push_data);

    const auto cmdbuf = Sched().Recorder();
    pipeline->BindCompute(Sched());
    cmdbuf.dispatchIndirect(buffer->Handle(), base);

    ResetBindings(true);
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() { scheduler.Finish(); }

void Rasterizer::OnSubmit() {
    occlusion.OnFrameEnd();
    buffer_cache.TickFrame();
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
    if (compute_runtime) {
        compute_runtime->TickFrame();
    }
}

void Rasterizer::OnFence() {
    texture_cache.ProcessDownloadImages();
    buffer_cache.PrefetchReadbacks();
}

void Rasterizer::RetireReadbacks() { buffer_cache.RetireReadbacks(); }

void Rasterizer::PrefetchDrawLookups(bool is_indexed, u32 index_offset) {
    // The entries of the current draw were prefetched with the draw before, unless it had none.
    if (const auto* draw = liverpool->CurrentScoutDraw(); draw != nullptr) {
        if (draw != last_prefetched_draw) {
            PrefetchScoutLookups(*draw);
        }
        // The memory its small buffers will copy, now that their stream entries are cached.
        const u32 num_stages = std::min<u32>(draw->num_stages, AmdGpu::ScoutDraw::MaxStages);
        for (u32 s = 0; s < num_stages; ++s) {
            const auto& stage = draw->stages[s];
            if (!stage.valid) {
                continue;
            }
            for (u32 i = 0; i < stage.num_buffer_ranges; ++i) {
                buffer_cache.PrefetchStreamSource(stage.buffer_addresses[i], stage.buffer_sizes[i]);
            }
        }
    }
    if (const auto* next = liverpool->PeekNextScoutDraw(); next != nullptr) {
        PrefetchScoutLookups(*next);
        last_prefetched_draw = next;
    }
    if (is_indexed) {
        // As BindIndexBuffer, which runs last.
        const auto& regs = liverpool->regs;
        const u32 index_size =
            regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
        buffer_cache.PrefetchLookup(regs.index_base_address.Address<VAddr>() +
                                        u64{index_offset} * index_size,
                                    u64{regs.num_indices} * index_size);
    }
}

void Rasterizer::PrefetchScoutLookups(const AmdGpu::ScoutDraw& draw) {
    const u32 num_stages = std::min<u32>(draw.num_stages, AmdGpu::ScoutDraw::MaxStages);
    for (u32 s = 0; s < num_stages; ++s) {
        const auto& stage = draw.stages[s];
        if (!stage.valid) {
            continue;
        }
        for (u32 i = 0; i < stage.num_buffer_ranges; ++i) {
            buffer_cache.PrefetchLookup(stage.buffer_addresses[i], stage.buffer_sizes[i]);
        }
        for (u32 i = 0; i < stage.num_image_keys; ++i) {
            PrefetchTexMemo(stage.image_keys[i]);
        }
    }
}

void Rasterizer::PrefetchTexMemo(u64 key) const noexcept {
    if (!tex_memos) {
        return;
    }
    const auto* entry = &(*tex_memos)[(key * 0x9E3779B97F4A7C15ULL) >> (64 - NumTexMemoBits)];
    VideoCore::PrefetchLine(entry);
    VideoCore::PrefetchLine(reinterpret_cast<const u8*>(entry) + sizeof(TexMemo) - 1);
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }
    if (!pipeline->IsReady() && !pipeline->RetainsComputeWork()) {
        // Still compiled by a worker thread (only the emulated dispatches above can run).
        RecordSkippedWrites(*pipeline);
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();
    descriptor_image_uses.clear();
    pending_flatbufs.clear();

    bool uses_dma = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(liverpool->regs);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
    }

    if (!pending_flatbufs.empty() &&
        buffer_cache.GetStreamBuffer().MayFlushOnMap(4_MB, instance.StorageMinAlignment())) {
        // Leave conservative room for the remaining vertex/index/small-image preparations
        // (streamed guest buffers are at most 16 KiB, tables here at most 4 KiB each).
        // Near a wrap or a wait on this tick, use fresh uploads now, as in the old order.
        // Uploading later must not submit and lose already recorded vertex/index bindings.
        for (auto& pending : pending_flatbufs) {
            pending.reusable = false;
        }
        FinalizeFlatbufUploads();
    }

    return true;
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline) {
    const auto& regs = liverpool->regs;
    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                              regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    if (instance.IsVertexInputDynamicState()) {
        // Update current vertex inputs.
        const auto cmdbuf = scheduler.Recorder();
        cmdbuf.setVertexInputEXT(bindings, attributes);
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const { return end_address - base_address; }
    };

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
        }
    }

    // Merge connecting ranges together
    VertexInputs<BufferRange> ranges_merged{};
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    ;
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        if (range.buffer != &buffer_cache.GetStreamBuffer()) {
            needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
        };
    }

    // Bind vertex buffers
    VertexInputs<vk::Buffer> host_buffers;
    VertexInputs<vk::DeviceSize> host_offsets;
    VertexInputs<vk::DeviceSize> host_sizes;
    VertexInputs<vk::DeviceSize> host_strides;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                    return buffer.base_address >= range.base_address &&
                           buffer.base_address < range.end_address;
                });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    const auto cmdbuf = scheduler.Recorder();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
}

void Rasterizer::BindIndexBuffer(u32 index_offset) {
    const auto& regs = liverpool->regs;

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
    if (buffer != &buffer_cache.GetStreamBuffer()) {
        needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
    }
    const auto cmdbuf = scheduler.Recorder();
    cmdbuf.bindIndexBuffer(buffer->Handle(), offset, index_type);
}

void Rasterizer::ResetBindings(bool is_compute) {
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    for (const auto [buffer, offset, size, is_written, address] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        const auto write_flag =
            is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone;
        Rt().AccessBuffer(buffer, offset, size, dst_stage,
                          vk::AccessFlagBits2::eShaderRead | write_flag);
    }
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
}

void Rasterizer::RecordSkippedWrites(const Pipeline& pipeline) {
    for (const auto* stage : pipeline.GetStages()) {
        if (!stage) {
            continue;
        }
        for (const auto& desc : stage->buffers) {
            if (!desc.is_written || desc.IsSpecial()) {
                continue;
            }
            const auto vsharp = desc.GetSharp(*stage);
            if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
                continue;
            }
            const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            if (size == 0) {
                continue;
            }
            if (skipped_writes.size() == skipped_writes.capacity()) {
                skipped_writes.erase(skipped_writes.begin());
            }
            skipped_writes.push_back(SkippedWrite{
                .start = vsharp.base_address,
                .end = vsharp.base_address + size,
                .producer = &pipeline,
            });
        }
    }
}

bool Rasterizer::ReadsSkippedWrites(VAddr address, u64 size) {
    if (skipped_writes.empty()) {
        return false;
    }
    // The data stays stale only until the producer is compiled and runs again, and it may never
    // run again (a one-shot pass): entries expire once their pipeline is ready.
    for (auto it = skipped_writes.begin(); it != skipped_writes.end();) {
        if (it->producer->IsReady()) {
            it = skipped_writes.erase(it);
        } else {
            ++it;
        }
    }
    return std::ranges::any_of(skipped_writes, [address, size](const SkippedWrite& range) {
        return address < range.end && range.start < address + size;
    });
}

void Rasterizer::ClearSkippedWrites(VAddr address, u64 size) {
    for (auto it = skipped_writes.begin(); it != skipped_writes.end();) {
        if (address < it->end && it->start < address + size) {
            it = skipped_writes.erase(it);
        } else {
            ++it;
        }
    }
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    runtime.ClearImage(&image1, range, clear);
    return true;
}

namespace {} // Anonymous namespace

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    const u64 alignment = instance.StorageMinAlignment();
    if (Core::Runtime::GetPolicy().bind_prefetch && stage.buffers.size() > 1) {
        // The lookups of the stage's buffers start together instead of one after the other,
        // unless the scout's entry of the draw had them prefetched already.
        const auto* scouted = pipeline_cache.TakenScoutStage(stage);
        if (scouted == nullptr || scouted->num_buffer_ranges == 0) {
            for (const auto& desc : stage.buffers) {
                if (desc.IsSpecial() || desc.is_written) {
                    continue;
                }
                const auto vsharp = desc.GetSharp(stage);
                if (vsharp.base_address != 0 && vsharp.GetSize() != 0) {
                    buffer_cache.PrefetchLookup(vsharp.base_address, vsharp.GetSize());
                }
            }
        }
    }
    for (const auto& desc : stage.buffers) {
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |=
                    Rt().IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
                buffer_cache.NoteGdsAccess(desc.is_written);
                bound_buffers.emplace_back(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.flattened_ud_buf.size() * sizeof(u32);
                if (Core::Runtime::GetPolicy().runtime_lookup_memo && !false && ubo_size <= 4096) {
                    pending_flatbufs.push_back(PendingFlatbuf{
                        .data = stage.flattened_ud_buf,
                        .buffer_info = static_cast<u32>(buffer_infos.size()),
                        .memo_index =
                            static_cast<u32>((stage.pgm_hash * 0x9E3779B97F4A7C15ULL) >> (64 - 6)),
                        .reusable = !desc.is_written,
                    });
                    // FinalizeFlatbufUploads fills the offset before these descriptors are
                    // recorded; other bindings may wrap the stream in the meantime.
                    buffer_infos.emplace_back(vk_buffer.Handle(), 0, ubo_size);
                } else {
                    const u64 offset =
                        vk_buffer.Copy(stage.flattened_ud_buf.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (liverpool->regs.clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = liverpool->regs.clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
                buffer_cache.NoteBdaAccess();
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = liverpool->GetCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            const auto vsharp = desc.GetSharp(stage);
            if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                if (size != vsharp.GetSize()) {
                    LOG_ERROR(Render, "Clamped size from {} to {} for stage {:#x}",
                              vsharp.GetSize(), size, stage.pgm_hash);
                }
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                    vsharp.base_address, size, desc.is_written, desc.is_formatted);
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
                    if (!skipped_writes.empty()) {
                        ClearSkippedWrites(vsharp.base_address, size);
                    }
                }
                // The stream buffer is only written by the CPU before submission, its reads
                // never need a barrier. Skipping it avoids tracking most bindings of a draw.
                if (buffer != &buffer_cache.GetStreamBuffer()) {
                    bound_buffers.emplace_back(buffer, offset, size, desc.is_written,
                                               vsharp.base_address);
                    needs_barrier |= Rt().IsBufferAccessed(buffer, offset, size, desc.is_written);
                }
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

void Rasterizer::FinalizeFlatbufUploads() {
    if (pending_flatbufs.empty()) {
        return;
    }
    auto& stream = buffer_cache.GetStreamBuffer();
    const u64 alignment = instance.StorageMinAlignment();
    const u64 tick = Sched().CurrentTick();
    const u64 generation = stream.AllocationGeneration();
    std::array<bool, Shader::NUM_BUFFERS> hits{};
    std::array<u64, Shader::NUM_BUFFERS> offsets{};
    u64 upload_size = 0;
    for (size_t i = 0; i < pending_flatbufs.size(); ++i) {
        const auto& pending = pending_flatbufs[i];
        auto& memo = flatbuf_upload_memos[pending.memo_index];
        hits[i] = pending.reusable &&
                  memo.Matches(std::as_bytes(pending.data), &stream, tick, generation);
        if (hits[i]) {
            offsets[i] = memo.Offset();
        } else {
            upload_size = Common::AlignUp(upload_size, alignment);
            offsets[i] = upload_size;
            upload_size += pending.data.size_bytes();
        }
    }
    if (upload_size != 0 && stream.WouldWrap(upload_size, alignment)) {
        // A wrap invalidates every reused offset. Put all tables in one fresh allocation,
        // so uploading a later table cannot overwrite a table prepared for this draw.
        upload_size = 0;
        for (size_t i = 0; i < pending_flatbufs.size(); ++i) {
            hits[i] = false;
            upload_size = Common::AlignUp(upload_size, alignment);
            offsets[i] = upload_size;
            upload_size += pending_flatbufs[i].data.size_bytes();
        }
    }
    u64 base_offset = 0;
    if (upload_size != 0) {
        const auto [destination, offset] = stream.Map(upload_size, alignment);
        ASSERT_MSG(destination, "Failed to upload shader tables ({:#x} bytes)", upload_size);
        base_offset = offset;
        for (size_t i = 0; i < pending_flatbufs.size(); ++i) {
            if (!hits[i]) {
                const auto data = std::as_bytes(pending_flatbufs[i].data);
                std::memcpy(destination + offsets[i], data.data(), data.size());
            }
        }
        stream.Commit();
    }
    for (size_t i = 0; i < pending_flatbufs.size(); ++i) {
        const auto& pending = pending_flatbufs[i];
        const auto data = std::as_bytes(pending.data);
        if (hits[i]) {
        } else {
            offsets[i] += base_offset;
            if (pending.reusable) {
                flatbuf_upload_memos[pending.memo_index].Capture(
                    data, &stream, Sched().CurrentTick(), stream.AllocationGeneration(),
                    offsets[i]);
            }
        }
        buffer_infos[pending.buffer_info].offset = offsets[i];
    }
    pending_flatbufs.clear();
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    image_bindings.clear();
    image_sources.clear();
    const u32 first_image_idx = image_infos.size();
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;
    // Texture binding memo (Tuning::tex_memo): the image and view found for a descriptor,
    // reused while the texture cache does not change (no image created, deleted or replaced)
    // and the image was not written: no search, no lock, no refresh check.
    const bool use_memo = Core::Runtime::GetPolicy().tex_memo;
    if (use_memo && !tex_memos) {
        tex_memos = std::make_unique<std::array<TexMemo, NumTexMemos>>();
    }
    const u64 cache_generation = use_memo ? texture_cache.Generation() : 0;

    // bind_prefetch: the memo keys of the first binding of the image resources, known before
    // the loop. Those of a stage taken from the scout are the scout's (computed from the same
    // flattened user data, their memo entries prefetched a draw ahead); otherwise they are
    // hashed first, so that the memo lookups of the stage start together.
    std::array<u64, AmdGpu::ScoutStage::MaxImageKeys> stage_keys;
    const u64* known_keys = nullptr;
    u32 num_known_keys = 0;
    bool keys_from_scout = false;
    if (use_memo && Core::Runtime::GetPolicy().bind_prefetch) {
        if (const auto* scouted = pipeline_cache.TakenScoutStage(stage);
            scouted != nullptr && scouted->num_image_keys != 0) {
            known_keys = scouted->image_keys.data();
            num_known_keys = scouted->num_image_keys;
            keys_from_scout = true;
        } else if (stage.images.size() > 1) {
            num_known_keys = std::min<u32>(static_cast<u32>(stage.images.size()),
                                           static_cast<u32>(stage_keys.size()));
            for (u32 k = 0; k < num_known_keys; ++k) {
                const auto& desc = stage.images[k];
                stage_keys[k] = TexMemoKey(desc.GetSharp(stage), 0, desc);
                PrefetchTexMemo(stage_keys[k]);
            }
            known_keys = stage_keys.data();
        }
    }

    u32 image_desc_index = 0;
    for (const auto& image_desc : stage.images) {
        const u32 desc_index = image_desc_index++;
        const auto tsharp = image_desc.GetSharp(stage);
        if (texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_sources.push_back(ImageSource{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_sources.push_back(ImageSource{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);

        for (auto i = 0; i < num_bindings; i++) {
            TexMemo* memo = nullptr;
            u64 memo_key = 0;
            if (use_memo) {
                // What ImageDesc(tsharp, image_desc) depends on (TexMemoKey).
                if (i == 0 && desc_index < num_known_keys) {
                    memo_key = known_keys[desc_index];

                } else {
                    memo_key = TexMemoKey(tsharp, static_cast<u32>(i), image_desc);
                }
                memo = &(*tex_memos)[(memo_key * 0x9E3779B97F4A7C15ULL) >> (64 - NumTexMemoBits)];
                if (memo->key == memo_key && memo->generation == cache_generation &&
                    memo->image_id &&
                    memo->image_generation == texture_cache.ImageGeneration(memo->image_id)) {
                    auto& [image_id, desc] = image_bindings.emplace_back(
                        std::piecewise_construct, std::tuple{memo->image_id}, std::tuple{});
                    desc.view_info = memo->view_info;
                    desc.type = memo->type;
                    image_sources.push_back(ImageSource{
                        .image_desc = &image_desc,
                        .tsharp = tsharp,
                        .index = static_cast<u32>(i),
                        .fresh = true,
                        .view_backing = memo->view_backing,
                        .view_id = memo->view_id,
                    });
                    auto& image = texture_cache.GetImage(memo->image_id);
                    image.tick_accessed_last = scheduler.CurrentTick();
                    if (image.binding.is_bound) {
                        image.binding.force_general |= image_desc.is_written;
                    }
                    image.binding.is_bound = 1u;
                    continue;
                }
            }
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{tsharp, image_desc});
            image_sources.push_back(ImageSource{
                .image_desc = &image_desc,
                .tsharp = tsharp,
                .index = static_cast<u32>(i),
                .fresh = false,
            });

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
            if (memo) {
                // The memo entry is completed in the second pass (view found): the cache
                // generation read after the search, an image it created counts as seen.
                memo->key = memo_key;
                memo->generation = 0;
                memo->image_id = image_id;
                image_sources.back().memo = memo;
            }
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    for (size_t binding_idx = 0; binding_idx < image_bindings.size(); ++binding_idx) {
        auto& [image_id, desc] = image_bindings[binding_idx];
        auto& source = image_sources[binding_idx];
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            bool fresh = source.fresh && !is_storage;
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                if (source.fresh) {
                    // The memo gave the view only: the full descriptor for the search.
                    desc = VideoCore::TextureCache::ImageDesc{source.tsharp, *source.image_desc};
                    if (source.image_desc->mip_fallback_mode ==
                        Shader::MipStorageFallbackMode::ConstantIndex) {
                        desc.view_info.range.base.level += source.image_desc->constant_mip_index;
                        desc.view_info.range.extent.levels = 1;
                    } else if (source.image_desc->mip_fallback_mode ==
                               Shader::MipStorageFallbackMode::DynamicIndex) {
                        desc.view_info.range.base.level += source.index;
                        desc.view_info.range.extent.levels = 1;
                    }
                    source.memo = nullptr;
                }
                image_id = texture_cache.FindImage(desc);
                fresh = false;
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            // A memoized texture is tracked and up to date: its view is enough (FindTexture
            // would lock the cache to check that again), and the memo remembers it.
            VideoCore::ImageView* image_view = nullptr;
            if (fresh) {
                if (source.view_backing == image.backing && source.view_id) {
                    image_view = &texture_cache.GetImageView(source.view_id);
                } else {
                    image_view = &image.FindView(desc.view_info);
                }
            } else {
                image_view = &texture_cache.FindTexture(image_id, desc);
                if (source.memo != nullptr && !is_storage) {
                    const auto& views = image.backing->image_view_ids;
                    const auto& infos = image.backing->image_view_infos;
                    const auto it = std::ranges::find(infos, desc.view_info);
                    *source.memo = TexMemo{
                        .key = source.memo->key,
                        .generation = texture_cache.Generation(),
                        .image_generation = texture_cache.ImageGeneration(image_id),
                        .image_id = image_id,
                        .view_id = it != infos.end() ? views[std::distance(infos.begin(), it)]
                                                     : VideoCore::ImageViewId{},
                        .view_backing = image.backing,
                        .view_info = desc.view_info,
                        .type = desc.type,
                    };
                }
            }
            const auto binding = image.binding;
            const auto& sample_range = image_view->info.range;
            const bool sampled_depth_target =
                stage.sw_stage != Shader::SwStage::Compute && !is_storage &&
                image.info.props.is_depth && db_desc.first &&
                texture_cache.GetImage(db_desc.first).backing == image.backing;
            const auto depth_layout = instance.IsAttachmentFeedbackLoopLayoutSupported()
                                          ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                          : vk::ImageLayout::eGeneral;

            // Aliases and wider sampled views must agree with the attached depth layout.
            // Transition the whole sampled view; BeginRendering adds attachment access.
            if (sampled_depth_target) {
                needs_barrier |=
                    runtime.Transit(&image, depth_layout, vk::PipelineStageFlagBits2::eAllGraphics,
                                    vk::AccessFlagBits2::eShaderRead, sample_range);
            } else if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                }
            } else {
                if (is_storage) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    needs_barrier |= runtime.Transit(
                        &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            descriptor_image_uses.push_back(
                {static_cast<u32>(image_infos.size()), image_id, sample_range, is_storage});
            image_infos.emplace_back(VK_NULL_HANDLE, *image_view->image_view,
                                     sampled_depth_target ? depth_layout
                                                          : image.backing->state.layout);
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                liverpool->regs.ta_bc_base.Address() == 0)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid S# max_aniso={}, filter_mode={}, mip_filter={}, "
                        "border_color_type={}, border_color_base={:#x}",
                        static_cast<u32>(ssharp.max_aniso.Value()),
                        static_cast<u32>(ssharp.filter_mode.Value()),
                        static_cast<u32>(ssharp.mip_filter.Value()),
                        static_cast<u32>(ssharp.border_color_type.Value()),
                        liverpool->regs.ta_bc_base.Address());
            ssharp = AmdGpu::Sampler{};
        }
        // Sampler memo: the handle found for an S# (and border color base) while no sampler
        // was deleted, without the cache lock.
        vk::Sampler vk_sampler{};
        SamplerMemo* sampler_memo = nullptr;
        u64 sampler_key = 0;
        if (use_memo) {
            struct SamplerKeyData {
                AmdGpu::Sampler ssharp;
                u64 border_color_base;
                u32 is_depth;
                u32 padding;
            } key_data{
                .ssharp = ssharp,
                .border_color_base = liverpool->regs.ta_bc_base.Address(),
                .is_depth = sampler.is_depth ? 1u : 0u,
                .padding = 0,
            };
            static_assert(sizeof(SamplerKeyData) == sizeof(AmdGpu::Sampler) + 16);
            sampler_key = XXH3_64bits(&key_data, sizeof(key_data)) | 1;
            sampler_memo = &sampler_memos[(sampler_key * 0x9E3779B97F4A7C15ULL) >> (64 - 10)];
            if (sampler_memo->key == sampler_key &&
                sampler_memo->generation == texture_cache.SamplersGeneration()) {
                vk_sampler = sampler_memo->sampler;
            }
        }
        if (!vk_sampler) {
            vk_sampler =
                texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base, sampler.is_depth);
            if (sampler_memo != nullptr) {
                *sampler_memo = SamplerMemo{
                    .key = sampler_key,
                    .generation = texture_cache.SamplersGeneration(),
                    .sampler = vk_sampler,
                };
            }
        }
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = {};
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.ConsumeMetaClear(col_buf.CmaskAddress(), slice);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop |= vk::ImageAspectFlagBits::eColor;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear =
            texture_cache.ConsumeMetaClear(htile_address, slice) ||
            (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const bool sampled = std::ranges::any_of(descriptor_image_uses, [&](const auto& use) {
            return !use.is_storage && texture_cache.GetImage(use.image).backing == image.backing;
        });
        const auto new_layout =
            sampled ? (instance.IsAttachmentFeedbackLoopLayoutSupported()
                           ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                           : vk::ImageLayout::eGeneral)
                    : DepthTargetLayout(has_stencil, desc.view_info.is_storage, stencil_write);
        if (sampled) {
            attachment_feedback_loop |= image.aspect_mask;
            // Resolve every sampled alias after all stages have bound their resources.
            // Include the entire sampled view, even if it is wider than the attachment.
            for (const auto& use : descriptor_image_uses) {
                auto& sampled_image = texture_cache.GetImage(use.image);
                if (!use.is_storage && sampled_image.backing == image.backing) {
                    needs_barrier |= runtime.Transit(&sampled_image, new_layout,
                                                     vk::PipelineStageFlagBits2::eAllGraphics,
                                                     vk::AccessFlagBits2::eShaderRead, use.range);
                    image_infos[use.index].imageLayout = new_layout;
                }
            }
        }
        const auto shader_stage =
            sampled ? vk::PipelineStageFlagBits2::eAllGraphics : vk::PipelineStageFlagBits2::eNone;
        const auto shader_access =
            sampled ? vk::AccessFlagBits2::eShaderRead : vk::AccessFlagBits2::eNone;
        needs_barrier |=
            runtime.Transit(&image, new_layout,
                            vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                vk::PipelineStageFlagBits2::eLateFragmentTests | shader_stage,
                            vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                vk::AccessFlagBits2::eDepthStencilAttachmentRead | shader_access,
                            desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ;
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    ;

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ;
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (!is_gds) {
        ForgetCountedResults(address, num_bytes);
    };
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.HasGpuData(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            buffer_cache.OnGpuThreadWrite(address, num_bytes);
            return;
        }
    }
    // asc_queue: a fill of one of the guest's compute queues runs on the compute queue.
    const ComputeQueueScope queue_scope{*this, UseComputeQueueForCopy()};
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            buffer_cache.NoteGdsAccess(true);
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    Rt().FillBuffer(buffer, offset, num_bytes, value);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (!dst_gds) {
        ForgetCountedResults(dst, num_bytes);
    };
    if (!dst_gds && !buffer_cache.HasGpuData(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.HasGpuData(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            buffer_cache.OnGpuThreadWrite(dst, num_bytes);
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    // asc_queue: a copy of one of the guest's compute queues runs on the compute queue (unless
    // the source may come from an image, tracked for the graphics queue).
    const ComputeQueueScope queue_scope{
        *this,
        UseComputeQueueForCopy() && (src_gds || !texture_cache.FindImageFromRange(src, num_bytes))};
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            buffer_cache.NoteGdsAccess(false);
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            buffer_cache.NoteGdsAccess(true);
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    Rt().CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
}

OcclusionDumpResult Rasterizer::OcclusionDump(VAddr address, u32 num_pairs) {
    if (!Core::Runtime::GetPolicy().real_occlusion) {
        // Real queries just turned off: a query begun counted (nothing written at its begin
        // dump) gets consistent emulated counters at both dumps.
        const bool begun_counted =
            address % 16 == sizeof(u64) && occlusion.IsOpen(address - sizeof(u64));
        if (occlusion.HasOpen()) {
            occlusion.AbandonAll();
        }
        counted_results.clear();
        return begun_counted ? OcclusionDumpResult::EmulatedWithBegin
                             : OcclusionDumpResult::Emulated;
    }
    num_pairs = std::clamp<u32>(num_pairs, 1, OcclusionQueries::MaxCounterPairs);
    const u32 size = num_pairs * 2 * sizeof(u64);
    const auto emulated = [this, address](OcclusionDumpResult result, std::string_view why) {
        return result;
    };
    if (address % 16 == 0) {
        // Begin dump: nothing is written, the GPU writes the whole query at the end dump.
        ForgetCountedResults(address, size);
        if (!IsMapped(address, size)) {
            return emulated(OcclusionDumpResult::Emulated, "unmapped");
        }
        if (!occlusion.Begin(address, num_pairs)) {
            return emulated(OcclusionDumpResult::Emulated, "too many open");
        };
        return OcclusionDumpResult::Counted;
    }
    if (address % 16 != sizeof(u64)) {
        return emulated(OcclusionDumpResult::Emulated, "unaligned");
    }
    // End dump. The begin dump of the query was not written: when the GPU did not count the
    // query, both are emulated, so that the query sees all its samples visible.
    const VAddr results = address - sizeof(u64);
    ForgetCountedResults(results, size);
    boost::container::static_vector<u32, OcclusionQueries::MaxCounterPairs> queries;
    if (!occlusion.End(results, queries)) {
        // The other open queries go on counting.
        occlusion.Resume();
        return emulated(OcclusionDumpResult::EmulatedWithBegin, "not counted");
    }
    texture_cache.InvalidateMemoryFromGPU(results, size);
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(results, size, true);
    const std::span<const u32> used{queries.data(), queries.size()};
    runtime.WriteOcclusionResults(buffer, offset, num_pairs, occlusion.Pool(), used);
    occlusion.MarkUsed(used);
    if (counted_results.size() == MaxCountedResults) {
        counted_results.erase(counted_results.begin());
    }
    counted_results.push_back(CountedResults{
        .address = results,
        .num_pairs = num_pairs,
        .num_counts = std::min<u32>(static_cast<u32>(queries.size()), num_pairs),
    });
    // Writing the results ended the rendering scope: the other open queries go on counting from
    // the next one.
    occlusion.Resume();
    return OcclusionDumpResult::Counted;
}

bool Rasterizer::IsCountedOcclusionResult(VAddr address, std::optional<u32>& value) const {
    // Written by Runtime::WriteOcclusionResults: every counter is valid (bit 63), begin counters
    // are 0 and the low half of an end counter holds the samples of its segment, if any.
    static constexpr u32 ValidBit = 0x80000000U;
    value.reset();
    for (auto it = counted_results.rbegin(); it != counted_results.rend(); ++it) {
        if (address < it->address || address >= it->address + it->num_pairs * 2 * sizeof(u64)) {
            continue;
        }
        const u64 offset = address - it->address;
        const u32 pair = static_cast<u32>(offset / (2 * sizeof(u64)));
        switch ((offset % (2 * sizeof(u64))) / sizeof(u32)) {
        case 0: // Begin counter, low half.
            value = 0U;
            break;
        case 2: // End counter, low half: the samples counted, unknown until the GPU is done.
            if (pair >= it->num_counts) {
                value = 0U;
            }
            break;
        default: // High halves.
            value = ValidBit;
            break;
        }
        return true;
    }
    return false;
}

void Rasterizer::ForgetCountedResults(VAddr address, u64 size) {
    if (counted_results.empty()) [[likely]] {
        return;
    }
    const auto overlaps = [address, size](const CountedResults& r) {
        return address < r.address + r.num_pairs * 2 * sizeof(u64) && r.address < address + size;
    };
    counted_results.erase(std::remove_if(counted_results.begin(), counted_results.end(), overlaps),
                          counted_results.end());
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    const auto [opened_addr, opened_size] =
        buffer_cache.InvalidateMemory(addr, size, assume_locks, true);
    texture_cache.InvalidateMemory(addr, size);
    if (opened_size != 0) {
        texture_cache.InvalidateMemory(opened_addr, opened_size);
    }
    return true;
}

void Rasterizer::OnGpuThreadWrite(VAddr addr, u64 size) {
    buffer_cache.OnGpuThreadWrite(addr, size);
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks, u64 guest_rip) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size, false, assume_locks, guest_rip);
    if (false) [[unlikely]] {
        // Frame x-ray: the GPU results the guest reads back (first dwords at the address).
        std::array<u32, 4> values{};
        std::memcpy(values.data(), reinterpret_cast<const void*>(addr & ~VAddr{0xF}),
                    sizeof(values));
    }
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    pipeline_cache.NotifyMemoryMapChanged();
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) { page_manager.OnGpuMap(addr, size); }

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    pipeline_cache.NotifyMemoryMapChanged();
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.Recorder());
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#ifdef __linux__
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}
#endif

} // namespace Vulkan
