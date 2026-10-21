// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/consumer_evidence.h"
#include "common/auxiliary_evidence.h"
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/logging/log.h"
#include "common/path_trace.h"
#include "common/perf_stats.h"

namespace Common::ConsumerEvidence {
namespace {
std::atomic<bool> enabled{false};
Word image_base{}, image_size{}, after_ns{};
unsigned sample_limit{}, delay_s{};
std::array<std::atomic<unsigned>, Sites.size()> used{};
std::atomic<Word> wrong_address{}, changed_code{};
thread_local const Snapshot* current{};
}
void Configure(std::string_view title, std::string_view version, std::string_view sha,
               Word base, std::size_t size, unsigned limit, unsigned delay_seconds) {
    // Called once by the loader, before guest execution; no resets during hot reload.
    if (!limit || !PathTrace::Active()) return;
    const auto image = std::span{reinterpret_cast<const std::uint8_t*>(base), size};
    const bool pinned = Verified(title, version, sha) && base && size &&
        std::all_of(Sites.begin(), Sites.end(), [&](const Site& site) {
            return MatchesSite(image,static_cast<unsigned>(&site-Sites.data()));
        });
    if (pinned) {
        image_base = base; image_size = size; sample_limit = std::min(limit, 1024u);
        delay_s = std::min(delay_seconds, 600u);
        after_ns = Perf::NowNs() + Word{delay_s} * 1'000'000'000;
        enabled.store(true, std::memory_order_release);
    }
    LOG_INFO(Debug, "R13 consumer evidence: pinned={} base={:x} limit={} per site delay={}s",
             pinned, base, pinned ? sample_limit : 0, delay_seconds);
}
FaultScope::FaultScope(void* context, Word rip, Word address, Word size, bool gpu_thread) noexcept {
    if (!PathTrace::Active() || !enabled.load(std::memory_order_acquire) || current || gpu_thread ||
        rip < image_base || Perf::NowNs() < after_ns) return;
    const Word rva = rip - image_base;
    const auto found = std::find_if(Sites.begin(), Sites.end(), [rva](const Site& site) { return site.rva == rva; });
    if (found == Sites.end()) return;
    const auto index = static_cast<unsigned>(found - Sites.begin());
    if (used[index].load(std::memory_order_relaxed) >= sample_limit) return;
    if (!MatchesSite({reinterpret_cast<const std::uint8_t*>(image_base), image_size}, index)) {
        changed_code.fetch_add(1, std::memory_order_relaxed); return;
    }
#ifdef _WIN32
    const auto* c = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord;
    snapshot.regs = {c->Rax,c->Rcx,c->Rdx,c->Rbx,c->Rsi,c->Rdi,c->R15,c->R14,c->Rsp,c->Rbp,c->R13,c->R12};
#else
    return; // This project captures native Windows x86-64 contexts only.
#endif
    if (size < 4 || OperandAddress(index, snapshot.regs) != address) {
        wrong_address.fetch_add(1, std::memory_order_relaxed); return;
    }
    const auto ordinal = Reserve(used[index], sample_limit);
    if (!ordinal) return;
    snapshot.rip = rip; snapshot.address = address; snapshot.size = size;
    snapshot.base = image_base; snapshot.rva = rva; snapshot.ordinal = ordinal;
    snapshot.width = found->width;
    current = &snapshot; captured = true;
}
FaultScope::~FaultScope() { if (captured) current = nullptr; }
const Snapshot* Current(Word rip, Word address, Word size) noexcept {
    return current && current->rip == rip && current->address == address && current->size == size ? current : nullptr;
}
void EmitSnapshot(Word id, const Snapshot& s) noexcept {
    using PathTrace::Emit; using PathTrace::Kind;
    Emit(Kind::ReadConsumer, id, s.rva, s.width, s.ordinal, s.base);
    Emit(Kind::ReadConsumerRegs0, id, s.regs[Rax],s.regs[Rcx],s.regs[Rdx],s.regs[Rbx]);
    Emit(Kind::ReadConsumerRegs1, id, s.regs[Rsi],s.regs[Rdi],s.regs[R15],s.regs[R14]);
    Emit(Kind::ReadConsumerRegs2, id, s.regs[Rsp],s.regs[Rbp],s.regs[R13],s.regs[R12]);
}
void WriteMetadata(std::ostream& out) {
    const bool active = enabled.load(std::memory_order_acquire);
    out << "{\"enabled\":" << (active ? "true" : "false") << ",\"base\":" << (active ? image_base : 0)
        << ",\"source_prefix_checked\":" << (active ? "true" : "false")
        << ",\"limit_per_site\":" << (active ? sample_limit : 0) << ",\"delay_seconds\":" << (active ? delay_s : 0)
        << ",\"wrong_operand_address\":" << wrong_address.load() << ",\"changed_instruction\":" << changed_code.load()
        << ",\"site_counts\":[";
    for (std::size_t i = 0; i < used.size(); ++i) out << (i ? "," : "") << used[i].load();
    out << "],\"site_rvas\":[";
    for (std::size_t i = 0; i < Sites.size(); ++i) out << (i ? "," : "") << Sites[i].rva;
    out << "]}";
}
} // namespace Common::ConsumerEvidence

