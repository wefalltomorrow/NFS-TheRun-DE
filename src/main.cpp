#include <windows.h>
#include <cstring>
#include <string>
#include "logger.h"
#include "config.h"
#include "features/features.h"

namespace Config {
    int EnsureIniDefaults(const std::string& iniPath);
}

static HMODULE g_hModule = NULL;

namespace {
    bool IniKeyExists(const char* section, const char* key, const std::string& iniPath) {
        static const char sentinel[] = "\x1DNFSTR_MISSING\x1D";
        char value[64] = {};
        GetPrivateProfileStringA(section, key, sentinel, value,
                                 static_cast<DWORD>(sizeof(value)), iniPath.c_str());
        return std::strcmp(value, sentinel) != 0;
    }

    int EnsureResearchDefaults(const std::string& iniPath) {
        int added = 0;
        struct Entry { const char* key; const char* value; };
        const Entry entries[] = {
            { "EnableNativeMSAAResearch", "0" },
            { "EnablePopInQualityResearch", "0" },
        };
        for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
            if (IniKeyExists("GRAPHICS_RESEARCH", entries[i].key, iniPath)) continue;
            if (!WritePrivateProfileStringA("GRAPHICS_RESEARCH", entries[i].key,
                                            entries[i].value, iniPath.c_str())) return -1;
            ++added;
        }

        // This branch is being tested on a deliberately offline installation, so
        // make the offline-service suppression visible and enabled automatically.
        // Existing values are still never overwritten; set it to 0 if online play
        // is ever wanted again.
        if (!IniKeyExists("OFFLINE", "DisableOnlineServices", iniPath)) {
            if (!WritePrivateProfileStringA("OFFLINE", "DisableOnlineServices", "1", iniPath.c_str())) return -1;
            ++added;
        }

        WritePrivateProfileStringA(nullptr, nullptr, nullptr, iniPath.c_str());
        return added;
    }
}

DWORD WINAPI MainThread(LPVOID /*lpParam*/) {
    // Determine path of the DLL to locate the INI file alongside it
    char dllPath[MAX_PATH];
    GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);

    std::string pathStr(dllPath);
    size_t lastSlash = pathStr.find_last_of("\\/");
    std::string baseDir = (lastSlash != std::string::npos) ? pathStr.substr(0, lastSlash + 1) : "";

    std::string iniPath = baseDir + "NFSTR_DefinitiveEdition.ini";
    std::string logPath = baseDir + "NFSTR_DefinitiveEdition.log";

    // Keep an existing INI up to date as test builds add settings. Existing keys
    // and values are never overwritten; only genuinely missing defaults are added.
    const int iniDefaultsAdded = Config::EnsureIniDefaults(iniPath);
    const int researchDefaultsAdded = EnsureResearchDefaults(iniPath);

    // 1. Load Configuration
    Config::Load(iniPath);

    // The two paths below are still research and both first become active while a
    // level renderer is being constructed. Keep them independently gated so a bad
    // level-load can be bisected without swapping ASIs. AntiAliasing may still say
    // 4 in an older INI; it is intentionally ignored unless the research gate is on.
    const bool enableNativeMsaaResearch =
        GetPrivateProfileIntA("GRAPHICS_RESEARCH", "EnableNativeMSAAResearch", 0, iniPath.c_str()) != 0;
    const bool enablePopInQualityResearch =
        GetPrivateProfileIntA("GRAPHICS_RESEARCH", "EnablePopInQualityResearch", 0, iniPath.c_str()) != 0;
    const bool disableOnlineServices =
        GetPrivateProfileIntA("OFFLINE", "DisableOnlineServices", 1, iniPath.c_str()) != 0;
    if (!enableNativeMsaaResearch) g_Config.AntiAliasing = 0;

    // 2. Initialize Logger, then log the config (LogSummary must run after Init,
    //    or its output is dropped because the logger isn't ready during Load).
    Logger::Init(logPath, g_Config.DebugLog != 0);
    Logger::Log("NFSTR_DefinitiveEdition thread started.");
    const int totalDefaultsAdded =
        ((iniDefaultsAdded > 0) ? iniDefaultsAdded : 0) +
        ((researchDefaultsAdded > 0) ? researchDefaultsAdded : 0);
    if (totalDefaultsAdded > 0) {
        Logger::Log("INI updater added %d missing setting(s) without changing existing values.", totalDefaultsAdded);
    }
    if (iniDefaultsAdded < 0 || researchDefaultsAdded < 0) {
        Logger::Log("INI updater could not write one or more missing settings; existing values were left unchanged.");
    }
    Config::LogSummary();
    Logger::Log("Graphics research gates: NativeMSAA=%d  PopInQuality=%d",
                enableNativeMsaaResearch ? 1 : 0,
                enablePopInQualityResearch ? 1 : 0);
    Logger::Log("Offline services: DisableOnlineServices=%d", disableOnlineServices ? 1 : 0);
    if (!enableNativeMsaaResearch) {
        Logger::Log("Native MSAA research is gated OFF; [GRAPHICS_QUALITY] AntiAliasing is ignored for this run.");
    }

    // 3. Initialize Features
    // Put offline suppression first so it can take ownership as soon as
    // NfsOnlineSettings is registered by the engine.
    Features::InitOfflineMode(disableOnlineServices);
    Features::InitGarageCarRender();
    Features::InitExtraUIOptions();
    Features::InitPhotoMode();
    Features::InitTrackRules();
    Features::InitTrafficControls();
    Features::InitEngineAudioSlewFix();
    Features::InitParticleFix();
    Features::InitInputStateHook();
    Features::InitAiDifficulty();
    Features::InitNosTuning();
    Features::InitTodRandomizer();
    Features::InitGinsuDiagnostics();

    // Graphics-quality work stays completely separate from GameTime. The loading
    // patch modifies only the game's hardcoded VSync display routine. Native MSAA
    // uses Frostbite's own renderer settings; no Present/post-process hook exists.
    Features::InitLoadingVSyncOptimization();
    Features::InitAntiAliasing();
    Features::InitGraphicsVerification(iniPath.c_str());

    Features::InitFramerateUnlocker();

    Logger::Log("All features initialized successfully.");

    // 4. Background Ticker Loop
    while (true) {
        Features::UpdateOfflineMode();   // apply as soon as NfsOnlineSettings exists
        Features::UpdateFramerateUnlocker();
        Features::UpdateParticleFix();
        Features::UpdateDifficulty();     // decides whether the mode is engaged
        Features::UpdateInputState();     // reads that decision, so it runs after
        Features::UpdateAiDifficulty();
        Features::UpdateTrackRules();     // reads that decision, so it runs after
        Features::UpdateTrafficControls();
        Features::UpdateNosTuning();
        Features::UpdateTodRandomizer();
        Features::UpdateDifficultyText();
        Features::UpdatePlayerVehicle();  // reads that decision, so it runs after
        Features::UpdateAntiAliasing();   // no-op when native-MSAA research gate is off
        if (enablePopInQualityResearch) {
            Features::UpdatePopInQuality();
        }
        Features::UpdateRenderSettings(); // normal named graphics settings
        // Explicit research overrides run last so an A/B test can override any
        // earlier high-quality default without requiring another ASI build.
        Features::UpdateGraphicsVerification();
        Features::UpdateSettingsProbe();
        Sleep(16); // ~60 Hz tick
    }

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*lpvReserved*/) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        Logger::Close();
        break;
    }
    return TRUE;
}
