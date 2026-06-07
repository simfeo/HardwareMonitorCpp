// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/gpu/amd_gpu_source.hpp"

#ifdef _WIN32

#include <map>

namespace idimus_hw
{
namespace sources
{

namespace
{
constexpr int kAmdOrdinalBase = 100; // keep DeviceIds distinct from other GPU sources
}

std::vector<DeviceInfo> AmdGpuSource::discover()
{
    adl_ = std::make_unique<amd::Adl>();
    std::vector<DeviceInfo> result;
    if (!adl_->ok())
    {
        return result;
    }
    for (const amd::AdapterRef& a : adl_->adapters())
    {
        DeviceInfo info;
        info.id = DeviceId{DeviceKind::GpuDiscrete, kAmdOrdinalBase + int(adapterIndex_.size())};
        info.name = a.name.empty() ? "AMD GPU" : a.name;
        info.vendor = "AMD";
        adapterIndex_.push_back(a.index);
        result.push_back(std::move(info));
    }
    return result;
}

void AmdGpuSource::sample(std::vector<Reading>& out)
{
    if (!adl_ || !adl_->ok())
    {
        return;
    }
    for (size_t i = 0; i < adapterIndex_.size(); ++i)
    {
        DeviceId id{DeviceKind::GpuDiscrete, kAmdOrdinalBase + int(i)};
        auto emit = [&](Quantity q, Unit u, const std::string& ch, double v)
        { out.push_back(Reading{id, q, u, ch, v}); };
        std::map<int, double> s = adl_->queryPmLog(adapterIndex_[i]);
        auto has = [&](int k) { return s.find(k) != s.end(); };

        if (has(amd::TempEdge))
        {
            emit(Quantity::Temperature, Unit::Celsius, "Edge", s[amd::TempEdge]);
        }
        if (has(amd::TempHotspot))
        {
            emit(Quantity::Temperature, Unit::Celsius, "Hotspot", s[amd::TempHotspot]);
        }
        if (has(amd::TempMem))
        {
            emit(Quantity::Temperature, Unit::Celsius, "Memory", s[amd::TempMem]);
        }
        if (has(amd::ClkGfx))
        {
            emit(Quantity::Clock, Unit::Megahertz, "Core", s[amd::ClkGfx]);
        }
        if (has(amd::ClkMem))
        {
            emit(Quantity::Clock, Unit::Megahertz, "Memory", s[amd::ClkMem]);
        }
        if (has(amd::ActivityGfx))
        {
            emit(Quantity::Load, Unit::Percent, "Core", s[amd::ActivityGfx]);
        }
        if (has(amd::ActivityMem))
        {
            emit(Quantity::Load, Unit::Percent, "Memory Controller", s[amd::ActivityMem]);
        }
        if (has(amd::FanRpm))
        {
            emit(Quantity::FanSpeed, Unit::Rpm, "Fan", s[amd::FanRpm]);
        }
        else if (has(amd::FanPercent))
        {
            emit(Quantity::Load, Unit::Percent, "Fan", s[amd::FanPercent]);
        }
        if (has(amd::AsicPower))
        {
            emit(Quantity::Power, Unit::Watt, "Package", s[amd::AsicPower]);
        }
        else if (has(amd::GfxPower))
        {
            emit(Quantity::Power, Unit::Watt, "Core", s[amd::GfxPower]);
        }
        if (has(amd::GfxVoltage))
        {
            emit(Quantity::Voltage, Unit::Volt, "Core", s[amd::GfxVoltage] / 1000.0);
        }

        int vramMb = adl_->dedicatedVramMb(adapterIndex_[i]);
        if (vramMb > 0)
        {
            emit(Quantity::DataVolume, Unit::Byte, "Memory Used", double(vramMb) * 1024 * 1024);
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // _WIN32
