#include "features.h"
#include "../memory.h"
#include "../logger.h"
#include <windows.h>
#include <cstdint>

// Low-impact, logging-only instrumentation for the controller-mapping
// persistence bug.
//
// v1 hooked the whole input-state updater at exe+0x5270A0. That function is a
// hot path and produced thousands of log entries, so it was removed in v2.
// v2 proved that a reset launch still calls Vehicle Inputs A three times and B
// once, while C never runs. That means C is not the reset path and A/B alone do
// not tell us why one launch keeps the custom mappings and another replaces
// them.
//
// v3 keeps the three Vehicle Inputs hooks and adds only TWO cold event sites
// inside the input-state updater:
//   exe+0x5270DC - state 2/3 transition event (hash 0x4BB4E56D)
//   exe+0x527133 - explicit OnInputDeviceChanged dispatch
// These sites execute only when the updater actually changes/announces device
// state. Each snapshot records the state field at +0x3C and the global device
// flag at exe+0x249227E, without logging the per-frame updater itself.

extern "C" {
    volatile uint32_t g_InputDiagVehicleA_Count = 0;
    volatile uint32_t g_InputDiagVehicleB_Count = 0;
    volatile uint32_t g_InputDiagVehicleC_Count = 0;

    volatile uintptr_t g_InputDiagVehicleA_This = 0;
    volatile uintptr_t g_InputDiagVehicleB_This = 0;
    volatile uintptr_t g_InputDiagVehicleC_This = 0;

    volatile uintptr_t g_InputDiagVehicleA_Caller = 0;
    volatile uintptr_t g_InputDiagVehicleB_Caller = 0;
    volatile uintptr_t g_InputDiagVehicleC_Caller = 0;

    volatile uint32_t g_InputDiagStateTransition_Count = 0;
    volatile uint32_t g_InputDiagStateTransition_State = 0;
    volatile uint32_t g_InputDiagStateTransition_DeviceFlag = 0;

    volatile uint32_t g_InputDiagOnInputChanged_Count = 0;
    volatile uint32_t g_InputDiagOnInputChanged_State = 0;
    volatile uint32_t g_InputDiagOnInputChanged_DeviceFlag = 0;

    uintptr_t g_InputDiagVehicleA_Return = 0;
    uintptr_t g_InputDiagVehicleB_Return = 0;
    uintptr_t g_InputDiagVehicleC_Return = 0;
    uintptr_t g_InputDiagStateTransition_Return = 0;
    uintptr_t g_InputDiagOnInputChanged_Return = 0;

    uintptr_t g_InputDiagVehicleInputsName = 0;
    uintptr_t g_InputDiagOnInputDeviceChangedName = 0;
    uintptr_t g_InputDiagDeviceFlagAddress = 0;

    void InputDiagVehicleA_HookAsm();
    void InputDiagVehicleB_HookAsm();
    void InputDiagVehicleC_HookAsm();
    void InputDiagStateTransition_HookAsm();
    void InputDiagOnInputChanged_HookAsm();
}

// At entry, [esp] is the caller return address. Preserve flags/EAX while
// snapshotting caller + ECX, then replay the stolen instruction.
//
// exe+0x527140. Stolen bytes: push 0x024832F4 ("Vehicle Inputs")
asm(
    ".text\n"
    ".globl _InputDiagVehicleA_HookAsm\n"
    "_InputDiagVehicleA_HookAsm:\n"
    "    pushfl\n"
    "    pushl %eax\n"
    "    movl %ecx, _g_InputDiagVehicleA_This\n"
    "    movl 8(%esp), %eax\n"
    "    movl %eax, _g_InputDiagVehicleA_Caller\n"
    "    incl _g_InputDiagVehicleA_Count\n"
    "    popl %eax\n"
    "    popfl\n"
    "    pushl _g_InputDiagVehicleInputsName\n"
    "    jmpl *_g_InputDiagVehicleA_Return\n"
);

// exe+0x527150. Stolen bytes: push 0x024832F4 ("Vehicle Inputs")
asm(
    ".text\n"
    ".globl _InputDiagVehicleB_HookAsm\n"
    "_InputDiagVehicleB_HookAsm:\n"
    "    pushfl\n"
    "    pushl %eax\n"
    "    movl %ecx, _g_InputDiagVehicleB_This\n"
    "    movl 8(%esp), %eax\n"
    "    movl %eax, _g_InputDiagVehicleB_Caller\n"
    "    incl _g_InputDiagVehicleB_Count\n"
    "    popl %eax\n"
    "    popfl\n"
    "    pushl _g_InputDiagVehicleInputsName\n"
    "    jmpl *_g_InputDiagVehicleB_Return\n"
);

