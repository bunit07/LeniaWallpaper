#pragma once
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

// 16 kernel lanes, mirroring the site's mat4 packing (max observed kernels: 15).
constexpr int kLanes = 16;

struct SpeciesInit {
    bool isSeed = false;       // else "random" soup
    double baseNoise = 0.1;
    double randomScale = 1.0;
    int sh = 0, sw = 0, sc = 0;  // seed: rows, cols, channels
    std::vector<float> cells;
};

struct Species {
    std::string name;
    std::string catalogPath;  // catalog-relative path, e.g. "named/fission.json"
    double R = 12.0, T = 2.0;
    int worldSize = 128;   // grid the species was tuned on (seed-stamp density)
    int numKernels = 0;
    double betaLen[kLanes]{}, beta0[kLanes]{}, beta1[kLanes]{}, beta2[kLanes]{};
    double mu[kLanes]{}, sigma[kLanes]{}, eta[kLanes]{}, relR[kLanes]{};
    int src[kLanes]{}, dst[kLanes]{};
    bool mono = false;
    SpeciesInit init;
};

// One neighbourhood tap: integer offset + per-lane weights pre-multiplied by the
// source-channel masks and pre-normalized by the per-lane weight total. Layout
// matches the HLSL StructuredBuffer<Tap> exactly (200-byte stride).
struct Tap {
    int32_t dx, dy;
    float wR[kLanes];  // lane weight if src[lane] == 0 (red), else 0
    float wG[kLanes];
    float wB[kLanes];
};
static_assert(sizeof(Tap) == 200, "Tap must match HLSL structured buffer stride");

// Mirrors cbuffer SimParams (400 bytes, 16-byte multiple).
struct SimConstants {
    int32_t gridW, gridH, tapCount;
    float invT;
    float mu[kLanes], sigma[kLanes], eta[kLanes];
    float dstR[kLanes], dstG[kLanes], dstB[kLanes];
};
static_assert(sizeof(SimConstants) % 16 == 0, "cbuffer size must be 16-byte aligned");

bool ParseSpecies(const std::filesystem::path& file, Species* out, std::string* err);

// Replace anonymous / numbered labels with "#ID : Name" for overlay + tray.
// Real catalog names keep their label; discovered ones get a Latin binomial.
void AssignDisplayName(Species& s);

// CPU precompute of the kernel weights the site evaluates per pixel per frame
// (getWeight: ring bell curves with global ring width 0.15). Zero-weight taps
// are dropped; weights are normalized so the shader skips the total-divide.
std::vector<Tap> BuildTaps(const Species& s);

SimConstants BuildConstants(const Species& s, int gridW, int gridH, int tapCount);

// Initial grid as RGBA float32 (gridW*gridH*4). Honors the species' own init
// (seed blit centered / low-frequency value-noise soup, ported from the site).
std::vector<float> BuildInitGrid(const Species& s, int gridW, int gridH, std::mt19937& rng);
// Random soup regardless of the species init (site's "restart with random soup").
std::vector<float> BuildRandomSoup(const Species& s, int gridW, int gridH, std::mt19937& rng);

class Catalog {
public:
    bool Load(const std::filesystem::path& catalogDir, std::string* err);
    size_t Count() const { return entries_.size(); }
    // Index display name for a catalog-relative path; empty if unknown.
    std::string NameForPath(const std::string& relPath) const;
    // Load a specific catalog-relative path (no R filter). Sets catalogPath.
    bool LoadPath(const std::string& relPath, Species* out, std::string* err = nullptr);
    // Random species with R <= maxR; skips unparsable entries. False if none found.
    bool PickRandom(std::mt19937& rng, double maxR, Species* out);
    // Random pick from an explicit path list with R <= maxR.
    bool PickRandomFrom(const std::vector<std::string>& paths, std::mt19937& rng, double maxR,
                        Species* out);

private:
    struct Entry { std::string path, name; };
    std::filesystem::path dir_;
    std::vector<Entry> entries_;
};
