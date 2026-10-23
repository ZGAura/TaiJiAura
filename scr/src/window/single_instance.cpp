#include <desktopball/window/single_instance.hpp>
#include <system_error>
#include <iterator>
#include <stdexcept>

namespace desktopball {
SingleInstance::SingleInstance(const wchar_t* name) {
    mutex_=CreateMutexW(nullptr,FALSE,name);
    const DWORD status=GetLastError();
    if (!mutex_) throw std::system_error(static_cast<int>(status),std::system_category(),"Could not create application instance guard");
    primary_=status!=ERROR_ALREADY_EXISTS;
    // A rejected launcher must not keep the instance alive after the owner exits.
    if (!primary_) { CloseHandle(mutex_); mutex_=nullptr; }
}
SingleInstance::~SingleInstance() { if (mutex_) CloseHandle(mutex_); }
std::wstring applicationSettingsPath() {
    wchar_t module[32768]{};
    const DWORD count=GetModuleFileNameW(nullptr,module,static_cast<DWORD>(std::size(module)));
    if (!count || count>=std::size(module)) throw std::runtime_error("Could not locate application settings");
    const std::wstring path(module,count);
    const auto directory=path.substr(0,path.find_last_of(L"\\/")+1);
    const auto renamed=directory+L"TaiJiAura.ini";
    const auto legacy=directory+L"DesktopBall.ini";
    if (GetFileAttributesW(renamed.c_str())==INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(legacy.c_str())!=INVALID_FILE_ATTRIBUTES) {
        // Copy once; retain the original so an older build can still read it.
        if (!CopyFileW(legacy.c_str(),renamed.c_str(),TRUE) && GetLastError()!=ERROR_FILE_EXISTS)
            return legacy;
    }
    return renamed;
}
}
