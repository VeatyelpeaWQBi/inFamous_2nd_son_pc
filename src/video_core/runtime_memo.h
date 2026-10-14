// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace VideoCore {

/// Owned, bounded inputs for a runtime lookup. Equality checks the actual bytes rather than
/// trusting a hash, a guest address, or a global write generation. Command processor only.
template <std::size_t MaxBytes>
class RuntimeSnapshot {
public:
    bool Matches(std::span<const std::byte> input) const noexcept {
        return valid && input.size() == bytes.size() &&
               (input.empty() || std::memcmp(input.data(), bytes.data(), input.size()) == 0);
    }

    bool Capture(std::span<const std::byte> input) {
        if (input.size() > MaxBytes) {
            Invalidate();
            return false;
        }
        if (input.size() > bytes.capacity()) {
            // Do not let geometric vector growth reserve more than the snapshot limit.
            std::vector<std::byte> owned(input.begin(), input.end());
            bytes.swap(owned);
        } else {
            bytes.resize(input.size());
            if (!input.empty()) {
                std::memcpy(bytes.data(), input.data(), input.size());
            }
        }
        valid = true;
        return true;
    }

    void Invalidate() noexcept {
        valid = false;
        bytes.clear();
    }

    std::size_t Size() const noexcept {
        return bytes.size();
    }

    std::span<const std::byte> Data() const noexcept {
        return bytes;
    }

private:
    std::vector<std::byte> bytes;
    bool valid{};
};

/// A read-only upload is reusable only in the same stream allocation generation and GPU
/// submission. A ring wrap or queue switch must never return an overwritten offset.
template <std::size_t MaxBytes>
class RuntimeUploadMemo {
public:
    bool Matches(std::span<const std::byte> input, const void* stream, std::uint64_t tick,
                 std::uint64_t generation) const noexcept {
        return owner == stream && submit_tick == tick && allocation_generation == generation &&
               contents.Matches(input);
    }

    void Capture(std::span<const std::byte> input, const void* stream, std::uint64_t tick,
                 std::uint64_t generation, std::uint64_t offset) {
        contents.Capture(input);
        owner = stream;
        submit_tick = tick;
        allocation_generation = generation;
        upload_offset = offset;
    }

    std::uint64_t Offset() const noexcept {
        return upload_offset;
    }

private:
    RuntimeSnapshot<MaxBytes> contents;
    const void* owner{};
    std::uint64_t submit_tick{};
    std::uint64_t allocation_generation{};
    std::uint64_t upload_offset{};
};

} // namespace VideoCore
