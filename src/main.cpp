// Lenia live wallpaper: runs a 3-channel multi-kernel Lenia simulation behind
// the desktop icons on every monitor. See README.md.
#include <windows.h>
#include <wtsapi32.h>
#include <objbase.h>

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "config.h"
#include "renderer.h"
#include "scheduler.h"
#include "species.h"
#include "tray.h"
#include "util.h"
#include "wallpaper_window.h"

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

constexpr wchar_t kCtrlClass[] = L"LeniaWallpaperCtrl";

// One independent sim + wallpaper window per physical monitor.
struct MonitorSurface {
    WallpaperWindow window;
    Renderer renderer;
    bool isPaused = false;
    ULONGLONG pausedAt = 0;
    bool pauseWasForeground = false;  // occluded or fullscreen -> rotate on resume
    unsigned secondsSinceProbe = 0;
    int consecutiveDeadChecks = 0;
    ULONGLONG rotateDeadlineMs = 0;
};

class App {
public:
    bool Init(HINSTANCE inst);
    int Run();
    void Shutdown();

    LRESULT CtrlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    bool CreateSurfaces();
    void DestroySurfaces();
    void Tick();
    void PollAndEvaluatePause();
    void CheckLifecycle(MonitorSurface& s);
    bool LoadSpeciesOnto(MonitorSurface& s);
    bool LoadPathOnto(MonitorSurface& s, const std::string& relPath);
    void NextSpeciesAll();
    void NextSpecies(MonitorSurface& s);
    void LoadFavoriteAll(const std::string& relPath);
    void UpdateTraySpecies();
    void SaveConfig();
    void ToggleDiscovery();
    void ToggleRandomSoup();
    void AddFavorite(const std::string& path);
    void RemoveFavorite(const std::string& path);
    void OnDisplayChange();
    void SetTimerMode(bool active);

    HINSTANCE inst_ = nullptr;
    HWND ctrlHwnd_ = nullptr;
    HPOWERNOTIFY powerNotify_ = nullptr;
    std::filesystem::path catalogPath_;
    std::filesystem::path configPath_;
    std::wstring savedWallpaper_;
    Config config_;
    Catalog catalog_;
    std::vector<std::unique_ptr<MonitorSurface>> surfaces_;
    PauseController pause_;
    FrameTimer timer_;
    TrayIcon tray_;
    std::mt19937 rng_{std::random_device{}()};

    bool timerActive_ = true;
    unsigned tickCount_ = 0;
    unsigned ticksPerSecond_ = 24;
    unsigned pacedFrames_ = 0;
    ULONGLONG paceWindowStartMs_ = 0;
    std::string lastStatus_;
};

App* g_app = nullptr;

LRESULT CALLBACK CtrlProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) return g_app->CtrlWndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool App::CreateSurfaces() {
    DestroySurfaces();
    auto monitors = EnumerateMonitors();
    if (monitors.empty()) {
        LogLine("no monitors found");
        return false;
    }

    for (const auto& mon : monitors) {
        auto s = std::make_unique<MonitorSurface>();
        if (!s->window.Create(inst_, mon)) {
            LogLine("wallpaper window failed for monitor %dx%d", mon.Width(), mon.Height());
            continue;
        }
        std::string err;
        if (!s->renderer.Init(s->window.Hwnd(), s->window.Width(), s->window.Height(),
                              config_.cellScale, &err)) {
            LogLine("renderer init failed (%dx%d): %s", mon.Width(), mon.Height(), err.c_str());
            s->window.Destroy();
            continue;
        }
        LogLine("monitor %dx%d at (%d,%d)", mon.Width(), mon.Height(), mon.rc.left, mon.rc.top);
        surfaces_.push_back(std::move(s));
    }
    return !surfaces_.empty();
}

void App::DestroySurfaces() {
    for (auto& s : surfaces_) {
        s->renderer.CancelProbe();
        s->window.Destroy();
    }
    surfaces_.clear();
}

