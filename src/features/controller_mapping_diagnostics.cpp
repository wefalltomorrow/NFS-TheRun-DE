#include "features.h"
#include "../memory.h"
#include "../logger.h"
#include <windows.h>
#include <cstdint>

// Logging-only instrumentation for the controller-mapping persistence bug.
// This deliberately does not modify profile/input behavior.  It counts entries
// into the three small "Vehicle Inputs" wrappers and the nearby input-device
// change path so a real startup log can tell us which operation runs when the
// saved custom bindings are replaced by defaults.

extern "C" {
    volatile uint32_t g_InputDiagDeviceChangedCount = 0;
    volatile uint32_t g_InputDiagVehicleA_Count = 0;
    volatile uint32_t g_InputDiagVehicleB_Count = 0;
    volatile uint32_t g_InputDiagVehicleC_Count = 0;

    uintptr_t g_InputDiagDeviceChangedReturn = 0;
    uintptr_t g_InputDiagVehicleA_Return = 0;
    uintptr_t g_InputDiagVehicleB_Return = 0;
    uintptr_t g_InputDiagVehicleC_Return = 0;
    uintptr_t g_InputDiagVehicleInputsName = 0;

    void InputDiagDeviceChangedHookAsm();
    void InputDiagVehicleA_HookAsm();
    void InputDiagVehicleB_HookAsm();
    void InputDiagVehicleC_HookAsm();
}

// exe+0x5270A0 (absolute 0x009270A0 at the stock 0x00400000 base)
// Stolen bytes: push esi ; mov esi,ecx ; mov eax,[esi+3C]
asm(
    ".text\n"
    ".globl _InputDiagDeviceChangedHookAsm\n"
    "_InputDiagDeviceChangedHookAsm:\n"
    "    pushfl\n"
    "    incl _g_InputDiagDeviceChangedCount\n"
    "    popfl\n"
    "    pushl %esi\n"
    "    movl %ecx, %esi\n"
    "    movl 0x3c(%esi), %eax\n"
    "    jmpl *_g_InputDiagDeviceChangedReturn\n"
);

// exe+0x527140. Stolen bytes: push 0x024832F4 ("Vehicle Inputs")
asm(
    ".text\n"
    ".globl _InputDiagVehicleA_HookAsm\n"
    "_InputDiagVehicleA_HookAsm:\n"
    "    pushfl\n"
    "    incl _g_InputDiagVehicleA_Count\n"
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
    "    incl _g_InputDiagVehicleB_Count\n"
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
    "    incl _g_InputDiagVehicleC_Count\n"
    "    popfl\n"
    "    movb $0x01, 0x4e(%ecx)\n"
    "    pushl _g_InputDiagVehicleInputsName\n"
    "    jmpl *_g_InputDiagVehicleC_Return\n"
);

namespace {
    bool g_Installed = false;
    uint32_t g_LastDeviceChanged = 0;
    uint32_t g_LastVehicleA = 0;
    uint32_t g_LastVehicleB = 0;
    uint32_t g_LastVehicleC = 0;

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
        uintptr_t base = Memory::GetGameBase();
        g_InputDiagVehicleInputsName = base + 0x20832F4;

        const uint8_t deviceExpected[6] = { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x3C };
        const uint8_t vehicleExpected[5] = { 0x68, 0xF4, 0x32, 0x48, 0x02 };
        const uint8_t vehicleCExpected[9] = { 0xC6, 0x41, 0x4E, 0x01, 0x68, 0xF4, 0x32, 0x48, 0x02 };

        bool ok = true;
        ok &= InstallOne(base + 0x5270A0, deviceExpected, sizeof(deviceExpected),
                         reinterpret_cast<uintptr_t>(InputDiagDeviceChangedHookAsm),
                         g_InputDiagDeviceChangedReturn, "input-device-change path");
        ok &= InstallOne(base + 0x527140, vehicleExpected, sizeof(vehicleExpected),
                         reinterpret_cast<uintptr_t>(InputDiagVehicleA_HookAsm),
                         g_InputDiagVehicleA_Return, "Vehicle Inputs wrapper A");
        ok &= InstallOne(base + 0x527150, vehicleExpected, sizeof(vehicleExpected),
                         reinterpret_cast<uintptr_t>(InputDiagVehicleB_HookAsm),
                         g_InputDiagVehicleB_Return, "Vehicle Inputs wrapper B");
        ok &= InstallOne(base + 0x527160, vehicleCExpected, sizeof(vehicleCExpected),
                         reinterpret_cast<uintptr_t>(InputDiagVehicleC_HookAsm),
                         g_InputDiagVehicleC_Return, "Vehicle Inputs wrapper C");

        g_Installed = ok;
        Logger::Log("[INPUT-DIAG] Controller mapping diagnostics %s. Logging only; no input/profile behavior is changed.",
                    ok ? "armed" : "partially armed");
    }

    void UpdateControllerMappingDiagnostics() {
        if (!g_Installed && g_InputDiagDeviceChangedCount == 0 &&
            g_InputDiagVehicleA_Count == 0 && g_InputDiagVehicleB_Count == 0 &&
            g_InputDiagVehicleC_Count == 0) {
            return;
        }

        uint32_t nowDevice = g_InputDiagDeviceChangedCount;
        uint32_t nowA = g_InputDiagVehicleA_Count;
        uint32_t nowB = g_InputDiagVehicleB_Count;
        uint32_t nowC = g_InputDiagVehicleC_Count;

        if (nowDevice != g_LastDeviceChanged) {
            Logger::Log("[INPUT-DIAG] input-device-change path entered: count %u -> %u",
                        g_LastDeviceChanged, nowDevice);
            g_LastDeviceChanged = nowDevice;
        }
        if (nowA != g_LastVehicleA) {
            Logger::Log("[INPUT-DIAG] Vehicle Inputs wrapper A entered: count %u -> %u",
                        g_LastVehicleA, nowA);
            g_LastVehicleA = nowA;
        }
        if (nowB != g_LastVehicleB) {
            Logger::Log("[INPUT-DIAG] Vehicle Inputs wrapper B entered: count %u -> %u",
                        g_LastVehicleB, nowB);
            g_LastVehicleB = nowB;
        }
        if (nowC != g_LastVehicleC) {
            Logger::Log("[INPUT-DIAG] Vehicle Inputs wrapper C entered: count %u -> %u",
                        g_LastVehicleC, nowC);
            g_LastVehicleC = nowC;
        }
    }
}
