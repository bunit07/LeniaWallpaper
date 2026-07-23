#include "species.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <string>

#include "json.h"
#include "util.h"

namespace {

// Global kernel ring parameters (site defaults; we have no live sliders).
constexpr double kRingCenter = 0.5;
constexpr double kRingWidth = 0.15;

double Bell(double x, double m, double s) {
    double d = (x - m) / s;
    return std::exp(-0.5 * d * d);
}

bool ReadLaneArray(const json::Value& root, const char* key, double* out, int n, bool defOne) {
    const json::Value* v = root.find(key);
    for (int i = 0; i < kLanes; i++) out[i] = defOne ? 1.0 : 0.0;
    if (!v || v->type != json::Value::Array) return false;
    for (int i = 0; i < n && i < (int)v->arr.size() && i < kLanes; i++)
        out[i] = v->arr[i].numOr(out[i]);
    return true;
}

// Port of the site's per-lane getWeight(r) (lenia.html SIM_FS), evaluated once
// per offset on the CPU instead of per pixel per frame on the GPU.
void LaneWeights(const Species& s, double r, double* w) {
    for (int i = 0; i < kLanes; i++) w[i] = 0.0;
    if (r > s.R) return;
    for (int i = 0; i < s.numKernels; i++) {
        double relR = s.relR[i] != 0.0 ? s.relR[i] : 1.0;
        double Br = s.betaLen[i] * (r / s.R) / relR;
        int ring = (int)std::floor(Br);
        double height = ring == 0 ? s.beta0[i] : ring == 1 ? s.beta1[i] : ring == 2 ? s.beta2[i] : 0.0;
        w[i] = height * Bell(Br - ring, kRingCenter, kRingWidth);
    }
}

// Port of the site's valueNoise(): low-frequency lattice noise in [-1,1].
class ValueNoise {
public:
    ValueNoise(int span, double scale, std::mt19937& rng) : scale_(scale) {
        cells_ = std::max(2, (int)std::ceil(span / scale) + 1);
        grid_.resize((size_t)cells_ * cells_);
        std::uniform_real_distribution<double> d(-1.0, 1.0);
        for (auto& g : grid_) g = d(rng);
    }
    double At(int x, int y) const {
        double gx = x / scale_, gy = y / scale_;
        int x0 = (int)std::floor(gx), y0 = (int)std::floor(gy);
        double fx = Smooth(gx - x0), fy = Smooth(gy - y0);
        double a = G(x0, y0), b = G(x0 + 1, y0), c = G(x0, y0 + 1), d = G(x0 + 1, y0 + 1);
        return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
    }

private:
    static double Smooth(double t) { return t * t * (3.0 - 2.0 * t); }
    double G(int ix, int iy) const {
        return grid_[(size_t)(iy % cells_) * cells_ + (ix % cells_)];
    }
    int cells_;
    double scale_;
    std::vector<double> grid_;
};

std::vector<float> BuildSoup(int gridW, int gridH, double base, double scale, std::mt19937& rng) {
    std::vector<float> data((size_t)gridW * gridH * 4, 0.0f);
    int span = std::max(gridW, gridH);
    ValueNoise n0(span, scale, rng), n1(span, scale, rng), n2(span, scale, rng);
    const ValueNoise* n[3] = {&n0, &n1, &n2};
    for (int y = 0; y < gridH; y++)
        for (int x = 0; x < gridW; x++) {
            size_t i = ((size_t)y * gridW + x) * 4;
            for (int ch = 0; ch < 3; ch++)
                data[i + ch] = (float)std::clamp(base + n[ch]->At(x, y), 0.0, 1.0);
            data[i + 3] = 1.0f;
        }
    return data;
}

}  // namespace