bool App::Init(HINSTANCE inst) {
    inst_ = inst;
    configPath_ = ExeDir() / "config.json";
    Config::WriteDefaultIfMissing(configPath_);
    config_.Load(configPath_);
    ticksPerSecond_ = (unsigned)config_.fps;
    pause_.SetPauseOnBattery(config_.pauseOnBattery);
    pause_.SetOccludeCoverage(config_.occludeCoverage);
    pause_.SetDisableAuto(config_.disableAutoPause);

    // Remember the user's static wallpaper so Exit can restore it cleanly.
    wchar_t wallpaperBuf[MAX_PATH]{};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wallpaperBuf, 0) && wallpaperBuf[0])
        savedWallpaper_ = wallpaperBuf;

    std::string err;
    std::filesystem::path catalogPath = ExeDir() / Utf8ToWide(config_.catalogDir);
    if (!std::filesystem::exists(catalogPath / "index.json")) {
        // Dev convenience: exe lives in build\, catalog next to the source tree.
        catalogPath = ExeDir().parent_path() / Utf8ToWide(config_.catalogDir);
    }
    catalogPath_ = catalogPath;
    if (!catalog_.Load(catalogPath, &err)) {
        MessageBoxW(nullptr,
                    (L"Could not load the species catalog:\n" + Utf8ToWide(err) +
                     L"\n\nRun tools/fetch_catalog.py first.")
                        .c_str(),
                    L"Lenia Wallpaper", MB_ICONERROR);
        return false;
    }

    // Hidden top-level window: receives tray callbacks, session/power/display
    // broadcasts. (Message-only windows do not receive broadcasts.)
    WNDCLASSW wc{};
    wc.lpfnWndProc = CtrlProcThunk;
    wc.hInstance = inst;
    wc.lpszClassName = kCtrlClass;
    RegisterClassW(&wc);
    ctrlHwnd_ = CreateWindowExW(0, kCtrlClass, L"Lenia Control", WS_OVERLAPPED, 0, 0, 0, 0,
                                nullptr, nullptr, inst, nullptr);
    if (!ctrlHwnd_) return false;
    WTSRegisterSessionNotification(ctrlHwnd_, NOTIFY_FOR_THIS_SESSION);
    powerNotify_ = RegisterPowerSettingNotification(ctrlHwnd_, &GUID_CONSOLE_DISPLAY_STATE,
                                                    DEVICE_NOTIFY_WINDOW_HANDLE);

    if (!CreateSurfaces()) {
        MessageBoxW(nullptr, L"Could not attach to the desktop (WorkerW not found).",
                    L"Lenia Wallpaper", MB_ICONERROR);
        return false;
    }

    TrayIcon::Callbacks cb;
    cb.onNextSpecies = [this] { NextSpeciesAll(); };
    cb.onReseed = [this] {
        for (auto& s : surfaces_) {
            s->renderer.ReseedCurrent(config_.randomSoup, rng_);
            s->renderer.StepAndPresent();
            s->consecutiveDeadChecks = 0;
            s->rotateDeadlineMs = 0;
        }
    };
    cb.onToggleRandomSoup = [this] { ToggleRandomSoup(); };
    cb.onTogglePause = [this] {
        pause_.SetUserPaused(!pause_.UserPaused());
        tray_.SetPausedByUser(pause_.UserPaused());
        PollAndEvaluatePause();
    };
    cb.onToggleStartup = [] { TrayIcon::SetStartupEnabled(!TrayIcon::IsStartupEnabled()); };
    cb.onToggleDiscovery = [this] { ToggleDiscovery(); };
    cb.onAddFavorite = [this](const std::string& path) { AddFavorite(path); };
    cb.onRemoveFavorite = [this](const std::string& path) { RemoveFavorite(path); };
    cb.onSelectFavorite = [this](const std::string& path) { LoadFavoriteAll(path); };
    cb.onExit = [this] { PostQuitMessage(0); };
    tray_.Create(ctrlHwnd_, inst, std::move(cb));

    NextSpeciesAll();
    if (!timer_.Start(1000 / (int)ticksPerSecond_)) {
        MessageBoxW(nullptr, L"Could not start the frame timer.", L"Lenia Wallpaper", MB_ICONERROR);
        return false;
    }
    timerActive_ = true;
    return true;
}

