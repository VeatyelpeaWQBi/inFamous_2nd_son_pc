// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "video_core/buffer_cache/writer_footprint.h"

namespace {
void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
} // namespace

int main() {
    using VideoCore::EvaluateWriterFootprint;
    constexpr VAddr base = 0x100000;
    constexpr u32 bind = 0x10000;
    const std::vector<u32> flat{0, 0x40, 0x80};

    Require(!EvaluateWriterFootprint(0, 0, 4, 0, flat, base, bind, false,
                                     false),
            "unknown plan must fall back");
    Require(!EvaluateWriterFootprint(1, 16, 20, 0, flat, base, bind, true,
                                     false),
            "add_tid must fall back");
    Require(!EvaluateWriterFootprint(1, 16, 20, 0, flat, base, bind, false,
                                     true),
            "swizzle must fall back");

    auto proven = EvaluateWriterFootprint(1, 16, 20, 0, flat, base, bind,
                                          false, false);
    Require(proven && proven->address == base + 16 && proven->size == 4,
            "immediate 4-byte store at +16");

    proven = EvaluateWriterFootprint(2, 0, 4, 1, flat, base, bind,
                                     false, false);
    Require(proven && proven->address == base + 0x40 && proven->size == 4,
            "flatbuf soffset dword 1 plus 4-byte store");

    Require(!EvaluateWriterFootprint(2, 0, 4, 8, flat, base, bind,
                                     false, false),
            "missing flatbuf dword must fall back");

    proven = EvaluateWriterFootprint(1, bind - 2, bind + 8, 0, flat, base,
                                     bind, false, false);
    Require(proven && proven->address == base + bind - 2 && proven->size == 2,
            "range overlapping the V# end is clamped");

    Require(!EvaluateWriterFootprint(1, bind, bind + 4, 0, flat, base,
                                     bind, false, false),
            "start at bind size must fall back");

    return 0;
}
