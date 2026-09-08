#include "features.h"
#include "../memory.h"
#include "../logger.h"
#include <windows.h>
#include <cstdint>

// Low-impact, logging-only instrumentation for the controller-mapping
// persistence bug.
//
// The first diagnostic build also hooked exe+0x5270A0 because it sits near the
// OnInputDeviceChanged-related code. A good-mapping run proved that site is a
// hot path (thousands of entries in under a minute), so hooking/logging it was
// both noisy and capable of perturbing startup timing. That hook is deliberately
// gone here.
//
// We now instrument only the three tiny helpers that directly reference the
// "Vehicle Inputs" profile property. Besides a count, each hook snapshots ECX
// and the caller return address so a good-vs-reset run can identify which helper
// and call site participates in the overwrite.

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

    uintptr_t g_InputDiagVehicleA_Return = 0;
    uintptr_t g_InputDiagVehicleB_Return = 0;
    uintptr_t g_InputDiagVehicleC_Return = 0;
    uintptr_t g_InputDiagVehicleInputsName = 0;

    void InputDiagVehicleA_HookAsm();
    void InputDiagVehicleB_HookAsm();
    void InputDiagVehicleC_HookAsm();
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

namespace {
    bool g_Installed = false;
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

        const uint8_t vehicleExpected[5] = { 0x68, 0xF4, 0x32, 0x48, 0x02 };
        const uint8_t vehicleCExpected[9] = { 0xC6, 0x41, 0x4E, 0x01, 0x68, 0xF4, 0x32, 0x48, 0x02 };

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

        g_Installed = ok;
        Logger::Log("[INPUT-DIAG] Controller mapping diagnostics %s. Low-impact logging only; no input/profile behavior is changed.",
                    ok ? "armed" : "partially armed");
    }

    void UpdateControllerMappingDiagnostics() {
        if (!g_Installed && g_InputDiagVehicleA_Count == 0 &&
            g_InputDiagVehicleB_Count == 0 && g_InputDiagVehicleC_Count == 0) {
            return;
        }

        const uint32_t nowA = g_InputDiagVehicleA_Count;
        const uint32_t nowB = g_InputDiagVehicleB_Count;
        const uint32_t nowC = g_InputDiagVehicleC_Count;

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
