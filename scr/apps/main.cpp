#include <desktopball/window/overlay.hpp>
#include <exception>
#include <desktopball/window/single_instance.hpp>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        desktopball::SingleInstance instanceGuard;
        if (!instanceGuard.primary() || FindWindowW(L"DesktopBall.NativeOverlay",nullptr)) {
            // Avoid a stack of notices when the shortcut is clicked repeatedly.
            desktopball::SingleInstance noticeGuard(L"Local\\DesktopBall.AlreadyRunningNotice.1");
            if (noticeGuard.primary()) {
                wchar_t saved[32]{};
                GetPrivateProfileStringW(L"Display",L"Language",L"zh-CN",saved,32,
                    desktopball::applicationSettingsPath().c_str());
                auto language=desktopball::parseLanguage(saved);
                if (const auto existing=FindWindowW(L"DesktopBall.NativeOverlay",nullptr)) {
                    const auto value=reinterpret_cast<UINT_PTR>(GetPropW(existing,desktopball::instanceLanguageProperty));
                    if (value==1 || value==2) language=value==2 ? desktopball::Language::english : desktopball::Language::chinese;
                }
                const auto message=desktopball::menuText(language,L"程序已在运行。");
                MessageBoxW(nullptr,message.c_str(),L"TaiJiAura",MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            }
            return 0;
        }
        desktopball::Overlay overlay(instance);
        return overlay.run();
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "TaiJiAura initialization error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
