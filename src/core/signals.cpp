// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <fmt/format.h>
#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/scope_exit.h"
#include "common/signal_context.h"
#include "core/cpu_patches.h" // Windows static guest red-zone protection
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#include "emulator.h"

#ifdef _WIN32
#include <atomic>
#include <exception>
#include <string>
#include <windows.h>
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
static constexpr DWORD MSVC_CPP_EXCEPTION = 0xE06D7363;
#else
#include <csignal>
#include <pthread.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#endif

namespace Core {

#if defined(_WIN32)

// Keep only scalar state, rather than copying the full Windows CONTEXT on every page fault.
// A nested exception must not overwrite the instruction that originally entered the handler.
struct WindowsExceptionSnapshot {
    DWORD code;
    u64 address;
    bool has_access_info;
    u64 access_kind;
    u64 access_target;
    u64 rip;
    u64 rsp;
    u64 rbp;
    u64 rax;
    u64 rbx;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    DWORD eflags;
};

static WindowsExceptionSnapshot SnapshotException(const EXCEPTION_POINTERS& exception) {
    const auto& record = *exception.ExceptionRecord;
    const auto& context = *exception.ContextRecord;
    const bool has_access_info = (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                                  record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
                                 record.NumberParameters >= 2;
    return {
        record.ExceptionCode,
        reinterpret_cast<u64>(record.ExceptionAddress),
        has_access_info,
        has_access_info ? record.ExceptionInformation[0] : 0,
        has_access_info ? record.ExceptionInformation[1] : 0,
        context.Rip,
        context.Rsp,
        context.Rbp,
        context.Rax,
        context.Rbx,
        context.Rcx,
        context.Rdx,
        context.Rsi,
        context.Rdi,
        context.R8,
        context.R9,
        context.R10,
        context.R11,
        context.R12,
        context.R13,
        context.R14,
        context.R15,
        context.EFlags,
    };
}

static void LogExceptionSnapshot(const char* label, const WindowsExceptionSnapshot& snapshot) {
    const char* access = "not-applicable";
    if (snapshot.has_access_info) {
        switch (snapshot.access_kind) {
        case EXCEPTION_READ_FAULT:
            access = "read";
            break;
        case EXCEPTION_WRITE_FAULT:
            access = "write";
            break;
        case EXCEPTION_EXECUTE_FAULT:
            access = "execute";
            break;
        default:
            access = "unknown";
            break;
        }
    }
    LOG_CRITICAL(Debug,
                 "CrashDiag v1 {}: code={:#x} exception_address={:#x} rip={:#x} "
                 "access={} kind={:#x} target={:#x} eflags={:#x}",
                 label, snapshot.code, snapshot.address, snapshot.rip, access, snapshot.access_kind,
                 snapshot.access_target, snapshot.eflags);
    LOG_CRITICAL(Debug,
                 "CrashDiag v1 {}: rax={:#x} rbx={:#x} rcx={:#x} rdx={:#x} "
                 "rsi={:#x} rdi={:#x} rsp={:#x} rbp={:#x}",
                 label, snapshot.rax, snapshot.rbx, snapshot.rcx, snapshot.rdx, snapshot.rsi,
                 snapshot.rdi, snapshot.rsp, snapshot.rbp);
    LOG_CRITICAL(Debug,
                 "CrashDiag v1 {}: r8={:#x} r9={:#x} r10={:#x} r11={:#x} "
                 "r12={:#x} r13={:#x} r14={:#x} r15={:#x}",
                 label, snapshot.r8, snapshot.r9, snapshot.r10, snapshot.r11, snapshot.r12,
                 snapshot.r13, snapshot.r14, snapshot.r15);

    // Read through the OS: an inaccessible RIP or stack must not fault inside the diagnostic.
    // Limit output to one instruction and eight stack words; these are raw words, not a backtrace.
    std::array<u8, 15> instruction{};
    SIZE_T instruction_size = 0;
    const BOOL instruction_read =
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(snapshot.rip),
                          instruction.data(), instruction.size(), &instruction_size);
    const DWORD instruction_error = instruction_read ? ERROR_SUCCESS : GetLastError();
    std::array<char, 15 * 3 + 1> instruction_hex{};
    constexpr char digits[] = "0123456789abcdef";
    for (SIZE_T i = 0; i < instruction_size && i < instruction.size(); ++i) {
        instruction_hex[i * 3] = digits[instruction[i] >> 4];
        instruction_hex[i * 3 + 1] = digits[instruction[i] & 0xf];
        instruction_hex[i * 3 + 2] = ' ';
    }
    LOG_CRITICAL(Debug, "CrashDiag v1 {}: instruction_bytes={} read={} error={} [{}]", label,
                 instruction_size, instruction_read != FALSE, instruction_error,
                 instruction_hex.data());

    std::array<u64, 8> stack{};
    SIZE_T stack_size = 0;
    const BOOL stack_read =
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(snapshot.rsp),
                          stack.data(), sizeof(stack), &stack_size);
    const DWORD stack_error = stack_read ? ERROR_SUCCESS : GetLastError();
    LOG_CRITICAL(Debug, "CrashDiag v1 {}: stack_bytes={} read={} error={}", label, stack_size,
                 stack_read != FALSE, stack_error);
    for (SIZE_T i = 0; i < stack_size / sizeof(u64) && i < stack.size(); ++i) {
        LOG_CRITICAL(Debug, "CrashDiag v1 {}: [rsp+{:#x}]={:#x}", label, i * sizeof(u64), stack[i]);
    }
}

