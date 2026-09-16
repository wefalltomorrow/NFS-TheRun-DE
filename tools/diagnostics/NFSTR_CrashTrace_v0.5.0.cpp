#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <cstring>

// NFSTR_CrashTrace.asi v0.5.0
// Targeted manual-arm tracer for the recurring Test 14-17 crash at
// Need For Speed The Run.exe + 0x0058CCA5.
//
// It hooks the containing function at RVA 0x0058CC80 before the fault,
// records ECX (the member-function this pointer), the immediate caller,
// and a raw stack window, then resumes the original instructions unchanged.
// It does NOT suppress or alter the crash.

namespace ct {

static HMODULE g_self = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_stop = nullptr;
static PVOID g_veh = nullptr;
static CRITICAL_SECTION g_logLock;
static LARGE_INTEGER g_freq{}, g_start{};
static volatile LONG g_armed = 0;
static volatile LONG g_callId = 0;

extern "C" {
    uintptr_t g_targetContinue = 0;
    void TargetHookAsm();
    void TargetLogC(uintptr_t thisPtr, uintptr_t caller, uintptr_t entryEsp);
}

static double Ms() {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return (double)(q.QuadPart - g_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static std::string W2U(const wchar_t* s) {
    if (!s || !*s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr)) return {};
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

static std::string ModRva(uintptr_t a) {
    if (!a) return "<null>";
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((const void*)a, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
        char x[32];
        _snprintf(x, sizeof(x)-1, "0x%08lX", (unsigned long)a);
        x[sizeof(x)-1] = 0;
        return x;
    }
    HMODULE m = (HMODULE)mbi.AllocationBase;
    wchar_t w[MAX_PATH]{};
    GetModuleFileNameW(m, w, MAX_PATH);
    std::string p = W2U(w);
    size_t k = p.find_last_of("\\/");
    if (k != std::string::npos) p = p.substr(k + 1);
    char x[MAX_PATH + 64];
    _snprintf(x, sizeof(x)-1, "%s+0x%08lX", p.c_str(),
              (unsigned long)(a - (uintptr_t)m));
    x[sizeof(x)-1] = 0;
    return x;
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

    EnterCriticalSection(&g_logLock);
    DWORD wr = 0;
    WriteFile(g_log, line, (DWORD)strlen(line), &wr, nullptr);
    if (flush) FlushFileBuffers(g_log);
    LeaveCriticalSection(&g_logLock);
}

static bool SafeRead(uintptr_t address, void* out, size_t size) {
    if (!address || !out || !size) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void*)address, out, size, &got) && got == size;
}

static bool IsLikelyCode(uintptr_t address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((const void*)address, &mbi, sizeof(mbi))) return false;
    DWORD p = mbi.Protect & 0xFFu;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

extern "C" void TargetLogC(uintptr_t thisPtr, uintptr_t caller, uintptr_t entryEsp) {
    LONG id = InterlockedIncrement(&g_callId);
    const bool nullThis = thisPtr == 0;
    Log("TARGET", nullThis,
        "#%ld ENTER RVA=0x0058CC80 this=0x%08lX caller=%s entryESP=0x%08lX%s",
        (long)id, (unsigned long)thisPtr, ModRva(caller).c_str(),
        (unsigned long)entryEsp, nullThis ? " *** NULL THIS ***" : "");

    // Dump raw entry stack. slot 0 is the immediate return address.
    uint32_t words[24]{};
    if (SafeRead(entryEsp, words, sizeof(words))) {
        for (int i = 0; i < 24; ++i) {
            uintptr_t v = (uintptr_t)words[i];
            if (IsLikelyCode(v)) {
                Log("STACK", nullThis,
                    "#%ld S[%02d]=0x%08lX -> %s",
                    (long)id, i, (unsigned long)v, ModRva(v).c_str());
            } else {
                Log("STACK", false,
                    "#%ld S[%02d]=0x%08lX",
                    (long)id, i, (unsigned long)v);
            }
        }
    } else {
        Log("STACK", true, "#%ld unable to read entry stack", (long)id);
    }
}

// Save all registers/flags, call logger with original ECX, immediate caller,
// and the original entry ESP, then reproduce the six overwritten bytes:
//   83 EC 50    sub esp,50h
//   57          push edi
//   8B F9       mov edi,ecx
asm(
    ".text\n"
    ".globl _TargetHookAsm\n"
    "_TargetHookAsm:\n"
    "    pushfl\n"
    "    pushal\n"
    "    movl 24(%esp), %eax\n"   // original ECX / this
    "    movl 36(%esp), %edx\n"   // immediate return address
    "    leal 36(%esp), %ebx\n"   // original entry ESP points at return address
    "    pushl %ebx\n"
    "    pushl %edx\n"
    "    pushl %eax\n"
    "    call _TargetLogC\n"
    "    addl $12, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    subl $0x50, %esp\n"
    "    pushl %edi\n"
    "    movl %ecx, %edi\n"
    "    jmpl *_g_targetContinue\n"
);

static bool PatchTarget() {
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    uintptr_t at = base + 0x0058CC80;
    const uint8_t expected[6] = {0x83,0xEC,0x50,0x57,0x8B,0xF9};

    uint8_t actual[6]{};
    if (!SafeRead(at, actual, sizeof(actual))) {
        Log("ARM", true, "FAILED reading target bytes at %s", ModRva(at).c_str());
        return false;
    }
    if (memcmp(actual, expected, sizeof(expected)) != 0) {
        char b[64]{};
        _snprintf(b, sizeof(b)-1, "%02X %02X %02X %02X %02X %02X",
                  actual[0],actual[1],actual[2],actual[3],actual[4],actual[5]);
        Log("ARM", true,
            "ABORT target-byte mismatch at %s: got %s expected 83 EC 50 57 8B F9",
            ModRva(at).c_str(), b);
        return false;
    }

    intptr_t rel64 = (intptr_t)(uintptr_t)TargetHookAsm - (intptr_t)(at + 5);
    if (rel64 < INT32_MIN || rel64 > INT32_MAX) {
        Log("ARM", true, "ABORT hook stub is outside rel32 range");
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)at, 6, PAGE_EXECUTE_READWRITE, &old)) {
        Log("ARM", true, "FAILED VirtualProtect at target error=%lu", (unsigned long)GetLastError());
        return false;
    }

    uint8_t patch[6] = {0xE9,0,0,0,0,0x90};
    *(int32_t*)&patch[1] = (int32_t)rel64;
    memcpy((void*)at, patch, sizeof(patch));
    DWORD tmp = 0;
    VirtualProtect((void*)at, 6, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 6);

    g_targetContinue = at + 6;
    Log("ARM", true, "target hook installed at %s continue=%s stub=%s",
        ModRva(at).c_str(), ModRva(g_targetContinue).c_str(),
        ModRva((uintptr_t)TargetHookAsm).c_str());
    return true;
}

