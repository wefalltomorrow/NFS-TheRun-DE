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

// NFSTR_Diagnostics.asi v0.2.0
// Standalone x86 diagnostic ASI for Need for Speed: The Run v1.1.0.0.
// Observes Win32 APIs through IAT hooks. It intentionally does not alter game/Frostbite data.

namespace diag {

static HMODULE g_self = nullptr;
static HANDLE  g_log = INVALID_HANDLE_VALUE;
static HANDLE  g_stopEvent = nullptr;
static CRITICAL_SECTION g_logLock;
static CRITICAL_SECTION g_stateLock;
static LARGE_INTEGER g_qpcFreq{};
static LARGE_INTEGER g_qpcStart{};
static wchar_t g_modulePath[MAX_PATH]{};
static wchar_t g_moduleDir[MAX_PATH]{};
static wchar_t g_iniPath[MAX_PATH]{};
static wchar_t g_logPath[MAX_PATH]{};
static wchar_t g_gameDir[MAX_PATH]{};
static volatile LONG g_callId = 0;
static volatile LONG g_logStopped = 0;
static uint64_t g_logBytes = 0;

struct Config {
    bool enabled = true;
    bool verbose = true;
    bool onlyGameDirectory = false;
    bool logCreates = true;
    bool logReads = true;
    bool logSeeks = true;
    bool logSizes = true;
    bool logMappings = true;
    bool logAttributes = true;
    bool logModules = true;
    bool logThreads = true;
    bool logWaits = true;
    bool logExceptions = true;
    bool logCloseHandle = true;
    bool logSyncObjects = true;
    bool logCallsites = true;
    bool threadSnapshots = true;
    bool flushImportant = true;
    DWORD minWaitMs = 25;
    DWORD moduleScanMs = 250;
    DWORD snapshotAfterMs = 1500;
    DWORD snapshotIntervalMs = 2000;
    DWORD snapshotStackDepth = 8;
    DWORD maxLogMB = 256;
} g_cfg;

struct FileState {
    std::wstring path;
    uint64_t pos = 0;
    bool posKnown = false;
    bool important = false;
};

struct MappingState {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
    bool important = false;
};

struct HandleState {
    std::string kind;
    std::wstring name;
    std::string creator;
};

static std::unordered_map<uintptr_t, FileState> g_files;
static std::unordered_map<uintptr_t, MappingState> g_mappings;
static std::unordered_map<uintptr_t, HandleState> g_handles;
static std::unordered_set<uintptr_t> g_patchedModules;

static const uint64_t kFrontEndDeltaOffset = 950432ULL; // 0x000E80A0
static const uint64_t kFrontEndDeltaSize   = 500944ULL; // 0x0007A4D0
static const uint64_t kFrontEndDeltaEnd    = kFrontEndDeltaOffset + kFrontEndDeltaSize;

static std::string WideToUtf8(const wchar_t* s) {
    if (!s || !*s) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr)) return std::string();
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

static std::wstring AnsiToWide(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, s, -1, &out[0], n)) return std::wstring();
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

static void CopyDirOfPath(const wchar_t* path, wchar_t* out, size_t outCount) {
    if (!path || !out || outCount == 0) return;
    wcsncpy(out, path, outCount - 1);
    out[outCount - 1] = 0;
    wchar_t* slash1 = wcsrchr(out, L'\\');
    wchar_t* slash2 = wcsrchr(out, L'/');
    wchar_t* slash = (slash1 && slash2) ? (slash1 > slash2 ? slash1 : slash2) : (slash1 ? slash1 : slash2);
    if (slash) *slash = 0;
}

static bool StartsWithI(const std::wstring& value, const std::wstring& prefix) {
    if (prefix.empty() || value.size() < prefix.size()) return false;
    return _wcsnicmp(value.c_str(), prefix.c_str(), prefix.size()) == 0;
}

static bool ContainsI(const std::wstring& value, const wchar_t* needle) {
    if (!needle || !*needle) return false;
    std::wstring a = value;
    std::wstring b = needle;
    auto lower = [](wchar_t c) -> wchar_t { return (wchar_t)towlower((wint_t)c); };
    std::transform(a.begin(), a.end(), a.begin(), lower);
    std::transform(b.begin(), b.end(), b.begin(), lower);
    return a.find(b) != std::wstring::npos;
}

static bool IsFrontEndPath(const std::wstring& path) {
    return ContainsI(path, L"\\Levels\\FE\\FrontEnd\\FrontEnd.sb") ||
           ContainsI(path, L"\\Levels\\FE\\FrontEnd\\FrontEnd.toc") ||
           ContainsI(path, L"/Levels/FE/FrontEnd/FrontEnd.sb") ||
           ContainsI(path, L"/Levels/FE/FrontEnd/FrontEnd.toc");
}

static bool IsGamePath(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring game = g_gameDir;
    if (StartsWithI(path, game)) return true;
    if (path.find(L":\\") == std::wstring::npos && path.find(L":/") == std::wstring::npos) return true;
    return false;
}

static bool RangeOverlapsFrontEndDelta(uint64_t off, uint64_t size) {
    if (size == 0) return off >= kFrontEndDeltaOffset && off < kFrontEndDeltaEnd;
    uint64_t end = off + size;
    if (end < off) end = UINT64_MAX;
    return off < kFrontEndDeltaEnd && end > kFrontEndDeltaOffset;
}

static double NowMs() {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return (double)(q.QuadPart - g_qpcStart.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

static uint64_t NextCallId() {
    return (uint64_t)(uint32_t)InterlockedIncrement(&g_callId);
}

static void RawWrite(const char* data, size_t len, bool flush) {
    if (g_log == INVALID_HANDLE_VALUE || !data || !len || InterlockedCompareExchange(&g_logStopped, 0, 0)) return;
    EnterCriticalSection(&g_logLock);
    if (!InterlockedCompareExchange(&g_logStopped, 0, 0)) {
        const uint64_t maxBytes = (uint64_t)g_cfg.maxLogMB * 1024ULL * 1024ULL;
        if (maxBytes && g_logBytes + len > maxBytes) {
            const char* msg = "\r\n[LOGGER] Maximum log size reached; further logging disabled.\r\n";
            DWORD wr = 0;
            WriteFile(g_log, msg, (DWORD)strlen(msg), &wr, nullptr);
            FlushFileBuffers(g_log);
            InterlockedExchange(&g_logStopped, 1);
        } else {
            DWORD wr = 0;
            WriteFile(g_log, data, (DWORD)len, &wr, nullptr);
            g_logBytes += wr;
            if (flush) FlushFileBuffers(g_log);
        }
    }
    LeaveCriticalSection(&g_logLock);
}

static void Logf(bool important, const char* category, const char* fmt, ...) {
    if (!g_cfg.enabled || g_log == INVALID_HANDLE_VALUE) return;
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(body, sizeof(body) - 1, fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = 0;
    char line[4608];
    const double ms = NowMs();
    const DWORD tid = GetCurrentThreadId();
    _snprintf(line, sizeof(line) - 1, "[%011.3f] [TID %5lu] [%s]%s %s\r\n",
              ms, (unsigned long)tid, category ? category : "GEN", important ? " [IMPORTANT]" : "", body);
    line[sizeof(line) - 1] = 0;
    RawWrite(line, strlen(line), important && g_cfg.flushImportant);
}

static int ReadInt(const wchar_t* section, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(section, key, def, g_iniPath);
}

static void LoadConfig() {
    g_cfg.enabled           = ReadInt(L"Diagnostics", L"Enabled", 1) != 0;
    g_cfg.verbose           = ReadInt(L"Diagnostics", L"Verbose", 1) != 0;
    g_cfg.onlyGameDirectory = ReadInt(L"Diagnostics", L"OnlyGameDirectory", 0) != 0;
    g_cfg.maxLogMB          = (DWORD)ReadInt(L"Diagnostics", L"MaxLogMB", 256);
    g_cfg.logCreates        = ReadInt(L"FileIO", L"LogCreates", 1) != 0;
    g_cfg.logReads          = ReadInt(L"FileIO", L"LogReads", 1) != 0;
    g_cfg.logSeeks          = ReadInt(L"FileIO", L"LogSeeks", 1) != 0;
    g_cfg.logSizes          = ReadInt(L"FileIO", L"LogSizes", 1) != 0;
    g_cfg.logMappings       = ReadInt(L"FileIO", L"LogMappings", 1) != 0;
    g_cfg.logAttributes     = ReadInt(L"FileIO", L"LogAttributes", 1) != 0;
    g_cfg.logCloseHandle    = ReadInt(L"FileIO", L"LogCloseHandle", 1) != 0;
    g_cfg.logModules        = ReadInt(L"Runtime", L"LogModules", 1) != 0;
    g_cfg.logThreads        = ReadInt(L"Runtime", L"LogThreads", 1) != 0;
    g_cfg.logWaits          = ReadInt(L"Runtime", L"LogWaits", 1) != 0;
    g_cfg.logExceptions     = ReadInt(L"Runtime", L"LogExceptions", 1) != 0;
    g_cfg.logSyncObjects    = ReadInt(L"Runtime", L"LogSyncObjects", 1) != 0;
    g_cfg.logCallsites      = ReadInt(L"Runtime", L"LogCallsites", 1) != 0;
    g_cfg.threadSnapshots   = ReadInt(L"Runtime", L"ThreadSnapshots", 1) != 0;
    g_cfg.minWaitMs         = (DWORD)ReadInt(L"Runtime", L"MinWaitMs", 25);
    g_cfg.moduleScanMs      = (DWORD)ReadInt(L"Runtime", L"ModuleScanMs", 250);
    g_cfg.snapshotAfterMs   = (DWORD)ReadInt(L"Runtime", L"ThreadSnapshotAfterMs", 1500);
    g_cfg.snapshotIntervalMs= (DWORD)ReadInt(L"Runtime", L"ThreadSnapshotIntervalMs", 2000);
    g_cfg.snapshotStackDepth= (DWORD)ReadInt(L"Runtime", L"ThreadSnapshotStackDepth", 8);
    if (g_cfg.moduleScanMs < 50) g_cfg.moduleScanMs = 50;
    if (g_cfg.snapshotIntervalMs < 250) g_cfg.snapshotIntervalMs = 250;
    if (g_cfg.snapshotStackDepth < 1) g_cfg.snapshotStackDepth = 1;
    if (g_cfg.snapshotStackDepth > 16) g_cfg.snapshotStackDepth = 16;
    g_cfg.flushImportant    = ReadInt(L"Diagnostics", L"FlushImportant", 1) != 0;
}

static bool ShouldLogPath(const std::wstring& path) {
    if (!g_cfg.onlyGameDirectory) return true;
    return IsGamePath(path);
}

static FileState GetFileState(HANDLE h) {
    FileState out;
    EnterCriticalSection(&g_stateLock);
    auto it = g_files.find((uintptr_t)h);
    if (it != g_files.end()) out = it->second;
    LeaveCriticalSection(&g_stateLock);
    return out;
}

static void PutFileState(HANDLE h, const FileState& st) {
    if (!h || h == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_stateLock);
    g_files[(uintptr_t)h] = st;
    LeaveCriticalSection(&g_stateLock);
}

static void UpdateFilePos(HANDLE h, uint64_t pos, bool known) {
    EnterCriticalSection(&g_stateLock);
    auto it = g_files.find((uintptr_t)h);
    if (it != g_files.end()) { it->second.pos = pos; it->second.posKnown = known; }
    LeaveCriticalSection(&g_stateLock);
}

static void AdvanceFilePos(HANDLE h, uint64_t count) {
    EnterCriticalSection(&g_stateLock);
    auto it = g_files.find((uintptr_t)h);
    if (it != g_files.end() && it->second.posKnown) it->second.pos += count;
    LeaveCriticalSection(&g_stateLock);
}

static MappingState GetMappingState(HANDLE h) {
    MappingState out;
    EnterCriticalSection(&g_stateLock);
    auto it = g_mappings.find((uintptr_t)h);
    if (it != g_mappings.end()) out = it->second;
    LeaveCriticalSection(&g_stateLock);
    return out;
}

static std::string ModuleAndRva(void* address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi))) return "<unknown>";
    HMODULE mod = (HMODULE)mbi.AllocationBase;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) return "<unknown>";
    std::string p = WideToUtf8(path);
    size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos) p = p.substr(slash + 1);
    uintptr_t rva = (uintptr_t)address - (uintptr_t)mod;
    char out[512];
    _snprintf(out, sizeof(out), "%s+0x%08lX", p.c_str(), (unsigned long)rva);
    return out;
}

