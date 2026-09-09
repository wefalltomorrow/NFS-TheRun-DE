#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// Derived from xan1242/NFSTR_UltimateUnlocker, but intentionally keeps ONLY
// the two reflection-name patches that neutralise the promo/hidden flags.
//
// Deliberately NOT included:
//   - Unlockers -> Unlocker (broad UnlockEverything behaviour)
//   - car m_isUnlocked hooks
//   - stage/challenge unlock branches
//   - Ebisu/OnNetworkConnected patches
//   - any network or Origin/Ebisu stubs
//
// Target: DRM-free NFS The Run v1.1.0.0 executable layout.

namespace {
    constexpr uintptr_t kImageBase = 0x00400000;
    constexpr uintptr_t kPromoTailVA  = 0x025A362D; // final 't' in "IsPromoContent"
    constexpr uintptr_t kHiddenTailVA = 0x025A363D; // final 'k' in "IsHiddenUnlock"

    bool PatchByte(uintptr_t address, uint8_t expected, uint8_t replacement) {
        auto* p = reinterpret_cast<uint8_t*>(address);
        if (*p != expected && *p != replacement)
            return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        *p = replacement;
        FlushInstructionCache(GetCurrentProcess(), p, 1);

        DWORD dummy = 0;
        VirtualProtect(p, 1, oldProtect, &dummy);
        return true;
    }

    void ApplyPatches() {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base)
            return;

        const uintptr_t promo  = base + (kPromoTailVA  - kImageBase);
        const uintptr_t hidden = base + (kHiddenTailVA - kImageBase);

        // "IsPromoContent" -> "IsPromoConten"
        // The field then remains at its default false value when Frostbite loads
        // Unlockable data, instead of content being classified as promo-only.
        PatchByte(promo, static_cast<uint8_t>('t'), 0);

        // "IsHiddenUnlock" -> "IsHiddenUnloc"
        // Likewise prevents the hidden-unlock flag from being deserialised.
        PatchByte(hidden, static_cast<uint8_t>('k'), 0);
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        ApplyPatches();
    return TRUE;
}
