# Experimental graphics-quality branch

Branch: `graphics-quality-experiments`

The fast level-loading VSync bypass is already on `main`. This branch is only for the remaining rendering experiments: native MSAA, forced mesh LOD and higher-quality vehicle liveries.

## `[GRAPHICS_QUALITY]`

```ini
[GRAPHICS_QUALITY]
AntiAliasing = 4
FastLoadingVSyncBypass = 1
ForceMeshLod = 0
MeshGlobalLodScale = 1000
VinylTargetSize = 4096
```

The experimental values are defaults on this branch even if the section is absent from the INI.

### AntiAliasing — native Frostbite MSAA

- `0` = off
- `2` = request native 2x MSAA
- `4` = request native 4x MSAA
- `8` = request native 8x MSAA

There is no FXAA/SMAA/post-process fallback here.

Earlier testing showed that changing only `WorldRenderSettings::MultisampleCount` (`+0xB8`) does not visibly enable MSAA. The executable still contains `DxMultisampleEnable`, MSAA render-target names, sample-count-specific depth resources and resolve shaders, so this experiment keeps both of the retail settings enabled:

- `WorldRenderSettings::MultisampleCount` at `+0xB8`
- `ShaderSystemSettings::DxMultisampleEnable` at `+0x102`

Both are reapplied when their settings containers are recreated. If the values stick but the actual render targets remain single-sampled, the next target is Frostbite's render-target creation / `AntiAliasingDeferred` path rather than pretending this setting works.

### FastLoadingVSyncBypass

This is no longer experimental. It is kept in the section for config compatibility, but the implementation is now on `main`.

It removes the hardcoded display/VSync pacing during actual level loading and is separate from FusionFix's `SkipIntro`, which speeds up startup/login/boot flow. Live testing showed a large reduction in event loading time without changing gameplay simulation speed.

### ForceMeshLod / MeshGlobalLodScale

Frostbite `MeshSettings` exposes:

- `ForceLod` at `+0x14`
- `GlobalLodScale` at `+0x18`

`ForceMeshLod=0` requests LOD0. `MeshGlobalLodScale=1000` pushes normal distance-based transitions far away. This affects all meshes, not just cars, so performance and visual side effects still need to be checked.

Set `ForceMeshLod=-1` and `MeshGlobalLodScale=-1` to leave the engine's choices alone.

### VinylTargetSize

`WorldRenderSettings::VinylTargetSize` is an int32 at `+0x90`. The branch holds it at the requested value to test whether the lower-quality liveries seen outside the garage come from the world renderer using a smaller dynamic vinyl target.

`4096` is the initial test value. Set `-1` to leave it untouched.

## Test priorities

For MSAA, verify the log reports both `MultisampleCount -> 4` and `DxMultisampleEnable -> 1`, then compare geometry edges after loading a new event. Also watch GPU memory/performance, excessive distant mesh detail and livery clarity outside the garage.

AA, forced mesh LOD and livery-target changes stay in this draft branch until each one is validated independently.
