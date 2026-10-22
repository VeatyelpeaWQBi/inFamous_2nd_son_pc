// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/hash.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_driver_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion =
    8u; // 7: legacy multiply-add (v_mac_legacy_f32),
        // 8: upstream ADDC/ALIGNBIT/CVT_PK_U8/thread id fixes
static constexpr u32 ShaderMetaVersion = 6u;
static constexpr u32 PipelineKeyVersion = 4u; // Includes auxiliary discard-fragment SPIR-V.

/// Compute shaders compiled with a different recompiler variant (see SetComputeShaderVariant)
/// are stored with another binary version, so changing the variant recompiles them once.
static u32 compute_shader_variant{};

static u32 BinaryVersionFor(const Shader::Info& info) {
    return info.hw_stage == Shader::HwStage::Compute
               ? ShaderBinaryVersion + compute_shader_variant * 100u
               : ShaderBinaryVersion;
}
} // namespace Serialization

namespace Vulkan {

void SetComputeShaderVariant(u32 variant) {
    Serialization::compute_shader_variant = variant;
}

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::BinaryVersionFor(info));

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version % 100u != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);
    if (perm_hash_ar != HashCombine(info.pgm_hash, u64{perm_idx}) ||
        (info.hw_stage == Shader::HwStage::Compute &&
         (info.sw_stage != Shader::SwStage::Compute || spec.start != Shader::Backend::Bindings{}))) {
        return false;
    }
    // Compiled with another recompiler variant: compiled again.
    return binary_version == Serialization::BinaryVersionFor(info);
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    if (ar.Remaining() < sizeof(ComputePipelineKey))
        return false;
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    u64 permutation_hash{};
    if (!LoadPipelineStage(meta_ar, 0, &permutation_hash) ||
        permutation_hash != compute_key.value) {
        return false;
    }
    return PrewarmComputePipeline(compute_key);
}

