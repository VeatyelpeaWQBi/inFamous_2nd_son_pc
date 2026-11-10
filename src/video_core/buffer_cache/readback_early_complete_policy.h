// SPDX-FileCopyrightText: Copyright 2026 inFamous_2nd_son_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace VideoCore {

/// O01 early-complete eligibility. Conservative: only precise, demand-sized guest reads
/// (one page or less). Larger conservative windows and writes stay on the ordinary Flush.
/// A learned-counter map is not wired here; when one exists it can be OR'd with this gate.
inline bool EarlyCompleteDemandEligible(bool enabled, bool is_write, std::uint64_t size,
                                        std::uint64_t page_size, bool precise) {
    return enabled && precise && !is_write && page_size != 0 && size > 0 && size <= page_size;
}

} // namespace VideoCore
