#include "util.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

bool ReadFileBytes(const std::filesystem::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    if (len < 0) return false;
    f.seekg(0, std::ios::beg);
    out->resize((size_t)len);
    f.read(out->data(), len);
    return f.good() || f.eof();
}

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

void LogLine(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    // Truncate on first write: a wallpaper process can run for weeks and an
    // append-mode log would grow without bound.
    static std::ofstream log(ExeDir() / "LeniaWallpaper.log", std::ios::trunc);
    if (log) {
        log << buf << std::endl;
    }
}
