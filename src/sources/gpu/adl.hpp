// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Thin dynamic loader for AMD's ADL (atiadlxx.dll). Uses the public ADL2 API + the PMLog sensor
// query. Signatures/struct layouts/enum values are documented ADL SDK facts. No SDK headers.
// UNVERIFIED on AMD hardware (implemented from facts; validate on a Radeon GPU).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace idimus_hw {
namespace amd {

// PMLog sensor identifiers we care about (ADL_PMLOG_* enum values from the ADL SDK).
enum PmLog {
    ClkGfx = 1,
    ClkMem = 2,
    TempEdge = 14,
    TempMem = 15,
    FanRpm = 20,
    FanPercent = 21,
    ActivityGfx = 25,
    ActivityMem = 26,
    GfxVoltage = 27,
    AsicPower = 29,
    TempHotspot = 33,
    GfxPower = 36,
};

struct AdapterRef {
    int index = 0;      // ADL adapter index
    std::string name;   // strAdapterName
};

class Adl {
public:
    Adl();
    ~Adl();
    Adl(const Adl&) = delete;
    Adl& operator=(const Adl&) = delete;

    bool ok() const { return ok_; }
    const std::vector<AdapterRef>& adapters() const { return adapters_; }

    // Queries all PMLog sensors for an adapter: returns sensorId -> value for supported sensors.
    std::map<int, double> queryPmLog(int adapterIndex);
    // Dedicated VRAM usage in MB (0 if unavailable).
    int dedicatedVramMb(int adapterIndex);

private:
    bool ok_ = false;
    void* lib_ = nullptr;
    void* ctx_ = nullptr; // ADL_CONTEXT_HANDLE
    void* fns_ = nullptr; // opaque function table (defined in the .cpp)
    std::vector<AdapterRef> adapters_;
};

} // namespace amd
} // namespace idimus_hw
