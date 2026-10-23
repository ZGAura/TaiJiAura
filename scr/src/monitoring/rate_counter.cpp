#include <desktopball/monitoring/rate_counter.hpp>

namespace desktopball {
std::optional<NetworkRate> RateCounter::update(std::uint64_t adapter, std::uint64_t received,
    std::uint64_t sent, Clock::time_point time) {
    const auto previous = baseline_;
    baseline_ = Baseline{adapter, received, sent, time};
    if (!previous || previous->adapter != adapter || received < previous->received || sent < previous->sent) {
        return std::nullopt;
    }
    const double seconds = std::chrono::duration<double>(time - previous->time).count();
    // Do not turn a long sleep/resume gap into a misleading current speed.
    if (seconds <= 0.0 || seconds > 10.0) { return std::nullopt; }
    return NetworkRate{static_cast<double>(received - previous->received) / seconds,
        static_cast<double>(sent - previous->sent) / seconds};
}
void RateCounter::reset() noexcept { baseline_.reset(); }
}
