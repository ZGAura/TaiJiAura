#include <desktopball/monitoring/thermal_zone_sampler.hpp>
#include <pdhmsg.h>
#include <algorithm>

namespace desktopball {
ThermalZoneSampler::ThermalZoneSampler() { refresh(); }
ThermalZoneSampler::~ThermalZoneSampler() { reset(); }
bool ThermalZoneSampler::open() {
    if (query_) return true;
    if (PdhOpenQueryW(nullptr,0,&query_)==ERROR_SUCCESS &&
        PdhAddEnglishCounterW(query_,L"\\Thermal Zone Information(*)\\Temperature",0,&counter_)==ERROR_SUCCESS) return true;
    reset(); return false;
}
void ThermalZoneSampler::reset() noexcept {
    if (query_) PdhCloseQuery(query_);
    query_=nullptr; counter_=nullptr; storage_.clear();
}
void ThermalZoneSampler::refresh() {
    reset(); supported_=false;
    if (!open()) return;
    // Discover instance paths without collecting temperature data. Translate
    // the English wildcard before expansion for localized Windows systems.
    DWORD bytes{};
    if (PdhGetCounterInfoW(counter_,FALSE,&bytes,nullptr)==PDH_MORE_DATA && bytes && bytes<=1024*1024) {
        std::vector<std::uint64_t> buffer((bytes+7)/8);
        auto* info=reinterpret_cast<PDH_COUNTER_INFO_W*>(buffer.data());
        if (PdhGetCounterInfoW(counter_,FALSE,&bytes,info)==ERROR_SUCCESS && info->szFullPath) {
            DWORD characters{};
            if (PdhExpandWildCardPathW(nullptr,info->szFullPath,nullptr,&characters,0)==PDH_MORE_DATA &&
                characters>1 && characters<=1024*1024) {
                std::vector<wchar_t> paths(characters);
                supported_=PdhExpandWildCardPathW(nullptr,info->szFullPath,paths.data(),&characters,0)==ERROR_SUCCESS && paths[0];
            }
        }
    }
    reset();
}
DeviceReading ThermalZoneSampler::device() const {
    DeviceReading reading{thermalZoneId,L"PDH 平台热区温度",L"TZ",DeviceKind::thermalZoneTemperature};
    reading.supported=supported_;
    return reading;
}
DeviceReading ThermalZoneSampler::sample(bool selected) {
    auto reading=device();
    if (!selected) { reset(); return reading; }
    if (!supported_ || !open() || PdhCollectQueryData(query_)!=ERROR_SUCCESS) return reading;
    DWORD bytes=static_cast<DWORD>(storage_.size()*sizeof(std::uint64_t)),count{};
    auto* items=storage_.empty() ? nullptr : reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage_.data());
    auto status=PdhGetFormattedCounterArrayW(counter_,PDH_FMT_DOUBLE,&bytes,&count,items);
    if (status==PDH_MORE_DATA && bytes && bytes<=1024*1024) {
        storage_.resize((bytes+7)/8);
        items=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage_.data());
        status=PdhGetFormattedCounterArrayW(counter_,PDH_FMT_DOUBLE,&bytes,&count,items);
    }
    if (status!=ERROR_SUCCESS) return reading;
    for (DWORD i=0;i<count;++i) {
        const auto& value=items[i].FmtValue;
        if (value.CStatus!=PDH_CSTATUS_VALID_DATA && value.CStatus!=PDH_CSTATUS_NEW_DATA) continue;
        if (const auto celsius=thermalZoneCelsius(value.doubleValue))
            reading.celsius=reading.celsius ? std::max(*reading.celsius,*celsius) : celsius;
    }
    return reading;
}
}
