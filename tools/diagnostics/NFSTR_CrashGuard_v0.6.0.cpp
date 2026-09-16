#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <cstring>
#include <string>

// NFSTR_CrashGuard.asi v0.6.0
// Diagnostic-only manual-arm guard for Need for Speed: The Run v1.1 x86.
//
// The recurring Escape From S.F. crash reaches EXE RVA 0x0058CC80 with ECX == 0.
// That routine reports an entity GUID through the OnlineEntity path and crashes at
// RVA 0x0058CCA5 when it dereferences [this+0x14].
//
// Once armed with F6, this ASI guards only that exact routine:
//   - ECX != 0: replay the original six bytes and continue completely stock.
//   - ECX == 0: log ClientCareerManager state and return to the original caller.
//
// It deliberately does NOT modify game data, TOCs, bundles or the non-null path.

namespace guard {

static HMODULE g_self = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_stop = nullptr;
static PVOID g_veh = nullptr;
static CRITICAL_SECTION g_lock;
static LARGE_INTEGER g_freq{}, g_start{};
static volatile LONG g_armed = 0;
static volatile LONG g_guardCount = 0;
static uintptr_t g_exeBase = 0;

extern "C" {
    uintptr_t g_continue = 0;
    void GuardStub();
    void GuardNullC(uintptr_t caller);
}

static double Ms() {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return (double)(q.QuadPart - g_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static std::string ModRva(uintptr_t a) {
    if (!a) return "<null>";
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
        char b[32];
        _snprintf(b, sizeof(b)-1, "0x%08lX", (unsigned long)a);
        b[sizeof(b)-1] = 0;
        return b;
    }
    HMODULE m = (HMODULE)mbi.AllocationBase;
    char p[MAX_PATH]{};
    GetModuleFileNameA(m, p, MAX_PATH);
    const char* n = strrchr(p, '\\');
    n = n ? n + 1 : p;
    char b[MAX_PATH + 64];
    _snprintf(b, sizeof(b)-1, "%s+0x%08lX", n,
              (unsigned long)(a - (uintptr_t)m));
    b[sizeof(b)-1] = 0;
    return b;
}

static void Log(const char* cat, bool flush, const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(body, sizeof(body)-1, fmt, ap);
    va_end(ap);
    body[sizeof(body)-1] = 0;

    char line[4608];
    _snprintf(line, sizeof(line)-1, "[%011.3f] [TID %5lu] [%s] %s\r\n",
              Ms(), (unsigned long)GetCurrentThreadId(), cat, body);
    line[sizeof(line)-1] = 0;

    EnterCriticalSection(&g_lock);
    DWORD wr = 0;
    WriteFile(g_log, line, (DWORD)strlen(line), &wr, nullptr);
    if (flush) FlushFileBuffers(g_log);
    LeaveCriticalSection(&g_lock);
}

static bool ReadMem(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (void*)a, out, n, &got) && got == n;
}

static void LogCareerState(uintptr_t caller) {
    uintptr_t manager = 0;
    const uintptr_t managerGlobal = g_exeBase + 0x024823B4;
    ReadMem(managerGlobal, &manager, sizeof(manager));

    LONG count = InterlockedIncrement(&g_guardCount);
    if (!manager) {
        Log("GUARD", true,
            "#%ld NULL this suppressed caller=%s ClientCareerManager=<null> global=0x%08lX",
            (long)count, ModRva(caller).c_str(), (unsigned long)managerGlobal);
        return;
    }

    uint32_t id[4]{};
    uintptr_t live = 0;
    uint8_t resolvedFlag = 0;
    ReadMem(manager + 0x2C, id, sizeof(id));
    ReadMem(manager + 0x3C, &live, sizeof(live));
    ReadMem(manager + 0x40, &resolvedFlag, sizeof(resolvedFlag));

    uintptr_t online = 0;
    ReadMem(g_exeBase + 0x02488F7C, &online, sizeof(online));

    Log("GUARD", true,
        "#%ld NULL this suppressed caller=%s manager=0x%08lX targetID=%08lX-%08lX-%08lX-%08lX live(+3C)=0x%08lX flag(+40)=%u OnlineEntity=0x%08lX",
        (long)count, ModRva(caller).c_str(), (unsigned long)manager,
        (unsigned long)id[0], (unsigned long)id[1], (unsigned long)id[2], (unsigned long)id[3],
        (unsigned long)live, (unsigned)resolvedFlag, (unsigned long)online);
}

extern "C" void GuardNullC(uintptr_t caller) {
    LogCareerState(caller);
}

// At entry to RVA 0x0058CC80 the original stack contains only the caller return
// address. For the null path, pushfl + pushal move that return address to ESP+36.
// Returning directly is valid because the original function is __thiscall-like
// and ends in a plain RET with no callee-cleaned arguments.
asm(
    ".text\n"
    ".globl _GuardStub\n"
    "_GuardStub:\n"
    "    testl %ecx, %ecx\n"
    "    jz 1f\n"
    "    subl $0x50, %esp\n"
    "    pushl %edi\n"
    "    movl %ecx, %edi\n"
    "    jmpl *_g_continue\n"
    "1:\n"
    "    pushfl\n"
    "    pushal\n"
    "    movl 36(%esp), %eax\n"
    "    pushl %eax\n"
    "    call _GuardNullC\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    ret\n"
);

static bool InstallGuard() {
    const uintptr_t target = g_exeBase + 0x0058CC80;
    const uint8_t expected[6] = {0x83,0xEC,0x50,0x57,0x8B,0xF9};
    if (memcmp((void*)target, expected, sizeof(expected)) != 0) {
        uint8_t got[6]{};
        memcpy(got, (void*)target, sizeof(got));
        Log("ARM", true,
            "ABORT target bytes mismatch at %s got=%02X%02X%02X%02X%02X%02X expected=83EC50578BF9",
            ModRva(target).c_str(), got[0],got[1],got[2],got[3],got[4],got[5]);
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)target, 6, PAGE_EXECUTE_READWRITE, &old)) {
        Log("ARM", true, "ABORT VirtualProtect failed error=%lu", (unsigned long)GetLastError());
        return false;
    }

    g_continue = target + 6;
    uint8_t* p = (uint8_t*)target;
    p[0] = 0xE9;
    *(int32_t*)(p+1) = (int32_t)((uintptr_t)GuardStub - (target + 5));
    p[5] = 0x90;

    DWORD tmp = 0;
    VirtualProtect((void*)target, 6, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 6);

    Log("ARM", true, "null guard installed target=%s continue=%s stub=%s",
        ModRva(target).c_str(), ModRva(g_continue).c_str(), ModRva((uintptr_t)GuardStub).c_str());
    return true;
}

