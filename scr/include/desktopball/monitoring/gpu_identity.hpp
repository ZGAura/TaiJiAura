#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace desktopball {
struct GpuIdentity {
    std::wstring physicalKey;
    std::uint64_t luid{};
    bool software{}, indirectDisplay{};
};
GpuIdentity queryGpuIdentity(LUID luid);
inline bool monitorableGpu(const GpuIdentity& identity) {
    return !identity.software && !identity.indirectDisplay;
}
inline bool samePhysicalGpu(const GpuIdentity& a, const GpuIdentity& b) {
    if (!a.physicalKey.empty() && !b.physicalKey.empty()) {
        return CompareStringOrdinal(a.physicalKey.c_str(),-1,b.physicalKey.c_str(),-1,TRUE)==CSTR_EQUAL;
    }
    // Missing physical identity must not merge two distinct same-model GPUs.
    return a.luid==b.luid;
}
}
