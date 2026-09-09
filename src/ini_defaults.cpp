#include <windows.h>
#include <cstring>
#include <string>

namespace {
    struct DefaultEntry {
        const char* section;
        const char* key;
        const char* value;
    };

    // Keep this list in sync with Config::Load(). Existing user values are never
    // overwritten; these are only inserted when a key is genuinely absent.
    const DefaultEntry kDefaults[] = {
        { "MAIN", "DebugLog", "1" },

        { "GAMEPLAY", "RandomizeTimeOfDay", "0" },
        { "GAMEPLAY", "RunForYourLife", "1" },

        { "GRAPHICS_FPS", "EnableFramerateUnlocker", "1" },
        { "GRAPHICS_FPS", "FPSLimit", "60" },
        { "GRAPHICS_FPS", "UnlockCutsceneFPS", "0" },
        { "GRAPHICS_FPS", "ClampSimRateWhenNoControl", "1" },

        { "GRAPHICS_QUALITY", "AntiAliasing", "4" },
        { "GRAPHICS_QUALITY", "FastLoadingVSyncBypass", "1" },
        { "GRAPHICS_QUALITY", "ForceMeshLod", "0" },
        { "GRAPHICS_QUALITY", "MeshGlobalLodScale", "1000" },
        { "GRAPHICS_QUALITY", "VinylTargetSize", "4096" },

        { "UI_DEBUG", "EnableExtraUIOptions", "0" },
        { "UI_DEBUG", "AlwaysShowPhotoMode", "1" },

        { "TRACK_RULES", "DisableCheckpointTimer", "0" },
        { "TRACK_RULES", "DisableResetOOB", "0" },
        { "TRACK_RULES", "DisableWrongWayRespawn", "0" },

        { "TRAFFIC", "EnableTrafficControls", "0" },
        { "TRAFFIC", "TrafficDensityScale", "0.05" },
        { "TRAFFIC", "TrafficMaxDensity", "0.15" },
        { "TRAFFIC", "TrafficVehicleLimit", "25" },

        { "VEHICLE", "DisablePlayerAssists", "0" },
        { "VEHICLE", "VehicleHealth", "-1" },
        { "VEHICLE", "AiSkillScale", "1" },
        { "VEHICLE", "AiGlueScale", "1" },
        { "VEHICLE", "AiNosRechargeScale", "1" },
        { "VEHICLE", "PlayerNosRechargeScale", "1" },
        { "VEHICLE", "PlayerNosBonusScale", "1" },
        { "VEHICLE", "PlayerNosStrengthScale", "1" },
        { "VEHICLE", "PlayerNosBoostScale", "1" },
        { "VEHICLE", "PlayerDraftRateScale", "1" },

        { "HIGH_FPS_FIXES", "FixEngineAudioSlew", "1" },
        { "HIGH_FPS_FIXES", "FixKickupParticles", "1" },
        { "HIGH_FPS_FIXES", "KickupVelocityScale", "-1" },

        { "RENDER", "EnableRenderTweaks", "1" },
        { "RENDER", "ForceFov", "60" },
        { "RENDER", "ForceFovOnlyWhileDriving", "1" },
        { "RENDER", "ForceRenderShiftEnabled", "0" },
        { "RENDER", "ForceRenderShiftX", "0" },
        { "RENDER", "ForceRenderShiftY", "-0.017" },
        { "RENDER", "ForceRoll", "0" },
        { "RENDER", "FixMinimapRendering", "1" },

        { "WORLDRENDER", "EnableWorldRenderTweaks", "0" },
        { "WORLDRENDER", "ShadowmapResolution", "-1" },
        { "WORLDRENDER", "ShadowmapQuality", "-1" },
        { "WORLDRENDER", "ShadowmapViewDistance", "-1" },

        { "TEXTURE", "AnisotropicFiltering", "-1" },

        { "GRAPHICS_VERIFY", "DumpCurrentValues", "0" },

        { "DIAGNOSTICS", "LogNosAwards", "0" },
        { "DIAGNOSTICS", "LogGinsuDiagnostics", "0" },
        { "DIAGNOSTICS", "LogSettingsContainers", "0" },
    };

    bool KeyExists(const char* section, const char* key, const char* iniPath) {
        // Use an unlikely sentinel as the default. An explicitly empty value is
        // still distinguishable from a missing key: present-empty returns "",
        // while absent returns the sentinel.
        static const char sentinel[] = "\x1DNFSTR_MISSING\x1D";
        char value[64];
        value[0] = '\0';
        GetPrivateProfileStringA(section, key, sentinel, value,
                                 static_cast<DWORD>(sizeof(value)), iniPath);
        return std::strcmp(value, sentinel) != 0;
    }
}

namespace Config {
    // Returns the number of keys inserted, 0 if the INI was already complete,
    // or -1 if Windows could not write one of the missing defaults.
    // WritePrivateProfileString updates only the requested key/section, so user
    // values are never replaced and we avoid rebuilding the INI ourselves.
    int EnsureIniDefaults(const std::string& iniPath) {
        int added = 0;
        for (size_t i = 0; i < sizeof(kDefaults) / sizeof(kDefaults[0]); ++i) {
            const DefaultEntry& d = kDefaults[i];
            if (KeyExists(d.section, d.key, iniPath.c_str())) continue;
            if (!WritePrivateProfileStringA(d.section, d.key, d.value, iniPath.c_str())) {
                return -1;
            }
            ++added;
        }
        // Flush the profile API's cache to disk before Config::Load rereads it.
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, iniPath.c_str());
        return added;
    }
}
