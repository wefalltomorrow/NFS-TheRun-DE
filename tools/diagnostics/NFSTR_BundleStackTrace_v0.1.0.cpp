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
#include <vector>

// NFSTR_BundleStackTrace.asi v0.1.0
// Targeted runtime tracer for Need for Speed: The Run v1.1 x86.
//
// Purpose:
//   Find the higher-level call path that chooses a Challenge Series sublevel
//   bundle (for example cs_sle_4) before Frostbite reaches ordinary file I/O.
//
// Strategy:
//   - Hook only the MAIN EXE import table.
//   - Track only Update\Patch\...\Level_0100_SanFrancisco.sb.
//   - On every seek/read for that exact patch SB, dump:
//       * current file offset / operation
//       * candidate return addresses found on the current thread stack that
//         point into the main EXE .text section
//       * interesting ASCII/UTF-16 strings referenced directly by stack slots
//   This is intentionally narrow so a working CS_SLE_4 run can be compared
//   against the broken CS_ITC_1 run without producing a massive generic log.
//
// Safety:
//   No code bytes are patched. Only selected MAIN EXE IAT entries are changed.
//   No F-key is required. It can coexist with NFSTR_CrashGuard because this
//   tracer does not use F6 and does not hook the guarded target.

namespace bst {

static HMODULE g_self = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_stop = nullptr;
static CRITICAL_SECTION g_logLock;
static CRITICAL_SECTION g_stateLock;
static LARGE_INTEGER g_freq{}, g_start{};
static volatile LONG g_readId = 0;
static uintptr_t g_exeBase = 0;
static uintptr_t g_textBegin = 0;
static uintptr_t g_textEnd = 0;

struct FileState {
    std::wstring path;
    uint64_t pos = 0;
    bool known = false;
    bool patchSfSb = false;
};
static std::unordered_map<uintptr_t, FileState> g_files;

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
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
    char body[8192];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(body, sizeof(body)-1, fmt, ap);
    va_end(ap);
    body[sizeof(body)-1] = 0;
    char line[8704];
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

static bool PageReadable(uintptr_t p, size_t need, MEMORY_BASIC_INFORMATION* outMbi = nullptr) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
    uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (p > end || need > (size_t)(end - p)) return false;
    if (outMbi) *outMbi = mbi;
    return true;
}

static bool InterestingLower(const std::string& s) {
    std::string q = s;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c){ return (char)tolower(c); });
    return q.find("cs_") != std::string::npos ||
           q.find("sle") != std::string::npos ||
           q.find("itc") != std::string::npos ||
           q.find("challenge") != std::string::npos ||
           q.find("sanfrancisco") != std::string::npos ||
           q.find("level_0100") != std::string::npos ||
           q.find("_c4/") != std::string::npos ||
           q.find("_c4\\") != std::string::npos;
}

static bool TryAscii(uintptr_t p, std::string& out) {
    out.clear();
    MEMORY_BASIC_INFORMATION mbi{};
    if (!PageReadable(p, 1, &mbi)) return false;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    size_t maxn = std::min<size_t>(192, (size_t)(regionEnd - p));
    if (maxn < 4) return false;
    const unsigned char* s = (const unsigned char*)p;
    size_t n = 0;
    for (; n < maxn; ++n) {
        unsigned char c = s[n];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return false;
    }
    if (n < 4 || n == maxn) return false;
    out.assign((const char*)s, n);
    return InterestingLower(out);
}

static bool TryWide(uintptr_t p, std::string& out) {
    out.clear();
    MEMORY_BASIC_INFORMATION mbi{};
    if (!PageReadable(p, sizeof(wchar_t), &mbi)) return false;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    size_t maxChars = std::min<size_t>(160, (size_t)(regionEnd - p) / sizeof(wchar_t));
    if (maxChars < 4) return false;
    const wchar_t* s = (const wchar_t*)p;
    size_t n = 0;
    for (; n < maxChars; ++n) {
        wchar_t c = s[n];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return false;
    }
    if (n < 4 || n == maxChars) return false;
    std::wstring w(s, n);
    out = W2U(w.c_str());
    return InterestingLower(out);
}

