#pragma once
#include <desktopball/monitoring/rate_counter.hpp>
#include <desktopball/monitoring/accelerator_sampler.hpp>
#include <desktopball/monitoring/thermal_zone_sampler.hpp>
#include <windows.h>
#include <pdh.h>
#include <optional>
#include <string>

namespace desktopball {
struct Metrics {
    bool cpuPresent{true};
    bool memoryPresent{true};
    std::optional<double> cpu;
    std::optional<double> memoryPercent;
    std::uint64_t memoryUsed{}, memoryTotal{};
    std::optional<NetworkRate> network;
    std::wstring adapterName;
    std::vector<DeviceReading> accelerators;
};

struct SampleRequest {
    bool cpu{true}, memory{true}, network{true};
    bool allAccelerators{true};
    std::vector<std::wstring> acceleratorIds;
};

class SystemSampler {
public:
    SystemSampler();
    ~SystemSampler();
    SystemSampler(const SystemSampler&) = delete;
    SystemSampler& operator=(const SystemSampler&) = delete;
    Metrics sample(const SampleRequest& request = {});
    std::vector<DeviceReading> devices() const;
    void reset() noexcept;
    void refreshDevices();
private:
    PDH_HQUERY query_{};
    PDH_HCOUNTER cpu_{};
    bool cpuPrimed_{};
    bool cpuPresent_{};
    RateCounter rates_;
    AcceleratorSampler accelerators_;
    ThermalZoneSampler thermalZones_;
};
}
