#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <algorithm>
#include <cwctype>
#include <cstring>

// NFSTR_InternalSeekCallerTrace.asi v0.1.0
// Need for Speed: The Run v1.1 x86 targeted diagnostic.
//
// Purpose:
//   Capture the REAL caller of the game's internal absolute 64-bit stream seek
//   method used immediately before the Win32 SetFilePointerEx call.
//
// Why this exists:
//   RtlCaptureStackBackTrace on this 32-bit optimized build only unwinds to the
//   generic file wrapper (EXE+0x00113541). Hooking the internal method itself
//   lets us read its return address directly from [ESP] at function entry,
//   independent of frame pointers / unwind metadata.
//
// Target method (canonical v1.1 EXE):
//   EXE+0x00113520
//   expected bytes: 8B 54 24 04 56
//     mov edx,[esp+4]
//     push esi
//
// Filtering:
//   The hook logs ONLY when this stream object's OS handle (+0xF0) matches the
//   Update\Patch San Francisco main .sb handle observed via CreateFileA/W.
//
// Safety:
//   - validates the 5 target bytes before patching
//   - patches exactly 5 bytes with a JMP and uses a trampoline
//   - hooks only CreateFileA/W + CloseHandle in the MAIN EXE IAT
//   - no F-key required

static HMODULE g_self = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_stop = nullptr;
static CRITICAL_SECTION g_logLock;
static LARGE_INTEGER g_freq{}, g_start{};
static uintptr_t g_exeBase = 0;
static volatile HANDLE g_patchSfHandle = INVALID_HANDLE_VALUE;
extern "C" void* g_seekTrampoline = nullptr;

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c){ return (wchar_t)towlower(c); });
    return s;
}

static bool IsPatchSanFranciscoSb(const std::wstring& p) {
    const std::wstring q = Lower(p);
    return q.find(L"update\\patch") != std::wstring::npos &&
           q.find(L"level_0100_sanfrancisco\\level_0100_sanfrancisco.sb") != std::wstring::npos;
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

static std::wstring A2W(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, s, -1, &out[0], n)) return {};
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

