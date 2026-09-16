#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cstring>

namespace lvl {

static HMODULE g_self = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_stop = nullptr;
static CRITICAL_SECTION g_lock;
static CRITICAL_SECTION g_state;
static LARGE_INTEGER g_freq{}, g_start{};
static std::unordered_set<uintptr_t> g_patched;
static volatile LONG g_id = 0;

struct FileState {
    std::wstring path;
    uint64_t pos = 0;
    bool known = false;
};
struct MapState {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
};
static std::unordered_map<uintptr_t, FileState> g_files;
static std::unordered_map<uintptr_t, MapState> g_maps;

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c){ return (wchar_t)towlower(c); });
    return s;
}
static bool IsSF(const std::wstring& p) {
    std::wstring s = Lower(p);
    return s.find(L"level_0100_sanfrancisco") != std::wstring::npos;
}
static std::string W2U(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8,0,s,-1,nullptr,0,nullptr,nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8,0,s,-1,&out[0],n,nullptr,nullptr);
    if (!out.empty() && out.back()==0) out.pop_back();
    return out;
}
static std::wstring A2W(const char* s) {
    if (!s) return {};
    int n = MultiByteToWideChar(CP_ACP,0,s,-1,nullptr,0);
    if (n <= 0) return {};
    std::wstring out((size_t)n,L'\0');
    MultiByteToWideChar(CP_ACP,0,s,-1,&out[0],n);
    if (!out.empty() && out.back()==0) out.pop_back();
    return out;
}
static double Ms() {
    LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
    return (double)(q.QuadPart-g_start.QuadPart)*1000.0/(double)g_freq.QuadPart;
}
static std::string ModRva(void* a) {
    if (!a) return "<null>";
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(a,&mbi,sizeof(mbi)) || !mbi.AllocationBase) {
        char x[32]; _snprintf(x,sizeof(x)-1,"0x%08lX",(unsigned long)(uintptr_t)a); x[sizeof(x)-1]=0; return x;
    }
    HMODULE m=(HMODULE)mbi.AllocationBase;
    wchar_t w[MAX_PATH]{}; GetModuleFileNameW(m,w,MAX_PATH);
    std::string p=W2U(w); size_t k=p.find_last_of("\\/"); if(k!=std::string::npos)p=p.substr(k+1);
    char x[MAX_PATH+64];
    _snprintf(x,sizeof(x)-1,"%s+0x%08lX",p.c_str(),(unsigned long)((uintptr_t)a-(uintptr_t)m));
    x[sizeof(x)-1]=0; return x;
}
static void Log(const char* cat, bool flush, const char* fmt, ...) {
    if (g_log==INVALID_HANDLE_VALUE) return;
    char b[4096];
    va_list ap; va_start(ap,fmt); _vsnprintf(b,sizeof(b)-1,fmt,ap); va_end(ap); b[sizeof(b)-1]=0;
    char line[4608];
    _snprintf(line,sizeof(line)-1,"[%011.3f] [TID %5lu] [%s] %s\r\n",Ms(),(unsigned long)GetCurrentThreadId(),cat,b);
    line[sizeof(line)-1]=0;
    EnterCriticalSection(&g_lock);
    DWORD wr=0; WriteFile(g_log,line,(DWORD)strlen(line),&wr,nullptr);
    if(flush) FlushFileBuffers(g_log);
    LeaveCriticalSection(&g_lock);
}

static FileState GetFile(HANDLE h) {
    FileState s;
    EnterCriticalSection(&g_state);
    auto it=g_files.find((uintptr_t)h); if(it!=g_files.end())s=it->second;
    LeaveCriticalSection(&g_state);
    return s;
}
static void PutFile(HANDLE h,const FileState& s) {
    EnterCriticalSection(&g_state); g_files[(uintptr_t)h]=s; LeaveCriticalSection(&g_state);
}
static void Drop(HANDLE h) {
    EnterCriticalSection(&g_state);
    g_files.erase((uintptr_t)h); g_maps.erase((uintptr_t)h);
    LeaveCriticalSection(&g_state);
}

