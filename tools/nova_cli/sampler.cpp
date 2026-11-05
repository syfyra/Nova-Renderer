#include "sampler.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format off
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>
// clang-format on
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#endif

namespace {

constexpr int kFrames = 24;   // the instruction pointer and up to 23 return addresses
constexpr size_t kMaxSamples = 1u << 20;

struct Sample {
    uint64_t pc[kFrames];
    uint8_t count;
};

} // namespace

struct NovaSampler::Impl {
    std::vector<Sample> samples;
    std::atomic<bool> running{false};
    std::thread thread;
    uint32_t mainThread = 0;

#if defined(_WIN32)
    // A sampled thread: its handle and its thread information block (the
    // stack's bounds).
    struct Target {
        HANDLE handle = nullptr;
        const NT_TIB *tib = nullptr;
    };

    // The TIB (the start of the TEB) of thread `h`, from ntdll; null if
    // unavailable (the thread is then sampled at its instruction pointer).
    static const NT_TIB *tibOf(HANDLE h) {
        struct BasicInformation { // THREAD_BASIC_INFORMATION
            LONG exitStatus;
            PVOID tebBaseAddress;
            HANDLE clientId[2];
            ULONG_PTR affinityMask;
            LONG priority, basePriority;
        };
        using Query = LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        static const auto query =
            reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        BasicInformation info{};
        if (!query || query(h, 0 /* ThreadBasicInformation */, &info, ULONG(sizeof(info)), nullptr) < 0)
            return nullptr;
        return static_cast<const NT_TIB *>(info.tebBaseAddress);
    }

    // Copies a suspended thread's stack from `rsp` up to its base, at most
    // `cap` bytes: plain reads, which take no lock the thread may hold. 0 if
    // rsp is not on the thread's stack (a fiber) or the memory is gone.
    static size_t copyStack(const NT_TIB *tib, DWORD64 rsp, void *dst, size_t cap) {
        if (!tib)
            return 0;
        __try {
            const auto base = reinterpret_cast<DWORD64>(tib->StackBase);
            const auto limit = reinterpret_cast<DWORD64>(tib->StackLimit);
            if (rsp < limit || rsp >= base)
                return 0;
            const size_t n = size_t(std::min<DWORD64>(base - rsp, DWORD64(cap)));
            std::memcpy(dst, reinterpret_cast<const void *>(rsp), n);
            return n;
        } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                     : EXCEPTION_CONTINUE_SEARCH) {
            return 0;
        }
    }

    // Points `ctx` and the copied stack `[copy, copy + n)` at the copy of
    // the stack at `rsp`: the registers and saved values that point into the
    // original (frame pointers, saved rsp) move by the same offset.
    static void rebase(CONTEXT &ctx, DWORD64 *copy, size_t n) {
        const DWORD64 lo = ctx.Rsp, hi = ctx.Rsp + n;
        const DWORD64 delta = reinterpret_cast<DWORD64>(copy) - lo;
        const auto shift = [&](DWORD64 &v) {
            if (v >= lo && v < hi)
                v += delta;
        };
        for (size_t i = 0; i < n / sizeof(DWORD64); ++i)
            shift(copy[i]);
        for (DWORD64 *r : {&ctx.Rbp, &ctx.Rbx, &ctx.Rsi, &ctx.Rdi, &ctx.R12, &ctx.R13, &ctx.R14, &ctx.R15})
            shift(*r);
        ctx.Rsp = reinterpret_cast<DWORD64>(copy);
    }

    // Walks a stack copy `[lo, hi)` from its context (x64 unwind data; a
    // frame without it is a leaf whose return address is at rsp). Runs with
    // the thread resumed: RtlLookupFunctionEntry takes the loader's function
    // table lock, which a suspended thread inside LoadLibrary may hold.
    static void walk(CONTEXT ctx, Sample &s, DWORD64 lo, DWORD64 hi) {
        s.count = 0;
        __try {
            for (int i = 0; i < kFrames && ctx.Rip; ++i) {
                s.pc[s.count++] = ctx.Rip;
                DWORD64 base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                if (!fn) {
                    if (ctx.Rsp < lo || ctx.Rsp + 8 > hi)
                        break;
                    ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                    ctx.Rsp += 8;
                    continue;
                }
                void *handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
                if (ctx.Rsp < lo || ctx.Rsp > hi) // beyond the copy (truncated): no more frames
                    break;
            }
        } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                     : EXCEPTION_CONTINUE_SEARCH) {
            // A frame read outside the copy: the frames so far.
        }
    }

    void loop() {
        timeBeginPeriod(1);
        const DWORD self = GetCurrentThreadId();
        std::unordered_map<DWORD, Target> targets;
        // The copy of a sampled stack, allocated here: nothing may allocate
        // while a thread is suspended (it may hold the heap lock).
        constexpr size_t kStackCopy = size_t(512) << 10;
        std::vector<DWORD64> stack(kStackCopy / sizeof(DWORD64));
        ULONGLONG lastList = 0;
        while (running.load()) {
            if (GetTickCount64() - lastList > 100) { // new threads (TBB starts its workers lazily)
                lastList = GetTickCount64();
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te{sizeof(te)};
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self ||
                        te.th32ThreadID == mainThread || targets.count(te.th32ThreadID))
                        continue;
                    if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                              FALSE, te.th32ThreadID))
                        targets[te.th32ThreadID] = Target{h, tibOf(h)};
                }
                CloseHandle(snap);
            }
            for (auto &[id, t] : targets) {
                if (samples.size() >= kMaxSamples)
                    break;
                // Suspended only for the registers and a copy of the stack;
                // the walk (which takes loader locks) runs once it resumed.
                if (SuspendThread(t.handle) == DWORD(-1))
                    continue;
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                const bool got = GetThreadContext(t.handle, &ctx) != 0;
                const size_t n = got ? copyStack(t.tib, ctx.Rsp, stack.data(), kStackCopy) : 0;
                ResumeThread(t.handle);
                if (!got)
                    continue;
                Sample s;
                if (n == 0) {
                    s.pc[0] = ctx.Rip;
                    s.count = ctx.Rip ? 1 : 0;
                } else {
                    rebase(ctx, stack.data(), n);
                    const auto lo = reinterpret_cast<DWORD64>(stack.data());
                    walk(ctx, s, lo, lo + n);
                }
                samples.push_back(s);
            }
            Sleep(1);
        }
        for (auto &[id, t] : targets)
            CloseHandle(t.handle);
        timeEndPeriod(1);
    }
