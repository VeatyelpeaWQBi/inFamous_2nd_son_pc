// SPDX-License-Identifier: GPL-2.0-or-later
// Scoped compatibility for the fingerprinted VTune 2026.4 libxml2 2.13.8 only.
// Pre-generate independent CNG seeds while the parent is alive. Keep all XML cleanup calls.
#pragma once
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace VtuneXmlRng {
constexpr size_t SeedCount = 128;
struct Seeds { LONG64 next{}; std::array<unsigned long long, SeedCount> values{}; };
static_assert(offsetof(Seeds, values) == 8);
using Rng = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);

inline std::vector<BYTE> Code(DWORD64 caller, DWORD64 storage, DWORD64 original) {
    std::vector<BYTE> out;
    const auto bytes = [&](std::initializer_list<BYTE> value) { out.insert(out.end(), value); };
    const auto qword = [&](DWORD64 value) {
        for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<BYTE>(value >> (i * 8)));
    };
    std::vector<size_t> fallback;
    const auto jne = [&] { bytes({0x0f, 0x85}); fallback.push_back(out.size()); bytes({0, 0, 0, 0}); };
    bytes({0x49,0xba}); qword(caller);                 // r10 = exact return site
    bytes({0x4c,0x39,0x14,0x24}); jne();              // cmp [rsp],r10
    bytes({0x48,0x85,0xc9}); jne();                   // algorithm == NULL
    bytes({0x48,0x85,0xd2,0x0f,0x84}); fallback.push_back(out.size()); bytes({0,0,0,0});
    bytes({0x41,0x83,0xf8,0x08}); jne();              // length == 8
    bytes({0x41,0x83,0xf9,0x02}); jne();              // SYSTEM_PREFERRED_RNG
    bytes({0x49,0xbb}); qword(storage);              // r11 = RW seed storage
    bytes({0x41,0xba,1,0,0,0});                      // r10 = 1
    bytes({0xf0,0x4d,0x0f,0xc1,0x13});              // lock xadd [r11],r10
    bytes({0x49,0x81,0xfa,0x80,0,0,0});              // cmp index,128
    bytes({0x0f,0x83}); const auto exhausted = out.size(); bytes({0,0,0,0});
    bytes({0x4f,0x8b,0x5c,0xd3,0x08});              // r11 = seeds[index]
    bytes({0x4c,0x89,0x1a,0x31,0xc0,0xc3});         // write exactly 8; STATUS_SUCCESS
    const auto failed = out.size();
    bytes({0xb8,1,0,0,0xc0,0xc3});                   // exhausted: STATUS_UNSUCCESSFUL
    const auto pass = out.size();
    bytes({0x48,0xb8}); qword(original); bytes({0xff,0xe0}); // all other calls unchanged
    const auto branch = [&](size_t at, size_t target) {
        const auto delta = static_cast<LONG>(target - (at + 4));
        std::memcpy(out.data() + at, &delta, 4);
    };
    for (auto at : fallback) branch(at, pass);
    branch(exhausted, failed);
    return out;
}

