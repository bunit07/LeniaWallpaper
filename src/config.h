#pragma once
#include <filesystem>
#include <string>

struct Config {
    int fps = 24;                      // render/sim rate, clamped 1..60
    int cellScale = 4;                 // screen pixels per sim cell
    int rotateOnResumeSeconds = 15;    // auto pause longer than this -> new species on resume
    int deathCheckSeconds = 2;         // cadence of the dead/static grid probe
    int deathGraceSeconds = 4;         // wait after death detected before rotating
    bool pauseOnBattery = true;
    double occludeCoverage = 0.9;      // foreground window covering this much of the monitor pauses
    double maxKernelRadius = 25.0;     // skip species with R above this (GPU cost grows ~R^2)
    std::string catalogDir = "catalog";
    std::string species;               // fixed species (catalog-relative path); empty = random
    bool disableAutoPause = false;     // debug: ignore battery/fullscreen/occlusion/lock pauses

    void Load(const std::filesystem::path& path);
    static void WriteDefaultIfMissing(const std::filesystem::path& path);
};
