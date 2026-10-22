// SPDX-License-Identifier: GPL-2.0-or-later
// Freeze watchdog (ISS_FREEZE_WATCHDOG_MS, off by default; the launcher's "Freeze and crash
// reports" sets 3000): when no frame has been presented for that long, the stacks of all threads go to freeze-<n>.txt next to frames.csv, and again
// at three times that long while the freeze lasts (who waits, who spins: each thread's CPU time).
// Seen on an RTX 4060 (2026-10-22): the command processor stood 10-15 s, the GPU idle, six guest
// threads waiting for readbacks, in Second Son and First Light alike.
// Each thread is suspended only while its context is read and its stack unwound (its own unwind
// data, frame pointers through guest code); module names and export names come after it resumes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "freeze_watchdog.h"

namespace iss::freeze_watchdog {
namespace {

std::atomic<uint64_t> g_presents{0};
std::atomic<uint64_t> g_last_present_ms{0};
std::atomic<bool> g_started{false};
uint64_t g_image_base = 0;

uint64_t NowMs() { return GetTickCount64(); }

struct ThreadStack {
    DWORD tid = 0;
    std::wstring name;
    uint64_t cpu_ms = 0;
    int priority = 0;
    uint64_t frames[48] = {};
    int count = 0;
    bool ok = false;
};

bool ReadU64(uint64_t address, uint64_t& value) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &value, 8, &read) &&
           read == 8;
}

struct Range {
    uint64_t lo, hi;
};

// The loaded modules, listed before any thread is suspended: a suspended thread may hold the
// loader lock, so nothing that takes it may run until it resumes.
std::vector<Range> ModuleRanges() {
    std::vector<Range> ranges;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return ranges;
    MODULEENTRY32W entry{sizeof(entry)};
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        const auto lo = reinterpret_cast<uint64_t>(entry.modBaseAddr);
        ranges.push_back({lo, lo + entry.modBaseSize});
    }
    CloseHandle(snapshot);
    return ranges;
}

// The stack of a suspended thread: the unwind data of each host module, frame pointers through
// code without it (guest code), the return address on top of the stack for a leaf.
void Unwind(CONTEXT context, const std::vector<Range>& modules, ThreadStack& out) {
    for (int i = 0; i < 48 && context.Rip; ++i) {
        out.frames[out.count++] = context.Rip;
        DWORD64 base = 0;
        if (RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &base, nullptr)) {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handler_data,
                             &establisher, nullptr);
            continue;
        }
        const uint64_t rip = context.Rip;
        if (std::any_of(modules.begin(), modules.end(), [rip](const Range& r) { return rip >= r.lo && rip < r.hi; })) {
            uint64_t ret = 0; // a leaf function of a module: its return address is on top
            if (!ReadU64(context.Rsp, ret)) break;
            context.Rip = ret;
            context.Rsp += 8;
            continue;
        }
        uint64_t next_rbp = 0, ret = 0; // guest code: the frame pointer chain
        if (context.Rbp <= context.Rsp || !ReadU64(context.Rbp, next_rbp) || !ReadU64(context.Rbp + 8, ret)) break;
        context.Rsp = context.Rbp + 16;
        context.Rbp = next_rbp;
        context.Rip = ret;
    }
}

// The export of a loaded module nearest below rva, else empty.
std::string NearestExport(HMODULE module, uint64_t rva) {
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return {};
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    DWORD best = 0;
    const char* best_name = nullptr;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const DWORD f = functions[ordinals[i]];
        if (f <= rva && f > best && (f < dir.VirtualAddress || f >= dir.VirtualAddress + dir.Size)) {
            best = f;
            best_name = reinterpret_cast<const char*>(base + names[i]);
        }
    }
    if (!best_name) return {};
    char text[300];
    std::snprintf(text, sizeof(text), " (%s+0x%llx)", best_name, static_cast<unsigned long long>(rva - best));
    return text;
}

std::string Describe(uint64_t rip) {
    char text[600];
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(rip), &module) && module) {
        char path[MAX_PATH] = "?";
        GetModuleFileNameA(module, path, sizeof(path));
        const char* file = std::strrchr(path, '\\');
        const uint64_t rva = rip - reinterpret_cast<uint64_t>(module);
        std::snprintf(text, sizeof(text), "%s+0x%llx%s", file ? file + 1 : path,
                      static_cast<unsigned long long>(rva), NearestExport(module, rva).c_str());
    } else if (g_image_base && rip >= g_image_base && rip - g_image_base < (uint64_t(1) << 30)) {
        std::snprintf(text, sizeof(text), "eboot+0x%llx", static_cast<unsigned long long>(rip - g_image_base));
    } else {
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(rip));
    }
    return text;
}

std::string LogDir() {
    if (const char* p = std::getenv("SHADPS4_FRAME_CSV"); p && *p) {
        std::string dir = p;
        const size_t cut = dir.find_last_of("/\\");
        if (cut != std::string::npos) return dir.substr(0, cut);
    }
    return ".";
}

