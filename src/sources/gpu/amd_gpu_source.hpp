// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// AMD Radeon GPU(s) via ADL PMLog. Windows. UNVERIFIED on AMD hardware.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "hardware_monitor_cpp/source.hpp"
#include "sources/gpu/adl.hpp"

namespace hardware_monitor_cpp
{
namespace sources
{

class AmdGpuSource : public Source
{
public:
    std::string id() const override
    {
        return "windows.gpu.amd";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    std::unique_ptr<amd::Adl> adl_;
    std::vector<int> adapterIndex_; // ADL index per device ordinal
};

} // namespace sources
} // namespace hardware_monitor_cpp
