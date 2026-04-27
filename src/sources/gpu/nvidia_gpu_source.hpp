// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// NVIDIA discrete GPU(s) via NVML. Cross-platform where NVML is present (Windows/Linux).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"
#include "sources/gpu/nvml.hpp"

namespace idimus_hw {
namespace sources {

class NvidiaGpuSource : public Source {
public:
    std::string id() const override { return "nvml.gpu"; }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    std::unique_ptr<nv::Nvml> nvml_;
    std::vector<void*> handles_; // one nvmlDevice_t per device ordinal
};

} // namespace sources
} // namespace idimus_hw
