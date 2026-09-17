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

// NFSTR_BundleCallTrace.asi v0.1.0
// Exact call-stack tracer for Need for Speed: The Run v1.1 x86.
// Tracks only Update\Patch\...\Level_0100_SanFrancisco.sb and logs a real
// RtlCaptureStackBackTrace for each file seek. Intended to compare a working
// CS_SLE_4 request (seek to 58800) against broken CS_ITC_1.

namespace bct {
static HMODULE g_self=nullptr;
static HANDLE g_log=INVALID_HANDLE_VALUE;
static HANDLE g_stop=nullptr;
static CRITICAL_SECTION g_lock,g_stateLock;
static LARGE_INTEGER g_freq{},g_start{};
static uintptr_t g_exeBase=0,g_textBegin=0,g_textEnd=0;
using PFN_RtlCaptureStackBackTrace=USHORT (WINAPI*)(ULONG,ULONG,PVOID*,PULONG);
static PFN_RtlCaptureStackBackTrace g_bt=nullptr;

struct FState { std::wstring path; uint64_t pos=0; bool known=false; bool target=false; };
static std::unordered_map<uintptr_t,FState> g_files;

static std::wstring Lower(std::wstring s){std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return (wchar_t)towlower(c);});return s;}
static bool IsTarget(const std::wstring& p){
    auto q=Lower(p);
    return q.find(L"update\\patch")!=std::wstring::npos &&
           q.find(L"level_0100_sanfrancisco\\level_0100_sanfrancisco.sb")!=std::wstring::npos &&
           q.find(L"terrain")==std::wstring::npos;
}
static std::string W2U(const wchar_t* s){
    if(!s||!*s)return{}; int n=WideCharToMultiByte(CP_UTF8,0,s,-1,nullptr,0,nullptr,nullptr); if(n<=0)return{};
    std::string o((size_t)n,'\0'); if(!WideCharToMultiByte(CP_UTF8,0,s,-1,&o[0],n,nullptr,nullptr))return{}; if(!o.empty()&&o.back()=='\0')o.pop_back(); return o;
}
static std::wstring A2W(const char* s){
    if(!s||!*s)return{}; int n=MultiByteToWideChar(CP_ACP,0,s,-1,nullptr,0); if(n<=0)return{};
    std::wstring o((size_t)n,L'\0'); if(!MultiByteToWideChar(CP_ACP,0,s,-1,&o[0],n))return{}; if(!o.empty()&&o.back()==L'\0')o.pop_back(); return o;
}
static double Ms(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return(double)(q.QuadPart-g_start.QuadPart)*1000.0/(double)g_freq.QuadPart;}
static std::string ModRva(void* a){
    if(!a)return"<null>"; MEMORY_BASIC_INFORMATION mbi{}; if(!VirtualQuery(a,&mbi,sizeof(mbi))||!mbi.AllocationBase){char x[32];_snprintf(x,31,"0x%08lX",(unsigned long)(uintptr_t)a);x[31]=0;return x;}
    HMODULE m=(HMODULE)mbi.AllocationBase; wchar_t w[MAX_PATH]{};GetModuleFileNameW(m,w,MAX_PATH);std::string p=W2U(w);size_t k=p.find_last_of("\\/");if(k!=std::string::npos)p=p.substr(k+1);
    char x[MAX_PATH+64];_snprintf(x,sizeof(x)-1,"%s+0x%08lX",p.c_str(),(unsigned long)((uintptr_t)a-(uintptr_t)m));x[sizeof(x)-1]=0;return x;
}
static void Log(const char* cat,const char* fmt,...){
    if(g_log==INVALID_HANDLE_VALUE)return; char b[8192];va_list ap;va_start(ap,fmt);_vsnprintf(b,sizeof(b)-1,fmt,ap);va_end(ap);b[sizeof(b)-1]=0;
    char l[8704];_snprintf(l,sizeof(l)-1,"[%011.3f] [TID %5lu] [%s] %s\r\n",Ms(),(unsigned long)GetCurrentThreadId(),cat,b);l[sizeof(l)-1]=0;
    EnterCriticalSection(&g_lock);DWORD wr=0;WriteFile(g_log,l,(DWORD)strlen(l),&wr,nullptr);FlushFileBuffers(g_log);LeaveCriticalSection(&g_lock);
}
static FState Get(HANDLE h){FState s;EnterCriticalSection(&g_stateLock);auto it=g_files.find((uintptr_t)h);if(it!=g_files.end())s=it->second;LeaveCriticalSection(&g_stateLock);return s;}
static void Put(HANDLE h,const FState&s){if(!h||h==INVALID_HANDLE_VALUE)return;EnterCriticalSection(&g_stateLock);g_files[(uintptr_t)h]=s;LeaveCriticalSection(&g_stateLock);}
static void Drop(HANDLE h){EnterCriticalSection(&g_stateLock);g_files.erase((uintptr_t)h);LeaveCriticalSection(&g_stateLock);}

