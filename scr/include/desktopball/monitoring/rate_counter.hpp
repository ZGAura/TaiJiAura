#pragma once
#include <chrono>
#include <cstdint>
#include <optional>

namespace desktopball {
struct NetworkRate { double download{}; double upload{}; };

// Keeps the baseline independently from UI refresh timing. An invalid interval,
// adapter change, or counter reset establishes a fresh baseline instead of a spike.
class RateCounter {
public:
    using Clock = std::chrono::steady_clock;
    std::optional<NetworkRate> update(std::uint64_t adapter, std::uint64_t received,
        std::uint64_t sent, Clock::time_point time);
    void reset() noexcept;
private:
    struct Baseline {
        std::uint64_t adapter, received, sent;
        Clock::time_point time;
    };
    std::optional<Baseline> baseline_;
};
}
