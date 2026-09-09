#include "features.h"
#include "../memory.h"
#include "../logger.h"
#include <windows.h>
#include <cstdint>

// Experimental high-distance / low-pop-in quality pass for the v1.1 retail exe.
//
// This deliberately lives on a research branch. The values below are fixed test
// values rather than permanent user-facing settings: first prove which Frostbite
// fields actually matter in The Run, then expose only the useful ones in the INI.
// Nothing here touches GameTime or simulation timing.
namespace {
    const uintptr_t kSettingsManagerPtr = 0x2446C74;
    const uintptr_t kGetContainerFn     = 0x0E72D0;

    const uintptr_t kGameRenderTypeInfo       = 0x2AACCBC - 0x400000;
    const uintptr_t kWorldRenderTypeInfo      = 0x2AE6E98 - 0x400000;
    const uintptr_t kVisualTerrainTypeInfo    = 0x2AAF720 - 0x400000;
    const uintptr_t kVegetationTypeInfo       = 0x2AE741C - 0x400000;
    const uintptr_t kTextureTypeInfo          = 0x2AA0A38 - 0x400000;
    const uintptr_t kTextureStreamingTypeInfo = 0x2AA09E0 - 0x400000;

    // Not present in the old curated probe list. Recovered from the exact v1.1
    // executable's static TypeInfo initialiser: c_TypeInfo @ 0x02AA22F4.
    const uintptr_t kMeshStreamingTypeInfo    = 0x2AA22F4 - 0x400000;

    typedef uintptr_t (__fastcall *GetContainerFn)(uintptr_t self, uintptr_t edx, uintptr_t typeInfo);

    uintptr_t ResolveContainer(uintptr_t typeInfoOffset) {
        const uintptr_t base = Memory::GetGameBase();
        if (!base) return 0;
        const uintptr_t mgrAddress = base + kSettingsManagerPtr;
        if (!Memory::IsReadable(mgrAddress, sizeof(uintptr_t))) return 0;
        const uintptr_t mgr = *reinterpret_cast<uintptr_t*>(mgrAddress);
        if (mgr < 0x10000) return 0;
        GetContainerFn fn = reinterpret_cast<GetContainerFn>(base + kGetContainerFn);
        const uintptr_t c = fn(mgr, 0, base + typeInfoOffset);
        return (c >= 0x10000) ? c : 0;
    }

    inline void WriteInt(uintptr_t base, uintptr_t off, int32_t value) {
        if (!Memory::IsReadable(base + off, sizeof(int32_t))) return;
        int32_t* p = reinterpret_cast<int32_t*>(base + off);
        if (*p != value) *p = value;
    }
    inline void WriteFloat(uintptr_t base, uintptr_t off, float value) {
        if (!Memory::IsReadable(base + off, sizeof(float))) return;
        float* p = reinterpret_cast<float*>(base + off);
        if (*p != value) *p = value;
    }
    inline void WriteBool(uintptr_t base, uintptr_t off, bool value) {
        if (!Memory::IsReadable(base + off, sizeof(uint8_t))) return;
        uint8_t* p = reinterpret_cast<uint8_t*>(base + off);
        const uint8_t wanted = value ? 1u : 0u;
        if (*p != wanted) *p = wanted;
    }

    // Fixed research values. These are intentionally aggressive enough that a
    // working field should be visible in an A/B test.
    const float kViewDistance                    = 100000.0f; // stock GameRender value is 20000
    const float kCullScreenAreaScale             = 5.0f;
    const float kEdgeModelLodScale               = 5.0f;
    const float kEdgeModelScreenAreaScale        = 5.0f;
    const float kTerrainLodScale                 = 5.0f;
    const float kVegetationMaxActiveDistance     = 450.0f;
    const float kMeshScatteringDistanceScale     = 5.0f;
    const int   kShadowmapResolution             = 4096;
    const float kShadowmapViewDistance           = 600.0f;

    // GameRenderSettings
    const uintptr_t GR_ViewDistance              = 0x048;
    const uintptr_t GR_EdgeModelLodScale         = 0x02C;
    const uintptr_t GR_EdgeModelScreenAreaScale  = 0x068;

