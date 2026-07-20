#include "config.h"

#include <algorithm>
#include <fstream>

#include "json.h"
#include "util.h"

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
}

void Config::WriteDefaultIfMissing(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) return;
    std::ofstream f(path);
    f << R"({
  "fps": 24,
  "cellScale": 4,
  "rotateOnResumeSeconds": 15,
  "deathCheckSeconds": 2,
  "deathGraceSeconds": 4,
  "pauseOnBattery": true,
  "occludeCoverage": 0.9,
  "maxKernelRadius": 25,
  "catalogDir": "catalog",
  "species": "",
  "disableAutoPause": false
}
)";
}
