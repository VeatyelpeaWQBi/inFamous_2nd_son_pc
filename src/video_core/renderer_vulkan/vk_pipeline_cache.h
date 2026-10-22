// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <shared_mutex>
#include <unordered_map>
#include <variant>
#include <tsl/robin_map.h>
#include <unordered_set>
#include <xxhash.h>
#include "video_core/amdgpu/scout.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/runtime_memo.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_prewarm_budget.h"
#include "vulkan/vulkan.hpp"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;
class PipelineCompiler;
class DriverPipelineCache;

/// Key of the texture binding memo (Rasterizer::BindTextures, Tuning::tex_memo) for binding
/// `index` of an image resource with this T#: what the image description depends on, without
/// padding bytes. Also computed by the scout thread for the stages it precomputes.
inline u64 TexMemoKey(const AmdGpu::Image& tsharp, u32 index,
                      const Shader::ImageResource& image_desc) noexcept {
    struct KeyData {
        AmdGpu::Image tsharp;
        u32 index;
        u32 flags;
    } key_data{
        .tsharp = tsharp,
        .index = index,
        .flags = (image_desc.is_depth ? 1u : 0u) | (image_desc.is_array ? 2u : 0u) |
                 (image_desc.is_written ? 4u : 0u) |
                 (static_cast<u32>(image_desc.mip_fallback_mode) << 8) |
                 (static_cast<u32>(image_desc.constant_mip_index) << 16),
    };
    static_assert(sizeof(KeyData) == sizeof(AmdGpu::Image) + 8);
    return XXH3_64bits(&key_data, sizeof(key_data)) | 1;
}

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};

    /// Permutation memo (Tuning::spec_memo): hash of the resource descriptors, runtime info and
    /// start bindings of recent bindings of this program, and the permutation they selected.
    /// Written by the command processor only, also read by the scout thread: each entry is a
    /// sequence lock (odd while being written).
    struct MemoEntry {
        std::atomic<u32> seq{};
        std::atomic<u32> perm_idx{};
        std::atomic<u64> key{};
        std::atomic<bool> valid{};
    };
    static constexpr size_t NumMemoEntries = 16;
    std::array<MemoEntry, NumMemoEntries> memo{};
    u32 memo_next{}; ///< Command processor only.

    /// Exact inputs and last selection; never accessed by the scout/compiler threads.
    /// Vertex attribute descriptors are additionally checked at every use.
    VideoCore::RuntimeSnapshot<4096> selection_inputs;
    VideoCore::RuntimeSnapshot<sizeof(Shader::RuntimeInfo)> selection_runtime;
    u32 selection_perm{};

    /// Command processor: remembers the permutation of a key (oldest entry replaced).
    void WriteMemo(u64 key, u32 perm_idx) noexcept {
        auto& entry = memo[memo_next];
        const u32 seq = entry.seq.load(std::memory_order_relaxed);
        entry.seq.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        entry.key.store(key, std::memory_order_relaxed);
        entry.perm_idx.store(perm_idx, std::memory_order_relaxed);
        entry.valid.store(true, std::memory_order_relaxed);
        entry.seq.store(seq + 2, std::memory_order_release);
        memo_next = (memo_next + 1) % NumMemoEntries;
    }

    /// Scout thread: the permutation remembered for the key, read consistently, most recent
    /// entries first.
    bool ReadMemo(u64 key, u32& perm_idx) const noexcept {
        for (size_t n = 0; n < NumMemoEntries; ++n) {
            // memo_next is the command processor's: a stale value only changes the order.
            const auto& entry =
                memo[(NumMemoEntries + memo_next - 1 - n) % NumMemoEntries];
            const u32 seq = entry.seq.load(std::memory_order_acquire);
            if ((seq & 1) != 0) {
                continue;
            }
            const bool valid = entry.valid.load(std::memory_order_relaxed);
            const u64 entry_key = entry.key.load(std::memory_order_relaxed);
            const u32 entry_perm = entry.perm_idx.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (entry.seq.load(std::memory_order_relaxed) != seq) {
                continue;
            }
            if (valid && entry_key == key) {
                perm_idx = entry_perm;
                return true;
            }
        }
        return false;
    }

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

class PipelineCache : public AmdGpu::ScoutClient {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache() override;

    /// Scout thread: the shader stage lookups of a draw, from the registers at that draw.
    void ScoutDraw(const AmdGpu::Regs& regs, AmdGpu::ScoutDraw& draw,
                   AmdGpu::ScoutArena& arena) override;

    void WarmUp();
    void Sync();

    /// Drain every scheduler that may own retained pipeline binds before invalidating them.
    void SetPipelineDrainCallback(std::function<void()>&& drain) {
        drain_pipeline_commands = std::move(drain);
    }

