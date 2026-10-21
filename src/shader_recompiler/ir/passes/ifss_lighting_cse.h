// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

namespace Shader::IR {
struct Program;
}

namespace Shader::Optimization {
struct LightingCseResult {
    std::size_t removed{};
    std::size_t cross_block{};
    std::size_t floating{};
    bool budget_reached{};
};

// Exact value reuse only. No memory operations, speculative execution or FP reassociation.
// The caller must also pin the full guest shader bytes before enabling this experiment.
LightingCseResult EliminateIfssLightingExpressions(IR::Program& program);
} // namespace Shader::Optimization