static double Ms() {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return (double)(q.QuadPart - g_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static std::string ModRva(uintptr_t a) {
    if (!a) return "<null>";
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
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
    if (k != std::string::npos) p = p.substr(k+1);
    char x[MAX_PATH+64];
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
    DWORD wr=0;
    WriteFile(g_log, line, (DWORD)strlen(line), &wr, nullptr);
    if (flush) FlushFileBuffers(g_log);
    LeaveCriticalSection(&g_logLock);
}

static bool Readable(uintptr_t p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
    DWORD pr = mbi.Protect & 0xFF;
    if (pr == PAGE_NOACCESS) return false;
    uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return p <= end && n <= (size_t)(end - p);
}

static void BytesAround(uintptr_t ret, char* out, size_t cap) {
    if (!out || cap < 4) return;
    out[0] = 0;
    uintptr_t start = ret >= 12 ? ret - 12 : ret;
    if (!Readable(start, 24)) return;
    size_t used = 0;
    for (int i=0;i<24;++i) {
        int n = _snprintf(out+used, cap-used, "%02X%s", ((unsigned char*)start)[i], (i==11)?" | ":" ");
        if (n <= 0 || used + (size_t)n >= cap) break;
        used += (size_t)n;
    }
    if (used < cap) out[used] = 0;
}

extern "C" void __cdecl LogInternalSeek(uintptr_t retAddr, uintptr_t selfPtr, uint32_t lo, uint32_t hi) {
    HANDLE tracked = (HANDLE)g_patchSfHandle;
    if (tracked == INVALID_HANDLE_VALUE || !selfPtr) return;
    if (!Readable(selfPtr + 0xF0, sizeof(HANDLE))) return;
    HANDLE h = *(HANDLE*)(selfPtr + 0xF0);
    if (h != tracked) return;

    uint64_t off = ((uint64_t)hi << 32) | lo;
    char bytes[256]{};
    BytesAround(retAddr, bytes, sizeof(bytes));
    Log("ISEEK", true,
        "SF patch internal seek offset=%llu (0x%llX) caller=%s ret=0x%08lX this=0x%08lX handle=0x%p bytes[-12..+11]=%s",
        (unsigned long long)off, (unsigned long long)off,
        ModRva(retAddr).c_str(), (unsigned long)retAddr,
        (unsigned long)selfPtr, h, bytes);
}

extern "C" __attribute__((naked)) void H_InternalSeekAbs64() {
    __asm__ __volatile__(
        "pushfl\n\t"
        "pushal\n\t"
        // after pushfl+pushal: +24 original ECX, +36 ret, +40 lo, +44 hi
        "movl 44(%esp), %eax\n\t"
        "pushl %eax\n\t"
        "movl 40(%esp), %eax\n\t"  // ESP changed by previous push: original lo now +44? fix below
        "addl $4, %esp\n\t"
        // restart argument pushes carefully using EDX as stable original saved-stack base
        "movl %esp, %edx\n\t"
        "movl 44(%edx), %eax\n\t"
        "pushl %eax\n\t"          // hi
        "movl 40(%edx), %eax\n\t"
        "pushl %eax\n\t"          // lo
        "movl 24(%edx), %eax\n\t"
        "pushl %eax\n\t"          // self / ECX
        "movl 36(%edx), %eax\n\t"
        "pushl %eax\n\t"          // return address
        "call _LogInternalSeek\n\t"
        "addl $16, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_seekTrampoline\n\t"
    );
}

using PFN_CreateFileW = HANDLE (WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_CreateFileA = HANDLE (WINAPI*)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_CloseHandle = BOOL (WINAPI*)(HANDLE);

static PFN_CreateFileW Real_CreateFileW = ::CreateFileW;
static PFN_CreateFileA Real_CreateFileA = ::CreateFileA;
static PFN_CloseHandle Real_CloseHandle = ::CloseHandle;

static HANDLE WINAPI H_CreateFileW(LPCWSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t) {
    HANDLE h = Real_CreateFileW(n,a,s,sa,c,f,t);
    DWORD e = GetLastError();
    std::wstring p = n ? n : L"";
    if (h != INVALID_HANDLE_VALUE && IsPatchSanFranciscoSb(p)) {
        g_patchSfHandle = h;
        Log("FILE", true, "TRACK patch SF .sb h=0x%p path=\"%s\"", h, W2U(p.c_str()).c_str());
    }
    SetLastError(e);
    return h;
}

static HANDLE WINAPI H_CreateFileA(LPCSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t) {
    HANDLE h = Real_CreateFileA(n,a,s,sa,c,f,t);
    DWORD e = GetLastError();
    std::wstring p = A2W(n ? n : "");
    if (h != INVALID_HANDLE_VALUE && IsPatchSanFranciscoSb(p)) {
        g_patchSfHandle = h;
        Log("FILE", true, "TRACK patch SF .sb h=0x%p path=\"%s\"", h, n?n:"");
    }
    SetLastError(e);
    return h;
}

static BOOL WINAPI H_CloseHandle(HANDLE h) {
    if (h == (HANDLE)g_patchSfHandle) {
        Log("FILE", true, "UNTRACK patch SF .sb h=0x%p", h);
        g_patchSfHandle = INVALID_HANDLE_VALUE;
    }
    return Real_CloseHandle(h);
}

static bool HookIAT(HMODULE mod,const char* name,void* hook) {
    uint8_t* base=(uint8_t*)mod;
    IMAGE_DOS_HEADER* dos=(IMAGE_DOS_HEADER*)base;
    if (!mod || dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE) return false;
    DWORD rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return false;
    bool any=false;
    IMAGE_IMPORT_DESCRIPTOR* imp=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);
    for (;imp->Name;++imp) {
        IMAGE_THUNK_DATA* oft=imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base+imp->OriginalFirstThunk) : (IMAGE_THUNK_DATA*)(base+imp->FirstThunk);
        IMAGE_THUNK_DATA* ft=(IMAGE_THUNK_DATA*)(base+imp->FirstThunk);
        for (;oft->u1.AddressOfData;++oft,++ft) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn=(IMAGE_IMPORT_BY_NAME*)(base+oft->u1.AddressOfData);
            if (strcmp((char*)ibn->Name,name)!=0) continue;
            DWORD old=0;
            if (!VirtualProtect(&ft->u1.Function,sizeof(uintptr_t),PAGE_READWRITE,&old)) continue;
            ft->u1.Function=(uintptr_t)hook;
            DWORD tmp=0;
            VirtualProtect(&ft->u1.Function,sizeof(uintptr_t),old,&tmp);
            FlushInstructionCache(GetCurrentProcess(),&ft->u1.Function,sizeof(uintptr_t));
            any=true;
        }
    }
    return any;
}

