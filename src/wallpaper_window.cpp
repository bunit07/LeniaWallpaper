#include "wallpaper_window.h"

#include "util.h"

namespace {

const wchar_t* kClassName = L"LeniaWallpaperView";
bool g_classRegistered = false;

// Finds the WorkerW window that sits behind the desktop icons. Sending 0x052C
// to Progman makes the shell spawn it if it does not exist yet.
HWND FindWallpaperHost() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, nullptr);

    // Classic layout: a top-level WorkerW sibling right after the one that
    // contains SHELLDLL_DefView.
    HWND workerw = nullptr;
    EnumWindows(
        [](HWND top, LPARAM out) -> BOOL {
            if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
                HWND next = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
                if (next) {
                    *(HWND*)out = next;
                    return FALSE;
                }
            }
            return TRUE;
        },
        (LPARAM)&workerw);
    if (workerw) return workerw;

    // Windows 11 24H2 layout: WorkerW is a child of Progman, behind the
    // SHELLDLL_DefView child.
    HWND child = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
    if (child) return child;

    // Last resort: parent to Progman itself (still renders behind icons on
    // builds where Progman hosts the wallpaper directly).
    return progman;
}

LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;  // D3D owns all the pixels
    case WM_PAINT: {
        ValidateRect(hwnd, nullptr);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi{sizeof(mi)};
            if (!GetMonitorInfoW(mon, &mi)) return TRUE;
            auto* v = (std::vector<MonitorInfo>*)lp;
            v->push_back({mon, mi.rcMonitor});
            return TRUE;
        },
        (LPARAM)&out);
    return out;
}

bool WallpaperWindow::EnsureClassRegistered(HINSTANCE inst) {
    if (g_classRegistered) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = ViewProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    wc.hbrBackground = nullptr;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    g_classRegistered = true;
    return true;
}

bool WallpaperWindow::Create(HINSTANCE inst, const MonitorInfo& mon) {
    if (!EnsureClassRegistered(inst)) return false;

    monitor_ = mon.handle;
    x_ = mon.rc.left;
    y_ = mon.rc.top;
    width_ = mon.Width();
    height_ = mon.Height();
    hwnd_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName, L"Lenia",
                            WS_POPUP, 0, 0, width_, height_, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    if (!AttachToDesktop()) {
        LogLine("could not find WorkerW; wallpaper window not attached");
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    return true;
}

bool WallpaperWindow::AttachToDesktop() {
    HWND host = FindWallpaperHost();
    if (!host) return false;

    // Becoming a child strips the popup styling and clips us to the host.
    SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_CHILD | WS_VISIBLE);
    SetParent(hwnd_, host);

    // Monitor rect in the host's coordinate space (the wallpaper host spans the
    // whole virtual desktop; virtual-screen origin maps to its client 0,0).
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    SetWindowPos(hwnd_, HWND_TOP, x_ - vx, y_ - vy, width_, height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

bool WallpaperWindow::HandleDisplayChange(const MonitorInfo& mon) {
    bool changed = (mon.Width() != width_ || mon.Height() != height_ || mon.rc.left != x_ ||
                    mon.rc.top != y_);
    monitor_ = mon.handle;
    x_ = mon.rc.left;
    y_ = mon.rc.top;
    width_ = mon.Width();
    height_ = mon.Height();
    AttachToDesktop();  // WorkerW can be recreated on display changes
    return changed;
}

void WallpaperWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    monitor_ = nullptr;
}
