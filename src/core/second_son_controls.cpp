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
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xbyak/xbyak.h>
#include <windows.h>
#include <bcrypt.h>

namespace Core::SecondSon {
namespace {
bool verified_build{}, want_prompts{}, want_chords{}, prompts_active{};
alignas(8) volatile LONG64 feedback_arena_moves[2]{};
alignas(8) volatile LONG64 feedback_arena_group_moves[32]{};
constexpr u32 FeedbackSideBytes = 30 * 1024 * 1024;
constexpr u32 FeedbackWindowBytes = 512 * 1024;
constexpr u32 FeedbackRecordPage = 0xd00000;
constexpr u32 FeedbackPoolStride = 256;
constexpr u32 FeedbackPoolCapacity = FeedbackWindowBytes / FeedbackPoolStride;
static_assert(FeedbackPoolCapacity * FeedbackPoolStride == FeedbackWindowBytes);
struct alignas(64) FeedbackPoolState {
    volatile LONG next[2]{};
    volatile LONG peak[2]{};
    alignas(8) volatile LONG64 moves[2]{}, overflows[2]{}, resets[2]{};
};
FeedbackPoolState feedback_pool;
volatile LONG feedback_pool_group_records{};
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

void InstallParticlePollPause(std::uintptr_t base, std::size_t image_size, void* arena) {
    const char* enabled = std::getenv("IFSS_PARTICLE_POLL_PAUSE");
    if (!verified_build || !enabled || std::string_view{enabled} != "1") {
        return;
    }
    constexpr std::size_t call = 0x169615;
    constexpr std::size_t original = 0x53b2b0;
    constexpr std::size_t scale = 0x7f78db8;
    constexpr std::size_t initialized = 0x7f78dc0;
    if (image_size <= initialized) {
        throw std::runtime_error("Second Son particle poll data is outside the image.");
    }
    Xbyak::CodeGenerator code(ParticlePollTrampolineSize, arena);
    Xbyak::Label fallback, loop;
    code.cmp(code.edi, 0x32);
    code.jne(fallback);
    code.mov(code.rax, base + initialized);
    code.cmp(code.byte[code.rax], 1);
    code.jne(fallback);
    code.mov(code.rax, base + scale);
    code.mov(code.ecx, code.edi);
    code.imul(code.rcx, code.qword[code.rax]);
    code.rdtsc();
    code.mov(code.eax, code.eax);
    code.shl(code.rdx, 32);
    code.or_(code.rdx, code.rax);
    code.add(code.rcx, code.rdx);
    code.L(loop);
    code.pause();
    code.rdtsc();
    code.mov(code.eax, code.eax);
    code.shl(code.rdx, 32);
    code.or_(code.rdx, code.rax);
    code.cmp(code.rdx, code.rcx);
    code.jl(loop);
    code.ret();
    code.L(fallback);
    code.jmp(reinterpret_cast<const void*>(base + original));
    code.ready();

    const ImageWrite write{call, Bytes({0xe8, 0x96, 0x1c, 0x3d, 0x00}),
                           Branch(base + call, reinterpret_cast<std::uintptr_t>(arena), 5, true)};
    Check(base, image_size, write);
    FlushInstructionCache(GetCurrentProcess(), arena, code.getSize());
    std::memcpy(reinterpret_cast<void*>(base + call), write.replacement.data(),
                write.replacement.size());
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(base + call), 5);
    LOG_INFO(Loader, "Particle poll PAUSE experiment: verified call replacement");
}

std::array<std::uint64_t, 2> FeedbackArenaMoves() {
    return {
        static_cast<u64>(InterlockedCompareExchange64(&feedback_arena_moves[0], 0, 0)),
        static_cast<u64>(InterlockedCompareExchange64(&feedback_arena_moves[1], 0, 0)),
    };
}

std::array<std::uint64_t, 32> FeedbackArenaGroupMoves() {
    std::array<std::uint64_t, 32> result{};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<u64>(InterlockedCompareExchange64(&feedback_arena_group_moves[i], 0, 0));
    }
    return result;
}