static void InitText(){
    HMODULE e=GetModuleHandleW(nullptr);g_exeBase=(uintptr_t)e;if(!e)return;uint8_t*b=(uint8_t*)e;auto*d=(IMAGE_DOS_HEADER*)b;if(d->e_magic!=IMAGE_DOS_SIGNATURE)return;auto*n=(IMAGE_NT_HEADERS*)(b+d->e_lfanew);if(n->Signature!=IMAGE_NT_SIGNATURE)return;
    auto*s=IMAGE_FIRST_SECTION(n);for(unsigned i=0;i<n->FileHeader.NumberOfSections;++i){char nm[9]{};memcpy(nm,s[i].Name,8);if(strcmp(nm,".text")==0){g_textBegin=g_exeBase+s[i].VirtualAddress;size_t z=s[i].Misc.VirtualSize?s[i].Misc.VirtualSize:s[i].SizeOfRawData;g_textEnd=g_textBegin+z;break;}}
}
static void Backtrace(const char* why,uint64_t oldPos,long long move,DWORD method){
    if(!g_bt){Log("BT","%s old=%llu move=%lld method=%lu unavailable",why,(unsigned long long)oldPos,move,(unsigned long)method);return;}
    PVOID f[64]{};USHORT n=g_bt(0,64,f,nullptr);
    char line[7600];size_t u=0;u+=(size_t)_snprintf(line+u,sizeof(line)-u,"%s old=%llu move=%lld method=%lu captured=%u EXE:",why,(unsigned long long)oldPos,move,(unsigned long)method,(unsigned)n);
    int ec=0;for(USHORT i=0;i<n;++i){uintptr_t v=(uintptr_t)f[i];if(v<g_textBegin||v>=g_textEnd)continue;int m=_snprintf(line+u,sizeof(line)-u," #%u=EXE+0x%08lX",(unsigned)i,(unsigned long)(v-g_exeBase));if(m<=0||u+(size_t)m>=sizeof(line)-64)break;u+=(size_t)m;++ec;}
    if(!ec)_snprintf(line+u,sizeof(line)-u," <none>");line[sizeof(line)-1]=0;Log("BT","%s",line);
    for(USHORT i=0;i<n&&i<24;++i)Log("FRAME","%s old=%llu move=%lld method=%lu #%u %s",why,(unsigned long long)oldPos,move,(unsigned long)method,(unsigned)i,ModRva(f[i]).c_str());
}