void App::SetTimerMode(bool active) {
    if (active == timerActive_) return;
    timerActive_ = active;
    // All paused: tick once a second just to re-poll the pause signals.
    timer_.Start(active ? 1000 / (int)ticksPerSecond_ : 1000);
}

bool App::LoadPathOnto(MonitorSurface& s, const std::string& relPath) {
    Species sp;
    std::string err;
    if (!catalog_.LoadPath(relPath, &sp, &err)) {
        LogLine("species %s: %s", relPath.c_str(), err.c_str());
        return false;
    }
    if (!s.renderer.LoadSpecies(sp, rng_, config_.randomSoup)) {
        LogLine("LoadSpecies failed for %s", sp.name.c_str());
        return false;
    }
    s.renderer.StepAndPresent();
    s.consecutiveDeadChecks = 0;
    s.rotateDeadlineMs = 0;
    s.secondsSinceProbe = 0;
    return true;
}

bool App::LoadSpeciesOnto(MonitorSurface& s) {
    Species sp;
    bool picked = false;
    if (!config_.species.empty()) {  // pinned species from config.json (all monitors)
        std::string err;
        picked = catalog_.LoadPath(config_.species, &sp, &err);
        if (!picked) LogLine("pinned species %s: %s", config_.species.c_str(), err.c_str());
    }
    if (!picked) {
        bool preferFavorites = false;
        if (!config_.favorites.empty()) {
            if (!config_.discoveryEnabled) {
                preferFavorites = true;
            } else {
                std::uniform_real_distribution<double> roll(0.0, 1.0);
                const double r = roll(rng_);
                preferFavorites = r < config_.favoriteChance;
                LogLine("pick roll=%.3f chance=%.3f -> %s", r, config_.favoriteChance,
                        preferFavorites ? "favorites" : "catalog");
            }
        }
        if (preferFavorites) {
            picked = catalog_.PickRandomFrom(config_.favorites, rng_, config_.maxKernelRadius, &sp);
            if (!picked && !config_.discoveryEnabled)
                LogLine("discovery off but no loadable favorites; falling back to catalog");
        }
        if (!picked) picked = catalog_.PickRandom(rng_, config_.maxKernelRadius, &sp);
    }
    if (!picked) {
        LogLine("no loadable species found");
        return false;
    }
    if (!s.renderer.LoadSpecies(sp, rng_, config_.randomSoup)) {
        LogLine("LoadSpecies failed for %s", sp.name.c_str());
        return false;
    }
    s.renderer.StepAndPresent();  // show even if this monitor is paused
    s.consecutiveDeadChecks = 0;
    s.rotateDeadlineMs = 0;
    s.secondsSinceProbe = 0;
    return true;
}

void App::UpdateTraySpecies() {
    std::vector<std::string> names;
    names.reserve(surfaces_.size());
    std::vector<TrayIcon::SpeciesEntry> currents;
    for (const auto& s : surfaces_) {
        if (!s->renderer.HasSpecies()) continue;
        const Species& cur = s->renderer.Current();
        names.push_back(cur.name);
        if (cur.catalogPath.empty()) continue;
        bool seen = false;
        for (const auto& e : currents) {
            if (e.path == cur.catalogPath) {
                seen = true;
                break;
            }
        }
        if (!seen) currents.push_back({cur.catalogPath, cur.name});
    }

    std::vector<TrayIcon::SpeciesEntry> favs;
    favs.reserve(config_.favorites.size());
    for (const std::string& path : config_.favorites) {
        // Prefer a live display name if this favorite is currently on screen.
        std::string label;
        for (const auto& c : currents) {
            if (c.path == path) {
                label = c.name;
                break;
            }
        }
        if (label.empty()) {
            label = catalog_.NameForPath(path);
            Species tmp;
            tmp.name = label;
            AssignDisplayName(tmp);
            label = tmp.name;
        }
        favs.push_back({path, label});
    }

    tray_.SetSpeciesNames(names);
    tray_.SetFavoritesMenu(config_.discoveryEnabled, config_.randomSoup, favs, currents);
}

void App::SaveConfig() { config_.Save(configPath_); }

void App::ToggleDiscovery() {
    config_.discoveryEnabled = !config_.discoveryEnabled;
    SaveConfig();
    UpdateTraySpecies();
    LogLine("discovery %s", config_.discoveryEnabled ? "on" : "off");
}

