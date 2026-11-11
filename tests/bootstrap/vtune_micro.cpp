// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded native workload for the installed VTune; never loads game files.
#include <windows.h>
#include <ittnotify.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

static volatile std::uint64_t sink;
__declspec(noinline) std::uint64_t MemoryPass(std::uint64_t* data, std::size_t count) {
    std::uint64_t value = sink;
    for (std::size_t i = 0; i < count; i += 8) {
        value = (value ^ data[i]) * 6364136223846793005ULL + 1;
        data[i] = value;
    }
    return value;
}
static void Receipt(const wchar_t* path, const char* value) {
    FILE* file{};
    if (_wfopen_s(&file, path, L"wb") == 0) {
        std::fputs(value, file);
        std::fclose(file);
    }
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) return 2;
    HMODULE mapped{};
    HANDLE mappedThread{};
    volatile LONG mappedStop{};
    if (argc == 4) {
        // Symbol-resolution reproducer only: map the already verified native PE
        // without resolving imports or running its entry point. A private page
        // holds a synthetic polling loop; no engine/game initialization runs.
        mapped = LoadLibraryExW(argv[3], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!mapped) return 6;
        auto* base = reinterpret_cast<unsigned char*>(mapped);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 7;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        constexpr std::size_t rva = 0x5b3bb0;
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt->OptionalHeader.SizeOfImage <= rva + 16) return 8;
        DWORD oldProtect{};
        if (!VirtualProtect(base + rva, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) return 9;
        const unsigned char loop[]{0x83,0x39,0x00,0x74,0xfb,0x31,0xc0,0xc3};
        std::memcpy(base + rva, loop, sizeof(loop));
        FlushInstructionCache(GetCurrentProcess(), base + rva, sizeof(loop));
        mappedThread = CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(base + rva), const_cast<LONG*>(&mappedStop), 0, nullptr);
        if (!mappedThread) return 10;
    }
    constexpr std::size_t bytes = 64 * 1024 * 1024;
    auto* data = static_cast<std::uint64_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!data) return 3;
    const auto started = GetTickCount64();
    while (GetTickCount64() - started < 180000) {
        sink = MemoryPass(data, bytes / sizeof(*data));
        if (GetFileAttributesW(argv[1]) != INVALID_FILE_ATTRIBUTES) {
            if (!__itt_domain_createA("IFSS.Diagnostics.Micro") || !__itt_detach_ptr) return 4;
            __itt_detach();
            InterlockedExchange(&mappedStop, 1);
            if (mappedThread) { WaitForSingleObject(mappedThread, 2000); CloseHandle(mappedThread); }
            Receipt(argv[2], "detached");
            Sleep(500);
            VirtualFree(data, 0, MEM_RELEASE);
            if (mapped) FreeLibrary(mapped);
            return 0;
        }
        Sleep(1);
    }
    VirtualFree(data, 0, MEM_RELEASE);
    return 5;
}
