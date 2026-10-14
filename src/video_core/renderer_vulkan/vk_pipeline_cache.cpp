// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <limits>
#include <mutex>
#include <ranges>
#include <thread>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <xxhash.h>

#include "common/hash.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/perf_stats.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_driver_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

/**
 * Worker threads creating the Vulkan pipeline objects whose compilation was deferred, so the
 * command processor does not stall for the driver (up to a few hundred milliseconds for each new
 * pipeline, which froze the game when an effect was seen for the first time).
 */
class PipelineCompiler {
public:
    explicit PipelineCompiler(u32 num_threads) {
        workers.reserve(num_threads);
        for (u32 i = 0; i < num_threads; ++i) {
            workers.emplace_back([this, i](std::stop_token stoken) { WorkerLoop(stoken, i); });
        }
    }

    ~PipelineCompiler() {
        WaitIdle();
        for (auto& worker : workers) {
            worker.request_stop();
        }
        workers.clear();
    }

    void Submit(Pipeline* pipeline, bool bounded = false) {
        {
            std::unique_lock lk{mutex};
            // Runtime bursts cannot indefinitely get ahead of the compiler. Startup warmup
            // already has its own memory budget and does not use this queue limit.
            if (bounded && pending >= MaxRuntimePending) {
                const u64 start = Common::Perf::NowNs();
                done_cv.wait(lk, [this] { return pending < MaxRuntimePending; });
                Common::Perf::Add(Common::Perf::ComputeCompileQueueWaitNs,
                                  Common::Perf::NowNs() - start);
            }
            queue.push_back(pipeline);
            ++pending;
        }
        work_cv.notify_one();
    }

    /// Waits until the pipeline is compiled or the timeout expires, returns true if it is ready.
    /// Does not wait when every worker is busy: the pipeline would not even start in time.
    bool WaitFor(const Pipeline* pipeline, std::chrono::microseconds timeout) {
        if (pipeline->IsReady()) {
            return true;
        }
        std::unique_lock lk{mutex};
        if (pending > workers.size()) {
            return false;
        }
        return done_cv.wait_for(lk, timeout, [pipeline] { return pipeline->IsReady(); });
    }

    /// Waits until every submitted pipeline is compiled.
    void WaitIdle() {
        std::unique_lock lk{mutex};
        done_cv.wait(lk, [this] { return pending == 0; });
    }

private:
    static constexpr u32 MaxRuntimePending = 64;
    void WorkerLoop(std::stop_token stoken, u32 index) {
        const auto name = fmt::format("shadPS4:PipelineCompiler{}", index);
        Common::SetCurrentThreadName(name.c_str());
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        while (!stoken.stop_requested()) {
            Pipeline* pipeline{};
            {
                std::unique_lock lk{mutex};
                Common::CondvarWait(work_cv, lk, stoken, [this] { return !queue.empty(); });
                if (stoken.stop_requested()) {
                    return;
                }
                pipeline = queue.front();
                queue.pop_front();
            }
            pipeline->Compile();
            {
                std::scoped_lock lk{mutex};
                --pending;
            }
            done_cv.notify_all();
        }
    }

    std::mutex mutex;
    std::condition_variable_any work_cv;
    std::condition_variable done_cv;
    std::deque<Pipeline*> queue;
    u32 pending{};
    std::vector<std::jthread> workers;
};

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

static void BuildSwRuntimeInfo(Shader::RuntimeInfo& info, const AmdGpu::Regs& regs, SwStage l_stage,
                               const DrawIndirectParams& indirect_params) {
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
}

void PipelineCache::FillColorBuffers(
    const AmdGpu::Regs& regs,
    std::array<Shader::PsColorBuffer, Shader::MaxColorBuffers>& color_buffers) {
    color_buffers.fill({});
    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }
}

bool PipelineCache::BuildRuntimeInfoFrom(
    Shader::RuntimeInfo& info, const AmdGpu::Regs& regs, HwStage stage, SwStage l_stage,
    const std::array<Shader::PsColorBuffer, Shader::MaxColorBuffers>& color_buffers,
    const DrawIndirectParams& indirect_params) const {
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local:
    case HwStage::Hull:
        // Read constants from memory through the hull shader: BuildRuntimeInfo.
        return false;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            switch (regs.tess_config.topology) {
            case AmdGpu::TessellationTopology::Point:
                info.hw.gs.in_primitive = AmdGpu::PrimitiveType::PointList;
                break;
            case AmdGpu::TessellationTopology::Line:
                info.hw.gs.in_primitive = AmdGpu::PrimitiveType::LineList;
                break;
            case AmdGpu::TessellationTopology::TriangleCw:
            case AmdGpu::TessellationTopology::TriangleCcw:
                info.hw.gs.in_primitive = AmdGpu::PrimitiveType::TriangleList;
                break;
            default:
                return false;
            }
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute:
        return false;
    default:
        return false;
    }
    BuildSwRuntimeInfo(info, regs, l_stage, indirect_params);
    return true;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(HwStage stage, SwStage l_stage) {
    auto& info = runtime_infos[u32(l_stage)];
    const auto& regs = liverpool->regs;
    if (BuildRuntimeInfoFrom(info, regs, stage, l_stage, graphics_key.color_buffers,
                             draw_indirect_params)) {
        if (stage == HwStage::Geometry) {
            DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                       "copy.bin");
        }
        return info;
    }
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Geometry:
        UNREACHABLE();
        break;
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    BuildSwRuntimeInfo(info, regs, l_stage, draw_indirect_params);
    return info;
}

