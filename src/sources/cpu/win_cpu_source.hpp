// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Windows CPU: per-core/total load (system processor performance counters), clock (processor
// power information), and - on Intel/AMD with PawnIO present - package temperature (digital
// thermal sensor) and RAPL package/cores/uncore power. Without that ring-0 path (ARM64, or no
// PawnIO) temperature falls back to ACPI thermal zones over WMI.
//
// One device per physical package: load, clock, temperature and power are all reported per CPU.
// Load covers every processor group, and the package MSRs are read with the thread pinned to a
// core of the package being sampled.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hardware_monitor_cpp/source.hpp"
#include "platform/windows/acpi_thermal.hpp"
#include "platform/windows/cpu_topology.hpp"
#include "platform/windows/pawnio.hpp"

namespace hardware_monitor_cpp
{
namespace sources
{

class WinCpuSource : public Source
{
public:
    std::string id() const override
    {
        return "windows.cpu";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Ticks
    {
        uint64_t idle = 0, total = 0;
    };
    // RAPL energy counter state for one domain (32-bit counter, wraps).
    struct Energy
    {
        uint32_t last = 0;
        double t = 0;
        bool primed = false;
    };

    enum class Vendor
    {
        Other,
        Intel,
        Amd
    };

    void readRapl(std::vector<Reading>& out, const DeviceId& dev, uint32_t msr, Energy& st,
                  const std::string& channel, double energyJoule);

    // Highest current clock (MHz) among this package's cores, from the per-core P-state MSRs,
    // pinning to each in turn; 0 if unavailable.
    double sampleMsrClock(const win::PackageInfo& pkg);

    std::vector<Ticks> prev_; // indexed by the flat, group-ordered processor array
    win::CpuTopology topo_;

    win::PawnIo pawn_;
    win::AcpiThermal acpi_; // driver-free temperature fallback when the MSR path is unavailable
    Vendor vendor_ = Vendor::Other;
    bool msr_ = false;         // ring-0 path active (module loaded + units read)
    double tjMax_ = 100.0;     // Intel
    double busClock_ = 100.0;  // Intel: MHz per P-state ratio step
    double baseMhz_ = 0.0;     // rated base (non-turbo) frequency, from registry
    double energyJoule_ = 0.0; // joules per RAPL energy tick (Intel or AMD)
    // Energy counter state per physical package. ePkg_ doubles as the AMD package domain.
    std::vector<Energy> ePkg_, ePp0_, ePp1_;
};

} // namespace sources
} // namespace hardware_monitor_cpp
