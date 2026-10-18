// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "common/perf_stats.h"
#include "shader_recompiler/ir/passes/ir_passes.h"

namespace Shader::Optimization {
namespace {
bool AddressArithmetic(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::IAdd32: case IR::Opcode::ISub32: case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32: case IR::Opcode::ShiftRightLogical32:
    case IR::Opcode::BitwiseAnd32: case IR::Opcode::BitwiseOr32: case IR::Opcode::BitwiseXor32:
        return true;
    default: return false;
    }
}
bool StableExpression(const IR::Inst& inst) {
    return AddressArithmetic(inst.GetOpcode()) || inst.GetOpcode() == IR::Opcode::GetAttribute ||
           inst.GetOpcode() == IR::Opcode::GetAttributeU32;
}
bool EqualExpression(const IR::Inst& a, const IR::Inst& b) {
    if (a.GetOpcode() != b.GetOpcode() || a.Flags<u64>() != b.Flags<u64>() ||
        a.NumArgs() != b.NumArgs()) return false;
    for (size_t i = 0; i < a.NumArgs(); ++i) if (a.Arg(i) != b.Arg(i)) return false;
    return true;
}
}

// Exact expressions only: no floating reassociation, no memory-load CSE, no derivative,
// image, subgroup, atomic or barrier changes. Single block scope establishes dominance.
void IfssAddressSchedulePass(IR::Program& program, u32 mask) {
    const bool lighting = program.info.sw_stage == SwStage::Compute &&
                          program.info.pgm_hash == 0xce1675e0ULL && (mask & 1);
    const bool particle = program.info.sw_stage == SwStage::Fragment &&
                          program.info.pgm_hash == 0x24dad4beULL && (mask & 2);
    if (!lighting && !particle) return;
    u64 eliminated = 0, moved = 0, shortened = 0;
    for (IR::Block* block : program.blocks) {
        std::vector<IR::Inst*> expressions;
        for (auto it = block->begin(); it != block->end();) {
            auto& inst = *it++;
            if (!StableExpression(inst) || inst.MayHaveSideEffects()) continue;
            const auto old = std::find_if(expressions.begin(), expressions.end(), [&](auto* prior) {
                return EqualExpression(*prior, inst);
            });
            if (old != expressions.end()) {
                inst.ReplaceUsesWithAndRemove(IR::Value{*old});
                block->Instructions().erase(block->Instructions().iterator_to(inst));
                ++eliminated;
            } else {
                // Bounded compile work; missing a match only loses an optimization.
                if (expressions.size() == 128) expressions.erase(expressions.begin());
                expressions.push_back(&inst);
            }
        }
        if (!lighting) continue;
        std::vector<IR::Inst*> order;
        std::unordered_map<IR::Inst*, size_t> positions;
        for (auto& inst : block->Instructions()) {
            positions.emplace(&inst, order.size());
            order.push_back(&inst);
        }
        for (auto it = order.rbegin(); it != order.rend() && moved < 128; ++it) {
            auto* inst = *it;
            if (!AddressArithmetic(inst->GetOpcode()) || inst->UseCount() != 1 ||
                inst->MayHaveSideEffects()) continue;
            auto* consumer = inst->Uses().front().user;
            const auto found = positions.find(consumer);
            if (found == positions.end() || consumer->GetOpcode() == IR::Opcode::Phi ||
                found->second < positions.at(inst) + 8) continue;
            bool extends_operand = false;
            for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
                const auto* producer = inst->Arg(arg).TryInst();
                if (!producer) continue;
                const bool already_live = std::ranges::any_of(producer->Uses(), [&](const auto& use) {
                    const auto user = positions.find(use.user);
                    return use.user != inst && user != positions.end() && user->second >= found->second;
                });
                if (!already_live) { extends_operand = true; break; }
            }
            if (extends_operand) continue;
            auto& list = block->Instructions();
            list.splice(list.iterator_to(*consumer), list, list.iterator_to(*inst));
            shortened += found->second - positions.at(inst) - 1;
            ++moved;
        }
    }
    Common::Perf::Add(lighting ? Common::Perf::V3LightingCse : Common::Perf::V3ParticleCse, eliminated);
    Common::Perf::Add(lighting ? Common::Perf::V3LightingMoves : Common::Perf::V3ParticleMoves, moved);
    LOG_INFO(Render_Recompiler, "V3 shader {:#x}: exact expressions removed {}, address values "
                               "scheduled {}, static live-gap reduction {} instructions",
             program.info.pgm_hash, eliminated, moved, shortened);
}
} // namespace Shader::Optimization
