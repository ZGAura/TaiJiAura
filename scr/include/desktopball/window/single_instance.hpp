#pragma once
#include <windows.h>
#include <string>

namespace desktopball {
inline constexpr wchar_t instanceMutexName[] = L"Local\\DesktopBall.Application.1";
inline constexpr wchar_t instanceLanguageProperty[] = L"DesktopBall.InterfaceLanguage";
class SingleInstance {
public:
    explicit SingleInstance(const wchar_t* name = instanceMutexName);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    bool primary() const { return primary_; }
private:
    HANDLE mutex_{};
    bool primary_{};
};
std::wstring applicationSettingsPath();
}
