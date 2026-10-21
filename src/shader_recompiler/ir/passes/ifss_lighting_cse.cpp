// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/ir/passes/ifss_lighting_cse.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>
#include "common/hash.h"
#include "shader_recompiler/ifss_depth_reduction.h"
#include "shader_recompiler/ir/dominance.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {
namespace {
bool IsExactPure(IR::Opcode opcode) {
    // Deliberately excludes loads (including constant buffers), phi, derivatives,
    // subgroup/lane operations, conditions, barriers and every side effect.
    switch (opcode) {
    case IR::Opcode::IAdd32:
    case IR::Opcode::ISub32:
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32:
    case IR::Opcode::ShiftRightLogical32:
    case IR::Opcode::BitwiseAnd32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
    case IR::Opcode::BitCastU32F32:
    case IR::Opcode::BitCastF32U32:
    case IR::Opcode::FPAbs32:
    case IR::Opcode::FPAdd32:
    case IR::Opcode::FPSub32:
    case IR::Opcode::FPMul32:
    case IR::Opcode::FPFma32:
    case IR::Opcode::FPNeg32:
    case IR::Opcode::CompositeConstructU32x2:
    case IR::Opcode::CompositeConstructU32x3:
    case IR::Opcode::CompositeConstructU32x4:
    case IR::Opcode::CompositeExtractU32x2:
    case IR::Opcode::CompositeExtractU32x3:
    case IR::Opcode::CompositeExtractU32x4:
    case IR::Opcode::CompositeConstructF32x2:
    case IR::Opcode::CompositeConstructF32x3:
    case IR::Opcode::CompositeConstructF32x4:
    case IR::Opcode::CompositeExtractF32x2:
    case IR::Opcode::CompositeExtractF32x3:
    case IR::Opcode::CompositeExtractF32x4:
        return true;
    default:
        return false;
    }
}

struct Expression {
    IR::Opcode opcode;
    u64 flags;
    std::array<IR::Value, 6> arguments{};
    bool operator==(const Expression&) const = default;
};
struct ExpressionHash {
    std::size_t operator()(const Expression& expression) const {
        u64 hash = HashCombine(static_cast<u64>(expression.opcode), expression.flags);
        for (const auto& argument : expression.arguments) {
            hash = HashCombine(hash, argument.Hash());
        }
        return static_cast<std::size_t>(hash);
    }
};
} // namespace

LightingCseResult EliminateIfssLightingExpressions(IR::Program& program) {
    LightingCseResult result;
    if (program.info.hw_stage != HwStage::Compute || program.info.sw_stage != SwStage::Compute ||
        program.info.pgm_hash != IfssLightingHash || program.post_order_blocks.empty()) {
        return result;
    }
    // Rebuild dominance on the final structured CFG, not the pre-structurization graph.
    for (auto* block : program.post_order_blocks) {
        block->immediate_dominator = nullptr;
    }
    IR::ComputeDominators(program.post_order_blocks);
    std::unordered_map<Expression, std::vector<IR::Inst*>, ExpressionHash> values;
    values.reserve(1024);
    constexpr std::size_t MaxExpressions = 4096, MaxAlternatives = 8;
    // Dominating definitions precede uses in reverse postorder. A loop's phi is
    // never folded: different iterations or different SSA inputs remain distinct.
    for (auto b = program.post_order_blocks.rbegin(); b != program.post_order_blocks.rend(); ++b) {
        auto* block = *b;
        for (auto it = block->begin(); it != block->end();) {
            auto& inst = *it++;
            if (!inst.HasUses() || !IsExactPure(inst.GetOpcode()) || inst.MayHaveSideEffects()) {
                continue;
            }
            Expression key{inst.GetOpcode(), inst.Flags<u64>()};
            for (std::size_t arg = 0; arg < inst.NumArgs(); ++arg) {
                key.arguments[arg] = inst.Arg(arg);
            }
            const auto found = values.find(key);
            if (found != values.end()) {
                const auto prior = std::ranges::find_if(found->second, [&](IR::Inst* definition) {
                    return IR::Dominates(definition->GetParent(), block);
                });
                if (prior != found->second.end()) {
                    result.cross_block += (*prior)->GetParent() != block;
                    result.floating += inst.Type() == IR::Type::F32;
                    // Value equality compares FP immediate bit patterns, including
                    // signed zero/NaN payloads. Opcode, operand order and all flags match.
                    inst.ReplaceUsesWithAndRemove(IR::Value{*prior});
                    block->Instructions().erase(block->Instructions().iterator_to(inst));
                    ++result.removed;
                    continue;
                }
                if (found->second.size() < MaxAlternatives) {
                    found->second.push_back(&inst);
                } else {
                    result.budget_reached = true;
                }
            } else if (values.size() < MaxExpressions) {
                values.emplace(key, std::vector<IR::Inst*>{&inst});
            } else {
                result.budget_reached = true;
            }
        }
    }
    return result;
}
} // namespace Shader::Optimization
