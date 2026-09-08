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

Two snapshots from the same affected setup have now been captured:
1. mappings already reset to the game's defaults;
2. desired custom mappings, captured immediately after changing them and exiting normally, before another launch.

The `PROF_SAVE_body` contains a dedicated binary property named `Vehicle Inputs`.

### Reset/default snapshot
- `PROF_SAVE_header`: 8 bytes
- `Vehicle Inputs`: 1312 bytes
- `Vehicle Inputs` magic: `0xABCDEF01`
- entry count: 43
- `Vehicle Inputs` SHA-256: `44e37a2efad30ca4a887f8d8a75f1146f7b7592f3601270e1649316ebc03669e`

### Desired/custom snapshot
- `PROF_SAVE_header`: byte-for-byte identical to the reset snapshot
- `Vehicle Inputs`: 1776 bytes
- `Vehicle Inputs` magic: `0xABCDEF01`
- entry count: 51
- `Vehicle Inputs` SHA-256: `46117a1b3df484d7f207e58b4c47f07f388b8063700348072194a6f1dbea9e09`

### Binary diff result
The two 1,560,576-byte `PROF_SAVE_body` files are identical everywhere except:
- bytes `0x00000000..0x00000003` (the body checksum/hash), and
- the `Vehicle Inputs` property/value region beginning at file offset `0x120F41` (decimal 1,183,553).

The body data before `Vehicle Inputs` is identical, and everything after the end of the larger custom block is also identical. The reset save's unused space where the larger custom block would extend is zero padding.

This is decisive: **the game successfully writes the custom controller mappings to disk when they are changed.** The persistence bug is therefore not "custom mappings never save". A later startup is replacing the stored `Vehicle Inputs` data with a different/default block and subsequently saving that replacement.

The property format observed in both snapshots is:

```text
uint32 valueSize
uint32 magic = 0xABCDEF01
uint32 entryCount

repeat entryCount times:
    uint32 actionId
    uint32 bindingCount
    repeat bindingCount times:
        uint32 field0
        uint32 field1
        uint32 field2
        uint32 field3
```

The custom and reset blocks differ structurally as well as in individual bindings: 51 actions are serialized in the desired mapping versus 43 after the reset.

## Executable targets
The v1.1 executable contains both `OnInputDeviceChanged` and `Vehicle Inputs` in the same input/profile subsystem.

Known xrefs in the tested executable:
- `OnInputDeviceChanged` string is dispatched from code around `0x00927133`.
- `Vehicle Inputs` is referenced by small wrappers at `0x00927140`, `0x00927150`, and `0x00927160`.
- `0x00927150` is called from at least `0x008786BF` and `0x00882C5E`.
- `0x00927160` is called from at least `0x00882CBE`.

The exact semantics of those three wrappers still need to be named (load/save/reset/dirty-state etc.), so they should be instrumented before patching behavior.

## Current conclusion
The strongest working theory is now a **startup/device-initialization overwrite**:

1. profile load restores the desired `Vehicle Inputs` data;
2. controller/device initialization or `OnInputDeviceChanged` rebuilds the active vehicle bindings from defaults on most launches;
3. the rebuilt/default block is later written back to `PROF_SAVE_body` with a valid new body checksum.

Because both snapshots have valid game-generated checksums and the header is unchanged, manually hex-editing the save is not the preferred fix.

## Next diagnostic / fix direction
Instrument the `Vehicle Inputs` wrappers and/or the input-device-change path to identify which call replaces the loaded custom block during startup.

The preferred permanent fix is generic: preserve the profile-loaded `Vehicle Inputs` block across device initialization, rather than hardcoding one user's button mappings or continuously rewriting the save file.

Keep this fix separate from the framerate code.
