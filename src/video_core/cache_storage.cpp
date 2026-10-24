// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/cache_storage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <set>
#include <miniz.h>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "video_core/cache_file.h"
#include "video_core/cache_writer.h"

namespace Storage {
namespace {
const char* Extension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta:
        return ".meta";
    case BlobType::ShaderBinary:
        return ".spv";
    case BlobType::PipelineKey:
        return ".key";
    case BlobType::ShaderProfile:
        return ".bin";
    }
    return ".invalid";
}
std::string BlobName(BlobType type, const std::string& name) {
    return std::filesystem::path{name}.replace_extension(Extension(type)).filename().string();
}
} // namespace

struct DataBase::Impl {
    std::atomic<bool> opened{};
    std::filesystem::path path, legacy;
    mz_zip_archive legacy_zip{};
    bool zip_open{}, allow_legacy{};
    u64 write_budget = 128_MB;
    std::unique_ptr<AsyncCacheWriter> writer;

    void CloseLegacy() {
        if (zip_open) {
            mz_zip_reader_end(&legacy_zip);
            zip_open = false;
        }
        allow_legacy = false;
    }

    bool Enqueue(const std::string& name, std::vector<u8>&& data) {
        if (!opened.load(std::memory_order_acquire) || data.empty() ||
            data.size() > CacheFile::MaxBlobBytes)
            return false;
        return writer->Enqueue(name, std::move(data));
    }

    bool Read(const std::string& name, std::vector<u8>& data) {
        data.clear();
        if (CacheFile::Read(path / name, data))
            return true;
        if (!allow_legacy)
            return false;
        bool found = CacheFile::Read(legacy / name, data, CacheFile::MaxBlobBytes, true);
        if (!found && zip_open) {
            const int index = mz_zip_reader_locate_file(&legacy_zip, name.c_str(), nullptr, 0);
            mz_zip_archive_file_stat stat{};
            if (index >= 0 && mz_zip_reader_file_stat(&legacy_zip, index, &stat) &&
                stat.m_uncomp_size > 0 && stat.m_uncomp_size <= CacheFile::MaxBlobBytes) {
                data.resize(stat.m_uncomp_size);
                found =
                    mz_zip_reader_extract_to_mem(&legacy_zip, index, data.data(), data.size(), 0);
                if (!found)
                    data.clear();
            }
        }
        if (found)
            Enqueue(name, std::vector<u8>{data}); // One-time, read-only legacy import.
        return found;
    }
};

DataBase::DataBase() : impl{std::make_unique<Impl>()} {}
DataBase::~DataBase() {
    Close();
}
bool DataBase::IsOpened() const {
    return impl->opened.load(std::memory_order_acquire);
}
void DataBase::SetWriteBudget(u64 bytes) {
    impl->write_budget = std::clamp<u64>(bytes, 16ULL * 1024 * 1024, 512ULL * 1024 * 1024);
    if (impl->writer)
        impl->writer->SetBudget(impl->write_budget);
}

void DataBase::Open() {
    if (IsOpened())
        return;
    impl->legacy = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                   Common::ElfInfo::Instance().GameSerial();
    impl->path = impl->legacy / "pipeline-v2";
    std::error_code ec;
    std::filesystem::create_directories(impl->path, ec);
    if (ec) {
        LOG_ERROR(Render, "Cannot create pipeline cache directory: {}", ec.message());
        return;
    }
    std::vector<u8> marker;
    impl->allow_legacy = !CacheFile::Read(impl->path / "legacy-import-complete", marker, 1) ||
                         marker != std::vector<u8>{1} ||
                         !std::filesystem::exists(impl->path / "profile.bin");
    if (impl->allow_legacy) {
        auto archive = impl->legacy;
        archive.replace_extension(".zip");
        impl->zip_open = mz_zip_reader_init_file(&impl->legacy_zip, archive.string().c_str(), 0);
    }
    impl->writer = std::make_unique<AsyncCacheWriter>(
        [this](const std::string& name, std::span<const u8> data) {
            if (name == "legacy-import-complete") {
                const auto counts = impl->writer->Statistics();
                if (counts.failed || counts.deferred)
                    return false;
            }
            return CacheFile::Write(impl->path / name, data);
        },
        [] {
            Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        });
    impl->writer->SetBudget(impl->write_budget);
    impl->opened.store(true, std::memory_order_release);
    LOG_INFO(Render, "Pipeline cache opened: {} (legacy import: {})", impl->path.string(),
             impl->allow_legacy);
}

void DataBase::Close() {
    if (!impl->opened.exchange(false, std::memory_order_acq_rel))
        return;
    impl->writer->Close();
    impl->CloseLegacy();
    const auto counts = impl->writer->Statistics();
    LOG_INFO(Render, "Cache drained: {} blobs saved, {} writes failed, {} budget deferrals",
             counts.written, counts.failed, counts.deferred);
}

void DataBase::Reset() {
    if (!IsOpened())
        return;
    Close();
    // Reset only this build's namespace. Original/other-emulator caches are preserved.
    auto old = impl->path;
    old += ".incompatible";
    std::error_code ec;
    std::filesystem::remove_all(old, ec);
    ec.clear();
    std::filesystem::rename(impl->path, old, ec);
    if (ec) {
        LOG_ERROR(Render, "Cannot reset incompatible cache: {}", ec.message());
        return;
    }
    Open();
    impl->CloseLegacy();
    impl->Enqueue("legacy-import-complete", std::vector<u8>{1});
    LOG_INFO(Render, "Incompatible cache moved to {}", old.string());
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    return impl->Enqueue(BlobName(type, name), std::move(data));
}
bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    std::vector<u8> bytes(data.size() * sizeof(u32));
    std::memcpy(bytes.data(), data.data(), bytes.size());
    return Save(type, name, std::move(bytes));
}
void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    data.clear();
    if (IsOpened())
        impl->Read(BlobName(type, name), data);
}
void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    std::vector<u8> bytes;
    Load(type, name, bytes);
    data.clear();
    if (bytes.size() % sizeof(u32) != 0)
        return;
    data.resize(bytes.size() / sizeof(u32));
    if (!bytes.empty())
        std::memcpy(data.data(), bytes.data(), bytes.size());
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    if (!IsOpened())
        return;
    std::set<std::string> names;
    const auto collect = [&](const std::filesystem::path& directory) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it{directory, ec}, end; !ec && it != end;
             it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == Extension(type))
                names.emplace(it->path().filename().string());
        }
    };
    collect(impl->path);
    if (impl->allow_legacy)
        collect(impl->legacy);
    if (impl->allow_legacy && impl->zip_open) {
        for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&impl->legacy_zip); ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> name{};
            mz_zip_reader_get_filename(&impl->legacy_zip, index, name.data(), name.size());
            const std::filesystem::path entry{name.data()};
            if (entry.filename() == entry && entry.extension() == Extension(type))
                names.emplace(name.data());
        }
    }
    for (const auto& name : names) {
        std::vector<u8> data;
        if (impl->Read(name, data))
            func(std::move(data));
    }
}

void DataBase::FinishPreload() {
    if (!IsOpened())
        return;
    if (impl->allow_legacy) {
        // Import all existing SPIR-V, including orphan stages not referenced by a
        // pipeline key. A marker is queued last and only after every import was accepted.
        ForEachBlob(BlobType::ShaderBinary, [](std::vector<u8>&&) {});
        if (impl->writer->Statistics().deferred == 0)
            impl->Enqueue("legacy-import-complete", std::vector<u8>{1});
    }
    impl->CloseLegacy();
}
} // namespace Storage
