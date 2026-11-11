// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <spirv/unified1/spirv.hpp11>
#include "shader_recompiler/gds_counter_plan.h"

namespace {
void Check(bool good, const char* message) {
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
std::vector<u32> Read(const char* path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    Check(bool(file), "open retained shader fixture");
    const auto size = static_cast<size_t>(file.tellg());
    Check(size >= 20 && size < 16*1024*1024 && size % 4 == 0, "fixture size");
    std::vector<u32> words(size/4);
    file.seekg(0);
    Check(bool(file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size))),
          "fixture read");
    if (std::memcmp(words.data(), "SHADCA01", 8) == 0) {
        u64 payload{};
        std::memcpy(&payload, words.data()+2, sizeof(payload));
        Check(size >= 44 && payload == size-24, "cache envelope length");
        words.erase(words.begin(), words.begin()+6);
    }
    Check(words[0] == spv::MagicNumber, "SPIR-V magic");
    return words;
}
u32 NamedId(const std::vector<u32>& words, const char* name) {
    u32 id{};
    for (size_t pos=5; pos<words.size(); pos+=words[pos]>>16) {
        const u32 count = words[pos]>>16;
        Check(count && count<=words.size()-pos, "instruction length");
        if ((words[pos]&0xffff)==u32(spv::Op::OpName) && count>=3 &&
            std::strlen(name)+1 <= (count-2)*4 &&
            std::memcmp(words.data()+pos+2,name,std::strlen(name)+1)==0) id=words[pos+1];
    }
    Check(id!=0, "named fixture descriptor");
    return id;
}
u32 Binding(const std::vector<u32>& words, const char* name) {
    const u32 id=NamedId(words,name);
    for (size_t pos=5; pos<words.size(); pos+=words[pos]>>16) {
        if ((words[pos]&0xffff)==u32(spv::Op::OpDecorate) && (words[pos]>>16)==4 &&
            words[pos+1]==id && words[pos+2]==u32(spv::Decoration::Binding)) return words[pos+3];
    }
    Check(false,"fixture binding decoration");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    Shader::GdsCounterPlan plan;
    std::array<u32,16> flat{};
    Check(!plan.Offset(flat,65536),"unknown plan never narrows");
    plan.Observe(14);
    flat[14] = 0x12370000;
    Check(plan.Offset(flat,65536)==std::optional<u32>{0x1240},"matches shift/add/round-down");
    Check(!plan.Offset(std::span{flat}.first(14),65536),"missing table word falls back");
    Check(!plan.Offset(flat,3),"short buffer falls back");
    flat[14]=0xffff0000;
    Check(!plan.Offset(flat,65536),"overflow beyond GDS falls back");
    flat[14]=0xfff00000;
    Check(plan.Offset(flat,65536)==std::optional<u32>{65532},"last full dword allowed");
    plan.Observe(14);
    Check(plan.flat_word==std::optional<u32>{14},"matching permutation retains plan");
    plan.Observe(13);
    Check(!plan.flat_word,"different permutation invalidates plan");
    plan.Observe(14);
    Check(!plan.flat_word,"invalidation is absorbing");
    Shader::GdsCounterPlan unknown;
    unknown.Observe({}); unknown.Observe(14);
    Check(!unknown.flat_word,"unknown permutation cannot revive a plan");

    Check(argc>1,"provide existing qualified SPIR-V cache fixtures");
    for (int i=1;i<argc;++i) {
        const auto words = Read(argv[i]);
        const u32 gds=Binding(words,"gds_buffer"), flat_binding=Binding(words,"srt_flatbuf");
        Check(Shader::AnalyzeGdsCounterPlan(words,gds,flat_binding)==std::optional<u32>{14},
              "retained actual module qualifies");
        auto changed=words;
        bool mutated=false;
        for (size_t pos=5;pos<changed.size();pos+=changed[pos]>>16) {
            if ((changed[pos]&0xffff)==u32(spv::Op::OpAtomicIAdd)) {
                changed[pos]=(changed[pos]&0xffff0000)|u32(spv::Op::OpAtomicExchange);
                mutated=true;
            }
        }
        Check(mutated && !Shader::AnalyzeGdsCounterPlan(changed,gds,flat_binding),
              "unsupported atomic/pointer consumer rejects whole module");

        changed=words;
        for (size_t pos=5;pos<changed.size();pos+=changed[pos]>>16) {
            if ((changed[pos]&0xffff)==u32(spv::Op::OpMemoryModel)) {
                changed[pos+1]=u32(spv::AddressingModel::PhysicalStorageBuffer64);
                break;
            }
        }
        Check(!Shader::AnalyzeGdsCounterPlan(changed,gds,flat_binding),
              "physical addressing with unproven aliases rejects");

        // An explicit pointer escape is rejected even when the copied pointer is unused.
        // Keep all original accesses, rather than merely removing the recognized atomic.
        changed=words;
        const u32 root=NamedId(words,"gds_buffer");
        mutated=false;
        for (size_t pos=5;pos<changed.size();pos+=changed[pos]>>16) {
            const u32 count=changed[pos]>>16;
            if ((changed[pos]&0xffff)==u32(spv::Op::OpAccessChain) && count==6 && changed[pos+3]==root) {
                const u32 new_id=changed[3]++;
                const std::array<u32,4> copy{(4u<<16)|u32(spv::Op::OpCopyObject),
                    changed[pos+1],new_id,changed[pos+2]};
                changed.insert(changed.begin()+pos+count,copy.begin(),copy.end());
                mutated=true;
                break;
            }
        }
        Check(mutated && !Shader::AnalyzeGdsCounterPlan(changed,gds,flat_binding),
              "GDS pointer copy invalidates the proof");

        changed=words;
        const u32 flat_root=NamedId(words,"srt_flatbuf");
        u32 uint_type{},zero{};
        for (size_t pos=5;pos<words.size();pos+=words[pos]>>16) {
            if ((words[pos]&0xffff)==u32(spv::Op::OpTypeInt) && (words[pos]>>16)==4 &&
                words[pos+2]==32 && words[pos+3]==0) { uint_type=words[pos+1]; break; }
        }
        Check(uint_type!=0,"retained fixture u32 type");
        for (size_t pos=5;pos<words.size();pos+=words[pos]>>16) {
            if ((words[pos]&0xffff)==u32(spv::Op::OpConstant) && (words[pos]>>16)==4 &&
                words[pos+1]==uint_type && words[pos+3]==0) { zero=words[pos+2]; break; }
        }
        Check(zero!=0,"retained fixture zero constant");
        mutated=false;
        for (size_t pos=5;pos<changed.size();pos+=changed[pos]>>16) {
            const auto op=static_cast<spv::Op>(changed[pos]&0xffff);
            const u32 count=changed[pos]>>16;
            if ((op==spv::Op::OpAccessChain || op==spv::Op::OpRawAccessChainNV) &&
                count>=6 && changed[pos+3]==flat_root) {
                const std::array<u32,3> store{(3u<<16)|u32(spv::Op::OpStore),changed[pos+2],zero};
                changed.insert(changed.begin()+pos+count,store.begin(),store.end());
                mutated=true;
                break;
            }
        }
        Check(mutated && !Shader::AnalyzeGdsCounterPlan(changed,gds,flat_binding),
              "mutable address table invalidates the proof");
        changed=words;
        changed.back()=0;
        Check(!Shader::AnalyzeGdsCounterPlan(changed,gds,flat_binding),"malformed instruction rejects");
        Check(!Shader::AnalyzeGdsCounterPlan(words,gds,gds),"aliased table and GDS reject");
    }
    std::puts("GDS proof, bounds, permutation and retained-module checks passed.");
}
