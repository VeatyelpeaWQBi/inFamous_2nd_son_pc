// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/second_son_controls.h"
#include "core/second_son_prompts.h"
#include "core/second_son_touchpad_icons.h"
#include "core/file_sys/ifile.h"
#include "common/logging/log.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace Core::SecondSon {
namespace {
// All packs are from the same read-only CUSA00309 01.07 installation as the program.
struct Pack {
    std::string_view name, sha256;
    std::size_t glyphs{}, atlas{};
    bool icons{};
};
constexpr std::array<Pack, 7> Packs{{
    {"lang_english_text.xpps", "eb7e93659e667ee213b520ee1bb8500d3968eb027b66b2fd5cbd64d7c35604d5", 0xbf570, 0x135808},
    {"lang_english_text_p.xpps", "61bb0a1a522deea4ffb3fa9a5a2d078e67b5cee98d168640a289904f51b10040"},
    {"lang_chinese_text.xpps", "57ee5a93f949dfe54ddc80cf3e63d1018c5f1861341f31856dc25ea9c0abecfa", 0xc3158, 0x539270},
    {"lang_chinese_text_p.xpps", "41ba5038e09b0e12755b2932eb390a0d10519c6ca6e514118a0085fc015d11b5"},
    {"lang_korean_text.xpps", "dfc913ef9e406ea3846c9971a067d15b1c844decc086d21722033817d989d2d9", 0xc9108, 0x53f3a0},
    {"lang_korean_text_p.xpps", "019139fa3067350c68a1555c447300820b277b0de214df41857344806ab558a1"},
    {"core_common.sprig.xpps", "3fb0e5f7337c73cb4774e85daa59095cb85758b9fe90bfa50e09fc9af17c334b", 0, 0, true}
}};

// An immutable, validated RAM view. Reads, preads and mmap all see the same bytes.
// Retain the underlying handle solely for metadata; no raw/native mapping can bypass the view.
class PromptFile final : public FileSys::IFile {
public:
    PromptFile(std::unique_ptr<FileSys::IFile> file, std::vector<u8> bytes)
        : source(std::move(file)), data(std::move(bytes)) {}
    s64 Read(void* dst, u64 size) override {
        const auto count = position < data.size() ? std::min<u64>(size, data.size() - position) : 0;
        if (count != 0) {
            std::memcpy(dst, data.data() + position, static_cast<std::size_t>(count));
        }
        position += count;
        return static_cast<s64>(count);
    }
    std::size_t ReplaceAtlas(const Pack& pack) {
        return ReplaceXboxAtlas(data, pack.glyphs, pack.atlas);
    }
    std::size_t ReplaceTouchText() { return ReplaceTouchpadText(data); }
    bool ReplaceIcons() {
        if (data.size() != TouchpadIcons::PackBytes ||
            std::memcmp(data.data() + TouchpadIcons::DescriptorOffset,
                        TouchpadIcons::Descriptor.data(), sizeof(TouchpadIcons::Descriptor)) != 0) {
            return false;
        }
        const auto bytes = std::span{data};
        return TouchpadIcons::Replace(
            bytes.subspan(TouchpadIcons::TextureOffset, TouchpadIcons::TextureBytes),
            {bytes.subspan(TouchpadIcons::SingleUvOffset, 32),
             bytes.subspan(TouchpadIcons::SinglePositionOffset, 48),
             bytes.subspan(TouchpadIcons::SingleBoundsOffset, 24)},
            {bytes.subspan(TouchpadIcons::PairUvOffset, 64),
             bytes.subspan(TouchpadIcons::PairPositionOffset, 96),
             bytes.subspan(TouchpadIcons::PairBoundsOffset, 24)});
    }
    std::unique_ptr<FileSys::IFile> Unwrap() {
        source->Seek(0, Common::FS::SeekOrigin::SetOrigin);
        return std::move(source);
    }
    s64 Write(const void*, u64) override { return -1; }
    bool Seek(s64 offset, Common::FS::SeekOrigin origin) override {
        const auto base = origin == Common::FS::SeekOrigin::SetOrigin ? 0
                        : origin == Common::FS::SeekOrigin::CurrentPosition ? position
                        : origin == Common::FS::SeekOrigin::End ? data.size()
                        : std::numeric_limits<u64>::max();
        if (base == std::numeric_limits<u64>::max() ||
            (offset < 0 && static_cast<u64>(-(offset + 1)) + 1 > base) ||
            (offset >= 0 && static_cast<u64>(offset) >
                                static_cast<u64>(std::numeric_limits<s64>::max()) - base)) {
            return false;
        }
        // POSIX seeks may pass EOF; the checked non-negative signed result cannot overflow.
        position = static_cast<u64>(static_cast<s64>(base) + offset);
        return true;
    }
    u64 Tell() const override { return position; }
    u64 Size() const override { return data.size(); }
    bool Flush() override { return true; }
    bool IsOpen() const override { return source->IsOpen(); }
    bool IsReadOnly() const override { return true; }
    void Stat(FileSys::FileStat& out) override { source->Stat(out); }
    std::optional<std::filesystem::path> GetMetadataHostPath() const override {
        return source->GetMetadataHostPath();
    }
    FileSys::MmapPolicy GetMmapPolicy() const override { return FileSys::MmapPolicy::Copy; }
    bool Map(u8* address, u64 size, u64 offset, u32 protection,
             const FileSys::FileMapContext& context) override {
        if (size > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        context.map_anonymous(address, size);
        const auto count = offset < data.size() ? std::min<u64>(size, data.size() - offset) : 0;
        if (count != 0) {
            std::memcpy(address, data.data() + offset, static_cast<std::size_t>(count));
        }
        std::memset(address + count, 0, static_cast<std::size_t>(size - count));
        context.protect(address, size, protection);
        return true;
    }
private:
    std::unique_ptr<FileSys::IFile> source;
    std::vector<u8> data;
    u64 position{};
};
} // namespace

std::unique_ptr<FileSys::IFile> XboxPromptFile(std::string_view path,
                                              std::unique_ptr<FileSys::IFile> file) {
    if (!XboxPromptsEnabled() || !file) {
        return file;
    }
    constexpr std::string_view app = "/app0/art/cache/", host = "/hostapp/art/cache/";
    if (path.starts_with(app)) {
        path.remove_prefix(app.size());
    } else if (path.starts_with(host)) {
        path.remove_prefix(host.size());
    } else {
        return file;
    }
    const auto pack = std::ranges::find_if(Packs, [&](const auto& entry) { return entry.name == path; });
    if (pack == Packs.end() || (pack->icons && !TouchpadChordsEnabled())) {
        return file;
    }
    const auto size = file->Size();
    if (pack->icons ? size != TouchpadIcons::PackBytes : size > 8 * 1024 * 1024) {
        LOG_WARNING(Loader, "Xbox labels skipped for changed resource {}", path);
        return file; // Preserve unknown builds and existing font/translation/resource mods.
    }
    if (!file->Seek(0, Common::FS::SeekOrigin::SetOrigin)) {
        return file;
    }
    std::unique_ptr<PromptFile> snapshot;
    try {
        // Keep the validated bytes themselves: reads/preads/mmap must not reopen a mutable file
        // after hashing it. core_common uses ~303 MiB until this resource handle is closed.
        std::vector<u8> bytes(static_cast<std::size_t>(size));
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto read = file->Read(bytes.data() + done, bytes.size() - done);
            if (read <= 0 || static_cast<u64>(read) > bytes.size() - done) {
                file->Seek(0, Common::FS::SeekOrigin::SetOrigin);
                return file;
            }
            done += static_cast<std::size_t>(read);
        }
        snapshot = std::make_unique<PromptFile>(std::move(file), std::move(bytes));
        // This also verifies all original texture/mip/UV bytes before any mutation.
        if (HashFile(*snapshot) != pack->sha256) {
            LOG_WARNING(Loader, "Xbox labels skipped for changed resource {}", path);
            return snapshot->Unwrap();
        }
        std::size_t glyphs = 0;
        if (pack->atlas != 0) {
            glyphs = snapshot->ReplaceAtlas(*pack);
            if (glyphs == 0) {
                LOG_WARNING(Loader, "Xbox atlas rejected for {}: glyph rectangles differ", path);
                return snapshot->Unwrap();
            }
        }
        const bool icons = pack->icons && snapshot->ReplaceIcons();
        if (pack->icons && !icons) {
            LOG_WARNING(Loader, "Touchpad icons rejected for {}: descriptor or UVs differ", path);
            return snapshot->Unwrap();
        }
        // Localization scanning applies only to language packs, never to the geometry pack.
        const auto texts = !pack->icons && TouchpadChordsEnabled() ? snapshot->ReplaceTouchText() : 0;
        LOG_INFO(Loader, "Xbox UI: {} shared glyphs, {} touchpad texts, touchpad icon atlas={} "
                         "in {} (RAM only)", glyphs, texts, icons, path);
        return snapshot;
    } catch (const std::exception& error) {
        // An optional UI patch must not prevent playing if snapshot allocation/hashing fails.
        LOG_WARNING(Loader, "Xbox labels skipped for {}: {}", path, error.what());
        if (snapshot) {
            return snapshot->Unwrap();
        }
        if (file) {
            file->Seek(0, Common::FS::SeekOrigin::SetOrigin);
        }
        return file;
    }
}
} // namespace Core::SecondSon
