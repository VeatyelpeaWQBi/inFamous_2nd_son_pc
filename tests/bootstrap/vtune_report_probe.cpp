// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit offline diagnostic: debug only the tool process this program creates.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include "vtune_xml_rng_guard.h"

static void PrintPath(const std::wstring& path) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                                       nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                        utf8.data(), size, nullptr, nullptr);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
}

static std::wstring Quote(const wchar_t* text) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (const wchar_t* p = text; *p; ++p) {
        if (*p == L'\\') { ++slashes; continue; }
        out.append(slashes * (*p == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (*p == L'"') out += L'\\';
        out += *p;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}

static std::wstring Path(HANDLE file) {
    wchar_t path[32768];
    const DWORD size = file && file != INVALID_HANDLE_VALUE
        ? GetFinalPathNameByHandleW(file, path, 32768, FILE_NAME_NORMALIZED) : 0;
    return size && size < 32768 ? std::wstring(path, size) : L"[unknown]";
}

static void Address(unsigned long long address, const std::map<DWORD64, std::wstring>& modules) {
    const auto end = modules.upper_bound(address);
    if (end == modules.begin()) { std::printf("0x%llx [unmapped]", address); return; }
    const auto at = std::prev(end);
    // The full module map is printed on load; frame labels stay compact.
    const auto slash = at->second.find_last_of(L"\\/");
    std::printf("0x%llx ", address);
    PrintPath(at->second.substr(slash == std::wstring::npos ? 0 : slash + 1));
    std::printf("+0x%llx", address - at->first);
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--self-test") return VtuneXmlRng::SelfTest();
    const bool xml_guard = argc > 1 && std::wstring(argv[1]) == L"--xml-rng-lifetime";
    if (xml_guard) { --argc; ++argv; }
    if (argc < 3) { std::fprintf(stderr, "usage: probe dump-path executable [arguments]\n"); return 2; }
    VtuneXmlRng::Seeds seeds;
    if (xml_guard) {
        const auto name = std::filesystem::path(argv[2]).filename().wstring();
        bool report = false, collect = false;
        for (int i=3; i<argc; ++i) {
            report |= std::wstring(argv[i]) == L"-report";
            collect |= std::wstring(argv[i]) == L"-collect";
        }
        if ((_wcsicmp(name.c_str(),L"vtune.exe") && _wcsicmp(name.c_str(),L"amplxe-cl.exe")) || !report || collect) {
            std::fprintf(stderr,"XML RNG guard is limited to an owned offline VTune report.\n"); return 2;
        }
        if (BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(seeds.values.data()),
                            static_cast<ULONG>(sizeof(seeds.values)),BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            std::fprintf(stderr,"Cannot pre-generate independent CNG seeds.\n"); return 3;
        }
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::wstring command;
    for (int i = 2; i < argc; ++i) {
        if (!command.empty()) command += L' ';
        command += Quote(argv[i]);
    }
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(argv[2], command.data(), nullptr, nullptr, FALSE,
                        DEBUG_ONLY_THIS_PROCESS | CREATE_UNICODE_ENVIRONMENT,
                        nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError()); return 3;
    }
    std::printf("Owned report PID %lu\n", process.dwProcessId);
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS |
                  SYMOPT_NO_PROMPTS | SYMOPT_IGNORE_NT_SYMPATH | SYMOPT_DISABLE_SYMSRV_AUTODETECT);
    const bool symbols = !xml_guard && SymInitialize(process.hProcess, "", FALSE) != FALSE;
    std::map<DWORD64, std::wstring> modules;
    const auto load = [&](HANDLE file, DWORD64 base) {
        const auto path = Path(file);
        modules[base] = path;
        std::printf("LOAD 0x%llx ", base); PrintPath(path); std::printf("\n");
        if (symbols) SymLoadModuleExW(process.hProcess, file, path.c_str(), nullptr, base, 0, nullptr, 0);
        if (file && file != INVALID_HANDLE_VALUE) CloseHandle(file);
    };
    const ULONGLONG started = GetTickCount64();
    DWORD exit_code = ERROR_TIMEOUT;
    bool timed_out = false;
    bool initial_breakpoint = true, guard_installed = false;
    for (;;) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 1000)) {
            if (GetTickCount64() - started < 180000 || timed_out) continue;
            std::fprintf(stderr, "Owned report exceeded 180 s; terminating this child only.\n");
            TerminateProcess(process.hProcess, ERROR_TIMEOUT); timed_out = true; continue;
        }
        DWORD status = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            load(event.u.CreateProcessInfo.hFile,
                 reinterpret_cast<DWORD64>(event.u.CreateProcessInfo.lpBaseOfImage));
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            load(event.u.LoadDll.hFile, reinterpret_cast<DWORD64>(event.u.LoadDll.lpBaseOfDll));
        } else if (event.dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT) {
            modules.erase(reinterpret_cast<DWORD64>(event.u.UnloadDll.lpBaseOfDll));
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            auto& exception = event.u.Exception;
            const auto code = exception.ExceptionRecord.ExceptionCode;
            status = DBG_EXCEPTION_NOT_HANDLED;
            if (code == EXCEPTION_BREAKPOINT && initial_breakpoint) {
                initial_breakpoint = false; status = DBG_CONTINUE;
                if (xml_guard) {
                    try {
                        for (const auto& [base,path] : modules) {
                            if (_wcsicmp(std::filesystem::path(path).filename().c_str(),L"libxml2.dll") == 0)
                                guard_installed = VtuneXmlRng::Install(process.hProcess,base,path,seeds);
                        }
                        if (!guard_installed) throw std::runtime_error("Verified libxml was not loaded at startup");
                    } catch (const std::exception& error) {
                        std::fprintf(stderr,"XML_RNG_GUARD_REFUSED: %s\n",error.what());
                        TerminateProcess(process.hProcess,ERROR_REVISION_MISMATCH);
                    }
                }
            }
            if (!exception.dwFirstChance) {
                std::printf("SECOND-CHANCE code 0x%08lx thread %lu at ", code, event.dwThreadId);
                Address(reinterpret_cast<DWORD64>(exception.ExceptionRecord.ExceptionAddress), modules);
                std::printf("\n");
                if (exception.ExceptionRecord.NumberParameters >= 2)
                    std::printf("operation %llu memory 0x%llx\n",
                        exception.ExceptionRecord.ExceptionInformation[0],
                        exception.ExceptionRecord.ExceptionInformation[1]);
                HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId);
                CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
                const bool have_context = thread && GetThreadContext(thread, &context);
                if (have_context) {
                    CONTEXT walk = context;
                    STACKFRAME64 frame{};
                    frame.AddrPC = {walk.Rip, 0, AddrModeFlat};
                    frame.AddrStack = {walk.Rsp, 0, AddrModeFlat};
                    frame.AddrFrame = {walk.Rbp, 0, AddrModeFlat};
                    for (unsigned i = 0; i < 64 && frame.AddrPC.Offset; ++i) {
                        std::printf("FRAME %u ", i); Address(frame.AddrPC.Offset, modules); std::printf("\n");
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process.hProcess, thread, &frame,
                                         &walk, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                    }
                }
                HANDLE dump = CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (dump != INVALID_HANDLE_VALUE) {
                    EXCEPTION_POINTERS pointers{&exception.ExceptionRecord, &context};
                    MINIDUMP_EXCEPTION_INFORMATION info{event.dwThreadId, &pointers, FALSE};
                    const bool ok = MiniDumpWriteDump(process.hProcess, process.dwProcessId, dump,
                        static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                        have_context ? &info : nullptr, nullptr, nullptr) != FALSE;
                    std::printf("DUMP saved=%u error=%lu\n", ok, ok ? 0 : GetLastError()); CloseHandle(dump);
                } else std::printf("DUMP not created error=%lu\n", GetLastError());
                if (thread) CloseHandle(thread);
                std::fflush(stdout);
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            exit_code = event.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status); break;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
    }
    std::printf("Owned report exited 0x%08lx\n", exit_code);
    if (xml_guard && !guard_installed && exit_code == 0) exit_code = ERROR_NOT_SUPPORTED;
    if (symbols) SymCleanup(process.hProcess);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}
