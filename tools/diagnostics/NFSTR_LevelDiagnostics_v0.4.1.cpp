#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cwctype>
#include <cstring>

// NFSTR_LevelDiagnostics.asi v0.4.1
// Manual-arm diagnostic for Need for Speed: The Run v1.1 x86.
// IMPORTANT: no game API hooks are installed until the user presses F6.
// Once armed, only the MAIN EXE import table is patched. System/runtime DLLs
// and other ASIs are never modified.

namespace lvl {

static HMODULE g_self = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_stop = nullptr;
static PVOID g_veh = nullptr;
static CRITICAL_SECTION g_logLock;
static CRITICAL_SECTION g_stateLock;
static LARGE_INTEGER g_freq{}, g_start{};
static volatile LONG g_armed = 0;
static volatile LONG g_readId = 0;

struct FileState {
    std::wstring path;
    uint64_t pos = 0;
    bool known = false;
};
static std::unordered_map<uintptr_t, FileState> g_files;

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}
static bool IsSF(const std::wstring& p) {
    return Lower(p).find(L"level_0100_sanfrancisco") != std::wstring::npos;
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
static std::string ModRva(void* a) {
    if (!a) return "<null>";
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(a, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
        char x[32];
        _snprintf(x, sizeof(x)-1, "0x%08lX", (unsigned long)(uintptr_t)a);
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
              (unsigned long)((uintptr_t)a - (uintptr_t)m));
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

static FileState GetFile(HANDLE h) {
    FileState s;
    EnterCriticalSection(&g_stateLock);
    auto it = g_files.find((uintptr_t)h);
    if (it != g_files.end()) s = it->second;
    LeaveCriticalSection(&g_stateLock);
    return s;
}
static void PutFile(HANDLE h, const FileState& s) {
    if (!h || h == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_stateLock);
    g_files[(uintptr_t)h] = s;
    LeaveCriticalSection(&g_stateLock);
}
static void DropFile(HANDLE h) {
    EnterCriticalSection(&g_stateLock);
    g_files.erase((uintptr_t)h);
    LeaveCriticalSection(&g_stateLock);
}

using PFN_CreateFileW = HANDLE (WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_CreateFileA = HANDLE (WINAPI*)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_ReadFile = BOOL (WINAPI*)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
using PFN_SetFilePointerEx = BOOL (WINAPI*)(HANDLE,LARGE_INTEGER,PLARGE_INTEGER,DWORD);
using PFN_SetFilePointer = DWORD (WINAPI*)(HANDLE,LONG,PLONG,DWORD);
using PFN_GetFileSizeEx = BOOL (WINAPI*)(HANDLE,PLARGE_INTEGER);
using PFN_CloseHandle = BOOL (WINAPI*)(HANDLE);

static PFN_CreateFileW Real_CreateFileW = ::CreateFileW;
static PFN_CreateFileA Real_CreateFileA = ::CreateFileA;
static PFN_ReadFile Real_ReadFile = ::ReadFile;
static PFN_SetFilePointerEx Real_SetFilePointerEx = ::SetFilePointerEx;
static PFN_SetFilePointer Real_SetFilePointer = ::SetFilePointer;
static PFN_GetFileSizeEx Real_GetFileSizeEx = ::GetFileSizeEx;
static PFN_CloseHandle Real_CloseHandle = ::CloseHandle;

static HANDLE WINAPI H_CreateFileW(LPCWSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t) {
    std::wstring p = n ? n : L"";
    HANDLE h = Real_CreateFileW(n,a,s,sa,c,f,t);
    DWORD e = GetLastError();
    if (h != INVALID_HANDLE_VALUE && IsSF(p)) {
        FileState st; st.path=p; st.pos=0; st.known=true; PutFile(h,st);
        Log("SF-FILE", true, "OPEN h=0x%p path=\"%s\" access=0x%08lX caller=%s",
            h, W2U(p.c_str()).c_str(), (unsigned long)a,
            ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e);
    return h;
}
static HANDLE WINAPI H_CreateFileA(LPCSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t) {
    std::wstring p = A2W(n ? n : "");
    HANDLE h = Real_CreateFileA(n,a,s,sa,c,f,t);
    DWORD e = GetLastError();
    if (h != INVALID_HANDLE_VALUE && IsSF(p)) {
        FileState st; st.path=p; st.pos=0; st.known=true; PutFile(h,st);
        Log("SF-FILE", true, "OPEN h=0x%p path=\"%s\" access=0x%08lX caller=%s",
            h, n?n:"", (unsigned long)a,
            ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e);
    return h;
}
static BOOL WINAPI H_GetFileSizeEx(HANDLE h, PLARGE_INTEGER sz) {
    BOOL r = Real_GetFileSizeEx(h, sz);
    DWORD e = GetLastError();
    FileState st = GetFile(h);
    if (!st.path.empty())
        Log("SF-SIZE", true, "h=0x%p size=%lld result=%d error=%lu caller=%s",
            h, sz?(long long)sz->QuadPart:-1LL, r, (unsigned long)e,
            ModRva(__builtin_return_address(0)).c_str());
    SetLastError(e);
    return r;
}
static BOOL WINAPI H_SetFilePointerEx(HANDLE h,LARGE_INTEGER d,PLARGE_INTEGER np,DWORD method) {
    FileState st = GetFile(h);
    BOOL r = Real_SetFilePointerEx(h,d,np,method);
    DWORD e = GetLastError();
    if (!st.path.empty()) {
        if (r && np) { st.pos=(uint64_t)np->QuadPart; st.known=true; PutFile(h,st); }
        Log("SF-SEEK", true, "h=0x%p move=%lld method=%lu result=%d new=%s%llu error=%lu caller=%s",
            h, (long long)d.QuadPart, (unsigned long)method, r, np?"":"?",
            (unsigned long long)(np?(uint64_t)np->QuadPart:0), (unsigned long)e,
            ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e);
    return r;
}
static DWORD WINAPI H_SetFilePointer(HANDLE h,LONG lo,PLONG hi,DWORD method) {
    FileState st = GetFile(h);
    DWORD r = Real_SetFilePointer(h,lo,hi,method);
    DWORD e = GetLastError();
    if (!st.path.empty()) {
        bool ok = !(r == INVALID_SET_FILE_POINTER && e != NO_ERROR);
        uint64_t pos = ((uint64_t)(hi ? *(DWORD*)hi : 0) << 32) | r;
        if (ok) { st.pos=pos; st.known=true; PutFile(h,st); }
        Log("SF-SEEK", true, "h=0x%p moveLo=%ld method=%lu result=0x%08lX new=%llu error=%lu caller=%s",
            h,(long)lo,(unsigned long)method,(unsigned long)r,
            (unsigned long long)pos,(unsigned long)e,
            ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e);
    return r;
}
static BOOL WINAPI H_ReadFile(HANDLE h,LPVOID b,DWORD want,LPDWORD got,LPOVERLAPPED ov) {
    FileState st = GetFile(h);
    uint64_t off = st.known ? st.pos : 0;
    if (ov) off=((uint64_t)ov->OffsetHigh<<32)|ov->Offset;
    LONG id = InterlockedIncrement(&g_readId);
    if (!st.path.empty())
        Log("SF-READ", true, "#%ld BEGIN h=0x%p off=%s%llu want=%lu overlapped=%d caller=%s",
            (long)id,h,(st.known||ov)?"":"?",(unsigned long long)off,
            (unsigned long)want,ov?1:0,ModRva(__builtin_return_address(0)).c_str());
    BOOL r = Real_ReadFile(h,b,want,got,ov);
    DWORD e = GetLastError();
    DWORD n = got ? *got : 0;
    if (!st.path.empty()) {
        Log("SF-READ", true, "#%ld END result=%d got=%lu error=%lu",(long)id,r,(unsigned long)n,(unsigned long)e);
        if (r && !ov && st.known) { st.pos += n; PutFile(h,st); }
    }
    SetLastError(e);
    return r;
}
static BOOL WINAPI H_CloseHandle(HANDLE h) {
    FileState st = GetFile(h);
    if (!st.path.empty())
        Log("SF-FILE", true, "CLOSE h=0x%p path=\"%s\" caller=%s",
            h,W2U(st.path.c_str()).c_str(),ModRva(__builtin_return_address(0)).c_str());
    DropFile(h);
    return Real_CloseHandle(h);
}

static void StackFromContext(CONTEXT* c) {
    if (!c) return;
    Log("CRASH", true, "STACK[0] %s", ModRva((void*)(uintptr_t)c->Eip).c_str());
    uintptr_t ebp=(uintptr_t)c->Ebp;
    for (int i=1; i<12 && ebp; ++i) {
        struct Frame { uintptr_t prev; uintptr_t ret; } fr{};
        SIZE_T br=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(LPCVOID)ebp,&fr,sizeof(fr),&br) || br!=sizeof(fr) || !fr.ret) break;
        Log("CRASH", true, "STACK[%d] %s", i, ModRva((void*)fr.ret).c_str());
        if (fr.prev<=ebp || fr.prev-ebp>0x100000) break;
        ebp=fr.prev;
    }
}
static LONG CALLBACK Veh(PEXCEPTION_POINTERS ep) {
    if (!InterlockedCompareExchange(&g_armed,0,0)) return EXCEPTION_CONTINUE_SEARCH;
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code=ep->ExceptionRecord->ExceptionCode;
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
    CONTEXT* x=ep->ContextRecord;
    Log("CRASH",true,"EXCEPTION code=0x%08lX addr=%s EIP=%08lX ESP=%08lX EBP=%08lX EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX",
        (unsigned long)code,ModRva(ep->ExceptionRecord->ExceptionAddress).c_str(),
        (unsigned long)x->Eip,(unsigned long)x->Esp,(unsigned long)x->Ebp,
        (unsigned long)x->Eax,(unsigned long)x->Ebx,(unsigned long)x->Ecx,
        (unsigned long)x->Edx,(unsigned long)x->Esi,(unsigned long)x->Edi);
    if (code==EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters>=2)
        Log("CRASH",true,"ACCESS type=%llu address=0x%08llX",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    StackFromContext(x);
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool HookOne(HMODULE mod,const char* name,void* hook) {
    if (!mod || mod==g_self) return false;
    uint8_t* base=(uint8_t*)mod;
    IMAGE_DOS_HEADER* dos=(IMAGE_DOS_HEADER*)base;
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
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
static int ArmHooks() {
    HMODULE exe=GetModuleHandleW(nullptr);
    int n=0;
    n += HookOne(exe,"CreateFileW",(void*)H_CreateFileW) ? 1 : 0;
    n += HookOne(exe,"CreateFileA",(void*)H_CreateFileA) ? 1 : 0;
    n += HookOne(exe,"ReadFile",(void*)H_ReadFile) ? 1 : 0;
    n += HookOne(exe,"SetFilePointerEx",(void*)H_SetFilePointerEx) ? 1 : 0;
    n += HookOne(exe,"SetFilePointer",(void*)H_SetFilePointer) ? 1 : 0;
    n += HookOne(exe,"GetFileSizeEx",(void*)H_GetFileSizeEx) ? 1 : 0;
    n += HookOne(exe,"CloseHandle",(void*)H_CloseHandle) ? 1 : 0;
    return n;
}

static DWORD WINAPI Worker(LPVOID) {
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_stateLock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);

    wchar_t me[MAX_PATH]{};
    GetModuleFileNameW(g_self,me,MAX_PATH);
    wchar_t* slash=wcsrchr(me,L'\\');
    if (slash) *(slash+1)=0;
    wchar_t lp[MAX_PATH]{};
    _snwprintf(lp,MAX_PATH-1,L"%sNFSTR_LevelDiagnostics.log",me);

    g_log=CreateFileW(lp,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                      CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if (g_log==INVALID_HANDLE_VALUE) return 1;
    const unsigned char bom[3]={0xEF,0xBB,0xBF};
    DWORD wr=0;
    WriteFile(g_log,bom,3,&wr,nullptr);

    Log("INIT",true,"NFSTR Level Diagnostics v0.4.1 loaded PID=%lu EXEbase=0x%p",
        (unsigned long)GetCurrentProcessId(),GetModuleHandleW(nullptr));
    Log("INIT",true,"SAFE MODE: zero game hooks installed. Navigate to Escape From S.F., then press F6 to arm.");

    bool prevF6=false;
    while (WaitForSingleObject(g_stop,50)==WAIT_TIMEOUT) {
        bool down=(GetAsyncKeyState(VK_F6)&0x8000)!=0;
        if (down && !prevF6 && !InterlockedCompareExchange(&g_armed,0,0)) {
            int n=ArmHooks();
            g_veh=AddVectoredExceptionHandler(1,Veh);
            if (n>0) {
                InterlockedExchange(&g_armed,1);
                Log("ARM",true,"F6 ARMED: patched %d API imports in MAIN EXE only; VEH=%s",n,g_veh?"yes":"no");
                Beep(1000,120);
                Beep(1300,120);
            } else {
                Log("ARM",true,"F6 FAILED: no matching imports were patched");
                Beep(350,300);
            }
        }
        prevF6=down;
    }

    Log("INIT",true,"stopping");
    FlushFileBuffers(g_log);
    Real_CloseHandle(g_log);
    g_log=INVALID_HANDLE_VALUE;
    return 0;
}

} // namespace lvl

BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
    using namespace lvl;
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
