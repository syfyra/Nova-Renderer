#include "crash.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dbghelp.h>

#include <csignal>
#include <cstdio>
#include <cstring>

namespace {

// The report is written by a thread of its own: a thread that overflowed its
// stack has no room left to walk it or to load symbols.
HANDLE g_request = nullptr;
HANDLE g_done = nullptr;
HANDLE g_faulting = nullptr;
EXCEPTION_POINTERS *g_info = nullptr;
volatile LONG g_reporting = 0;

const char *exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xE06D7363: return "uncaught C++ exception";
    case 0xC0000409: return "abort";
    default: return "exception";
    }
}

void printFrame(HANDLE process, int index, DWORD64 pc) {
    char module[MAX_PATH] = "?";
    DWORD64 base = 0;
    HMODULE handle = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &handle) &&
        handle) {
        base = reinterpret_cast<DWORD64>(handle);
        char path[MAX_PATH];
        if (GetModuleFileNameA(handle, path, MAX_PATH)) {
            const char *slash = std::strrchr(path, '\\');
            std::snprintf(module, sizeof(module), "%s", slash ? slash + 1 : path);
        }
    }
    char buffer[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD column = 0;
    std::fprintf(stderr, "  #%-2d %s+0x%llx", index, module, static_cast<unsigned long long>(pc - base));
    if (SymFromAddr(process, pc, &displacement, symbol))
        std::fprintf(stderr, "  %s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
    if (SymGetLineFromAddr64(process, pc, &column, &line))
        std::fprintf(stderr, "  (%s:%lu)", line.FileName, line.LineNumber);
    std::fputc('\n', stderr);
}

// The frames of `thread` from `context` on, without the first `skip`.
// dbghelp is not thread-safe: one walk at a time.
void printFrames(CONTEXT context, HANDLE thread, int skip) {
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    const HANDLE process = GetCurrentProcess();
    static bool symbols = false;
    if (!symbols) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
        SymInitialize(process, nullptr, TRUE);
        symbols = true;
    }
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    constexpr int kMaxFrames = 48; // enough to show a recursion's cycle
    for (int i = 0; i < kMaxFrames + skip; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0)
            break;
        if (i >= skip)
            printFrame(process, i - skip, frame.AddrPC.Offset);
    }
    std::fflush(stderr);
    ReleaseSRWLockExclusive(&lock);
}

void report(EXCEPTION_POINTERS *info, HANDLE thread) {
    const EXCEPTION_RECORD &record = *info->ExceptionRecord;
    std::fprintf(stderr, "nova_cli: crashed: %s (0x%08lX)", exceptionName(record.ExceptionCode), record.ExceptionCode);
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
        std::fprintf(stderr, " %s address 0x%llx", record.ExceptionInformation[0] == 1 ? "writing" : "reading",
                     static_cast<unsigned long long>(record.ExceptionInformation[1]));
    std::fputc('\n', stderr);
    printFrames(*info->ContextRecord, thread, 0);
}

DWORD WINAPI reporter(LPVOID) {
    WaitForSingleObject(g_request, INFINITE);
    report(g_info, g_faulting);
    SetEvent(g_done);
    return 0;
}

[[noreturn]] void reportAndExit(EXCEPTION_POINTERS *info) {
    if (InterlockedExchange(&g_reporting, 1) == 0) { // the first crashing thread reports
        g_info = info;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_faulting, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        SetEvent(g_request);
        WaitForSingleObject(g_done, 15000);
    } else {
        Sleep(INFINITE); // the reporting thread ends the process
    }
    TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
    for (;;) {}
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS *info) { reportAndExit(info); }

// A stack overflow may leave no room to reach the unhandled-exception
// filter; vectored handlers run first. Everything else may still be handled
// (C++ exceptions, the CVEX bridge's guarded calls) and is left alone here.
LONG WINAPI onVectored(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
        reportAndExit(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() (std::terminate, a failed CRT check) ends the process through a
// fast-fail that no exception filter sees: report from the signal instead.
void onAbort(int) {
    CONTEXT context;
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xC0000409;
    EXCEPTION_POINTERS info{&record, &context};
    reportAndExit(&info);
}

} // namespace

namespace nova_cli {

void installCrashHandler() {
    g_request = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!g_request || !g_done)
        return;
    if (HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr))
        CloseHandle(thread);
    else
        return;
    SetUnhandledExceptionFilter(onUnhandled);
    AddVectoredExceptionHandler(1, onVectored);
    std::signal(SIGABRT, onAbort);
    // No "program has stopped working" dialog in batch runs.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

void printCallStack() {
    CONTEXT context;
    RtlCaptureContext(&context);
    printFrames(context, GetCurrentThread(), 1); // without this function
}

} // namespace nova_cli

#else

namespace nova_cli {
void installCrashHandler() {}
void printCallStack() {}
} // namespace nova_cli

#endif
