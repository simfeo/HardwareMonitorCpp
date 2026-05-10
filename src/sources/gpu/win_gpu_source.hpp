// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Vendor-neutral Windows GPU source: identity via DXGI, system-wide load + memory via the PDH GPU
// performance counters (no vendor SDK, no elevation). Covers AMD and Intel (integrated + discrete)
// GPUs; NVIDIA is skipped here because NVML (NvidiaGpuSource) provides richer NVIDIA data.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw {
namespace sources {

class WinGpuSource : public Source {
public:
    ~WinGpuSource() override;

    std::string id() const override { return "windows.gpu"; }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Adapter {
        DeviceId id;
        std::string luidTag; // "luid_0x%08x_0x%08x" (lowercase) for matching PDH instances
    };
    std::vector<Adapter> adapters_;
    void* query_ = nullptr;   // PDH_HQUERY
    void* cUtil_ = nullptr;   // \GPU Engine(*)\Utilization Percentage
    void* cDed_ = nullptr;    // \GPU Adapter Memory(*)\Dedicated Usage
    void* cShared_ = nullptr; // \GPU Adapter Memory(*)\Shared Usage
};

} // namespace sources
} // namespace idimus_hw
