#include <desktopball/window/overlay.hpp>
#include <desktopball/ui/popup_menu.hpp>
#include <desktopball/window/single_instance.hpp>
#include "resource.h"
#include <windowsx.h>
#include <wtsapi32.h>
#include <dbt.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace desktopball {
namespace {
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT_PTR frameTimer = 1, sampleTimer = 2, dockTimer = 3, deviceTimer = 4;
constexpr float dockTransitionSeconds = .24f;
enum Command : UINT { showHide = 100, pauseResume, quit, smoke30 = 104, smoke60, autoDock, expandNow,
    cpuRender = 110, gpuRender, autoStart, toggleOutline,
    capsule30 = 121, capsule60, capsule90, smoke90, chineseLanguage = 130, englishLanguage, displayChoices = 140,
    renderGpuBase = 500, chooseBase = 1000, gaugeBase = 2000, scaleBase = 3000, screenBase = 4000 };
bool isFrameCommand(UINT command) {
    return command==smoke30 || command==smoke60 || (command>=capsule30 && command<=smoke90);
}
constexpr wchar_t windowClass[] = L"DesktopBall.NativeOverlay";
RECT monitorWork(HWND window) {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
    return info.rcWork;
}
int dip(HWND window, int value) { return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96); }
void place(HWND window, RECT rect) {
    SetWindowPos(window, nullptr, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
}
std::wstring usageText(const DeviceReading& reading) {
    if (isTemperature(reading.kind)) return temperatureText(reading);
    if (!reading.percent) return L"—";
    return std::to_wstring(static_cast<int>(std::lround(*reading.percent))) + L"%";
}
}
Overlay::Overlay(HINSTANCE instance, bool showWindow, StartupRegistration startup) : startup_(std::move(startup)) {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls); cls.hInstance = instance; cls.lpfnWndProc = windowProc;
    cls.lpszClassName = windowClass; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance,MAKEINTRESOURCEW(IDI_DESKTOPBALL));
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_DESKTOPBALL),IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("Could not register the overlay window class");
    }
    const UINT dpi = GetDpiForSystem();
    const int width = MulDiv(90, static_cast<int>(dpi), 96);
    const int height = MulDiv(40, static_cast<int>(dpi), 96);
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        windowClass, L"TaiJiAura", WS_POPUP, work.right-width-24, work.top+32,
        width, height, nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error("Could not create the overlay window");
    try {
        refreshRenderingDevices();
        loadSettings();
        SetPropW(window_,instanceLanguageProperty,reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(language_==Language::english ? 2 : 1)));
        const auto* selectedGpu=findReading(renderGpus_,renderGpuId_);
        try {
            renderer_ = std::make_unique<Renderer>(window_,!gpuRendering_,static_cast<float>(capsuleSmokePadding),
                selectedGpu ? selectedGpu->adapterLuid : 0);
        } catch (...) {
            if (!gpuRendering_) throw;
            renderer_=std::make_unique<Renderer>(window_,true,static_cast<float>(capsuleSmokePadding));
        }
        gpuRendering_ = !renderer_->software();
        renderer_->resize(width, height);
        sampleMetrics();
        applyScale(scalePercent_);
        refreshScreens();
        taskbarRestart_ = RegisterWindowMessageW(L"TaskbarCreated");
        addTray(); WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION);
        displayPowerNotification_=RegisterPowerSettingNotification(window_,&GUID_CONSOLE_DISPLAY_STATE,DEVICE_NOTIFY_WINDOW_HANDLE);
        if (showWindow) ShowWindow(window_, SW_SHOWNOACTIVATE);
        frameClock_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!frameClock_) frameClock_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        updateFrameRate(); schedule(); render();
    } catch (...) {
        if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &tray_);
        DestroyWindow(window_); window_ = nullptr;
        if (frameClock_) { CloseHandle(frameClock_); frameClock_ = nullptr; }
        throw;
    }
}
Overlay::~Overlay() {
    if (frameClock_) { CancelWaitableTimer(frameClock_); CloseHandle(frameClock_); frameClock_ = nullptr; }
    if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &tray_);
    if (window_ && IsWindow(window_)) {
        WTSUnRegisterSessionNotification(window_); DestroyWindow(window_);
    }
}
int Overlay::run() {
    for (;;) {
        const DWORD count = frameClock_ ? 1 : 0;
        const DWORD result = MsgWaitForMultipleObjectsEx(count, count ? &frameClock_ : nullptr,
            INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED) return 1;
        if (frameClock_ && result == WAIT_OBJECT_0) {
            try { if (!paused_) render(); armFrame(); }
            catch (const std::exception& error) {
                MessageBoxA(window_,error.what(),"TaiJiAura error",MB_OK | MB_ICONERROR);
                DestroyWindow(window_);
            }
        }
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if (message.message == WM_QUIT) return static_cast<int>(message.wParam);
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
}void Overlay::loadSettings() {
    settingsPath_ = applicationSettingsPath();
    const int saved = GetPrivateProfileIntW(L"Display", L"Count", -1, settingsPath_.c_str());
    if (saved >= 0) {
        settings_.selected.clear();
        for (int i = 0; i < std::min(saved, static_cast<int>(maxDisplayMetrics)); ++i) {
            wchar_t id[1024]{};
            GetPrivateProfileStringW(L"Display", (L"Item"+std::to_wstring(i)).c_str(), L"", id, 1024, settingsPath_.c_str());
            settings_.selected.emplace_back(id);
        }
    }
    wchar_t gauge[1024]{};
    GetPrivateProfileStringW(L"Display", L"Gauge", L"cpu", gauge, 1024, settingsPath_.c_str());
    settings_.gauge = gauge;
    settings_.autoDock = GetPrivateProfileIntW(L"Display", L"AutoDock", 1, settingsPath_.c_str()) != 0;
    settings_.showOutline = GetPrivateProfileIntW(L"Display", L"ShowOutline", 0, settingsPath_.c_str()) != 0;
    wchar_t screen[1024]{};
    GetPrivateProfileStringW(L"Display",L"Screen",L"",screen,1024,settingsPath_.c_str());
    screenId_ = screen;
    wchar_t language[32]{};
    GetPrivateProfileStringW(L"Display",L"Language",L"zh-CN",language,32,settingsPath_.c_str());
    language_ = parseLanguage(language);
    const auto devices = sampler_.devices();
    const auto* preferred = preferredGpu(devices);
    const int version = GetPrivateProfileIntW(L"Display",L"Version",0,settingsPath_.c_str());
    if (version < 2 && preferred) {
        for (auto& id : settings_.selected) if (const auto* d = findReading(devices,id); d && d->kind == DeviceKind::gpu) id = preferred->id;
        if (const auto* d = findReading(devices,settings_.gauge); d && d->kind == DeviceKind::gpu) settings_.gauge = preferred->id;
    }
    smokeFps_=normalizeSmokeFrameRate(GetPrivateProfileIntW(L"Display",L"SmokeFPS",60,settingsPath_.c_str()));
    const bool previouslyAutomatic=GetPrivateProfileIntW(L"Display",L"CapsuleAutoFPS",
        GetPrivateProfileIntW(L"Display",L"AutoFPS",0,settingsPath_.c_str()),settingsPath_.c_str())!=0;
    capsuleFps_=previouslyAutomatic ? 60 : normalizeSmokeFrameRate(GetPrivateProfileIntW(L"Display",L"CapsuleFPS",
        GetPrivateProfileIntW(L"Display",L"ManualFPS",60,settingsPath_.c_str()),settingsPath_.c_str()));
    const int savedScale=static_cast<int>(GetPrivateProfileIntW(L"Display",L"ScalePercent",
        version<3 ? defaultCapsuleScalePercent-20 : defaultCapsuleScalePercent,settingsPath_.c_str()));
    scalePercent_=normalizeCapsuleScalePercent(savedCapsuleScalePercent(savedScale,version));
    gpuRendering_ = gpuAvailable_ && GetPrivateProfileIntW(L"Display",L"GPURendering",0,settingsPath_.c_str()) != 0;
    wchar_t renderId[2048]{};
    GetPrivateProfileStringW(L"Display",L"RenderingGPUId",L"",renderId,2048,settingsPath_.c_str());
    renderGpuId_=renderId;
    if (!findReading(renderGpus_,renderGpuId_)) {
        // A missing explicitly selected card falls back to CPU. Old generic-GPU
        // settings select the first available physical card, with IGPU first.
        if (!renderGpuId_.empty()) gpuRendering_=false;
        const auto first=std::find_if(renderGpus_.begin(),renderGpus_.end(),[](const auto& d) { return d.supported; });
        renderGpuId_=first==renderGpus_.end() ? L"" : first->id;
    }
    normalize(settings_, devices);
}
void Overlay::saveSettings() {
    // Profile APIs preserve Unicode only when the file starts with a UTF-16 BOM.
    const HANDLE file = CreateFileW(settingsPath_.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const wchar_t bom = 0xfeff; DWORD written{};
        WriteFile(file, &bom, sizeof(bom), &written, nullptr); CloseHandle(file);
    }
    WritePrivateProfileStringW(L"Display",L"Version",L"3",settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display",L"GPURendering",gpuRendering_ ? L"1" : L"0",settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display",L"RenderingGPUId",renderGpuId_.c_str(),settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display",L"ScalePercent",std::to_wstring(scalePercent_).c_str(),settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display",L"SmokeFPS",std::to_wstring(smokeFps_).c_str(),settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display",L"CapsuleFPS",std::to_wstring(capsuleFps_).c_str(),settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display",L"CapsuleAutoFPS",nullptr,settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display",L"AutoFPS",nullptr,settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display",L"ManualFPS",nullptr,settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display", L"Count", std::to_wstring(settings_.selected.size()).c_str(), settingsPath_.c_str());
    for (size_t i = 0; i < settings_.selected.size(); ++i)
        WritePrivateProfileStringW(L"Display", (L"Item"+std::to_wstring(i)).c_str(), settings_.selected[i].c_str(), settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display", L"Gauge", settings_.gauge.c_str(), settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display", L"AutoDock", settings_.autoDock ? L"1" : L"0", settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display", L"ShowOutline", settings_.showOutline ? L"1" : L"0", settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display",L"Screen",screenId_.c_str(),settingsPath_.c_str());
    WritePrivateProfileStringW(L"Display",L"Language",languageCode(language_),settingsPath_.c_str());
}
void Overlay::addTray() {
    tray_ = {};
    tray_.cbSize = sizeof(tray_); tray_.hWnd = window_; tray_.uID = 1;
    tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    tray_.uCallbackMessage = trayMessage;
    tray_.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_DESKTOPBALL),IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
    // Integration test executables link the core without the application's resources.
    if (!tray_.hIcon) tray_.hIcon = LoadIconW(nullptr,IDI_APPLICATION);
    wcscpy_s(tray_.szTip, menuText(language_,L"TaiJiAura — 右键设置显示").c_str());
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
    if (trayAdded_) { tray_.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &tray_); updateTooltip(); }
}
void Overlay::updateTooltip() {
    if (!trayAdded_) return;
    const auto devices = readings(metrics_);
    std::wstring tip = L"TaiJiAura";
    for (const auto& device : devices) {
        if ((collapsed_ && device.id == settings_.gauge) ||
            (!collapsed_ && std::find(settings_.selected.begin(), settings_.selected.end(), device.id) != settings_.selected.end()))
            tip += L"\n" + device.label + L" " + usageText(device);
    }
    if (metrics_.network) {
        std::wostringstream s;
        s << std::fixed << std::setprecision(1) << L"\n↓ " << metrics_.network->download/1000 << L" KB/s  ↑ " << metrics_.network->upload/1000 << L" KB/s";
        tip += s.str();
    }
    wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE);
    NOTIFYICONDATAW update = tray_; update.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &update);
}
void Overlay::schedule() {
    KillTimer(window_, frameTimer); KillTimer(window_, sampleTimer); KillTimer(window_, dockTimer);
    if (frameClock_) CancelWaitableTimer(frameClock_);
    if (!displayOn_ || suspended_) {
        KillTimer(window_,deviceTimer);
        lastFrame_=Clock::now();
        return;
    }
    if (visible_ && !paused_ && !locked_) {
        if (frameClock_) armFrame();
        else {
            SetTimer(window_, frameTimer, (1000+fps_-1)/fps_, nullptr);
        }
    }
    if (visible_ && !locked_) {
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-lastSample_).count();
        SetTimer(window_,sampleTimer,static_cast<UINT>(std::clamp(1000-elapsed,1LL,1000LL)),nullptr);
    } else sampler_.reset();
    if (visible_ && !locked_ && dockSide_!=DockSide::none) SetTimer(window_, dockTimer, 30, nullptr);
    lastFrame_ = Clock::now();
}
void Overlay::sampleMetrics() {
    if (!visible_ || !outputAvailable()) return;
    // Availability comes from discovery, never from whether an unselected metric has a value.
    normalize(settings_, sampler_.devices());
    const auto request=samplingRequest(settings_,collapsed_);
    // Drop hidden values immediately, including network data in smoke mode.
    // Menu actions and mode changes must not trigger extra hardware reads.
    if (!request.cpu) metrics_.cpu.reset();
    if (!request.memory) {
        metrics_.memoryPercent.reset(); metrics_.memoryUsed=metrics_.memoryTotal=0;
    }
    if (!request.network) { metrics_.network.reset(); metrics_.adapterName.clear(); }
    for (auto& device : metrics_.accelerators)
        if (std::find(request.acceleratorIds.begin(),request.acceleratorIds.end(),device.id)==request.acceleratorIds.end()) {
            device.percent.reset(); device.celsius.reset(); device.used=0;
        }
    const auto now=Clock::now();
    if (lastSample_!=Clock::time_point{} && now-lastSample_<std::chrono::seconds(1)) {
        updateTooltip(); return;
    }
    metrics_ = sampler_.sample(request);
    lastSample_=Clock::now();
    lastSampleRequest_=request;
    ++sampleCount_;
    SetTimer(window_,sampleTimer,1000,nullptr);
    updateTooltip();
}
void Overlay::refreshRenderingDevices() {
    renderGpus_.clear(); gpuAvailable_=false;
    for (auto device : sampler_.devices()) if (device.kind==DeviceKind::gpu) {
        if (!device.renderingId.empty()) device.id=device.renderingId;
        const auto separator=device.name.find(L" · ");
        if (separator!=std::wstring::npos) device.name=device.name.substr(separator+3);
        device.supported=device.adapterLuid && Renderer::hardwareAvailable(device.adapterLuid);
        gpuAvailable_=gpuAvailable_ || device.supported;
        renderGpus_.push_back(std::move(device));
    }
}
void Overlay::setGpuRendering(bool enabled, const std::wstring& deviceId) {
    if (!displayOn_ || suspended_) return;
    const DeviceReading* selected{};
    if (enabled) {
        selected=findReading(renderGpus_,deviceId.empty() ? renderGpuId_ : deviceId);
        if (!selected && deviceId.empty()) {
            const auto first=std::find_if(renderGpus_.begin(),renderGpus_.end(),[](const auto& d) { return d.supported; });
            if (first!=renderGpus_.end()) selected=&*first;
        }
        if (!selected) return;
    }
    if (renderer_ && enabled==gpuRendering_ && (!enabled || renderer_->adapterLuid()==selected->adapterLuid)) return;
    try {
        if (enabled) renderer_->setHardware(selected->adapterLuid,animationSeconds_,metrics_,settings_,collapseBlend());
        else renderer_->setSoftware(true,animationSeconds_,metrics_,settings_,collapseBlend());
    }
    catch (const std::exception& error) {
        if (selected) for (auto& device : renderGpus_) if (device.id==selected->id) device.supported=false;
        MessageBoxA(window_,error.what(),"Could not switch renderer; previous renderer retained",MB_OK | MB_ICONERROR);
        return;
    }
    if (enabled) renderGpuId_=selected->id;
    gpuRendering_ = !renderer_->software();
    drawMilliseconds_=0;
    updateFrameRate(); saveSettings(); schedule(); render();
}
void Overlay::refreshDevices() {
    if (!displayOn_ || suspended_) return;
    sampler_.refreshDevices();
    refreshRenderingDevices();
    if (gpuRendering_) {
        if (findReading(renderGpus_,renderGpuId_)) setGpuRendering(true,renderGpuId_);
        else setGpuRendering(false);
    }
    sampleMetrics();
}
void Overlay::armFrame() {
    if (!frameClock_ || !visible_ || paused_ || !outputAvailable() || !IsWindow(window_)) return;
    const double elapsed = std::chrono::duration<double>(Clock::now()-lastFrame_).count();
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(std::max(.0005,1.0/fps_-elapsed)*10000000);
    SetWaitableTimer(frameClock_,&due,0,nullptr,nullptr,FALSE);
}
void Overlay::updateFrameRate() {
    MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    if (GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor) &&
        EnumDisplaySettingsExW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&mode,0) && mode.dmDisplayFrequency > 1)
        refreshRate_ = mode.dmDisplayFrequency;
    const unsigned target=activeFrameRate(collapsed_,capsuleFps_,smokeFps_,refreshRate_);
    if (target != fps_) {
        fps_ = target;
        KillTimer(window_,frameTimer);
        if (frameClock_) { CancelWaitableTimer(frameClock_); armFrame(); }
        else if (visible_ && !paused_ && outputAvailable()) {
            SetTimer(window_,frameTimer,(1000+fps_-1)/fps_,nullptr);
        }
    }
}
void Overlay::render() {
    if (!visible_ || !outputAvailable() || relocating_ || !renderer_) return;
    if (!collapsed_ && !transitioning_ && !moving_) {
        RECT rect{}; GetClientRect(window_,&rect);
        if (rect.right != scaledDip(capsuleWindowWidth(settings_.selected.size())) || rect.bottom != scaledDip(capsuleWindowHeight)) {
            applyScale(scalePercent_); return;
        }
    }
    const auto now = Clock::now();
    if (!paused_) animationSeconds_ += std::min(std::chrono::duration<float>(now-lastFrame_).count(), .25f);
    lastFrame_ = now;
    const float blend=collapseBlend();
    if (!renderer_->draw(animationSeconds_, metrics_, settings_, blend)) {
        try { renderer_->setSoftware(!gpuRendering_, animationSeconds_, metrics_, settings_, blend); }
        catch (...) {
            renderer_->setSoftware(true,animationSeconds_,metrics_,settings_,blend);
            gpuRendering_=false; saveSettings();
        }
        gpuRendering_ = !renderer_->software();
    }
    ++renderedFrames_;
    const double cost=renderer_->drawMilliseconds();
    drawMilliseconds_=drawMilliseconds_>0 ? drawMilliseconds_*.9+cost*.1 : cost;
}
int Overlay::scaledDip(int value) const {
    return MulDiv(value * actualCapsuleScalePercent(scalePercent_),static_cast<int>(GetDpiForWindow(window_)),9600);
}
void Overlay::rememberScreen() {
    const auto screens = enumerateScreens();
    const auto current = MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST);
    for (const auto& screen : screens) if (screen.handle==current) {
        screenId_ = screen.id; screenWork_ = screen.work;
        saveSettings(); return;
    }
}
void Overlay::moveToScreen(const Screen& requested) {
    // Revalidate after the native menu closes: a monitor may have been unplugged.
    const auto screens = enumerateScreens();
    const auto* screen = resolveScreen(screens,requested.id);
    if (!screen || !renderer_) return;
    RECT rect{}; GetWindowRect(window_,&rect);
    if (dockSide_!=DockSide::none) rect = expandedRect_;
    const RECT sourceWork = screenWork_.right>screenWork_.left ? screenWork_ : monitorWork(window_);
    const auto targetWork = screen->work;
    const int oldWidth = scaledDip(capsuleWindowWidth(settings_.selected.size())), oldHeight = scaledDip(capsuleWindowHeight);
    auto expanded = transferScreenRect(rect,sourceWork,targetWork,oldWidth,oldHeight);
    if (dockSide_!=DockSide::none) expanded = dockRect(expanded,targetWork,dockSide_,oldWidth,oldHeight);
    transitioning_ = false;
    relocating_ = true;
    // Moving the HWND updates its per-monitor DPI. Suppress intermediate draws and
    // let the final placement below size both dock states using that destination DPI.
    place(window_,collapsed_ ? dockRect(expanded,targetWork,dockSide_,scaledDip(14),scaledDip(100)) : expanded);
    const int width = scaledDip(capsuleWindowWidth(settings_.selected.size())), height = scaledDip(capsuleWindowHeight);
    expandedRect_ = transferScreenRect(rect,sourceWork,targetWork,width,height);
    if (dockSide_!=DockSide::none) expandedRect_ = dockRect(expandedRect_,targetWork,dockSide_,width,height);
    workRect_ = targetWork;
    place(window_,collapsed_ ? dockRect(expandedRect_,workRect_,dockSide_,scaledDip(14),scaledDip(100)) : expandedRect_);
    relocating_ = false;
    RECT client{}; GetClientRect(window_,&client);
    renderer_->resize(client.right,client.bottom);
    screenId_ = screen->id; screenWork_ = targetWork;
    dockActivity_ = Clock::now(); hoverSince_ = {};
    updateFrameRate(); saveSettings(); schedule(); render();
}
void Overlay::refreshScreens() {
    const auto screens = enumerateScreens();
    if (const auto* screen = resolveScreen(screens,screenId_)) moveToScreen(*screen);
}
void Overlay::applyScale(int percent) {
    scalePercent_ = normalizeCapsuleScalePercent(percent);
    transitioning_ = false;
    RECT rect{}; GetWindowRect(window_,&rect);
    workRect_ = monitorWork(window_);
    const int width = scaledDip(capsuleWindowWidth(settings_.selected.size())), height = scaledDip(capsuleWindowHeight);
    if (dockSide_ != DockSide::none) {
        expandedRect_ = dockRect(collapsed_ ? expandedRect_ : rect,workRect_,dockSide_,width,height);
        rect = collapsed_ ? dockRect(expandedRect_,workRect_,dockSide_,scaledDip(14),scaledDip(100)) : expandedRect_;
    } else {
        rect.left = std::clamp(rect.left,workRect_.left,std::max(workRect_.left,workRect_.right-width));
        rect.top = std::clamp(rect.top,workRect_.top,std::max(workRect_.top,workRect_.bottom-height));
        rect.right = rect.left+width; rect.bottom = rect.top+height;
    }
    place(window_,rect);
    dockActivity_ = Clock::now();
    render();
}
float Overlay::collapseBlend() const {
    if (!transitioning_) return collapsed_ ? 1.0f : 0.0f;
    const float f=std::clamp(std::chrono::duration<float>(Clock::now()-transitionStart_).count()/dockTransitionSeconds,0.0f,1.0f);
    const float eased=f*f*(3-2*f);
    return collapseFrom_+((collapsed_ ? 1.0f : 0.0f)-collapseFrom_)*eased;
}
void Overlay::setCollapsed(bool collapsed, bool animate) {
    if (dockSide_ == DockSide::none || collapsed == collapsed_) return;
    collapseFrom_=collapseBlend();
    transitioning_=false;
    GetWindowRect(window_, &animationFrom_);
    collapsed_ = collapsed;
    drawMilliseconds_=0;
    sampleMetrics();
    updateFrameRate();
    animationTo_ = collapsed ? dockRect(expandedRect_, workRect_, dockSide_, scaledDip(14), scaledDip(100)) : expandedRect_;
    dockActivity_ = Clock::now(); hoverSince_ = {};
    if (animate) { transitionStart_ = Clock::now(); transitioning_ = true; }
    else place(window_, animationTo_);
    schedule(); render(); updateTooltip();
}
void Overlay::cancelDock() {
    if (dockSide_ == DockSide::none) return;
    if (collapsed_) setCollapsed(false, false);
    if (transitioning_) { transitioning_ = false; place(window_, expandedRect_); }
    dockSide_ = DockSide::none; collapsed_ = false;
    schedule(); render();
}
void Overlay::snapToEdge() {
    if (!settings_.autoDock) { cancelDock(); return; }
    RECT rect{}; GetWindowRect(window_, &rect);
    workRect_ = monitorWork(window_);
    const auto side = nearestEdge(rect, workRect_, dip(window_,12));
    if (side == DockSide::none) { cancelDock(); return; }
    dockSide_ = side;
    expandedRect_ = dockRect(rect, workRect_, side, scaledDip(capsuleWindowWidth(settings_.selected.size())), scaledDip(capsuleWindowHeight));
    place(window_, expandedRect_);
    dockActivity_ = Clock::now(); hoverSince_ = {};
    if (visible_ && outputAvailable()) SetTimer(window_,dockTimer,30,nullptr);
}
void Overlay::updateDock() {
    if (dockSide_ == DockSide::none || !visible_ || !outputAvailable()) return;
    const auto now = Clock::now();
    if (transitioning_) {
        float f = std::clamp(std::chrono::duration<float>(now-transitionStart_).count()/dockTransitionSeconds, 0.0f, 1.0f);
        const float eased = f*f*(3-2*f);
        auto lerp = [&](LONG a, LONG b) { return a+static_cast<LONG>(std::lround((b-a)*eased)); };
        place(window_, {lerp(animationFrom_.left,animationTo_.left), lerp(animationFrom_.top,animationTo_.top),
            lerp(animationFrom_.right,animationTo_.right), lerp(animationFrom_.bottom,animationTo_.bottom)});
        if (f >= 1) { transitioning_ = false; schedule(); render(); }
        return;
    }
    if (moving_ || menuOpen_) return;
    POINT cursor{}; GetCursorPos(&cursor);
    RECT rect{}; GetWindowRect(window_, &rect);
    const bool hover = PtInRect(&rect, cursor) != FALSE;
    if (hover) {
        dockActivity_ = now;
        if (collapsed_) {
            if (hoverSince_ == Clock::time_point{}) hoverSince_ = now;
            if (now-hoverSince_ > std::chrono::milliseconds(120)) setCollapsed(false);
        }
    } else {
        hoverSince_ = {};
        if (!collapsed_ && now-dockActivity_ > std::chrono::milliseconds(800)) setCollapsed(true);
    }
}
void Overlay::trayMenu() {
    menuOpen_ = true;
    const auto devices = sampler_.devices();
    const auto screens = enumerateScreens();
    const auto currentScreen = MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST);
    PopupMenu theme(GetDpiForWindow(window_),language_);
    HMENU menu = theme.create(), choices = theme.create(), gaugeChoices = theme.create();
    theme.add(menu,0,L"TaiJiAura",L"桌面监控 · 轻量悬浮",false,false,nullptr,{},false,PopupMenu::Kind::header);
    theme.separator(menu);
    theme.add(menu,0,L"显示设置",{},false,false,nullptr,{},false,PopupMenu::Kind::section);
    theme.add(choices,0,L"显示内容",L"最多四项；满四项后替换最后一项",false,false,nullptr,{},false,PopupMenu::Kind::header);
    theme.add(gaugeChoices,0,L"收起后显示",L"选择贴边收起时的监控指标",false,false,nullptr,{},false,PopupMenu::Kind::header);
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        const bool checked = std::find(settings_.selected.begin(),settings_.selected.end(),device.id) != settings_.selected.end();
        const bool enabled = eligibleMetric(device) && !(checked && settings_.selected.size() <= minDisplayMetrics);
        const bool strike=isTemperature(device.kind) && !device.supported;
        theme.add(choices,static_cast<UINT>(chooseBase+i),device.name,{},checked,enabled,
            nullptr,{},false,PopupMenu::Kind::action,strike,isTemperature(device.kind));
        theme.add(gaugeChoices,static_cast<UINT>(gaugeBase+i),device.name,{},settings_.gauge == device.id,
            eligibleMetric(device),nullptr,{},false,PopupMenu::Kind::action,strike,isTemperature(device.kind));
    }
    theme.add(menu,displayChoices,L"显示内容",{},false,true,choices,std::to_wstring(settings_.selected.size())+L" 项");
    HMENU positions = theme.create();
    std::wstring positionValue;
    for (size_t i=0; i<screens.size(); ++i) {
        const bool current = screens[i].handle==currentScreen;
        theme.add(positions,static_cast<UINT>(screenBase+i),screenTitle(screens[i],i),{},current,screens.size()>1);
        if (current) positionValue = L"显示屏"+std::to_wstring(i+1);
    }
    if (screens.empty()) theme.add(positions,0,L"未检测到显示屏",{},false,false);
    theme.add(menu,0,L"显示位置",{},false,true,positions,positionValue);
    HMENU languages = theme.create();
    theme.add(languages,chineseLanguage,L"中文",{},language_==Language::chinese);
    theme.add(languages,englishLanguage,L"English",{},language_==Language::english);
    theme.add(menu,0,L"语言",{},false,true,languages,language_==Language::chinese ? L"中文" : L"English");
    const auto* gaugeReading=findReading(devices,settings_.gauge);
    theme.add(menu,0,L"收起后显示",{},false,true,gaugeChoices,gaugeReading ? gaugeReading->label : L"—");
    HMENU sizes = theme.create();
    theme.add(sizes,0,L"整体大小",L"图案、文字与网速同步缩放",false,false,nullptr,{},false,PopupMenu::Kind::header);
    for (const int percent : capsuleScalePercentages) {
        theme.add(sizes,scaleBase+percent,std::to_wstring(percent)+L"%",{},scalePercent_ == percent,true,nullptr,
            percent == defaultCapsuleScalePercent ? L"默认" : L"");
    }
    theme.add(menu,0,L"整体大小",{},false,true,sizes,std::to_wstring(scalePercent_)+L"%");
    theme.add(menu,toggleOutline,L"显示轮廓",L"仅展开胶囊：玻璃描边与内侧高光",settings_.showOutline);
    theme.add(menu,autoDock,L"贴边自动收起",L"靠近屏幕左 / 右边缘时收起",settings_.autoDock);
    if (collapsed_) theme.add(menu,expandNow,L"立即展开");
    theme.separator(menu);
    theme.add(menu,0,L"运行控制",{},false,false,nullptr,{},false,PopupMenu::Kind::section);
    HMENU rendering = theme.create();
    const auto renderingDevices=renderGpus_;
    theme.add(rendering,0,L"渲染方式",L"默认使用 CPU，可手动切换",false,false,nullptr,{},false,PopupMenu::Kind::header);
    for (size_t i=0;i<renderingDevices.size();++i) {
        const auto& gpu=renderingDevices[i];
        theme.add(rendering,renderGpuBase+static_cast<UINT>(i),gpu.label,{},
            gpuRendering_ && renderGpuId_==gpu.id,gpu.supported,nullptr,gpu.name,false,PopupMenu::Kind::action,false,true);
    }
    theme.add(rendering,cpuRender,L"CPU 渲染",{},!gpuRendering_,true,nullptr,L"默认");
    const auto* activeGpu=findReading(renderGpus_,renderGpuId_);
    theme.add(menu,0,L"渲染方式",{},false,true,rendering,gpuRendering_ && activeGpu ? activeGpu->label : L"CPU");
    HMENU capsuleFrames=theme.create(), frames=theme.create();
    theme.add(capsuleFrames,0,L"胶囊帧率",L"展开胶囊：手动选择，受屏幕刷新率限制",false,false,nullptr,{},false,PopupMenu::Kind::header);
    theme.add(capsuleFrames,capsule30,L"30 FPS",{},capsuleFps_==30);
    theme.add(capsuleFrames,capsule60,L"60 FPS",{},capsuleFps_==60,true,nullptr,L"默认");
    theme.add(capsuleFrames,capsule90,L"90 FPS",{},capsuleFps_==90);
    theme.add(menu,0,L"胶囊帧率",{},false,true,capsuleFrames,std::to_wstring(capsuleFps_)+L" FPS");
    theme.add(frames,0,L"烟流帧率",L"仅收起后的竖向烟流；独立于胶囊帧率",false,false,nullptr,{},false,PopupMenu::Kind::header);
    theme.add(frames,smoke30,L"30 FPS",{},smokeFps_==30);
    theme.add(frames,smoke60,L"60 FPS",{},smokeFps_==60,true,nullptr,L"默认");
    theme.add(frames,smoke90,L"90 FPS",{},smokeFps_==90);
    theme.add(menu,0,L"烟流帧率",{},false,true,frames,std::to_wstring(smokeFps_)+L" FPS");
    theme.add(menu,autoStart,L"开机自动启动",L"登录当前 Windows 用户后启动",startup_.enabled());
    theme.add(menu,pauseResume,paused_ ? L"继续动画" : L"暂停动画");
    theme.add(menu,showHide,visible_ ? L"隐藏胶囊" : L"显示胶囊");
    theme.separator(menu);
    theme.add(menu,quit,L"退出 TaiJiAura",{},false,true,nullptr,{},true);
    POINT point{}; GetCursorPos(&point); SetForegroundWindow(window_);
    const auto selectMetric = [&](UINT command) {
        if (command<chooseBase || command-chooseBase>=devices.size()) return false;
        if (!eligibleMetric(devices[command-chooseBase])) return false;
        const auto& id=devices[command-chooseBase].id;
        if (toggleDisplayMetric(settings_,id)) {
            sampleMetrics(); applyScale(scalePercent_); saveSettings(); updateFrameRate();
        }
        return true;
    };
    const UINT selected = theme.track(menu,window_,point);
    PostMessageW(window_, WM_NULL, 0, 0);
    menuOpen_ = false; dockActivity_ = Clock::now();
    if (selected>=renderGpuBase && selected-renderGpuBase<renderingDevices.size()) {
        setGpuRendering(true,renderingDevices[selected-renderGpuBase].id); return;
    }
    if (selected>=screenBase && selected-screenBase<screens.size()) {
        if (screens.size()>1 && screens[selected-screenBase].handle!=currentScreen)
            moveToScreen(screens[selected-screenBase]);
        return;
    }
    if (isCapsuleScalePercent(static_cast<int>(selected)-scaleBase)) {
        SendMessageW(window_,WM_COMMAND,selected,0); return;
    }
    if (selected == cpuRender || selected == gpuRender || isFrameCommand(selected) ||
        selected == chineseLanguage || selected == englishLanguage) {
        SendMessageW(window_,WM_COMMAND,selected,0); return;
    }
    if (selected >= chooseBase && selected < chooseBase+devices.size()) {
        selectMetric(selected); return;
    }
    if (selected >= gaugeBase && selected < gaugeBase+devices.size()) {
        if (!eligibleMetric(devices[selected-gaugeBase])) return;
        settings_.gauge = devices[selected-gaugeBase].id;
        sampleMetrics(); saveSettings(); render(); return;
    }
    switch (selected) {
    case toggleOutline:
        settings_.showOutline = !settings_.showOutline;
        saveSettings(); break;
    case autoDock:
        settings_.autoDock = !settings_.autoDock;
        if (!settings_.autoDock) cancelDock(); else snapToEdge();
        saveSettings(); break;
    case expandNow: setCollapsed(false); break;
    case showHide:
        if (visible_ && !trayAdded_) break;
        visible_ = !visible_; ShowWindow(window_,visible_ ? SW_SHOWNOACTIVATE : SW_HIDE); break;
    case pauseResume: paused_ = !paused_; break;
    case autoStart: {
        const auto status=startup_.setEnabled(!startup_.enabled());
        if (status!=ERROR_SUCCESS) {
            const auto message=menuText(language_,L"无法更新开机启动设置，错误代码：")+std::to_wstring(status);
            MessageBoxW(window_,message.c_str(),menuText(language_,L"开机自动启动").c_str(),MB_OK | MB_ICONERROR);
        }
        break;
    }
    case quit: DestroyWindow(window_); return;
    default: return;
    }
    schedule(); render();
}
LRESULT CALLBACK Overlay::windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window,message,wparam,lparam);
    try { return self->handle(message,wparam,lparam); }
    catch (const std::exception& error) {
        MessageBoxA(window,error.what(),"TaiJiAura error",MB_OK | MB_ICONERROR);
        DestroyWindow(window); return 0;
    }
}
LRESULT Overlay::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    if (PopupMenu::handleMessage(message,wparam,lparam)) return 0;
    if (taskbarRestart_ && message == taskbarRestart_) { addTray(); return 0; }
    switch (message) {
    case WM_MEASUREITEM:
        if (PopupMenu::measure(reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_DRAWITEM:
        if (PopupMenu::draw(reinterpret_cast<DRAWITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_COMMAND:
        if (LOWORD(wparam)>=renderGpuBase && LOWORD(wparam)<chooseBase) {
            const size_t index=LOWORD(wparam)-renderGpuBase;
            if (index<renderGpus_.size()) setGpuRendering(true,renderGpus_[index].id);
            return 0;
        }
        if (LOWORD(wparam)==chineseLanguage || LOWORD(wparam)==englishLanguage) {
            language_ = LOWORD(wparam)==englishLanguage ? Language::english : Language::chinese;
            SetPropW(window_,instanceLanguageProperty,reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(language_==Language::english ? 2 : 1)));
            saveSettings(); updateTooltip(); return 0;
        }
        if (LOWORD(wparam)>=screenBase) {
            const auto screens = enumerateScreens();
            const size_t index = LOWORD(wparam)-screenBase;
            if (screens.size()>1 && index<screens.size()) moveToScreen(screens[index]);
            return 0;
        }
        if (isFrameCommand(LOWORD(wparam))) {
            switch (LOWORD(wparam)) {
            case capsule30: capsuleFps_=30; break;
            case capsule60: capsuleFps_=60; break;
            case capsule90: capsuleFps_=90; break;
            case smoke30: smokeFps_=30; break;
            case smoke60: smokeFps_=60; break;
            case smoke90: smokeFps_=90; break;
            }
            sampleMetrics(); updateFrameRate(); saveSettings(); schedule(); render(); return 0;
        }
        if (LOWORD(wparam) == cpuRender || LOWORD(wparam) == gpuRender) {
            setGpuRendering(LOWORD(wparam) == gpuRender); return 0;
        }
        if (isCapsuleScalePercent(static_cast<int>(LOWORD(wparam))-scaleBase)) {
            applyScale(LOWORD(wparam)-scaleBase); saveSettings(); return 0;
        }
        break;
    case WM_TIMER:
        if (wparam == sampleTimer) {
            sampleMetrics(); updateFrameRate();
            if (paused_) render();
        } else if (wparam == frameTimer) render();
        else if (wparam == dockTimer) updateDock();
        else if (wparam == deviceTimer) {
            KillTimer(window_,deviceTimer); if (menuOpen_) EndMenu(); refreshDevices(); refreshScreens(); render();
        }
        return 0;
    case WM_DEVICECHANGE:
        if (wparam == DBT_DEVNODES_CHANGED && displayOn_ && !suspended_) SetTimer(window_,deviceTimer,1500,nullptr);
        return 0;
    case WM_ENTERSIZEMOVE:
        moving_ = true; cancelDock(); return 0;
    case WM_EXITSIZEMOVE:
        moving_ = false; rememberScreen(); updateFrameRate(); snapToEdge(); return 0;
    case WM_LBUTTONDOWN:
        if (collapsed_) setCollapsed(false);
        return 0;
    case WM_NCHITTEST: {
        if (collapsed_) return HTCLIENT;
        POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ScreenToClient(window_,&point);
        RECT rect{}; GetClientRect(window_,&rect);
        if (!rect.right || !rect.bottom) return HTTRANSPARENT;
        const float canvas = static_cast<float>(capsuleLayout(settings_.selected.size()).width)*6;
        const float scale=std::min(static_cast<float>(rect.right)/(canvas/6+2*capsuleSmokePadding),
            static_cast<float>(rect.bottom)/capsuleWindowHeight);
        const float x = (point.x-(rect.right-canvas/6*scale)*.5f)/scale*6-canvas*.5f+7.5f;
        const float y = (point.y-(rect.bottom-40*scale)*.5f)/scale*6-120;
        const float core = canvas*.5f-106.5f;
        const float dx = x-std::clamp(x,-core,core);
        return dx*dx+y*y <= 96*96 ? HTCAPTION : HTTRANSPARENT;
    }
    case WM_NCRBUTTONUP: case WM_CONTEXTMENU: trayMenu(); return 0;
    case trayMessage:
        if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP) trayMenu();
        else if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT) {
            if (!visible_) { visible_ = true; ShowWindow(window_,SW_SHOWNOACTIVATE); }
            if (collapsed_) setCollapsed(false); schedule(); render();
        }
        return 0;
    case WM_SIZE:
        if (!relocating_ && renderer_ && LOWORD(lparam) && HIWORD(lparam)) { renderer_->resize(LOWORD(lparam),HIWORD(lparam)); render(); }
        return 0;
    case WM_DPICHANGED: {
        if (relocating_) return 0;
        const bool wasDocked = dockSide_ != DockSide::none;
        cancelDock();
        auto suggested = *reinterpret_cast<RECT*>(lparam);
        if (wasDocked) {
            suggested.right = suggested.left + MulDiv(capsuleWindowWidth(settings_.selected.size())*actualCapsuleScalePercent(scalePercent_), LOWORD(wparam), 9600);
            suggested.bottom = suggested.top + MulDiv(capsuleWindowHeight*actualCapsuleScalePercent(scalePercent_), HIWORD(wparam), 9600);
        }
        place(window_,suggested); updateFrameRate(); return 0;
    }
    case WM_DISPLAYCHANGE: {
        if (menuOpen_) EndMenu();
        refreshScreens(); return 0;
    }
    case WM_SETTINGCHANGE:
        if (wparam==SPI_SETWORKAREA) { if (menuOpen_) EndMenu(); refreshScreens(); }
        break;
    case WM_WTSSESSION_CHANGE:
        if (wparam == WTS_SESSION_LOCK) locked_ = true;
        else if (wparam == WTS_SESSION_UNLOCK) { locked_ = false; sampler_.reset(); sampleMetrics(); }
        schedule(); render(); return 0;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND) {
            suspended_=true; displayOn_=false;
            if (menuOpen_) EndMenu();
            schedule();
        } else if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
            suspended_=false;
            // Automatic maintenance wakes must stay idle until the display actually turns on.
            if (wparam==PBT_APMRESUMESUSPEND || !displayPowerNotification_) displayOn_=true;
            sampler_.reset(); refreshDevices(); schedule(); render();
        } else if (wparam==PBT_POWERSETTINGCHANGE && lparam) {
            const auto* setting=reinterpret_cast<const POWERBROADCAST_SETTING*>(lparam);
            if (IsEqualGUID(setting->PowerSetting,GUID_CONSOLE_DISPLAY_STATE) && setting->DataLength==sizeof(DWORD)) {
                DWORD state{}; std::memcpy(&state,setting->Data,sizeof(state));
                if (state<=2) {
                    const bool wasOn=displayOn_;
                    displayOn_=state!=0; // Dimmed displays are still visible.
                    if (!displayOn_ && menuOpen_) EndMenu();
                    if (!wasOn && displayOn_ && !suspended_) { sampler_.reset(); refreshDevices(); }
                    schedule(); render();
                }
            }
        }
        return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(window_,&paint); EndPaint(window_,&paint); return 0; }
    case WM_CLOSE: DestroyWindow(window_); return 0;
    case WM_DESTROY:
        if (displayPowerNotification_) {
            UnregisterPowerSettingNotification(displayPowerNotification_); displayPowerNotification_=nullptr;
        }
        RemovePropW(window_,instanceLanguageProperty);
        if (frameClock_) CancelWaitableTimer(frameClock_);
        KillTimer(window_,frameTimer); KillTimer(window_,sampleTimer); KillTimer(window_,dockTimer); KillTimer(window_,deviceTimer);
        WTSUnRegisterSessionNotification(window_);
        if (trayAdded_) { Shell_NotifyIconW(NIM_DELETE,&tray_); trayAdded_ = false; }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window_,message,wparam,lparam);
}
}


