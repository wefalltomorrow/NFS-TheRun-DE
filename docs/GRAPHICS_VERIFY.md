# Hidden graphics verification

`graphics-popin-texture-quality` contains a generic verifier for Frostbite renderer settings that are not exposed in Need for Speed: The Run's normal graphics menu.

The verifier reads `[GRAPHICS_VERIFY]` from `NFSTR_DefinitiveEdition.ini`. Each override is written as:

```ini
Class.Field=value
```

Supported reflected classes:

- `GameRender`
- `WorldRender`
- `VisualEnvironment`
- `GlobalPostProcess`
- `VisualTerrain`
- `VegetationSystem`
- `EmitterSystem`
- `DebrisSystem`
- `EnlightenRuntime`
- `Occlusion`
- `Decal`
- `Texture`
- `TextureStreaming`
- `Mesh`
- `MeshStreaming`
- `ShaderSystem`
- `DxDisplay`
- `DebugRender`
- `EffectManager`

The game reflection type is used automatically for bool, int, float, vec2 and vec4 fields. An explicit type can be forced for enum/unknown fields:

```ini
SomeClass.SomeBool=b:1
SomeClass.SomeEnum=i:2
SomeClass.SomeFloat=f:1.5
SomeClass.SomeVec2=v2:1.0,2.0
SomeClass.SomeVec4=v4:1.0,2.0,3.0,4.0
```

`DumpCurrentValues=1` logs the reflected fields and current values for every supported graphics container as it becomes available. World-level containers do not register until an event is loaded, so a full dump requires entering a race.

## Exact upstream re-tests

The fields upstream documented as having no visible effect can now be reproduced without a separate ASI:

```ini
[GRAPHICS_QUALITY]
; Set this to 0 for the exact old MultisampleCount-only test.
AntiAliasing=0

[GRAPHICS_VERIFY]
DumpCurrentValues=0

; Upstream standalone MSAA test:
WorldRender.MultisampleCount=4

; Other upstream no-effect tests:
; WorldRender.ShadowmapSliceCount=4
; GameRender.ViewDistance=f:100000
; GameRender.ForceBlurAmount=f:1.0
; GameRender.DrawFps=b:1
; WorldRender.MotionBlurScale=f:0.0
; WorldRender.MotionBlurQuality=0
; WorldRender.MotionBlurMaxSampleCount=1
; WorldRender.MotionBlurEnable=b:0
```

To compare the old standalone MSAA test with the newer two-part experiment:

1. `AntiAliasing=0` and `WorldRender.MultisampleCount=4` tests the upstream method alone.
2. Remove/comment the `WorldRender.MultisampleCount` override and set `AntiAliasing=4` to enable both `WorldRender.MultisampleCount=4` and `ShaderSystem.DxMultisampleEnable=1` through the native-MSAA feature.

## Pop-in / quality candidates

Useful hidden fields to work through include:

```ini
[GRAPHICS_VERIFY]
; Geometry / culling
; GameRender.ViewDistance=f:100000
; GameRender.EdgeModelLodScale=f:5.0
; GameRender.EdgeModelScreenAreaScale=f:5.0
; WorldRender.CullScreenAreaScale=f:5.0
; Mesh.ForceLod=i:0
; Mesh.GlobalLodScale=f:1000

; Terrain / vegetation
; VisualTerrain.TextureDetailFalloffDistance=f:10000
; VisualTerrain.TextureDetailFalloffFactor=f:0.0
; VisualTerrain.TextureGenerationMipBias=f:0.0
; VisualTerrain.TextureLevelOffset=i:0
; VegetationSystem.MaxActiveDistance=f:500
; VegetationSystem.ShadowDistanceOffset=f:500
; VegetationSystem.ForceShadowLod=i:0

; Texture streaming
; Texture.SkipMipmapCount=i:0
; TextureStreaming.MipmapBias=f:0.0
; TextureStreaming.ForceMipmap=i:0
; TextureStreaming.InstantUnloadingEnable=b:0
; TextureStreaming.PushBasedLoadingEnable=b:1
; TextureStreaming.ForceWantedEnable=b:1

; Mesh streaming
; MeshStreaming.ForceLod=i:0
; MeshStreaming.InstantUnloadingEnable=b:0
; MeshStreaming.PushBasedLoadingEnable=b:1
; MeshStreaming.PrioritizeVisibleMeshes=b:1
; MeshStreaming.PrioritizeVisibleLods=b:1
; MeshStreaming.PrioritizeVisibleLoads=b:1
; MeshStreaming.PrioritizeTextures=b:1
; MeshStreaming.HighestPriority=b:1
; MeshStreaming.PrioritizeNearestPoint=b:1

; Shadows
; WorldRender.ShadowmapResolution=i:4096
; WorldRender.ShadowmapViewDistance=f:600
; WorldRender.ShadowmapSliceCount=i:4
; WorldRender.ShadowmapSliceSchemeWeight=f:0.8
; WorldRender.ShadowmapFirstSliceScale=f:1.0
; WorldRender.ShadowmapMinScreenArea=f:0.0

; AA / sampling
; WorldRender.MultisampleCount=i:4
; ShaderSystem.DxMultisampleEnable=b:1
; ShaderSystem.MaxAnisotropy=i:16
; ShaderSystem.MipmapBias=f:0.0
```

Only test one or a small group of related fields at a time. A reflected field existing in the retail executable does not prove that the renderer still uses it, so every result should be verified in-game and documented as working, no-effect, harmful, or uncertain.
