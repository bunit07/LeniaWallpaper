#pragma once
#include <windows.h>

#include <string>

// Aggregates every "should the sim be running?" signal. Event-driven signals
// (session lock, display off) are fed in from window messages; polled signals
// (battery, fullscreen) are refreshed by PollSlowSignals() about once a second.
// Desktop occlusion is checked per monitor via IsMonitorOccluded().
class PauseController {
public:
    void SetPauseOnBattery(bool v) { pauseOnBattery_ = v; }
    void SetOccludeCoverage(double v) { occludeCoverage_ = v; }
    void SetDisableAuto(bool v) { disableAuto_ = v; }
    bool DisableAuto() const { return disableAuto_; }
    double OccludeCoverage() const { return occludeCoverage_; }

    void SetSessionLocked(bool v) { sessionLocked_ = v; }
    void SetDisplayOff(bool v) { displayOff_ = v; }
    void SetUserPaused(bool v) { userPaused_ = v; }
    bool UserPaused() const { return userPaused_; }

    void PollSlowSignals();

    // Global reasons that pause every monitor.
    bool ShouldPauseGlobal() const {
        if (disableAuto_) return userPaused_;
        return userPaused_ || sessionLocked_ || displayOff_ || onBattery_ || fullscreenApp_;
    }
    // True when any visible top-level window covers most of this monitor
    // (not just the focused one — secondary-monitor apps stay occluding).
    bool IsMonitorOccluded(HMONITOR mon) const;

    // Combined pause for one monitor (global + that monitor's occlusion).
    bool ShouldPauseMonitor(HMONITOR mon) const {
        if (ShouldPauseGlobal()) return true;
        if (disableAuto_) return false;
        return IsMonitorOccluded(mon);
    }

    // True for pause reasons where "user came back to the desktop" applies,
    // i.e. the ones that should rotate the species on resume.
    bool AutoPausedByForeground() const { return fullscreenApp_; }
    bool AutoPausedByOcclusion(HMONITOR mon) const {
        return !disableAuto_ && IsMonitorOccluded(mon);
    }

    std::string Describe(bool anyRunning, bool anyOccluded) const;

private:
    bool pauseOnBattery_ = true;
    double occludeCoverage_ = 0.9;
    bool disableAuto_ = false;

    bool userPaused_ = false;
    bool sessionLocked_ = false;
    bool displayOff_ = false;
    bool onBattery_ = false;
    bool fullscreenApp_ = false;
};

// Waitable-timer pacing with coalescing (~8 ms tolerance) so the CPU can reach
// deep sleep states between ticks. periodMs is the wake interval in milliseconds
// (e.g. 1000/fps while rendering, 1000 while fully paused).
class FrameTimer {
public:
    bool Start(int periodMs);
    void Stop();
    HANDLE Handle() const { return timer_; }

private:
    HANDLE timer_ = nullptr;
};