bool ParseSpecies(const std::filesystem::path& file, Species* out, std::string* err) {
    std::string text;
    if (!ReadFileBytes(file, &text)) {
        if (err) *err = "cannot read file";
        return false;
    }
    json::Value v;
    if (!json::parse(text, &v, err)) return false;
    if (v.type != json::Value::Object) {
        if (err) *err = "root not an object";
        return false;
    }

    out->name = v.fieldStr("name", file.stem().string());
    out->R = v.fieldNum("R", 0.0);
    out->T = v.fieldNum("T", 0.0);
    out->worldSize = std::max(32, (int)v.fieldNum("worldSize", 128.0));
    out->numKernels = (int)v.fieldNum("numKernels", 0.0);
    out->mono = v.fieldBool("mono", false);
    if (out->R <= 0.0 || out->T <= 0.0 || out->numKernels < 1 || out->numKernels > kLanes) {
        if (err) *err = "invalid R/T/numKernels";
        return false;
    }

    int nk = out->numKernels;
    ReadLaneArray(v, "betaLen", out->betaLen, nk, false);
    ReadLaneArray(v, "beta0", out->beta0, nk, false);
    ReadLaneArray(v, "beta1", out->beta1, nk, false);
    ReadLaneArray(v, "beta2", out->beta2, nk, false);
    ReadLaneArray(v, "mu", out->mu, nk, false);
    ReadLaneArray(v, "sigma", out->sigma, nk, true);   // pad 1: avoid div-by-zero in bell
    ReadLaneArray(v, "eta", out->eta, nk, false);      // pad 0: dead lanes add no growth
    ReadLaneArray(v, "relR", out->relR, nk, true);     // pad 1: avoid div-by-zero
    // Explicit zeros in JSON still reach the shader; clamp so /sigma and /relR stay finite.
    for (int i = 0; i < nk; i++) {
        if (out->sigma[i] < 1e-6) out->sigma[i] = 1e-6;
        if (out->relR[i] < 1e-6) out->relR[i] = 1e-6;
    }
    double tmp[kLanes];
    ReadLaneArray(v, "src", tmp, nk, false);
    for (int i = 0; i < kLanes; i++) out->src[i] = (int)tmp[i];
    ReadLaneArray(v, "dst", tmp, nk, false);
    for (int i = 0; i < kLanes; i++) out->dst[i] = (int)tmp[i];

    const json::Value* init = v.find("init");
    if (!init || init->type != json::Value::Object) {
        if (err) *err = "missing init";
        return false;
    }
    std::string type = init->fieldStr("type", "random");
    if (type == "seed") {
        out->init.isSeed = true;
        const json::Value* size = init->find("size");
        const json::Value* cells = init->find("cells");
        if (!size || size->type != json::Value::Array || size->arr.size() < 3 ||
            !cells || cells->type != json::Value::Array) {
            if (err) *err = "bad seed init";
            return false;
        }
        out->init.sh = (int)size->arr[0].numOr(0);
        out->init.sw = (int)size->arr[1].numOr(0);
        out->init.sc = (int)size->arr[2].numOr(0);
        size_t need = (size_t)out->init.sh * out->init.sw * out->init.sc;
        if (out->init.sh <= 0 || out->init.sw <= 0 || out->init.sc <= 0 || out->init.sc > 3 ||
            cells->arr.size() < need) {
            if (err) *err = "seed size/cells mismatch";
            return false;
        }
        out->init.cells.resize(need);
        for (size_t i = 0; i < need; i++)
            out->init.cells[i] = (float)cells->arr[i].numOr(0.0);
    } else {
        out->init.isSeed = false;
        out->init.baseNoise = init->fieldNum("baseNoise", 0.1);
        out->init.randomScale = init->fieldNum("randomScale", 1.0);
    }
    return true;
}

