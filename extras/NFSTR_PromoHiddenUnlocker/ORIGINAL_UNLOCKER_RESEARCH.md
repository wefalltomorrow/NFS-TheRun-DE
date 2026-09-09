# Original NFSTR Ultimate Unlocker research

Source reviewed: `xan1242/NFSTR_UltimateUnlocker` (`dllmain.cpp`) plus NFS The Run v1.1 executable disassembly.

## Active patches in the original

### 1. `0x834303` / `0x83434F` — Ebisu / OnNetworkConnected state machine

These are not normal progression-unlock patches.

The containing function calls `EbisuUpdate` (`0x18AF2D0`) every update and later dispatches the string/event `OnNetworkConnected`.

- `0x834303`: NOPs a `JE` taken when a queue/vector is empty. Removing it forces the non-empty path even when the container reports no entries.
- `0x83434F`: NOPs a `JLE` after comparing a state/timer value against `1000`, forcing the timeout/transition block immediately rather than waiting.

These may have been intended to make old offline/Origin-bypass setups advance through the connection state, but the first patch can make an empty container follow a path that dereferences its first entry. Do not treat these as safe general-purpose online optimisations.

### 2. `0x25A35EC` — `Unlockers` -> `Unlocker`

This corrupts the reflected property name `Unlockers`.

Modern NFS The Run FusionFix confirms the meaning: its `UnlockEverything` option hooks the same `Unlockers` property and replaces it with an undefined property. This is a broad progression bypass and should NOT be present in a selective promo/DLC build.

### 3. `0x25A362D` — `IsPromoContent` -> `IsPromoConten`

`IsPromoContent` is a bool field of the game's `Unlockable` class at object offset `0x1C`.

Breaking the reflected name prevents Frostbite from deserialising that property, so the field remains at its default false value. This is one of the two patches retained by `NFSTR_PromoHiddenUnlocker`.

### 4. `0x25A363D` — `IsHiddenUnlock` -> `IsHiddenUnloc`

`IsHiddenUnlock` is a bool field of `Unlockable` at object offset `0x1B`.

Same mechanism as `IsPromoContent`: retained by the selective build.

### 5. `0x93E00D` / `0x93F214` — force car unlocks

The original inline hooks set the `Unlockable` byte at `+0x18` (`m_isUnlocked`) to `1` and skip the normal locked path.

Modern FusionFix has an equivalent `UnlockAllCars` feature that forces `m_isUnlocked = true`.

Omitted from the selective build.

### 6. `0x930C00` / `0x9313A2` — stage/challenge selection unlock

The original source labels these `stage select unlock`. They bypass lock-related branches in two stage/challenge UI paths.

Omitted from the selective build.

## Commented-out patches in the original source

### `0xDD76CB` — NOT a useful "disable network" patch

The containing code calls the Winsock `socket()` import (WS2_32 ordinal 23), compares the result with `-1` (`INVALID_SOCKET`), and normally returns early on failure.

NOPing the `JNE` at `0xDD76CB` makes the function continue even when `socket()` returned `INVALID_SOCKET`. This is the opposite of a clean network-disable optimisation and is potentially unsafe. Do not port it.

### Ebisu function stubs (`0x18AEF80` through `0x18AF640`)

The commented block proposed redirecting many Origin/Ebisu wrapper functions to one `retZeroInt()` function:

- EbisuStartup
- EbisuRequestTicket
- EbisuRequestTicketSync
- EbisuGetSetting
- EbisuDestroyHandle
- EbisuShutdown
- EbisuUpdate
- EbisuGetDefaultUser
- EbisuCheckOnline
- EbisuGetErrorInfo
- EbisuGetErrorDescription
- EbisuCheckPermission
- EbisuGetProfileSync
- EbisuGetProfile
- EbisuSubscribePresence

This should NOT be ported wholesale. These APIs do not all share the same return semantics. In particular, `EbisuGetDefaultUser` returns a 64-bit value in `EDX:EAX`; jumping it to a normal `int` function that only guarantees `EAX=0` is ABI-incorrect. Other functions may return handles, pointers, booleans, status/error codes, or require output parameters to be filled.

A safe offline optimisation should hook individual high-level service calls or settings with understood semantics instead of replacing the entire Ebisu API with one zero-return stub.

## Recommended split

- **Selective promo/hidden plugin:** only `IsPromoContent` and `IsHiddenUnlock` reflection patches.
- **Normal progression:** leave `Unlockers`, car `m_isUnlocked`, and stage/challenge lock paths untouched.
- **Offline optimisation:** keep separate from unlocking; use known `NfsOnlineSettings` controls or individually researched Ebisu calls, not the original blanket network hacks.
