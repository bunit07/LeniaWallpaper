#include "config.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

#include "json.h"
#include "util.h"

namespace {

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
            break;
        }
    }
    return out;
}

}  // namespace

void Config::Load(const std::filesystem::path& path) {
    std::string text;
    if (!ReadFileBytes(path, &text)) return;
    json::Value v;
    std::string err;
    if (!json::parse(text, &v, &err)) {
        LogLine("config.json parse error: %s (using defaults)", err.c_str());
        return;
    }
    fps = std::clamp((int)v.fieldNum("fps", fps), 1, 60);
    cellScale = std::clamp((int)v.fieldNum("cellScale", cellScale), 1, 16);
    rotateOnResumeSeconds = std::max(1, (int)v.fieldNum("rotateOnResumeSeconds", rotateOnResumeSeconds));
    deathCheckSeconds = std::max(1, (int)v.fieldNum("deathCheckSeconds", deathCheckSeconds));
    deathGraceSeconds = std::max(0, (int)v.fieldNum("deathGraceSeconds", deathGraceSeconds));
    pauseOnBattery = v.fieldBool("pauseOnBattery", pauseOnBattery);
    occludeCoverage = std::clamp(v.fieldNum("occludeCoverage", occludeCoverage), 0.3, 1.0);
    maxKernelRadius = std::clamp(v.fieldNum("maxKernelRadius", maxKernelRadius), 5.0, 64.0);
    catalogDir = v.fieldStr("catalogDir", catalogDir);
    species = v.fieldStr("species", species);
    disableAutoPause = v.fieldBool("disableAutoPause", disableAutoPause);
    favoriteChance = std::clamp(v.fieldNum("favoriteChance", favoriteChance), 0.0, 1.0);
    discoveryEnabled = v.fieldBool("discoveryEnabled", discoveryEnabled);
    randomSoup = v.fieldBool("randomSoup", randomSoup);

    favorites.clear();
    if (const json::Value* fav = v.find("favorites"); fav && fav->type == json::Value::Array) {
        favorites.reserve(fav->arr.size());
        for (const auto& e : fav->arr) {
            if (e.type != json::Value::String || e.str.empty()) continue;
            if (std::find(favorites.begin(), favorites.end(), e.str) == favorites.end())
                favorites.push_back(e.str);
        }
    }
}

void Config::Save(const std::filesystem::path& path) const {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) {
            LogLine("config.json: cannot write temp file");
            return;
        }
        f << "{\n";
        f << "  \"fps\": " << fps << ",\n";
        f << "  \"cellScale\": " << cellScale << ",\n";
        f << "  \"rotateOnResumeSeconds\": " << rotateOnResumeSeconds << ",\n";
        f << "  \"deathCheckSeconds\": " << deathCheckSeconds << ",\n";
        f << "  \"deathGraceSeconds\": " << deathGraceSeconds << ",\n";
        f << "  \"pauseOnBattery\": " << (pauseOnBattery ? "true" : "false") << ",\n";
        f << "  \"occludeCoverage\": " << occludeCoverage << ",\n";
        f << "  \"maxKernelRadius\": " << maxKernelRadius << ",\n";
        f << "  \"catalogDir\": \"" << JsonEscape(catalogDir) << "\",\n";
        f << "  \"species\": \"" << JsonEscape(species) << "\",\n";
        f << "  \"disableAutoPause\": " << (disableAutoPause ? "true" : "false") << ",\n";
        f << "  \"favoriteChance\": " << favoriteChance << ",\n";
        f << "  \"discoveryEnabled\": " << (discoveryEnabled ? "true" : "false") << ",\n";
        f << "  \"randomSoup\": " << (randomSoup ? "true" : "false") << ",\n";
        f << "  \"favorites\": [";
        for (size_t i = 0; i < favorites.size(); i++) {
            if (i) f << ", ";
            f << "\"" << JsonEscape(favorites[i]) << "\"";
        }
        f << "]\n";
        f << "}\n";
        if (!f) {
            LogLine("config.json: write failed");
            return;
        }
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        LogLine("config.json: rename failed (%s)", ec.message().c_str());
        std::filesystem::remove(tmp, ec);
    }
}

void Config::WriteDefaultIfMissing(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) return;
    Config def;
    def.Save(path);
}
