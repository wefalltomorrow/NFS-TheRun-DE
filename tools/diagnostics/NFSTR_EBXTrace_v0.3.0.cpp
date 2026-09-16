#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <unordered_map>
#include <cstring>

namespace ebxtrace {

static HMODULE g_self = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_stop = nullptr;
static CRITICAL_SECTION g_logLock;
static CRITICAL_SECTION g_stateLock;
static LARGE_INTEGER g_freq{};
static LARGE_INTEGER g_start{};
static volatile LONG g_serial = 0;

struct TraceState {
    uint64_t serial = 0;
    DWORD tid = 0;
    uint32_t declared = 0;
    double started = 0.0;
    std::string guid;
    std::string label;
};

static std::unordered_map<uintptr_t, TraceState> g_states;

extern "C" {
    uintptr_t g_ebxEntryContinue = 0;
    uintptr_t g_ebxFinalizeTarget = 0;
    uintptr_t g_ebxCompleteContinue = 0;
    void EbxParserEntryAsm();
    void EbxParserCompleteAsm();
    void EbxTraceEntryC(uintptr_t obj, const uint8_t* data, uint32_t len, uintptr_t caller);
    void EbxTraceCompleteC(uintptr_t obj);
}

static double NowMs() {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return (double)(q.QuadPart - g_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static void Logf(bool flush, const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(body, sizeof(body) - 1, fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = 0;

    char line[4608];
    _snprintf(line, sizeof(line) - 1, "[%011.3f] [TID %5lu] %s\r\n",
              NowMs(), (unsigned long)GetCurrentThreadId(), body);
    line[sizeof(line) - 1] = 0;

    EnterCriticalSection(&g_logLock);
    DWORD wr = 0;
    WriteFile(g_log, line, (DWORD)strlen(line), &wr, nullptr);
    if (flush) FlushFileBuffers(g_log);
    LeaveCriticalSection(&g_logLock);
}

static std::string ModuleAndRva(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((const void*)address, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
        char tmp[32];
        _snprintf(tmp, sizeof(tmp) - 1, "0x%08lX", (unsigned long)address);
        tmp[sizeof(tmp) - 1] = 0;
        return tmp;
    }
    HMODULE mod = (HMODULE)mbi.AllocationBase;
    char path[MAX_PATH]{};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    char out[MAX_PATH + 64];
    _snprintf(out, sizeof(out) - 1, "%s+0x%08lX", base,
              (unsigned long)(address - (uintptr_t)mod));
    out[sizeof(out) - 1] = 0;
    return out;
}

static bool Readable(const void* ptr, size_t len) {
    if (!ptr || !len) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
    DWORD p = mbi.Protect & 0xFFu;
    if (p == PAGE_NOACCESS) return false;
    uintptr_t a = (uintptr_t)ptr;
    uintptr_t b = a + len;
    uintptr_t e = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return b >= a && b <= e;
}

static uint32_t U32(const uint8_t* p, bool be) {
    if (be) return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static std::string Hex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789ABCDEF";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = h[p[i] >> 4];
        s[i * 2 + 1] = h[p[i] & 15];
    }
    return s;
}

static const char* Label(const uint8_t* g) {
    static const uint8_t a[16] = {0x4A,0xA6,0x8A,0x41,0x83,0x9E,0xE0,0x11,0xA1,0xCD,0xD2,0xE0,0x45,0x3A,0xD9,0x68};
    static const uint8_t b[16] = {0xD1,0xAD,0x50,0x90,0x9A,0x0C,0xE1,0x46,0x89,0x91,0xB0,0x70,0xBB,0x66,0x05,0x45};
    static const uint8_t c[16] = {0x61,0x12,0x5E,0x13,0xCF,0x05,0xAD,0x4E,0xA3,0xBB,0x2C,0xBD,0xC8,0xFC,0x0B,0xCB};
    static const uint8_t d[16] = {0xA3,0xCE,0xD7,0x1C,0x01,0xE3,0xD2,0x4F,0xA2,0xF6,0x40,0x33,0x82,0xC2,0x0B,0xC3};
    if (!memcmp(g,a,16)) return "ChallengeModeInfo_CANARY";
    if (!memcmp(g,b,16)) return "ITC_ROOT";
    if (!memcmp(g,c,16)) return "ITC_1";
    if (!memcmp(g,d,16)) return "CARFILTER_TIER5_ITC1";
    return "";
}

extern "C" void EbxTraceEntryC(uintptr_t obj, const uint8_t* data, uint32_t len, uintptr_t caller) {
    uint32_t state = 0xFFFFFFFFu;
    if (Readable((void*)(obj + 0x0C), 4))
        state = *(uint32_t*)(obj + 0x0C);

    if (state == 0 && Readable(data, 80)) {
        const bool le = data[0]==0xCE && data[1]==0xD1 && data[2]==0xB2 && data[3]==0x0F;
        const bool be = data[0]==0x0F && data[1]==0xB2 && data[2]==0xD1 && data[3]==0xCE;
        const uint64_t serial = (uint64_t)(uint32_t)InterlockedIncrement(&g_serial);
        TraceState st;
        st.serial = serial;
        st.tid = GetCurrentThreadId();
        st.started = NowMs();

        if (le || be) {
            const uint32_t strOff = U32(data+4, be);
            const uint32_t strToEof = U32(data+8, be);
            const uint32_t extGuid = U32(data+12, be);
            const uint32_t inst = U32(data+20, be);
            const uint32_t complex = U32(data+24, be);
            const uint32_t field = U32(data+28, be);
            const uint32_t nameLen = U32(data+32, be);
            const uint32_t strLen = U32(data+36, be);
            const uint32_t arrays = U32(data+40, be);
            const uint32_t payload = U32(data+44, be);
            st.declared = strOff + strToEof;
            st.guid = Hex(data+48, 16);
            st.label = Label(data+48);

            EnterCriticalSection(&g_stateLock);
            g_states[obj] = st;
            LeaveCriticalSection(&g_stateLock);

            Logf(!st.label.empty(),
                 "[EBX] #%llu BEGIN obj=0x%08lX caller=%s buf=0x%p len=%lu declared=%lu whole=%d endian=%s GUID=%s%s%s hdr{strOff=%lu strToEOF=%lu extGUID=%lu inst=%lu complex=%lu field=%lu nameLen=%lu strLen=%lu arrays=%lu payload=%lu}",
                 (unsigned long long)serial, (unsigned long)obj, ModuleAndRva(caller).c_str(),
                 data, (unsigned long)len, (unsigned long)st.declared, len >= st.declared,
                 be ? "BE" : "LE", st.guid.c_str(), st.label.empty() ? "" : " label=",
                 st.label.c_str(), (unsigned long)strOff, (unsigned long)strToEof,
                 (unsigned long)extGuid, (unsigned long)inst, (unsigned long)complex,
                 (unsigned long)field, (unsigned long)nameLen, (unsigned long)strLen,
                 (unsigned long)arrays, (unsigned long)payload);
        } else {
            EnterCriticalSection(&g_stateLock);
            g_states[obj] = st;
            LeaveCriticalSection(&g_stateLock);
            Logf(true, "[EBX] #%llu BEGIN BAD_MAGIC obj=0x%08lX caller=%s buf=0x%p len=%lu first16=%s",
                 (unsigned long long)serial, (unsigned long)obj, ModuleAndRva(caller).c_str(),
                 data, (unsigned long)len, Hex(data,16).c_str());
        }
        return;
    }

    TraceState st;
    bool found = false;
    EnterCriticalSection(&g_stateLock);
    auto it = g_states.find(obj);
    if (it != g_states.end()) { st = it->second; found = true; }
    LeaveCriticalSection(&g_stateLock);
    Logf(false, "[EBX] %s obj=0x%08lX state=%lu caller=%s buf=0x%p len=%lu%s%s",
         found ? ("#" + std::to_string((unsigned long long)st.serial) + " CONT").c_str() : "CONT",
         (unsigned long)obj, (unsigned long)state, ModuleAndRva(caller).c_str(), data,
         (unsigned long)len, found && !st.label.empty() ? " label=" : "",
         found && !st.label.empty() ? st.label.c_str() : "");
}

extern "C" void EbxTraceCompleteC(uintptr_t obj) {
    TraceState st;
    bool found = false;
    EnterCriticalSection(&g_stateLock);
    auto it = g_states.find(obj);
    if (it != g_states.end()) {
        st = it->second;
        found = true;
        g_states.erase(it);
    }
    LeaveCriticalSection(&g_stateLock);

    if (found) {
        Logf(!st.label.empty(), "[EBX] #%llu FINALIZE obj=0x%08lX declared=%lu duration=%.3fms GUID=%s%s%s",
             (unsigned long long)st.serial, (unsigned long)obj, (unsigned long)st.declared,
             NowMs() - st.started, st.guid.c_str(), st.label.empty() ? "" : " label=", st.label.c_str());
    } else {
        Logf(false, "[EBX] FINALIZE obj=0x%08lX (no tracked BEGIN)", (unsigned long)obj);
    }
}

asm(
    ".text\n"
    ".globl _EbxParserEntryAsm\n"
    "_EbxParserEntryAsm:\n"
    "    pushfl\n"
    "    pushal\n"
    "    movl 44(%esp), %eax\n"
    "    movl 40(%esp), %edx\n"
    "    movl 36(%esp), %ebx\n"
    "    movl 24(%esp), %ecx\n"
    "    pushl %ebx\n"
    "    pushl %eax\n"
    "    pushl %edx\n"
    "    pushl %ecx\n"
    "    call _EbxTraceEntryC\n"
    "    addl $16, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    subl $0x0C, %esp\n"
    "    movl 0x10(%esp), %eax\n"
    "    jmpl *_g_ebxEntryContinue\n"
    ".globl _EbxParserCompleteAsm\n"
    "_EbxParserCompleteAsm:\n"
    "    pushfl\n"
    "    pushal\n"
    "    movl 8(%esp), %eax\n"
    "    pushl %eax\n"
    "    call _EbxTraceCompleteC\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    movl %ebp, %ecx\n"
    "    call *_g_ebxFinalizeTarget\n"
    "    jmpl *_g_ebxCompleteContinue\n"
);

static bool Inject7(uintptr_t at, uintptr_t target) {
    DWORD old = 0;
    if (!VirtualProtect((void*)at, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xE9;
    *(int32_t*)(p+1) = (int32_t)(target - (at + 5));
    p[5] = 0x90; p[6] = 0x90;
    DWORD ignored = 0;
    VirtualProtect((void*)at, 7, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 7);
    return true;
}

static bool Install() {
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    uintptr_t entry = base + 0x0014A450;
    uintptr_t done  = base + 0x0014A700;
    const uint8_t a[7] = {0x83,0xEC,0x0C,0x8B,0x44,0x24,0x10};
    const uint8_t b[7] = {0x8B,0xCD,0xE8,0x59,0xF8,0xFF,0xFF};

    if (memcmp((void*)entry,a,7)) {
        Logf(true, "[INIT] ABORT entry bytes at %s are %s (expected 83EC0C8B442410)",
             ModuleAndRva(entry).c_str(), Hex((uint8_t*)entry,7).c_str());
        return false;
    }
    if (memcmp((void*)done,b,7)) {
        Logf(true, "[INIT] ABORT finalize bytes at %s are %s (expected 8BCDE859F8FFFF)",
             ModuleAndRva(done).c_str(), Hex((uint8_t*)done,7).c_str());
        return false;
    }

    g_ebxEntryContinue = entry + 7;
    g_ebxFinalizeTarget = base + 0x00149F60;
    g_ebxCompleteContinue = done + 7;
    if (!Inject7(entry,(uintptr_t)EbxParserEntryAsm)) return false;
    if (!Inject7(done,(uintptr_t)EbxParserCompleteAsm)) return false;

    Logf(true, "[INIT] hooks installed entry=%s finalize=%s helper=%s",
         ModuleAndRva(entry).c_str(), ModuleAndRva(done).c_str(),
         ModuleAndRva(g_ebxFinalizeTarget).c_str());
    return true;
}

static DWORD WINAPI Worker(LPVOID) {
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_stateLock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);

    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(g_self,self,MAX_PATH);
    wchar_t* slash = wcsrchr(self,L'\\');
    if (slash) *(slash+1)=0;
    wchar_t logPath[MAX_PATH]{};
    _snwprintf(logPath,MAX_PATH-1,L"%sNFSTR_EBXTrace.log",self);

    g_log = CreateFileW(logPath,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                        CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if (g_log==INVALID_HANDLE_VALUE) return 1;

    const unsigned char bom[3]={0xEF,0xBB,0xBF};
    DWORD wr=0; WriteFile(g_log,bom,3,&wr,nullptr);

    Logf(true,"[INIT] NFSTR EBX Trace v0.3.0 loaded PID=%lu base=0x%p",
         (unsigned long)GetCurrentProcessId(),GetModuleHandleW(nullptr));
    Logf(true,"[INIT] tracing Frostbite 2 EBX parser RVA 0x0014A450; paired FINALIZE at RVA 0x0014A700");
    Logf(true,"[INIT] known Test 11 GUID labels enabled");

    Install();

    WaitForSingleObject(g_stop,INFINITE);
    Logf(true,"[INIT] stopping");
    FlushFileBuffers(g_log);
    CloseHandle(g_log);
    g_log=INVALID_HANDLE_VALUE;
    return 0;
}

} // namespace ebxtrace

BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
    using namespace ebxtrace;
    if (reason==DLL_PROCESS_ATTACH) {
        g_self=(HMODULE)h;
        DisableThreadLibraryCalls(h);
        g_stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE th=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
        if (th) CloseHandle(th);
    } else if (reason==DLL_PROCESS_DETACH) {
        if (g_stop) SetEvent(g_stop);
    }
    return TRUE;
}