    // WorldRenderSettings. TerrainLodScale* and the reflection fields are present
    // in the retail reflection array beyond the unreliable nominal field count.
    const uintptr_t WR_CullScreenAreaScale       = 0x030;
    const uintptr_t WR_TerrainLodScale           = 0x064;
    const uintptr_t WR_TerrainLodScaleReflection = 0x03C;
    const uintptr_t WR_TerrainLodScaleShadow     = 0x02C;
    const uintptr_t WR_ShadowmapResolution       = 0x044;
    const uintptr_t WR_ShadowmapViewDistance     = 0x058;
    const uintptr_t WR_ShadowQuarterDownsample   = 0x1B5;
    const uintptr_t WR_SkyEnvmapResolution       = 0x168;
    const uintptr_t WR_DynamicEnvmapResolution   = 0x178;
    const uintptr_t WR_PlanarReflectionWidth     = 0x06C;
    const uintptr_t WR_PlanarReflectionHeight    = 0x138;
    const uintptr_t WR_SpotLightShadowResolution = 0x0F0;

    // VisualTerrainSettings
    const uintptr_t VT_TextureLevelOffset             = 0x084;
    const uintptr_t VT_MeshScatteringDistanceScale    = 0x08C;
    const uintptr_t VT_TextureBlockOnStreamingEnable  = 0x131;
    const uintptr_t VT_TextureKeepPoolFullEnable      = 0x141;

    // VegetationSystemSettings
    const uintptr_t VEG_MaxActiveDistance         = 0x024;

    // TextureSettings
    const uintptr_t TEX_SkipMipmapCount            = 0x00C;

    // TextureStreamingSettings
    const uintptr_t TS_OnDemandPoolSize             = 0x00C;
    const uintptr_t TS_PoolSize                     = 0x010;
    const uintptr_t TS_MaxPendingLoadCount          = 0x028;
    const uintptr_t TS_MipmapBias                   = 0x034;
    const uintptr_t TS_InstantUnloadingEnable       = 0x067;
    const uintptr_t TS_PushBasedLoadingEnable       = 0x075;

    // MeshStreamingSettings
    const uintptr_t MS_ForceLod                     = 0x014;
    const uintptr_t MS_PoolHeadroomSize             = 0x028;
    const uintptr_t MS_MaxPendingLoadCount          = 0x034;
    const uintptr_t MS_PoolSize                     = 0x040;
    const uintptr_t MS_InstantUnloadingEnable       = 0x050;
    const uintptr_t MS_PushBasedLoadingEnable       = 0x051;
    const uintptr_t MS_PrioritizeVisibleMeshes      = 0x052;
    const uintptr_t MS_PrioritizeVisibleLods        = 0x053;
    const uintptr_t MS_PrioritizeVisibleLoads       = 0x054;
    const uintptr_t MS_PrioritizeTextures           = 0x055;
    const uintptr_t MS_HighestPriority              = 0x056;
    const uintptr_t MS_PrioritizeNearestPoint       = 0x057;

    bool g_LogGameRender = false;
    bool g_LogWorld = false;
    bool g_LogTerrain = false;
    bool g_LogVegetation = false;
    bool g_LogTexture = false;
    bool g_LogTextureStreaming = false;
    bool g_LogMeshStreaming = false;

    void ApplyGameRender() {
        const uintptr_t c = ResolveContainer(kGameRenderTypeInfo);
        if (!c) return;
        if (!g_LogGameRender) {
            Logger::Log("POPIN-QUALITY GameRender: ViewDistance %.1f -> %.1f, EdgeModelLodScale %.3f -> %.1f, EdgeModelScreenAreaScale %.3f -> %.1f",
                *reinterpret_cast<float*>(c + GR_ViewDistance), kViewDistance,
                *reinterpret_cast<float*>(c + GR_EdgeModelLodScale), kEdgeModelLodScale,
                *reinterpret_cast<float*>(c + GR_EdgeModelScreenAreaScale), kEdgeModelScreenAreaScale);
            g_LogGameRender = true;
        }
        WriteFloat(c, GR_ViewDistance,             kViewDistance);
        WriteFloat(c, GR_EdgeModelLodScale,        kEdgeModelLodScale);
        WriteFloat(c, GR_EdgeModelScreenAreaScale, kEdgeModelScreenAreaScale);
    }

