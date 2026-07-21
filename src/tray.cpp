#include "tray.h"

#include <shellapi.h>

#include "resource.h"
#include "util.h"

namespace {

constexpr UINT kIdNext = 101;
constexpr UINT kIdReseed = 102;
constexpr UINT kIdSoup = 103;
constexpr UINT kIdPause = 104;
constexpr UINT kIdStartup = 105;
constexpr UINT kIdExit = 106;
constexpr UINT kIdName = 107;
constexpr UINT kIdDiscovery = 200;
constexpr UINT kIdAddBase = 300;     // + index into currents_
constexpr UINT kIdRemoveBase = 400;  // + index into currents_
constexpr UINT kIdFavBase = 500;     // + index into favorites_
constexpr UINT kMaxMenuItems = 80;

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"LeniaWallpaper";

std::wstring TruncateMenuLabel(std::wstring s, size_t maxChars = 48) {
    if (s.size() > maxChars) s = s.substr(0, maxChars - 3) + L"...";
    return s;
}

}  // namespace

bool TrayIcon::Create(HWND ownerHwnd, HINSTANCE inst, Callbacks cb) {
    owner_ = ownerHwnd;
    cb_ = std::move(cb);

    icon_ = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                              GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                              0);
    if (!icon_) icon_ = LoadIconW(nullptr, IDI_APPLICATION);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = icon_;
    wcscpy_s(nid.szTip, L"Lenia Wallpaper");
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != 0;
    return added_;
}