using PFN_CreateFileW=HANDLE(WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_CreateFileA=HANDLE(WINAPI*)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_SetFilePointerEx=BOOL(WINAPI*)(HANDLE,LARGE_INTEGER,PLARGE_INTEGER,DWORD);
using PFN_SetFilePointer=DWORD(WINAPI*)(HANDLE,LONG,PLONG,DWORD);
using PFN_ReadFile=BOOL(WINAPI*)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
using PFN_CloseHandle=BOOL(WINAPI*)(HANDLE);
static PFN_CreateFileW R_CreateFileW=::CreateFileW;static PFN_CreateFileA R_CreateFileA=::CreateFileA;static PFN_SetFilePointerEx R_SetFilePointerEx=::SetFilePointerEx;static PFN_SetFilePointer R_SetFilePointer=::SetFilePointer;static PFN_ReadFile R_ReadFile=::ReadFile;static PFN_CloseHandle R_CloseHandle=::CloseHandle;

static HANDLE WINAPI H_CreateFileW(LPCWSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t){std::wstring p=n?n:L"";HANDLE h=R_CreateFileW(n,a,s,sa,c,f,t);DWORD e=GetLastError();if(h!=INVALID_HANDLE_VALUE&&IsTarget(p)){FState st;st.path=p;st.known=true;st.target=true;Put(h,st);Log("FILE","OPEN h=0x%p path=\"%s\" caller=%s",h,W2U(p.c_str()).c_str(),ModRva(__builtin_return_address(0)).c_str());}SetLastError(e);return h;}
static HANDLE WINAPI H_CreateFileA(LPCSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t){std::wstring p=A2W(n?n:"");HANDLE h=R_CreateFileA(n,a,s,sa,c,f,t);DWORD e=GetLastError();if(h!=INVALID_HANDLE_VALUE&&IsTarget(p)){FState st;st.path=p;st.known=true;st.target=true;Put(h,st);Log("FILE","OPEN h=0x%p path=\"%s\" caller=%s",h,n?n:"",ModRva(__builtin_return_address(0)).c_str());}SetLastError(e);return h;}
static BOOL WINAPI H_SetFilePointerEx(HANDLE h,LARGE_INTEGER d,PLARGE_INTEGER np,DWORD m){FState st=Get(h);if(st.target){Log("SEEK","BEGIN old=%s%llu move=%lld method=%lu caller=%s",st.known?"":"?",(unsigned long long)st.pos,(long long)d.QuadPart,(unsigned long)m,ModRva(__builtin_return_address(0)).c_str());Backtrace("SEEK",st.pos,(long long)d.QuadPart,m);}BOOL r=R_SetFilePointerEx(h,d,np,m);DWORD e=GetLastError();if(st.target&&r&&np){st.pos=(uint64_t)np->QuadPart;st.known=true;Put(h,st);Log("SEEK","END new=%llu error=%lu",(unsigned long long)st.pos,(unsigned long)e);}SetLastError(e);return r;}
static DWORD WINAPI H_SetFilePointer(HANDLE h,LONG lo,PLONG hi,DWORD m){FState st=Get(h);if(st.target){Log("SEEK","BEGIN32 old=%s%llu move=%ld method=%lu caller=%s",st.known?"":"?",(unsigned long long)st.pos,(long)lo,(unsigned long)m,ModRva(__builtin_return_address(0)).c_str());Backtrace("SEEK32",st.pos,(long long)lo,m);}DWORD r=R_SetFilePointer(h,lo,hi,m);DWORD e=GetLastError();if(st.target){bool ok=!(r==INVALID_SET_FILE_POINTER&&e!=NO_ERROR);uint64_t p=((uint64_t)(hi?*(DWORD*)hi:0)<<32)|r;if(ok){st.pos=p;st.known=true;Put(h,st);}Log("SEEK","END32 new=%llu error=%lu",(unsigned long long)p,(unsigned long)e);}SetLastError(e);return r;}
static BOOL WINAPI H_ReadFile(HANDLE h,LPVOID b,DWORD w,LPDWORD got,LPOVERLAPPED ov){FState st=Get(h);BOOL r=R_ReadFile(h,b,w,got,ov);DWORD e=GetLastError();if(st.target&&r&&!ov&&st.known){st.pos+=(got?*got:0);Put(h,st);}SetLastError(e);return r;}
static BOOL WINAPI H_CloseHandle(HANDLE h){FState st=Get(h);if(st.target)Log("FILE","CLOSE h=0x%p",h);Drop(h);return R_CloseHandle(h);}

static bool Hook(HMODULE mod,const char* name,void* fn){if(!mod||mod==g_self)return false;uint8_t*b=(uint8_t*)mod;auto*d=(IMAGE_DOS_HEADER*)b;if(d->e_magic!=IMAGE_DOS_SIGNATURE)return false;auto*n=(IMAGE_NT_HEADERS*)(b+d->e_lfanew);if(n->Signature!=IMAGE_NT_SIGNATURE)return false;DWORD rva=n->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;if(!rva)return false;bool any=false;auto*imp=(IMAGE_IMPORT_DESCRIPTOR*)(b+rva);for(;imp->Name;++imp){auto*oft=imp->OriginalFirstThunk?(IMAGE_THUNK_DATA*)(b+imp->OriginalFirstThunk):(IMAGE_THUNK_DATA*)(b+imp->FirstThunk);auto*ft=(IMAGE_THUNK_DATA*)(b+imp->FirstThunk);for(;oft->u1.AddressOfData;++oft,++ft){if(IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal))continue;auto*ibn=(IMAGE_IMPORT_BY_NAME*)(b+oft->u1.AddressOfData);if(strcmp((char*)ibn->Name,name))continue;DWORD old=0;if(!VirtualProtect(&ft->u1.Function,sizeof(uintptr_t),PAGE_READWRITE,&old))continue;ft->u1.Function=(uintptr_t)fn;DWORD x=0;VirtualProtect(&ft->u1.Function,sizeof(uintptr_t),old,&x);FlushInstructionCache(GetCurrentProcess(),&ft->u1.Function,sizeof(uintptr_t));any=true;}}return any;}
static int Arm(){HMODULE e=GetModuleHandleW(nullptr);int n=0;n+=Hook(e,"CreateFileW",(void*)H_CreateFileW);n+=Hook(e,"CreateFileA",(void*)H_CreateFileA);n+=Hook(e,"SetFilePointerEx",(void*)H_SetFilePointerEx);n+=Hook(e,"SetFilePointer",(void*)H_SetFilePointer);n+=Hook(e,"ReadFile",(void*)H_ReadFile);n+=Hook(e,"CloseHandle",(void*)H_CloseHandle);return n;}

