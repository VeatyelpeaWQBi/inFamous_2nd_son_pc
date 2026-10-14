// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <charconv>
#include <cctype>
#include <array>
#include <cstdlib>
#include <string_view>
#include <codecvt>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_state.h"
#include "core/file_format/psf.h"
#include "memory_patcher.h"

namespace MemoryPatcher {

EXPORT uintptr_t g_eboot_address;
uint64_t g_eboot_image_size;
std::string g_eboot_name;
std::string g_game_serial;
std::string patch_file;
bool patches_applied = false;
std::vector<patchInfo> pending_patches;

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
static int convertNumBase(const std::string& s) {
    return (isHex0x(s) || isHexSym(s)) ? 16 : 10;
}

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
    } else {
        LOG_INFO(Loader, "Error applying Patch, unknown type: {}", type);
    }
    return result;
}

void ApplyPendingPatches();

void ApplyPatchesFromXML(std::filesystem::path path) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(path.c_str());

    auto* param_sfo = Common::Singleton<PSF>::Instance();
    auto app_version = param_sfo->GetString("APP_VER").value_or("Unknown version");

    if (result) {
        auto patchXML = doc.child("Patch");
        for (pugi::xml_node_iterator it = patchXML.children().begin();
             it != patchXML.children().end(); ++it) {

            if (std::string(it->name()) == "Metadata") {
                if (std::string(it->attribute("isEnabled").value()) == "true") {
                    std::string currentPatchName = it->attribute("Name").value();
                    std::string metadataAppVer = it->attribute("AppVer").value();
                    std::string metadataAppElf = it->attribute("AppElf").value();
                    bool versionMatches = metadataAppVer == app_version;

                    if (metadataAppElf != g_eboot_name)
                        continue;

                    auto patchList = it->first_child();
                    for (pugi::xml_node_iterator patchLineIt = patchList.children().begin();
                         patchLineIt != patchList.children().end(); ++patchLineIt) {

                        std::string type = patchLineIt->attribute("Type").value();
                        if (!versionMatches && type != "mask" && type != "mask_jump32")
                            continue;

                        std::string address = patchLineIt->attribute("Address").value();
                        std::string patchValue = patchLineIt->attribute("Value").value();
                        std::string originalStr = patchLineIt->attribute("Original").value();
                        std::string originalHashStr =
                            patchLineIt->attribute("OriginalHash").value();
                        std::string maskOffsetStr = patchLineIt->attribute("Offset").value();
                        std::string targetStr = "";
                        std::string sizeStr = "";
                        if (type == "mask_jump32") {
                            targetStr = patchLineIt->attribute("Target").value();
                            sizeStr = patchLineIt->attribute("Size").value();
                        } else {
                            try {
                                patchValue = convertValueToHex(type, patchValue);
                            } catch (std::exception& e) {
                                ASSERT_MSG(false,
                                           "Failed to parse patch value \"{}\" for \"{}\" in "
                                           "patch \"{}\", error: \"{}\"\n"
                                           "If the patch was working on earlier versions, then it "
                                           "was using a format that shadPS4 handled incorrectly, "
                                           "and the patch should instead be fixed.",
                                           patchValue, address, currentPatchName, e.what());
                            }
                        }

                        bool littleEndian = false;
                        if (type == "bytes16" || type == "bytes32" || type == "bytes64") {
                            littleEndian = true;
                        }

                        MemoryPatcher::PatchMask patchMask = MemoryPatcher::PatchMask::None;
                        int maskOffsetValue = 0;

                        if (type == "mask")
                            patchMask = MemoryPatcher::PatchMask::Mask;

                        if (type == "mask_jump32")
                            patchMask = MemoryPatcher::PatchMask::Mask_Jump32;

                        if ((type == "mask" || type == "mask_jump32") && !maskOffsetStr.empty()) {
                            maskOffsetValue = std::stoi(maskOffsetStr, 0, 10);
                        }

                        const patchInfo patch = {
                            .gameSerial = "*",
                            .modNameStr = currentPatchName,
                            .offsetStr = address,
                            .valueStr = patchValue,
                            .targetStr = targetStr,
                            .sizeStr = sizeStr,
                            .littleEndian = littleEndian,
                            .patchMask = patchMask,
                            .maskOffset = maskOffsetValue,
                            .originalStr = originalStr,
                            .originalHashStr = originalHashStr,
                        };
                        if (!MemoryPatcher::PatchMemory(patch) &&
                            (!originalStr.empty() || !originalHashStr.empty())) {
                            // The lines of a patch depend on each other (code written first,
                            // then the jumps to it): stop at the first line that does not match.
                            LOG_ERROR(Loader, "Patch {}: the remaining lines are skipped",
                                      currentPatchName);
                            break;
                        }
                    }
                }
            }
        }
    } else {
        LOG_ERROR(Loader, "Could not parse patch XML: {}", result.description());
    }
}