namespace Common::AuxiliaryEvidence {
namespace {
std::atomic<bool> enabled{false};
std::atomic<unsigned> contexts{}, omitted_packets{};
Word after_ns{};
unsigned context_limit{}, delay_s{};
}
void Configure(std::string_view title, std::string_view version, std::string_view sha,
               unsigned limit, unsigned delay_seconds) {
    if (!limit || !PathTrace::Active() || !ConsumerEvidence::Verified(title, version, sha)) return;
    context_limit = std::min(limit, 1024u);
    delay_s = std::min(delay_seconds, 600u);
    after_ns = Perf::NowNs() + Word{delay_s} * 1'000'000'000;
    enabled.store(true, std::memory_order_release);
    LOG_INFO(Debug, "Auxiliary command evidence pinned: contexts={} packets_per_context={} delay={}s",
             context_limit, MaxPackets, delay_s);
}
Context Begin(Word queue, Word base, Word bytes, Word parent) noexcept {
    if (!enabled.load(std::memory_order_acquire) || !PathTrace::Active() ||
        !bytes || (bytes & 3) || queue >= 56 || base > UINT64_MAX - bytes ||
        contexts.load(std::memory_order_relaxed) >= context_limit || Perf::NowNs() < after_ns ||
        !ConsumerEvidence::Reserve(contexts, context_limit)) return {};
    Context result{ConsumerEvidence::Tag(PathTrace::NextId(), true)};
    PathTrace::Emit(PathTrace::Kind::AuxBegin, result.id, queue, base, bytes, parent);
    return result;
}
void Packet(Context& context, unsigned kind, Word address, bool buffered,
            std::span<const std::uint32_t> words) noexcept {
    if (!context.id) return;
    const unsigned expected = kind == 3 ? 4 : 7;
    if (!ValidWords(kind, words) || context.packets == MaxPackets) {
        if (context.omitted != UINT32_MAX) ++context.omitted;
        omitted_packets.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto id = context.id;
    PathTrace::Emit(PathTrace::Kind::AuxPacket, id, kind, ++context.packets, address, buffered);
    PathTrace::Emit(PathTrace::Kind::AuxWords, id,
                    Pack(words[0], words[1]), Pack(words[2], words[3]),
                    expected == 7 ? Pack(words[4], words[5]) : 0,
                    Pack(expected == 7 ? words[6] : 0, expected));
}
void End(const Context& context, bool finished, bool consumed, bool indirect) noexcept {
    PathTrace::Emit(PathTrace::Kind::AuxEnd, context.id, context.packets, context.omitted,
                    finished, (consumed ? 1 : 0) | (indirect ? 2 : 0));
}
void WriteMetadata(std::ostream& output) {
    output << "{\"enabled\":" << (enabled.load(std::memory_order_acquire) ? "true" : "false")
           << ",\"context_limit\":" << context_limit << ",\"contexts\":" << contexts.load()
           << ",\"packets_per_context\":" << MaxPackets << ",\"omitted_packets\":" << omitted_packets.load()
           << ",\"delay_seconds\":" << delay_s
           << ",\"scope\":\"compute DMA/RELEASE_MEM/IB metadata; no complete CPU consumption proof\"}";
}
} // namespace Common::AuxiliaryEvidence
