// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/ifss_depth_reduction.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <spirv/unified1/spirv.hpp11>

namespace Shader::Optimization {
namespace {
using U32 = std::uint32_t;
using Op = spv::Op;
struct Instruction {
    std::span<const U32> w;
    U32 function{}, block{};
    Op op() const { return static_cast<Op>(w[0] & 65535u); }
};
struct Module {
    std::vector<Instruction> inst;
    std::unordered_map<U32, std::size_t> defs;
    std::unordered_map<U32, U32> builtins, strides, offsets;
    U32 entry{}, first_block{};
    bool local_size{}, arithmetic{}, basic{};
    std::size_t first_function{}, capabilities_end{};

    const Instruction* Def(U32 id) const {
        const auto it = defs.find(id);
        return it == defs.end() ? nullptr : &inst[it->second];
    }
    std::optional<U32> Constant(U32 id) const {
        const auto* d = Def(id);
        if (!d || d->op() != Op::OpConstant || d->w.size() != 4) {
            return {};
        }
        const auto* type = Def(d->w[1]);
        if (!type || type->op() != Op::OpTypeInt || type->w.size() != 4 || type->w[2] != 32) {
            return {};
        }
        return d->w[3];
    }
    bool Eq(U32 a, U32 b, unsigned depth = 0) const {
        if (a == b) {
            return true;
        }
        if (depth > 16) {
            return false;
        }
        const auto ca = Constant(a), cb = Constant(b);
        if (ca && cb) {
            return ca == cb;
        }
        const auto* x = Def(a);
        const auto* y = Def(b);
        if (!x || !y || x->op() != y->op() || x->w.size() != y->w.size() || x->w[1] != y->w[1]) {
            return false;
        }
        switch (x->op()) {
        case Op::OpIAdd:
        case Op::OpIMul:
        case Op::OpShiftRightLogical:
        case Op::OpShiftLeftLogical:
        case Op::OpBitwiseAnd:
            return x->w.size() == 5 && Eq(x->w[3], y->w[3], depth + 1) &&
                   Eq(x->w[4], y->w[4], depth + 1);
        default:
            return false;
        }
    }
    // Only the small, unsigned local-index expression is interpreted; unknown expressions reject.
    std::optional<U32> Eval(U32 id, U32 x, U32 y, unsigned depth = 0) const {
        if (depth > 32) {
            return {};
        }
        if (auto c = Constant(id)) {
            return c;
        }
        const auto* d = Def(id);
        if (!d) {
            return {};
        }
        if (d->op() == Op::OpCompositeExtract && d->w.size() == 5 && d->w[4] < 3) {
            const auto* load = Def(d->w[3]);
            if (!load || load->op() != Op::OpLoad || load->w.size() != 4) {
                return {};
            }
            const auto it = builtins.find(load->w[3]);
            if (it == builtins.end() ||
                it->second != static_cast<U32>(spv::BuiltIn::LocalInvocationId)) {
                return {};
            }
            return std::array<U32, 3>{x, y, 0}[d->w[4]];
        }
        if (d->w.size() < 5) {
            return {};
        }
        auto a = Eval(d->w[3], x, y, depth + 1), b = Eval(d->w[4], x, y, depth + 1);
        if (!a || !b) {
            return {};
        }
        switch (d->op()) {
        case Op::OpIAdd:
            return *a + *b;
        case Op::OpIMul:
            return *a * *b;
        case Op::OpBitwiseAnd:
            return *a & *b;
        case Op::OpShiftLeftLogical:
            return *b < 32 ? std::optional<U32>{*a << *b} : std::nullopt;
        case Op::OpShiftRightLogical:
            return *b < 32 ? std::optional<U32>{*a >> *b} : std::nullopt;
        case Op::OpBitFieldUExtract: {
            if (d->w.size() != 6) {
                return {};
            }
            auto count = Eval(d->w[5], x, y, depth + 1);
            if (!count || *b >= 32 || *count > 32 - *b) {
                return {};
            }
            const U32 mask = *count == 32 ? ~U32{} : (U32{1} << *count) - 1;
            return (*a >> *b) & mask;
        }
        default:
            return {};
        }
    }
    struct Pointer {
        U32 base, index, bytes, elements;
    };
    std::optional<Pointer> SharedPointer(U32 id) const {
        const auto* p = Def(id);
        if (!p || p->op() != Op::OpAccessChain || p->w.size() != 6 || Constant(p->w[4]) != 0) {
            return {};
        }
        const auto* t = Def(p->w[1]);
        const auto* var = Def(p->w[3]);
        if (!t || t->op() != Op::OpTypePointer || t->w.size() != 4 ||
            t->w[2] != static_cast<U32>(spv::StorageClass::Workgroup) || !var ||
            var->op() != Op::OpVariable || var->w.size() != 4 || var->w[3] != t->w[2]) {
            return {};
        }
        const auto* scalar = Def(t->w[3]);
        if (!scalar || scalar->op() != Op::OpTypeInt || scalar->w.size() != 4 ||
            (scalar->w[2] != 32 && scalar->w[2] != 64)) {
            return {};
        }
        const auto* base_type = Def(var->w[1]);
        if (!base_type || base_type->op() != Op::OpTypePointer || base_type->w.size() != 4 ||
            base_type->w[2] != t->w[2]) {
            return {};
        }
        const auto* structure = Def(base_type->w[3]);
        if (!structure || structure->op() != Op::OpTypeStruct || structure->w.size() != 3) {
            return {};
        }
        const auto* array = Def(structure->w[2]);
        if (!array || array->op() != Op::OpTypeArray || array->w.size() != 4 ||
            array->w[2] != t->w[3] || !Constant(array->w[3])) {
            return {};
        }
        const auto stride = strides.find(array->w[1]), offset = offsets.find(structure->w[1]);
        if (stride == strides.end() || stride->second != scalar->w[2] / 8 ||
            offset == offsets.end() || offset->second != 0) {
            return {};
        }
        return Pointer{p->w[3], p->w[5], scalar->w[2] / 8, *Constant(array->w[3])};
    }
};
void Append(std::vector<U32>& out, Op op, std::initializer_list<U32> args) {
    out.push_back((static_cast<U32>(args.size() + 1) << 16) | static_cast<U32>(op));
    out.insert(out.end(), args.begin(), args.end());
}
std::optional<Module> Parse(std::span<const U32> words) {
    if (words.size() < 5 || words[0] != spv::MagicNumber || words[1] < 0x00010300 || words[3] < 1 ||
        words[3] > 0x003ffff0 || words[4] != 0) {
        return {};
    }
    Module m;
    U32 function = 0, block = 0;
    m.first_function = std::numeric_limits<std::size_t>::max();
    for (std::size_t at = 5; at < words.size();) {
        const auto count = words[at] >> 16;
        if (!count || count > words.size() - at) {
            return {};
        }
        auto w = words.subspan(at, count);
        const auto op = static_cast<Op>(w[0] & 65535u);
        if (op == Op::OpFunction) {
            if (count != 5) {
                return {};
            }
            function = w[2];
            block = 0;
            m.first_function = std::min(m.first_function, m.inst.size());
        }
        if (op == Op::OpLabel) {
            if (count != 2) {
                return {};
            }
            block = w[1];
        }
        const std::size_t index = m.inst.size();
        m.inst.push_back({w, function, block});
        U32 result = 0;
        switch (op) {
        case Op::OpTypeInt:
        case Op::OpTypeBool:
        case Op::OpTypePointer:
        case Op::OpTypeVector:
        case Op::OpTypeArray:
        case Op::OpTypeRuntimeArray:
        case Op::OpTypeStruct:
        case Op::OpLabel:
            if (count < 2) {
                return {};
            }
            result = w[1];
            break;
        case Op::OpConstant:
        case Op::OpVariable:
        case Op::OpLoad:
        case Op::OpAccessChain:
        case Op::OpIAdd:
        case Op::OpIMul:
        case Op::OpShiftLeftLogical:
        case Op::OpShiftRightLogical:
        case Op::OpBitFieldUExtract:
        case Op::OpBitwiseAnd:
        case Op::OpCompositeExtract:
        case Op::OpAtomicSMin:
        case Op::OpAtomicSMax:
        case Op::OpFOrdNotEqual:
            if (count < 3) {
                return {};
            }
            result = w[2];
            break;
        default:
            break;
        }
        if (result && !m.defs.emplace(result, index).second) {
            return {};
        }
        if (op == Op::OpEntryPoint) {
            if (count < 4 || m.entry || w[1] != static_cast<U32>(spv::ExecutionModel::GLCompute)) {
                return {};
            }
            m.entry = w[2];
        }
        if (op == Op::OpExecutionMode && count == 6 &&
            w[2] == static_cast<U32>(spv::ExecutionMode::LocalSize)) {
            m.local_size = w[1] == m.entry && w[3] == 16 && w[4] == 16 && w[5] == 1;
        }
        if (op == Op::OpDecorate && count == 4 &&
            w[2] == static_cast<U32>(spv::Decoration::BuiltIn)) {
            m.builtins[w[1]] = w[3];
        }
        if (op == Op::OpDecorate && count == 4 &&
            w[2] == static_cast<U32>(spv::Decoration::ArrayStride)) {
            m.strides[w[1]] = w[3];
        }
        if (op == Op::OpMemberDecorate && count == 5 && w[2] == 0 &&
            w[3] == static_cast<U32>(spv::Decoration::Offset)) {
            m.offsets[w[1]] = w[4];
        }
        if (op == Op::OpCapability && count == 2) {
            m.capabilities_end = index + 1;
            m.basic |= w[1] == static_cast<U32>(spv::Capability::GroupNonUniform);
            m.arithmetic |= w[1] == static_cast<U32>(spv::Capability::GroupNonUniformArithmetic);
        }
        if (op == Op::OpLabel && function == m.entry && !m.first_block) {
            m.first_block = block;
        }
        if (op == Op::OpFunctionEnd) {
            function = 0;
            block = 0;
        }
        at += count;
    }
    if (!m.entry || !m.first_block || !m.local_size || m.first_function >= m.inst.size()) {
        return {};
    }
    return m;
}
} // namespace

DepthReductionResult ReduceIfssDepthAtomics(std::vector<U32>& words) {
    auto parsed = Parse(words);
    if (!parsed) {
        return {false, "module-or-workgroup"};
    }
    const auto& m = *parsed;
    const Instruction *min = nullptr, *max = nullptr;
    for (const auto& i : m.inst) {
        if (i.op() == Op::OpAtomicSMin) {
            if (min) {
                return {false, "multiple-min"};
            }
            min = &i;
        }
        if (i.op() == Op::OpAtomicSMax) {
            if (max) {
                return {false, "multiple-max"};
            }
            max = &i;
        }
    }
    if (!min || !max || min->w.size() != 7 || max->w.size() != 7 || min->function != m.entry ||
        max->function != m.entry || min->block != max->block || min->w[1] != max->w[1] ||
        min->w[6] != max->w[6] || m.Constant(min->w[4]) != 1 || m.Constant(max->w[4]) != 1 ||
        m.Constant(min->w[5]) != 0 || m.Constant(max->w[5]) != 0) {
        return {false, "atomic-pair"};
    }
    const U32 body = min->block, value = min->w[6], uint_type = min->w[1];
    const auto min_ptr = m.SharedPointer(min->w[3]), max_ptr = m.SharedPointer(max->w[3]);
    const auto* load = m.Def(value);
    const auto* type = m.Def(uint_type);
    if (!type || type->op() != Op::OpTypeInt || type->w.size() != 4 || type->w[2] != 32 ||
        type->w[3] != 0 || !min_ptr || !max_ptr || min_ptr->base != max_ptr->base ||
        min_ptr->bytes != 4 || max_ptr->bytes != 4 || !load || load->op() != Op::OpLoad ||
        load->w.size() != 4 || load->w[1] != uint_type || load->block != body) {
        return {false, "shared-pair"};
    }
    const auto source = m.SharedPointer(load->w[3]);
    if (!source || source->base != min_ptr->base || source->bytes != 4) {
        return {false, "source-pointer"};
    }
    const auto lo = m.Eval(min_ptr->index, 0, 0), hi = m.Eval(max_ptr->index, 0, 0);
    if (!lo || !hi || *lo != 384 || *hi != 385 || *lo >= min_ptr->elements ||
        *hi >= max_ptr->elements) {
        return {false, "depth-addresses"};
    }
    // Fixed atomic addresses must remain uniform for every local invocation.
    for (U32 y = 0; y < 16; ++y) {
        for (U32 x = 0; x < 16; ++x) {
            if (m.Eval(min_ptr->index, x, y) != lo || m.Eval(max_ptr->index, x, y) != hi) {
                return {false, "varying-depth-address"};
            }
        }
    }
    const Instruction *branch = nullptr, *merge = nullptr, *barrier = nullptr;
    std::vector<const Instruction*> pure, stores;
    U32 body_merge = 0;
    unsigned loads = 0, predecessors = 0;
    for (const auto& i : m.inst) {
        if (i.function != m.entry) {
            continue;
        }
        if (i.op() == Op::OpSwitch) {
            return {false, "switch"};
        }
        if (i.op() == Op::OpBranch && i.w.size() == 2 && i.w[1] == body) {
            ++predecessors;
        }
        if (i.op() == Op::OpBranchConditional && i.w.size() >= 4 &&
            (i.w[2] == body || i.w[3] == body)) {
            ++predecessors;
            if (i.block == m.first_block && i.w.size() == 4 && i.w[2] == body) {
                branch = &i;
            }
        }
        if (i.block == body) {
            switch (i.op()) {
            case Op::OpLabel:
                break;
            case Op::OpBranch:
                if (i.w.size() != 2 || body_merge) {
                    return {false, "body-exit"};
                }
                body_merge = i.w[1];
                break;
            case Op::OpAtomicSMin:
            case Op::OpAtomicSMax:
                break;
            case Op::OpLoad:
                if (&i != load) {
                    return {false, "extra-load"};
                }
                ++loads;
                pure.push_back(&i);
                break;
            case Op::OpIAdd:
            case Op::OpShiftRightLogical:
                if (i.w.size() != 5) {
                    return {false, "body-expression"};
                }
                pure.push_back(&i);
                break;
            case Op::OpAccessChain:
                if (i.w.size() != 6) {
                    return {false, "body-pointer"};
                }
                pure.push_back(&i);
                break;
            default:
                return {false, "body-effects"};
            }
        }
        if (i.block == m.first_block) {
            if (i.op() == Op::OpSelectionMerge) {
                if (merge) {
                    return {false, "header-merge"};
                }
                merge = &i;
            }
            if (i.op() == Op::OpControlBarrier) {
                if (barrier) {
                    return {false, "header-barriers"};
                }
                barrier = &i;
            }
            if (i.op() == Op::OpStore) {
                if (barrier || i.w.size() != 3) {
                    return {false, "store-after-barrier"};
                }
                stores.push_back(&i);
            }
        }
        // The atomics' return values must not affect any later computation or control flow.
        if (&i != min && &i != max && i.op() != Op::OpLine && i.op() != Op::OpNoLine) {
            for (auto operand : i.w.subspan(1)) {
                if (operand == min->w[2] || operand == max->w[2]) {
                    return {false, "atomic-result-used"};
                }
            }
        }
    }
    if (!branch || !merge || !barrier || loads != 1 || predecessors != 1 || !body_merge ||
        !m.Def(body_merge) || m.Def(body_merge)->op() != Op::OpLabel ||
        m.Def(body_merge)->function != m.entry || merge->w.size() != 3 ||
        merge->w[1] != body_merge || branch->w[3] != body_merge || barrier->w.size() != 4 ||
        m.Constant(barrier->w[1]) != 2 || m.Constant(barrier->w[2]) != 2 ||
        !m.Constant(barrier->w[3]) || ((*m.Constant(barrier->w[3]) & 264u) != 264u)) {
        return {false, "control-or-barrier"};
    }
    const auto* predicate = m.Def(branch->w[1]);
    if (!predicate || predicate->block != m.first_block || predicate->w.size() < 3) {
        return {false, "predicate"};
    }
    // Pin the first converged selection: no calls, extra operations or merge between the
    // retained workgroup barrier, predicate, selection merge and branch.
    if (predicate->op() != Op::OpFOrdNotEqual || predicate->w.size() != 5 ||
        barrier + 1 != predicate || predicate + 1 != merge || merge + 1 != branch) {
        return {false, "header-shape"};
    }
    const U32 bool_type = predicate->w[1];
    const auto* boolean = m.Def(bool_type);
    if (!boolean || boolean->op() != Op::OpTypeBool) {
        return {false, "predicate-type"};
    }
    // Prove each lane initializes exactly the shared word it will read after the retained barrier.
    unsigned own_stores = 0;
    std::unordered_set<U32> addresses;
    for (const auto* s : stores) {
        const auto p = m.SharedPointer(s->w[1]);
        if (!p) {
            return {false, "unknown-initializer"};
        }
        if (p->base == source->base && p->bytes == 4 && m.Eq(p->index, source->index)) {
            ++own_stores;
        }
    }
    if (own_stores != 1) {
        return {false, "source-initialization"};
    }
    for (U32 y = 0; y < 16; ++y) {
        for (U32 x = 0; x < 16; ++x) {
            auto address = m.Eval(source->index, x, y);
            if (!address || *address < 387 || *address > 642 || *address >= source->elements ||
                !addresses.insert(*address).second) {
                return {false, "source-local-index"};
            }
            for (const auto* s : stores) {
                const auto p = *m.SharedPointer(s->w[1]);
                if (p.base == source->base && p.bytes == 4 && m.Eq(p.index, source->index)) {
                    continue;
                }
                auto other = m.Eval(p.index, x, y);
                if (!other ||
                    std::uint64_t(*other) * p.bytes + p.bytes > std::numeric_limits<U32>::max()) {
                    return {false, "initializer-address"};
                }
                const std::uint64_t a = std::uint64_t(*address) * 4,
                                    b = std::uint64_t(*other) * p.bytes;
                if (a < b + p.bytes && b < a + 4) {
                    return {false, "aliased-initializer"};
                }
            }
        }
    }
    // No depth observation before the next retained workgroup barrier. The sole optional
    // store in this interval is the original invalid-pixel flag, not a light/depth consumer.
    bool saw_second_barrier = false;
    for (const auto& i : m.inst) {
        if (i.function != m.entry || i.w.data() <= m.Def(body_merge)->w.data()) {
            continue;
        }
        if (i.op() == Op::OpControlBarrier) {
            saw_second_barrier = i.w.size() == 4 && m.Constant(i.w[1]) == 2 &&
                                 m.Constant(i.w[2]) == 2 && m.Constant(i.w[3]) &&
                                 ((*m.Constant(i.w[3]) & 264u) == 264u);
            break;
        }
        switch (i.op()) {
        case Op::OpLabel:
        case Op::OpLogicalNot:
        case Op::OpSelectionMerge:
        case Op::OpBranchConditional:
        case Op::OpBranch:
        case Op::OpShiftRightLogical:
        case Op::OpAccessChain:
            break;
        case Op::OpStore: {
            if (i.w.size() != 3) {
                return {false, "pre-barrier-store"};
            }
            auto flag = m.SharedPointer(i.w[1]);
            if (!flag || flag->base != source->base || flag->bytes != 4 ||
                m.Eval(flag->index, 0, 0) != 386 || m.Constant(i.w[2]) != 1) {
                return {false, "pre-barrier-store"};
            }
            for (U32 y = 0; y < 16; ++y) {
                for (U32 x = 0; x < 16; ++x) {
                    if (m.Eval(flag->index, x, y) != 386) {
                        return {false, "varying-flag"};
                    }
                }
            }
            break;
        }
        default:
            return {false, "pre-barrier-observation"};
        }
    }
    if (!saw_second_barrier) {
        return {false, "missing-consumer-barrier"};
    }
    U32 bound = words[3];
    std::vector<U32> constants;
    auto constant = [&](U32 n) {
        for (const auto& i : m.inst) {
            if (i.op() == Op::OpConstant && i.w.size() == 4 && i.w[1] == uint_type && i.w[3] == n) {
                return i.w[2];
            }
        }
        const U32 id = bound++;
        Append(constants, Op::OpConstant, {uint_type, id, n});
        return id;
    };
    const U32 scope = constant(3), min_identity = constant(0x7fffffff),
              max_identity = constant(0x80000000);
    const U32 selected_min = bound++, selected_max = bound++, reduced_min = bound++,
              reduced_max = bound++, elect = bound++;
    const U32 has_min = bound++, has_max = bound++, has_values = bound++, selected_leader = bound++;
    std::vector<U32> out(words.begin(), words.begin() + 5);
    out.reserve(words.size() + 128);
    out[3] = bound;
    for (std::size_t n = 0; n < m.inst.size(); ++n) {
        const auto& i = m.inst[n];
        if (n == m.capabilities_end) {
            if (!m.basic) {
                Append(out, Op::OpCapability, {static_cast<U32>(spv::Capability::GroupNonUniform)});
            }
            if (!m.arithmetic) {
                Append(out, Op::OpCapability,
                       {static_cast<U32>(spv::Capability::GroupNonUniformArithmetic)});
            }
        }
        if (n == m.first_function) {
            out.insert(out.end(), constants.begin(), constants.end());
        }
        if (&i == merge) {
            // Entry block: every live invocation reaches both reductions before the first branch.
            for (auto* p : pure) {
                out.insert(out.end(), p->w.begin(), p->w.end());
            }
            Append(out, Op::OpSelect, {uint_type, selected_min, branch->w[1], value, min_identity});
            Append(out, Op::OpSelect, {uint_type, selected_max, branch->w[1], value, max_identity});
            Append(out, Op::OpGroupNonUniformSMin,
                   {uint_type, reduced_min, scope, static_cast<U32>(spv::GroupOperation::Reduce),
                    selected_min});
            Append(out, Op::OpGroupNonUniformSMax,
                   {uint_type, reduced_max, scope, static_cast<U32>(spv::GroupOperation::Reduce),
                    selected_max});
            Append(out, Op::OpGroupNonUniformElect, {bool_type, elect, scope});
            // Preserve the original no-op for all-invalid subgroups; never add neutral atomics.
            Append(out, Op::OpINotEqual, {bool_type, has_min, reduced_min, min_identity});
            Append(out, Op::OpINotEqual, {bool_type, has_max, reduced_max, max_identity});
            Append(out, Op::OpLogicalOr, {bool_type, has_values, has_min, has_max});
            Append(out, Op::OpLogicalAnd, {bool_type, selected_leader, elect, has_values});
        }
        if (std::find(pure.begin(), pure.end(), &i) != pure.end()) {
            continue;
        }
        const auto start = out.size();
        out.insert(out.end(), i.w.begin(), i.w.end());
        if (&i == branch) {
            out[start + 1] = selected_leader;
        }
        if (&i == min) {
            out[start + 6] = reduced_min;
        }
        if (&i == max) {
            out[start + 6] = reduced_max;
        }
    }
    words.swap(out);
    return {true, "subgroup-depth-minmax"};
}
} // namespace Shader::Optimization
