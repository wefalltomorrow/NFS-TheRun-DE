# Experimental graphics-quality branch

Branch: `graphics-aa-lod-livery-loading`

This branch keeps the known-good framerate/GameTime implementation unchanged and adds four independent rendering experiments.

## `[GRAPHICS_QUALITY]`

```ini
[GRAPHICS_QUALITY]
AntiAliasing = 2
FastLoadingVSyncBypass = 1
ForceMeshLod = 0
MeshGlobalLodScale = 1000
VinylTargetSize = 4096
```

The experimental branch uses those values as defaults even when the section is absent, so an existing INI can be used unchanged for the first test.

### AntiAliasing

- `0` = off
- `1` = FXAA
- `2` = higher-quality/wider FXAA search

The game's reflected `WorldRenderSettings.MultisampleCount` field was already wired up and tested by the upstream project and has no visible effect in the retail renderer. This option therefore does **not** pretend to enable native MSAA. It hooks the D3D11 swap-chain `Present` path and performs a real post-process FXAA pass on the final back buffer.

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

Watch for GPU-memory/performance cost, excessive distant object detail, livery clarity outside the garage, loading-time changes, and any overlay/UI softness introduced by the final-frame FXAA pass. The features remain on this branch until each one is validated independently.
