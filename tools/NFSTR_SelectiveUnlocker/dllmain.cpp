// Need for Speed: The Run - Selective Promo/DLC Unlocker (research build)
//
// Derived from the three reflection-name edits in xan1242/NFSTR_UltimateUnlocker.
// Deliberately DOES NOT include its all-car hooks or stage-select unlock NOPs.
// Goal: preserve normal career/challenge progression while testing whether the
// promo/hidden-content reflection edits are sufficient to expose genuine DLC.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
    HMODULE g_Module = nullptr;

    constexpr uintptr_t kPreferredImageBase = 0x00400000;
    constexpr uintptr_t kUnlockersLastChar  = 0x025A35EC; // 's' in "Unlockers"
    constexpr uintptr_t kPromoLastChar      = 0x025A362D; // 't' in "IsPromoContent"
    constexpr uintptr_t kHiddenLastChar     = 0x025A363D; // 'k' in "IsHiddenUnlock"

    struct Config {
        bool PatchUnlockersReflection = true;
        bool PatchPromoContentReflection = true;
        bool PatchHiddenUnlockReflection = true;
    } g_Config;

    void Log(const char* fmt, ...) {
        char dllPath[MAX_PATH] = {};
        GetModuleFileNameA(g_Module, dllPath, MAX_PATH);
        char* slash = std::strrchr(dllPath, '\\');
        if (slash) std::strcpy(slash + 1, "NFSTR_SelectiveUnlocker.log");
        else std::strcpy(dllPath, "NFSTR_SelectiveUnlocker.log");

        FILE* f = std::fopen(dllPath, "a");
        if (!f) return;
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
        std::fclose(f);
    }

    bool ReadBoolIni(const char* key, bool def) {
        char dllPath[MAX_PATH] = {};
        GetModuleFileNameA(g_Module, dllPath, MAX_PATH);
        char* slash = std::strrchr(dllPath, '\\');
        if (slash) std::strcpy(slash + 1, "NFSTR_SelectiveUnlocker.ini");
        else std::strcpy(dllPath, "NFSTR_SelectiveUnlocker.ini");
        return GetPrivateProfileIntA("SELECTIVE_UNLOCK", key, def ? 1 : 0, dllPath) != 0;
    }

    void EnsureIni() {
        char dllPath[MAX_PATH] = {};
        GetModuleFileNameA(g_Module, dllPath, MAX_PATH);
        char* slash = std::strrchr(dllPath, '\\');
        if (slash) std::strcpy(slash + 1, "NFSTR_SelectiveUnlocker.ini");
        else std::strcpy(dllPath, "NFSTR_SelectiveUnlocker.ini");

        char value[32] = {};
        auto ensure = [&](const char* key, const char* def) {
            static const char sentinel[] = "__MISSING__";
            GetPrivateProfileStringA("SELECTIVE_UNLOCK", key, sentinel, value, sizeof(value), dllPath);
            if (std::strcmp(value, sentinel) == 0)
                WritePrivateProfileStringA("SELECTIVE_UNLOCK", key, def, dllPath);
        };
        ensure("PatchUnlockersReflection", "1");
        ensure("PatchPromoContentReflection", "1");
        ensure("PatchHiddenUnlockReflection", "1");
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, dllPath);
    }

    bool PatchByte(uintptr_t preferredVa, uint8_t expected, uint8_t replacement, const char* label) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        if (!base) return false;
        const uintptr_t address = base + (preferredVa - kPreferredImageBase);
        auto* p = reinterpret_cast<uint8_t*>(address);
        if (*p != expected && *p != replacement) {
            Log("SKIP %s: unexpected byte %02X at %08X (expected %02X)", label, *p,
                static_cast<unsigned>(address), expected);
            return false;
        }
        if (*p == replacement) {
            Log("%s already applied", label);
            return true;
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        *p = replacement;
        FlushInstructionCache(GetCurrentProcess(), p, 1);
        DWORD dummy = 0;
        VirtualProtect(p, 1, oldProtect, &dummy);
        Log("Applied %s at %08X", label, static_cast<unsigned>(address));
        return true;
    }

    DWORD WINAPI InitThread(LPVOID) {
        EnsureIni();
        g_Config.PatchUnlockersReflection = ReadBoolIni("PatchUnlockersReflection", true);
        g_Config.PatchPromoContentReflection = ReadBoolIni("PatchPromoContentReflection", true);
        g_Config.PatchHiddenUnlockReflection = ReadBoolIni("PatchHiddenUnlockReflection", true);

        Log("Selective unlocker started. Unlockers=%d Promo=%d Hidden=%d",
            g_Config.PatchUnlockersReflection ? 1 : 0,
            g_Config.PatchPromoContentReflection ? 1 : 0,
            g_Config.PatchHiddenUnlockReflection ? 1 : 0);
        Log("All-car and stage-select unlock patches are intentionally absent.");

        if (g_Config.PatchUnlockersReflection)
            PatchByte(kUnlockersLastChar, 's', 0, "Unlockers -> Unlocker reflection edit");
        if (g_Config.PatchPromoContentReflection)
            PatchByte(kPromoLastChar, 't', 0, "IsPromoContent reflection edit");
        if (g_Config.PatchHiddenUnlockReflection)
            PatchByte(kHiddenLastChar, 'k', 0, "IsHiddenUnlock reflection edit");
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_Module = module;
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