std::vector<Tap> BuildTaps(const Species& s) {
    int ir = (int)std::ceil(s.R);
    double total[kLanes]{};
    struct Raw { int dx, dy; double w[kLanes]; };
    std::vector<Raw> raws;
    raws.reserve((size_t)(3.2 * s.R * s.R) + 8);

    for (int dy = -ir; dy <= ir; dy++)
        for (int dx = -ir; dx <= ir; dx++) {
            double r = std::sqrt((double)dx * dx + (double)dy * dy);
            if (r > s.R) continue;
            Raw raw{dx, dy, {}};
            LaneWeights(s, r, raw.w);
            bool any = false;
            for (int i = 0; i < kLanes; i++) {
                total[i] += raw.w[i];
                if (raw.w[i] != 0.0) any = true;
            }
            if (any) raws.push_back(raw);
        }

    // Normalize by per-lane totals (replaces the shader's sum/total divide) and
    // premultiply the source-channel masks so the shader is pure fetch + MAD.
    std::vector<Tap> taps;
    taps.reserve(raws.size());
    for (const Raw& raw : raws) {
        Tap t{};
        t.dx = raw.dx;
        t.dy = raw.dy;
        for (int i = 0; i < kLanes; i++) {
            float wn = total[i] > 1e-9 ? (float)(raw.w[i] / total[i]) : 0.0f;
            if (wn == 0.0f) continue;
            (s.src[i] == 0 ? t.wR : s.src[i] == 1 ? t.wG : t.wB)[i] = wn;
        }
        taps.push_back(t);
    }
    return taps;
}

SimConstants BuildConstants(const Species& s, int gridW, int gridH, int tapCount) {
    SimConstants c{};
    c.gridW = gridW;
    c.gridH = gridH;
    c.tapCount = tapCount;
    c.invT = (float)(1.0 / s.T);
    for (int i = 0; i < kLanes; i++) {
        c.mu[i] = (float)s.mu[i];
        c.sigma[i] = (float)s.sigma[i];
        c.eta[i] = (float)s.eta[i];
        c.dstR[i] = s.dst[i] == 0 && i < s.numKernels ? 1.0f : 0.0f;
        c.dstG[i] = s.dst[i] == 1 && i < s.numKernels ? 1.0f : 0.0f;
        c.dstB[i] = s.dst[i] == 2 && i < s.numKernels ? 1.0f : 0.0f;
    }
    return c;
}