// CrashDiag v2: call stack with module offsets, and the type, message and origin of C++
// exceptions (MSVC ABI). Everything read from an exception is validated first, as this runs
// inside the exception handler.

/// Number of readable bytes from address to the end of its memory region, 0 if not readable.
static size_t ReadableBytes(const void* address) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (address == nullptr || VirtualQuery(address, &info, sizeof(info)) == 0) {
        return 0;
    }
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize -
           reinterpret_cast<uintptr_t>(address);
}

static bool IsReadable(const void* address, size_t size) noexcept {
    return ReadableBytes(address) >= size;
}

static std::string DescribeHostAddress(u64 address) {
    HMODULE module = nullptr;
    if (address != 0 &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        module != nullptr) {
        std::array<char, MAX_PATH> path{};
        const DWORD length =
            GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
        const char* name = path.data();
        for (DWORD i = 0; i < length && i < path.size(); ++i) {
            if (path[i] == '\\' || path[i] == '/') {
                name = path.data() + i + 1;
            }
        }
        return fmt::format("{}+{:#x}", name, address - reinterpret_cast<u64>(module));
    }
    return fmt::format("{:#x}", address);
}

/// Logs the call stack from a context by unwinding host code with its unwind data. Stops at the
/// first frame without unwind data after the first one (guest or generated code).
static void LogBacktrace(const char* label, const CONTEXT& start) {
    CONTEXT context = start;
    ULONG_PTR stack_low = 0;
    ULONG_PTR stack_high = 0;
    GetCurrentThreadStackLimits(&stack_low, &stack_high);
    const bool known_stack = context.Rsp >= stack_low && context.Rsp < stack_high;
    for (u32 frame = 0; frame < 40; ++frame) {
        const u64 pc = context.Rip;
        if (pc == 0) {
            break;
        }
        LOG_CRITICAL(Debug, "CrashDiag v2 {} stack #{}: {}", label, frame, DescribeHostAddress(pc));
        DWORD64 image_base = 0;
        const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
        if (function != nullptr) {
            PVOID handler_data = nullptr;
            DWORD64 establisher_frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, function, &context, &handler_data,
                             &establisher_frame, nullptr);
        } else {
            if (frame != 0 || !IsReadable(reinterpret_cast<const void*>(context.Rsp), 8)) {
                break;
            }
            // Leaf function: the return address is on the top of the stack.
            context.Rip = *reinterpret_cast<const u64*>(context.Rsp);
            context.Rsp += 8;
        }
        if (known_stack && (context.Rsp < stack_low || context.Rsp >= stack_high)) {
            break;
        }
    }
}