static DWORD WINAPI Worker(LPVOID){InitializeCriticalSection(&g_lock);InitializeCriticalSection(&g_stateLock);QueryPerformanceFrequency(&g_freq);QueryPerformanceCounter(&g_start);InitText();HMODULE k=GetModuleHandleW(L"kernel32.dll");if(k)g_bt=(PFN_RtlCaptureStackBackTrace)GetProcAddress(k,"RtlCaptureStackBackTrace");if(!g_bt){HMODULE n=GetModuleHandleW(L"ntdll.dll");if(n)g_bt=(PFN_RtlCaptureStackBackTrace)GetProcAddress(n,"RtlCaptureStackBackTrace");}
    wchar_t me[MAX_PATH]{};GetModuleFileNameW(g_self,me,MAX_PATH);wchar_t*sl=wcsrchr(me,L'\\');if(sl)*(sl+1)=0;wchar_t lp[MAX_PATH]{};_snwprintf(lp,MAX_PATH-1,L"%sNFSTR_BundleCallTrace_%lu.log",me,(unsigned long)GetCurrentProcessId());g_log=CreateFileW(lp,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);if(g_log==INVALID_HANDLE_VALUE)return 1;const unsigned char bom[3]={0xEF,0xBB,0xBF};DWORD wr=0;WriteFile(g_log,bom,3,&wr,nullptr);
    Log("INIT","NFSTR Bundle Call Trace v0.1.0 PID=%lu EXEbase=0x%p text=[0x%08lX,0x%08lX) RtlCaptureStackBackTrace=%s",(unsigned long)GetCurrentProcessId(),GetModuleHandleW(nullptr),(unsigned long)(g_textBegin-g_exeBase),(unsigned long)(g_textEnd-g_exeBase),g_bt?"yes":"no");int n=Arm();Log("ARM","AUTO ARMED: patched %d MAIN EXE imports",n);while(WaitForSingleObject(g_stop,50)==WAIT_TIMEOUT){}Log("INIT","stopping");FlushFileBuffers(g_log);R_CloseHandle(g_log);g_log=INVALID_HANDLE_VALUE;return 0;}
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){using namespace bct;if(r==DLL_PROCESS_ATTACH){g_self=(HMODULE)h;DisableThreadLibraryCalls(h);g_stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);HANDLE t=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);if(t)CloseHandle(t);}else if(r==DLL_PROCESS_DETACH){if(g_stop)SetEvent(g_stop);}return TRUE;}
