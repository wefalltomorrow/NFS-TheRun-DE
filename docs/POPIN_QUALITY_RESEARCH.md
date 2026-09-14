# Pop-in / hidden quality research

This work is intentionally experimental and kept off `main` until each setting is verified in Need for Speed: The Run v1.1.

The retail executable contains substantially more rendering fields than the game's graphics menu exposes. The most relevant families for visible pop-in are `GameRenderSettings`, `WorldRenderSettings`, `MeshSettings`, `MeshStreamingSettings`, `TextureSettings`, `TextureStreamingSettings`, `VisualTerrainSettings`, and `VegetationSystemSettings`.

Current priorities:

- geometry LOD / screen-area culling
- terrain LOD and mesh-scattering distance
- vegetation active distance
- mesh-streaming priorities and unload policy
- texture mip selection / texture-streaming unload policy
- shadow distance and resolution
- optional environment-map / planar-reflection resolution

Do not treat a reflected setting name as proof that the retail renderer uses it. Every option must be tested in-game and the original value is logged before any override is applied.