inline std::vector<BYTE> VerifiedImage(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                             FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot read libxml image");
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0 || length.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file); throw std::runtime_error("Invalid libxml image size");
    }
    std::vector<BYTE> image(static_cast<size_t>(length.QuadPart));
    DWORD read{};
    const bool complete = ReadFile(file, image.data(), static_cast<DWORD>(image.size()), &read, nullptr) && read == image.size();
    CloseHandle(file);
    if (!complete) throw std::runtime_error("Short libxml image read");
    BCRYPT_ALG_HANDLE hash{};
    std::array<BYTE,32> digest{};
    if (BCryptOpenAlgorithmProvider(&hash, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider unavailable");
    const auto status = BCryptHash(hash, nullptr, 0, image.data(), static_cast<ULONG>(image.size()), digest.data(), digest.size());
    BCryptCloseAlgorithmProvider(hash, 0);
    constexpr std::array<BYTE,32> expected{
        0xc5,0xd1,0x08,0x07,0x22,0x1a,0xa0,0x02,0x84,0x69,0xe9,0x25,0xbf,0xe0,0x98,0xf0,
        0x01,0x12,0x61,0xd6,0xa4,0x8b,0x81,0xf4,0x28,0x17,0x24,0xee,0x7f,0x48,0x9d,0xc4};
    if (status < 0 || digest != expected) throw std::runtime_error("Unsupported libxml SHA256; guard refused");
    return image;
}

inline DWORD Iat(const std::vector<BYTE>& image) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    const auto raw = [&](DWORD rva) -> const BYTE* {
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (rva >= sections[i].VirtualAddress && rva - sections[i].VirtualAddress < sections[i].SizeOfRawData)
                return image.data() + sections[i].PointerToRawData + rva - sections[i].VirtualAddress;
        throw std::runtime_error("Verified image has an unexpected RVA");
    };
    const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(raw(nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress));
    for (; descriptor->Name; ++descriptor) {
        if (_stricmp(reinterpret_cast<const char*>(raw(descriptor->Name)), "bcrypt.dll")) continue;
        const auto* names = reinterpret_cast<const ULONGLONG*>(raw(descriptor->OriginalFirstThunk));
        for (unsigned i = 0; names[i]; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i])) continue;
            if (std::strcmp(reinterpret_cast<const char*>(raw(static_cast<DWORD>(names[i]))) + 2, "BCryptGenRandom") == 0)
                return descriptor->FirstThunk + i * 8;
        }
    }
    throw std::runtime_error("Expected BCryptGenRandom import absent");
}

inline bool Install(HANDLE process, DWORD64 base, const std::wstring& path, const Seeds& seeds) {
    const auto image = VerifiedImage(path);
    const auto iat = base + Iat(image);
    DWORD64 original{}; SIZE_T transferred{};
    const auto read = [&](DWORD64 at, void* output, size_t size) {
        return ReadProcessMemory(process, reinterpret_cast<void*>(at), output, size, &transferred) && transferred == size;
    };
    if (!read(iat, &original, 8)) throw std::runtime_error("Cannot read resolved RNG import");
    MEMORY_BASIC_INFORMATION target{};
    if (!VirtualQueryEx(process, reinterpret_cast<void*>(original), &target, sizeof(target)) ||
        target.Type != MEM_IMAGE || target.State != MEM_COMMIT)
        throw std::runtime_error("RNG import is not a resolved image function");
    constexpr std::array<BYTE,5> call{0xe8,0xe8,0x7f,0x0d,0x00};
    std::array<BYTE,5> actual{};
    if (!read(base + 0xa7f8, actual.data(), actual.size()) || actual != call)
        throw std::runtime_error("RNG caller bytes differ; guard refused");
    auto* storage = VirtualAllocEx(process, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    auto* executable = VirtualAllocEx(process, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!storage || !executable) throw std::runtime_error("Cannot reserve scoped guard storage");
    const auto code = Code(base + 0xa7fd, reinterpret_cast<DWORD64>(storage), original);
    const auto write = [&](void* at, const void* input, size_t size) {
        return WriteProcessMemory(process, at, input, size, &transferred) && transferred == size;
    };
    DWORD previous{};
    if (!write(storage, &seeds, sizeof(seeds)) || !write(executable, code.data(), code.size()) ||
        !VirtualProtectEx(process, executable, 4096, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(process, executable, code.size()))
        throw std::runtime_error("Cannot publish scoped RNG guard");
    DWORD protect{};
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(iat), 8, PAGE_READWRITE, &protect))
        throw std::runtime_error("Cannot access the owned libxml import slot");
    const auto pointer = reinterpret_cast<DWORD64>(executable);
    const bool installed = write(reinterpret_cast<void*>(iat), &pointer, 8);
    const bool restored = VirtualProtectEx(process, reinterpret_cast<void*>(iat), 8, protect, &previous) != FALSE;
    if (!installed || !restored) throw std::runtime_error("Cannot restore scoped import protection");
    std::printf("XML_RNG_LIFETIME_GUARD installed: fingerprint verified; 128 independent CNG seeds; all cleanup retained\n");
    return true;
}