static bool InstallInternalSeekHook() {
    uint8_t* target = (uint8_t*)(g_exeBase + 0x00113520);
    const uint8_t expected[5] = {0x8B,0x54,0x24,0x04,0x56};
    if (memcmp(target, expected, 5) != 0) {
        Log("ARM", true, "REFUSED internal hook: EXE+0x00113520 bytes do not match expected 8B 54 24 04 56");
        return false;
    }

    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        Log("ARM", true, "REFUSED internal hook: VirtualAlloc trampoline failed error=%lu", GetLastError());
        return false;
    }
    memcpy(tramp, target, 5);
    tramp[5] = 0xE9;
    *(int32_t*)(tramp+6) = (int32_t)((target+5) - (tramp+10));
    g_seekTrampoline = tramp;

    DWORD old=0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log("ARM", true, "REFUSED internal hook: VirtualProtect failed error=%lu", GetLastError());
        return false;
    }
    target[0]=0xE9;
    *(int32_t*)(target+1)=(int32_t)((uint8_t*)&H_InternalSeekAbs64 - (target+5));
    DWORD tmp=0;
    VirtualProtect(target,5,old,&tmp);
    FlushInstructionCache(GetCurrentProcess(),target,5);
    Log("ARM", true, "internal absolute seek hook installed at EXE+0x00113520 trampoline=0x%p", tramp);
    return true;
}

static DWORD WINAPI Worker(LPVOID) {
    InitializeCriticalSection(&g_logLock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    g_exeBase=(uintptr_t)GetModuleHandleW(nullptr);

    wchar_t me[MAX_PATH]{};
    GetModuleFileNameW(g_self,me,MAX_PATH);
    wchar_t* slash=wcsrchr(me,L'\\');
    if (slash) *(slash+1)=0;
    wchar_t lp[MAX_PATH]{};
    _snwprintf(lp,MAX_PATH-1,L"%sNFSTR_InternalSeekCallerTrace_%lu.log",me,(unsigned long)GetCurrentProcessId());
    g_log=CreateFileW(lp,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if (g_log==INVALID_HANDLE_VALUE) return 1;
    const unsigned char bom[3]={0xEF,0xBB,0xBF}; DWORD wr=0; WriteFile(g_log,bom,3,&wr,nullptr);

    Log("INIT",true,"NFSTR Internal Seek Caller Trace v0.1.0 PID=%lu EXEbase=0x%p",
        (unsigned long)GetCurrentProcessId(),(void*)g_exeBase);
    Log("INIT",true,"target=EXE+0x00113520; logs only when stream handle matches Update/Patch SanFrancisco .sb");

    HMODULE exe=(HMODULE)g_exeBase;
    int n=0;
    n += HookIAT(exe,"CreateFileW",(void*)H_CreateFileW)?1:0;
    n += HookIAT(exe,"CreateFileA",(void*)H_CreateFileA)?1:0;
    n += HookIAT(exe,"CloseHandle",(void*)H_CloseHandle)?1:0;
    bool internal=InstallInternalSeekHook();
    Log("ARM",true,"IAT hooks=%d internalSeek=%s",n,internal?"yes":"no");

    while (WaitForSingleObject(g_stop,50)==WAIT_TIMEOUT) {}
    Log("INIT",true,"stopping");
    FlushFileBuffers(g_log);
    Real_CloseHandle(g_log);
    g_log=INVALID_HANDLE_VALUE;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
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
