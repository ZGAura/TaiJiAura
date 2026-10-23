#include <desktopball/window/startup.hpp>
#include <stdexcept>
#include <cwchar>
#include <iterator>
#include <utility>

namespace desktopball {
namespace {
bool matches(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& command) {
    if (command.empty()) return false;
    DWORD bytes{};
    if (RegGetValueW(root,path.c_str(),name,RRF_RT_REG_SZ,nullptr,nullptr,&bytes)!=ERROR_SUCCESS ||
        !bytes || bytes>65536) return false;
    std::wstring value(bytes/sizeof(wchar_t)+1,L'\0');
    return RegGetValueW(root,path.c_str(),name,RRF_RT_REG_SZ,nullptr,value.data(),&bytes)==ERROR_SUCCESS &&
        _wcsicmp(value.c_str(),command.c_str())==0;
}
}
StartupRegistration::StartupRegistration(std::wstring executable, std::wstring registryPath)
    : registryPath_(std::move(registryPath)) {
    if (executable.empty()) {
        wchar_t module[32768]{};
        const DWORD length=GetModuleFileNameW(nullptr,module,static_cast<DWORD>(std::size(module)));
        if (!length || length>=std::size(module)) throw std::runtime_error("Could not locate the startup executable");
        executable.assign(module,length);
    }
    command_=L"\""+executable+L"\"";
    const auto separator=executable.find_last_of(L"\\/");
    if (_wcsicmp(executable.substr(separator==std::wstring::npos ? 0 : separator+1).c_str(),L"TaiJiAura.exe")==0) {
        legacyCommand_=L"\""+executable.substr(0,separator==std::wstring::npos ? 0 : separator+1)+L"DesktopBall.exe\"";
        DWORD type{}, bytes{};
        const bool renamedAbsent=RegGetValueW(HKEY_CURRENT_USER,registryPath_.c_str(),L"TaiJiAura",
            RRF_RT_ANY,&type,nullptr,&bytes)==ERROR_FILE_NOT_FOUND;
        if (renamedAbsent && matches(HKEY_CURRENT_USER,registryPath_,L"DesktopBall",legacyCommand_))
            setEnabled(true);
    }
}
bool StartupRegistration::enabled() const {
    return matches(HKEY_CURRENT_USER,registryPath_,L"TaiJiAura",command_) ||
        matches(HKEY_CURRENT_USER,registryPath_,L"DesktopBall",command_) ||
        matches(HKEY_CURRENT_USER,registryPath_,L"DesktopBall",legacyCommand_);
}
LSTATUS StartupRegistration::setEnabled(bool enabled) const {
    if (!enabled) {
        if (matches(HKEY_CURRENT_USER,registryPath_,L"DesktopBall",command_) ||
            matches(HKEY_CURRENT_USER,registryPath_,L"DesktopBall",legacyCommand_)) {
            const auto legacy=RegDeleteKeyValueW(HKEY_CURRENT_USER,registryPath_.c_str(),L"DesktopBall");
            if (legacy!=ERROR_SUCCESS && legacy!=ERROR_FILE_NOT_FOUND && legacy!=ERROR_PATH_NOT_FOUND) return legacy;
        }
        const auto status=RegDeleteKeyValueW(HKEY_CURRENT_USER,registryPath_.c_str(),L"TaiJiAura");
        return status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : status;
    }
    HKEY key{};
    const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,registryPath_.c_str(),0,nullptr,
        REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&key,nullptr);
    if (status!=ERROR_SUCCESS) return status;
    const auto written=RegSetValueExW(key,L"TaiJiAura",0,REG_SZ,
        reinterpret_cast<const BYTE*>(command_.c_str()),static_cast<DWORD>((command_.size()+1)*sizeof(wchar_t)));
    RegCloseKey(key);
    if (written==ERROR_SUCCESS && (matches(HKEY_CURRENT_USER,registryPath_,L"DesktopBall",command_) ||
        matches(HKEY_CURRENT_USER,registryPath_,L"DesktopBall",legacyCommand_)))
        RegDeleteKeyValueW(HKEY_CURRENT_USER,registryPath_.c_str(),L"DesktopBall");
    return written;
}
}
