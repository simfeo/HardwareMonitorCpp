// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/gpu/nvidia_gpu_source.hpp"

namespace idimus_hw {
namespace sources {

std::vector<DeviceInfo> NvidiaGpuSource::discover() {
    nvml_ = std::make_unique<nv::Nvml>();
    std::vector<DeviceInfo> result;
    if (!nvml_->ok())
        return result;
    for (unsigned i = 0; i < nvml_->deviceCount(); ++i) {
        void* dev = nvml_->deviceHandle(i);
        if (!dev)
            continue;
        handles_.push_back(dev);
        DeviceInfo info;
        info.id = DeviceId{DeviceKind::GpuDiscrete, int(handles_.size() - 1)};
        std::string name = nvml_->name(dev);
        info.name = name.empty() ? "NVIDIA GPU" : name;
        info.vendor = "NVIDIA";
        result.push_back(std::move(info));
    }
    return result;
}

void NvidiaGpuSource::sample(std::vector<Reading>& out) {
    if (!nvml_ || !nvml_->ok())
        return;
    for (size_t i = 0; i < handles_.size(); ++i) {
        void* dev = handles_[i];
        DeviceId id{DeviceKind::GpuDiscrete, int(i)};
        auto emit = [&](Quantity q, Unit u, const std::string& ch, double v) {
            out.push_back(Reading{id, q, u, ch, v});
        };

        double t = 0, p = 0;
        if (nvml_->temperatureC(dev, t))
            emit(Quantity::Temperature, Unit::Celsius, "Core", t);
        if (nvml_->powerW(dev, p))
            emit(Quantity::Power, Unit::Watt, "Power", p);

        nv::Utilization u;
        if (nvml_->utilization(dev, u)) {
            emit(Quantity::Load, Unit::Percent, "Core", u.gpu);
            emit(Quantity::Load, Unit::Percent, "Memory Controller", u.memory);
        }
        nv::MemoryInfo m;
        if (nvml_->memory(dev, m)) {
            emit(Quantity::DataVolume, Unit::Byte, "Memory Used", double(m.used));
            emit(Quantity::DataVolume, Unit::Byte, "Memory Total", double(m.total));
            if (m.total)
                emit(Quantity::Load, Unit::Percent, "Memory", 100.0 * double(m.used) / double(m.total));
        }
        unsigned mhz = 0;
        if (nvml_->clockMhz(dev, 0, mhz)) emit(Quantity::Clock, Unit::Megahertz, "Core", mhz);
        if (nvml_->clockMhz(dev, 2, mhz)) emit(Quantity::Clock, Unit::Megahertz, "Memory", mhz);
        unsigned fan = 0;
        if (nvml_->fanPercent(dev, fan))
            emit(Quantity::Load, Unit::Percent, "Fan", fan);
    }
}

} // namespace sources
} // namespace idimus_hw
