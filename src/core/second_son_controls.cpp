// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/second_son_controls.h"
#include "core/second_son_chords.h"
#include "core/file_sys/ifile.h"
#include "common/scope_exit.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "common/singleton.h"
#include "input/controller.h"
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xbyak/xbyak.h>
#include <windows.h>
#include <bcrypt.h>

namespace Core::SecondSon {
namespace {
bool verified_build{}, want_prompts{}, want_chords{}, prompts_active{};
struct ControllerChords {
    ChordSide left, right;
    unsigned contacts{};
    Input::GamepadDevice device;
    s32 handle{-1};
};
std::mutex chord_mutex;
std::unordered_map<const u8*, ControllerChords> chord_states;

using Converter = PS4_SYSV_ABI void (*)(u8*);
using Coordinate = PS4_SYSV_ABI float (*)(u8*, int);
using SecondCoordinate = PS4_SYSV_ABI float (*)(u8*);
using Normalize = PS4_SYSV_ABI float (*)(const u8*, float);
Converter original_converter{};
Coordinate original_x{}, original_y{};
SecondCoordinate original_second_x{}, original_second_y{};
Normalize normalize{};

template <class T>
T Read(const u8* object, std::size_t offset) {
    T value;
    std::memcpy(&value, object + offset, sizeof(value));
    return value;
}
template <class T>
void Write(u8* object, std::size_t offset, T value) {
    std::memcpy(object + offset, &value, sizeof(value));
}
unsigned Contacts(const u8* object) {
    const auto handle = Read<s32>(object, 0xc8);
    const auto device = Libraries::Pad::GetControllerDevice(handle);
    if (device.family != Input::GamepadFamily::Xbox) {
        return 0;
    }
    std::scoped_lock lock{chord_mutex};
    const auto found = chord_states.find(object);
    return found == chord_states.end() || found->second.device != device ||
                   found->second.handle != handle ? 0 : found->second.contacts;
}

// This replaces the game's own converter call, not libScePad or the SDL input report.
void PS4_SYSV_ABI Convert(u8* object) {
    original_converter(object);
    const auto handle = Read<s32>(object, 0xc8);
    const auto device = Libraries::Pad::GetControllerDevice(handle);
    std::scoped_lock lock{chord_mutex};
    if (device.family != Input::GamepadFamily::Xbox ||
        Read<s32>(object, 0x184) != 0 || handle < 0 ||
        Read<u8>(object, 0x11c) == 0 || Read<u32>(object, 0x7f0) == 2) {
        chord_states.erase(object); // Locked, disconnected, or unidentified alternate input mode.
        return;
    }
    auto& state = chord_states[object];
    if (state.device != device || state.handle != handle) {
        state = {};
        state.device = device;
        state.handle = handle;
    }
    const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto raw = Read<u32>(object, 0xd0);
    auto buttons = Read<u32>(object, 0x20);
    const auto left = state.left.Step((raw & 0x400) != 0, Read<u8>(object, 0xd8),
                                     buttons & 0x300, Read<float>(object, 0x18c), now);
    const auto right = state.right.Step((raw & 0x800) != 0, Read<u8>(object, 0xd9),
                                       buttons & 0xc00, Read<float>(object, 0x190), now);
    buttons = (buttons & ~0xf00u) | left.buttons | right.buttons;
    state.contacts = (left.touch ? 1u : 0u) | (right.touch ? 2u : 0u);
    if (state.contacts) {
        buttons |= 0x50000; // Game contact + click predicates.
        if (state.contacts == 3) {
            buttons |= 0x20000; // Game two-finger click predicate.
        }
    }
    Write(object, 0x20, buttons);
    Write(object, 0x18c, left.trigger);
    Write(object, 0x190, right.trigger);
    // Raw OrbisPadData, controller configuration and real touch reports remain untouched.
}

int PS4_SYSV_ABI TouchCount(u8* object) {
    const auto contacts = Contacts(object);
    return contacts ? (contacts == 3 ? 2 : 1) : Read<u8>(object, 0x104);
}
float ChordCoordinate(u8* object, int index, unsigned contacts, bool horizontal) {
    const int count = contacts == 3 ? 2 : 1;
    if (index < 0 || index >= count) {
        return 0;
    }
    const bool right = contacts == 2 || (contacts == 3 && index == 1);
    const float raw = horizontal ? (right ? 1440.f : 480.f) : 475.f;
    return normalize(object + (horizontal ? 0x164 : 0x174), raw);
}
float PS4_SYSV_ABI TouchX(u8* object, int index) {
    const auto contacts = Contacts(object);
    return contacts ? ChordCoordinate(object, index, contacts, true) : original_x(object, index);
}
float PS4_SYSV_ABI TouchY(u8* object, int index) {
    const auto contacts = Contacts(object);
    return contacts ? ChordCoordinate(object, index, contacts, false) : original_y(object, index);
}
float PS4_SYSV_ABI FirstX(u8* object) { return TouchX(object, 0); }
float PS4_SYSV_ABI FirstY(u8* object) { return TouchY(object, 0); }
float PS4_SYSV_ABI SecondX(u8* object) {
    const auto contacts = Contacts(object);
    return contacts ? ChordCoordinate(object, 1, contacts, true) : original_second_x(object);
}
float PS4_SYSV_ABI SecondY(u8* object) {
    const auto contacts = Contacts(object);
    return contacts ? ChordCoordinate(object, 1, contacts, false) : original_second_y(object);
}

struct ImageWrite {
    std::size_t offset;
    std::vector<u8> original, replacement;
};
std::vector<u8> Bytes(std::initializer_list<u8> value) { return value; }

void Check(std::uintptr_t base, std::size_t size, const ImageWrite& write) {
    if (write.original.empty() || write.original.size() != write.replacement.size() ||
        write.offset > size || write.original.size() > size - write.offset ||
        std::memcmp(reinterpret_cast<const void*>(base + write.offset), write.original.data(),
                    write.original.size()) != 0) {
        throw std::runtime_error("Second Son controls patch rejected: original bytes differ.");
    }
}
std::vector<u8> Branch(std::uintptr_t source, std::uintptr_t target, std::size_t size, bool call) {
    const auto distance = static_cast<s64>(target) - static_cast<s64>(source + 5);
    if (size < 5 || distance < std::numeric_limits<s32>::min() ||
        distance > std::numeric_limits<s32>::max()) {
        throw std::runtime_error("Second Son controls trampoline is out of range.");
    }
    std::vector<u8> bytes(size, 0x90);
    bytes[0] = call ? 0xe8 : 0xe9;
    const auto relative = static_cast<s32>(distance);
    std::memcpy(bytes.data() + 1, &relative, sizeof(relative));
    return bytes;
}
} // namespace

std::string HashFile(FileSys::IFile& file) {
    const auto original_position = file.Tell();
    if (!file.Seek(0, Common::FS::SeekOrigin::SetOrigin) || file.Size() == 0) {
        throw std::runtime_error("Cannot read the mounted game executable.");
    }
    SCOPE_EXIT { file.Seek(original_position, Common::FS::SeekOrigin::SetOrigin); };
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("Cannot initialize SHA-256.");
    }
    SCOPE_EXIT { BCryptCloseAlgorithmProvider(algorithm, 0); };
    DWORD size{}, returned{};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size),
                          sizeof(size), &returned, 0) < 0) {
        throw std::runtime_error("Cannot query SHA-256 storage.");
    }
    std::vector<unsigned char> object(size);
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) < 0) {
        throw std::runtime_error("Cannot create SHA-256.");
    }
    SCOPE_EXIT { BCryptDestroyHash(hash); };
    std::array<unsigned char, 65536> buffer{};
    u64 remaining = file.Size();
    while (remaining != 0) {
        const auto wanted = std::min<u64>(remaining, buffer.size());
        const auto count = file.Read(buffer.data(), wanted);
        if (count <= 0 || static_cast<u64>(count) > wanted ||
            BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0) {
            throw std::runtime_error("Cannot hash the mounted game executable.");
        }
        remaining -= count;
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), digest.size(), 0) < 0) {
        throw std::runtime_error("Cannot finish SHA-256.");
    }
    constexpr std::string_view hex = "0123456789abcdef";
    std::string actual;
    actual.reserve(64);
    for (const auto byte : digest) {
        actual += hex[byte >> 4];
        actual += hex[byte & 15];
    }
    return actual;
}

