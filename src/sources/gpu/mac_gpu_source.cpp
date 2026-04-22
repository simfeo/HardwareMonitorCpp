// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/gpu/mac_gpu_source.hpp"

#ifdef __APPLE__

#include <sys/sysctl.h>

#include "platform/macos/cf_util.hpp"

namespace idimus_hw {
namespace sources {
namespace {
std::string brand() {
    char b[128];
    size_t n = sizeof(b);
    return sysctlbyname("machdep.cpu.brand_string", b, &n, nullptr, 0) == 0 ? std::string(b)
                                                                            : std::string();
}
} // namespace

MacGpuSource::MacGpuSource() {
    service_ = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    if (service_) {
        long long cc = 0;
        if (mac::regNumber(service_, CFSTR("gpu-core-count"), cc))
            cores_ = int(cc);
    }
    if (smc_.ok()) {
        for (const std::string& k : smc_.allKeys()) {
            if (k.size() == 4 && k[0] == 'T' && k[1] == 'g') {
                float t;
                if (smc_.readFloat(k, t) && t > 1.0f && t < 130.0f)
                    tempKeys_.push_back(k);
            }
        }
    }
}

MacGpuSource::~MacGpuSource() {
    if (service_)
        IOObjectRelease(service_);
}

std::vector<DeviceInfo> MacGpuSource::discover() {
    if (!service_)
        return {};
    DeviceInfo info;
    info.id = dev_;
    std::string b = brand();
    info.name = (b.empty() ? "Apple" : b) + " GPU";
    info.vendor = "Apple";
    if (cores_ > 0)
        info.attributes["cores"] = std::to_string(cores_);
    return {info};
}

void MacGpuSource::sample(std::vector<Reading>& out) {
    if (!service_)
        return;
    auto emit = [&](Quantity q, Unit u, const std::string& ch, double v) {
        out.push_back(Reading{dev_, q, u, ch, v});
    };

    CFTypeRef stats =
        IORegistryEntryCreateCFProperty(service_, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
    if (stats && CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
        auto d = (CFDictionaryRef)stats;
        double v = 0;
        if (mac::dictNumber(d, "Device Utilization %", v)) emit(Quantity::Load, Unit::Percent, "Core", v);
        if (mac::dictNumber(d, "Renderer Utilization %", v)) emit(Quantity::Load, Unit::Percent, "Renderer", v);
        if (mac::dictNumber(d, "Tiler Utilization %", v)) emit(Quantity::Load, Unit::Percent, "Tiler", v);
        if (mac::dictNumber(d, "In use system memory", v)) emit(Quantity::DataVolume, Unit::Byte, "Memory In Use", v);
    }
    if (stats)
        CFRelease(stats);

    // Temperature: count only plausible readings (the Tg sensors floor sub-ambient when powergated).
    if (!tempKeys_.empty()) {
        double sum = 0;
        int n = 0;
        for (const std::string& k : tempKeys_) {
            float t;
            if (smc_.readFloat(k, t) && t > 20.0f && t < 130.0f) { sum += t; ++n; }
        }
        if (n > 0)
            emit(Quantity::Temperature, Unit::Celsius, "Die", sum / n);
    }

    if (ioreport_.ok()) {
        mac::IoReport::Sample s;
        if (ioreport_.sample(s)) {
            if (s.powerW.count("GPU")) emit(Quantity::Power, Unit::Watt, "Power", s.powerW["GPU"]);
            if (s.freqMhz.count("GPU")) emit(Quantity::Clock, Unit::Megahertz, "Core", s.freqMhz["GPU"]);
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __APPLE__
