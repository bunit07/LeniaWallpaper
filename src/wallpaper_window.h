#pragma once
#include <windows.h>

#include <vector>

// One physical display: handle + virtual-desktop rectangle.
struct MonitorInfo {
    HMONITOR handle = nullptr;
    RECT rc{};
    int Width() const { return rc.right - rc.left; }
    int Height() const { return rc.bottom - rc.top; }
};

std::vector<MonitorInfo> EnumerateMonitors();

// Creates a window parented into the desktop's WorkerW (behind the icons) that
// covers one monitor. Standard "wallpaper window" technique used by
// Wallpaper Engine / Lively. Create one instance per monitor for multi-display.
class WallpaperWindow {
public:
    bool Create(HINSTANCE inst, const MonitorInfo& mon);
    // Re-locate WorkerW and move/resize after WM_DISPLAYCHANGE. Returns true if
    // the monitor size changed (renderer must resize).
    bool HandleDisplayChange(const MonitorInfo& mon);
    void Destroy();

    HWND Hwnd() const { return hwnd_; }
    HMONITOR Monitor() const { return monitor_; }
    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    bool AttachToDesktop();
    static bool EnsureClassRegistered(HINSTANCE inst);

    HWND hwnd_ = nullptr;
    HMONITOR monitor_ = nullptr;
    int x_ = 0, y_ = 0;  // virtual-desktop origin of this monitor
    int width_ = 0, height_ = 0;
};
