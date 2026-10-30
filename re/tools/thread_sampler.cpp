// Samples the busiest thread of a process: prints RIP and module-relative return addresses found on its stack.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <vector>
#include <string>

struct Mod { uintptr_t base, size; std::string name; };
std::vector<Mod> g_mods;

std::string Where(uintptr_t a)
{
    for (auto& m : g_mods)
        if (a >= m.base && a < m.base + m.size) {
            char b[300];
            snprintf(b, sizeof(b), "%s+%llx", m.name.c_str(), (unsigned long long)(a - m.base));
            return b;
        }
    return "";
}

int main(int argc, char** argv)
{
    DWORD pid = strtoul(argv[1], nullptr, 10);
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { printf("OpenProcess failed %lu\n", GetLastError()); return 1; }
    HMODULE mods[1024]; DWORD need;
    EnumProcessModulesEx(proc, mods, sizeof(mods), &need, LIST_MODULES_ALL);
    for (DWORD i = 0; i < need / sizeof(HMODULE); ++i) {
        MODULEINFO mi; char name[MAX_PATH];
        GetModuleInformation(proc, mods[i], &mi, sizeof(mi));
        GetModuleBaseNameA(proc, mods[i], name, sizeof(name));
        g_mods.push_back({(uintptr_t)mi.lpBaseOfDll, mi.SizeOfImage, name});
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    DWORD best = 0; ULONGLONG bestT = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        FILETIME c, e, k, u;
        if (t && GetThreadTimes(t, &c, &e, &k, &u)) {
            ULONGLONG v = ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime) + ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime);
            if (v > bestT) { bestT = v; best = te.th32ThreadID; }
        }
        if (t) CloseHandle(t);
    }
    printf("busiest thread %lu cpu %.1fs\n", best, bestT / 1e7);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, best);
    for (int s = 0; s < 8; ++s) {
        SuspendThread(th);
        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
        GetThreadContext(th, &ctx);
        printf("sample %d rip=%llx %s\n", s, ctx.Rip, Where(ctx.Rip).c_str());
        if (s == 0) {
            uintptr_t stack[512]; SIZE_T got = 0;
            ReadProcessMemory(proc, (void*)ctx.Rsp, stack, sizeof(stack), &got);
            int shown = 0;
            for (size_t i = 0; i < got / 8 && shown < 30; ++i) {
                std::string w = Where(stack[i]);
                if (!w.empty()) { printf("   [rsp+%zx] %s\n", i * 8, w.c_str()); ++shown; }
            }
        }
        ResumeThread(th);
        Sleep(50);
    }
}
