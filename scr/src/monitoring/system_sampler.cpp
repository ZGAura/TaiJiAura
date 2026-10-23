#include <desktopball/monitoring/system_sampler.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace desktopball {
SystemSampler::SystemSampler() {
    SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
    cpuPresent_ = system.dwNumberOfProcessors > 0;
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) { return; }
    // English counter names work across localized Windows installations.
    if (PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Utility", 0, &cpu_) != ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(query_, L"\\Processor(_Total)\\% Processor Time", 0, &cpu_) != ERROR_SUCCESS) {
            PdhCloseQuery(query_); query_ = nullptr; cpu_ = nullptr;
        }
    }
}
SystemSampler::~SystemSampler() { if (query_) { PdhCloseQuery(query_); } }
void SystemSampler::reset() noexcept { cpuPrimed_ = false; rates_.reset(); accelerators_.reset(); thermalZones_.reset(); }
void SystemSampler::refreshDevices() { accelerators_.refresh(); thermalZones_.refresh(); reset(); }
std::vector<DeviceReading> SystemSampler::devices() const {
    std::vector<DeviceReading> result;
    if (cpuPresent_) result.push_back({L"cpu", L"CPU 占用率", L"CPU", DeviceKind::cpu});
    result.push_back({L"memory", L"内存占用率", L"RAM", DeviceKind::memory});
    const auto accelerators = accelerators_.devices();
    result.insert(result.end(), accelerators.begin(), accelerators.end());
    result.push_back(thermalZones_.device());
    return result;
}
Metrics SystemSampler::sample(const SampleRequest& request) {
    Metrics result;
    result.cpuPresent = cpuPresent_;
    result.accelerators = request.allAccelerators ? accelerators_.sample() : accelerators_.sample(request.acceleratorIds);
    const bool thermalSelected=request.allAccelerators ||
        std::find(request.acceleratorIds.begin(),request.acceleratorIds.end(),thermalZoneId)!=request.acceleratorIds.end();
    result.accelerators.push_back(thermalZones_.sample(thermalSelected));
    if (request.cpu && query_) {
        const auto status = PdhCollectQueryData(query_);
        if (status == ERROR_SUCCESS) {
            PDH_FMT_COUNTERVALUE value{};
            if (cpuPrimed_ && PdhGetFormattedCounterValue(cpu_, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS &&
                (value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA) &&
                std::isfinite(value.doubleValue)) {
                result.cpu = std::clamp(value.doubleValue, 0.0, 100.0);
            }
            cpuPrimed_ = true;
        } else { cpuPrimed_ = false; }
    } else { cpuPrimed_ = false; }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (request.memory && GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys) {
        result.memoryTotal = memory.ullTotalPhys;
        result.memoryUsed = memory.ullTotalPhys - memory.ullAvailPhys;
        result.memoryPercent = 100.0 * static_cast<double>(result.memoryUsed) / static_cast<double>(result.memoryTotal);
    }
    if (!request.network) { rates_.reset(); return result; }
    // The lowest-metric default route identifies the primary interface. Prefer
    // IPv4, then IPv6; never sum VPN, virtual, and physical adapters together.
    MIB_IPFORWARD_TABLE2* rawRoutes = nullptr;
    if (GetIpForwardTable2(AF_UNSPEC, &rawRoutes) == NO_ERROR) {
        const std::unique_ptr<MIB_IPFORWARD_TABLE2, decltype(&FreeMibTable)> routes(rawRoutes, FreeMibTable);
        std::optional<MIB_IF_ROW2> selected;
        std::uint64_t bestScore = UINT64_MAX;
        for (ULONG i = 0; i < routes->NumEntries; ++i) {
            const auto& route = routes->Table[i];
            if (route.DestinationPrefix.PrefixLength != 0) { continue; }
            MIB_IF_ROW2 row{};
            row.InterfaceLuid = route.InterfaceLuid;
            if (GetIfEntry2(&row) != NO_ERROR || row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK) { continue; }
            MIB_IPINTERFACE_ROW ip{};
            InitializeIpInterfaceEntry(&ip);
            ip.Family = route.DestinationPrefix.Prefix.si_family;
            ip.InterfaceLuid = row.InterfaceLuid;
            if (GetIpInterfaceEntry(&ip) != NO_ERROR) { continue; }
            const std::uint64_t score = static_cast<std::uint64_t>(route.Metric) + ip.Metric +
                (ip.Family == AF_INET6 ? (1ULL << 32) : 0);
            if (score < bestScore) { selected = row; bestScore = score; }
        }
        if (selected) {
            result.adapterName = selected->Alias;
            result.network = rates_.update(selected->InterfaceLuid.Value, selected->InOctets,
                selected->OutOctets, RateCounter::Clock::now());
        } else { rates_.reset(); }
    } else { rates_.reset(); }
    return result;
}
}

