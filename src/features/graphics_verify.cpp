#include "features.h"
#include "../memory.h"
#include "../logger.h"
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

// Generic Frostbite graphics-settings verifier for NFS The Run v1.1.
//
// [GRAPHICS_VERIFY] accepts Class.Field=value and writes reflected settings live.
// DumpCurrentValues=1 enumerates the same reflected fields and logs their values.
//
// Important Frostbite 2 reflection layout (32-bit retail build):
//   TypeInfoData::size       @ +0x06 (uint16)
//   TypeInfoData::fieldCount @ +0x0D (uint8)
//   ClassInfoData::super     @ +0x10 (ClassInfo*)
//   ClassInfoData::fields    @ +0x18 (FieldInfoData*)
//   FieldInfoData stride     = 12 bytes
//
// Earlier research code accidentally read a uint16 at +0x04 as fieldCount and
// then scanned beyond it. That walked into adjacent reflection tables and produced
// bogus bools, NaNs and garbage pool sizes. This implementation follows the FB2
// layout and walks superclass reflection explicitly instead of over-scanning.

namespace {
    const uintptr_t kSettingsManagerPtr = 0x2446C74; // fb::g_settingsManager
    const uintptr_t kGetContainerFn     = 0x0E72D0;  // SettingsManager::getContainer

    struct GraphicsClass {
        const char* alias;
        uintptr_t typeInfoOffset;
        bool dumped;
    };

    GraphicsClass g_Classes[] = {
        { "GameRender",        0x2AACCBC - 0x400000, false },
        { "WorldRender",       0x2AE6E98 - 0x400000, false },
        { "VisualEnvironment", 0x2AE6E4C - 0x400000, false },
        { "GlobalPostProcess", 0x2AA26AC - 0x400000, false },
        { "VisualTerrain",     0x2AAF720 - 0x400000, false },
        { "VegetationSystem",  0x2AE741C - 0x400000, false },
        { "EmitterSystem",     0x2AD2B34 - 0x400000, false },
        { "DebrisSystem",      0x2AE735C - 0x400000, false },
        { "EnlightenRuntime",  0x2AE5FF8 - 0x400000, false },
        { "Occlusion",         0x2AE641C - 0x400000, false },
        { "Decal",             0x2AA1CA4 - 0x400000, false },
        { "Texture",           0x2AA0A38 - 0x400000, false },
        { "TextureStreaming",  0x2AA09E0 - 0x400000, false },
        { "Mesh",              0x2AA22C8 - 0x400000, false },
        { "MeshStreaming",     0x2AA22F4 - 0x400000, false },
        { "ShaderSystem",      0x2AA3428 - 0x400000, false },
        { "DxDisplay",         0x2AA0884 - 0x400000, false },
        { "DebugRender",       0x2A96600 - 0x400000, false },
        { "EffectManager",     0x2ABD588 - 0x400000, false },
    };

    const size_t kClassCount = sizeof(g_Classes) / sizeof(g_Classes[0]);

    // Reflection type objects from the exact v1.1 exe.
    const uintptr_t kTypeBool  = 0x2A95E38 - 0x400000;
    const uintptr_t kTypeIntA  = 0x2A95E88 - 0x400000;
    const uintptr_t kTypeIntB  = 0x2A95E98 - 0x400000;
    const uintptr_t kTypeFloat = 0x2A95EC8 - 0x400000;
    const uintptr_t kTypeVec2  = 0x2760A68 - 0x400000;
    const uintptr_t kTypeVec4A = 0x2760AA0 - 0x400000;
    const uintptr_t kTypeVec4B = 0x2760AD0 - 0x400000;

    typedef uintptr_t (__fastcall *GetContainerFn)(uintptr_t self, uintptr_t edx, uintptr_t typeInfo);

    struct Override {
        std::string className;
        std::string fieldName;
        std::string value;
        bool logged = false;
        bool missingLogged = false;
    };

    struct FieldInfo {
        uint16_t offset = 0;
        uintptr_t typePtr = 0;
        bool found = false;
        bool inherited = false;
    };

    std::vector<Override> g_Overrides;
    bool g_DumpCurrentValues = false;
    bool g_Initialized = false;

    bool IEquals(const std::string& a, const char* b) {
        if (!b || a.size() != std::strlen(b)) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    }

    bool SafeCStringEquals(uintptr_t ptr, const std::string& wanted) {
        if (ptr < 0x10000) return false;
        for (size_t i = 0; i <= wanted.size(); ++i) {
            if (!Memory::IsReadable(ptr + i, 1)) return false;
            const char c = *reinterpret_cast<const char*>(ptr + i);
            if (i == wanted.size()) return c == '\0';
            if (std::tolower(static_cast<unsigned char>(c)) !=
                std::tolower(static_cast<unsigned char>(wanted[i]))) return false;
        }
        return false;
    }

