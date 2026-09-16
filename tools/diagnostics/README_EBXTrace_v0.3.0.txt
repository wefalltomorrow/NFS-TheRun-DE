NFSTR EBX Trace v0.3.0
======================

Purpose
-------
Targeted read-only diagnostic ASI for the Need for Speed: The Run v1.1 DRM-free executable used by this project.

It traces the Frostbite 2 EBX streaming parser at the two exact code sites identified from the Test 11B hang:
  parser entry/fill: EXE RVA 0x0014A450
  parser finalize:   EXE RVA 0x0014A700

The ASI validates the original bytes at both sites before installing either hook. If the executable does not match, it logs ABORT and leaves the parser untouched.

Known Test 11 assets are labeled by their raw EBX file GUID:
  ChallengeModeInfo_CANARY
  ITC_ROOT
  ITC_1
  CARFILTER_TIER5_ITC1

Installation / Test
-------------------
1. Keep Test 11B game data exactly unchanged.
2. Put NFSTR_EBXTrace.asi in the same plugins folder used by the ASI loader.
3. For the cleanest run, temporarily remove/rename NFSTR_Diagnostics.asi v0.2.0. The normal gameplay/fix ASIs can remain.
4. Launch the game and let the startup hang reproduce for roughly 10-15 seconds.
5. Terminate the game and upload plugins/NFSTR_EBXTrace.log.

Interpretation
--------------
Each EBX begins with a line like:
  [EBX] #123 BEGIN ... GUID=... label=...

A successfully completed EBX receives:
  [EBX] #123 FINALIZE ...

If a BEGIN has no matching FINALIZE, the last such object is the parser object that did not complete. CONT lines show the parser state (0..7) across streaming calls.

This ASI does not modify game data, bundle contents, EBX contents, or parser results. Its only code changes are temporary JMP hooks at the two validated parser sites for logging.

Build
-----
i686-w64-mingw32-g++ -shared -O2 -std=c++17 -Wall -Wextra \
  -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 \
  NFSTR_EBXTrace_v0.3.0.cpp -o NFSTR_EBXTrace.asi \
  -lkernel32 -static-libgcc -static-libstdc++
