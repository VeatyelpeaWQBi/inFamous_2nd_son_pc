// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <limits>
#include <vector>
#include <spirv/unified1/spirv.hpp11>
#include "shader_recompiler/gds_counter_plan.h"

namespace Shader {
namespace {
class Inspector {
public:
    explicit Inspector(std::span<const u32> code_) : code{code_} {
        if (code.size() < 5 || code[0] != spv::MagicNumber || code[3] == 0 ||
            code[3] > (1u << 20) || code.size() > std::numeric_limits<u32>::max()) return;
        definitions.resize(code[3]);
        types.resize(code[3]);
        bindings.assign(code[3], Unknown);
        sets.assign(code[3], Unknown);
        strides.resize(code[3]);
        zero_member.resize(code[3]);
        bool logical_model = false;
        for (size_t pos = 5; pos < code.size();) {
            const u32 count = code[pos] >> 16;
            if (!count || count > code.size()-pos) return;
            const auto words = code.subspan(pos, count);
            const auto op = Op(words);
            if (op == spv::Op::OpMemoryModel) {
                if (count != 3 || words[1] != u32(spv::AddressingModel::Logical)) return;
                logical_model = true; // Exclude arbitrary physical-pointer aliases.
            }
            positions.push_back(static_cast<u32>(pos));
            if (op >= spv::Op::OpTypeVoid && op < spv::Op::OpTypeForwardPointer) {
                if (count < 2 || words[1] >= definitions.size()) return;
                definitions[words[1]] = static_cast<u32>(pos);
                types[words[1]] = true;
            } else if (count >= 3 && words[1] < types.size() && types[words[1]]) {
                if (words[2] >= definitions.size()) return;
                definitions[words[2]] = static_cast<u32>(pos);
            }
            if (op == spv::Op::OpDecorate && count >= 4 && words[1] < bindings.size()) {
                const auto decoration = static_cast<spv::Decoration>(words[2]);
                if (decoration == spv::Decoration::Binding) bindings[words[1]] = words[3];
                if (decoration == spv::Decoration::DescriptorSet) sets[words[1]] = words[3];
                if (decoration == spv::Decoration::ArrayStride) strides[words[1]] = words[3];
            }
            if (op == spv::Op::OpMemberDecorate && count == 5 && words[1] < zero_member.size() &&
                words[2] == 0 && words[3] == u32(spv::Decoration::Offset) && words[4] == 0) {
                zero_member[words[1]] = true;
            }
            pos += count;
        }
        valid = logical_model;
    }

