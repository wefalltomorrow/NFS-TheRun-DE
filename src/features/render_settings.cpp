#include "features.h"
#include "../config.h"
#include "../memory.h"
#include "../logger.h"
#include <windows.h>
#include <cstdint>

// fb::GameRenderSettings tweaks.
//
// The engine keeps its settings objects in a table of cached pointers. The
// accessor at 0x00418010 shows how GameRenderSettings is fetched:
//     container = dword_2753F28;
//     if (!container)
//         container = fb::SettingsManager::getContainer(g_settingsManager,
//                                                       &GameRenderSettings::c_TypeInfo);
// so the live object is simply *[0x02753F28] (module +0x2353F28). GameTimeSettings
// sits 8 bytes earlier in the same table at [0x02753F20].
//
// The pointer is populated lazily, so this runs from the ticker rather than at
// startup, and each field is only written when it differs from what's there.
//
// Field offsets confirmed live in ReClass against the running game:
//   0x0C float ForceRoll
//   0x14 float ForceRenderShiftX
//   0x1C float ForceBlurAmount
//   0x20 float ForceFov                 (-1 = engine default, which is 48)
//   0x34 float ForceRenderShiftY
//   0x48 float ViewDistance
//   0x77 bool  InitialClearEnable
//   0x7B bool  DrawFps
//   0x8B bool  ForceRenderShiftEnabled  (gates ForceRenderShiftX/Y)

// Validated control state, maintained by fps_unlocker.cpp: -1 unknown, 0 no
// control, 1 driving. Read this rather than the raw byte the hook captures — that
// pointer goes stale when the object behind it is freed, and a garbage read there
// would apply the driving FOV in the menus.
extern "C" int PlayerControlState();

namespace {
    const uintptr_t kSettingsPtrOffset = 0x2353F28; // -> [0x02753F28]

    // What the engine itself stores in ForceFov: -1 means "no override".
    const float kFovDisabled = -1.0f;
    const float kVerifyFloatDisabled = -9000.0f;

    const uintptr_t kOffForceRoll         = 0x0C;
    const uintptr_t kOffForceRenderShiftX = 0x14;
    const uintptr_t kOffForceBlurAmount   = 0x1C;
    const uintptr_t kOffForceFov          = 0x20;
    const uintptr_t kOffForceRenderShiftY = 0x34;
    const uintptr_t kOffViewDistance      = 0x48;
    const uintptr_t kOffInitialClear      = 0x77;
    const uintptr_t kOffDrawFps           = 0x7B;
    const uintptr_t kOffForceShiftEnabled = 0x8B;

    bool g_LoggedApply = false;
    bool g_LoggedGameRenderVerify = false;

    inline bool VerifyFloatEnabled(float v) {
        return v > kVerifyFloatDisabled;
    }

    inline void WriteFloat(uintptr_t base, uintptr_t off, float v) {
        float* p = reinterpret_cast<float*>(base + off);
        if (*p != v) *p = v;
    }
    inline void WriteBool(uintptr_t base, uintptr_t off, bool v) {
        uint8_t* p = reinterpret_cast<uint8_t*>(base + off);
        uint8_t want = v ? 1 : 0;
        if (*p != want) *p = want;
    }
}

// ---------------------------------------------------------------------------
// Shared SettingsManager resolver for the other Frostbite settings containers.
namespace {
    const uintptr_t kSettingsManagerPtr   = 0x2446C74; // fb::g_settingsManager
    const uintptr_t kGetContainerFn       = 0x0E72D0;  // SettingsManager::getContainer
    const uintptr_t kWorldRenderTypeInfo  = 0x2AE6E98 - 0x400000;
    const uintptr_t kMeshTypeInfo         = 0x2AA22C8 - 0x400000;

    typedef uintptr_t (__fastcall *GetContainerFn)(uintptr_t self, uintptr_t edx, uintptr_t typeInfo);

    // Resolves a settings container by its c_TypeInfo module offset. Returns 0
    // until the manager exists and that class has been registered, which for the
    // render classes means "not until a level is loaded".
    uintptr_t ResolveContainer(uintptr_t typeInfoOffset) {
        uintptr_t gameBase = Memory::GetGameBase();
        uintptr_t mgr = *reinterpret_cast<uintptr_t*>(gameBase + kSettingsManagerPtr);
        if (mgr < 0x10000) return 0;

        GetContainerFn getContainer =
            reinterpret_cast<GetContainerFn>(gameBase + kGetContainerFn);
        uintptr_t c = getContainer(mgr, 0, gameBase + typeInfoOffset);
        return (c < 0x10000) ? 0 : c;
    }

