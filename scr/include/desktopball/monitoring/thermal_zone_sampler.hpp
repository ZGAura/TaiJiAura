#pragma once
#include <desktopball/monitoring/accelerator_sampler.hpp>
#include <windows.h>
#include <pdh.h>

namespace desktopball {
inline constexpr wchar_t thermalZoneId[]=L"pdh:thermal-zone:temperature";
inline std::optional<double> thermalZoneCelsius(double kelvin) {
    // PDH Temperature uses whole kelvin, unlike WMI's tenths of kelvin.
    return kelvin>=223.15 && kelvin<=473.15 ? std::optional<double>(kelvin-273.15) : std::nullopt;
}
class ThermalZoneSampler {
public:
    ThermalZoneSampler();
    ~ThermalZoneSampler();
    ThermalZoneSampler(const ThermalZoneSampler&)=delete;
    ThermalZoneSampler& operator=(const ThermalZoneSampler&)=delete;
    DeviceReading device() const;
    DeviceReading sample(bool selected);
    void reset() noexcept;
    void refresh();
private:
    bool open();
    PDH_HQUERY query_{};
    PDH_HCOUNTER counter_{};
    bool supported_{};
    std::vector<std::uint64_t> storage_;
};
}