static void PutHandleState(HANDLE h, const char* kind, const std::wstring& name, void* caller) {
    if (!h || h == INVALID_HANDLE_VALUE) return;
    HandleState st; st.kind = kind ? kind : "handle"; st.name = name; st.creator = ModuleAndRva(caller);
    EnterCriticalSection(&g_stateLock); g_handles[(uintptr_t)h] = st; LeaveCriticalSection(&g_stateLock);
}

static HandleState GetHandleState(HANDLE h) {
    HandleState out;
    EnterCriticalSection(&g_stateLock);
    auto it = g_handles.find((uintptr_t)h);
    if (it != g_handles.end()) out = it->second;
    LeaveCriticalSection(&g_stateLock);
    return out;
}

static std::string DescribeHandle(HANDLE h) {
    FileState fs = GetFileState(h);
    if (!fs.path.empty()) return std::string("file:") + WideToUtf8(fs.path.c_str());
    MappingState ms = GetMappingState(h);
    if (!ms.path.empty()) return std::string("mapping:") + WideToUtf8(ms.path.c_str());
    HandleState hs = GetHandleState(h);
    if (!hs.kind.empty()) {
        std::string out = hs.kind;
        if (!hs.name.empty()) out += std::string(":") + WideToUtf8(hs.name.c_str());
        if (!hs.creator.empty()) out += std::string(" created@") + hs.creator;
        return out;
    }
    return "<untracked>";
}

#if defined(__GNUC__)
#define DIAG_CALLER() __builtin_return_address(0)
#else
#define DIAG_CALLER() nullptr
#endif

using PFN_CreateFileW = HANDLE (WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_CreateFileA = HANDLE (WINAPI*)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using PFN_ReadFile = BOOL (WINAPI*)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
using PFN_SetFilePointer = DWORD (WINAPI*)(HANDLE,LONG,PLONG,DWORD);
using PFN_SetFilePointerEx = BOOL (WINAPI*)(HANDLE,LARGE_INTEGER,PLARGE_INTEGER,DWORD);
using PFN_GetFileSize = DWORD (WINAPI*)(HANDLE,LPDWORD);
using PFN_GetFileSizeEx = BOOL (WINAPI*)(HANDLE,PLARGE_INTEGER);
using PFN_CreateFileMappingW = HANDLE (WINAPI*)(HANDLE,LPSECURITY_ATTRIBUTES,DWORD,DWORD,DWORD,LPCWSTR);
using PFN_CreateFileMappingA = HANDLE (WINAPI*)(HANDLE,LPSECURITY_ATTRIBUTES,DWORD,DWORD,DWORD,LPCSTR);
using PFN_MapViewOfFile = LPVOID (WINAPI*)(HANDLE,DWORD,DWORD,DWORD,SIZE_T);
using PFN_MapViewOfFileEx = LPVOID (WINAPI*)(HANDLE,DWORD,DWORD,DWORD,SIZE_T,LPVOID);
using PFN_UnmapViewOfFile = BOOL (WINAPI*)(LPCVOID);
using PFN_CloseHandle = BOOL (WINAPI*)(HANDLE);
using PFN_GetFileAttributesW = DWORD (WINAPI*)(LPCWSTR);
using PFN_GetFileAttributesA = DWORD (WINAPI*)(LPCSTR);
using PFN_GetFileAttributesExW = BOOL (WINAPI*)(LPCWSTR,GET_FILEEX_INFO_LEVELS,LPVOID);
using PFN_GetFileAttributesExA = BOOL (WINAPI*)(LPCSTR,GET_FILEEX_INFO_LEVELS,LPVOID);
using PFN_LoadLibraryW = HMODULE (WINAPI*)(LPCWSTR);
using PFN_LoadLibraryA = HMODULE (WINAPI*)(LPCSTR);
using PFN_LoadLibraryExW = HMODULE (WINAPI*)(LPCWSTR,HANDLE,DWORD);
using PFN_LoadLibraryExA = HMODULE (WINAPI*)(LPCSTR,HANDLE,DWORD);
using PFN_CreateThread = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES,SIZE_T,LPTHREAD_START_ROUTINE,LPVOID,DWORD,LPDWORD);
using PFN_WaitForSingleObject = DWORD (WINAPI*)(HANDLE,DWORD);
using PFN_WaitForMultipleObjects = DWORD (WINAPI*)(DWORD,const HANDLE*,BOOL,DWORD);
using PFN_WaitForSingleObjectEx = DWORD (WINAPI*)(HANDLE,DWORD,BOOL);
using PFN_WaitForMultipleObjectsEx = DWORD (WINAPI*)(DWORD,const HANDLE*,BOOL,DWORD,BOOL);
using PFN_SignalObjectAndWait = DWORD (WINAPI*)(HANDLE,HANDLE,DWORD,BOOL);
using PFN_CreateEventW = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES,BOOL,BOOL,LPCWSTR);
using PFN_CreateEventA = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES,BOOL,BOOL,LPCSTR);
using PFN_SetEvent = BOOL (WINAPI*)(HANDLE);
using PFN_ResetEvent = BOOL (WINAPI*)(HANDLE);
using PFN_PulseEvent = BOOL (WINAPI*)(HANDLE);
using PFN_CreateSemaphoreW = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES,LONG,LONG,LPCWSTR);
using PFN_CreateSemaphoreA = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES,LONG,LONG,LPCSTR);
using PFN_ReleaseSemaphore = BOOL (WINAPI*)(HANDLE,LONG,LPLONG);
using PFN_CreateMutexW = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES,BOOL,LPCWSTR);
using PFN_CreateMutexA = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES,BOOL,LPCSTR);
using PFN_ReleaseMutex = BOOL (WINAPI*)(HANDLE);
using PFN_Sleep = VOID (WINAPI*)(DWORD);
using PFN_SleepEx = DWORD (WINAPI*)(DWORD,BOOL);
using PFN_ExitThread = VOID (WINAPI*)(DWORD);

