// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Linux CPU: one device per physical package, each reporting per-core/total load (/proc/stat),
// clock (cpufreq), temperature (hwmon coretemp/k10temp on x86, SoC hwmon or a thermal zone on
// ARM) and package power (RAPL powercap energy counter, x86 only).
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

    struct Package
    {
        DeviceId dev{DeviceKind::Cpu, 0};
        int physicalId = 0;
        std::vector<int> cpus;       // logical processor indices belonging to this package
        std::string hwmonDir;        // coretemp/k10temp hwmon for this package, if any
        std::string thermalZoneDir;  // /sys/class/thermal fallback, only when no hwmon was found
        RaplDomain rapl;             // energyPath empty when this package exposes no RAPL domain
    };

    std::vector<Package> packages_;
    std::vector<Ticks> prev_; // indexed by logical processor, from /proc/stat's per-cpu rows
};

} // namespace sources
} // namespace hardware_monitor_cpp
