#pragma once
#include <desktopball/monitoring/system_sampler.hpp>
#include <desktopball/rendering/renderer.hpp>
#include <desktopball/ui/frame_rate.hpp>
#include <desktopball/window/startup.hpp>
#include <desktopball/window/screens.hpp>
#include <desktopball/ui/language.hpp>
#include <shellapi.h>
#include <chrono>
#include <memory>

namespace desktopball {
class Overlay {
public:
    explicit Overlay(HINSTANCE instance, bool showWindow = true, StartupRegistration startup = StartupRegistration{});
    ~Overlay();
    int run();
    struct FrameStatistics {
        unsigned targetFps, refreshRate;
        std::uint64_t renderedFrames;
        bool collapsed;
        double drawMilliseconds;
        bool software;
        std::uint64_t adapterLuid;
    };
    FrameStatistics frameStatistics() const {
        return {fps_,refreshRate_,renderedFrames_,collapsed_,drawMilliseconds_,renderer_ ? renderer_->software() : true,
            renderer_ ? renderer_->adapterLuid() : 0};
    }
    struct SamplingStatistics {
        std::uint64_t samples;
        SampleRequest request;
    };
    SamplingStatistics samplingStatistics() const { return {sampleCount_, lastSampleRequest_}; }
private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void addTray();
    void trayMenu();
    void schedule();
    bool outputAvailable() const { return !locked_ && displayOn_ && !suspended_; }
    void armFrame();
    void updateFrameRate();
    void render();
    void loadSettings();
    void saveSettings();
    void updateTooltip();
    void sampleMetrics();
    void refreshDevices();
    void setGpuRendering(bool enabled, const std::wstring& deviceId = {});
    void refreshRenderingDevices();
    void updateDock();
    void snapToEdge();
    void setCollapsed(bool collapsed, bool animate = true);
    float collapseBlend() const;
    void cancelDock();
    int scaledDip(int value) const;
    void applyScale(int percent);
    void moveToScreen(const Screen& screen);
    void refreshScreens();
    void rememberScreen();
    int scalePercent_{defaultCapsuleScalePercent};
    HWND window_{};
    NOTIFYICONDATAW tray_{};
    UINT taskbarRestart_{};
    bool trayAdded_{}, paused_{}, locked_{}, visible_{true};
    bool displayOn_{true}, suspended_{};
    HPOWERNOTIFY displayPowerNotification_{};
    unsigned fps_{60};
    bool gpuRendering_{}, gpuAvailable_{};
    std::wstring renderGpuId_;
    // Rendering-only copies: supported denotes D3D11 availability, independently of telemetry.
    std::vector<DeviceReading> renderGpus_;
    unsigned capsuleFps_{60}, smokeFps_{60}, refreshRate_{60};
    double drawMilliseconds_{};
    std::uint64_t renderedFrames_{};
    HANDLE frameClock_{};
    StartupRegistration startup_;
    SystemSampler sampler_;
    Metrics metrics_;
    std::unique_ptr<Renderer> renderer_;
    using Clock = std::chrono::steady_clock;
    Clock::time_point lastFrame_{Clock::now()};
    Clock::time_point lastSample_{};
    std::uint64_t sampleCount_{};
    SampleRequest lastSampleRequest_;
    float animationSeconds_{};
    DisplaySettings settings_;
    std::wstring settingsPath_;
    std::wstring screenId_;
    Language language_{Language::chinese};
    RECT screenWork_{};
    bool relocating_{};
    DockSide dockSide_{DockSide::none};
    RECT expandedRect_{}, workRect_{}, animationFrom_{}, animationTo_{};
    bool collapsed_{}, transitioning_{}, moving_{}, menuOpen_{};
    float collapseFrom_{};
    Clock::time_point dockActivity_{Clock::now()}, hoverSince_{}, transitionStart_{};
};
}