    inline void WriteInt(uintptr_t base, uintptr_t off, int v) {
        int32_t* p = reinterpret_cast<int32_t*>(base + off);
        if (*p != v) *p = v;
    }
}

// ---------------------------------------------------------------------------
// fb::WorldRenderSettings — normal shipped tweaks plus the upstream "no effect"
// fields exposed under [UPSTREAM_VERIFY]. The verification writes deliberately
// stay resident across level transitions, because the renderer reads several of
// these fields while setting a level up rather than continuously mid-race.
namespace {
    const uintptr_t kOffShadowmapResolution       = 0x044;
    const uintptr_t kOffShadowmapQuality          = 0x048;
    const uintptr_t kOffShadowmapSliceCount       = 0x04C;
    const uintptr_t kOffShadowmapViewDistance     = 0x058;
    const uintptr_t kOffVinylTargetSize           = 0x090;
    const uintptr_t kOffMotionBlurScale           = 0x098;
    const uintptr_t kOffMotionBlurQuality         = 0x0A0;
    const uintptr_t kOffMotionBlurMaxSampleCount  = 0x0AC;
    const uintptr_t kOffMultisampleCount          = 0x0B8;
    const uintptr_t kOffMotionBlurEnable          = 0x1AE;

    bool g_LoggedWorld = false;
    bool g_LoggedWorldVerify = false;
    bool g_LoggedRawMsaaConflict = false;

    bool WantWorldVerification() {
        return g_Config.TestRawMultisampleCount >= 0 ||
               g_Config.TestShadowmapSliceCount >= 0 ||
               VerifyFloatEnabled(g_Config.TestMotionBlurScale) ||
               g_Config.TestMotionBlurQuality >= 0 ||
               g_Config.TestMotionBlurMaxSampleCount >= 0 ||
               g_Config.TestMotionBlurEnable >= 0;
    }

    void ApplyWorldRender() {
        const bool wantWorldTweaks = g_Config.EnableWorldRenderTweaks != 0;
        const bool wantVinyl = g_Config.VinylTargetSize > 0;
        const bool wantVerify = WantWorldVerification();
        if (!wantWorldTweaks && !wantVinyl && !wantVerify) return;

        uintptr_t w = ResolveContainer(kWorldRenderTypeInfo);
        if (!w) return;

        if (!g_LoggedWorld) {
            Logger::Log("World render tweaks: WorldRenderSettings at 0x%08X (VinylTargetSize was %d).",
                        w, *reinterpret_cast<int32_t*>(w + kOffVinylTargetSize));
            g_LoggedWorld = true;
        }

        if (wantWorldTweaks) {
            if (g_Config.ShadowmapResolution   > 0) WriteInt(w, kOffShadowmapResolution,   g_Config.ShadowmapResolution);
            if (g_Config.ShadowmapQuality      >= 0) WriteInt(w, kOffShadowmapQuality,     g_Config.ShadowmapQuality);
            if (g_Config.ShadowmapViewDistance > 0.0f) WriteFloat(w, kOffShadowmapViewDistance, g_Config.ShadowmapViewDistance);
        }

        // The garage uses a high-quality dynamic vinyl target while normal-world
        // rendering can choose a smaller one. Keep the target at the requested
        // size so liveries do not fall back to the lower-quality outside-garage
        // render target. The field comes directly from Frostbite reflection data.
        if (wantVinyl) {
            WriteInt(w, kOffVinylTargetSize, g_Config.VinylTargetSize);
        }

        if (wantVerify) {
            if (!g_LoggedWorldVerify) {
                Logger::Log("UPSTREAM_VERIFY WorldRender before writes: SliceCount=%d MultisampleCount=%d MotionBlurScale=%.3f MotionBlurQuality=%d MotionBlurMaxSamples=%d MotionBlurEnable=%u.",
                            *reinterpret_cast<int32_t*>(w + kOffShadowmapSliceCount),
                            *reinterpret_cast<int32_t*>(w + kOffMultisampleCount),
                            *reinterpret_cast<float*>(w + kOffMotionBlurScale),
                            *reinterpret_cast<int32_t*>(w + kOffMotionBlurQuality),
                            *reinterpret_cast<int32_t*>(w + kOffMotionBlurMaxSampleCount),
                            static_cast<unsigned>(*reinterpret_cast<uint8_t*>(w + kOffMotionBlurEnable)));
                g_LoggedWorldVerify = true;
            }

            if (g_Config.TestShadowmapSliceCount >= 0)
                WriteInt(w, kOffShadowmapSliceCount, g_Config.TestShadowmapSliceCount);

            if (g_Config.TestRawMultisampleCount >= 0) {
                if (g_Config.AntiAliasing > 1 && !g_LoggedRawMsaaConflict) {
                    Logger::Log("UPSTREAM_VERIFY warning: TestRawMultisampleCount is active while AntiAliasing=%d also enables DxMultisampleEnable. Set AntiAliasing=0 to reproduce upstream's standalone MultisampleCount test exactly.", g_Config.AntiAliasing);
                    g_LoggedRawMsaaConflict = true;
                }
                WriteInt(w, kOffMultisampleCount, g_Config.TestRawMultisampleCount);
            }

            if (VerifyFloatEnabled(g_Config.TestMotionBlurScale))
                WriteFloat(w, kOffMotionBlurScale, g_Config.TestMotionBlurScale);

            if (g_Config.TestMotionBlurQuality >= 0)
                WriteInt(w, kOffMotionBlurQuality, g_Config.TestMotionBlurQuality);

            if (g_Config.TestMotionBlurMaxSampleCount >= 0)
                WriteInt(w, kOffMotionBlurMaxSampleCount, g_Config.TestMotionBlurMaxSampleCount);

            if (g_Config.TestMotionBlurEnable >= 0)
                WriteBool(w, kOffMotionBlurEnable, g_Config.TestMotionBlurEnable != 0);
        }
    }
}

