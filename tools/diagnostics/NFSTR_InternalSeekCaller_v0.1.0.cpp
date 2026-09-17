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

// NFSTR_InternalSeekCaller.asi v0.1.0
// Need for Speed: The Run v1.1 x86
//
// Purpose:
//   Capture the REAL caller of the game's internal absolute 64-bit file seek
//   method at EXE+0x00113520. API-level backtraces stop at the generic wrapper
//   because the retail x86 executable has no usable unwind chain there.
//
// Method:
//   - Track only Update\Patch\...\Level_0100_SanFrancisco.sb via main-EXE
//     CreateFileA/W hooks.
//   - Detour the game's internal seek method itself.
//   - When that method is called for the tracked San Francisco patch handle,
//     log the requested absolute 64-bit offset AND __builtin_return_address(0).
//   - The detour is signature-checked before patching.
//
// Known target bytes at RVA 0x00113520:
//   8B 54 24 04 56
// which are complete instructions:
//   mov edx,[esp+4]
//   push esi
//
// Safety:
//   MAIN EXE only. 5-byte entry detour with a trampoline reproducing those
//   exact bytes, then returning at RVA 0x00113525. No F-key is required.
//   Designed to coexist with NFSTR_CrashGuard v0.6.0.

namespace isc {

static HMODULE g_self = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_stop = nullptr;
static CRITICAL_SECTION g_logLock;
static LARGE_INTEGER g_freq{}, g_start{};
static uintptr_t g_exeBase = 0;
static volatile HANDLE g_patchSfHandle = INVALID_HANDLE_VALUE;

static const DWORD kSeekRva = 0x00113520;
static const unsigned char kSeekSig[5] = {0x8B,0x54,0x24,0x04,0x56};

using InternalSeekFn = void (__attribute__((thiscall)) *)(void*, uint32_t, int32_t);
static InternalSeekFn Real_InternalSeek = nullptr;

using PFN_CreateFileW = HANDLE (WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_CreateFileA = HANDLE (WINAPI*)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_CloseHandle = BOOL (WINAPI*)(HANDLE);

static PFN_CreateFileW Real_CreateFileW = ::CreateFileW;
static PFN_CreateFileA Real_CreateFileA = ::CreateFileA;
static PFN_CloseHandle Real_CloseHandle = ::CloseHandle;

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c){ return (wchar_t)towlower(c); });
    return s;
}

static bool IsPatchSanFranciscoSb(const std::wstring& p) {
    const std::wstring q = Lower(p);
    if (q.find(L"update\\patch") == std::wstring::npos) return false;
    if (q.find(L"level_0100_sanfrancisco") == std::wstring::npos) return false;
    if (q.find(L"level_0100_sanfrancisco.sb") == std::wstring::npos) return false;
    if (q.find(L"terrain") != std::wstring::npos) return false;
    return true;
}

static std::string W2U(const wchar_t* s) {
    if (!s || !*s) return {};
    int n = WideCharToMultiByte(CP_UTF8,0,s,-1,nullptr,0,nullptr,nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n,'\0');
    if (!WideCharToMultiByte(CP_UTF8,0,s,-1,&out[0],n,nullptr,nullptr)) return {};
    if (!out.empty() && out.back()=='\0') out.pop_back();
    return out;
}

static std::wstring A2W(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_ACP,0,s,-1,nullptr,0);
    if (n <= 0) return {};
    std::wstring out((size_t)n,L'\0');
    if (!MultiByteToWideChar(CP_ACP,0,s,-1,&out[0],n)) return {};
    if (!out.empty() && out.back()==L'\0') out.pop_back();
    return out;
}

static double Ms() {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return (double)(q.QuadPart-g_start.QuadPart)*1000.0/(double)g_freq.QuadPart;
}

static std::string ModRva(void* a) {
    if (!a) return "<null>";
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(a,&mbi,sizeof(mbi)) || !mbi.AllocationBase) {
        char x[32];
        _snprintf(x,sizeof(x)-1,"0x%08lX",(unsigned long)(uintptr_t)a);
        x[sizeof(x)-1]=0;
        return x;
    }
    HMODULE m=(HMODULE)mbi.AllocationBase;
    wchar_t w[MAX_PATH]{};
    GetModuleFileNameW(m,w,MAX_PATH);
    std::string p=W2U(w);
    size_t k=p.find_last_of("\\/");
    if(k!=std::string::npos) p=p.substr(k+1);
    char x[MAX_PATH+64];
    _snprintf(x,sizeof(x)-1,"%s+0x%08lX",p.c_str(),
              (unsigned long)((uintptr_t)a-(uintptr_t)m));
    x[sizeof(x)-1]=0;
    return x;
}