static void InitExeTextRange() {
    HMODULE exe = GetModuleHandleW(nullptr);
    g_exeBase = (uintptr_t)exe;
    if (!exe) return;
    uint8_t* base = (uint8_t*)exe;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9]{};
        memcpy(name, sec[i].Name, 8);
        if (strcmp(name, ".text") == 0) {
            g_textBegin = g_exeBase + sec[i].VirtualAddress;
            size_t vsz = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            g_textEnd = g_textBegin + vsz;
            break;
        }
    }
}

static void DumpStackEvidence(const char* reason, uint64_t fileOff) {
#if defined(__i386__)
    uintptr_t* sp = nullptr;
    __asm__ __volatile__("movl %%esp,%0" : "=r"(sp));
#else
    uintptr_t* sp = (uintptr_t*)_AddressOfReturnAddress();
#endif
    if (!sp || !g_textBegin || !g_textEnd) return;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(sp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    uintptr_t scanEndAddr = std::min<uintptr_t>((uintptr_t)sp + 4096, regionEnd);
    uintptr_t* end = (uintptr_t*)scanEndAddr;

    std::vector<uintptr_t> seenCode;
    char codeLine[7600];
    size_t used = 0;
    used += (size_t)_snprintf(codeLine + used, sizeof(codeLine)-used,
                             "%s off=%llu candidates:", reason, (unsigned long long)fileOff);
    int codeCount = 0;
    for (uintptr_t* q = sp; q < end && codeCount < 40; ++q) {
        uintptr_t v = *q;
        if (v < g_textBegin || v >= g_textEnd) continue;
        if (std::find(seenCode.begin(), seenCode.end(), v) != seenCode.end()) continue;
        seenCode.push_back(v);
        unsigned slot = (unsigned)(q - sp);
        int n = _snprintf(codeLine + used, sizeof(codeLine)-used,
                          " s%u=EXE+0x%08lX", slot,
                          (unsigned long)(v - g_exeBase));
        if (n <= 0 || used + (size_t)n >= sizeof(codeLine)-64) break;
        used += (size_t)n;
        ++codeCount;
    }
    codeLine[sizeof(codeLine)-1] = 0;
    Log("STACK", true, "%s", codeLine);

    int stringCount = 0;
    for (uintptr_t* q = sp; q < end && stringCount < 24; ++q) {
        uintptr_t p = *q;
        std::string s;
        const char* kind = nullptr;
        if (TryAscii(p, s)) kind = "A";
        else if (TryWide(p, s)) kind = "W";
        if (!kind) continue;
        unsigned slot = (unsigned)(q - sp);
        Log("STACKSTR", true, "%s off=%llu s%u ptr=0x%08lX %s=\"%s\"",
            reason, (unsigned long long)fileOff, slot, (unsigned long)p, kind, s.c_str());
        ++stringCount;
    }
}

using PFN_CreateFileW = HANDLE (WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_CreateFileA = HANDLE (WINAPI*)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_ReadFile = BOOL (WINAPI*)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
using PFN_SetFilePointerEx = BOOL (WINAPI*)(HANDLE,LARGE_INTEGER,PLARGE_INTEGER,DWORD);
using PFN_SetFilePointer = DWORD (WINAPI*)(HANDLE,LONG,PLONG,DWORD);
using PFN_CloseHandle = BOOL (WINAPI*)(HANDLE);

static PFN_CreateFileW Real_CreateFileW = ::CreateFileW;
static PFN_CreateFileA Real_CreateFileA = ::CreateFileA;
static PFN_ReadFile Real_ReadFile = ::ReadFile;
static PFN_SetFilePointerEx Real_SetFilePointerEx = ::SetFilePointerEx;
static PFN_SetFilePointer Real_SetFilePointer = ::SetFilePointer;
static PFN_CloseHandle Real_CloseHandle = ::CloseHandle;

static HANDLE WINAPI H_CreateFileW(LPCWSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t) {
    std::wstring p = n ? n : L"";
    HANDLE h = Real_CreateFileW(n,a,s,sa,c,f,t);
    DWORD e = GetLastError();
    if (h != INVALID_HANDLE_VALUE && IsPatchSanFranciscoSb(p)) {
        FileState st; st.path=p; st.pos=0; st.known=true; st.patchSfSb=true; PutFile(h,st);
        Log("FILE", true, "OPEN patch-SF-SB h=0x%p path=\"%s\" caller=%s",
            h, W2U(p.c_str()).c_str(), ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e);
    return h;
}

static HANDLE WINAPI H_CreateFileA(LPCSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t) {
    std::wstring p = A2W(n ? n : "");
    HANDLE h = Real_CreateFileA(n,a,s,sa,c,f,t);
    DWORD e = GetLastError();
    if (h != INVALID_HANDLE_VALUE && IsPatchSanFranciscoSb(p)) {
        FileState st; st.path=p; st.pos=0; st.known=true; st.patchSfSb=true; PutFile(h,st);
        Log("FILE", true, "OPEN patch-SF-SB h=0x%p path=\"%s\" caller=%s",
            h, n?n:"", ModRva(__builtin_return_address(0)).c_str());
    }
    SetLastError(e);
    return h;
}

static BOOL WINAPI H_SetFilePointerEx(HANDLE h,LARGE_INTEGER d,PLARGE_INTEGER np,DWORD method) {
    FileState st = GetFile(h);
    if (st.patchSfSb) {
        Log("SEEK", true, "BEGIN h=0x%p old=%s%llu move=%lld method=%lu caller=%s",
            h, st.known?"":"?", (unsigned long long)st.pos,
            (long long)d.QuadPart, (unsigned long)method,
            ModRva(__builtin_return_address(0)).c_str());
        DumpStackEvidence("SEEK", st.pos);
    }
    BOOL r = Real_SetFilePointerEx(h,d,np,method);
    DWORD e = GetLastError();
    if (st.patchSfSb) {
        if (r && np) { st.pos=(uint64_t)np->QuadPart; st.known=true; PutFile(h,st); }
        Log("SEEK", true, "END h=0x%p result=%d new=%s%llu error=%lu",
            h, r, np?"":"?", (unsigned long long)(np?(uint64_t)np->QuadPart:0), (unsigned long)e);
    }
    SetLastError(e);
    return r;
}

static DWORD WINAPI H_SetFilePointer(HANDLE h,LONG lo,PLONG hi,DWORD method) {
    FileState st = GetFile(h);
    if (st.patchSfSb) {
        Log("SEEK", true, "BEGIN32 h=0x%p old=%s%llu moveLo=%ld method=%lu caller=%s",
            h, st.known?"":"?", (unsigned long long)st.pos,
            (long)lo, (unsigned long)method,
            ModRva(__builtin_return_address(0)).c_str());
        DumpStackEvidence("SEEK32", st.pos);
    }
    DWORD r = Real_SetFilePointer(h,lo,hi,method);
    DWORD e = GetLastError();
    if (st.patchSfSb) {
        bool ok = !(r == INVALID_SET_FILE_POINTER && e != NO_ERROR);
        uint64_t pos = ((uint64_t)(hi ? *(DWORD*)hi : 0) << 32) | r;
        if (ok) { st.pos=pos; st.known=true; PutFile(h,st); }
        Log("SEEK", true, "END32 h=0x%p result=0x%08lX new=%llu error=%lu",
            h, (unsigned long)r, (unsigned long long)pos, (unsigned long)e);
    }
    SetLastError(e);
    return r;
}

static BOOL WINAPI H_ReadFile(HANDLE h,LPVOID b,DWORD want,LPDWORD got,LPOVERLAPPED ov) {
    FileState st = GetFile(h);
    uint64_t off = st.known ? st.pos : 0;
    if (ov) off=((uint64_t)ov->OffsetHigh<<32)|ov->Offset;
    LONG id = InterlockedIncrement(&g_readId);
    if (st.patchSfSb) {
        Log("READ", true, "#%ld BEGIN h=0x%p off=%s%llu want=%lu overlapped=%d caller=%s",
            (long)id,h,(st.known||ov)?"":"?",(unsigned long long)off,
            (unsigned long)want,ov?1:0,ModRva(__builtin_return_address(0)).c_str());
        DumpStackEvidence("READ", off);
    }
    BOOL r = Real_ReadFile(h,b,want,got,ov);
    DWORD e = GetLastError();
    DWORD n = got ? *got : 0;
    if (st.patchSfSb) {
        Log("READ", true, "#%ld END result=%d got=%lu error=%lu",(long)id,r,(unsigned long)n,(unsigned long)e);
        if (r && !ov && st.known) { st.pos += n; PutFile(h,st); }
    }
    SetLastError(e);
    return r;
}

static BOOL WINAPI H_CloseHandle(HANDLE h) {
    FileState st = GetFile(h);
    if (st.patchSfSb)
        Log("FILE", true, "CLOSE patch-SF-SB h=0x%p path=\"%s\" caller=%s",
            h, W2U(st.path.c_str()).c_str(), ModRva(__builtin_return_address(0)).c_str());
    DropFile(h);
    return Real_CloseHandle(h);
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
    n += HookOne(exe,"CloseHandle",(void*)H_CloseHandle) ? 1 : 0;
    return n;
}

static DWORD WINAPI Worker(LPVOID) {
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_stateLock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    InitExeTextRange();

    wchar_t me[MAX_PATH]{};
    GetModuleFileNameW(g_self,me,MAX_PATH);
    wchar_t* slash=wcsrchr(me,L'\\');
    if (slash) *(slash+1)=0;
    wchar_t lp[MAX_PATH]{};
    _snwprintf(lp,MAX_PATH-1,L"%sNFSTR_BundleStackTrace_%lu.log",me,(unsigned long)GetCurrentProcessId());

    g_log=CreateFileW(lp,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                      CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if (g_log==INVALID_HANDLE_VALUE) return 1;
    const unsigned char bom[3]={0xEF,0xBB,0xBF};
    DWORD wr=0;
    WriteFile(g_log,bom,3,&wr,nullptr);

    Log("INIT",true,"NFSTR Bundle Stack Trace v0.1.0 loaded PID=%lu EXEbase=0x%p text=[0x%08lX,0x%08lX)",
        (unsigned long)GetCurrentProcessId(),GetModuleHandleW(nullptr),
        (unsigned long)(g_textBegin-g_exeBase),(unsigned long)(g_textEnd-g_exeBase));
    Log("INIT",true,"AUTO TRACE: exact Update/Patch SanFrancisco main .sb only; MAIN EXE IAT only; no F-key required.");
    Log("INIT",true,"Each SEEK/READ dumps raw EXE return-address candidates and interesting stack strings.");

    int n=ArmHooks();
    if (n>0) Log("ARM",true,"AUTO ARMED: patched %d API imports in MAIN EXE only",n);
    else Log("ARM",true,"AUTO ARM FAILED: no matching imports were patched");

    while (WaitForSingleObject(g_stop,50)==WAIT_TIMEOUT) {
        // Passive wait. Hooks do all logging.
    }

    Log("INIT",true,"stopping");
    FlushFileBuffers(g_log);
    Real_CloseHandle(g_log);
    g_log=INVALID_HANDLE_VALUE;
    return 0;
}

} // namespace bst

BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
    using namespace bst;
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