// ---------------------------------------------------------------------------
// fb::MeshSettings — force the highest mesh LOD and push transitions away.
//
// Frostbite exposes both ForceLod and GlobalLodScale. ForceLod=0 requests LOD0;
// GlobalLodScale=1000 is the established Frostbite high-detail configuration and
// prevents the normal distance scaler immediately selecting a lower LOD again.
// This affects all meshes, not only vehicles; the option is deliberately kept in
// the experimental graphics section until its cost and side effects are tested.
namespace {
    const uintptr_t kOffForceLod       = 0x014;
    const uintptr_t kOffGlobalLodScale = 0x018;
    bool g_LoggedMesh = false;

    void ApplyMeshQuality() {
        if (g_Config.ForceMeshLod < 0 && g_Config.MeshGlobalLodScale <= 0.0f) return;

        uintptr_t m = ResolveContainer(kMeshTypeInfo);
        if (!m) return;

        if (!g_LoggedMesh) {
            Logger::Log("Mesh quality: MeshSettings at 0x%08X, ForceLod %d -> %d, GlobalLodScale %.3f -> %.3f.",
                        m,
                        *reinterpret_cast<int32_t*>(m + kOffForceLod), g_Config.ForceMeshLod,
                        *reinterpret_cast<float*>(m + kOffGlobalLodScale), g_Config.MeshGlobalLodScale);
            g_LoggedMesh = true;
        }

        if (g_Config.ForceMeshLod >= 0) {
            WriteInt(m, kOffForceLod, g_Config.ForceMeshLod);
        }
        if (g_Config.MeshGlobalLodScale > 0.0f) {
            WriteFloat(m, kOffGlobalLodScale, g_Config.MeshGlobalLodScale);
        }
    }
}

// ---------------------------------------------------------------------------
// fb::ShaderSystemSettings — anisotropic filtering.
//
// MaxAnisotropy is an int32 at 0x94, confirmed against the game's own reflection
// data and read live in ReClass. It ships at 4 and the engine writes it back to 4
// every time a level loads, so this has to be reapplied rather than set once; the
// ticker does that for free.
namespace {
    const uintptr_t kShaderSystemTypeInfo = 0x2AA3428 - 0x400000;
    const uintptr_t kOffMaxAnisotropy     = 0x94;

    bool g_LoggedShader = false;