static void ScanCrashStack(CONTEXT* c) {
    if (!c) return;
    Log("CRASH", true, "EIP %s", ModRva((uintptr_t)c->Eip).c_str());
    uint32_t words[64]{};
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)(uintptr_t)c->Esp,
                           words, sizeof(words), &got)) return;
    size_t count = got / sizeof(uint32_t);
    for (size_t i=0; i<count; ++i) {
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t a = words[i];
        if (a && VirtualQuery((void*)a, &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) {
            Log("CRASHSTK", true, "ESP+0x%02lX = 0x%08lX -> %s",
                (unsigned long)(i*4), (unsigned long)a, ModRva(a).c_str());
        }
    }
}

static LONG CALLBACK Veh(PEXCEPTION_POINTERS ep) {
    if (!InterlockedCompareExchange(&g_armed,0,0)) return EXCEPTION_CONTINUE_SEARCH;
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch(code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT* x = ep->ContextRecord;
    Log("CRASH", true,
        "EXCEPTION code=0x%08lX addr=%s EIP=%08lX ESP=%08lX EBP=%08lX EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX",
        (unsigned long)code, ModRva((uintptr_t)ep->ExceptionRecord->ExceptionAddress).c_str(),
        (unsigned long)x->Eip,(unsigned long)x->Esp,(unsigned long)x->Ebp,
        (unsigned long)x->Eax,(unsigned long)x->Ebx,(unsigned long)x->Ecx,
        (unsigned long)x->Edx,(unsigned long)x->Esi,(unsigned long)x->Edi);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        Log("CRASH", true, "ACCESS type=%llu address=0x%08llX",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    ScanCrashStack(x);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI Worker(LPVOID) {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    g_exeBase = (uintptr_t)GetModuleHandleW(nullptr);

    wchar_t me[MAX_PATH]{};
    GetModuleFileNameW(g_self, me, MAX_PATH);
    wchar_t* slash = wcsrchr(me, L'\\');
    if (slash) *(slash+1) = 0;
    wchar_t lp[MAX_PATH]{};
    _snwprintf(lp, MAX_PATH-1, L"%sNFSTR_CrashGuard.log", me);

    g_log = CreateFileW(lp, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH, nullptr);
    if (g_log == INVALID_HANDLE_VALUE) return 1;
    const unsigned char bom[3] = {0xEF,0xBB,0xBF};
    DWORD wr = 0;
    WriteFile(g_log, bom, 3, &wr, nullptr);

    Log("INIT", true, "NFSTR Crash Guard v0.6.0 loaded PID=%lu EXEbase=0x%p",
        (unsigned long)GetCurrentProcessId(), (void*)g_exeBase);
    Log("INIT", true,
        "SAFE MODE: nothing patched until F6. Guard target RVA=0x0058CC80; only NULL ECX calls are suppressed.");

    bool prev = false;
    while (WaitForSingleObject(g_stop, 50) == WAIT_TIMEOUT) {
        bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (down && !prev && !InterlockedCompareExchange(&g_armed,0,0)) {
            bool ok = InstallGuard();
            if (ok) {
                g_veh = AddVectoredExceptionHandler(1, Veh);
                InterlockedExchange(&g_armed,1);
                Log("ARM", true, "F6 ARMED: null guard=yes VEH=%s", g_veh?"yes":"no");
                Beep(1000,120);
                Beep(1300,120);
            } else {
                Log("ARM", true, "F6 FAILED: guard not installed");
                Beep(350,300);
            }
        }
        prev = down;
    }

    Log("INIT", true, "stopping guardCount=%ld", (long)g_guardCount);
    FlushFileBuffers(g_log);
    CloseHandle(g_log);
    g_log = INVALID_HANDLE_VALUE;
    return 0;
}

} // namespace guard

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    using namespace guard;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = (HMODULE)h;
        DisableThreadLibraryCalls(h);
        g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_stop) SetEvent(g_stop);
    }
    return TRUE;
}