    void ApplyWorld() {
        const uintptr_t c = ResolveContainer(kWorldRenderTypeInfo);
        if (!c) return;
        if (!g_LogWorld) {
            Logger::Log("POPIN-QUALITY WorldRender: CullScreenAreaScale %.3f -> %.1f, TerrainLodScale %.3f -> %.1f, TerrainReflection %.3f -> %.1f, TerrainShadow %.3f -> %.1f",
                *reinterpret_cast<float*>(c + WR_CullScreenAreaScale), kCullScreenAreaScale,
                *reinterpret_cast<float*>(c + WR_TerrainLodScale), kTerrainLodScale,
                *reinterpret_cast<float*>(c + WR_TerrainLodScaleReflection), kTerrainLodScale,
                *reinterpret_cast<float*>(c + WR_TerrainLodScaleShadow), kTerrainLodScale);
            Logger::Log("POPIN-QUALITY WorldRender stock: Shadowmap=%d @ %.1f, quarterDownsample=%u, SkyEnvmap=%d, DynamicEnvmap=%d, Planar=%dx%d, SpotShadow=%d",
                *reinterpret_cast<int32_t*>(c + WR_ShadowmapResolution),
                *reinterpret_cast<float*>(c + WR_ShadowmapViewDistance),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + WR_ShadowQuarterDownsample)),
                *reinterpret_cast<int32_t*>(c + WR_SkyEnvmapResolution),
                *reinterpret_cast<int32_t*>(c + WR_DynamicEnvmapResolution),
                *reinterpret_cast<int32_t*>(c + WR_PlanarReflectionWidth),
                *reinterpret_cast<int32_t*>(c + WR_PlanarReflectionHeight),
                *reinterpret_cast<int32_t*>(c + WR_SpotLightShadowResolution));
            g_LogWorld = true;
        }
        WriteFloat(c, WR_CullScreenAreaScale,       kCullScreenAreaScale);
        WriteFloat(c, WR_TerrainLodScale,           kTerrainLodScale);
        WriteFloat(c, WR_TerrainLodScaleReflection, kTerrainLodScale);
        WriteFloat(c, WR_TerrainLodScaleShadow,     kTerrainLodScale);
        WriteInt(c,   WR_ShadowmapResolution,       kShadowmapResolution);
        WriteFloat(c, WR_ShadowmapViewDistance,     kShadowmapViewDistance);
        WriteBool(c,  WR_ShadowQuarterDownsample,   false);
    }

    void ApplyTerrain() {
        const uintptr_t c = ResolveContainer(kVisualTerrainTypeInfo);
        if (!c) return;
        if (!g_LogTerrain) {
            Logger::Log("POPIN-QUALITY VisualTerrain: TextureLevelOffset=%d, MeshScatteringDistanceScale %.3f -> %.1f, BlockOnStreaming=%u, KeepPoolFull=%u",
                *reinterpret_cast<int32_t*>(c + VT_TextureLevelOffset),
                *reinterpret_cast<float*>(c + VT_MeshScatteringDistanceScale), kMeshScatteringDistanceScale,
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + VT_TextureBlockOnStreamingEnable)),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + VT_TextureKeepPoolFullEnable)));
            g_LogTerrain = true;
        }
        WriteInt(c,   VT_TextureLevelOffset,            0);
        WriteFloat(c, VT_MeshScatteringDistanceScale,   kMeshScatteringDistanceScale);
        WriteBool(c,  VT_TextureBlockOnStreamingEnable, true);
        WriteBool(c,  VT_TextureKeepPoolFullEnable,     true);
    }

    void ApplyVegetation() {
        const uintptr_t c = ResolveContainer(kVegetationTypeInfo);
        if (!c) return;
        if (!g_LogVegetation) {
            Logger::Log("POPIN-QUALITY Vegetation: MaxActiveDistance %.1f -> %.1f",
                *reinterpret_cast<float*>(c + VEG_MaxActiveDistance), kVegetationMaxActiveDistance);
            g_LogVegetation = true;
        }
        WriteFloat(c, VEG_MaxActiveDistance, kVegetationMaxActiveDistance);
    }

    void ApplyTexture() {
        const uintptr_t c = ResolveContainer(kTextureTypeInfo);
        if (!c) return;
        if (!g_LogTexture) {
            Logger::Log("POPIN-QUALITY Texture: SkipMipmapCount %d -> 0",
                *reinterpret_cast<int32_t*>(c + TEX_SkipMipmapCount));
            g_LogTexture = true;
        }
        WriteInt(c, TEX_SkipMipmapCount, 0);
    }

    void ApplyTextureStreaming() {
        const uintptr_t c = ResolveContainer(kTextureStreamingTypeInfo);
        if (!c) return;
        if (!g_LogTextureStreaming) {
            Logger::Log("POPIN-QUALITY TextureStreaming stock: PoolSize=%d OnDemand=%d MaxPending=%d MipmapBias=%.3f InstantUnload=%u PushBased=%u",
                *reinterpret_cast<int32_t*>(c + TS_PoolSize),
                *reinterpret_cast<int32_t*>(c + TS_OnDemandPoolSize),
                *reinterpret_cast<int32_t*>(c + TS_MaxPendingLoadCount),
                *reinterpret_cast<float*>(c + TS_MipmapBias),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + TS_InstantUnloadingEnable)),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + TS_PushBasedLoadingEnable)));
            g_LogTextureStreaming = true;
        }
        // Keep already-loaded texture data around instead of immediately throwing
        // it away, and use Frostbite's push-based loading path. Pool sizes are only
        // logged in this first build: guessing their units/caps is not worth a crash.
        WriteBool(c, TS_InstantUnloadingEnable, false);
        WriteBool(c, TS_PushBasedLoadingEnable, true);
    }

    void ApplyMeshStreaming() {
        const uintptr_t c = ResolveContainer(kMeshStreamingTypeInfo);
        if (!c) return;
        if (!g_LogMeshStreaming) {
            Logger::Log("POPIN-QUALITY MeshStreaming stock: ForceLod=%d PoolSize=%d Headroom=%d MaxPending=%d InstantUnload=%u PushBased=%u VisibleMeshes=%u VisibleLods=%u VisibleLoads=%u Textures=%u Highest=%u Nearest=%u",
                *reinterpret_cast<int32_t*>(c + MS_ForceLod),
                *reinterpret_cast<int32_t*>(c + MS_PoolSize),
                *reinterpret_cast<int32_t*>(c + MS_PoolHeadroomSize),
                *reinterpret_cast<int32_t*>(c + MS_MaxPendingLoadCount),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + MS_InstantUnloadingEnable)),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + MS_PushBasedLoadingEnable)),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + MS_PrioritizeVisibleMeshes)),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + MS_PrioritizeVisibleLods)),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + MS_PrioritizeVisibleLoads)),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + MS_PrioritizeTextures)),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + MS_HighestPriority)),
                static_cast<unsigned>(*reinterpret_cast<uint8_t*>(c + MS_PrioritizeNearestPoint)));
            g_LogMeshStreaming = true;
        }
        WriteInt(c,  MS_ForceLod,                0);
        WriteBool(c, MS_InstantUnloadingEnable, false);
        WriteBool(c, MS_PushBasedLoadingEnable, true);
        WriteBool(c, MS_PrioritizeVisibleMeshes,true);
        WriteBool(c, MS_PrioritizeVisibleLods,  true);
        WriteBool(c, MS_PrioritizeVisibleLoads, true);
        WriteBool(c, MS_PrioritizeTextures,     true);
        WriteBool(c, MS_HighestPriority,        true);
        WriteBool(c, MS_PrioritizeNearestPoint, true);
    }
}

namespace Features {
    void UpdatePopInQuality() {
        ApplyGameRender();
        ApplyWorld();
        ApplyTerrain();
        ApplyVegetation();
        ApplyTexture();
        ApplyTextureStreaming();
        ApplyMeshStreaming();
    }
}