inline std::atomic<unsigned> forwarded{};
inline NTSTATUS WINAPI Fake(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG) { ++forwarded; return 0x123456; }
inline int SelfTest() {
    Seeds seeds;
    for (size_t i=0; i<SeedCount; ++i) seeds.values[i] = 0x1122334400000000ULL + i;
    auto* caller = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto* stub = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!caller || !stub) return 1;
    const auto code = Code(reinterpret_cast<DWORD64>(caller + 16), reinterpret_cast<DWORD64>(&seeds), reinterpret_cast<DWORD64>(&Fake));
    std::memcpy(stub, code.data(), code.size());
    const BYTE enter[]{0x48,0x83,0xec,0x28,0x48,0xb8};
    const BYTE leave[]{0xff,0xd0,0x48,0x83,0xc4,0x28,0xc3};
    std::memcpy(caller, enter, sizeof(enter));
    const auto pointer = reinterpret_cast<DWORD64>(stub);
    std::memcpy(caller + 6, &pointer, 8); std::memcpy(caller + 14, leave, sizeof(leave));
    DWORD old{};
    if (!VirtualProtect(caller,4096,PAGE_EXECUTE_READ,&old) || !VirtualProtect(stub,4096,PAGE_EXECUTE_READ,&old)) return 2;
    FlushInstructionCache(GetCurrentProcess(),nullptr,0);
    const auto invoke = reinterpret_cast<Rng>(caller);
    unsigned long long untouched = 0xaabbccddeeff0011ULL;
    if (reinterpret_cast<Rng>(stub)(nullptr,reinterpret_cast<PUCHAR>(&untouched),8,2) != 0x123456 ||
        invoke(nullptr,reinterpret_cast<PUCHAR>(&untouched),9,2) != 0x123456 ||
        invoke(nullptr,reinterpret_cast<PUCHAR>(&untouched),8,0) != 0x123456 ||
        invoke(reinterpret_cast<void*>(1),reinterpret_cast<PUCHAR>(&untouched),8,2) != 0x123456 ||
        invoke(nullptr,nullptr,8,2) != 0x123456 ||
        untouched != 0xaabbccddeeff0011ULL || forwarded != 5 || seeds.next != 0) return 3;
    std::vector<unsigned long long> received; std::mutex mutex; std::atomic<bool> bad{};
    std::vector<std::thread> threads;
    for (unsigned t=0;t<8;++t) threads.emplace_back([&] {
        for(unsigned i=0;i<16;++i) {
            unsigned long long result[2]{0,0xdeadbeef};
            if (invoke(nullptr,reinterpret_cast<PUCHAR>(result),8,2) != 0 || result[1] != 0xdeadbeef) bad=true;
            std::scoped_lock lock(mutex); received.push_back(result[0]);
        }
    });
    for(auto& thread:threads) thread.join();
    std::sort(received.begin(),received.end());
    if(bad || !std::equal(received.begin(),received.end(),seeds.values.begin()) || seeds.next != 128) return 4;
    if(invoke(nullptr,reinterpret_cast<PUCHAR>(&untouched),8,2) >= 0 || untouched != 0xaabbccddeeff0011ULL || forwarded != 5) return 5;
    VirtualFree(caller,0,MEM_RELEASE); VirtualFree(stub,0,MEM_RELEASE);
    std::puts("PASS: exact caller/arguments, untouched fallback, concurrent unique CNG-seed consumption, bounded exhaustion.");
    return 0;
}
} // namespace VtuneXmlRng
