// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <system_error>

#include "memory_patcher.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/file_format/psf.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <codecvt>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <pugixml.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace MemoryPatcher {

EXPORT uintptr_t g_eboot_address;
uint64_t g_eboot_image_size;
std::string g_eboot_name;
std::string g_game_serial;
std::string patch_file;

std::string toHex(u64 value, size_t byteSize) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(byteSize * 2) << value;
    return ss.str();
}

static bool isHexSym(const std::string& s) {
    return (s.size() >= 2 && (s[0] == '$' || s[0] == '#'));
}

static bool isHex0x(const std::string& s) {
    return (s.size() >= 3 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
}

// possible prefixes == (s == `0x` || s == `#` || s == `$`) will be interpet as hex value, else is
// decimal
static int convertNumBase(const std::string& s) { return (isHex0x(s) || isHexSym(s)) ? 16 : 10; }

std::string convertValueToHex(const std::string type, const std::string valueStr) {
    std::string result;

    if (type == "byte") {
        const u32 value = std::stoul(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 1);
    } else if (type == "bytes16") {
        const u32 value = std::stoul(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 2);
    } else if (type == "bytes32") {
        const u32 value = std::stoul(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 4);
    } else if (type == "bytes64") {
        const u64 value = std::stoull(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 8);
    } else if (type == "float32") {
        union {
            float f;
            uint32_t i;
        } floatUnion;
        floatUnion.f = std::stof(valueStr);
        result = toHex(std::byteswap(floatUnion.i), sizeof(floatUnion.i));
    } else if (type == "float64") {
        union {
            double d;
            uint64_t i;
        } doubleUnion;
        doubleUnion.d = std::stod(valueStr);
        result = toHex(std::byteswap(doubleUnion.i), sizeof(doubleUnion.i));
    } else if (type == "utf8") {
        std::vector<unsigned char> byteArray =
            std::vector<unsigned char>(valueStr.begin(), valueStr.end());
        byteArray.push_back('\0');
        std::stringstream ss;
        for (unsigned char c : byteArray) {
            ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
        }
        result = ss.str();
    } else if (type == "utf16") {
        std::wstring wide_str(valueStr.size(), L'\0');
        std::mbstowcs(&wide_str[0], valueStr.c_str(), valueStr.size());
        wide_str.resize(std::wcslen(wide_str.c_str()));

        std::u16string valueStringU16;

        for (wchar_t wc : wide_str) {
            if (wc <= 0xFFFF) {
                valueStringU16.push_back(static_cast<char16_t>(wc));
            } else {
                wc -= 0x10000;
                valueStringU16.push_back(static_cast<char16_t>(0xD800 | (wc >> 10)));
                valueStringU16.push_back(static_cast<char16_t>(0xDC00 | (wc & 0x3FF)));
            }
        }

        std::vector<unsigned char> byteArray;
        // convert to little endian
        for (char16_t ch : valueStringU16) {
            unsigned char low_byte = static_cast<unsigned char>(ch & 0x00FF);
            unsigned char high_byte = static_cast<unsigned char>((ch >> 8) & 0x00FF);

            byteArray.push_back(low_byte);
            byteArray.push_back(high_byte);
        }
        byteArray.push_back('\0');
        byteArray.push_back('\0');
        std::stringstream ss;

        for (unsigned char ch : byteArray) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
        result = ss.str();
    } else if (type == "bytes") {
        result = valueStr;
    } else if (type == "mask" || type == "mask_jump32") {
        result = valueStr;
    }
    return result;
}

void ApplyPatchesFromXML(std::filesystem::path path) {
    pugi::xml_document doc;
    if (!doc.load_file(path.c_str())) {
        throw std::runtime_error("Cannot read the selected runtime patch file.");
    }
    const auto root = doc.child("Patch");
    bool declared_title = false;
    for (const auto id : root.child("TitleID").children("ID")) {
        declared_title |= g_game_serial == id.child_value();
    }
    if (!declared_title) {
        throw std::runtime_error("Runtime patches do not match the mounted game title.");
    }
    const auto version = Common::Singleton<PSF>::Instance()->GetString("APP_VER").value_or("");
    std::string elf = g_eboot_name;
    std::ranges::transform(elf, elf.begin(), [](unsigned char c) { return std::tolower(c); });
    std::vector<patchInfo> patches;
    std::vector<std::pair<u64, u64>> ranges;
    constexpr std::string_view hex = "0123456789abcdefABCDEF";
    for (const auto meta : root.children("Metadata")) {
        if (!meta.attribute("isEnabled").as_bool()) {
            continue;
        }
        if (version != meta.attribute("AppVer").as_string() ||
            elf != meta.attribute("AppElf").as_string()) {
            throw std::runtime_error("An enabled runtime patch targets another version.");
        }
        if (!meta.child("PatchList").child("Line")) {
            throw std::runtime_error("An enabled runtime patch has no guarded writes.");
        }
        for (const auto line : meta.child("PatchList").children("Line")) {
            patchInfo patch{
                .gameSerial = g_game_serial,
                .modNameStr = meta.attribute("Name").as_string(),
                .offsetStr = line.attribute("Address").as_string(),
                .valueStr = line.attribute("Value").as_string(),
                .targetStr = "",
                .sizeStr = "",
                .isOffset = false,
                .littleEndian = false,
                .patchMask = PatchMask::None,
                .maskOffset = 0,
                .originalStr = line.attribute("Original").as_string(),
                .originalHashStr = line.attribute("OriginalHash").as_string(),
            };
            // Release profiles use direct, non-overlapping, fully guarded byte writes.
            if (std::string_view(line.attribute("Type").as_string()) != "bytes" ||
                (patch.originalStr.empty() && patch.originalHashStr.empty()) ||
                (!patch.originalStr.empty() && patch.originalStr.size() != patch.valueStr.size()) ||
                (!patch.originalHashStr.empty() &&
                 (patch.originalHashStr.size() != 16 ||
                  patch.originalHashStr.find_first_not_of(hex) != std::string::npos)) ||
                !PatchMemory(patch, true)) {
                throw std::runtime_error(
                    "Patch validation failed; no runtime patches have been written.");
            }
            // PatchMemory validated the address and range, so the conversion cannot overflow.
            const auto address = std::stoull(patch.offsetStr, nullptr, 16);
            const auto end = address + patch.valueStr.size() / 2;
            for (const auto& [start, finish] : ranges) {
                if (address < finish && start < end) {
                    throw std::runtime_error(
                        "Overlapping runtime patches rejected before any writes.");
                }
            }
            ranges.emplace_back(address, end);
            patches.push_back(std::move(patch));
        }
    }
    // Every enabled write has passed validation before the first write is performed.
    for (const auto& patch : patches) {
        if (!PatchMemory(patch)) {
            throw std::runtime_error("Runtime patch input changed after validation.");
        }
    }
}

void OnGameLoaded() {
    if (patch_file.empty()) {
        throw std::runtime_error("A Second Son patch selection must be prepared before loading.");
    }
    ApplyPatchesFromXML(std::filesystem::path{std::u8string(patch_file.begin(), patch_file.end())});
}

static bool WithinImage(uintptr_t address, size_t size) {
    return g_eboot_address != 0 && address >= g_eboot_address && size <= g_eboot_image_size &&
           address - g_eboot_address <= g_eboot_image_size - size;
}

bool PatchMemory(const patchInfo& patch, bool dry_run) {
    if (patch.valueStr.empty() || patch.valueStr.size() % 2 != 0 ||
        patch.valueStr.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        LOG_ERROR(Loader, "Invalid byte payload for patch {}", patch.modNameStr);
        return false;
    }
    // Send a request to modify the process memory.
    void* cheatAddress = nullptr;

    if (patch.patchMask == PatchMask::None) {
        std::string_view text{patch.offsetStr};
        if (text.starts_with("0x") || text.starts_with("0X")) {
            text.remove_prefix(2);
        }
        u64 offset{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), offset, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            (!patch.isOffset && offset < 0x400000)) {
            LOG_ERROR(Loader, "Invalid offset for patch {}", patch.modNameStr);
            return false;
        }
        if (!patch.isOffset) {
            offset -= 0x400000;
        }
        if (offset >= g_eboot_image_size) {
            return false;
        }
        cheatAddress = reinterpret_cast<void*>(g_eboot_address + offset);
    }

    if (patch.patchMask == PatchMask::Mask) {
        cheatAddress = reinterpret_cast<void*>(PatternScan(patch.offsetStr) + patch.maskOffset);
    }

    if (patch.patchMask == PatchMask::Mask_Jump32) {
        int jumpSize{};
        const auto parsedSize = std::from_chars(
            patch.sizeStr.data(), patch.sizeStr.data() + patch.sizeStr.size(), jumpSize);
        if (parsedSize.ec != std::errc{} ||
            parsedSize.ptr != patch.sizeStr.data() + patch.sizeStr.size()) {
            return false;
        }

        constexpr int MAX_PATTERN_LENGTH = 256;
        if (jumpSize < 5) {
            LOG_ERROR(Loader, "Jump size must be at least 5 bytes");
            return false;
        }
        if (jumpSize > MAX_PATTERN_LENGTH) {
            LOG_ERROR(Loader, "Jump size must be no more than {} bytes.", MAX_PATTERN_LENGTH);
            return false;
        }

        // Find the base address using "Address"
        uintptr_t baseAddress = PatternScan(patch.offsetStr);
        if (baseAddress == 0) {
            LOG_ERROR(Loader, "PatternScan failed for mask_jump32 with pattern: {}",
                      patch.offsetStr);
            return false;
        }
        uintptr_t patchAddress = baseAddress + patch.maskOffset;

        // Use "Target" to locate the start of the code cave
        uintptr_t jump_target = PatternScan(patch.targetStr);
        if (jump_target == 0) {
            LOG_ERROR(Loader, "PatternScan failed to Target with pattern: {}", patch.targetStr);
            return false;
        }

        // Converts the Value attribute to a byte array (payload)
        std::vector<u8> payload;
        for (size_t i = 0; i < patch.valueStr.length(); i += 2) {
            std::string tempStr = patch.valueStr.substr(i, 2);
            const char* byteStr = tempStr.c_str();
            char* endPtr;
            unsigned int byteVal = std::strtoul(byteStr, &endPtr, 16);

            if (endPtr != byteStr + 2) {
                LOG_ERROR(Loader, "Invalid byte in Value: {}", patch.valueStr.substr(i, 2));
                return false;
            }
            payload.push_back(static_cast<u8>(byteVal));
        }

        // Calculates the end of the code cave (where the return jump will be inserted)
        uintptr_t code_cave_end = jump_target + payload.size();

        if (!WithinImage(patchAddress, jumpSize) || !WithinImage(jump_target, payload.size() + 5)) {
            LOG_ERROR(Loader, "Patch jump/cave lies outside the loaded image");
            return false;
        }
        const s64 forward = static_cast<s64>(jump_target) - static_cast<s64>(patchAddress + 5);
        const s64 backward =
            static_cast<s64>(patchAddress + jumpSize) - static_cast<s64>(code_cave_end + 5);
        if ((patchAddress < code_cave_end + 5 && jump_target < patchAddress + jumpSize) ||
            forward < std::numeric_limits<s32>::min() ||
            forward > std::numeric_limits<s32>::max() ||
            backward < std::numeric_limits<s32>::min() ||
            backward > std::numeric_limits<s32>::max()) {
            LOG_ERROR(Loader, "Patch cave overlaps the source or needs an out-of-range jump");
            return false;
        }
        if (dry_run) {
            return true;
        }
        // No memory is modified until both locations and the payload are valid.
        std::vector<u8> nopBytes(jumpSize, 0x90);
        std::memcpy(reinterpret_cast<void*>(patchAddress), nopBytes.data(), nopBytes.size());

        // Write the payload to the code cave, from jump_target
        std::memcpy(reinterpret_cast<void*>(jump_target), payload.data(), payload.size());

        // Inserts the initial jump in the original region to divert to the code cave
        u8 jumpInstruction[5];
        jumpInstruction[0] = 0xE9;
        s32 relJump = static_cast<s32>(forward);
        std::memcpy(&jumpInstruction[1], &relJump, sizeof(relJump));
        std::memcpy(reinterpret_cast<void*>(patchAddress), jumpInstruction,
                    sizeof(jumpInstruction));

        // Inserts jump back at the end of the code cave to resume execution after patching
        u8 jumpBack[5];
        jumpBack[0] = 0xE9;
        // Calculates the relative offset to return to the instruction immediately following the
        // overwritten region
        s32 target_return = static_cast<s32>(backward);
        std::memcpy(&jumpBack[1], &target_return, sizeof(target_return));
        std::memcpy(reinterpret_cast<void*>(code_cave_end), jumpBack, sizeof(jumpBack));

        return true;
    }

    if (!WithinImage(reinterpret_cast<uintptr_t>(cheatAddress), patch.valueStr.size() / 2)) {
        LOG_ERROR(Loader, "Failed to get address for patch {}", patch.modNameStr);
        return false;
    }

    std::vector<unsigned char> bytePatch;

    for (size_t i = 0; i < patch.valueStr.length(); i += 2) {
        unsigned char byte = static_cast<unsigned char>(
            std::strtol(patch.valueStr.substr(i, 2).c_str(), nullptr, 16));

        bytePatch.push_back(byte);
    }

    if (patch.littleEndian) {
        std::reverse(bytePatch.begin(), bytePatch.end());
    }

    if (!patch.originalStr.empty()) {
        if (patch.originalStr.size() % 2 != 0 ||
            patch.originalStr.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
            !WithinImage(reinterpret_cast<uintptr_t>(cheatAddress), patch.originalStr.size() / 2)) {
            return false;
        }
        std::vector<unsigned char> original;
        for (size_t i = 0; i + 1 < patch.originalStr.length(); i += 2) {
            original.push_back(static_cast<unsigned char>(
                std::strtol(patch.originalStr.substr(i, 2).c_str(), nullptr, 16)));
        }
        if (std::memcmp(cheatAddress, original.data(), original.size()) != 0) {
            LOG_ERROR(Loader,
                      "Patch {} skipped: the bytes at {:#x} are not the expected ones, the patch "
                      "is probably made for another version of the game",
                      patch.modNameStr, reinterpret_cast<uintptr_t>(cheatAddress));
            return false;
        }
    }

    if (!patch.originalHashStr.empty()) {
        // FNV-1a (64 bits) of the bytes the patch writes over: checks the version of the game
        // without the patch file carrying the game's own bytes.
        u64 hash = 0xcbf29ce484222325ULL;
        const auto* current = static_cast<const unsigned char*>(cheatAddress);
        for (size_t i = 0; i < bytePatch.size(); ++i) {
            hash = (hash ^ current[i]) * 0x100000001b3ULL;
        }
        if (hash != std::strtoull(patch.originalHashStr.c_str(), nullptr, 16)) {
            LOG_ERROR(Loader,
                      "Patch {} skipped: the code at {:#x} is not the expected one, the patch is "
                      "probably made for another version of the game",
                      patch.modNameStr, reinterpret_cast<uintptr_t>(cheatAddress));
            return false;
        }
    }

    if (!dry_run) {
        std::memcpy(cheatAddress, bytePatch.data(), bytePatch.size());
    }
    return true;
}

