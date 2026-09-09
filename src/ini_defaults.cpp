#include <windows.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
    struct DefaultEntry {
        const char* section;
        const char* key;
        const char* value;
    };

    // Keep this list in sync with Config::Load(). Existing user values are never
    // overwritten; these are only inserted when a key is genuinely absent.
    const DefaultEntry kDefaults[] = {
        { "MAIN", "DebugLog", "1" },

        { "GAMEPLAY", "RandomizeTimeOfDay", "0" },
        { "GAMEPLAY", "RunForYourLife", "1" },

        { "GRAPHICS_FPS", "EnableFramerateUnlocker", "1" },
        { "GRAPHICS_FPS", "FPSLimit", "60" },
        { "GRAPHICS_FPS", "UnlockCutsceneFPS", "0" },
        { "GRAPHICS_FPS", "ClampSimRateWhenNoControl", "1" },

        { "GRAPHICS_QUALITY", "AntiAliasing", "4" },
        { "GRAPHICS_QUALITY", "FastLoadingVSyncBypass", "1" },
        { "GRAPHICS_QUALITY", "ForceMeshLod", "0" },
        { "GRAPHICS_QUALITY", "MeshGlobalLodScale", "1000" },
        { "GRAPHICS_QUALITY", "VinylTargetSize", "4096" },

        { "UI_DEBUG", "EnableExtraUIOptions", "0" },
        { "UI_DEBUG", "AlwaysShowPhotoMode", "1" },

        { "TRACK_RULES", "DisableCheckpointTimer", "0" },
        { "TRACK_RULES", "DisableResetOOB", "0" },
        { "TRACK_RULES", "DisableWrongWayRespawn", "0" },

        { "TRAFFIC", "EnableTrafficControls", "0" },
        { "TRAFFIC", "TrafficDensityScale", "0.05" },
        { "TRAFFIC", "TrafficMaxDensity", "0.15" },
        { "TRAFFIC", "TrafficVehicleLimit", "25" },

        { "VEHICLE", "DisablePlayerAssists", "0" },
        { "VEHICLE", "VehicleHealth", "-1" },
        { "VEHICLE", "AiSkillScale", "1" },
        { "VEHICLE", "AiGlueScale", "1" },
        { "VEHICLE", "AiNosRechargeScale", "1" },
        { "VEHICLE", "PlayerNosRechargeScale", "1" },
        { "VEHICLE", "PlayerNosBonusScale", "1" },
        { "VEHICLE", "PlayerNosStrengthScale", "1" },
        { "VEHICLE", "PlayerNosBoostScale", "1" },
        { "VEHICLE", "PlayerDraftRateScale", "1" },

        { "HIGH_FPS_FIXES", "FixEngineAudioSlew", "1" },
        { "HIGH_FPS_FIXES", "FixKickupParticles", "1" },
        { "HIGH_FPS_FIXES", "KickupVelocityScale", "-1" },

        { "RENDER", "EnableRenderTweaks", "1" },
        { "RENDER", "ForceFov", "60" },
        { "RENDER", "ForceFovOnlyWhileDriving", "1" },
        { "RENDER", "ForceRenderShiftEnabled", "0" },
        { "RENDER", "ForceRenderShiftX", "0" },
        { "RENDER", "ForceRenderShiftY", "-0.017" },
        { "RENDER", "ForceRoll", "0" },
        { "RENDER", "FixMinimapRendering", "1" },

        { "WORLDRENDER", "EnableWorldRenderTweaks", "0" },
        { "WORLDRENDER", "ShadowmapResolution", "-1" },
        { "WORLDRENDER", "ShadowmapQuality", "-1" },
        { "WORLDRENDER", "ShadowmapViewDistance", "-1" },

        { "TEXTURE", "AnisotropicFiltering", "-1" },

        { "GRAPHICS_VERIFY", "DumpCurrentValues", "0" },

        { "DIAGNOSTICS", "LogNosAwards", "0" },
        { "DIAGNOSTICS", "LogGinsuDiagnostics", "0" },
        { "DIAGNOSTICS", "LogSettingsContainers", "0" },
    };

    std::string Trim(const std::string& s) {
        size_t first = 0;
        while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
        size_t last = s.size();
        while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) --last;
        return s.substr(first, last - first);
    }

    std::string Lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    bool ParseSection(const std::string& line, std::string& section) {
        std::string t = Trim(line);
        if (t.size() < 3 || t.front() != '[' || t.back() != ']') return false;
        section = Lower(Trim(t.substr(1, t.size() - 2)));
        return !section.empty();
    }

    bool ParseKey(const std::string& line, std::string& key) {
        std::string t = Trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') return false;
        size_t eq = t.find('=');
        if (eq == std::string::npos) return false;
        key = Lower(Trim(t.substr(0, eq)));
        return !key.empty();
    }

    std::vector<std::string> SplitLines(const std::string& text) {
        std::vector<std::string> lines;
        std::string line;
        std::istringstream in(text);
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        if (text.empty()) lines.clear();
        return lines;
    }
}

