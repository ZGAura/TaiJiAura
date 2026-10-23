#include <desktopball/window/screens.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

namespace desktopball {
std::vector<Screen> enumerateScreens() {
    struct Context {
        std::vector<Screen> screens;
        std::vector<std::wstring> devices;
    } context;
    DISPLAY_DEVICEW adapter{}; adapter.cb = sizeof(adapter);
    for (DWORD i=0; EnumDisplayDevicesW(nullptr,i,&adapter,0); ++i) {
        if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
            !(adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) context.devices.emplace_back(adapter.DeviceName);
        adapter = {}; adapter.cb = sizeof(adapter);
    }
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR handle,HDC,LPRECT,LPARAM data)->BOOL {
        auto& state = *reinterpret_cast<Context*>(data);
        MONITORINFOEXW info{}; info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(handle,&info) ||
            std::find(state.devices.begin(),state.devices.end(),info.szDevice)==state.devices.end() ||
            info.rcWork.right<=info.rcWork.left || info.rcWork.bottom<=info.rcWork.top) return TRUE;
        // Skip disconnected child devices retained by the display driver. The active
        // physical interface survives changes in the display enumeration order.
        std::wstring identity = info.szDevice;
        DISPLAY_DEVICEW monitor{}; monitor.cb = sizeof(monitor);
        for (DWORD i=0; EnumDisplayDevicesW(info.szDevice,i,&monitor,EDD_GET_DEVICE_INTERFACE_NAME); ++i) {
            if ((monitor.StateFlags & DISPLAY_DEVICE_ACTIVE) && monitor.DeviceID[0]) {
                identity = monitor.DeviceID; break;
            }
            monitor = {}; monitor.cb = sizeof(monitor);
        }
        state.screens.push_back({handle,std::move(identity),info.szDevice,
            info.rcMonitor,info.rcWork,(info.dwFlags & MONITORINFOF_PRIMARY)!=0});
        return TRUE;
    },reinterpret_cast<LPARAM>(&context));
    std::sort(context.screens.begin(),context.screens.end(),[](const Screen& a,const Screen& b) {
        return a.device < b.device;
    });
    return context.screens;
}
const Screen* resolveScreen(const std::vector<Screen>& screens, const std::wstring& id) {
    const auto selected = std::find_if(screens.begin(),screens.end(),[&](const auto& screen) { return screen.id==id; });
    if (selected!=screens.end()) return &*selected;
    const auto primary = std::find_if(screens.begin(),screens.end(),[](const auto& screen) { return screen.primary; });
    return primary!=screens.end() ? &*primary : screens.empty() ? nullptr : &screens.front();
}
std::wstring screenTitle(const Screen& screen, size_t index) {
    return L"显示屏"+std::to_wstring(index+1)+(screen.primary ? L"（主显示屏）" : L"");
}
RECT transferScreenRect(RECT rect, RECT sourceWork, RECT targetWork, int width, int height) {
    auto coordinate = [](LONG position,LONG sourceStart,LONG sourceEnd,LONG oldSize,
        LONG targetStart,LONG targetEnd,int newSize) {
        const LONG available = std::max(0L,sourceEnd-sourceStart-oldSize);
        const double fraction = available ? std::clamp(static_cast<double>(position-sourceStart)/available,0.0,1.0) : .5;
        return targetStart+static_cast<LONG>(std::lround(fraction*std::max(0L,targetEnd-targetStart-newSize)));
    };
    const LONG x = coordinate(rect.left,sourceWork.left,sourceWork.right,rect.right-rect.left,
        targetWork.left,targetWork.right,width);
    const LONG y = coordinate(rect.top,sourceWork.top,sourceWork.bottom,rect.bottom-rect.top,
        targetWork.top,targetWork.bottom,height);
    return {x,y,x+width,y+height};
}
}
