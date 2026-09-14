<p align="center">
  <img src="assets/the-run-de.png" alt="NFS The Run Definitive Edition" width="640">
</p>

# NFS The Run Definitive Edition

An ASI plugin for **Need for Speed: The Run** (PC, v1.1.0.0) that fixes high-framerate problems, adds graphics/gameplay options, and makes the game work better on modern PCs.

This repository is my fork of [BadassBaboon/NFS-TheRun-DE](https://github.com/BadassBaboon/NFS-TheRun-DE). The original mod is still the base; this fork is where I am testing and adding fixes that I want to keep upstream-friendly where possible.

## Current fork additions

### Faster level loading

The game has a hardcoded VSync/display path that unnecessarily slows actual event and level loading. This fork ports the VSync bypass from _mRally2's research and applies it only after verifying the expected v1.1 instructions.

This is separate from **FusionFix's `SkipIntro`** feature:

- FusionFix speeds up **game startup** by skipping the intro/login flow.
- `FastLoadingVSyncBypass` speeds up **race and level loading** by removing the hardcoded VSync pacing during loading.

The level-loading fix does **not** touch `MaxSimFps`, `MaxVariableFps`, GameTime or the gameplay simulation rate. It was tested in-game with a large, obvious reduction in event loading time.

It is enabled by default. To disable it, add this to `NFSTR_DefinitiveEdition.ini`:

```ini
[GRAPHICS_QUALITY]
FastLoadingVSyncBypass = 0
```

### Menu and cutscene FPS work

The fork also contains the current menu/cutscene FPS work and compatibility changes for running alongside FusionFix. Driving keeps the fixed simulation behaviour needed by the game while non-driving scenes can be handled separately.

### Ongoing graphics research

Higher-quality AA, mesh LOD, livery quality, texture/pop-in and other Frostbite rendering controls are still being tested on separate branches. They are **not** being dumped into `main` until each one is proven useful on its own.

## What the mod already fixes

**High-framerate engine audio.** Above 30 FPS the engine synth can keep restarting its pitch glide before it has time to move, leaving the engine note stuck. The fix stops that behaviour so pitch follows RPM again.

**Tyre spray, drift smoke and dirt dust.** Kickup particles inherit too much wheel velocity above 30 FPS. The mod corrects that without changing vehicle physics.

**Quick-time prompts.** The game's QTE timing assumes a 30 FPS simulation. The mod can clamp the relevant no-control moments back to the safe timing while leaving normal driving at the chosen framerate.

**Minimap rendering.** Clearing the render target fixes events where the minimap is broken, invisible or missing road sections.

**Photo mode.** The pause-menu entry can be restored even though Autolog is dead. Saving through the original online path still does not work, so use an external screenshot key.

## Run For Your Life

The mod can turn Extreme into **DEADLY**, a harder ruleset rather than just faster AI. It reduces the player's safety margin, removes player assists, makes nitrous something you have to earn, ramps drafting instead of giving full slipstream immediately, strengthens the AI and increases traffic pressure.

The important part is that player-only restrictions stay player-only. AI cars keep their own assists and drafting instead of being accidentally weakened by broad cheat-table patches.

## Other options

The INI also exposes settings for:

- framerate limit and cutscene/menu FPS behaviour
- field of view and camera/viewport adjustments
- shadow resolution, filtering and distance
- anisotropic filtering
- time-of-day randomisation / Night Run
- traffic density and vehicle limits
- AI skill and rubber-banding
- nitrous behaviour
- track-rule changes such as checkpoint timer, out-of-bounds reset and wrong-way respawn
- QA/debug UI options and diagnostics

See `NFSTR_DefinitiveEdition.ini` for the actual settings and defaults.

## Install

The original release is available on [NFSMods](https://nfsmods.xyz/mod/5373). This fork is still development work, so current builds come from this repository's GitHub Actions / test branches rather than a separate public release.

1. Install an ASI loader if you do not already have one. FusionFix includes one.
2. Put `NFSTR_DefinitiveEdition.asi` and `NFSTR_DefinitiveEdition.ini` in the folder your ASI loader scans (normally `plugins`).
3. Set `FPSLimit` in the INI to suit your display.

Built and tested against the DRM-free **Need for Speed: The Run v1.1.0.0** executable. Patches verify the expected bytes before writing, so an unsupported executable should be skipped rather than blindly patched.

## Build

MinGW-w64 targeting 32-bit, C++17:

```bat
build.bat
```

or with CMake:

```text
cmake -B build -A Win32
cmake --build build --config Release
```

Output: `NFSTR_DefinitiveEdition.asi`

Pull requests are also built automatically by GitHub Actions.

## Related projects

- [NFSTR Ultimate Unlocker](https://github.com/wefalltomorrow/NFSTR_UltimateUnlocker) — restores the old DLC/preorder/promo entitlements without having to unlock normal career progression, with a separate full-unlock mode if wanted.
- [The Run Tools Research](https://github.com/wefalltomorrow/The-Run-Tools-Research) — reverse-engineering notes and console-to-PC content-port work, including the Xbox 360 Italian Pack and console-only Challenge Series research.
- [NFS The Run FusionFix](https://github.com/ThirteenAG/WidescreenFixesPack) — windowed mode, camera, startup skipping and other fixes. This mod is designed to run alongside it.

## Research / development status

Current work is intentionally split instead of putting every experiment into one build:

- **Stable/proven fixes** go toward `main`.
- **Graphics experiments** stay on graphics branches until validated.
- **DLC/entitlement work** lives in `NFSTR_UltimateUnlocker` rather than bloating this plugin.
- **Console content ports** and file-format research live in `The-Run-Tools-Research` until there is something ready to integrate.

The current larger research targets are the two console-only Signature Challenge Series, the Xbox 360 Italian Pack, dead Autolog/online-service cleanup, graphics quality/pop-in and the remaining high-FPS/UI issues.

## Documentation

`docs/RESEARCH.md` is the main reverse-engineering log. `docs/SETTINGS_FIELDS.md` documents Frostbite settings fields recovered from the executable. The `research/` folder contains unfinished or deliberately parked experiments that are not part of the normal build.

## Credits

- **BadassBaboon** — original Definitive Edition project and the bulk of the mod this fork is built on.
- **Brawltendo** — IDA research and NFS Rivals framerate-unlocker work that helped with the Frostbite timing side.
- **_mRally2** — The Run Master Table, TOD Randomizer and the hardcoded VSync/loading research used by the fast level-loading patch.
- **ThirteenAG** — FusionFix and the ASI loader.

## Compatibility

PC **v1.1.0.0** only. The current work is tested against the DRM-free v1.1 executable layout.