namespace Config {
    // Returns the number of keys inserted, 0 if the INI was already complete,
    // or -1 if the file could not be safely rewritten.
    int EnsureIniDefaults(const std::string& iniPath) {
        std::ifstream input(iniPath.c_str(), std::ios::binary);
        std::string original;
        if (input) {
            std::ostringstream ss;
            ss << input.rdbuf();
            original = ss.str();
        }

        const std::string eol = (original.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
        std::vector<std::string> lines = SplitLines(original);

        std::map<std::string, std::set<std::string> > present;
        std::set<std::string> sectionsPresent;
        std::string current;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string section;
            if (ParseSection(lines[i], section)) {
                current = section;
                sectionsPresent.insert(current);
                continue;
            }
            if (current.empty()) continue;
            std::string key;
            if (ParseKey(lines[i], key)) present[current].insert(key);
        }

        std::map<std::string, std::vector<const DefaultEntry*> > missing;
        std::vector<std::string> sectionOrder;
        int added = 0;
        for (size_t i = 0; i < sizeof(kDefaults) / sizeof(kDefaults[0]); ++i) {
            const DefaultEntry& d = kDefaults[i];
            const std::string section = Lower(d.section);
            const std::string key = Lower(d.key);
            if (present[section].find(key) != present[section].end()) continue;
            if (missing.find(section) == missing.end()) sectionOrder.push_back(section);
            missing[section].push_back(&d);
            ++added;
        }

        if (added == 0) return 0;

        std::vector<std::string> out;
        out.reserve(lines.size() + static_cast<size_t>(added) + sectionOrder.size() * 2u);
        current.clear();

        const auto appendMissingFor = [&](const std::string& section, std::vector<std::string>& dst) {
            std::map<std::string, std::vector<const DefaultEntry*> >::iterator it = missing.find(section);
            if (it == missing.end() || it->second.empty()) return;
            if (!dst.empty() && !Trim(dst.back()).empty()) dst.push_back("");
            for (size_t j = 0; j < it->second.size(); ++j) {
                const DefaultEntry* d = it->second[j];
                dst.push_back(std::string(d->key) + " = " + d->value);
            }
            it->second.clear();
        };

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string nextSection;
            if (ParseSection(lines[i], nextSection)) {
                if (!current.empty()) appendMissingFor(current, out);
                current = nextSection;
            }
            out.push_back(lines[i]);
        }
        if (!current.empty()) appendMissingFor(current, out);

        // Sections that do not exist at all are appended once, in the same order
        // as the defaults table. Duplicate user sections are left untouched.
        for (size_t i = 0; i < sectionOrder.size(); ++i) {
            const std::string& section = sectionOrder[i];
            std::map<std::string, std::vector<const DefaultEntry*> >::iterator it = missing.find(section);
            if (it == missing.end() || it->second.empty()) continue;
            if (!out.empty() && !Trim(out.back()).empty()) out.push_back("");
            out.push_back(std::string("[") + it->second[0]->section + "]");
            for (size_t j = 0; j < it->second.size(); ++j) {
                const DefaultEntry* d = it->second[j];
                out.push_back(std::string(d->key) + " = " + d->value);
            }
            it->second.clear();
        }

        const std::string tempPath = iniPath + ".nfstr.tmp";
        {
            std::ofstream output(tempPath.c_str(), std::ios::binary | std::ios::trunc);
            if (!output) return -1;
            for (size_t i = 0; i < out.size(); ++i) {
                output << out[i];
                if (i + 1 < out.size() || !out.empty()) output << eol;
            }
            if (!output.good()) {
                output.close();
                DeleteFileA(tempPath.c_str());
                return -1;
            }
        }

        if (!MoveFileExA(tempPath.c_str(), iniPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileA(tempPath.c_str());
            return -1;
        }
        return added;
    }
}
