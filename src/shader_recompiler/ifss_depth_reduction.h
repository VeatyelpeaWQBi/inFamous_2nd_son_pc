// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace Shader::Optimization {
inline constexpr std::uint64_t IfssLightingHash = 0xce1675e0ULL;
inline constexpr std::uint32_t IfssDepthReductionBit = 4u;

// Keep unrelated cached shaders on their existing binary version.
constexpr std::uint32_t IfssShaderVariant(std::uint32_t mask, std::uint64_t hash, bool compute) {
    return compute && hash == IfssLightingHash ? mask : mask & ~IfssDepthReductionBit;
}
inline bool IsIfssDepthShader(std::span<const std::uint32_t> code) {
    if (code.size_bytes() != 7312) {
        return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto word : code) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash = (hash ^ ((word >> shift) & 255u)) * 1099511628211ULL;
        }
    }
    return hash == 0xc8dbb17d6de5a42aULL;
}
struct DepthReductionResult {
    bool applied{};
    const char* reason{};
};

// Caller must pin the guest program above and compute subgroup BASIC+ARITHMETIC support.
// Transactional: unmatched or unsupported SPIR-V leaves the input byte-for-byte unchanged.
DepthReductionResult ReduceIfssDepthAtomics(std::vector<std::uint32_t>& words);
} // namespace Shader::Optimization
