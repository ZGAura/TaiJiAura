#pragma once
#include <windows.h>
#include <string>

namespace desktopball {
class StartupRegistration {
public:
    // An alternate registry path lets tests exercise the same code without changing Windows startup.
    explicit StartupRegistration(std::wstring executable = {},
        std::wstring registryPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    bool enabled() const;
    LSTATUS setEnabled(bool enabled) const;
private:
    std::wstring command_, legacyCommand_, registryPath_;
};
}
