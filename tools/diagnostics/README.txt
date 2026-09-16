NFSTR Diagnostics v0.1.0
========================

Purpose
-------
A standalone diagnostic ASI for Need for Speed: The Run PC v1.1.0.0.
It observes startup/runtime activity and writes NFSTR_Diagnostics.log.
It does not patch Challenge Series data, Frostbite objects, physics, FPS, or game state.

Install
-------
Copy these two files into the SAME plugins folder used by your ASI loader:

  NFSTR_Diagnostics.asi
  NFSTR_Diagnostics.ini

For the current Test 11 investigation that should be the plugins folder inside:

  E:\Need for Speed The Run Mod Test\

Run the game normally. The log is written beside the ASI:

  NFSTR_Diagnostics.log

What v0.1.0 watches
-------------------
* CreateFileA/W
* ReadFile
* SetFilePointer / SetFilePointerEx
* GetFileSize / GetFileSizeEx
* CreateFileMappingA/W
* MapViewOfFile / MapViewOfFileEx / UnmapViewOfFile
* GetFileAttributesA/W / GetFileAttributesExA/W
* LoadLibraryA/W / LoadLibraryExA/W
* CreateThread
* WaitForSingleObject / WaitForMultipleObjects
* selected serious first-chance exceptions
* newly loaded modules (periodic IAT rescanning)

FrontEnd-specific watch
-----------------------
FrontEnd.sb and FrontEnd.toc accesses are tagged [IMPORTANT].
The current Test 11 registered FrontEnd delta region is additionally flagged:

  offset: 950432 bytes = 0x000E80A0
  size:   500944 bytes = 0x0007A4D0
  end:    1451376 bytes = 0x00162570

Any read/map overlapping that region gets [FRONTEND-DELTA-OVERLAP].

How to read a hang
------------------
Calls are logged as paired lines:

  #123 BEGIN ReadFile ...
  #123 END   ReadFile ...

If the process freezes and the final relevant BEGIN has no matching END, that call never returned.
Infinite waits are also marked [IMPORTANT].

Recommended first three captures
--------------------------------
1. Stock/known-good FrontEnd files + Diagnostics ASI.
2. Known-good Test 10 / Buffalo Gap canary + Diagnostics ASI.
3. Test 11 + Diagnostics ASI.

That gives us a direct known-good vs rewritten-delta vs #18 comparison.

Notes
-----
* This is an x86/32-bit ASI because NFS The Run v1.1.0.0 is 32-bit.
* The hook method patches imported Win32 functions in the game and other non-system
  modules. System DLLs and the diagnostics ASI itself are deliberately not patched.
* It logs metadata, offsets, sizes, return values and timing. It does NOT dump file
  contents, keeping the trace useful without producing multi-gigabyte logs.
* Default maximum log size is 256 MB and can be changed in NFSTR_Diagnostics.ini.