// exe+0x527160. Stolen bytes:
// mov byte ptr [ecx+4E],1 ; push 0x024832F4 ("Vehicle Inputs")
asm(
    ".text\n"
    ".globl _InputDiagVehicleC_HookAsm\n"
    "_InputDiagVehicleC_HookAsm:\n"
    "    pushfl\n"
    "    pushl %eax\n"
    "    movl %ecx, _g_InputDiagVehicleC_This\n"
    "    movl 8(%esp), %eax\n"
    "    movl %eax, _g_InputDiagVehicleC_Caller\n"
    "    incl _g_InputDiagVehicleC_Count\n"
    "    popl %eax\n"
    "    popfl\n"
    "    movb $0x01, 0x4e(%ecx)\n"
    "    pushl _g_InputDiagVehicleInputsName\n"
    "    jmpl *_g_InputDiagVehicleC_Return\n"
);

// exe+0x5270DC. This site is reached only after the updater changes its +0x3C
// state between 2 and 3. The original sequence is:
//   push 0x4BB4E56D
//   call eax
// ESI is the updater object; EAX is the dispatch function pointer.
asm(
    ".text\n"
    ".globl _InputDiagStateTransition_HookAsm\n"
    "_InputDiagStateTransition_HookAsm:\n"
    "    pushfl\n"
    "    pushl %eax\n"
    "    movl 0x3c(%esi), %eax\n"
    "    movl %eax, _g_InputDiagStateTransition_State\n"
    "    movl _g_InputDiagDeviceFlagAddress, %eax\n"
    "    movzbl (%eax), %eax\n"
    "    movl %eax, _g_InputDiagStateTransition_DeviceFlag\n"
    "    incl _g_InputDiagStateTransition_Count\n"
    "    popl %eax\n"
    "    popfl\n"
    "    pushl $0x4bb4e56d\n"
    "    call *%eax\n"
    "    jmpl *_g_InputDiagStateTransition_Return\n"
);

// exe+0x527133. This is the literal OnInputDeviceChanged dispatch reached when
// the updater resets +0x3C to 0. The original sequence is:
//   push 0x024832C8 ("OnInputDeviceChanged")
//   call eax
// ESI is still the updater object and EAX is the dispatch function pointer.
asm(
    ".text\n"
    ".globl _InputDiagOnInputChanged_HookAsm\n"
    "_InputDiagOnInputChanged_HookAsm:\n"
    "    pushfl\n"
    "    pushl %eax\n"
    "    movl 0x3c(%esi), %eax\n"
    "    movl %eax, _g_InputDiagOnInputChanged_State\n"
    "    movl _g_InputDiagDeviceFlagAddress, %eax\n"
    "    movzbl (%eax), %eax\n"
    "    movl %eax, _g_InputDiagOnInputChanged_DeviceFlag\n"
    "    incl _g_InputDiagOnInputChanged_Count\n"
    "    popl %eax\n"
    "    popfl\n"
    "    pushl _g_InputDiagOnInputDeviceChangedName\n"
    "    call *%eax\n"
    "    jmpl *_g_InputDiagOnInputChanged_Return\n"
);

namespace {
    bool g_Installed = false;
    uint32_t g_LastVehicleA = 0;
    uint32_t g_LastVehicleB = 0;
    uint32_t g_LastVehicleC = 0;
    uint32_t g_LastStateTransition = 0;
    uint32_t g_LastOnInputChanged = 0;

    bool InstallOne(uintptr_t addr, const uint8_t* expected, size_t expectedSize,
                    uintptr_t hook, uintptr_t& ret, const char* label) {
        if (!Memory::VerifyBytes(addr, expected, expectedSize)) {
            Logger::Log("[INPUT-DIAG] %s hook skipped at 0x%08X: got [%s]",
                        label, addr, Memory::BytesToHex(addr, expectedSize).c_str());
            return false;
        }
        ret = addr + expectedSize;
        if (!Memory::InjectJMP(addr, hook, expectedSize)) {
            Logger::Log("[INPUT-DIAG] %s hook failed at 0x%08X", label, addr);
            return false;
        }
        Logger::Log("[INPUT-DIAG] %s hook installed at 0x%08X", label, addr);
        return true;
    }
}

