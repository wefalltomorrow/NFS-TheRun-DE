from pathlib import Path

p = Path("src/features/fps_unlocker.cpp")
s = p.read_text()

# Known-good FPS compatibility fix developed against upstream commit
# 01f77aed0e5274ad0787a4713a0d65a20802c7e8.
#
# Scope is deliberately narrow:
#   * preserve BadassBaboon/NFS-TheRun-DE's existing FPS variables/policy
#   * sample PlayerHasVehicleControl by value at hook time instead of later
#     dereferencing a potentially stale object pointer
#   * restore the pre-Aug-11 VariableSimTickEnable=1 behavior that keeps normal
#     gameplay at the configured FPS on the tested v1.1/FusionFix setup
#   * retain the current 1500 ms MaxVariableFps dwell for short no-control
#     windows versus longer menu/cutscene windows
#
# It does NOT introduce MaxSimFps, MaxInactiveVariableFps, a 1000 Hz sim rate,
# mRally2 timing logic, or any additional gameplay FPS variable.

# ---------------------------------------------------------------------------
# 1) Hook-time PlayerHasVehicleControl value sampling
# ---------------------------------------------------------------------------
if "g_ControlSampleCounter" not in s:
    old = '''    // Captured pointer to the PlayerHasVehicleControl byte (game sets/clears it).\n    uint8_t* g_pHasControl = nullptr;\n    uintptr_t g_pControlReturn = 0;       // return addr for the direct (unhooked-site) path\n'''
    new = '''    // Keep the original captured pointer for existing DE features that use it,\n    // but fps_unlocker.cpp itself consumes the hook-time value below.\n    uint8_t* g_pHasControl = nullptr;\n    volatile uint32_t g_ControlSampleCounter = 0;\n    volatile uint8_t  g_ControlSampleValue = 0;\n    uintptr_t g_pControlReturn = 0;       // return addr for the direct (unhooked-site) path\n'''
    assert old in s, "control globals block not found"
    s = s.replace(old, new, 1)

    old = '''    "    pushl %ebx\\n"\n    "    leal 0x04(%esi), %ebx\\n"\n    "    movl %ebx, _g_pHasControl\\n"\n    "    popl %ebx\\n"\n    "    cmpb $0x00, 0x04(%esi)\\n"   // stolen: cmp byte ptr [esi+04],00\n'''
    new = '''    "    pushl %eax\\n"\n    "    leal 0x04(%esi), %eax\\n"\n    "    movl %eax, _g_pHasControl\\n"\n    "    movzbl 0x04(%esi), %eax\\n"\n    "    movb %al, _g_ControlSampleValue\\n"\n    "    incl _g_ControlSampleCounter\\n"\n    "    popl %eax\\n"\n    "    cmpb $0x00, 0x04(%esi)\\n"   // stolen: cmp byte ptr [esi+04],00\n'''
    assert old in s, "direct control hook block not found"
    s = s.replace(old, new, 1)

    old = '''    "    pushl %eax\\n"\n    "    leal 0x04(%esi), %eax\\n"\n    "    movl %eax, _g_pHasControl\\n"\n    "    popl %eax\\n"\n    "    jmpl *_g_pControlChainTarget\\n"\n'''
    new = '''    "    pushfl\\n"\n    "    pushl %eax\\n"\n    "    leal 0x04(%esi), %eax\\n"\n    "    movl %eax, _g_pHasControl\\n"\n    "    movzbl 0x04(%esi), %eax\\n"\n    "    movb %al, _g_ControlSampleValue\\n"\n    "    incl _g_ControlSampleCounter\\n"\n    "    popl %eax\\n"\n    "    popfl\\n"\n    "    jmpl *_g_pControlChainTarget\\n"\n'''
    assert old in s, "chained control hook block not found"
    s = s.replace(old, new, 1)

    old = '''static int  g_LastControlState = -1;   // -1 unknown, 0 no control, 1 has control\nstatic bool g_WarnedStaleControl = false;\nstatic bool g_WarnedCutsceneConflict = false;\n\n// True while the control byte is unreadable garbage. The clamp can safely hold\n// its last known state through this; the cutscene unlock CANNOT, and must fail\n// closed instead. See the note at the tick write.\nstatic bool g_ControlReadStale = false;\n\n// When the no-control window began, for the cutscene-unlock dwell below.\n'''
    new = '''static int  g_LastControlState = -1;   // -1 unknown, 0 no control, 1 has control\nstatic uint32_t g_LastControlSampleCounter = 0;\nstatic bool g_WarnedCutsceneConflict = false;\n\n// When the no-control window began, for the cutscene-unlock dwell below.\n'''
    assert old in s, "control state globals block not found"
    s = s.replace(old, new, 1)

    start_marker = '''        // Sim-rate clamp: when the player has no vehicle control (QTE / cutscene),\n'''
    end_marker = '''        // Unknown state (-1) counts as "driving" everywhere below, which is the\n'''
    start = s.index(start_marker)
    end = s.index(end_marker, start)
    new_block = '''        // Sim-rate clamp / cutscene state detection. Preserve the original DE\n        // policy, but consume the bool sampled synchronously by the hook. The old\n        // code dereferenced g_pHasControl here after the owning object could have\n        // been freed/reused; a reused 0/1 byte looked valid and could leave the\n        // FPS logic stuck in the wrong state.\n        const uint32_t sampleCounter = g_ControlSampleCounter;\n        if (sampleCounter != g_LastControlSampleCounter) {\n            g_LastControlSampleCounter = sampleCounter;\n            const uint8_t ctlByte = g_ControlSampleValue ? 1u : 0u;\n\n            if (!g_LogCapturedControl) {\n                Logger::Log("Control-check hook active: PlayerHasVehicleControl sampled by value (initial value %u)",\n                            ctlByte);\n                g_LogCapturedControl = true;\n            }\n\n            const int hasControl = ctlByte ? 1 : 0;\n            if (hasControl != g_LastControlState) {\n                const char* effect;\n                if (hasControl) {\n                    effect = "driving, target framerate";\n                } else if (g_Config.UnlockCutsceneFPS) {\n                    effect = "no control, menu/cutscene dwell started";\n                } else if (g_Config.ClampSimRateWhenNoControl) {\n                    effect = "no control, clamping to 30";\n                } else {\n                    effect = "no control, sim rate left alone";\n                }\n                Logger::Log("Vehicle control changed: PlayerHasVehicleControl=%u -> %s.",\n                            ctlByte, effect);\n                g_LastControlState = hasControl;\n                if (!hasControl) g_NoControlSince = GetTickCount();\n            }\n        }\n\n'''
    s = s[:start] + new_block + s[end:]

    old = '''        const bool dwellMet = noControl\n                           && !g_ControlReadStale\n                           && (GetTickCount() - g_NoControlSince) >= kCutsceneDwellMs;\n'''
    new = '''        const bool dwellMet = noControl\n                           && (GetTickCount() - g_NoControlSince) >= kCutsceneDwellMs;\n'''
    assert old in s, "cutscene dwell block not found"
    s = s.replace(old, new, 1)

