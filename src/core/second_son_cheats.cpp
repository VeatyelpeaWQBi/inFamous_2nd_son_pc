// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/second_son_cheats.h"
#include "core/second_son_compatibility.h"
#include "common/logging/log.h"
#include "common/types.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <pugixml.hpp>
#include <nlohmann/json.hpp>
#include <windows.h>

namespace Core::SecondSon::Cheats {
namespace {
// All game instructions, addresses, arithmetic and parameters come from the build's XML.
struct Choice { u32 bits{}, display{}; };
struct Control { std::size_t offset{}; bool generation{}; std::vector<Choice> choices; };
struct Runtime { u8* arena{}; std::array<Control, 3> controls; };
struct ImageWrite { std::size_t offset; std::vector<u8> original, replacement; };
struct Range { std::size_t begin, end; };
struct Selection {
    std::string title, version, sha256;
    std::filesystem::path profile;
} selection;
std::atomic<std::shared_ptr<Runtime>> runtime;
std::atomic<Status> status{Status::Unsupported};
constexpr std::array<std::string_view, 3> ControlIds{"recovery", "karma", "experience"};
static_assert(std::atomic_ref<u32>::is_always_lock_free);

[[noreturn]] void Reject(const char* reason) { throw std::runtime_error(reason); }
u64 Hex(std::string_view text) {
    if (text.starts_with("0x")) text.remove_prefix(2);
    u64 result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, 16);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        Reject("Invalid XML hex integer.");
    return result;
}
std::vector<u8> Bytes(std::string_view text) {
    if (text.empty() || text.size() % 2 || text.size() > TrampolineSize * 2 ||
        text.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos)
        Reject("Invalid XML byte payload.");
    std::vector<u8> result;
    result.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2)
        result.push_back(static_cast<u8>(Hex(text.substr(i, 2))));
    return result;
}
void Bounds(u64 offset, u64 size, u64 limit) {
    if (size == 0 || offset > limit || size > limit - offset) Reject("XML patch range is out of bounds.");
}
void AddRange(std::vector<Range>& ranges, std::size_t offset, std::size_t size) {
    for (const auto& range : ranges)
        if (offset < range.end && range.begin < offset + size) Reject("Overlapping XML patch ranges.");
    ranges.push_back({offset, offset + size});
}
std::size_t ImageOffset(const pugi::xml_attribute& attribute, std::size_t image_size) {
    // Preserve the 0x400000-based addresses used by existing XML PatchList/Line entries.
    const auto address = Hex(attribute.as_string());
    if (address < 0x400000) Reject("Invalid XML image address.");
    Bounds(address - 0x400000, 1, image_size);
    return static_cast<std::size_t>(address - 0x400000);
}
s32 Relative(std::uintptr_t source_end, std::uintptr_t target) {
    const auto distance = static_cast<s64>(target) - static_cast<s64>(source_end);
    if (distance < std::numeric_limits<s32>::min() || distance > std::numeric_limits<s32>::max())
        Reject("XML patch branch is outside rel32 range.");
    return static_cast<s32>(distance);
}
std::atomic_ref<u32> Word(const Runtime& active, const Control& control) {
    return std::atomic_ref(*reinterpret_cast<u32*>(active.arena + control.offset));
}
bool Enabled(const Runtime& active, const Control& control) {
    return (Word(active, control).load(std::memory_order_acquire) & 1) != 0;
}
void SetToggle(std::size_t index, bool enabled) {
    if (auto active = runtime.load(std::memory_order_acquire)) {
        auto word = Word(*active, active->controls[index]);
        auto old = word.load(std::memory_order_relaxed);
        // Low bit is enabled; remaining bits identify a toggle generation for XML code.
        while (static_cast<bool>(old & 1) != enabled &&
               !word.compare_exchange_weak(old, (old + 2) ^ 1u, std::memory_order_release,
                                            std::memory_order_relaxed)) {}
    }
}
} // namespace