namespace Features {
    void InitControllerMappingDiagnostics() {
        const uintptr_t base = Memory::GetGameBase();
        g_InputDiagVehicleInputsName = base + 0x20832F4;
        g_InputDiagOnInputDeviceChangedName = base + 0x20832C8;
        g_InputDiagDeviceFlagAddress = base + 0x249227E;

        const uint8_t vehicleExpected[5] = { 0x68, 0xF4, 0x32, 0x48, 0x02 };
        const uint8_t vehicleCExpected[9] = { 0xC6, 0x41, 0x4E, 0x01, 0x68, 0xF4, 0x32, 0x48, 0x02 };
        const uint8_t stateTransitionExpected[7] = { 0x68, 0x6D, 0xE5, 0xB4, 0x4B, 0xFF, 0xD0 };
        const uint8_t onInputChangedExpected[7] = { 0x68, 0xC8, 0x32, 0x48, 0x02, 0xFF, 0xD0 };

        bool ok = true;
        ok &= InstallOne(base + 0x527140, vehicleExpected, sizeof(vehicleExpected),
                         reinterpret_cast<uintptr_t>(InputDiagVehicleA_HookAsm),
                         g_InputDiagVehicleA_Return, "Vehicle Inputs wrapper A");
        ok &= InstallOne(base + 0x527150, vehicleExpected, sizeof(vehicleExpected),
                         reinterpret_cast<uintptr_t>(InputDiagVehicleB_HookAsm),
                         g_InputDiagVehicleB_Return, "Vehicle Inputs wrapper B");
        ok &= InstallOne(base + 0x527160, vehicleCExpected, sizeof(vehicleCExpected),
                         reinterpret_cast<uintptr_t>(InputDiagVehicleC_HookAsm),
                         g_InputDiagVehicleC_Return, "Vehicle Inputs wrapper C");
        ok &= InstallOne(base + 0x5270DC, stateTransitionExpected, sizeof(stateTransitionExpected),
                         reinterpret_cast<uintptr_t>(InputDiagStateTransition_HookAsm),
                         g_InputDiagStateTransition_Return, "input state 2/3 transition event");
        ok &= InstallOne(base + 0x527133, onInputChangedExpected, sizeof(onInputChangedExpected),
                         reinterpret_cast<uintptr_t>(InputDiagOnInputChanged_HookAsm),
                         g_InputDiagOnInputChanged_Return, "OnInputDeviceChanged dispatch");

        g_Installed = ok;
        Logger::Log("[INPUT-DIAG] Controller mapping diagnostics %s. Cold-event logging only; no input/profile behavior is changed.",
                    ok ? "armed" : "partially armed");
    }

    void UpdateControllerMappingDiagnostics() {
        if (!g_Installed && g_InputDiagVehicleA_Count == 0 &&
            g_InputDiagVehicleB_Count == 0 && g_InputDiagVehicleC_Count == 0 &&
            g_InputDiagStateTransition_Count == 0 && g_InputDiagOnInputChanged_Count == 0) {
            return;
        }

        const uint32_t nowA = g_InputDiagVehicleA_Count;
        const uint32_t nowB = g_InputDiagVehicleB_Count;
        const uint32_t nowC = g_InputDiagVehicleC_Count;
        const uint32_t nowTransition = g_InputDiagStateTransition_Count;
        const uint32_t nowOnInputChanged = g_InputDiagOnInputChanged_Count;

        if (nowTransition != g_LastStateTransition) {
            Logger::Log("[INPUT-DIAG] input state 2/3 transition: count %u -> %u, state=%u deviceFlag=%u",
                        g_LastStateTransition, nowTransition,
                        g_InputDiagStateTransition_State,
                        g_InputDiagStateTransition_DeviceFlag);
            g_LastStateTransition = nowTransition;
        }
        if (nowOnInputChanged != g_LastOnInputChanged) {
            Logger::Log("[INPUT-DIAG] OnInputDeviceChanged dispatched: count %u -> %u, state=%u deviceFlag=%u",
                        g_LastOnInputChanged, nowOnInputChanged,
                        g_InputDiagOnInputChanged_State,
                        g_InputDiagOnInputChanged_DeviceFlag);
            g_LastOnInputChanged = nowOnInputChanged;
        }
        if (nowA != g_LastVehicleA) {
            Logger::Log("[INPUT-DIAG] Vehicle Inputs wrapper A: count %u -> %u, this=0x%08X caller=0x%08X",
                        g_LastVehicleA, nowA,
                        static_cast<unsigned>(g_InputDiagVehicleA_This),
                        static_cast<unsigned>(g_InputDiagVehicleA_Caller));
            g_LastVehicleA = nowA;
        }
        if (nowB != g_LastVehicleB) {
            Logger::Log("[INPUT-DIAG] Vehicle Inputs wrapper B: count %u -> %u, this=0x%08X caller=0x%08X",
                        g_LastVehicleB, nowB,
                        static_cast<unsigned>(g_InputDiagVehicleB_This),
                        static_cast<unsigned>(g_InputDiagVehicleB_Caller));
            g_LastVehicleB = nowB;
        }
        if (nowC != g_LastVehicleC) {
            Logger::Log("[INPUT-DIAG] Vehicle Inputs wrapper C: count %u -> %u, this=0x%08X caller=0x%08X",
                        g_LastVehicleC, nowC,
                        static_cast<unsigned>(g_InputDiagVehicleC_This),
                        static_cast<unsigned>(g_InputDiagVehicleC_Caller));
            g_LastVehicleC = nowC;
        }
    }
}