// Fixes built into this build for the games it is tuned for. They do not depend on the patch
// files or on the launcher: each one is checked against the code of the game (FNV-1a 64 hash of
// the bytes it replaces) and left out on another version of the game, or when a patch already
// made the same change. SHADPS4_GAME_PROFILE=0 disables them, with the game profile.
static void ApplyBuiltInFixes() {
    const char* profile = std::getenv("SHADPS4_GAME_PROFILE");
    if (profile != nullptr && std::string_view{profile} == "0") {
        return;
    }
    constexpr std::string_view Address = "0x005f0cbd";
    constexpr std::string_view Value = "4531C990";
    constexpr std::string_view OriginalHash = "5c04f08ca74bb842";
    struct BuiltInFix {
        std::string_view serial;
        std::string_view name;
        std::string_view app_version{}; // empty: retain the original byte-checked version policy
    };
    static constexpr std::array fixes{
        // inFAMOUS Second Son, PS4 Pro mode: its checkerboard rendering shows a green screen,
        // the game renders natively instead (sete r9b -> xor r9d, r9d).
        BuiltInFix{"CUSA00004", "PS4 Pro mode without checkerboard (built in)"},
        // CUSA00309 01.07: verified against the user's dumped executable. The instruction
        // and its offset match the EU patch; other HK versions are not assumed compatible.
        BuiltInFix{"CUSA00309", "PS4 Pro mode without checkerboard (HK 01.07, built in)", "01.07"},
    };
    const auto app_version =
        Common::Singleton<PSF>::Instance()->GetString("APP_VER").value_or("Unknown version");
    for (const auto& fix : fixes) {
        if (fix.serial != g_game_serial) {
            continue;
        }
        if (!fix.app_version.empty() && fix.app_version != app_version) {
            LOG_WARNING(Loader, "Built-in fix {} skipped: APP_VER {} is not {}", fix.name,
                        app_version, fix.app_version);
            continue;
        }
        const u64 offset = std::stoull(std::string{Address}, nullptr, 16) - 0x400000;
        constexpr size_t patch_size = Value.size() / 2;
        if (g_eboot_address == 0 || offset >= g_eboot_image_size ||
            patch_size > g_eboot_image_size - offset) {
            LOG_ERROR(Loader, "Built-in fix {} skipped: address is outside the executable",
                      fix.name);
            continue;
        }
        const auto* current = reinterpret_cast<const unsigned char*>(g_eboot_address + offset);
        bool already = true;
        for (size_t i = 0; i < patch_size; ++i) {
            const auto byte = static_cast<unsigned char>(
                std::stoul(std::string{Value.substr(i * 2, 2)}, nullptr, 16));
            if (current[i] != byte) {
                already = false;
                break;
            }
        }
        if (already) {
            LOG_INFO(Loader, "Built-in fix {}: already made by a patch", fix.name);
            continue;
        }
        PatchMemory({
            .gameSerial = std::string{fix.serial},
            .modNameStr = std::string{fix.name},
            .offsetStr = std::string{Address},
            .valueStr = std::string{Value},
            .targetStr = "",
            .sizeStr = "",
            .isOffset = false,
            .littleEndian = false,
            .patchMask = PatchMask::None,
            .maskOffset = 0,
            .originalStr = "",
            .originalHashStr = std::string{OriginalHash},
        });
    }
}

