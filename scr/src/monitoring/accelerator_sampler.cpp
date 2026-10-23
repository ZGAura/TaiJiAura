#include <desktopball/monitoring/accelerator_sampler.hpp>
#include <desktopball/monitoring/gpu_identity.hpp>
#include <windows.h>
#include <dxgi1_2.h>
#include <initguid.h>
#include <dxcore.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <map>
#include <string_view>
#include <sstream>
#include <iomanip>

namespace desktopball {
using Microsoft::WRL::ComPtr;
namespace {
using Clock = std::chrono::steady_clock;
std::wstring luidToken(LUID luid) {
    std::wostringstream s;
    s << L"luid_0x" << std::hex << std::setfill(L'0') << std::setw(8) << static_cast<unsigned long>(luid.HighPart)
      << L"_0x" << std::setw(8) << luid.LowPart;
    return s.str();
}
std::wstring description(IDXCoreAdapter* adapter) {
    size_t bytes{};
    if (FAILED(adapter->GetPropertySize(DXCoreAdapterProperty::DriverDescription, &bytes)) || bytes > 65536) return L"NPU";
    std::vector<char> text(bytes + 1);
    if (FAILED(adapter->GetProperty(DXCoreAdapterProperty::DriverDescription, bytes, text.data()))) return L"NPU";
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), -1, nullptr, 0);
    if (length <= 0) return L"NPU";
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), -1, result.data(), length);
    result.pop_back(); return result;
}
}
struct AcceleratorSampler::Impl {
    struct Query {
        PDH_HQUERY handle{};
        PDH_HCOUNTER counter{};
        bool primed{};
        // PDH writes both its aligned item array and instance names here. Reuse
        // the allocation while this selected counter is alive.
        std::vector<std::uint64_t> storage;
        explicit Query(const std::wstring& path) {
            if (PdhOpenQueryW(nullptr, 0, &handle) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(handle, path.c_str(), 0, &counter) != ERROR_SUCCESS) counter = nullptr;
        }
        ~Query() { if (handle) PdhCloseQuery(handle); }
        template<class Consumer>
        void collect(bool rate, Consumer&& consume) {
            const bool collected = counter && PdhCollectQueryData(handle) == ERROR_SUCCESS;
            const bool ready = collected && (!rate || primed);
            primed = collected;
            if (!ready) return;
            DWORD bytes = static_cast<DWORD>(storage.size() * sizeof(std::uint64_t)), count = 0;
            auto* items = storage.empty() ? nullptr : reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
            auto status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items);
            if (status == PDH_MORE_DATA) {
                storage.resize((static_cast<size_t>(bytes) + 7) / 8);
                items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
                // Instances may still appear between calls; retry next sample
                // if this buffer is already too small again.
                status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items);
            }
            if (status != ERROR_SUCCESS) return;
            for (DWORD i = 0; i < count; ++i) {
                const auto& value = items[i].FmtValue;
                if ((value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) ||
                    !std::isfinite(value.doubleValue) || value.doubleValue < 0) continue;
                auto* name = items[i].szName;
                const std::wstring_view view(name);
                std::transform(name, name + view.size(), name, [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                // Views remain valid until the next collection on this query.
                consume(view, value.doubleValue);
            }
        }
    };
    struct Adapter {
        std::wstring id, name, label, token, renderingId;
        DeviceKind kind{};
        std::uint64_t dedicated{};
        std::uint64_t luid{};
        ComPtr<IDXCoreAdapter1> core;
        ComPtr<IDXCoreAdapter> temperatureCore;
        bool temperatureSupported{};
        std::vector<DXCoreAdapterEngineIndex> engines;
        std::vector<std::optional<std::uint64_t>> previous;
        Clock::time_point last{};
        bool memoryQuery{};
        unsigned physicalCount{1};
        std::uint32_t vendor{};
        bool integrated{};
        bool integratedKnown{};
        std::unique_ptr<Query> usageQuery, memoryQueryCounter;
    };
    // Optional DLL: Windows versions without DXCore still get DXGI + PDH GPU support.
    HMODULE module{LoadLibraryExW(L"dxcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    ComPtr<IDXCoreAdapterFactory> factory;
    std::vector<Adapter> adapters;
    std::vector<DeviceReading> catalog;
    Impl() {
        if (module) {
            using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
            auto create = reinterpret_cast<CreateFactory>(GetProcAddress(module, "DXCoreCreateAdapterFactory"));
            if (create) create(IID_PPV_ARGS(&factory));
        }
        discover();
    }
    ~Impl() {
        adapters.clear(); factory.Reset();
        if (module) FreeLibrary(module);
    }
    void attachCore(Adapter& device, IDXCoreAdapter* adapter) {
        if (!adapter) return;
        bool integrated{};
        // IsIntegrated belongs to the base interface, even when engine telemetry
        // through IDXCoreAdapter1 is unavailable on this Windows/driver version.
        if (adapter->IsPropertySupported(DXCoreAdapterProperty::IsIntegrated) &&
            SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::IsIntegrated, &integrated))) {
            device.integrated = integrated;
            device.integratedKnown = true;
        }
        // Capability is independent of any individual reading, including on
        // drivers exposing the base interface without IDXCoreAdapter1.
        if (device.kind==DeviceKind::gpu && device.integratedKnown && device.integrated) {
            device.temperatureCore=adapter;
            device.temperatureSupported=adapter->IsQueryStateSupported(DXCoreAdapterState::AdapterTemperatureCelsius);
        }
        uint32_t physical = 1;
        if (SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::PhysicalAdapterCount, &physical)))
            device.physicalCount = std::clamp(physical, 1u, 16u);
        if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&device.core)))) return;
        auto* core = device.core.Get();
        if (core->IsPropertySupported(DXCoreAdapterProperty::AdapterEngineCount) &&
            core->IsQueryStateSupported(DXCoreAdapterState::AdapterEngineRunningTimeMicroseconds)) {
            for (uint32_t p = 0; p < device.physicalCount; ++p) {
                uint32_t count{};
                if (FAILED(core->GetPropertyWithInput(DXCoreAdapterProperty::AdapterEngineCount, &p, &count))) continue;
                for (uint32_t e = 0; e < std::min(count, 256u); ++e) device.engines.push_back({p, e});
            }
        }
        device.previous.resize(device.engines.size());
        device.memoryQuery = core->IsQueryStateSupported(DXCoreAdapterState::AdapterMemoryUsageBytes);
    }
    void discoverAdapters() {
        adapters.clear();
        ComPtr<IDXGIFactory1> dxgi;
        std::vector<GpuIdentity> gpuIdentities;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi)))) {
            for (UINT i = 0; ; ++i) {
                ComPtr<IDXGIAdapter1> gpu;
                if (dxgi->EnumAdapters1(i, &gpu) == DXGI_ERROR_NOT_FOUND) break;
                if (!gpu) break;
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(gpu->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
                auto identity=queryGpuIdentity(desc.AdapterLuid);
                if (!monitorableGpu(identity) || std::any_of(gpuIdentities.begin(),gpuIdentities.end(),
                    [&](const auto& existing) { return samePhysicalGpu(existing,identity); })) continue;
                Adapter device;
                device.kind = DeviceKind::gpu; device.name = desc.Description;
                device.label = L"GPU";
                device.vendor = desc.VendorId;
                // Treat unclassified adapters conservatively as integrated. Reserved
                // UMA memory is not evidence that an integrated GPU is discrete.
                device.integrated = true;
                // Description + PCI identity remain stable over reboot (unlike a LUID).
                device.id = L"gpu:" + std::to_wstring(desc.VendorId) + L":" + std::to_wstring(desc.DeviceId)
                    + L":" + std::to_wstring(desc.SubSysId) + L":" + device.name;
                const auto duplicates = std::count_if(adapters.begin(), adapters.end(), [&](const auto& a) { return a.id.starts_with(device.id); });
                if (duplicates) device.id += L":" + std::to_wstring(duplicates);
                device.renderingId=identity.physicalKey.empty() ? device.id : L"physical:"+identity.physicalKey;
                std::transform(device.renderingId.begin(),device.renderingId.end(),device.renderingId.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                device.token = luidToken(desc.AdapterLuid);
                device.luid = identity.luid;
                device.dedicated = desc.DedicatedVideoMemory;
                if (factory) {
                    ComPtr<IDXCoreAdapter> core;
                    if (SUCCEEDED(factory->GetAdapterByLuid(desc.AdapterLuid, core.GetAddressOf()))) attachCore(device, core.Get());
                }
                adapters.push_back(std::move(device));
                gpuIdentities.push_back(std::move(identity));
            }
        }
        if (!factory) return;
        ComPtr<IDXCoreAdapterList> npus;
        // Hardware type avoids mistaking a GPU supporting DirectML for an NPU.
        const GUID filter = DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU;
        ComPtr<IDXCoreAdapterFactory1> newerFactory;
        if (SUCCEEDED(factory.As(&newerFactory))) {
            newerFactory->CreateAdapterListByWorkload(DXCoreWorkload::MachineLearning,
                DXCoreRuntimeFilterFlags::None, DXCoreHardwareTypeFilterFlags::NPU, npus.GetAddressOf());
        }
        if (!npus && FAILED(factory->CreateAdapterList(1, &filter, npus.GetAddressOf()))) return;
        unsigned npuIndex = 0;
        for (uint32_t i = 0; i < npus->GetAdapterCount(); ++i) {
            ComPtr<IDXCoreAdapter> core;
            if (FAILED(npus->GetAdapter(i, core.GetAddressOf()))) continue;
            bool hardware{};
            if (FAILED(core->GetProperty(DXCoreAdapterProperty::IsHardware, &hardware)) || !hardware ||
                !core->IsAttributeSupported(DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU)) continue;
            Adapter device;
            device.kind = DeviceKind::npu;
            device.name = description(core.Get());
            device.id = L"npu:" + device.name + L":" + std::to_wstring(npuIndex);
            device.label = npuIndex==0 ? L"NPU" : L"NPU" + std::to_wstring(npuIndex+1);
            ++npuIndex;
            LUID luid{};
            if (SUCCEEDED(core->GetProperty(DXCoreAdapterProperty::InstanceLuid, &luid))) device.token = luidToken(luid);
            attachCore(device, core.Get());
            adapters.push_back(std::move(device));
        }
    }
    void discover() {
        discoverAdapters();
        std::stable_sort(adapters.begin(),adapters.end(),[](const auto& a,const auto& b) {
            const int left=a.kind==DeviceKind::gpu ? (a.integrated ? 0 : 1) : 2;
            const int right=b.kind==DeviceKind::gpu ? (b.integrated ? 0 : 1) : 2;
            return left<right;
        });
        const auto integratedCount=std::count_if(adapters.begin(),adapters.end(),[](const auto& a) {
            return a.kind==DeviceKind::gpu && a.integrated;
        });
        unsigned integratedIndex=0, discreteIndex=0;
        for (auto& adapter : adapters) if (adapter.kind==DeviceKind::gpu) {
            if (adapter.integrated) adapter.label=integratedCount==1 ? L"IGPU" : L"IGPU"+std::to_wstring(integratedIndex++);
            else adapter.label=L"GPU"+std::to_wstring(discreteIndex++);
        }
        // Device metadata changes only on discovery, not on every sample.
        catalog = makeCatalog();
    }
    std::vector<DeviceReading> makeCatalog() const {
        std::vector<DeviceReading> result;
        result.reserve(adapters.size() * 3);
        for (const auto& device : adapters) {
            const auto name = device.kind==DeviceKind::gpu ? device.label+L" 占用率 · "+device.name : device.label+L" · "+device.name;
            result.push_back({device.id, name, device.label, device.kind,
                std::nullopt, 0, device.dedicated, device.vendor, device.integrated});
            result.back().adapterLuid=device.luid;
            result.back().renderingId=device.renderingId;
            if (device.kind == DeviceKind::gpu && !device.integrated && device.dedicated)
                result.push_back({device.id + L":vram", device.label+L" 显存占用率 · " + device.name, L"VRAM", DeviceKind::videoMemory,
                    std::nullopt, 0, device.dedicated, device.vendor, device.integrated});
            if (device.kind==DeviceKind::gpu && device.integratedKnown && device.integrated)
                result.push_back({device.id+L":temperature", device.label+L" 温度 · "+device.name, L"GPU", DeviceKind::temperature,
                    std::nullopt, 0, 0, device.vendor, device.integrated, device.temperatureSupported});
        }
        return result;
    }
    std::vector<DeviceReading> devices() const { return catalog; }
    std::vector<DeviceReading> sample(const std::vector<std::wstring>& ids, bool all = false) {
        const auto wanted = [&](const std::wstring& id) { return all || std::find(ids.begin(), ids.end(), id) != ids.end(); };
        const auto now = Clock::now();
        auto result = devices();
        size_t readingIndex = 0;
        for (auto& device : adapters) {
            auto& usageReading = result[readingIndex++];
            auto* memoryReading = device.kind == DeviceKind::gpu && !device.integrated && device.dedicated ? &result[readingIndex++] : nullptr;
            auto* temperatureReading = device.kind == DeviceKind::gpu && device.integratedKnown && device.integrated
                ? &result[readingIndex++] : nullptr;
            const bool sampleUsage = wanted(device.id);
            const bool sampleMemory = memoryReading && wanted(memoryReading->id);
            if (device.temperatureSupported && temperatureReading && wanted(temperatureReading->id) &&
                device.temperatureCore && device.temperatureCore->IsValid()) {
                std::optional<double> hottest;
                bool valid=true;
                for (uint32_t p=0; p<device.physicalCount; ++p) {
                    uint32_t celsius{};
                    if (FAILED(device.temperatureCore->QueryState(DXCoreAdapterState::AdapterTemperatureCelsius,&p,&celsius)) || celsius>200) {
                        valid=false; break;
                    }
                    hottest=std::max(hottest.value_or(0),static_cast<double>(celsius));
                }
                temperatureReading->celsius=valid ? hottest : std::nullopt;
            }
            if (!sampleUsage) {
                for (auto& previous : device.previous) previous.reset();
                device.last = {}; device.usageQuery.reset();
            }
            if (!sampleMemory) device.memoryQueryCounter.reset();
            if (!sampleUsage && !sampleMemory) continue;
            std::optional<double> usage;
            if (sampleUsage && device.core && device.core->IsValid() && !device.engines.empty()) {
                const double elapsed = std::chrono::duration<double, std::micro>(now - device.last).count();
                for (size_t i = 0; i < device.engines.size(); ++i) {
                    uint64_t running{};
                    if (FAILED(device.core->QueryState(DXCoreAdapterState::AdapterEngineRunningTimeMicroseconds, &device.engines[i], &running))) {
                        device.previous[i].reset(); continue;
                    }
                    if (device.previous[i] && running >= *device.previous[i] && elapsed > 0 && elapsed < 15000000) {
                        const double percent = std::clamp((running - *device.previous[i]) * 100.0 / elapsed, 0.0, 100.0);
                        usage = std::max(usage.value_or(0), percent);
                    }
                    device.previous[i] = running;
                }
                device.last = now;
            }
            if (sampleUsage && !usage && !device.token.empty()) {
                // Scope PDH to this adapter; never collect engines or memory of unselected devices.
                if (!device.usageQuery) device.usageQuery = std::make_unique<Query>(
                    L"\\GPU Engine(*" + device.token + L"*)\\Utilization Percentage");
                // Sum processes on each engine, then take the busiest engine, never sum different engines.
                std::map<std::wstring_view, double> byEngine;
                device.usageQuery->collect(true, [&](std::wstring_view name, double value) {
                    const auto pos = name.find(device.token);
                    const auto engine = name.find(L"_eng_");
                    if (pos != std::wstring_view::npos && engine != std::wstring_view::npos) {
                        const auto physical = name.find(L"_phys_");
                        byEngine[name.substr(physical != std::wstring_view::npos ? physical : engine)] += value;
                    }
                });
                for (const auto& [id, value] : byEngine) usage = std::max(usage.value_or(0), std::clamp(value, 0.0, 100.0));
            } else if (usage) { device.usageQuery.reset(); }
            usageReading.percent = usage;
            if (!sampleMemory) continue;
            std::optional<uint64_t> used;
            if (device.core && device.core->IsValid() && device.memoryQuery) {
                uint64_t sum{}; bool valid = true;
                for (uint32_t p = 0; p < device.physicalCount; ++p) {
                    DXCoreMemoryQueryInput input{p, DXCoreMemoryType::Dedicated};
                    DXCoreMemoryUsage memoryUsage{};
                    if (FAILED(device.core->QueryState(DXCoreAdapterState::AdapterMemoryUsageBytes, &input, &memoryUsage))) { valid = false; break; }
                    sum += memoryUsage.resident;
                }
                if (valid) used = sum;
            }
            if (!used) {
                if (!device.memoryQueryCounter) device.memoryQueryCounter = std::make_unique<Query>(
                    L"\\GPU Adapter Memory(*" + device.token + L"*)\\Dedicated Usage");
                device.memoryQueryCounter->collect(false, [&](std::wstring_view name, double value) {
                    if (name.find(device.token) != std::wstring_view::npos)
                        used = used.value_or(0) + static_cast<uint64_t>(value);
                });
            }
            memoryReading->percent = used ? std::optional<double>(std::clamp(*used * 100.0 / device.dedicated, 0.0, 100.0)) : std::nullopt;
            memoryReading->used = used.value_or(0);
        }
        return result;
    }
};
AcceleratorSampler::AcceleratorSampler() : impl_(std::make_unique<Impl>()) {}
AcceleratorSampler::~AcceleratorSampler() = default;
std::vector<DeviceReading> AcceleratorSampler::sample() {
    return impl_->sample({}, true);
}
std::vector<DeviceReading> AcceleratorSampler::sample(const std::vector<std::wstring>& ids) { return impl_->sample(ids); }
std::vector<DeviceReading> AcceleratorSampler::devices() const { return impl_->devices(); }
void AcceleratorSampler::reset() noexcept {
    for (auto& device : impl_->adapters) {
        device.usageQuery.reset(); device.memoryQueryCounter.reset(); device.last = {};
        for (auto& previous : device.previous) previous.reset();
    }
}
void AcceleratorSampler::refresh() { impl_->discover(); }
}



