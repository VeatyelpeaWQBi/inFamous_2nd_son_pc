// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
namespace Shader::IR { struct Program; }
namespace Shader::Optimization {
// Caller pins the complete guest binary. Only four retained words from ONE original
// scalar-memory instruction may be combined, with no intervening memory operation.
std::size_t PackIfssLightingReads(IR::Program& program);
}