uint64_t CpuMs(HANDLE thread) {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
    const auto t = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (t(kernel) + t(user)) / 10000;
}

// Every thread's stack into path: appended when pass, after a header line.
void DumpTo(const std::string& path, bool append, const std::string& header) {
    std::vector<ThreadStack> stacks;
    stacks.reserve(512);
    const std::vector<Range> modules = ModuleRanges();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{sizeof(entry)};
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   entry.th32ThreadID);
        if (!thread) continue;
        ThreadStack s;
        s.tid = entry.th32ThreadID;
        PWSTR name = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &name)) && name) {
            s.name = name;
            LocalFree(name);
        }
        s.cpu_ms = CpuMs(thread);
        s.priority = GetThreadPriority(thread);
        if (SuspendThread(thread) != DWORD(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                s.ok = true;
                Unwind(context, modules, s);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
        stacks.push_back(std::move(s));
    }
    CloseHandle(snapshot);

    FILE* f = std::fopen(path.c_str(), append ? "a" : "w");
    if (!f) return;
    std::fprintf(f, "%s; %zu threads\n", header.c_str(), stacks.size());
    for (const auto& s : stacks) {
        char name[200] = "";
        WideCharToMultiByte(CP_UTF8, 0, s.name.c_str(), -1, name, sizeof(name), nullptr, nullptr);
        std::fprintf(f, "\nthread %lu \"%s\" cpu_ms=%llu priority=%d%s\n", s.tid, name,
                     static_cast<unsigned long long>(s.cpu_ms), s.priority, s.ok ? "" : " (no context)");
        for (int i = 0; i < s.count; ++i)
            std::fprintf(f, "  %2d %s\n", i, Describe(s.frames[i]).c_str());
    }
    std::fclose(f);
}

void Dump(int number, int pass, uint64_t stalled_ms) {
    const std::string path = LogDir() + "\\freeze-" + std::to_string(number) + ".txt";
    char header[200];
    std::snprintf(header, sizeof(header), "%s no frame presented for %llu ms (presents %llu)",
                  pass ? "\n=== AGAIN:" : "=== FREEZE:", static_cast<unsigned long long>(stalled_ms),
                  static_cast<unsigned long long>(g_presents.load()));
    DumpTo(path, pass != 0, header);
    std::fprintf(stderr, "FREEZE_WATCHDOG stalled_ms=%llu dump=%s pass=%d\n",
                 static_cast<unsigned long long>(stalled_ms), path.c_str(), pass);
    std::fflush(stderr);
}

void Watch(uint64_t threshold_ms) {
    SetThreadDescription(GetCurrentThread(), L"ISS:FreezeWatchdog");
    int freezes = 0, passes = 0;
    uint64_t watched = 0, since = 0;
    for (;;) {
        Sleep(250);
        const uint64_t presents = g_presents.load(std::memory_order_relaxed);
        const uint64_t stalled = NowMs() - g_last_present_ms.load(std::memory_order_relaxed);
        if (presents != watched) {
            if (passes)
                std::fprintf(stderr, "FREEZE_WATCHDOG freeze %d ended after about %llu ms\n", freezes,
                             static_cast<unsigned long long>(NowMs() - since));
            watched = presents;
            passes = 0;
            continue;
        }
        if (passes == 0 && stalled >= threshold_ms) {
            ++freezes;
            since = NowMs() - stalled;
            Dump(freezes, passes++, stalled);
        } else if (passes == 1 && stalled >= 3 * threshold_ms) {
            Dump(freezes, passes++, stalled);
        }
    }
}

} // namespace

void OnPresent() {
    g_last_present_ms.store(NowMs(), std::memory_order_relaxed);
    g_presents.fetch_add(1, std::memory_order_relaxed);
    if (g_started.load(std::memory_order_relaxed) || g_started.exchange(true)) return;
    const char* p = std::getenv("ISS_FREEZE_WATCHDOG_MS");
    const uint64_t threshold = p && *p ? std::strtoull(p, nullptr, 10) : 0;
    if (threshold) std::thread(Watch, threshold).detach();
}

void SetImageBase(uint64_t base) { g_image_base = base; }

} // namespace iss::freeze_watchdog

// A host fault the game cannot survive (src/probe.c host_fault): every thread's stack into
// crash-threads.txt next to frames.csv, the faulting thread (tid) marked, before the process ends.
extern "C" __declspec(dllexport) void iss_gpu_dump_threads(unsigned long tid) {
    using namespace iss::freeze_watchdog;
    char header[160];
    std::snprintf(header, sizeof(header), "=== CRASH: host fault on thread %lu (presents %llu)", tid,
                  static_cast<unsigned long long>(g_presents.load()));
    DumpTo(LogDir() + "\\crash-threads.txt", false, header);
}
