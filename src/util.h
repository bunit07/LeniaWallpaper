#pragma once
#include <filesystem>
#include <string>

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
bool ReadFileBytes(const std::filesystem::path& p, std::string* out);
std::filesystem::path ExeDir();
void LogLine(const char* fmt, ...);