void Configure(std::string_view title, std::string_view version, std::string_view sha256,
               const std::filesystem::path& matched_profile) {
    runtime.store(nullptr, std::memory_order_release);
    selection = {std::string(title), std::string(version), std::string(sha256), matched_profile};
    status.store(!matched_profile.empty() && IsTitle(title) && IsVersion(version) && IsSha256(sha256)
                     ? Status::Pending : Status::Unsupported, std::memory_order_release);
}
Status GetStatus() { return status.load(std::memory_order_acquire); }
void ConfigureFromManifest(std::string_view title, std::string_view version,
                           std::string_view sha256, const std::filesystem::path& project_root) {
    Configure(title, version, sha256, {});
    try {
        std::ifstream input(project_root / "profiles/second-son-builds.json");
        if (!input) Reject("Cannot read the cheat build manifest.");
        const auto registry = nlohmann::json::parse(input);
        if (registry.at("schema") != 1 || registry.at("game") != "inFAMOUS Second Son" ||
            !registry.at("builds").is_array()) Reject("Invalid cheat build manifest.");
        std::filesystem::path matched;
        for (const auto& entry : registry.at("builds")) {
            const SecondSon::Build build{
                .title_ids = entry.at("title_ids").get<std::vector<std::string>>(),
                .version = entry.at("version").get<std::string>(),
                .sha256 = entry.at("sha256").get<std::string>(),
                .patch_file = entry.at("patch_file").get<std::string>(),
                .legacy_cache_title = entry.value("legacy_cache_title", std::string{}),
            };
            if (!build.IsValid()) Reject("Invalid cheat build entry.");
            if (build.Matches(title, version, sha256)) {
                if (!matched.empty()) Reject("Ambiguous cheat build entries.");
                matched = project_root / "profiles" / build.patch_file;
            }
        }
        Configure(title, version, sha256, matched);
    } catch (const std::exception& error) {
        status.store(Status::Rejected, std::memory_order_release);
        LOG_ERROR(Loader, "Second Son cheat manifest rejected: {}", error.what());
    }
}
Options GetOptions() {
    if (auto active = runtime.load(std::memory_order_acquire)) {
        const auto& control = active->controls[2];
        const auto value = Word(*active, control).load(std::memory_order_acquire);
        const auto choice = std::ranges::find(control.choices, value, &Choice::bits);
        return {Enabled(*active, active->controls[0]), Enabled(*active, active->controls[1]),
                choice != control.choices.end() ? choice->display : 1};
    }
    return {};
}
void SetRecovery(bool enabled) { SetToggle(0, enabled); }
void SetKarma(bool enabled) { SetToggle(1, enabled); }
void CycleExperience() {
    if (auto active = runtime.load(std::memory_order_acquire)) {
        const auto& control = active->controls[2];
        auto word = Word(*active, control);
        auto old = word.load(std::memory_order_relaxed);
        for (;;) {
            const auto current = std::ranges::find(control.choices, old, &Choice::bits);
            if (current == control.choices.end()) return;
            const auto index = static_cast<std::size_t>(current - control.choices.begin());
            const auto next = control.choices[(index + 1) % control.choices.size()].bits;
            if (word.compare_exchange_weak(old, next, std::memory_order_release,
                                          std::memory_order_relaxed)) return;
        }
    }
}