# ---------------------------------------------------------------------------
# 2) Restore the known-good pre-Aug-11 gameplay tick behavior
# ---------------------------------------------------------------------------
if "preserving pre-Aug-11 DE gameplay timing" not in s:
    old = '''        // The field is only ever written when the option is on, and it is driven\n        // back to 0 the moment control returns rather than left set. An earlier\n        // version set it once and never cleared it, which is why enabling this\n        // used to break driving: the whole game ran on a variable step from the\n        // first cutscene onwards. Writing it unconditionally is also avoided, so\n        // with the option off the game owns the field exactly as it always did.\n        if (g_Config.UnlockCutsceneFPS) {\n            const uint32_t want = cutsceneUnlock ? 1u : 0u;\n            if (*g_pSimTickEnable != want) {\n                *g_pSimTickEnable = want;\n            }\n            if (!g_WarnedCutsceneConflict) {\n                Logger::Log("UnlockCutsceneFPS is ON: the variable sim tick is enabled only after "\n                            "%lu ms without vehicle control, so driving and crash physics keep "\n                            "their fixed 30 Hz step. QTE prompts run at the target framerate and "\n                            "time out faster.", static_cast<unsigned long>(kCutsceneDwellMs));\n                g_WarnedCutsceneConflict = true;\n            }\n        }\n'''
    new = '''        // Preserve the pre-Aug-11 DE gameplay path. On the tested v1.1/FusionFix\n        // setup, forcing VariableSimTickEnable back to 0 during driving makes actual\n        // gameplay render at 30 FPS even when MaxVariableFps is the configured target.\n        // The older DE behavior kept this field enabled and gameplay ran correctly.\n        // Keep that proven behavior; the 1500 ms dwell above only distinguishes\n        // short no-control windows from longer menu/cutscene windows for\n        // MaxVariableFps. No other GameTime timing field is modified.\n        if (g_Config.UnlockCutsceneFPS) {\n            if (*g_pSimTickEnable != 1u) {\n                *g_pSimTickEnable = 1u;\n            }\n            if (!g_WarnedCutsceneConflict) {\n                Logger::Log("UnlockCutsceneFPS is ON: preserving pre-Aug-11 DE gameplay timing "\n                            "(VariableSimTickEnable=1). MaxVariableFps stays at the configured "\n                            "target while driving; short no-control windows remain at 30 and "\n                            "long menu/cutscene windows return to target after %lu ms.",\n                            static_cast<unsigned long>(kCutsceneDwellMs));\n                g_WarnedCutsceneConflict = true;\n            }\n        }\n'''
    assert old in s, "current DE VariableSimTickEnable block not found"
    s = s.replace(old, new, 1)

# Hard guard against the abandoned experiments.
assert "MaxSimFps" not in s
assert "MaxInactiveVariableFps" not in s

p.write_text(s)
print("Known-good menu/cutscene FPS fix applied (or already present).")
