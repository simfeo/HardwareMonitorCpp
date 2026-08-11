// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// System memory via GlobalMemoryStatusEx. Windows only.
#pragma once

#include <string>
#include <vector>

#include "hardware_monitor_cpp/source.hpp"

namespace hardware_monitor_cpp
{
namespace sources
{

class WinMemorySource : public Source
{
public:
    std::string id() const override
    {
        return "windows.memory";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    DeviceId dev_{DeviceKind::Memory, 0};
};

} // namespace sources
} // namespace hardware_monitor_cpp