void Install(std::uintptr_t base, std::size_t image_size, void* arena, std::size_t arena_size) {
    if (GetStatus() != Status::Pending) return;
    try {
        const auto arena_base = reinterpret_cast<std::uintptr_t>(arena);
        constexpr auto address_limit = static_cast<std::uintptr_t>(std::numeric_limits<s64>::max());
        if (!arena || arena_size < TrampolineSize || arena_base % alignof(u32) ||
            image_size > address_limit || base > address_limit - image_size ||
            arena_base > address_limit - TrampolineSize) Reject("Invalid XML patch memory reservation.");
        pugi::xml_document document;
        if (!document.load_file(selection.profile.c_str())) Reject("Cannot read the selected patch XML.");
        const auto root = document.child("Patch");
        bool title_matches = false;
        for (const auto id : root.child("TitleID").children("ID"))
            title_matches |= selection.title == id.child_value();
        const auto definition = root.child("RuntimeCheats");
        if (!definition) {
            status.store(Status::Unsupported, std::memory_order_release);
            return;
        }
        if (!title_matches || definition.next_sibling("RuntimeCheats") ||
            std::string_view(definition.attribute("Schema").as_string()) != "1" ||
            selection.version != definition.attribute("AppVer").as_string() ||
            selection.sha256 != definition.attribute("SHA256").as_string() ||
            std::string_view(definition.attribute("AppElf").as_string()) != "eboot.bin" ||
            Hex(definition.attribute("ArenaSize").as_string()) != TrampolineSize)
            Reject("Runtime cheat XML does not match the mounted title, version and SHA256.");
        for (const auto name : {"Data", "Relocations", "Controls", "Hooks"}) {
            const auto section = definition.child(name);
            if (!section || section.next_sibling(name)) Reject("Missing or duplicate runtime XML section.");
        }
        for (const auto node : definition.children()) {
            if (node.type() != pugi::node_element) continue;
            const std::string_view name = node.name();
            if (name != "Data" && name != "Code" && name != "Relocations" &&
                name != "Controls" && name != "Hooks") Reject("Unknown runtime XML section.");
        }

        const auto data = definition.child("Data");
        const auto data_size = Hex(data.attribute("Size").as_string());
        Bounds(0, data_size, TrampolineSize);
        std::vector<u8> payload(TrampolineSize, 0);
        std::vector<Range> data_ranges, code_ranges, relocations, controls, hook_ranges;
        for (const auto line : data.children("Line")) {
            const auto offset = Hex(line.attribute("Offset").as_string());
            const auto bytes = Bytes(line.attribute("Value").as_string());
            Bounds(offset, bytes.size(), data_size);
            AddRange(data_ranges, offset, bytes.size());
            std::memcpy(payload.data() + offset, bytes.data(), bytes.size());
        }
        for (const auto code : definition.children("Code")) {
            const auto offset = Hex(code.attribute("Offset").as_string());
            const auto bytes = Bytes(code.attribute("Value").as_string());
            Bounds(offset, bytes.size(), payload.size());
            if (offset < data_size) Reject("XML code overlaps its control data.");
            AddRange(code_ranges, offset, bytes.size());
            std::memcpy(payload.data() + offset, bytes.data(), bytes.size());
        }
        const auto in_code = [&](u64 offset, u64 size) {
            return std::ranges::any_of(code_ranges, [&](const Range& range) {
                return offset >= range.begin && offset < range.end && size <= range.end - offset;
            });
        };
        if (code_ranges.empty()) Reject("Runtime cheat XML has no code.");
        for (const auto relocation : definition.child("Relocations").children("Relocation")) {
            const auto offset = Hex(relocation.attribute("Offset").as_string());
            const std::string_view type = relocation.attribute("Type").as_string();
            const auto width = type == "abs64" ? 8u : type == "rel32" ? 4u : 0u;
            Bounds(offset, width, payload.size());
            if (!in_code(offset, width)) Reject("XML relocation lies outside code.");
            AddRange(relocations, offset, width);
            if (!std::all_of(payload.begin() + offset, payload.begin() + offset + width,
                             [](u8 byte) { return byte == 0; })) Reject("XML relocation placeholder is not zero.");
            const std::string_view space = relocation.attribute("Space").as_string();
            std::uintptr_t target{};
            if (space == "image") {
                target = base + ImageOffset(relocation.attribute("Target"), image_size);
            } else if (space == "arena") {
                const auto target_offset = Hex(relocation.attribute("Target").as_string());
                Bounds(target_offset, 1, payload.size());
                if (target_offset >= data_size && !in_code(target_offset, 1)) Reject("XML arena target is not defined.");
                target = arena_base + target_offset;
            } else Reject("Unknown XML relocation address space.");
            if (width == 8) {
                const auto value = static_cast<u64>(target);
                std::memcpy(payload.data() + offset, &value, sizeof(value));
            } else {
                // Schema 1 RIP-relative instructions end immediately after disp32.
                const auto value = Relative(arena_base + offset + width, target);
                std::memcpy(payload.data() + offset, &value, sizeof(value));
            }
        }
        auto active = std::make_shared<Runtime>();
        active->arena = static_cast<u8*>(arena);
        std::array<bool, ControlIds.size()> found{};
        for (const auto entry : definition.child("Controls").children("Control")) {
            const auto id = std::ranges::find(ControlIds, std::string_view(entry.attribute("Id").as_string()));
            if (id == ControlIds.end()) Reject("Unknown XML menu control.");
            const auto index = static_cast<std::size_t>(id - ControlIds.begin());
            if (std::exchange(found[index], true)) Reject("Duplicate XML menu control.");
            auto& control = active->controls[index];
            const auto offset = Hex(entry.attribute("Offset").as_string());
            Bounds(offset, sizeof(u32), data_size);
            if (offset % alignof(u32)) Reject("XML control is not aligned.");
            AddRange(controls, offset, sizeof(u32));
            control.offset = static_cast<std::size_t>(offset);
            const std::string_view mode = entry.attribute("Mode").as_string();
            control.generation = mode == "generation";
            if ((index < 2 && !control.generation) || (index == 2 && mode != "cycle"))
                Reject("XML menu control mode does not match the menu.");
            u32 initial{};
            std::memcpy(&initial, payload.data() + offset, sizeof(initial));
            if (control.generation && initial != 0) Reject("XML toggle must initially be off.");
            for (const auto value : entry.children("Value")) {
                const auto bytes = Bytes(value.attribute("Bytes").as_string());
                if (bytes.size() != sizeof(u32) || control.generation) Reject("Invalid XML control value.");
                Choice choice{};
                std::memcpy(&choice.bits, bytes.data(), sizeof(choice.bits));
                choice.display = value.attribute("Display").as_uint();
                if (!choice.display || std::ranges::find(control.choices, choice.bits, &Choice::bits) != control.choices.end())
                    Reject("Invalid or duplicate XML control choice.");
                control.choices.push_back(choice);
            }
            if (!control.generation && (control.choices.size() < 2 ||
                control.choices.front().bits != initial || control.choices.front().display != 1))
                Reject("XML cycle must initially select its off value.");
        }
        if (!std::ranges::all_of(found, [](bool value) { return value; })) Reject("Missing XML menu control.");
        std::vector<ImageWrite> writes;
        for (const auto hook : definition.child("Hooks").children("Hook")) {
            const auto offset = ImageOffset(hook.attribute("Address"), image_size);
            auto original = Bytes(hook.attribute("Original").as_string());
            Bounds(offset, original.size(), image_size);
            if (original.size() < 5 || original.size() > 32) Reject("Invalid XML hook length.");
            AddRange(hook_ranges, offset, original.size());
            const auto target = Hex(hook.attribute("Target").as_string());
            if (!in_code(target, 1)) Reject("XML hook target lies outside code.");
            const std::string_view kind = hook.attribute("Kind").as_string();
            if (kind != "call" && kind != "jump") Reject("Unknown XML hook kind.");
            auto replacement = std::vector<u8>(original.size(), 0x90);
            replacement[0] = kind == "call" ? 0xe8 : 0xe9;
            const auto relative = Relative(base + offset + 5, arena_base + target);
            std::memcpy(replacement.data() + 1, &relative, sizeof(relative));
            if (std::memcmp(reinterpret_cast<const void*>(base + offset), original.data(), original.size()) != 0)
                Reject("XML cheat preflight failed: original game bytes differ.");
            writes.push_back({offset, std::move(original), std::move(replacement)});
        }
        if (writes.empty()) Reject("Runtime cheat XML has no hooks.");
        const auto* reserved = static_cast<const u8*>(arena);
        if (!std::all_of(reserved, reserved + TrampolineSize, [](u8 byte) { return byte == 0; }))
            Reject("XML cheat arena is already occupied.");
        // Complete preflight before the first live write; no allocation/parsing follows.
        std::memcpy(arena, payload.data(), payload.size());
        for (const auto& control : active->controls) {
            u32 initial{};
            std::memcpy(&initial, payload.data() + control.offset, sizeof(initial));
            std::construct_at(reinterpret_cast<u32*>(active->arena + control.offset), initial);
        }
        for (const auto& write : writes)
            std::memcpy(reinterpret_cast<void*>(base + write.offset), write.replacement.data(), write.replacement.size());
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(base), image_size);
        FlushInstructionCache(GetCurrentProcess(), arena, payload.size());
        runtime.store(std::move(active), std::memory_order_release);
        status.store(Status::Ready, std::memory_order_release);
        LOG_INFO(Loader, "Second Son runtime cheat XML installed; all options initially off.");
    } catch (const std::exception& error) {
        status.store(Status::Rejected, std::memory_order_release);
        LOG_ERROR(Loader, "Second Son runtime cheat XML disabled: {}", error.what());
    }
}
} // namespace Core::SecondSon::Cheats
