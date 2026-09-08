# Controller mapping persistence research

## Symptom
Custom controller mappings usually reset to the game's defaults after restarting Need for Speed: The Run. They occasionally persist, but the reset happens on most launches.

## Environment / ruled out
- Game is launched directly from `Need For Speed The Run.exe`.
- Offline; no EA App / Origin client is running.
- Reproduced both with and without the PCGamingWiki x360ce controller fix, so x360ce is not the cause.

## Save data
The relevant files are:
- `Documents\NFSTR\settings\PROF_SAVE_body`
- `Documents\NFSTR\settings\PROF_SAVE_header`

A reset/default-mappings snapshot has been captured from the affected setup.

The `PROF_SAVE_body` contains a dedicated binary property named `Vehicle Inputs` (1312 bytes in the captured save, 43 entries). This strongly suggests the mappings are persisted in the Frostbite profile save rather than a separate controller config.

## Current working theory
The reset is likely happening inside the game's profile/input-device initialization path rather than through cloud sync or an external launcher. The executable contains profile/input-device code and `OnInputDeviceChanged`, so startup device reinitialization is a candidate.

## Next diagnostic
Capture a second `PROF_SAVE_body` / `PROF_SAVE_header` pair immediately after saving the desired custom mappings and exiting normally, then diff only the `Vehicle Inputs` block against the reset snapshot.

That will distinguish:
1. the game overwriting the stored block with defaults, from
2. the saved custom block remaining intact while the game ignores/replaces it only in memory.

Keep any eventual fix separate from the framerate code.
