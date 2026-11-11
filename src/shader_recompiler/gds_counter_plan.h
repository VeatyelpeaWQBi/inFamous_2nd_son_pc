// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <span>
#include "common/types.h"

namespace Shader {
struct Info;

// Derived from the actual module, never serialized or used to change its instructions.
// Supports only a single uniform 32-bit append counter whose address is:
// (((flat[word] >> 16) + 12) >> 2) * 4. Every other GDS access stays conservative.
struct GdsCounterPlan {
    bool observed{};
    std::optional<u32> flat_word;

    void Observe(std::optional<u32> next) {
        if (!observed) flat_word = next;
        else if (flat_word != next) flat_word.reset();
        observed = true; // An unqualified permutation permanently disables this program.
    }

    std::optional<u32> Offset(std::span<const u32> flat, u64 buffer_size) const {
        if (!flat_word || *flat_word >= flat.size()) return {};
        const u32 offset = ((flat[*flat_word] >> 16) + 12u) & ~3u;
        if (buffer_size < sizeof(u32) || offset > buffer_size - sizeof(u32)) return {};
        return offset;
    }
};

std::optional<u32> AnalyzeGdsCounterPlan(std::span<const u32> spv, u32 gds_binding,
                                       u32 flat_binding);
void ObserveGdsCounterPlan(Info& info, std::span<const u32> spv, bool patched = false);
} // namespace Shader