/// Logs a C++ exception: where it was thrown, the module that threw it, its type, its message
/// and the call stack. Logged a limited number of times, libraries may throw and catch
/// exceptions routinely.
static void LogCppException(const EXCEPTION_POINTERS& exception) {
    static std::atomic<u32> num_logged{0};
    static thread_local bool logging = false;
    if (logging || num_logged.fetch_add(1, std::memory_order_relaxed) >= 16) {
        return;
    }
    logging = true;
    SCOPE_EXIT {
        logging = false;
    };
    const EXCEPTION_RECORD& record = *exception.ExceptionRecord;
    LOG_CRITICAL(Debug, "CrashDiag v2: C++ exception at {}",
                 DescribeHostAddress(reinterpret_cast<u64>(record.ExceptionAddress)));
    if (record.NumberParameters >= 4) {
        const u64 object = record.ExceptionInformation[1];
        const u64 throw_info = record.ExceptionInformation[2];
        const u64 image_base = record.ExceptionInformation[3];
        LOG_CRITICAL(Debug, "CrashDiag v2: thrown by module {}", DescribeHostAddress(image_base));
        // x64 ThrowInfo: attributes, unwind, forward compat, catchable type array (image RVAs).
        const auto* info = reinterpret_cast<const s32*>(throw_info);
        if (image_base != 0 && IsReadable(info, 4 * sizeof(s32)) && info[3] != 0) {
            const auto* types = reinterpret_cast<const s32*>(image_base + static_cast<u32>(info[3]));
            const s32 num_types = IsReadable(types, sizeof(s32)) ? std::min<s32>(types[0], 8) : 0;
            for (s32 i = 0; i < num_types && IsReadable(types + 1 + i, sizeof(s32)); ++i) {
                // CatchableType: properties, type descriptor, PMD (mdisp, pdisp, vdisp), size.
                const auto* catchable =
                    reinterpret_cast<const s32*>(image_base + static_cast<u32>(types[1 + i]));
                if (!IsReadable(catchable, 6 * sizeof(s32))) {
                    break;
                }
                // TypeDescriptor: vftable, spare, decorated name.
                const char* name = reinterpret_cast<const char*>(
                    image_base + static_cast<u32>(catchable[1]) + 2 * sizeof(void*));
                const size_t max_length = std::min<size_t>(ReadableBytes(name), 160);
                std::string type_name;
                for (size_t c = 0; c < max_length && name[c] != '\0'; ++c) {
                    type_name.push_back(name[c]);
                }
                LOG_CRITICAL(Debug, "CrashDiag v2: exception type {}", type_name);
                if (type_name == ".?AVexception@std@@" && catchable[3] == -1 && object != 0) {
                    const auto* error =
                        reinterpret_cast<const std::exception*>(object + catchable[2]);
                    if (IsReadable(error, 2 * sizeof(void*))) {
                        LOG_CRITICAL(Debug, "CrashDiag v2: what(): {}", error->what());
                    }
                }
            }
        }
    }
    LogBacktrace("C++ exception", *exception.ContextRecord);
}

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    using namespace Libraries::Kernel;
    if (pExp == nullptr || pExp->ExceptionRecord == nullptr || pExp->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto entry_snapshot = SnapshotException(*pExp);
    static thread_local u32 exception_depth = 0;
    static thread_local const WindowsExceptionSnapshot* first_exception = nullptr;
    if (exception_depth == 0) {
        first_exception = &entry_snapshot;
    }
    ++exception_depth;
    SCOPE_EXIT {
        if (--exception_depth == 0) {
            first_exception = nullptr;
        }
    };
    const auto* signals = Signals::Instance();

    const bool use_static_windows_guest_red_zone_protection =
        WindowsGuestRedZoneProtection::IsStaticPatchingEnabled();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    Ucontext guest_context{pExp->ContextRecord};
    Siginfo guest_info{
        ._si_signo = 0,
        ._si_errno = 0,
        ._si_code = POSIX_SI_NOINFO,
        ._si_addr = (void*)guest_context.uc_mcontext.mc_rip,
    };

    bool handled = false;
    bool static_protection_exception = false; // Windows static guest red-zone protection
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        guest_info._si_signo = POSIX_SIGSEGV;
        guest_info._si_code = POSIX_SEGV_MAPERR;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_ILLOPC;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case EXCEPTION_PRIV_INSTRUCTION: // Windows static guest red-zone protection
        if (use_static_windows_guest_red_zone_protection) {
            static_protection_exception = true;
            handled = signals->DispatchIllegalInstruction(pExp);
        }
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        guest_info._si_signo = POSIX_SIGBUS;
        guest_info._si_code = POSIX_BUS_ADRALN;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTDIV;
        break;
    case EXCEPTION_INT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTOVF;
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTDIV;
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTINV;
        break;
    case EXCEPTION_FLT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTOVF;
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTUND;
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTSUB; // i am not sure about this one
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTRES;
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_BADSTK; // i am not sure about this one either
        break;
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
        guest_info._si_signo = POSIX_SIGTRAP;
        guest_info._si_code = POSIX_TRAP_BRKPT;
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    case MSVC_CPP_EXCEPTION:
        // A C++ throw, seen here before any catch: the code that throws may catch it itself (a
        // driver or a library). It keeps its normal course, and is logged with its type, message
        // and call stack. If nothing catches it, the C++ runtime terminates the program.
        LogCppException(*pExp);
        return EXCEPTION_CONTINUE_SEARCH;
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (guest_info._si_signo != 0) {
        if (g_curthread &&
            g_curthread->DispatchSignal(guest_info._si_signo, &guest_info, &guest_context)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    const bool report_unhandled =
        use_static_windows_guest_red_zone_protection ? static_protection_exception : true;
    if (report_unhandled) {
        static thread_local bool reporting_exception = false;
        if (reporting_exception) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        reporting_exception = true;
        SCOPE_EXIT {
            reporting_exception = false;
        };
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
        LOG_CRITICAL(Debug, "CrashDiag v1: exception_depth={}", exception_depth);
        if (exception_depth > 1 && first_exception != nullptr) {
            LogExceptionSnapshot("first", *first_exception);
        }
        LogExceptionSnapshot("unhandled", entry_snapshot);
        LogBacktrace("unhandled", *pExp->ContextRecord);
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

static s32 NativeSiCodeToGuest(s32 sig, s32 code) {
    using namespace Libraries::Kernel;
    switch (sig) {
    case SIGUSR1:
        return POSIX_SI_LWP;
    case SIGSEGV:
        switch (code) {
        case SEGV_MAPERR:
            return POSIX_SEGV_MAPERR;
        case SEGV_ACCERR:
            return POSIX_SEGV_ACCERR;
        }
    case SIGBUS:
        switch (code) {
        case BUS_ADRALN:
            return POSIX_BUS_ADRALN;
        case BUS_ADRERR:
            return POSIX_BUS_ADRERR;
        case BUS_OBJERR:
            return POSIX_BUS_OBJERR;
        }
    case SIGILL:
        switch (code) {
        case ILL_ILLOPC:
            return POSIX_ILL_ILLOPC;
        case ILL_ILLOPN:
            return POSIX_ILL_ILLOPN;
        case ILL_ILLADR:
            return POSIX_ILL_ILLADR;
        case ILL_ILLTRP:
            return POSIX_ILL_ILLTRP;
        case ILL_PRVOPC:
            return POSIX_ILL_PRVOPC;
        case ILL_PRVREG:
            return POSIX_ILL_PRVREG;
        case ILL_COPROC:
            return POSIX_ILL_COPROC;
        case ILL_BADSTK:
            return POSIX_ILL_BADSTK;
        }
    case SIGFPE:
        switch (code) {
        case FPE_INTOVF:
            return POSIX_FPE_INTOVF;
        case FPE_INTDIV:
            return POSIX_FPE_INTDIV;
        case FPE_FLTDIV:
            return POSIX_FPE_FLTDIV;
        case FPE_FLTOVF:
            return POSIX_FPE_FLTOVF;
        case FPE_FLTUND:
            return POSIX_FPE_FLTUND;
        case FPE_FLTRES:
            return POSIX_FPE_FLTRES;
        case FPE_FLTINV:
            return POSIX_FPE_FLTINV;
        case FPE_FLTSUB:
            return POSIX_FPE_FLTSUB;
        }
    case SIGTRAP:
        switch (code) {
        case TRAP_BRKPT:
            return POSIX_TRAP_BRKPT;
        case TRAP_TRACE:
            return POSIX_TRAP_TRACE;
#ifdef __FreeBSD__
        case TRAP_DTRACE:
            return POSIX_TRAP_DTRACE;
#endif
        }

    default:
        return POSIX_SI_NOINFO;
    }
}

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    using namespace Libraries::Kernel;
    auto* thread = g_curthread;
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    Ucontext context{info, reinterpret_cast<ucontext_t*>(raw_context)};
    Siginfo guest_info{};
    if (info) {
        guest_info = *reinterpret_cast<Siginfo*>(info);
        guest_info._si_signo = sig == SIGUSR1 ? 0 : NativeToOrbisSignal(info->si_signo);
        guest_info._si_errno = NativeToPosixErrno(info->si_errno);
        guest_info._si_code = NativeSiCodeToGuest(sig, info->si_code);
        guest_info._si_addr = (void*)context.uc_mcontext.mc_rip;
    }
    Siginfo* info_p = info ? &guest_info : nullptr;
    Ucontext* context_p = raw_context ? &context : nullptr;

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        const bool is_write = Common::IsWriteError(raw_context);
        const bool is_exec = Common::IsExecuteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
                return;
            }
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address),
                            is_write  ? "Write to"
                            : is_exec ? "Executed from"
                                      : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (signals->DispatchIllegalInstruction(raw_context)) {
            return;
        }
    case SIGFPE:
    case SIGTRAP:
    case SIGSYS: {
        if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
            return;
        }

        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
    case SIGSLEEP: {
        // Sleep thread until signal is received again
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGSLEEP);
        sigwait(&sigset, &sig);
        break;
    }
    case SIGUSR1:
        if (thread) {
            thread->DispatchPendingSignals(info_p, context_p);
        }
        break;
    default:
        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
}

#endif

SignalDispatch::SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
    LOG_INFO(Debug, "CrashDiag v1 enabled: bounded CPU patching and fatal exception context");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(
        sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
            sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
            sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
            sigaction(SIGUSR1, &action, nullptr) == 0 && sigaction(SIGSLEEP, &action, nullptr) == 0,
        "Failed to register signal handlers.");
#endif
}

void SignalDispatch::RemoveHandlers() {
    // asserting here would get into an infinite loop until too
    // many nested exceptions makes the OS kill the process
#if defined(_WIN32)
    if (!(RemoveVectoredExceptionHandler(handle))) {
        LOG_CRITICAL(Core, "Failed to remove exception handler.");
        std::quick_exit(1);
    }
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    if (!(sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
          sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
          sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
          sigaction(SIGUSR1, &action, nullptr) == 0 &&
          sigaction(SIGSLEEP, &action, nullptr) == 0)) {
        LOG_CRITICAL(Core, "Failed to remove signal handlers.");
        std::quick_exit(1);
    }
#endif
}

SignalDispatch::~SignalDispatch() {
    RemoveHandlers();
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
