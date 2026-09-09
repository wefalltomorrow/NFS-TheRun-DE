# Intermittent menu text corruption

Observed in the retail PC v1.1.0.0 executable before the experimental hidden-graphics changes.

## Symptom

Text-heavy non-gameplay menus can intermittently render with glyph fragments duplicated, overwritten or composited into the wrong label while menu images/icons and some text remain correct. A captured example occurred in Challenge Series at 60 FPS.

Do not attribute this to the new MSAA/LOD/streaming experiments: the user observed it before those changes.

## Strong lead: Scaleform GFx glyph cache

The exact v1.1 executable contains Scaleform GFx font/rendering infrastructure, including:

- `GFxFontCacheManager`
- `gfxfontlib.swf`
- `_c4/UI/Fontlib_%s.gfx`
- `ShaderProgram_Ui_Scaleform_GlyphFilterShadow`
- `ShaderProgram_Ui_Scaleform_GlyphFilterBlur`
- `ShaderProgram_Ui_Scaleform_GlyphTextTextureColor`
- `ShaderProgram_Ui_Scaleform_GlyphTextTextureColorMultiply`
- `ShaderProgram_Ui_Scaleform_GlyphTextTextureAlpha`

More importantly, the executable contains its own diagnostic warnings:

- `Warning: Increase vector glyph cache capacity - SetMaxVectorCacheSize().`
- `Warning: Increase raster glyph cache capacity - TextureConfig.`

The vector-cache warning is referenced around `0x0159FF5F` in the exact v1.1 EXE. The raster-cache warning is referenced in the Scaleform glyph allocation path around `0x015A2533` (and another nearby path).

That does not yet prove cache exhaustion is the cause, but it is a much better fit for fragmented/reused glyphs than a missing Windows font.

Scaleform's own documentation says corrupted/lost glyphs or text effects can result when the font cache overflows, and recommends checking frame bracketing plus increasing glyph-cache texture capacity / texture count.

A separate RenoDX mod for The Run independently lists `Text glitching when many are present. (Only non-gameplay menus.)` as a known issue, which is notably similar to this symptom.

## Next reverse-engineering targets

1. Locate The Run's `GFxFontCacheManager::TextureConfig` initialization and log the stock texture width/height, texture count, max slot height and padding.
2. Locate the call that sets maximum vector-cache size and log the stock capacity.
3. Build a diagnostic-only test that increases raster/vector cache capacity without modifying any world graphics settings.
4. A/B test the same text-heavy Challenge Series screen at stock 30-FPS menus and unlocked menus. High menu FPS is a possible amplifier but is **not yet proven to be the cause**.
5. If larger caches do not help, trace Scaleform BeginFrame/EndFrame integration and batch-package lifetime.

Keep any eventual fix independent from the known-good gameplay timing logic.
