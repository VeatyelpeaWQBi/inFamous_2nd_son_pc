// SPDX-License-Identifier: GPL-2.0-or-later
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <fmt/format.h>
#include "shader_recompiler/ifss_depth_reduction.h"
#include "shader_recompiler/ir/passes/ifss_lighting_cse.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/ir/program.h"

// Only assertion reporting is substituted: tests execute the real IR and pass,
// without linking the emulator, a Vulkan device, filesystem or game startup.
void assert_fail_impl() { std::abort(); }
[[noreturn]] void unreachable_impl() { std::abort(); }
namespace Common::Log {
void VLog(Class, Level, const char*, int, const char*, fmt::string_view format,
          fmt::format_args args) {
    std::fputs(fmt::vformat(format, args).c_str(), stderr);
}
} // namespace Common::Log

namespace {
using namespace Shader;
using namespace Shader::Optimization;
void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
struct Fixture {
    Common::ObjectPool<IR::Inst> pool;
    std::vector<std::unique_ptr<IR::Block>> blocks;
    Info info{};
    IR::Program program{info};
    Fixture() {
        info.hw_stage = HwStage::Compute;
        info.sw_stage = SwStage::Compute;
        info.pgm_hash = IfssLightingHash;
    }
    IR::Block* Block() {
        auto* block = blocks.emplace_back(std::make_unique<IR::Block>(pool)).get();
        program.blocks.push_back(block);
        return block;
    }
    IR::Inst* Add(IR::Block* b, IR::Opcode op, std::initializer_list<IR::Value> args = {},
                  u32 flags = 0) {
        return &*b->PrependNewInst(b->end(), op, args, flags);
    }
    IR::Inst* Use(IR::Block* b, IR::Inst* value) {
        return Add(b, IR::Opcode::FPOrdNotEqual32, {IR::Value{value}, IR::Value{0.0f}});
    }
    LightingCseResult Run() {
        program.post_order_blocks = IR::PostOrder(program.blocks.front());
        return EliminateIfssLightingExpressions(program);
    }
};
void DominanceAndOperandChains() {
    Fixture f;
    auto* entry = f.Block(); auto* child = f.Block();
    entry->AddBranch(child);
    auto* input = f.Add(entry, IR::Opcode::UndefF32);
    auto* original = f.Add(entry, IR::Opcode::FPMul32, {IR::Value{input}, IR::Value{3.0f}});
    f.Use(entry, original);
    auto* duplicate = f.Add(child, IR::Opcode::FPMul32, {IR::Value{input}, IR::Value{3.0f}});
    auto* next = f.Add(child, IR::Opcode::FPAdd32, {IR::Value{duplicate}, IR::Value{1.0f}});
    f.Use(child, next);
    auto* next_duplicate = f.Add(child, IR::Opcode::FPAdd32, {IR::Value{original}, IR::Value{1.0f}});
    auto* consumer = f.Use(child, next_duplicate);
    const auto result = f.Run();
    Require(result.removed == 2 && result.cross_block == 1 && result.floating == 2,
            "reuse dominates and updated operands enable a chained exact match");
    Require(consumer->Arg(0).TryInst() == next && next->Arg(0).TryInst() == original,
            "real IR use lists are redirected to the surviving definitions");
    Require(f.Run().removed == 0, "a second pass is stable");
}
void SiblingsAndLoopIterations() {
    Fixture f;
    auto* entry = f.Block(); auto* left = f.Block(); auto* right = f.Block();
    auto* join = f.Block(); auto* body = f.Block(); auto* exit = f.Block();
    entry->AddBranch(left); entry->AddBranch(right); left->AddBranch(join);
    right->AddBranch(join); join->AddBranch(body); join->AddBranch(exit); body->AddBranch(join);
    auto* input = f.Add(entry, IR::Opcode::UndefF32);
    auto* a = f.Add(left, IR::Opcode::FPAdd32, {IR::Value{input}, IR::Value{2.0f}});
    auto* b = f.Add(right, IR::Opcode::FPAdd32, {IR::Value{input}, IR::Value{2.0f}});
    auto* phi = f.Add(join, IR::Opcode::Phi);
    phi->SetFlags(IR::Type::F32);
    phi->AddPhiOperand(left, IR::Value{a}); phi->AddPhiOperand(right, IR::Value{b});
    auto* step = f.Add(body, IR::Opcode::FPAdd32, {IR::Value{phi}, IR::Value{2.0f}});
    phi->AddPhiOperand(body, IR::Value{step});
    auto* c = f.Add(exit, IR::Opcode::FPAdd32, {IR::Value{input}, IR::Value{2.0f}});
    f.Use(exit, c); f.Use(body, step);
    Require(f.Run().removed == 0, "neither a sibling nor a previous loop iteration dominates");
    Require(phi->GetOpcode() == IR::Opcode::Phi && phi->Arg(2).TryInst() == step,
            "loop-carried values and incoming edges remain intact");
}
void FloatingBitsAndFlags() {
    Fixture f; auto* b = f.Block();
    for (u32 bits : {0u, 0x80000000u, 0x7fc00001u, 0x7fc00002u}) {
        auto value = IR::Value{std::bit_cast<float>(bits)};
        f.Use(b, f.Add(b, IR::Opcode::FPNeg32, {value}));
    }
    auto* input = f.Add(b, IR::Opcode::UndefF32);
    auto* a = f.Add(b, IR::Opcode::FPMul32, {IR::Value{input}, IR::Value{2.0f}});
    auto* flags = f.Add(b, IR::Opcode::FPMul32, {IR::Value{input}, IR::Value{2.0f}}, 1);
    auto* order = f.Add(b, IR::Opcode::FPMul32, {IR::Value{2.0f}, IR::Value{input}});
    f.Use(b, a); f.Use(b, flags); f.Use(b, order);
    Require(f.Run().removed == 0, "signed zero, NaN payloads, flags and operand order differ");
}
void MemoryAndLaneOperations() {
    Fixture f; auto* b = f.Block();
    auto* x = f.Add(b, IR::Opcode::ReadConstBuffer, {IR::Value{0u}, IR::Value{4u}});
    auto* y = f.Add(b, IR::Opcode::ReadConstBuffer, {IR::Value{0u}, IR::Value{4u}});
    f.Add(b, IR::Opcode::WriteSharedU32, {IR::Value{0u}, IR::Value{x}});
    f.Add(b, IR::Opcode::Barrier);
    f.Add(b, IR::Opcode::WriteSharedU32, {IR::Value{4u}, IR::Value{y}});
    auto* a = f.Add(b, IR::Opcode::BitCastF32U32, {IR::Value{x}});
    auto* c = f.Add(b, IR::Opcode::BitCastF32U32, {IR::Value{y}});
    f.Use(b, a); f.Use(b, c);
    const auto before = b->size();
    Require(f.Run().removed == 0 && b->size() == before,
            "identical memory addresses do not establish identical SSA values");
    Require(x->HasUses() && y->HasUses(), "memory reads, stores and barriers survive");
}
void ScopeAndCacheIsolation() {
    Require(IfssShaderVariant(8, IfssLightingHash, true) == 8 &&
            IfssShaderVariant(8, 0x24dad4be, false) == 0 &&
            IfssShaderVariant(12, IfssLightingHash, false) == 0 &&
            IfssShaderVariant(7, IfssLightingHash, true) == 7,
            "only the pinned compute shader receives the new cache variant");
    Fixture f; auto* b = f.Block(); auto* x = f.Add(b, IR::Opcode::UndefF32);
    auto* a = f.Add(b, IR::Opcode::FPNeg32, {IR::Value{x}});
    auto* c = f.Add(b, IR::Opcode::FPNeg32, {IR::Value{x}}); f.Use(b, a); f.Use(b, c);
    f.info.pgm_hash = 0x24dad4be;
    Require(f.Run().removed == 0, "unrelated shaders are unchanged");
    f.info.pgm_hash = IfssLightingHash; f.info.sw_stage = SwStage::Fragment;
    Require(f.Run().removed == 0, "wrong shader stages are unchanged");
}
} // namespace
void RunLightingVectorTests();
int main() {
    DominanceAndOperandChains(); SiblingsAndLoopIterations(); FloatingBitsAndFlags();
    MemoryAndLaneOperations(); ScopeAndCacheIsolation();
    RunLightingVectorTests();
    std::puts("Lighting CSE dominance, loop, FP-bit, memory and cache guards passed.");
}
