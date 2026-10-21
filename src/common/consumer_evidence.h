// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <ostream>
#include <string_view>
#include "common/consumer_evidence_policy.h"

namespace Common::ConsumerEvidence {
struct Snapshot {
    Word rip, address, size, base, rva, ordinal, width;
    std::array<Word, NumRegisters> regs;
};
void Configure(std::string_view title, std::string_view version, std::string_view sha,
               Word base, std::size_t size, unsigned limit, unsigned delay_seconds);
class FaultScope {
public:
    FaultScope(void* context, Word rip, Word address, Word size, bool gpu_thread) noexcept;
    ~FaultScope();
    FaultScope(const FaultScope&) = delete;
    FaultScope& operator=(const FaultScope&) = delete;
private:
    Snapshot snapshot; // Not initialized or read when disabled.
    bool captured{false};
};
const Snapshot* Current(Word rip, Word address, Word size) noexcept;
void EmitSnapshot(Word id, const Snapshot& snapshot) noexcept;
void WriteMetadata(std::ostream& output);
} // namespace Common::ConsumerEvidence