static PFN_CreateFileW Real_CreateFileW = ::CreateFileW;
static PFN_CreateFileA Real_CreateFileA = ::CreateFileA;
static PFN_ReadFile Real_ReadFile = ::ReadFile;
static PFN_SetFilePointer Real_SetFilePointer = ::SetFilePointer;
static PFN_SetFilePointerEx Real_SetFilePointerEx = ::SetFilePointerEx;
static PFN_GetFileSize Real_GetFileSize = ::GetFileSize;
static PFN_GetFileSizeEx Real_GetFileSizeEx = ::GetFileSizeEx;
static PFN_CreateFileMappingW Real_CreateFileMappingW = ::CreateFileMappingW;
static PFN_CreateFileMappingA Real_CreateFileMappingA = ::CreateFileMappingA;
static PFN_MapViewOfFile Real_MapViewOfFile = ::MapViewOfFile;
static PFN_MapViewOfFileEx Real_MapViewOfFileEx = ::MapViewOfFileEx;
static PFN_UnmapViewOfFile Real_UnmapViewOfFile = ::UnmapViewOfFile;
static PFN_CloseHandle Real_CloseHandle = ::CloseHandle;
static PFN_GetFileAttributesW Real_GetFileAttributesW = ::GetFileAttributesW;
static PFN_GetFileAttributesA Real_GetFileAttributesA = ::GetFileAttributesA;
static PFN_GetFileAttributesExW Real_GetFileAttributesExW = ::GetFileAttributesExW;
static PFN_GetFileAttributesExA Real_GetFileAttributesExA = ::GetFileAttributesExA;
static PFN_LoadLibraryW Real_LoadLibraryW = ::LoadLibraryW;
static PFN_LoadLibraryA Real_LoadLibraryA = ::LoadLibraryA;
static PFN_LoadLibraryExW Real_LoadLibraryExW = ::LoadLibraryExW;
static PFN_LoadLibraryExA Real_LoadLibraryExA = ::LoadLibraryExA;
static PFN_CreateThread Real_CreateThread = ::CreateThread;
static PFN_WaitForSingleObject Real_WaitForSingleObject = ::WaitForSingleObject;
static PFN_WaitForMultipleObjects Real_WaitForMultipleObjects = ::WaitForMultipleObjects;
static PFN_WaitForSingleObjectEx Real_WaitForSingleObjectEx = ::WaitForSingleObjectEx;
static PFN_WaitForMultipleObjectsEx Real_WaitForMultipleObjectsEx = ::WaitForMultipleObjectsEx;
static PFN_SignalObjectAndWait Real_SignalObjectAndWait = ::SignalObjectAndWait;
static PFN_CreateEventW Real_CreateEventW = ::CreateEventW;
static PFN_CreateEventA Real_CreateEventA = ::CreateEventA;
static PFN_SetEvent Real_SetEvent = ::SetEvent;
static PFN_ResetEvent Real_ResetEvent = ::ResetEvent;
static PFN_PulseEvent Real_PulseEvent = ::PulseEvent;
static PFN_CreateSemaphoreW Real_CreateSemaphoreW = ::CreateSemaphoreW;
static PFN_CreateSemaphoreA Real_CreateSemaphoreA = ::CreateSemaphoreA;
static PFN_ReleaseSemaphore Real_ReleaseSemaphore = ::ReleaseSemaphore;
static PFN_CreateMutexW Real_CreateMutexW = ::CreateMutexW;
static PFN_CreateMutexA Real_CreateMutexA = ::CreateMutexA;
static PFN_ReleaseMutex Real_ReleaseMutex = ::ReleaseMutex;
static PFN_Sleep Real_Sleep = ::Sleep;
static PFN_SleepEx Real_SleepEx = ::SleepEx;
static PFN_ExitThread Real_ExitThread = ::ExitThread;

/* File I/O hooks: unchanged v0.1 behavior, omitted here for brevity in comments only. */

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags, HANDLE templ) {
    std::wstring path = name ? name : L"<null>";
    bool important = IsFrontEndPath(path), show = g_cfg.logCreates && ShouldLogPath(path);
    uint64_t id = NextCallId(); double t0 = NowMs();
    if (show) Logf(important,"FILE","#%llu BEGIN CreateFileW path=\"%s\" access=0x%08lX share=0x%08lX creation=%lu flags=0x%08lX",(unsigned long long)id,WideToUtf8(path.c_str()).c_str(),(unsigned long)access,(unsigned long)share,(unsigned long)creation,(unsigned long)flags);
    HANDLE h=Real_CreateFileW(name,access,share,sa,creation,flags,templ); DWORD err=GetLastError();
    if(h!=INVALID_HANDLE_VALUE){ FileState st; st.path=path; st.important=important; st.pos=0; st.posKnown=true; PutFileState(h,st); }
    if(show) Logf(important,"FILE","#%llu END   CreateFileW handle=0x%p result=%s error=%lu duration=%.3fms",(unsigned long long)id,h,h==INVALID_HANDLE_VALUE?"FAIL":"OK",(unsigned long)err,NowMs()-t0);
    SetLastError(err); return h;
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags, HANDLE templ) {
    std::wstring path=AnsiToWide(name?name:"<null>"); bool important=IsFrontEndPath(path), show=g_cfg.logCreates&&ShouldLogPath(path); uint64_t id=NextCallId(); double t0=NowMs();
    if(show) Logf(important,"FILE","#%llu BEGIN CreateFileA path=\"%s\" access=0x%08lX share=0x%08lX creation=%lu flags=0x%08lX",(unsigned long long)id,name?name:"<null>",(unsigned long)access,(unsigned long)share,(unsigned long)creation,(unsigned long)flags);
    HANDLE h=Real_CreateFileA(name,access,share,sa,creation,flags,templ); DWORD err=GetLastError();
    if(h!=INVALID_HANDLE_VALUE){ FileState st; st.path=path; st.important=important; st.pos=0; st.posKnown=true; PutFileState(h,st); }
    if(show) Logf(important,"FILE","#%llu END   CreateFileA handle=0x%p result=%s error=%lu duration=%.3fms",(unsigned long long)id,h,h==INVALID_HANDLE_VALUE?"FAIL":"OK",(unsigned long)err,NowMs()-t0);
    SetLastError(err); return h;
}

static BOOL WINAPI Hook_ReadFile(HANDLE h,LPVOID b,DWORD req,LPDWORD got,LPOVERLAPPED ov){ FileState fs=GetFileState(h); uint64_t off=fs.posKnown?fs.pos:(ov?(((uint64_t)ov->OffsetHigh<<32)|ov->Offset):0); bool show=g_cfg.logReads&&(!fs.path.empty()?ShouldLogPath(fs.path):!g_cfg.onlyGameDirectory); bool delta=fs.important&&RangeOverlapsFrontEndDelta(off,req); uint64_t id=NextCallId(); double t0=NowMs(); if(show)Logf(fs.important||delta,"READ","#%llu BEGIN ReadFile h=0x%p path=\"%s\" offset=%llu (0x%llX) request=%lu%s",(unsigned long long)id,h,WideToUtf8(fs.path.c_str()).c_str(),(unsigned long long)off,(unsigned long long)off,(unsigned long)req,delta?" [FRONTEND-DELTA-OVERLAP]":""); BOOL r=Real_ReadFile(h,b,req,got,ov); DWORD err=GetLastError(); DWORD n=got?*got:0; if(r&&!ov)AdvanceFilePos(h,n); if(show)Logf(fs.important||delta,"READ","#%llu END   ReadFile result=%s bytes=%lu error=%lu duration=%.3fms",(unsigned long long)id,r?"OK":"FAIL",(unsigned long)n,(unsigned long)err,NowMs()-t0); SetLastError(err); return r; }

static BOOL WINAPI Hook_SetFilePointerEx(HANDLE h,LARGE_INTEGER move,PLARGE_INTEGER newp,DWORD method){ FileState fs=GetFileState(h); bool show=g_cfg.logSeeks&&(!fs.path.empty()?ShouldLogPath(fs.path):!g_cfg.onlyGameDirectory); uint64_t id=NextCallId(); double t0=NowMs(); if(show)Logf(fs.important,"SEEK","#%llu BEGIN SetFilePointerEx h=0x%p path=\"%s\" move=%lld method=%lu",(unsigned long long)id,h,WideToUtf8(fs.path.c_str()).c_str(),(long long)move.QuadPart,(unsigned long)method); LARGE_INTEGER tmp{}; BOOL r=Real_SetFilePointerEx(h,move,newp?newp:&tmp,method); DWORD err=GetLastError(); LARGE_INTEGER p=newp?*newp:tmp; if(r)UpdateFilePos(h,(uint64_t)p.QuadPart,true); bool delta=fs.important&&p.QuadPart>=0&&(uint64_t)p.QuadPart>=kFrontEndDeltaOffset&&(uint64_t)p.QuadPart<kFrontEndDeltaEnd; if(show)Logf(fs.important||delta,"SEEK","#%llu END   SetFilePointerEx result=%s new=%lld (0x%llX)%s error=%lu duration=%.3fms",(unsigned long long)id,r?"OK":"FAIL",(long long)p.QuadPart,(unsigned long long)p.QuadPart,delta?" [FRONTEND-DELTA]":"",(unsigned long)err,NowMs()-t0); SetLastError(err); return r; }

