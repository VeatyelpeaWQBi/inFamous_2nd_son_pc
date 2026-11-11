// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>
#include <optional>
#include <span>

#include "common/types.h"

namespace VideoCore {

/// Guest range a producer was proven to write. Empty optional means "use the full descriptor".
struct WrittenRange {
    VAddr address{};
    u32 size{};
};

/// Evaluates a compile-time write plan against the bind-time V# and flatbuf.
/// add_tid / swizzle are V# bits that invalidate a constant-offset proof.
/// mode matches Shader::WriteFootprintMode (0 Unknown, 1 Immediate, 2 SoffsetUserData).
[[nodiscard]] inline std::optional<WrittenRange> EvaluateWriterFootprint(
    u8 mode, u32 imm_offset, u32 end, u16 soffset_dw,
    std::span<const u32> flatbuf, VAddr base, u32 bind_size, bool add_tid,
    bool swizzle) noexcept {
    if (mode == 0 || add_tid || swizzle || bind_size == 0 ||
        end <= imm_offset) {
        return std::nullopt;
    }
    u64 rel0 = imm_offset;
    u64 rel1 = end;
    if (mode == 2) {
        if (soffset_dw >= flatbuf.size()) {
            return std::nullopt;
        }
        const u64 scalar = flatbuf[soffset_dw];
        // A shared scalar term must preserve alignment for each contributing
        // store, including 64-bit stores.
        if (scalar % 8 != 0) {
            return std::nullopt;
        }
        rel0 += scalar;
        rel1 += scalar;
    } else if (mode != 1) {
        return std::nullopt;
    }
    if (rel0 >= bind_size || rel1 > bind_size ||
        static_cast<u64>(base) > std::numeric_limits<u64>::max() - rel1) {
        return std::nullopt;
    }
    return WrittenRange{static_cast<VAddr>(static_cast<u64>(base) + rel0),
                        static_cast<u32>(rel1 - rel0)};
}

} // namespace VideoCore