namespace {

bool IsAllDigits(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool IsAnonymousName(const std::string& name) {
    if (IsAllDigits(name)) return true;
    static constexpr const char kPrefix[] = "Discovered #";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (name.size() > kPrefixLen && name.compare(0, kPrefixLen, kPrefix) == 0)
        return IsAllDigits(name.substr(kPrefixLen));
    return false;
}

std::string GenerateSpeciesName(std::mt19937& rng) {
    // Binomials in the same register as the catalog (Orbium unicaudatus, Scutium serratus…).
    static constexpr const char* kGenus[] = {
        "Luminorbium",  "Nexorbium",    "Aurobium",     "Velorbium",    "Cristorbium",
        "Phasorbium",   "Undulorbium",  "Radiorbium",   "Spirorbium",   "Noctorbium",
        "Prismorbium",  "Flororbium",   "Glacorbium",   "Ignorbium",    "Silvorbium",
        "Gyropteron",   "Vagopteron",   "Nexoptera",    "Luminoptera",  "Auroptera",
        "Cristoptera",  "Radioptera",   "Spiraptera",   "Noctoptera",   "Floraptera",
        "Scutium",      "Discutium",    "Catenium",     "Helicium",     "Pulsium",
        "Tessellium",   "Corallium",    "Nebulium",     "Aetherium",    "Chronium",
        "Synorbium",    "Parorbium",    "Vagorbium",    "Pyroscutium",  "Heliorbium",
    };
    static constexpr const char* kEpithet[] = {
        "unicaudatus",  "bicaudatus",   "undulatus",    "revolvens",    "gyrans",
        "solidus",      "phantasma",    "virtualis",    "dividuus",     "adhaerens",
        "valvatus",     "serratus",     "fluens",       "vagus",        "saliens",
        "labens",       "velox",        "turbulentus",  "cavus",        "arcus",
        "sinus",        "orbis",        "limus",        "ambiguus",     "furiosus",
        "radians",      "lucens",       "tenuis",       "crassus",      "florens",
        "migrans",      "pulsans",      "spiralis",     "fractus",      "aequus",
        "nitens",       "obscurus",     "aureus",       "viridis",      "caeruleus",
        "ignis",        "tractus",      "centrale",     "laterale",     "pedes",
        "mirabilis",    "subtilis",     "grandis",      "minutus",      "elegans",
    };
    std::uniform_int_distribution<size_t> g(0, sizeof(kGenus) / sizeof(kGenus[0]) - 1);
    std::uniform_int_distribution<size_t> e(0, sizeof(kEpithet) / sizeof(kEpithet[0]) - 1);
    size_t e1 = e(rng);
    // ~1 in 4 get a second epithet, matching multi-word catalog forms.
    std::string name = std::string(kGenus[g(rng)]) + " " + kEpithet[e1];
    if (std::uniform_int_distribution<int>(0, 3)(rng) == 0) {
        size_t e2 = e(rng);
        if (e2 != e1) name += std::string(" ") + kEpithet[e2];
    }
    return name;
}

int ResolveSpeciesId(const std::string& name) {
    if (IsAllDigits(name)) return std::stoi(name);
    static constexpr const char kPrefix[] = "Discovered #";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (name.size() > kPrefixLen && name.compare(0, kPrefixLen, kPrefix) == 0 &&
        IsAllDigits(name.substr(kPrefixLen)))
        return std::stoi(name.substr(kPrefixLen));
    // Stable 4-digit-ish id for named catalog species.
    return (int)(std::hash<std::string>{}(name) % 9000u) + 1000;
}

}  // namespace

void AssignDisplayName(Species& s) {
    // Already formatted (e.g. called from PickRandom then LoadSpeciesOnto).
    if (s.name.size() >= 2 && s.name[0] == '#' && s.name.find(" : ") != std::string::npos) return;

    const std::string original = s.name;
    const int id = ResolveSpeciesId(original);
    std::string label;
    if (IsAnonymousName(original)) {
        std::mt19937 rng((uint32_t)std::hash<std::string>{}(original));
        label = GenerateSpeciesName(rng);
    } else {
        label = original;
    }
    s.name = "#" + std::to_string(id) + " : " + label;
}

std::vector<float> BuildInitGrid(const Species& s, int gridW, int gridH, std::mt19937& rng) {
    if (!s.init.isSeed) {
        double scale = s.R * s.init.randomScale;
        return BuildSoup(gridW, gridH, s.init.baseNoise, std::max(1.0, scale), rng);
    }
    std::vector<float> data((size_t)gridW * gridH * 4, 0.0f);
    for (size_t i = 0; i < (size_t)gridW * gridH; i++) data[i * 4 + 3] = 1.0f;

    // Random toroidal origin + dihedral orientation so each load/re-seed differs
    // (plain translation alone is equivalent on a torus).
    std::uniform_int_distribution<int> posX(0, std::max(1, gridW) - 1);
    std::uniform_int_distribution<int> posY(0, std::max(1, gridH) - 1);
    std::uniform_int_distribution<int> orient(0, 7);
    const int ox = posX(rng);
    const int oy = posY(rng);
    const int o = orient(rng);
    const bool flip = o >= 4;
    const int rot = o & 3;  // 0, 90, 180, 270 CW
    const int sw = s.init.sw, sh = s.init.sh;

    auto wrap = [](int v, int n) { return ((v % n) + n) % n; };
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int sx = flip ? (sw - 1 - x) : x;
            int sy = y;
            int tx = sx, ty = sy;
            switch (rot) {
                case 1: tx = sh - 1 - sy; ty = sx; break;          // 90 CW
                case 2: tx = sw - 1 - sx; ty = sh - 1 - sy; break;  // 180
                case 3: tx = sy; ty = sw - 1 - sx; break;          // 270 CW
                default: break;
            }
            int gx = wrap(ox + tx, gridW);
            int gy = wrap(oy + ty, gridH);
            for (int ch = 0; ch < s.init.sc; ch++)
                data[((size_t)gy * gridW + gx) * 4 + ch] =
                    s.init.cells[((size_t)y * sw + x) * s.init.sc + ch];
        }
    }
    return data;
}