bool PipelineCache::PrewarmComputePipeline(const ComputePipelineKey& key) {
    if (!infos[0] || !modules[0] || infos[0]->hw_stage != Shader::HwStage::Compute ||
        infos[0]->sw_stage != Shader::SwStage::Compute)
        return false;
    const auto* spec = prewarmed_specs[0];
    if (!spec || spec->info != infos[0] || spec->images.size() != infos[0]->images.size() ||
        spec->buffers.size() != infos[0]->buffers.size() ||
        spec->buffers.size() + spec->images.size() > Shader::StageSpecialization::MaxStageResources)
        return false;
    if (compute_pipelines.contains(key))
        return true;
    std::vector<u32> image_bindings;
    image_bindings.reserve(spec->images.size());
    for (size_t index = 0; index < spec->images.size(); ++index) {
        const u32 count = spec->bitset[spec->buffers.size() + index]
                              ? spec->images[index].num_bindings
                              : 1; // Null descriptors still occupy one binding.
        if (count == 0 || count > 16 ||
            (infos[0]->images[index].mip_fallback_mode !=
                 Shader::MipStorageFallbackMode::DynamicIndex && count != 1))
            return false;
        image_bindings.push_back(count);
    }
    if (!CanPrewarm(64_KB))
        return false;
    const auto [it, is_new] = compute_pipelines.try_emplace(key);
    if (!is_new)
        return true;

    ComputePipeline::SerializationSupport sdata{}; // Compute keys have no extra layout state.
    try {
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       driver_cache->Handle(), key,
                                                       *infos[0], modules[0], sdata, true, true,
                                                       image_bindings);
    } catch (...) {
        compute_pipelines.erase(it); // Never leave a null cache entry after a failed restore.
        throw;
    }
    CompilePreloaded(it.value().get());

    infos.fill(nullptr);
    modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
    sdata.Write(fragment);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    if (!ar.IsEoS())
        sdata.Read(fragment); // Import version-3 graphics records too.
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar, bool legacy_record) {
    graphics_key.Deserialize(ar);
    if (graphics_key.num_color_attachments > AmdGpu::NUM_COLOR_BUFFERS)
        return false;
    if (legacy_record && profile.needs_clip_distance_emulation &&
        !graphics_key.stage_hashes[u32(Shader::SwStage::Fragment)])
        return false; // Version 3 did not save the required auxiliary fragment shader.

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
        if (u32(infos[stage_idx]->sw_stage) != u32(stage_idx))
            return false;
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    if (!is_new)
        return true;

    try {
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, graphics_key, driver_cache->Handle(), infos,
            runtime_infos, fetch_shader, modules, sdata, true, true);
    } catch (...) {
        graphics_pipelines.erase(it);
        throw;
    }
    CompilePreloaded(it.value().get());

    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage,
                                     u64* permutation_hash) {
    if (stage >= MaxShaderStages)
        return false;
    const auto select_stage = [&](Program& program, size_t index) {
        auto& module = program.modules[index];
        if (permutation_hash)
            *permutation_hash = HashCombine(program.info.pgm_hash, u64{index});
        infos[stage] = &program.info;
        prewarmed_specs[stage] = &module.spec;
        modules[stage] = module.module;
        runtime_infos[stage] = module.spec.runtime_info;
        if (!module.spec.fetch_shader_data.Empty())
            fetch_shader = &module.spec.fetch_shader_data;
        return true;
    };
    const u64 identity = XXH3_64bits(ar.CurrPtr(), ar.Remaining());
    if (const auto cached = prewarmed_stages.find(identity); cached != prewarmed_stages.end()) {
        std::shared_lock lock{program_cache_mutex};
        const auto program = program_cache.find(cached->second.first);
        if (program != program_cache.end() &&
            cached->second.second < program->second->modules.size()) {
            if (program->second->modules[cached->second.second].module)
                return select_stage(*program->second, cached->second.second);
        }
    }
    if (!prewarm_budget.CanContinue(AvailableHostMemory(), AvailableDeviceMemory(instance),
                                    prewarm_estimated_bytes))
        return false;
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, spec, perm_idx) ||
        perm_idx > 4095 || u32(program->info.sw_stage) >= Shader::MaxStageTypes) {
        return false;
    }
    if (permutation_hash)
        *permutation_hash = HashCombine(program->info.pgm_hash, u64{perm_idx});

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.size() < 5 || spv[0] != 0x07230203) {
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};

    // The scout thread looks programs and their permutations up meanwhile.
    std::unique_lock lock{program_cache_mutex};
    auto it_pgm = program_cache.find(program->info.pgm_hash);
    const bool new_program = it_pgm == program_cache.end();
    const size_t old_slots = new_program ? 0 : it_pgm.value()->modules.size();
    const u64 slot_bytes =
        perm_idx + 1 > old_slots ? (perm_idx + 1 - old_slots) * sizeof(Program::Module) : 0;
    if (!new_program) {
        const auto& cached = it_pgm.value()->modules;
        if (perm_idx < cached.size() && cached[perm_idx].module) {
            Serialization::Archive existing, incoming;
            cached[perm_idx].spec.Serialize(existing);
            spec.Serialize(incoming);
            if (existing.TakeOff() != incoming.TakeOff()) {
                LOG_WARNING(Render_Vulkan, "Conflicting cached shader slot {} of {:#x}", perm_idx,
                            program->info.pgm_hash);
                return false;
            }
            module = cached[perm_idx].module;
        }
    }
    if (!module) {
        if (!CanPrewarm(spv.size() * sizeof(u32) * 4 + ar.SizeBytes() + slot_bytes +
                        (new_program ? sizeof(Program) : 0)))
            return false;
        module = CompileSPV(spv, instance.GetDevice());
        if (new_program)
            it_pgm = program_cache.try_emplace(program->info.pgm_hash, std::move(program)).first;
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);
    it_pgm.value()->modules[perm_idx].spec.info = &it_pgm.value()->info;
    prewarmed_stages.emplace(identity, std::pair{it_pgm.value()->info.pgm_hash, perm_idx});

    return select_stage(*it_pgm.value(), perm_idx);
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled() || EmulatorSettings.IsPatchShaders()) {
        // External shader files can change without changing the original GCN hash.
        // Do not restore stale shader modules over a user-supplied patch.
        return;
    }

    Storage::DataBase::Instance().Open();
    if (!Storage::DataBase::Instance().IsOpened())
        return;
    Storage::DataBase::Instance().SetWriteBudget(prewarm_budget.host_bytes / 8);
    LOG_INFO(Render, "Prewarm budget: host {} MiB, host reserve {} MiB, GPU reserve {} MiB",
             prewarm_budget.host_bytes / 1_MB, prewarm_budget.host_reserve / 1_MB,
             prewarm_budget.device_reserve / 1_MB);

    const auto finish_preload = [this] {
        std::vector<u8> new_profile(sizeof(profile));
        std::memcpy(new_profile.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(new_profile));
        Storage::DataBase::Instance().FinishPreload();
    };

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    Shader::Profile cached_profile{};
    const bool valid_size = profile_data.size() == sizeof(cached_profile);
    if (valid_size)
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    if (!valid_size || cached_profile != profile) {
        LOG_WARNING(Render, "Missing, invalid or incompatible pipeline cache profile; rebuilding");
        Storage::DataBase::Instance().Reset();
        finish_preload();
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    u32 num_stages{}, num_rejected_stages{};
    const auto clear_stages = [this] {
        infos.fill(nullptr);
        modules.fill(nullptr);
        runtime_infos.fill(Shader::RuntimeInfo{});
        prewarmed_specs.fill(nullptr);
        fetch_shader = nullptr;
    };
    // Restore every cached specialization, even if the previous run ended before
    // its pipeline record was published. No live guest pointers are read here.
    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::ShaderMeta, [&](std::vector<u8>&& data) {
            try {
                Serialization::Archive ar{std::move(data)};
                u64 permutation_hash{};
                if (LoadPipelineStage(ar, 0, &permutation_hash)) {
                    ++num_stages;
                    // A compute pipeline is fully described by this validated specialization.
                    // A session may have saved its shader but ended before saving a .key file.
                    // Build it now instead of waiting for the first in-game dispatch.
                    if (infos[0]->hw_stage == Shader::HwStage::Compute)
                        PrewarmComputePipeline(ComputePipelineKey{permutation_hash});
                } else {
                    ++num_rejected_stages;
                }
            } catch (const std::exception& error) {
                ++num_rejected_stages;
                LOG_WARNING(Render, "Ignoring invalid cached shader: {}", error.what());
            }
            clear_stages();
        });

    const size_t compute_from_stages = compute_pipelines.size();

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;
            clear_stages();
            try {
                Serialization::Archive ar{std::move(data)};
                Serialization::Reader pldata{ar};

                u32 version{};
                pldata.Read(version);
                if (version != 3 && version != Serialization::PipelineKeyVersion) {
                    return;
                }
                u32 is_compute{};
                pldata.Read(is_compute);
                if (is_compute > 1)
                    return;

                bool result{};
                if (is_compute) {
                    result = LoadComputePipeline(ar);
                } else if (CanPrewarm(64_KB)) {
                    result = LoadGraphicsPipeline(ar, version == 3);
                }

                if (result) {
                    ++num_pipelines;
                }
            } catch (const std::exception& error) {
                LOG_WARNING(Render, "Ignoring invalid cached pipeline: {}", error.what());
            }
        });

    FinishPreloaded(); // Game execution starts only after every queued warmup is ready.
    prewarmed_stages.clear();
    clear_stages();
    LOG_INFO(Render, "Preloaded {} shader specializations; {} rejected or deferred", num_stages,
             num_rejected_stages);
    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    LOG_INFO(Render,
             "Prewarmed {} compute pipelines from complete shader specializations; "
             "{} total unique pipelines ready",
             compute_from_stages,
             compute_pipelines.size() + graphics_pipelines.size());
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} cached pipelines rejected or deferred by the memory budget",
                    num_total_pipelines - num_pipelines);
    }

    finish_preload();
}

void PipelineCache::Sync() {
    // quick_exit does not run renderer destructors. Stop new driver-cache leases,
    // snapshot completed compilations, then drain owned shader/pipeline records.
    driver_cache->Stop();
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        if (walker_func_size > ar.Remaining()) {
            throw std::runtime_error("Truncated cached SRT walker");
        }
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    } else {
        walker_func = nullptr; // Never restore a process address without its code snapshot.
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (!fetch_shader_data.Empty()) {
        spec.Write(sizeof(fetch_shader_data));
        fetch_shader_data.Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