void TrayIcon::Destroy() {
    if (!added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
    if (icon_) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
}

void TrayIcon::SetSpeciesNames(const std::vector<std::string>& names) {
    speciesName_.clear();
    for (size_t i = 0; i < names.size(); i++) {
        if (i) speciesName_ += L" | ";
        speciesName_ += Utf8ToWide(names[i]);
    }
    UpdateTip();
}

void TrayIcon::SetFavoritesMenu(bool discoveryEnabled, bool randomSoup,
                                const std::vector<SpeciesEntry>& favorites,
                                const std::vector<SpeciesEntry>& currents) {
    discoveryEnabled_ = discoveryEnabled;
    randomSoup_ = randomSoup;
    favorites_ = favorites;
    currents_ = currents;
}

void TrayIcon::SetStatusText(const std::string& status) {
    status_ = Utf8ToWide(status);
    UpdateTip();
}

void TrayIcon::UpdateTip() {
    if (!added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    std::wstring tip = L"Lenia: " + speciesName_;
    if (!status_.empty()) tip += L"\n" + status_;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::HandleMessage(WPARAM /*wp*/, LPARAM lp) {
    if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU ||
        LOWORD(lp) == WM_LBUTTONUP)
        ShowMenu();
}

void TrayIcon::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    // Menu items are truncated visually if huge; tooltip already has the full list.
    std::wstring nameItem = speciesName_.empty() ? L"(no species)" : speciesName_;
    if (nameItem.size() > 60) nameItem = nameItem.substr(0, 57) + L"...";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kIdName, nameItem.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdNext, L"Next species");
    AppendMenuW(menu, MF_STRING, kIdReseed, L"Re-seed");
    AppendMenuW(menu, MF_STRING | (randomSoup_ ? MF_CHECKED : 0), kIdSoup, L"Random soup");
    AppendMenuW(menu, MF_STRING | (discoveryEnabled_ ? MF_CHECKED : 0), kIdDiscovery,
                L"Discovery");

    HMENU favMenu = CreatePopupMenu();
    HMENU addMenu = CreatePopupMenu();
    HMENU removeMenu = CreatePopupMenu();
    size_t addCount = 0, removeCount = 0;
    for (size_t i = 0; i < currents_.size() && i < kMaxMenuItems; i++) {
        const SpeciesEntry& cur = currents_[i];
        if (cur.path.empty()) continue;
        std::wstring label = TruncateMenuLabel(Utf8ToWide(cur.name.empty() ? cur.path : cur.name));
        bool isFav = false;
        for (const auto& f : favorites_) {
            if (f.path == cur.path) {
                isFav = true;
                break;
            }
        }
        if (!isFav) {
            AppendMenuW(addMenu, MF_STRING, kIdAddBase + (UINT)i, label.c_str());
            addCount++;
        } else {
            AppendMenuW(removeMenu, MF_STRING, kIdRemoveBase + (UINT)i, label.c_str());
            removeCount++;
        }
    }
    if (addCount == 0)
        AppendMenuW(addMenu, MF_STRING | MF_GRAYED, 0, L"(none)");
    if (removeCount == 0)
        AppendMenuW(removeMenu, MF_STRING | MF_GRAYED, 0, L"(none)");
    AppendMenuW(favMenu, MF_POPUP, (UINT_PTR)addMenu, L"Add current");
    AppendMenuW(favMenu, MF_POPUP, (UINT_PTR)removeMenu, L"Remove current");
    AppendMenuW(favMenu, MF_SEPARATOR, 0, nullptr);

    if (favorites_.empty()) {
        AppendMenuW(favMenu, MF_STRING | MF_GRAYED, 0, L"(no favorites)");
    } else {
        for (size_t i = 0; i < favorites_.size() && i < kMaxMenuItems; i++) {
            const SpeciesEntry& f = favorites_[i];
            std::wstring label = TruncateMenuLabel(Utf8ToWide(f.name.empty() ? f.path : f.name));
            AppendMenuW(favMenu, MF_STRING, kIdFavBase + (UINT)i, label.c_str());
        }
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)favMenu, L"Favorites");

    AppendMenuW(menu, MF_STRING | (userPaused_ ? MF_CHECKED : 0), kIdPause,
                userPaused_ ? L"Resume" : L"Pause");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0), kIdStartup,
                L"Run at startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdExit, L"Exit");

    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow(owner_);
    POINT pt{};
    GetCursorPos(&pt);
    UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                                    owner_, nullptr);
    DestroyMenu(menu);

    if (cmd == kIdNext) {
        if (cb_.onNextSpecies) cb_.onNextSpecies();
    } else if (cmd == kIdReseed) {
        if (cb_.onReseed) cb_.onReseed();
    } else if (cmd == kIdSoup) {
        if (cb_.onToggleRandomSoup) cb_.onToggleRandomSoup();
    } else if (cmd == kIdPause) {
        if (cb_.onTogglePause) cb_.onTogglePause();
    } else if (cmd == kIdStartup) {
        if (cb_.onToggleStartup) cb_.onToggleStartup();
    } else if (cmd == kIdExit) {
        if (cb_.onExit) cb_.onExit();
    } else if (cmd == kIdDiscovery) {
        if (cb_.onToggleDiscovery) cb_.onToggleDiscovery();
    } else if (cmd >= kIdAddBase && cmd < kIdAddBase + kMaxMenuItems) {
        size_t i = cmd - kIdAddBase;
        if (i < currents_.size() && cb_.onAddFavorite) cb_.onAddFavorite(currents_[i].path);
    } else if (cmd >= kIdRemoveBase && cmd < kIdRemoveBase + kMaxMenuItems) {
        size_t i = cmd - kIdRemoveBase;
        if (i < currents_.size() && cb_.onRemoveFavorite) cb_.onRemoveFavorite(currents_[i].path);
    } else if (cmd >= kIdFavBase && cmd < kIdFavBase + kMaxMenuItems) {
        size_t i = cmd - kIdFavBase;
        if (i < favorites_.size() && cb_.onSelectFavorite) cb_.onSelectFavorite(favorites_[i].path);
    }
}

bool TrayIcon::IsStartupEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    bool present = RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, &size) ==
                       ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(key);
    return present;
}

void TrayIcon::SetStartupEnabled(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)quoted.c_str(),
                       (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}
