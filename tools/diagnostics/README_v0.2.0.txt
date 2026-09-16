NFSTR Diagnostics v0.2.0
========================

Purpose
-------
Standalone x86 diagnostics ASI for Need for Speed: The Run v1.1.0.0.
It is deliberately separate from NFSTR_DefinitiveEdition.asi and does not modify Challenge Series / Frostbite data.

v0.2 adds the synchronization diagnostics needed after Test 11:
- caller module+RVA for waits
- CreateEvent / SetEvent / ResetEvent / PulseEvent
- semaphore and mutex creation/signalling
- WaitForSingleObjectEx / WaitForMultipleObjectsEx / SignalObjectAndWait
- Sleep / SleepEx
- handle lifetime/reuse tracking through CloseHandle
- periodic thread snapshots with EIP/ESP/EBP and short stack chains
- ExitThread logging

The existing v0.1 file diagnostics remain, including explicit FrontEnd delta highlighting.

Current FrontEnd watch window
-----------------------------
offset : 950432  / 0x000E80A0
size   : 500944  / 0x0007A4D0
end    : 1451376 / 0x00162570

Test 11 finding from v0.1
------------------------
The game successfully read the entire registered 500,944-byte patch delta window in five contiguous reads, plus the corresponding base FrontEnd data. No file call was left hanging and no selected exception was recorded.

After the final FrontEnd read the worker repeatedly waited in 30 ms intervals. v0.2 is intended to identify what synchronization object is involved, the exact wait callsite, whether a completion signal is missing, and where the other game threads sit during the apparent hang.

Install / run
-------------
1. Replace v0.1 NFSTR_Diagnostics.asi with v0.2.
2. Rename/copy NFSTR_Diagnostics_v0.2.0.ini to NFSTR_Diagnostics.ini beside the ASI.
3. Keep Test 11 files unchanged.
4. Launch the game and leave it in the stuck state for roughly 8-15 seconds.
5. Close/terminate the game.
6. Send NFSTR_Diagnostics.log back for comparison.

A known-good Test 10 run with the same v0.2 ASI is also extremely useful. Run it separately and preserve both logs.
