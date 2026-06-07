// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Thin dynamic loader for Intel's IGCL (ControlLib.dll): enumerate adapters + power telemetry.
// Struct layouts mirror the public igcl_api.h and are Size/Version-guarded so an ABI mismatch
// fails safe (no data) rather than crashing. Windows. UNVERIFIED on Intel Arc hardware.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace idimus_hw
{
namespace intel
{

struct GpuReadings
{
    bool hasTemp = false;
    double tempC = 0;
    bool hasClock = false;
    double clockMhz = 0;
    bool hasActivity = false;
    double activityPct = 0;
    bool hasPower = false;
    double powerW = 0; // computed from the energy-counter delta between calls
};

struct AdapterRef
{
    void* handle = nullptr; // ctl_device_adapter_handle_t
    std::string name;
    bool discrete = false;
};

class Igcl
{
public:
    Igcl();
    ~Igcl();
    Igcl(const Igcl&) = delete;
    Igcl& operator=(const Igcl&) = delete;

    bool ok() const
    {
        return ok_;
    }
    const std::vector<AdapterRef>& adapters() const
    {
        return adapters_;
    }

    // Reads telemetry for an adapter; computes power from the energy delta vs the previous call.
    GpuReadings read(size_t adapterOrdinal);

private:
    bool ok_ = false;
    void* lib_ = nullptr;
    void* api_ = nullptr; // ctl_api_handle_t
    void* fns_ = nullptr; // opaque table
    std::vector<AdapterRef> adapters_;
    std::vector<double> prevEnergyJ_;
    std::vector<double> prevTime_;
};

} // namespace intel
} // namespace idimus_hw
