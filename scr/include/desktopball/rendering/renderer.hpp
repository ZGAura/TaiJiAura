#pragma once
#include <desktopball/monitoring/system_sampler.hpp>
#include <desktopball/ui/display.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace desktopball {
class Renderer {
public:
    // A null HWND creates an offscreen target for automated rendering checks.
    explicit Renderer(HWND window, bool software = true, float expandedPadding = 0, std::uint64_t adapterLuid = 0);
    static bool hardwareAvailable(std::uint64_t adapterLuid = 0);
    bool software() const;
    std::uint64_t adapterLuid() const;
    void setHardware(std::uint64_t adapterLuid, float seconds, const Metrics& metrics,
        const DisplaySettings& settings = {}, float collapse = 0);
    // Keep the current frame and HWND target until the replacement has presented its first frame.
    void setSoftware(bool software, float seconds, const Metrics& metrics,
        const DisplaySettings& settings = {}, float collapse = 0);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void resize(unsigned width, unsigned height);
    // collapse blends continuously from the full capsule (0) to vertical smoke (1).
    bool draw(float seconds, const Metrics& metrics, const DisplaySettings& settings = {}, float collapse = 0);
    double drawMilliseconds() const;
    std::vector<std::uint32_t> readPixels();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