namespace {
template <typename InfoT>
u64 SpecializationKey(const InfoT& info, const Shader::RuntimeInfo& runtime_info,
                      const Shader::Backend::Bindings& binding);
template <typename InfoT>
bool VertexAttributesMatch(const InfoT& info, const Shader::RuntimeInfo& runtime_info,
                           const Shader::StageSpecialization& spec);
} // Anonymous namespace

Program* PipelineCache::FindProgram(u64 hash) {
    std::shared_lock lock{program_cache_mutex};
    const auto it = program_cache.find(hash);
    return it != program_cache.end() ? it->second.get() : nullptr;
}

namespace {

/// What the specialization key reads of a stage, for the scout thread (Shader::Info is the
/// command processor's, with its own user data and flattened buffer).
struct ScoutInfo {
    std::span<const u32> user_data;
    std::span<u32> flattened_ud_buf;
    VAddr pgm_base;
    bool has_fetch_shader;
    u32 fetch_shader_sgpr_base;
    Shader::SwStage sw_stage;
    const Shader::BufferResourceList& buffers;
    const Shader::ImageResourceList& images;
    const Shader::FMaskResourceList& fmasks;
    const Shader::SamplerResourceList& samplers;

    template <typename T>
    T ReadUdSharp(u32 sharp_idx) const noexcept {
        T data;
        std::memcpy(&data, &flattened_ud_buf[sharp_idx], sizeof(T));
        return data;
    }

    template <typename T>
    T ReadUdReg(u32 ptr_index, u32 dword_offset) const noexcept {
        T data;
        const u32* base = user_data.data();
        if (ptr_index != Shader::IR::NumScalarRegs) {
            std::memcpy(&base, &user_data[ptr_index], sizeof(base));
            base = reinterpret_cast<const u32*>(VAddr(base) & 0xFFFFFFFFFFFFULL);
        }
        std::memcpy(&data, base + dword_offset, sizeof(T));
        return data;
    }
};

} // Anonymous namespace