using PFN_CreateFileW = HANDLE (WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_CreateFileA = HANDLE (WINAPI*)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_ReadFile = BOOL (WINAPI*)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
using PFN_SetFilePointerEx = BOOL (WINAPI*)(HANDLE,LARGE_INTEGER,PLARGE_INTEGER,DWORD);
using PFN_SetFilePointer = DWORD (WINAPI*)(HANDLE,LONG,PLONG,DWORD);
using PFN_GetFileSizeEx = BOOL (WINAPI*)(HANDLE,PLARGE_INTEGER);
using PFN_CreateFileMappingW = HANDLE (WINAPI*)(HANDLE,LPSECURITY_ATTRIBUTES,DWORD,DWORD,DWORD,LPCWSTR);
using PFN_CreateFileMappingA = HANDLE (WINAPI*)(HANDLE,LPSECURITY_ATTRIBUTES,DWORD,DWORD,DWORD,LPCSTR);
using PFN_MapViewOfFile = LPVOID (WINAPI*)(HANDLE,DWORD,DWORD,DWORD,SIZE_T);
using PFN_CloseHandle = BOOL (WINAPI*)(HANDLE);

static PFN_CreateFileW Real_CreateFileW = ::CreateFileW;
static PFN_CreateFileA Real_CreateFileA = ::CreateFileA;
static PFN_ReadFile Real_ReadFile = ::ReadFile;
static PFN_SetFilePointerEx Real_SetFilePointerEx = ::SetFilePointerEx;
static PFN_SetFilePointer Real_SetFilePointer = ::SetFilePointer;
static PFN_GetFileSizeEx Real_GetFileSizeEx = ::GetFileSizeEx;
static PFN_CreateFileMappingW Real_CreateFileMappingW = ::CreateFileMappingW;
static PFN_CreateFileMappingA Real_CreateFileMappingA = ::CreateFileMappingA;
static PFN_MapViewOfFile Real_MapViewOfFile = ::MapViewOfFile;
static PFN_CloseHandle Real_CloseHandle = ::CloseHandle;

static HANDLE WINAPI H_CreateFileW(LPCWSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t) {
    std::wstring p=n?n:L"";
    HANDLE h=Real_CreateFileW(n,a,s,sa,c,f,t); DWORD e=GetLastError();
    if(h!=INVALID_HANDLE_VALUE && IsSF(p)) {
        FileState st; st.path=p; st.pos=0; st.known=true; PutFile(h,st);
        Log("SF-FILE",true,"OPEN h=0x%p path=\"%s\" access=0x%08lX caller=%s",h,W2U(p.c_str()).c_str(),(unsigned long)a,ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e); return h;
}
static HANDLE WINAPI H_CreateFileA(LPCSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t) {
    std::wstring p=A2W(n?n:"");
    HANDLE h=Real_CreateFileA(n,a,s,sa,c,f,t); DWORD e=GetLastError();
    if(h!=INVALID_HANDLE_VALUE && IsSF(p)) {
        FileState st; st.path=p; st.pos=0; st.known=true; PutFile(h,st);
        Log("SF-FILE",true,"OPEN h=0x%p path=\"%s\" access=0x%08lX caller=%s",h,n?n:"",(unsigned long)a,ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e); return h;
}
static BOOL WINAPI H_GetFileSizeEx(HANDLE h,PLARGE_INTEGER sz) {
    BOOL r=Real_GetFileSizeEx(h,sz); DWORD e=GetLastError(); FileState st=GetFile(h);
    if(!st.path.empty()) Log("SF-SIZE",true,"h=0x%p size=%lld result=%d error=%lu caller=%s",h,sz?(long long)sz->QuadPart:-1LL,r,(unsigned long)e,ModRva(__builtin_return_address(0)).c_str());
    SetLastError(e); return r;
}
static BOOL WINAPI H_SetFilePointerEx(HANDLE h,LARGE_INTEGER d,PLARGE_INTEGER np,DWORD m) {
    FileState before=GetFile(h);
    BOOL r=Real_SetFilePointerEx(h,d,np,m); DWORD e=GetLastError();
    if(!before.path.empty()) {
        uint64_t pos=np?(uint64_t)np->QuadPart:0;
        if(r) { before.pos=pos; before.known=true; PutFile(h,before); }
        Log("SF-SEEK",true,"h=0x%p move=%lld method=%lu result=%d new=%s%llu error=%lu caller=%s",h,(long long)d.QuadPart,(unsigned long)m,r,np?"":"?",(unsigned long long)pos,(unsigned long)e,ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e); return r;
}
static DWORD WINAPI H_SetFilePointer(HANDLE h,LONG lo,PLONG hi,DWORD m) {
    FileState before=GetFile(h);
    DWORD r=Real_SetFilePointer(h,lo,hi,m); DWORD e=GetLastError();
    if(!before.path.empty()) {
        bool ok=!(r==INVALID_SET_FILE_POINTER && e!=NO_ERROR);
        uint64_t pos=((uint64_t)(hi?*(DWORD*)hi:0)<<32)|r;
        if(ok){before.pos=pos;before.known=true;PutFile(h,before);}
        Log("SF-SEEK",true,"h=0x%p moveLo=%ld method=%lu result=0x%08lX new=%llu error=%lu caller=%s",h,(long)lo,(unsigned long)m,(unsigned long)r,(unsigned long long)pos,(unsigned long)e,ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e); return r;
}
static BOOL WINAPI H_ReadFile(HANDLE h,LPVOID b,DWORD want,LPDWORD got,LPOVERLAPPED ov) {
    FileState st=GetFile(h);
    uint64_t off=st.known?st.pos:0;
    if(ov) off=((uint64_t)ov->OffsetHigh<<32)|ov->Offset;
    LONG id=InterlockedIncrement(&g_id);
    if(!st.path.empty()) Log("SF-READ",true,"#%ld BEGIN h=0x%p off=%s%llu want=%lu overlapped=%d caller=%s",(long)id,h,(st.known||ov)?"":"?",(unsigned long long)off,(unsigned long)want,ov?1:0,ModRva(__builtin_return_address(0)).c_str());
    BOOL r=Real_ReadFile(h,b,want,got,ov); DWORD e=GetLastError();
    DWORD n=got?*got:0;
    if(!st.path.empty()) {
        Log("SF-READ",true,"#%ld END result=%d got=%lu error=%lu",(long)id,r,(unsigned long)n,(unsigned long)e);
        if(r && !ov && st.known){st.pos+=n;PutFile(h,st);}
    }
    SetLastError(e); return r;
}
static HANDLE WINAPI H_CreateFileMappingW(HANDLE f,LPSECURITY_ATTRIBUTES sa,DWORD p,DWORD hi,DWORD lo,LPCWSTR n) {
    HANDLE h=Real_CreateFileMappingW(f,sa,p,hi,lo,n); DWORD e=GetLastError(); FileState st=GetFile(f);
    if(h && !st.path.empty()){EnterCriticalSection(&g_state);g_maps[(uintptr_t)h]={f,st.path};LeaveCriticalSection(&g_state);Log("SF-MAP",true,"CreateFileMapping h=0x%p file=0x%p max=0x%08lX%08lX caller=%s",h,f,(unsigned long)hi,(unsigned long)lo,ModRva(__builtin_return_address(0)).c_str());}
    SetLastError(e);return h;
}
static HANDLE WINAPI H_CreateFileMappingA(HANDLE f,LPSECURITY_ATTRIBUTES sa,DWORD p,DWORD hi,DWORD lo,LPCSTR n) {
    HANDLE h=Real_CreateFileMappingA(f,sa,p,hi,lo,n); DWORD e=GetLastError(); FileState st=GetFile(f);
    if(h && !st.path.empty()){EnterCriticalSection(&g_state);g_maps[(uintptr_t)h]={f,st.path};LeaveCriticalSection(&g_state);Log("SF-MAP",true,"CreateFileMappingA h=0x%p file=0x%p max=0x%08lX%08lX caller=%s",h,f,(unsigned long)hi,(unsigned long)lo,ModRva(__builtin_return_address(0)).c_str());}
    SetLastError(e);return h;
}
static LPVOID WINAPI H_MapViewOfFile(HANDLE m,DWORD a,DWORD hi,DWORD lo,SIZE_T n) {
    MapState ms; EnterCriticalSection(&g_state);auto it=g_maps.find((uintptr_t)m);if(it!=g_maps.end())ms=it->second;LeaveCriticalSection(&g_state);
    LPVOID p=Real_MapViewOfFile(m,a,hi,lo,n); DWORD e=GetLastError();
    if(!ms.path.empty()) Log("SF-MAP",true,"MapView mapping=0x%p off=0x%08lX%08lX size=%llu -> 0x%p error=%lu caller=%s",m,(unsigned long)hi,(unsigned long)lo,(unsigned long long)n,p,(unsigned long)e,ModRva(__builtin_return_address(0)).c_str());
    SetLastError(e);return p;
}
static BOOL WINAPI H_CloseHandle(HANDLE h) {
    FileState st=GetFile(h);
    if(!st.path.empty()) Log("SF-FILE",true,"CLOSE h=0x%p path=\"%s\" caller=%s",h,W2U(st.path.c_str()).c_str(),ModRva(__builtin_return_address(0)).c_str());
    Drop(h); return Real_CloseHandle(h);
}

static void StackFromContext(CONTEXT* c) {
    if(!c)return;
    Log("CRASH",true,"STACK[0] %s",ModRva((void*)(uintptr_t)c->Eip).c_str());
    uintptr_t ebp=(uintptr_t)c->Ebp;
    for(int i=1;i<12 && ebp;i++){
        struct Frame{uintptr_t prev;uintptr_t ret;} fr{}; SIZE_T br=0;
        if(!ReadProcessMemory(GetCurrentProcess(),(LPCVOID)ebp,&fr,sizeof(fr),&br)||br!=sizeof(fr)||!fr.ret)break;
        Log("CRASH",true,"STACK[%d] %s",i,ModRva((void*)fr.ret).c_str());
        if(fr.prev<=ebp || fr.prev-ebp>0x100000)break; ebp=fr.prev;
    }
}
static LONG CALLBACK Veh(PEXCEPTION_POINTERS ep) {
    if(!ep||!ep->ExceptionRecord||!ep->ContextRecord)return EXCEPTION_CONTINUE_SEARCH;
    DWORD c=ep->ExceptionRecord->ExceptionCode;
    switch(c){
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: break;
        default:return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* x=ep->ContextRecord;
    Log("CRASH",true,"EXCEPTION code=0x%08lX addr=%s EIP=%08lX ESP=%08lX EBP=%08lX EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX",
        (unsigned long)c,ModRva(ep->ExceptionRecord->ExceptionAddress).c_str(),(unsigned long)x->Eip,(unsigned long)x->Esp,(unsigned long)x->Ebp,
        (unsigned long)x->Eax,(unsigned long)x->Ebx,(unsigned long)x->Ecx,(unsigned long)x->Edx,(unsigned long)x->Esi,(unsigned long)x->Edi);
    if(c==EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters>=2)
        Log("CRASH",true,"ACCESS type=%llu address=0x%08llX",(unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],(unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    StackFromContext(x);
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool HookOne(HMODULE mod,const char* name,void* hook) {
    if(!mod||mod==g_self)return false;
    auto base=(uint8_t*)mod;
    auto dos=(IMAGE_DOS_HEADER*)base;
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
    auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE)return false;
    DWORD rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if(!rva)return false;
    bool any=false;
    auto imp=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);
    for(;imp->Name;imp++){
        auto oft=imp->OriginalFirstThunk?(IMAGE_THUNK_DATA*)(base+imp->OriginalFirstThunk):(IMAGE_THUNK_DATA*)(base+imp->FirstThunk);
        auto ft=(IMAGE_THUNK_DATA*)(base+imp->FirstThunk);
        for(;oft->u1.AddressOfData;oft++,ft++){
            if(IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal))continue;
            auto ibn=(IMAGE_IMPORT_BY_NAME*)(base+oft->u1.AddressOfData);
            if(strcmp((char*)ibn->Name,name))continue;
            DWORD old=0;
            if(VirtualProtect(&ft->u1.Function,sizeof(uintptr_t),PAGE_READWRITE,&old)){
                ft->u1.Function=(uintptr_t)hook;
                DWORD tmp=0;VirtualProtect(&ft->u1.Function,sizeof(uintptr_t),old,&tmp);
                FlushInstructionCache(GetCurrentProcess(),&ft->u1.Function,sizeof(uintptr_t));
                any=true;
            }
        }
    }
    return any;
}
static void PatchModule(HMODULE m) {
    if(!m||m==g_self)return;
    EnterCriticalSection(&g_state);
    bool seen=g_patched.count((uintptr_t)m)!=0;
    if(!seen)g_patched.insert((uintptr_t)m);
    LeaveCriticalSection(&g_state);
    if(seen)return;
    int n=0;
    n+=HookOne(m,"CreateFileW",(void*)H_CreateFileW);
    n+=HookOne(m,"CreateFileA",(void*)H_CreateFileA);
    n+=HookOne(m,"ReadFile",(void*)H_ReadFile);
    n+=HookOne(m,"SetFilePointerEx",(void*)H_SetFilePointerEx);
    n+=HookOne(m,"SetFilePointer",(void*)H_SetFilePointer);
    n+=HookOne(m,"GetFileSizeEx",(void*)H_GetFileSizeEx);
    n+=HookOne(m,"CreateFileMappingW",(void*)H_CreateFileMappingW);
    n+=HookOne(m,"CreateFileMappingA",(void*)H_CreateFileMappingA);
    n+=HookOne(m,"MapViewOfFile",(void*)H_MapViewOfFile);
    n+=HookOne(m,"CloseHandle",(void*)H_CloseHandle);
    if(n)Log("HOOK",false,"patched %d imports in %s",n,ModRva((void*)m).c_str());
}
static void ScanModules() {
    HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,GetCurrentProcessId());
    if(s==INVALID_HANDLE_VALUE)return;
    MODULEENTRY32W me{};me.dwSize=sizeof(me);
    if(Module32FirstW(s,&me)){do{PatchModule(me.hModule);}while(Module32NextW(s,&me));}
    Real_CloseHandle(s);
}

static DWORD WINAPI Worker(LPVOID) {
    InitializeCriticalSection(&g_lock);InitializeCriticalSection(&g_state);
    QueryPerformanceFrequency(&g_freq);QueryPerformanceCounter(&g_start);
    wchar_t me[MAX_PATH]{};GetModuleFileNameW(g_self,me,MAX_PATH);
    wchar_t* p=wcsrchr(me,L'\\');if(p)*(p+1)=0;
    wchar_t lp[MAX_PATH]{};_snwprintf(lp,MAX_PATH-1,L"%sNFSTR_LevelDiagnostics.log",me);
    g_log=CreateFileW(lp,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(g_log==INVALID_HANDLE_VALUE)return 1;
    const unsigned char bom[3]={0xEF,0xBB,0xBF};DWORD wr=0;WriteFile(g_log,bom,3,&wr,nullptr);
    Log("INIT",true,"NFSTR Level Diagnostics v0.4.0 loaded PID=%lu EXEbase=0x%p",(unsigned long)GetCurrentProcessId(),GetModuleHandleW(nullptr));
    Log("INIT",true,"watching ONLY paths containing Level_0100_SanFrancisco; crash VEH active");
    AddVectoredExceptionHandler(1,Veh);
    ScanModules();
    while(WaitForSingleObject(g_stop,250)==WAIT_TIMEOUT)ScanModules();
    Log("INIT",true,"stopping");
    FlushFileBuffers(g_log);CloseHandle(g_log);g_log=INVALID_HANDLE_VALUE;
    return 0;
}

} // namespace lvl

BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID) {
    using namespace lvl;
    if(r==DLL_PROCESS_ATTACH){
        g_self=(HMODULE)h;DisableThreadLibraryCalls(h);
        g_stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE t=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);if(t)CloseHandle(t);
    } else if(r==DLL_PROCESS_DETACH) {
        if(g_stop)SetEvent(g_stop);
    }
    return TRUE;
}
