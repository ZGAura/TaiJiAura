#pragma once
#include <desktopball/monitoring/system_sampler.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace desktopball {
inline constexpr std::array<int,8> capsuleScalePercentages{100,110,120,150,170,190,220,230};
inline constexpr int defaultCapsuleScalePercent=170;
inline constexpr size_t maxDisplayMetrics=4;
inline constexpr size_t minDisplayMetrics=2;
// Transparent room for the full animated rim, outside the unchanged capsule body.
inline constexpr int capsuleSmokePadding=10;
inline constexpr int capsuleWindowHeight=40+2*capsuleSmokePadding;
inline int actualCapsuleScalePercent(int percent) { return percent-20; }
inline int savedCapsuleScalePercent(int percent, int version) {
    return version < 3 ? percent+20 : percent;
}
inline bool isCapsuleScalePercent(int percent) {
    return std::find(capsuleScalePercentages.begin(),capsuleScalePercentages.end(),percent)!=capsuleScalePercentages.end();
}
inline int normalizeCapsuleScalePercent(int percent) {
    return isCapsuleScalePercent(percent) ? percent : defaultCapsuleScalePercent;
}
struct DisplaySettings {
    std::vector<std::wstring> selected{L"cpu", L"memory"};
    std::wstring gauge{L"cpu"};
    bool autoDock{true};
    bool showOutline{false};
};
inline bool eligibleMetric(const DeviceReading& device) {
    return device.supported && !(device.kind==DeviceKind::videoMemory && device.integrated) &&
        !(device.kind==DeviceKind::temperature && !device.integrated);
}
inline std::wstring temperatureText(const DeviceReading& device) {
    if (!device.celsius || !std::isfinite(*device.celsius)) return L"-";
    return std::to_wstring(static_cast<int>(std::lround(*device.celsius)))+L"℃";
}
struct CapsuleLayout {
    float first{30}, step{}, columnWidth{20};
    int width{};
    float last{};
};
inline CapsuleLayout capsuleLayout(size_t count) {
    const int columns = static_cast<int>(std::clamp(count,size_t{2},maxDisplayMetrics));
    const bool wide = columns >= 3;
    const int width=wide ? 99+(columns-3)*19 : 79;
    const float last=static_cast<float>(width)-27.0f;
    return {30,(last-30)/static_cast<float>(columns-1),20,width,last};
}
inline float capsuleLastColumnX(const CapsuleLayout& layout) {
    return layout.width-3.0f-4.0f-layout.columnWidth;
}
inline float capsuleUploadX(const CapsuleLayout& layout) {
    // Match the two-item upload slot's seven-pixel inset for every item count.
    return static_cast<float>(layout.width)-30.0f;
}
inline int capsuleWindowWidth(size_t count) { return capsuleLayout(count).width+2*capsuleSmokePadding; }
inline std::vector<DeviceReading> readings(const Metrics& data) {
    std::vector<DeviceReading> result;
    if (data.cpuPresent) result.push_back({L"cpu", L"CPU 占用率", L"CPU", DeviceKind::cpu, data.cpu});
    if (data.memoryPresent) result.push_back({L"memory", L"内存占用率", L"RAM", DeviceKind::memory,
        data.memoryPercent, data.memoryUsed, data.memoryTotal});
    for (const auto& device : data.accelerators) if (eligibleMetric(device)) result.push_back(device);
    return result;
}
inline SampleRequest samplingRequest(const DisplaySettings& settings, bool collapsed) {
    SampleRequest request;
    request.cpu = request.memory = false;
    request.network = !collapsed; // Network readouts are always visible in the expanded capsule.
    request.allAccelerators = false;
    const auto ids = collapsed ? std::vector<std::wstring>{settings.gauge} : settings.selected;
    for (const auto& id : ids) {
        if (id == L"cpu") request.cpu = true;
        else if (id == L"memory") request.memory = true;
        else request.acceleratorIds.push_back(id);
    }
    return request;
}
inline const DeviceReading* findReading(const std::vector<DeviceReading>& devices, const std::wstring& id) {
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& d) { return d.id == id && eligibleMetric(d); });
    return it == devices.end() ? nullptr : &*it;
}
inline const DeviceReading* preferredGpu(const std::vector<DeviceReading>& devices) {
    const DeviceReading* best{};
    auto rank = [](const DeviceReading& d) {
        if (d.integrated) return 0;
        return d.vendor==0x10de ? 3 : d.vendor==0x1002 ? 2 : 1;
    };
    for (const auto& device : devices) if (device.kind == DeviceKind::gpu &&
        (!best || rank(device) > rank(*best))) best = &device;
    return best;
}
inline const DeviceReading* formerGpuAlias(const std::vector<DeviceReading>& devices, const std::wstring& id) {
    const auto separator=id.find_last_of(L':');
    if (separator==std::wstring::npos || separator+1==id.size()) return nullptr;
    if (!std::all_of(id.begin()+separator+1,id.end(),[](wchar_t c) { return c>=L'0' && c<=L'9'; })) return nullptr;
    const auto* canonical=findReading(devices,id.substr(0,separator));
    return canonical && canonical->kind==DeviceKind::gpu ? canonical : nullptr;
}
inline bool toggleDisplayMetric(DisplaySettings& settings, const std::wstring& id) {
    const auto found = std::find(settings.selected.begin(),settings.selected.end(),id);
    if (found!=settings.selected.end()) {
        if (settings.selected.size()<=minDisplayMetrics) return false;
        settings.selected.erase(found);
    } else if (settings.selected.size()<maxDisplayMetrics) settings.selected.push_back(id);
    else settings.selected.back() = id;
    return true;
}
inline void normalize(DisplaySettings& settings, const std::vector<DeviceReading>& devices) {
    const auto first=std::find_if(devices.begin(),devices.end(),eligibleMetric);
    const std::wstring fallback=first==devices.end() ? L"" : first->id;
    std::vector<std::wstring> selected;
    for (const auto& id : settings.selected) {
        auto resolved = id;
        if (!findReading(devices,id) && id.starts_with(L"gpu:") && !id.ends_with(L":vram") && !id.ends_with(L":temperature")) {
            const auto* gpu=formerGpuAlias(devices,id);
            if (!gpu) gpu=preferredGpu(devices);
            if (gpu) resolved=gpu->id;
        }
        if (selected.size() < maxDisplayMetrics && findReading(devices, resolved) &&
            std::find(selected.begin(), selected.end(), resolved) == selected.end()) selected.push_back(resolved);
    }
    const auto appendMissing = [&](const std::wstring& id) {
        if (selected.size()<minDisplayMetrics && findReading(devices,id) &&
            std::find(selected.begin(),selected.end(),id)==selected.end()) selected.push_back(id);
    };
    for (const auto* id : {L"cpu",L"memory"}) appendMissing(id);
    for (const auto& device : devices) appendMissing(device.id);
    settings.selected = std::move(selected);
    if (!findReading(devices, settings.gauge)) {
        const auto* gpu = formerGpuAlias(devices,settings.gauge);
        if (!gpu) gpu=preferredGpu(devices);
        if (gpu && settings.gauge.starts_with(L"gpu:") && !settings.gauge.ends_with(L":vram") && !settings.gauge.ends_with(L":temperature")) settings.gauge = gpu->id;
        else settings.gauge = fallback;
    }
}
enum class DockSide { none, left, right };
inline DockSide nearestEdge(RECT window, RECT work, int threshold) {
    if (window.left <= work.left + threshold) return DockSide::left;
    if (window.right >= work.right - threshold) return DockSide::right;
    return DockSide::none;
}
inline RECT dockRect(RECT expanded, RECT work, DockSide side, int width, int height) {
    width = std::min(width, static_cast<int>(work.right - work.left));
    height = std::min(height, static_cast<int>(work.bottom - work.top));
    const LONG x = side == DockSide::left ? work.left : work.right - width;
    const LONG y = std::clamp((expanded.top + expanded.bottom - height) / 2, work.top, work.bottom - height);
    return {x, y, x + width, y + height};
}
}