void PipelineCache::ScoutDraw(const AmdGpu::Regs& regs, AmdGpu::ScoutDraw& draw,
                              AmdGpu::ScoutArena& arena) {
    draw.num_stages = 0;
    std::array<Shader::PsColorBuffer, Shader::MaxColorBuffers> color_buffers;
    FillColorBuffers(regs, color_buffers);
    const DrawIndirectParams indirect_params{
        .vertex_sgpr_offset = draw.vertex_sgpr_offset,
        .instance_sgpr_offset = draw.instance_sgpr_offset,
    };
    Shader::Backend::Bindings binding{};
    // Mirrors RefreshGraphicsStages: the stages in the order they are bound, the bindings
    // accumulating. Returns false when the stages after this one cannot be precomputed.
    const auto scout_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        if (!regs.stage_enable.IsStageEnabled(static_cast<u32>(stage_in))) {
            return true;
        }
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage_in));
        if (!pgm || !pgm->Address<u32*>()) {
            return true;
        }
        if (draw.num_stages >= AmdGpu::ScoutDraw::MaxStages) {
            return false;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        auto& out = draw.stages[draw.num_stages++];
        // Entries are reused without being cleared: every field read is set below.
        out.valid = false;
        out.resolved = false;
        out.num_image_keys = 0;
        out.num_buffer_ranges = 0;
        out.hw_stage = static_cast<u8>(stage_in);
        out.sw_stage = static_cast<u8>(stage_out);
        out.pgm_base = params.Base();
        out.start = binding;
        out.ud_hash = XXH3_64bits(params.user_data.data(), params.user_data.size_bytes());
        Program* program = FindProgram(params.hash);
        if (program == nullptr) {
            // Not compiled yet: the command processor compiles it, and only it knows the
            // bindings of the stages after it.
            return false;
        }
        const auto& pinfo = program->info;
        Shader::RuntimeInfo runtime_info = runtime_infos[0];
        if (!BuildRuntimeInfoFrom(runtime_info, regs, stage_in, stage_out, color_buffers,
                                  indirect_params)) {
            return false;
        }
        out.SetRuntimeInfo(runtime_info);
        // The user data, flattened with the tables it points to (as Info::RefreshFlatBuf).
        const u32 flat_size = pinfo.srt_info.flattened_bufsize_dw;
        u32* flat = arena.Allocate(std::max<u32>(flat_size, 1));
        if (flat == nullptr) {
            return false;
        }
        std::memcpy(flat, params.user_data.data(),
                    std::min<size_t>(params.user_data.size_bytes(), flat_size * sizeof(u32)));
        if (pinfo.srt_info.walker_func) {
            // Tables the command processor itself writes before this draw are not readable
            // yet: no entry for the stage.
            const auto* scout = liverpool->GetScout();
            for (u32 i = 0; i + 1 < params.user_data.size(); ++i) {
                VAddr pointer;
                std::memcpy(&pointer, &params.user_data[i], sizeof(pointer));
                pointer &= 0xFFFFFFFFFFFFULL;
                if (pointer != 0 && scout != nullptr && scout->MayBeWrittenLater(pointer, 4_KB)) {
                    binding.buffer += static_cast<u32>(pinfo.buffers.size());
                    binding.unified += static_cast<u32>(pinfo.buffers.size() +
                                                        pinfo.images.size() +
                                                        pinfo.samplers.size());
                    return true;
                }
            }
            pinfo.srt_info.walker_func(params.user_data.data(), flat);
        }
        const ScoutInfo shim{
            .user_data = params.user_data,
            .flattened_ud_buf = std::span<u32>{flat, flat_size},
            .pgm_base = params.Base(),
            .has_fetch_shader = pinfo.has_fetch_shader,
            .fetch_shader_sgpr_base = pinfo.fetch_shader_sgpr_base,
            .sw_stage = stage_out,
            .buffers = pinfo.buffers,
            .images = pinfo.images,
            .fmasks = pinfo.fmasks,
            .samplers = pinfo.samplers,
        };
        out.memo_key = SpecializationKey(shim, runtime_info, binding);
        out.flat = flat;
        out.flat_size = flat_size;
        // Lookups of the stage's resources (Tuning::bind_prefetch), as the command processor
        // makes them from the same flattened user data: texture memo keys of the image
        // resources in order, ranges of the buffers read only.
        out.num_image_keys = 0;
        out.num_buffer_ranges = 0;
        if (Common::Perf::GetTuning().bind_prefetch.load(std::memory_order_relaxed)) {
            for (const auto& image_desc : pinfo.images) {
                if (out.num_image_keys == AmdGpu::ScoutStage::MaxImageKeys) {
                    break;
                }
                out.image_keys[out.num_image_keys++] =
                    TexMemoKey(image_desc.GetSharp(shim), 0, image_desc);
            }
            for (const auto& desc : pinfo.buffers) {
                if (out.num_buffer_ranges == AmdGpu::ScoutStage::MaxBufferRanges) {
                    break;
                }
                if (desc.IsSpecial() || desc.is_written) {
                    continue;
                }
                const auto vsharp = desc.GetSharp(shim);
                if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
                    continue;
                }
                out.buffer_addresses[out.num_buffer_ranges] = vsharp.base_address;
                out.buffer_sizes[out.num_buffer_ranges] = static_cast<u32>(vsharp.GetSize());
                ++out.num_buffer_ranges;
            }
        }
        out.program = program;
        out.params_hash = params.hash;
        out.code_dwords = static_cast<u32>(params.code.size());
        std::memcpy(out.user_data.data(), params.user_data.data(), sizeof(out.user_data));
        // The permutation the program's memo has for the key, checked as the command
        // processor would (Tuning::scout_fast: the command processor then takes it as is).
        u32 perm_idx;
        if (program->ReadMemo(out.memo_key, perm_idx)) {
            // The permutations change under the command processor's exclusive lock.
            std::shared_lock lock{program_cache_mutex};
            if (perm_idx < program->modules.size() &&
                VertexAttributesMatch(shim, runtime_info, program->modules[perm_idx].spec)) {
                out.perm_idx = perm_idx;
                out.resolved = true;
            }
        }
        out.valid = true;
        binding.buffer += static_cast<u32>(pinfo.buffers.size());
        binding.unified +=
            static_cast<u32>(pinfo.buffers.size() + pinfo.images.size() + pinfo.samplers.size());
        return true;
    };
    if (!scout_stage(HwStage::Fragment, SwStage::Fragment)) {
        return;
    }
    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        scout_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (scout_stage(HwStage::Export, SwStage::Vertex)) {
            scout_stage(HwStage::Geometry, SwStage::Geometry);
        }
        break;
    default:
        // Tessellation: the stages read constants from memory, not precomputed.
        break;
    }
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .lds_barriers_multi_wave =
            Common::Perf::GetTuning().lds_barriers_multi_wave.load(std::memory_order_relaxed),
        .wave_memory_barriers =
            Common::Perf::GetTuning().wave_memory_barriers.load(std::memory_order_relaxed),
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };
    // Each recompiler option changing compute shaders has its own bit: cached shaders compiled
    // with other options are compiled again once.
    const bool lds_multi_wave = profile.needs_lds_barriers && profile.lds_barriers_multi_wave;
    SetComputeShaderVariant((lds_multi_wave ? 1u : 0u) | (profile.wave_memory_barriers ? 2u : 0u));
    LOG_INFO(Render_Vulkan,
             "Compute shader LDS barriers for multi wave workgroups: {}, buffer memory barriers "
             "within split GCN waves: {}",
             lds_multi_wave, profile.wave_memory_barriers);
    prewarm_budget =
        PrewarmBudget::FromAvailable(AvailableHostMemory(), AvailableDeviceMemory(instance));
    const auto cache_path = EmulatorSettings.IsPipelineCacheEnabled()
                                ? Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                                      Common::ElfInfo::Instance().GameSerial() / "pipeline-v2" /
                                      "driver.vkc"
                                : std::filesystem::path{};
    driver_cache =
        std::make_unique<DriverPipelineCache>(instance, cache_path, prewarm_budget.driver_bytes);
    const u32 num_compilers = std::clamp(std::thread::hardware_concurrency() / 4, 2U, 8U);
    compiler = std::make_unique<PipelineCompiler>(num_compilers);
    LOG_INFO(Render_Vulkan, "Pipelines can be compiled by {} worker threads", num_compilers);
    WarmUp(); // Both compiler and driver cache exist before cached pipelines are created.
    driver_cache->Start();
}

PipelineCache::~PipelineCache() {
    // Workers reference the pipelines, stop them first.
    compiler.reset();
    driver_cache->Stop();
}

