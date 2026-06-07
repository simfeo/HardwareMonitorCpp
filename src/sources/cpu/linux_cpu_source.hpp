// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Linux CPU: per-core/total load (/proc/stat), clock (cpufreq), temperature (hwmon coretemp/
// k10temp), package power (RAPL powercap energy counter).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw
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
    DeviceId dev_{DeviceKind::Cpu, 0};
    int cores_ = 0;
    std::vector<Ticks> prev_;    // index 0 = aggregate, 1.. = per core
    std::string hwmonDir_;       // resolved CPU hwmon directory
    std::string raplEnergyPath_; // powercap package energy_uj
    uint64_t raplMaxRange_ = 0;  // wrap range (max_energy_range_uj)
    double prevEnergyUj_ = -1, prevEnergyTime_ = 0;
};

} // namespace sources
} // namespace idimus_hw
