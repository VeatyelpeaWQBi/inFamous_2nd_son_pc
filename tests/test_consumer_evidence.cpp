// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include "common/consumer_evidence_policy.h"
#include "common/path_trace.h"
namespace {
void Check(bool value, const char* why) {
    if (!value) { std::fprintf(stderr, "Consumer evidence: %s\n", why); std::exit(1); }
}
}
int main() {
    using namespace Common::ConsumerEvidence;
    Check(Verified("CUSA00309","01.07",Fingerprint),"pinned title/version/full program");
    Check(!Verified("CUSA00004","01.07",Fingerprint) && !Verified("CUSA00309","01.06",Fingerprint) &&
          !Verified("CUSA00309","01.07",Fingerprint.substr(1)),"unknown programs excluded");
    const Site site{2,{0x8b,0x40,4},3};
    std::array<std::uint8_t,5> image{0,0,0x8b,0x40,4};
    Check(Matches(image,site) && !Matches(std::span{image}.first(4),site),"full original instruction required");
    image[4]=5; Check(!Matches(image,site),"changed process instruction rejected");
    std::vector<std::uint8_t> mapped(Sites[0].rva+Sites[0].length);
    std::copy_n(Sites[0].bytes.begin(),Sites[0].length,mapped.begin()+Sites[0].rva);
    std::copy_n(SourceLoads[0].bytes.begin(),SourceLoads[0].length,mapped.begin()+SourceLoads[0].rva);
    Check(MatchesSite(mapped,0),"dominating pointer load and intervening code are pinned");
    mapped[SourceLoads[0].rva+3]=0xf8;
    Check(!MatchesSite(mapped,0),"unchanged fault opcode cannot hide a changed object field load");
    Check(!MatchesSite(mapped,99),"unknown instruction index rejected");
    std::vector<std::uint8_t> record(RecordCopies[1].rva+RecordCopies[1].length);
    std::copy_n(RecordCopies[1].bytes.begin(),RecordCopies[1].length,record.begin()+RecordCopies[1].rva);
    Check(MatchesSite(record,7) && Sites[7].width==8,"QWORD record load/store/advance guard");
    record[RecordCopies[1].rva+6]=0x55;
    Check(!MatchesSite(record,7),"changed CPU destination store rejects the record inference");
    std::array<Word,NumRegisters> regs{};
    regs[Rax]=~Word{1}; regs[R15]=8; regs[Rcx]=32;
    Check(OperandAddress(0,regs)==2 && OperandAddress(1,regs)==38 &&
          OperandAddress(2,regs)==6 && OperandAddress(3,regs)==14 &&
          OperandAddress(4,regs)==10 && OperandAddress(5,regs)==16,"native modulo64 effective addresses");
    Check(OperandAddress(6,regs)==40 && OperandAddress(7,regs)==32,"record-copy effective addresses");
    Check(Tag(0,true)==0 && Tag(7,false)==7 && Tag(7,true)==(ConsumerIdBit|7),"no fabricated or colliding disabled IDs");
    using Common::PathTrace::ConsumerTraceKind; using Common::PathTrace::Kind;
    Check(ConsumerTraceKind(Kind::ReadRequest) && ConsumerTraceKind(Kind::ReadReturn) &&
          ConsumerTraceKind(Kind::ReadProducerDomain) && ConsumerTraceKind(Kind::ReadConsumerRegs2),"request closure retained");
    Check(!ConsumerTraceKind(Kind::FrameSample) && !ConsumerTraceKind(Kind::CopyRecord) &&
          !ConsumerTraceKind(Kind::SubmitQueued) && !ConsumerTraceKind(Kind::FeedbackFence),"unrelated events excluded even for high hash IDs");
    std::atomic<unsigned> used{}, accepted{};
    std::array<std::atomic<unsigned>,65> ordinals{};
    std::vector<std::thread> workers;
    for (unsigned i=0;i<4;++i) workers.emplace_back([&] {
        for (unsigned n=0;n<1000;++n) if (auto ordinal=Reserve(used,64)) {
            accepted.fetch_add(1); ordinals[ordinal].fetch_add(1);
        }
    });
    for (auto& worker:workers) worker.join();
    Check(used==64 && accepted==64 && !Reserve(used,64),"lifetime budget saturates under concurrent faults");
    for (unsigned i=1;i<=64;++i) Check(ordinals[i]==1,"ordinals unique across threads");
    std::puts("Consumer pinning, instruction bounds, effective addresses, request filtering and concurrent budget checks passed.");
}