    std::optional<u32> Inspect(u32 gds_binding, u32 flat_binding) const {
        if (!valid) return {};
        const auto gds = Root(gds_binding), flat = Root(flat_binding);
        if (!gds || !flat || *gds == *flat || !ArrayRoot(*gds) || !ArrayRoot(*flat) ||
            !ReadOnlyTable(*flat)) return {};
        std::optional<u32> result;
        bool accessed = false;
        for (const auto pos : positions) {
            const auto words = Instruction(pos);
            if (!Uses(words, *gds) || Metadata(Op(words))) continue;
            if ((Op(words) != spv::Op::OpAccessChain &&
                 Op(words) != spv::Op::OpInBoundsAccessChain) || words.size() != 6 ||
                words[3] != *gds || !ConstantIs(words[4], 0) || !U32Pointer(words[1])) return {};
            const auto word = CounterWord(words[5], *flat);
            if (!word || (result && result != word)) return {};
            const u32 pointer = words[2];
            bool consumed = false;
            for (const auto use_pos : positions) {
                const auto use = Instruction(use_pos);
                if (!Uses(use, pointer) || Metadata(Op(use))) continue;
                // Only a direct u32 append atomic is currently supported. A pointer copied,
                // passed to a function, stored, loaded or used by any other operation rejects
                // the whole module. Scope/semantics/returned slot remain untouched.
                if (Op(use) != spv::Op::OpAtomicIAdd || use.size() != 7 ||
                    use[3] != pointer || !U32(use[1]) ||
                    std::count(use.begin()+1, use.end(), pointer) != 1) return {};
                consumed = true;
            }
            if (consumed) { result = word; accessed = true; }
        }
        return accessed ? result : std::nullopt;
    }

private:
    static constexpr u32 Unknown = std::numeric_limits<u32>::max();
    static spv::Op Op(std::span<const u32> words) {
        return words.empty() ? spv::Op::OpNop : static_cast<spv::Op>(words[0] & 0xffff);
    }
    std::span<const u32> Instruction(u32 pos) const { return code.subspan(pos, code[pos] >> 16); }
    std::span<const u32> Def(u32 id) const {
        return id < definitions.size() && definitions[id] ? Instruction(definitions[id])
                                                         : std::span<const u32>{};
    }
    static bool Metadata(spv::Op op) {
        // These instructions cannot consume a runtime pointer. Do not treat capability,
        // execution-mode, type or constant literals as pointer IDs (e.g. capability 61
        // happens to equal the table variable's ID in a retained real module).
        if ((op >= spv::Op::OpTypeVoid && op <= spv::Op::OpTypeForwardPointer) ||
            (op >= spv::Op::OpConstantTrue && op <= spv::Op::OpSpecConstantOp)) return true;
        switch (op) {
        case spv::Op::OpSourceContinued:
        case spv::Op::OpSource:
        case spv::Op::OpSourceExtension:
        case spv::Op::OpName:
        case spv::Op::OpMemberName:
        case spv::Op::OpString:
        case spv::Op::OpLine:
        case spv::Op::OpExtension:
        case spv::Op::OpMemoryModel:
        case spv::Op::OpEntryPoint:
        case spv::Op::OpExecutionMode:
        case spv::Op::OpExecutionModeId:
        case spv::Op::OpCapability:
        case spv::Op::OpDecorate:
        case spv::Op::OpMemberDecorate:
        case spv::Op::OpDecorationGroup:
        case spv::Op::OpGroupDecorate:
        case spv::Op::OpGroupMemberDecorate:
        case spv::Op::OpDecorateId:
        case spv::Op::OpModuleProcessed:
            return true;
        default:
            return false;
        }
    }
    bool Uses(std::span<const u32> words, u32 id) const {
        for (size_t i = 1; i < words.size(); ++i) {
            // Result declarations are not uses. Literal words equal to an ID can cause a
            // conservative rejection; they can never silently admit an unknown pointer use.
            if (words[i] != id) continue;
            if (i == 2 && words.size() >= 3 && words[1] < types.size() && types[words[1]]) continue;
            return true;
        }
        return false;
    }
    bool U32(u32 type) const {
        const auto w = Def(type);
        return Op(w) == spv::Op::OpTypeInt && w.size() == 4 && w[2] == 32 && w[3] == 0;
    }
    bool U32Pointer(u32 type) const {
        const auto w = Def(type);
        return Op(w) == spv::Op::OpTypePointer && w.size() == 4 &&
               w[2] == u32(spv::StorageClass::StorageBuffer) && U32(w[3]);
    }
    std::optional<u32> Constant(u32 id) const {
        const auto w = Def(id);
        if (Op(w) != spv::Op::OpConstant || w.size() != 4 || !U32(w[1])) return {};
        return w[3];
    }
    bool ConstantIs(u32 id, u32 value) const { return Constant(id) == std::optional{value}; }
    std::optional<u32> Root(u32 binding) const {
        std::optional<u32> root;
        for (u32 id = 0; id < bindings.size(); ++id) {
            if (bindings[id] == binding && sets[id] == 0) {
                if (root) return {}; // Aliased typed views are not supported by this plan.
                root = id;
            }
        }
        return root;
    }
    bool ArrayRoot(u32 root) const {
        const auto var = Def(root);
        if (Op(var) != spv::Op::OpVariable || var.size() != 4 ||
            var[3] != u32(spv::StorageClass::StorageBuffer)) return false;
        const auto pointer = Def(var[1]);
        if (Op(pointer) != spv::Op::OpTypePointer || pointer.size() != 4 ||
            pointer[2] != u32(spv::StorageClass::StorageBuffer)) return false;
        const auto object = Def(pointer[3]);
        if (Op(object) != spv::Op::OpTypeStruct || object.size() != 3 ||
            !zero_member[pointer[3]]) return false;
        const auto array = Def(object[2]);
        return Op(array) == spv::Op::OpTypeRuntimeArray && array.size() == 3 &&
               U32(array[2]) && strides[object[2]] == 4;
    }
    bool ReadOnlyTable(u32 flat) const {
        for (const auto pos : positions) {
            const auto words = Instruction(pos);
            if (!Uses(words, flat) || Metadata(Op(words))) continue;
            if (words.size() < 4 || words[3] != flat || !U32Pointer(words[1]) ||
                !((Op(words) == spv::Op::OpAccessChain && words.size() == 6) ||
                  (Op(words) == spv::Op::OpInBoundsAccessChain && words.size() == 6) ||
                  (Op(words) == spv::Op::OpRawAccessChainNV && words.size() == 8))) return false;
            const u32 pointer = words[2];
            for (const auto use_pos : positions) {
                const auto use = Instruction(use_pos);
                if (!Uses(use, pointer) || Metadata(Op(use))) continue;
                if (Op(use) != spv::Op::OpLoad || use.size() < 4 || use[3] != pointer ||
                    !U32(use[1]) || std::count(use.begin()+1,use.end(),pointer) != 1) return false;
            }
        }
        return true;
    }
    std::optional<u32> CounterWord(u32 index, u32 flat) const {
        const auto shift = Def(index);
        if (Op(shift) != spv::Op::OpShiftRightLogical || shift.size() != 5 ||
            !U32(shift[1]) || !ConstantIs(shift[4], 2)) return {};
        const auto add = Def(shift[3]);
        if (Op(add) != spv::Op::OpIAdd || add.size() != 5 || !U32(add[1]) ||
            !ConstantIs(add[4], 12)) return {};
        const auto extract = Def(add[3]);
        if (Op(extract) != spv::Op::OpBitFieldUExtract || extract.size() != 6 ||
            !U32(extract[1]) || !ConstantIs(extract[4], 16) || !ConstantIs(extract[5], 16)) return {};
        const auto load = Def(extract[3]);
        if (Op(load) != spv::Op::OpLoad || load.size() < 4 || !U32(load[1])) return {};
        const auto pointer = Def(load[3]);
        if (pointer.size() < 4 || pointer[3] != flat || !U32Pointer(pointer[1])) return {};
        if ((Op(pointer) == spv::Op::OpAccessChain || Op(pointer) == spv::Op::OpInBoundsAccessChain) &&
            pointer.size() == 6 && ConstantIs(pointer[4], 0)) return Constant(pointer[5]);
        if (Op(pointer) == spv::Op::OpRawAccessChainNV && pointer.size() == 8 &&
            ConstantIs(pointer[4], 4) && ConstantIs(pointer[6], 0)) return Constant(pointer[5]);
        return {};
    }
    std::span<const u32> code;
    std::vector<u32> definitions, bindings, sets, strides, positions;
    std::vector<bool> types, zero_member;
    bool valid{};
};
} // namespace

std::optional<u32> AnalyzeGdsCounterPlan(std::span<const u32> spv, u32 gds_binding, u32 flat_binding) {
    return Inspector{spv}.Inspect(gds_binding, flat_binding);
}

} // namespace Shader