void ConfigureControls(std::string_view title, std::string_view version, std::string_view sha256,
                       bool xbox_prompts, bool touchpad_chords) {
    verified_build = title == "CUSA00309" && version == "01.07" && sha256 == ControlsFingerprint;
    want_prompts = verified_build && xbox_prompts;
    want_chords = verified_build && touchpad_chords;
    prompts_active = false;
    std::scoped_lock lock{chord_mutex};
    chord_states.clear();
}
bool IsVerifiedBuild() { return verified_build; }
bool XboxPromptsEnabled() { return prompts_active; }
bool TouchpadChordsEnabled() { return prompts_active && want_chords; }

void InstallControls(std::uintptr_t base, std::size_t image_size, void* arena,
                     std::size_t arena_size) {
    if (!want_chords && !want_prompts) {
        return;
    }
    // HLE registration has already assigned devices to players. Never pick an arbitrary
    // connected Xbox controller when player 1 is actually using a PlayStation controller.
    const auto device = (*Common::Singleton<Input::GameControllers>::Instance())[0]->GetDevice();
    const bool xbox = device.family == Input::GamepadFamily::Xbox;
    want_prompts &= xbox;
    want_chords &= xbox;
    LOG_INFO(Loader, "Player 1 controller: {}; Xbox labels={}, touchpad chords={}. "
                     "Restart the game after changing controller families.",
             Input::GamepadFamilyName(device.family), want_prompts, want_chords);
    if (!want_chords && !want_prompts) {
        return; // PS, unknown controllers and keyboard-only sessions receive no input/UI hooks.
    }
    std::vector<ImageWrite> writes;
    Xbyak::CodeGenerator code(arena_size, arena);
    const auto thunk = [&](auto function) {
        code.align(16);
        const auto address = reinterpret_cast<std::uintptr_t>(code.getCurr());
        code.jmp(code.ptr[code.rip]);
        code.dq(reinterpret_cast<std::uintptr_t>(function));
        return address;
    };
    // Prologues contain no relative references; replay whole instructions in the trampoline.
    const auto hook = [&](std::size_t offset, std::vector<u8> before, auto function) {
        ImageWrite write{offset, before, Branch(base + offset, thunk(function), before.size(), false)};
        Check(base, image_size, write);
        code.align(16);
        const auto original = reinterpret_cast<std::uintptr_t>(code.getCurr());
        code.db(before.data(), before.size());
        code.jmp(reinterpret_cast<const void*>(base + offset + before.size()));
        writes.push_back(std::move(write));
        return original;
    };
    if (want_chords) {
        original_converter = reinterpret_cast<Converter>(base + 0x24b340);
        normalize = reinterpret_cast<Normalize>(base + 0x618c40);
        ImageWrite call{0x24a321, Bytes({0xe8, 0x1a, 0x10, 0, 0}),
                        Branch(base + 0x24a321, thunk(Convert), 5, true)};
        Check(base, image_size, call);
        writes.push_back(std::move(call));
        const auto xy = Bytes({0x55, 0x48, 0x89, 0xe5, 0x41, 0x56});
        original_x = reinterpret_cast<Coordinate>(hook(0x24da90, xy, TouchX));
        original_y = reinterpret_cast<Coordinate>(hook(0x24dae0, xy, TouchY));
        const auto second = Bytes({0x55, 0x48, 0x89, 0xe5, 0x53, 0x50});
        hook(0x24e8a0, second, FirstX);
        hook(0x24e8f0, second, FirstY);
        original_second_x = reinterpret_cast<SecondCoordinate>(hook(0x24e940, second, SecondX));
        original_second_y = reinterpret_cast<SecondCoordinate>(hook(0x24e990, second, SecondY));
        const auto count = Bytes({0x0f, 0xb6, 0x87, 0x04, 0x01, 0, 0});
        writes.push_back({0x24e9e0, count,
                          Branch(base + 0x24e9e0, thunk(TouchCount), count.size(), false)});
    }
    // Preflight the entire group before changing the loaded game image.
    for (std::size_t i = 0; i < writes.size(); ++i) {
        Check(base, image_size, writes[i]);
        for (std::size_t j = 0; j < i; ++j) {
            if (writes[i].offset < writes[j].offset + writes[j].replacement.size() &&
                writes[j].offset < writes[i].offset + writes[i].replacement.size()) {
                throw std::runtime_error("Second Son controls patch writes overlap.");
            }
        }
    }
    code.ready();
    for (const auto& write : writes) {
        std::memcpy(reinterpret_cast<void*>(base + write.offset), write.replacement.data(),
                    write.replacement.size());
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(base), image_size);
    FlushInstructionCache(GetCurrentProcess(), arena, code.getSize());
    prompts_active = want_prompts;
    LOG_INFO(Loader, "Second Son game-side controls: Xbox labels={}, touchpad chords={}",
             want_prompts, want_chords);
}
} // namespace Core::SecondSon