static std::vector<int32_t> PatternToByte(const std::string& pattern) {
    std::vector<int32_t> bytes;
    size_t cursor = 0;
    while (cursor < pattern.size()) {
        if (std::isspace(static_cast<unsigned char>(pattern[cursor]))) {
            ++cursor;
        } else if (pattern[cursor] == '?') {
            ++cursor;
            if (cursor < pattern.size() && pattern[cursor] == '?') {
                ++cursor;
            }
            bytes.push_back(-1);
        } else {
            if (pattern.size() - cursor < 2) {
                return {};
            }
            unsigned value{};
            const auto start = pattern.data() + cursor;
            const auto parsed = std::from_chars(start, start + 2, value, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != start + 2) {
                return {};
            }
            bytes.push_back(static_cast<int32_t>(value));
            cursor += 2;
        }
    }

    return bytes;
}

uintptr_t PatternScan(const std::string& signature) {
    std::vector<int32_t> patternBytes = PatternToByte(signature);
    const auto scanBytes = static_cast<uint8_t*>((void*)g_eboot_address);

    const int32_t* sigPtr = patternBytes.data();
    const size_t sigSize = patternBytes.size();

    if (!g_eboot_address || sigSize == 0 || sigSize > g_eboot_image_size) {
        return 0;
    }
    for (size_t i = 0; i <= g_eboot_image_size - sigSize; ++i) {
        bool found = true;
        for (size_t j = 0; j < sigSize; ++j) {
            if (scanBytes[i + j] != sigPtr[j] && sigPtr[j] != -1) {
                found = false;
                break;
            }
        }

        if (found) {
            return reinterpret_cast<uintptr_t>(&scanBytes[i]);
        }
    }

    return 0;
}

} // namespace MemoryPatcher
