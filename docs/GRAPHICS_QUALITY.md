# Experimental graphics-quality branch

Branch: `graphics-aa-lod-livery-loading`

This branch keeps the known-good framerate/GameTime implementation unchanged and adds four independent rendering experiments.

## `[GRAPHICS_QUALITY]`

```ini
[GRAPHICS_QUALITY]
AntiAliasing = 4
FastLoadingVSyncBypass = 1
ForceMeshLod = 0
MeshGlobalLodScale = 1000
VinylTargetSize = 4096
```

The experimental branch uses those values as defaults even when the section is absent, so an existing INI can be used unchanged for the first test.

### AntiAliasing — native Frostbite MSAA

- `0` = off / leave the game's AA path alone
- `2` = request native 2x MSAA
- `4` = request native 4x MSAA (experimental branch default)
- `8` = request native 8x MSAA

There is **no FXAA, SMAA or other post-process AA in this branch**.

Earlier upstream testing wrote only `WorldRenderSettings::MultisampleCount` (`+0xB8`) and saw no visible change. That proves the sample-count field alone is insufficient; it does not prove Frostbite's native MSAA path is absent. Anisotropic filtering is a sampler-state change and can be changed live, whereas MSAA requires the renderer to create and use multisampled color/depth resources and resolve them.

The exact NFS The Run v1.1 executable still contains the native infrastructure: `DxMultisampleEnable`, `MultisampleCount`, `mainTextureMsaa`, `depthTextureMsaa2x/4x/8x`, MSAA resolve programs and sample-count-specific depth shader programs. It also contains live code that reads `ShaderSystemSettings::DxMultisampleEnable` at `+0x102`.

This experiment therefore keeps **both** of these Frostbite settings enabled:

- `WorldRenderSettings::MultisampleCount` at `+0xB8`
- `ShaderSystemSettings::DxMultisampleEnable` at `+0x102`

Both are reapplied when the settings containers become available because the renderer can recreate/reset them during level transitions. If both settings are accepted in the log but the actual render targets remain single-sampled, the next research target is the native render-target creation / `AntiAliasingDeferred` path rather than falling back to post-process AA.

### FastLoadingVSyncBypass

- `0` = off
- `1` = on

Port of the `VSync` patch in mRally2's Framerate Unlocker cheat table. It modifies only the seven verified instructions in the game's hardcoded display/VSync path around `0x004106F8..0x0041076F`. It does not write `GameTime.MaxSimFps`, `MaxVariableFps` or any simulation-rate field.

The community research describes this patch as removing the hardcoded VSync that slows loading. Because the original patch itself is a low-level display-routine patch rather than a named `LoadingScreen` setting, this remains experimental until loading time and normal presentation behaviour are measured in-game.

### ForceMeshLod / MeshGlobalLodScale

Frostbite `MeshSettings` exposes:

- `ForceLod` at `+0x14`
- `GlobalLodScale` at `+0x18`

`ForceMeshLod=0` requests LOD0 (highest mesh detail). `MeshGlobalLodScale=1000` pushes normal distance-based LOD transitions far away so the engine does not immediately select a lower LOD again.

This affects **all meshes**, not only vehicles. Use `ForceMeshLod=-1` and `MeshGlobalLodScale=-1` to leave the engine's mesh-LOD choices alone.

### VinylTargetSize

Frostbite `WorldRenderSettings::VinylTargetSize` is an int32 at `+0x90`. The branch keeps it at the configured size to test whether the lower-quality vehicle liveries seen outside the FrontEnd garage are caused by the game's smaller world-render vinyl target.

`4096` is the initial test value. Set `-1` to leave the engine's value untouched.

## Test priorities

For MSAA, first verify the log reports both `MultisampleCount -> 4` and `DxMultisampleEnable -> 1`, then load a new event and compare geometry edges. Also watch GPU-memory/performance cost, excessive distant object detail, livery clarity outside the garage, loading-time changes and any VSync/presentation side effects. The features remain on this branch until each one is validated independently.
