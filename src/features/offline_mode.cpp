#include "features.h"
#include "../memory.h"
#include "../logger.h"
#include <cstdint>

// Offline-only optimisation for users who deliberately never use Autolog or
// multiplayer. This targets NFS/EA online-service settings only. It does NOT touch
// Frostbite NetworkSettings because the engine can use client/server networking
// internally even in single-player.
namespace {
    const uintptr_t kSettingsManagerPtr = 0x2446C74; // fb::g_settingsManager
    const uintptr_t kGetContainerFn     = 0x0E72D0;  // SettingsManager::getContainer

    // Exact NFS The Run v1.1 TypeInfo object recovered from the retail executable.
    // NfsOnlineSettings TypeInfo @ 0x02ADF6B0.
    const uintptr_t kNfsOnlineTypeInfo  = 0x2ADF6B0 - 0x400000;

    typedef uintptr_t (__fastcall *GetContainerFn)(uintptr_t self, uintptr_t edx, uintptr_t typeInfo);

    bool g_Enabled = false;
    bool g_Logged = false;

    uintptr_t ResolveNfsOnlineSettings() {
        const uintptr_t base = Memory::GetGameBase();
        if (!base) return 0;

        const uintptr_t mgrSlot = base + kSettingsManagerPtr;
        if (!Memory::IsReadable(mgrSlot, sizeof(uintptr_t))) return 0;
        const uintptr_t mgr = *reinterpret_cast<uintptr_t*>(mgrSlot);
        if (mgr < 0x10000) return 0;

        GetContainerFn fn = reinterpret_cast<GetContainerFn>(base + kGetContainerFn);
        const uintptr_t c = fn(mgr, 0, base + kNfsOnlineTypeInfo);
        return (c >= 0x10000) ? c : 0;
    }

    inline uint8_t ReadBool(uintptr_t c, uintptr_t off) {
        if (!Memory::IsReadable(c + off, sizeof(uint8_t))) return 0;
        return *reinterpret_cast<uint8_t*>(c + off);
    }

    inline void WriteBool(uintptr_t c, uintptr_t off, bool value) {
        if (!Memory::IsReadable(c + off, sizeof(uint8_t))) return;
        uint8_t* p = reinterpret_cast<uint8_t*>(c + off);
        const uint8_t wanted = value ? 1u : 0u;
        if (*p != wanted) *p = wanted;
    }

    // Inherited OnlineSettings fields.
    const uintptr_t O_MatchmakeImmediately      = 0x02D;
    const uintptr_t O_SupportHostMigration      = 0x02F;
    const uintptr_t O_SendTestMessages          = 0x030;

    // NfsOnlineSettings fields from the exact v1.1 reflection table.
    const uintptr_t N_EnableVoip                         = 0x082;
    const uintptr_t N_EnableProtoTunnel                  = 0x087;
    const uintptr_t N_UseLocalV8Server                   = 0x088;
    const uintptr_t N_MatchmakeImmediately               = 0x08A;
    const uintptr_t N_ServerTelemetryEnabled             = 0x08B;
    const uintptr_t N_ClientTelemetryEnabled             = 0x08C;
    const uintptr_t N_CheckWinConditionForGameReports    = 0x08D;
    const uintptr_t N_CheckBlazeStatsForSplitTimeUpload  = 0x08E;
    const uintptr_t N_ShowMeInSplitTimes                 = 0x08F;
    const uintptr_t N_AllowSinglePlayerOnline            = 0x091;
    const uintptr_t N_UseFallback                        = 0x092;

    void ApplyOfflineSettings(uintptr_t c) {
        if (!g_Logged) {
            Logger::Log(
                "Offline mode: NfsOnlineSettings at 0x%08X; stock SinglePlayerOnline=%u ClientTelemetry=%u ServerTelemetry=%u Matchmake=%u/%u Voip=%u ProtoTunnel=%u Fallback=%u GameReports=%u BlazeSplitUpload=%u ShowInSplitTimes=%u",
                static_cast<unsigned>(c),
                static_cast<unsigned>(ReadBool(c, N_AllowSinglePlayerOnline)),
                static_cast<unsigned>(ReadBool(c, N_ClientTelemetryEnabled)),
                static_cast<unsigned>(ReadBool(c, N_ServerTelemetryEnabled)),
                static_cast<unsigned>(ReadBool(c, O_MatchmakeImmediately)),
                static_cast<unsigned>(ReadBool(c, N_MatchmakeImmediately)),
                static_cast<unsigned>(ReadBool(c, N_EnableVoip)),
                static_cast<unsigned>(ReadBool(c, N_EnableProtoTunnel)),
                static_cast<unsigned>(ReadBool(c, N_UseFallback)),
                static_cast<unsigned>(ReadBool(c, N_CheckWinConditionForGameReports)),
                static_cast<unsigned>(ReadBool(c, N_CheckBlazeStatsForSplitTimeUpload)),
                static_cast<unsigned>(ReadBool(c, N_ShowMeInSplitTimes)));
            Logger::Log("Offline mode: suppressing Autolog/Blaze-facing single-player online, telemetry, matchmaking, VOIP, fallback and upload/report flags. Frostbite NetworkSettings remain untouched.");
            g_Logged = true;
        }

        // These switches belong to EA/Autolog/multiplayer-facing services and are
        // unnecessary in a deliberately offline single-player session.
        WriteBool(c, O_MatchmakeImmediately,     false);
        WriteBool(c, O_SupportHostMigration,     false);
        WriteBool(c, O_SendTestMessages,         false);

        WriteBool(c, N_EnableVoip,                        false);
        WriteBool(c, N_EnableProtoTunnel,                 false);
        WriteBool(c, N_UseLocalV8Server,                  false);
        WriteBool(c, N_MatchmakeImmediately,              false);
        WriteBool(c, N_ServerTelemetryEnabled,            false);
        WriteBool(c, N_ClientTelemetryEnabled,            false);
        WriteBool(c, N_CheckWinConditionForGameReports,   false);
        WriteBool(c, N_CheckBlazeStatsForSplitTimeUpload, false);
        WriteBool(c, N_ShowMeInSplitTimes,                false);
        WriteBool(c, N_AllowSinglePlayerOnline,           false);
        WriteBool(c, N_UseFallback,                       false);
    }
}

namespace Features {
    void InitOfflineMode(bool enabled) {
        g_Enabled = enabled;
        if (!g_Enabled) {
            Logger::Log("Offline mode disabled; NFS online/Autolog settings left to the game.");
        }
    }

    void UpdateOfflineMode() {
        if (!g_Enabled) return;
        const uintptr_t c = ResolveNfsOnlineSettings();
        if (!c) return;
        ApplyOfflineSettings(c);
    }
}
