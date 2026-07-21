#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// System tray icon with the control menu. Owner must forward the callback
// message (kTrayMessage) from its window proc to HandleMessage.
class TrayIcon {
public:
    static constexpr UINT kTrayMessage = WM_APP + 1;

    struct SpeciesEntry {
        std::string path;  // catalog-relative; empty if unknown
        std::string name;  // display label
    };

    struct Callbacks {
        std::function<void()> onNextSpecies;
        std::function<void()> onReseed;
        std::function<void()> onToggleRandomSoup;
        std::function<void()> onTogglePause;
        std::function<void()> onToggleStartup;
        std::function<void()> onToggleDiscovery;
        std::function<void(const std::string& path)> onAddFavorite;
        std::function<void(const std::string& path)> onRemoveFavorite;
        std::function<void(const std::string& path)> onSelectFavorite;
        std::function<void()> onExit;
    };

    bool Create(HWND ownerHwnd, HINSTANCE inst, Callbacks cb);
    void Destroy();
    void HandleMessage(WPARAM wp, LPARAM lp);

    void SetSpeciesNames(const std::vector<std::string>& names);
    void SetFavoritesMenu(bool discoveryEnabled, bool randomSoup,
                          const std::vector<SpeciesEntry>& favorites,
                          const std::vector<SpeciesEntry>& currents);
    void SetStatusText(const std::string& status);  // shown in the tooltip
    void SetPausedByUser(bool paused) { userPaused_ = paused; }

    static bool IsStartupEnabled();
    static void SetStartupEnabled(bool enable);

private:
    void ShowMenu();
    void UpdateTip();

    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    Callbacks cb_;
    std::wstring speciesName_, status_;
    bool userPaused_ = false;
    bool discoveryEnabled_ = true;
    bool randomSoup_ = false;
    std::vector<SpeciesEntry> favorites_;
    std::vector<SpeciesEntry> currents_;
    bool added_ = false;
};
