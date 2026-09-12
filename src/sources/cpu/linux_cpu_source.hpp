// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Linux CPU: per-core/total load (/proc/stat), clock (cpufreq), temperature (hwmon coretemp/
// k10temp on x86, SoC hwmon or a thermal zone on ARM), package power (RAPL powercap energy
// counter, x86 only).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hardware_monitor_cpp/source.hpp"

namespace hardware_monitor_cpp
{
namespace sources
{

class LinuxCpuSource : public Source
{
public:
    std::string id() const override
    {
        return "linux.cpu";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Ticks
    {
        uint64_t idle = 0, total = 0;
    };
    // One powercap package domain per socket; the counter is 32/48-bit and wraps at maxRange.
    struct RaplDomain
    {
        std::string energyPath;
        uint64_t maxRange = 0;
        double prevUj = -1, prevTime = 0;
    };

    DeviceId dev_{DeviceKind::Cpu, 0};
    int cores_ = 0;
    std::vector<Ticks> prev_; // index 0 = aggregate, 1.. = per core

    std::vector<std::string> hwmonDirs_; // CPU hwmon per physical package, ordered by hwmon index
    std::string thermalZoneDir_; // /sys/class/thermal fallback, used only when hwmon found nothing
    std::vector<RaplDomain> rapl_;
};

} // namespace sources
} // namespace hardware_monitor_cpp