    void ApplyShaderSystem() {
        if (g_Config.AnisotropicFiltering < 0) return;

        uintptr_t s = ResolveContainer(kShaderSystemTypeInfo);
        if (!s) return;

        if (!g_LoggedShader) {
            Logger::Log("Anisotropic filtering: ShaderSystemSettings at 0x%08X, MaxAnisotropy -> %d (was %d).",
                        s, g_Config.AnisotropicFiltering,
                        *reinterpret_cast<int32_t*>(s + kOffMaxAnisotropy));
            g_LoggedShader = true;
        }

        WriteInt(s, kOffMaxAnisotropy, g_Config.AnisotropicFiltering);
    }
}

namespace {
    void ApplyGameRenderVerification() {
        const bool wantVerify = VerifyFloatEnabled(g_Config.TestForceBlurAmount) ||
                                g_Config.TestViewDistance >= 0.0f ||
                                g_Config.TestDrawFps >= 0;
        if (!wantVerify) return;

        uintptr_t slot = Memory::GetGameBase() + kSettingsPtrOffset;
        uintptr_t settings = *reinterpret_cast<uintptr_t*>(slot);
        if (settings < 0x10000) return;

        if (!g_LoggedGameRenderVerify) {
            Logger::Log("UPSTREAM_VERIFY GameRender before writes: ForceBlurAmount=%.3f ViewDistance=%.1f DrawFps=%u.",
                        *reinterpret_cast<float*>(settings + kOffForceBlurAmount),
                        *reinterpret_cast<float*>(settings + kOffViewDistance),
                        static_cast<unsigned>(*reinterpret_cast<uint8_t*>(settings + kOffDrawFps)));
            g_LoggedGameRenderVerify = true;
        }

        if (VerifyFloatEnabled(g_Config.TestForceBlurAmount))
            WriteFloat(settings, kOffForceBlurAmount, g_Config.TestForceBlurAmount);
        if (g_Config.TestViewDistance >= 0.0f)
            WriteFloat(settings, kOffViewDistance, g_Config.TestViewDistance);
        if (g_Config.TestDrawFps >= 0)
            WriteBool(settings, kOffDrawFps, g_Config.TestDrawFps != 0);
    }
}

namespace Features {
    void UpdateRenderSettings() {
        ApplyWorldRender();
        ApplyMeshQuality();
        ApplyShaderSystem();
        ApplyGameRenderVerification();

        if (!g_Config.EnableRenderTweaks) return;

        uintptr_t slot = Memory::GetGameBase() + kSettingsPtrOffset;
        uintptr_t settings = *reinterpret_cast<uintptr_t*>(slot);
        if (settings < 0x10000) return; // not populated yet

        if (!g_LoggedApply) {
            Logger::Log("Render tweaks: GameRenderSettings at 0x%08X (via [0x%08X]).", settings, slot);
            g_LoggedApply = true;
        }

        // ForceFov applies globally, including the garage, car select and menus,
        // where a widened view is not wanted. Gate it on the same vehicle-control
        // flag the sim-rate clamp uses, so the override is live while driving and
        // released back to the engine's own FOV everywhere else. If the control
        // hook was never installed we cannot tell, so the override just stays on.
        if (g_Config.ForceFov > 0.0f) {
            bool applyFov = true;
            if (g_Config.ForceFovOnlyWhileDriving) {
                int state = PlayerControlState();
                if (state >= 0) applyFov = (state != 0);   // unknown: leave it on
            }
            WriteFloat(settings, kOffForceFov, applyFov ? g_Config.ForceFov : kFovDisabled);
        }

        // Viewport shift. The enable flag gates both axes; on its own it lifts the
        // chase camera's viewport slightly.
        WriteBool(settings, kOffForceShiftEnabled, g_Config.ForceRenderShiftEnabled != 0);
        if (g_Config.ForceRenderShiftEnabled) {
            WriteFloat(settings, kOffForceRenderShiftX, g_Config.ForceRenderShiftX);
            WriteFloat(settings, kOffForceRenderShiftY, g_Config.ForceRenderShiftY);
        }

        // Camera roll works independently of the shift enable.
        WriteFloat(settings, kOffForceRoll, g_Config.ForceRoll);

        // InitialClearEnable: clears the render target each frame. Fixes the minimap
        // rendering glitchy, invisible or missing road segments on some events.
        if (g_Config.FixMinimapRendering) {
            WriteBool(settings, kOffInitialClear, true);
        }
    }
}
