#include "features.h"
#include "../config.h"
#include "../memory.h"
#include "../logger.h"
#include <windows.h>
#include <cstdint>

// Native Frostbite MSAA experiment.
//
// Upstream testing only proved that changing WorldRenderSettings::MultisampleCount
// by itself has no visible effect. That does not prove the renderer's MSAA path is
// absent: The Run's executable still contains DxMultisampleEnable, MSAA render
// target names, sample-count-specific depth resources and MSAA resolve shaders.
//
// This experiment enables BOTH sides exposed by the retail executable:
//   fb::WorldRenderSettings::MultisampleCount   +0x0B8 (int32)
//   fb::ShaderSystemSettings::DxMultisampleEnable +0x102 (bool)
//
// The values are kept resident because Frostbite settings containers can be
// recreated/reset on level transitions. No Present hook, no FXAA/SMAA, no
// post-process anti-aliasing and no GameTime changes are used here.

namespace {
    const uintptr_t kSettingsManagerPtr      = 0x2446C74; // fb::g_settingsManager
    const uintptr_t kGetContainerFn          = 0x0E72D0;  // SettingsManager::getContainer
    const uintptr_t kWorldRenderTypeInfo     = 0x2AE6E98 - 0x400000;
    const uintptr_t kShaderSystemTypeInfo    = 0x2AA3428 - 0x400000;

    const uintptr_t kOffMultisampleCount     = 0x0B8;
    const uintptr_t kOffDxMultisampleEnable  = 0x102;

    typedef uintptr_t (__fastcall *GetContainerFn)(uintptr_t self, uintptr_t edx, uintptr_t typeInfo);

    bool g_Enabled = false;
    bool g_LoggedWorld = false;
    bool g_LoggedShader = false;

    bool ValidSampleCount(int samples) {
        return samples == 2 || samples == 4 || samples == 8;
    }

    uintptr_t ResolveContainer(uintptr_t typeInfoOffset) {
        const uintptr_t base = Memory::GetGameBase();
        if (!base) return 0;

        const uintptr_t mgrAddress = base + kSettingsManagerPtr;
        if (!Memory::IsReadable(mgrAddress, sizeof(uintptr_t))) return 0;

        const uintptr_t mgr = *reinterpret_cast<uintptr_t*>(mgrAddress);
        if (mgr < 0x10000) return 0;

        GetContainerFn getContainer =
            reinterpret_cast<GetContainerFn>(base + kGetContainerFn);
        const uintptr_t c = getContainer(mgr, 0, base + typeInfoOffset);
        return (c < 0x10000) ? 0 : c;
    }
}

namespace Features {
    void InitAntiAliasing() {
        const int samples = g_Config.AntiAliasing;

        if (samples <= 1) {
            Logger::Log("Native MSAA disabled (AntiAliasing=%d). No post-process AA is installed.", samples);
            return;
        }

        if (!ValidSampleCount(samples)) {
            Logger::Log("Native MSAA disabled: AntiAliasing must be 0, 2, 4 or 8 (got %d).", samples);
            return;
        }

        g_Enabled = true;
        Logger::Log("Native MSAA requested: %dx. Enabling Frostbite DxMultisampleEnable + MultisampleCount; no post-process AA.", samples);
    }

    void UpdateAntiAliasing() {
        if (!g_Enabled) return;

        const int samples = g_Config.AntiAliasing;

        const uintptr_t world = ResolveContainer(kWorldRenderTypeInfo);
        if (world && Memory::IsReadable(world + kOffMultisampleCount, sizeof(int32_t))) {
            int32_t* p = reinterpret_cast<int32_t*>(world + kOffMultisampleCount);
            if (!g_LoggedWorld) {
                Logger::Log("Native MSAA: WorldRenderSettings at 0x%08X, MultisampleCount %d -> %d.",
                            world, *p, samples);
                g_LoggedWorld = true;
            }
            if (*p != samples) *p = samples;
        }

        const uintptr_t shader = ResolveContainer(kShaderSystemTypeInfo);
        if (shader && Memory::IsReadable(shader + kOffDxMultisampleEnable, sizeof(uint8_t))) {
            uint8_t* p = reinterpret_cast<uint8_t*>(shader + kOffDxMultisampleEnable);
            if (!g_LoggedShader) {
                Logger::Log("Native MSAA: ShaderSystemSettings at 0x%08X, DxMultisampleEnable %u -> 1.",
                            shader, static_cast<unsigned>(*p));
                g_LoggedShader = true;
            }
            if (*p != 1) *p = 1;
        }
    }
}