    std::string SafeCString(uintptr_t ptr, size_t maxLen = 96) {
        std::string out;
        if (ptr < 0x10000) return out;
        for (size_t i = 0; i < maxLen; ++i) {
            if (!Memory::IsReadable(ptr + i, 1)) break;
            const char c = *reinterpret_cast<const char*>(ptr + i);
            if (!c) break;
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) break;
            out.push_back(c);
        }
        return out;
    }

    GraphicsClass* FindClass(const std::string& alias) {
        for (size_t i = 0; i < kClassCount; ++i) {
            if (IEquals(alias, g_Classes[i].alias)) return &g_Classes[i];
        }
        return nullptr;
    }

    uintptr_t ResolveContainer(const GraphicsClass& cls) {
        const uintptr_t base = Memory::GetGameBase();
        if (!base) return 0;
        const uintptr_t mgrSlot = base + kSettingsManagerPtr;
        if (!Memory::IsReadable(mgrSlot, sizeof(uintptr_t))) return 0;
        const uintptr_t mgr = *reinterpret_cast<uintptr_t*>(mgrSlot);
        if (mgr < 0x10000) return 0;
        GetContainerFn fn = reinterpret_cast<GetContainerFn>(base + kGetContainerFn);
        const uintptr_t c = fn(mgr, 0, base + cls.typeInfoOffset);
        return (c >= 0x10000) ? c : 0;
    }

    uintptr_t RootTypeData(const GraphicsClass& cls) {
        const uintptr_t base = Memory::GetGameBase();
        if (!base) return 0;
        const uintptr_t typeInfo = base + cls.typeInfoOffset;
        if (!Memory::IsReadable(typeInfo, sizeof(uintptr_t))) return 0;
        const uintptr_t typeData = *reinterpret_cast<uintptr_t*>(typeInfo);
        if (typeData < 0x10000 || !Memory::IsReadable(typeData, 0x1C)) return 0;
        return typeData;
    }

    uint16_t TypeSize(uintptr_t typeData) {
        if (!typeData || !Memory::IsReadable(typeData + 0x06, sizeof(uint16_t))) return 0;
        return *reinterpret_cast<uint16_t*>(typeData + 0x06);
    }

    bool GetDeclaredFields(uintptr_t typeData, uintptr_t& fieldArray, uint8_t& fieldCount) {
        if (!typeData || !Memory::IsReadable(typeData + 0x18, sizeof(uintptr_t))) return false;
        if (!Memory::IsReadable(typeData + 0x0D, sizeof(uint8_t))) return false;

        fieldCount = *reinterpret_cast<uint8_t*>(typeData + 0x0D);
        fieldArray = *reinterpret_cast<uintptr_t*>(typeData + 0x18);

        if (fieldCount == 0) {
            fieldArray = 0;
            return true;
        }
        return fieldArray >= 0x10000 && Memory::IsReadable(fieldArray, static_cast<size_t>(fieldCount) * 12u);
    }

    uintptr_t GetSuperTypeData(uintptr_t typeData) {
        if (!typeData || !Memory::IsReadable(typeData + 0x10, sizeof(uintptr_t))) return 0;
        const uintptr_t superTypeInfo = *reinterpret_cast<uintptr_t*>(typeData + 0x10);
        if (superTypeInfo < 0x10000 || !Memory::IsReadable(superTypeInfo, sizeof(uintptr_t))) return 0;
        const uintptr_t superTypeData = *reinterpret_cast<uintptr_t*>(superTypeInfo);
        if (superTypeData < 0x10000 || superTypeData == typeData || !Memory::IsReadable(superTypeData, 0x1C)) return 0;
        return superTypeData;
    }

    bool ValidFieldOffset(uint16_t objectSize, uint16_t offset, size_t bytes) {
        if (objectSize == 0) return true;
        return static_cast<size_t>(offset) + bytes <= static_cast<size_t>(objectSize);
    }

    FieldInfo FindField(const GraphicsClass& cls, const std::string& fieldName) {
        FieldInfo out;
        uintptr_t typeData = RootTypeData(cls);
        if (!typeData) return out;
        const uint16_t objectSize = TypeSize(typeData);

        for (unsigned depth = 0; typeData && depth < 16; ++depth) {
            uintptr_t fields = 0;
            uint8_t count = 0;
            if (!GetDeclaredFields(typeData, fields, count)) return out;

            for (uint8_t i = 0; i < count; ++i) {
                const uintptr_t entry = fields + static_cast<uintptr_t>(i) * 12u;
                const uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(entry + 0);
                if (!SafeCStringEquals(namePtr, fieldName)) continue;

                const uint16_t off = *reinterpret_cast<uint16_t*>(entry + 6);
                if (!ValidFieldOffset(objectSize, off, 1)) return out;
                out.offset = off;
                out.typePtr = *reinterpret_cast<uintptr_t*>(entry + 8);
                out.found = true;
                out.inherited = (depth != 0);
                return out;
            }
            typeData = GetSuperTypeData(typeData);
        }
        return out;
    }

    enum class Kind { Bool, Int, Float, Vec2, Vec4, Unknown };

    Kind AutoKind(uintptr_t typePtr) {
        const uintptr_t base = Memory::GetGameBase();
        if (typePtr == base + kTypeBool) return Kind::Bool;
        if (typePtr == base + kTypeIntA || typePtr == base + kTypeIntB) return Kind::Int;
        if (typePtr == base + kTypeFloat) return Kind::Float;
        if (typePtr == base + kTypeVec2) return Kind::Vec2;
        if (typePtr == base + kTypeVec4A || typePtr == base + kTypeVec4B) return Kind::Vec4;
        return Kind::Unknown;
    }

    size_t KindSize(Kind k) {
        switch (k) {
            case Kind::Bool: return 1;
            case Kind::Int: return 4;
            case Kind::Float: return 4;
            case Kind::Vec2: return 8;
            case Kind::Vec4: return 16;
            default: return 4;
        }
    }

    bool ParseFloats(const char* s, float* out, int count) {
        for (int i = 0; i < count; ++i) {
            if (!s || !*s) return false;
            char* end = nullptr;
            out[i] = std::strtof(s, &end);
            if (end == s) return false;
            if (i + 1 < count) {
                while (*end == ' ' || *end == '\t') ++end;
                if (*end != ',') return false;
                s = end + 1;
            } else {
                while (*end == ' ' || *end == '\t') ++end;
                if (*end != '\0') return false;
            }
        }
        return true;
    }

    const char* KindName(Kind k) {
        switch (k) {
            case Kind::Bool: return "bool";
            case Kind::Int: return "int";
            case Kind::Float: return "float";
            case Kind::Vec2: return "vec2";
            case Kind::Vec4: return "vec4";
            default: return "unknown";
        }
    }

    bool ApplyOverride(Override& ov) {
        GraphicsClass* cls = FindClass(ov.className);
        if (!cls) {
            if (!ov.missingLogged) {
                Logger::Log("GRAPHICS_VERIFY unknown class '%s' for %s.%s.", ov.className.c_str(), ov.className.c_str(), ov.fieldName.c_str());
                ov.missingLogged = true;
            }
            return false;
        }

        const uintptr_t container = ResolveContainer(*cls);
        if (!container) return false;

        FieldInfo fi = FindField(*cls, ov.fieldName);
        if (!fi.found) {
            if (!ov.missingLogged) {
                Logger::Log("GRAPHICS_VERIFY field not found in declared/inherited reflection: %s.%s.", cls->alias, ov.fieldName.c_str());
                ov.missingLogged = true;
            }
            return false;
        }

        Kind kind = AutoKind(fi.typePtr);
        const char* value = ov.value.c_str();
        bool forcedKind = false;
        if (std::strlen(value) > 2 && value[1] == ':') {
            forcedKind = true;
            switch (std::tolower(static_cast<unsigned char>(value[0]))) {
                case 'b': kind = Kind::Bool; break;
                case 'i': kind = Kind::Int; break;
                case 'f': kind = Kind::Float; break;
                default: forcedKind = false; break;
            }
            if (forcedKind) value += 2;
        } else if (_strnicmp(value, "v2:", 3) == 0) {
            kind = Kind::Vec2;
            value += 3;
            forcedKind = true;
        } else if (_strnicmp(value, "v4:", 3) == 0) {
            kind = Kind::Vec4;
            value += 3;
            forcedKind = true;
        }

        const uintptr_t root = RootTypeData(*cls);
        const uint16_t objectSize = TypeSize(root);
        const size_t bytes = KindSize(kind);
        if (!ValidFieldOffset(objectSize, fi.offset, bytes)) return false;

        const uintptr_t addr = container + fi.offset;
        if (!Memory::IsReadable(addr, bytes)) return false;
        const char* inherited = fi.inherited ? " (inherited)" : "";

        switch (kind) {
            case Kind::Bool: {
                const uint8_t old = *reinterpret_cast<uint8_t*>(addr);
                const uint8_t wanted = (std::atoi(value) != 0) ? 1u : 0u;
                if (!ov.logged) Logger::Log("GRAPHICS_VERIFY %s.%s +0x%03X [%s%s] %u -> %u%s",
                    cls->alias, ov.fieldName.c_str(), fi.offset, forcedKind ? "forced " : "", KindName(kind),
                    static_cast<unsigned>(old), static_cast<unsigned>(wanted), inherited);
                if (old != wanted) *reinterpret_cast<uint8_t*>(addr) = wanted;
                break;
            }
            case Kind::Int: {
                const int32_t old = *reinterpret_cast<int32_t*>(addr);
                const int32_t wanted = static_cast<int32_t>(std::strtol(value, nullptr, 0));
                if (!ov.logged) Logger::Log("GRAPHICS_VERIFY %s.%s +0x%03X [%s%s] %d -> %d%s",
                    cls->alias, ov.fieldName.c_str(), fi.offset, forcedKind ? "forced " : "", KindName(kind),
                    old, wanted, inherited);
                if (old != wanted) *reinterpret_cast<int32_t*>(addr) = wanted;
                break;
            }
            case Kind::Float: {
                const float old = *reinterpret_cast<float*>(addr);
                const float wanted = std::strtof(value, nullptr);
                if (!ov.logged) Logger::Log("GRAPHICS_VERIFY %s.%s +0x%03X [%s%s] %.6g -> %.6g%s",
                    cls->alias, ov.fieldName.c_str(), fi.offset, forcedKind ? "forced " : "", KindName(kind),
                    old, wanted, inherited);
                if (old != wanted) *reinterpret_cast<float*>(addr) = wanted;
                break;
            }
            case Kind::Vec2: {
                float wanted[2];
                if (!ParseFloats(value, wanted, 2)) return false;
                float* p = reinterpret_cast<float*>(addr);
                if (!ov.logged) Logger::Log("GRAPHICS_VERIFY %s.%s +0x%03X [%s%s] (%.4g,%.4g) -> (%.4g,%.4g)%s",
                    cls->alias, ov.fieldName.c_str(), fi.offset, forcedKind ? "forced " : "", KindName(kind),
                    p[0], p[1], wanted[0], wanted[1], inherited);
                p[0] = wanted[0];
                p[1] = wanted[1];
                break;
            }
            case Kind::Vec4: {
                float wanted[4];
                if (!ParseFloats(value, wanted, 4)) return false;
                float* p = reinterpret_cast<float*>(addr);
                if (!ov.logged) Logger::Log("GRAPHICS_VERIFY %s.%s +0x%03X [%s%s] (%.4g,%.4g,%.4g,%.4g) -> (%.4g,%.4g,%.4g,%.4g)%s",
                    cls->alias, ov.fieldName.c_str(), fi.offset, forcedKind ? "forced " : "", KindName(kind),
                    p[0], p[1], p[2], p[3], wanted[0], wanted[1], wanted[2], wanted[3], inherited);
                for (int i = 0; i < 4; ++i) p[i] = wanted[i];
                break;
            }
            case Kind::Unknown:
            default:
                if (!ov.missingLogged) {
                    Logger::Log("GRAPHICS_VERIFY %s.%s has unsupported reflected type 0x%08X. Use i:/f:/b:/v2:/v4: to force a test type.",
                        cls->alias, ov.fieldName.c_str(), static_cast<unsigned>(fi.typePtr));
                    ov.missingLogged = true;
                }
                return false;
        }

        ov.logged = true;
        return true;
    }

    void DumpOneField(const GraphicsClass& cls, uintptr_t container, uint16_t objectSize,
                      uintptr_t entry, bool inherited) {
        if (!Memory::IsReadable(entry, 12)) return;
        const uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(entry + 0);
        const std::string name = SafeCString(namePtr);
        if (name.empty()) return;

        const uint16_t off = *reinterpret_cast<uint16_t*>(entry + 6);
        const uintptr_t typePtr = *reinterpret_cast<uintptr_t*>(entry + 8);
        const Kind kind = AutoKind(typePtr);
        const size_t bytes = KindSize(kind);
        if (!ValidFieldOffset(objectSize, off, bytes)) return;

        const uintptr_t addr = container + off;
        if (!Memory::IsReadable(addr, bytes)) return;
        const char* suffix = inherited ? " (inherited)" : "";

        switch (kind) {
            case Kind::Bool:
                Logger::Log("  %s.%s +0x%03X bool=%u%s", cls.alias, name.c_str(), off,
                    static_cast<unsigned>(*reinterpret_cast<uint8_t*>(addr)), suffix);
                break;
            case Kind::Int:
                Logger::Log("  %s.%s +0x%03X int=%d%s", cls.alias, name.c_str(), off,
                    *reinterpret_cast<int32_t*>(addr), suffix);
                break;
            case Kind::Float:
                Logger::Log("  %s.%s +0x%03X float=%.7g%s", cls.alias, name.c_str(), off,
                    *reinterpret_cast<float*>(addr), suffix);
                break;
            case Kind::Vec2: {
                float* p = reinterpret_cast<float*>(addr);
                Logger::Log("  %s.%s +0x%03X vec2=(%.5g,%.5g)%s", cls.alias, name.c_str(), off,
                    p[0], p[1], suffix);
                break;
            }
            case Kind::Vec4: {
                float* p = reinterpret_cast<float*>(addr);
                Logger::Log("  %s.%s +0x%03X vec4=(%.5g,%.5g,%.5g,%.5g)%s", cls.alias, name.c_str(), off,
                    p[0], p[1], p[2], p[3], suffix);
                break;
            }
            default:
                Logger::Log("  %s.%s +0x%03X type=0x%08X raw32=0x%08X%s", cls.alias, name.c_str(), off,
                    static_cast<unsigned>(typePtr), static_cast<unsigned>(*reinterpret_cast<uint32_t*>(addr)), suffix);
                break;
        }
    }

    void DumpClass(GraphicsClass& cls) {
        if (cls.dumped) return;
        const uintptr_t container = ResolveContainer(cls);
        if (!container) return;

        uintptr_t typeData = RootTypeData(cls);
        if (!typeData) return;
        const uint16_t objectSize = TypeSize(typeData);

        unsigned totalFields = 0;
        uintptr_t walk = typeData;
        for (unsigned depth = 0; walk && depth < 16; ++depth) {
            uintptr_t fields = 0;
            uint8_t count = 0;
            if (!GetDeclaredFields(walk, fields, count)) break;
            totalFields += count;
            walk = GetSuperTypeData(walk);
        }

        Logger::Log("=== GRAPHICS_VERIFY DUMP %s (%u reflected fields incl. inherited, objectSize=0x%X) container=0x%08X ===",
            cls.alias, totalFields, static_cast<unsigned>(objectSize), static_cast<unsigned>(container));

        walk = typeData;
        for (unsigned depth = 0; walk && depth < 16; ++depth) {
            uintptr_t fields = 0;
            uint8_t count = 0;
            if (!GetDeclaredFields(walk, fields, count)) break;
            for (uint8_t i = 0; i < count; ++i) {
                DumpOneField(cls, container, objectSize,
                    fields + static_cast<uintptr_t>(i) * 12u, depth != 0);
            }
            walk = GetSuperTypeData(walk);
        }
        cls.dumped = true;
    }
}

