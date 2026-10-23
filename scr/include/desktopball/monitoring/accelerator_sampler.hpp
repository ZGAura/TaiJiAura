#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <memory>

namespace desktopball {
enum class DeviceKind { cpu, memory, gpu, videoMemory, npu, temperature, thermalZoneTemperature };
inline bool isTemperature(DeviceKind kind) {
    return kind==DeviceKind::temperature || kind==DeviceKind::thermalZoneTemperature;
}
struct DeviceReading {
    std::wstring id, name, label;
    DeviceKind kind{};
    std::optional<double> percent;
    std::uint64_t used{}, total{};
    std::uint32_t vendor{};
    bool integrated{};
    bool supported{true};
    std::optional<double> celsius;
    // Runtime routing only; persistent settings continue to use the stable id.
    std::uint64_t adapterLuid{};
    std::wstring renderingId;
};
class AcceleratorSampler {
public:
    AcceleratorSampler();
    ~AcceleratorSampler();
    std::vector<DeviceReading> sample();
    std::vector<DeviceReading> sample(const std::vector<std::wstring>& ids);
    std::vector<DeviceReading> devices() const;
    void reset() noexcept;
    void refresh();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