void App::ToggleRandomSoup() {
    config_.randomSoup = !config_.randomSoup;
    SaveConfig();
    UpdateTraySpecies();
    LogLine("random soup %s", config_.randomSoup ? "on" : "off");
}

void App::AddFavorite(const std::string& path) {
    if (path.empty()) return;
    if (std::find(config_.favorites.begin(), config_.favorites.end(), path) !=
        config_.favorites.end())
        return;
    config_.favorites.push_back(path);
    SaveConfig();
    UpdateTraySpecies();
    LogLine("favorited %s", path.c_str());
}

void App::RemoveFavorite(const std::string& path) {
    auto it = std::find(config_.favorites.begin(), config_.favorites.end(), path);
    if (it == config_.favorites.end()) return;
    config_.favorites.erase(it);
    SaveConfig();
    UpdateTraySpecies();
    LogLine("unfavorited %s", path.c_str());
}

void App::LoadFavoriteAll(const std::string& relPath) {
    for (auto& s : surfaces_) LoadPathOnto(*s, relPath);
    UpdateTraySpecies();
}

void App::NextSpecies(MonitorSurface& s) {
    if (LoadSpeciesOnto(s)) UpdateTraySpecies();
}

void App::NextSpeciesAll() {
    for (auto& s : surfaces_) LoadSpeciesOnto(*s);
    UpdateTraySpecies();
}

void App::PollAndEvaluatePause() {
    pause_.PollSlowSignals();

    bool anyRunning = false;
    bool anyOccluded = false;
    bool anyJustResumedForeground = false;

    for (auto& s : surfaces_) {
        HMONITOR mon = s->window.Monitor();
        bool occluded = pause_.AutoPausedByOcclusion(mon);
        if (occluded) anyOccluded = true;
        bool shouldPause = pause_.ShouldPauseMonitor(mon);

        if (shouldPause && !s->isPaused) {
            s->isPaused = true;
            s->pausedAt = GetTickCount64();
            s->pauseWasForeground = false;
            s->renderer.CancelProbe();
        }
        if (s->isPaused && shouldPause && !pause_.UserPaused() &&
            (pause_.AutoPausedByForeground() || occluded))
            s->pauseWasForeground = true;

        if (!shouldPause && s->isPaused) {
            ULONGLONG pausedForMs = GetTickCount64() - s->pausedAt;
            s->isPaused = false;
            // User came back to this desktop after being away: fresh species.
            if (s->pauseWasForeground &&
                pausedForMs >= (ULONGLONG)config_.rotateOnResumeSeconds * 1000) {
                NextSpecies(*s);
                anyJustResumedForeground = true;
            }
            s->pauseWasForeground = false;
        }

        if (!s->isPaused) anyRunning = true;
    }

    SetTimerMode(anyRunning || anyJustResumedForeground);

    std::string status = pause_.Describe(anyRunning, anyOccluded);
    if (status != lastStatus_) {
        lastStatus_ = status;
        tray_.SetStatusText(status);
        LogLine("%s", status.c_str());
    }
}

void App::CheckLifecycle(MonitorSurface& s) {
    if (s.rotateDeadlineMs) {
        if (GetTickCount64() >= s.rotateDeadlineMs) NextSpecies(s);
        return;
    }
    if (++s.secondsSinceProbe < (unsigned)config_.deathCheckSeconds) return;
    s.secondsSinceProbe = 0;

    ProbeResult p;
    if (s.renderer.FetchProbe(&p)) {
        // Slow species (large T) legitimately change little between probes.
        double t = s.renderer.HasSpecies() ? s.renderer.Current().T : 2.0;
        float frozenThreshold = (float)(1e-5 / std::max(1.0, t / 10.0));
        bool dead = p.maxValue < 0.05f;                        // grid faded to nothing
        bool saturated = p.mean > 0.90f;                       // exploded to solid color
        bool frozen = p.meanDelta < frozenThreshold && p.maxValue >= 0.05f;  // alive but static
        if (dead || saturated || frozen)
            s.consecutiveDeadChecks++;
        else
            s.consecutiveDeadChecks = 0;
        if (s.consecutiveDeadChecks >= 3) {
            LogLine("grid %s (mean=%.4f max=%.3f delta=%.6f); rotating soon",
                    dead ? "dead" : saturated ? "saturated" : "frozen", p.mean, p.maxValue,
                    p.meanDelta);
            s.rotateDeadlineMs = GetTickCount64() + (ULONGLONG)config_.deathGraceSeconds * 1000;
            return;
        }
    }
    s.renderer.StartProbe();  // mapped on the next check, one interval from now
}