namespace Features {
    void InitGraphicsVerification(const char* iniPath) {
        if (!iniPath || !*iniPath) return;

        char buffer[32768] = {};
        const DWORD n = GetPrivateProfileSectionA("GRAPHICS_VERIFY", buffer, sizeof(buffer), iniPath);
        if (n == 0) {
            Logger::Log("GRAPHICS_VERIFY: no overrides configured.");
            g_Initialized = true;
            return;
        }

        for (char* p = buffer; *p; p += std::strlen(p) + 1) {
            char* eq = std::strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            std::string key(p);
            std::string value(eq + 1);

            if (IEquals(key, "DumpCurrentValues")) {
                g_DumpCurrentValues = (std::atoi(value.c_str()) != 0);
                continue;
            }

            const size_t dot = key.find('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 >= key.size()) {
                Logger::Log("GRAPHICS_VERIFY ignored malformed key '%s' (expected Class.Field).", key.c_str());
                continue;
            }

            Override ov;
            ov.className = key.substr(0, dot);
            ov.fieldName = key.substr(dot + 1);
            ov.value = value;
            g_Overrides.push_back(ov);
        }

        Logger::Log("GRAPHICS_VERIFY initialized: %u overrides, DumpCurrentValues=%d.",
            static_cast<unsigned>(g_Overrides.size()), g_DumpCurrentValues ? 1 : 0);
        g_Initialized = true;
    }

    void UpdateGraphicsVerification() {
        if (!g_Initialized) return;

        if (g_DumpCurrentValues) {
            for (size_t i = 0; i < kClassCount; ++i) DumpClass(g_Classes[i]);
        }

        for (size_t i = 0; i < g_Overrides.size(); ++i) {
            ApplyOverride(g_Overrides[i]);
        }
    }
}
