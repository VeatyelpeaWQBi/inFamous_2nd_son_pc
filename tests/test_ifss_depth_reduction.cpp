// SPDX-License-Identifier: GPL-2.0-or-later
// Own synthetic SPIR-V only. Optional arguments read user-owned actual dump files; no disk writes.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>
#include <spirv/unified1/spirv.hpp11>
#include "shader_recompiler/ifss_depth_reduction.h"
using U32 = std::uint32_t;
using Op = spv::Op;
using namespace Shader::Optimization;
static void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
static void Add(std::vector<U32>& out, Op op, std::initializer_list<U32> args) {
    out.push_back((static_cast<U32>(args.size() + 1) << 16) | static_cast<U32>(op));
    out.insert(out.end(), args.begin(), args.end());
}
static std::vector<U32> Fixture() {
    std::vector<U32> w{spv::MagicNumber, 0x00010500, 0, 120, 0};
    auto add = [&](Op op, std::initializer_list<U32> a) { Add(w, op, a); };
    add(Op::OpCapability, {static_cast<U32>(spv::Capability::Shader)});
    add(Op::OpCapability, {static_cast<U32>(spv::Capability::WorkgroupMemoryExplicitLayoutKHR)});
    constexpr std::string_view extension = "SPV_KHR_workgroup_memory_explicit_layout";
    const auto literal_words = (extension.size() + 1 + 3) / 4;
    w.push_back((static_cast<U32>(literal_words + 1) << 16) | static_cast<U32>(Op::OpExtension));
    for (std::size_t at = 0; at < literal_words; ++at) {
        U32 word = 0;
        for (unsigned byte = 0; byte < 4 && at * 4 + byte < extension.size(); ++byte) {
            word |= U32(static_cast<unsigned char>(extension[at * 4 + byte])) << (byte * 8);
        }
        w.push_back(word);
    }
    add(Op::OpMemoryModel, {0, 1});
    add(Op::OpEntryPoint,
        {static_cast<U32>(spv::ExecutionModel::GLCompute), 60, 0x6e69616d, 0, 20, 23});
    add(Op::OpExecutionMode, {60, static_cast<U32>(spv::ExecutionMode::LocalSize), 16, 16, 1});
    add(Op::OpDecorate, {17, static_cast<U32>(spv::Decoration::ArrayStride), 4});
    add(Op::OpDecorate, {18, static_cast<U32>(spv::Decoration::Block)});
    add(Op::OpMemberDecorate, {18, 0, static_cast<U32>(spv::Decoration::Offset), 0});
    add(Op::OpDecorate, {23, static_cast<U32>(spv::Decoration::BuiltIn),
                         static_cast<U32>(spv::BuiltIn::LocalInvocationId)});
    add(Op::OpTypeVoid, {1});
    add(Op::OpTypeBool, {2});
    add(Op::OpTypeInt, {3, 32, 0});
    add(Op::OpTypeVector, {5, 3, 3});
    add(Op::OpTypeFloat, {6, 32});
    for (auto [id, value] : std::array<std::pair<U32, U32>, 17>{{{7, 0},
                                                                 {8, 1},
                                                                 {9, 2},
                                                                 {10, 3},
                                                                 {11, 16},
                                                                 {12, 24},
                                                                 {13, 1536},
                                                                 {14, 1540},
                                                                 {15, 1548},
                                                                 {16, 264},
                                                                 {26, 0x7fffffff},
                                                                 {27, 0x3f800000},
                                                                 {28, 1792},
                                                                 {31, 386},
                                                                 {32, 0x80000000},
                                                                 {33, 256},
                                                                 {34, 1544}}}) {
        add(Op::OpConstant, {3, id, value});
    }
    add(Op::OpConstant, {6, 29, 0});
    add(Op::OpConstant, {6, 30, 0x3f800000});
    add(Op::OpTypeArray, {17, 3, 28});
    add(Op::OpTypeStruct, {18, 17});
    add(Op::OpTypePointer, {19, static_cast<U32>(spv::StorageClass::Workgroup), 18});
    add(Op::OpVariable, {19, 20, static_cast<U32>(spv::StorageClass::Workgroup)});
    add(Op::OpTypePointer, {21, static_cast<U32>(spv::StorageClass::Workgroup), 3});
    add(Op::OpTypePointer, {22, static_cast<U32>(spv::StorageClass::Input), 5});
    add(Op::OpVariable, {22, 23, static_cast<U32>(spv::StorageClass::Input)});
    add(Op::OpTypeFunction, {25, 1});
    add(Op::OpFunction, {1, 60, 0, 25});
    add(Op::OpLabel, {61});
    add(Op::OpLoad, {5, 40, 23});
    add(Op::OpCompositeExtract, {3, 41, 40, 0});
    add(Op::OpCompositeExtract, {3, 42, 40, 1});
    add(Op::OpBitFieldUExtract, {3, 43, 42, 7, 12});
    add(Op::OpIMul, {3, 44, 43, 11});
    add(Op::OpIAdd, {3, 45, 44, 41});
    add(Op::OpShiftLeftLogical, {3, 46, 45, 9});
    add(Op::OpIAdd, {3, 47, 46, 15});
    add(Op::OpShiftRightLogical, {3, 48, 47, 9});
    add(Op::OpAccessChain, {21, 49, 20, 7, 48});
    add(Op::OpStore, {49, 27});
    add(Op::OpShiftRightLogical, {3, 50, 13, 9});
    add(Op::OpAccessChain, {21, 51, 20, 7, 50});
    add(Op::OpStore, {51, 26});
    add(Op::OpShiftRightLogical, {3, 52, 14, 9});
    add(Op::OpAccessChain, {21, 53, 20, 7, 52});
    add(Op::OpStore, {53, 7});
    add(Op::OpControlBarrier, {9, 9, 16});
    add(Op::OpFOrdNotEqual, {2, 70, 30, 29});
    add(Op::OpSelectionMerge, {64, 0});
    add(Op::OpBranchConditional, {70, 62, 64});
    add(Op::OpLabel, {62});
    add(Op::OpIAdd, {3, 75, 46, 15});
    add(Op::OpShiftRightLogical, {3, 76, 75, 9});
    add(Op::OpAccessChain, {21, 77, 20, 7, 76});
    add(Op::OpLoad, {3, 78, 77});
    add(Op::OpShiftRightLogical, {3, 79, 13, 9});
    add(Op::OpAccessChain, {21, 80, 20, 7, 79});
    add(Op::OpAtomicSMin, {3, 81, 80, 8, 7, 78});
    add(Op::OpShiftRightLogical, {3, 82, 14, 9});
    add(Op::OpAccessChain, {21, 83, 20, 7, 82});
    add(Op::OpAtomicSMax, {3, 84, 83, 8, 7, 78});
    add(Op::OpBranch, {64});
    add(Op::OpLabel, {64});
    // Keep the original invalid-pixel test independent from the elected atomic branch.
    add(Op::OpLogicalNot, {2, 86, 70});
    add(Op::OpSelectionMerge, {67, 0});
    add(Op::OpBranchConditional, {86, 65, 67});
    add(Op::OpLabel, {65});
    add(Op::OpShiftRightLogical, {3, 87, 34, 9});
    add(Op::OpAccessChain, {21, 88, 20, 7, 87});
    add(Op::OpStore, {88, 8});
    add(Op::OpBranch, {67});
    add(Op::OpLabel, {67});
    add(Op::OpControlBarrier, {9, 9, 16});
    add(Op::OpReturn, {});
    add(Op::OpFunctionEnd, {});
    return w;
}
static std::size_t Find(const std::vector<U32>& words, Op op, unsigned occurrence = 0) {
    for (std::size_t n = 5; n < words.size(); n += words[n] >> 16) {
        if ((words[n] & 65535) == static_cast<U32>(op) && occurrence-- == 0) {
            return n;
        }
    }
    std::abort();
}
static unsigned Count(const std::vector<U32>& words, Op op) {
    unsigned result = 0;
    for (std::size_t n = 5; n < words.size();) {
        const auto size = words[n] >> 16;
        Require(size && size <= words.size() - n, "valid instruction bounds");
        result += (words[n] & 65535) == static_cast<U32>(op);
        n += size;
    }
    return result;
}
static void Reject(std::vector<U32> words) {
    const auto original = words;
    Require(!ReduceIfssDepthAtomics(words).applied, "unsupported pattern must reject");
    Require(words == original, "fallback must leave every input word unchanged");
}
static void TransformChecks() {
    auto original = Fixture(), words = original;
    auto result = ReduceIfssDepthAtomics(words);
    Require(result.applied, "synthetic matching prefix is optimized");
    Require(Count(words, Op::OpGroupNonUniformSMin) == 1 &&
                Count(words, Op::OpGroupNonUniformSMax) == 1 &&
                Count(words, Op::OpGroupNonUniformElect) == 1,
            "exactly one native reduction pair and election");
    Require(Count(words, Op::OpAtomicSMin) == 1 && Count(words, Op::OpAtomicSMax) == 1 &&
                Count(words, Op::OpControlBarrier) == Count(original, Op::OpControlBarrier),
            "keep original atomics and every barrier");
    auto branch = Find(words, Op::OpBranchConditional),
         elect = Find(words, Op::OpGroupNonUniformElect);
    auto selected = Find(words, Op::OpLogicalAnd);
    Require(words[branch + 1] == words[selected + 2] && words[selected + 3] == words[elect + 2],
            "only an elected lane in a nonempty subgroup performs the atomics");
    Require(words[Find(words, Op::OpLogicalNot) + 3] == 70 &&
                words[Find(words, Op::OpBranchConditional, 1) + 1] == 86 &&
                words[Find(words, Op::OpStore, 3) + 2] == 8,
            "original invalid-pixel predicate and flag write must remain unchanged");
    auto min = Find(words, Op::OpAtomicSMin), max = Find(words, Op::OpAtomicSMax);
    Require(words[min + 6] == words[Find(words, Op::OpGroupNonUniformSMin) + 2] &&
                words[max + 6] == words[Find(words, Op::OpGroupNonUniformSMax) + 2],
            "atomic data must be subgroup results");
    Require(Find(words, Op::OpLoad, 1) < branch, "moved shared load dominates the atomic branch");
    auto again = words;
    Require(!ReduceIfssDepthAtomics(again).applied && again == words,
            "second application cannot rewrite the candidate again");
    auto bad = original;
    bad[5] = 0;
    Reject(bad);
    bad = original;
    bad[Find(bad, Op::OpExecutionMode) + 4] = 8;
    Reject(bad);
    bad = original;
    bad[Find(bad, Op::OpAtomicSMin) + 5] = 8;
    Reject(bad); // Acquire/release is not relaxed.
    bad = original;
    bad[Find(bad, Op::OpStore) + 2] = 81;
    Reject(bad); // Atomic return becomes observable.
    bad = original;
    bad[Find(bad, Op::OpConstant, 8) + 3] = 1536;
    Reject(bad); // Temporary aliases depth.
    bad = original;
    auto second = Find(bad, Op::OpControlBarrier, 1);
    bad.erase(bad.begin() + second, bad.begin() + second + 4);
    Reject(bad);
    bad = original;
    bad[Find(bad, Op::OpMemberDecorate) + 4] = 16;
    Reject(bad); // Unknown aliased layout.
    bad = original;
    auto before = Find(bad, Op::OpAtomicSMin);
    std::vector<U32> store;
    Add(store, Op::OpStore, {77, 27});
    bad.insert(bad.begin() + before, store.begin(), store.end());
    Reject(bad);
    Require(IfssShaderVariant(4, IfssLightingHash, true) == 4 &&
                IfssShaderVariant(4, IfssLightingHash, false) == 0 &&
                IfssShaderVariant(7, 123, true) == 3,
            "only target compute binary changes cache variant");
    Require(!IsIfssDepthShader({}), "guest fingerprint cannot accept empty or unrelated code");
}
static void IntegerEquivalence() {
    U32 random = 0x87654321;
    auto next = [&] {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return random;
    };
    constexpr auto min_identity = std::numeric_limits<std::int32_t>::max();
    constexpr auto max_identity = std::numeric_limits<std::int32_t>::min();
    for (unsigned size : {8, 16, 32, 64, 128, 256}) {
        for (unsigned trial = 0; trial < 1024; ++trial) {
            std::array<U32, 256> bits{};
            std::array<bool, 256> active{}, valid{};
            std::int32_t expected_min = min_identity, expected_max = 0, actual_min = min_identity,
                         actual_max = 0;
            unsigned original_atomics = 0, candidate_atomics = 0;
            for (unsigned i = 0; i < 256; ++i) {
                bits[i] = trial == 0   ? 0
                          : trial == 1 ? 0x80000000
                          : trial == 2 ? 0x7fc00000
                                       : next();
                active[i] = (next() & 3) != 0;
                const bool nan = (bits[i] & 0x7f800000) == 0x7f800000 && (bits[i] & 0x7fffff) != 0;
                valid[i] = (bits[i] & 0x7fffffff) != 0 && !nan;
                if (active[i] && valid[i]) {
                    original_atomics += 2;
                    const auto v = std::bit_cast<std::int32_t>(bits[i]);
                    expected_min = std::min(expected_min, v);
                    expected_max = std::max(expected_max, v);
                }
            }
            for (unsigned begin = 0; begin < 256; begin += size) {
                std::int32_t group_min = min_identity, group_max = max_identity;
                bool elected = false;
                for (unsigned i = begin; i < begin + size; ++i) {
                    if (active[i]) {
                        elected = true;
                        const auto v = std::bit_cast<std::int32_t>(bits[i]);
                        group_min = std::min(group_min, valid[i] ? v : min_identity);
                        group_max = std::max(group_max, valid[i] ? v : max_identity);
                    }
                }
                if (elected && (group_min != min_identity || group_max != max_identity)) {
                    candidate_atomics += 2;
                    actual_min = std::min(actual_min, group_min);
                    actual_max = std::max(actual_max, group_max);
                }
            }
            Require(expected_min == actual_min && expected_max == actual_max,
                    "signed bitwise reduction must preserve original masked atomics");
            Require(candidate_atomics <= original_atomics,
                    "empty/sparse subgroups cannot increase atomic count");
        }
    }
}
int main(int argc, char** argv) {
    TransformChecks();
    IntegerEquivalence();
    for (int n = 1; n < argc; ++n) {
        std::ifstream stream(argv[n], std::ios::binary | std::ios::ate);
        Require(bool(stream), "open actual dump");
        const std::streamoff length = stream.tellg();
        Require(length > 0 && length % 4 == 0, "dump word size");
        std::vector<U32> words(static_cast<std::size_t>(length) / 4);
        stream.seekg(0);
        Require(bool(stream.read(reinterpret_cast<char*>(words.data()), length)),
                "read entire dump");
        const auto before = words;
        const auto result = ReduceIfssDepthAtomics(words);
        if (!result.applied) {
            std::fprintf(stderr, "Actual dump rejected: %s\n", result.reason);
        }
        Require(result.applied, "observed target permutation must match");
        Require(Count(words, Op::OpControlBarrier) == Count(before, Op::OpControlBarrier),
                "actual dump retains barriers");
    }
    std::puts("C1 depth reduction regression checks passed; no GPU or game was started.");
}
