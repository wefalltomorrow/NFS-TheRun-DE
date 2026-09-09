# NFSTR Promo / Hidden Unlocker

A deliberately narrow derivative of [xan1242/NFSTR_UltimateUnlocker](https://github.com/xan1242/NFSTR_UltimateUnlocker) for Need for Speed: The Run v1.1.0.0.

## Goal

Expose content marked by the game's `Unlockable` data as **promo** or **hidden**, without installing the Time Saver / "unlock everything" behaviour.

This build keeps only two patches from the original Ultimate Unlocker:

- `IsPromoContent` -> `IsPromoConten`
- `IsHiddenUnlock` -> `IsHiddenUnloc`

Those names are fields of the game's `Unlockable` class. Breaking the reflected field names prevents Frostbite from deserialising those two booleans, so they stay at their default `false` values.

## Explicitly NOT included

The original Ultimate Unlocker also contains broad progression-bypass patches. They are intentionally omitted:

- `Unlockers` -> `Unlocker` — broad unlock-system bypass; modern FusionFix's `UnlockEverything` implements the same idea by redirecting the `Unlockers` property.
- two hooks that force vehicle `m_isUnlocked = true`.
- two stage/challenge-selection branch patches.
- the two active Ebisu / `OnNetworkConnected` state-machine NOPs at `0x834303` and `0x83434F`.
- the commented `0xDD76CB` network patch.
- the commented Origin/Ebisu function stubs.

This plugin therefore does **not** intentionally unlock normal progression cars, stages, Challenge Series events, or standard unlock conditions.

## Important limitation

`IsPromoContent` and `IsHiddenUnlock` are not explicit DLC-entitlement fields. This plugin should be treated as a **promo/hidden-content experiment** until verified in-game. Genuine installed DLC that the game already recognises should remain handled by the game's normal DLC/entitlement path.

## Compatibility

Designed for the DRM-free v1.1.0.0 executable layout used by the original Ultimate Unlocker. The patches are applied base-relative and verify the expected final character before writing.

## Credits

Original Ultimate Unlocker research/implementation: Xan / Tenjoin (`xan1242`).
