// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// ACPI thermal zone temperature over WMI (root\WMI, MSAcpi_ThermalZoneTemperature). This is the
// only usermode temperature source that needs no driver, so it is what ARM64 and PawnIO-less x86
// fall back to. Coarser than a package sensor and absent on plenty of OEM machines.
#pragma once

// Declared unconditionally, like pawnio.hpp: win_cpu_source.hpp holds one by value and is
// compiled on every platform. Only the implementation is Windows-gated.
struct IWbemServices;

namespace hardware_monitor_cpp
{
namespace win
{

class AcpiThermal
{
public:
    AcpiThermal() = default;
    ~AcpiThermal();
    AcpiThermal(const AcpiThermal&) = delete;
    AcpiThermal& operator=(const AcpiThermal&) = delete;

    // Hottest plausible zone in Celsius; false when WMI, the class or any usable zone is missing.
    bool sample(double& celsius);

private:
    bool connect();

    IWbemServices* svc_ = nullptr;
    bool comInit_ = false; // this object called CoInitializeEx and owns the matching uninit
    bool failed_ = false;  // connect() failed once; do not keep retrying every sample
};

} // namespace win
} // namespace hardware_monitor_cpp
