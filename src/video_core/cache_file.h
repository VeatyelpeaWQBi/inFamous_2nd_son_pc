// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "common/types.h"

namespace Storage::CacheFile {

constexpr u64 MaxBlobBytes = 256ULL * 1024 * 1024;
// New files are checksummed envelopes. Legacy reads are explicitly allowed only when
// importing the original cache; older emulators never see the new cache directory.
bool Read(const std::filesystem::path& path, std::vector<u8>& bytes, u64 limit = MaxBlobBytes,
          bool allow_legacy = false);
bool Write(const std::filesystem::path& path, std::span<const u8> bytes);

} // namespace Storage::CacheFile
