// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/cache_file.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <limits>
#include <xxhash.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Storage::CacheFile {
namespace {
constexpr std::array<char, 8> Magic{'S', 'H', 'A', 'D', 'C', 'A', '0', '1'};
constexpr u64 HeaderBytes = 24;
std::atomic<u64> next_file{};
} // namespace

bool Read(const std::filesystem::path& path, std::vector<u8>& bytes, u64 limit, bool allow_legacy) {
    bytes.clear();
    std::error_code ec;
    const u64 size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || (size > HeaderBytes && size - HeaderBytes > limit) ||
        size > std::numeric_limits<size_t>::max()) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    std::array<u8, HeaderBytes> header{};
    const auto header_size = std::min<u64>(size, HeaderBytes);
    if (!file.read(reinterpret_cast<char*>(header.data()), header_size)) {
        return false;
    }
    if (size >= Magic.size() && size < HeaderBytes &&
        std::memcmp(header.data(), Magic.data(), Magic.size()) == 0)
        return false;
    const bool enveloped =
        size >= HeaderBytes && std::memcmp(header.data(), Magic.data(), Magic.size()) == 0;
    u64 payload_size = size;
    u64 checksum{};
    if (enveloped) {
        std::memcpy(&payload_size, header.data() + 8, sizeof(payload_size));
        std::memcpy(&checksum, header.data() + 16, sizeof(checksum));
        if (payload_size == 0 || payload_size > limit || payload_size != size - HeaderBytes) {
            return false;
        }
    } else {
        if (!allow_legacy || size > limit) {
            return false;
        }
        file.clear();
        file.seekg(0);
    }
    bytes.resize(payload_size);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), payload_size) ||
        (enveloped && XXH3_64bits(bytes.data(), bytes.size()) != checksum)) {
        bytes.clear();
        return false;
    }
    return true;
}

bool Write(const std::filesystem::path& path, std::span<const u8> bytes) {
    if (bytes.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    auto pending = path;
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    pending += ".pending." + std::to_string(pid) + "." +
               std::to_string(next_file.fetch_add(1, std::memory_order_relaxed));
    std::array<u8, HeaderBytes> header{};
    std::memcpy(header.data(), Magic.data(), Magic.size());
    const u64 size = bytes.size();
    const u64 checksum = XXH3_64bits(bytes.data(), bytes.size());
    std::memcpy(header.data() + 8, &size, sizeof(size));
    std::memcpy(header.data() + 16, &checksum, sizeof(checksum));
    {
        std::ofstream file(pending, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(header.data()), header.size());
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        file.close();
        if (!file) {
            std::filesystem::remove(pending, ec);
            return false;
        }
    }
#ifdef _WIN32
    if (MoveFileExW(pending.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
#else
    std::filesystem::rename(pending, path, ec);
    if (!ec) {
        return true;
    }
#endif
    std::filesystem::remove(pending, ec);
    return false;
}
} // namespace Storage::CacheFile