static DWORD WINAPI Hook_SetFilePointer(HANDLE h,LONG low,PLONG high,DWORD method){ FileState fs=GetFileState(h); bool show=g_cfg.logSeeks&&(!fs.path.empty()?ShouldLogPath(fs.path):!g_cfg.onlyGameDirectory); uint64_t id=NextCallId(); double t0=NowMs(); if(show)Logf(fs.important,"SEEK","#%llu BEGIN SetFilePointer h=0x%p path=\"%s\" low=%ld method=%lu",(unsigned long long)id,h,WideToUtf8(fs.path.c_str()).c_str(),(long)low,(unsigned long)method); DWORD r=Real_SetFilePointer(h,low,high,method); DWORD err=GetLastError(); if(r!=INVALID_SET_FILE_POINTER||err==NO_ERROR){ uint64_t p=((uint64_t)(high?(uint32_t)*high:0)<<32)|r; UpdateFilePos(h,p,true); } if(show)Logf(fs.important,"SEEK","#%llu END   SetFilePointer result=0x%08lX error=%lu duration=%.3fms",(unsigned long long)id,(unsigned long)r,(unsigned long)err,NowMs()-t0); SetLastError(err); return r; }

static BOOL WINAPI Hook_GetFileSizeEx(HANDLE h,PLARGE_INTEGER size){ FileState fs=GetFileState(h); double t0=NowMs(); BOOL r=Real_GetFileSizeEx(h,size); DWORD err=GetLastError(); if(g_cfg.logSizes&&(!fs.path.empty()?ShouldLogPath(fs.path):!g_cfg.onlyGameDirectory))Logf(fs.important,"SIZE","GetFileSizeEx h=0x%p path=\"%s\" result=%s size=%lld error=%lu duration=%.3fms",h,WideToUtf8(fs.path.c_str()).c_str(),r?"OK":"FAIL",size?(long long)size->QuadPart:-1LL,(unsigned long)err,NowMs()-t0); SetLastError(err); return r; }
static DWORD WINAPI Hook_GetFileSize(HANDLE h,LPDWORD high){ FileState fs=GetFileState(h); double t0=NowMs(); DWORD r=Real_GetFileSize(h,high); DWORD err=GetLastError(); if(g_cfg.logSizes&&(!fs.path.empty()?ShouldLogPath(fs.path):!g_cfg.onlyGameDirectory))Logf(fs.important,"SIZE","GetFileSize h=0x%p path=\"%s\" low=%lu high=%lu error=%lu duration=%.3fms",h,WideToUtf8(fs.path.c_str()).c_str(),(unsigned long)r,high?(unsigned long)*high:0,(unsigned long)err,NowMs()-t0); SetLastError(err); return r; }

static HANDLE WINAPI Hook_CreateFileMappingW(HANDLE f,LPSECURITY_ATTRIBUTES sa,DWORD prot,DWORD hi,DWORD lo,LPCWSTR name){ FileState fs=GetFileState(f); bool show=g_cfg.logMappings&&(!fs.path.empty()?ShouldLogPath(fs.path):!g_cfg.onlyGameDirectory); uint64_t id=NextCallId(); double t0=NowMs(); if(show)Logf(fs.important,"MAP","#%llu BEGIN CreateFileMappingW file=0x%p path=\"%s\" protect=0x%08lX maxSize=%llu name=\"%s\"",(unsigned long long)id,f,WideToUtf8(fs.path.c_str()).c_str(),(unsigned long)prot,(unsigned long long)(((uint64_t)hi<<32)|lo),WideToUtf8(name?name:L"").c_str()); HANDLE m=Real_CreateFileMappingW(f,sa,prot,hi,lo,name); DWORD err=GetLastError(); if(m){ MappingState ms; ms.file=f; ms.path=fs.path; ms.important=fs.important; EnterCriticalSection(&g_stateLock); g_mappings[(uintptr_t)m]=ms; LeaveCriticalSection(&g_stateLock);} if(show)Logf(fs.important,"MAP","#%llu END   CreateFileMappingW mapping=0x%p result=%s error=%lu duration=%.3fms",(unsigned long long)id,m,m?"OK":"FAIL",(unsigned long)err,NowMs()-t0); SetLastError(err); return m; }
static HANDLE WINAPI Hook_CreateFileMappingA(HANDLE f,LPSECURITY_ATTRIBUTES sa,DWORD prot,DWORD hi,DWORD lo,LPCSTR name){ return Hook_CreateFileMappingW(f,sa,prot,hi,lo,nullptr); }
static LPVOID WINAPI Hook_MapViewOfFile(HANDLE m,DWORD access,DWORD hi,DWORD lo,SIZE_T size){ MappingState ms=GetMappingState(m); uint64_t off=((uint64_t)hi<<32)|lo; bool show=g_cfg.logMappings&&(ms.path.empty()?!g_cfg.onlyGameDirectory:ShouldLogPath(ms.path)); bool delta=ms.important&&RangeOverlapsFrontEndDelta(off,size); uint64_t id=NextCallId(); double t0=NowMs(); if(show)Logf(ms.important||delta,"MAP","#%llu BEGIN MapViewOfFile mapping=0x%p path=\"%s\" access=0x%08lX offset=%llu (0x%llX) size=%llu%s",(unsigned long long)id,m,WideToUtf8(ms.path.c_str()).c_str(),(unsigned long)access,(unsigned long long)off,(unsigned long long)off,(unsigned long long)size,delta?" [FRONTEND-DELTA-OVERLAP]":""); LPVOID p=Real_MapViewOfFile(m,access,hi,lo,size); DWORD err=GetLastError(); if(show)Logf(ms.important||delta,"MAP","#%llu END   MapViewOfFile ptr=0x%p result=%s error=%lu duration=%.3fms",(unsigned long long)id,p,p?"OK":"FAIL",(unsigned long)err,NowMs()-t0); SetLastError(err); return p; }
static LPVOID WINAPI Hook_MapViewOfFileEx(HANDLE m,DWORD access,DWORD hi,DWORD lo,SIZE_T size,LPVOID base){ MappingState ms=GetMappingState(m); uint64_t off=((uint64_t)hi<<32)|lo; bool show=g_cfg.logMappings&&(ms.path.empty()?!g_cfg.onlyGameDirectory:ShouldLogPath(ms.path)); bool delta=ms.important&&RangeOverlapsFrontEndDelta(off,size); uint64_t id=NextCallId(); double t0=NowMs(); if(show)Logf(ms.important||delta,"MAP","#%llu BEGIN MapViewOfFileEx mapping=0x%p path=\"%s\" access=0x%08lX offset=%llu (0x%llX) size=%llu base=0x%p%s",(unsigned long long)id,m,WideToUtf8(ms.path.c_str()).c_str(),(unsigned long)access,(unsigned long long)off,(unsigned long long)off,(unsigned long long)size,base,delta?" [FRONTEND-DELTA-OVERLAP]":""); LPVOID p=Real_MapViewOfFileEx(m,access,hi,lo,size,base); DWORD err=GetLastError(); if(show)Logf(ms.important||delta,"MAP","#%llu END   MapViewOfFileEx ptr=0x%p result=%s error=%lu duration=%.3fms",(unsigned long long)id,p,p?"OK":"FAIL",(unsigned long)err,NowMs()-t0); SetLastError(err); return p; }
static BOOL WINAPI Hook_UnmapViewOfFile(LPCVOID base){ uint64_t id=NextCallId(); double t0=NowMs(); if(g_cfg.logMappings&&g_cfg.verbose)Logf(false,"MAP","#%llu BEGIN UnmapViewOfFile base=0x%p",(unsigned long long)id,base); BOOL ok=Real_UnmapViewOfFile(base); DWORD err=GetLastError(); if(g_cfg.logMappings&&g_cfg.verbose)Logf(false,"MAP","#%llu END   UnmapViewOfFile result=%s error=%lu duration=%.3fms",(unsigned long long)id,ok?"OK":"FAIL",(unsigned long)err,NowMs()-t0); SetLastError(err); return ok; }

static BOOL WINAPI Hook_CloseHandle(HANDLE h){ FileState fs=GetFileState(h); MappingState ms=GetMappingState(h); HandleState hs=GetHandleState(h); void* caller=DIAG_CALLER(); if(g_cfg.logCloseHandle&&(!fs.path.empty()||!ms.path.empty()||!hs.kind.empty()))Logf(fs.important||ms.important,"HANDLE","CloseHandle h=0x%p type=\"%s\" caller=%s",h,DescribeHandle(h).c_str(),ModuleAndRva(caller).c_str()); EnterCriticalSection(&g_stateLock); g_files.erase((uintptr_t)h); g_mappings.erase((uintptr_t)h); g_handles.erase((uintptr_t)h); LeaveCriticalSection(&g_stateLock); BOOL ok=Real_CloseHandle(h); DWORD err=GetLastError(); SetLastError(err); return ok; }

static DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR n){ std::wstring p=n?n:L"<null>"; bool imp=IsFrontEndPath(p),show=g_cfg.logAttributes&&ShouldLogPath(p); uint64_t id=NextCallId(); double t0=NowMs(); DWORD r=Real_GetFileAttributesW(n); DWORD e=GetLastError(); if(show)Logf(imp,"ATTR","#%llu GetFileAttributesW path=\"%s\" attr=0x%08lX error=%lu duration=%.3fms",(unsigned long long)id,WideToUtf8(p.c_str()).c_str(),(unsigned long)r,(unsigned long)e,NowMs()-t0); SetLastError(e); return r; }
static DWORD WINAPI Hook_GetFileAttributesA(LPCSTR n){ std::wstring p=AnsiToWide(n?n:"<null>"); bool imp=IsFrontEndPath(p),show=g_cfg.logAttributes&&ShouldLogPath(p); uint64_t id=NextCallId(); double t0=NowMs(); DWORD r=Real_GetFileAttributesA(n); DWORD e=GetLastError(); if(show)Logf(imp,"ATTR","#%llu GetFileAttributesA path=\"%s\" attr=0x%08lX error=%lu duration=%.3fms",(unsigned long long)id,n?n:"<null>",(unsigned long)r,(unsigned long)e,NowMs()-t0); SetLastError(e); return r; }
static BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR n,GET_FILEEX_INFO_LEVELS l,LPVOID i){ std::wstring p=n?n:L"<null>"; bool imp=IsFrontEndPath(p),show=g_cfg.logAttributes&&ShouldLogPath(p); uint64_t id=NextCallId(); double t0=NowMs(); BOOL r=Real_GetFileAttributesExW(n,l,i); DWORD e=GetLastError(); if(show)Logf(imp,"ATTR","#%llu GetFileAttributesExW path=\"%s\" result=%s error=%lu duration=%.3fms",(unsigned long long)id,WideToUtf8(p.c_str()).c_str(),r?"OK":"FAIL",(unsigned long)e,NowMs()-t0); SetLastError(e); return r; }
static BOOL WINAPI Hook_GetFileAttributesExA(LPCSTR n,GET_FILEEX_INFO_LEVELS l,LPVOID i){ std::wstring p=AnsiToWide(n?n:"<null>"); bool imp=IsFrontEndPath(p),show=g_cfg.logAttributes&&ShouldLogPath(p); uint64_t id=NextCallId(); double t0=NowMs(); BOOL r=Real_GetFileAttributesExA(n,l,i); DWORD e=GetLastError(); if(show)Logf(imp,"ATTR","#%llu GetFileAttributesExA path=\"%s\" result=%s error=%lu duration=%.3fms",(unsigned long long)id,n?n:"<null>",r?"OK":"FAIL",(unsigned long)e,NowMs()-t0); SetLastError(e); return r; }

static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR n){ uint64_t id=NextCallId(); double t0=NowMs(); if(g_cfg.logModules)Logf(false,"MODULE","#%llu BEGIN LoadLibraryW \"%s\"",(unsigned long long)id,WideToUtf8(n?n:L"").c_str()); HMODULE m=Real_LoadLibraryW(n); DWORD e=GetLastError(); if(g_cfg.logModules)Logf(false,"MODULE","#%llu END   LoadLibraryW module=0x%p error=%lu duration=%.3fms",(unsigned long long)id,m,(unsigned long)e,NowMs()-t0); SetLastError(e); return m; }
static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR n){ uint64_t id=NextCallId(); double t0=NowMs(); if(g_cfg.logModules)Logf(false,"MODULE","#%llu BEGIN LoadLibraryA \"%s\"",(unsigned long long)id,n?n:""); HMODULE m=Real_LoadLibraryA(n); DWORD e=GetLastError(); if(g_cfg.logModules)Logf(false,"MODULE","#%llu END   LoadLibraryA module=0x%p error=%lu duration=%.3fms",(unsigned long long)id,m,(unsigned long)e,NowMs()-t0); SetLastError(e); return m; }
static HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR n,HANDLE f,DWORD flags){ uint64_t id=NextCallId(); double t0=NowMs(); if(g_cfg.logModules)Logf(false,"MODULE","#%llu BEGIN LoadLibraryExW \"%s\" flags=0x%08lX",(unsigned long long)id,WideToUtf8(n?n:L"").c_str(),(unsigned long)flags); HMODULE m=Real_LoadLibraryExW(n,f,flags); DWORD e=GetLastError(); if(g_cfg.logModules)Logf(false,"MODULE","#%llu END   LoadLibraryExW module=0x%p error=%lu duration=%.3fms",(unsigned long long)id,m,(unsigned long)e,NowMs()-t0); SetLastError(e); return m; }
static HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR n,HANDLE f,DWORD flags){ uint64_t id=NextCallId(); double t0=NowMs(); if(g_cfg.logModules)Logf(false,"MODULE","#%llu BEGIN LoadLibraryExA \"%s\" flags=0x%08lX",(unsigned long long)id,n?n:"",(unsigned long)flags); HMODULE m=Real_LoadLibraryExA(n,f,flags); DWORD e=GetLastError(); if(g_cfg.logModules)Logf(false,"MODULE","#%llu END   LoadLibraryExA module=0x%p error=%lu duration=%.3fms",(unsigned long long)id,m,(unsigned long)e,NowMs()-t0); SetLastError(e); return m; }

static HANDLE WINAPI Hook_CreateThread(LPSECURITY_ATTRIBUTES sa,SIZE_T stack,LPTHREAD_START_ROUTINE start,LPVOID param,DWORD flags,LPDWORD tid){ uint64_t id=NextCallId(); double t0=NowMs(); if(g_cfg.logThreads)Logf(false,"THREAD","#%llu BEGIN CreateThread start=0x%p (%s) param=0x%p stack=%llu flags=0x%08lX",(unsigned long long)id,(void*)start,ModuleAndRva((void*)start).c_str(),param,(unsigned long long)stack,(unsigned long)flags); HANDLE h=Real_CreateThread(sa,stack,start,param,flags,tid); DWORD e=GetLastError(); if(g_cfg.logThreads)Logf(false,"THREAD","#%llu END   CreateThread handle=0x%p tid=%lu error=%lu duration=%.3fms",(unsigned long long)id,h,tid?(unsigned long)*tid:0UL,(unsigned long)e,NowMs()-t0); SetLastError(e); return h; }

