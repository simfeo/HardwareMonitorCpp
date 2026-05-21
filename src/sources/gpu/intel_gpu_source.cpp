// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/gpu/intel_gpu_source.hpp"

#ifdef _WIN32

namespace idimus_hw {
namespace sources {

namespace {
constexpr int kIntelArcOrdinalBase = 200; // keep DeviceIds distinct from other GPU sources
}

std::vector<DeviceInfo> IntelGpuSource::discover() {
    igcl_ = std::make_unique<intel::Igcl>();
    std::vector<DeviceInfo> result;
    if (!igcl_->ok())
        return result;
    for (const intel::AdapterRef& a : igcl_->adapters()) {
        DeviceInfo info;
        info.id = DeviceId{DeviceKind::GpuDiscrete, kIntelArcOrdinalBase + int(count_)};
        info.name = a.name.empty() ? "Intel GPU" : a.name;
        info.vendor = "Intel";
        result.push_back(std::move(info));
        ++count_;
    }
    return result;
}

void IntelGpuSource::sample(std::vector<Reading>& out) {
    if (!igcl_ || !igcl_->ok())
        return;
    for (size_t i = 0; i < count_; ++i) {
        DeviceId id{DeviceKind::GpuDiscrete, kIntelArcOrdinalBase + int(i)};
        intel::GpuReadings r = igcl_->read(i);
        if (r.hasTemp)
            out.push_back(Reading{id, Quantity::Temperature, Unit::Celsius, "Core", r.tempC});
        if (r.hasClock)
            out.push_back(Reading{id, Quantity::Clock, Unit::Megahertz, "Core", r.clockMhz});
        if (r.hasActivity)
            out.push_back(Reading{id, Quantity::Load, Unit::Percent, "Core", r.activityPct});
        if (r.hasPower)
            out.push_back(Reading{id, Quantity::Power, Unit::Watt, "Power", r.powerW});
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // _WIN32
