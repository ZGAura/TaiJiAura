#pragma once
#include <algorithm>

namespace desktopball {
inline unsigned normalizeSmokeFrameRate(unsigned value) {
    return value==30 || value==60 || value==90 ? value : 60;
}
inline unsigned smokeFrameRate(unsigned selected, unsigned refreshRate) {
    return std::min(normalizeSmokeFrameRate(selected),std::max(1u,refreshRate));
}
inline unsigned activeFrameRate(bool collapsed, unsigned capsule, unsigned smoke, unsigned refreshRate) {
    return smokeFrameRate(collapsed ? smoke : capsule,refreshRate);
}
}