static DWORD WINAPI Hook_WaitForSingleObject(HANDLE h,DWORD ms){ bool show=g_cfg.logWaits&&(ms==INFINITE||ms>=g_cfg.minWaitMs); uint64_t id=NextCallId(); double t0=NowMs(); void* caller=DIAG_CALLER(); std::string desc=show?DescribeHandle(h):std::string(); std::string call=(show&&g_cfg.logCallsites)?ModuleAndRva(caller):std::string(); if(show)Logf(ms==INFINITE,"WAIT","#%llu BEGIN WaitForSingleObject handle=0x%p type=\"%s\" timeout=%s%lu caller=%s",(unsigned long long)id,h,desc.c_str(),ms==INFINITE?"INFINITE/":"",(unsigned long)ms,call.empty()?"<disabled>":call.c_str()); DWORD r=Real_WaitForSingleObject(h,ms); DWORD e=GetLastError(); if(show)Logf(ms==INFINITE,"WAIT","#%llu END   WaitForSingleObject result=0x%08lX error=%lu duration=%.3fms",(unsigned long long)id,(unsigned long)r,(unsigned long)e,NowMs()-t0); SetLastError(e); return r; }
static DWORD WINAPI Hook_WaitForMultipleObjects(DWORD c,const HANDLE* hs,BOOL all,DWORD ms){ bool show=g_cfg.logWaits&&(ms==INFINITE||ms>=g_cfg.minWaitMs); uint64_t id=NextCallId(); double t0=NowMs(); void* caller=DIAG_CALLER(); if(show)Logf(ms==INFINITE,"WAIT","#%llu BEGIN WaitForMultipleObjects count=%lu waitAll=%d timeout=%s%lu handles=0x%p caller=%s",(unsigned long long)id,(unsigned long)c,(int)all,ms==INFINITE?"INFINITE/":"",(unsigned long)ms,hs,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); if(show&&hs)for(DWORD i=0;i<c&&i<16;++i)Logf(false,"WAIT","#%llu   handle[%lu]=0x%p type=\"%s\"",(unsigned long long)id,(unsigned long)i,hs[i],DescribeHandle(hs[i]).c_str()); DWORD r=Real_WaitForMultipleObjects(c,hs,all,ms); DWORD e=GetLastError(); if(show)Logf(ms==INFINITE,"WAIT","#%llu END   WaitForMultipleObjects result=0x%08lX error=%lu duration=%.3fms",(unsigned long long)id,(unsigned long)r,(unsigned long)e,NowMs()-t0); SetLastError(e); return r; }
static DWORD WINAPI Hook_WaitForSingleObjectEx(HANDLE h,DWORD ms,BOOL a){ bool show=g_cfg.logWaits&&(ms==INFINITE||ms>=g_cfg.minWaitMs); uint64_t id=NextCallId(); double t0=NowMs(); void* caller=DIAG_CALLER(); if(show)Logf(ms==INFINITE,"WAIT","#%llu BEGIN WaitForSingleObjectEx handle=0x%p type=\"%s\" timeout=%s%lu alertable=%d caller=%s",(unsigned long long)id,h,DescribeHandle(h).c_str(),ms==INFINITE?"INFINITE/":"",(unsigned long)ms,(int)a,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); DWORD r=Real_WaitForSingleObjectEx(h,ms,a); DWORD e=GetLastError(); if(show)Logf(ms==INFINITE,"WAIT","#%llu END   WaitForSingleObjectEx result=0x%08lX error=%lu duration=%.3fms",(unsigned long long)id,(unsigned long)r,(unsigned long)e,NowMs()-t0); SetLastError(e); return r; }
static DWORD WINAPI Hook_WaitForMultipleObjectsEx(DWORD c,const HANDLE* hs,BOOL all,DWORD ms,BOOL a){ bool show=g_cfg.logWaits&&(ms==INFINITE||ms>=g_cfg.minWaitMs); uint64_t id=NextCallId(); double t0=NowMs(); void* caller=DIAG_CALLER(); if(show)Logf(ms==INFINITE,"WAIT","#%llu BEGIN WaitForMultipleObjectsEx count=%lu waitAll=%d timeout=%s%lu alertable=%d caller=%s",(unsigned long long)id,(unsigned long)c,(int)all,ms==INFINITE?"INFINITE/":"",(unsigned long)ms,(int)a,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); if(show&&hs)for(DWORD i=0;i<c&&i<16;++i)Logf(false,"WAIT","#%llu   handle[%lu]=0x%p type=\"%s\"",(unsigned long long)id,(unsigned long)i,hs[i],DescribeHandle(hs[i]).c_str()); DWORD r=Real_WaitForMultipleObjectsEx(c,hs,all,ms,a); DWORD e=GetLastError(); if(show)Logf(ms==INFINITE,"WAIT","#%llu END   WaitForMultipleObjectsEx result=0x%08lX error=%lu duration=%.3fms",(unsigned long long)id,(unsigned long)r,(unsigned long)e,NowMs()-t0); SetLastError(e); return r; }
static DWORD WINAPI Hook_SignalObjectAndWait(HANDLE sig,HANDLE wait,DWORD ms,BOOL a){ uint64_t id=NextCallId(); double t0=NowMs(); void* caller=DIAG_CALLER(); bool show=g_cfg.logWaits||g_cfg.logSyncObjects; if(show)Logf(ms==INFINITE,"SYNC","#%llu BEGIN SignalObjectAndWait signal=0x%p(\"%s\") wait=0x%p(\"%s\") timeout=%s%lu alertable=%d caller=%s",(unsigned long long)id,sig,DescribeHandle(sig).c_str(),wait,DescribeHandle(wait).c_str(),ms==INFINITE?"INFINITE/":"",(unsigned long)ms,(int)a,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); DWORD r=Real_SignalObjectAndWait(sig,wait,ms,a); DWORD e=GetLastError(); if(show)Logf(ms==INFINITE,"SYNC","#%llu END   SignalObjectAndWait result=0x%08lX error=%lu duration=%.3fms",(unsigned long long)id,(unsigned long)r,(unsigned long)e,NowMs()-t0); SetLastError(e); return r; }

