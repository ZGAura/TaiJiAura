#include <windows.h>
#include <shellapi.h>
#include <string>
#include <iterator>

// Installer entry point only. The registered Inno Setup engine owns all removal logic.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (!length || length >= std::size(module)) return 1;
    const std::wstring ownPath(module, length);
    const auto directory = ownPath.substr(0, ownPath.find_last_of(L"\\/") + 1);
    wchar_t command[32768]{};
    DWORD bytes = sizeof(command);
    constexpr wchar_t key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{83A5F9E7-438A-4C64-9AF4-61FE9628735D}_is1";
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"UninstallString", RRF_RT_REG_SZ,
        nullptr, command, &bytes) == ERROR_SUCCESS) {
        int count{};
        auto parsed = CommandLineToArgvW(command, &count);
        if (parsed) {
            const std::wstring engine = count == 1 ? parsed[0] : L"";
            LocalFree(parsed);
            const auto engineDirectory = engine.substr(0, engine.find_last_of(L"\\/") + 1);
            if (_wcsicmp(engineDirectory.c_str(), directory.c_str()) == 0 &&
                _wcsicmp(engine.c_str(), ownPath.c_str()) != 0) {
                SHELLEXECUTEINFOW info{sizeof(info)};
                info.lpVerb = L"open";
                info.lpFile = engine.c_str();
                info.lpParameters = arguments;
                info.lpDirectory = directory.c_str();
                info.nShow = SW_SHOWNORMAL;
                if (ShellExecuteExW(&info)) return 0;
            }
        }
    }
    MessageBoxW(nullptr,
        L"无法启动卸载程序，请从 Windows 设置中的应用列表卸载 TaiJiAura。\n\n"
        L"Could not start the uninstaller. Uninstall TaiJiAura from the apps list in Windows Settings.",
        L"TaiJiAura", MB_OK | MB_ICONERROR);
    return 1;
}