void App::Tick() {
    tickCount_++;
    if (!timerActive_) {
        // 1 Hz idle tick: only re-poll whether we can resume.
        PollAndEvaluatePause();
        return;
    }
    if (tickCount_ % ticksPerSecond_ == 0) PollAndEvaluatePause();

    unsigned presented = 0;
    int tapSum = 0;
    for (auto& s : surfaces_) {
        if (s->isPaused) continue;
        s->renderer.StepAndPresent();
        presented++;
        tapSum += s->renderer.TapCount();
        if (tickCount_ % ticksPerSecond_ == 0) CheckLifecycle(*s);
    }

    // Once a second: warn only if pacing drifts well off the target rate.
    pacedFrames_++;
    ULONGLONG now = GetTickCount64();
    if (paceWindowStartMs_ == 0) paceWindowStartMs_ = now;
    else if (now - paceWindowStartMs_ >= 1000) {
        double fps = pacedFrames_ * 1000.0 / (double)(now - paceWindowStartMs_);
        if (fps < ticksPerSecond_ * 0.75 || fps > ticksPerSecond_ * 1.25)
            LogLine("pacing: %.1f ticks/s (target %u), %u monitors presenting, taps=%d", fps,
                    ticksPerSecond_, presented, tapSum);
        pacedFrames_ = 0;
        paceWindowStartMs_ = now;
    }
}

void App::OnDisplayChange() {
    // Topology may have changed; rebuild every surface and pick fresh species.
    if (!CreateSurfaces()) {
        LogLine("display change: failed to recreate wallpaper surfaces");
        return;
    }
    NextSpeciesAll();
    PollAndEvaluatePause();
}

LRESULT App::CtrlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case TrayIcon::kTrayMessage:
        tray_.HandleMessage(wp, lp);
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) pause_.SetSessionLocked(true);
        else if (wp == WTS_SESSION_UNLOCK) pause_.SetSessionLocked(false);
        PollAndEvaluatePause();
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            auto* s = (POWERBROADCAST_SETTING*)lp;
            if (s && IsEqualGUID(s->PowerSetting, GUID_CONSOLE_DISPLAY_STATE))
                pause_.SetDisplayOff(s->Data[0] == 0);
        }
        PollAndEvaluatePause();
        return TRUE;
    case WM_DISPLAYCHANGE:
        OnDisplayChange();
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int App::Run() {
    for (;;) {
        HANDLE h = timer_.Handle();
        DWORD r = MsgWaitForMultipleObjectsEx(1, &h, INFINITE, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) {
            Tick();
        } else {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return (int)msg.wParam;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

void App::Shutdown() {
    timer_.Stop();
    tray_.Destroy();
    if (powerNotify_) {
        UnregisterPowerSettingNotification(powerNotify_);
        powerNotify_ = nullptr;
    }
    if (ctrlHwnd_) WTSUnRegisterSessionNotification(ctrlHwnd_);
    DestroySurfaces();
    // Restore the user's previous wallpaper (or nudge the shell to redraw).
    if (!savedWallpaper_.empty()) {
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void*)savedWallpaper_.c_str(),
                              SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    } else {
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, nullptr, SPIF_SENDCHANGE);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"LeniaWallpaperSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        MessageBoxW(nullptr, L"Lenia Wallpaper is already running.\nCheck the system tray.",
                    L"Lenia Wallpaper", MB_ICONINFORMATION);
        CoUninitialize();
        return 0;
    }

    App app;
    g_app = &app;
    int rc = 1;
    if (app.Init(inst)) rc = app.Run();
    app.Shutdown();
    g_app = nullptr;
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return rc;
}