void PipelineCache::CompilePreloaded(Pipeline* pipeline) {
    pipeline->SetDriverCache(driver_cache.get());
    compiler->Submit(pipeline);
}
void PipelineCache::FinishPreloaded() {
    compiler->WaitIdle();
}
bool PipelineCache::CanPrewarm(u64 bytes) {
    if (bytes > prewarm_budget.host_bytes -
                    std::min(prewarm_estimated_bytes, prewarm_budget.host_bytes) ||
        !prewarm_budget.CanContinue(AvailableHostMemory(), AvailableDeviceMemory(instance),
                                    prewarm_estimated_bytes))
        return false;
    prewarm_estimated_bytes += bytes;
    return true;
}

void PipelineCache::CompileAsync(Pipeline* pipeline) {
    compiler->Submit(pipeline);
    // A pipeline found in the driver cache is ready within a few milliseconds, which is worth
    // waiting for instead of skipping the draw.
    const u32 wait_us =
        Common::Perf::GetTuning().async_pipeline_wait_us.load(std::memory_order_relaxed);
    if (wait_us != 0) {
        compiler->WaitFor(pipeline, std::chrono::microseconds{wait_us});
    }
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params) {
    draw_indirect_params = params;
    if (!RefreshGraphicsKey()) {
        return nullptr;
    }
    // Pipeline key memo: draws in a row mostly use the same pipeline, found again without
    // hashing the key and probing the map.
    const bool key_memo =
        Common::Perf::GetTuning().pipeline_key_memo.load(std::memory_order_relaxed);
    if (key_memo && last_graphics_pipeline != nullptr && last_graphics_key == graphics_key) {
        Common::Perf::Add(Common::Perf::PipelineKeyMemoHits);
        return last_graphics_pipeline;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    if (!is_new) {
        last_graphics_key = graphics_key;
        last_graphics_pipeline = it->second.get();
    }
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);

        const u64 start_ns = Common::Perf::NowNs();
        const bool defer =
            Common::Perf::GetTuning().async_pipelines.load(std::memory_order_relaxed) >= 1;
        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, graphics_key, driver_cache->Handle(), infos,
            runtime_infos, fetch_shader, modules, sdata, false, true);
        GraphicsPipeline* const pipeline = it.value().get();
        pipeline->SetDriverCache(driver_cache.get());
        last_graphics_key = graphics_key;
        last_graphics_pipeline = pipeline;

        RegisterPipelineData(graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (infos[stage]) {
                    auto& m = modules[stage];
                    module_related_pipelines[m].emplace_back(graphics_key);
                }
            }
        }
        if (defer) {
            CompileAsync(pipeline);
        } else {
            pipeline->Compile();
        }
        Common::Perf::Add(Common::Perf::PipelineStallNs, Common::Perf::NowNs() - start_ns);
        return pipeline;
    }
    return it->second.get();
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);

        const u64 start_ns = Common::Perf::NowNs();
        const bool retain = Common::Perf::GetTuning().compute_compile_parallel.load(
            std::memory_order_relaxed);
        const bool defer =
            Common::Perf::GetTuning().async_pipelines.load(std::memory_order_relaxed) >= 2;
        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       driver_cache->Handle(), compute_key, *infos[0],
                                                       modules[0], sdata, false, true);
        ComputePipeline* const pipeline = it.value().get();
        pipeline->SetDriverCache(driver_cache.get());
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
        if (retain) {
            pipeline->RetainComputeWork();
            compiler->Submit(pipeline, true);
            Common::Perf::Add(Common::Perf::ComputeCompilesQueued);
        } else if (defer) {
            CompileAsync(pipeline);
        } else {
            pipeline->Compile();
        }
        Common::Perf::Add(Common::Perf::PipelineStallNs, Common::Perf::NowNs() - start_ns);
        return pipeline;
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey() {
    std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // First pass to fill render target information needed by shader recompiler
    FillColorBuffers(regs, key.color_buffers);

    // Compile and bind shader stages
    if (!RefreshGraphicsStages()) {
        return false;
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

template <typename ProgramRegs>
Shader::ShaderParams PipelineCache::GetStageParams(const ProgramRegs& pgm) {
    constexpr size_t HeaderBytes = offsetof(AmdGpu::BinaryInfo, shader_hash) + sizeof(u64);
    if (!Common::Perf::GetTuning().shader_params_memo.load(std::memory_order_relaxed)) {
        return AmdGpu::GetParams(pgm);
    }
    const auto* code = pgm.template Address<u32*>();
    const VAddr address = reinterpret_cast<VAddr>(code);
    // Read before the code: a submission or guest wait while reading makes the entry unusable.
    const u64 epoch = VideoCore::CurrentUploadEpoch();
    const u64 host_write_gen = VideoCore::CurrentHostWriteGen();
    const u64 mapping_generation = shader_mapping_generation.load(std::memory_order_acquire);
    const auto memory_unchanged = [&] {
        return VideoCore::CurrentUploadEpoch() == epoch &&
               VideoCore::CurrentHostWriteGen() == host_write_gen &&
               shader_mapping_generation.load(std::memory_order_acquire) == mapping_generation;
    };
    const u32 set_index =
        (address * 0x9E3779B97F4A7C15ULL) >> (64 - NumParamsMemoBits);
    auto& set = params_memos[set_index];
    const auto found = std::ranges::find_if(set, [address](const auto& entry) {
        return entry.address == address && entry.num_dwords != 0;
    });
    const u32 way = found != set.end() ? static_cast<u32>(found - set.begin())
                                     : params_memo_next[set_index];
    auto& memo = set[way];
    params_memo_next[set_index] = (way + 1) % ParamsMemoWays;
    const bool same_mapping =
        memo.address == address && memo.mapping_generation == mapping_generation;
    bool hit = same_mapping && memo.num_dwords != 0 && memo.epoch == epoch &&
               memo.host_write_gen == host_write_gen;
    const bool revalidate =
        Common::Perf::GetTuning().runtime_lookup_memo.load(std::memory_order_relaxed);
    if (!hit && same_mapping && memo.num_dwords != 0 && revalidate &&
        memo.scan_input.Size() != 0) {
        const auto* bytes = reinterpret_cast<const std::byte*>(code);
        const bool same_scan = memo.scan_input.Matches({bytes, memo.scan_input.Size()});
        Common::Perf::Add(Common::Perf::ParamsValidationBytes, memo.scan_input.Size());
        // For a scanned header the snapshot already includes the metadata. A direct header
        // may lie beyond the bounded search prefix and is checked separately.
        const bool same_header =
            same_scan && (!memo.direct_header ||
                          memo.header_input.Matches({bytes + memo.header_offset,
                                                     HeaderBytes}));
        if (same_header && memory_unchanged()) {
            hit = true;
            memo.epoch = epoch;
            memo.host_write_gen = host_write_gen;
            Common::Perf::Add(Common::Perf::ParamsMemoRevalidated);
        }
    }
    if (hit) {
        Common::Perf::Add(Common::Perf::ParamsMemoHits);
        return {
            .user_data = pgm.user_data,
            .code = std::span<const u32>{code, memo.num_dwords},
            .hash = memo.hash,
        };
    }
    Common::Perf::Add(Common::Perf::ParamsMemoMisses);
    Common::Perf::Add(Common::Perf::ParamsHeaderScans);
    const auto& header = AmdGpu::SearchBinaryInfo(code);
    // Only snapshot fields read by GetParams; CRC and trailing ABI padding are not needed.
    AmdGpu::BinaryInfo header_copy{};
    std::memcpy(&header_copy, &header, HeaderBytes);
    const u32 num_dwords = header_copy.length / sizeof(u32);
    const u64 shader_hash = header_copy.shader_hash;
    const auto header_snapshot = std::as_bytes(std::span{&header_copy, 1}).first(HeaderBytes);
    const Shader::ShaderParams params{
        .user_data = pgm.user_data,
        .code = std::span<const u32>{code, num_dwords},
        .hash = shader_hash,
    };
    memo.address = address;
    memo.epoch = epoch;
    memo.host_write_gen = host_write_gen;
    memo.mapping_generation = mapping_generation;
    memo.hash = params.hash;
    memo.num_dwords = static_cast<u32>(params.code.size());
    memo.scan_input.Invalidate();
    memo.header_input.Invalidate();
    if (revalidate) {
        const auto* bytes = reinterpret_cast<const std::byte*>(code);
        const auto* header_bytes = reinterpret_cast<const std::byte*>(&header);
        const size_t header_offset = header_bytes - bytes;
        std::array<u32, 2> code_start;
        std::memcpy(code_start.data(), code, sizeof(code_start));
        memo.direct_header = code_start[0] == 0xBEEB03FF &&
                             &header == std::bit_cast<const AmdGpu::BinaryInfo*>(
                                            code + (code_start[1] + 1) * 2);
        // SearchBinaryInfo's fallback examines at most 0x4000 words. Larger direct
        // offsets are valid, but cannot be narrowed into the stored byte offset.
        if (header_offset <= std::numeric_limits<u32>::max()) {
            memo.header_offset = static_cast<u32>(header_offset);
            if (memo.direct_header) {
                memo.scan_input.Capture(std::as_bytes(std::span{code_start}));
                memo.header_input.Capture(header_snapshot);
            } else {
                const size_t scan_size = header_offset + HeaderBytes;
                if (memo.scan_input.Capture({bytes, scan_size})) {
                    // Verify the owned copy still selects the same first header. A guest
                    // mutation while it was copied must not cache a different scan's result.
                    const auto snapshot = memo.scan_input.Data();
                    const auto signature_at = [&](size_t offset) {
                        return offset + AmdGpu::BinaryInfo::signature_ref.size() <= snapshot.size() &&
                               std::memcmp(snapshot.data() + offset,
                                           AmdGpu::BinaryInfo::signature_ref.data(),
                                           AmdGpu::BinaryInfo::signature_ref.size()) == 0;
                    };
                    bool valid = true;
                    u32 token, immediate;
                    std::memcpy(&token, snapshot.data(), sizeof(token));
                    std::memcpy(&immediate, snapshot.data() + sizeof(u32), sizeof(immediate));
                    if (token == 0xBEEB03FF) {
                        const u64 direct_offset = u64{u32{immediate + 1}} * 2 * sizeof(u32);
                        valid = direct_offset <= snapshot.size() - HeaderBytes;
                        if (valid && signature_at(direct_offset)) {
                            valid = direct_offset == header_offset;
                        }
                    }
                    for (size_t offset = 0; valid && offset < header_offset; offset += sizeof(u32)) {
                        valid = !signature_at(offset);
                    }
                    if (!valid || std::memcmp(snapshot.data() + header_offset, header_snapshot.data(),
                                              HeaderBytes) != 0) {
                        memo.scan_input.Invalidate();
                    }
                }
            }
            if (header.length / sizeof(u32) != num_dwords || header.shader_hash != shader_hash) {
                memo.scan_input.Invalidate();
                memo.header_input.Invalidate();
            }
        }
        // A wait/readback while reading may have changed guest memory. Keep the original
        // epoch (so the next lookup misses), and never carry that snapshot across epochs.
        if (!memory_unchanged()) {
            memo.scan_input.Invalidate();
            memo.header_input.Invalidate();
        }
    }
    return params;
}

template <typename ProgramRegs>
bool PipelineCache::TakeScoutedStage(Shader::HwStage stage_in, Shader::SwStage stage_out,
                                     const ProgramRegs& pgm, Shader::Backend::Bindings& binding) {
    if (!Common::Perf::GetTuning().scout_fast.load(std::memory_order_relaxed)) {
        return false;
    }
    const auto* draw = liverpool->CurrentScoutDraw();
    if (draw == nullptr) {
        return false;
    }
    const AmdGpu::ScoutStage* scouted = nullptr;
    for (u32 i = 0; i < draw->num_stages; ++i) {
        if (draw->stages[i].sw_stage == static_cast<u8>(stage_out)) {
            scouted = &draw->stages[i];
            break;
        }
    }
    if (scouted == nullptr || !scouted->valid || !scouted->resolved ||
        scouted->hw_stage != static_cast<u8>(stage_in)) {
        return false;
    }
    // The scout's inputs must be the command processor's: program, bindings so far, user data.
    const auto* code = pgm.template Address<u32*>();
    if (scouted->pgm_base != reinterpret_cast<VAddr>(code) || scouted->start != binding ||
        std::memcmp(scouted->user_data.data(), pgm.user_data.data(),
                    sizeof(scouted->user_data)) != 0) {
        Common::Perf::Add(Common::Perf::ScoutStale);
        return false;
    }
    auto* program = static_cast<Program*>(const_cast<void*>(scouted->program));
    auto& info = program->info;
    if (scouted->flat_size != info.srt_info.flattened_bufsize_dw ||
        scouted->perm_idx >= program->modules.size() ||
        Common::Perf::FrameDumpActive().load(std::memory_order_relaxed)) {
        return false;
    }
    // What GetProgram does for a permutation found in the memo, with the scout's lookups.
    const auto stage_out_idx = static_cast<u32>(stage_out);
    runtime_infos[stage_out_idx] = scouted->GetRuntimeInfo();
    info.pgm_base = reinterpret_cast<VAddr>(code);
    info.user_data = pgm.user_data;
    info.flattened_ud_buf.assign(scouted->flat, scouted->flat + scouted->flat_size);
    info.AddBindings(binding);
    auto& perm = program->modules[scouted->perm_idx];
    if (auto& fetch = perm.spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }
    infos[stage_out_idx] = &info;
    modules[stage_out_idx] = perm.module;
    taken_scout_infos[stage_out_idx] = &info;
    taken_scout_stages[stage_out_idx] = scouted;
    graphics_key.stage_hashes[stage_out_idx] =
        HashCombine(scouted->params_hash, u64{scouted->perm_idx});
    Common::Perf::Add(Common::Perf::SpecCalls);
    Common::Perf::Add(Common::Perf::ScoutFast);
    return true;
}

bool PipelineCache::RefreshGraphicsStages() {
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;
    fetch_shader = nullptr;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        if (TakeScoutedStage(stage_in, stage_out, *pgm, binding)) {
            return true;
        }
        const auto params = GetStageParams(*pgm);
        std::tie(infos[stage_out_idx], modules[stage_out_idx], key.stage_hashes[stage_out_idx]) =
            GetProgram(stage_in, stage_out, params, binding);
        return true;
    };

    infos.fill(nullptr);
    modules.fill(nullptr);
    taken_scout_infos.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);

    const auto* fs_info = infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    const auto* vs_info = infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    taken_scout_infos.fill(nullptr);
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = GetStageParams(cs_pgm);
    std::tie(infos[0], modules[0], compute_key.value) =
        GetProgram(HwStage::Compute, SwStage::Compute, cs_params, binding);
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
    Common::Perf::ScopedTimer compile_timer{Common::Perf::ShaderCompiles,
                                            Common::Perf::ShaderCompileNs};
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

namespace {

/// Permutation memo key: the fields of the resource descriptors the specialization reads
/// (Shader::StageSpecialization), the fetch shader code, the runtime info and the start
/// bindings. Addresses and sizes are left out, so that a shader bound with other buffers and
/// textures of the same layout finds its permutation.
template <typename InfoT>
void SpecializationInputs(const InfoT& info, const Shader::Backend::Bindings& binding,
                          boost::container::small_vector<u32, 160>& words) {
    words.push_back(binding.unified);
    words.push_back(binding.buffer);
    if (info.has_fetch_shader) {
        // The fetch shader tells the vertex attributes (their descriptors are checked against
        // the permutation found, see VertexAttributesMatch): its code, up to the end of its page.
        const u32* code;
        std::memcpy(&code, &info.user_data[info.fetch_shader_sgpr_base], sizeof(code));
        const auto address = reinterpret_cast<uintptr_t>(code);
        const u64 room = (0x1000 - (address & 0xFFF)) / sizeof(u32);
        const u64 count = std::min<u64>(room, 48);
        words.push_back(static_cast<u32>(address));
        words.push_back(static_cast<u32>(address >> 32));
        words.insert(words.end(), code, code + count);
    }
    for (const auto& desc : info.buffers) {
        const auto sharp = desc.GetSharp(info);
        const auto* raw = reinterpret_cast<const u32*>(&sharp);
        words.push_back(static_cast<bool>(sharp));
        words.push_back(raw[1] & ~0xFFu); // Stride, swizzle enable (not the address).
        words.push_back(raw[3]);          // Formats, swizzles, index stride, element size.
    }
    for (const auto& desc : info.images) {
        const auto sharp = desc.GetSharp(info);
        const auto* raw = reinterpret_cast<const u32*>(&sharp);
        words.push_back(static_cast<bool>(sharp));
        words.push_back(raw[1] & ~0x3Fu); // Formats (not the address).
        words.push_back(raw[3]);          // Swizzles, levels, type.
        words.push_back(raw[4]);
        words.push_back(raw[5]);
    }
    for (const auto& desc : info.fmasks) {
        const auto sharp = desc.GetSharp(info);
        const auto* raw = reinterpret_cast<const u32*>(&sharp);
        words.push_back(static_cast<bool>(sharp));
        words.push_back(raw[2]); // Width and height.
    }
    for (const auto& desc : info.samplers) {
        const auto sharp = desc.GetSharp(info);
        words.push_back(static_cast<u32>(sharp.raw0 & ((1u << 15) | (1u << 20))));
    }
}

u64 SpecializationKey(std::span<const u32> words, const Shader::RuntimeInfo& runtime_info) {
    return HashCombine(XXH3_64bits(words.data(), words.size() * sizeof(u32)),
                       XXH3_64bits(&runtime_info, sizeof(runtime_info)));
}

template <typename InfoT>
u64 SpecializationKey(const InfoT& info, const Shader::RuntimeInfo& runtime_info,
                      const Shader::Backend::Bindings& binding) {
    boost::container::small_vector<u32, 160> words;
    SpecializationInputs(info, binding, words);
    return SpecializationKey({words.data(), words.size()}, runtime_info);
}

/// Whether the vertex attributes of the permutation (from its fetch shader) are specialized
/// as the descriptors bound now require (the key above leaves them out: their descriptors are
/// found by parsing the fetch shader).
template <typename InfoT>
bool VertexAttributesMatch(const InfoT& info, const Shader::RuntimeInfo& runtime_info,
                           const Shader::StageSpecialization& spec) {
    if (info.sw_stage != Shader::SwStage::Vertex) {
        return true;
    }
    const auto& attributes = spec.fetch_shader_data.attributes;
    if (spec.vs_attribs.size() != attributes.size()) {
        return false;
    }
    for (size_t i = 0; i < attributes.size(); ++i) {
        const auto& attribute = attributes[i];
        const auto sharp = attribute.GetSharp(info);
        Shader::VsAttribSpecialization expected{};
        if (sharp) {
            using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
            if (const auto step_rate = attribute.GetStepRate(); step_rate != InstanceIdType::None) {
                expected.divisor = step_rate == InstanceIdType::OverStepRate0
                                       ? runtime_info.sw.vs.step_rate_0
                                       : (step_rate == InstanceIdType::OverStepRate1
                                              ? runtime_info.sw.vs.step_rate_1
                                              : 1);
            }
            expected.num_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
            expected.dst_select = sharp.DstSelect();
        }
        if (!(spec.vs_attribs[i] == expected)) {
            return false;
        }
    }
    return true;
}

} // Anonymous namespace

PipelineCache::Result PipelineCache::GetProgram(HwStage hw_stage, SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    Common::Perf::Add(Common::Perf::SpecCalls);
    // Scout: the stage lookup precomputed for this draw, if its inputs are those of the draw
    // the command processor is at (the scout decides from its own copy of the registers).
    const AmdGpu::ScoutStage* scouted = nullptr;
    if (const auto* draw = liverpool->CurrentScoutDraw(); draw != nullptr) {
        for (u32 i = 0; i < draw->num_stages; ++i) {
            if (draw->stages[i].sw_stage == static_cast<u8>(sw_stage)) {
                scouted = &draw->stages[i];
                break;
            }
        }
        if (scouted == nullptr || !scouted->valid) {
            scouted = nullptr;
            Common::Perf::Add(Common::Perf::ScoutMisses);
        } else if (scouted->pgm_base != params.Base() || scouted->start != binding ||
                   scouted->ud_hash !=
                       XXH3_64bits(params.user_data.data(), params.user_data.size_bytes())) {
            scouted = nullptr;
            Common::Perf::Add(Common::Perf::ScoutStale);
        } else {
            Common::Perf::Add(Common::Perf::ScoutHits);
        }
    }
    Shader::RuntimeInfo runtime_info = [&] {
        if (scouted != nullptr) {
            runtime_infos[u32(sw_stage)] = scouted->GetRuntimeInfo();
            return runtime_infos[u32(sw_stage)];
        }
        return BuildRuntimeInfo(hw_stage, sw_stage);
    }();
    auto it_pgm = program_cache.find(params.hash);
    const bool new_program = it_pgm == program_cache.end();
    if (new_program) {
        auto new_program_ptr = std::make_unique<Program>(hw_stage, sw_stage, params);
        Program* const program = new_program_ptr.get();
        auto start = binding;
        const auto module = CompileModule(program->info, runtime_info, params.code, 0, binding);
        auto spec = Shader::StageSpecialization(program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderMeta(program->info, spec.fetch_shader_data, spec, perm_hash, 0);
        program->AddPermut(module, std::move(spec));
        {
            // The scout thread looks programs up while the command processor adds them: a
            // program is added complete (its info and first permutation written).
            std::unique_lock lock{program_cache_mutex};
            program_cache.emplace(params.hash, std::move(new_program_ptr));
        }
        if (auto& fetch = program->modules[0].spec.fetch_shader_data; !fetch.Empty()) {
            fetch_shader = &fetch;
        }
        return std::make_tuple(&program->info, module, perm_hash);
    }

    auto& program = it_pgm.value();
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    if (scouted != nullptr && scouted->flat_size == info.srt_info.flattened_bufsize_dw) {
        info.flattened_ud_buf.assign(scouted->flat, scouted->flat + scouted->flat_size);
    } else {
        scouted = nullptr;
        info.RefreshFlatBuf();
    }
    Common::Perf::Add(Common::Perf::SpecFlatDwords, info.flattened_ud_buf.size());
    if (Common::Perf::FrameDumpActive().load(std::memory_order_relaxed)) [[unlikely]] {
        // Frame x-ray: the GCN code of the shaders used (once per shader).
        if (xray_dumped.insert(params.hash).second) {
            using namespace Common::FS;
            const auto dir = GetUserPath(PathType::ShaderDir) / "xray";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const auto file = IOFile{dir / fmt::format("{}_{:#018x}.bin", hw_stage, params.hash),
                                     FileAccessMode::Create};
            file.WriteSpan(params.code);
        }
    }

    // Permutation memo: the specialization is a function of a few fields of the resource
    // descriptors (formats, strides, swizzles: not their addresses), of the fetch shader, the
    // runtime info and the start bindings. Bound again with the same ones, the stage reuses the
    // permutation found then instead of walking its resources again (and parsing its fetch
    // shader). Tessellation stages also read constants from memory: not memoized.
    const bool memoize = Common::Perf::GetTuning().spec_memo.load(std::memory_order_relaxed) &&
                         sw_stage != SwStage::TessellationControl &&
                         sw_stage != SwStage::TessellationEval;
    const bool exact_memo = memoize && scouted == nullptr &&
                            Common::Perf::GetTuning().runtime_lookup_memo.load(
                                std::memory_order_relaxed);
    boost::container::small_vector<u32, 160> selection_words;
    const auto runtime_bytes = std::as_bytes(std::span{&runtime_info, 1});
    const auto reuse_permutation = [&](u32 perm) {
        Common::Perf::Add(Common::Perf::SpecMemoHits);
        info.AddBindings(binding);
        auto& module = program->modules[perm];
        if (!module.spec.fetch_shader_data.Empty()) {
            fetch_shader = &module.spec.fetch_shader_data;
        }
        return std::make_tuple(&info, module.module, HashCombine(params.hash, u64{perm}));
    };
    if (exact_memo) {
        SpecializationInputs(info, binding, selection_words);
        const auto input_bytes = std::as_bytes(
            std::span<const u32>{selection_words.data(), selection_words.size()});
        const u32 perm = program->selection_perm;
        if (program->selection_inputs.Matches(input_bytes) &&
            program->selection_runtime.Matches(runtime_bytes) && perm < program->modules.size() &&
            program->modules[perm].module &&
            VertexAttributesMatch(info, runtime_info, program->modules[perm].spec)) {
            Common::Perf::Add(Common::Perf::SelectionMemoHits);
            return reuse_permutation(perm);
        }
        Common::Perf::Add(Common::Perf::SelectionMemoMisses);
    }
    const auto remember_selection = [&](u32 perm) {
        if (exact_memo) {
            program->selection_inputs.Capture(std::as_bytes(
                std::span<const u32>{selection_words.data(), selection_words.size()}));
            program->selection_runtime.Capture(runtime_bytes);
            program->selection_perm = perm;
        }
    };
    u64 memo_key = 0;
    if (memoize) {
        memo_key = scouted != nullptr ? scouted->memo_key
                   : exact_memo ? SpecializationKey(
                                      {selection_words.data(), selection_words.size()}, runtime_info)
                                : SpecializationKey(info, runtime_info, binding);
        for (const auto& entry : program->memo) {
            // The command processor is the only writer: plain (relaxed) reads.
            const u32 entry_perm = entry.perm_idx.load(std::memory_order_relaxed);
            if (entry.valid.load(std::memory_order_relaxed) &&
                entry.key.load(std::memory_order_relaxed) == memo_key &&
                entry_perm < program->modules.size() &&
                program->modules[entry_perm].module &&
                VertexAttributesMatch(info, runtime_info, program->modules[entry_perm].spec)) {
                remember_selection(entry_perm);
                return reuse_permutation(entry_perm);
            }
        }
        Common::Perf::Add(Common::Perf::SpecMemoMisses);
    }
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    const auto it = std::ranges::find_if(program->modules, [&](const Program::Module& permutation) {
        return permutation.module && permutation.spec == spec;
    });
    if (it == program->modules.end()) {
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        // The scout thread reads the permutations of the program (shared lock).
        std::unique_lock lock{program_cache_mutex};
        program->AddPermut(module, std::move(spec));
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    if (memoize) {
        program->WriteMemo(memo_key, static_cast<u32>(perm_idx));
    }
    remember_selection(static_cast<u32>(perm_idx));
    if (auto& fetch = program->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }
    return std::make_tuple(&program->info, module, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    // Pipelines being compiled use the modules and can be erased below.
    compiler->WaitIdle();
    // Retained binds hold pipeline pointers until replay; also finish GPU use of old handles.
    if (drain_pipeline_commands) {
        drain_pipeline_commands();
    } else {
        scheduler.Finish();
    }
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                last_graphics_pipeline = nullptr;
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
