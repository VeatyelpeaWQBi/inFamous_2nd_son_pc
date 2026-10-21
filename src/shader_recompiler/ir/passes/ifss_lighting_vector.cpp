// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/ir/passes/ifss_lighting_vector.h"
#include <array>
#include <vector>
#include "shader_recompiler/ifss_depth_reduction.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {
namespace {
using namespace IR;
struct Index { Value base; u32 offset{}; };
Index SplitIndex(Value value, unsigned depth = 0) {
    if (value.IsImmediate()) return {{}, value.U32()};
    if (depth < 12) {
        const auto* inst = value.TryInst();
        if (inst && inst->GetOpcode() == Opcode::IAdd32) {
            for (unsigned i = 0; i < 2; ++i) {
                if (inst->Arg(i).IsImmediate()) {
                    auto result = SplitIndex(inst->Arg(1 - i), depth + 1);
                    result.offset += inst->Arg(i).U32();
                    return result;
                }
            }
        }
    }
    return {value, 0};
}
bool CanPass(const Inst& inst, u64 flags, Value handle) {
    switch (inst.GetOpcode()) {
    case Opcode::ReadConstBuffer:
        return inst.Flags<u64>() == flags && inst.Arg(0) == handle;
    case Opcode::IAdd32:
    case Opcode::ISub32:
    case Opcode::ShiftRightLogical32:
    case Opcode::ShiftLeftLogical32:
    case Opcode::BitCastU32F32:
    case Opcode::BitCastF32U32:
        return !inst.MayHaveSideEffects();
    default: return false;
    }
}
}

std::size_t PackIfssLightingReads(IR::Program& program) {
    using namespace IR;
    if (program.info.pgm_hash != IfssLightingHash || program.info.hw_stage != HwStage::Compute ||
        program.info.sw_stage != SwStage::Compute) return 0;
    std::size_t packed = 0;
    for (auto* block : program.post_order_blocks) {
        // Snapshot pointers before insertion; retain instruction order inside each group.
        std::vector<Inst*> original;
        for (auto& inst : *block) original.push_back(&inst);
        for (std::size_t first = 0; first < original.size(); ++first) {
            auto* load = original[first];
            if (load->GetOpcode() != Opcode::ReadConstBuffer || !load->HasUses() ||
                !load->Arg(0).IsImmediate()) continue;
            const auto flags = load->Flags<BufferInstInfo>();
            const auto handle = load->Arg(0);
            const u32 binding = handle.U32();
            if (!flags.pc || flags.sharp_source || flags.flatbuf_off_dw || flags.system_coherent ||
                flags.globally_coherent || binding >= program.info.buffers.size()) continue;
            auto& resource = program.info.buffers[binding];
            if (resource.IsSpecial() || resource.is_written || resource.is_formatted) continue;
            const auto index = SplitIndex(load->Arg(1));
            if (index.offset > ~u32{0} - 3) continue;
            std::array<Inst*, 4> members{load};
            unsigned count = 1;
            for (std::size_t at = first + 1; at < original.size() && count < 4; ++at) {
                auto* next = original[at];
                if (!CanPass(*next, flags.raw, handle)) break;
                if (next->GetOpcode() != Opcode::ReadConstBuffer) continue;
                const auto next_index = SplitIndex(next->Arg(1));
                if (!next->HasUses() || next_index.base != index.base ||
                    next_index.offset != index.offset + count) break;
                members[count++] = next;
            }
            if (count != 4) continue;
            auto insertion = block->Instructions().iterator_to(*load);
            auto vector = block->PrependNewInst(insertion, Opcode::IfssReadConstBufferU32x4,
                                                {handle, load->Arg(1)});
            vector->SetFlags(flags.raw);
            for (u32 i = 0; i < 4; ++i) {
                auto extract = block->PrependNewInst(insertion, Opcode::CompositeExtractU32x4,
                                                     {Value{&*vector}, Value{i}});
                members[i]->ReplaceUsesWithAndRemove(Value{&*extract});
            }
            resource.used_types |= Type::U32 | Type::U32x4;
            ++packed;
        }
    }
    return packed;
}
}