static void Log(const char* cat,bool flush,const char* fmt,...) {
    if(g_log==INVALID_HANDLE_VALUE) return;
    char body[4096];
    va_list ap;
    va_start(ap,fmt);
    _vsnprintf(body,sizeof(body)-1,fmt,ap);
    va_end(ap);
    body[sizeof(body)-1]=0;
    char line[4608];
    _snprintf(line,sizeof(line)-1,"[%011.3f] [TID %5lu] [%s] %s\r\n",
              Ms(),(unsigned long)GetCurrentThreadId(),cat,body);
    line[sizeof(line)-1]=0;
    EnterCriticalSection(&g_logLock);
    DWORD wr=0;
    WriteFile(g_log,line,(DWORD)strlen(line),&wr,nullptr);
    if(flush) FlushFileBuffers(g_log);
    LeaveCriticalSection(&g_logLock);
}

static HANDLE WINAPI H_CreateFileW(LPCWSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t) {
    std::wstring p=n?n:L"";
    HANDLE h=Real_CreateFileW(n,a,s,sa,c,f,t);
    DWORD e=GetLastError();
    if(h!=INVALID_HANDLE_VALUE && IsPatchSanFranciscoSb(p)) {
        InterlockedExchangePointer((PVOID volatile*)&g_patchSfHandle,h);
        Log("FILE",true,"TRACK h=0x%p path=\"%s\" caller=%s",h,W2U(p.c_str()).c_str(),ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e);
    return h;
}

static HANDLE WINAPI H_CreateFileA(LPCSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t) {
    std::wstring p=A2W(n?n:"");
    HANDLE h=Real_CreateFileA(n,a,s,sa,c,f,t);
    DWORD e=GetLastError();
    if(h!=INVALID_HANDLE_VALUE && IsPatchSanFranciscoSb(p)) {
        InterlockedExchangePointer((PVOID volatile*)&g_patchSfHandle,h);
        Log("FILE",true,"TRACK h=0x%p path=\"%s\" caller=%s",h,n?n:"",ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e);
    return h;
}

static BOOL WINAPI H_CloseHandle(HANDLE h) {
    HANDLE tracked=(HANDLE)InterlockedCompareExchangePointer((PVOID volatile*)&g_patchSfHandle,nullptr,nullptr);
    if(h==tracked) {
        Log("FILE",true,"UNTRACK h=0x%p caller=%s",h,ModRva(__builtin_return_address(0)).c_str());
        InterlockedCompareExchangePointer((PVOID volatile*)&g_patchSfHandle,INVALID_HANDLE_VALUE,h);
    }
    return Real_CloseHandle(h);
}

static bool SafeReadHandle(void* self,HANDLE& out) {
    out=INVALID_HANDLE_VALUE;
    if(!self) return false;
    uintptr_t p=(uintptr_t)self+0xF0;
    MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery((LPCVOID)p,&mbi,sizeof(mbi))) return false;
    if(mbi.State!=MEM_COMMIT || (mbi.Protect&PAGE_GUARD) || ((mbi.Protect&0xFF)==PAGE_NOACCESS)) return false;
    uintptr_t end=(uintptr_t)mbi.BaseAddress+mbi.RegionSize;
    if(p+sizeof(HANDLE)>end) return false;
    out=*(HANDLE*)p;
    return true;
}

static void __attribute__((fastcall)) H_InternalSeek(void* self,void*,uint32_t lo,int32_t hi) {
    void* caller=__builtin_return_address(0);
    HANDLE h=INVALID_HANDLE_VALUE;
    HANDLE tracked=(HANDLE)InterlockedCompareExchangePointer((PVOID volatile*)&g_patchSfHandle,nullptr,nullptr);
    if(SafeReadHandle(self,h) && h==tracked && tracked!=INVALID_HANDLE_VALUE) {
        uint64_t off=((uint64_t)(uint32_t)hi<<32)|(uint64_t)lo;
        Log("ISEEK",true,"self=0x%p h=0x%p off=%llu lo=%u hi=%ld caller=%s",
            self,h,(unsigned long long)off,(unsigned)lo,(long)hi,ModRva(caller).c_str());
    }
    Real_InternalSeek(self,lo,hi);
}

static bool HookOneIAT(HMODULE mod,const char* name,void* hook) {
    if(!mod || mod==g_self) return false;
    uint8_t* base=(uint8_t*)mod;
    IMAGE_DOS_HEADER* dos=(IMAGE_DOS_HEADER*)base;
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE) return false;
    DWORD rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if(!rva) return false;
    bool any=false;
    IMAGE_IMPORT_DESCRIPTOR* imp=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);
    for(;imp->Name;++imp) {
        IMAGE_THUNK_DATA* oft=imp->OriginalFirstThunk?(IMAGE_THUNK_DATA*)(base+imp->OriginalFirstThunk):(IMAGE_THUNK_DATA*)(base+imp->FirstThunk);
        IMAGE_THUNK_DATA* ft=(IMAGE_THUNK_DATA*)(base+imp->FirstThunk);
        for(;oft->u1.AddressOfData;++oft,++ft) {
            if(IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn=(IMAGE_IMPORT_BY_NAME*)(base+oft->u1.AddressOfData);
            if(strcmp((char*)ibn->Name,name)!=0) continue;
            DWORD old=0;
            if(!VirtualProtect(&ft->u1.Function,sizeof(uintptr_t),PAGE_READWRITE,&old)) continue;
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
    uint8_t* target=(uint8_t*)(g_exeBase+kSeekRva);
    if(memcmp(target,kSeekSig,sizeof(kSeekSig))!=0) {
        char got[64]{};
        _snprintf(got,sizeof(got)-1,"%02X %02X %02X %02X %02X",target[0],target[1],target[2],target[3],target[4]);
        Log("HOOK",true,"REFUSED: seek signature mismatch at EXE+0x%08lX got=[%s] expected=[8B 54 24 04 56]",(unsigned long)kSeekRva,got);
        return false;
    }

    uint8_t* tramp=(uint8_t*)VirtualAlloc(nullptr,16,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!tramp) {
        Log("HOOK",true,"REFUSED: VirtualAlloc trampoline failed err=%lu",(unsigned long)GetLastError());
        return false;
    }
    memcpy(tramp,target,5);
    tramp[5]=0xE9;
    *(int32_t*)(tramp+6)=(int32_t)((target+5)-(tramp+10));
    FlushInstructionCache(GetCurrentProcess(),tramp,10);
    Real_InternalSeek=(InternalSeekFn)tramp;

    DWORD old=0;
    if(!VirtualProtect(target,5,PAGE_EXECUTE_READWRITE,&old)) {
        Log("HOOK",true,"REFUSED: VirtualProtect target failed err=%lu",(unsigned long)GetLastError());
        return false;
    }
    target[0]=0xE9;
    *(int32_t*)(target+1)=(int32_t)((uint8_t*)&H_InternalSeek-(target+5));
    DWORD tmp=0;
    VirtualProtect(target,5,old,&tmp);
    FlushInstructionCache(GetCurrentProcess(),target,5);

    Log("HOOK",true,"installed internal seek detour target=EXE+0x%08lX hook=%s trampoline=0x%p",
        (unsigned long)kSeekRva,ModRva((void*)&H_InternalSeek).c_str(),tramp);
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
    if(slash) *(slash+1)=0;
    wchar_t lp[MAX_PATH]{};
    _snwprintf(lp,MAX_PATH-1,L"%sNFSTR_InternalSeekCaller_%lu.log",me,(unsigned long)GetCurrentProcessId());

    g_log=CreateFileW(lp,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                      CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(g_log==INVALID_HANDLE_VALUE) return 1;
    const unsigned char bom[3]={0xEF,0xBB,0xBF};
    DWORD wr=0;
    WriteFile(g_log,bom,3,&wr,nullptr);

    Log("INIT",true,"NFSTR Internal Seek Caller v0.1.0 PID=%lu EXEbase=0x%p",
        (unsigned long)GetCurrentProcessId(),(void*)g_exeBase);
    Log("INIT",true,"target internal seek=EXE+0x00113520; exact patch SanFrancisco .sb handle only");

    HMODULE exe=GetModuleHandleW(nullptr);
    int iat=0;
    iat += HookOneIAT(exe,"CreateFileW",(void*)H_CreateFileW)?1:0;
    iat += HookOneIAT(exe,"CreateFileA",(void*)H_CreateFileA)?1:0;
    iat += HookOneIAT(exe,"CloseHandle",(void*)H_CloseHandle)?1:0;
    bool code=InstallInternalSeekHook();
    Log("ARM",true,"IAT hooks=%d internalSeek=%s",iat,code?"yes":"NO");

    while(WaitForSingleObject(g_stop,50)==WAIT_TIMEOUT) {}

    Log("INIT",true,"stopping");
    FlushFileBuffers(g_log);
    Real_CloseHandle(g_log);
    g_log=INVALID_HANDLE_VALUE;
    return 0;
}

} // namespace isc

BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
    using namespace isc;
    if(reason==DLL_PROCESS_ATTACH) {
        g_self=(HMODULE)h;
        DisableThreadLibraryCalls(h);
        g_stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE t=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
        if(t) CloseHandle(t);
    } else if(reason==DLL_PROCESS_DETACH) {
        if(g_stop) SetEvent(g_stop);
    }
    return TRUE;
}