    /// Map/unmap can reuse a shader address with a shorter allocation. Forget old scan ranges
    /// before examining them, independently of uploads and writes (called by memory threads).
    void NotifyMemoryMapChanged() noexcept {
        shader_mapping_generation.fetch_add(1, std::memory_order_acq_rel);
    }

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool PrewarmComputePipeline(const ComputePipelineKey& key);
    bool LoadGraphicsPipeline(Serialization::Archive& ar, bool legacy_record = false);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage,
                           u64* permutation_hash = nullptr);

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {});

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule, u64>;
    Result GetProgram(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

    /// The scout stage taken as is (scout_fast) for this stage info in the current draw, null
    /// when the stage was looked up by the command processor: the scout's precomputed resource
    /// lookups (Tuning::bind_prefetch) apply to the info's bindings.
    const AmdGpu::ScoutStage* TakenScoutStage(const Shader::Info& info) const noexcept {
        for (size_t i = 0; i < taken_scout_infos.size(); ++i) {
            if (taken_scout_infos[i] == &info) {
                return taken_scout_stages[i];
            }
        }
        return nullptr;
    }

private:
    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();

    /// AmdGpu::GetParams of a stage program, through the params memo (shader_params_memo,
    /// command processor thread only).
    template <typename ProgramRegs>
    Shader::ShaderParams GetStageParams(const ProgramRegs& pgm);

    /// Tuning::scout_fast: the stage lookup the scout thread resolved for this draw, taken as
    /// is when its inputs are the command processor's (fills infos, modules, the key's stage
    /// hash and the bindings). False: GetProgram as usual.
    template <typename ProgramRegs>
    bool TakeScoutedStage(Shader::HwStage stage_in, Shader::SwStage stage_out,
                          const ProgramRegs& pgm, Shader::Backend::Bindings& binding);

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::HwStage stage, Shader::SwStage l_stage);
    /// The part of the runtime info that only depends on the registers (any thread). Returns
    /// false for the stages that read more (tessellation).
    bool BuildRuntimeInfoFrom(Shader::RuntimeInfo& info, const AmdGpu::Regs& regs,
                              Shader::HwStage stage, Shader::SwStage l_stage,
                              const std::array<Shader::PsColorBuffer, Shader::MaxColorBuffers>&
                                  color_buffers,
                              const DrawIndirectParams& indirect_params) const;
    /// The color targets of the graphics key, from the registers.
    static void FillColorBuffers(
        const AmdGpu::Regs& regs,
        std::array<Shader::PsColorBuffer, Shader::MaxColorBuffers>& color_buffers);
    /// The program of a shader, null when not compiled yet (any thread).
    Program* FindProgram(u64 hash);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

    /// Hands a pipeline created with a deferred compilation to the worker threads, then waits
    /// for it at most async_pipeline_wait_ms. Callers skip their work while it is not ready.
    void CompileAsync(Pipeline* pipeline);
    void CompilePreloaded(Pipeline* pipeline);
    void FinishPreloaded();
    bool CanPrewarm(u64 bytes);

private:
    const Instance& instance;
    // Proof follows the exact bytes used to create this module, never a later disk file.
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    std::unique_ptr<DriverPipelineCache> driver_cache;
    std::function<void()> drain_pipeline_commands;
    PrewarmBudget prewarm_budget;
    u64 prewarm_estimated_bytes{};
    // Startup-only identity memo: pipeline records often reference the same metadata.
    // Avoid decoding/registering the same native SRT walker again for every reference.
    std::unordered_map<u64, std::pair<u64, size_t>> prewarmed_stages;
    std::array<const Shader::StageSpecialization*, MaxShaderStages> prewarmed_specs{};
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    DrawIndirectParams draw_indirect_params{};
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    /// Held exclusively while the command processor adds a program, shared by the scout
    /// thread while it looks one up (the command processor reads without it: it is the only
    /// writer).
    std::shared_mutex program_cache_mutex;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    /// Epoch-local shader params, optionally revalidated against owned metadata/scan inputs
    /// across epochs. A scanned header snapshots the entire searched prefix, so an earlier
    /// header inserted at the same address cannot be missed. Direct headers need only the
    /// first two words and their metadata. At most 256 * (64 KiB + two headers) of snapshots.
    struct ParamsMemo {
        VAddr address{};
        u64 epoch{};
        u64 host_write_gen{};
        u64 mapping_generation{};
        u64 hash{};
        u32 num_dwords{};
        u32 header_offset{};
        bool direct_header{};
        VideoCore::RuntimeSnapshot<64 * 1024 + sizeof(AmdGpu::BinaryInfo)> scan_input;
        VideoCore::RuntimeSnapshot<sizeof(AmdGpu::BinaryInfo)> header_input;
    };
    static constexpr u32 NumParamsMemoBits = 6;
    static constexpr u32 ParamsMemoWays = 4;
    std::array<std::array<ParamsMemo, ParamsMemoWays>, 1U << NumParamsMemoBits> params_memos{};
    std::array<u8, 1U << NumParamsMemoBits> params_memo_next{};
    std::atomic<u64> shader_mapping_generation{1};
    /// Pipeline key memo (pipeline_key_memo): the key and pipeline of the last draw.
    GraphicsPipelineKey last_graphics_key{};
    GraphicsPipeline* last_graphics_pipeline{};
    std::unordered_set<u64> xray_dumped; ///< Shaders whose code the frame x-ray wrote.
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    /// The stages of the current draw taken from the scout (TakeScoutedStage), by stage.
    std::array<const Shader::Info*, MaxShaderStages> taken_scout_infos{};
    std::array<const AmdGpu::ScoutStage*, MaxShaderStages> taken_scout_stages{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    Shader::Gcn::FetchShaderData* fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;

    std::unique_ptr<PipelineCompiler> compiler;
};

} // namespace Vulkan