void OnGameLoaded() {
    std::filesystem::path patch_dir = Common::FS::GetUserPath(Common::FS::PathType::PatchesDir);
    if (!patch_file.empty()) {

        auto file_path = (patch_dir / patch_file).native();
        if (std::filesystem::exists(patch_file)) {
            ApplyPatchesFromXML(patch_file);
        } else {
            ApplyPatchesFromXML(file_path);
        }
    } else if (EmulatorState::GetInstance()->IsAutoPatchesLoadEnabled()) {
        for (auto const& repo : std::filesystem::directory_iterator(patch_dir)) {
            if (!repo.is_directory()) {
                continue;
            }

            const auto files_json_path = repo.path() / "files.json";

            if (!std::filesystem::is_regular_file(files_json_path)) {
                LOG_ERROR(Loader, "The folder '{}' does not contain the files.json file.",
                          repo.path().filename().string());
                continue;
            }

            std::ifstream json_file{files_json_path};
            nlohmann::json available_patches;

            if (!json_file.is_open()) {
                LOG_ERROR(Loader, "Could not open patch index: {}", files_json_path.string());
                continue;
            }

            try {
                available_patches = nlohmann::json::parse(json_file);
            } catch (const nlohmann::json::parse_error& e) {
                LOG_ERROR(Loader, "Could not parse patch index {}: {}", files_json_path.string(),
                          e.what());
                continue;
            }

            std::filesystem::path game_patch_file;
            for (auto const& [filename, serials] : available_patches.items()) {
                if (std::find(serials.begin(), serials.end(), g_game_serial) != serials.end()) {
                    game_patch_file = repo.path() / filename;
                    break;
                }
            }
            if (std::filesystem::exists(game_patch_file)) {
                ApplyPatchesFromXML(game_patch_file);
            }
        }
    }
    ApplyPendingPatches();
    ApplyBuiltInFixes();
}

void AddPatchToQueue(const patchInfo& patchToAdd) {
    if (patches_applied) {
        PatchMemory(patchToAdd);
        return;
    }
    pending_patches.push_back(patchToAdd);
}

void ApplyPendingPatches() {
    patches_applied = true;
    for (size_t i = 0; i < pending_patches.size(); ++i) {
        const patchInfo& currentPatch = pending_patches[i];

        if (currentPatch.gameSerial != "*" && currentPatch.gameSerial != g_game_serial)
            continue;

        PatchMemory(currentPatch);
    }

    pending_patches.clear();
}

static bool WithinImage(uintptr_t address, size_t size) {
    return g_eboot_address != 0 && address >= g_eboot_address &&
           size <= g_eboot_image_size &&
           address - g_eboot_address <= g_eboot_image_size - size;
}

bool PatchMemory(const patchInfo& patch) {
    if (patch.valueStr.empty() || patch.valueStr.size() % 2 != 0 ||
        patch.valueStr.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        LOG_ERROR(Loader, "Invalid byte payload for patch {}", patch.modNameStr);
        return false;
    }
    // Send a request to modify the process memory.
    void* cheatAddress = nullptr;

    if (patch.patchMask == PatchMask::None) {
        std::string_view text{patch.offsetStr};
        if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
        u64 offset{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), offset, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            (!patch.isOffset && offset < 0x400000)) {
            LOG_ERROR(Loader, "Invalid offset for patch {}", patch.modNameStr);
            return false;
        }
        if (!patch.isOffset) offset -= 0x400000;
        if (offset >= g_eboot_image_size) return false;
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
            parsedSize.ptr != patch.sizeStr.data() + patch.sizeStr.size()) return false;

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
        const s64 backward = static_cast<s64>(patchAddress + jumpSize) -
                             static_cast<s64>(code_cave_end + 5);
        if ((patchAddress < code_cave_end + 5 && jump_target < patchAddress + jumpSize) ||
            forward < std::numeric_limits<s32>::min() || forward > std::numeric_limits<s32>::max() ||
            backward < std::numeric_limits<s32>::min() || backward > std::numeric_limits<s32>::max()) {
            LOG_ERROR(Loader, "Patch cave overlaps the source or needs an out-of-range jump");
            return false;
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

        LOG_INFO(Loader,
                 "Applied Patch mask_jump32: {}, PatchAddress: {:#x}, JumpTarget: {:#x}, "
                 "CodeCaveEnd: {:#x}, JumpSize: {}",
                 patch.modNameStr, patchAddress, jump_target, code_cave_end, jumpSize);
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

    std::memcpy(cheatAddress, bytePatch.data(), bytePatch.size());

    LOG_INFO(Loader, "Applied patch: {}, Offset: {:#x}, Value: {}", patch.modNameStr,
             (uintptr_t)cheatAddress, patch.valueStr);
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
            if (cursor < pattern.size() && pattern[cursor] == '?') ++cursor;
            bytes.push_back(-1);
        } else {
            if (pattern.size() - cursor < 2) return {};
            unsigned value{};
            const auto start = pattern.data() + cursor;
            const auto parsed = std::from_chars(start, start + 2, value, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != start + 2) return {};
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

    if (!g_eboot_address || sigSize == 0 || sigSize > g_eboot_image_size) return 0;
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
