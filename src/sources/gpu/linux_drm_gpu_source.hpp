// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Linux AMD (amdgpu) + Intel (i915) GPUs via /sys/class/drm + hwmon. NVIDIA is handled by NVML.
#pragma once

#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw {
namespace sources {

class LinuxDrmGpuSource : public Source {
public:
    std::string id() const override { return "linux.gpu.drm"; }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Card {
        DeviceId id;
        std::string cardName;   // "cardN"
        std::string devicePath; // /sys/class/drm/cardN/device
        std::string hwmonPath;  // .../hwmon/hwmonX (may be empty)
        unsigned vendor = 0;
    };
    std::vector<Card> cards_;
};

} // namespace sources
} // namespace idimus_hw