static void DumpCrashStack(CONTEXT* c) {
    if (!c) return;
    Log("CRASH", true, "EIP %s", ModRva((uintptr_t)c->Eip).c_str());

    uint32_t words[48]{};
    if (SafeRead((uintptr_t)c->Esp, words, sizeof(words))) {
        for (int i = 0; i < 48; ++i) {
            uintptr_t v = (uintptr_t)words[i];
            if (IsLikelyCode(v))
                Log("CRASHSTK", true, "ESP+0x%02X = 0x%08lX -> %s",
                    i*4, (unsigned long)v, ModRva(v).c_str());
        }
    }

    uintptr_t ebp=(uintptr_t)c->Ebp;
    for (int i=0; i<16 && ebp; ++i) {
        struct Frame { uintptr_t prev; uintptr_t ret; } fr{};
        if (!SafeRead(ebp, &fr, sizeof(fr)) || !fr.ret) break;
        Log("CRASHFP", true, "[%02d] EBP=0x%08lX ret=%s",
            i, (unsigned long)ebp, ModRva(fr.ret).c_str());
        if (fr.prev <= ebp || fr.prev - ebp > 0x100000) break;
        ebp = fr.prev;
    }
}

static LONG CALLBACK Veh(PEXCEPTION_POINTERS ep) {
    if (!InterlockedCompareExchange(&g_armed,0,0)) return EXCEPTION_CONTINUE_SEARCH;
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code=ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* x=ep->ContextRecord;
    Log("CRASH",true,
        "EXCEPTION code=0x%08lX addr=%s EIP=%08lX ESP=%08lX EBP=%08lX EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX",
        (unsigned long)code,ModRva((uintptr_t)ep->ExceptionRecord->ExceptionAddress).c_str(),
        (unsigned long)x->Eip,(unsigned long)x->Esp,(unsigned long)x->Ebp,
        (unsigned long)x->Eax,(unsigned long)x->Ebx,(unsigned long)x->Ecx,
        (unsigned long)x->Edx,(unsigned long)x->Esi,(unsigned long)x->Edi);
    if (code==EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters>=2)
        Log("CRASH",true,"ACCESS type=%llu address=0x%08llX",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    DumpCrashStack(x);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI Worker(LPVOID) {
    InitializeCriticalSection(&g_logLock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);

    wchar_t me[MAX_PATH]{};
    GetModuleFileNameW(g_self,me,MAX_PATH);
    wchar_t* slash=wcsrchr(me,L'\\');
    if (slash) *(slash+1)=0;
    wchar_t logPath[MAX_PATH]{};
    _snwprintf(logPath,MAX_PATH-1,L"%sNFSTR_CrashTrace.log",me);

    g_log=CreateFileW(logPath,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                      CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if (g_log==INVALID_HANDLE_VALUE) return 1;
    const unsigned char bom[3]={0xEF,0xBB,0xBF};
    DWORD wr=0; WriteFile(g_log,bom,3,&wr,nullptr);

    Log("INIT",true,"NFSTR Crash Trace v0.5.0 loaded PID=%lu EXEbase=0x%p",
        (unsigned long)GetCurrentProcessId(),GetModuleHandleW(nullptr));
    Log("INIT",true,"SAFE MODE: no game code patched until F6. Target RVA=0x0058CC80; crash RVA=0x0058CCA5.");

    bool prevF6=false;
    while (WaitForSingleObject(g_stop,50)==WAIT_TIMEOUT) {
        bool down=(GetAsyncKeyState(VK_F6)&0x8000)!=0;
        if (down && !prevF6 && !InterlockedCompareExchange(&g_armed,0,0)) {
            bool ok=PatchTarget();
            if (ok) {
                g_veh=AddVectoredExceptionHandler(1,Veh);
                InterlockedExchange(&g_armed,1);
                Log("ARM",true,"F6 ARMED: target hook=yes VEH=%s",g_veh?"yes":"no");
                Beep(1000,120); Beep(1300,120);
            } else {
                Log("ARM",true,"F6 FAILED: target hook not installed");
                Beep(350,300);
            }
        }
        prevF6=down;
    }

    Log("INIT",true,"stopping");
    FlushFileBuffers(g_log);
    CloseHandle(g_log);
    g_log=INVALID_HANDLE_VALUE;
    return 0;
}

} // namespace ct

BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
    using namespace ct;
    if (reason==DLL_PROCESS_ATTACH) {
        g_self=(HMODULE)h;
        DisableThreadLibraryCalls(h);
        g_stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE t=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
        if (t) CloseHandle(t);
    } else if (reason==DLL_PROCESS_DETACH) {
        if (g_stop) SetEvent(g_stop);
    }
    return TRUE;
}
