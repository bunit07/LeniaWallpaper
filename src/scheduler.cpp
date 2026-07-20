#include "scheduler.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace {

bool IsOnBattery() {
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s)) return false;
    return s.ACLineStatus == 0;
}

bool IsFullscreenAppActive() {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN ||
           state == QUNS_PRESENTATION_MODE;
}

bool IsShellOrWallpaperClass(const wchar_t* cls) {
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
           !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd") ||
           !wcscmp(cls, L"LeniaWallpaperView") || !wcscmp(cls, L"LeniaWallpaperCtrl");
}

struct OccludeEnumCtx {
    RECT mon{};
    double coverage = 0.9;
    bool hit = false;
};

BOOL CALLBACK EnumOccludingWindow(HWND hwnd, LPARAM lp) {
    auto* ctx = (OccludeEnumCtx*)lp;
    if (ctx->hit) return FALSE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (IsShellOrWallpaperClass(cls)) return TRUE;

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked)
        return TRUE;

    RECT wr{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr))))
        GetWindowRect(hwnd, &wr);

    RECT inter{};
    if (!IntersectRect(&inter, &wr, &ctx->mon)) return TRUE;

    double interArea = (double)(inter.right - inter.left) * (inter.bottom - inter.top);
    double monArea =
        (double)(ctx->mon.right - ctx->mon.left) * (ctx->mon.bottom - ctx->mon.top);
    if (monArea > 0 && interArea / monArea >= ctx->coverage) ctx->hit = true;
    return TRUE;
}

}  // namespace

bool PauseController::IsMonitorOccluded(HMONITOR mon) const {
    MONITORINFO mi{sizeof(mi)};
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;

    OccludeEnumCtx ctx{mi.rcMonitor, occludeCoverage_, false};
    EnumWindows(EnumOccludingWindow, (LPARAM)&ctx);
    return ctx.hit;
}

void PauseController::PollSlowSignals() {
    onBattery_ = pauseOnBattery_ && IsOnBattery();
    fullscreenApp_ = IsFullscreenAppActive();
}

std::string PauseController::Describe(bool anyRunning, bool anyOccluded) const {
    if (userPaused_) return "paused (manual)";
    if (sessionLocked_) return "paused (locked)";
    if (displayOff_) return "paused (display off)";
    if (onBattery_) return "paused (on battery)";
    if (fullscreenApp_) return "paused (fullscreen app)";
    if (!anyRunning && anyOccluded) return "paused (desktop hidden)";
    if (anyRunning && anyOccluded) return "running (partial)";
    if (anyRunning) return "running";
    return "paused";
}

bool FrameTimer::Start(int periodMs) {
    Stop();
    // Auto-reset (synchronization) timer: signaled once per period.
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (!timer_) return false;
    if (periodMs < 1) periodMs = 1;
    LARGE_INTEGER due{};
    due.QuadPart = -(LONGLONG)periodMs * 10000;
    // 8 ms tolerable delay lets Windows coalesce our wakeups with others.
    return SetWaitableTimerEx(timer_, &due, (LONG)periodMs, nullptr, nullptr, nullptr, 8) != 0;
}

void FrameTimer::Stop() {
    if (timer_) {
        CancelWaitableTimer(timer_);
        CloseHandle(timer_);
        timer_ = nullptr;
    }
}
