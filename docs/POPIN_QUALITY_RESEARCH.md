# Pop-in / hidden quality research

This work is intentionally experimental and kept off `main` until each setting is verified in Need for Speed: The Run v1.1.

The retail executable contains substantially more rendering fields than the game's graphics menu exposes. The most relevant families for visible pop-in are `GameRenderSettings`, `WorldRenderSettings`, `MeshSettings`, `MeshStreamingSettings`, `TextureSettings`, `TextureStreamingSettings`, `VisualTerrainSettings`, and `VegetationSystemSettings`.

## First aggressive test pass

The first build deliberately uses large enough changes to make working fields easy to identify. These are research values, not proposed final defaults:

- `GameRender.ViewDistance = 100000` (upstream recorded stock `20000`)
- `GameRender.EdgeModelLodScale = 5`
- `GameRender.EdgeModelScreenAreaScale = 5`
- `WorldRender.CullScreenAreaScale = 5`
- `WorldRender.TerrainLodScale = 5`
- `WorldRender.TerrainLodScaleReflection = 5`
- `WorldRender.TerrainLodScaleShadow = 5`
- `WorldRender.ShadowmapResolution = 4096`
- `WorldRender.ShadowmapViewDistance = 600`
- `WorldRender.ShadowmapQuarterDownsampleEnable = false`
- `VisualTerrain.TextureLevelOffset = 0`
- `VisualTerrain.MeshScatteringDistanceScaleFactor = 5`
- `VisualTerrain.TextureBlockOnStreamingEnable = true`
- `VisualTerrain.TextureKeepPoolFullEnable = true`
- `VegetationSystem.MaxActiveDistance = 450`
- `Texture.SkipMipmapCount = 0`
- `TextureStreaming.InstantUnloadingEnable = false`
- `TextureStreaming.PushBasedLoadingEnable = true`
- `MeshStreaming.ForceLod = 0`
- `MeshStreaming.InstantUnloadingEnable = false`
- all six reflected mesh-streaming priority switches are enabled

Texture/mesh streaming pool sizes and load-count limits are **logged but not changed** in this build. Their units and practical limits need to be observed on the exact game before increasing them; blindly multiplying an allocator budget is an unnecessary crash/OOM risk.

The build also logs, without modifying, the game's environment-map, planar-reflection and spotlight-shadow resolutions so they can be considered as later hidden quality options.

## What to watch

Compare a repeatable stretch of road with the normal graphics build and look for:

- buildings, barriers, signs and roadside props changing geometry or appearing late
- terrain detail transitions
- grass/small scattered-object pop-in
- trees/vegetation appearing or disappearing in the distance
- blurry textures becoming sharp only when approached
- mip transitions while driving quickly
- increased loading stalls, VRAM use, or hitching

A field name in Frostbite reflection is not proof the retail renderer actually consumes it. The log records the original values before overrides so each successful change can be isolated and converted into a clean user-facing setting later.