FeedbackPoolCounts FeedbackArenaPoolCounts() {
    FeedbackPoolCounts result;
    result.capacity = FeedbackPoolCapacity;
    result.stride = FeedbackPoolStride;
    result.group_records = static_cast<u32>(InterlockedCompareExchange(&feedback_pool_group_records, 0, 0));
    for (size_t side = 0; side < 2; ++side) {
        result.moves[side] = static_cast<u64>(InterlockedCompareExchange64(&feedback_pool.moves[side], 0, 0));
        result.overflows[side] = static_cast<u64>(InterlockedCompareExchange64(&feedback_pool.overflows[side], 0, 0));
        result.used[side] = static_cast<u32>(InterlockedCompareExchange(&feedback_pool.peak[side], 0, 0));
        result.resets[side] = static_cast<u64>(InterlockedCompareExchange64(&feedback_pool.resets[side], 0, 0));
    }
    return result;
}

namespace {

void EmitFeedbackPoolClear(Xbyak::CodeGenerator& code, u64 state) {
    static_assert(sizeof(FeedbackPoolState) % 8 == 0);
    code.mov(code.r10, code.rax);
    code.mov(code.rdi, state);
    code.xor_(code.eax, code.eax);
    code.mov(code.ecx, sizeof(FeedbackPoolState) / 8);
    code.rep();
    code.stosq();
    code.mov(code.rax, code.r10);
}

// Called only before the original side initializer publishes its first reservation cursor.
// The original reset/side-flip synchronization owns this boundary, not the recording worker.
void EmitFeedbackPoolEpoch(Xbyak::CodeGenerator& code, u64 state) {
    code.mov(code.r11, state);
    code.mov(code.dword[code.r11 + code.rcx * 4 + offsetof(FeedbackPoolState, next)], 0);
    code.lock();
    code.inc(code.qword[code.r11 + code.rcx * 8 + offsetof(FeedbackPoolState, resets)]);
}

// Each successful original reservation invokes this once. A private slot is owned until
// that same original side is reset and initialized again; it is never recycled earlier.
// RAX/R10 supply the original reservation/allocation. Preserve RCX/RDX/R8 through done.
void EmitFeedbackPoolMapping(Xbyak::CodeGenerator& code, Xbyak::Label& done, u64 state,
                             u32 group_records) {
    constexpr u32 next = offsetof(FeedbackPoolState, next);
    constexpr u32 peak = offsetof(FeedbackPoolState, peak);
    constexpr u32 moves = offsetof(FeedbackPoolState, moves);
    constexpr u32 overflows = offsetof(FeedbackPoolState, overflows);
    constexpr u32 resets = offsetof(FeedbackPoolState, resets);
    static_assert(std::has_single_bit(FeedbackPoolCapacity));
    Xbyak::Label update_peak, peak_done, exhausted, rejected;
    code.push(code.rax);
    code.push(code.rcx);
    code.push(code.rdx);
    code.push(code.r8);
    code.mov(code.rdx, code.rax);
    code.sub(code.rdx, code.r10);
    code.cmp(code.rdx, 2 * FeedbackSideBytes);
    code.jae(rejected, code.T_NEAR);
    code.test(code.rdx, 0xff);
    code.jnz(rejected, code.T_NEAR);
    code.xor_(code.ecx, code.ecx);
    code.cmp(code.rdx, FeedbackSideBytes);
    code.setae(code.cl);
    code.mov(code.r11, state);
    code.cmp(code.qword[code.r11 + code.rcx * 8 + resets], 0);
    code.je(rejected, code.T_NEAR); // An unobserved initializer never authorizes reuse.
    code.mov(code.eax, 1);
    code.lock();
    code.xadd(code.dword[code.r11 + code.rcx * 4 + next], code.eax);
    code.cmp(code.eax, FeedbackPoolCapacity);
    code.jae(exhausted, code.T_NEAR);
    code.mov(code.edx, code.eax); // Unique slot, independent of worker completion order.
    code.lea(code.r8d, code.ptr[code.eax + 1]);
    code.mov(code.eax, code.dword[code.r11 + code.rcx * 4 + peak]);
    code.L(update_peak);
    code.cmp(code.eax, code.r8d);
    code.jae(peak_done);
    code.lock();
    code.cmpxchg(code.dword[code.r11 + code.rcx * 4 + peak], code.r8d);
    code.jne(update_peak);
    code.L(peak_done);
    code.lock();
    code.inc(code.qword[code.r11 + code.rcx * 8 + moves]);
    code.shl(code.rcx, std::countr_zero(FeedbackPoolCapacity));
    code.add(code.rcx, code.rdx);
    code.mov(code.r8, code.rcx);
    code.and_(code.r8, group_records - 1);
    code.shl(code.r8, std::countr_zero(FeedbackPoolStride));
    code.shr(code.rcx, std::countr_zero(group_records));
    code.shl(code.rcx, std::countr_zero(FeedbackWindowBytes));
    code.add(code.rcx, code.r8);
    code.lea(code.rax, code.ptr[code.r10 + 2 * FeedbackSideBytes + FeedbackWindowBytes - 1]);
    code.and_(code.rax, -static_cast<s32>(FeedbackWindowBytes));
    code.add(code.rax, code.rcx);
    code.pop(code.r8);
    code.pop(code.rdx);
    code.pop(code.rcx);
    code.add(code.rsp, 8);
    code.jmp(done, code.T_NEAR);
    code.L(exhausted);
    code.lock();
    code.inc(code.qword[code.r11 + code.rcx * 8 + overflows]);
    code.L(rejected);
    code.pop(code.r8);
    code.pop(code.rdx);
    code.pop(code.rcx);
    code.pop(code.rax);
    code.jmp(done, code.T_NEAR);
}

void EmitFeedbackPoolInitializer(Xbyak::CodeGenerator& code, u64 manager,
                                 Xbyak::Label& allocation, u64 state) {
    Xbyak::Label done;
    code.push(code.rax);
    code.push(code.rcx);
    code.push(code.r10);
    code.push(code.r11);
    code.mov(code.r10, manager);
    code.lea(code.r11, code.ptr[code.r10 + 8]);
    code.cmp(code.rdi, code.r11);
    code.jne(done, code.T_NEAR);
    code.cmp(code.edx, FeedbackSideBytes);
    code.jne(done, code.T_NEAR);
    code.cmp(code.qword[code.rdi + 8], 0);
    code.jne(done, code.T_NEAR); // The original initializer is a no-op with a live cursor.
    code.mov(code.r11, code.qword[code.rip + allocation]);
    code.test(code.r11, code.r11);
    code.jz(done, code.T_NEAR);
    code.cmp(code.r11, code.qword[code.r10]);
    code.jne(done, code.T_NEAR);
    code.mov(code.ecx, code.dword[code.r10 + 0x28]);
    code.cmp(code.ecx, 1);
    code.ja(done, code.T_NEAR);
    code.imul(code.rax, code.rcx, FeedbackSideBytes);
    code.add(code.rax, code.r11);
    code.cmp(code.rsi, code.rax);
    code.jne(done, code.T_NEAR);
    EmitFeedbackPoolEpoch(code, state);
    code.L(done);
    code.pop(code.r11);
    code.pop(code.r10);
    code.pop(code.rcx);
    code.pop(code.rax);
}

void CheckFeedbackPoolMapping(u32 group_records) {
    if (!std::has_single_bit(group_records) || group_records > FeedbackPoolCapacity) {
        throw std::runtime_error("Unsupported feedback pool group geometry.");
    }
    FeedbackPoolState fixture{};
    const auto state = reinterpret_cast<u64>(&fixture);
    Xbyak::CodeGenerator probe(1024);
    Xbyak::Label done;
    probe.mov(probe.rax, probe.rcx);
    probe.mov(probe.r10, probe.rdx);
    EmitFeedbackPoolMapping(probe, done, state, group_records);
    probe.L(done);
    probe.ret();
    probe.ready();
    const auto run = probe.getCode<u64 (*)(u64, u64)>();
    Xbyak::CodeGenerator clear(128);
    clear.push(clear.rdi); // The probe is called through the Windows ABI.
    clear.mov(clear.rax, 0x12340000);
    EmitFeedbackPoolClear(clear, state);
    clear.pop(clear.rdi);
    clear.ret();
    clear.ready();
    const auto reset = clear.getCode<u64 (*)()>();
    Xbyak::CodeGenerator begin(128);
    EmitFeedbackPoolEpoch(begin, state); // Windows RCX supplies the side index.
    begin.ret();
    begin.ready();
    const auto epoch = begin.getCode<void (*)(u64)>();
    constexpr u64 allocation = 0x200030000ULL;
    constexpr u64 first = (allocation + 2 * FeedbackSideBytes + FeedbackWindowBytes - 1) &
                          ~(u64{FeedbackWindowBytes} - 1);
    const auto private_pointer = [=](u32 index) {
        return first + (index / group_records) * u64{FeedbackWindowBytes} +
               (index % group_records) * FeedbackPoolStride;
    };
    const auto original = [](u32 index) {
        return allocation + (index / FeedbackPoolCapacity) * FeedbackSideBytes +
               (index % FeedbackPoolCapacity) * 59 * 256;
    };
    if (run(original(0), allocation) != original(0) || fixture.next[0]) {
        throw std::runtime_error("Feedback pool missing initializer rejection failed.");
    }
    epoch(0);
    epoch(1);
    for (u32 index = 0; index < 2 * FeedbackPoolCapacity; ++index) {
        const u64 wanted = private_pointer(index);
        if (run(original(index), allocation) != wanted) {
            throw std::runtime_error("Feedback pool unique reservation check failed.");
        }
    }
    for (u32 side = 0; side < 2; ++side) {
        const u64 pointer = allocation + (side + 1) * FeedbackSideBytes - 256;
        if (run(pointer, allocation) != pointer || fixture.overflows[side] != 1) {
            throw std::runtime_error("Feedback pool capacity fallback check failed.");
        }
        epoch(side);
        if (fixture.next[side] || fixture.resets[side] != 2 ||
            fixture.peak[side] != FeedbackPoolCapacity ||
            run(pointer, allocation) != private_pointer(side * FeedbackPoolCapacity)) {
            throw std::runtime_error("Feedback pool original-side reuse check failed.");
        }
    }
    for (u64 pointer : {allocation - 256, allocation + 1, allocation + 2 * FeedbackSideBytes}) {
        if (run(pointer, allocation) != pointer) {
            throw std::runtime_error("Feedback pool range rejection check failed.");
        }
    }
    if (reset() != 0x12340000 || fixture.next[0] || fixture.peak[1] ||
        fixture.moves[0] || fixture.overflows[1] || fixture.resets[0]) {
        throw std::runtime_error("Feedback pool new allocation reset check failed.");
    }
    epoch(0);
    epoch(1);
    std::array<u64, 2 * FeedbackPoolCapacity> pointers{};
    constexpr u32 ThreadCount = 8;
    std::array<std::jthread, ThreadCount> threads;
    for (u32 thread = 0; thread < threads.size(); ++thread) {
        threads[thread] = std::jthread([&, thread] {
            for (u32 index = thread; index < pointers.size(); index += ThreadCount) {
                pointers[index] = run(original(index), allocation);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::array<bool, 2 * FeedbackPoolCapacity> seen{};
    for (u32 index = 0; index < pointers.size(); ++index) {
        const u64 pointer = pointers[index];
        const u64 offset = pointer - first;
        const u64 inside = offset % FeedbackWindowBytes;
        const u64 slot = (offset / FeedbackWindowBytes) * group_records + inside / FeedbackPoolStride;
        if (pointer < first || inside % FeedbackPoolStride || inside / FeedbackPoolStride >= group_records ||
            slot >= seen.size() || seen[slot] ||
            slot / FeedbackPoolCapacity != index / FeedbackPoolCapacity) {
            throw std::runtime_error("Feedback pool concurrent unique reservation check failed.");
        }
        seen[slot] = true;
    }
    if (fixture.next[0] != FeedbackPoolCapacity || fixture.next[1] != FeedbackPoolCapacity ||
        fixture.peak[0] != FeedbackPoolCapacity || fixture.peak[1] != FeedbackPoolCapacity ||
        fixture.moves[0] != FeedbackPoolCapacity || fixture.moves[1] != FeedbackPoolCapacity ||
        fixture.overflows[0] || fixture.overflows[1]) {
        throw std::runtime_error("Feedback pool concurrent capacity check failed.");
    }
    // Replay the exact guarded initializer, without publishing or modifying any guest memory.
    std::array<u64, 6> manager{};
    manager[0] = allocation;
    Xbyak::CodeGenerator initialize(512);
    Xbyak::Label allocation_value;
    initialize.push(initialize.rdi);
    initialize.push(initialize.rsi);
    initialize.mov(initialize.rdi, initialize.rcx);
    initialize.mov(initialize.rsi, initialize.rdx);
    initialize.mov(initialize.edx, initialize.r8d);
    EmitFeedbackPoolInitializer(initialize, reinterpret_cast<u64>(manager.data()),
                                allocation_value, state);
    initialize.pop(initialize.rsi);
    initialize.pop(initialize.rdi);
    initialize.ret();
    initialize.align(8);
    initialize.L(allocation_value);
    initialize.dq(allocation);
    initialize.ready();
    const auto init = initialize.getCode<void (*)(u64, u64, u64)>();
    const u64 argument = reinterpret_cast<u64>(manager.data()) + 8;
    init(argument + 8, allocation, FeedbackSideBytes); // Another manager / wrong subobject.
    init(argument, allocation, FeedbackSideBytes - 1);
    manager[2] = allocation + 256; // A live cursor makes the original initializer a no-op.
    init(argument, allocation, FeedbackSideBytes);
    manager[2] = 0;
    init(argument, allocation + 256, FeedbackSideBytes);
    manager[5] = 2;
    init(argument, allocation, FeedbackSideBytes);
    manager[5] = 0;
    manager[0] = 0;
    init(argument, allocation, FeedbackSideBytes);
    manager[0] = allocation;
    if (fixture.resets[0] != 1 || fixture.resets[1] != 1) {
        throw std::runtime_error("Feedback pool initializer guard rejection failed.");
    }
    init(argument, allocation, FeedbackSideBytes);
    manager[5] = 1;
    init(argument, allocation + FeedbackSideBytes, FeedbackSideBytes);
    if (fixture.resets[0] != 2 || fixture.resets[1] != 2 ||
        fixture.next[0] || fixture.next[1] || fixture.peak[0] != FeedbackPoolCapacity) {
        throw std::runtime_error("Feedback pool guarded side initializer check failed.");
    }
    LOG_INFO(Loader, "Feedback arena pool: generated reservation, epoch, capacity and concurrency checks passed, group {}", group_records);
}

// Input RAX is the successful original reservation and R10 is the enlarged allocation base.
// On success RAX names its private window and R11 its diagnostic counter. Rejects keep RAX.
void EmitFeedbackGroupMapping(Xbyak::CodeGenerator& code, Xbyak::Label& done) {
    constexpr u32 side_size = FeedbackSideBytes;
    constexpr u32 window = FeedbackWindowBytes;
    constexpr u32 page = FeedbackRecordPage;
    Xbyak::Label side_one, indexed;
    // Encode the original side and page-relative 256 B slot in [0,32). No new
    // reservation is made; each successful original reservation retains its lifetime.
    code.mov(code.r11, code.rax);
    code.sub(code.r11, code.r10);
    code.cmp(code.r11, side_size);
    code.jae(side_one);
    code.sub(code.r11, page);
    code.cmp(code.r11, 0x1000);
    code.jae(done, code.T_NEAR);
    code.jmp(indexed);
    code.L(side_one);
    code.sub(code.r11, side_size + page);
    code.cmp(code.r11, 0x1000);
    code.jae(done, code.T_NEAR);
    code.add(code.r11, 0x1000);
    code.L(indexed);
    code.test(code.r11, 0xff);
    code.jnz(done, code.T_NEAR);
    code.shr(code.r11, 8);
    code.lea(code.rax, code.ptr[code.r10 + 2 * side_size + window - 1]);
    code.and_(code.rax, -static_cast<s32>(window));
    code.mov(code.r10, code.r11);
    code.shl(code.r10, 19);
    code.add(code.rax, code.r10);
    code.mov(code.r10, reinterpret_cast<std::uintptr_t>(&feedback_arena_group_moves[0]));
    code.lea(code.r11, code.ptr[code.r10 + code.r11 * 8]);
}

void CheckFeedbackGroupMapping() {
    // Exercise the exact mapping emitter with numeric guest addresses. The generated probe
    // returns addresses only and never accesses guest memory or changes any move counter.
    Xbyak::CodeGenerator probe(512);
    Xbyak::Label done;
    probe.mov(probe.rax, probe.rcx);
    probe.mov(probe.r10, probe.rdx);
    EmitFeedbackGroupMapping(probe, done);
    probe.L(done);
    probe.mov(probe.qword[probe.r8], probe.rax);
    probe.mov(probe.qword[probe.r8 + 8], probe.r11);
    probe.ret();
    probe.ready();
    struct Result { u64 address, counter; };
    const auto run = probe.getCode<void (*)(u64, u64, Result*)>();
    for (u64 residue = 0; residue < FeedbackWindowBytes; residue += 256) {
        const u64 allocation = 0x200000000ULL + residue;
        const u64 first = (allocation + 2 * FeedbackSideBytes + FeedbackWindowBytes - 1) &
                          ~(u64{FeedbackWindowBytes} - 1);
        for (u64 index = 0; index < 32; ++index) {
            const u64 original = allocation + (index / 16) * FeedbackSideBytes +
                                 FeedbackRecordPage + (index % 16) * 256;
            Result result{};
            run(original, allocation, &result);
            if (result.address != first + index * FeedbackWindowBytes ||
                result.counter != reinterpret_cast<u64>(&feedback_arena_group_moves[index])) {
                throw std::runtime_error("Feedback group generated mapping check failed.");
            }
        }
        for (u64 offset : {u64{FeedbackRecordPage - 256}, u64{FeedbackRecordPage + 1},
                           u64{FeedbackRecordPage + 0xfff}, u64{FeedbackRecordPage + 0x1000},
                           u64{FeedbackSideBytes + FeedbackRecordPage + 0x1000}}) {
            Result result{};
            run(allocation + offset, allocation, &result);
            if (result.address != allocation + offset) {
                throw std::runtime_error("Feedback group rejection check failed.");
            }
        }
    }
    LOG_INFO(Loader, "Feedback arena group: generated mapping checks passed");
}
} // namespace

void InstallFeedbackArena(std::uintptr_t base, std::size_t image_size, void* arena) {
    const char* enabled = std::getenv("IFSS_FEEDBACK_ARENA");
    const char* group_env = std::getenv("IFSS_FEEDBACK_ARENA_GROUP");
    const bool group = group_env && std::string_view{group_env} == "1";
    const char* pool_env = std::getenv("IFSS_FEEDBACK_ARENA_POOL");
    const bool tiled = pool_env && std::string_view{pool_env} == "tiled64";
    const bool pool = tiled || (pool_env && std::string_view{pool_env} == "1");
    const u32 group_records = tiled ? 64 : FeedbackPoolCapacity;
    if (!verified_build || (!pool && !group && (!enabled || std::string_view{enabled} != "1"))) {
        return;
    }
    constexpr std::size_t alloc_call = 0x3e97ac;
    constexpr std::size_t native_alloc = 0x59d50;
    constexpr std::size_t record_store = 0x3f04dc;
    constexpr std::size_t manager = 0x7d3eb10;
    constexpr u32 side_size = FeedbackSideBytes;
    constexpr u32 window = FeedbackWindowBytes;
    const u32 extra_windows = pool ? 2 * (FeedbackPoolCapacity / group_records) + 1 : group ? 33 : 3;
    const u32 expanded_size = 2 * side_size + extra_windows * window;
    // The observed record follows the 12 MiB particle array and 1 MiB sort list.
    constexpr u32 slot = 0xd00400;
    static_assert((slot & ~0xfffU) == FeedbackRecordPage);
    const u64 max_base = (1ULL << 40) - expanded_size;
    if (image_size < manager + 0x2c) {
        throw std::runtime_error("Second Son feedback arena manager is outside the image.");
    }

    if (group) {
        CheckFeedbackGroupMapping();
    }
    if (pool) {
        CheckFeedbackPoolMapping(FeedbackPoolCapacity);
        CheckFeedbackPoolMapping(64);
        InterlockedExchange(&feedback_pool_group_records, group_records);
    }

    Xbyak::CodeGenerator code(FeedbackArenaTrampolineSize, arena);
    Xbyak::Label ordinary, retry, grown_base, done, second, count;
    code.mov(code.r11, base + manager);
    code.cmp(code.rbx, code.r11);
    code.jne(ordinary);
    code.sub(code.rsp, 8); // SysV alignment for the nested allocator call.
    code.mov(code.esi, expanded_size);
    code.call(reinterpret_cast<const void*>(base + native_alloc));
    code.add(code.rsp, 8);
    code.mov(code.qword[code.rip + grown_base], code.rax);
    code.test(code.rax, code.rax);
    code.jz(retry);
    if (pool) {
        // A new native allocation owns new counters. Side reuse follows its own initializer.
        EmitFeedbackPoolClear(code, reinterpret_cast<u64>(&feedback_pool));
    }
    code.ret();
    code.L(retry);
    code.mov(code.rdi, code.r14); // Original call-site arguments.
    code.mov(code.esi, 2 * side_size);
    code.mov(code.edx, 0x100);
    code.mov(code.ecx, 0x25);
    code.L(ordinary);
    code.jmp(reinterpret_cast<const void*>(base + native_alloc));

    const auto redirect = reinterpret_cast<std::uintptr_t>(code.getCurr());
    // The original reservation and capacity check already ran. Preserve their result
    // unless this allocation belongs to the verified, successfully enlarged manager.
    code.push(code.r10);
    code.push(code.r11);
    code.test(code.rax, code.rax);
    code.jz(done, code.T_NEAR);
    code.mov(code.r10, base + manager);
    code.cmp(code.r8, code.r10);
    code.jne(done, code.T_NEAR);
    code.mov(code.r10, code.qword[code.rip + grown_base]);
    code.test(code.r10, code.r10);
    code.jz(done, code.T_NEAR);
    code.cmp(code.r10, code.qword[code.r8]);
    code.jne(done, code.T_NEAR);
    code.mov(code.r11, max_base);
    code.cmp(code.r10, code.r11);
    code.ja(done, code.T_NEAR);
    code.mov(code.r11d, code.dword[code.r8 + 0x28]);
    code.cmp(code.r11d, 1);
    code.ja(done, code.T_NEAR);
    code.imul(code.r11, code.r11, side_size);
    code.add(code.r11, code.r10);
    code.cmp(code.r11, code.qword[code.r8 + 0x8]);
    code.jne(done, code.T_NEAR);
    code.add(code.r11, side_size);
    code.cmp(code.rdx, code.r11);
    code.jne(done, code.T_NEAR);
    if (pool) {
        code.cmp(code.rax, code.r11);
        code.jae(done, code.T_NEAR);
        code.sub(code.r11, side_size);
        code.cmp(code.rax, code.r11);
        code.jb(done, code.T_NEAR);
        EmitFeedbackPoolMapping(code, done, reinterpret_cast<u64>(&feedback_pool), group_records);
    } else if (group) {
        EmitFeedbackGroupMapping(code, done);
    } else {
        code.sub(code.r11, side_size - slot);
        code.cmp(code.rax, code.r11);
        code.jne(done, code.T_NEAR);
        code.sub(code.r11, code.r10);
        code.lea(code.rax, code.ptr[code.r10 + 2 * side_size + window - 1]);
        code.and_(code.rax, -static_cast<s32>(window));
        code.cmp(code.r11, slot);
        code.jne(second);
        code.mov(code.r11, reinterpret_cast<std::uintptr_t>(&feedback_arena_moves[0]));
        code.jmp(count);
        code.L(second);
        code.add(code.rax, window);
        code.mov(code.r11, reinterpret_cast<std::uintptr_t>(&feedback_arena_moves[1]));
    }
    if (!pool) {
        code.L(count);
        code.lock();
        code.inc(code.qword[code.r11]);
    }
    code.L(done);
    code.pop(code.r11);
    code.pop(code.r10);
    code.mov(code.qword[code.r12 + 0xf0], code.rax);
    code.jmp(reinterpret_cast<const void*>(base + record_store + 8));
    const auto initialize = reinterpret_cast<std::uintptr_t>(code.getCurr());
    if (pool) {
        EmitFeedbackPoolInitializer(code, base + manager, grown_base,
                                    reinterpret_cast<u64>(&feedback_pool));
        code.jmp(reinterpret_cast<const void*>(base + 0x577cb0));
    }
    code.align(8);
    code.L(grown_base);
    code.dq(0);
    code.ready();

    std::vector<ImageWrite> writes{
        ImageWrite{alloc_call, Bytes({0xe8, 0x9f, 0x05, 0xc7, 0xff}),
                   Branch(base + alloc_call, reinterpret_cast<std::uintptr_t>(arena), 5, true)},
        ImageWrite{record_store, Bytes({0x49, 0x89, 0x84, 0x24, 0xf0, 0, 0, 0}),
                   Branch(base + record_store, redirect, 8, false)},
    };
    if (pool) {
        // Both verified manager initialization paths share the original lifetime boundary.
        // The unrelated manager using 0x577cb0 is not patched.
        writes.push_back(ImageWrite{0x3ee87c, Bytes({0xe8, 0x2f, 0x94, 0x18, 0}),
                                    Branch(base + 0x3ee87c, initialize, 5, true)});
        writes.push_back(ImageWrite{0x3ed667, Bytes({0xe9, 0x44, 0xa6, 0x18, 0}),
                                    Branch(base + 0x3ed667, initialize, 5, false)});
    }
    for (const auto& write : writes) {
        Check(base, image_size, write);
    }
    FlushInstructionCache(GetCurrentProcess(), arena, code.getSize());
    for (const auto& write : writes) {
        auto* address = reinterpret_cast<void*>(base + write.offset);
        std::memcpy(address, write.replacement.data(), write.replacement.size());
        FlushInstructionCache(GetCurrentProcess(), address, write.replacement.size());
    }
    LOG_INFO(Loader, "Feedback arena experiment: verified allocation and record hooks");
    if (group) {
        LOG_INFO(Loader, "Feedback arena group: 32 isolated windows, 16.5 MiB extension");
    }
    if (pool) {
        LOG_INFO(Loader, "Feedback arena pool: 2048 records per original side epoch, {} records per window, {} KiB extension",
                 group_records, extra_windows * (window / 1024));
    }
}
} // namespace Core::SecondSon
