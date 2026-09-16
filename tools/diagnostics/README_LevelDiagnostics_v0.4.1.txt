NFSTR Level Diagnostics v0.4.1
==============================

This is the safe/manual-arm replacement for v0.4.0.

v0.4.0 patched imports across every loaded module and could terminate the game during startup. Do not use v0.4.0.

v0.4.1 behavior
---------------
- Loads with ZERO game API hooks installed.
- Navigate normally to Escape From S.F.
- Press F6 once before starting the event.
- Two short beeps mean the tracer armed successfully.
- After F6, ONLY the main Need For Speed The Run.exe import table is patched.
- System DLLs, runtime DLLs, and other ASIs are never modified.
- A crash handler is installed only after F6.
- Only files whose path contains Level_0100_SanFrancisco are logged.

Test procedure
--------------
1. Keep Test 14 installed exactly as-is.
2. Remove/rename NFSTR_LevelDiagnostics v0.4.0.
3. For a clean capture, temporarily remove/rename NFSTR_ITC_RuntimeProbe.asi, NFSTR_Diagnostics.asi, and NFSTR_EBXTrace.asi if present.
4. Install NFSTR_LevelDiagnostics.asi v0.4.1 in plugins.
5. Launch the game normally.
6. Navigate to Escape From S.F. and stop on the event details screen before launching the race.
7. Press F6 once. Wait for the two beeps.
8. Start the event and reproduce the crash naturally.
9. Upload plugins/NFSTR_LevelDiagnostics.log.

If F6 produces one low beep, the main EXE imports could not be patched; upload the log anyway.
