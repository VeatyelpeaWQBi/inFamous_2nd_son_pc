// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "shader_recompiler/ifss_depth_reduction.h"
#include "shader_recompiler/ifss_vector_array_length.h"
#include "shader_recompiler/ir/passes/ifss_lighting_vector.h"
#include "shader_recompiler/ir/program.h"
namespace {
using namespace Shader;
void Check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL vector: %s\n", why); std::exit(1); }
}
struct Fixture {
    Common::ObjectPool<IR::Inst> pool;
    Info info{};
    IR::Program program{info};
    IR::Block block{pool};
    std::array<IR::Inst*, 8> reads{}, uses{};
    Fixture(unsigned count = 4, int obstruction = 0) {
        info.pgm_hash = Optimization::IfssLightingHash;
        info.hw_stage = HwStage::Compute; info.sw_stage = SwStage::Compute;
        info.buffers.push_back({.used_types=IR::Type::U32, .buffer_type=BufferType::Guest});
        program.post_order_blocks.push_back(&block);
        for (u32 i = 0; i < count; ++i) {
            auto read = block.PrependNewInst(block.end(), IR::Opcode::ReadConstBuffer,
                                             {IR::Value{0u}, IR::Value{8u + i}});
            read->SetFlags(u64{23} << 32);
            reads[i] = &*read;
            if (i == 1 && obstruction == 1) block.AppendNewInst(IR::Opcode::Barrier, {});
            if (i == 1 && obstruction == 2) block.AppendNewInst(IR::Opcode::WriteSharedU32,
                                                               {IR::Value{0u}, IR::Value{9u}});
        }
        for (u32 i = 0; i < count; ++i) {
            auto use = block.PrependNewInst(block.end(), IR::Opcode::WriteSharedU32,
                                            {IR::Value{i * 4u}, IR::Value{reads[i]}});
            uses[i] = &*use;
        }
    }
    std::size_t Run() { return Optimization::PackIfssLightingReads(program); }
};
void ExactOriginAndMemoryBoundaries() {
    for (unsigned count : {4u, 8u}) {
        Fixture f(count);
        Check(f.Run() == count / 4, "retained original instruction chunks are packed");
        for (unsigned i = 0; i < count; ++i) {
            const auto* extract = f.uses[i]->Arg(1).Inst();
            Check(extract->GetOpcode() == IR::Opcode::CompositeExtractU32x4 &&
                  extract->Arg(1).U32() == i % 4 &&
                  extract->Arg(0).Inst()->GetOpcode() == IR::Opcode::IfssReadConstBufferU32x4,
                  "every raw word keeps its component and bit identity");
        }
        Check(f.Run() == 0, "packing is stable and does not recurse into its output");
    }
    for (int obstruction : {1, 2}) { Fixture f(4, obstruction); Check(f.Run() == 0, "never cross a barrier/store"); }
    Fixture mixed; mixed.reads[2]->SetFlags(u64{24} << 32);
    Check(mixed.Run() == 0, "identical adjacent addresses from different guest instructions stay scalar");
    Fixture partial(3); Check(partial.Run() == 0, "never fetch a discarded fourth word");
    Fixture nonconsecutive; nonconsecutive.reads[3]->SetArg(1, IR::Value{15u});
    Check(nonconsecutive.Run() == 0, "gaps are never widened");
    Fixture written; written.info.buffers[0].is_written = true;
    Check(written.Run() == 0, "written/coherent resource is excluded");
    Fixture sharp; IR::BufferInstInfo flags{}; flags.pc.Assign(23); flags.sharp_source.Assign(1);
    for (auto* read : sharp.reads) if (read) read->SetFlags(flags.raw);
    Check(sharp.Run() == 0, "descriptor source loads stay on the resource-flattening path");
    Fixture unknown; for (auto* read : unknown.reads) if (read) read->SetFlags(u64{0});
    Check(unknown.Run() == 0, "missing original PC cannot establish one instruction");
    Fixture wrong; wrong.info.pgm_hash = 123;
    Check(wrong.Run() == 0, "unrelated shader is untouched");
    Fixture dynamic;
    auto base = dynamic.block.PrependNewInst(dynamic.block.begin(), IR::Opcode::UndefU32);
    for (u32 i = 0; i < 4; ++i) {
        auto at = dynamic.block.Instructions().iterator_to(*dynamic.reads[i]);
        auto address = dynamic.block.PrependNewInst(at, IR::Opcode::IAdd32,
                                                   {IR::Value{&*base}, IR::Value{i}});
        dynamic.reads[i]->SetArg(1, IR::Value{&*address});
    }
    Check(dynamic.Run() == 1, "shared dynamic base is retained with modulo U32 offsets");
    Check(Optimization::IfssShaderVariant(16, Optimization::IfssLightingHash, true) == 16 &&
          Optimization::IfssShaderVariant(16, 123, true) == 0 &&
          Optimization::IfssShaderVariant(16, Optimization::IfssLightingHash, false) == 0,
          "candidate binary cache is isolated to the target compute shader");
}
void BoundaryAndWrapModel() {
    // Different lengths include null, partial final vectors, and exact ends. Raw values
    // include NaN/signed-zero bit patterns; comparison is always bitwise integer equality.
    const std::array<u32, 20> memory{0,0x80000000,0x7fc00001,0xffffffff,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19};
    for (u32 length = 0; length <= memory.size(); ++length) {
        for (u32 base : {0u,1u,2u,3u,4u,12u,16u,17u,19u,20u,0xfffffffcu,0xfffffffeu,0xffffffffu}) {
            for (u32 adjust = 0; adjust < 64; ++adjust) {
                const u32 index = base + adjust;
                const bool fast = (index & 3u) == 0 && length >= 4 && index <= length - 4;
                for (u32 component = 0; component < 4; ++component) {
                    const u32 original_index = (base + component) + adjust;
                    const u32 old_value = original_index < length ? memory[original_index] : 0;
                    const u32 selected_index = fast ? (index / 4) * 4 + component : index + component;
                    const u32 new_value = selected_index < length ? memory[selected_index] : 0;
                    Check(old_value == new_value, "guarded vector/fallback preserves null, tail, wrap and FP bits");
                }
            }
        }
    }
}
void ArrayLengthFinalization() {
    using Optimization::IfssArrayLength;
    using Optimization::FinalizeIfssArrayLengths;
    const std::vector<u32> input{0x07230203,0x00010500,0,20,0,(5u<<16)|128u,2,6,3,3};
    const std::array<IfssArrayLength,1> request{{{6,2,3,8}}};
    auto words = input;
    Check(FinalizeIfssArrayLengths(words, request) && words[5] == ((5u<<16)|68u) &&
          words[6] == 2 && words[7] == 6 && words[8] == 8 && words[9] == 0,
          "tracked result becomes array length with a literal member index");
    const std::array<IfssArrayLength,2> duplicate{{request[0], request[0]}};
    words = input;
    Check(!FinalizeIfssArrayLengths(words, duplicate) && words == input,
          "duplicate request IDs cannot overwrite one another");
    for (unsigned mode = 0; mode < 4; ++mode) {
        words = input;
        if (mode == 0) words[7] = 7;
        if (mode == 1) words[9] = 4;
        if (mode == 2) words.push_back(0);
        if (mode == 3) words.insert(words.end(), input.begin()+5, input.end());
        const auto original = words;
        Check(!FinalizeIfssArrayLengths(words, request) && words == original,
              "missing, wrong, truncated or duplicate placeholder is transactional");
    }
}
}
void RunLightingVectorTests() {
    ExactOriginAndMemoryBoundaries(); BoundaryAndWrapModel(); ArrayLengthFinalization();
    std::puts("Lighting vector original-PC, memory-boundary, partial-read and U32-wrap tests passed.");
}