std::vector<float> BuildRandomSoup(const Species& s, int gridW, int gridH, std::mt19937& rng) {
    double base = s.init.isSeed ? 0.1 : s.init.baseNoise;
    double scale = s.init.isSeed ? s.R : s.R * s.init.randomScale;
    return BuildSoup(gridW, gridH, base, std::max(1.0, scale), rng);
}

bool Catalog::Load(const std::filesystem::path& catalogDir, std::string* err) {
    dir_ = catalogDir;
    std::string text;
    if (!ReadFileBytes(catalogDir / "index.json", &text)) {
        if (err) *err = "cannot read " + (catalogDir / "index.json").string();
        return false;
    }
    json::Value v;
    if (!json::parse(text, &v, err)) return false;
    const json::Value* list = v.find("species");
    if (!list || list->type != json::Value::Array) {
        if (err) *err = "index.json missing species array";
        return false;
    }
    entries_.clear();
    entries_.reserve(list->arr.size());
    for (const auto& e : list->arr) {
        if (e.type != json::Value::Object) continue;
        Entry entry{e.fieldStr("path", ""), e.fieldStr("name", "")};
        if (!entry.path.empty()) entries_.push_back(std::move(entry));
    }
    if (entries_.empty()) {
        if (err) *err = "catalog index is empty";
        return false;
    }
    return true;
}

std::string Catalog::NameForPath(const std::string& relPath) const {
    for (const Entry& e : entries_) {
        if (e.path == relPath) return e.name.empty() ? relPath : e.name;
    }
    return relPath;
}

bool Catalog::LoadPath(const std::string& relPath, Species* out, std::string* err) {
    Species s;
    if (!ParseSpecies(dir_ / Utf8ToWide(relPath), &s, err)) return false;
    s.catalogPath = relPath;
    if (s.name.empty()) s.name = NameForPath(relPath);
    AssignDisplayName(s);
    *out = std::move(s);
    return true;
}

bool Catalog::PickRandom(std::mt19937& rng, double maxR, Species* out) {
    if (entries_.empty()) return false;
    std::uniform_int_distribution<size_t> pick(0, entries_.size() - 1);
    for (int attempt = 0; attempt < 64; attempt++) {
        const Entry& e = entries_[pick(rng)];
        Species s;
        std::string err;
        if (!ParseSpecies(dir_ / Utf8ToWide(e.path), &s, &err)) {
            LogLine("species %s: %s", e.path.c_str(), err.c_str());
            continue;
        }
        if (s.R > maxR) continue;  // too expensive for the wallpaper budget
        s.catalogPath = e.path;
        if (s.name.empty()) s.name = e.name;
        AssignDisplayName(s);
        *out = std::move(s);
        return true;
    }
    return false;
}

bool Catalog::PickRandomFrom(const std::vector<std::string>& paths, std::mt19937& rng, double maxR,
                            Species* out) {
    if (paths.empty()) return false;
    std::vector<size_t> order(paths.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);
    for (size_t i : order) {
        const std::string& path = paths[i];
        Species s;
        std::string err;
        if (!ParseSpecies(dir_ / Utf8ToWide(path), &s, &err)) {
            LogLine("favorite %s: %s", path.c_str(), err.c_str());
            continue;
        }
        if (s.R > maxR) continue;
        s.catalogPath = path;
        if (s.name.empty()) s.name = NameForPath(path);
        AssignDisplayName(s);
        *out = std::move(s);
        return true;
    }
    return false;
}