static HANDLE WINAPI Hook_CreateEventW(LPSECURITY_ATTRIBUTES sa,BOOL manual,BOOL initial,LPCWSTR name){ void* caller=DIAG_CALLER(); HANDLE h=Real_CreateEventW(sa,manual,initial,name); DWORD e=GetLastError(); if(h)PutHandleState(h,manual?"event-manual":"event-auto",name?name:L"",caller); if(g_cfg.logSyncObjects)Logf(false,"SYNC","CreateEventW h=0x%p manual=%d initial=%d name=\"%s\" error=%lu caller=%s",h,(int)manual,(int)initial,WideToUtf8(name?name:L"").c_str(),(unsigned long)e,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return h; }
static HANDLE WINAPI Hook_CreateEventA(LPSECURITY_ATTRIBUTES sa,BOOL manual,BOOL initial,LPCSTR name){ void* caller=DIAG_CALLER(); HANDLE h=Real_CreateEventA(sa,manual,initial,name); DWORD e=GetLastError(); if(h)PutHandleState(h,manual?"event-manual":"event-auto",AnsiToWide(name?name:""),caller); if(g_cfg.logSyncObjects)Logf(false,"SYNC","CreateEventA h=0x%p manual=%d initial=%d name=\"%s\" error=%lu caller=%s",h,(int)manual,(int)initial,name?name:"",(unsigned long)e,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return h; }
static BOOL WINAPI Hook_SetEvent(HANDLE h){ void* caller=DIAG_CALLER(); double t0=NowMs(); BOOL r=Real_SetEvent(h); DWORD e=GetLastError(); if(g_cfg.logSyncObjects)Logf(false,"SYNC","SetEvent h=0x%p type=\"%s\" result=%s error=%lu duration=%.3fms caller=%s",h,DescribeHandle(h).c_str(),r?"OK":"FAIL",(unsigned long)e,NowMs()-t0,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return r; }
static BOOL WINAPI Hook_ResetEvent(HANDLE h){ void* caller=DIAG_CALLER(); BOOL r=Real_ResetEvent(h); DWORD e=GetLastError(); if(g_cfg.logSyncObjects)Logf(false,"SYNC","ResetEvent h=0x%p type=\"%s\" result=%s error=%lu caller=%s",h,DescribeHandle(h).c_str(),r?"OK":"FAIL",(unsigned long)e,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return r; }
static BOOL WINAPI Hook_PulseEvent(HANDLE h){ void* caller=DIAG_CALLER(); BOOL r=Real_PulseEvent(h); DWORD e=GetLastError(); if(g_cfg.logSyncObjects)Logf(false,"SYNC","PulseEvent h=0x%p type=\"%s\" result=%s error=%lu caller=%s",h,DescribeHandle(h).c_str(),r?"OK":"FAIL",(unsigned long)e,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return r; }
static HANDLE WINAPI Hook_CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa,LONG initial,LONG maximum,LPCWSTR name){ void* caller=DIAG_CALLER(); HANDLE h=Real_CreateSemaphoreW(sa,initial,maximum,name); DWORD e=GetLastError(); if(h)PutHandleState(h,"semaphore",name?name:L"",caller); if(g_cfg.logSyncObjects)Logf(false,"SYNC","CreateSemaphoreW h=0x%p initial=%ld max=%ld name=\"%s\" error=%lu caller=%s",h,(long)initial,(long)maximum,WideToUtf8(name?name:L"").c_str(),(unsigned long)e,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return h; }
static HANDLE WINAPI Hook_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa,LONG initial,LONG maximum,LPCSTR name){ void* caller=DIAG_CALLER(); HANDLE h=Real_CreateSemaphoreA(sa,initial,maximum,name); DWORD e=GetLastError(); if(h)PutHandleState(h,"semaphore",AnsiToWide(name?name:""),caller); if(g_cfg.logSyncObjects)Logf(false,"SYNC","CreateSemaphoreA h=0x%p initial=%ld max=%ld name=\"%s\" error=%lu caller=%s",h,(long)initial,(long)maximum,name?name:"",(unsigned long)e,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return h; }
static BOOL WINAPI Hook_ReleaseSemaphore(HANDLE h,LONG rel,LPLONG prev){ void* caller=DIAG_CALLER(); BOOL r=Real_ReleaseSemaphore(h,rel,prev); DWORD e=GetLastError(); if(g_cfg.logSyncObjects)Logf(false,"SYNC","ReleaseSemaphore h=0x%p type=\"%s\" release=%ld previous=%ld result=%s error=%lu caller=%s",h,DescribeHandle(h).c_str(),(long)rel,prev?(long)*prev:-1L,r?"OK":"FAIL",(unsigned long)e,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return r; }
static HANDLE WINAPI Hook_CreateMutexW(LPSECURITY_ATTRIBUTES sa,BOOL owner,LPCWSTR name){ void* caller=DIAG_CALLER(); HANDLE h=Real_CreateMutexW(sa,owner,name); DWORD e=GetLastError(); if(h)PutHandleState(h,"mutex",name?name:L"",caller); if(g_cfg.logSyncObjects)Logf(false,"SYNC","CreateMutexW h=0x%p owner=%d name=\"%s\" error=%lu caller=%s",h,(int)owner,WideToUtf8(name?name:L"").c_str(),(unsigned long)e,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return h; }
static HANDLE WINAPI Hook_CreateMutexA(LPSECURITY_ATTRIBUTES sa,BOOL owner,LPCSTR name){ void* caller=DIAG_CALLER(); HANDLE h=Real_CreateMutexA(sa,owner,name); DWORD e=GetLastError(); if(h)PutHandleState(h,"mutex",AnsiToWide(name?name:""),caller); if(g_cfg.logSyncObjects)Logf(false,"SYNC","CreateMutexA h=0x%p owner=%d name=\"%s\" error=%lu caller=%s",h,(int)owner,name?name:"",(unsigned long)e,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return h; }
static BOOL WINAPI Hook_ReleaseMutex(HANDLE h){ void* caller=DIAG_CALLER(); BOOL r=Real_ReleaseMutex(h); DWORD e=GetLastError(); if(g_cfg.logSyncObjects)Logf(false,"SYNC","ReleaseMutex h=0x%p type=\"%s\" result=%s error=%lu caller=%s",h,DescribeHandle(h).c_str(),r?"OK":"FAIL",(unsigned long)e,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); SetLastError(e); return r; }
static VOID WINAPI Hook_Sleep(DWORD ms){ void* caller=DIAG_CALLER(); if(g_cfg.logWaits&&ms>=g_cfg.minWaitMs)Logf(false,"WAIT","Sleep %lu ms caller=%s",(unsigned long)ms,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); Real_Sleep(ms); }
static DWORD WINAPI Hook_SleepEx(DWORD ms,BOOL a){ void* caller=DIAG_CALLER(); double t0=NowMs(); if(g_cfg.logWaits&&ms>=g_cfg.minWaitMs)Logf(false,"WAIT","BEGIN SleepEx %lu ms alertable=%d caller=%s",(unsigned long)ms,(int)a,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); DWORD r=Real_SleepEx(ms,a); if(g_cfg.logWaits&&ms>=g_cfg.minWaitMs)Logf(false,"WAIT","END SleepEx result=0x%08lX duration=%.3fms",(unsigned long)r,NowMs()-t0); return r; }
static VOID WINAPI Hook_ExitThread(DWORD code){ void* caller=DIAG_CALLER(); if(g_cfg.logThreads)Logf(true,"THREAD","ExitThread code=%lu caller=%s",(unsigned long)code,g_cfg.logCallsites?ModuleAndRva(caller).c_str():"<disabled>"); Real_ExitThread(code); }

struct HookDef { const char* name; void* hook; };
static const HookDef kHooks[] = {
    {"CreateFileW",(void*)&Hook_CreateFileW},{"CreateFileA",(void*)&Hook_CreateFileA},{"ReadFile",(void*)&Hook_ReadFile},{"SetFilePointer",(void*)&Hook_SetFilePointer},{"SetFilePointerEx",(void*)&Hook_SetFilePointerEx},{"GetFileSize",(void*)&Hook_GetFileSize},{"GetFileSizeEx",(void*)&Hook_GetFileSizeEx},{"CreateFileMappingW",(void*)&Hook_CreateFileMappingW},{"CreateFileMappingA",(void*)&Hook_CreateFileMappingA},{"MapViewOfFile",(void*)&Hook_MapViewOfFile},{"MapViewOfFileEx",(void*)&Hook_MapViewOfFileEx},{"UnmapViewOfFile",(void*)&Hook_UnmapViewOfFile},{"CloseHandle",(void*)&Hook_CloseHandle},{"GetFileAttributesW",(void*)&Hook_GetFileAttributesW},{"GetFileAttributesA",(void*)&Hook_GetFileAttributesA},{"GetFileAttributesExW",(void*)&Hook_GetFileAttributesExW},{"GetFileAttributesExA",(void*)&Hook_GetFileAttributesExA},{"LoadLibraryW",(void*)&Hook_LoadLibraryW},{"LoadLibraryA",(void*)&Hook_LoadLibraryA},{"LoadLibraryExW",(void*)&Hook_LoadLibraryExW},{"LoadLibraryExA",(void*)&Hook_LoadLibraryExA},{"CreateThread",(void*)&Hook_CreateThread},{"ExitThread",(void*)&Hook_ExitThread},{"WaitForSingleObject",(void*)&Hook_WaitForSingleObject},{"WaitForMultipleObjects",(void*)&Hook_WaitForMultipleObjects},{"WaitForSingleObjectEx",(void*)&Hook_WaitForSingleObjectEx},{"WaitForMultipleObjectsEx",(void*)&Hook_WaitForMultipleObjectsEx},{"SignalObjectAndWait",(void*)&Hook_SignalObjectAndWait},{"CreateEventW",(void*)&Hook_CreateEventW},{"CreateEventA",(void*)&Hook_CreateEventA},{"SetEvent",(void*)&Hook_SetEvent},{"ResetEvent",(void*)&Hook_ResetEvent},{"PulseEvent",(void*)&Hook_PulseEvent},{"CreateSemaphoreW",(void*)&Hook_CreateSemaphoreW},{"CreateSemaphoreA",(void*)&Hook_CreateSemaphoreA},{"ReleaseSemaphore",(void*)&Hook_ReleaseSemaphore},{"CreateMutexW",(void*)&Hook_CreateMutexW},{"CreateMutexA",(void*)&Hook_CreateMutexA},{"ReleaseMutex",(void*)&Hook_ReleaseMutex},{"Sleep",(void*)&Hook_Sleep},{"SleepEx",(void*)&Hook_SleepEx},
};

static void* HookForName(const char* name){ if(!name)return nullptr; for(const auto& h:kHooks)if(strcmp(h.name,name)==0)return h.hook; return nullptr; }
static bool ShouldSkipModule(HMODULE module){ if(!module||module==g_self)return true; wchar_t p[MAX_PATH]{}; if(!GetModuleFileNameW(module,p,MAX_PATH))return false; const wchar_t* base=wcsrchr(p,L'\\'); base=base?base+1:p; static const wchar_t* skip[]={L"kernel32.dll",L"KernelBase.dll",L"ntdll.dll",L"user32.dll",L"gdi32.dll",L"advapi32.dll",L"msvcrt.dll",L"ucrtbase.dll",L"sechost.dll",L"rpcrt4.dll"}; for(const wchar_t* x:skip)if(_wcsicmp(base,x)==0)return true; return false; }
static int PatchModuleIAT(HMODULE module){ if(ShouldSkipModule(module))return 0; EnterCriticalSection(&g_stateLock); if(g_patchedModules.count((uintptr_t)module)){LeaveCriticalSection(&g_stateLock);return 0;} g_patchedModules.insert((uintptr_t)module); LeaveCriticalSection(&g_stateLock); auto base=(uint8_t*)module; auto dos=(IMAGE_DOS_HEADER*)base; if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return 0; auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew); if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC)return 0; const auto& dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if(!dir.VirtualAddress||!dir.Size)return 0; int patched=0; auto desc=(IMAGE_IMPORT_DESCRIPTOR*)(base+dir.VirtualAddress); for(;desc->Name;++desc){ auto first=(IMAGE_THUNK_DATA*)(base+desc->FirstThunk); auto orig=desc->OriginalFirstThunk?(IMAGE_THUNK_DATA*)(base+desc->OriginalFirstThunk):nullptr; if(!first||!orig)continue; for(;orig->u1.AddressOfData;++orig,++first){ if(IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal))continue; auto ibn=(IMAGE_IMPORT_BY_NAME*)(base+orig->u1.AddressOfData); void* repl=HookForName((const char*)ibn->Name); if(!repl)continue; DWORD old=0; if(VirtualProtect(&first->u1.Function,sizeof(uintptr_t),PAGE_READWRITE,&old)){first->u1.Function=(uintptr_t)repl; DWORD ign=0; VirtualProtect(&first->u1.Function,sizeof(uintptr_t),old,&ign); FlushInstructionCache(GetCurrentProcess(),&first->u1.Function,sizeof(uintptr_t)); ++patched;}}} if(patched&&g_cfg.logModules){wchar_t p[MAX_PATH]{};GetModuleFileNameW(module,p,MAX_PATH);Logf(false,"HOOK","Patched %d imports in module=0x%p \"%s\"",patched,module,WideToUtf8(p).c_str());} return patched; }
static int PatchLoadedModules(){ int total=0; HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,GetCurrentProcessId()); if(snap==INVALID_HANDLE_VALUE)return 0; MODULEENTRY32W me{}; me.dwSize=sizeof(me); if(Module32FirstW(snap,&me)){do{total+=PatchModuleIAT(me.hModule);}while(Module32NextW(snap,&me));} CloseHandle(snap); return total; }

static LONG CALLBACK VectoredHandler(PEXCEPTION_POINTERS ep){ if(!g_cfg.logExceptions||!ep||!ep->ExceptionRecord||!ep->ContextRecord)return EXCEPTION_CONTINUE_SEARCH; DWORD code=ep->ExceptionRecord->ExceptionCode; switch(code){case EXCEPTION_ACCESS_VIOLATION:case EXCEPTION_ILLEGAL_INSTRUCTION:case EXCEPTION_PRIV_INSTRUCTION:case EXCEPTION_STACK_OVERFLOW:case EXCEPTION_INT_DIVIDE_BY_ZERO:case EXCEPTION_FLT_DIVIDE_BY_ZERO:case EXCEPTION_BREAKPOINT:break;default:return EXCEPTION_CONTINUE_SEARCH;} CONTEXT* c=ep->ContextRecord; Logf(true,"EXCEPT","code=0x%08lX address=0x%p (%s) EIP=0x%08lX ESP=0x%08lX EBP=0x%08lX EAX=0x%08lX EBX=0x%08lX ECX=0x%08lX EDX=0x%08lX ESI=0x%08lX EDI=0x%08lX flags=0x%08lX",(unsigned long)code,ep->ExceptionRecord->ExceptionAddress,ModuleAndRva(ep->ExceptionRecord->ExceptionAddress).c_str(),(unsigned long)c->Eip,(unsigned long)c->Esp,(unsigned long)c->Ebp,(unsigned long)c->Eax,(unsigned long)c->Ebx,(unsigned long)c->Ecx,(unsigned long)c->Edx,(unsigned long)c->Esi,(unsigned long)c->Edi,(unsigned long)c->EFlags); return EXCEPTION_CONTINUE_SEARCH; }

static void SnapshotThreads(){ if(!g_cfg.threadSnapshots)return; DWORD pid=GetCurrentProcessId(),selfTid=GetCurrentThreadId(); HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0); if(snap==INVALID_HANDLE_VALUE){Logf(true,"SNAP","CreateToolhelp32Snapshot(THREAD) failed error=%lu",(unsigned long)GetLastError());return;} Logf(true,"SNAP","========== THREAD SNAPSHOT BEGIN =========="); THREADENTRY32 te{}; te.dwSize=sizeof(te); if(Thread32First(snap,&te)){do{ if(te.th32OwnerProcessID!=pid||te.th32ThreadID==selfTid)continue; HANDLE th=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,te.th32ThreadID); if(!th){Logf(false,"SNAP","TID=%lu OpenThread failed error=%lu",(unsigned long)te.th32ThreadID,(unsigned long)GetLastError());continue;} DWORD sp=SuspendThread(th); if(sp==(DWORD)-1){Logf(false,"SNAP","TID=%lu SuspendThread failed error=%lu",(unsigned long)te.th32ThreadID,(unsigned long)GetLastError());CloseHandle(th);continue;} CONTEXT c{}; c.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER; BOOL got=GetThreadContext(th,&c); std::vector<uintptr_t> frames; if(got){frames.push_back((uintptr_t)c.Eip); uintptr_t ebp=(uintptr_t)c.Ebp; for(DWORD d=1;d<g_cfg.snapshotStackDepth&&ebp;++d){struct FP{uintptr_t prev;uintptr_t ret;} p{}; SIZE_T br=0; if(!ReadProcessMemory(GetCurrentProcess(),(LPCVOID)ebp,&p,sizeof(p),&br)||br!=sizeof(p)||!p.ret)break; frames.push_back(p.ret); if(p.prev<=ebp||p.prev-ebp>0x100000)break; ebp=p.prev;}} ResumeThread(th); CloseHandle(th); if(!got){Logf(false,"SNAP","TID=%lu GetThreadContext failed error=%lu",(unsigned long)te.th32ThreadID,(unsigned long)GetLastError());continue;} std::string stack; for(size_t i=0;i<frames.size();++i){if(i)stack+=" <- ";stack+=ModuleAndRva((void*)frames[i]);} Logf(true,"SNAP","TID=%lu EIP=0x%08lX (%s) ESP=0x%08lX EBP=0x%08lX stack=%s",(unsigned long)te.th32ThreadID,(unsigned long)c.Eip,ModuleAndRva((void*)(uintptr_t)c.Eip).c_str(),(unsigned long)c.Esp,(unsigned long)c.Ebp,stack.empty()?"<no-frame-chain>":stack.c_str()); }while(Thread32Next(snap,&te)); } CloseHandle(snap); Logf(true,"SNAP","========== THREAD SNAPSHOT END =========="); }

static void LogEnvironment(){ wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);wchar_t cwd[MAX_PATH]{};GetCurrentDirectoryW(MAX_PATH,cwd);Logf(true,"INIT","NFSTR Diagnostics v0.2.0 loaded successfully");Logf(false,"INIT","EXE=\"%s\"",WideToUtf8(exe).c_str());Logf(false,"INIT","ASI=\"%s\"",WideToUtf8(g_modulePath).c_str());Logf(false,"INIT","CWD=\"%s\"",WideToUtf8(cwd).c_str());Logf(false,"INIT","GameDir=\"%s\"",WideToUtf8(g_gameDir).c_str());Logf(false,"INIT","PID=%lu processBase=0x%p pointerSize=%u",(unsigned long)GetCurrentProcessId(),GetModuleHandleW(nullptr),(unsigned)sizeof(void*));Logf(false,"INIT","FrontEnd delta watch: offset=%llu (0x%llX), slot=%llu (0x%llX), end=%llu (0x%llX)",(unsigned long long)kFrontEndDeltaOffset,(unsigned long long)kFrontEndDeltaOffset,(unsigned long long)kFrontEndDeltaSize,(unsigned long long)kFrontEndDeltaSize,(unsigned long long)kFrontEndDeltaEnd,(unsigned long long)kFrontEndDeltaEnd);Logf(false,"CONFIG","Verbose=%d OnlyGameDirectory=%d MaxLogMB=%lu",g_cfg.verbose,g_cfg.onlyGameDirectory,(unsigned long)g_cfg.maxLogMB);Logf(false,"CONFIG","FileIO creates=%d reads=%d seeks=%d sizes=%d mappings=%d attributes=%d close=%d",g_cfg.logCreates,g_cfg.logReads,g_cfg.logSeeks,g_cfg.logSizes,g_cfg.logMappings,g_cfg.logAttributes,g_cfg.logCloseHandle);Logf(false,"CONFIG","Runtime modules=%d threads=%d waits=%d exceptions=%d sync=%d callsites=%d MinWaitMs=%lu ModuleScanMs=%lu",g_cfg.logModules,g_cfg.logThreads,g_cfg.logWaits,g_cfg.logExceptions,g_cfg.logSyncObjects,g_cfg.logCallsites,(unsigned long)g_cfg.minWaitMs,(unsigned long)g_cfg.moduleScanMs);Logf(false,"CONFIG","ThreadSnapshots=%d after=%lums interval=%lums depth=%lu",g_cfg.threadSnapshots,(unsigned long)g_cfg.snapshotAfterMs,(unsigned long)g_cfg.snapshotIntervalMs,(unsigned long)g_cfg.snapshotStackDepth);}

static DWORD WINAPI WorkerThread(LPVOID){ InitializeCriticalSection(&g_logLock);InitializeCriticalSection(&g_stateLock);QueryPerformanceFrequency(&g_qpcFreq);QueryPerformanceCounter(&g_qpcStart);GetModuleFileNameW(g_self,g_modulePath,MAX_PATH);CopyDirOfPath(g_modulePath,g_moduleDir,MAX_PATH);GetModuleFileNameW(nullptr,g_gameDir,MAX_PATH);wchar_t tmp[MAX_PATH]{};CopyDirOfPath(g_gameDir,tmp,MAX_PATH);wcsncpy(g_gameDir,tmp,MAX_PATH-1);_snwprintf(g_iniPath,MAX_PATH-1,L"%s\\NFSTR_Diagnostics.ini",g_moduleDir);_snwprintf(g_logPath,MAX_PATH-1,L"%s\\NFSTR_Diagnostics.log",g_moduleDir);g_iniPath[MAX_PATH-1]=0;g_logPath[MAX_PATH-1]=0;LoadConfig();g_log=CreateFileW(g_logPath,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);if(g_log==INVALID_HANDLE_VALUE)return 1;const unsigned char bom[]={0xEF,0xBB,0xBF};RawWrite((const char*)bom,sizeof(bom),false);LogEnvironment();if(g_cfg.logExceptions)AddVectoredExceptionHandler(1,VectoredHandler);int first=PatchLoadedModules();Logf(true,"HOOK","Initial IAT scan complete; patched %d imports",first);Logf(true,"INIT","Diagnostics armed. BEGIN without matching END can identify a call that never returned.");double nextSnapshot=(double)g_cfg.snapshotAfterMs;while(WaitForSingleObject(g_stopEvent,g_cfg.moduleScanMs)==WAIT_TIMEOUT){PatchLoadedModules();double now=NowMs();if(g_cfg.threadSnapshots&&now>=nextSnapshot){SnapshotThreads();nextSnapshot=now+(double)g_cfg.snapshotIntervalMs;}}Logf(false,"INIT","Diagnostics worker stopping");FlushFileBuffers(g_log);CloseHandle(g_log);g_log=INVALID_HANDLE_VALUE;return 0;}

} // namespace diag

BOOL WINAPI DllMain(HINSTANCE hinst,DWORD reason,LPVOID){using namespace diag;if(reason==DLL_PROCESS_ATTACH){g_self=(HMODULE)hinst;DisableThreadLibraryCalls(hinst);g_stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);HANDLE th=CreateThread(nullptr,0,WorkerThread,nullptr,0,nullptr);if(th)CloseHandle(th);}else if(reason==DLL_PROCESS_DETACH){if(g_stopEvent)SetEvent(g_stopEvent);}return TRUE;}