#endif
};

NovaSampler::NovaSampler() : _impl(std::make_unique<Impl>()) {}

NovaSampler::~NovaSampler() {
    if (_impl->running.exchange(false) && _impl->thread.joinable())
        _impl->thread.join();
}

void NovaSampler::start() {
#if defined(_WIN32)
    _impl->samples.reserve(kMaxSamples);
    _impl->mainThread = GetCurrentThreadId();
    _impl->running = true;
    _impl->thread = std::thread([this] { _impl->loop(); });
#endif
}

bool NovaSampler::stop(std::string const &path) {
    if (_impl->running.exchange(false) && _impl->thread.joinable())
        _impl->thread.join();
    FILE *out = nullptr;
#if defined(_WIN32)
    if (fopen_s(&out, path.c_str(), "w") != 0)
        out = nullptr;
#else
    out = std::fopen(path.c_str(), "w");
#endif
    if (!out)
        return false;
#if defined(_WIN32)
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    struct Where {
        std::string function, line;
    };
    std::unordered_map<uint64_t, Where> cache;
    const auto resolve = [&](uint64_t pc) -> Where const & {
        auto it = cache.find(pc);
        if (it != cache.end())
            return it->second;
        Where w;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 displacement = 0;
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        const std::string mod = SymGetModuleInfo64(process, pc, &module) ? module.ModuleName : "?";
        w.function = SymFromAddr(process, pc, &displacement, sym) ? mod + "!" + sym->Name : mod + "!?";
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, pc, &lineDisplacement, &line)) {
            std::string file = line.FileName;
            const size_t slash = file.find_last_of("/\\");
            w.line = (slash == std::string::npos ? file : file.substr(slash + 1)) + ":" + std::to_string(line.LineNumber);
        } else {
            w.line = w.function;
        }
        return cache.emplace(pc, std::move(w)).first->second;
    };

    // Threads waiting in the kernel (idle workers, the host) are not work.
    const auto idle = [](std::string const &f) {
        return f.rfind("ntdll!Nt", 0) == 0 || f.rfind("ntdll!Zw", 0) == 0 || f.rfind("win32u!", 0) == 0 ||
               f.find("!SwitchToThread") != std::string::npos || f.find("WaitFor") != std::string::npos ||
               f.find("!Sleep") != std::string::npos;
    };
    std::map<std::string, size_t> self, inclusive, lines;
    size_t busy = 0;
    for (const Sample &s : _impl->samples) {
        if (s.count == 0)
            continue;
        const Where &top = resolve(s.pc[0]);
        if (idle(top.function))
            continue;
        ++busy;
        ++self[top.function];
        ++lines[top.line];
        std::set<std::string> seen;
        for (int i = 0; i < s.count; ++i)
            // Return addresses point after the call: look up the call itself.
            seen.insert(resolve(i == 0 ? s.pc[i] : s.pc[i] - 1).function);
        for (std::string const &f : seen)
            ++inclusive[f];
    }
    SymCleanup(process);
    const auto print = [&](char const *title, std::map<std::string, size_t> const &m, size_t top) {
        std::vector<std::pair<size_t, std::string>> v;
        for (auto const &[k, n] : m)
            v.emplace_back(n, k);
        std::sort(v.rbegin(), v.rend());
        std::fprintf(out, "\n%s\n", title);
        for (size_t i = 0; i < std::min(top, v.size()); ++i)
            std::fprintf(out, "%6.2f %%  %8zu  %s\n", 100.0 * double(v[i].first) / double(std::max<size_t>(busy, 1)),
                         v[i].first, v[i].second.c_str());
    };
    std::fprintf(out, "%zu samples, %zu busy\n", _impl->samples.size(), busy);
    print("Self (function):", self, 60);
    print("Inclusive (function):", inclusive, 60);
    print("Self (line):", lines, 80);
#else
    std::fprintf(out, "nova_cli --profile is only implemented on Windows\n");
#endif
    std::fclose(out);
    return true;
}
